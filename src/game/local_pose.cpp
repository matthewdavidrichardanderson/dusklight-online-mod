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

json pack_matrices(std::initializer_list<MatrixSlot> slots,
                   LocalPoseDiagnostics* diagnostics) {
    std::vector<uint8_t> packed;
    packed.reserve(32 * 1024);
    static constexpr char magic[] = "DMPM";
    append(packed, magic, 4);
    const uint8_t version = 1;
    const uint8_t count = static_cast<uint8_t>(slots.size());
    append_value(packed, version);
    append_value(packed, count);
    size_t slotIndex = 0;
    uint8_t presentSlots = 0;
    for (const MatrixSlot& slot : slots) {
        if (slot.model != nullptr) ++presentSlots;
        append_model(packed, slot.model, slotIndex++);
    }
    if (diagnostics != nullptr) {
        diagnostics->matrixPackedBytes = packed.size();
        diagnostics->matrixPresentSlots = presentSlots;
    }
    return {
        {"format", "qrot16_trans32_womit_bin_v1"},
        {"data", json::binary(std::move(packed))},
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

json link_ik_array(const daAlink_footData_c* values) {
    json out = json::array();
    for (int i = 0; i < 2; ++i) {
        out.push_back(values != nullptr ? int(values[i].field_0x2) : 0);
        out.push_back(values != nullptr ? int(values[i].field_0x4) : 0);
        out.push_back(values != nullptr ? int(values[i].field_0x6) : 0);
    }
    return out;
}

json csxyz_pair_array(const csXyz* values) {
    json out = json::array();
    for (int i = 0; i < 2; ++i) {
        out.push_back(values != nullptr ? int(values[i].x) : 0);
        out.push_back(values != nullptr ? int(values[i].y) : 0);
        out.push_back(values != nullptr ? int(values[i].z) : 0);
    }
    return out;
}

json csxyz_array(const csXyz& value) {
    return json::array({int(value.x), int(value.y), int(value.z)});
}

bool matrices_nearly_equal(MtxP lhs, MtxP rhs) {
    if (lhs == nullptr || rhs == nullptr) return false;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            if (std::fabs(lhs[row][column] - rhs[row][column]) > 0.001f) return false;
        }
    }
    return true;
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

bool build_local_pose(uint32_t sequence, bool manualSyncReady,
                      bool semanticVisualsEnabled, json& poseMessage,
                      LocalPoseDiagnostics* diagnostics) {
    if (diagnostics != nullptr) *diagnostics = {};
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_ALINK_e) return false;
    auto* link = static_cast<daAlink_c*>(player);
    if (link->mpLinkModel == nullptr) return false;

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
    struct PoseAnimSlot {
        int bck;
        int arc;
        float frame;
        float rate;
        float ratio;
    };
    PoseAnimSlot underSlots[3];
    PoseAnimSlot upperSlots[3];
    for (int i = 0; i < 3; ++i) {
        underSlots[i] = {int(link->mUnderAnmHeap[i].getIdx()),
                         int(link->mUnderAnmHeap[i].getArcNo()),
                         link->mUnderFrameCtrl[i].getFrame(),
                         link->mUnderFrameCtrl[i].getRate(),
                         link->mNowAnmPackUnder[i].getRatio()};
        upperSlots[i] = {int(link->mUpperAnmHeap[i].getIdx()),
                         int(link->mUpperAnmHeap[i].getArcNo()),
                         link->mUpperFrameCtrl[i].getFrame(),
                         link->mUpperFrameCtrl[i].getRate(),
                         link->mNowAnmPackUpper[i].getRatio()};
    }
    enum VisualUnsupportedReason : uint32_t {
        kUnsupportedStageTransition = 1u << 0,
        kUnsupportedModelRecreation = 1u << 1,
        kUnsupportedAnimationArchive = 1u << 2,
    };
    uint32_t visualUnsupportedReasons = 0;
    if (dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() || fopOvlpM_IsDoingReq()) {
        visualUnsupportedReasons |= kUnsupportedStageTransition;
    }
    if (link->mClothesChangeWaitTimer != 0) {
        visualUnsupportedReasons |= kUnsupportedModelRecreation;
    }
    const auto slotNeedsArchiveFallback = [](const PoseAnimSlot& slot) {
        return slot.ratio > 0.001f && slot.bck > 0 && slot.bck != 0xFFFF &&
               slot.arc != 0xFFFF;
    };
    for (int i = 0; i < 3; ++i) {
        if (slotNeedsArchiveFallback(underSlots[i]) ||
            slotNeedsArchiveFallback(upperSlots[i])) {
            visualUnsupportedReasons |= kUnsupportedAnimationArchive;
        }
    }
    const auto validPoseBck = [](int id) { return id > 0 && id != 0xFFFF; };
    // Link commonly points an upper blend pack at a lower animation without
    // populating the corresponding upper heap. Preserve that relationship so
    // the receiver does not silently drop an active upper-body slot.
    for (int upper = 0; upper < 3; ++upper) {
        if (validPoseBck(upperSlots[upper].bck)) continue;
        J3DAnmTransform* active = link->mNowAnmPackUpper[upper].getAnmTransform();
        if (active == nullptr) continue;
        for (int lower = 0; lower < 3; ++lower) {
            if (active == link->mNowAnmPackUnder[lower].getAnmTransform() &&
                validPoseBck(underSlots[lower].bck)) {
                upperSlots[upper].bck = underSlots[lower].bck;
                upperSlots[upper].arc = underSlots[lower].arc;
                upperSlots[upper].frame = underSlots[lower].frame;
                upperSlots[upper].rate = underSlots[lower].rate;
                break;
            }
        }
    }
    const float underFrame0 = underSlots[0].frame;
    const auto audioEvents = drain_local_audio_events();
    const auto activeAudioEvents = drain_local_active_audio_events();
    if (!std::isfinite(poseX) || !std::isfinite(poseY) || !std::isfinite(poseZ) ||
        !std::isfinite(underFrame0)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(underSlots[i].frame) || !std::isfinite(underSlots[i].rate) ||
            !std::isfinite(underSlots[i].ratio) || !std::isfinite(upperSlots[i].frame) ||
            !std::isfinite(upperSlots[i].rate) || !std::isfinite(upperSlots[i].ratio))
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
    const bool swordHandAttached =
        humanParts && link->mSwordModel != nullptr &&
        link->mpLinkModel->getModelData()->getJointNum() > 10 &&
        matrices_nearly_equal(link->mSwordModel->getBaseTRMtx(),
                              link->mpLinkModel->getAnmMtx(10));
    const bool shieldHandAttached =
        humanParts && link->mShieldModel != nullptr &&
        link->mpLinkModel->getModelData()->getJointNum() > 15 &&
        matrices_nearly_equal(link->mShieldModel->getBaseTRMtx(),
                              link->mpLinkModel->getAnmMtx(15));
    const bool bodyRootValid = link->mpLinkModel->getModelData() != nullptr &&
                               link->mpLinkModel->getModelData()->getJointNum() > 0;
    MtxP bodyRoot = bodyRootValid ? link->mpLinkModel->getAnmMtx(0) : nullptr;

    const bool semanticGameplay = visualUnsupportedReasons == 0;
    json state = {
        {"visual_mode", visualUnsupportedReasons == 0 ? "semantic_gameplay" :
                                                         "hidden_unsupported"},
        {"visual_unsupported_reasons", visualUnsupportedReasons},
        {"matrix_scope", !semanticVisualsEnabled ? "full_body" :
                             semanticGameplay ? "attachments" : "none"},
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
        {"under_bck0", underSlots[0].bck}, {"under_arc0", underSlots[0].arc},
        {"under_frame0", underSlots[0].frame}, {"under_rate0", underSlots[0].rate},
        {"under_ratio0", underSlots[0].ratio},
        {"under_bck1", underSlots[1].bck}, {"under_arc1", underSlots[1].arc},
        {"under_frame1", underSlots[1].frame}, {"under_rate1", underSlots[1].rate},
        {"under_ratio1", underSlots[1].ratio},
        {"under_bck2", underSlots[2].bck}, {"under_arc2", underSlots[2].arc},
        {"under_frame2", underSlots[2].frame}, {"under_rate2", underSlots[2].rate},
        {"under_ratio2", underSlots[2].ratio},
        {"upper_bck0", upperSlots[0].bck}, {"upper_arc0", upperSlots[0].arc},
        {"upper_frame0", upperSlots[0].frame}, {"upper_rate0", upperSlots[0].rate},
        {"upper_ratio0", upperSlots[0].ratio},
        {"upper_bck1", upperSlots[1].bck}, {"upper_arc1", upperSlots[1].arc},
        {"upper_frame1", upperSlots[1].frame}, {"upper_rate1", upperSlots[1].rate},
        {"upper_ratio1", upperSlots[1].ratio},
        {"upper_bck2", upperSlots[2].bck}, {"upper_arc2", upperSlots[2].arc},
        {"upper_frame2", upperSlots[2].frame}, {"upper_rate2", upperSlots[2].rate},
        {"upper_ratio2", upperSlots[2].ratio},
        {"hat_rot_a", i16_array<10>(link->field_0x302c)},
        {"hat_rot_b", i16_array<10>(link->field_0x3040)},
        {"hat_swing", i16_array<3>(link->field_0x3066)},
        {"hat_shape_y", int(link->field_0x3062)},
        {"shape_angle_x", int(link->shape_angle.x)},
        {"shape_angle_z", int(link->shape_angle.z)},
        {"body_angle_x", int(link->mBodyAngle.x)},
        {"body_angle_y", int(link->mBodyAngle.y)},
        {"body_angle_z", int(link->mBodyAngle.z)},
        {"body_twist_y", int(link->field_0x30c8)},
        {"neck_joint_x", int(link->field_0x3124.x)},
        {"neck_joint_y", int(link->field_0x3124.y)},
        {"neck_joint_z", int(link->field_0x3124.z)},
        {"lower_joint_x", int(link->field_0x3088)},
        {"lower_joint_z", int(link->field_0x308a)},
        {"root_joint_x", int(link->field_0x3080)},
        {"root_joint_z", int(link->field_0x3082)},
        {"blend_mode", int(link->field_0x2fb6)},
        {"upper_saved_ratio", link->field_0x3444},
        {"body_root_valid", bodyRootValid},
        {"body_root_x", bodyRootValid ? bodyRoot[0][3] : 0.0f},
        {"body_root_y", bodyRootValid ? bodyRoot[1][3] : 0.0f},
        {"body_root_z", bodyRootValid ? bodyRoot[2][3] : 0.0f},
        {"leg_ik_angles", link_ik_array(link->mFootData1)},
        {"arm_ik_angles", link_ik_array(link->mFootData2)},
        {"arm_rot_a", csxyz_pair_array(link->field_0x312a)},
        {"arm_rot_b", csxyz_pair_array(link->field_0x3136)},
        // The fishing-rod actor writes these procedural rotations after the
        // BCK pose is selected. Matrix streaming carried them implicitly;
        // semantic animation needs the two joint corrections explicitly.
        {"fishing_arm_1", csxyz_array(link->mFishingArm1Angle)},
        {"fishing_arm_2", csxyz_array(link->field_0x3160)},
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
        {"sword_hand_attached", swordHandAttached},
        {"shield_hand_attached", shieldHandAttached},
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
    if (!semanticVisualsEnabled) {
        if (diagnostics != nullptr) {
            diagnostics->matrixScope = LocalPoseMatrixScope::FullBody;
        }
        state["link_matrices"] = pack_matrices({
            {link->mpLinkModel}, {nullptr}, {humanCore ? link->mpLinkFaceModel : nullptr},
            {humanCore ? link->mpLinkHandModel : nullptr},
            {humanParts ? link->mSwordModel : nullptr},
            {humanParts ? link->mSheathModel : nullptr},
            {humanParts ? link->mShieldModel : nullptr},
            {humanParts ? link->mHeldItemModel : nullptr},
            {humanParts ? link->mpHookTipModel : nullptr},
            {humanParts ? link->field_0x0710 : nullptr},
            {humanParts ? link->field_0x0714 : nullptr}, {arrow},
            {humanParts ? link->mpKanteraModel : nullptr},
            {humanParts ? link->mpKanteraGlowModel : nullptr},
            {humanParts ? itemActorModel : nullptr}, {spinner},
            {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr},
        }, diagnostics);
    } else if (semanticGameplay) {
        // Link, wolf, head/hair, face, hands and heavy boots are reconstructed
        // from semantic animation/body state. Preserve only independently
        // moving props. Remote Midna is intentionally absent in every mode.
        if (diagnostics != nullptr) {
            diagnostics->matrixScope = LocalPoseMatrixScope::Attachments;
        }
        state["link_matrices"] = pack_matrices({
            {nullptr}, {nullptr}, {nullptr}, {nullptr},
            {humanParts ? link->mSwordModel : nullptr},
            {humanParts ? link->mSheathModel : nullptr},
            {humanParts ? link->mShieldModel : nullptr},
            {humanParts ? link->mHeldItemModel : nullptr},
            {humanParts ? link->mpHookTipModel : nullptr},
            {humanParts ? link->field_0x0710 : nullptr},
            {humanParts ? link->field_0x0714 : nullptr}, {arrow},
            {humanParts ? link->mpKanteraModel : nullptr},
            {humanParts ? link->mpKanteraGlowModel : nullptr},
            {humanParts ? itemActorModel : nullptr}, {spinner},
            {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr},
        }, diagnostics);
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
