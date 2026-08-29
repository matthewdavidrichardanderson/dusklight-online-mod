#pragma once

#include "dusklight_online/net/transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <mods/api.h>
#include <mods/svc/config.h>
#include <mods/svc/ui.h>

namespace dusklight_online {

namespace game {
class GameAdapter;
class ProtocolRouter;
}

class OnlineApp {
public:
    OnlineApp();
    ~OnlineApp();

    OnlineApp(const OnlineApp&) = delete;
    OnlineApp& operator=(const OnlineApp&) = delete;

    ModResult initialize(ModError* error);
    void consume_progression_prompt_input();
    void update();
    void shutdown();

private:
    struct Config {
        ConfigVarHandle playerName = 0;
        ConfigVarHandle directRoom = 0;
        ConfigVarHandle bindHost = 0;
        ConfigVarHandle publicHost = 0;
        ConfigVarHandle port = 0;
        ConfigVarHandle directInvite = 0;
        ConfigVarHandle relayCode = 0;
        ConfigVarHandle relayRoom = 0;
        ConfigVarHandle relayPassword = 0;
        ConfigVarHandle relayLocal = 0;
        ConfigVarHandle dummyModel = 0;
        ConfigVarHandle nameLabels = 0;
        ConfigVarHandle syncFlags = 0;
        ConfigVarHandle syncWorld = 0;
        ConfigVarHandle displayMidna = 0;
        ConfigVarHandle semanticRenderingExperiment = 0;
        ConfigVarHandle remoteCollision = 0;
        ConfigVarHandle pvp = 0;
        ConfigVarHandle playerList = 0;
    } config_;

    net::Transport transport_;
    std::unique_ptr<game::GameAdapter> game_;
    std::unique_ptr<game::ProtocolRouter> router_;
    UiWindowHandle window_ = 0;
    UiMenuTabHandle menuTab_ = 0;
    UiElementHandle panelStatus_ = 0;
    UiElementHandle windowStatus_ = 0;
    UiElementHandle sessionActionsHeading_ = 0;
    std::string panelRenderedStatus_;
    std::string windowRenderedStatus_;
    std::string directCodeDisplay_;
    std::string relayCodeDisplay_;
    bool reopenWindowPending_ = false;
    bool sessionActionsVisible_ = false;
    std::string statusMessage_ = "Not connected";
    std::string requestedDisconnectStatus_;
    std::string activeCode_;
    bool livePublishInitialized_ = false;
    bool relayHostIntent_ = false;
    bool lastWantPuppet_ = true;
    bool lastWantMidna_ = false;
    std::vector<std::string> manualPeerIds_;
    std::vector<std::string> manualPeerLabels_;
    std::vector<UiElementHandle> manualPeerButtonElements_;
    struct ManualPeerButtonContext {
        OnlineApp* app = nullptr;
        int64_t index = 0;
    };
    std::vector<ManualPeerButtonContext> manualPeerButtonContexts_;
    int64_t selectedManualPeer_ = 0;

    ModResult register_config(ModError* error);
    ModResult register_ui(ModError* error);
    std::string string_value(ConfigVarHandle handle) const;
    bool bool_value(ConfigVarHandle handle, bool fallback = false) const;
    int64_t int_value(ConfigVarHandle handle, int64_t fallback = 0) const;
    net::RoomSettings configured_settings() const;
    net::RoomSettings displayed_settings() const;
    std::string status_text() const;
    std::string dashboard_rml() const;
    void open_window();
    void host_direct();
    void join_direct();
    void host_relay();
    void join_relay();
    void disconnect();
    void publish_live_options();
    void refresh_manual_peer_choices();
    void request_manual_sync(bool flagsOnly);

public:
    // C service callbacks must be addressable by the descriptor-building
    // helpers; they immediately recover the owning OnlineApp from user_data.
    static ModResult build_panel(ModContext*, UiElementHandle, void*, ModError*);
    static ModResult update_panel(ModContext*, void*, ModError*);
    static ModResult build_session_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                       UiElementHandle, void*, ModError*);
    static ModResult build_direct_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                      UiElementHandle, void*, ModError*);
    static ModResult build_relay_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                     UiElementHandle, void*, ModError*);
    static ModResult update_window(ModContext*, void*, ModError*);
    static void window_closed(ModContext*, UiWindowHandle, void*);
    static void open_pressed(ModContext*, void*);
    static void menu_selected(ModContext*, void*);
    static void disconnect_pressed(ModContext*, void*);
    static void stop_hosting_pressed(ModContext*, void*);
    static void host_direct_pressed(ModContext*, void*);
    static void join_direct_pressed(ModContext*, void*);
    static void host_relay_pressed(ModContext*, void*);
    static void join_relay_pressed(ModContext*, void*);
    static void direct_code_get(ModContext*, void*, UiControlValue*);
    static void direct_code_set(ModContext*, void*, const UiControlValue*);
    static void relay_code_get(ModContext*, void*, UiControlValue*);
    static void relay_code_set(ModContext*, void*, const UiControlValue*);
    static void copy_direct_code_pressed(ModContext*, void*);
    static void paste_direct_code_pressed(ModContext*, void*);
    static void copy_relay_code_pressed(ModContext*, void*);
    static void paste_relay_code_pressed(ModContext*, void*);
    static void manual_peer_pressed(ModContext*, void*);
    static bool manual_peer_selected(ModContext*, void*);
    static void manual_sync_warp_pressed(ModContext*, void*);
    static void manual_sync_flags_pressed(ModContext*, void*);
    static void refresh_peers_pressed(ModContext*, void*);
    static bool manual_sync_unavailable(ModContext*, void*);
    static bool session_active(ModContext*, void*);
    static bool host_inactive(ModContext*, void*);
    static bool joiner_inactive(ModContext*, void*);
    static bool direct_host_inactive(ModContext*, void*);
    static bool direct_join_inactive(ModContext*, void*);
    static bool relay_host_inactive(ModContext*, void*);
    static bool relay_join_inactive(ModContext*, void*);
    static bool room_setting_locked(ModContext*, void*);
    static bool remote_collision_setting_locked(ModContext*, void*);
    static bool pvp_setting_locked(ModContext*, void*);
    static void dummy_model_get(ModContext*, void*, UiControlValue*);
    static void dummy_model_set(ModContext*, void*, const UiControlValue*);
    static void sync_flags_get(ModContext*, void*, UiControlValue*);
    static void sync_flags_set(ModContext*, void*, const UiControlValue*);
    static void sync_world_get(ModContext*, void*, UiControlValue*);
    static void sync_world_set(ModContext*, void*, const UiControlValue*);
    static void performance_mode_get(ModContext*, void*, UiControlValue*);
    static void performance_mode_set(ModContext*, void*, const UiControlValue*);
    static void remote_collision_get(ModContext*, void*, UiControlValue*);
    static void remote_collision_set(ModContext*, void*, const UiControlValue*);
    static void pvp_get(ModContext*, void*, UiControlValue*);
    static void pvp_set(ModContext*, void*, const UiControlValue*);
};

}  // namespace dusklight_online
