#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class dCcD_GObjInf;
class fopAc_ac_c;

namespace dusk::multiplayer {

// Matches the pinned TwilitRealm Online branch. Remote Midna streaming is
// deliberately disabled there because the matrix stream desynchronizes and
// consumes excessive bandwidth. Keep the capability in the protocol for wire
// compatibility, but never advertise, transmit, accept, or render it.
inline constexpr bool kRemoteMidnaStreamingEnabled = false;

struct RemoteModelMatrixSnapshot {
    bool valid = false;
    uint16_t jointCount = 0;
    uint16_t weightCount = 0;
    bool weightsOmitted = false;
    std::array<float, 12> base{};
    std::vector<float> joints;
    std::vector<float> weights;
};

struct RemoteLinkMatrixSnapshot {
    bool valid = false;
    RemoteModelMatrixSnapshot body;
    RemoteModelMatrixSnapshot hat;
    RemoteModelMatrixSnapshot face;
    RemoteModelMatrixSnapshot hand;
    RemoteModelMatrixSnapshot sword;
    RemoteModelMatrixSnapshot sheath;
    RemoteModelMatrixSnapshot shield;
    RemoteModelMatrixSnapshot heldItem;
    RemoteModelMatrixSnapshot hookTip;
    RemoteModelMatrixSnapshot hookSubItem;
    RemoteModelMatrixSnapshot hookSubTip;
    RemoteModelMatrixSnapshot arrow;
    RemoteModelMatrixSnapshot kantera;
    RemoteModelMatrixSnapshot kanteraGlow;
    RemoteModelMatrixSnapshot itemActor;
    RemoteModelMatrixSnapshot rideActor;
    RemoteModelMatrixSnapshot midna;
    RemoteModelMatrixSnapshot midnaMask;
    RemoteModelMatrixSnapshot midnaHand;
    RemoteModelMatrixSnapshot midnaHair;
    RemoteModelMatrixSnapshot midnaGlow;
    int midnaHairShape = 0;
};

struct RemoteAudioEvent {
    uint32_t sequence = 0;
    uint32_t soundId = 0;
    uint32_t mapInfo = 0;
    int8_t reverb = -1;
    uint8_t sourceKind = 0;
    bool level = false;
};

enum RemoteObjectKind : uint8_t {
    REMOTE_OBJECT_NONE = 0,
    REMOTE_OBJECT_BOMB = 1,
};

enum RemoteItemActorKind : uint8_t {
    REMOTE_ITEM_ACTOR_NONE = 0,
    REMOTE_ITEM_ACTOR_BOOMERANG = 1,
    REMOTE_ITEM_ACTOR_BOMB_NORMAL = 2,
    REMOTE_ITEM_ACTOR_BOMB_WATER = 3,
    REMOTE_ITEM_ACTOR_BOMB_INSECT = 4,
};

enum RemoteRideActorKind : uint8_t {
    REMOTE_RIDE_ACTOR_NONE = 0,
    REMOTE_RIDE_ACTOR_SPINNER = 1,
};

struct RemoteObjectSnapshot {
    bool valid = false;
    std::string peerId;
    uint8_t objectKind = REMOTE_OBJECT_NONE;
    int32_t objectId = -1;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string stage;
    int room = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    int kind = 0;
    int exTime = -1;
    bool active = false;
    bool exploding = false;
};

using RemoteBombObjectSnapshot = RemoteObjectSnapshot;

enum RemoteAudioSourceKind : uint8_t {
    REMOTE_AUDIO_SOURCE_GENERIC = 0,
    REMOTE_AUDIO_SOURCE_LINK_SOUND = 1,
    REMOTE_AUDIO_SOURCE_LINK_SOUND_LEVEL = 2,
    REMOTE_AUDIO_SOURCE_LINK_VOICE = 3,
    REMOTE_AUDIO_SOURCE_LINK_VOICE_LEVEL = 4,
    REMOTE_AUDIO_SOURCE_LINK_SWORD = 5,
    REMOTE_AUDIO_SOURCE_LINK_COLLISION = 6,
    REMOTE_AUDIO_SOURCE_LINK_HIT_ITEM = 7,
};

struct PeerPoseSnapshot {
    bool valid = false;
    std::string peerId;
    uint32_t sequence = 0;
    uint32_t ageTicks = 0;
    std::string stage;
    int room = -1;
    int layer = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    bool finalGanondorfReady = false;
    int procId = 0;
    int procVar0 = 0;
    int procVar1 = 0;
    int procVar2 = 0;
    int procVar3 = 0;
    int procVar5 = 0;
    int cutType = 0;
    int cutCount = 0;
    bool jumpCancelTurn = false;
    bool manualSyncReady = false;
    float underFrame = 0.0f;
    int underBck0 = 0;
    float underFrame0 = 0.0f;
    float underRate0 = 1.0f;
    int upperBck2 = 0;
    float upperFrame2 = 0.0f;
    float upperRate2 = 1.0f;
    std::array<int16_t, 10> hatRotA{};
    std::array<int16_t, 10> hatRotB{};
    std::array<int16_t, 3> hatSwing{};
    int hatShapeY = 0;
    bool isWolf = false;
    bool isTransforming = false;
    bool transformFromWolf = false;
    bool transformToWolf = false;
    int transformProcVar0 = 0;
    int transformProcVar5 = 0;
    int transformClothesWait = 0;
    float transformFrame = 0.0f;
    int transformProcVar2 = 0;
    int transformProcVar3 = 0;
    int transformShapeX = 0;
    uint16_t equipItem = 0xFFFF;
    int swordVariant = 0;
    int shieldVariant = 0;
    int clothesVariant = 0;
    bool swordDraw = false;
    bool shieldDraw = false;
    bool shieldGuardActive = false;
    bool swordOut = false;
    bool midnaDraw = false;
    bool midnaMaskDraw = false;
    bool midnaHandDraw = false;
    bool midnaHairDraw = false;
    bool midnaShadowForm = false;
    bool heavyBoots = false;
    bool itemDraw = false;
    bool kanteraDraw = false;
    int itemActorKind = 0;
    int itemActorBombExTime = -1;
    int itemActorBombFlash = -1;
    int rideActorKind = 0;
    RemoteLinkMatrixSnapshot linkMatrices;
    bool linkMatricesFresh = false;
    std::vector<RemoteAudioEvent> audioEvents;
    std::vector<RemoteAudioEvent> activeAudioEvents;
};

using RemotePvpHitCallback = void (*)(fopAc_ac_c*, fopAc_ac_c*, dCcD_GObjInf*);
using RemoteBombActorCallback = void (*)(int32_t);

void configure_remote_actor_bridge(bool pvpEnabled, RemotePvpHitCallback pvpHit,
                                   RemoteBombActorCallback bombCreated);
void set_remote_pvp_hit_callback(RemotePvpHitCallback callback);
void set_remote_bomb_actor_callback(RemoteBombActorCallback callback);
void set_remote_actor_options(bool displayMidna, bool syncWorld,
                              bool remoteCollision, bool pvpEnabled);
void set_remote_bomb_object(const RemoteBombObjectSnapshot& object);
void erase_remote_actor_peer(const std::string& peerId);
void reset_remote_actor_bridge();
bool pvp_enabled();
bool display_remote_midna_enabled();
bool remote_collision_enabled();
bool get_remote_bomb_object_for_peer(const std::string& peerId,
                                     RemoteBombObjectSnapshot* out);
void report_remote_link_pvp_target_hit(fopAc_ac_c* remoteLinkActor,
                                       fopAc_ac_c* attackActor,
                                       dCcD_GObjInf* attackInfo);
void register_remote_bomb_actor_id(int32_t actorId);

}  // namespace dusk::multiplayer
