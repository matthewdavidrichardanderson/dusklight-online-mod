#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>

namespace dusklight_online::game {

bool build_local_pose(uint32_t sequence, bool includeMidna, bool manualSyncReady,
                      nlohmann::json& poseMessage,
                      nlohmann::json& midnaMessage);
void reset_local_pose_state();

}  // namespace dusklight_online::game
