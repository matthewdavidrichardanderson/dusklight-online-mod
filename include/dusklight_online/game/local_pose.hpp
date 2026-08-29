#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>

namespace dusklight_online::game {

enum class LocalPoseMatrixScope : uint8_t {
    None,
    Attachments,
    FullBody,
};

struct LocalPoseDiagnostics {
    LocalPoseMatrixScope matrixScope = LocalPoseMatrixScope::None;
    size_t matrixPackedBytes = 0;
    uint8_t matrixPresentSlots = 0;
};

bool build_local_pose(uint32_t sequence, bool manualSyncReady,
                      bool semanticVisualsEnabled,
                      nlohmann::json& poseMessage,
                      LocalPoseDiagnostics* diagnostics = nullptr);
void reset_local_pose_state();

}  // namespace dusklight_online::game
