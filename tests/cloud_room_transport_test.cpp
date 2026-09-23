#include "dusklight_online/net/transport.hpp"

#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace dusklight_online::net;
using json = nlohmann::json;

namespace {

class Channel;

struct Broker {
    std::map<std::string, Channel*> members;
    uint32_t nextId = 1;
    bool gameplayForwardingAttempt = false;
    bool forwardIce = true;
    RoomSettings roomSettings;
    std::string owner;
    void accept(Channel& channel, const json& message);
    void leave(Channel& channel);
};

class Channel final : public RoomChannel {
public:
    explicit Channel(Broker& broker) : broker(broker) {}
    ~Channel() override { close(); }
    bool open(std::string_view, std::string&) override {
        active = true;
        events.push_back({EventKind::Open, {}});
        return true;
    }
    bool send(std::string_view text) override {
        if (!active) return false;
        broker.accept(*this, json::parse(text));
        return true;
    }
    bool poll(Event& event) override {
        if (events.empty()) return false;
        event = std::move(events.front()); events.pop_front();
        return true;
    }
    void close() override {
        if (!active) return;
        active = false;
        broker.leave(*this);
        events.clear();
        id.clear();
    }
    void push(const json& value) { events.push_back({EventKind::Message, value.dump()}); }
    Broker& broker;
    std::string id;
    std::string name;
    std::deque<Event> events;
    bool active = false;
};

void Broker::accept(Channel& channel, const json& message) {
    const auto type = message.value("type", "");
    if (type == "hello") {
        if (message.value("protocol_version", 0) != 3) throw std::runtime_error("wrong cloud protocol");
        channel.id = "client_" + std::to_string(nextId++);
        channel.name = message.value("name", "Player");
        if (owner.empty()) owner = channel.id;
        json peers = json::array();
        for (const auto& [id, other] : members)
            peers.push_back({{"client_id", id}, {"name", other->name}, {"want_puppet", true}});
        members[channel.id] = &channel;
        channel.push({{"type", "welcome"}, {"client_id", channel.id},
            {"owner_client_id", owner}, {"peers", peers},
            {"settings", {{"dummy_model", true}, {"sync_flags", true},
                          {"sync_world", false}, {"remote_collision", true}, {"pvp", false}}},
            {"settings_generation", 0}, {"settings_pending", false},
            {"semantic_visuals_ready", true}, {"snapshot_deltas_ready", true}});
        for (const auto& [id, other] : members)
            if (other != &channel)
                other->push({{"type", "peer_joined"}, {"client_id", channel.id},
                    {"name", channel.name}, {"want_puppet", true},
                    {"semantic_visuals_ready", true}, {"snapshot_deltas_ready", true}});
    } else if (type == "ice_signal") {
        if (!forwardIce) return;
        auto it = members.find(message.value("target_client_id", ""));
        if (it != members.end()) it->second->push({{"type", "ice_signal"},
            {"client_id", channel.id}, {"kind", message.at("kind")},
            {"data", message.at("data")}, {"generation", message.at("generation")}});
    } else if (type == "room_settings" || type == "settings_ready" || type == "kick") {
        // Covered by the Worker protocol test. This broker only needs to
        // exercise the real native ICE/gameplay path.
    } else {
        gameplayForwardingAttempt = true;
    }
}

void Broker::leave(Channel& channel) {
    if (channel.id.empty()) return;
    members.erase(channel.id);
    for (const auto& [id, other] : members)
        other->push({{"type", "peer_left"}, {"client_id", channel.id},
            {"semantic_visuals_ready", true}, {"snapshot_deltas_ready", true}});
    if (channel.id == owner) {
        owner = members.empty() ? "" : members.begin()->first;
        for (const auto& [id, other] : members)
            other->push({{"type", "owner_changed"}, {"owner_client_id", owner}});
    }
}

[[noreturn]] void fail(const char* reason) {
    std::cerr << "cloud transport test failed: " << reason << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    Broker broker;
    Transport host;
    Transport guest;
    CloudRoomConfig configuration;
    configuration.name = "Host";
    configuration.room = "Test Lobby";
    configuration.password = "secret-password";
    configuration.serverUrl = "https://rooms.example.test";
    configuration.stunHost.clear(); // host candidates suffice on one machine
    configuration.stunPort = 0;
    configuration.createRoom = true;
    std::string error;
    if (!host.start_cloud_room(configuration, std::make_unique<Channel>(broker), &error))
        fail(error.c_str());
    configuration.createRoom = false;
    configuration.name = "Guest";
    if (!guest.start_cloud_room(configuration, std::make_unique<Channel>(broker), &error))
        fail(error.c_str());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool sent = false, delivered = false, sawIceTx = false, sawIceRx = false;
    while (std::chrono::steady_clock::now() < deadline &&
           !(delivered && host.status().natPeerCount == 1 &&
             guest.status().natPeerCount == 1)) {
        host.tick();
        guest.tick();
        if (host.status().welcomed && guest.status().welcomed &&
            host.peers().size() == 1 && !sent) {
            if (!host.send({{"type", "chat"}, {"text", "direct only"}}))
                fail("gameplay send was not buffered before direct ICE connection");
            sent = true;
        }
        while (guest.has_events()) {
            const auto event = guest.pop_event();
            if (event.kind == EventKind::Diagnostic && event.detail.starts_with("ice_tx kind=0"))
                sawIceTx = true;
            if (event.kind == EventKind::Diagnostic && event.detail.starts_with("ice_rx kind=0") &&
                event.detail.find("accepted=yes") != std::string::npos)
                sawIceRx = true;
            if (event.message.is_object() && event.message.value("type", "") == "chat" &&
                event.message.value("text", "") == "direct only") delivered = true;
        }
        if (broker.gameplayForwardingAttempt) fail("gameplay was sent to the room channel");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!sent || !delivered || !sawIceTx || !sawIceRx || host.status().natPeerCount != 1 ||
        guest.status().natPeerCount != 1 || host.status().relayPeerCount != 0) {
        std::cerr << "sent=" << sent << " delivered=" << delivered
                  << " host_nat=" << host.status().natPeerCount
                  << " guest_nat=" << guest.status().natPeerCount
                  << " host_error=" << host.status().error
                  << " guest_error=" << guest.status().error << '\n';
        fail("direct-only ICE did not deliver the buffered gameplay message");
    }
    guest.disconnect();
    host.disconnect();

    // A room-service membership is not a peer connection. Without signaling,
    // the Cloud Room must fail promptly and explain which ICE step stalled.
    Broker stalled;
    stalled.forwardIce = false;
    Transport stalledHost;
    Transport stalledGuest;
    configuration.createRoom = true;
    if (!stalledHost.start_cloud_room(configuration, std::make_unique<Channel>(stalled), &error))
        fail(error.c_str());
    configuration.createRoom = false;
    if (!stalledGuest.start_cloud_room(configuration, std::make_unique<Channel>(stalled), &error))
        fail(error.c_str());
    bool timedOut = false, sawTimeoutDiagnostic = false;
    const auto stalledDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
    while (std::chrono::steady_clock::now() < stalledDeadline && !timedOut) {
        stalledHost.tick();
        stalledGuest.tick();
        for (Transport* side : {&stalledHost, &stalledGuest}) {
            while (side->has_events()) {
                const auto event = side->pop_event();
                if (event.kind == EventKind::Diagnostic && event.detail.starts_with("ice_timeout"))
                    sawTimeoutDiagnostic = true;
            }
            timedOut |= side->status().error.find("Direct peer connection timed out after 15 seconds") !=
                std::string::npos;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!timedOut || !sawTimeoutDiagnostic || stalled.gameplayForwardingAttempt)
        fail("stalled Cloud Room ICE did not fail with diagnostics at the 15-second deadline");
    stalledGuest.disconnect();
    stalledHost.disconnect();
    std::cout << "cloud transport test passed\n";
}
