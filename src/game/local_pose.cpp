#include "dusklight_online/game/local_pose.hpp"
#include "dusklight_online/game/audio_bridge.hpp"

#include "d/dolzel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_midna.h"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_name.h"

namespace dusklight_online::game {
namespace {

using nlohmann::json;

struct MatrixSlot {
    J3DModel* model;
};

struct WeightState {
    bool initialized = false;
    uint16_t joints = 0;
    uint16_t weights = 0;
    uint64_t hash = 0;
    uint32_t stableFrames = 0;
};

std::array<WeightState, 21> sWeightStates{};
bool sLocalTransformObserved = false;
bool sLocalTransformFromWolf = false;
bool sLocalTransformToWolf = false;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t hash_bytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

void append(std::vector<uint8_t>& out, const void* value, size_t size) {
    const auto* first = static_cast<const uint8_t*>(value);
    out.insert(out.end(), first, first + size);
}

template <typename T>
void append_value(std::vector<uint8_t>& out, const T& value) {
    append(out, &value, sizeof(value));
}

int16_t quantize_basis(float value) {
    return static_cast<int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

void append_matrix(std::vector<uint8_t>& out, CMtxP matrix) {
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            const int16_t value = quantize_basis(matrix[row][col]);
            append_value(out, value);
        }
    for (int row = 0; row < 3; ++row) append_value(out, matrix[row][3]);
}

uint64_t hash_weights(J3DModel* model, uint16_t weights) {
    uint64_t hash = hash_bytes(kFnvOffset, &weights, sizeof(weights));
    for (uint16_t i = 0; i < weights; ++i) {
        CMtxP matrix = model->getWeightAnmMtx(i);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const int16_t value = quantize_basis(matrix[row][col]);
                hash = hash_bytes(hash, &value, sizeof(value));
            }
            hash = hash_bytes(hash, &matrix[row][3], sizeof(float));
        }
    }
    return hash;
}

void append_model(std::vector<uint8_t>& out, J3DModel* model, size_t slotIndex) {
    if (model == nullptr || model->getModelData() == nullptr) {
        const uint8_t absent = 0;
        append_value(out, absent);
        return;
    }
    const uint8_t present = 1;
    const uint16_t joints = model->getModelData()->getJointNum();
    const uint16_t weights = model->getModelData()->getWEvlpMtxNum();
    bool omitWeights = false;
    if (weights > 0 && slotIndex < sWeightStates.size()) {
        const uint64_t hash = hash_weights(model, weights);
        WeightState& state = sWeightStates[slotIndex];
        const bool changed = !state.initialized || state.joints != joints ||
                             state.weights != weights || state.hash != hash;
        state.stableFrames = changed ? 0 : state.stableFrames + 1;
        state.initialized = true;
        state.joints = joints;
        state.weights = weights;
        state.hash = hash;
        omitWeights = !changed && state.stableFrames >= 6 &&
                      (state.stableFrames % 30) != 0;
    }
    const uint8_t weightsIncluded = omitWeights ? 0 : 1;
    append_value(out, present);
    append_value(out, joints);
    append_value(out, weights);
    append_value(out, weightsIncluded);
    append_matrix(out, model->getBaseTRMtx());
    for (uint16_t i = 0; i < joints; ++i) append_matrix(out, model->getAnmMtx(i));
    if (!omitWeights) {
        for (uint16_t i = 0; i < weights; ++i) append_matrix(out, model->getWeightAnmMtx(i));
    }
}

json pack_matrices(std::initializer_list<MatrixSlot> slots, int midnaHairShape = 0) {
    std::vector<uint8_t> packed;
    packed.reserve(32 * 1024);
    static constexpr char magic[] = "DMPM";
    append(packed, magic, 4);
    const uint8_t version = 1;
    const uint8_t count = static_cast<uint8_t>(slots.size());
    append_value(packed, version);
    append_value(packed, count);
    size_t slotIndex = 0;
    for (const MatrixSlot& slot : slots) append_model(packed, slot.model, slotIndex++);
    return {
        {"format", "qrot16_trans32_womit_bin_v1"},
        {"data", json::binary(std::move(packed))},
        {"midna_hair_shape", midnaHairShape},
    };
}

int sword_variant(const daAlink_c* link) {
    if (link == nullptr || link->mSwordModel == nullptr) return 0;
    if (link->mSwordModel == link->mWoodSwordModel) return 2;
    if (link->mSwordModel == link->mpSwMModel) return 3;
    if (link->mSwordModel == link->mpSwAModel) return 1;
    return 0;
}

int shield_variant() {
    switch (dComIfGs_getSelectEquipShield()) {
    case dItemNo_WOOD_SHIELD_e: return 1;
    case dItemNo_SHIELD_e: return 2;
    case dItemNo_HYLIA_SHIELD_e: return 3;
    default: return 0;
    }
}

int clothes_variant() {
    if (daPy_py_c::checkCasualWearFlg()) return 1;
    if (daPy_py_c::checkZoraWearFlg()) return 2;
    if (daPy_py_c::checkMagicArmorWearFlg()) return 3;
    return 0;
}

template <size_t N>
json i16_array(const int16_t* values) {
    json out = json::array();
    for (size_t i = 0; i < N; ++i) out.push_back(values != nullptr ? int(values[i]) : 0);
    return out;
}

int visible_hair_shape(J3DModel* model) {
    if (model == nullptr || model->getModelData() == nullptr) return 0;
    const int count = std::min<int>(3, model->getModelData()->getMaterialNum());
    for (int i = 0; i < count; ++i) {
        J3DMaterial* material = model->getModelData()->getMaterialNodePointer(i);
        J3DShape* shape = material != nullptr ? material->getShape() : nullptr;
        if (shape != nullptr && !shape->checkFlag(J3DShpFlag_Visible)) return i;
    }
    return 0;
}

json audio_json(const std::vector<dusk::multiplayer::RemoteAudioEvent>& events) {
    json out = json::array();
    for (const auto& event : events) out.push_back({
        {"seq", event.sequence}, {"sound_id", event.soundId},
        {"mapinfo", event.mapInfo}, {"reverb", int(event.reverb)},
        {"source", int(event.sourceKind)}, {"level", event.level},
    });
    return out;
}

}  // namespace

bool build_local_pose(uint32_t sequence, bool includeMidna, bool manualSyncReady,
                      json& poseMessage, json& midnaMessage) {
    includeMidna = includeMidna && dusk::multiplayer::kRemoteMidnaStreamingEnabled;
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_ALINK_e) return false;
    auto* link = static_cast<daAlink_c*>(player);
    if (link->mClothesChangeWaitTimer != 0 || link->mpLinkModel == nullptr) return false;

    const bool wolf = static_cast<bool>(link->checkWolf());
    const bool transforming = link->mProcID == daAlink_c::PROC_METAMORPHOSE ||
                              link->mProcID == daAlink_c::PROC_METAMORPHOSE_ONLY;
    if (transforming && !sLocalTransformObserved) {
        sLocalTransformObserved = true;
        sLocalTransformFromWolf = wolf;
        sLocalTransformToWolf = !wolf;
    } else if (!transforming) {
        sLocalTransformObserved = false;
    }
    const bool transformFromWolf = transforming && sLocalTransformFromWolf;
    const bool transformToWolf = transforming && sLocalTransformToWolf;

    const float poseX = player->current.pos.x;
    const float poseY = player->current.pos.y;
    const float poseZ = player->current.pos.z;
    const float underFrame0 = link->mUnderFrameCtrl[0].getFrame();
    const float underRate0 = link->mUnderFrameCtrl[0].getRate();
    const float upperFrame2 = link->mUpperFrameCtrl[2].getFrame();
    const float upperRate2 = link->mUpperFrameCtrl[2].getRate();
    const auto audioEvents = drain_local_audio_events();
    const auto activeAudioEvents = drain_local_active_audio_events();
    if (!std::isfinite(poseX) || !std::isfinite(poseY) || !std::isfinite(poseZ) ||
        !std::isfinite(underFrame0) || !std::isfinite(underRate0) ||
        !std::isfinite(upperFrame2) || !std::isfinite(upperRate2)) {
        return false;
    }
    const bool humanCore = !wolf;
    const bool humanParts = humanCore && !transforming;
    fopAc_ac_c* itemActor = humanParts ? link->mItemAcKeep.getActor() : nullptr;
    fopAc_ac_c* thrownBoomerang =
        humanParts ? link->getThrowBoomerangAcKeep()->getActor() : nullptr;
    fopAc_ac_c* grabbedActor = humanParts ? link->mGrabItemAcKeep.getActor() : nullptr;
    if (thrownBoomerang != nullptr &&
        fopAcM_GetName(thrownBoomerang) == fpcNm_BOOMERANG_e) {
        itemActor = thrownBoomerang;
    } else if (grabbedActor != nullptr &&
               fopAcM_GetName(grabbedActor) == fpcNm_BOOMERANG_e) {
        itemActor = grabbedActor;
    }
    // Bombs have an authoritative world-object lane with fuse/explosion state.
    // Never also serialize their held model as a generic attached item.
    if (itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_NBOMB_e) itemActor = nullptr;
    J3DModel* arrow = itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_ARROW_e ?
                      itemActor->model : nullptr;
    J3DModel* itemActorModel =
        itemActor != nullptr && fopAcM_GetName(itemActor) != fpcNm_ARROW_e ?
            itemActor->model : nullptr;
    const int itemActorKind =
        itemActor != nullptr && fopAcM_GetName(itemActor) == fpcNm_BOOMERANG_e ?
            dusk::multiplayer::REMOTE_ITEM_ACTOR_BOOMERANG :
            dusk::multiplayer::REMOTE_ITEM_ACTOR_NONE;
    fopAc_ac_c* rideActor = humanParts ? link->mRideAcKeep.getActor() : nullptr;
    J3DModel* spinner = rideActor != nullptr && fopAcM_GetName(rideActor) == fpcNm_SPINNER_e ?
                        rideActor->model : nullptr;

    json state = {
        {"stage", dComIfGp_getStartStageName()},
        {"room", int(fopAcM_GetRoomNo(player))},
        {"layer", int(dComIfGp_getStartStageLayer())},
        {"x", poseX}, {"y", poseY}, {"z", poseZ},
        {"angle_y", int(player->shape_angle.y)},
        {"proc_id", int(link->mProcID)}, {"proc_v0", link->mProcVar0.field_0x3008},
        {"proc_v1", link->mProcVar1.field_0x300a}, {"proc_v2", link->mProcVar2.field_0x300c},
        {"proc_v3", link->mProcVar3.field_0x300e}, {"proc_v5", link->mProcVar5.field_0x3012},
        {"cut_type", int(link->getCutType())}, {"cut_count", int(link->getCutCount())},
        {"jump_cancel_turn", bool(link->checkCutJumpCancelTurn())},
        {"manual_sync_ready", manualSyncReady},
        {"under_frame", underFrame0},
        {"under_bck0", int(link->mUnderAnmHeap[0].getIdx())},
        {"under_frame0", underFrame0},
        {"under_rate0", underRate0},
        {"upper_bck2", int(link->mUpperAnmHeap[2].getIdx())},
        {"upper_frame2", upperFrame2},
        {"upper_rate2", upperRate2},
        {"hat_rot_a", i16_array<10>(link->field_0x302c)},
        {"hat_rot_b", i16_array<10>(link->field_0x3040)},
        {"hat_swing", i16_array<3>(link->field_0x3066)},
        {"hat_shape_y", int(link->field_0x3062)},
        {"is_wolf", wolf}, {"is_transforming", transforming},
        {"transform_from_wolf", transformFromWolf},
        {"transform_to_wolf", transformToWolf},
        {"transform_proc_v0", link->mProcVar0.field_0x3008},
        {"transform_proc_v5", link->mProcVar5.field_0x3012},
        {"transform_clothes_wait", int(link->mClothesChangeWaitTimer)},
        {"transform_frame", underFrame0},
        {"transform_proc_v2", link->mProcVar2.field_0x300c},
        {"transform_proc_v3", link->mProcVar3.field_0x300e},
        {"transform_shape_x", int(link->shape_angle.x)},
        {"equip_item", int(link->mEquipItem)}, {"sword_variant", sword_variant(link)},
        {"shield_variant", shield_variant()}, {"clothes_variant", clothes_variant()},
        {"sword_draw", bool(link->checkSwordDraw())},
        {"shield_draw", bool(link->checkShieldDraw())},
        {"shield_guard_active", !wolf && bool(link->checkShieldDraw()) &&
                                  bool(link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_8000000))},
        {"sword_out", !wolf && link->mEquipItem == 0x103},
        {"heavy_boots", !wolf && bool(link->checkEquipHeavyBoots())},
        {"item_draw", !wolf && bool(link->checkItemDraw())},
        {"kantera_draw", !wolf && (link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_1) ||
                                     link->checkNoResetFlg2(daPy_py_c::FLG2_UNK_20000))},
        {"item_actor_kind", itemActorKind},
        {"ride_actor_kind", spinner != nullptr ?
                                dusk::multiplayer::REMOTE_RIDE_ACTOR_SPINNER :
                                dusk::multiplayer::REMOTE_RIDE_ACTOR_NONE},
        {"audio_events", audio_json(audioEvents)},
        {"active_audio_events", audio_json(activeAudioEvents)},
    };
    state["link_matrices"] = pack_matrices({
        {link->mpLinkModel}, {nullptr}, {humanCore ? link->mpLinkFaceModel : nullptr},
        {humanCore ? link->mpLinkHandModel : nullptr}, {humanParts ? link->mSwordModel : nullptr},
        {humanParts ? link->mSheathModel : nullptr}, {humanParts ? link->mShieldModel : nullptr},
        {humanParts ? link->mHeldItemModel : nullptr}, {humanParts ? link->mpHookTipModel : nullptr},
        {humanParts ? link->field_0x0710 : nullptr}, {humanParts ? link->field_0x0714 : nullptr},
        {arrow}, {humanParts ? link->mpKanteraModel : nullptr},
        {humanParts ? link->mpKanteraGlowModel : nullptr},
        {humanParts ? itemActorModel : nullptr}, {spinner},
        {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr},
    });

    midnaMessage = json();
    if (includeMidna && !transforming) {
        daMidna_c* midna = daPy_py_c::getMidnaActor();
        J3DModel* body = wolf ? link->getMidnaModel() : nullptr;
        J3DModel* mask = wolf ? link->getMidnaMaskModel() : nullptr;
        J3DModel* hand = wolf ? link->getMidnaHandModel() : nullptr;
        J3DModel* hair = wolf ? link->getMidnaHairHandModel() : nullptr;
        J3DModel* glow = nullptr;
        // Shadow Midna model accessors are unavailable. Link's riding-Midna
        // model set remains available for this pose.
        const bool shadow = false;
        state["midna_draw"] = body != nullptr;
        state["midna_mask_draw"] = mask != nullptr;
        state["midna_hand_draw"] = hand != nullptr;
        state["midna_hair_draw"] = hair != nullptr;
        state["midna_shadow_form"] = shadow;
        if (body != nullptr || mask != nullptr || hand != nullptr || hair != nullptr || glow != nullptr) {
            json midnaState;
            midnaState["link_matrices"] = pack_matrices({
                {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr},
                {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr},
                {nullptr}, {nullptr}, {body}, {mask}, {hand}, {hair}, {glow},
            }, visible_hair_shape(hair));
            midnaMessage = {{"type", "midna_pose"}, {"sequence", sequence},
                            {"stage", dComIfGp_getStartStageName()},
                            {"state", std::move(midnaState)}};
        }
    }

    poseMessage = {{"type", "pose"}, {"sequence", sequence}, {"state", std::move(state)}};
    return true;
}

void reset_local_pose_state() {
    sWeightStates = {};
    sLocalTransformObserved = false;
    sLocalTransformFromWolf = false;
    sLocalTransformToWolf = false;
}

}  // namespace dusklight_online::game
