#include "dusklight_online/net/sdk_room_channel.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

using namespace dusklight_online::net;
using json = nlohmann::json;

namespace {

json wait_for(RoomChannel& channel, const std::string& type) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        RoomChannel::Event event{};
        while (channel.poll(event)) {
            if (event.kind == RoomChannel::EventKind::Closed)
                throw std::runtime_error(event.text);
            if (event.kind != RoomChannel::EventKind::Message) {
                if (type == "open" && event.kind == RoomChannel::EventKind::Open) return {};
                continue;
            }
            const json value = json::parse(event.text);
            if (value.value("type", "") == "error")
                throw std::runtime_error(value.value("error", "room error"));
            if (value.value("type", "") == type) return value;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Timed out waiting for " + type);
}

json hello(const std::string& action, const std::string& room, const std::string& name) {
    json result = {
        {"type", "hello"}, {"protocol_version", 3}, {"action", action},
        {"room_id", room}, {"password", "channel-test-password"},
        {"name", name}, {"want_puppet", false},
    };
    if (action == "create") {
        result["settings"] = {
            {"dummy_model", true}, {"sync_flags", true}, {"sync_world", false},
            {"remote_collision", true}, {"pvp", false},
        };
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: room_channel_live_test <wss://room-service/room/test-name>\n";
        return 2;
    }
    try {
        auto host = make_sdk_room_channel();
        auto guest = make_sdk_room_channel();
        std::string error;
        if (!host->open(argv[1], error)) throw std::runtime_error(error);
        wait_for(*host, "open");
        const std::string room = "channel-test";
        if (!host->send(hello("create", room, "Test Host").dump()))
            throw std::runtime_error("Could not send host hello");
        const auto hostWelcome = wait_for(*host, "welcome");

        if (!guest->open(argv[1], error)) throw std::runtime_error(error);
        wait_for(*guest, "open");
        if (!guest->send(hello("join", room, "Test Guest").dump()))
            throw std::runtime_error("Could not send guest hello");
        const auto guestWelcome = wait_for(*guest, "welcome");
        const auto joined = wait_for(*host, "peer_joined");
        if (joined.value("client_id", "") != guestWelcome.value("client_id", ""))
            throw std::runtime_error("Unexpected joined peer");

        constexpr int kSignals = 12;
        for (int i = 0; i < kSignals; ++i) {
            const json fromHost = {
                {"type", "ice_signal"}, {"target_client_id", guestWelcome.at("client_id")},
                {"kind", 2}, {"generation", 0}, {"data", "host-signal-" + std::to_string(i)},
            };
            const json fromGuest = {
                {"type", "ice_signal"}, {"target_client_id", hostWelcome.at("client_id")},
                {"kind", 2}, {"generation", 0}, {"data", "guest-signal-" + std::to_string(i)},
            };
            if (!host->send(fromHost.dump()) || !guest->send(fromGuest.dump()))
                throw std::runtime_error("Could not queue all signals");
        }

        std::set<std::string> receivedByHost;
        std::set<std::string> receivedByGuest;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline &&
               (receivedByHost.size() < kSignals || receivedByGuest.size() < kSignals)) {
            for (auto* channel : {host.get(), guest.get()}) {
                RoomChannel::Event event{};
                while (channel->poll(event)) {
                    if (event.kind == RoomChannel::EventKind::Closed)
                        throw std::runtime_error(event.text);
                    if (event.kind != RoomChannel::EventKind::Message) continue;
                    const auto value = json::parse(event.text);
                    if (value.value("type", "") == "error")
                        throw std::runtime_error(value.value("error", "room error"));
                    if (value.value("type", "") == "ice_signal") {
                        const auto data = value.value("data", "");
                        if (channel == host.get()) receivedByHost.insert(data);
                        else receivedByGuest.insert(data);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (receivedByHost.size() != kSignals || receivedByGuest.size() != kSignals)
            throw std::runtime_error("Repeated signals did not reach both clients");

        // The connection must remain usable after the initial handshake and
        // its configured connection timeout have both elapsed.
        std::this_thread::sleep_for(std::chrono::seconds(12));
        const json lateSignal = {
            {"type", "ice_signal"}, {"target_client_id", guestWelcome.at("client_id")},
            {"kind", 2}, {"generation", 0}, {"data", "late-signal"},
        };
        if (!host->send(lateSignal.dump()) ||
            wait_for(*guest, "ice_signal").value("data", "") != "late-signal")
            throw std::runtime_error("Connection could not send after idle time");
        host->close();
        guest->close();
        std::cout << "Room channel delivered " << kSignals
                  << " signals in each direction and remained open after idle time\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Room channel test failed: " << exception.what() << '\n';
        return 1;
    }
}
