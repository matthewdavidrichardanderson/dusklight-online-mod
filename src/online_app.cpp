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
#include <cctype>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dusklight_online {
namespace {

constexpr uint32_t kManualSyncCooldownTicks = 5 * 30;

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

bool never_modified(ModContext*, void*) { return false; }

void add_bound_control(UiElementHandle pane, UiControlKind kind, const char* label,
                       ConfigVarHandle variable, int64_t minimum = 0,
                       int64_t maximum = 0, int64_t step = 1,
                       int32_t maxLength = 0, UiPredicateFn isDisabled = nullptr,
                       void* userData = nullptr, const char* styleClass = nullptr,
                       const char* layoutClass = nullptr, const char* helpRml = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = kind;
    control.label = label;
    control.help_rml = helpRml;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = variable;
    control.min = minimum;
    control.max = maximum;
    control.step = step;
    control.max_length = maxLength;
    control.is_disabled = isDisabled;
    control.is_modified = &never_modified;
    control.user_data = userData;
    UiElementHandle element = 0;
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, &element) == MOD_OK &&
        element != 0 && styleClass != nullptr) {
        svc_ui->elem_set_class(mod_ctx, element, styleClass, true);
    }
    if (element != 0 && layoutClass != nullptr) {
        svc_ui->elem_set_class(mod_ctx, element, layoutClass, true);
    }
}

void add_session_toggle(UiElementHandle pane, const char* label,
                        UiControlGetFn get, UiControlSetFn set,
                        UiPredicateFn isDisabled, OnlineApp* app,
                        const char* helpRml = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = helpRml;
    control.get = get;
    control.set = set;
    control.is_disabled = isDisabled;
    control.is_modified = &never_modified;
    control.user_data = app;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

void add_form_string(UiElementHandle pane, const char* label, ConfigVarHandle variable,
                     int32_t maxLength, const char* layoutClass = nullptr) {
    add_bound_control(pane, UI_CONTROL_STRING, label, variable, 0, 0, 1, maxLength,
                      nullptr, nullptr, "online-form-field", layoutClass);
}

UiElementHandle add_button(UiElementHandle pane, const char* label, UiPressedFn callback,
                           void* userData, UiPredicateFn isDisabled = nullptr,
                           UiPredicateFn isModified = nullptr,
                           const char* styleClass = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = label;
    control.on_pressed = callback;
    control.user_data = userData;
    control.is_disabled = isDisabled;
    control.is_modified = isModified;
    UiElementHandle element = 0;
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, &element) == MOD_OK &&
        element != 0 && styleClass != nullptr) {
        svc_ui->elem_set_class(mod_ctx, element, styleClass, true);
    }
    return element;
}

void add_code_control(UiElementHandle pane, const char* label, UiControlGetFn get,
                      UiControlSetFn set, OnlineApp* app) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_STRING;
    control.label = label;
    control.get = get;
    control.set = set;
    control.is_modified = &never_modified;
    control.user_data = app;
    control.max_length = 2048;
    UiElementHandle element = 0;
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, &element) == MOD_OK && element != 0) {
        svc_ui->elem_set_class(mod_ctx, element, "online-code", true);
        svc_ui->elem_set_class(mod_ctx, element, "online-form-field", true);
        svc_ui->elem_set_class(mod_ctx, element, "online-code-row", true);
    }
}

std::string rml_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string trim_clipboard_text(std::string text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string friendly_status_text(std::string text) {
    std::replace(text.begin(), text.end(), '_', ' ');
    if (!text.empty()) text.front() = static_cast<char>(std::toupper(
        static_cast<unsigned char>(text.front())));
    return text;
}

std::string friendly_error_sentence(std::string_view error) {
    if (error.empty()) return {};
    if (error == "lobby_not_found") return "The lobby does not exist.";
    if (error == "lobby_exists") return "A lobby with that name already exists.";
    if (error == "bad_password") return "The lobby password is incorrect.";
    if (error == "lobby_full") return "The lobby is full.";
    if (error == "protocol_version") return "The relay protocol is incompatible.";
    if (error == "password_too_short") return "The lobby password is too short.";
    if (error == "password_too_long") return "The lobby password is too long.";
    if (error == "lobby_too_long") return "The lobby name is too long.";
    if (error == "username_too_long") return "The player name is too long.";
    if (error == "remote closed") return "The connection was closed.";
    std::string sentence = friendly_status_text(std::string(error));
    if (!sentence.empty() && sentence.back() != '.' && sentence.back() != '!' &&
        sentence.back() != '?') {
        sentence.push_back('.');
    }
    return sentence;
}

std::string failure_message(std::string_view prefix, std::string_view detail) {
    std::string message(prefix);
    if (!message.empty() && message.back() != '.') message.push_back('.');
    const std::string reason = friendly_error_sentence(detail);
    if (!reason.empty()) {
        if (!message.empty()) message.push_back(' ');
        message += reason;
    }
    return message;
}

bool write_clipboard(std::string_view text) {
#ifdef _WIN32
    if (text.empty() || !OpenClipboard(nullptr)) return false;
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }
    void* const bytes = GlobalLock(memory);
    if (bytes == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(bytes, text.data(), text.size());
    static_cast<char*>(bytes)[text.size()] = '\0';
    GlobalUnlock(memory);
    EmptyClipboard();
    if (SetClipboardData(CF_TEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
#else
    (void)text;
    return false;
#endif
}

std::string read_clipboard() {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return {};
    const HANDLE handle = GetClipboardData(CF_TEXT);
    if (handle == nullptr) {
        CloseClipboard();
        return {};
    }
    const char* const bytes = static_cast<const char*>(GlobalLock(handle));
    if (bytes == nullptr) {
        CloseClipboard();
        return {};
    }
    std::string text(bytes);
    GlobalUnlock(handle);
    CloseClipboard();
    return trim_clipboard_text(std::move(text));
#else
    return {};
#endif
}

void push_clipboard_toast(const char* message, bool warning = false) {
    const std::string body = warning ?
        "<row><span>" + rml_escape(message) +
            "</span><icon class=\"warning\"></icon></row>" :
        rml_escape(message);
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.type = warning ? "online-warning" : "online";
    toast.body_rml = body.c_str();
    toast.duration_ms = 2500;
    svc_ui->push_toast(mod_ctx, &toast);
}

constexpr const char* kOnlineOverlayRcss = R"RCSS(
toast[mod-id="io.github.mdra5000.dusklight_online"] {
    top: 76dp;
    padding: 7dp 10dp;
    gap: 3dp;
    border-radius: 10dp;
}
toast.online heading,
toast.online-warning heading {
    display: none;
}
toast.online-warning {
    border: 1dp #C2A42D;
}
toast.online-warning message row {
    align-items: center;
    gap: 10dp;
}
toast.online-warning message row > span {
    flex: 1 1 auto;
}
)RCSS";

constexpr const char* kOnlineWindowRcss = R"RCSS(
window content pane:last-of-type > div {
    line-height: 1.2;
}
window content pane.online-form-pane {
    flex-flow: row wrap;
    align-content: flex-start;
}
window content pane.online-form-pane > div {
    flex: 0 0 100%;
}
.online-state {
    display: block;
    padding: 14dp 16dp;
    margin-bottom: 6dp;
    border: 1dp rgba(146, 135, 91, 55%);
    border-radius: 8dp;
    background-color: rgba(224, 219, 200, 5%);
}
.online-state-title {
    display: block;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
    font-size: 22dp;
    color: #e0dbc8;
}
.online-state-detail {
    display: block;
    margin-top: 4dp;
    font-size: 16dp;
    color: rgba(224, 219, 200, 70%);
}
.online-detail-row {
    display: flex;
    margin-bottom: 3dp;
}
.online-detail-label {
    display: block;
    flex: 0 0 116dp;
    width: 116dp;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
    color: rgba(224, 219, 200, 55%);
}
.online-detail-value {
    display: block;
    flex: 1 1 auto;
    min-width: 0;
}
button.online-peer-selected {
    color: #e0dbc8;
    background-color: rgba(194, 164, 45, 24%);
    box-shadow: #d4b83f 0 0 0 1dp;
}
button.online-sync-pending {
    opacity: 0.35;
    cursor: default;
}
select-button.online-code {
    min-width: 0;
}
select-button.online-form-field {
    display: block;
    position: relative;
    flex: 1 1 100%;
    height: 57dp;
    min-width: 0;
    padding: 0dp;
    border-radius: 10dp;
    background-color: rgba(17, 16, 10, 32%);
}
select-button.online-form-field.online-half-field {
    flex: 1 1 100%;
}
select-button.online-code-row {
    flex: 1 1 100%;
}
select-button.online-form-field key {
    display: block;
    position: absolute;
    top: 7dp;
    left: 12dp;
    right: 12dp;
    height: 17dp;
    margin: 0dp;
    font-size: 15dp;
    line-height: 17dp;
    opacity: 0.72;
}
select-button.online-form-field value {
    display: block;
    position: absolute;
    top: 29dp;
    left: 12dp;
    right: 12dp;
    width: auto;
    height: 22dp;
    min-width: 0;
    text-align: left;
    font-size: 20dp;
    line-height: 22dp;
}
select-button.online-form-field input {
    position: absolute;
    top: 28dp;
    left: 12dp;
    right: 12dp;
    width: auto;
    height: 24dp;
    min-width: 0;
    text-align: left;
    font-size: 20dp;
    line-height: 22dp;
}
select-button.online-code value {
    display: block;
    right: auto;
    width: 92%;
    min-width: 0;
    max-width: 92%;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
}
select-button.online-code input {
    min-width: 0;
}
button.online-copy-action {
    display: block;
    flex: 1 1 100%;
    height: 32dp;
    min-height: 32dp;
    align-self: center;
    padding: 0dp 12dp;
    line-height: 32dp;
    text-align: center;
    font-size: 17dp;
    font-weight: normal;
    color: rgba(204, 184, 119, 92%);
    box-shadow: rgba(146, 135, 91, 25%) 0 0 0 1dp;
}
button.online-primary-action {
    color: #d4b83f;
    box-shadow: rgba(194, 164, 45, 65%) 0 0 0 1dp;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
}
button.online-danger-action {
    color: #e2534d;
    box-shadow: rgba(226, 83, 77, 65%) 0 0 0 1dp;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
}
select-button.online-wide-control {
    flex: 1 1 100%;
}
window content pane.online-form-pane > button.online-primary-action,
window content pane.online-form-pane > button.online-danger-action {
    flex: 1 1 100%;
}
button.online-primary-action:disabled,
button.online-danger-action:disabled,
button.online-copy-action:disabled,
.online-session-actions-hidden {
    display: none;
}
window content pane.online-form-pane > button.online-primary-action:not(:disabled):focus-visible,
window content pane.online-form-pane > button.online-primary-action:not(:disabled):active,
window content pane.online-form-pane > button.online-danger-action:not(:disabled):focus-visible,
window content pane.online-form-pane > button.online-danger-action:not(:disabled):active {
    opacity: 0.9;
    background-color: rgba(17, 16, 10, 20%);
}
window content pane.online-form-pane > button:not(:disabled):selected,
window content pane.online-form-pane > select-button:not(:disabled):selected,
window content pane.online-session-pane > button:not(:disabled):selected {
    opacity: 1;
    background-color: rgba(17, 16, 10, 0%);
}
)RCSS";

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

bool room_settings_locked(const net::Status& status, bool relayHostIntent) {
    if (!status.enabled) return false;
    if (status.mode == net::Mode::DirectJoin) return true;
    if (status.mode == net::Mode::Relay) return !relayHostIntent && !status.isOwner;
    return false;
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
    if (reopenSyncWindowPending_ && syncWindow_ == 0) {
        reopenSyncWindowPending_ = false;
        open_sync_window();
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
        if (event.kind == net::EventKind::Message &&
            event.message.value("type", std::string()) == "owner_changed" &&
            event.ingress.mode == net::Mode::Relay) {
            const std::string ownerId = event.message.value("owner_client_id", std::string());
            const bool isOwner = !event.ingress.clientId.empty() &&
                                 ownerId == event.ingress.clientId;
            if (relayOwnerStateKnown_ && !wasRelayOwner_ && isOwner) {
                game::push_online_notification("You are now the lobby host.");
            }
            relayOwnerStateKnown_ = true;
            wasRelayOwner_ = isOwner;
        }
        switch (event.kind) {
        case net::EventKind::Connected:
            connectedLobbyName_ = transport_.status().room;
            statusMessage_ = "Connected to " + connectedLobbyName_;
            game::push_online_notification("Joined lobby " + connectedLobbyName_ + ".");
            pendingLobbyFailurePrefix_.clear();
            pendingLobbyFailureNotified_ = false;
            relayOwnerStateKnown_ = event.ingress.mode == net::Mode::Relay;
            wasRelayOwner_ = relayOwnerStateKnown_ &&
                event.message.value("owner_client_id", std::string()) ==
                    event.message.value("client_id", event.ingress.clientId);
            livePublishInitialized_ = false;
            break;
        case net::EventKind::Disconnected:
            if (!connectedLobbyName_.empty()) {
                if (event.detail == "user requested") {
                    game::push_online_notification("Left lobby " + connectedLobbyName_ + ".");
                } else {
                    game::push_online_notification(
                        failure_message("Disconnected from lobby " + connectedLobbyName_,
                                        event.detail),
                        5.0f, true);
                }
                connectedLobbyName_.clear();
            } else if (!pendingLobbyFailurePrefix_.empty()) {
                notify_lobby_attempt_failure(event.detail);
            }
            if (event.detail == "user requested" && !requestedDisconnectStatus_.empty()) {
                statusMessage_ = requestedDisconnectStatus_;
            } else {
                statusMessage_ = event.detail.empty() ? "Disconnected" :
                    "Disconnected: " + event.detail;
            }
            requestedDisconnectStatus_.clear();
            pendingLobbyFailurePrefix_.clear();
            pendingLobbyFailureNotified_ = false;
            relayHostIntent_ = false;
            relayOwnerStateKnown_ = false;
            wasRelayOwner_ = false;
            manualSyncCooldownTicks_ = 0;
            set_manual_sync_pending_visual(false);
            if (router_ != nullptr) {
                router_->clear();
            }
            if (game_ != nullptr) {
                game_->reset_session();
            }
            break;
        case net::EventKind::Error:
            if (!pendingLobbyFailurePrefix_.empty()) {
                statusMessage_ = "Online error: " + event.detail;
                notify_lobby_attempt_failure(event.detail);
            } else if (!event.detail.empty()) {
                statusMessage_ = "Online error: " + event.detail;
                game::push_online_notification(
                    failure_message("Online error", event.detail), 5.0f, true);
            }
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
        // World synchronization remains protocol-compatible but is intentionally
        // dormant until it is complete enough to expose as a supported feature.
        constexpr bool syncWorldEnabled = false;
        const bool remoteCollisionEnabled = currentStatus.enabled ?
            net::effective_remote_collision(currentStatus.settings) :
            bool_value(config_.dummyModel, true) &&
                bool_value(config_.remoteCollision, true);
        const bool pvpEnabled = currentStatus.enabled ?
            net::effective_pvp(currentStatus.settings) :
            remoteCollisionEnabled && bool_value(config_.pvp, false);
        const bool semanticRenderingEnabled = currentStatus.enabled ?
            currentStatus.settings.performanceMode &&
                currentStatus.semanticVisualsReady :
            bool_value(config_.semanticRenderingExperiment, true);
        game_->update(syncFlagsEnabled, syncWorldEnabled, remoteModelEnabled,
                      bool_value(config_.nameLabels, true), false,
                      semanticRenderingEnabled,
                      remoteCollisionEnabled, pvpEnabled,
                      bool_value(config_.playerList, false));
        const bool waiting = game_->manual_sync_waiting();
        if (manualSyncWasWaiting_ && !waiting && game_->manual_sync_failed()) {
            if (game_->manual_sync_timed_out()) {
                game::push_online_notification(
                    "Sync request failed. Ensure both players are not in a cutscene.",
                    5.0f, true);
            } else {
                game::push_online_notification("Sync request failed.", 5.0f, true);
            }
        }
        manualSyncWasWaiting_ = waiting;
    }
    if (manualSyncCooldownTicks_ > 0 && --manualSyncCooldownTicks_ == 0) {
        set_manual_sync_pending_visual(false);
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
    if (settingsWindow_ != 0) {
        svc_ui->window_close(mod_ctx, settingsWindow_);
        settingsWindow_ = 0;
    }
    if (syncWindow_ != 0) {
        svc_ui->window_close(mod_ctx, syncWindow_);
        syncWindow_ = 0;
    }
    if (window_ != 0) {
        svc_ui->window_close(mod_ctx, window_);
        window_ = 0;
    }
    if (menuTab_ != 0) {
        svc_ui->unregister_menu_tab(mod_ctx, menuTab_);
        menuTab_ = 0;
    }
    if (overlayStyle_ != 0) {
        svc_ui->unregister_styles(mod_ctx, overlayStyle_);
        overlayStyle_ = 0;
    }
    panelStatus_ = 0;
    windowStatus_ = 0;
    router_.reset();
    game_.reset();
    livePublishInitialized_ = false;
    manualSyncWasWaiting_ = false;
    manualSyncCooldownTicks_ = 0;
    manualSyncFlagsButton_ = 0;
    manualSyncWarpButton_ = 0;
    relayOwnerStateKnown_ = false;
    wasRelayOwner_ = false;
    pendingLobbyFailurePrefix_.clear();
    pendingLobbyFailureNotified_ = false;
    connectedLobbyName_.clear();
}

ModResult OnlineApp::register_config(ModError* error) {
    struct StringVar { const char* name; const char* value; ConfigVarHandle* handle; };
    const std::array strings = {
        StringVar{"player-name", "Player", &config_.playerName},
        StringVar{"direct-room", "Lobby", &config_.directRoom},
        StringVar{"bind-host", "0.0.0.0", &config_.bindHost},
        StringVar{"public-host", "127.0.0.1", &config_.publicHost},
        StringVar{"direct-invite", "", &config_.directInvite},
        StringVar{"relay-code", "", &config_.relayCode},
        StringVar{"relay-room", "Lobby", &config_.relayRoom},
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
        BoolVar{"display-midna", false, &config_.displayMidna},
        BoolVar{"semantic-rendering-experiment", true,
                &config_.semanticRenderingExperiment},
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
    if (svc_ui->register_styles(mod_ctx, UI_SCOPE_OVERLAY, kOnlineOverlayRcss,
                                &overlayStyle_) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online overlay styles");
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
    settings.syncWorld = false;
    settings.performanceMode = bool_value(config_.semanticRenderingExperiment, true);
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
    std::string activity = "Not connected";
    std::string connection = "—";
    std::string session = "—";
    std::string health = "Disconnected";
    if (status.enabled) {
        const bool hosting = status.mode == net::Mode::DirectHost ||
            (status.mode == net::Mode::Relay && (relayHostIntent_ || status.isOwner));
        activity = hosting ? "Hosting" : "Joining";
        connection = status.mode == net::Mode::Relay ? "Relay" : "Direct";
        const size_t playerCount = transport_.peers().size() + 1u;
        session = (status.room.empty() ? std::string("Unnamed lobby") : status.room) +
            " — " + std::to_string(playerCount) + (playerCount == 1 ? " player" : " players");
        if (!status.error.empty()) {
            health = "Error — " + friendly_status_text(status.error);
        } else {
            switch (status.state) {
            case net::State::Connected: health = "Connected"; break;
            case net::State::Connecting: health = "Connecting"; break;
            case net::State::Listening: health = "Ready — waiting for players"; break;
            default: health = "Disconnected"; break;
            }
        }
    } else if (!status.error.empty()) {
        health = "Error — " + friendly_status_text(status.error);
    } else if (!statusMessage_.empty() && statusMessage_ != "Not connected") {
        health = friendly_status_text(statusMessage_);
    }

    const auto summary_row = [](std::string_view label, std::string_view value) {
        return "<div style=\"display: flex; margin-bottom: 3dp;\">"
            "<span style=\"display: block; flex: 0 0 116dp; width: 116dp; "
            "font-family: 'Fira Sans Condensed'; font-weight: bold; "
            "color: rgba(224, 219, 200, 55%);\">" + rml_escape(label) +
            "</span><span style=\"display: block; flex: 1 1 auto; min-width: 0;\">" +
            rml_escape(value) + "</span></div>";
    };
    return summary_row("Activity", activity) +
        summary_row("Connection", connection) +
        summary_row("Session", session) +
        summary_row("Status", health);
}

std::string OnlineApp::dashboard_rml() const {
    const net::Status status = transport_.status();
    const auto detail_row = [](std::string_view label, std::string_view value) {
        return "<div class=\"online-detail-row\"><span class=\"online-detail-label\">" +
            rml_escape(label) + "</span><span class=\"online-detail-value\">" +
            rml_escape(value) + "</span></div>";
    };

    std::string activity = "Not connected";
    std::string connection = "—";
    std::string session = "—";
    std::string health = "Disconnected";
    if (status.enabled) {
        const bool hosting = status.mode == net::Mode::DirectHost ||
            (status.mode == net::Mode::Relay && (relayHostIntent_ || status.isOwner));
        activity = hosting ? "Hosting" : "Joining";
        connection = status.mode == net::Mode::Relay ? "Relay" : "Direct";
        const size_t playerCount = transport_.peers().size() + 1u;
        session = (status.room.empty() ? std::string("Unnamed lobby") : status.room) +
            " — " + std::to_string(playerCount) + (playerCount == 1 ? " player" : " players");
        if (!status.error.empty()) {
            health = "Error — " + friendly_status_text(status.error);
        } else {
            switch (status.state) {
            case net::State::Connected: health = "Connected"; break;
            case net::State::Connecting: health = "Connecting"; break;
            case net::State::Listening: health = "Ready — waiting for players"; break;
            default: health = "Disconnected"; break;
            }
        }
    } else if (!status.error.empty()) {
        health = "Error — " + friendly_status_text(status.error);
    } else if (!statusMessage_.empty() && statusMessage_ != "Not connected") {
        health = friendly_status_text(statusMessage_);
    }

    std::string rml;
    rml += detail_row("Activity", activity);
    rml += detail_row("Connection", connection);
    rml += detail_row("Session", session);
    rml += detail_row("Status", health);
    return rml;
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
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &window_);
}

void OnlineApp::open_settings_window() {
    if (settingsWindow_ != 0) return;
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Settings";
    tab.build = &OnlineApp::build_settings_tab;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::settings_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &settingsWindow_);
}

void OnlineApp::open_sync_window() {
    if (syncWindow_ != 0) return;
    refresh_manual_peer_choices();
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Sync players";
    tab.build = &OnlineApp::build_sync_tab;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::sync_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &syncWindow_);
}

void OnlineApp::refresh_manual_peer_choices() {
    const std::string selectedId = selectedManualPeer_ >= 0 &&
        selectedManualPeer_ < static_cast<int64_t>(manualPeerIds_.size()) ?
            manualPeerIds_[selectedManualPeer_] : std::string();
    manualPeerIds_.clear();
    manualPeerLabels_.clear();
    manualPeerButtonContexts_.clear();
    for (const auto& [id, name] : transport_.peers()) {
        manualPeerIds_.push_back(id);
        manualPeerLabels_.push_back(name.empty() ? "Player" : name);
    }
    manualPeerButtonContexts_.reserve(manualPeerIds_.size());
    for (int64_t i = 0; i < static_cast<int64_t>(manualPeerIds_.size()); ++i) {
        manualPeerButtonContexts_.push_back({this, i});
    }
    selectedManualPeer_ = 0;
    if (!selectedId.empty()) {
        const auto selected = std::find(manualPeerIds_.begin(), manualPeerIds_.end(), selectedId);
        if (selected != manualPeerIds_.end()) {
            selectedManualPeer_ = std::distance(manualPeerIds_.begin(), selected);
        }
    }
}

void OnlineApp::set_manual_sync_pending_visual(bool pending) {
    const UiElementHandle buttons[] = {manualSyncFlagsButton_, manualSyncWarpButton_};
    for (const UiElementHandle button : buttons) {
        if (button != 0 &&
            svc_ui->elem_set_class(mod_ctx, button, "online-sync-pending", pending) != MOD_OK) {
            if (button == manualSyncFlagsButton_) manualSyncFlagsButton_ = 0;
            if (button == manualSyncWarpButton_) manualSyncWarpButton_ = 0;
        }
    }
}

void OnlineApp::begin_lobby_attempt(std::string failurePrefix) {
    pendingLobbyFailurePrefix_ = std::move(failurePrefix);
    pendingLobbyFailureNotified_ = false;
}

void OnlineApp::notify_lobby_attempt_failure(std::string_view detail) {
    if (pendingLobbyFailurePrefix_.empty() || pendingLobbyFailureNotified_) return;
    game::push_online_notification(
        failure_message(pendingLobbyFailurePrefix_, detail), 5.0f, true);
    pendingLobbyFailureNotified_ = true;
}

void OnlineApp::request_manual_sync(bool flagsOnly) {
    if (manualSyncCooldownTicks_ > 0) return;
    if (game_ == nullptr || selectedManualPeer_ < 0 ||
        selectedManualPeer_ >= static_cast<int64_t>(manualPeerIds_.size())) {
        statusMessage_ = "Manual sync failed: reopen Online and choose a connected peer";
        game::push_online_notification(
            "Sync request failed. Choose a connected player.", 5.0f, true);
        return;
    }
    std::string error;
    if (!game_->request_manual_sync(manualPeerIds_[selectedManualPeer_], flagsOnly, &error)) {
        statusMessage_ = "Manual sync failed: " +
            (error.empty() ? std::string("request could not be sent") : error);
        game::push_online_notification(
            failure_message("Sync request failed",
                            error.empty() ? "The request could not be sent." : error),
            5.0f, true);
    } else {
        manualSyncWasWaiting_ = true;
        manualSyncCooldownTicks_ = kManualSyncCooldownTicks;
        set_manual_sync_pending_visual(true);
        const std::string& peerName = manualPeerLabels_[selectedManualPeer_];
        game::push_online_notification(
            (flagsOnly ? "Flag sync to " : "Sync to ") + peerName + " requested.");
        statusMessage_ = flagsOnly ? "Requested flags sync" : "Requested sync and warp";
    }
}

void OnlineApp::host_direct() {
    const int64_t port = int_value(config_.port, 34197);
    if (port < 1 || port > 65535) {
        statusMessage_ = "Direct host failed: port must be 1-65535";
        game::push_online_notification(
            "Could not host the direct lobby. The port must be between 1 and 65535.",
            5.0f, true);
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
        game::push_online_notification(
            failure_message("Could not host the direct lobby", error), 5.0f, true);
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
        game::push_online_notification(
            failure_message("Could not join the direct lobby",
                            error.empty() ? "The invite code is not for a direct lobby." : error),
            5.0f, true);
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
        game::push_online_notification(
            failure_message("Could not join the direct lobby", error), 5.0f, true);
    } else {
        begin_lobby_attempt("Could not join the direct lobby");
        activeCode_ = code;
        statusMessage_ = "Joining direct lobby";
    }
}

void OnlineApp::host_relay() {
    relayHostIntent_ = false;
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
        game::push_online_notification(
            failure_message("Could not create the relay lobby",
                            error.empty() ? "The invite code is not for a relay." : error),
            5.0f, true);
        return;
    }
    const bool useLocalRelay = bool_value(config_.relayLocal);
    config.host = useLocalRelay ? "127.0.0.1" : endpoint->host;
    config.port = static_cast<uint16_t>(endpoint->port);
    config.sessionId = endpoint->sessionId;
    config.sessionKey = endpoint->sessionKey;
    if (!transport_.start_relay(config, &error)) {
        statusMessage_ = "Relay host failed: " + error;
        game::push_online_notification(
            failure_message("Could not create the relay lobby", error), 5.0f, true);
    } else {
        begin_lobby_attempt("Could not create the relay lobby");
        relayHostIntent_ = true;
        activeCode_ = code;
        statusMessage_ = useLocalRelay
            ? "Creating relay lobby using the relay on this PC"
            : "Creating relay lobby using the server in the relay code";
    }
}

void OnlineApp::join_relay() {
    relayHostIntent_ = false;
    const std::string code = string_value(config_.relayCode);
    std::string error;
    const auto endpoint = dusk::multiplayer::decode_invite_code(code, &error);
    if (!endpoint || endpoint->transport != "relay") {
        statusMessage_ = "Relay join failed: " +
                         (error.empty() ? std::string("not a relay code") : error);
        game::push_online_notification(
            failure_message("Could not join the relay lobby",
                            error.empty() ? "The invite code is not for a relay." : error),
            5.0f, true);
        return;
    }
    net::RelayConfig config;
    config.name = string_value(config_.playerName);
    config.room = string_value(config_.relayRoom);
    config.password = string_value(config_.relayPassword);
    const bool useLocalRelay = bool_value(config_.relayLocal);
    config.host = useLocalRelay ? "127.0.0.1" : endpoint->host;
    config.port = static_cast<uint16_t>(endpoint->port);
    config.sessionId = endpoint->sessionId;
    config.sessionKey = endpoint->sessionKey;
    config.createRoom = false;
    config.settings = configured_settings();
    config.wantPuppet = bool_value(config_.dummyModel, true);
    if (!transport_.start_relay(config, &error)) {
        statusMessage_ = "Relay join failed: " + error;
        game::push_online_notification(
            failure_message("Could not join the relay lobby", error), 5.0f, true);
    } else {
        begin_lobby_attempt("Could not join the relay lobby");
        relayHostIntent_ = false;
        activeCode_ = code;
        statusMessage_ = useLocalRelay
            ? "Joining relay lobby using the relay on this PC"
            : "Joining relay lobby using the server in the relay code";
    }
}

void OnlineApp::disconnect() {
    if (requestedDisconnectStatus_.empty()) requestedDisconnectStatus_ = "Disconnected";
    transport_.disconnect();
    if (router_ != nullptr) {
        router_->clear();
    }
    if (game_ != nullptr) {
        game_->reset_session();
    }
    statusMessage_ = requestedDisconnectStatus_;
    activeCode_.clear();
    relayHostIntent_ = false;
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
    svc_ui->pane_add_section(mod_ctx, panel, "Status");
    const std::string status = app.status_text();
    svc_ui->pane_add_rml(mod_ctx, panel, status.c_str(), &app.panelStatus_);
    app.panelRenderedStatus_ = status;
    add_button(panel, "Open Online", &OnlineApp::open_pressed, &app);
    return MOD_OK;
}

ModResult OnlineApp::update_panel(ModContext*, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (app.panelStatus_ != 0) {
        const std::string status = app.status_text();
        if (status != app.panelRenderedStatus_ &&
            svc_ui->elem_set_rml(mod_ctx, app.panelStatus_, status.c_str()) != MOD_OK) {
            // Panel contents are rebuilt on host-tab changes; never poll a
            // generation-checked element after its owning content is gone.
            app.panelStatus_ = 0;
        }
        app.panelRenderedStatus_ = status;
    }
    return MOD_OK;
}

ModResult OnlineApp::build_session_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                       UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.windowStatus_ = 0;
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
    app.sessionActionsHeading_ = 0;
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Status");
    const std::string status = app.dashboard_rml();
    svc_ui->pane_add_rml(mod_ctx, left, status.c_str(), &app.windowStatus_);
    app.windowRenderedStatus_ = status;

    svc_ui->pane_add_section(mod_ctx, left, "Connected players");
    if (!app.manualPeerLabels_.empty()) {
        for (const std::string& name : app.manualPeerLabels_) {
            svc_ui->pane_add_text(mod_ctx, left, name.c_str(), nullptr);
        }
    } else {
        svc_ui->pane_add_text(mod_ctx, left, "No other players are connected.", nullptr);
    }

    svc_ui->pane_add_section(mod_ctx, right, "Session");
    add_button(right, "Settings", &OnlineApp::settings_pressed, &app);
    add_button(right, "Sync players", &OnlineApp::sync_menu_pressed, &app,
               &OnlineApp::sync_menu_unavailable);
    svc_ui->pane_add_rml(mod_ctx, right,
                         "<div class=\"section-heading\">Session actions</div>",
                         &app.sessionActionsHeading_);
    app.sessionActionsVisible_ = app.transport_.status().enabled;
    if (app.sessionActionsHeading_ != 0) {
        svc_ui->elem_set_class(mod_ctx, app.sessionActionsHeading_,
                               "online-session-actions-hidden",
                               !app.sessionActionsVisible_);
    }
    add_button(right, "Stop hosting", &OnlineApp::stop_hosting_pressed, &app,
               &OnlineApp::host_inactive, nullptr, "online-danger-action");
    add_button(right, "Disconnect", &OnlineApp::disconnect_pressed, &app,
               &OnlineApp::joiner_inactive, nullptr, "online-danger-action");
    return MOD_OK;
}

ModResult OnlineApp::build_settings_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                        UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Gameplay");
    add_session_toggle(
        left, "Remote Link model", &OnlineApp::dummy_model_get, &OnlineApp::dummy_model_set,
        &OnlineApp::room_setting_locked, &app,
        "<p>Show connected players as fully animated Link models.</p>");
    add_session_toggle(
        left, "Sync flags", &OnlineApp::sync_flags_get, &OnlineApp::sync_flags_set,
        &OnlineApp::room_setting_locked, &app,
        "<p>Share supported story, item and progression state with the lobby.</p>");
    add_session_toggle(
        left, "Performance Mode (Recommended)", &OnlineApp::performance_mode_get,
        &OnlineApp::performance_mode_set, &OnlineApp::room_setting_locked, &app,
        "<p>Recreate Remote Link locally using compact animation state for substantially lower bandwidth.</p>");
    add_session_toggle(
        left, "Remote collision", &OnlineApp::remote_collision_get,
        &OnlineApp::remote_collision_set, &OnlineApp::remote_collision_setting_locked, &app,
        "<p>Allow connected players' Link models to collide with the local player.</p>");
    add_session_toggle(
        left, "PvP", &OnlineApp::pvp_get, &OnlineApp::pvp_set,
        &OnlineApp::pvp_setting_locked, &app,
        "<p>Allow connected players to damage each other. Remote collision must also be enabled.</p>");
    svc_ui->pane_add_section(mod_ctx, left, "Display");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Name labels", app.config_.nameLabels,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>Show each connected player's name above their Link model.</p>");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Player list overlay", app.config_.playerList,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>Keep the connected-player list visible during gameplay.</p>");
    svc_ui->pane_add_section(mod_ctx, right, "Settings");
    svc_ui->pane_add_text(mod_ctx, right,
                          "Select an option to see its description.", nullptr);
    return MOD_OK;
}

ModResult OnlineApp::build_sync_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                    UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->elem_set_class(mod_ctx, right, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Sync options");
    app.manualSyncFlagsButton_ = add_button(
        left, "Sync flags", &OnlineApp::manual_sync_flags_pressed, &app,
        &OnlineApp::manual_sync_unavailable);
    app.manualSyncWarpButton_ = add_button(
        left, "Sync and warp", &OnlineApp::manual_sync_warp_pressed, &app,
        &OnlineApp::manual_sync_unavailable);
    add_button(left, "Refresh players", &OnlineApp::refresh_sync_peers_pressed, &app);
    app.set_manual_sync_pending_visual(app.manualSyncCooldownTicks_ > 0);

    svc_ui->pane_add_section(mod_ctx, right, "Players");
    if (app.manualPeerButtonContexts_.empty()) {
        svc_ui->pane_add_text(mod_ctx, right, "No other players are connected.", nullptr);
    } else {
        for (size_t i = 0; i < app.manualPeerButtonContexts_.size(); ++i) {
            const UiElementHandle button = add_button(
                right, app.manualPeerLabels_[i].c_str(), &OnlineApp::manual_peer_pressed,
                &app.manualPeerButtonContexts_[i]);
            app.manualPeerButtonElements_.push_back(button);
            if (button != 0 && app.selectedManualPeer_ == static_cast<int64_t>(i)) {
                svc_ui->elem_set_class(mod_ctx, button, "online-peer-selected", true);
            }
        }
    }
    return MOD_OK;
}

ModResult OnlineApp::build_direct_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                      UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    // Activating any tab destroys the previous tab's elements.
    app.windowStatus_ = 0;
    app.sessionActionsHeading_ = 0;
    svc_ui->elem_set_class(mod_ctx, left, "online-form-pane", true);
    svc_ui->elem_set_class(mod_ctx, right, "online-form-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Host direct");
    add_button(left, "Host direct lobby", &OnlineApp::host_direct_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action");
    add_button(left, "Stop hosting", &OnlineApp::stop_hosting_pressed, &app,
               &OnlineApp::direct_host_inactive, nullptr, "online-danger-action");
    add_form_string(left, "Player name", app.config_.playerName, 32);
    add_form_string(left, "Lobby name", app.config_.directRoom, 64, "online-half-field");
    add_form_string(left, "Host address", app.config_.publicHost, 255, "online-half-field");
    add_bound_control(left, UI_CONTROL_NUMBER, "Port", app.config_.port, 1, 65535, 1,
                      0, nullptr, nullptr, "online-form-field", "online-half-field");
    add_code_control(left, "Invite code", &OnlineApp::direct_code_get,
                     &OnlineApp::direct_code_set, &app);
    add_button(left, "Copy", &OnlineApp::copy_direct_code_pressed, &app,
               nullptr, nullptr, "online-copy-action");
    svc_ui->pane_add_section(mod_ctx, right, "Join direct");
    add_button(right, "Join direct lobby", &OnlineApp::join_direct_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action");
    add_button(right, "Disconnect", &OnlineApp::disconnect_pressed, &app,
               &OnlineApp::direct_join_inactive, nullptr, "online-danger-action");
    add_form_string(right, "Player name", app.config_.playerName, 32);
    add_code_control(right, "Invite code", &OnlineApp::direct_code_get,
                     &OnlineApp::direct_code_set, &app);
    add_button(right, "Paste", &OnlineApp::paste_direct_code_pressed, &app,
               nullptr, nullptr, "online-copy-action");
    return MOD_OK;
}

ModResult OnlineApp::build_relay_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                     UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    // Activating any tab destroys the previous tab's elements.
    app.windowStatus_ = 0;
    app.sessionActionsHeading_ = 0;
    svc_ui->elem_set_class(mod_ctx, left, "online-form-pane", true);
    svc_ui->elem_set_class(mod_ctx, right, "online-form-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Host relay");
    add_button(left, "Host relay lobby", &OnlineApp::host_relay_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action");
    add_button(left, "Stop hosting", &OnlineApp::stop_hosting_pressed, &app,
               &OnlineApp::relay_host_inactive, nullptr, "online-danger-action");
    add_form_string(left, "Player name", app.config_.playerName, 32);
    add_code_control(left, "Relay code", &OnlineApp::relay_code_get,
                     &OnlineApp::relay_code_set, &app);
    add_button(left, "Copy", &OnlineApp::copy_relay_code_pressed, &app,
               nullptr, nullptr, "online-copy-action");
    add_form_string(left, "Lobby name", app.config_.relayRoom, 64, "online-half-field");
    add_form_string(left, "Password", app.config_.relayPassword, 128, "online-half-field");
    add_bound_control(left, UI_CONTROL_TOGGLE,
                      "Use relay on this PC", app.config_.relayLocal,
                      0, 0, 1, 0, nullptr, nullptr, "online-wide-control");
    svc_ui->pane_add_section(mod_ctx, right, "Join relay");
    add_button(right, "Join relay lobby", &OnlineApp::join_relay_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action");
    add_button(right, "Disconnect", &OnlineApp::disconnect_pressed, &app,
               &OnlineApp::relay_join_inactive, nullptr, "online-danger-action");
    add_form_string(right, "Player name", app.config_.playerName, 32);
    add_code_control(right, "Relay code", &OnlineApp::relay_code_get,
                     &OnlineApp::relay_code_set, &app);
    add_button(right, "Paste", &OnlineApp::paste_relay_code_pressed, &app,
               nullptr, nullptr, "online-copy-action");
    add_form_string(right, "Lobby name", app.config_.relayRoom, 64, "online-half-field");
    add_form_string(right, "Password", app.config_.relayPassword, 128, "online-half-field");
    add_bound_control(right, UI_CONTROL_TOGGLE,
                      "Use relay on this PC", app.config_.relayLocal,
                      0, 0, 1, 0, nullptr, nullptr, "online-wide-control");
    return MOD_OK;
}

ModResult OnlineApp::update_window(ModContext*, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (app.windowStatus_ != 0) {
        const std::string status = app.dashboard_rml();
        if (status != app.windowRenderedStatus_ &&
            svc_ui->elem_set_rml(mod_ctx, app.windowStatus_, status.c_str()) != MOD_OK) {
            // A tab rebuild invalidates every element handle from its prior
            // generation. Stop immediately if the host rebuilt underneath us.
            app.windowStatus_ = 0;
        }
        app.windowRenderedStatus_ = status;
    }
    if (app.sessionActionsHeading_ != 0) {
        const bool visible = app.transport_.status().enabled;
        if (visible != app.sessionActionsVisible_) {
            if (svc_ui->elem_set_class(mod_ctx, app.sessionActionsHeading_,
                                       "online-session-actions-hidden", !visible) != MOD_OK) {
                app.sessionActionsHeading_ = 0;
            } else {
                app.sessionActionsVisible_ = visible;
            }
        }
    }
    return MOD_OK;
}

void OnlineApp::window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.window_ = 0;
    app.windowStatus_ = 0;
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
    app.sessionActionsHeading_ = 0;
}

void OnlineApp::settings_window_closed(ModContext*, UiWindowHandle, void* data) {
    static_cast<OnlineApp*>(data)->settingsWindow_ = 0;
}

void OnlineApp::sync_window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.syncWindow_ = 0;
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
}

void OnlineApp::open_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->open_window(); }
void OnlineApp::settings_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_settings_window();
}
void OnlineApp::sync_menu_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_sync_window();
}
void OnlineApp::menu_selected(ModContext*, void* data) { static_cast<OnlineApp*>(data)->open_window(); }
void OnlineApp::disconnect_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->disconnect(); }
void OnlineApp::stop_hosting_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.requestedDisconnectStatus_ = "Not connected";
    app.disconnect();
}
void OnlineApp::host_direct_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->host_direct(); }
void OnlineApp::join_direct_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->join_direct(); }
void OnlineApp::host_relay_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->host_relay(); }
void OnlineApp::join_relay_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->join_relay(); }
void OnlineApp::direct_code_get(ModContext*, void* data, UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.directCodeDisplay_ = app.string_value(app.config_.directInvite);
    value->string_value = app.directCodeDisplay_.c_str();
}
void OnlineApp::direct_code_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string entered = value->string_value != nullptr ? value->string_value : "";
    if (entered != app.directCodeDisplay_) {
        svc_config->set_string(mod_ctx, app.config_.directInvite,
                               trim_clipboard_text(entered).c_str());
    }
}
void OnlineApp::relay_code_get(ModContext*, void* data, UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.relayCodeDisplay_ = app.string_value(app.config_.relayCode);
    value->string_value = app.relayCodeDisplay_.c_str();
}
void OnlineApp::relay_code_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string entered = value->string_value != nullptr ? value->string_value : "";
    if (entered != app.relayCodeDisplay_) {
        svc_config->set_string(mod_ctx, app.config_.relayCode,
                               trim_clipboard_text(entered).c_str());
    }
}
void OnlineApp::copy_direct_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = !app.activeCode_.empty() ? app.activeCode_ :
        app.string_value(app.config_.directInvite);
    const bool copied = write_clipboard(code);
    push_clipboard_toast(copied ? "Direct invite code copied" :
        "Could not copy the direct invite code", !copied);
}
void OnlineApp::paste_direct_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = read_clipboard();
    if (code.empty()) {
        push_clipboard_toast("Clipboard does not contain a code", true);
        return;
    }
    svc_config->set_string(mod_ctx, app.config_.directInvite, code.c_str());
    push_clipboard_toast("Direct invite code pasted");
}
void OnlineApp::copy_relay_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = !app.activeCode_.empty() ? app.activeCode_ :
        app.string_value(app.config_.relayCode);
    const bool copied = write_clipboard(code);
    push_clipboard_toast(copied ? "Relay code copied" :
        "Could not copy the relay code", !copied);
}
void OnlineApp::paste_relay_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = read_clipboard();
    if (code.empty()) {
        push_clipboard_toast("Clipboard does not contain a code", true);
        return;
    }
    svc_config->set_string(mod_ctx, app.config_.relayCode, code.c_str());
    push_clipboard_toast("Relay code pasted");
}
void OnlineApp::manual_peer_pressed(ModContext*, void* data) {
    const auto& context = *static_cast<ManualPeerButtonContext*>(data);
    if (context.app != nullptr && context.index >= 0 &&
        context.index < static_cast<int64_t>(context.app->manualPeerIds_.size())) {
        context.app->selectedManualPeer_ = context.index;
        for (size_t i = 0; i < context.app->manualPeerButtonElements_.size(); ++i) {
            const UiElementHandle button = context.app->manualPeerButtonElements_[i];
            if (button != 0) {
                svc_ui->elem_set_class(mod_ctx, button, "online-peer-selected",
                                       static_cast<int64_t>(i) == context.index);
            }
        }
    }
}
bool OnlineApp::manual_peer_selected(ModContext*, void* data) {
    const auto& context = *static_cast<ManualPeerButtonContext*>(data);
    return context.app != nullptr && context.app->selectedManualPeer_ == context.index;
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
void OnlineApp::refresh_sync_peers_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.reopenSyncWindowPending_ = true;
    if (app.syncWindow_ != 0) svc_ui->window_close(mod_ctx, app.syncWindow_);
}
bool OnlineApp::manual_sync_unavailable(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    return !app.transport_.status().welcomed || app.manualPeerIds_.empty() ||
           app.manualSyncCooldownTicks_ > 0 ||
           (app.game_ != nullptr && app.game_->manual_sync_waiting());
}
bool OnlineApp::sync_menu_unavailable(ModContext*, void* data) {
    return !static_cast<OnlineApp*>(data)->transport_.status().welcomed;
}
bool OnlineApp::session_active(ModContext*, void* data) {
    return static_cast<OnlineApp*>(data)->transport_.status().enabled;
}
bool OnlineApp::host_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled ||
        (status.mode != net::Mode::DirectHost &&
         !(status.mode == net::Mode::Relay && (app.relayHostIntent_ || status.isOwner)));
}
bool OnlineApp::joiner_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled || status.mode == net::Mode::DirectHost ||
        (status.mode == net::Mode::Relay && (app.relayHostIntent_ || status.isOwner));
}
bool OnlineApp::direct_host_inactive(ModContext*, void* data) {
    const net::Status status = static_cast<OnlineApp*>(data)->transport_.status();
    return !status.enabled || status.mode != net::Mode::DirectHost;
}
bool OnlineApp::direct_join_inactive(ModContext*, void* data) {
    const net::Status status = static_cast<OnlineApp*>(data)->transport_.status();
    return !status.enabled || status.mode != net::Mode::DirectJoin;
}
bool OnlineApp::relay_host_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled || status.mode != net::Mode::Relay ||
        (!app.relayHostIntent_ && !status.isOwner);
}
bool OnlineApp::relay_join_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled || status.mode != net::Mode::Relay ||
        app.relayHostIntent_ || status.isOwner;
}
bool OnlineApp::room_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    return room_settings_locked(app.transport_.status(), app.relayHostIntent_);
}
bool OnlineApp::remote_collision_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    const bool roomLocked = room_settings_locked(status, app.relayHostIntent_);
    const bool modelEnabled = status.enabled ? status.settings.dummyModel :
                                               app.bool_value(app.config_.dummyModel, true);
    return roomLocked || !modelEnabled;
}
bool OnlineApp::pvp_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    const bool roomLocked = room_settings_locked(status, app.relayHostIntent_);
    // The raw room collision option controls this UI lock. The remote-model
    // option participates only in effective runtime PvP.
    const bool collisionEnabled = status.enabled ? status.settings.remoteCollision :
        app.bool_value(app.config_.remoteCollision, true);
    return roomLocked || !collisionEnabled;
}

void OnlineApp::dummy_model_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().dummyModel;
}

void OnlineApp::dummy_model_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.dummyModel, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.dummyModel = value->bool_value;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::sync_flags_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().syncFlags;
}

void OnlineApp::sync_flags_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.syncFlags, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.syncFlags = value->bool_value;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::performance_mode_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value =
        static_cast<OnlineApp*>(data)->displayed_settings().performanceMode;
}

void OnlineApp::performance_mode_set(ModContext*, void* data,
                                     const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.semanticRenderingExperiment,
                         value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.performanceMode = value->bool_value;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::remote_collision_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().remoteCollision;
}

void OnlineApp::remote_collision_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.remoteCollision, value->bool_value);
    if (!value->bool_value) svc_config->set_bool(mod_ctx, app.config_.pvp, false);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.remoteCollision = value->bool_value;
        if (!settings.remoteCollision) settings.pvp = false;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::pvp_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = net::effective_pvp(
        static_cast<OnlineApp*>(data)->displayed_settings());
}

void OnlineApp::pvp_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.pvp, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (status.mode == net::Mode::Relay && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.pvp = value->bool_value && settings.remoteCollision;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

}  // namespace dusklight_online
