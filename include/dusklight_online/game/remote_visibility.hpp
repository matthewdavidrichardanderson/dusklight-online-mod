#pragma once

namespace dusklight_online::game {

inline constexpr unsigned kUnsupportedEventPresentation = 1u << 3;

// A requested stage is still live during the native exit walk/fade. Peek is
// the overlap manager's scene-swap barrier, after the outgoing fade finishes.
constexpr bool remote_link_transition_hidden(bool nextStageRequested,
                                             bool overlapPeek, bool loadExit) {
    return overlapPeek || (nextStageRequested && !loadExit);
}

// Apply independently to sender and receiver. Cleanup is allowed only for a
// previously observed permitted event, never just because isOrderOK is true.
constexpr bool remote_link_scene_hidden(bool sceneChanging, bool eventRunning,
                                       bool talking, bool loadEntrance,
                                       bool doorTraversal, bool allowedEventCleanup = false) {
    return sceneChanging ||
           (eventRunning && !allowedEventCleanup && !talking && !loadEntrance && !doorTraversal);
}

struct RemoteLinkEventVisibility {
    bool allowedEvent = false;

    constexpr bool hidden(bool sceneChanging, bool eventRunning, bool eventOrderOK,
                          bool talking, bool loadEntrance, bool doorTraversal) {
        if (sceneChanging || !eventRunning) {
            allowedEvent = false;
            return sceneChanging;
        }
        // A new active event replaces the old exception immediately. No
        // timeout or procedure-only carryover can expose a later cinematic.
        if (!eventOrderOK) {
            allowedEvent = !remote_link_scene_hidden(false, true, talking,
                                                      loadEntrance, doorTraversal);
        }
        return !allowedEvent;
    }
};

} // namespace dusklight_online::game
