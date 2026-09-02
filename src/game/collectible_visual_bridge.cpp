#include "dusklight_online/game/collectible_visual_bridge.hpp"

#include "d/dolzel.h"

// These fields are private only as a source-level encapsulation detail. Keep
// the chest visual repair isolated here without changing the SDK checkout or
// relying on a fixed byte offset.
#define private public
#include "d/actor/d_a_tbox.h"
#include "d/actor/d_a_door_mbossL1.h"
#include "d/actor/d_a_door_shutter.h"
#include "d/actor/d_a_obj_cblock.h"
#include "d/actor/d_a_obj_kshutter.h"
#include "d/actor/d_a_obj_lv4PoGate.h"
#include "d/actor/d_a_obj_scannon.h"
#undef private

#include "d/actor/d_a_e_pz.h"
#include "d/actor/d_a_obj_carry.h"
#include "d/actor/d_a_obj_Lv5Key.h"
#include "d/actor/d_a_obj_kgate.h"
#include "d/actor/d_a_obj_keyhole.h"
#include "d/actor/d_a_obj_drop.h"
#include "d/actor/d_a_obj_item.h"
#include "d/actor/d_a_obj_life_container.h"
#include "d/actor/d_a_obj_bbox.h"
#include "d/actor/d_a_obj_so.h"
#include "d/actor/d_a_obj_smallkey.h"
#include "d/actor/d_a_obj_sword.h"
#include "d/d_com_inf_game.h"
#include "d/d_door_param2.h"
#include "d/d_path.h"
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

void* judge_sewers_breakable_box(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_BBox_e) return nullptr;
    auto* box = static_cast<daObjBBox_c*>(actor);
    return box->getSwNo() == *static_cast<int*>(data) ? actor : nullptr;
}

void* repair_chain_block(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_ChainBlock_e) return nullptr;
    auto* block = static_cast<daObjCBlk_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (block->getSwNo() != flag || block->getPathID() == 0xFF ||
        !fopAcM_isSwitch(block, block->getSwNo())) {
        return nullptr;
    }
    dPath* path = dPath_GetRoomPath(block->getPathID(), fopAcM_GetHomeRoomNo(block));
    if (path == nullptr || path->m_num < 2) return nullptr;
    dPnt* point = dPath_GetPnt(path, 1);
    if (point == nullptr || block->current.pos.abs(point->m_position) <= 1.0f) return nullptr;
    block->current.pos = point->m_position;
    block->old.pos = block->current.pos;
    block->setBaseMtx();
    fopAcM_SetMtx(block, block->model1->getBaseTRMtx());
    return actor;
}

void* repair_jump_tbox(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_TBOX_e) return nullptr;
    auto* tbox = static_cast<daTbox_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (tbox->getSwNo() != flag ||
        !dComIfGs_isSwitch(tbox->getSwNo(), fopAcM_GetRoomNo(tbox))) {
        return nullptr;
    }
    const int funcType = tbox->getFuncType();
    if (!((funcType == 6 && tbox->getSwType() == 15) || funcType == 7)) return nullptr;
    dPath* path = dPath_GetRoomPath(tbox->getPathId(), -1);
    if (path == nullptr || path->m_num <= 0) return nullptr;
    dPnt* point = &path->m_points[path->m_num - 1];
    if (tbox->current.pos.abs(point->m_position) <= 1.0f) return nullptr;
    tbox->current.pos = point->m_position;
    tbox->home.pos = point->m_position;
    tbox->old.pos = tbox->current.pos;
    tbox->attention_info.position = tbox->current.pos;
    tbox->eyePos = tbox->current.pos;
    tbox->initBaseMtx();
    fopAcM_SetMtx(tbox, tbox->mpModel->getBaseTRMtx());
    dTres_c::setPosition(tbox->getTboxNo(), &tbox->current.pos);
    return actor;
}

void* repair_lv4_poe_gate(void* actor, void*) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Lv4PoGate_e) return nullptr;
    auto* gate = static_cast<daLv4PoGate_c*>(actor);
    const bool wasClosed = gate->mInitMove != 0 || gate->mMoveValue != gate->mMoveTarget ||
                           gate->mMode != daLv4PoGate_c::MODE_WAIT_e;
    if (gate->mSw != 0xFF) fopAcM_offSwitch(gate, gate->mSw);
    gate->mInitMove = 0;
    gate->mMoveValue = gate->mMoveTarget;
    gate->speedF = 0.0f;
    gate->init_modeWait();
    gate->setBaseMtx();
    fopAcM_SetMtx(gate, gate->mpModel->getBaseTRMtx());
    return wasClosed ? actor : nullptr;
}

void* judge_pz_mist(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_KYTAG12_e) return nullptr;
    return fopAcM_GetRoomNo(static_cast<fopAc_ac_c*>(actor)) == *static_cast<int*>(data)
               ? actor
               : nullptr;
}

void* repair_phantom_zant(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_E_PZ_e) return nullptr;
    auto* phantom = static_cast<daE_PZ_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (phantom->bitSw != flag || (phantom->arg0 != 0 && phantom->arg0 != 1) ||
        phantom->bitSw == 0xFF || !fopAcM_isSwitch(phantom, phantom->bitSw)) {
        return nullptr;
    }
    int room = fopAcM_GetRoomNo(phantom);
    if (fopAcIt_Judge(judge_pz_mist, &room) == nullptr) {
        cXyz pos(0.0f, 0.0f, -1300.0f);
        fopAcM_create(fpcNm_KYTAG12_e, 1, &pos, room, nullptr, nullptr, -1);
    }
    fopAcM_delete(phantom);
    return actor;
}

void* repair_sky_cannon(void* actor, void* data) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_SCannon_e) return nullptr;
    auto* cannon = static_cast<daSCannon_c*>(actor);
    const int flag = *static_cast<int*>(data);
    if (cannon->mLayerNo != 1 || cannon->getSw1() != flag ||
        !fopAcM_isSwitch(cannon, cannon->getSw1())) {
        return nullptr;
    }
    if (cannon->mIsPortal == FALSE && cannon->mMode == daSCannon_c::MODE_END &&
        cannon->mDrawShadow == TRUE) {
        return nullptr;
    }
    cannon->mIsPortal = FALSE;
    cannon->mDrawShadow = TRUE;
    cannon->mMode = daSCannon_c::MODE_END;
    cannon->setModelMtx();
    fopAcM_SetMtx(cannon, cannon->mpModels[cannon->mIsRepaired]->getBaseTRMtx());
    return actor;
}

struct FaronCageSearch {
    int flag;
    bool repairBokoblin;
};

void* repair_faron_cage_actor(void* actor, void* data) {
    if (actor == nullptr || data == nullptr) return nullptr;
    const auto* search = static_cast<FaronCageSearch*>(data);
    if (search->repairBokoblin) {
        if (fopAcM_GetName(actor) != fpcNm_E_RD_e) return nullptr;
        const int actorFlag = (fopAcM_GetParam(actor) >> 24) & 0xFF;
        if (actorFlag != search->flag) return nullptr;
        auto* enemy = static_cast<fopAc_ac_c*>(actor);
        dComIfGs_onSwitch(actorFlag, fopAcM_GetRoomNo(enemy));
        fopAcM_createDisappear(enemy, &enemy->current.pos, 10, 0, 11);
        fopAcM_delete(enemy);
        return actor;
    }
    if (fopAcM_GetName(actor) != fpcNm_OBJ_SO_e) return nullptr;
    auto* cage = static_cast<obj_so_class*>(actor);
    const uint32_t params = fopAcM_GetParam(&cage->actor);
    const int highFlag = (params >> 24) & 0xFF;
    const int midFlag = (params >> 16) & 0xFF;
    if (highFlag != search->flag && midFlag != search->flag) return nullptr;
    cage->actor.health = 0;
    cage->field_0xdae = 1;
    cage->field_0xdb0 = 2;
    cage->field_0xdc8 = 0.0f;
    cage->field_0x1054 = 1;
    for (int i = 0; i < 8; ++i) {
        cage->field_0x1a98[i] = 2;
        cage->field_0x750[i + 2] = 0.0f;
    }
    dComIfGs_onSwitch(search->flag, fopAcM_GetRoomNo(&cage->actor));
    return actor;
}

void* repair_sewers_carry_box(void* actor, void*) {
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_Carry_e) return nullptr;
    auto* box = static_cast<daObjCarry_c*>(actor);
    if (box->getType() != daObjCarry_c::TYPE_KIBAKO) return nullptr;
    box->obj_break(false, true, true);
    fopAcM_delete(box);
    return actor;
}

void* repair_faron_mist_gate(void* actor, void*) {
    constexpr int kGateFlag = 0x14;
    constexpr int kGateRoom = 5;
    if (actor == nullptr || fopAcM_GetName(actor) != fpcNm_Obj_KkrGate_e) return nullptr;
    auto* gate = static_cast<daObjKGate_c*>(actor);
    if (gate->getSwNo() != kGateFlag || fopAcM_GetRoomNo(gate) != kGateRoom) return nullptr;
    gate->setAction(2);
    return actor;
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

bool repair_remote_switch_actors(int stage, int flag) {
    if (stage != current_stage_table()) return false;
    bool repaired = false;
    repaired = fopAcIt_Judge(repair_chain_block, &flag) != nullptr || repaired;
    repaired = fopAcIt_Judge(repair_jump_tbox, &flag) != nullptr || repaired;
    if (stage == 19 && flag == 0x26) {
        repaired = fopAcIt_Judge(repair_lv4_poe_gate, nullptr) != nullptr || repaired;
    }
    repaired = fopAcIt_Judge(repair_phantom_zant, &flag) != nullptr || repaired;
    repaired = fopAcIt_Judge(repair_sky_cannon, &flag) != nullptr || repaired;

    if (stage == dStage_SaveTbl_FARON) {
        constexpr int kBothBokoblins = 45;
        constexpr int kCageBroken = 46;
        constexpr int kLeftBokoblin = 47;
        constexpr int kRightBokoblin = 48;
        const auto repairBokoblin = [&](int bokoblinFlag) {
            FaronCageSearch search{bokoblinFlag, true};
            repaired = fopAcIt_Judge(repair_faron_cage_actor, &search) != nullptr || repaired;
        };
        if (flag == kLeftBokoblin || flag == kRightBokoblin) {
            repairBokoblin(flag);
        } else if (flag == kBothBokoblins) {
            repairBokoblin(kLeftBokoblin);
            repairBokoblin(kRightBokoblin);
        } else if (flag == kCageBroken) {
            FaronCageSearch search{flag, false};
            repaired = fopAcIt_Judge(repair_faron_cage_actor, &search) != nullptr || repaired;
        } else if (flag == 0x14) {
            constexpr int kGateRoom = 5;
            if (!dComIfGs_isSwitch(flag, kGateRoom)) {
                dComIfGs_onSwitch(flag, kGateRoom);
                repaired = true;
            }
            repaired = fopAcIt_Judge(repair_faron_mist_gate, nullptr) != nullptr || repaired;
        }
    } else if (stage == dStage_SaveTbl_PRISON) {
        if (flag == 10) {
            auto* box = static_cast<daObjBBox_c*>(
                fopAcIt_Judge(judge_sewers_breakable_box, &flag));
            if (box != nullptr) {
                static constexpr u16 particleIds[] = {0x83B0, 0x83B1, 0x83B2, 0x83B3,
                                                      0x83B4};
                for (u16 particleId : particleIds) {
                    dComIfGp_particle_set(particleId, &box->current.pos, nullptr, &box->scale,
                                          0xff, nullptr, -1, nullptr, nullptr, nullptr);
                }
                fopAcM_seStart(box, Z2SE_OBJ_WOODBOX_BREAK, 0);
                fopAcM_delete(box);
                repaired = true;
            }
        } else if (flag == 17) {
            repaired = fopAcIt_Judge(repair_sewers_carry_box, nullptr) != nullptr || repaired;
        }
    }
    return repaired;
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
    constexpr int kFirstSewersBoxSwitch = 10;
    if (stage == dStage_SaveTbl_PRISON &&
        dComIfGs_isStageSwitch(stage, kFirstSewersBoxSwitch)) {
        repair_remote_switch_actors(stage, kFirstSewersBoxSwitch);
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
