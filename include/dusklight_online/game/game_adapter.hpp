#pragma once

#include "dusklight_online/game/protocol_router.hpp"
#include "dusk/multiplayer/multiplayer.hpp"

#include <mods/api.h>
#include <mods/svc/item.h>
#include <mods/svc/save.h>

#include <cstdint>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dusklight_online::game {

// Version-neutral game integration: every engine call is made through public
// game headers/symbols from the active Dusklight GameService epoch. No fixed
// executable addresses or per-build offsets are stored in the mod.
class GameAdapter final : public MessageConsumer {
public:
    explicit GameAdapter(net::Transport& transport);

    ModResult initialize_hooks(ModError* error);
    void shutdown_hooks();
    void update(bool syncFlagsEnabled, bool syncWorldEnabled, bool remoteModelEnabled,
                bool nameLabelsEnabled, bool displayMidnaEnabled,
                bool semanticRenderingExperimentEnabled,
                bool remoteCollisionEnabled,
                bool pvpEnabled, bool playerListEnabled);
    // Capture mutations made by the completed game tick before inbound
    // progression can rebaseline the observer around a remote application.
    void capture_local_mutations_before_remote(bool syncFlagsEnabled);
    // Called from the fapGm_Execute pre-hook, after Dusklight has sampled the
    // controller but before vanilla menu/gameplay code consumes D-Pad Down.
    void consume_progression_prompt_input();
    void publish_local(nlohmann::json message);
    bool request_manual_sync(std::string_view peerId, bool flagsOnly, std::string* error = nullptr);
    [[nodiscard]] bool applying_remote() const;
    [[nodiscard]] bool randomizer_active() const;

    [[nodiscard]] bool stage_ready() const override;
    [[nodiscard]] bool allow_stage_unready(const RoutedMessage& message) const override;
    [[nodiscard]] bool discard_stage_message(const RoutedMessage& message) const override;
    ApplyResult consume(const RoutedMessage& message) override;
    ApplyResult consume_udp(const net::Event& event) override;
    void peer_joined(std::string_view peerId, std::string_view name) override;
    void peer_left(std::string_view peerId) override;

    void report_pvp_target_hit(fopAc_ac_c* remoteLinkActor, fopAc_ac_c* attackActor,
                               dCcD_GObjInf* attackInfo);
    void notify_local_save_reset();
    void notify_local_save_loaded();
    void notify_local_save_written();
    void notify_room_scene_initialized(int room);
    void notify_local_event_bit(uint16_t flag);
    void observe_local_memory_item(int stage, int flag);
    void notify_local_fish_caught(int fishIndex);
    void notify_local_light_drop_num(int area, int previous, int value);
    void notify_local_dark_clear(int level);
    void notify_local_max_life(int previous, int value);
    [[nodiscard]] bool should_suppress_local_bottle_source(uint8_t sourceItem) const;
    void notify_local_bottle_slots(int previous, int value, uint8_t sourceItem);
    void notify_local_rupees(int previous, int value);
    void notify_local_item_grant(const ItemGiveInfo& info);

    void reset_session();
    [[nodiscard]] const std::string& last_error() const;
    [[nodiscard]] std::string manual_sync_status_text() const;

private:
    net::Transport& transport_;
    std::map<std::string, std::string> peerNames_;
    std::map<std::string, uint8_t> peerColorSlots_;
    std::map<std::string, nlohmann::json> peerPresence_;
    std::map<std::string, nlohmann::json> peerProgressionStates_;
    std::map<std::string, uint32_t> peerProgressionAges_;
    std::deque<RoutedMessage> deferredFaronInbound_;
    std::map<std::string, dusk::multiplayer::PeerPoseSnapshot> peerPoses_;
    std::map<std::string, uint32_t> latestAckSequence_;
    std::map<std::string, uint32_t> pvpRemoteHitLastSequence_;
    // Link exposes several sword attack objects during one swing. Remember
    // contacts already reported during this game update so those colliders
    // produce one network hit without imposing a cross-frame cooldown.
    std::set<std::pair<std::string, uintptr_t>> pvpLocalHitContactsThisUpdate_;
    std::deque<nlohmann::json> deferredSwitches_;
    std::map<std::string, uint32_t> permanentPickupSequence_;
    std::map<std::string, uint32_t> fishCatchSequence_;
    std::map<int, std::set<int>> observedMemoryItems_;
    // A randomized location may legitimately contain the same item as many
    // other locations. Deduplicate by ItemService check identity, never by the
    // resolved item byte.
    std::set<std::string> completedRandomizerChecks_;
    // Vanilla has four fixed bottle rewards. Remember their identities so the
    // same world reward observed by two peers is applied once, while distinct
    // rewards acquired concurrently still add separate slots.
    std::set<uint8_t> completedBottleSources_;
    bool bottleSourcesComplete_ = false;
    std::map<std::string, bool> appliedTearEvents_;
    std::string lastError_;
    uint32_t progressionTicks_ = 0;
    uint32_t presenceTicks_ = 0;
    uint32_t collectibleRepairTicks_ = 0;
    uint32_t localPoseSequence_ = 0;
    uint32_t localPvpHitSequence_ = 0;
    bool applyingRemote_ = false;
    bool syncFlagsEnabled_ = true;
    bool syncWorldEnabled_ = false;
    bool hooksInstalled_ = false;
    SaveObserverHandle saveObserver_ = 0;
    ItemGiveHandle itemGiveObserver_ = 0;
    uint8_t localColorSlot_ = 0;
    bool sharedOoccooAuthoritative_ = false;
    bool sharedOoccooBoundToSave_ = false;
    nlohmann::json sharedOoccooState_ = {{"exists", false}};
    nlohmann::json localObservedState_;
    std::string stableStageName_;
    int stableRoom_ = -128;
    uint32_t stableRoomTicks_ = 0;
    std::string initializedStageName_;
    int initializedRoom_ = -128;
    uint32_t initializedRoomTicks_ = 0;
    std::array<uint8_t, 3> pendingDarkClears_{};
    uint32_t localPermanentSequence_ = 0;
    uint32_t localFishSequence_ = 0;
    std::optional<uint16_t> pendingRupeePublicationToSuppress_;
    std::optional<uint8_t> pendingMaxLifePublicationToSuppress_;
    std::vector<uint8_t> pendingManualInfo_;
    std::vector<uint8_t> pendingManualFlagsSave_;
    std::optional<uint8_t> pendingManualVibration_;
    bool manualTransitionActive_ = false;
    bool manualReloadPending_ = false;
    std::set<uint16_t> pendingOrdonEventBits_;
    uint32_t ordonReloadSafeTicks_ = 0;
    uint32_t ordonReloadWaitTicks_ = 0;
    bool ordonReloadTransitionActive_ = false;
    bool ordonReloadSawStageLoad_ = false;
    bool mirrorReloadPending_ = false;
    nlohmann::json zoraThawPending_;
    std::deque<nlohmann::json> deferredStoryEvents_;
    std::deque<nlohmann::json> deferredLocalEvents_;
    uint32_t faronDayBroadcastHoldTicks_ = 0;
    bool localFaronCageSequenceActive_ = false;
    bool localFaronWarpSequenceActive_ = false;
    int lastLocalTboxStage_ = -1;
    int lastLocalTboxFlag_ = -1;
    std::chrono::steady_clock::time_point lastLocalTboxAt_{};

    struct ProgressionPrompt {
        bool active = false;
        std::string peerId;
        std::string peerName;
        std::string cueKey;
        std::string title;
        std::string body;
        uint32_t ageTicks = 0;
        uint32_t holdTicks = 0;
        bool waiting = false;
    } progressionPrompt_;
    bool progressionPromptAcceptHeld_ = false;
    struct PendingProgressionCue {
        std::string peerId;
        std::string cueKey;
        std::string title;
        std::string body;
        std::string expectedStage;
    };
    std::vector<PendingProgressionCue> pendingProgressionCues_;
    std::string pendingProgressionPeerId_;
    std::string pendingProgressionCueKey_;
    std::string awaitingManualSyncCueKey_;
    std::string awaitingManualSyncPeerId_;
    std::set<std::string> handledProgressionCues_;
    struct PendingSyncReply {
        std::string peerId;
        std::string cueKey;
        bool flagsOnly = false;
        uint32_t waitTicks = 0;
    };
    std::vector<PendingSyncReply> pendingSyncReplies_;
    enum class ManualSyncState : uint8_t { None, Waiting, Succeeded, Failed };
    ManualSyncState manualSyncState_ = ManualSyncState::None;
    bool manualSyncFlagsOnly_ = false;
    std::string manualSyncPeerId_;
    uint32_t manualSyncWaitTicks_ = 0;

    ApplyResult consume_progression(const RoutedMessage& message);
    ApplyResult consume_randomizer(const RoutedMessage& message);
    ApplyResult consume_pvp_hit(const RoutedMessage& message);
    nlohmann::json make_save_snapshot();
    ApplyResult apply_save_snapshot(const RoutedMessage& message);
    ApplyResult apply_switch_bit(const nlohmann::json& message,
                                 std::string_view peerId = {});
    ApplyResult apply_snapshot_switch_bit(int stage, int flag);
    void flush_deferred_switches();
    ApplyResult apply_dark_clear(int level);
    void flush_pending_dark_clears();
    bool accept_ooccoo_state(const nlohmann::json& state);
    void apply_shared_ooccoo_local_form();
    nlohmann::json observe_local_ooccoo_state();
    void send_snapshot_to(std::string_view peerId = {}, bool manual = false,
                          bool flagsOnly = false);
    std::string encode_manual_full_state();
    bool apply_manual_full_state(const std::string& encoded, bool flagsOnly,
                                 std::string_view peerId);
    void tick_manual_transition();
    void update_pending_sync_replies();
    [[nodiscard]] bool engine_stage_ready() const;
    ApplyResult apply_event_bit(const RoutedMessage& message);
    void flush_story_events();
    void send_progression_state(bool force = false);
    void set_local_faron_warp_sequence_active(bool active);
    void update_local_faron_cage_sequence_state();
    bool has_active_faron_cage_sequence_peer() const;
    bool should_defer_faron_warp_sequence() const;
    void poll_local_state(bool publish);
    void clear_disabled_sync_flags_state();
    void clear_replaced_save_progression_state();
    void load_bottle_source_state();
    void persist_bottle_source_state() const;
    void replace_bottle_source_state(const nlohmann::json& message);
    void remember_memory_item(int stage, int flag);
    void reapply_observed_memory_items_for_current_stage();
    ApplyResult reject(std::string reason);
    void assign_peer_color(std::string_view peerId);
    void consume_welcome_membership(const nlohmann::json& message);
    bool request_manual_sync_impl(std::string_view peerId, bool flagsOnly,
                                  std::string_view cueKey, std::string* error,
                                  bool trackStatus = true);
    void update_progression_prompts();
    void maybe_queue_progression_event_prompt(std::string_view peerId, uint16_t flag);
    void maybe_queue_progression_switch_prompt(std::string_view peerId, int stage, int flag);
};

}  // namespace dusklight_online::game
