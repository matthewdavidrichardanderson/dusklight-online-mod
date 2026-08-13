#pragma once

#include "dolphin/types.h"
#include "f_pc/f_pc_base.h"

class csXyz;
class fopAc_ac_c;
struct cXyz;

namespace dusklight_online::game {

bool required_game_abi_available();

fpc_ProcID create_actor_compat(s16 profile, u32 parameters, const cXyz* position,
                               int room, const csXyz* angle, const cXyz* scale,
                               s8 argument);

void execute_item_get_compat(u8 item);

}  // namespace dusklight_online::game
