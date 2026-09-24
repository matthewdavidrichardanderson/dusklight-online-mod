#include "dusklight_online/net/sdk_room_channel.hpp"
#include "dusklight_online/net/transport.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace dusklight_online::net;

namespace {

void tick_all(std::vector<std::unique_ptr<Transport>>& players,
              std::vector<std::vector<bool>>* received = nullptr) {
    for (size_t recipient = 0; recipient < players.size(); ++recipient) {
        players[recipient]->tick();
        while (players[recipient]->has_events()) {
            const Event event = players[recipient]->pop_event();
            if (event.kind == EventKind::Disconnected || event.kind == EventKind::Error)
                throw std::runtime_error("player " + std::to_string(recipient) + ": " + event.detail);
            if (event.kind == EventKind::RouteChanged || event.kind == EventKind::Diagnostic)
                std::cout << "player " << recipient << " peer=" << event.peerId
                          << " " << event.detail << '\n';
            if (!received || !event.message.is_object() ||
                event.message.value("type", "") != "chat") continue;
            for (size_t sender = 0; sender < players.size(); ++sender)
                if (event.message.value("text", "") == "live-mesh-" + std::to_string(sender))
                    (*received)[recipient][sender] = true;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: cloud_room_live_mesh_test <https://room-service> <3|4>\n";
        return 2;
    }
    try {
        const int count = std::stoi(argv[2]);
        if (count != 3 && count != 4) throw std::runtime_error("Player count must be 3 or 4");
        const auto nonce = std::chrono::system_clock::now().time_since_epoch().count();
        CloudRoomConfig configuration;
        configuration.serverUrl = argv[1];
        configuration.room = "mesh-test-" + std::to_string(nonce);
        configuration.password = "temporary-" + configuration.room;
        configuration.stunHost = "stun.cloudflare.com";
        configuration.stunPort = 3478;
        std::cout << "Testing " << count << " clients in " << configuration.room << '\n';

        std::vector<std::unique_ptr<Transport>> players;
        for (int index = 0; index < count; ++index) {
            auto player = std::make_unique<Transport>();
            configuration.createRoom = index == 0;
            configuration.name = "Test Player " + std::to_string(index);
            std::string error;
            if (!player->start_cloud_room(configuration, make_sdk_room_channel(), &error))
                throw std::runtime_error("start player " + std::to_string(index) + ": " + error);
            players.push_back(std::move(player));

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
            bool connected = false;
            while (std::chrono::steady_clock::now() < deadline) {
                tick_all(players);
                connected = true;
                for (size_t peer = 0; peer < players.size(); ++peer) {
                    const auto status = players[peer]->status();
                    connected &= status.welcomed && status.natPeerCount == players.size() - 1;
                }
                if (connected) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!connected) {
                for (size_t peer = 0; peer < players.size(); ++peer) {
                    const auto status = players[peer]->status();
                    std::cerr << "player " << peer << " nat=" << status.natPeerCount
                              << " welcomed=" << status.welcomed << " error=" << status.error << '\n';
                }
                throw std::runtime_error("not all peer links became direct");
            }
            std::cout << players.size() << " clients fully connected\n";
        }

        for (size_t sender = 0; sender < players.size(); ++sender)
            if (!players[sender]->send({{"type", "chat"},
                                        {"text", "live-mesh-" + std::to_string(sender)}}))
                throw std::runtime_error("gameplay send failed");
        std::vector<std::vector<bool>> received(players.size(), std::vector<bool>(players.size()));
        const auto deliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool delivered = false;
        while (std::chrono::steady_clock::now() < deliveryDeadline) {
            tick_all(players, &received);
            delivered = true;
            for (size_t recipient = 0; recipient < players.size(); ++recipient)
                for (size_t sender = 0; sender < players.size(); ++sender)
                    if (recipient != sender) delivered &= received[recipient][sender];
            if (delivered) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!delivered) throw std::runtime_error("not all gameplay messages arrived");
        const auto stableUntil = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        auto nextProbe = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < stableUntil) {
            tick_all(players);
            for (size_t index = 0; index < players.size(); ++index) {
                const auto status = players[index]->status();
                if (status.natPeerCount != players.size() - 1 || status.relayPeerCount)
                    throw std::runtime_error("player " + std::to_string(index) +
                                             " lost a direct route after connection");
            }
            if (std::chrono::steady_clock::now() >= nextProbe) {
                for (size_t index = 0; index < players.size(); ++index)
                    if (!players[index]->send({{"type", "chat"}, {"text", "stability probe"}}))
                        throw std::runtime_error("stability probe send failed");
                nextProbe += std::chrono::seconds(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const size_t returning = players.size() - 1;
        players[returning]->disconnect();
        while (players[returning]->has_events()) players[returning]->pop_event();
        const auto departureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool departed = false;
        while (std::chrono::steady_clock::now() < departureDeadline) {
            tick_all(players);
            departed = true;
            for (size_t index = 0; index < returning; ++index)
                departed &= players[index]->peers().size() == returning - 1;
            if (departed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!departed) throw std::runtime_error("existing clients did not see departure");
        configuration.createRoom = false;
        configuration.name = "Returning Player";
        std::string rejoinError;
        if (!players[returning]->start_cloud_room(configuration, make_sdk_room_channel(), &rejoinError))
            throw std::runtime_error("rejoin failed: " + rejoinError);
        const auto rejoinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
        bool rejoined = false;
        while (std::chrono::steady_clock::now() < rejoinDeadline) {
            tick_all(players);
            rejoined = true;
            for (const auto& player : players)
                rejoined &= player->status().welcomed &&
                    player->status().natPeerCount == players.size() - 1;
            if (rejoined) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!rejoined) throw std::runtime_error("rejoined player did not regain every direct route");
        if (!players[returning]->send({{"type", "chat"}, {"text", "live-rejoined"}}))
            throw std::runtime_error("rejoined player could not send gameplay");
        std::vector<bool> receivedRejoin(returning);
        const auto deliveryAfterRejoin = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deliveryAfterRejoin) {
            for (size_t index = 0; index < players.size(); ++index) {
                players[index]->tick();
                while (players[index]->has_events()) {
                    const Event event = players[index]->pop_event();
                    if (event.kind == EventKind::Disconnected || event.kind == EventKind::Error)
                        throw std::runtime_error("post-rejoin error: " + event.detail);
                    if (index < returning && event.message.is_object() &&
                        event.message.value("type", "") == "chat" &&
                        event.message.value("text", "") == "live-rejoined")
                        receivedRejoin[index] = true;
                }
            }
            if (std::all_of(receivedRejoin.begin(), receivedRejoin.end(), [](bool value) { return value; }))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!std::all_of(receivedRejoin.begin(), receivedRejoin.end(), [](bool value) { return value; }))
            throw std::runtime_error("rejoined player's gameplay was not received by every peer");
        for (auto& player : players) player->disconnect();
        std::cout << count << "-client live mesh passed gameplay, route stability and leave/rejoin\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Live cloud mesh failed: " << exception.what() << '\n';
        return 1;
    }
}
