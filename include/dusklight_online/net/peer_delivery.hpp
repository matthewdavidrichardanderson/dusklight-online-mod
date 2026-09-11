#pragma once
#include <string_view>

namespace dusklight_online::net {
// Control/presence remains on the authoritative relay stream. Only gameplay
// bodies are eligible; a peer body can never masquerade as lobby control.
inline bool peer_delivery_type(std::string_view type) {
    constexpr std::string_view types[] = {
        "progression_state","sync_request","event_bit","tbox_bit","switch_bit","room_switch_bit","item_bit",
        "dungeon_item_bit","save_snapshot","key_num","light_drop_num","light_drop_get_flag",
        "max_life_update","bottle_slots","bomb_bag_slot","rupee_count","rupee_delta","poe_count",
        "malo_fundraising","charlo_offering","fish_record","collect_smell","item_get","rando_item_get",
        "item_first_bit","collect_crystal","collect_mirror","dark_clear_lv","transform_lv","region_bit",
        "collect","visited_room","letter_get","pvp_hit","ganondorf_owner_claim","ganondorf_owner",
        "ganondorf_hit","ganondorf_reaction","ganondorf_player_damage","ganondorf_state","ooccoo_state"};
    for(auto value:types) if(type==value) return true;
    return false;
}
}
