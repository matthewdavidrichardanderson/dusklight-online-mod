#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace dusklight_online::game::actor_sync {

// Engine-independent authority rules. Actor adapters decide eligibility and
// interaction locks, and serialize named state; this layer never copies actors.
constexpr bool valid_release(bool coordinator, std::string_view sender,
                             std::string_view owner, uint32_t epoch,
                             uint32_t releaseEpoch) {
    return coordinator && !owner.empty() && sender == owner && epoch != 0 &&
           epoch == releaseEpoch;
}

constexpr bool valid_grant(uint32_t epoch, uint32_t newestEpoch,
                           uint32_t currentEpoch, std::string_view owner,
                           std::string_view currentOwner) {
    return epoch != 0 && epoch >= newestEpoch && epoch >= currentEpoch &&
           !owner.empty() && !(epoch == currentEpoch && !currentOwner.empty() &&
                              owner != currentOwner);
}

constexpr bool valid_snapshot(std::string_view sender, std::string_view owner,
                              uint32_t epoch, uint32_t currentEpoch,
                              uint32_t sequence, uint32_t lastSequence) {
    return !owner.empty() && sender == owner && epoch != 0 &&
           epoch == currentEpoch && sequence != 0 && sequence > lastSequence;
}

struct Authority {
    std::string ownerPeerId;
    uint32_t epoch = 0;
    uint32_t nextEpoch = 0;
    uint32_t localSequence = 0;
    bool ownerLossLocked = false;
    bool handoffPending = false;
    bool takeoverPending = false;

    bool simulates(std::string_view localPeer) const {
        return !ownerPeerId.empty() && ownerPeerId == localPeer &&
               !ownerLossLocked && !handoffPending && !takeoverPending;
    }

    bool begin_release(std::string_view localPeer, bool interactionLocked) {
        if (interactionLocked || !simulates(localPeer)) return false;
        handoffPending = true;
        return true;
    }

    // Call only after authenticating the coordinator and validating the entire
    // actor payload. A duplicate grant must not replay an old takeover state.
    bool accept_grant(uint32_t grantedEpoch, std::string_view owner,
                      std::string_view localPeer, bool hasTransferState) {
        if (ownerLossLocked || !valid_grant(grantedEpoch, nextEpoch, epoch,
                                           owner, ownerPeerId)) return false;
        if (grantedEpoch == epoch && owner == ownerPeerId) return true;
        epoch = nextEpoch = grantedEpoch;
        ownerPeerId = owner;
        localSequence = 1;
        handoffPending = false;
        takeoverPending = hasTransferState && owner == localPeer;
        return true;
    }

    // Never wrap an authority epoch back onto a previously valid generation.
    uint32_t next_grant_epoch() const {
        return nextEpoch == std::numeric_limits<uint32_t>::max() ? 0 : nextEpoch + 1;
    }
};

// Shared storage for one registered actor instance. Snapshot and transfer
// payload types are supplied by its adapter, not by the ownership layer.
template<class Snapshot, class TransferState>
struct Instance {
    Authority authority;
    Snapshot snapshot;
    TransferState ownershipState;
    uint32_t lastAppliedCollisionSequence = 0;
    uint32_t lastAppliedStateSequence = 0;
};

} // namespace dusklight_online::game::actor_sync
