#pragma once

namespace dusklight_online::game {

// Apply the non-save-data side effects used by the pinned Online branch when
// a permanent collectible bit arrives from a peer.
void repair_remote_tbox_collectible(int stage, int flag, bool newlySet);
void repair_remote_memory_item_collectible(int stage, int flag);

// Timing-independent safety pass. Actors can spawn after a snapshot or live
// bit is applied, so the original implementation repeats this while playing.
void repair_current_stage_collectibles();

// Reproduce the Online branch's per-execute live small-key lock repair for
// the three door actors whose keyhole state is latched during creation.
void repair_remote_key_door_actor(void* process);

}  // namespace dusklight_online::game
