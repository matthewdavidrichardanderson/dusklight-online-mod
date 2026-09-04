#pragma once

#include "dusklight_online/game/actor_sync.hpp"
#include "dusklight_online/game/protocol_router.hpp"
#include <algorithm>
#include <functional>
#include <set>

namespace dusklight_online::game::actor_sync {

inline std::string local_peer_id(const net::Status& status) {
    return status.mode == net::Mode::DirectHost ? "direct" : status.clientId;
}
inline bool is_coordinator(const net::Status& status) {
    return status.mode == net::Mode::DirectHost ||
           (status.mode == net::Mode::Relay && status.isOwner);
}
inline std::string coordinator_peer_id(const net::Status& status) {
    return status.mode == net::Mode::DirectJoin ? "direct" :
           status.mode == net::Mode::Relay ? status.ownerClientId : "direct";
}

// One session per explicitly registered world actor instance. No engine actor
// pointers, animations, or Link-specific logic belong here. Payload parsing and
// transfer locks are supplied by the adapter and run before authority changes.
// Unexpected owner loss fails closed; recovery is not guessed for new actors.
template<class Snapshot>
struct Session : Instance<Snapshot, nlohmann::json> {
    struct Timing {
        uint32_t claimInterval = 30;
        uint32_t ownerBroadcastInterval = 60;
        uint32_t stateSendInterval = 2;
        uint32_t maxStateAge = 45;
        uint32_t resetEmptyTicks = 60;
    } timing;
    std::string id, stage;
    net::Status status;
    bool featureReady = false;
    bool localReady = false;
    std::string localPeerId;
    std::set<std::string> readyPeers;
    std::string targetPeerId;
    bool targetRemote = false;
    uint32_t claimTicks = 0, ownerBroadcastTicks = 0, stateSendTicks = 0;
    uint32_t ownerMissingTicks = 0, encounterEmptyTicks = 0;

    std::function<bool(const nlohmann::json&)> send;
    std::function<bool(const nlohmann::json&, Snapshot&)> parse;
    std::function<bool(const Snapshot&)> transferLocked;
    std::function<nlohmann::json()> capture;
    std::function<bool()> canSimulate, canRelease;
    std::function<void()> onGranted, onSent;
    std::function<void(std::string_view, const Snapshot&)> onSnapshot;
    std::function<void(const std::string&)> log = [](const std::string&) {};

    void broadcast_owner(std::string_view reason) {
        if (!send || !is_coordinator(this->status) ||
            this->authority.epoch == 0) return;
        (void)send({
            {"type", "actor_owner"},
            {"sync_id", id},
            {"stage", stage},
            {"encounter_epoch", this->authority.epoch},
            {"owner_peer_id", this->authority.ownerPeerId},
            {"active", !this->authority.ownerPeerId.empty()},
            {"reason", reason},
            {"transfer_state", this->ownershipState},
        });
    }

    void choose_owner(std::string ownerPeerId, std::string_view reason) {
        if (!is_coordinator(this->status) || ownerPeerId.empty() ||
            this->authority.ownerLossLocked) return;
        if (this->authority.ownerPeerId.empty()) {
            const uint32_t nextEpoch = this->authority.next_grant_epoch();
            if (nextEpoch == 0 || !this->authority.accept_grant(
                    nextEpoch, ownerPeerId, this->localPeerId, false)) return;
            this->snapshot = {};
            this->ownershipState = nullptr;
            this->lastAppliedCollisionSequence = 0;
            this->lastAppliedStateSequence = 0;
            this->ownerMissingTicks = 0;
            log("Actor sync assigned encounter owner " +
                                       this->authority.ownerPeerId + " (epoch " +
                                       std::to_string(this->authority.epoch) + ")");
        }
        broadcast_owner(reason);
    }

    void abort_owner(std::string_view reason) {
        if (this->authority.ownerPeerId.empty()) return;
        const std::string previous = this->authority.ownerPeerId;
        this->authority.ownerLossLocked = this->authority.epoch != 0;
        this->encounterEmptyTicks = 0;
        this->authority.ownerPeerId.clear();
        this->authority.handoffPending = false;
        this->authority.takeoverPending = false;
        this->ownershipState = nullptr;
        this->snapshot = {};
        this->lastAppliedCollisionSequence = 0;
        this->lastAppliedStateSequence = 0;
        this->ownerMissingTicks = 0;
        if (is_coordinator(this->status)) broadcast_owner(reason);
        this->authority.epoch = 0;
        log("Actor sync released encounter owner " + previous +
                                   " (" + std::string(reason) + ")");
    }

    bool grant_handoff(const std::string& target, const nlohmann::json& state) {
        if (!is_coordinator(this->status) || target.empty() ||
            this->authority.ownerLossLocked) return false;
        if (target == this->localPeerId ? !this->localReady :
            !this->readyPeers.contains(target)) return false;
        Snapshot snapshot;
        if (!parse({{"encounter_epoch", this->authority.epoch}, {"sequence", 1},
                             {"state", state}}, snapshot)) return false;
        if (transferLocked(snapshot)) return false;
        const uint32_t nextEpoch = this->authority.next_grant_epoch();
        if (nextEpoch == 0 || !this->authority.accept_grant(
                nextEpoch, target, this->localPeerId, true)) return false;
        snapshot.epoch = this->authority.epoch;
        this->authority.ownerPeerId = target;
        this->ownershipState = state;
        this->snapshot = snapshot;
        this->authority.handoffPending = false;
        this->lastAppliedStateSequence = 0;
        this->lastAppliedCollisionSequence = 0;
        this->authority.localSequence = 1;
        this->ownerMissingTicks = 0;
        onGranted();
        broadcast_owner("nearest_participant");
        log("Actor ownership transferred to " + target +
                                  " at epoch " + std::to_string(this->authority.epoch));
        return true;
    }

    void request_handoff() {
        if (!canRelease() || !this->targetRemote) return;
        const auto state = capture();
        if (is_coordinator(this->status)) {
            (void)grant_handoff(this->targetPeerId, state);
            return;
        }
        // Reliable release-before-grant: the old owner stops before asking the
        // coordinator to authorize the new one. There is never a speculative
        // second owner, even when the round trip exceeds 100 ms.
        if (!this->authority.begin_release(this->localPeerId, false)) return;
        (void)send({
            {"type", "actor_owner_claim"}, {"sync_id", id},
            {"stage", stage}, {"owner_peer_id", this->targetPeerId},
            {"handoff", true}, {"encounter_epoch", this->authority.epoch}, {"state", state},
        });
        log("Actor relinquishing ownership to " + this->targetPeerId);
    }


    void tick() {
        if (this->authority.ownerLossLocked) {
            if (!this->localReady && this->readyPeers.empty()) {
                if (++this->encounterEmptyTicks >= timing.resetEmptyTicks) {
                    this->authority.ownerLossLocked = false;
                    this->encounterEmptyTicks = 0;
                    log(
                        "Actor sync is ready for a new encounter epoch");
                }
            } else {
                this->encounterEmptyTicks = 0;
            }
        }

        if (!this->featureReady) {
            if (!this->authority.ownerPeerId.empty()) abort_owner("world_sync_disabled");
            return;
        }
        if (this->snapshot.valid && this->snapshot.ageTicks < std::numeric_limits<uint32_t>::max()) {
            ++this->snapshot.ageTicks;
        }

        if (is_coordinator(this->status)) {
            if (!this->authority.ownerLossLocked && this->authority.ownerPeerId.empty() &&
                this->localReady) {
                choose_owner(this->localPeerId, "coordinator_local_ready");
            }
            if (!this->authority.ownerPeerId.empty()) {
                const bool ownerReady = this->authority.ownerPeerId == this->localPeerId ? this->localReady :
                    this->readyPeers.contains(this->authority.ownerPeerId);
                this->ownerMissingTicks = ownerReady ? 0 : this->ownerMissingTicks + 1;
                if (this->ownerMissingTicks > timing.maxStateAge) {
                    abort_owner("owner_left_encounter");
                } else if (++this->ownerBroadcastTicks >= timing.ownerBroadcastInterval) {
                    this->ownerBroadcastTicks = 0;
                    broadcast_owner("periodic_confirmation");
                }
            }
        } else if (!this->authority.ownerLossLocked && this->authority.ownerPeerId.empty() &&
                   this->localReady &&
                   ++this->claimTicks >= timing.claimInterval) {
            this->claimTicks = 0;
            (void)send({
                {"type", "actor_owner_claim"}, {"sync_id", id},
                {"stage", stage}, {"owner_peer_id", this->localPeerId},
            });
        }

        if (canSimulate() &&
            ++this->stateSendTicks >= timing.stateSendInterval) {
            this->stateSendTicks = 0;
            if (++this->authority.localSequence == 0) ++this->authority.localSequence;
            (void)send({
                {"type", "actor_state"}, {"sequence", this->authority.localSequence},
                {"sync_id", id}, {"encounter_epoch", this->authority.epoch},
                {"state", capture()},
            });
            onSent();
        }
    }

    ApplyResult consume(const RoutedMessage& message) {
        try {
        if (!this->featureReady || !send) {
            return ApplyResult::IgnoredByPolicy;
        }
        const std::string type = message.payload.value("type", std::string());
        const std::string syncId = message.payload.value("sync_id", std::string());

        if (type == "actor_owner_claim") {
            if (message.payload.value("handoff", false)) {
                if (syncId != id ||
                    !actor_sync::valid_release(is_coordinator(this->status), message.peerId,
                        this->authority.ownerPeerId, this->authority.epoch,
                        message.payload.value("encounter_epoch", 0U)) ||
                    !message.payload.contains("state")) return ApplyResult::Rejected;
                if (grant_handoff(message.payload.value("owner_peer_id", std::string()),
                                  message.payload["state"])) return ApplyResult::Applied;
                // The proposed participant may have left during the round trip.
                // Regrant the released state with a fresh epoch; never tell the
                // previous owner to resume under an ambiguous old epoch.
                return grant_handoff(message.peerId, message.payload["state"]) ?
                       ApplyResult::Applied : ApplyResult::Rejected;
            }
            if (!is_coordinator(this->status) || syncId != id ||
                message.peerId.empty() ||
                message.payload.value("owner_peer_id", std::string()) != message.peerId) {
                return ApplyResult::Rejected;
            }
            // A claim cannot create ownership until a fresh semantic pose proves
            // that the claimant is really inside the final-duel domain.
            if (this->authority.ownerLossLocked || !this->readyPeers.contains(message.peerId)) {
                return ApplyResult::Rejected;
            }
            choose_owner(message.peerId, "peer_claim");
            return ApplyResult::Applied;
        }

        if (type == "actor_owner") {
            const std::string coordinator = coordinator_peer_id(this->status);
            if (syncId != id || message.peerId != coordinator ||
                message.payload.value("stage", std::string()) != stage) {
                return ApplyResult::Rejected;
            }
            const uint32_t epoch = message.payload.value("encounter_epoch", 0U);
            const std::string owner = message.payload.value("owner_peer_id", std::string());
            const bool active = message.payload.value("active", true);
            if (epoch == 0 || epoch < this->authority.epoch || (active && owner.empty()) ||
                (!active && !owner.empty())) return ApplyResult::Rejected;
            if (!active) {
                this->authority.ownerPeerId.clear();
                this->authority.handoffPending = false;
                this->authority.takeoverPending = false;
                this->ownershipState = nullptr;
                this->snapshot = {};
                this->lastAppliedCollisionSequence = 0;
                this->lastAppliedStateSequence = 0;
                this->authority.ownerLossLocked = true;
                this->encounterEmptyTicks = 0;
                this->authority.epoch = 0;
                this->authority.nextEpoch = std::max(this->authority.nextEpoch, epoch);
                return ApplyResult::Applied;
            }
            if (this->authority.ownerLossLocked || !actor_sync::valid_grant(epoch, this->authority.nextEpoch,
                this->authority.epoch, owner, this->authority.ownerPeerId)) return ApplyResult::Rejected;
            const bool changed = epoch != this->authority.epoch || owner != this->authority.ownerPeerId;
            Snapshot transferred;
            if (changed && message.payload.contains("transfer_state") &&
                !message.payload["transfer_state"].is_null()) {
                if (!parse({{"encounter_epoch", epoch}, {"sequence", 1},
                                     {"state", message.payload["transfer_state"]}},
                                    transferred)) return ApplyResult::Rejected;
            }
            if (!this->authority.accept_grant(epoch, owner, this->localPeerId,
                                             transferred.valid)) return ApplyResult::Rejected;
            if (changed) {
                this->snapshot = transferred;
                this->authority.handoffPending = false;
                this->lastAppliedCollisionSequence = 0;
                this->lastAppliedStateSequence = 0;
                this->authority.localSequence = 1;
            }
            this->authority.epoch = epoch;
            this->authority.nextEpoch = std::max(this->authority.nextEpoch, epoch);
            this->authority.ownerPeerId = owner;
            this->ownerMissingTicks = 0;
            if (changed) {
                this->authority.takeoverPending = transferred.valid && owner == this->localPeerId;
                onGranted();
            }
            return ApplyResult::Applied;
        }

        if (type == "actor_state") {
            if (this->authority.ownerLossLocked || message.peerId.empty() ||
                message.peerId != this->authority.ownerPeerId ||
                !message.payload.contains("state") || !message.payload["state"].is_object()) {
                return ApplyResult::Rejected;
            }
            if (syncId != id ||
                message.payload.value("encounter_epoch", 0U) != this->authority.epoch) {
                return ApplyResult::Rejected;
            }
            Snapshot snapshot;
            if (!parse(message.payload, snapshot) ||
                !actor_sync::valid_snapshot(message.peerId, this->authority.ownerPeerId,
                    snapshot.epoch, this->authority.epoch, snapshot.sequence,
                    this->snapshot.valid ? this->snapshot.sequence : 0)) {
                return ApplyResult::Rejected;
            }
            this->snapshot = snapshot;
            onSnapshot(message.peerId, snapshot);
            return ApplyResult::Applied;
        }

        return ApplyResult::IgnoredByPolicy;
        } catch (const nlohmann::json::exception&) {
            // Network input is untrusted. A field with the wrong JSON type must
            // reject the packet, never escape into the game thread.
            return ApplyResult::Rejected;
        }
    }

};
} // namespace dusklight_online::game::actor_sync
