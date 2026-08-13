#include "dusklight_online/online_app.hpp"

#include "dusk/multiplayer/invite_code.hpp"
#include "dusklight_online/game/game_adapter.hpp"
#include "dusklight_online/game/protocol_router.hpp"
#include "dusklight_online/game/visual_bridge.hpp"

#include <mods/service.hpp>
#include <mods/svc/config.h>
#include <mods/svc/ui.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>
#include <vector>

namespace dusklight_online {
namespace {

ModResult add_config(const char* name, ConfigVarType type, const char* defaultString,
                     int64_t defaultInt, bool defaultBool, ConfigVarHandle& output,
                     ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = type;
    desc.default_string = defaultString;
    desc.default_int = defaultInt;
    desc.default_bool = defaultBool;
    if (svc_config->register_var(mod_ctx, &desc, &output) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online configuration");
    }
    return MOD_OK;
}

void add_bound_control(UiElementHandle pane, UiControlKind kind, const char* label,
                       ConfigVarHandle variable, int64_t minimum = 0,
                       int64_t maximum = 0, int64_t step = 1,
                       int32_t maxLength = 0, UiPredicateFn isDisabled = nullptr,
                       void* userData = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = kind;
    control.label = label;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = variable;
    control.min = minimum;
    control.max = maximum;
    control.step = step;
    control.max_length = maxLength;
    control.is_disabled = isDisabled;
    control.user_data = userData;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

void add_session_toggle(UiElementHandle pane, const char* label,
                        UiControlGetFn get, UiControlSetFn set,
                        UiPredicateFn isDisabled, OnlineApp* app) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.get = get;
    control.set = set;
    control.is_disabled = isDisabled;
    control.user_data = app;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

void add_button(UiElementHandle pane, const char* label, UiPressedFn callback,
                OnlineApp* app, bool disableWhileActive = false) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = label;
    control.on_pressed = callback;
    control.user_data = app;
    control.is_disabled = disableWhileActive ? &OnlineApp::session_active : nullptr;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

const char* mode_text(net::Mode mode) {
    switch (mode) {
    case net::Mode::DirectHost: return "direct host";
    case net::Mode::DirectJoin: return "direct join";
    case net::Mode::Relay: return "relay";
    default: return "disabled";
    }
}

const char* state_text(net::State state) {
    switch (state) {
    case net::State::Listening: return "listening";
    case net::State::Connecting: return "connecting";
    case net::State::Connected: return "connected";
    default: return "disconnected";
    }
}

bool room_settings_host_controlled(const net::Status& status) {
    return status.enabled &&
        (status.mode == net::Mode::DirectJoin ||
         (status.mode == net::Mode::Relay && status.welcomed && !status.isOwner));
}

}  // namespace

OnlineApp::OnlineApp() = default;
OnlineApp::~OnlineApp() = default;

ModResult OnlineApp::initialize(ModError* error) {
    if (register_config(error) != MOD_OK) {
        return MOD_ERROR;
    }
    if (register_ui(error) != MOD_OK) {
        return MOD_ERROR;
    }
    game_ = std::make_unique<game::GameAdapter>(transport_);
    if (game_->initialize_hooks(error) != MOD_OK) {
        game_.reset();
        return MOD_ERROR;
    }
    router_ = std::make_unique<game::ProtocolRouter>(*game_);
    return MOD_OK;
}

void OnlineApp::consume_progression_prompt_input() {
    if (game_ != nullptr) {
        game_->consume_progression_prompt_input();
    }
}

void OnlineApp::update() {
    if (reopenWindowPending_ && window_ == 0) {
        reopenWindowPending_ = false;
        open_window();
    }
    transport_.tick();
    const net::Status captureStatus = transport_.status();
    const bool captureSyncFlags = captureStatus.enabled ? captureStatus.settings.syncFlags :
                                                          bool_value(config_.syncFlags, true);
    if (game_ != nullptr) {
        game_->capture_local_mutations_before_remote(captureSyncFlags);
    }
    bool protocolFatal = false;
    while (transport_.has_events()) {
        net::Event event = transport_.pop_event();
        switch (event.kind) {
        case net::EventKind::Connected:
            statusMessage_ = "Connected to " + transport_.status().room;
            game::push_online_notification("Joined lobby " + transport_.status().room);
            livePublishInitialized_ = false;
            break;
        case net::EventKind::Disconnected:
            statusMessage_ = "Disconnected: " + event.detail;
            if (router_ != nullptr) {
                router_->clear();
            }
            if (game_ != nullptr) {
                game_->reset_session();
            }
            break;
        case net::EventKind::Error:
            statusMessage_ = "Online error: " + event.detail;
            break;
        default:
            if (router_ != nullptr) {
                const net::Status routeStatus = transport_.status();
                const bool routeSyncFlags = routeStatus.enabled ? routeStatus.settings.syncFlags :
                    bool_value(config_.syncFlags, true);
                const game::ApplyResult result = router_->route(event, routeSyncFlags);
                if ((result == game::ApplyResult::Rejected ||
                     result == game::ApplyResult::Unsupported) &&
                    !router_->last_error().empty()) {
                    statusMessage_ = "Online protocol error: " + router_->last_error();
                }
                if (router_->fatal_error()) {
                    statusMessage_ = "Online protocol overload: " + router_->last_error();
                    transport_.disconnect();
                    router_->clear();
                    if (game_ != nullptr) game_->reset_session();
                    protocolFatal = true;
                }
            }
            break;
        }
        if (protocolFatal) break;
    }
    if (protocolFatal) return;
    const net::Status currentStatus = transport_.status();
    const bool syncFlagsEnabled = currentStatus.enabled ? currentStatus.settings.syncFlags :
        bool_value(config_.syncFlags, true);
    if (router_ != nullptr) {
        router_->flush(syncFlagsEnabled);
    }
    if (game_ != nullptr) {
        const bool remoteModelEnabled = currentStatus.enabled ? currentStatus.settings.dummyModel :
            bool_value(config_.dummyModel, true);
        const bool syncWorldEnabled = currentStatus.enabled ? currentStatus.settings.syncWorld :
            bool_value(config_.syncWorld, false);
        const bool remoteCollisionEnabled = currentStatus.enabled ?
            net::effective_remote_collision(currentStatus.settings) :
            bool_value(config_.dummyModel, true) &&
                bool_value(config_.remoteCollision, true);
        const bool pvpEnabled = currentStatus.enabled ?
            net::effective_pvp(currentStatus.settings) :
            remoteCollisionEnabled && bool_value(config_.pvp, false);
        game_->update(syncFlagsEnabled, syncWorldEnabled, remoteModelEnabled,
                      bool_value(config_.nameLabels, true), false,
                      remoteCollisionEnabled, pvpEnabled,
                      bool_value(config_.playerList, false));
    }
    publish_live_options();
}

void OnlineApp::shutdown() {
    transport_.disconnect();
    if (router_ != nullptr) {
        router_->clear();
    }
    if (game_ != nullptr) {
        game_->reset_session();
        game_->shutdown_hooks();
    }
    if (window_ != 0) {
        svc_ui->window_close(mod_ctx, window_);
        window_ = 0;
    }
    if (menuTab_ != 0) {
        svc_ui->unregister_menu_tab(mod_ctx, menuTab_);
        menuTab_ = 0;
    }
    panelStatus_ = 0;
    windowStatus_ = 0;
    router_.reset();
    game_.reset();
    livePublishInitialized_ = false;
}

ModResult OnlineApp::register_config(ModError* error) {
    struct StringVar { const char* name; const char* value; ConfigVarHandle* handle; };
    const std::array strings = {
        StringVar{"player-name", "TP Player", &config_.playerName},
        StringVar{"direct-room", "dev", &config_.directRoom},
        StringVar{"bind-host", "0.0.0.0", &config_.bindHost},
        StringVar{"public-host", "127.0.0.1", &config_.publicHost},
        StringVar{"direct-invite", "", &config_.directInvite},
        StringVar{"relay-code", "", &config_.relayCode},
        StringVar{"relay-room", "dev", &config_.relayRoom},
        StringVar{"relay-password", "", &config_.relayPassword},
    };
    for (const auto& variable : strings) {
        if (add_config(variable.name, CONFIG_VAR_STRING, variable.value, 0, false,
                       *variable.handle, error) != MOD_OK) {
            return MOD_ERROR;
        }
    }
    if (add_config("port", CONFIG_VAR_INT, nullptr, 34197, false, config_.port, error) != MOD_OK) {
        return MOD_ERROR;
    }
    struct BoolVar { const char* name; bool value; ConfigVarHandle* handle; };
    const std::array booleans = {
        BoolVar{"relay-local", false, &config_.relayLocal},
        BoolVar{"remote-model", true, &config_.dummyModel},
        BoolVar{"name-labels", true, &config_.nameLabels},
        BoolVar{"sync-flags", true, &config_.syncFlags},
        BoolVar{"sync-world", false, &config_.syncWorld},
        BoolVar{"display-midna", false, &config_.displayMidna},
        BoolVar{"remote-collision", true, &config_.remoteCollision},
        BoolVar{"pvp", false, &config_.pvp},
        BoolVar{"player-list-overlay", false, &config_.playerList},
    };
    for (const auto& variable : booleans) {
        if (add_config(variable.name, CONFIG_VAR_BOOL, nullptr, 0, variable.value,
                       *variable.handle, error) != MOD_OK) {
            return MOD_ERROR;
        }
    }
    return MOD_OK;
}

ModResult OnlineApp::register_ui(ModError* error) {
    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = &OnlineApp::build_panel;
    panel.update = &OnlineApp::update_panel;
    panel.user_data = this;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online UI panel");
    }
    UiMenuTabDesc menu = UI_MENU_TAB_DESC_INIT;
    menu.label = "Online";
    menu.on_selected = &OnlineApp::menu_selected;
    menu.user_data = this;
    if (svc_ui->register_menu_tab(mod_ctx, &menu, &menuTab_) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online menu tab");
    }
    return MOD_OK;
}

std::string OnlineApp::string_value(ConfigVarHandle handle) const {
    size_t length = 0;
    if (svc_config->get_string(mod_ctx, handle, nullptr, 0, &length) != MOD_OK) {
        return {};
    }
    std::vector<char> bytes(length + 1, '\0');
    if (svc_config->get_string(mod_ctx, handle, bytes.data(), bytes.size(), nullptr) != MOD_OK) {
        return {};
    }
    return std::string(bytes.data(), length);
}

bool OnlineApp::bool_value(ConfigVarHandle handle, bool fallback) const {
    bool value = fallback;
    svc_config->get_bool(mod_ctx, handle, &value);
    return value;
}

int64_t OnlineApp::int_value(ConfigVarHandle handle, int64_t fallback) const {
    int64_t value = fallback;
    svc_config->get_int(mod_ctx, handle, &value);
    return value;
}

net::RoomSettings OnlineApp::configured_settings() const {
    net::RoomSettings settings;
    settings.dummyModel = bool_value(config_.dummyModel, true);
    settings.syncFlags = bool_value(config_.syncFlags, true);
    settings.syncWorld = bool_value(config_.syncWorld, false);
    settings.remoteCollision = bool_value(config_.remoteCollision, true);
    settings.pvp = bool_value(config_.pvp, false) && settings.remoteCollision;
    return settings;
}

net::RoomSettings OnlineApp::displayed_settings() const {
    const net::Status status = transport_.status();
    return status.enabled ? status.settings : configured_settings();
}

std::string OnlineApp::status_text() const {
    const net::Status status = transport_.status();
    std::ostringstream text;
    text << statusMessage_ << "\nMode: " << mode_text(status.mode)
         << "\nState: " << state_text(status.state);
    if (status.enabled) {
        text << "\nPlayer: " << status.name << "\nLobby: " << status.room
             << "\nPeers: " << transport_.peers().size();
        for (const auto& [id, name] : transport_.peers()) {
            text << "\n  " << name << " [" << id << ']';
        }
        if (status.mode == net::Mode::Relay) {
            text << "\nOwner: " << (status.isOwner ? "you" : status.ownerClientId)
                 << "\nUDP: " << (status.udpReady ? "ready" : "registering");
        }
        text << "\nRoom settings: flags=" << (status.settings.syncFlags ? "on" : "off")
             << ", world=" << (status.settings.syncWorld ? "on" : "off")
             << ", collision=" << (status.settings.remoteCollision ? "on" : "off")
             << ", PvP=" << (net::effective_pvp(status.settings) ? "on" : "off");
    }
    if (!activeCode_.empty()) {
        text << "\nInvite/relay code: " << activeCode_;
    }
    if (!status.error.empty()) {
        text << "\nLast error: " << status.error;
    }
    if (game_ != nullptr) {
        const std::string manual = game_->manual_sync_status_text();
        if (!manual.empty()) text << "\nManual sync: " << manual;
    }
    return text.str();
}

void OnlineApp::open_window() {
    if (window_ != 0) {
        return;
    }
    refresh_manual_peer_choices();
    static UiTabDesc tabs[3];
    tabs[0] = UI_TAB_DESC_INIT;
    tabs[0].title = "Session";
    tabs[0].build = &OnlineApp::build_session_tab;
    tabs[0].update = &OnlineApp::update_window;
    tabs[0].user_data = this;
    tabs[1] = UI_TAB_DESC_INIT;
    tabs[1].title = "Direct";
    tabs[1].build = &OnlineApp::build_direct_tab;
    tabs[1].user_data = this;
    tabs[2] = UI_TAB_DESC_INIT;
    tabs[2].title = "Relay";
    tabs[2].build = &OnlineApp::build_relay_tab;
    tabs[2].user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = std::size(tabs);
    desc.on_closed = &OnlineApp::window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &window_);
}

void OnlineApp::refresh_manual_peer_choices() {
    const std::string selectedId = selectedManualPeer_ >= 0 &&
        selectedManualPeer_ < static_cast<int64_t>(manualPeerIds_.size()) ?
            manualPeerIds_[selectedManualPeer_] : std::string();
    manualPeerIds_.clear();
    manualPeerLabels_.clear();
    manualPeerOptionPtrs_.clear();
    for (const auto& [id, name] : transport_.peers()) {
        manualPeerIds_.push_back(id);
        manualPeerLabels_.push_back(name + " [" + id + "]");
    }
    for (const std::string& label : manualPeerLabels_) manualPeerOptionPtrs_.push_back(label.c_str());
    selectedManualPeer_ = 0;
    if (!selectedId.empty()) {
        const auto selected = std::find(manualPeerIds_.begin(), manualPeerIds_.end(), selectedId);
        if (selected != manualPeerIds_.end()) {
            selectedManualPeer_ = std::distance(manualPeerIds_.begin(), selected);
        }
    }
}

void OnlineApp::request_manual_sync(bool flagsOnly) {
    if (game_ == nullptr || selectedManualPeer_ < 0 ||
        selectedManualPeer_ >= static_cast<int64_t>(manualPeerIds_.size())) {
        statusMessage_ = "Manual sync failed: reopen Online and choose a connected peer";
        return;
    }
    std::string error;
    if (!game_->request_manual_sync(manualPeerIds_[selectedManualPeer_], flagsOnly, &error)) {
        statusMessage_ = "Manual sync failed: " +
            (error.empty() ? std::string("request could not be sent") : error);
    } else {
        statusMessage_ = flagsOnly ? "Requested flags sync" : "Requested sync and warp";
    }
}

void OnlineApp::host_direct() {
    const int64_t port = int_value(config_.port, 34197);
    if (port < 1 || port > 65535) {
        statusMessage_ = "Direct host failed: port must be 1-65535";
        return;
    }
    net::DirectHostConfig config;
    config.name = string_value(config_.playerName);
    config.room = string_value(config_.directRoom);
    config.bindHost = string_value(config_.bindHost);
    config.publicHost = string_value(config_.publicHost);
    config.port = static_cast<uint16_t>(port);
    config.settings = configured_settings();
    config.wantPuppet = config.settings.dummyModel;
    config.wantMidna = false;
    config.sessionId = dusk::multiplayer::make_session_token(9);
    config.sessionKey = dusk::multiplayer::make_session_token(16);
    dusk::multiplayer::InviteCodePayload invite;
    invite.transport = "direct";
    invite.host = config.publicHost;
    invite.port = config.port;
    invite.room = config.room;
    invite.sessionId = config.sessionId;
    invite.sessionKey = config.sessionKey;
    activeCode_ = dusk::multiplayer::create_invite_code(invite);
    std::string error;
    if (!transport_.start_direct_host(config, &error)) {
        statusMessage_ = "Direct host failed: " + error;
        activeCode_.clear();
    } else {
        svc_config->set_string(mod_ctx, config_.directInvite, activeCode_.c_str());
        statusMessage_ = "Hosting direct lobby";
    }
}

void OnlineApp::join_direct() {
    const std::string code = string_value(config_.directInvite);
    std::string error;
    const auto invite = dusk::multiplayer::decode_invite_code(code, &error);
    if (!invite || invite->transport != "direct") {
        statusMessage_ = "Direct join failed: " +
                         (error.empty() ? std::string("not a direct invite") : error);
        return;
    }
    net::DirectJoinConfig config;
    config.name = string_value(config_.playerName);
    config.room = invite->room;
    config.host = invite->host;
    config.port = static_cast<uint16_t>(invite->port);
    config.sessionId = invite->sessionId;
    config.sessionKey = invite->sessionKey;
    config.settings = configured_settings();
    config.wantPuppet = bool_value(config_.dummyModel, true);
    config.wantMidna = false;
    if (!transport_.start_direct_join(config, &error)) {
        statusMessage_ = "Direct join failed: " + error;
    } else {
        activeCode_ = code;
        statusMessage_ = "Joining direct lobby";
    }
}

void OnlineApp::host_relay() {
    net::RelayConfig config;
    config.name = string_value(config_.playerName);
    config.room = string_value(config_.relayRoom);
    config.password = string_value(config_.relayPassword);
    config.createRoom = true;
    config.settings = configured_settings();
    config.wantPuppet = config.settings.dummyModel;
    const std::string code = string_value(config_.relayCode);
    std::string error;
    const auto endpoint = dusk::multiplayer::decode_invite_code(code, &error);
    if (!endpoint || endpoint->transport != "relay") {
        statusMessage_ = "Relay host failed: " +
                         (error.empty() ? std::string("not a relay code") : error);
        return;
    }
    config.host = bool_value(config_.relayLocal) ? "127.0.0.1" : endpoint->host;
    config.port = static_cast<uint16_t>(endpoint->port);
    config.sessionId = endpoint->sessionId;
    config.sessionKey = endpoint->sessionKey;
    if (!transport_.start_relay(config, &error)) {
        statusMessage_ = "Relay host failed: " + error;
    } else {
        activeCode_ = code;
        statusMessage_ = "Creating relay lobby";
    }
}

void OnlineApp::join_relay() {
    const std::string code = string_value(config_.relayCode);
    std::string error;
    const auto endpoint = dusk::multiplayer::decode_invite_code(code, &error);
    if (!endpoint || endpoint->transport != "relay") {
        statusMessage_ = "Relay join failed: " +
                         (error.empty() ? std::string("not a relay code") : error);
        return;
    }
    net::RelayConfig config;
    config.name = string_value(config_.playerName);
    config.room = string_value(config_.relayRoom);
    config.password = string_value(config_.relayPassword);
    config.host = bool_value(config_.relayLocal) ? "127.0.0.1" : endpoint->host;
    config.port = static_cast<uint16_t>(endpoint->port);
    config.sessionId = endpoint->sessionId;
    config.sessionKey = endpoint->sessionKey;
    config.createRoom = false;
    config.settings = configured_settings();
    config.wantPuppet = bool_value(config_.dummyModel, true);
    if (!transport_.start_relay(config, &error)) {
        statusMessage_ = "Relay join failed: " + error;
    } else {
        activeCode_ = code;
        statusMessage_ = "Joining relay lobby";
    }
}

void OnlineApp::disconnect() {
    transport_.disconnect();
    if (router_ != nullptr) {
        router_->clear();
    }
    if (game_ != nullptr) {
        game_->reset_session();
    }
    statusMessage_ = "Disconnected";
    activeCode_.clear();
    livePublishInitialized_ = false;
}

void OnlineApp::publish_live_options() {
    const net::Status status = transport_.status();
    if (!status.welcomed) {
        livePublishInitialized_ = false;
        return;
    }

    // The active room settings are authoritative after a session starts.
    // Persistent preferences must never overwrite a host's values on welcome
    // or owner migration, and the puppet preference must follow the active
    // room model rather than a joiner's stale local preference.
    const bool wantsPuppet = status.settings.dummyModel;
    const bool wantsMidna = false;

    if (!livePublishInitialized_) {
        lastWantPuppet_ = !wantsPuppet; // force one preference publication
        lastWantMidna_ = !wantsMidna;
        livePublishInitialized_ = true;
    }

    if (wantsPuppet != lastWantPuppet_ || wantsMidna != lastWantMidna_) {
        if (transport_.publish_visual_preferences(wantsPuppet, wantsMidna)) {
            lastWantPuppet_ = wantsPuppet;
            lastWantMidna_ = wantsMidna;
        }
    }
}

ModResult OnlineApp::build_panel(ModContext*, UiElementHandle panel, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.panelStatus_ = 0;
    svc_ui->pane_add_section(mod_ctx, panel, "Online");
    const std::string status = app.status_text();
    svc_ui->pane_add_text(mod_ctx, panel, status.c_str(), &app.panelStatus_);
    app.panelRenderedStatus_ = status;
    add_button(panel, "Open Online", &OnlineApp::open_pressed, &app);
    return MOD_OK;
}

ModResult OnlineApp::update_panel(ModContext*, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (app.panelStatus_ != 0) {
        const std::string status = app.status_text();
        if (status != app.panelRenderedStatus_ &&
            svc_ui->elem_set_text(mod_ctx, app.panelStatus_, status.c_str()) != MOD_OK) {
            // Panel contents are rebuilt on host-tab changes; never poll a
            // generation-checked element after its owning content is gone.
            app.panelStatus_ = 0;
        }
        app.panelRenderedStatus_ = status;
    }
    return MOD_OK;
}

ModResult OnlineApp::build_session_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                       UiElementHandle, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.windowStatus_ = 0;
    svc_ui->pane_add_section(mod_ctx, left, "Current session");
    const std::string status = app.status_text();
    svc_ui->pane_add_text(mod_ctx, left, status.c_str(), &app.windowStatus_);
    app.windowRenderedStatus_ = status;
    add_session_toggle(left, "Remote Link model", &OnlineApp::dummy_model_get,
                       &OnlineApp::dummy_model_set, &OnlineApp::room_setting_locked, &app);
    add_bound_control(left, UI_CONTROL_TOGGLE, "Name labels", app.config_.nameLabels);
    add_bound_control(left, UI_CONTROL_TOGGLE, "Player list overlay", app.config_.playerList);
    add_session_toggle(left, "Sync flags", &OnlineApp::sync_flags_get,
                       &OnlineApp::sync_flags_set, &OnlineApp::room_setting_locked, &app);
    add_session_toggle(left, "Sync world", &OnlineApp::sync_world_get,
                       &OnlineApp::sync_world_set, &OnlineApp::room_setting_locked, &app);
    add_session_toggle(left, "Remote collision", &OnlineApp::remote_collision_get,
                       &OnlineApp::remote_collision_set,
                       &OnlineApp::remote_collision_setting_locked, &app);
    add_session_toggle(left, "PvP", &OnlineApp::pvp_get, &OnlineApp::pvp_set,
                       &OnlineApp::pvp_setting_locked, &app);
    svc_ui->pane_add_section(mod_ctx, left, "Manual peer sync");
    if (!app.manualPeerOptionPtrs_.empty()) {
        UiControlDesc peer = UI_CONTROL_DESC_INIT;
        peer.kind = UI_CONTROL_SELECT;
        peer.label = "Sync from peer";
        peer.get = &OnlineApp::manual_peer_get;
        peer.set = &OnlineApp::manual_peer_set;
        peer.user_data = &app;
        peer.options = app.manualPeerOptionPtrs_.data();
        peer.option_count = app.manualPeerOptionPtrs_.size();
        svc_ui->pane_add_control(mod_ctx, left, &peer, nullptr);
        add_button(left, "Sync flags", &OnlineApp::manual_sync_flags_pressed, &app);
        add_button(left, "Sync and warp", &OnlineApp::manual_sync_warp_pressed, &app);
    } else {
        svc_ui->pane_add_text(mod_ctx, left,
            "No connected peers are available for manual sync.",
            nullptr);
    }
    add_button(left, "Refresh connected peers", &OnlineApp::refresh_peers_pressed, &app);
    add_button(left, "Disconnect", &OnlineApp::disconnect_pressed, &app);
    return MOD_OK;
}

ModResult OnlineApp::build_direct_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                      UiElementHandle, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    // Activating any tab destroys the previous tab's elements.
    app.windowStatus_ = 0;
    svc_ui->pane_add_section(mod_ctx, left, "Direct host / join");
    add_bound_control(left, UI_CONTROL_STRING, "Player name", app.config_.playerName, 0, 0, 1, 32);
    add_bound_control(left, UI_CONTROL_STRING, "Lobby name", app.config_.directRoom, 0, 0, 1, 64);
    add_bound_control(left, UI_CONTROL_STRING, "Bind host", app.config_.bindHost, 0, 0, 1, 255);
    add_bound_control(left, UI_CONTROL_STRING, "Public host", app.config_.publicHost, 0, 0, 1, 255);
    add_bound_control(left, UI_CONTROL_NUMBER, "Port", app.config_.port, 1, 65535, 1);
    add_button(left, "Host direct lobby", &OnlineApp::host_direct_pressed, &app, true);
    add_bound_control(left, UI_CONTROL_STRING, "Invite code", app.config_.directInvite, 0, 0, 1, 2048);
    add_button(left, "Join direct lobby", &OnlineApp::join_direct_pressed, &app, true);
    return MOD_OK;
}

ModResult OnlineApp::build_relay_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                     UiElementHandle, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    // Activating any tab destroys the previous tab's elements.
    app.windowStatus_ = 0;
    svc_ui->pane_add_section(mod_ctx, left, "Relay host / join");
    add_bound_control(left, UI_CONTROL_STRING, "Player name", app.config_.playerName, 0, 0, 1, 32);
    add_bound_control(left, UI_CONTROL_STRING, "Relay code", app.config_.relayCode, 0, 0, 1, 2048);
    add_bound_control(left, UI_CONTROL_STRING, "Lobby name", app.config_.relayRoom, 0, 0, 1, 64);
    add_bound_control(left, UI_CONTROL_STRING, "Password", app.config_.relayPassword, 0, 0, 1, 128);
    add_bound_control(left, UI_CONTROL_TOGGLE, "Relay is on this PC", app.config_.relayLocal);
    add_button(left, "Host relay lobby", &OnlineApp::host_relay_pressed, &app, true);
    add_button(left, "Join relay lobby", &OnlineApp::join_relay_pressed, &app, true);
    return MOD_OK;
}

ModResult OnlineApp::update_window(ModContext*, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (app.windowStatus_ != 0) {
        const std::string status = app.status_text();
        if (status != app.windowRenderedStatus_ &&
            svc_ui->elem_set_text(mod_ctx, app.windowStatus_, status.c_str()) != MOD_OK) {
            // A tab rebuild invalidates every element handle from its prior
            // generation. Stop immediately if the host rebuilt underneath us.
            app.windowStatus_ = 0;
        }
        app.windowRenderedStatus_ = status;
    }
    return MOD_OK;
}

void OnlineApp::window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.window_ = 0;
    app.windowStatus_ = 0;
}

void OnlineApp::open_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->open_window(); }
void OnlineApp::menu_selected(ModContext*, void* data) { static_cast<OnlineApp*>(data)->open_window(); }
void OnlineApp::disconnect_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->disconnect(); }
void OnlineApp::host_direct_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->host_direct(); }
void OnlineApp::join_direct_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->join_direct(); }
void OnlineApp::host_relay_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->host_relay(); }
void OnlineApp::join_relay_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->join_relay(); }
void OnlineApp::manual_peer_get(ModContext*, void* data, UiControlValue* value) {
    value->int_value = static_cast<OnlineApp*>(data)->selectedManualPeer_;
}
void OnlineApp::manual_peer_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (value->int_value >= 0 &&
        value->int_value < static_cast<int64_t>(app.manualPeerIds_.size())) {
        app.selectedManualPeer_ = value->int_value;
    }
}
void OnlineApp::manual_sync_warp_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->request_manual_sync(false);
}
void OnlineApp::manual_sync_flags_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->request_manual_sync(true);
}
void OnlineApp::refresh_peers_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.reopenWindowPending_ = true;
    if (app.window_ != 0) svc_ui->window_close(mod_ctx, app.window_);
}
bool OnlineApp::manual_sync_unavailable(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    return !app.transport_.status().welcomed || app.manualPeerIds_.empty();
}
bool OnlineApp::session_active(ModContext*, void* data) {
    return static_cast<OnlineApp*>(data)->transport_.status().enabled;
}
bool OnlineApp::room_setting_locked(ModContext*, void* data) {
    const net::Status status = static_cast<OnlineApp*>(data)->transport_.status();
    return room_settings_host_controlled(status);
}
bool OnlineApp::remote_collision_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    const bool roomLocked = room_settings_host_controlled(status);
    const bool modelEnabled = status.enabled ? status.settings.dummyModel :
                                               app.bool_value(app.config_.dummyModel, true);
    return roomLocked || !modelEnabled;
}
bool OnlineApp::pvp_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    const bool roomLocked = room_settings_host_controlled(status);
    // Online locks this control from the raw room collision option. The
    // remote-model option participates only in effective runtime PvP.
    const bool collisionEnabled = status.enabled ? status.settings.remoteCollision :
        app.bool_value(app.config_.remoteCollision, true);
    return roomLocked || !collisionEnabled;
}

void OnlineApp::dummy_model_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().dummyModel;
}

void OnlineApp::dummy_model_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_config->set_bool(mod_ctx, app.config_.dummyModel, value->bool_value);
    const net::Status status = app.transport_.status();
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.dummyModel = value->bool_value;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::sync_flags_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().syncFlags;
}

void OnlineApp::sync_flags_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_config->set_bool(mod_ctx, app.config_.syncFlags, value->bool_value);
    const net::Status status = app.transport_.status();
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.syncFlags = value->bool_value;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::sync_world_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().syncWorld;
}

void OnlineApp::sync_world_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_config->set_bool(mod_ctx, app.config_.syncWorld, value->bool_value);
    const net::Status status = app.transport_.status();
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.syncWorld = value->bool_value;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::remote_collision_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().remoteCollision;
}

void OnlineApp::remote_collision_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_config->set_bool(mod_ctx, app.config_.remoteCollision, value->bool_value);
    if (!value->bool_value) svc_config->set_bool(mod_ctx, app.config_.pvp, false);
    const net::Status status = app.transport_.status();
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.remoteCollision = value->bool_value;
        if (!settings.remoteCollision) settings.pvp = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::pvp_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = net::effective_pvp(
        static_cast<OnlineApp*>(data)->displayed_settings());
}

void OnlineApp::pvp_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_config->set_bool(mod_ctx, app.config_.pvp, value->bool_value);
    const net::Status status = app.transport_.status();
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.pvp = value->bool_value && settings.remoteCollision;
        app.transport_.publish_room_settings(settings);
    }
}

}  // namespace dusklight_online
