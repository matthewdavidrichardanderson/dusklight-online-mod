#pragma once
#include "dusklight_online/net/reliable_udp.hpp"
#include <string_view>
#include <string>

namespace dusklight_online::net {
// Native carrier/session adapter. Game transport depends on this connection
// facade; ReliableUdp below it remains unaware of sockets and handshakes.
class UdpConnection {
public:
    using Id = uintptr_t;
    static constexpr Id invalid = static_cast<Id>(-1);
    struct Address { uint32_t ipv4 = 0; uint16_t port = 0; };
    UdpConnection();
    ~UdpConnection();
    bool open(std::string_view host, uint16_t port, size_t capacity, bool server);
    void close();
    Id connect(std::string_view host, uint16_t port);
    Id accept(Address& address);
    void disconnect(Id peer);
    bool connected(Id peer) const;
    bool alive(Id peer) const;
    bool drained(Id peer) const;
    bool send(Id peer, const char* data, size_t size);
    // -1 idle, 0 closed, positive bytes. Framing stays in the application.
    int receive(Id peer, char* data, size_t size);
    bool send_realtime(Address address, std::span<const uint8_t> bytes);
    // Called only AFTER the existing relay token/sender validation succeeds.
    // Preserves authenticated visual endpoint rebinding without changing the
    // reliable session's address or allocating a second bandwidth allowance.
    bool bind_realtime(Id peer, Address address);
    int receive_realtime(Address& address, std::span<uint8_t> bytes);
    void poll();
    // Relay idle wait: wake for received data without imposing a polling delay.
    // Does not service game callbacks or bypass the shared datagram budget.
    void wait_for_activity(uint32_t timeoutMs);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
