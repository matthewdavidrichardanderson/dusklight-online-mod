#include "dusklight_online/game/bomb_bag_sync.hpp"
#include <stdexcept>

using dusklight_online::game::should_publish_bomb_bag;
void require(bool condition) {
    if (!condition) throw std::runtime_error("Bomb bag sync regression");
}
int main() {
    constexpr int normal = 1, water = 2;
    // Death/glitch retains the exact bag contents but removes rental status.
    require(should_publish_bomb_bag(false, true, false, normal, 10, true, normal, 10));
    // Once observed as permanent, it must not repeatedly announce the bag.
    require(!should_publish_bomb_bag(false, true, false, normal, 10, false, normal, 10));
    require(!should_publish_bomb_bag(false, true, true, normal, 10, true, normal, 10));
    require(!should_publish_bomb_bag(true, true, false, normal, 10, true, normal, 10));
    require(!should_publish_bomb_bag(false, false, false, normal, 10, true, normal, 10));
    require(!should_publish_bomb_bag(false, true, false, normal, 0, true, normal, 10));
    // Preserve existing acquisition, type-change and empty-refill behaviour.
    require(should_publish_bomb_bag(false, true, false, normal, 10, false, -1, 0));
    require(should_publish_bomb_bag(false, true, false, water, 10, false, normal, 10));
    require(should_publish_bomb_bag(false, true, false, normal, 10, false, normal, 0));
    require(!should_publish_bomb_bag(false, true, false, normal, 20, false, normal, 10));
    require(!should_publish_bomb_bag(false, true, false, normal, 5, false, normal, 10));
}
