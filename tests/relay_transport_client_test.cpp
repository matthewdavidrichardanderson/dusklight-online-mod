#include "dusklight_online/net/transport.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <algorithm>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

using dusklight_online::net::RelayConfig;
using dusklight_online::net::Transport;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "relay transport client test failed: " << message << '\n';
    std::exit(1);
}

bool wait_until(Transport& first, Transport& second, const auto& predicate,
                int timeoutMilliseconds = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        first.tick();
        second.tick();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool consume_type(Transport& transport, const std::string& type,
                  nlohmann::json* message = nullptr) {
    bool found = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.message.is_object() && event.message.value("type", "") == type) {
            found = true;
            if (message != nullptr) {
                *message = std::move(event.message);
            }
        }
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Model a game process with a fine timer cadence; relay timing is separate.
    timeBeginPeriod(1);
#endif
    if (argc != 2) {
        fail("expected relay TCP port argument");
    }
    const int parsedPort = std::stoi(argv[1]);
    if (parsedPort < 1 || parsedPort > 65535) {
        fail("invalid relay port argument");
    }

    Transport owner;
    Transport joiner;
    RelayConfig ownerConfig;
    ownerConfig.name = "Owner";
    ownerConfig.room = "transport-client-test";
    ownerConfig.password = "testing-only";
    ownerConfig.host = "127.0.0.1";
    ownerConfig.port = static_cast<uint16_t>(parsedPort);
    ownerConfig.createRoom = true;
    ownerConfig.settings.syncWorld = true;
    ownerConfig.settings.pvp = true;

    std::string error;
    if (!owner.start_relay(ownerConfig, &error)) {
        fail("owner start: " + error);
    }
    if (!wait_until(owner, joiner, [&] { return owner.status().welcomed; })) {
        fail("owner welcome timeout: " + owner.status().error);
    }

    RelayConfig joinConfig = ownerConfig;
    joinConfig.name = "Joiner";
    joinConfig.createRoom = false;
    if (!joiner.start_relay(joinConfig, &error)) {
        fail("joiner start: " + error);
    }
    if (!wait_until(owner, joiner, [&] {
            return joiner.status().welcomed && owner.peers().size() == 1 &&
                   owner.status().udpReady && joiner.status().udpReady;
        })) {
        fail("joiner/UDP welcome timeout: " + joiner.status().error);
    }
    if (!owner.status().isOwner || joiner.status().isOwner ||
        !joiner.status().settings.syncWorld || !joiner.status().settings.pvp ||
        !owner.status().semanticVisualsReady || !joiner.status().semanticVisualsReady ||
        !owner.status().snapshotDeltasReady || !joiner.status().snapshotDeltasReady) {
        fail("relay owner/settings state was not normalized from welcome");
    }

    while (owner.has_events()) owner.pop_event();
    while (joiner.has_events()) joiner.pop_event();

    if (!owner.send_visual({{"type", "pose"}, {"sequence", 5},
                            {"state", {{"stage", "F_SP103"}, {"x", 12.25f}}}})) {
        fail("relay UDP pose send failed");
    }
    nlohmann::json udpPose;
    if (!wait_until(owner, joiner, [&] {
            return consume_type(joiner, "pose", &udpPose);
        }) || udpPose.value("client_id", "") != owner.status().clientId ||
        udpPose.value("sequence", 0U) != 5) {
        fail("relay did not route the mod client's UDP pose");
    }

    // Exercise the game cadence on independently timed clients. Fast polling
    // hides repeated/skipped poses when the carrier batches across frame edges.
    // Padding models roughly one full semantic pose datagram without a game.
    std::string padding; uint32_t random=12345;
    for(int n=0;n<1050;++n) {random^=random<<13;random^=random>>17;random^=random<<5;padding.push_back(char(33+random%90));}
    using Clock=std::chrono::steady_clock;
    uint32_t base=100;
    for (int phase : {0,2,4,8,16,25}) {
        auto begin=Clock::now()+std::chrono::milliseconds(100);
        struct Stats { int holds=0, skips=0, received=0, errors=0; uint32_t last=0; double maxGap=0; std::vector<double> age; } stats[2];
        auto run=[&](Transport& t,int side) {
            auto next=begin+std::chrono::milliseconds(side?phase:0);
            auto previous=next;
            for(int tick=0; tick<120; ++tick) {
                std::this_thread::sleep_until(next);
                auto now=Clock::now();
                stats[side].maxGap=std::max(stats[side].maxGap,std::chrono::duration<double,std::milli>(now-previous).count()); previous=now;
                t.tick(); bool got=false;
                while(t.has_events()) {
                    auto e=t.pop_event();
                    if(e.kind==dusklight_online::net::EventKind::Error) ++stats[side].errors;
                    if(!e.message.is_object() || !e.message.contains("sent_ms")) continue;
                    auto seq=e.message.value("sequence",0u);
                    if(tick>=10) {
                        got=true; ++stats[side].received;
                        if(stats[side].last && seq>stats[side].last) stats[side].skips+=seq-stats[side].last-1;
                        stats[side].age.push_back(std::chrono::duration<double,std::milli>(Clock::now()-begin).count()-e.message["sent_ms"].get<double>());
                    }
                    stats[side].last=seq;
                }
                if(tick>=10 && !got) ++stats[side].holds;
                double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
                if (!t.send_visual({{"type","pose"},{"sequence",base+tick},{"sent_ms",ms},{"padding",padding},{"state",{{"stage","F_SP103"},{"x",ms}}}})) fail("cadence pose send failed");
                next+=std::chrono::nanoseconds(33333333);
            }
        };
        std::thread a(run,std::ref(owner),0), b(run,std::ref(joiner),1);a.join();b.join();
        for(int side=0;side<2;++side) {
            auto& s=stats[side];std::sort(s.age.begin(),s.age.end());
            std::cout<<"phase="<<phase<<" side="<<side<<" received="<<s.received<<" holds="<<s.holds<<" skips="<<s.skips<<" errors="<<s.errors<<" max_tick_ms="<<s.maxGap<<" p95_age="<<(s.age.empty()?0:s.age[s.age.size()*95/100])<<std::endl;
        }
        // At phase zero either client's send can straddle the other's update
        // even on the old transport. Report it, but check separated phases.
        for (int side=0;side<2;++side) {
            if (stats[side].errors || stats[side].received == 0)
                fail("cadence transport error or no poses");
            if (phase != 0 && stats[side].holds > 11)
                fail("more than 10 percent of cadence updates lacked a new pose");
            std::cout << "cadence wire_bytes=" << (side ? joiner : owner).last_visual_send_stats().wireBytes << '\n';
        }
        base+=200;
        while(owner.has_events()) owner.pop_event();while(joiner.has_events()) joiner.pop_event();
    }

    dusklight_online::net::udp::RemoteObjectPacket object;
    object.sequence = 6;
    object.objectId = 611;
    object.objectKind = 1;
    object.flags = dusklight_online::net::udp::ObjectActive;
    if (!owner.send_remote_object(object)) {
        fail("relay UDP object send failed");
    }
    bool receivedObject = false;
    if (!wait_until(owner, joiner, [&] {
            while (joiner.has_events()) {
                auto event = joiner.pop_event();
                receivedObject |= event.kind == dusklight_online::net::EventKind::UdpRemoteObject &&
                                  event.remoteObject.objectId == 611;
            }
            return receivedObject;
        })) {
        fail("relay did not route the mod client's UDP remote object");
    }

    // This optional MFB/randomizer lane must remain wire-compatible even when
    // its game adapter is absent from a particular Dusklight build.
    if (!owner.send({{"type", "rando_item_get"}, {"item", 0x48}})) {
        fail("rando_item_get send failed");
    }
    nlohmann::json received;
    if (!wait_until(owner, joiner, [&] {
            return consume_type(joiner, "rando_item_get", &received);
        }) || received.value("item", -1) != 0x48 ||
        received.value("client_id", "").empty()) {
        fail("rando_item_get was not relay-routed with authenticated identity");
    }

    if (!joiner.send({{"type", "sync_request"},
                      {"target_client_id", owner.status().clientId},
                      {"flags_only", true}})) {
        fail("targeted sync_request send failed");
    }
    if (!wait_until(owner, joiner, [&] {
            return consume_type(owner, "sync_request", &received);
        }) || received.value("client_id", "") != joiner.status().clientId) {
        fail("targeted sync_request did not reach the owner");
    }

    auto changed = owner.status().settings;
    changed.remoteCollision = false;
    changed.pvp = true;
    if (!owner.publish_room_settings(changed)) {
        fail("owner room_settings send failed");
    }
    if (!wait_until(owner, joiner, [&] {
            return !joiner.status().settings.remoteCollision;
        }) || joiner.status().settings.pvp) {
        fail("relay did not normalize and publish room settings");
    }

    owner.disconnect();
    if (!wait_until(owner, joiner, [&] { return joiner.status().isOwner; })) {
        fail("relay ownership did not transfer after owner disconnect");
    }
    bool sawOwnerChanged = false;
    while (joiner.has_events()) {
        const auto event = joiner.pop_event();
        if (event.kind == dusklight_online::net::EventKind::Message &&
            event.message.value("type", std::string()) == "owner_changed" &&
            event.message.value("owner_client_id", std::string()) ==
                joiner.status().clientId) {
            sawOwnerChanged = true;
        }
    }
    if (!sawOwnerChanged) {
        fail("relay ownership transfer did not deliver owner_changed");
    }

    joiner.disconnect();
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
    std::cout << "relay transport client test passed\n";
    return 0;
}
