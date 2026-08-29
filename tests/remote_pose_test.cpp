#include "dusklight_online/game/remote_pose.hpp"

#include <nlohmann/json.hpp>

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

    // Even malformed/malicious type-7 input cannot restore body matrix
    // streaming. Legitimate attachment slots survive the representation gate.
    json injected = pose_message(2, "semantic_gameplay");
    injected["state"]["link_matrices"] = {
        {"body", identity_model()},
        {"sword", identity_model()},
        {"midna", identity_model()},
    };
    PeerPoseSnapshot injectedPose;
    if (!decode_and_enforce(injected, &pose, injectedPose, error) ||
        injectedPose.linkMatrices.body.valid ||
        !injectedPose.linkMatrices.sword.valid ||
        injectedPose.linkMatrices.midna.valid) {
        std::cerr << "semantic matrix representation gate failed: " << error << '\n';
        return 1;
    }

    // Hidden means no presentation payload at all, including cached matrices
    // hydrated from an earlier pose.
    json hidden = pose_message(3, "hidden_unsupported");
    hidden["state"]["visual_unsupported_reasons"] = 1;
    PeerPoseSnapshot hiddenPose;
    if (!decode_and_enforce(hidden, &injectedPose, hiddenPose, error) ||
        hiddenPose.linkMatrices.valid || hiddenPose.linkMatricesFresh) {
        std::cerr << "hidden semantic pose retained matrices: " << error << '\n';
        return 1;
    }

    // Type-7 packets must identify their representation explicitly.
    json unknown = pose_message(4, "");
    PeerPoseSnapshot unknownPose;
    if (decode_and_enforce(unknown, &hiddenPose, unknownPose, error) || error.empty()) {
        std::cerr << "semantic pose without visual mode was accepted\n";
        return 1;
    }

    // Existing global sequence ordering remains authoritative across visual
    // representation transitions.
    json stale = pose_message(3, "semantic_gameplay");
    PeerPoseSnapshot stalePose;
    if (dusklight_online::game::decode_peer_pose(stale, "peer", &hiddenPose,
                                                  stalePose, error) ||
        error != "stale pose sequence") {
        std::cerr << "stale semantic sequence was accepted\n";
        return 1;
    }

    return 0;
}
