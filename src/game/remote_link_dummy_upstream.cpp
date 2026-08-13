#include "dusklight_online/game/remote_actor_bridge.hpp"
#include "f_op/f_op_actor_mng.h"

// Keep every process-name comparison in the exact upstream dummy source
// consistent with the private ID used by the standalone profile bridge.
#define fopAcM_create(profile, actorParams, position, room, angle, scale, argument) \
    ::dusklight_online::game::create_remote_actor_process(                       \
        actorParams, position, room, angle, scale, argument)
#define fopAcM_delete(processId) \
    ::dusklight_online::game::delete_remote_actor_process(processId)
#include "remote_link_dummy_upstream.inc"
#undef fopAcM_delete
#undef fopAcM_create
