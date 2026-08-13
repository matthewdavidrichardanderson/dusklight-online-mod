#include "dusklight_online/net/transport.hpp"

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#include <cstdlib>
#include <iostream>
#include <string>

using dusklight_online::net::DirectHostConfig;
using dusklight_online::net::DirectJoinConfig;
using dusklight_online::net::EventKind;
using dusklight_online::net::Transport;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "transport test failed: " << message << '\n';
    std::exit(1);
}

uint16_t reserve_test_port() {
    const auto socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    if (socketHandle == INVALID_SOCKET) {
#else
    if (socketHandle < 0) {
#endif
        fail("could not create test socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        fail("could not reserve test port");
    }
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        fail("could not read test port");
    }
    const uint16_t port = ntohs(address.sin_port);
#if defined(_WIN32)
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
    return port;
}

bool drain_for_type(Transport& transport, const std::string& type,
                    nlohmann::json* found = nullptr) {
    bool matched = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.message.is_object() && event.message.value("type", "") == type) {
            matched = true;
            if (found != nullptr) {
                *found = std::move(event.message);
            }
        }
    }
    return matched;
}

bool drain_udp_for_sequence(Transport& transport, uint32_t sequence,
                            nlohmann::json* found = nullptr) {
    bool matched = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.kind == EventKind::UdpMessage &&
            event.message.value("sequence", 0U) == sequence) {
            matched = true;
            if (found != nullptr) {
                *found = std::move(event.message);
            }
        }
    }
    return matched;
}

bool drain_udp_object(Transport& transport, int32_t objectId) {
    bool matched = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.kind == EventKind::UdpRemoteObject &&
            event.remoteObject.objectId == objectId) {
            matched = true;
        }
    }
    return matched;
}

void pump(Transport& host, Transport& first, Transport& second, int ticks = 300) {
    for (int i = 0; i < ticks; ++i) {
        host.tick();
        first.tick();
        second.tick();
    }
}

}  // namespace

int main() {
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fail("WSAStartup failed");
    }
#endif

    const uint16_t port = reserve_test_port();
    Transport host;
    Transport alice;
    Transport bob;

    DirectHostConfig hostConfig;
    hostConfig.name = "Host";
    hostConfig.room = "transport-test";
    hostConfig.bindHost = "127.0.0.1";
    hostConfig.publicHost = "127.0.0.1";
    hostConfig.port = port;
    hostConfig.settings.syncWorld = true;
    hostConfig.settings.pvp = true;

    std::string error;
    if (!host.start_direct_host(hostConfig, &error)) {
        fail("host start: " + error);
    }

    DirectJoinConfig aliceConfig;
    aliceConfig.name = "Alice";
    aliceConfig.room = hostConfig.room;
    aliceConfig.host = hostConfig.publicHost;
    aliceConfig.port = port;
    if (!alice.start_direct_join(aliceConfig, &error)) {
        fail("Alice start: " + error);
    }

    DirectJoinConfig bobConfig = aliceConfig;
    bobConfig.name = "Bob";
    if (!bob.start_direct_join(bobConfig, &error)) {
        fail("Bob start: " + error);
    }

    pump(host, alice, bob);
    if (!alice.status().welcomed || !bob.status().welcomed || host.peers().size() != 2) {
        fail("three-way direct handshake did not complete");
    }
    if (!alice.status().settings.syncWorld || !alice.status().settings.pvp) {
        fail("host room settings were not applied by joiner");
    }

    while (host.has_events()) host.pop_event();
    while (alice.has_events()) alice.pop_event();
    while (bob.has_events()) bob.pop_event();

    // Bob's first pose registers his observed UDP endpoint with the direct
    // host. Alice's following pose must then be decoded and re-encoded by the
    // host for Bob, retaining Alice's authenticated peer identity.
    if (!bob.send_visual({{"type", "pose"}, {"sequence", 1},
                          {"state", {{"stage", "F_SP103"}, {"x", 1.0f}}}})) {
        fail("Bob UDP registration pose failed");
    }
    pump(host, alice, bob, 30);
    while (host.has_events()) host.pop_event();
    while (alice.has_events()) alice.pop_event();
    while (bob.has_events()) bob.pop_event();

    if (!alice.send_visual({{"type", "pose"}, {"sequence", 2},
                            {"state", {{"stage", "F_SP103"}, {"x", 9.5f}}}})) {
        fail("Alice UDP pose send failed");
    }
    pump(host, alice, bob, 60);
    nlohmann::json udpPose;
    if (!drain_udp_for_sequence(bob, 2, &udpPose) ||
        udpPose.value("client_id", "") != alice.status().clientId ||
        udpPose.value("state", nlohmann::json::object()).value("x", 0.0f) != 9.5f) {
        fail("direct UDP pose was not routed through the host");
    }

    dusklight_online::net::udp::RemoteObjectPacket object;
    object.sequence = 3;
    object.objectId = 9001;
    object.objectKind = 1;
    object.flags = dusklight_online::net::udp::ObjectActive;
    if (!alice.send_remote_object(object)) {
        fail("Alice UDP remote-object send failed");
    }
    pump(host, alice, bob, 60);
    if (!drain_udp_object(bob, 9001)) {
        fail("direct UDP remote object was not routed through the host");
    }

    const nlohmann::json eventBit = {
        {"type", "event_bit"}, {"event", 42}, {"value", true}};
    if (!alice.send(eventBit)) {
        fail("Alice gameplay send failed");
    }
    pump(host, alice, bob, 30);
    nlohmann::json received;
    if (!drain_for_type(host, "event_bit", &received) ||
        received.value("client_id", "").empty()) {
        fail("host did not receive authenticated gameplay message");
    }
    if (!drain_for_type(bob, "event_bit", &received) ||
        received.value("event", -1) != 42) {
        fail("direct host did not fan out gameplay message");
    }

    const std::string bobId = bob.status().clientId;
    if (!alice.send({{"type", "sync_request"}, {"target_client_id", bobId}})) {
        fail("targeted sync request send failed");
    }
    pump(host, alice, bob, 30);
    if (!drain_for_type(bob, "sync_request")) {
        fail("targeted direct sync request did not reach Bob");
    }
    if (drain_for_type(host, "sync_request")) {
        fail("targeted sync request leaked into host game handler");
    }

    dusklight_online::net::RoomSettings changed = host.status().settings;
    changed.remoteCollision = false;
    changed.pvp = true;
    if (!host.publish_room_settings(changed)) {
        fail("host room settings publish failed");
    }
    pump(host, alice, bob, 30);
    if (alice.status().settings.remoteCollision || alice.status().settings.pvp) {
        fail("joiner did not apply direct settings messages");
    }
    if (host.status().settings.pvp || bob.status().settings.pvp) {
        fail("PvP was not forced off with remote collision");
    }

    alice.disconnect();
    pump(host, alice, bob, 30);
    if (!drain_for_type(bob, "peer_left")) {
        fail("peer disconnect was not broadcast");
    }

    bob.disconnect();
    host.disconnect();
    std::cout << "transport test passed\n";
    return 0;
}
