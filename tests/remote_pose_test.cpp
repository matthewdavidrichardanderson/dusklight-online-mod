#include "dusklight_online/game/remote_pose.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using dusk::multiplayer::PeerPoseSnapshot;
using nlohmann::json;

json identity_model() {
    return {
        {"joint_count", 0},
        {"weight_count", 0},
        {"base", std::array<float, 12>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
        {"joints", json::array()},
        {"weights", json::array()},
    };
}

json pose_message(uint32_t sequence, const char* visualMode) {
    return {
        {"type", "pose"},
        {"sequence", sequence},
        {"state", {
            {"visual_mode", visualMode},
            {"stage", "F_SP00"},
            {"room", 0},
            {"x", 1.0f},
            {"y", 2.0f},
            {"z", 3.0f},
        }},
    };
}

bool decode_and_enforce(const json& message, const PeerPoseSnapshot* previous,
                        PeerPoseSnapshot& pose, std::string& error) {
    return dusklight_online::game::decode_peer_pose(message, "peer", previous, pose,
                                                     error) &&
           dusklight_online::game::enforce_semantic_pose_invariants(pose, error);
}

}  // namespace

int main() {
    std::string error;
    PeerPoseSnapshot pose;

    // A semantic packet may contain only the explicit empty attachment
    // container. It must still clear old props and remain a valid snapshot.
    json empty = pose_message(1, "semantic_gameplay");
    std::vector<std::uint8_t> packed{'D', 'M', 'P', 'M', 1, 21};
    packed.resize(27, 0);
    empty["state"]["link_matrices"] = {
        {"format", "qrot16_trans32_womit_bin_v1"},
        {"data", json::binary(packed)},
    };
    if (!decode_and_enforce(empty, nullptr, pose, error) ||
        !pose.linkMatrices.valid || pose.linkMatrices.body.valid) {
        std::cerr << "empty semantic attachment container rejected: " << error << '\n';
        return 1;
    }

    // Fishing rod movement is a pair of procedural arm rotations rather than
    // a BCK slot. Preserve it independently of the (optional) rod model.
    json fishing = pose_message(2, "semantic_gameplay");
    fishing["state"]["fishing_arm_1"] = json::array({100, -200, 300});
    fishing["state"]["fishing_arm_2"] = json::array({-400, 500, -600});
    PeerPoseSnapshot fishingPose;
    if (!decode_and_enforce(fishing, &pose, fishingPose, error) ||
        fishingPose.fishingArm1Angle != std::array<int16_t, 3>{100, -200, 300} ||
        fishingPose.fishingArm2Angle != std::array<int16_t, 3>{-400, 500, -600}) {
        std::cerr << "fishing arm state was not decoded: " << error << '\n';
        return 1;
    }

    json facial = pose_message(3, "semantic_gameplay");
    facial["state"].update({
        {"face_bck", 101}, {"face_bck_arc", 0xFFFF}, {"face_bck_frame", 4.5f},
        {"face_btp", 202}, {"face_btp_arc", 0xFFFF}, {"face_btp_frame", 2.0f},
        {"face_btk", 303}, {"face_btk_arc", 0xFFFF}, {"face_btk_frame", 3.0f},
    });
    PeerPoseSnapshot facialPose;
    if (!decode_and_enforce(facial, &fishingPose, facialPose, error) ||
        facialPose.faceBck != 101 || facialPose.faceBckFrame != 4.5f ||
        facialPose.faceBtp != 202 || facialPose.faceBtpFrame != 2.0f ||
        facialPose.faceBtk != 303 || facialPose.faceBtkFrame != 3.0f) {
        std::cerr << "facial animation state was not decoded: " << error << '\n';
        return 1;
    }

    // Even malformed/malicious type-7 input cannot restore body or ordinary
    // equipped-gear matrix streaming. Independently moving props survive the
    // representation gate.
    json injected = pose_message(4, "semantic_gameplay");
    injected["state"]["link_matrices"] = {
        {"body", identity_model()},
        {"sword", identity_model()},
        {"sheath", identity_model()},
        {"shield", identity_model()},
        {"held_item", identity_model()},
        {"midna", identity_model()},
    };
    PeerPoseSnapshot injectedPose;
    if (!decode_and_enforce(injected, &facialPose, injectedPose, error) ||
        injectedPose.linkMatrices.body.valid ||
        injectedPose.linkMatrices.sword.valid ||
        injectedPose.linkMatrices.sheath.valid ||
        injectedPose.linkMatrices.shield.valid ||
        !injectedPose.linkMatrices.heldItem.valid ||
        injectedPose.linkMatrices.midna.valid) {
        std::cerr << "semantic matrix representation gate failed: " << error << '\n';
        return 1;
    }

    // Hidden means no presentation payload at all, including cached matrices
    // hydrated from an earlier pose.
    json hidden = pose_message(5, "hidden_unsupported");
    hidden["state"]["visual_unsupported_reasons"] = 1;
    PeerPoseSnapshot hiddenPose;
    if (!decode_and_enforce(hidden, &injectedPose, hiddenPose, error) ||
        hiddenPose.linkMatrices.valid || hiddenPose.linkMatricesFresh) {
        std::cerr << "hidden semantic pose retained matrices: " << error << '\n';
        return 1;
    }

    // Type-7 packets must identify their representation explicitly.
    json unknown = pose_message(6, "");
    PeerPoseSnapshot unknownPose;
    if (decode_and_enforce(unknown, &hiddenPose, unknownPose, error) || error.empty()) {
        std::cerr << "semantic pose without visual mode was accepted\n";
        return 1;
    }

    // Existing global sequence ordering remains authoritative across visual
    // representation transitions.
    json stale = pose_message(5, "semantic_gameplay");
    PeerPoseSnapshot stalePose;
    if (dusklight_online::game::decode_peer_pose(stale, "peer", &hiddenPose,
                                                  stalePose, error) ||
        error != "stale pose sequence") {
        std::cerr << "stale semantic sequence was accepted\n";
        return 1;
    }

    // Quake-style snapshot deltas always reference an explicitly acknowledged
    // full state. Losing the packet that starts a roll must not make subsequent
    // snapshots depend on that missing packet.
    dusklight_online::game::clear_remote_matrix_history();
    json idle = pose_message(10, "semantic_gameplay");
    idle["state"].update({
        {"proc_id", 1}, {"proc_var", 0}, {"anim_frame", 0.0f},
        {"anim_rate", 1.0f}, {"wolf", false}, {"yaw", 100},
        {"face_bck", 20}, {"face_btp", 21}, {"face_btk", 22},
        {"hair_angles", json::array({1, 2, 3, 4, 5, 6})},
    });
    json senderIdle = idle;
    if (!dusklight_online::game::prepare_remote_matrix_delta(
            senderIdle, "snapshot-peer", 7, 10, 0, true, false, error) ||
        senderIdle.value("snapshot_delta_v1", false)) {
        std::cerr << "initial semantic snapshot was not full: " << error << '\n';
        return 1;
    }

    json rollStart = idle;
    rollStart["sequence"] = 11;
    rollStart["state"]["proc_id"] = 77;
    rollStart["state"]["anim_frame"] = 1.0f;
    if (!dusklight_online::game::prepare_remote_matrix_delta(
            rollStart, "snapshot-peer", 7, 11, 10, true, true, error) ||
        !rollStart.value("snapshot_delta_v1", false)) {
        std::cerr << "roll-start snapshot was not delta encoded: " << error << '\n';
        return 1;
    }
    const auto changedKeys = rollStart.value("_snapshot_debug_changed_keys", json::array());
    const auto unchangedKeys = rollStart.value("_snapshot_debug_unchanged_keys", json::array());
    if (std::find(changedKeys.begin(), changedKeys.end(), "proc_id") == changedKeys.end() ||
        std::find(changedKeys.begin(), changedKeys.end(), "anim_frame") == changedKeys.end() ||
        std::find(unchangedKeys.begin(), unchangedKeys.end(), "wolf") == unchangedKeys.end()) {
        std::cerr << "snapshot field diagnostics did not reflect the acknowledged baseline\n";
        return 1;
    }
    rollStart.erase("_snapshot_debug_changed_keys");
    rollStart.erase("_snapshot_debug_unchanged_keys");
    rollStart.erase("_snapshot_debug_removed_keys");

    json rollContinues = idle;
    rollContinues["sequence"] = 12;
    rollContinues["state"]["proc_id"] = 77;
    rollContinues["state"]["anim_frame"] = 2.0f;
    if (!dusklight_online::game::prepare_remote_matrix_delta(
            rollContinues, "snapshot-peer", 7, 12, 10, true, false, error) ||
        !rollContinues.value("snapshot_delta_v1", false) ||
        rollContinues.value("snapshot_base", 0U) != 10) {
        std::cerr << "post-loss roll snapshot did not retain acknowledged baseline: "
                  << error << '\n';
        return 1;
    }
    rollContinues.erase("_snapshot_debug_reason");

    // Simulate a separate receiver which got snapshot 10, lost 11, and got 12.
    dusklight_online::game::clear_remote_matrix_history();
    if (!dusklight_online::game::expand_remote_matrix_delta(
            idle, "snapshot-peer", 7, 10, error) ||
        !dusklight_online::game::expand_remote_matrix_delta(
            rollContinues, "snapshot-peer", 7, 12, error) ||
        rollContinues["state"].value("proc_id", 0) != 77 ||
        rollContinues["state"].value("anim_frame", 0.0f) != 2.0f) {
        std::cerr << "lost roll-start snapshot was not recovered: " << error << '\n';
        return 1;
    }

    json missingBaseline = pose_message(12, "semantic_gameplay");
    missingBaseline["state"] = {{"proc_id", 77}, {"anim_frame", 2.0f}};
    missingBaseline["snapshot_delta_v1"] = true;
    missingBaseline["snapshot_base"] = 10;
    dusklight_online::game::clear_remote_matrix_history();
    if (dusklight_online::game::expand_remote_matrix_delta(
            missingBaseline, "snapshot-peer", 7, 12, error) ||
        error.rfind("semantic snapshot delta", 0) != 0) {
        std::cerr << "missing semantic baseline was not rejected\n";
        return 1;
    }

    // Applying an empty/small patch to the named idle baseline must restore
    // idle even if the receiver's current presentation is still the roll.
    dusklight_online::game::clear_remote_matrix_history();
    json senderBaseline = idle;
    if (!dusklight_online::game::prepare_remote_matrix_delta(
            senderBaseline, "snapshot-peer", 7, 10, 0, true, false, error)) return 1;
    json backToIdle = idle;
    backToIdle["sequence"] = 13;
    if (!dusklight_online::game::prepare_remote_matrix_delta(
            backToIdle, "snapshot-peer", 7, 13, 10, true, false, error) ||
        !backToIdle.value("snapshot_delta_v1", false)) {
        std::cerr << "idle restoration snapshot was not delta encoded: " << error << '\n';
        return 1;
    }
    backToIdle.erase("_snapshot_debug_reason");
    dusklight_online::game::clear_remote_matrix_history();
    if (!dusklight_online::game::expand_remote_matrix_delta(
            idle, "snapshot-peer", 7, 10, error) ||
        !dusklight_online::game::expand_remote_matrix_delta(
            backToIdle, "snapshot-peer", 7, 13, error) ||
        backToIdle["state"].value("proc_id", 0) != 1) {
        std::cerr << "named baseline did not restore idle state: " << error << '\n';
        return 1;
    }

    return 0;
}
