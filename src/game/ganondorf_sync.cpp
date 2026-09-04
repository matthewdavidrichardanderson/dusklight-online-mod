#include "dusklight_online/game/ganondorf_sync.hpp"
#include "dusklight_online/game/actor_sync_session.hpp"
#include "dusklight_online/game/actor_sync_registry.hpp"
#include "dusklight_online/game/ganondorf_ownership.hpp"

#include "dusklight_online/logging.hpp"

#include "d/dolzel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "d/actor/d_a_b_gnd.h"
#include "d/d_com_inf_game.h"
#include "d/d_com_inf_actor.h"
#include "d/d_cc_uty.h"
#include "d/d_s_play.h"
#include "f_op/f_op_actor_mng.h"
#include "SSystem/SComponent/c_math.h"
#include "f_pc/f_pc_name.h"
#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "Z2AudioLib/Z2Instances.h"

namespace dusklight_online::game {
namespace {

constexpr std::string_view kEncounterId = "D_MN09B:GANONDORF_FINAL";
constexpr std::string_view kFinalStage = "D_MN09B";
constexpr uint32_t kStateMaxAgeTicks = 45;
constexpr uint32_t kPeerReadyMaxAgeTicks = 30;
constexpr int kFirstFinalAnimation = 0x13;
constexpr int kLastFinalAnimation = 0x5D;
constexpr int kGanondorfRunAnimation = 0x49;
constexpr int kGanondorfDashAnimationResource = 0x40;
constexpr int kGanondorfDownWaitAnimation = 0x2B;
constexpr int kMaxActionMode = 22;
constexpr int kMaxMoveMode = 64;
constexpr float kMaxCoordinate = 1.0e7f;
constexpr float kMaxSpeed = 1.0e5f;
constexpr float kTargetMaxDistance = 5000.0f;
constexpr float kTargetMaxHeightDifference = 1500.0f;

struct BossSnapshot {
    bool valid = false;
    uint32_t epoch = 0;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    int room = -1;
    int health = 0;
    int actionMode = 0;
    int moveMode = 0;
    int damageInvulnerabilityTimer = 0;
    int attackGate = 0;
    int attackJoint = 0;
    int defenseGate = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float oldX = 0.0f;
    float oldY = 0.0f;
    float oldZ = 0.0f;
    int shapeAngleX = 0;
    int shapeAngleY = 0;
    int shapeAngleZ = 0;
    int currentAngleX = 0;
    int currentAngleY = 0;
    int currentAngleZ = 0;
    float speedX = 0.0f;
    float speedY = 0.0f;
    float speedZ = 0.0f;
    float speedF = 0.0f;
    int animationId = 0;
    int animationMode = 0;
    float animationFrame = 0.0f;
    float animationRate = 1.0f;
    float animationMorph = 1.0f;
    float animationPreviousMorph = 1.0f;
    float animationMorphStep = 0.0f;
    float animationMorphRate = 0.0f;
    nlohmann::json combat;
    bool down = false;
    bool paused = false;
    int attackChance = 0;
    int demoMode = 0;
};

struct GanondorfSyncState : actor_sync::Session<BossSnapshot> {
    net::Transport* transport = nullptr;
    bool stageReady = false;
    bool hooksInstalled = false;
    b_gnd_class* boss = nullptr;
    GanondorfEndingSequence ending;
    b_gnd_class* endingBoss = nullptr;
    int endingRoom = -1;
    int ownerAttackGate = 0;
    int ownerAttackJoint = 0;
    int ownerDefenseGate = 0;
    cXyz targetPosition;
    float targetDistance = 0.0f;
    s16 targetAngleY = 0;
    int lastLoggedAction = -1;
    int lastLoggedMove = -1;
    int lastLoggedHealth = std::numeric_limits<int>::min();
};

GanondorfSyncState sSync;

DEFINE_HOOK_SYMBOL("dusk_online_b_gnd_action", void(b_gnd_class*),
                   GanondorfActionHook);
DEFINE_HOOK_SYMBOL("dusk_online_b_gnd_damage_check", void(b_gnd_class*),
                   GanondorfDamageHook);
DEFINE_HOOK_SYMBOL("dusk_online_b_gnd_demo_camera", void(b_gnd_class*),
                   GanondorfDemoCameraHook);

bool finite_bounded(float value, float limit) {
    return std::isfinite(value) && std::fabs(value) <= limit;
}

bool local_final_duel_ready() {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr && std::strcmp(stage, kFinalStage.data()) == 0 &&
           dComIfGs_isSaveDunSwitch(1);
}

bool peer_ready(const std::map<std::string, dusk::multiplayer::PeerPoseSnapshot>& poses,
                std::string_view peerId) {
    const auto found = poses.find(std::string(peerId));
    if (found == poses.end()) return false;
    const auto& pose = found->second;
    return pose.valid && pose.ageTicks <= kPeerReadyMaxAgeTicks &&
           pose.finalGanondorfReady && pose.stage == kFinalStage &&
           !pose.isWolf && !pose.isTransforming;
}

bool puppet_active(const b_gnd_class* boss) {
    return boss != nullptr && boss == sSync.boss && sSync.featureReady &&
           !sSync.authority.ownerLossLocked && sSync.stageReady && sSync.localReady &&
           !sSync.authority.ownerPeerId.empty() &&
           sSync.authority.ownerPeerId != sSync.localPeerId && sSync.snapshot.valid &&
           sSync.snapshot.epoch == sSync.authority.epoch &&
           sSync.snapshot.ageTicks <= kStateMaxAgeTicks;
}

int animation_resource_id(int animationId) {
    // Ganondorf records RRUN as the logical state but anm_init loads RDASH.
    return animationId == kGanondorfRunAnimation ?
        kGanondorfDashAnimationResource : animationId;
}

float normalize_animation_frame(mDoExt_McaMorfSO& morf, float frame) {
    const float start = morf.getStartFrame();
    const float end = morf.getEndFrame();
    if (morf.getPlayMode() != 2) return std::clamp(frame, start, end);
    const float span = end - start;
    if (span <= 0.0f) return start;
    while (frame > end) frame -= span;
    while (frame < start) frame += span;
    return frame;
}

// Transfer only named combat values. Resources, actor IDs, collision pointers,
// cameras and event-manager ownership always stay local.
nlohmann::json capture_combat(const b_gnd_class& boss) {
    return {
        {"mCounter", boss.mCounter},
        {"field_0xc5a", boss.field_0xc5a},
        {"field_0xc70", boss.field_0xc70},
        {"field_0xc72", boss.field_0xc72},
        {"field_0xc74", boss.field_0xc74},
        {"field_0xeac", boss.field_0xeac},
        {"field_0x1fc8", boss.field_0x1fc8},
        {"mGndArmRRotX", boss.mGndArmRRotX},
        {"mGndShoulderLRotY", boss.mGndShoulderLRotY},
        {"mGndLegRotX", boss.mGndLegRotX},
        {"field_0x26c2", boss.field_0x26c2},
        {"mGndBodyRotX", boss.mGndBodyRotX},
        {"mGndHeadRotZ", boss.mGndHeadRotZ},
        {"field_0xc7a", boss.field_0xc7a},
        {"field_0xc7b", boss.field_0xc7b},
        {"field_0xc7c", boss.field_0xc7c},
        {"field_0x1e08", boss.field_0x1e08},
        {"field_0x1e09", boss.field_0x1e09},
        {"field_0x2698", boss.field_0x2698},
        {"field_0x2699", boss.field_0x2699},
        {"field_0x1e0a", boss.field_0x1e0a},
        {"field_0x1e0c", boss.field_0x1e0c},
        {"field_0xeb0", boss.field_0xeb0},
        {"timers", {boss.field_0xc44[0], boss.field_0xc44[1], boss.field_0xc44[2], boss.field_0xc44[3], boss.field_0xc44[4], boss.field_0xc44[5], boss.field_0xc44[6], boss.field_0xc44[7], boss.field_0xc44[8], boss.field_0xc44[9]}}
    };
}

bool valid_combat(const nlohmann::json& state) {
    if (!state.is_object()) return false;
    const auto integer = [&](const char* key, int low, int high) {
        const auto it = state.find(key);
        if (it == state.end() || !it->is_number_integer()) return false;
        const auto value = it->get<int64_t>();
        return value >= low && value <= high;
    };
    if (!integer("mCounter", -32768, 32767)) return false;
    if (!integer("field_0xc5a", -32768, 32767)) return false;
    if (!integer("field_0xc70", -32768, 32767)) return false;
    if (!integer("field_0xc72", -32768, 32767)) return false;
    if (!integer("field_0xc74", -32768, 32767)) return false;
    if (!integer("field_0xeac", -32768, 32767)) return false;
    if (!integer("field_0x1fc8", -32768, 32767)) return false;
    if (!integer("mGndArmRRotX", -32768, 32767)) return false;
    if (!integer("mGndShoulderLRotY", -32768, 32767)) return false;
    if (!integer("mGndLegRotX", -32768, 32767)) return false;
    if (!integer("field_0x26c2", -32768, 32767)) return false;
    if (!integer("mGndBodyRotX", -32768, 32767)) return false;
    if (!integer("mGndHeadRotZ", -32768, 32767)) return false;
    if (!integer("field_0xc7a", 0, 255)) return false;
    if (!integer("field_0xc7b", 0, 255)) return false;
    if (!integer("field_0xc7c", 0, 255)) return false;
    if (!integer("field_0x1e08", 0, 255)) return false;
    if (!integer("field_0x1e09", 0, 255)) return false;
    if (!integer("field_0x2698", 0, 255)) return false;
    if (!integer("field_0x2699", 0, 255)) return false;
    if (!integer("field_0x1e0a", 0, 65535)) return false;
    if (!integer("field_0x1e0c", 0, 65535)) return false;
    if (!state.contains("field_0xeb0") || !state["field_0xeb0"].is_number() ||
        !finite_bounded(state["field_0xeb0"].get<float>(), kMaxSpeed)) return false;
    const auto timers = state.find("timers");
    if (timers == state.end() || !timers->is_array() || timers->size() != 10) return false;
    for (const auto& timer : *timers) {
        if (!timer.is_number_integer() || timer.get<int64_t>() < 0 ||
            timer.get<int64_t>() > 32767) return false;
    }
    return true;
}

void restore_combat(b_gnd_class& boss, const nlohmann::json& state) {
    boss.mCounter = state.at("mCounter").get<decltype(boss.mCounter)>();
    boss.field_0xc5a = state.at("field_0xc5a").get<decltype(boss.field_0xc5a)>();
    boss.field_0xc70 = state.at("field_0xc70").get<decltype(boss.field_0xc70)>();
    boss.field_0xc72 = state.at("field_0xc72").get<decltype(boss.field_0xc72)>();
    boss.field_0xc74 = state.at("field_0xc74").get<decltype(boss.field_0xc74)>();
    boss.field_0xeac = state.at("field_0xeac").get<decltype(boss.field_0xeac)>();
    boss.field_0x1fc8 = state.at("field_0x1fc8").get<decltype(boss.field_0x1fc8)>();
    boss.mGndArmRRotX = state.at("mGndArmRRotX").get<decltype(boss.mGndArmRRotX)>();
    boss.mGndShoulderLRotY = state.at("mGndShoulderLRotY").get<decltype(boss.mGndShoulderLRotY)>();
    boss.mGndLegRotX = state.at("mGndLegRotX").get<decltype(boss.mGndLegRotX)>();
    boss.field_0x26c2 = state.at("field_0x26c2").get<decltype(boss.field_0x26c2)>();
    boss.mGndBodyRotX = state.at("mGndBodyRotX").get<decltype(boss.mGndBodyRotX)>();
    boss.mGndHeadRotZ = state.at("mGndHeadRotZ").get<decltype(boss.mGndHeadRotZ)>();
    boss.field_0xc7a = state.at("field_0xc7a").get<decltype(boss.field_0xc7a)>();
    boss.field_0xc7b = state.at("field_0xc7b").get<decltype(boss.field_0xc7b)>();
    boss.field_0xc7c = state.at("field_0xc7c").get<decltype(boss.field_0xc7c)>();
    boss.field_0x1e08 = state.at("field_0x1e08").get<decltype(boss.field_0x1e08)>();
    boss.field_0x1e09 = state.at("field_0x1e09").get<decltype(boss.field_0x1e09)>();
    boss.field_0x2698 = state.at("field_0x2698").get<decltype(boss.field_0x2698)>();
    boss.field_0x2699 = state.at("field_0x2699").get<decltype(boss.field_0x2699)>();
    boss.field_0x1e0a = state.at("field_0x1e0a").get<decltype(boss.field_0x1e0a)>();
    boss.field_0x1e0c = state.at("field_0x1e0c").get<decltype(boss.field_0x1e0c)>();
    boss.field_0xeb0 = state.at("field_0xeb0").get<decltype(boss.field_0xeb0)>();
    for (int i = 0; i < 10; ++i) boss.field_0xc44[i] = state.at("timers")[i].get<s16>();
    // Handoffs never occur inside a demo, including the duel and final blow.
    boss.mDemoCamMode = 0;
    boss.mDemoCamTimer = 0;
    boss.mDemoCamSyncTicks = 0;
    // Preserve this client's real collision results. They were calculated
    // against its local puppet and have not yet been consumed as owner.
}

bool local_entry_pending(const b_gnd_class* boss) {
    return boss != nullptr &&
           ganondorf_local_entry_pending(boss->mNoDrawTimer, boss->mDemoCamMode);
}

void apply_snapshot(b_gnd_class* boss, bool takeover = false) {
    if (sSync.ending.pending || sSync.ending.active) return;
    if (local_entry_pending(boss)) return;
    if (!takeover && !puppet_active(boss)) return;
    const BossSnapshot& state = sSync.snapshot;
    if (takeover) restore_combat(*boss, state.combat);
    if (!takeover && state.sequence == sSync.lastAppliedStateSequence) return;
    sSync.lastAppliedStateSequence = state.sequence;
    auto* actor = static_cast<fopAc_ac_c*>(boss);

    actor->current.pos.set(state.x, state.y, state.z);
    actor->old.pos.set(state.oldX, state.oldY, state.oldZ);
    actor->shape_angle.set(static_cast<s16>(state.shapeAngleX),
                           static_cast<s16>(state.shapeAngleY),
                           static_cast<s16>(state.shapeAngleZ));
    actor->current.angle.set(static_cast<s16>(state.currentAngleX),
                             static_cast<s16>(state.currentAngleY),
                             static_cast<s16>(state.currentAngleZ));
    actor->speed.set(state.speedX, state.speedY, state.speedZ);
    actor->speedF = state.speedF;
    actor->health = static_cast<s16>(state.health);
    boss->mActionMode = static_cast<s16>(state.actionMode);
    boss->mMoveMode = static_cast<s16>(state.moveMode);
    boss->mDamageInvulnerabilityTimer =
        static_cast<s16>(state.damageInvulnerabilityTimer);
    boss->mPlaySpeed = state.animationRate;
    boss->mDrawHorse = FALSE;
    if (state.down) boss->onDownFlg(); else boss->offDownFlg();
    boss->field_0x2740 = static_cast<u8>(state.attackChance);
    // These are one-tick collision gates consumed later in Execute. Replaying
    // the owner's attack gate lets the puppet hurt only this client's Link;
    // damage_check remains skipped so this client cannot mutate boss health.
    const bool newCollisionTick =
        state.sequence != sSync.lastAppliedCollisionSequence;
    boss->field_0xc77 = newCollisionTick ? static_cast<u8>(state.attackGate) : 0;
    boss->field_0xc78 = newCollisionTick ? static_cast<u8>(state.attackJoint) : 0;
    boss->field_0xc79 = newCollisionTick ? static_cast<u8>(state.defenseGate) : 0;
    if (!takeover && state.paused) {
        actor->speed.zero();
        actor->speedF = 0;
        boss->field_0xc77 = boss->field_0xc79 = 0;
    }
    if (newCollisionTick) sSync.lastAppliedCollisionSequence = state.sequence;

    if (boss->mpModelMorf != nullptr) {
        auto& morf = *boss->mpModelMorf;
        if (boss->mAnmID != state.animationId) {
            auto* animation = static_cast<J3DAnmTransform*>(
                dComIfG_getObjectRes("B_gnd", animation_resource_id(state.animationId)));
            if (animation != nullptr) {
                boss->mAnmID = state.animationId;
                const float morphFrames = state.animationMorphRate > 0.0f ?
                    1.0f / state.animationMorphRate : 0.0f;
                morf.setAnm(animation, state.animationMode, morphFrames,
                            state.animationRate, 0.0f, -1.0f);
                morf.mCurMorf = state.animationMorph;
                morf.mPrevMorf = state.animationPreviousMorph;
                morf.mMorfStep = state.animationMorphStep;
                morf.field_0x34 = state.animationMorphRate;
                morf.setFrameF(normalize_animation_frame(morf, state.animationFrame));
            }
        }
        if (takeover || state.paused) {
            morf.setFrameF(normalize_animation_frame(morf, state.animationFrame));
            morf.mCurMorf = state.animationMorph;
            morf.mPrevMorf = state.animationPreviousMorph;
            morf.mMorfStep = state.animationMorphStep;
            morf.field_0x34 = state.animationMorphRate;
        }
        morf.setPlayMode(state.animationMode);
        morf.setPlaySpeed(!takeover && state.paused ? 0.0f : state.animationRate);
    }
}

// Only the owner runs combat. A stale snapshot never authorizes a second
// simulation. While relinquishing ownership the previous owner is also paused.
bool owns_combat(const b_gnd_class* boss) {
    return boss != nullptr && boss == sSync.boss && sSync.localReady &&
           !sSync.ending.pending && !sSync.ending.active &&
           !local_entry_pending(boss) && sSync.authority.simulates(sSync.localPeerId);
}

bool managed_boss(const b_gnd_class* boss) {
    return boss != nullptr && boss == sSync.boss && sSync.featureReady && sSync.localReady;
}

bool local_ending(const b_gnd_class* boss) {
    return boss != nullptr && boss == sSync.boss &&
           boss == sSync.endingBoss && sSync.ending.active;
}

void start_local_ending() {
    auto* boss = sSync.boss;
    if (!sSync.ending.pending || boss == nullptr || boss != sSync.endingBoss ||
        local_entry_pending(boss) || boss->mDemoCamMode != 0 ||
        dComIfGp_isPauseFlag() || dScnPly_c::isPause() || dComIfA_PauseCheck() ||
        dComIfGp_event_runCheck() || dComIfGp_getPlayer(0) == nullptr) return;
    auto* animation = static_cast<J3DAnmTransform*>(
        dComIfG_getObjectRes("B_gnd", kGanondorfDownWaitAnimation));
    if (boss->mpModelMorf == nullptr || animation == nullptr) return;

    // Begin at the native entry point even if the received snapshot is already
    // later in the ending. Modes 61+ assume this client's camera/Link were set
    // up by mode 60. No health change or synthetic finishing hit is performed.
    if (!sSync.ending.start(true)) return;
    boss->mAnmID = kGanondorfDownWaitAnimation;
    boss->mpModelMorf->setAnm(animation, 2, 0.0f, 1.0f, 0.0f, -1.0f);
    boss->mPlaySpeed = 1.0f;
    boss->mActionMode = 22;
    boss->mMoveMode = 0;
    boss->mDemoCamMode = 60;
    boss->mDemoCamTimer = 0;
    boss->mDemoCamSyncTicks = 0;
    boss->mDamageInvulnerabilityTimer = 10;
    boss->speed.zero();
    boss->speedF = 0;
    boss->mDrawHorse = FALSE;
    boss->offDownFlg();
    boss->field_0xc77 = boss->field_0xc78 = boss->field_0xc79 = 0;
    boss->field_0x2740 = 0;
    Z2GetAudioMgr()->bgmStop(30, 0);
    dusklight_online::log_info("Ganondorf synced ending cutscene started locally");
}

HookAction action_pre(ModContext*, void* args, void*, void*) {
    auto* boss = mods::arg<b_gnd_class*>(args, 0);
    if (local_ending(boss))
        return boss->mActionMode == 22 ? HOOK_CONTINUE : HOOK_SKIP_ORIGINAL;
    if (boss != nullptr && boss == sSync.endingBoss && sSync.ending.pending &&
        !local_entry_pending(boss)) {
        boss->speed.zero();
        boss->speedF = 0;
        boss->field_0xc77 = boss->field_0xc79 = 0;
        return HOOK_SKIP_ORIGINAL;
    }
    if (!managed_boss(boss) || owns_combat(boss)) return HOOK_CONTINUE;
    // Do not replace the fresh actor's entry state with a combat snapshot.
    // Its own demo_camera must first release this client's Link and camera.
    if (local_entry_pending(boss)) return HOOK_SKIP_ORIGINAL;
    if (puppet_active(boss)) apply_snapshot(boss);
    // A handoff must not continue moving/attacking on the relinquishing peer.
    if (sSync.authority.handoffPending || !sSync.snapshot.valid ||
        sSync.snapshot.ageTicks > kStateMaxAgeTicks) {
        boss->speed.zero();
        boss->speedF = 0;
        boss->field_0xc77 = boss->field_0xc79 = 0;
        if (boss->mpModelMorf) boss->mpModelMorf->setPlaySpeed(0);
    }
    return HOOK_SKIP_ORIGINAL;
}

void action_post(ModContext*, void* args, void*, void*) {
    auto* boss = mods::arg<b_gnd_class*>(args, 0);
    if (!owns_combat(boss)) return;
    if (boss->field_0xc77 != 0) {
        sSync.ownerAttackGate = boss->field_0xc77;
        sSync.ownerAttackJoint = boss->field_0xc78;
    }
    if (boss->field_0xc79 != 0) sSync.ownerDefenseGate = boss->field_0xc79;
}

HookAction damage_pre(ModContext*, void* args, void*, void*) {
    auto* boss = mods::arg<b_gnd_class*>(args, 0);
    if (local_ending(boss) || (boss != nullptr && boss == sSync.endingBoss &&
                              sSync.ending.pending)) return HOOK_SKIP_ORIGINAL;
    return managed_boss(boss) && !owns_combat(boss) ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

HookAction demo_camera_pre(ModContext*, void* args, void*, void*) {
    auto* boss = mods::arg<b_gnd_class*>(args, 0);
    if (local_ending(boss))
        return ganondorf_ending_demo(boss->mDemoCamMode) ? HOOK_CONTINUE : HOOK_SKIP_ORIGINAL;
    if (boss != nullptr && ganondorf_local_entry_demo(boss->mDemoCamMode))
        return HOOK_CONTINUE;
    return managed_boss(boss) && !owns_combat(boss) ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

nlohmann::json capture_state(const b_gnd_class& boss) {
    const auto& actor = static_cast<const fopAc_ac_c&>(boss);
    const float animationFrame = boss.mpModelMorf != nullptr ?
        boss.mpModelMorf->getFrame() : 0.0f;
    const float animationRate = boss.mpModelMorf != nullptr ?
        boss.mpModelMorf->getPlaySpeed() : 0.0f;
    const int animationMode = boss.mpModelMorf != nullptr ?
        boss.mpModelMorf->getPlayMode() : 0;
    const float animationMorph = boss.mpModelMorf != nullptr ?
        boss.mpModelMorf->mCurMorf : 1.0f;
    const float animationPreviousMorph = boss.mpModelMorf != nullptr ?
        boss.mpModelMorf->mPrevMorf : 1.0f;
    const float animationMorphStep = boss.mpModelMorf != nullptr ?
        boss.mpModelMorf->mMorfStep : 0.0f;
    const float animationMorphRate = boss.mpModelMorf != nullptr ?
        boss.mpModelMorf->field_0x34 : 0.0f;
    return {
        {"combat", capture_combat(boss)},
        {"paused", bool(dComIfGp_isPauseFlag() || dScnPly_c::isPause() || dComIfA_PauseCheck())},
        {"demo_mode", boss.mDemoCamMode},
        {"down", bool(const_cast<b_gnd_class&>(boss).checkDownFlg())},
        {"attack_chance", boss.field_0x2740},
        {"room", static_cast<int>(fopAcM_GetRoomNo(&actor))},
        {"health", actor.health}, {"action_mode", boss.mActionMode},
        {"move_mode", boss.mMoveMode}, {"damage_invuln", boss.mDamageInvulnerabilityTimer},
        {"attack_gate", sSync.ownerAttackGate}, {"attack_joint", sSync.ownerAttackJoint},
        {"defense_gate", sSync.ownerDefenseGate},
        {"position", {actor.current.pos.x, actor.current.pos.y, actor.current.pos.z}},
        {"old_position", {actor.old.pos.x, actor.old.pos.y, actor.old.pos.z}},
        {"shape_angle", {actor.shape_angle.x, actor.shape_angle.y, actor.shape_angle.z}},
        {"current_angle", {actor.current.angle.x, actor.current.angle.y,
                            actor.current.angle.z}},
        {"speed", {actor.speed.x, actor.speed.y, actor.speed.z}},
        {"forward_speed", actor.speedF},
        {"animation", {boss.mAnmID, animationMode, animationFrame, animationRate}},
        {"animation_blend", {animationMorph, animationPreviousMorph,
                             animationMorphStep, animationMorphRate}},
    };
}

bool parse_snapshot(const nlohmann::json& message, BossSnapshot& out) {
    if (!message.is_object() || !message.contains("state") ||
        !message["state"].is_object()) return false;
    const auto& state = message["state"];
    const auto read_float3 = [&](const char* key, float& x, float& y, float& z) {
        const auto found = state.find(key);
        if (found == state.end() || !found->is_array() || found->size() != 3) return false;
        x = (*found)[0].get<float>();
        y = (*found)[1].get<float>();
        z = (*found)[2].get<float>();
        return true;
    };
    const auto read_int3 = [&](const char* key, int& x, int& y, int& z) {
        const auto found = state.find(key);
        if (found == state.end() || !found->is_array() || found->size() != 3) return false;
        x = (*found)[0].get<int>();
        y = (*found)[1].get<int>();
        z = (*found)[2].get<int>();
        return true;
    };
    BossSnapshot parsed;
    if (!state.contains("combat") || !valid_combat(state["combat"])) return false;
    parsed.combat = state["combat"];
    parsed.down = state.value("down", false);
    parsed.paused = state.value("paused", false);
    parsed.attackChance = state.value("attack_chance", 0);
    parsed.demoMode = state.value("demo_mode", 0);
    if (parsed.demoMode < 0 || parsed.demoMode > 100) return false;
    if (parsed.attackChance < 0 || parsed.attackChance > 1) return false;
    parsed.valid = true;
    parsed.epoch = message.value("encounter_epoch", 0U);
    parsed.sequence = message.value("sequence", 0U);
    parsed.room = state.value("room", -1);
    parsed.health = state.value("health", 0);
    parsed.actionMode = state.value("action_mode", -1);
    parsed.moveMode = state.value("move_mode", -1);
    parsed.damageInvulnerabilityTimer = state.value("damage_invuln", 0);
    parsed.attackGate = state.value("attack_gate", 0);
    parsed.attackJoint = state.value("attack_joint", 0);
    parsed.defenseGate = state.value("defense_gate", 0);
    if (!read_float3("position", parsed.x, parsed.y, parsed.z) ||
        !read_float3("old_position", parsed.oldX, parsed.oldY, parsed.oldZ) ||
        !read_int3("shape_angle", parsed.shapeAngleX, parsed.shapeAngleY,
                   parsed.shapeAngleZ) ||
        !read_int3("current_angle", parsed.currentAngleX, parsed.currentAngleY,
                   parsed.currentAngleZ) ||
        !read_float3("speed", parsed.speedX, parsed.speedY, parsed.speedZ)) {
        return false;
    }
    parsed.speedF = state.value("forward_speed", 0.0f);
    const auto animation = state.find("animation");
    if (animation == state.end() || !animation->is_array() || animation->size() != 4) {
        return false;
    }
    parsed.animationId = (*animation)[0].get<int>();
    parsed.animationMode = (*animation)[1].get<int>();
    parsed.animationFrame = (*animation)[2].get<float>();
    parsed.animationRate = (*animation)[3].get<float>();
    const auto animationBlend = state.find("animation_blend");
    if (animationBlend == state.end() || !animationBlend->is_array() ||
        animationBlend->size() != 4) {
        return false;
    }
    parsed.animationMorph = (*animationBlend)[0].get<float>();
    parsed.animationPreviousMorph = (*animationBlend)[1].get<float>();
    parsed.animationMorphStep = (*animationBlend)[2].get<float>();
    parsed.animationMorphRate = (*animationBlend)[3].get<float>();

    const bool valid = parsed.epoch != 0 && parsed.sequence != 0 &&
        parsed.room >= -1 && parsed.room < 64 && parsed.health >= -1000 && parsed.health <= 1000 &&
        parsed.actionMode >= 0 && parsed.actionMode <= kMaxActionMode &&
        parsed.moveMode >= 0 && parsed.moveMode <= kMaxMoveMode &&
        parsed.damageInvulnerabilityTimer >= 0 && parsed.damageInvulnerabilityTimer <= 32767 &&
        parsed.attackGate >= 0 && parsed.attackGate <= 2 &&
        parsed.attackJoint >= 0 && parsed.attackJoint <= 3 &&
        parsed.defenseGate >= 0 && parsed.defenseGate <= 1 &&
        parsed.animationId >= kFirstFinalAnimation &&
        parsed.animationId <= kLastFinalAnimation &&
        parsed.animationMode >= 0 && parsed.animationMode <= 2 &&
        finite_bounded(parsed.x, kMaxCoordinate) && finite_bounded(parsed.y, kMaxCoordinate) &&
        finite_bounded(parsed.z, kMaxCoordinate) && finite_bounded(parsed.oldX, kMaxCoordinate) &&
        finite_bounded(parsed.oldY, kMaxCoordinate) && finite_bounded(parsed.oldZ, kMaxCoordinate) &&
        finite_bounded(parsed.speedX, kMaxSpeed) && finite_bounded(parsed.speedY, kMaxSpeed) &&
        finite_bounded(parsed.speedZ, kMaxSpeed) && finite_bounded(parsed.speedF, kMaxSpeed) &&
        finite_bounded(parsed.animationFrame, 10000.0f) &&
        finite_bounded(parsed.animationRate, 4.0f) &&
        finite_bounded(parsed.animationMorph, 2.0f) &&
        finite_bounded(parsed.animationPreviousMorph, 2.0f) &&
        finite_bounded(parsed.animationMorphStep, 2.0f) &&
        finite_bounded(parsed.animationMorphRate, 2.0f);
    if (!valid) return false;
    out = parsed;
    return true;
}

// SM64's default ownership follows the nearest player; its held/cutscene
// exceptions keep a coupled interaction on one participant. TP's sword duel,
// damage reaction and finishing demo likewise cannot change Link underneath.
bool ownership_locked(const b_gnd_class& boss) {
    return ganondorf_ownership_locked(boss.mActionMode, boss.mMoveMode, boss.mDemoCamMode);
}


void configure_actor_session() {
    sSync.id = kEncounterId;
    sSync.stage = kFinalStage;
    sSync.send = [](const nlohmann::json& message) {
        return sSync.transport != nullptr && sSync.transport->send(message);
    };
    sSync.parse = &parse_snapshot;
    sSync.transferLocked = [](const BossSnapshot& state) {
        return ganondorf_ownership_locked(state.actionMode, state.moveMode, state.demoMode) ||
               state.paused;
    };
    sSync.capture = [] { return capture_state(*sSync.boss); };
    sSync.canSimulate = [] { return owns_combat(sSync.boss); };
    sSync.canRelease = [] {
        return owns_combat(sSync.boss) && !ownership_locked(*sSync.boss) &&
               !dComIfGp_isPauseFlag() && !dScnPly_c::isPause() && !dComIfA_PauseCheck();
    };
    sSync.onGranted = [] {
        sSync.ownerAttackGate = sSync.ownerAttackJoint = sSync.ownerDefenseGate = 0;
        if (sSync.authority.takeoverPending && sSync.boss != nullptr &&
            !local_entry_pending(sSync.boss)) {
            apply_snapshot(sSync.boss, true);
            sSync.authority.takeoverPending = false;
        }
    };
    sSync.onSent = [] {
        sSync.ownerAttackGate = sSync.ownerAttackJoint = sSync.ownerDefenseGate = 0;
    };
    sSync.log = [](const std::string& text) { dusklight_online::log_info(sSync.id + ": " + text); };
    sSync.onSnapshot = [](std::string_view peerId, const BossSnapshot& snapshot) {
        const bool sameArea = sSync.localReady && sSync.boss != nullptr &&
            std::string(peerId) != sSync.localPeerId && snapshot.room >= 0 &&
            snapshot.room == fopAcM_GetRoomNo(sSync.boss) &&
            snapshot.room == dComIfGp_roomControl_getStayNo();
        if (sSync.ending.observe(snapshot.actionMode, snapshot.demoMode, sameArea)) {
            sSync.endingBoss = sSync.boss;
            sSync.endingRoom = snapshot.room;
            dusklight_online::log_info("Ganondorf ending cutscene queued from owner " +
                                      std::string(peerId));
        }
        if (snapshot.actionMode != sSync.lastLoggedAction ||
            snapshot.moveMode != sSync.lastLoggedMove ||
            snapshot.health != sSync.lastLoggedHealth) {
            sSync.lastLoggedAction = snapshot.actionMode;
            sSync.lastLoggedMove = snapshot.moveMode;
            sSync.lastLoggedHealth = snapshot.health;
            dusklight_online::log_info(
                "Ganondorf state epoch " + std::to_string(snapshot.epoch) +
                " sequence " + std::to_string(snapshot.sequence) +
                " action " + std::to_string(snapshot.actionMode) + "/" +
                std::to_string(snapshot.moveMode) + " health " +
                std::to_string(snapshot.health));
        }

    };
}

void update_owner_target(
    const std::map<std::string, dusk::multiplayer::PeerPoseSnapshot>& peerPoses) {
    if (!sSync.localReady || sSync.boss == nullptr ||
        sSync.authority.ownerPeerId != sSync.localPeerId) {
        sSync.targetPeerId.clear();
        sSync.targetRemote = false;
        return;
    }

    struct Candidate {
        std::string peerId;
        cXyz position;
        float distance = std::numeric_limits<float>::infinity();
        bool remote = false;
    };
    const cXyz bossPosition = sSync.boss->current.pos;
    const auto make_candidate = [&](std::string peerId, const cXyz& position,
                                    bool remote) -> Candidate {
        const float dx = position.x - bossPosition.x;
        const float dz = position.z - bossPosition.z;
        return {std::move(peerId), position, std::sqrt(dx * dx + dz * dz), remote};
    };

    std::vector<Candidate> candidates;
    if (const auto* player = dComIfGp_getPlayer(0); player != nullptr) {
        candidates.push_back(make_candidate(sSync.localPeerId, player->current.pos, false));
    }
    for (const auto& [peerId, pose] : peerPoses) {
        if (!peer_ready(peerPoses, peerId) || !pose.manualSyncReady) continue;
        cXyz position(pose.x, pose.y, pose.z);
        Candidate candidate = make_candidate(peerId, position, true);
        if (candidate.distance <= kTargetMaxDistance &&
            std::fabs(position.y - bossPosition.y) <= kTargetMaxHeightDifference) {
            candidates.push_back(std::move(candidate));
        }
    }
    if (candidates.empty()) {
        sSync.targetPeerId.clear();
        sSync.targetRemote = false;
        return;
    }

    const auto best = std::min_element(
        candidates.begin(), candidates.end(),
        [](const Candidate& lhs, const Candidate& rhs) { return lhs.distance < rhs.distance; });
    auto selected = best;
    const bool targetChanged = sSync.targetPeerId != selected->peerId;
    sSync.targetPeerId = selected->peerId;
    sSync.targetPosition = selected->position;
    sSync.targetDistance = selected->distance;
    sSync.targetAngleY = cM_atan2s(selected->position.x - bossPosition.x,
                                   selected->position.z - bossPosition.z);
    sSync.targetRemote = selected->remote;
    if (targetChanged) {
        dusklight_online::log_info(
            "Ganondorf target selected " + sSync.targetPeerId +
            (sSync.targetRemote ? " (remote, " : " (local, ") +
            std::to_string(static_cast<int>(sSync.targetDistance)) + " units)");
    }
}

}  // namespace

ModResult install_ganondorf_sync_hooks(net::Transport& transport, ModError* error) {
    sSync.transport = &transport;
    configure_actor_session();
    const ModResult action = mods::hook::add_pre<GanondorfActionHook>(&action_pre);
    const ModResult actionPost = action == MOD_OK ?
        mods::hook::add_post<GanondorfActionHook>(&action_post) : action;
    const ModResult damage = actionPost == MOD_OK ?
        mods::hook::add_pre<GanondorfDamageHook>(&damage_pre) : actionPost;
    const ModResult camera = damage == MOD_OK ?
        mods::hook::add_pre<GanondorfDemoCameraHook>(&demo_camera_pre) : damage;
    if (camera != MOD_OK) {
        uninstall_ganondorf_sync_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE,
                               "required final Ganondorf actor hooks are unavailable");
    }
    sSync.hooksInstalled = true;
    if (!actor_sync::registry().add(std::string(kEncounterId), {
            &update_ganondorf_sync, &consume_ganondorf_message,
            &ganondorf_peer_left, &reset_ganondorf_sync,
            [] { return (sSync.stageReady && local_final_duel_ready()) ||
                        !sSync.readyPeers.empty(); }})) {
        uninstall_ganondorf_sync_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE, "duplicate Ganondorf sync instance");
    }
    return MOD_OK;
}

void uninstall_ganondorf_sync_hooks() {
    actor_sync::registry().remove(kEncounterId);
    mods::hook::uninstall<GanondorfDemoCameraHook>();
    mods::hook::uninstall<GanondorfDamageHook>();
    mods::hook::uninstall<GanondorfActionHook>();
    reset_ganondorf_sync();
    sSync.transport = nullptr;
    sSync.hooksInstalled = false;
}

void update_ganondorf_sync(
    const net::Status& status, bool featureReady, bool stageReady,
    const std::map<std::string, dusk::multiplayer::PeerPoseSnapshot>& peerPoses) {
    sSync.status = status;
    sSync.featureReady = status.welcomed && featureReady;
    sSync.stageReady = stageReady;
    sSync.localPeerId = actor_sync::local_peer_id(status);
    sSync.localReady = sSync.featureReady && stageReady && local_final_duel_ready();
    // Once accepted, the local ending finishes even if the owner exits first.
    // Discard the latch on leaving the area or replacing the actor instance.
    sSync.boss = stageReady && local_final_duel_ready() &&
        (sSync.featureReady || sSync.ending.pending || sSync.ending.active) ?
        static_cast<b_gnd_class*>(fopAcM_SearchByName(fpcNm_B_GND_e)) : nullptr;
    if (sSync.boss == nullptr || sSync.boss != sSync.endingBoss ||
        dComIfGp_roomControl_getStayNo() != sSync.endingRoom) {
        sSync.ending = {};
        sSync.endingBoss = nullptr;
        sSync.endingRoom = -1;
    }
    sSync.localReady = sSync.localReady && sSync.boss != nullptr;
    start_local_ending();
    sSync.readyPeers.clear();
    for (const auto& [peerId, pose] : peerPoses) {
        (void)pose;
        if (peer_ready(peerPoses, peerId)) sSync.readyPeers.insert(peerId);
    }

    if (sSync.authority.takeoverPending && sSync.boss != nullptr &&
        !local_entry_pending(sSync.boss)) {
        apply_snapshot(sSync.boss, true);
        sSync.authority.takeoverPending = false;
    }
    update_owner_target(peerPoses);
    sSync.request_handoff();

    sSync.tick();
}

ApplyResult consume_ganondorf_message(const RoutedMessage& message) {
    return sSync.consume(message);
}

void ganondorf_peer_left(std::string_view peerId) {
    if (peerId == sSync.authority.ownerPeerId) sSync.abort_owner("owner_disconnected");
}

void reset_ganondorf_sync() {
    net::Transport* transport = sSync.transport;
    const bool hooksInstalled = sSync.hooksInstalled;
    sSync = {};
    sSync.transport = transport;
    sSync.hooksInstalled = hooksInstalled;
    configure_actor_session();
}

}  // namespace dusklight_online::game
