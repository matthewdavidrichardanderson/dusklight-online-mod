#include "f_pc/f_pc_profile.h"

// The selected SDK has the required handle pool and starter machinery but
// does not expose the no-cull entry point used by Remote Link. Access the
// existing field only inside this compatibility translation unit.
#define private public
#include "Z2AudioLib/Z2SeMgr.h"
#undef private
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2Audience.h"
#include "Z2AudioLib/Z2SoundInfo.h"
#include "Z2AudioLib/Z2SoundStarter.h"
#include "f_op/f_op_actor_mng.h"

namespace dusklight_online::game {

JAUAudibleParam remote_audio_audible_params(JAISoundID soundId) {
    Z2AudioMgr* audioMgr = Z2GetAudioMgr();
    return audioMgr == nullptr ? JAUAudibleParam{}
                               : audioMgr->mSoundInfo.getAudibleSwFull(soundId);
}

Z2Audience* remote_audio_audience() {
    Z2AudioMgr* audioMgr = Z2GetAudioMgr();
    return audioMgr == nullptr ? nullptr : &audioMgr->mAudience;
}

bool remote_audio_start_no_cull(Z2AudioMgr* audioMgr, JAISoundID soundId, u32 mapInfo,
                                s8 reverb, f32 pitch, f32 volume, f32 pan, f32 dolby) {
    if (audioMgr == nullptr || soundId == 0xFFFFFFFF || audioMgr->isLevelSe(soundId)) {
        return false;
    }
    JAISoundHandle* handle = audioMgr->mSoundHandles.getFreeHandle();
    if (handle == nullptr) return false;
    // Do not use the header-inline Z2GetSoundStarter() from a mod DLL. Its
    // template static can bind to the DLL's own uninitialised singleton slot
    // instead of the game's slot, which is null during Remote Link playback.
    return audioMgr->mSoundStarter.startSound(soundId, handle, nullptr, mapInfo,
                                              reverb / 127.0f, pitch, volume, pan,
                                              dolby, 0);
}

}  // namespace dusklight_online::game

// A mod owns this profile definition; it is not imported from the game DLL.
#undef DUSK_PROFILE
#define DUSK_PROFILE extern

// OSPanic is not part of the public mod import surface. These two calls are
// debug-only layer assertions inside fopAcM_ct; the actual initialization and
// condition bookkeeping remain unchanged.
#define OSPanic(...) ((void)0)

// The actor uses a private process ID. The bridge returns this mod-owned
// profile only while resolving a recorded Remote Link creation.
#include "d_a_remote_link_impl.inc"
