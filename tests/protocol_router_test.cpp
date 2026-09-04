#include "dusklight_online/game/protocol_router.hpp"

#include <cassert>
#include <array>
#include <string>
#include <vector>

using dusklight_online::game::ApplyResult;
using dusklight_online::game::MessageConsumer;
using dusklight_online::game::MessageDomain;
using dusklight_online::game::ProtocolRouter;
using dusklight_online::game::RoutedMessage;
using dusklight_online::net::Event;
using dusklight_online::net::EventKind;

namespace {

class Consumer final : public MessageConsumer {
public:
    bool ready = false;
    std::vector<std::string> types;

    bool stage_ready() const override { return ready; }
    ApplyResult consume(const RoutedMessage& message) override {
        types.push_back(message.payload.at("type").get<std::string>());
        return ApplyResult::Applied;
    }
    ApplyResult consume_udp(const Event&) override { return ApplyResult::Applied; }
    void peer_joined(std::string_view, std::string_view) override {}
    void peer_left(std::string_view) override {}
};

Event message(std::string type, bool syncFlags = true) {
    Event event;
    event.kind = EventKind::Message;
    event.peerId = "peer";
    event.message = {{"type", std::move(type)}};
    event.ingress.settings.syncFlags = syncFlags;
    return event;
}

}  // namespace

int main() {
    using Domain = MessageDomain;
    struct Expected { const char* type; Domain domain; bool stageDependent; bool syncFlagGated; };
    constexpr Expected expected[] = {
        Expected{"welcome",Domain::Session,false,false}, {"error",Domain::Session,false,false},
        {"ping",Domain::Session,false,false}, {"pong",Domain::Session,false,false},
        {"ack",Domain::Session,false,false}, {"udp_ready",Domain::Session,false,false},
        {"peer_joined",Domain::Membership,false,false}, {"peer_left",Domain::Membership,false,false},
        {"owner_changed",Domain::Membership,false,false}, {"room_settings",Domain::Membership,false,false},
        {"name_labels",Domain::Membership,false,false}, {"dummy_model",Domain::Membership,false,false},
        {"sync_flags",Domain::Membership,false,false}, {"sync_world",Domain::Membership,false,false},
        {"remote_collision",Domain::Membership,false,false}, {"pvp_enabled",Domain::Membership,false,false},
        {"presence",Domain::Presence,false,false}, {"progression_state",Domain::Presence,false,true},
        {"puppet_preference",Domain::Presence,false,false}, {"midna_preference",Domain::Presence,false,false},
        {"sync_request",Domain::Progression,false,false}, {"save_snapshot",Domain::Progression,true,true},
        {"event_bit",Domain::Progression,false,true}, {"tbox_bit",Domain::Progression,true,true},
        {"switch_bit",Domain::Progression,true,true}, {"room_switch_bit",Domain::Progression,true,true},
        {"item_bit",Domain::Progression,true,true}, {"dungeon_item_bit",Domain::Progression,true,true},
        {"item_get",Domain::Progression,false,true}, {"item_first_bit",Domain::Progression,false,true},
        {"collect_crystal",Domain::Progression,false,true}, {"collect_mirror",Domain::Progression,false,true},
        {"dark_clear_lv",Domain::Progression,true,true}, {"transform_lv",Domain::Progression,false,true},
        {"region_bit",Domain::Progression,false,true}, {"collect",Domain::Progression,false,true},
        {"visited_room",Domain::Progression,true,true}, {"letter_get",Domain::Progression,false,true},
        {"ooccoo_state",Domain::Progression,false,true}, {"collect_smell",Domain::Progression,false,true},
        {"key_num",Domain::Progression,true,true}, {"light_drop_num",Domain::Progression,false,true},
        {"light_drop_get_flag",Domain::Progression,false,true}, {"max_life_update",Domain::Progression,false,true},
        {"bottle_slots",Domain::Progression,false,true}, {"bomb_bag_slot",Domain::Progression,false,true},
        {"rupee_count",Domain::Progression,false,true}, {"rupee_delta",Domain::Progression,false,true},
        {"poe_count",Domain::Progression,false,true},
        {"malo_fundraising",Domain::Progression,false,true}, {"charlo_offering",Domain::Progression,false,true},
        {"fish_record",Domain::Progression,false,true},
        {"rando_item_get",Domain::OptionalRandomizer,false,true},
        {"pose",Domain::Visual,false,false}, {"midna_pose",Domain::Visual,false,false},
        {"pvp_hit",Domain::Interaction,false,false},
        {"actor_owner_claim",Domain::ActorSync,false,false}, {"actor_owner",Domain::ActorSync,false,false},
        {"ganondorf_hit",Domain::ActorSync,false,false}, {"ganondorf_reaction",Domain::ActorSync,false,false},
        {"ganondorf_player_damage",Domain::ActorSync,false,false}, {"actor_state",Domain::ActorSync,false,false},
    };
    for (const auto& item : expected) {
        const auto spec = ProtocolRouter::classify(item.type);
        assert(spec.domain == item.domain);
        assert(spec.stageDependent == item.stageDependent);
        assert(spec.syncFlagsControlled == item.syncFlagGated);
        assert(ProtocolRouter::is_known_type(item.type));
    }
    assert(ProtocolRouter::classify("save_snapshot").domain == MessageDomain::Progression);
    assert(ProtocolRouter::classify("save_snapshot").stageDependent);
    assert(ProtocolRouter::classify("rando_item_get").domain ==
           MessageDomain::OptionalRandomizer);
    assert(!ProtocolRouter::classify("rando_item_get").stageDependent);
    assert(ProtocolRouter::classify("actor_state").domain == MessageDomain::ActorSync);
    assert(!ProtocolRouter::is_known_type("future_unreviewed_lane"));

    Consumer consumer;
    ProtocolRouter router(consumer);
    assert(router.route(message("save_snapshot"), true) == ApplyResult::Deferred);
    assert(router.stats().pendingMessages == 1);
    consumer.ready = true;
    router.flush(true);
    assert(router.stats().pendingMessages == 0);
    assert(consumer.types.size() == 1 && consumer.types.front() == "save_snapshot");

    // Match the AIO: a resolved randomizer reward is never queued behind its
    // non-stage-dependent absolute companion counter.
    consumer.ready = false;
    assert(router.route(message("rando_item_get"), true) == ApplyResult::Applied);
    assert(router.stats().pendingMessages == 0);
    assert(consumer.types.size() == 2 && consumer.types.back() == "rando_item_get");

    assert(router.route(message("event_bit", false), false) == ApplyResult::IgnoredByPolicy);
    assert(consumer.types.size() == 2);
    assert(router.route(message("future_unreviewed_lane"), true) == ApplyResult::Unsupported);

    Event malformed;
    malformed.kind = EventKind::Message;
    malformed.message = nullptr;
    assert(router.route(malformed, true) == ApplyResult::Rejected);
    return 0;
}
