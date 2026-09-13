#pragma once

#include <cstdint>

namespace dusklight_online::game {

// Apply the visual and actor side effects of a permanent collectible bit.
void repair_remote_tbox_collectible(int stage, int flag, bool newlySet);
void repair_remote_memory_item_collectible(int stage, int flag);

// Apply actor-side state latched before a remotely received switch changed.
// This covers moving/set-piece actors which do not fully react to the save bit
// alone while their room is already loaded.
bool repair_remote_switch_actors(int stage, int flag);

// Read a loaded web's native destruction timer, or -1 for another actor.
int web_delete_timer(void* actor);

// Start the exact loaded web's native destruction sequence. The actor owns
// the animation, completion switch, collision release, and deletion.
bool apply_remote_web_timer(int actorName, int room, uint32_t params, int timer);

// Apply a web's authoritative completion switch and remove the exact actor.
bool repair_remote_web_actor(int actorName, int room, int flag, uint32_t params);

// Actors can spawn after a snapshot or live bit is applied, so repeat the
// repair while playing.
void repair_current_stage_collectibles();

// Repair live small-key locks whose keyhole state is latched during creation.
void repair_remote_key_door_actor(void* process);

}  // namespace dusklight_online::game
