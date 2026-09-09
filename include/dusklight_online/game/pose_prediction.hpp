#pragma once
#include "dusk/multiplayer/multiplayer.hpp"
#include <cmath>
#include <cstdint>

namespace dusklight_online::game {
// Caller restricts this to official walking/running procedures. Only visual
// position and existing animation clocks advance; gameplay events never do.
inline bool predict_pose(const dusk::multiplayer::PeerPoseSnapshot& before,
                         const dusk::multiplayer::PeerPoseSnapshot& latest,
                         int64_t ahead, dusk::multiplayer::PeerPoseSnapshot& out) {
    using Pose = dusk::multiplayer::PeerPoseSnapshot;
    if (ahead < 1 || ahead > 2 || !before.valid || !latest.valid ||
        latest.sequence <= before.sequence || latest.sequence - before.sequence > 4 ||
        latest.visualMode != Pose::VisualMode::SemanticGameplay || latest.linkMatrices.valid ||
        before.procId != latest.procId || before.isWolf != latest.isWolf ||
        before.stage != latest.stage || before.room != latest.room || before.layer != latest.layer ||
        before.isTransforming || latest.isTransforming || before.equipItem != latest.equipItem ||
        before.clothesVariant != latest.clothesVariant) return false;
    const float factor = float(ahead) / float(latest.sequence - before.sequence);
    const float dx = (latest.x - before.x) * factor;
    const float dy = (latest.y - before.y) * factor;
    const float dz = (latest.z - before.z) * factor;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
        dx*dx + dy*dy + dz*dz > 120.0f*120.0f || std::abs(dy) > 30.0f) return false;
    out = latest;
    out.x += dx; out.y += dy; out.z += dz;
    if (out.bodyRootValid) { out.bodyRootX += dx; out.bodyRootY += dy; out.bodyRootZ += dz; }
    if (out.lanternVisualValid && out.lanternLinkAnchored) {
        out.lanternX += dx; out.lanternY += dy; out.lanternZ += dz;
    }
    if (out.boomerangVisualValid && out.boomerangLinkAnchored) {
        out.boomerangX += dx; out.boomerangY += dy; out.boomerangZ += dz;
    }
    if (out.ironBallVisualValid && out.ironBallLinkAnchored) {
        out.ironBallX += dx; out.ironBallY += dy; out.ironBallZ += dz;
    }
    if (out.hookshotVisualValid && out.hookshotTopLinkAnchored) {
        out.hookshotTopX += dx; out.hookshotTopY += dy; out.hookshotTopZ += dz;
    }
    if (out.hookshotVisualValid && out.hookshotSubTopLinkAnchored) {
        out.hookshotSubTopX += dx; out.hookshotSubTopY += dy; out.hookshotSubTopZ += dz;
    }
    // The renderer already wraps loop animations and clamps one-shot frames
    // using the real resource duration. Never extrapolate across a clip change.
    const auto advance = [ahead](int oldClip, int clip, int oldArc, int arc,
                                 float rate, float& frame) {
        if (oldClip == clip && oldArc == arc && std::isfinite(rate) && std::abs(rate) <= 4)
            frame += rate * float(ahead);
    };
#define ADVANCE_SLOT(name) advance(before.name##Bck0, latest.name##Bck0, before.name##BckArc0, latest.name##BckArc0, latest.name##Rate0, out.name##Frame0); \
    advance(before.name##Bck1, latest.name##Bck1, before.name##BckArc1, latest.name##BckArc1, latest.name##Rate1, out.name##Frame1); \
    advance(before.name##Bck2, latest.name##Bck2, before.name##BckArc2, latest.name##BckArc2, latest.name##Rate2, out.name##Frame2)
    ADVANCE_SLOT(under);
    ADVANCE_SLOT(upper);
#undef ADVANCE_SLOT
    out.audioEvents.clear();
    out.activeAudioEvents.clear();
    out.ageTicks += static_cast<uint32_t>(ahead);
    return true;
}
} // namespace dusklight_online::game
