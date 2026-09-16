#pragma once

#include "dusklight_online/game/ooccoo_sync.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace dusklight_online::game::ooccoo {

inline nlohmann::json encode(Progress state) {
    return {{"version", 2}, {"acquired", state.acquired}, {"completed", state.completed},
            {"city_special", state.citySpecial}, {"unbound_note", state.unboundNote}};
}

// Range-check in the JSON's original width BEFORE narrowing. Reject partial,
// legacy and coordinate-bearing payloads atomically; don't treat them as clears.
inline std::optional<unsigned> bounded_integer(const nlohmann::json& value, unsigned max) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number <= max) return static_cast<unsigned>(number);
    } else if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if (number >= 0 && static_cast<uint64_t>(number) <= max)
            return static_cast<unsigned>(number);
    }
    return std::nullopt;
}

inline std::optional<Progress> decode(const nlohmann::json& state) {
    if (!state.is_object() || state.size() != 5 || !state.contains("version") ||
        !state.contains("acquired") || !state.contains("completed") ||
        !state.contains("city_special") || !state.contains("unbound_note")) return std::nullopt;
    const auto version = bounded_integer(state["version"], 2);
    const auto acquired = bounded_integer(state["acquired"], DungeonMask);
    const auto completed = bounded_integer(state["completed"], DungeonMask);
    if (!version || *version != 2 || !acquired || !completed ||
        !state["city_special"].is_boolean() || !state["unbound_note"].is_boolean()) {
        return std::nullopt;
    }
    return Progress{static_cast<uint8_t>(*acquired), static_cast<uint8_t>(*completed),
                    state["city_special"].get<bool>(), state["unbound_note"].get<bool>()};
}

} // namespace dusklight_online::game::ooccoo
