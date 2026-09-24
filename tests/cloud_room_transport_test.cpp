#include "dusklight_online/net/transport.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dusklight_online::net;
using json = nlohmann::json;

namespace {

class Channel;

struct Broker {
    std::map<std::string, Channel*> members;
    uint32_t nextId = 1;
    bool gameplayForwardingAttempt = false;
    bool forwardIce = true;
    std::string dropJoinNotificationTo;
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
            if (other != &channel && other->name != dropJoinNotificationTo)
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

    // A joined peer can receive authenticated ICE even if its peer-joined
    // notification was lost. It must recover the link and reply, not discard
    // the first description as an unknown peer.
    {
        Broker missedJoinBroker;
        missedJoinBroker.dropJoinNotificationTo = "Host";
        Transport waitingHost;
        Transport arrivingGuest;
        configuration.createRoom = true;
        configuration.name = "Host";
        if (!waitingHost.start_cloud_room(configuration,
                                          std::make_unique<Channel>(missedJoinBroker), &error))
            fail(error.c_str());
        configuration.createRoom = false;
        configuration.name = "Guest";
        if (!arrivingGuest.start_cloud_room(configuration,
                                            std::make_unique<Channel>(missedJoinBroker), &error))
            fail(error.c_str());
        const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        bool recovered = false;
        while (std::chrono::steady_clock::now() < recoveryDeadline) {
            waitingHost.tick();
            arrivingGuest.tick();
            recovered = waitingHost.status().natPeerCount == 1 &&
                        arrivingGuest.status().natPeerCount == 1 &&
                        waitingHost.peers().size() == 1;
            if (recovered) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!recovered || missedJoinBroker.gameplayForwardingAttempt)
            fail("missed peer-joined notification did not recover through ICE");
        const std::string departedId = arrivingGuest.status().clientId;
        arrivingGuest.disconnect();
        waitingHost.tick();
        auto* hostChannel = missedJoinBroker.members.at(waitingHost.status().clientId);
        hostChannel->push({{"type", "ice_signal"}, {"client_id", departedId},
                           {"kind", 0}, {"data", "late description"}, {"generation", 0}});
        waitingHost.tick();
        if (!waitingHost.peers().empty())
            fail("late ICE from a departed peer recreated a ghost link");
        waitingHost.disconnect();
    }

    // Grow an established room rather than admitting everyone at once. Each
    // player must have a direct route to every other player, and gameplay must
    // reach every recipient without passing through the room service.
    for (size_t playerCount : {size_t{3}, size_t{4}}) {
        Broker meshBroker;
        std::vector<std::unique_ptr<Transport>> players;
        for (size_t index = 0; index < playerCount; ++index) {
            auto player = std::make_unique<Transport>();
            configuration.createRoom = index == 0;
            configuration.name = "Player " + std::to_string(index);
            if (!player->start_cloud_room(configuration, std::make_unique<Channel>(meshBroker), &error))
                fail(error.c_str());
            players.push_back(std::move(player));

            const auto connectedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
            bool connected = false;
            while (std::chrono::steady_clock::now() < connectedDeadline) {
                for (auto& active : players) active->tick();
                connected = true;
                for (const auto& active : players) {
                    if (!active->status().error.empty()) {
                        std::cerr << "players=" << players.size() << " error=" << active->status().error << '\n';
                        fail("cloud mesh participant disconnected during admission");
                    }
                    connected &= active->status().welcomed &&
                        active->status().natPeerCount == players.size() - 1;
                }
                if (connected) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!connected) {
                for (size_t peer = 0; peer < players.size(); ++peer)
                    std::cerr << "players=" << players.size() << " peer=" << peer
                              << " nat=" << players[peer]->status().natPeerCount
                              << " error=" << players[peer]->status().error << '\n';
                fail("cloud mesh did not fully connect");
            }
        }

        for (size_t sender = 0; sender < players.size(); ++sender)
            if (!players[sender]->send({{"type", "chat"}, {"text", "mesh-" + std::to_string(sender)}}))
                fail("cloud mesh could not queue gameplay");
        std::vector<std::vector<bool>> received(players.size(), std::vector<bool>(players.size()));
        const auto deliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool allDelivered = false;
        while (std::chrono::steady_clock::now() < deliveryDeadline) {
            for (size_t recipient = 0; recipient < players.size(); ++recipient) {
                players[recipient]->tick();
                while (players[recipient]->has_events()) {
                    const auto event = players[recipient]->pop_event();
                    if (!event.message.is_object() || event.message.value("type", "") != "chat") continue;
                    for (size_t sender = 0; sender < players.size(); ++sender)
                        if (event.message.value("text", "") == "mesh-" + std::to_string(sender))
                            received[recipient][sender] = true;
                }
            }
            allDelivered = true;
            for (size_t recipient = 0; recipient < players.size(); ++recipient)
                for (size_t sender = 0; sender < players.size(); ++sender)
                    if (recipient != sender) allDelivered &= received[recipient][sender];
            if (allDelivered) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!allDelivered || meshBroker.gameplayForwardingAttempt)
            fail("cloud mesh failed all-to-all direct gameplay delivery");

        // Reuse the same transport after a player leaves and rejoins an
        // established room. Existing peers must admit the new identity and
        // complete a fresh ICE exchange with the returnee.
        const size_t returning = playerCount - 1;
        players[returning]->disconnect();
        const auto departureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool departed = false;
        while (std::chrono::steady_clock::now() < departureDeadline) {
            for (auto& active : players) active->tick();
            departed = true;
            for (size_t peer = 0; peer < returning; ++peer)
                departed &= players[peer]->peers().size() == playerCount - 2;
            if (departed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!departed) fail("remaining players did not observe cloud peer departure");
        configuration.createRoom = false;
        configuration.name = "Returning Player";
        if (!players[returning]->start_cloud_room(
                configuration, std::make_unique<Channel>(meshBroker), &error))
            fail(error.c_str());
        const auto rejoinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        bool rejoined = false;
        while (std::chrono::steady_clock::now() < rejoinDeadline) {
            for (auto& active : players) active->tick();
            rejoined = true;
            for (const auto& active : players)
                rejoined &= active->status().welcomed &&
                    active->status().natPeerCount == playerCount - 1 &&
                    active->status().error.empty();
            if (rejoined) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!rejoined) fail("returning cloud player did not reconnect to every existing peer");
        if (!players[returning]->send({{"type", "chat"}, {"text", "after-rejoin"}}))
            fail("returning cloud player could not send gameplay");
        std::vector<bool> rejoinReceived(returning);
        const auto rejoinDeliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < rejoinDeliveryDeadline) {
            for (size_t peer = 0; peer < playerCount; ++peer) {
                players[peer]->tick();
                while (players[peer]->has_events()) {
                    const auto event = players[peer]->pop_event();
                    if (peer < returning && event.message.is_object() &&
                        event.message.value("type", "") == "chat" &&
                        event.message.value("text", "") == "after-rejoin")
                        rejoinReceived[peer] = true;
                }
            }
            if (std::all_of(rejoinReceived.begin(), rejoinReceived.end(), [](bool received) { return received; }))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!std::all_of(rejoinReceived.begin(), rejoinReceived.end(), [](bool received) { return received; }))
            fail("returning cloud player's gameplay was not delivered to all existing peers");
        for (auto& player : players) player->disconnect();
    }

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
            timedOut |= side->status().error.find("Direct peer connection timed out after 15 seconds.") !=
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
