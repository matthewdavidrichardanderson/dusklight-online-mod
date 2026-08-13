#include "dusklight_online/net/transport.hpp"
#include "dusk/multiplayer/multiplayer.hpp"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    constexpr int kSendFlags = 0;
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    constexpr socket_t kInvalidSocket = -1;
    #if defined(__APPLE__)
        constexpr int kSendFlags = 0;
    #else
        constexpr int kSendFlags = MSG_NOSIGNAL;
    #endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace dusklight_online::net {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxDirectPeers = 7;
constexpr size_t kTcpRxBufferMaxBytes = 2 * 1024 * 1024;
constexpr size_t kTcpTxBufferMaxBytes = 256 * 1024;
constexpr size_t kMaxMaterializedEvents = 1024;
constexpr size_t kUdpTxPacerMaxQueuedDatagrams = 512;
constexpr size_t kUdpTxPacerBaseDestinationBytesPerSecond = 512 * 1024;
constexpr size_t kUdpTxPacerVisualFlowBytesPerSecond = 256 * 1024;
constexpr auto kUdpTxPacerFlowActiveWindow = std::chrono::seconds(2);
constexpr auto kUdpTxPacerMinDatagramGap = std::chrono::milliseconds(1);
#if defined(_WIN32)
std::mutex sNetworkStackMutex;
size_t sNetworkStackOwners = 0;
#endif

void close_socket(socket_t& socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
    socket = kInvalidSocket;
}

bool would_block() {
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

bool set_nonblocking(socket_t socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool suppress_sigpipe(socket_t socket) {
#if defined(__APPLE__)
    int enabled = 1;
    return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#else
    (void)socket;
    return true;
#endif
}

int socket_error(socket_t socket) {
    int error = 0;
#if defined(_WIN32)
    int length = sizeof(error);
    getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
#else
    socklen_t length = sizeof(error);
    getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length);
#endif
    return error;
}

bool fill_ipv4(sockaddr_in& address, const std::string& host, uint16_t port) {
    address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1) {
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    address.sin_addr = reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
}

bool acquire_network_stack(bool& owned, std::string* error) {
    if (owned) return true;
#if defined(_WIN32)
    std::lock_guard lock(sNetworkStackMutex);
    if (sNetworkStackOwners == 0) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            if (error != nullptr) *error = "WSAStartup failed";
            return false;
        }
    }
    ++sNetworkStackOwners;
#else
    (void)error;
#endif
    owned = true;
    return true;
}

void release_network_stack(bool& owned) {
    if (!owned) return;
#if defined(_WIN32)
    std::lock_guard lock(sNetworkStackMutex);
    if (sNetworkStackOwners > 0 && --sNetworkStackOwners == 0) WSACleanup();
#endif
    owned = false;
}

json settings_json(const RoomSettings& settings) {
    return {
        {"dummy_model", settings.dummyModel},
        {"sync_flags", settings.syncFlags},
        {"sync_world", settings.syncWorld},
        {"remote_collision", settings.remoteCollision},
        {"pvp", effective_pvp(settings)},
    };
}

RoomSettings parse_settings(const json& value, RoomSettings fallback) {
    if (!value.is_object()) {
        return fallback;
    }
    fallback.dummyModel = value.value("dummy_model", fallback.dummyModel);
    fallback.syncFlags = value.value("sync_flags", fallback.syncFlags);
    fallback.syncWorld = value.value("sync_world", fallback.syncWorld);
    fallback.remoteCollision = value.value("remote_collision", fallback.remoteCollision);
    fallback.pvp = value.value("pvp", fallback.pvp) && fallback.remoteCollision;
    return fallback;
}

bool should_forward_direct(const std::string& type) {
    return type != "hello" && type != "ping" && type != "pong" && type != "error";
}

}  // namespace

struct Transport::Impl {
    struct Peer {
        socket_t socket = kInvalidSocket;
        sockaddr_in udpAddress{};
        std::string id;
        std::string name = "Peer";
        std::string rx;
        std::string tx;
        bool welcomed = false;
        bool udpAddressKnown = false;
        bool wantPuppet = true;
        bool wantMidna = false;
    };

    struct PeerPresence {
        std::string stage;
        uint32_t ageTicks = 0;
    };

    struct PacedDatagram {
        sockaddr_in address{};
        udp::Datagram datagram;
        std::string queueKey;
    };

    Status status;
    socket_t socket = kInvalidSocket;
    socket_t listenSocket = kInvalidSocket;
    socket_t udpSocket = kInvalidSocket;
    sockaddr_in udpRemoteAddress{};
    std::string rx;
    std::string tx;
    std::map<std::string, Peer> directPeers;
    std::map<std::string, std::string> peerNames;
    std::map<std::string, PeerPresence> peerStages;
    std::map<std::string, std::string> peerPoseStages;
    std::deque<Event> events;
    uint32_t nextDirectPeerId = 1;
    uint32_t reconnectTicks = 0;
    uint32_t relayUdpRegisterTicks = 0;
    bool automaticReconnect = true;
    bool helloSent = false;
    bool relayCreateRoom = false;
    bool relayMayRecreateRoom = false;
    bool handshakeRejected = false;
    bool wantPuppet = true;
    bool wantMidna = false;
    std::string password;
    std::string sessionId;
    std::string sessionKey;
    bool udpRemoteAddressKnown = false;
    udp::Decoder udpDecoder;
    Transport::MatrixExpandCallback matrixExpand = nullptr;
    Transport::MatrixPrepareCallback matrixPrepare = nullptr;
    std::map<std::string, uint32_t> matrixAckSequences;
    std::mutex udpTxMutex;
    std::condition_variable udpTxCv;
    std::deque<PacedDatagram> udpTxQueue;
    std::thread udpTxThread;
    bool udpTxStop = false;
    bool udpTxRunning = false;
    bool networkStackOwned = false;
    bool eventQueueOverflow = false;
    uint64_t connectionEpoch = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> udpTxNextSendByDestination;
    std::map<std::string,
             std::map<std::string, std::chrono::steady_clock::time_point>>
        udpTxVisualFlowsByDestination;

    ~Impl() {
        close_all();
        release_network_stack(networkStackOwned);
    }

    bool push_event(Event event) {
        event.ingress = {
            connectionEpoch, status.mode, status.welcomed,
            status.settings, status.clientId,
        };
        if (event.kind == EventKind::UdpMessage) {
            for (auto it = events.rbegin(); it != events.rend(); ++it) {
                if (it->kind == event.kind && it->peerId == event.peerId &&
                    it->udpType == event.udpType) {
                    if (event.udpSequence > it->udpSequence) *it = std::move(event);
                    return true;
                }
            }
        } else if (event.kind == EventKind::UdpAck) {
            for (auto it = events.rbegin(); it != events.rend(); ++it) {
                if (it->kind == event.kind && it->peerId == event.peerId &&
                    it->detail == event.detail && it->udpType == event.udpType) {
                    if (event.udpSequence > it->udpSequence) *it = std::move(event);
                    return true;
                }
            }
        } else if (event.kind == EventKind::UdpRemoteObject) {
            for (auto it = events.rbegin(); it != events.rend(); ++it) {
                if (it->kind == event.kind && it->peerId == event.peerId &&
                    it->remoteObject.objectId == event.remoteObject.objectId) {
                    if (event.remoteObject.sequence > it->remoteObject.sequence) {
                        *it = std::move(event);
                    }
                    return true;
                }
            }
        }
        if (events.size() >= kMaxMaterializedEvents) {
            eventQueueOverflow = true;
            return false;
        }
        events.push_back(std::move(event));
        return true;
    }

    bool emit(EventKind kind, std::string peerId = {}, std::string detail = {},
              json message = {}) {
        Event event;
        event.kind = kind;
        event.peerId = std::move(peerId);
        event.detail = std::move(detail);
        event.message = std::move(message);
        return push_event(std::move(event));
    }

    static std::string udp_tx_address_key(const sockaddr_in& address) {
        return std::to_string(ntohl(address.sin_addr.s_addr)) + ":" +
               std::to_string(ntohs(address.sin_port));
    }

    static std::string udp_tx_queue_key(const sockaddr_in& address,
                                        std::string_view senderId,
                                        udp::PacketType type,
                                        std::string_view receiverId) {
        return udp_tx_address_key(address) + "|" +
               std::to_string(static_cast<unsigned>(type)) + "|" +
               std::string(senderId) + "|" + std::string(receiverId);
    }

    static bool is_paced_visual_type(udp::PacketType type) {
        return type == udp::PacketType::PoseMsgpack ||
               type == udp::PacketType::MidnaMsgpack;
    }

    size_t udp_tx_target_rate_locked(
        const std::string& destinationKey,
        std::chrono::steady_clock::time_point now) {
        auto destination = udpTxVisualFlowsByDestination.find(destinationKey);
        if (destination == udpTxVisualFlowsByDestination.end()) {
            return kUdpTxPacerBaseDestinationBytesPerSecond;
        }
        auto& flows = destination->second;
        for (auto it = flows.begin(); it != flows.end();) {
            if (now - it->second > kUdpTxPacerFlowActiveWindow) {
                it = flows.erase(it);
            } else {
                ++it;
            }
        }
        if (flows.empty()) {
            udpTxVisualFlowsByDestination.erase(destination);
            return kUdpTxPacerBaseDestinationBytesPerSecond;
        }
        return std::max(kUdpTxPacerBaseDestinationBytesPerSecond,
                        flows.size() * kUdpTxPacerVisualFlowBytesPerSecond);
    }

    static std::chrono::microseconds udp_tx_gap(const udp::Datagram& datagram,
                                                 size_t bytesPerSecond) {
        const size_t bytes = std::max<size_t>(datagram.bytes.size(), 1);
        const auto byteRateGap = std::chrono::microseconds(
            static_cast<int64_t>((bytes * 1000000ull) / bytesPerSecond));
        return std::max(
            std::chrono::duration_cast<std::chrono::microseconds>(kUdpTxPacerMinDatagramGap),
            byteRateGap);
    }

    bool send_udp_datagram_now(const sockaddr_in& address,
                               const udp::Datagram& datagram) {
        if (udpSocket == kInvalidSocket || datagram.bytes.empty()) {
            return false;
        }
        const int size = static_cast<int>(datagram.bytes.size());
        return sendto(udpSocket, reinterpret_cast<const char*>(datagram.bytes.data()), size, 0,
                      reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == size;
    }

    void udp_tx_thread_main() {
        for (;;) {
            PacedDatagram paced;
            std::string destinationKey;
            size_t destinationRate = kUdpTxPacerBaseDestinationBytesPerSecond;
            {
                std::unique_lock<std::mutex> lock(udpTxMutex);
                udpTxCv.wait(lock, [this] { return udpTxStop || !udpTxQueue.empty(); });
                for (;;) {
                    if (udpTxStop) return;
                    const auto now = std::chrono::steady_clock::now();
                    auto ready = udpTxQueue.end();
                    auto nextReady = std::chrono::steady_clock::time_point::max();
                    for (auto it = udpTxQueue.begin(); it != udpTxQueue.end(); ++it) {
                        const std::string key = udp_tx_address_key(it->address);
                        const auto lane = udpTxNextSendByDestination.find(key);
                        const auto eligibleAt = lane == udpTxNextSendByDestination.end() ?
                            now : lane->second;
                        if (eligibleAt <= now) {
                            ready = it;
                            break;
                        }
                        nextReady = std::min(nextReady, eligibleAt);
                    }
                    if (ready != udpTxQueue.end()) {
                        destinationKey = udp_tx_address_key(ready->address);
                        destinationRate = udp_tx_target_rate_locked(destinationKey, now);
                        paced = std::move(*ready);
                        udpTxQueue.erase(ready);
                        break;
                    }
                    udpTxCv.wait_until(lock, nextReady);
                }
            }

            const auto gap = udp_tx_gap(paced.datagram, destinationRate);
            if (!send_udp_datagram_now(paced.address, paced.datagram)) {
                if (would_block()) {
                    std::lock_guard<std::mutex> lock(udpTxMutex);
                    if (!udpTxStop) udpTxQueue.push_front(std::move(paced));
                    udpTxNextSendByDestination[destinationKey] =
                        std::chrono::steady_clock::now() + gap * 2;
                }
            } else {
                std::lock_guard<std::mutex> lock(udpTxMutex);
                udpTxNextSendByDestination[destinationKey] =
                    std::chrono::steady_clock::now() + gap;
            }
        }
    }

    void start_udp_tx_pacer() {
        std::lock_guard<std::mutex> lock(udpTxMutex);
        if (udpTxRunning) return;
        udpTxQueue.clear();
        udpTxNextSendByDestination.clear();
        udpTxVisualFlowsByDestination.clear();
        udpTxStop = false;
        udpTxRunning = true;
        udpTxThread = std::thread([this] { udp_tx_thread_main(); });
    }

    void stop_udp_tx_pacer() {
        {
            std::lock_guard<std::mutex> lock(udpTxMutex);
            if (!udpTxRunning) {
                udpTxQueue.clear();
                udpTxNextSendByDestination.clear();
                udpTxVisualFlowsByDestination.clear();
                return;
            }
            udpTxStop = true;
            udpTxQueue.clear();
            udpTxNextSendByDestination.clear();
            udpTxVisualFlowsByDestination.clear();
        }
        udpTxCv.notify_all();
        if (udpTxThread.joinable()) udpTxThread.join();
        {
            std::lock_guard<std::mutex> lock(udpTxMutex);
            udpTxStop = false;
            udpTxRunning = false;
        }
    }

    bool enqueue_udp_tx_datagrams(const sockaddr_in& address,
                                  std::vector<udp::Datagram> datagrams,
                                  std::string_view senderId,
                                  std::string_view receiverId,
                                  udp::PacketType type) {
        if (datagrams.empty()) return false;
        const uint32_t sequence = datagrams.front().sequence;
        const std::string queueKey =
            udp_tx_queue_key(address, senderId, type, receiverId);
        {
            std::lock_guard<std::mutex> lock(udpTxMutex);
            if (!udpTxRunning) return false;
            const std::string destinationKey = udp_tx_address_key(address);
            const auto now = std::chrono::steady_clock::now();
            if (is_paced_visual_type(type)) {
                udpTxVisualFlowsByDestination[destinationKey][queueKey] = now;
            }
            (void)udp_tx_target_rate_locked(destinationKey, now);

            std::set<uint32_t> fullyQueuedStaleSequences;
            for (const PacedDatagram& queued : udpTxQueue) {
                if (queued.queueKey != queueKey || queued.datagram.sequence >= sequence) {
                    continue;
                }
                if (queued.datagram.chunkIndex == 0) {
                    fullyQueuedStaleSequences.insert(queued.datagram.sequence);
                }
            }
            for (auto it = udpTxQueue.begin(); it != udpTxQueue.end();) {
                if (it->queueKey == queueKey &&
                    fullyQueuedStaleSequences.contains(it->datagram.sequence)) {
                    it = udpTxQueue.erase(it);
                } else {
                    ++it;
                }
            }
            for (udp::Datagram& datagram : datagrams) {
                udpTxQueue.push_back({address, std::move(datagram), queueKey});
            }
            while (udpTxQueue.size() > kUdpTxPacerMaxQueuedDatagrams) {
                const auto completeSnapshot = std::find_if(
                    udpTxQueue.begin(), udpTxQueue.end(), [](const PacedDatagram& queued) {
                        return queued.datagram.chunkIndex == 0;
                    });
                if (completeSnapshot == udpTxQueue.end()) break;
                const std::string droppedKey = completeSnapshot->queueKey;
                const uint32_t droppedSequence = completeSnapshot->datagram.sequence;
                for (auto it = udpTxQueue.begin(); it != udpTxQueue.end();) {
                    if (it->queueKey == droppedKey &&
                        it->datagram.sequence == droppedSequence) {
                        it = udpTxQueue.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        udpTxCv.notify_one();
        return true;
    }

    void close_all() {
        stop_udp_tx_pacer();
        close_socket(socket);
        close_socket(listenSocket);
        close_socket(udpSocket);
        for (auto& [id, peer] : directPeers) {
            (void)id;
            close_socket(peer.socket);
        }
        directPeers.clear();
    }

    void reset_runtime(bool keepConfiguration, bool preserveAcceptedEvents = false) {
        std::deque<Event> accepted;
        if (preserveAcceptedEvents) accepted = std::move(events);
        close_all();
        rx.clear();
        tx.clear();
        events.clear();
        peerNames.clear();
        peerStages.clear();
        peerPoseStages.clear();
        matrixAckSequences.clear();
        status.clientId.clear();
        status.ownerClientId.clear();
        status.udpToken.clear();
        status.udpReady = false;
        status.welcomed = false;
        status.isOwner = false;
        status.state = State::Disconnected;
        nextDirectPeerId = 1;
        reconnectTicks = 0;
        relayUdpRegisterTicks = 0;
        helloSent = false;
        handshakeRejected = false;
        udpRemoteAddressKnown = false;
        udpDecoder.reset();
        eventQueueOverflow = false;
        if (!keepConfiguration) {
            status = {};
            automaticReconnect = true;
            relayCreateRoom = false;
            relayMayRecreateRoom = false;
            wantPuppet = true;
            wantMidna = false;
            password.clear();
            sessionId.clear();
            sessionKey.clear();
        }
        if (preserveAcceptedEvents) events = std::move(accepted);
    }

    void fail(const std::string& reason, bool allowReconnect = true,
              bool preserveAcceptedEvents = true) {
        const bool wasActive = status.state != State::Disconnected;
        const bool reconnect = automaticReconnect && allowReconnect;
        // A reconnect is a new transport epoch. Retaining UDP decoder
        // sequences, ACK baselines, peer identity or the old UDP endpoint can
        // make valid packets in the next connection look stale or route them
        // into the departed session.
        reset_runtime(true, preserveAcceptedEvents);
        status.error = reason;
        automaticReconnect = reconnect;
        if (wasActive) {
            emit(EventKind::Disconnected, {}, reason);
        }
    }

    bool flush(socket_t target, std::string& buffer) {
        if (target == kInvalidSocket) {
            return false;
        }
        while (!buffer.empty()) {
            const int count = static_cast<int>(std::min<size_t>(
                buffer.size(), static_cast<size_t>(std::numeric_limits<int>::max())));
            const int sent = ::send(target, buffer.data(), count, kSendFlags);
            if (sent > 0) {
                buffer.erase(0, static_cast<size_t>(sent));
                continue;
            }
            if (would_block()) {
                return true;
            }
            return false;
        }
        return true;
    }

    bool queue(socket_t target, std::string& buffer, const json& message) {
        std::string line = message.dump();
        line.push_back('\n');
        if (buffer.size() + line.size() > kTcpTxBufferMaxBytes) {
            return false;
        }
        buffer.append(line);
        return flush(target, buffer);
    }

    bool queue_peer(Peer& peer, const json& message) {
        if (queue(peer.socket, peer.tx, message)) return true;
        // Match final's central send_json_to_peer failure contract: a peer
        // that cannot accept a bounded reliable frame is no longer usable.
        // Map erasure remains deferred to the owning pump iteration.
        close_socket(peer.socket);
        return false;
    }

    void remove_peer(const std::string& peerId, const std::string& reason) {
        auto it = directPeers.find(peerId);
        if (it == directPeers.end()) {
            return;
        }
        close_socket(it->second.socket);
        directPeers.erase(it);
        peerNames.erase(peerId);
        peerStages.erase(peerId);
        peerPoseStages.erase(peerId);
        emit(EventKind::PeerLeft, peerId, reason,
             {{"type", "peer_left"}, {"client_id", peerId}});
        const json left = {{"type", "peer_left"}, {"client_id", peerId}};
        std::vector<std::string> failed;
        for (auto& [id, peer] : directPeers) {
            if (peer.welcomed && !queue_peer(peer, left)) {
                failed.push_back(id);
            }
        }
        for (const std::string& id : failed) {
            // Defer map erasure to pump_direct_peers(). This function can be
            // reached while that map is being iterated.
            close_socket(directPeers.at(id).socket);
        }
        status.welcomed = std::any_of(directPeers.begin(), directPeers.end(),
                                     [](const auto& item) { return item.second.welcomed; });
    }

    bool broadcast(const json& message, const std::string& excluded = {}) {
        bool sentAny = false;
        std::vector<std::string> failed;
        for (auto& [id, peer] : directPeers) {
            if (!peer.welcomed || id == excluded) {
                continue;
            }
            if (queue_peer(peer, message)) {
                sentAny = true;
            } else {
                failed.push_back(id);
            }
        }
        for (const std::string& id : failed) {
            // See remove_peer(): socket invalidation is safe during an
            // iteration; map erasure is performed by the next pump.
            close_socket(directPeers.at(id).socket);
        }
        return sentAny || directPeers.empty();
    }

    json direct_peer_list(const std::string& excluded) const {
        json peers = json::array();
        for (const auto& [id, peer] : directPeers) {
            if (peer.welcomed && id != excluded) {
                peers.push_back({{"client_id", id}, {"name", peer.name}});
            }
        }
        return peers;
    }

    void welcome_direct_peer(Peer& peer) {
        if (peer.welcomed) {
            return;
        }
        const json welcome = {
            {"type", "welcome"},
            {"protocol_version", 1},
            {"room_id", status.room},
            {"client_id", peer.id},
            {"direct_peer_name", status.name},
            {"dummy_model", status.settings.dummyModel},
            {"sync_flags", status.settings.syncFlags},
            {"sync_world", status.settings.syncWorld},
            {"remote_collision", status.settings.remoteCollision},
            {"pvp", status.settings.pvp && status.settings.remoteCollision},
            {"want_puppet", status.settings.dummyModel},
            {"want_midna", wantMidna},
            {"peers", direct_peer_list(peer.id)},
        };
        peer.welcomed = queue_peer(peer, welcome);
        status.welcomed = status.welcomed || peer.welcomed;
    }

    void send_hello() {
        if (helloSent || socket == kInvalidSocket) {
            return;
        }
        json hello = {
            {"type", "hello"},
            {"protocol_version", status.mode == Mode::Relay ? 2 : 1},
            {"room_id", status.room},
            {"session_id", sessionId},
            {"password", password},
            {"name", status.name},
            {"want_puppet", wantPuppet},
            {"want_midna", wantMidna},
        };
        if (status.mode == Mode::Relay) {
            hello["action"] = relayCreateRoom ? "create" : "join";
            if (relayCreateRoom) {
                hello["settings"] = settings_json(status.settings);
            }
        }
        helloSent = queue(socket, tx, hello);
    }

    bool begin_connect() {
        // A retry is a new generation. Anything not drained from an older
        // failed generation is stale and must never replay into it.
        events.clear();
        ++connectionEpoch;
        // Online treats UDP as an optional visual fast path. A machine which
        // cannot open/resolve the datagram channel must still be able to join
        // over the reliable TCP gameplay channel.
        close_socket(socket);
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == kInvalidSocket) {
            status.error = "socket failed";
            return false;
        }
        if (!suppress_sigpipe(socket)) {
            fail("socket SIGPIPE setup failed", false);
            return false;
        }
        if (!set_nonblocking(socket)) {
            fail("nonblocking failed", false);
            return false;
        }
        sockaddr_in address{};
        if (!fill_ipv4(address, status.host, status.port)) {
            fail("invalid host", false);
            return false;
        }
        // Establish the reliable socket/address first. A later TCP failure
        // cannot strand a datagram pacer which was opened for no connection.
        (void)setup_client_udp();
        const int result = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result == 0) {
            status.state = State::Connected;
            send_hello();
            return true;
        }
        if (!would_block()) {
            fail("connect failed");
            return false;
        }
        status.state = State::Connecting;
        return true;
    }

    bool begin_host() {
        events.clear();
        ++connectionEpoch;
        close_socket(listenSocket);
        listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == kInvalidSocket) {
            status.error = "socket failed";
            return false;
        }
        if (!suppress_sigpipe(listenSocket)) {
            fail("listen SIGPIPE setup failed", false);
            return false;
        }
        int reuse = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (!set_nonblocking(listenSocket)) {
            fail("listen nonblocking failed", false);
            return false;
        }
        sockaddr_in address{};
        if (!fill_ipv4(address, status.bindHost, status.port)) {
            fail("invalid bind host", false);
            return false;
        }
        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listenSocket, SOMAXCONN) != 0) {
            fail("listen failed", false);
            return false;
        }
        status.state = State::Listening;
        status.error.clear();
        // Match Online's best-effort direct-host visual channel. TCP owns the
        // session; UDP failure only disables pose/object datagrams.
        (void)open_udp(status.bindHost, status.port);
        return true;
    }

    bool open_udp(const std::string& bindHost, uint16_t bindPort) {
        stop_udp_tx_pacer();
        close_socket(udpSocket);
        udpRemoteAddressKnown = false;
        udpDecoder.reset();
        udpSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udpSocket == kInvalidSocket || !set_nonblocking(udpSocket)) {
            close_socket(udpSocket);
            return false;
        }
        int bufferBytes = 1024 * 1024;
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
        setsockopt(udpSocket, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
        sockaddr_in address{};
        if (!fill_ipv4(address, bindHost, bindPort) ||
            bind(udpSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close_socket(udpSocket);
            return false;
        }
        start_udp_tx_pacer();
        return true;
    }

    bool setup_client_udp() {
        if (!open_udp("0.0.0.0", 0)) {
            return false;
        }
        if (!fill_ipv4(udpRemoteAddress, status.host, status.port)) {
            stop_udp_tx_pacer();
            close_socket(udpSocket);
            return false;
        }
        udpRemoteAddressKnown = true;
        return true;
    }

    bool send_udp_datagram(const sockaddr_in& address, const udp::Datagram& datagram) {
        return send_udp_datagram_now(address, datagram);
    }

    std::string local_udp_sender_id() const {
        if ((status.mode == Mode::DirectJoin || status.mode == Mode::Relay) &&
            !status.clientId.empty()) {
            return status.clientId;
        }
        return "direct";
    }

    static std::string matrix_ack_key(std::string_view receiverId,
                                      std::string_view senderId,
                                      udp::PacketType type) {
        return std::string(receiverId) + '\x1f' + std::string(senderId) + '\x1f' +
               std::to_string(static_cast<uint8_t>(type));
    }

    uint32_t matrix_ack_sequence(std::string_view receiverId,
                                 std::string_view senderId,
                                 udp::PacketType type) const {
        const auto it = matrixAckSequences.find(matrix_ack_key(receiverId, senderId, type));
        return it == matrixAckSequences.end() ? 0 : it->second;
    }

    bool stages_match(const json& message, const std::string& peerId,
                      std::string_view senderId) const {
        std::string source = message.value("stage", "");
        if (source.empty()) {
            const json state = message.value("state", json::object());
            if (state.is_object()) source = state.value("stage", "");
        }
        if (source.empty() && !senderId.empty()) {
            const auto pose = peerPoseStages.find(std::string(senderId));
            if (pose != peerPoseStages.end()) source = pose->second;
        }
        std::string targetStage;
        const auto presence = peerStages.find(peerId);
        if (presence != peerStages.end()) targetStage = presence->second.stage;
        if (targetStage.empty()) {
            const auto pose = peerPoseStages.find(peerId);
            if (pose != peerPoseStages.end()) targetStage = pose->second;
        }
        return source.empty() || targetStage.empty() || source == targetStage;
    }

    bool send_visual_to_direct_peers(const json& message, std::string_view senderId,
                                     udp::PacketType type,
                                     const std::string& excluded = {}) {
        bool sentAny = false;
        for (auto& [id, peer] : directPeers) {
            if (id == excluded || !peer.welcomed || !peer.udpAddressKnown ||
                !peer.wantPuppet || (type == udp::PacketType::MidnaMsgpack && !peer.wantMidna) ||
                !stages_match(message, id, senderId)) {
                continue;
            }
            json wireMessage = message;
            const uint32_t sequence = wireMessage.value("sequence", 0U);
            std::string error;
            if (matrixPrepare != nullptr &&
                !matrixPrepare(wireMessage, std::string(senderId),
                               static_cast<uint8_t>(type), sequence,
                               matrix_ack_sequence(id, senderId, type), error)) {
                emit(EventKind::Error, id, error);
                continue;
            }
            const auto datagrams = udp::encode_message(wireMessage, senderId, type, &error);
            if (datagrams.empty()) {
                emit(EventKind::Error, id, error);
                continue;
            }
            const bool peerOk = enqueue_udp_tx_datagrams(
                peer.udpAddress, datagrams, senderId, id, type);
            sentAny = peerOk || sentAny;
        }
        return sentAny || directPeers.empty();
    }

    bool send_object_to_direct_peers(const udp::RemoteObjectPacket& object,
                                     std::string_view senderId,
                                     const std::string& excluded = {}) {
        const auto datagram = udp::encode_remote_object(senderId, object);
        bool sentAny = false;
        for (auto& [id, peer] : directPeers) {
            if (id == excluded || !peer.welcomed || !peer.udpAddressKnown) {
                continue;
            }
            sentAny = send_udp_datagram(peer.udpAddress, datagram) || sentAny;
        }
        return sentAny || directPeers.empty();
    }

    void send_relay_udp_registration() {
        if (status.mode != Mode::Relay || !status.welcomed || !udpRemoteAddressKnown ||
            status.clientId.empty() || status.udpToken.empty()) {
            return;
        }
        send_udp_datagram(udpRemoteAddress,
                          udp::encode_relay_registration(status.clientId, status.udpToken));
    }

    bool same_endpoint(const sockaddr_in& left, const sockaddr_in& right) const {
        return left.sin_family == right.sin_family && left.sin_port == right.sin_port &&
               left.sin_addr.s_addr == right.sin_addr.s_addr;
    }

    void handle_udp_result(udp::DecodeResult decoded, const sockaddr_in& from) {
        if (decoded.kind == udp::DecodeKind::None ||
            decoded.senderId == local_udp_sender_id()) {
            return;
        }
        if (status.mode == Mode::DirectHost) {
            auto peer = directPeers.find(decoded.senderId);
            if (peer == directPeers.end()) {
                return;
            }
            peer->second.udpAddress = from;
            peer->second.udpAddressKnown = true;
        }

        if (decoded.kind == udp::DecodeKind::Message) {
            if (decoded.type == udp::PacketType::MidnaMsgpack) return;
            json routed = std::move(decoded.message);
            if (decoded.senderId != "direct") {
                routed["client_id"] = decoded.senderId;
            }
            if (matrixExpand != nullptr) {
                std::string error;
                if (!matrixExpand(routed, decoded.senderId,
                                  static_cast<uint8_t>(decoded.type), decoded.sequence,
                                  error)) {
                    udpDecoder.discard_message(decoded.messageToken);
                    emit(EventKind::Error, decoded.senderId,
                         error.empty() ? "matrix delta expansion failed" : error);
                    return;
                }
            }
            std::string poseStage = routed.value("stage", "");
            if (poseStage.empty()) {
                const json state = routed.value("state", json::object());
                if (state.is_object()) poseStage = state.value("stage", "");
            }
            // A corrupt/missing matrix delta is not a valid pose observation
            // and must not poison DirectHost's stage-routing fallback.
            if (!decoded.senderId.empty() && !poseStage.empty()) {
                peerPoseStages[decoded.senderId] = std::move(poseStage);
            }
            Event event;
            event.kind = EventKind::UdpMessage;
            event.peerId = decoded.senderId.empty() ? "direct" : decoded.senderId;
            event.message = routed;
            event.udpType = decoded.type;
            event.udpSequence = decoded.sequence;
            event.udpStressFlags = decoded.stressFlags;
            if (!push_event(std::move(event))) {
                udpDecoder.discard_message(decoded.messageToken);
                return;
            }

            if (status.mode == Mode::DirectHost) {
                send_visual_to_direct_peers(routed, decoded.senderId, decoded.type,
                                            decoded.senderId);
            }
            udpDecoder.commit_message(decoded.messageToken);
            if (decoded.type == udp::PacketType::PoseMsgpack ||
                decoded.type == udp::PacketType::MidnaMsgpack) {
                send_udp_datagram(from, udp::encode_ack(local_udp_sender_id(), decoded.senderId,
                                                        decoded.type, decoded.sequence,
                                                        decoded.stressFlags));
            }
        } else if (decoded.kind == udp::DecodeKind::RemoteObject) {
            if (!status.settings.syncWorld || decoded.remoteObject.objectKind == 0) return;
            Event event;
            event.kind = EventKind::UdpRemoteObject;
            event.peerId = decoded.senderId.empty() ? "direct" : decoded.senderId;
            event.udpType = decoded.type;
            event.udpSequence = decoded.sequence;
            event.remoteObject = decoded.remoteObject;
            if (!push_event(std::move(event))) return;
            if (status.mode == Mode::DirectHost) {
                send_object_to_direct_peers(decoded.remoteObject, decoded.senderId,
                                            decoded.senderId);
            }
        } else if (decoded.kind == udp::DecodeKind::Ack) {
            const auto ackedType = static_cast<udp::PacketType>(decoded.ack.ackedType);
            const std::string ackedSender = udp::acked_sender_id(decoded.ack);
            const std::string key = matrix_ack_key(decoded.senderId, ackedSender, ackedType);
            uint32_t& ackedSequence = matrixAckSequences[key];
            if (decoded.ack.sequence <= ackedSequence) return;
            ackedSequence = decoded.ack.sequence;
            Event event;
            event.kind = EventKind::UdpAck;
            event.peerId = decoded.senderId;
            event.udpType = ackedType;
            event.udpSequence = decoded.ack.sequence;
            event.udpStressFlags = decoded.ack.stressFlags;
            event.detail = ackedSender;
            (void)push_event(std::move(event));
        }
    }

    void pump_udp() {
        if (udpSocket == kInvalidSocket) {
            return;
        }
        std::array<uint8_t, 58 + udp::kChunkPayloadBytes> packet{};
        while (true) {
            sockaddr_in from{};
#if defined(_WIN32)
            int fromLength = sizeof(from);
#else
            socklen_t fromLength = sizeof(from);
#endif
            const int count = recvfrom(udpSocket, reinterpret_cast<char*>(packet.data()),
                                       static_cast<int>(packet.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (count < 0) {
                return;
            }
            if (status.mode == Mode::Relay &&
                (!udpRemoteAddressKnown || !same_endpoint(from, udpRemoteAddress))) {
                continue;
            }
            const std::span<const uint8_t> wire(
                packet.data(), static_cast<size_t>(count));
            const std::optional<udp::DatagramInfo> info = udp::inspect_datagram(wire);
            if (!info.has_value()) continue;
            if (!dusk::multiplayer::kRemoteMidnaStreamingEnabled &&
                info->type == udp::PacketType::MidnaMsgpack) {
                continue;
            }

            const std::string admittedSender = info->senderId.empty() ?
                std::string("direct") : info->senderId;
            if (status.mode == Mode::DirectHost) {
                auto peer = directPeers.find(admittedSender);
                if (peer == directPeers.end() || !peer->second.welcomed) {
                    // Unknown sender IDs must allocate zero Decoder state.
                    continue;
                }
                // Valid partial chunks are sufficient to learn the endpoint,
                // but only after the sender was admitted by its TCP identity.
                peer->second.udpAddress = from;
                peer->second.udpAddressKnown = true;
            } else if (admittedSender == local_udp_sender_id()) {
                continue;
            }

            auto decoded = udpDecoder.accept(wire);
            handle_udp_result(std::move(decoded), from);
            if (eventQueueOverflow) return;
        }
    }

    void update_connecting() {
        fd_set writes;
        fd_set errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(socket, &writes);
        FD_SET(socket, &errors);
        timeval timeout{0, 0};
#if defined(_WIN32)
        const int result = select(0, nullptr, &writes, &errors, &timeout);
#else
        const int result = select(socket + 1, nullptr, &writes, &errors, &timeout);
#endif
        if (result < 0 || FD_ISSET(socket, &errors) || socket_error(socket) != 0) {
            fail("connect poll failed");
            return;
        }
        if (!FD_ISSET(socket, &writes)) {
            return;
        }
        status.state = State::Connected;
        status.error.clear();
        send_hello();
    }

    void accept_peers() {
        while (listenSocket != kInvalidSocket) {
            sockaddr_in address{};
#if defined(_WIN32)
            int length = sizeof(address);
#else
            socklen_t length = sizeof(address);
#endif
            socket_t accepted =
                accept(listenSocket, reinterpret_cast<sockaddr*>(&address), &length);
            if (accepted == kInvalidSocket) {
                if (!would_block()) {
                    fail("accept failed");
                }
                return;
            }
            if (directPeers.size() >= kMaxDirectPeers) {
                close_socket(accepted);
                continue;
            }
            if (!suppress_sigpipe(accepted) || !set_nonblocking(accepted)) {
                close_socket(accepted);
                fail("accepted socket setup failed");
                return;
            }
            Peer peer;
            peer.socket = accepted;
            peer.id = "direct" + std::to_string(nextDirectPeerId++);
            directPeers.emplace(peer.id, std::move(peer));
            status.state = State::Connected;
        }
    }

    void handle_direct_message(const json& input, Peer& sender) {
        if (!input.is_object()) {
            emit(EventKind::Error, sender.id, "non-object JSON message rejected");
            return;
        }
        const std::string type = input.value("type", "");
        if (type == "hello") {
            sender.name = input.value("name", sender.name);
            sender.wantPuppet = input.value("want_puppet", sender.wantPuppet);
            sender.wantMidna = false;
            peerNames[sender.id] = sender.name;
            welcome_direct_peer(sender);
            const json joined = {
                {"type", "peer_joined"}, {"client_id", sender.id}, {"name", sender.name}};
            broadcast(joined, sender.id);
            emit(EventKind::PeerJoined, sender.id, sender.name, joined);
            return;
        }
        if (!sender.welcomed) {
            return;
        }

        json routed = input;
        routed["client_id"] = sender.id;
        if (type == "presence") {
            peerStages[sender.id] = {routed.value("stage", ""), 0};
        }
        if (type == "ping") {
            queue_peer(sender, {{"type", "pong"}});
            return;
        }
        if (type == "pong" || type == "error") {
            emit(EventKind::Message, sender.id, {}, routed);
            return;
        }
        if (type == "puppet_preference") {
            sender.wantPuppet = routed.value("want_puppet", sender.wantPuppet);
            sender.wantMidna = false;
            return;
        }

        const std::string target = routed.value("target_client_id", "");
        const bool targetedSync = type == "sync_request" ||
            (type == "save_snapshot" && routed.value("manual_sync", false));
        if (targetedSync && !status.settings.syncFlags) return;
        if (targetedSync && !target.empty() && target != "direct" && target != "host") {
            auto targetIt = directPeers.find(target);
            if (targetIt != directPeers.end() && targetIt->second.welcomed) {
                if (!queue_peer(targetIt->second, routed)) {
                    close_socket(targetIt->second.socket);
                }
            } else {
                emit(EventKind::Error, target, "target peer unavailable", routed);
            }
            return;
        }

        // The host must retain a reliable mutation locally before exposing it
        // to other peers. Otherwise queue pressure can create a split state in
        // which joiners apply an event the host silently dropped.
        if (!emit(EventKind::Message, sender.id, {}, routed)) return;
        if (should_forward_direct(type) &&
            type != "sync_request" &&
            !(type == "save_snapshot" && routed.value("manual_sync", false))) {
            broadcast(routed, sender.id);
        }
    }

    void handle_primary_message(const json& message) {
        if (!message.is_object()) {
            emit(EventKind::Error, {}, "non-object JSON message rejected");
            return;
        }
        const std::string type = message.value("type", "");
        if (type == "welcome") {
            status.welcomed = true;
            status.error.clear();
            status.clientId = message.value("client_id", "");
            status.ownerClientId = message.value("owner_client_id", "");
            status.udpToken = message.value("udp_token", "");
            status.isOwner = !status.clientId.empty() && status.clientId == status.ownerClientId;
            if (status.mode == Mode::Relay) {
                if (relayCreateRoom) {
                    relayMayRecreateRoom = true;
                }
                relayCreateRoom = false;
                status.settings = parse_settings(message.value("settings", json::object()),
                                                 status.settings);
                send_relay_udp_registration();
            } else {
                status.settings.dummyModel = message.value("dummy_model", status.settings.dummyModel);
                status.settings.syncFlags = message.value("sync_flags", status.settings.syncFlags);
                status.settings.syncWorld = message.value("sync_world", status.settings.syncWorld);
                status.settings.remoteCollision =
                    message.value("remote_collision", status.settings.remoteCollision);
                status.settings.pvp = message.value("pvp", status.settings.pvp) &&
                                      status.settings.remoteCollision;
                const std::string hostName = message.value("direct_peer_name", "Host");
                peerNames["direct"] = hostName;
            }
            const json peers = message.value("peers", json::array());
            if (!peers.is_array()) {
                emit(EventKind::Error, {}, "welcome peers field is not an array");
                return;
            }
            for (const json& peer : peers) {
                if (!peer.is_object()) continue;
                const std::string id = peer.value("client_id", "");
                if (!id.empty()) {
                    peerNames[id] = peer.value("name", id);
                }
            }
            emit(EventKind::Connected, status.clientId, {}, message);
            emit(EventKind::Message, status.clientId, {}, message);
            return;
        }
        if (type == "peer_joined") {
            const std::string id = message.value("client_id", "");
            if (!id.empty()) {
                peerNames[id] = message.value("name", id);
            }
            emit(EventKind::PeerJoined, id, message.value("name", id), message);
        } else if (type == "peer_left") {
            const std::string id = message.value("client_id", "");
            peerNames.erase(id);
            peerStages.erase(id);
            peerPoseStages.erase(id);
            emit(EventKind::PeerLeft, id, {}, message);
        } else if (type == "owner_changed") {
            status.ownerClientId = message.value("owner_client_id", "");
            status.isOwner = !status.clientId.empty() && status.clientId == status.ownerClientId;
            emit(EventKind::Message, {}, {}, message);
        } else if (type == "room_settings") {
            status.ownerClientId = message.value("owner_client_id", status.ownerClientId);
            status.isOwner = !status.clientId.empty() && status.clientId == status.ownerClientId;
            status.settings = parse_settings(message.value("settings", json::object()),
                                             status.settings);
            emit(EventKind::Message, {}, {}, message);
        } else if (type == "udp_ready") {
            status.udpReady = true;
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "dummy_model") {
            status.settings.dummyModel = message.value("enabled", status.settings.dummyModel);
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "sync_flags") {
            status.settings.syncFlags = message.value("enabled", status.settings.syncFlags);
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "sync_world") {
            status.settings.syncWorld = message.value("enabled", status.settings.syncWorld);
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "remote_collision") {
            status.settings.remoteCollision =
                message.value("enabled", status.settings.remoteCollision);
            if (!status.settings.remoteCollision) {
                status.settings.pvp = false;
            }
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "pvp_enabled") {
            status.settings.pvp = message.value("enabled", status.settings.pvp) &&
                                  status.settings.remoteCollision;
            emit(EventKind::Message, {}, {}, message);
        } else if (type == "ping") {
            queue(socket, tx, {{"type", "pong"}});
        } else if (type == "error") {
            const std::string reason = message.value("error", "remote error");
            emit(EventKind::Error, {}, reason, message);
            if (status.mode == Mode::Relay && !status.welcomed) {
                const bool recreate = reason == "lobby_not_found" && relayMayRecreateRoom;
                relayCreateRoom = recreate;
                automaticReconnect = recreate;
                handshakeRejected = true;
                status.error = recreate ? "Relay room vanished; recreating it" : reason;
            }
        } else {
            if (type == "presence") {
                const std::string id = message.value("client_id", "direct");
                peerStages[id] = {message.value("stage", ""), 0};
            }
            // Direct-host messages intentionally omit client_id on the wire.
            // Match the original Online implementation's resolve_peer_id()
            // fallback so host-originated gameplay is attributed to the host
            // instead of being discarded as an anonymous/self-invalid event.
            const char* fallbackSender = status.mode == Mode::DirectJoin ? "direct" : "";
            emit(EventKind::Message, message.value("client_id", fallbackSender), {}, message);
        }
    }

    template <typename Handler>
    bool receive(socket_t source, std::string& buffer, Handler&& handler) {
        std::array<char, 4096> bytes{};
        while (true) {
            const int count = recv(source, bytes.data(), static_cast<int>(bytes.size()), 0);
            if (count > 0) {
                buffer.append(bytes.data(), static_cast<size_t>(count));
                if (buffer.size() > kTcpRxBufferMaxBytes) {
                    return false;
                }
                size_t newline = std::string::npos;
                while ((newline = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, newline);
                    buffer.erase(0, newline + 1);
                    if (line.empty()) {
                        continue;
                    }
                    try {
                        handler(json::parse(line));
                    } catch (const json::exception&) {
                        emit(EventKind::Error, {}, "invalid JSON");
                    }
                    if (handshakeRejected) {
                        return false;
                    }
                    if (eventQueueOverflow) return false;
                }
                continue;
            }
            if (count == 0) {
                return false;
            }
            return would_block();
        }
    }

    void pump_primary() {
        if (!flush(socket, tx) ||
            !receive(socket, rx, [this](const json& message) { handle_primary_message(message); }) ||
            !flush(socket, tx)) {
            const bool overflow = eventQueueOverflow;
            const bool rejected = handshakeRejected;
            handshakeRejected = false;
            std::string reason = "remote closed";
            if (overflow) {
                reason = "transport event queue limit reached";
            } else if (rejected) {
                reason = status.error.empty() ? "relay handshake rejected" : status.error;
            }
            fail(reason,
                 overflow ? true : (rejected ? automaticReconnect : true),
                 !overflow && !rejected);
        }
    }

    void pump_direct_peers() {
        std::vector<std::string> failed;
        for (auto& [id, peer] : directPeers) {
            if (!flush(peer.socket, peer.tx) ||
                !receive(peer.socket, peer.rx,
                         [this, &peer](const json& message) {
                             handle_direct_message(message, peer);
                         }) ||
                !flush(peer.socket, peer.tx)) {
                failed.push_back(id);
            }
        }
        for (const std::string& id : failed) {
            remove_peer(id, "remote closed");
        }
    }

    void tick() {
        if (!status.enabled) {
            return;
        }
        for (auto it = peerStages.begin(); it != peerStages.end();) {
            if (++it->second.ageTicks > 180) {
                it = peerStages.erase(it);
            } else {
                ++it;
            }
        }
        if (status.state == State::Disconnected && automaticReconnect) {
            if ((reconnectTicks++ % 30) == 0) {
                if (status.mode == Mode::DirectHost) {
                    begin_host();
                } else {
                    begin_connect();
                }
            }
            return;
        }
        if (status.state == State::Connecting) {
            update_connecting();
            return;
        }
        if (status.mode == Mode::DirectHost &&
            (status.state == State::Listening || status.state == State::Connected)) {
            accept_peers();
            if (status.state == State::Disconnected) return;
            pump_direct_peers();
            if (eventQueueOverflow) {
                fail("transport event queue limit reached");
                return;
            }
            pump_udp();
            if (eventQueueOverflow) fail("transport event queue limit reached");
            return;
        }
        if (status.state == State::Connected) {
            send_hello();
            pump_primary();
            if (status.state == State::Disconnected) return;
            pump_udp();
            if (eventQueueOverflow) {
                fail("transport event queue limit reached");
                return;
            }
            if (status.mode == Mode::Relay && status.welcomed &&
                (++relayUdpRegisterTicks % 30) == 0) {
                send_relay_udp_registration();
            }
        }
    }
};

Transport::Transport() : impl_(std::make_unique<Impl>()) {}
Transport::~Transport() = default;

bool Transport::start_direct_host(const DirectHostConfig& config, std::string* error) {
    if (config.port == 0 || config.bindHost.empty() || config.publicHost.empty()) {
        if (error != nullptr) {
            *error = "Direct host requires a valid port, bind host, and public host";
        }
        return false;
    }
    if (!acquire_network_stack(impl_->networkStackOwned, error)) {
        return false;
    }
    impl_->reset_runtime(false);
    impl_->status.enabled = true;
    impl_->status.mode = Mode::DirectHost;
    impl_->status.name = config.name.empty() ? "Host" : config.name;
    impl_->status.room = config.room.empty() ? "dev" : config.room;
    impl_->status.bindHost = config.bindHost;
    impl_->status.publicHost = config.publicHost;
    impl_->status.port = config.port;
    impl_->status.settings = config.settings;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    impl_->sessionId = config.sessionId;
    impl_->sessionKey = config.sessionKey;
    impl_->wantPuppet = config.wantPuppet;
    impl_->wantMidna = false;
    impl_->automaticReconnect = true;
    if (!impl_->begin_host()) {
        impl_->status.enabled = false;
        if (error != nullptr) {
            *error = impl_->status.error;
        }
        return false;
    }
    return true;
}

bool Transport::start_direct_join(const DirectJoinConfig& config, std::string* error) {
    if (config.port == 0 || config.host.empty()) {
        if (error != nullptr) {
            *error = "Direct join requires a valid host and port";
        }
        return false;
    }
    if (!acquire_network_stack(impl_->networkStackOwned, error)) {
        return false;
    }
    impl_->reset_runtime(false);
    impl_->status.enabled = true;
    impl_->status.mode = Mode::DirectJoin;
    impl_->status.name = config.name.empty() ? "Joiner" : config.name;
    impl_->status.room = config.room.empty() ? "dev" : config.room;
    impl_->status.host = config.host;
    impl_->status.port = config.port;
    impl_->status.settings = config.settings;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    impl_->sessionId = config.sessionId;
    impl_->sessionKey = config.sessionKey;
    impl_->wantPuppet = config.wantPuppet;
    impl_->wantMidna = false;
    impl_->automaticReconnect = true;
    if (!impl_->begin_connect() && impl_->status.state == State::Disconnected) {
        if (error != nullptr) {
            *error = impl_->status.error;
        }
        return false;
    }
    return true;
}

bool Transport::start_relay(const RelayConfig& config, std::string* error) {
    if (config.port == 0 || config.host.empty() || config.room.empty()) {
        if (error != nullptr) {
            *error = "Relay connection requires a valid host, port, and lobby name";
        }
        return false;
    }
    if (config.password.size() < 6 || config.password.size() > 128) {
        if (error != nullptr) {
            *error = "Relay password must be between 6 and 128 characters";
        }
        return false;
    }
    if (!acquire_network_stack(impl_->networkStackOwned, error)) {
        return false;
    }
    impl_->reset_runtime(false);
    impl_->status.enabled = true;
    impl_->status.mode = Mode::Relay;
    impl_->status.name = config.name.empty() ? (config.createRoom ? "Host" : "Player") : config.name;
    impl_->status.room = config.room;
    impl_->status.host = config.host;
    impl_->status.port = config.port;
    impl_->status.settings = config.settings;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    impl_->password = config.password;
    impl_->sessionId = config.sessionId;
    impl_->sessionKey = config.sessionKey;
    impl_->relayCreateRoom = config.createRoom;
    impl_->relayMayRecreateRoom = false;
    impl_->wantPuppet = config.wantPuppet;
    impl_->wantMidna = false;
    impl_->automaticReconnect = true;
    if (!impl_->begin_connect() && impl_->status.state == State::Disconnected) {
        if (error != nullptr) {
            *error = impl_->status.error;
        }
        return false;
    }
    return true;
}

void Transport::tick() {
    impl_->tick();
}

bool Transport::send(const nlohmann::json& message) {
    if (!impl_->status.enabled || !message.is_object()) {
        return false;
    }
    if (impl_->status.mode == Mode::DirectHost) {
        return impl_->broadcast(message);
    }
    if (impl_->status.state != State::Connected || !impl_->queue(impl_->socket, impl_->tx, message)) {
        impl_->fail("send failed");
        return false;
    }
    return true;
}

bool Transport::send_to(const std::string& peerId, const nlohmann::json& message) {
    if (!impl_->status.enabled || peerId.empty() || !message.is_object()) {
        return false;
    }
    if (impl_->status.mode == Mode::DirectHost) {
        auto peer = impl_->directPeers.find(peerId);
        return peer != impl_->directPeers.end() && peer->second.welcomed &&
               impl_->queue_peer(peer->second, message);
    }
    nlohmann::json targeted = message;
    targeted["target_client_id"] = peerId;
    if (impl_->status.state != State::Connected ||
        !impl_->queue(impl_->socket, impl_->tx, targeted)) {
        impl_->fail("targeted send failed");
        return false;
    }
    return true;
}

bool Transport::send_visual(const nlohmann::json& message, udp::PacketType type) {
    if (!impl_->status.enabled || !impl_->status.welcomed || !message.is_object() ||
        type == udp::PacketType::MidnaMsgpack ||
        (type != udp::PacketType::PoseJson && type != udp::PacketType::PoseMsgpack &&
         type != udp::PacketType::MidnaMsgpack)) {
        return false;
    }
    if (impl_->status.mode == Mode::DirectHost) {
        return impl_->send_visual_to_direct_peers(message, impl_->local_udp_sender_id(), type);
    }
    if (!impl_->udpRemoteAddressKnown) {
        return false;
    }
    nlohmann::json wireMessage = message;
    const std::string senderId = impl_->local_udp_sender_id();
    const std::string receiverId = impl_->status.mode == Mode::DirectJoin ? "direct" : "";
    const uint32_t sequence = wireMessage.value("sequence", 0U);
    std::string error;
    if (impl_->matrixPrepare != nullptr &&
        !impl_->matrixPrepare(wireMessage, senderId, static_cast<uint8_t>(type), sequence,
                              impl_->matrix_ack_sequence(receiverId, senderId, type), error)) {
        impl_->emit(EventKind::Error, {}, error);
        return false;
    }
    const auto datagrams =
        udp::encode_message(wireMessage, senderId, type, &error);
    if (datagrams.empty()) {
        impl_->emit(EventKind::Error, {}, error);
        return false;
    }
    return impl_->enqueue_udp_tx_datagrams(impl_->udpRemoteAddress, datagrams,
                                           senderId, receiverId, type);
}

bool Transport::send_remote_object(const udp::RemoteObjectPacket& object) {
    if (!impl_->status.enabled || !impl_->status.welcomed ||
        !impl_->status.settings.syncWorld) {
        return false;
    }
    if (impl_->status.mode == Mode::DirectHost) {
        return impl_->send_object_to_direct_peers(object, impl_->local_udp_sender_id());
    }
    return impl_->udpRemoteAddressKnown &&
           impl_->send_udp_datagram(
               impl_->udpRemoteAddress,
               udp::encode_remote_object(impl_->local_udp_sender_id(), object));
}

void Transport::disconnect() {
    if (!impl_->status.enabled) {
        return;
    }
    impl_->reset_runtime(false);
    impl_->automaticReconnect = false;
    impl_->emit(EventKind::Disconnected, {}, "user requested");
}

Status Transport::status() const {
    return impl_->status;
}

const std::map<std::string, std::string>& Transport::peers() const {
    return impl_->peerNames;
}

bool Transport::has_events() const {
    return !impl_->events.empty();
}

Event Transport::pop_event() {
    if (impl_->events.empty()) {
        return {};
    }
    Event event = std::move(impl_->events.front());
    impl_->events.pop_front();
    return event;
}

bool Transport::publish_room_settings(const RoomSettings& settings) {
    if (impl_->status.mode == Mode::Relay && !impl_->status.isOwner) {
        return false;
    }
    if (impl_->status.mode != Mode::Relay && impl_->status.mode != Mode::DirectHost) {
        return false;
    }
    const RoomSettings previous = impl_->status.settings;
    impl_->status.settings = settings;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    if (impl_->status.mode == Mode::Relay) {
        return send({{"type", "room_settings"},
                     {"settings", settings_json(impl_->status.settings)}});
    }
    bool ok = true;
    if (previous.dummyModel != impl_->status.settings.dummyModel) {
        ok &= send({{"type", "dummy_model"},
                    {"enabled", impl_->status.settings.dummyModel}});
    }
    if (previous.syncFlags != impl_->status.settings.syncFlags) {
        ok &= send({{"type", "sync_flags"},
                    {"enabled", impl_->status.settings.syncFlags}});
    }
    if (previous.syncWorld != impl_->status.settings.syncWorld) {
        ok &= send({{"type", "sync_world"},
                    {"enabled", impl_->status.settings.syncWorld}});
    }
    const bool collisionChanged =
        previous.remoteCollision != impl_->status.settings.remoteCollision;
    if (collisionChanged) {
        ok &= send({{"type", "remote_collision"},
                    {"enabled", impl_->status.settings.remoteCollision}});
    }
    if (collisionChanged || previous.pvp != impl_->status.settings.pvp) {
        ok &= send({{"type", "pvp_enabled"},
                    {"enabled", effective_pvp(impl_->status.settings)}});
    }
    return ok;
}

bool Transport::publish_visual_preferences(bool wantPuppet, bool wantMidna) {
    (void)wantMidna;
    impl_->wantPuppet = wantPuppet;
    impl_->wantMidna = false;
    return send({{"type", "puppet_preference"},
                 {"want_puppet", wantPuppet},
                 {"want_midna", false}});
}

void Transport::set_matrix_codec(MatrixExpandCallback expand,
                                 MatrixPrepareCallback prepare) {
    impl_->matrixExpand = expand;
    impl_->matrixPrepare = prepare;
}

}  // namespace dusklight_online::net
