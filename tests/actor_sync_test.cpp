#include "dusklight_online/game/actor_sync.hpp"
#include "dusklight_online/game/actor_sync_session.hpp"
#include "dusklight_online/game/actor_sync_registry.hpp"
#include "dusklight_online/game/ganondorf_ownership.hpp"
#include <cstdlib>
#include <iostream>

using namespace dusklight_online::game;
void check(bool result) {
    if (!result) { std::cerr << "Actor sync invariant failed\n"; std::exit(1); }
}

struct TestSnapshot {
    bool valid = false;
    uint32_t epoch = 0, sequence = 0, ageTicks = 0;
    int value = 0;
    bool locked = false;
};

// A second, engine-free actor proves the framework does not depend on Ganon,
// Link, process addresses, or the real network transport.
struct TestActor {
    actor_sync::Session<TestSnapshot> session;
    std::vector<nlohmann::json> outgoing;
    int value = 7, restored = 0, received = 0;
    bool locked = false, allowRestore = true;

    TestActor(std::string id, bool host) {
        session.id = std::move(id);
        session.stage = "test-stage";
        session.status.mode = host ? dusklight_online::net::Mode::DirectHost :
                                     dusklight_online::net::Mode::DirectJoin;
        session.status.welcomed = true;
        session.status.settings.syncWorld = true;
        session.localPeerId = host ? "direct" : "guest";
        session.localReady = session.featureReady = true;
        session.readyPeers.insert(host ? "guest" : "direct");
        session.send = [this](const nlohmann::json& packet) { outgoing.push_back(packet); return true; };
        session.parse = [](const nlohmann::json& packet, TestSnapshot& result) {
            const auto& state = packet.at("state");
            if (!state.at("value").is_number_integer() || !state.at("locked").is_boolean()) return false;
            result = {true, packet.at("encounter_epoch").get<uint32_t>(),
                      packet.at("sequence").get<uint32_t>(), 0,
                      state.at("value").get<int>(), state.at("locked").get<bool>()};
            return true;
        };
        session.transferLocked = [](const TestSnapshot& state) { return state.locked; };
        session.capture = [this] { return nlohmann::json{{"value", value}, {"locked", locked}}; };
        session.canSimulate = [this] { return session.authority.simulates(session.localPeerId); };
        session.canRelease = [this] { return session.canSimulate() && !locked; };
        session.onGranted = [this] {
            if (session.authority.takeoverPending && allowRestore) {
                value = session.snapshot.value;
                ++restored;
                session.authority.takeoverPending = false;
            }
        };
        session.onSent = [] {};
        session.onSnapshot = [this](std::string_view, const TestSnapshot&) { ++received; };
    }
    ApplyResult receive(std::string peer, const nlohmann::json& packet) {
        RoutedMessage message;
        message.peerId = std::move(peer);
        message.payload = packet;
        return session.consume(message);
    }
};

void test_sessions() {
    TestActor host("stage:actor:1", true), guest("stage:actor:1", false);
    TestActor other("stage:actor:2", false);
    host.session.choose_owner("direct", "test");
    const auto initial = host.outgoing.back();
    check(guest.receive("not-the-host", initial) == ApplyResult::Rejected);
    check(other.receive("direct", initial) == ApplyResult::Rejected);
    check(guest.receive("direct", initial) == ApplyResult::Applied);
    check(host.session.canSimulate() && !guest.session.canSimulate());
    host.session.tick(); host.session.tick();
    const auto state = host.outgoing.back();
    check(state.at("type") == "actor_state");
    check(guest.receive("forged", state) == ApplyResult::Rejected);
    auto malformed = state;
    malformed["state"]["value"] = "wrong-type";
    check(guest.receive("direct", malformed) == ApplyResult::Rejected);
    check(guest.receive("direct", state) == ApplyResult::Applied);
    check(guest.received == 1);
    check(guest.receive("direct", state) == ApplyResult::Rejected);

    host.session.targetRemote = true;
    host.session.targetPeerId = "guest";
    host.locked = true;
    host.session.request_handoff();
    check(host.session.canSimulate());
    check(host.session.authority.epoch == 1);
    host.locked = false;
    host.session.request_handoff();
    const auto grant = host.outgoing.back();
    // Five 30 Hz updates are over 100 ms. Neither side simulates while the
    // grant is in transit, and the new owner must restore before resuming.
    for (int tick = 0; tick < 5; ++tick) {
        host.session.tick(); guest.session.tick();
        check(!host.session.canSimulate() && !guest.session.canSimulate());
    }
    guest.allowRestore = false;
    check(guest.receive("direct", grant) == ApplyResult::Applied);
    check(!guest.session.canSimulate());
    guest.allowRestore = true;
    guest.session.onGranted();
    check(guest.session.canSimulate() && guest.restored == 1 && guest.value == 7);
    check(guest.receive("direct", grant) == ApplyResult::Applied);
    check(guest.restored == 1);
    check(guest.receive("direct", initial) == ApplyResult::Rejected);
    check(guest.receive("direct", state) == ApplyResult::Rejected);

    guest.session.targetRemote = true;
    guest.session.targetPeerId = "direct";
    guest.value = 42;
    guest.session.request_handoff();
    check(!guest.session.canSimulate() && !host.session.canSimulate());
    const auto release = guest.outgoing.back();
    check(host.receive("forged", release) == ApplyResult::Rejected);
    check(host.receive("guest", release) == ApplyResult::Applied);
    check(host.session.canSimulate() && host.restored == 1 && host.value == 42);
    check(guest.receive("direct", host.outgoing.back()) == ApplyResult::Applied);
    check(!guest.session.canSimulate());
    check(other.session.authority.epoch == 0); // Completely separate instance.

    host.session.abort_owner("test_disconnect");
    const auto stop = host.outgoing.back();
    check(guest.receive("direct", stop) == ApplyResult::Applied);
    check(!host.session.canSimulate() && !guest.session.canSimulate());
    host.session.featureReady = false;
    check(host.receive("guest", release) == ApplyResult::IgnoredByPolicy);
}

int main() {
    test_sessions();
    for (int demo = 60; demo <= 65; ++demo) {
        GanondorfEndingSequence ending;
        check(!ending.observe(22, demo, false)); // Other rooms/stages stay alone.
        check(!ending.observe(21, demo, true)); // Downed is not finished.
        check(!ending.observe(22, 50, true)); // Duel is not the ending.
        check(ending.observe(22, demo, true)); // Also handles a delayed first packet.
        for (int pausedTicks = 0; pausedTicks < 300; ++pausedTicks) {
            check(!ending.start(false));
            check(ending.pending && !ending.active);
            check(!ending.observe(22, demo, true));
        }
        check(ending.start(true));
        check(!ending.pending && ending.active);
        for (int laterDemo = 60; laterDemo <= 65; ++laterDemo) {
            check(!ending.observe(22, laterDemo, true));
            check(!ending.start(true)); // Never rewind local camera/animation.
        }
        ending = {}; // Leaving the area discards pending/active presentation.
        check(!ending.start(true));
        check(ending.observe(22, demo, true));
    }
    check(!ganondorf_ending_demo(59));
    check(!ganondorf_ending_demo(66));
    check(!ganondorf_ending_demo(95));
    check(ganondorf_local_entry_pending(2, 0));
    check(ganondorf_local_entry_pending(1, 0));
    check(ganondorf_local_entry_pending(0, 95));
    check(ganondorf_local_entry_pending(0, 96));
    check(!ganondorf_local_entry_pending(0, 0));
    check(ganondorf_local_entry_demo(95));
    check(ganondorf_local_entry_demo(96));
    // Do not authorize another client's native duel or ending-blow camera.
    check(!ganondorf_local_entry_demo(50));
    check(!ganondorf_local_entry_demo(60));
    check(!ganondorf_local_entry_demo(90));
    actor_sync::Registry registry;
    int firstCalls = 0, secondCalls = 0;
    bool firstActive = false;
    const actor_sync::Adapter adapter{
        +[](const dusklight_online::net::Status&, bool, bool, const actor_sync::PeerPoses&) {},
        [&](const RoutedMessage&) { ++firstCalls; return ApplyResult::Applied; },
        +[](std::string_view) {}, +[]() {}, [&] { return firstActive; }
    };
    check(registry.add("stage:actor:1", adapter));
    check(!registry.add("stage:actor:1", adapter));
    auto secondAdapter = adapter;
    secondAdapter.consume = [&](const RoutedMessage&) { ++secondCalls; return ApplyResult::Applied; };
    secondAdapter.active = [] { return false; };
    check(registry.add("stage:actor:2", secondAdapter));
    check(!registry.active());
    firstActive = true;
    check(registry.active());
    RoutedMessage routed;
    routed.payload = {{"sync_id", "stage:actor:1"}};
    check(registry.consume(routed) == ApplyResult::IgnoredByPolicy);
    dusklight_online::net::Status worldStatus;
    worldStatus.welcomed = true;
    worldStatus.settings.syncWorld = true;
    registry.update(worldStatus, true, {});
    routed.ingress.welcomed = true;
    routed.ingress.settings.syncWorld = true;
    check(registry.consume(routed) == ApplyResult::Applied);
    check(firstCalls == 1 && secondCalls == 0);
    registry.remove("stage:actor:1");
    check(!registry.active());
    check(registry.consume(routed) == ApplyResult::IgnoredByPolicy);
    routed.payload["sync_id"] = "stage:actor:2";
    check(registry.consume(routed) == ApplyResult::Applied);
    check(firstCalls == 1 && secondCalls == 1);
    routed.payload["sync_id"] = "stage:ooccoo:1";
    check(registry.consume(routed) == ApplyResult::IgnoredByPolicy);
    routed.payload["sync_id"] = 2;
    check(registry.consume(routed) == ApplyResult::Rejected);
    worldStatus.settings.syncWorld = false;
    registry.update(worldStatus, true, {});
    routed.payload["sync_id"] = "stage:actor:2";
    check(registry.consume(routed) == ApplyResult::IgnoredByPolicy);
    registry.reset();
    check(!registry.active());
    actor_sync::Authority a, b;
    check(a.accept_grant(1, "A", "A", false));
    check(b.accept_grant(1, "A", "B", false));
    check(a.simulates("A") && !b.simulates("B"));
    check(!a.begin_release("A", true));
    check(a.begin_release("A", false));
    for (int delayedTicks = 0; delayedTicks < 120; ++delayedTicks)
        check(!a.simulates("A") && !b.simulates("B"));
    check(!actor_sync::valid_release(true, "B", "A", 1, 1));
    check(!actor_sync::valid_release(true, "A", "A", 2, 1));
    check(actor_sync::valid_release(true, "A", "A", 1, 1));
    check(b.accept_grant(2, "B", "B", true));
    check(!b.simulates("B")); // Must restore before native simulation.
    b.takeoverPending = false;
    check(b.simulates("B") && !a.simulates("A"));
    check(a.accept_grant(2, "B", "A", true));
    check(!a.simulates("A"));
    check(b.accept_grant(2, "B", "B", true));
    check(!b.takeoverPending); // Duplicate cannot rewind the actor.
    check(!b.accept_grant(1, "A", "B", true));
    check(!b.accept_grant(2, "A", "B", true));
    check(!actor_sync::valid_snapshot("A", "B", 2, 2, 10, 2));
    check(!actor_sync::valid_snapshot("B", "B", 1, 2, 10, 2));
    check(!actor_sync::valid_snapshot("B", "B", 2, 2, 2, 2));
    check(actor_sync::valid_snapshot("B", "B", 2, 2, 3, 2));
    b.nextEpoch = UINT32_MAX;
    check(b.next_grant_epoch() == 0);
    check(ganondorf_ownership_locked(19, 0, 0));
    check(ganondorf_ownership_locked(20, 0, 0));
    check(ganondorf_ownership_locked(22, 0, 0));
    check(ganondorf_ownership_locked(10, 5, 0));
    check(ganondorf_ownership_locked(11, 0, 50));
    check(!ganondorf_ownership_locked(21, 2, 0));
    std::cout << "Actor sync invariants passed\n";
}
