#pragma once

#include "dusklight_online/game/protocol_router.hpp"
#include "dusk/multiplayer/multiplayer.hpp"
#include <map>
#include <functional>
#include <string>
#include <string_view>

namespace dusklight_online::game::actor_sync {

using PeerPoses = std::map<std::string, dusk::multiplayer::PeerPoseSnapshot>;

// Each adapter owns its engine hooks and validates its actor-specific payload.
// The registry dispatches by stable instance identity, never by process address.
struct Adapter {
    std::function<void(const net::Status&, bool, bool, const PeerPoses&)> update;
    std::function<ApplyResult(const RoutedMessage&)> consume;
    std::function<void(std::string_view)> peerLeft;
    std::function<void()> reset;
    std::function<bool()> active = [] { return false; };
};

class Registry {
public:
    bool add(std::string id, Adapter adapter) {
        if (id.empty() || !adapter.update || !adapter.consume ||
            !adapter.peerLeft || !adapter.reset) return false;
        return adapters_.emplace(std::move(id), adapter).second;
    }
    void remove(std::string_view id) { adapters_.erase(std::string(id)); }
    ApplyResult consume(const RoutedMessage& message) const {
        if (!worldActive_ || !message.ingress.welcomed ||
            !message.ingress.settings.syncWorld) return ApplyResult::IgnoredByPolicy;
        const auto id = message.payload.find("sync_id");
        if (id == message.payload.end() || !id->is_string()) return ApplyResult::Rejected;
        const auto found = adapters_.find(id->get_ref<const std::string&>());
        return found == adapters_.end() ? ApplyResult::IgnoredByPolicy :
                                        found->second.consume(message);
    }
    void update(const net::Status& status, bool stageLoaded, const PeerPoses& poses) {
        worldActive_ = status.welcomed && status.settings.syncWorld;
        for (const auto& [id, adapter] : adapters_)
            adapter.update(status, worldActive_, stageLoaded, poses);
    }
    bool active() const {
        for (const auto& [id, adapter] : adapters_)
            if (adapter.active && adapter.active()) return true;
        return false;
    }
    void peer_left(std::string_view peer) const {
        for (const auto& [id, adapter] : adapters_) adapter.peerLeft(peer);
    }
    void reset() {
        worldActive_ = false;
        for (const auto& [id, adapter] : adapters_) adapter.reset();
    }
private:
    bool worldActive_ = false;
    std::map<std::string, Adapter, std::less<>> adapters_;
};

inline Registry& registry() {
    static Registry instance;
    return instance;
}

} // namespace dusklight_online::game::actor_sync
