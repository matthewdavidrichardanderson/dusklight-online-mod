#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
using NativeSocket = int;
#endif
#include "dusklight_online/net/udp_connection.hpp"
#include "dusklight_online/net/datagram_scheduler.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace dusklight_online::net {
namespace {
constexpr NativeSocket badSocket = static_cast<NativeSocket>(-1);
uint32_t clock_ms() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool resolve(std::string_view name, uint16_t port, sockaddr_in& address) {
    address = {}; address.sin_family = AF_INET; address.sin_port = htons(port);
    std::string host(name);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1) return true;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) || !result) return false;
    address.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result); return true;
}
uint64_t read64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}
void write64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) bytes[i] = uint8_t(value >> (i * 8));
}
bool same(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
}
struct UdpConnection::Impl : DatagramTransport {
    struct Peer {
        sockaddr_in address{};
        sockaddr_in realtimeAddress{};
        uint64_t nonce = 0, session = 0;
        uint32_t started = 0, lastReceive = 0, lastControl = 0;
        bool ready = false, closed = false, accepted = false;
        std::string rx;
    };
    struct Raw { Address address; std::vector<uint8_t> bytes; };
    mutable std::mutex mutex;
    std::condition_variable activity;
    uint64_t activityGeneration = 0;
    std::jthread worker;
    NativeSocket socket = badSocket;
    bool stack = false, server = false;
    size_t capacity = 0;
    Id nextId = 1;
    std::map<Id, Peer> peers;
    std::deque<Id> accepted;
    std::deque<Raw> realtime;
    DatagramScheduler budget{*this};
    ReliableUdp reliable{budget, 4096};
    std::random_device random;
    uint64_t nonce() { uint64_t n = (uint64_t(random()) << 32) ^ random(); return n ? n : 1; }
    Id find(const sockaddr_in& address) const {
        for (const auto& [id, peer] : peers) if (!peer.closed && same(peer.address, address)) return id;
        return invalid;
    }
    bool send(LogicalPeerId id, std::span<const uint8_t> bytes) override {
        auto it = peers.find(static_cast<Id>(id));
        if (it == peers.end() || it->second.closed || socket == badSocket) return false;
        const auto& destination = datagram_kind(bytes) == DatagramKind::Realtime &&
            it->second.realtimeAddress.sin_port ? it->second.realtimeAddress : it->second.address;
        return sendto(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(sockaddr_in)) == static_cast<int>(bytes.size());
    }
    // Fixed-size, rate-limited session control outside reliability. Challenge
    // echoes establish return routability; game/room authentication remains in
    // the existing hello handler. They do not claim cryptographic identity.
    void control(Id id, uint8_t type) {
        auto& peer = peers.at(id);
        std::array<uint8_t, 21> bytes{'D','U','C','1',type};
        write64(bytes.data() + 5, peer.nonce); write64(bytes.data() + 13, peer.session);
        send(id, bytes);
        peer.lastControl = clock_ms();
    }
    bool ready(Id id) {
        auto& peer = peers.at(id);
        if (peer.ready) return true;
        if (!budget.add_peer(id)) return false;
        if (!reliable.add_peer(id, peer.session)) { budget.remove_peer(id); return false; }
        peer.ready = true;
        return true;
    }
    void close_peer(Id id, bool notify) {
        auto it = peers.find(id);
        if (it == peers.end()) return;
        if (notify && it->second.ready && !it->second.closed) control(id, 6);
        it->second.closed = true;
        reliable.remove_peer(id); budget.remove_peer(id);
    }
    void close() {
        for (auto& [id, peer] : peers) {
            if (peer.ready && !peer.closed && socket != badSocket) control(id, 6);
            reliable.remove_peer(id); budget.remove_peer(id);
        }
        peers.clear(); accepted.clear(); realtime.clear();
        if (socket != badSocket) {
#if defined(_WIN32)
            closesocket(socket);
#else
            ::close(socket);
#endif
        }
        socket = badSocket;
#if defined(_WIN32)
        if (stack) WSACleanup();
#endif
        stack = false;
    }
    void receive_control(const sockaddr_in& from, std::span<const uint8_t> bytes, uint32_t now) {
        if (bytes.size() != 21) return;
        uint8_t type = bytes[4]; uint64_t token = read64(bytes.data() + 5), session = read64(bytes.data() + 13);
        Id id = find(from);
        if (type == 1 && server && token && session == 0) {
            if (id == invalid) {
                if (peers.size() >= capacity) return;
                id = nextId++;
                Peer peer; peer.address = from; peer.nonce = token; peer.session = nonce();
                peer.started = peer.lastReceive = now;
                peers.emplace(id, std::move(peer));
            }
            auto& peer = peers.at(id);
            if (peer.nonce == token && uint32_t(now - peer.lastControl) >= 250) control(id, 2);
            return;
        }
        if (id == invalid) return;
        auto& peer = peers.at(id);
        if (peer.nonce != token || peer.closed) return;
        if (type == 2 && !server && !peer.ready && session) {
            if (peer.session && peer.session != session) return;
            const bool firstChallenge = peer.session == 0;
            peer.session = session; peer.lastReceive = now;
            if (firstChallenge || uint32_t(now - peer.lastControl) >= 100) control(id, 3);
            return;
        }
        if (!session || peer.session != session) return;
        if (type == 3 && server) {
            const bool firstConfirm = !peer.ready;
            if (!ready(id)) { close_peer(id, false); return; }
            if (!peer.accepted) { accepted.push_back(id); peer.accepted = true; }
            peer.lastReceive = now;
            if (firstConfirm || uint32_t(now - peer.lastControl) >= 100) control(id, 4);
        } else if (type == 4 && !server) {
            if (!ready(id)) { close_peer(id, false); return; }
            peer.lastReceive = now;
        } else if (type == 5 && peer.ready) peer.lastReceive = now;
        else if (type == 6 && peer.ready) close_peer(id, false);
    }
};
UdpConnection::UdpConnection() : impl_(std::make_unique<Impl>()) {}
UdpConnection::~UdpConnection() { close(); }
void UdpConnection::close() {
    // Join before taking the protocol lock: the worker may be inside poll().
    if (impl_->worker.joinable()) { impl_->worker.request_stop(); impl_->worker.join(); }
    std::lock_guard lock(impl_->mutex); impl_->close();
    ++impl_->activityGeneration;
    impl_->activity.notify_all();
}
bool UdpConnection::open(std::string_view host, uint16_t port, size_t capacity, bool server) {
    close();
    std::lock_guard lock(impl_->mutex);
    if (!capacity || capacity > 4096) return false;
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data) != 0) return false;
    impl_->stack = true;
#endif
    sockaddr_in address{};
    if (!resolve(host, port, address)) { impl_->close(); return false; }
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == badSocket) { impl_->close(); return false; }
#if defined(_WIN32)
    u_long enabled = 1;
    bool nonblocking = ioctlsocket(impl_->socket, FIONBIO, &enabled) == 0;
#else
    bool nonblocking = fcntl(impl_->socket, F_SETFL, O_NONBLOCK) == 0;
#endif
    int size = 1024 * 1024;
    setsockopt(impl_->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    setsockopt(impl_->socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    if (!nonblocking || bind(impl_->socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        impl_->close(); return false;
    }
    impl_->capacity = capacity; impl_->server = server;
    impl_->budget.update(clock_ms());
    // Socket servicing must survive game loading/paused updates just as TCP's
    // kernel ACK processing did. All access to protocol state is serialized;
    // no game callbacks execute on this worker.
    const NativeSocket nativeSocket = impl_->socket;
    impl_->worker = std::jthread([this, nativeSocket](std::stop_token stop) {
        while (!stop.stop_requested()) {
            poll();
            // Wake immediately for ingress. The timeout still services KCP,
            // pacing and shutdown when no datagrams arrive. close() joins this
            // worker before closing/reusing nativeSocket; never hold the
            // protocol mutex while waiting for socket readiness.
#if defined(_WIN32)
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(nativeSocket, &readable);
            timeval timeout{0, 5000};
            select(0, &readable, nullptr, nullptr, &timeout);
#else
            pollfd readable{nativeSocket, POLLIN, 0};
            ::poll(&readable, 1, 5);
#endif
        }
    });
    return true;
}
UdpConnection::Id UdpConnection::connect(std::string_view host, uint16_t port) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->socket == badSocket || impl_->server || impl_->peers.size() >= impl_->capacity) return invalid;
    Impl::Peer peer;
    if (!resolve(host, port, peer.address)) return invalid;
    peer.nonce = impl_->nonce(); peer.started = peer.lastReceive = clock_ms();
    Id id = impl_->nextId++; impl_->peers.emplace(id, std::move(peer));
    impl_->control(id, 1); return id;
}
UdpConnection::Id UdpConnection::accept(Address& address) {
    std::lock_guard lock(impl_->mutex);
    while (!impl_->accepted.empty()) {
        Id id = impl_->accepted.front(); impl_->accepted.pop_front();
        auto it = impl_->peers.find(id);
        if (it == impl_->peers.end() || it->second.closed) continue;
        address = {it->second.address.sin_addr.s_addr, ntohs(it->second.address.sin_port)};
        return id;
    }
    return invalid;
}
void UdpConnection::disconnect(Id id) {
    std::lock_guard lock(impl_->mutex); impl_->close_peer(id, true); impl_->peers.erase(id);
}
bool UdpConnection::alive(Id id) const {
    std::lock_guard lock(impl_->mutex); auto it = impl_->peers.find(id);
    return it != impl_->peers.end() && !it->second.closed;
}
bool UdpConnection::connected(Id id) const {
    std::lock_guard lock(impl_->mutex); auto it = impl_->peers.find(id);
    return it != impl_->peers.end() && it->second.ready && !it->second.closed;
}
bool UdpConnection::drained(Id id) const { std::lock_guard lock(impl_->mutex); return impl_->reliable.drained(id); }
bool UdpConnection::send(Id id, const char* bytes, size_t size) {
    std::lock_guard lock(impl_->mutex);
    return impl_->reliable.send(id, {reinterpret_cast<const uint8_t*>(bytes), size});
}
int UdpConnection::receive(Id id, char* bytes, size_t size) {
    std::lock_guard lock(impl_->mutex); auto it = impl_->peers.find(id);
    if (it == impl_->peers.end()) return 0;
    auto& peer = it->second;
    if (peer.rx.empty()) return peer.closed ? 0 : -1;
    size_t count = std::min(size, peer.rx.size());
    std::memcpy(bytes, peer.rx.data(), count); peer.rx.erase(0, count); return static_cast<int>(count);
}
bool UdpConnection::send_realtime(Address address, std::span<const uint8_t> bytes) {
    std::lock_guard lock(impl_->mutex);
    sockaddr_in endpoint{}; endpoint.sin_addr.s_addr = address.ipv4; endpoint.sin_port = htons(address.port);
    Id id = invalid;
    for (const auto& [candidate, peer] : impl_->peers) {
        const auto& destination = peer.realtimeAddress.sin_port ? peer.realtimeAddress : peer.address;
        if (!peer.closed && same(endpoint, destination)) { id = candidate; break; }
    }
    if (id == invalid || !impl_->peers.at(id).ready) return false;
    const bool accepted = impl_->budget.send(id, bytes);
    // Flush available tokens now, rather than adding up to one worker period
    // to each visual hop. Both traffic classes still use the same scheduler.
    impl_->budget.update(clock_ms());
    return accepted;
}
bool UdpConnection::bind_realtime(Id id, Address address) {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->peers.find(id);
    if (it == impl_->peers.end() || it->second.closed || !it->second.ready || !address.port) return false;
    it->second.realtimeAddress.sin_family = AF_INET;
    it->second.realtimeAddress.sin_addr.s_addr = address.ipv4;
    it->second.realtimeAddress.sin_port = htons(address.port);
    return true;
}
int UdpConnection::receive_realtime(Address& address, std::span<uint8_t> bytes) {
    std::unique_lock lock(impl_->mutex);
    if (impl_->realtime.empty()) {
        // A datagram can arrive after the caller's initial poll, before its
        // pose read. Do not defer that ready datagram to the next game frame
        // merely because the worker has not been scheduled yet. Match the
        // old recvfrom-at-consumption behavior; poll is nonblocking and still
        // owns admission, demultiplexing and the shared outbound budget.
        lock.unlock();
        poll();
        lock.lock();
        if (impl_->realtime.empty()) return -1;
    }
    auto packet = std::move(impl_->realtime.front()); impl_->realtime.pop_front();
    address = packet.address;
    size_t size = std::min(bytes.size(), packet.bytes.size());
    std::copy_n(packet.bytes.begin(), size, bytes.begin()); return static_cast<int>(size);
}
void UdpConnection::poll() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->socket == badSocket) return;
    uint32_t now = clock_ms();
    std::array<uint8_t, 2049> bytes{};
    bool received = false;
    for (size_t n = 0; n < 512; ++n) {
        sockaddr_in from{};
#if defined(_WIN32)
        int length = sizeof(from);
#else
        socklen_t length = sizeof(from);
#endif
        int size = recvfrom(impl_->socket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &length);
        if (size < 0) break;
        received = true;
        auto packet = std::span<const uint8_t>(bytes).first(static_cast<size_t>(size));
        if (packet.size() >= 4 && std::memcmp(packet.data(), "DUC1", 4) == 0) {
            impl_->receive_control(from, packet, now); continue;
        }
        // Admission for existing visual registration/rebinding remains in the
        // game/relay token validator. This bounded queue allocates no peer state.
        if (datagram_kind(packet) == DatagramKind::Realtime) {
            if (impl_->realtime.size() < 512)
                impl_->realtime.push_back({{from.sin_addr.s_addr, ntohs(from.sin_port)}, {packet.begin(), packet.end()}});
            continue;
        }
        Id id = impl_->find(from);
        if (id == invalid || !impl_->peers.at(id).ready) continue;
        bool accepted = dispatch_datagram(id, packet, impl_->reliable, [&](auto, auto data) {
            if (impl_->realtime.size() >= 512) return false;
            impl_->realtime.push_back({{from.sin_addr.s_addr, ntohs(from.sin_port)}, {data.begin(), data.end()}});
            return true;
        });
        if (accepted) impl_->peers.at(id).lastReceive = now;
    }
    impl_->reliable.update(now);
    ReliableUdp::Message message;
    while (impl_->reliable.pop(message)) {
        auto it = impl_->peers.find(static_cast<Id>(message.peer));
        if (it == impl_->peers.end() || it->second.closed) continue;
        if (it->second.rx.size() + message.bytes.size() > ReliableUdp::maxBufferedBytes)
            impl_->close_peer(it->first, true);
        else it->second.rx.append(reinterpret_cast<const char*>(message.bytes.data()), message.bytes.size());
    }
    for (auto it = impl_->peers.begin(); it != impl_->peers.end();) {
        Id id = it->first; auto& peer = it->second;
        if (!peer.closed && ((peer.ready && (impl_->reliable.failed(id) || uint32_t(now - peer.lastReceive) >= 15000)) ||
            (!peer.ready && uint32_t(now - peer.started) >= 10000))) impl_->close_peer(id, true);
        if (peer.closed && impl_->server && !peer.accepted) { it = impl_->peers.erase(it); continue; }
        if (!peer.closed && uint32_t(now - peer.lastControl) >= (peer.ready ? 1000U : 500U))
            impl_->control(id, peer.ready ? 5 : (impl_->server ? 2 : (peer.session ? 3 : 1)));
        ++it;
    }
    impl_->budget.update(now);
    if (received) {
        ++impl_->activityGeneration;
        impl_->activity.notify_all();
    }
}
void UdpConnection::wait_for_activity(uint32_t timeoutMs) {
    std::unique_lock lock(impl_->mutex);
    const uint64_t generation = impl_->activityGeneration;
    impl_->activity.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        if (impl_->socket == badSocket || impl_->activityGeneration != generation ||
            !impl_->realtime.empty() || !impl_->accepted.empty()) return true;
        return std::any_of(impl_->peers.begin(), impl_->peers.end(),
            [](const auto& entry) { return !entry.second.rx.empty(); });
    });
}
}
