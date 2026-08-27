#include "dusklight_online/game/collectible_visual_bridge.hpp"

#include "d/dolzel.h"

// These fields are private only as a source-level encapsulation detail. Keep
// the chest visual repair isolated here without changing the SDK checkout or
// relying on a fixed byte offset.
#define private public
#include "d/actor/d_a_tbox.h"
#include "d/actor/d_a_door_mbossL1.h"
#include "d/actor/d_a_door_shutter.h"
#include "d/actor/d_a_obj_kshutter.h"
#undef private

#include "d/actor/d_a_obj_Lv5Key.h"
#include "d/actor/d_a_obj_keyhole.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/actor/d_a_obj_item.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_smallkey.h"
#include "d/actor/d_a_obj_sword.h"
#include "d/d_com_inf_game.h"
#include "d/d_door_param2.h"
#include "d/d_save.h"
#include "d/d_stage.h"
#include "d/d_tresure.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

namespace dusklight_online::game {
namespace {

void* judge_tbox(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_TBOX_e) return nullptr;
    auto* tbox = static_cast<daTbox_c*>(actor);
    return tbox->getTboxNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_key(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_SmallKey_e) return nullptr;
    auto* key = static_cast<daKey_c*>(actor);
    return key->getSaveBitNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_drop(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Drop_e) return nullptr;
    auto* drop = static_cast<daObjDrop_c*>(actor);
    return drop->getSave() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_life(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_LifeContainer_e) return nullptr;
    auto* life = static_cast<daObjLife_c*>(actor);
    return life->getSaveBitNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_item(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_ITEM_e) return nullptr;
    auto* item = static_cast<daItem_c*>(actor);
    return daItem_prm::getItemBitNo(item) == *static_cast<int*>(data) ? actor : nullptr;
}

void* judge_sword(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Sword_e) return nullptr;
    auto* sword = static_cast<daObjSword_c*>(actor);
    return sword->getItemBit() == *static_cast<int*>(data) ? actor : nullptr;
}

void repair_tbox_visual(int flag) {
    auto* tbox = static_cast<daTbox_c*>(fopAcIt_Judge(judge_tbox, &flag));
    if (tbox == nullptr) return;
    if (tbox->mpAnm != nullptr) tbox->mpAnm->setFrame(tbox->mpAnm->getEndFrame());
    tbox->setAction(&daTbox_c::actionWait);
    tbox->setDzb();
    dTres_c::offStatus(0, flag, 1);
}

bool repair_key_visual(int flag) {
    auto* key = static_cast<daKey_c*>(fopAcIt_Judge(judge_key, &flag));
    if (key == nullptr) return false;
    dTres_c::offStatus(flag, 1);
    fopAcM_delete(key);
    return true;
}

bool repair_drop_visual(int flag) {
    auto* drop = static_cast<daObjDrop_c*>(fopAcIt_Judge(judge_drop, &flag));
    if (drop == nullptr) return false;
    fopAcM_delete(drop);
    return true;
}

bool repair_life_visual(int globalBit) {
    auto* life = static_cast<daObjLife_c*>(fopAcIt_Judge(judge_life, &globalBit));
    if (life == nullptr) return false;
    fopAcM_delete(life);
    return true;
}

bool repair_item_visual(int flag) {
    auto* item = static_cast<daItem_c*>(fopAcIt_Judge(judge_item, &flag));
    if (item == nullptr) return false;
    fopAcM_delete(item);
    return true;
}

bool repair_sword_visual(int globalBit) {
    auto* sword = static_cast<daObjSword_c*>(fopAcIt_Judge(judge_sword, &globalBit));
    if (sword == nullptr) return false;
    fopAcM_delete(sword);
    return true;
}

int current_stage_table() {
    stage_stag_info_class* info = dComIfGp_getStageStagInfo();
    return info != nullptr ? dStage_stagInfo_GetSaveTbl(info) : -1;
}

}  // namespace

void repair_remote_tbox_collectible(int stage, int flag, bool newlySet) {
    if (!newlySet || stage != current_stage_table()) return;
    repair_tbox_visual(flag);
    if (!repair_key_visual(flag)) repair_drop_visual(flag);
}

void repair_remote_memory_item_collectible(int stage, int flag) {
    if (stage != current_stage_table()) return;
    const int globalBit = flag + dSv_info_c::MEMORY_ITEM;
    if (!repair_item_visual(globalBit) && !repair_life_visual(globalBit)) {
        repair_sword_visual(globalBit);
    }
}

void repair_current_stage_collectibles() {
    const int stage = current_stage_table();
    if (stage < 0) return;
    for (int flag = 0; flag < 64; ++flag) {
        if (dComIfGs_isStageTbox(stage, flag)) repair_tbox_visual(flag);
    }
    for (int flag = 0; flag < dSv_info_c::DAN_ITEM; ++flag) {
        // Current-stage memory is authoritative while an item cutscene is in
        // flight; Savedata can lag it until the stage/save commit.
        if (g_dComIfG_gameInfo.info.getMemory().getBit().isItem(flag)) {
            const int globalBit = flag + dSv_info_c::MEMORY_ITEM;
            if (!repair_item_visual(globalBit) && !repair_life_visual(globalBit)) {
                repair_sword_visual(globalBit);
            }
        }
    }
}

void repair_remote_key_door_actor(void* process) {
    if (process == nullptr || !fopAcM_IsActor(process)) return;
    switch (fpcM_GetName(process)) {
    case fpcNm_DOOR20_e:
    {
        auto* door = static_cast<daDoor20_c*>(process);
        const int swBit = door_param2_c::getSwbit(door);
        if (!door->field_0x5f0 || door->field_0x5ec == fpcM_ERROR_PROCESS_ID_e ||
            swBit == 0xFF || !dComIfGs_isSwitch(swBit, 0xFFFFFFFF) ||
            (door_param2_c::getFrontOption(door) != 2 &&
             door_param2_c::getBackOption(door) != 2)) {
            return;
        }
        if (door->field_0x673 == 1) {
            auto* key = static_cast<daObjLv5Key_c*>(fopAcM_SearchByID(door->field_0x5ec));
            if (key == nullptr) return;
            key->keylock_open_start();
        } else {
            auto* keyhole = reinterpret_cast<obj_keyhole_class*>(
                fopAcM_SearchByID(door->field_0x5ec));
            if (keyhole == nullptr) return;
            keyhole->setOpen();
        }
        door->field_0x5f0 = false;
        return;
    }
    case fpcNm_L1MBOSS_DOOR_e:
    {
        auto* door = static_cast<daMBdoorL1_c*>(process);
        const u8 swBit = door_param2_c::getSwbit(door);
        if (door->mKeyHoleId == fpcM_ERROR_PROCESS_ID_e ||
            door_param2_c::getFrontOption(door) != 2 || swBit == 0xFF ||
            !fopAcM_isSwitch(door, swBit)) {
            return;
        }
        auto* keyhole = reinterpret_cast<obj_keyhole_class*>(
            fopAcM_SearchByID(door->mKeyHoleId));
        if (keyhole != nullptr && !keyhole->checkOpen() && !keyhole->checkOpenEnd()) {
            keyhole->setOpen();
        }
        return;
    }
    case fpcNm_Obj_Kshutter_e:
    {
        auto* door = static_cast<daObjKshtr_c*>(process);
        if (!door->mIsCheckKey || door->mKeyHoleId == fpcM_ERROR_PROCESS_ID_e ||
            door->mSwNo == 0xFF || !fopAcM_isSwitch(door, door->mSwNo)) {
            return;
        }
        auto* keyhole = reinterpret_cast<obj_keyhole_class*>(
            fopAcM_SearchByID(door->mKeyHoleId));
        if (keyhole != nullptr && !keyhole->checkOpen() && !keyhole->checkOpenEnd()) {
            keyhole->setOpen();
        }
        return;
    }
    default:
        return;
    }
}

}  // namespace dusklight_online::game
