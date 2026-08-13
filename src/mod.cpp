#include "dusklight_online/online_app.hpp"
#include "dusklight_online/game/abi_compat.hpp"

#include "f_ap/f_ap_game.h"
#include "mods/service.hpp"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/save.h"
#include "mods/svc/config.h"
#include "mods/svc/ui.h"

#include <memory>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(SaveService, svc_save);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

namespace {

std::unique_ptr<dusklight_online::OnlineApp> sApp;

// Advance multiplayer immediately after fapGm_Execute(), once actor execution
// and frame-interpolation recording have completed. ModLoader::tick() runs
// near the start of fapGm_Execute(), so updating from mod_update would move
// Remote Link one simulation phase too early.
DEFINE_HOOK(&fapGm_Execute, OnlineGameExecuteHook);

HookAction game_execute_pre(ModContext*, void*, void*, void*) {
    if (sApp != nullptr) {
        sApp->consume_progression_prompt_input();
    }
    return HOOK_CONTINUE;
}

void game_execute_post(ModContext*, void*, void*, void*) {
    if (sApp != nullptr) {
        sApp->update();
    }
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    if (!dusklight_online::game::required_game_abi_available()) {
        return mods::set_error(error, MOD_UNAVAILABLE,
                               "compatible actor and item APIs are unavailable");
    }
    sApp = std::make_unique<dusklight_online::OnlineApp>();
    if (sApp->initialize(error) != MOD_OK) {
        sApp.reset();
        return MOD_ERROR;
    }
    const ModResult preHookResult =
        mods::hook::add_pre<OnlineGameExecuteHook>(&game_execute_pre);
    const ModResult postHookResult = preHookResult == MOD_OK ?
        mods::hook::add_post<OnlineGameExecuteHook>(&game_execute_post) : preHookResult;
    if (postHookResult != MOD_OK) {
        // install() can have succeeded before add_post() fails, so make the
        // rollback explicit and idempotent.
        mods::hook::uninstall<OnlineGameExecuteHook>();
        sApp->shutdown();
        sApp.reset();
        return mods::set_error(error, postHookResult,
                               "Online game-execute hooks are unavailable");
    }
    svc_log->info(mod_ctx, "Dusklight Online initialization started");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    // Intentionally empty: see OnlineGameExecuteHook. This entry point runs
    // before fpcM_Management(), which is too early for multiplayer updates.
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    mods::hook::uninstall<OnlineGameExecuteHook>();
    if (sApp != nullptr) {
        sApp->shutdown();
        sApp.reset();
    }
    svc_log->info(mod_ctx, "Dusklight Online shut down");
    return MOD_OK;
}

}
