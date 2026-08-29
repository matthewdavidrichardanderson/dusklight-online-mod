#include "dusk/multiplayer/remote_link_dummy.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <string>

#include "SSystem/SComponent/c_math.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_remote_link.h"
#include "d/d_bg_s_acch.h"
#include "d/d_com_inf_game.h"
#include "dusk/logging.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "f_pc/f_pc_node.h"

namespace dusk::multiplayer {
namespace {

enum class ReceiverPresentationMode : uint8_t {
    FullMatrices,
    SemanticGameplay,
    HiddenUnsupported,
};

struct RemoteLinkActorDummy {
    fpc_ProcID actorId = fpcM_ERROR_PROCESS_ID_e;
    uint32_t logCount = 0;
    uint32_t traceApplyTicks = 0;
    uint32_t lastTraceSequence = 0;
    int clothesVariant = -1;
    int pendingClothesVariant = -1;
    bool isWolf = false;
    bool pendingIsWolf = false;
    bool transformAnchorValid = false;
    cXyz transformAnchorPos;
    uint32_t lastAudioEventSequence = 0;
    uint8_t recreateDelayTicks = 0;
    uint8_t visibleStableTicks = 0;
    bool recreatePending = false;
    uint32_t traceSeqGapCount = 0;
    uint32_t traceMatrixFalseCount = 0;
    uint32_t traceAgeSpikeCount = 0;
    uint32_t traceRepeatCount = 0;
    uint32_t traceMaxSeqDelta = 0;
    uint32_t traceMaxAge = 0;
    RemoteLinkMatrixSnapshot retainedMatrices;
    uint32_t retainedMatrixSequence = 0;
    ReceiverPresentationMode presentationMode = ReceiverPresentationMode::FullMatrices;
    bool semanticPresentationValid = false;
    uint32_t semanticSourceSequence = 0;
    uint32_t semanticTicksSinceSample = 0;
    uint32_t semanticBlendDurationTicks = 1;
    uint32_t semanticActionSequence = 0;
    int semanticRoom = -1;
    cXyz semanticStartPos;
    cXyz semanticTargetPos;
    cXyz semanticPresentedPos;
    s16 semanticStartYaw = 0;
    s16 semanticTargetYaw = 0;
    s16 semanticPresentedYaw = 0;
    std::array<int16_t, 10> semanticStartHatRotA{};
    std::array<int16_t, 10> semanticTargetHatRotA{};
    std::array<int16_t, 10> semanticPresentedHatRotA{};
    std::array<int16_t, 10> semanticStartHatRotB{};
    std::array<int16_t, 10> semanticTargetHatRotB{};
    std::array<int16_t, 10> semanticPresentedHatRotB{};
    std::array<int16_t, 3> semanticStartHatSwing{};
    std::array<int16_t, 3> semanticTargetHatSwing{};
    std::array<int16_t, 3> semanticPresentedHatSwing{};
    s16 semanticStartHatShapeY = 0;
    s16 semanticTargetHatShapeY = 0;
    s16 semanticPresentedHatShapeY = 0;
};

struct RemoteBodyCollision {
    bool initialized = false;
};

std::map<std::string, RemoteLinkActorDummy> sActorDummies;
std::map<std::string, RemoteBodyCollision> sBodyCollision;
uint32_t sActorSyncLogCount = 0;

constexpr f32 kRemoteHumanBodyRadius = 35.0f;
constexpr f32 kRemoteHumanBodyHeight = 180.0f;
constexpr f32 kRemoteWolfBodyRadius = 35.0f;
constexpr f32 kRemoteWolfBodyHeight = 95.0f;
constexpr f32 kRemoteWolfBodyHalfLength = 45.0f;
constexpr f32 kLocalLinkBodyRadius = 35.0f;
constexpr uint8_t kRemoteSpawnVisibleWarmupTicks = 3;
constexpr f32 kRemotePushMaxCorrection = 20.0f;
constexpr f32 kRemotePushMaxGroundDelta = 80.0f;

bool dummy_trace_enabled() {
    const char* value = std::getenv("DUSK_MP_DUMMY_TRACE");
    return value == nullptr ||
           !(std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
             std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
             std::strcmp(value, "OFF") == 0);
}

bool semantic_animation_slot_supported(int bck, int arc, f32 ratio) {
    if (ratio < 0.0f || ratio > 1.0f) return false;
    if (ratio <= 0.001f) return true;
    return bck > 0 && bck != 0xFFFF && arc == 0xFFFF;
}

bool phase3_semantic_pose_supported(const PeerPoseSnapshot& pose) {
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::HiddenUnsupported) return false;
    return pose.valid && pose.transformClothesWait == 0 &&
           (pose.itemActorKind == REMOTE_ITEM_ACTOR_NONE ||
            pose.itemActorKind == REMOTE_ITEM_ACTOR_BOOMERANG) &&
           (pose.rideActorKind == REMOTE_RIDE_ACTOR_NONE ||
            pose.rideActorKind == REMOTE_RIDE_ACTOR_SPINNER) &&
           semantic_animation_slot_supported(pose.underBck0, pose.underBckArc0,
                                             pose.underRatio0) &&
           semantic_animation_slot_supported(pose.underBck1, pose.underBckArc1,
                                             pose.underRatio1) &&
           semantic_animation_slot_supported(pose.underBck2, pose.underBckArc2,
                                             pose.underRatio2) &&
           semantic_animation_slot_supported(pose.upperBck0, pose.upperBckArc0,
                                             pose.upperRatio0) &&
           semantic_animation_slot_supported(pose.upperBck1, pose.upperBckArc1,
                                             pose.upperRatio1) &&
           semantic_animation_slot_supported(pose.upperBck2, pose.upperBckArc2,
                                             pose.upperRatio2);
}

ReceiverPresentationMode choose_presentation_mode(const PeerPoseSnapshot& pose) {
    if (!semantic_rendering_experiment_enabled()) {
        return ReceiverPresentationMode::FullMatrices;
    }
    return phase3_semantic_pose_supported(pose) ?
               ReceiverPresentationMode::SemanticGameplay :
               ReceiverPresentationMode::HiddenUnsupported;
}

s16 interpolate_angle(s16 start, s16 target, f32 step) {
    const s16 delta = static_cast<s16>(target - start);
    return static_cast<s16>(start + static_cast<s16>(static_cast<f32>(delta) * step));
}

template <size_t N>
void interpolate_angle_array(const std::array<int16_t, N>& start,
                             const std::array<int16_t, N>& target, f32 step,
                             std::array<int16_t, N>* output) {
    for (size_t i = 0; i < N; ++i) {
        (*output)[i] = interpolate_angle(start[i], target[i], step);
    }
}

void reset_semantic_presentation(RemoteLinkActorDummy& dummy,
                                 const PeerPoseSnapshot& pose, const cXyz& targetPos) {
    dummy.semanticPresentationValid = true;
    dummy.semanticSourceSequence = pose.sequence;
    dummy.semanticTicksSinceSample = 0;
    dummy.semanticBlendDurationTicks = 1;
    dummy.semanticRoom = pose.room;
    dummy.semanticStartPos = targetPos;
    dummy.semanticTargetPos = targetPos;
    dummy.semanticPresentedPos = targetPos;
    dummy.semanticStartYaw = static_cast<s16>(pose.angleY);
    dummy.semanticTargetYaw = dummy.semanticStartYaw;
    dummy.semanticPresentedYaw = dummy.semanticStartYaw;
    dummy.semanticStartHatRotA = pose.hatRotA;
    dummy.semanticTargetHatRotA = pose.hatRotA;
    dummy.semanticPresentedHatRotA = pose.hatRotA;
    dummy.semanticStartHatRotB = pose.hatRotB;
    dummy.semanticTargetHatRotB = pose.hatRotB;
    dummy.semanticPresentedHatRotB = pose.hatRotB;
    dummy.semanticStartHatSwing = pose.hatSwing;
    dummy.semanticTargetHatSwing = pose.hatSwing;
    dummy.semanticPresentedHatSwing = pose.hatSwing;
    dummy.semanticStartHatShapeY = static_cast<s16>(pose.hatShapeY);
    dummy.semanticTargetHatShapeY = dummy.semanticStartHatShapeY;
    dummy.semanticPresentedHatShapeY = dummy.semanticStartHatShapeY;
}

void update_semantic_presentation(RemoteLinkActorDummy& dummy,
                                  const PeerPoseSnapshot& pose, const cXyz& targetPos) {
    const f32 dx = targetPos.x - dummy.semanticPresentedPos.x;
    const f32 dy = targetPos.y - dummy.semanticPresentedPos.y;
    const f32 dz = targetPos.z - dummy.semanticPresentedPos.z;
    const bool discontinuity = dummy.semanticRoom != pose.room ||
                               dx * dx + dy * dy + dz * dz > 600.0f * 600.0f;
    if (!dummy.semanticPresentationValid || discontinuity) {
        reset_semantic_presentation(dummy, pose, targetPos);
        return;
    }

    if (pose.sequence != dummy.semanticSourceSequence) {
        dummy.semanticStartPos = dummy.semanticPresentedPos;
        dummy.semanticTargetPos = targetPos;
        dummy.semanticStartYaw = dummy.semanticPresentedYaw;
        dummy.semanticTargetYaw = static_cast<s16>(pose.angleY);
        dummy.semanticStartHatRotA = dummy.semanticPresentedHatRotA;
        dummy.semanticTargetHatRotA = pose.hatRotA;
        dummy.semanticStartHatRotB = dummy.semanticPresentedHatRotB;
        dummy.semanticTargetHatRotB = pose.hatRotB;
        dummy.semanticStartHatSwing = dummy.semanticPresentedHatSwing;
        dummy.semanticTargetHatSwing = pose.hatSwing;
        dummy.semanticStartHatShapeY = dummy.semanticPresentedHatShapeY;
        dummy.semanticTargetHatShapeY = static_cast<s16>(pose.hatShapeY);
        dummy.semanticBlendDurationTicks =
            std::clamp(dummy.semanticTicksSinceSample + 1, uint32_t{1}, uint32_t{4});
        dummy.semanticTicksSinceSample = 0;
        dummy.semanticSourceSequence = pose.sequence;
        dummy.semanticRoom = pose.room;
    }

    if (dummy.semanticTicksSinceSample < dummy.semanticBlendDurationTicks) {
        ++dummy.semanticTicksSinceSample;
    }
    const f32 step = std::clamp(
        static_cast<f32>(dummy.semanticTicksSinceSample) /
            static_cast<f32>(dummy.semanticBlendDurationTicks),
        0.0f, 1.0f);
    dummy.semanticPresentedPos.x =
        dummy.semanticStartPos.x + (dummy.semanticTargetPos.x - dummy.semanticStartPos.x) * step;
    dummy.semanticPresentedPos.y =
        dummy.semanticStartPos.y + (dummy.semanticTargetPos.y - dummy.semanticStartPos.y) * step;
    dummy.semanticPresentedPos.z =
        dummy.semanticStartPos.z + (dummy.semanticTargetPos.z - dummy.semanticStartPos.z) * step;
    dummy.semanticPresentedYaw =
        interpolate_angle(dummy.semanticStartYaw, dummy.semanticTargetYaw, step);
    interpolate_angle_array(dummy.semanticStartHatRotA, dummy.semanticTargetHatRotA, step,
                            &dummy.semanticPresentedHatRotA);
    interpolate_angle_array(dummy.semanticStartHatRotB, dummy.semanticTargetHatRotB, step,
                            &dummy.semanticPresentedHatRotB);
    interpolate_angle_array(dummy.semanticStartHatSwing, dummy.semanticTargetHatSwing, step,
                            &dummy.semanticPresentedHatSwing);
    dummy.semanticPresentedHatShapeY = interpolate_angle(
        dummy.semanticStartHatShapeY, dummy.semanticTargetHatShapeY, step);
}

void rebase_matrix(float* matrix, const cXyz& sourcePos, const cXyz& presentedPos,
                   s16 yawDelta) {
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const f32 oldRow0[4] = {matrix[0], matrix[1], matrix[2], matrix[3]};
    const f32 oldRow2[4] = {matrix[8], matrix[9], matrix[10], matrix[11]};
    for (int column = 0; column < 3; ++column) {
        matrix[column] = cosine * oldRow0[column] + sine * oldRow2[column];
        matrix[8 + column] = -sine * oldRow0[column] + cosine * oldRow2[column];
    }
    const f32 relativeX = oldRow0[3] - sourcePos.x;
    const f32 relativeZ = oldRow2[3] - sourcePos.z;
    matrix[3] = presentedPos.x + cosine * relativeX + sine * relativeZ;
    matrix[7] += presentedPos.y - sourcePos.y;
    matrix[11] = presentedPos.z - sine * relativeX + cosine * relativeZ;
}

void rebase_model(RemoteModelMatrixSnapshot& model, const cXyz& sourcePos,
                  const cXyz& presentedPos, s16 yawDelta) {
    if (!model.valid) return;
    rebase_matrix(model.base.data(), sourcePos, presentedPos, yawDelta);
    for (size_t offset = 0; offset + 12 <= model.joints.size(); offset += 12) {
        rebase_matrix(model.joints.data() + offset, sourcePos, presentedPos, yawDelta);
    }
    for (size_t offset = 0; offset + 12 <= model.weights.size(); offset += 12) {
        rebase_matrix(model.weights.data() + offset, sourcePos, presentedPos, yawDelta);
    }
}

cXyz rebase_point(const cXyz& point, const cXyz& sourcePos, const cXyz& presentedPos,
                  s16 yawDelta) {
    const f32 sine = cM_ssin(yawDelta);
    const f32 cosine = cM_scos(yawDelta);
    const f32 relativeX = point.x - sourcePos.x;
    const f32 relativeZ = point.z - sourcePos.z;
    return cXyz(presentedPos.x + cosine * relativeX + sine * relativeZ,
                point.y + presentedPos.y - sourcePos.y,
                presentedPos.z - sine * relativeX + cosine * relativeZ);
}

RemoteLinkMatrixSnapshot rebase_semantic_attachments(
    const RemoteLinkMatrixSnapshot& source, const cXyz& sourcePos, s16 sourceYaw,
    const cXyz& presentedPos, s16 presentedYaw) {
    RemoteLinkMatrixSnapshot rebased = source;
    const s16 yawDelta = static_cast<s16>(presentedYaw - sourceYaw);
    RemoteModelMatrixSnapshot* attachments[] = {
        &rebased.sword, &rebased.sheath, &rebased.shield, &rebased.heldItem,
        &rebased.hookTip, &rebased.hookSubItem, &rebased.hookSubTip,
        &rebased.arrow, &rebased.kantera, &rebased.kanteraGlow,
        &rebased.itemActor, &rebased.rideActor,
    };
    for (RemoteModelMatrixSnapshot* model : attachments) {
        rebase_model(*model, sourcePos, presentedPos, yawDelta);
    }
    return rebased;
}

f32 clamp_f32(f32 value, f32 min, f32 max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

bool correct_remote_body_push_position(fopAc_ac_c* playerActor, const cXyz& originalPos,
                                       cXyz* candidatePos, bool localIsWolf) {
    if (playerActor == nullptr || candidatePos == nullptr) {
        return false;
    }

    cXyz oldPos = originalPos;
    cXyz speed(candidatePos->x - originalPos.x, candidatePos->y - originalPos.y,
               candidatePos->z - originalPos.z);
    dBgS_AcchCir acchCir;
    dBgS_ObjAcch acch;
    acchCir.SetWall(localIsWolf ? 75.0f : kRemoteHumanBodyHeight,
                    localIsWolf ? kRemoteWolfBodyRadius : kLocalLinkBodyRadius);
    acch.Set(candidatePos, &oldPos, playerActor, 1, &acchCir, &speed, NULL, NULL);
    acch.CrrPos(dComIfG_Bgsp());

    const f32 correctionX = candidatePos->x - (originalPos.x + speed.x);
    const f32 correctionZ = candidatePos->z - (originalPos.z + speed.z);
    const f32 correctionSq = correctionX * correctionX + correctionZ * correctionZ;
    if (correctionSq > kRemotePushMaxCorrection * kRemotePushMaxCorrection) {
        return false;
    }

    if (acch.ChkGroundHit() &&
        std::fabs(acch.GetGroundH() - originalPos.y) <= kRemotePushMaxGroundDelta)
    {
        candidatePos->y = originalPos.y;
        return true;
    }

    return !acch.ChkWallHit();
}

void get_body_segment(f32 x, f32 z, s16 angleY, bool isWolf, f32* outAx, f32* outAz,
                      f32* outBx, f32* outBz) {
    if (!isWolf) {
        *outAx = x;
        *outAz = z;
        *outBx = x;
        *outBz = z;
        return;
    }

    const f32 forwardX = cM_ssin(angleY);
    const f32 forwardZ = cM_scos(angleY);
    *outAx = x - forwardX * kRemoteWolfBodyHalfLength;
    *outAz = z - forwardZ * kRemoteWolfBodyHalfLength;
    *outBx = x + forwardX * kRemoteWolfBodyHalfLength;
    *outBz = z + forwardZ * kRemoteWolfBodyHalfLength;
}

void closest_points_on_segments_xz(f32 aX, f32 aZ, f32 bX, f32 bZ, f32 cX, f32 cZ,
                                   f32 dX, f32 dZ, f32* outLocalX, f32* outLocalZ,
                                   f32* outRemoteX, f32* outRemoteZ) {
    const f32 ux = bX - aX;
    const f32 uz = bZ - aZ;
    const f32 vx = dX - cX;
    const f32 vz = dZ - cZ;
    const f32 wx = aX - cX;
    const f32 wz = aZ - cZ;

    const f32 aa = ux * ux + uz * uz;
    const f32 bb = ux * vx + uz * vz;
    const f32 cc = vx * vx + vz * vz;
    const f32 dd = ux * wx + uz * wz;
    const f32 ee = vx * wx + vz * wz;
    const f32 denom = aa * cc - bb * bb;

    f32 s = 0.0f;
    if (denom > 0.0001f) {
        s = clamp_f32((bb * ee - cc * dd) / denom, 0.0f, 1.0f);
    }

    f32 t = 0.0f;
    if (cc > 0.0001f) {
        t = clamp_f32((bb * s + ee) / cc, 0.0f, 1.0f);
    }

    if (aa > 0.0001f) {
        s = clamp_f32((bb * t - dd) / aa, 0.0f, 1.0f);
    }

    *outLocalX = aX + ux * s;
    *outLocalZ = aZ + uz * s;
    *outRemoteX = cX + vx * t;
    *outRemoteZ = cZ + vz * t;
}

void apply_remote_body_push(const PeerPoseSnapshot& pose, fopAc_ac_c* playerActor) {
    if (playerActor == nullptr) {
        return;
    }

    const bool localIsWolf =
        fopAcM_GetName(playerActor) == fpcNm_ALINK_e &&
        static_cast<daAlink_c*>(playerActor)->checkWolf();
    const f32 localHeight = localIsWolf ? kRemoteWolfBodyHeight : kRemoteHumanBodyHeight;
    const f32 remoteHeight = pose.isWolf ? kRemoteWolfBodyHeight : kRemoteHumanBodyHeight;
    const f32 localMinY = playerActor->current.pos.y - 20.0f;
    const f32 localMaxY = playerActor->current.pos.y + localHeight;
    const f32 remoteMinY = pose.y - 20.0f;
    const f32 remoteMaxY = pose.y + remoteHeight;
    if (localMaxY < remoteMinY || remoteMaxY < localMinY) {
        return;
    }

    f32 localAx;
    f32 localAz;
    f32 localBx;
    f32 localBz;
    f32 remoteAx;
    f32 remoteAz;
    f32 remoteBx;
    f32 remoteBz;
    get_body_segment(playerActor->current.pos.x, playerActor->current.pos.z,
                     playerActor->shape_angle.y, localIsWolf, &localAx, &localAz, &localBx,
                     &localBz);
    get_body_segment(pose.x, pose.z, static_cast<s16>(pose.angleY), pose.isWolf, &remoteAx,
                     &remoteAz, &remoteBx, &remoteBz);

    f32 localClosestX;
    f32 localClosestZ;
    f32 remoteClosestX;
    f32 remoteClosestZ;
    closest_points_on_segments_xz(localAx, localAz, localBx, localBz, remoteAx, remoteAz,
                                  remoteBx, remoteBz, &localClosestX, &localClosestZ,
                                  &remoteClosestX, &remoteClosestZ);

    const f32 combinedRadius =
        (localIsWolf ? kRemoteWolfBodyRadius : kLocalLinkBodyRadius) +
        (pose.isWolf ? kRemoteWolfBodyRadius : kRemoteHumanBodyRadius);
    f32 dx = localClosestX - remoteClosestX;
    f32 dz = localClosestZ - remoteClosestZ;
    f32 distSq = dx * dx + dz * dz;
    if (distSq >= combinedRadius * combinedRadius) {
        return;
    }

    if (distSq < 0.0001f) {
        dx = cM_ssin(playerActor->shape_angle.y);
        dz = cM_scos(playerActor->shape_angle.y);
        distSq = dx * dx + dz * dz;
    }

    const f32 dist = std::sqrt(distSq);
    if (dist < 0.0001f) {
        return;
    }

    const f32 push = combinedRadius - dist;
    const cXyz originalPos = playerActor->current.pos;
    cXyz candidatePos = originalPos;
    candidatePos.x += (dx / dist) * push;
    candidatePos.z += (dz / dist) * push;
    if (correct_remote_body_push_position(playerActor, originalPos, &candidatePos,
                                          localIsWolf))
    {
        playerActor->current.pos = candidatePos;
    }
}

void update_actor_dummy_collision(const std::string& peerId, const PeerPoseSnapshot& pose) {
    if (!remote_collision_enabled()) {
        sBodyCollision.erase(peerId);
        return;
    }

    fopAc_ac_c* playerActor = dComIfGp_getPlayer(0);
    if (playerActor == nullptr) {
        return;
    }

    sBodyCollision[peerId].initialized = true;
    apply_remote_body_push(pose, playerActor);
}

bool remote_link_actor_pose_supported(const PeerPoseSnapshot& pose) {
    return pose.valid && pose.ageTicks <= 30;
}

daRemoteLink_c* find_remote_link_actor(RemoteLinkActorDummy& dummy) {
    if (dummy.actorId == fpcM_ERROR_PROCESS_ID_e) {
        return nullptr;
    }

    fopAc_ac_c* actor = fopAcM_SearchByID(dummy.actorId);
    if (actor == nullptr) {
        if (!fpcM_IsCreating(dummy.actorId)) {
            dummy.actorId = fpcM_ERROR_PROCESS_ID_e;
        }
        return nullptr;
    }

    if (fopAcM_GetName(actor) != fpcNm_REMOTE_LINK_e) {
        DuskLog.warn("Multiplayer remote Link actor: proc id {} resolved to unexpected actor {}",
                     dummy.actorId, fopAcM_GetName(actor));
        dummy.actorId = fpcM_ERROR_PROCESS_ID_e;
        return nullptr;
    }

    return static_cast<daRemoteLink_c*>(actor);
}

void destroy_remote_link_actor_dummy(const std::string& peerId) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        sBodyCollision.erase(peerId);
        return;
    }

    if (it->second.actorId != fpcM_ERROR_PROCESS_ID_e) {
        fopAcM_delete(it->second.actorId);
    }
    sActorDummies.erase(it);
    sBodyCollision.erase(peerId);
}

fpc_ProcID create_remote_link_actor(u32 actorParams, cXyz* pos, s8 room, csXyz* angle,
                                    cXyz* scale) {
    if (!daRemoteLink_c::canReserveSlot()) {
        static uint32_t sRemoteLinkCapWaitLogCount = 0;
        if (sRemoteLinkCapWaitLogCount < 40) {
            ++sRemoteLinkCapWaitLogCount;
            DuskLog.warn("Multiplayer remote Link actor: spawn delayed, live actor cap full");
        }
        return fpcM_ERROR_PROCESS_ID_e;
    }

    layer_class* savedLayer = fpcLy_CurrentLayer();
    base_process_class* playScene = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
    if (playScene != nullptr) {
        fpcLy_SetCurrentLayer(&reinterpret_cast<process_node_class*>(playScene)->layer);
    }

    fpc_ProcID id =
        fopAcM_create(fpcNm_REMOTE_LINK_e, actorParams, pos, room, angle, scale, -1);

    fpcLy_SetCurrentLayer(savedLayer);
    return id;
}

}  // namespace

void sync_remote_link_actor_dummies(const std::map<std::string, PeerPoseSnapshot>& poses) {
    const char* localStage = dComIfGp_getStartStageName();
    if (localStage == nullptr || dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() ||
        fopOvlpM_IsDoingReq())
    {
        destroy_all_remote_link_dummies();
        return;
    }

    for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
        const std::string peerId = it->first;
        if (poses.find(peerId) == poses.end()) {
            ++it;
            destroy_remote_link_actor_dummy(peerId);
            continue;
        }
        ++it;
    }

    for (const auto& entry : poses) {
        const std::string& peerId = entry.first;
        const PeerPoseSnapshot& pose = entry.second;
        const bool supported = remote_link_actor_pose_supported(pose) &&
                               pose.stage == localStage &&
                               dComIfGp_roomControl_checkRoomDisp(pose.room);
        if (!supported) {
            auto existing = sActorDummies.find(peerId);
            if (pose.valid && pose.isTransforming && existing != sActorDummies.end() &&
                choose_presentation_mode(pose) !=
                    ReceiverPresentationMode::HiddenUnsupported) {
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            destroy_remote_link_actor_dummy(peerId);
            continue;
        }

        RemoteLinkActorDummy& dummy = sActorDummies[peerId];
        if (dummy.visibleStableTicks < kRemoteSpawnVisibleWarmupTicks) {
            ++dummy.visibleStableTicks;
        }
        daRemoteLink_c* actor = find_remote_link_actor(dummy);
        const ReceiverPresentationMode presentationMode = choose_presentation_mode(pose);
        if (presentationMode == ReceiverPresentationMode::HiddenUnsupported) {
            sBodyCollision.erase(peerId);
            if (actor != nullptr) {
                actor->setRemotePresentationVisible(false);
            }
            dummy.presentationMode = presentationMode;
            dummy.semanticPresentationValid = false;
            dummy.semanticActionSequence = 0;
            for (const RemoteAudioEvent& event : pose.audioEvents) {
                dummy.lastAudioEventSequence =
                    std::max(dummy.lastAudioEventSequence, event.sequence);
            }
            continue;
        }
        const bool semanticTransform =
            presentationMode == ReceiverPresentationMode::SemanticGameplay &&
            pose.isTransforming;
        const bool anchorWolfToHumanTransform =
            !semanticTransform && pose.isTransforming && pose.transformFromWolf &&
            !pose.transformToWolf;
        if (anchorWolfToHumanTransform && !dummy.transformAnchorValid) {
            dummy.transformAnchorPos.set(pose.x, pose.y, pose.z);
            dummy.transformAnchorValid = true;
            if (sActorSyncLogCount < 20) {
                ++sActorSyncLogCount;
                DuskLog.info("Multiplayer remote Link actor: wolf->human transform anchor peer={} "
                             "pos=({}, {}, {}) seq={}",
                             peerId, pose.x, pose.y, pose.z, pose.sequence);
            }
        } else if (!pose.isTransforming) {
            dummy.transformAnchorValid = false;
        }

        const cXyz sourceActorPos(pose.x, pose.y, pose.z);
        const cXyz actorPos =
            anchorWolfToHumanTransform && dummy.transformAnchorValid
                ? dummy.transformAnchorPos
                : sourceActorPos;
        if (dummy.clothesVariant != -1 &&
            (dummy.clothesVariant != pose.clothesVariant || dummy.isWolf != pose.isWolf))
        {
            if (!dummy.recreatePending) {
                if (dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                    fopAcM_delete(dummy.actorId);
                }
                dummy.pendingClothesVariant = pose.clothesVariant;
                dummy.pendingIsWolf = pose.isWolf;
                dummy.recreateDelayTicks = 3;
                dummy.recreatePending = true;
                if (sActorSyncLogCount < 20) {
                    ++sActorSyncLogCount;
                    DuskLog.info("Multiplayer remote Link actor: recreate requested peer={} "
                                 "old_clothes={} new_clothes={} old_wolf={} new_wolf={} "
                                 "old_id={}",
                                 peerId, dummy.clothesVariant, pose.clothesVariant,
                                 dummy.isWolf, pose.isWolf, dummy.actorId);
                }
            }

            if (actor != nullptr || dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                update_actor_dummy_collision(peerId, pose);
                continue;
            }

            dummy.clothesVariant = -1;
            dummy.isWolf = false;
            if (dummy.recreateDelayTicks > 0) {
                --dummy.recreateDelayTicks;
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            dummy.recreatePending = false;
        }

        if (actor == nullptr) {
            if (dummy.actorId != fpcM_ERROR_PROCESS_ID_e) {
                continue;
            }

            csXyz angle(0, static_cast<s16>(pose.angleY), 0);
            cXyz scale(1.0f, 1.0f, 1.0f);
            dummy.clothesVariant = pose.clothesVariant;
            dummy.isWolf = pose.isWolf;
            dummy.pendingClothesVariant = -1;
            dummy.pendingIsWolf = false;
            dummy.recreateDelayTicks = 0;
            dummy.recreatePending = false;
            dummy.presentationMode = ReceiverPresentationMode::FullMatrices;
            dummy.semanticPresentationValid = false;
            dummy.semanticActionSequence = 0;
            const u32 actorParams =
                static_cast<u32>(pose.clothesVariant & 0xFF) | (pose.isWolf ? 0x100 : 0);
            cXyz spawnPos = actorPos;
            if (dummy.visibleStableTicks < kRemoteSpawnVisibleWarmupTicks) {
                if (sActorSyncLogCount < 20) {
                    ++sActorSyncLogCount;
                    DuskLog.info("Multiplayer remote Link actor: spawn warming peer={} "
                                 "ticks={}/{} stage={} room={} seq={}",
                                 peerId, dummy.visibleStableTicks,
                                 kRemoteSpawnVisibleWarmupTicks, pose.stage, pose.room,
                                 pose.sequence);
                }
                update_actor_dummy_collision(peerId, pose);
                continue;
            }
            dummy.actorId = create_remote_link_actor(actorParams, &spawnPos,
                                                     static_cast<s8>(pose.room), &angle,
                                                     &scale);
            if (sActorSyncLogCount < 20) {
                ++sActorSyncLogCount;
                DuskLog.info(
                    "Multiplayer remote Link actor: spawn requested peer={} id={} clothes={} "
                    "wolf={} pos=({}, {}, {}) room={} angleY={}",
                    peerId, dummy.actorId, pose.clothesVariant, pose.isWolf, actorPos.x,
                    actorPos.y, actorPos.z, pose.room, pose.angleY);
            }
            continue;
        }

        update_actor_dummy_collision(peerId, pose);
        if (pose.linkMatricesFresh && pose.linkMatrices.valid &&
            dummy.retainedMatrixSequence != pose.sequence) {
            dummy.retainedMatrices = pose.linkMatrices;
            dummy.retainedMatrixSequence = pose.sequence;
        }
        actor->setRemotePresentationVisible(true);
        const bool matricesRequired =
            presentationMode == ReceiverPresentationMode::FullMatrices;
        if (matricesRequired &&
            (!pose.linkMatricesFresh || !pose.linkMatrices.body.valid)) {
            if (dummy_trace_enabled()) {
                const uint32_t sequenceDelta =
                    dummy.lastTraceSequence != 0 ? pose.sequence - dummy.lastTraceSequence : 0;
                dummy.lastTraceSequence = pose.sequence;
                if (sequenceDelta > 1) {
                    ++dummy.traceSeqGapCount;
                    if (sequenceDelta > dummy.traceMaxSeqDelta) {
                        dummy.traceMaxSeqDelta = sequenceDelta;
                    }
                } else if (sequenceDelta == 0) {
                    ++dummy.traceRepeatCount;
                }
                if (pose.ageTicks > 1) {
                    ++dummy.traceAgeSpikeCount;
                    if (pose.ageTicks > dummy.traceMaxAge) {
                        dummy.traceMaxAge = pose.ageTicks;
                    }
                }

                ++dummy.traceApplyTicks;
                if ((dummy.traceApplyTicks % 60) == 1) {
                    DuskLog.info(
                        "Multiplayer dummy trace summary peer={} id={} applies={} seq={} "
                        "last_delta={} gaps={} max_gap={} repeats={} age={} age_spikes={} "
                        "max_age={} matrix={} matrix_fresh=false matrix_false={} "
                        "pos=({}, {}, {}) room={} proc={} under_bck={} under_frame={} "
                        "upper_bck={} upper_frame={}",
                        peerId, dummy.actorId, dummy.traceApplyTicks, pose.sequence,
                        sequenceDelta, dummy.traceSeqGapCount, dummy.traceMaxSeqDelta,
                        dummy.traceRepeatCount, pose.ageTicks, dummy.traceAgeSpikeCount,
                        dummy.traceMaxAge, pose.linkMatrices.valid, dummy.traceMatrixFalseCount,
                        pose.x, pose.y, pose.z, pose.room, pose.procId, pose.underBck0,
                        pose.underFrame0, pose.upperBck2, pose.upperFrame2);
                }
            }
            continue;
        }
        cXyz presentedActorPos = actorPos;
        s16 presentedAngleY = static_cast<s16>(pose.angleY);
        const std::array<int16_t, 10>* presentedHatRotA = &pose.hatRotA;
        const std::array<int16_t, 10>* presentedHatRotB = &pose.hatRotB;
        const std::array<int16_t, 3>* presentedHatSwing = &pose.hatSwing;
        s16 presentedHatShapeY = static_cast<s16>(pose.hatShapeY);
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay) {
            if (semanticTransform) {
                // Vanilla deliberately shifts current.pos when the model swaps between the
                // wolf and human phases while keeping joint zero visually continuous. The
                // old matrix renderer anchored the actor during wolf->human because its
                // streamed matrices were already world-space. Interpolating or anchoring
                // that actor-position correction in semantic mode applies it a second time
                // when bodyRoot is rebased, producing the large forward/backward jump.
                reset_semantic_presentation(dummy, pose, actorPos);
            } else {
                update_semantic_presentation(dummy, pose, actorPos);
            }
            presentedActorPos = dummy.semanticPresentedPos;
            presentedAngleY = dummy.semanticPresentedYaw;
            presentedHatRotA = &dummy.semanticPresentedHatRotA;
            presentedHatRotB = &dummy.semanticPresentedHatRotB;
            presentedHatSwing = &dummy.semanticPresentedHatSwing;
            presentedHatShapeY = dummy.semanticPresentedHatShapeY;
        } else {
            dummy.semanticPresentationValid = false;
            dummy.semanticActionSequence = 0;
        }
        actor->setRemotePose(presentedActorPos, presentedAngleY, static_cast<s8>(pose.room));
        const bool applyActionState =
            presentationMode != ReceiverPresentationMode::SemanticGameplay ||
            dummy.semanticActionSequence != pose.sequence;
        if (applyActionState) {
            actor->setRemoteActionState(pose.procId, pose.procVar0, pose.procVar1, pose.procVar2,
                                        pose.procVar3, pose.procVar5, pose.underFrame,
                                        static_cast<u16>(pose.underBck0), pose.underFrame0,
                                        pose.underRate0, pose.underRatio0,
                                        static_cast<u16>(pose.underBck1), pose.underFrame1,
                                        pose.underRate1, pose.underRatio1,
                                        static_cast<u16>(pose.underBck2), pose.underFrame2,
                                        pose.underRate2, pose.underRatio2,
                                        static_cast<u16>(pose.upperBck0), pose.upperFrame0,
                                        pose.upperRate0, pose.upperRatio0,
                                        static_cast<u16>(pose.upperBck1), pose.upperFrame1,
                                        pose.upperRate1, pose.upperRatio1,
                                        static_cast<u16>(pose.upperBck2), pose.upperFrame2,
                                        pose.upperRate2, pose.upperRatio2, pose.equipItem,
                                        pose.swordVariant, pose.shieldVariant, pose.swordDraw,
                                        pose.shieldDraw, pose.shieldGuardActive,
                                        pose.swordHandAttached, pose.shieldHandAttached,
                                        pose.swordOut,
                                        pose.heavyBoots, pose.itemDraw, pose.kanteraDraw,
                                        false, false, false, false, false, pose.itemActorKind,
                                        pose.itemActorBombExTime, pose.itemActorBombFlash,
                                        pose.rideActorKind);
            actor->setRemoteAnimationSourceState(
                static_cast<u16>(pose.underBckArc0), static_cast<u16>(pose.underBckArc1),
                static_cast<u16>(pose.underBckArc2), static_cast<u16>(pose.upperBckArc0),
                static_cast<u16>(pose.upperBckArc1), static_cast<u16>(pose.upperBckArc2));
            if (presentationMode == ReceiverPresentationMode::SemanticGameplay) {
                dummy.semanticActionSequence = pose.sequence;
            }
        }
        // Animation resources/frames change only with a new received sample, but
        // the semantic presentation position keeps advancing between samples.
        // Rebase and apply the body root every tick so root-motion animations
        // cannot drift ahead of the interpolated actor and snap back on exit.
        const cXyz presentedBodyRoot = rebase_point(
            cXyz(pose.bodyRootX, pose.bodyRootY, pose.bodyRootZ), sourceActorPos,
            presentedActorPos,
            static_cast<s16>(presentedAngleY - static_cast<s16>(pose.angleY)));
        actor->setRemoteBodyState(
            static_cast<s16>(pose.shapeAngleX), static_cast<s16>(pose.shapeAngleZ),
            static_cast<s16>(pose.bodyAngleX), static_cast<s16>(pose.bodyAngleY),
            static_cast<s16>(pose.bodyAngleZ), static_cast<s16>(pose.bodyTwistY),
            static_cast<s16>(pose.neckJointX), static_cast<s16>(pose.neckJointY),
            static_cast<s16>(pose.neckJointZ), static_cast<s16>(pose.lowerJointX),
            static_cast<s16>(pose.lowerJointZ), static_cast<s16>(pose.rootJointX),
            static_cast<s16>(pose.rootJointZ), static_cast<u8>(pose.blendMode),
            pose.upperSavedRatio, pose.bodyRootValid, presentedBodyRoot, pose.legIkAngles,
            pose.armIkAngles, pose.armRotA, pose.armRotB);
        actor->setRemoteHatState(*presentedHatRotA, *presentedHatRotB, *presentedHatSwing,
                                 presentedHatShapeY);
        if (presentationMode == ReceiverPresentationMode::SemanticGameplay) {
            if (dummy.presentationMode != ReceiverPresentationMode::SemanticGameplay) {
                // Clearing MatrixValid hands model calculation back to the remote actor.
                // The last complete snapshot remains cached above for one-tick fallback.
                actor->setRemoteMatrices(RemoteLinkMatrixSnapshot{});
            }
            if (pose.linkMatrices.valid) {
                actor->setRemoteAttachmentMatrices(rebase_semantic_attachments(
                    pose.linkMatrices, sourceActorPos, static_cast<s16>(pose.angleY),
                    presentedActorPos, presentedAngleY));
            }
        } else if (pose.linkMatricesFresh) {
            actor->setRemoteMatrices(pose.linkMatrices);
        } else {
            actor->setRemoteMatrices(dummy.retainedMatrices);
        }
        dummy.presentationMode = presentationMode;
        RemoteBombObjectSnapshot bombObject;
        if (get_remote_bomb_object_for_peer(peerId, &bombObject)) {
            actor->setRemoteBombObjectState(bombObject);
        } else {
            actor->setRemoteBombObjectState(RemoteBombObjectSnapshot{});
        }
        if (dummy_trace_enabled()) {
            const uint32_t sequenceDelta =
                dummy.lastTraceSequence != 0 ? pose.sequence - dummy.lastTraceSequence : 0;
            dummy.lastTraceSequence = pose.sequence;
            if (sequenceDelta > 1) {
                ++dummy.traceSeqGapCount;
                if (sequenceDelta > dummy.traceMaxSeqDelta) {
                    dummy.traceMaxSeqDelta = sequenceDelta;
                }
            } else if (sequenceDelta == 0) {
                ++dummy.traceRepeatCount;
            }
            if (!pose.linkMatrices.valid) {
                ++dummy.traceMatrixFalseCount;
            }
            if (pose.ageTicks > 1) {
                ++dummy.traceAgeSpikeCount;
                if (pose.ageTicks > dummy.traceMaxAge) {
                    dummy.traceMaxAge = pose.ageTicks;
                }
            }

            ++dummy.traceApplyTicks;
            if ((dummy.traceApplyTicks % 60) == 1) {
                DuskLog.info(
                    "Multiplayer dummy trace summary peer={} id={} applies={} seq={} "
                    "last_delta={} gaps={} max_gap={} repeats={} age={} age_spikes={} "
                    "max_age={} matrix={} matrix_fresh={} matrix_false={} pos=({}, {}, {}) "
                    "room={} proc={} under_bck={} under_frame={} upper_bck={} upper_frame={}",
                    peerId, dummy.actorId, dummy.traceApplyTicks, pose.sequence, sequenceDelta,
                    dummy.traceSeqGapCount, dummy.traceMaxSeqDelta, dummy.traceRepeatCount,
                    pose.ageTicks, dummy.traceAgeSpikeCount, dummy.traceMaxAge,
                    pose.linkMatrices.valid, pose.linkMatricesFresh,
                    dummy.traceMatrixFalseCount, pose.x, pose.y, pose.z, pose.room, pose.procId,
                    pose.underBck0, pose.underFrame0, pose.upperBck2, pose.upperFrame2);
            }
        }
        for (const RemoteAudioEvent& event : pose.audioEvents) {
            if (event.sequence > dummy.lastAudioEventSequence) {
                actor->playRemoteSound(event);
                dummy.lastAudioEventSequence = event.sequence;
                static uint32_t sAudioRxLogCount = 0;
                if (sAudioRxLogCount < 20) {
                    ++sAudioRxLogCount;
                    DuskLog.info("Multiplayer audio rx peer={} seq={} sound={:#x} mapinfo={} "
                                 "reverb={} source={} level={}",
                                 peerId, event.sequence, event.soundId, event.mapInfo,
                                 static_cast<int>(event.reverb),
                                 static_cast<int>(event.sourceKind), event.level);
                }
            }
        }
        actor->syncRemoteActiveSounds(pose.activeAudioEvents);
        if (dummy.logCount < 5) {
            ++dummy.logCount;
            DuskLog.info("Multiplayer remote Link actor: updated peer={} id={} pos=({}, {}, {}) "
                         "room={} angleY={}",
                         peerId, dummy.actorId, pose.x, pose.y, pose.z, pose.room, pose.angleY);
        }
    }
}

bool is_remote_link_dummy_visible(const std::string& peerId) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        return false;
    }

    daRemoteLink_c* actor = find_remote_link_actor(it->second);
    return actor != nullptr && actor->isRemotePresentationVisible();
}

bool get_remote_link_dummy_label_position(const std::string& peerId, cXyz* outPos) {
    auto it = sActorDummies.find(peerId);
    if (it == sActorDummies.end()) {
        return false;
    }

    daRemoteLink_c* actor = find_remote_link_actor(it->second);
    return actor != nullptr && actor->isRemotePresentationVisible() &&
           actor->getNameLabelPosition(outPos);
}

bool get_remote_link_dummy_peer_id_for_actor(fopAc_ac_c* actor, std::string* outPeerId) {
    if (actor == nullptr || outPeerId == nullptr || fopAcM_GetName(actor) != fpcNm_REMOTE_LINK_e) {
        return false;
    }

    auto* remoteLink = static_cast<daRemoteLink_c*>(actor);
    if (!remoteLink->isRemotePresentationVisible()) {
        return false;
    }

    const fpc_ProcID actorId = fopAcM_GetID(actor);
    for (const auto& entry : sActorDummies) {
        if (entry.second.actorId == actorId) {
            *outPeerId = entry.first;
            return !outPeerId->empty();
        }
    }

    return false;
}

void destroy_remote_link_dummy(const std::string& peerId) {
    destroy_remote_link_actor_dummy(peerId);
}

void destroy_all_remote_link_dummies() {
    for (auto it = sActorDummies.begin(); it != sActorDummies.end();) {
        const std::string peerId = it->first;
        ++it;
        destroy_remote_link_actor_dummy(peerId);
    }
    sBodyCollision.clear();
}

void draw_remote_link_dummy(const std::string& peerId, const PeerPoseSnapshot& pose) {
    const auto dummy = sActorDummies.find(peerId);
    if (pose.valid && dummy != sActorDummies.end() &&
        dummy->second.presentationMode != ReceiverPresentationMode::HiddenUnsupported) {
        update_actor_dummy_collision(peerId, pose);
    } else {
        sBodyCollision.erase(peerId);
    }
}

}  // namespace dusk::multiplayer
