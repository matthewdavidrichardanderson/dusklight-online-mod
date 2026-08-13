#pragma once

#include "dusklight_online/net/udp_codec.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace dusklight_online::net {

enum class Mode : uint8_t {
    Disabled,
    DirectHost,
    DirectJoin,
    Relay,
};

enum class State : uint8_t {
    Disconnected,
    Listening,
    Connecting,
    Connected,
};

struct RoomSettings {
    bool dummyModel = true;
    bool syncFlags = true;
    bool syncWorld = false;
    bool remoteCollision = true;
    bool pvp = false;
};

[[nodiscard]] inline bool effective_remote_collision(const RoomSettings& settings) {
    return settings.dummyModel && settings.remoteCollision;
}

[[nodiscard]] inline bool effective_pvp(const RoomSettings& settings) {
    return effective_remote_collision(settings) && settings.pvp;
}

struct DirectHostConfig {
    std::string name = "Host";
    std::string room = "dev";
    std::string bindHost = "0.0.0.0";
    std::string publicHost = "127.0.0.1";
    std::string sessionId;
    std::string sessionKey;
    uint16_t port = 34197;
    RoomSettings settings;
    bool wantPuppet = true;
    bool wantMidna = false;
};

struct DirectJoinConfig {
    std::string name = "Joiner";
    std::string room = "dev";
    std::string host = "127.0.0.1";
    std::string sessionId;
    std::string sessionKey;
    uint16_t port = 34197;
    RoomSettings settings;
    bool wantPuppet = true;
    bool wantMidna = false;
};

struct RelayConfig {
    std::string name = "Player";
    std::string room = "dev";
    std::string password;
    std::string host = "127.0.0.1";
    std::string sessionId;
    std::string sessionKey;
    uint16_t port = 34197;
    bool createRoom = false;
    RoomSettings settings;
    bool wantPuppet = true;
    bool wantMidna = false;
};

enum class EventKind : uint8_t {
    Connected,
    Disconnected,
    PeerJoined,
    PeerLeft,
    Message,
    UdpMessage,
    UdpRemoteObject,
    UdpAck,
    Error,
};

struct EventContext {
    uint64_t epoch = 0;
    Mode mode = Mode::Disabled;
    bool welcomed = false;
    RoomSettings settings;
    std::string clientId;
};

struct Event {
    EventKind kind = EventKind::Message;
    std::string peerId;
    std::string detail;
    nlohmann::json message;
    udp::PacketType udpType = udp::PacketType::PoseMsgpack;
    uint32_t udpSequence = 0;
    uint8_t udpStressFlags = 0;
    udp::RemoteObjectPacket remoteObject;
    EventContext ingress;
};

struct Status {
    Mode mode = Mode::Disabled;
    State state = State::Disconnected;
    bool enabled = false;
    bool welcomed = false;
    bool udpReady = false;
    bool isOwner = false;
    std::string name;
    std::string room;
    std::string host;
    std::string bindHost;
    std::string publicHost;
    std::string clientId;
    std::string ownerClientId;
    std::string udpToken;
    std::string error;
    uint16_t port = 0;
    RoomSettings settings;
};

// Non-blocking reliable transport for every JSON gameplay lane. tick() is
// called from the mod's update hook; it never waits for network input.
class Transport {
public:
    using MatrixExpandCallback = bool (*)(nlohmann::json&, const std::string&, uint8_t,
                                          uint32_t, std::string&);
    using MatrixPrepareCallback = bool (*)(nlohmann::json&, const std::string&, uint8_t,
                                           uint32_t, uint32_t, std::string&);
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool start_direct_host(const DirectHostConfig& config, std::string* error = nullptr);
    bool start_direct_join(const DirectJoinConfig& config, std::string* error = nullptr);
    bool start_relay(const RelayConfig& config, std::string* error = nullptr);

    void tick();
    bool send(const nlohmann::json& message);
    bool send_to(const std::string& peerId, const nlohmann::json& message);
    bool send_visual(const nlohmann::json& message,
                     udp::PacketType type = udp::PacketType::PoseMsgpack);
    bool send_remote_object(const udp::RemoteObjectPacket& object);
    void disconnect();

    [[nodiscard]] Status status() const;
    [[nodiscard]] const std::map<std::string, std::string>& peers() const;
    [[nodiscard]] bool has_events() const;
    Event pop_event();

    // Room owners use this after changing a host-controlled option. Direct
    // sessions emit the legacy individual setting messages; relay sessions
    // emit protocol-2 room_settings.
    bool publish_room_settings(const RoomSettings& settings);
    bool publish_visual_preferences(bool wantPuppet, bool wantMidna);
    void set_matrix_codec(MatrixExpandCallback expand, MatrixPrepareCallback prepare);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dusklight_online::net
