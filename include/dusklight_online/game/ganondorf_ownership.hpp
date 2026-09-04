#pragma once

namespace dusklight_online::game {

// Reloading an already-unlocked final fight runs a local two-step entry demo.
// It owns this client's Link/camera cleanup, not shared boss combat.
constexpr bool ganondorf_local_entry_pending(int noDrawTimer, int demo) {
    return noDrawTimer != 0 || demo == 95 || demo == 96;
}

constexpr bool ganondorf_local_entry_demo(int demo) {
    return demo == 95 || demo == 96;
}

constexpr bool ganondorf_ending_demo(int demo) {
    return demo >= 60 && demo <= 65;
}

// Observe only authenticated owner snapshots. The local cutscene is a one-shot
// presentation, not another combat owner, and must not restart on later packets.
struct GanondorfEndingSequence {
    bool pending = false;
    bool active = false;

    bool observe(int action, int demo, bool sameArea) {
        if (pending || active || !sameArea || action != 22 ||
            !ganondorf_ending_demo(demo)) return false;
        pending = true;
        return true;
    }

    bool start(bool localReady) {
        if (!pending || !localReady) return false;
        pending = false;
        active = true;
        return true;
    }
};

// Final-fight action numbers are defined by the original B_GND actor.
constexpr bool ganondorf_ownership_locked(int action, int mode, int demo) {
    return demo != 0 || action == 19 || action == 20 || action == 22 ||
           (action == 10 && mode >= 5);
}

} // namespace dusklight_online::game
