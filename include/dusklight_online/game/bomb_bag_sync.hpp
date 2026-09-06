#pragma once

namespace dusklight_online::game {
// A retained rental becoming permanent is an acquisition even when its
// item and nonzero ammunition count have not changed.
inline bool should_publish_bomb_bag(bool randomizer, bool syncable, bool rental,
        int item, int count, bool previousRental, int previousItem, int previousCount) {
    return !randomizer && syncable && !rental && count > 0 &&
           (previousRental || previousItem != item || previousCount == 0);
}
} // namespace dusklight_online::game
