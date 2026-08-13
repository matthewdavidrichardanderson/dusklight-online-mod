#pragma once

#include "dusk/multiplayer/multiplayer.hpp"

#include <mods/api.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace dusklight_online::game {

struct ProgressionPromptView {
    bool active = false;
    bool waiting = false;
    std::string title;
    std::string body;
    float ageSeconds = 0.0f;
    float remainingRatio = 0.0f;
    float holdRatio = 0.0f;
};

ModResult install_visual_hooks(ModError* error);
void uninstall_visual_hooks();
void update_visual_overlays(
    bool connected, bool gameplayReady, bool nameLabelsEnabled, bool remoteModelEnabled,
    bool playerListEnabled, std::string_view room, std::string_view localStatus,
    std::string_view localName, uint8_t localColorSlot,
    const std::map<std::string, dusk::multiplayer::PeerPoseSnapshot>& poses,
    const std::map<std::string, std::string>& names,
    const std::map<std::string, uint8_t>& colorSlots,
    const ProgressionPromptView& progressionPrompt);
void push_online_notification(std::string text, float durationSeconds = 5.0f);
void push_online_player_notification(std::string playerName, std::string text,
                                     uint8_t colorSlot, float durationSeconds = 5.0f);
void reset_visual_overlays();

}  // namespace dusklight_online::game
