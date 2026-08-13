#include "dusklight_online/game/abi_compat.hpp"

#include "f_pc/f_pc_manager.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dusklight_online::game {
namespace {

void* find_game_symbol(const char* windowsName, const char* unixName) {
#if defined(_WIN32)
    const HMODULE game = GetModuleHandleW(nullptr);
    return game == nullptr ? nullptr
                           : reinterpret_cast<void*>(GetProcAddress(game, windowsName));
#else
    return dlsym(RTLD_DEFAULT, unixName);
#endif
}

using CreateActorOld = fpc_ProcID (*)(s16, u32, const cXyz*, int, const csXyz*,
                                      const cXyz*, s8);
using CreateActorNew = fpc_ProcID (*)(s16, u32, const cXyz*, int, const csXyz*,
                                      const cXyz*, s8, u32);
using ExecuteItemGetOld = void (*)(u8);
using ExecuteItemGetNew = void (*)(u8, u32, fopAc_ac_c*);

CreateActorNew create_actor_new() {
    static const auto function = reinterpret_cast<CreateActorNew>(find_game_symbol(
        "?fopAcM_create@@YAIFIPEBUcXyz@@HPEBVcsXyz@@0CI@Z",
        "_Z13fopAcM_createsjPK4cXyziPK5csXyzS1_aj"));
    return function;
}

CreateActorOld create_actor_old() {
    static const auto function = reinterpret_cast<CreateActorOld>(find_game_symbol(
        "?fopAcM_create@@YAIFIPEBUcXyz@@HPEBVcsXyz@@0C@Z",
        "_Z13fopAcM_createsjPK4cXyziPK5csXyzS1_a"));
    return function;
}

ExecuteItemGetNew execute_item_get_new() {
    static const auto function = reinterpret_cast<ExecuteItemGetNew>(find_game_symbol(
        "?execItemGet@@YAXEIPEAVfopAc_ac_c@@@Z",
        "_Z11execItemGethjP10fopAc_ac_c"));
    return function;
}

ExecuteItemGetOld execute_item_get_old() {
    static const auto function = reinterpret_cast<ExecuteItemGetOld>(find_game_symbol(
        "?execItemGet@@YAXE@Z", "_Z11execItemGeth"));
    return function;
}

}  // namespace

bool required_game_abi_available() {
    return (create_actor_new() != nullptr || create_actor_old() != nullptr) &&
           (execute_item_get_new() != nullptr || execute_item_get_old() != nullptr);
}

fpc_ProcID create_actor_compat(s16 profile, u32 parameters, const cXyz* position,
                               int room, const csXyz* angle, const cXyz* scale,
                               s8 argument) {
    if (const auto function = create_actor_new(); function != nullptr) {
        return function(profile, parameters, position, room, angle, scale, argument, 0);
    }
    if (const auto function = create_actor_old(); function != nullptr) {
        return function(profile, parameters, position, room, angle, scale, argument);
    }
    return fpcM_ERROR_PROCESS_ID_e;
}

void execute_item_get_compat(u8 item) {
    if (const auto function = execute_item_get_new(); function != nullptr) {
        function(item, 0, nullptr);
    } else if (const auto function = execute_item_get_old(); function != nullptr) {
        function(item);
    }
}

}  // namespace dusklight_online::game
