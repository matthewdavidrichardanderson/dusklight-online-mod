#pragma once

namespace dusklight_online::game {

// Apply the visual and actor side effects of a permanent collectible bit.
void repair_remote_tbox_collectible(int stage, int flag, bool newlySet);
void repair_remote_memory_item_collectible(int stage, int flag);

// Actors can spawn after a snapshot or live bit is applied, so repeat the
// repair while playing.
void repair_current_stage_collectibles();

// Repair live small-key locks whose keyhole state is latched during creation.
void repair_remote_key_door_actor(void* process);

}  // namespace dusklight_online::game
