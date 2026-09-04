#pragma once

#include "dusklight_online/game/protocol_router.hpp"
#include "dusk/multiplayer/multiplayer.hpp"

#include <mods/api.h>

#include <map>
#include <string_view>

namespace dusklight_online::game {

// Final-duel synchronization is deliberately isolated from general progression
// and Remote Link rendering. The lobby owner coordinates one encounter
// owner at a time; only that peer runs Ganondorf's AI, while other copies consume its
// authoritative actor state.
ModResult install_ganondorf_sync_hooks(net::Transport& transport, ModError* error);
void uninstall_ganondorf_sync_hooks();

void update_ganondorf_sync(
    const net::Status& status, bool featureReady, bool stageReady,
    const std::map<std::string, dusk::multiplayer::PeerPoseSnapshot>& peerPoses);
ApplyResult consume_ganondorf_message(const RoutedMessage& message);
void ganondorf_peer_left(std::string_view peerId);
void reset_ganondorf_sync();

}  // namespace dusklight_online::game
