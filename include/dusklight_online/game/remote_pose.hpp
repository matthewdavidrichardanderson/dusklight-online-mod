#pragma once

#include "dusk/multiplayer/multiplayer.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace dusklight_online::game {

bool decode_peer_pose(const nlohmann::json& message, const std::string& peerId,
                      const dusk::multiplayer::PeerPoseSnapshot* previous,
                      dusk::multiplayer::PeerPoseSnapshot& output,
                      std::string& error);

bool merge_midna_pose(const nlohmann::json& message,
                      dusk::multiplayer::PeerPoseSnapshot& pose,
                      std::string& error);

bool expand_remote_matrix_delta(nlohmann::json& message, const std::string& peerId,
                                uint8_t packetType, uint32_t sequence,
                                std::string& error);
bool prepare_remote_matrix_delta(nlohmann::json& message, const std::string& peerId,
                                 uint8_t packetType, uint32_t sequence,
                                 uint32_t baselineSequence, std::string& error);
void clear_remote_matrix_history(const std::string& peerId = {});

}  // namespace dusklight_online::game
