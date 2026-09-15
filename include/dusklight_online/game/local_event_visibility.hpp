#pragma once

#include "dusklight_online/game/remote_visibility.hpp"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"

namespace dusklight_online::game {

inline bool local_link_in_load_exit(daAlink_c* link) {
    if (link == nullptr || !dComIfGp_isEnableNextStage()) return false;
    // checkSceneChange sets this flag and ORIGINAL 26/17 only after a
    // successful ordinary exit. A warp or cinematic stage request is not an
    // exit-walk exception. Doors retain their own event command instead.
    const auto mode = link->mDemo.getDemoMode();
    return link->eventInfo.checkCommandDoor() ||
           link->mProcID == daAlink_c::PROC_DOOR_OPEN ||
           (link->mDemo.getDemoType() == daPy_demo_c::DEMO_TYPE_ORIGINAL_e &&
            link->checkNoResetFlg0(daPy_py_c::FLG0_UNK_4000) &&
            (mode == daPy_demo_c::DEMO_UNK_26_e ||
             mode == daPy_demo_c::DEMO_UNK_17_e));
}

inline bool local_transition_hides_remote_link() {
    auto* link = daAlink_getAlinkActorClass();
    if (link == nullptr) return true;
    // IsDoingReq spans both visible fades and the request's final cleanup;
    // it is not a teardown signal. IsPeek marks the actual swap barrier.
    return remote_link_transition_hidden(dComIfGp_isEnableNextStage(),
                                         fopOvlpM_IsPeek(),
                                         local_link_in_load_exit(link));
}

inline bool local_scene_hides_remote_link() {
    static RemoteLinkEventVisibility visibility;
    static fpc_ProcID previousPlayer = fpcM_ERROR_PROCESS_ID_e;
    auto* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        visibility = {};
        previousPlayer = fpcM_ERROR_PROCESS_ID_e;
        return true;
    }
    const auto playerId = fopAcM_GetID(link);
    if (playerId != previousPlayer) {
        visibility = {};
        previousPlayer = playerId;
    }
    // TALK is ordinary conversation, not dialogue embedded in a DEMO event.
    // START is the native post-load entrance; authored arrival movies use
    // SYSTEM/TOOL instead. Door traversal and the native item-get procedure
    // seed a local exception tied to the current engine event through handoff.
    // Do not whitelist a whole demo type or bypass transition/animation guards.
    const bool door = link->eventInfo.checkCommandDoor() ||
        link->mProcID == daAlink_c::PROC_DOOR_OPEN || local_link_in_load_exit(link);
    const bool itemGet = link->mProcID == daAlink_c::PROC_GET_ITEM;
    return visibility.hidden(
        local_transition_hides_remote_link(),
        dComIfGp_event_runCheck(),
        dComIfGp_getEvent()->isOrderOK(),
        dComIfGp_event_getMode() == dEvt_mode_TALK_e,
        link->mDemo.getDemoType() == daPy_demo_c::DEMO_TYPE_START_e,
        door || itemGet,
        link->eventInfo.getEventId());
}

} // namespace dusklight_online::game
