#include "dusklight_online/game/visual_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// These two public game classes do not expose the draw-buffer/minimap layout
// needed by their own draw methods. The access change is local to this TU and
// does not alter the compiled game ABI.
#define private public
#include "d/d_drawlist.h"
#include "d/d_map.h"
#include "d/d_meter_map.h"
#undef private

#include "d/dolzel.h"

#include "JSystem/J3DGraphBase/J3DShape.h"
#include "SSystem/SComponent/c_math.h"
#include "d/d_camera.h"
#include "d/d_msg_object.h"
#include "d/d_s_play.h"
#include "dusk/multiplayer/remote_link_dummy.hpp"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/resource.h"

#include <imgui.h>

namespace dusklight_online::game {

DEFINE_HOOK(&dDlst_list_c::drawOpaDrawList, OpaqueDrawListHook);
DEFINE_HOOK_SYMBOL("dMeterMap_c::draw", void(dMeterMap_c*), MeterMapDrawHook);
DEFINE_HOOK_SYMBOL("?PostDraw@ImGuiConsole@dusk@@QEAAXXZ", void(void*),
                   HostImGuiPostDrawHook);
DEFINE_HOOK_SYMBOL("?GetCurrentContext@ImGui@@YAPEAUImGuiContext@@XZ",
                   ImGuiContext*(), HostImGuiGetCurrentContextSymbol);

namespace {

using dusk::multiplayer::PeerPoseSnapshot;

struct PlayerColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct MinimapMarker {
    int room = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int angleY = 0;
    PlayerColor color{255, 255, 255, 255};
};

struct NameLabelFontAtlas {
    ImFontAtlas atlas;
    ImFont* font = nullptr;
    ImFont* uiFont = nullptr;
    ResourceBuffer fontBuffer = RESOURCE_BUFFER_INIT;
    ResourceBuffer uiFontBuffer = RESOURCE_BUFFER_INIT;
    std::vector<u8> rgbaPixels;
    TGXTexObj texObj{};
    int texWidth = 0;
    int texHeight = 0;
    bool attempted = false;
    bool valid = false;
};

struct Notification {
    std::string playerName;
    std::string text;
    PlayerColor playerColor{255, 255, 255, 255};
    float ageSeconds = 0.0f;
    float durationSeconds = 5.0f;
};

bool sConnected = false;
bool sGameplayReady = false;
bool sNameLabelsEnabled = true;
bool sRemoteModelEnabled = true;
bool sPlayerListEnabled = false;
std::string sRoom;
std::string sLocalStatus;
std::string sLocalName;
uint8_t sLocalColorSlot = 0;
std::map<std::string, PeerPoseSnapshot> sPoses;
std::map<std::string, std::string> sNames;
std::map<std::string, uint8_t> sColorSlots;
std::unique_ptr<NameLabelFontAtlas> sFontAtlas;
ProgressionPromptView sProgressionPrompt;
std::vector<Notification> sNotifications;

PlayerColor color_for_slot(uint8_t slot) {
    static constexpr PlayerColor kColors[8] = {
        {255, 255, 255, 255}, {94, 211, 255, 255}, {255, 214, 92, 255},
        {101, 232, 132, 255}, {255, 133, 203, 255}, {255, 169, 82, 255},
        {184, 160, 255, 255}, {90, 232, 209, 255},
    };
    return kColors[std::min<uint8_t>(slot, 7)];
}

PlayerColor color_for_peer(const std::string& peerId) {
    const auto it = sColorSlots.find(peerId);
    return color_for_slot(it == sColorSlots.end() ? 7 : it->second);
}

std::vector<MinimapMarker> collect_minimap_markers() {
    std::vector<MinimapMarker> markers;
    const char* localStage = dComIfGp_getStartStageName();
    if (!sConnected || !sGameplayReady || localStage == nullptr || localStage[0] == '\0') {
        return markers;
    }
    for (const auto& [peerId, pose] : sPoses) {
        if (!pose.valid || pose.ageTicks > 30 || pose.stage != localStage ||
            !dusk::multiplayer::is_remote_link_dummy_visible(peerId)) continue;
        const PlayerColor color = color_for_peer(peerId);
        markers.push_back({pose.room, pose.x, pose.y, pose.z, pose.angleY, color});
    }
    return markers;
}

Vec transformed_map_pos_for_room(const MinimapMarker& marker) {
    BE(Vec) pos;
    pos.x = marker.x;
    pos.y = marker.y;
    pos.z = marker.z;
    if (dStage_FileList2_dt_c* fileList = dStage_roomControl_c::getFileList2(marker.room)) {
        dMapInfo_n::rotAngle(fileList, &pos);
        dMapInfo_n::offsetPlus(fileList, &pos);
    }
    return pos;
}

bool map_world_to_screen(dMap_c* map, const Vec& mapPos, f32 drawX, f32 drawY, f32 drawW,
                         f32 drawH, f32& outX, f32& outY) {
    if (map == nullptr || map->mTexSizeX == 0 || map->getTexSizeY() == 0) return false;
    const f32 texelPerCm = map->getTexelPerCm();
    const f32 texX = f32(map->mTexSizeX) * 0.5f +
                     (mapPos.x - map->mCenterX) * texelPerCm;
    const f32 texY = f32(map->getTexSizeY()) * 0.5f +
                     (mapPos.z - map->getCenterZ()) * texelPerCm;
    if (texX < 0.0f || texY < 0.0f || texX > map->mTexSizeX || texY > map->getTexSizeY()) {
        return false;
    }
    outX = drawX + texX / f32(map->mTexSizeX) * drawW;
    outY = drawY + texY / f32(map->getTexSizeY()) * drawH;
    return true;
}

void setup_minimap_gx() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_FALSE);
}

void draw_minimap_arrow(f32 x, f32 y, s16 angleY, const PlayerColor& color, u8 alpha,
                        f32 cursorSize, f32 scaleX, f32 scaleY) {
    static constexpr Vec kOffsets[3] = {
        {0.0f, 0.0f, 400.0f}, {-200.0f, 0.0f, -240.0f}, {200.0f, 0.0f, -240.0f},
    };
    const f32 baseScale = cursorSize / 640.0f;
    const f32 sinY = cM_ssin(angleY);
    const f32 cosY = cM_scos(angleY);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (const Vec& offset : kOffsets) {
        const f32 localX = offset.x * baseScale;
        const f32 localZ = offset.z * baseScale;
        GXPosition2f32(x + (localX * cosY + localZ * sinY) * scaleX,
                       y + (localZ * cosY - localX * sinY) * scaleY);
        GXColor4u8(color.r, color.g, color.b,
                   static_cast<u8>((u16(color.a) * alpha) / 255));
    }
    GXEnd();
}

void draw_minimap_markers(dMeterMap_c* meter) {
    dMap_c* map = meter != nullptr ? meter->mMap : nullptr;
    if (map == nullptr || !map->isDraw() || meter->mMapAlpha == 0) return;
    const auto markers = collect_minimap_markers();
    if (markers.empty()) return;

    const f32 unscaledTopOffset = map->getTexSizeY() -
        map->getTexelPerCm() * (map->getPackZ() - map->getPackPlusZ()) -
        map->getTopEdgePlus();
    const f32 bottom = meter->mDrawPosY + meter->mSizeH;
    const f32 hudScale = unscaledTopOffset > 0.001f ?
        std::clamp((bottom - meter->getMapDispEdgeTop()) / unscaledTopOffset, 0.5f, 2.0f) :
        1.0f;
    const f32 drawW = meter->mSizeW * hudScale;
    const f32 drawH = meter->mSizeH * hudScale;
    const f32 drawX = mDoGph_gInf_c::ScaleHUDXLeft(meter->mDrawPosX);
    const f32 drawY = meter->mDrawPosY + meter->mSizeH - drawH;
    const f32 scaleX = drawW / f32(map->mTexSizeX);
    const f32 scaleY = drawH / f32(map->getTexSizeY());
    setup_minimap_gx();
    for (const MinimapMarker& marker : markers) {
        if (marker.room < 0 || marker.room >= 64) continue;
        if (map->isCheckFloor() &&
            dMapInfo_c::calcFloorNo(marker.y, true, marker.room) !=
                dMapInfo_c::getNowStayFloorNo()) continue;
        f32 x = 0.0f, y = 0.0f;
        if (!map_world_to_screen(map, transformed_map_pos_for_room(marker),
                                 drawX, drawY, drawW, drawH, x, y)) continue;
        s16 angle = static_cast<s16>(marker.angleY);
        if (dStage_FileList2_dt_c* fileList = dStage_roomControl_c::getFileList2(marker.room)) {
            angle += fileList->field_0x1c;
        }
        draw_minimap_arrow(x, y, angle, marker.color, meter->mMapAlpha,
                           map->getPlayerCursorSize(), scaleX, scaleY);
    }
}

NameLabelFontAtlas* get_font_atlas() {
    if (sFontAtlas == nullptr) sFontAtlas = std::make_unique<NameLabelFontAtlas>();
    NameLabelFontAtlas& atlas = *sFontAtlas;
    if (atlas.attempted) return atlas.valid ? &atlas : nullptr;
    atlas.attempted = true;
    if (svc_resource->load(mod_ctx, "Inter-Regular.ttf", &atlas.uiFontBuffer) != MOD_OK ||
        atlas.uiFontBuffer.data == nullptr || atlas.uiFontBuffer.size == 0 ||
        svc_resource->load(mod_ctx, "AlegreyaSC-Bold.ttf", &atlas.fontBuffer) != MOD_OK ||
        atlas.fontBuffer.data == nullptr || atlas.fontBuffer.size == 0) return nullptr;

    ImFontConfig uiConfig;
    uiConfig.SizePixels = 18.0f;
    uiConfig.FontDataOwnedByAtlas = false;
    atlas.uiFont = atlas.atlas.AddFontFromMemoryTTF(
        atlas.uiFontBuffer.data, static_cast<int>(atlas.uiFontBuffer.size), uiConfig.SizePixels,
        &uiConfig);
    ImFontConfig config;
    config.SizePixels = 64.0f;
    config.OversampleH = 3;
    config.OversampleV = 3;
    config.PixelSnapH = false;
    config.FontDataOwnedByAtlas = false;
    atlas.font = atlas.atlas.AddFontFromMemoryTTF(
        atlas.fontBuffer.data, static_cast<int>(atlas.fontBuffer.size), config.SizePixels,
        &config);
    if (atlas.uiFont == nullptr || atlas.font == nullptr || !atlas.atlas.Build()) return nullptr;

    unsigned char* alphaPixels = nullptr;
    int bytesPerPixel = 0;
    atlas.atlas.GetTexDataAsAlpha8(&alphaPixels, &atlas.texWidth, &atlas.texHeight,
                                   &bytesPerPixel);
    if (alphaPixels == nullptr || atlas.texWidth <= 0 || atlas.texHeight <= 0) return nullptr;
    atlas.rgbaPixels.resize(size_t(atlas.texWidth) * atlas.texHeight * 4);
    for (int i = 0; i < atlas.texWidth * atlas.texHeight; ++i) {
        atlas.rgbaPixels[i * 4 + 0] = 0xff;
        atlas.rgbaPixels[i * 4 + 1] = 0xff;
        atlas.rgbaPixels[i * 4 + 2] = 0xff;
        atlas.rgbaPixels[i * 4 + 3] = alphaPixels[i];
    }
    GXInitTexObj(&atlas.texObj, atlas.rgbaPixels.data(), static_cast<u16>(atlas.texWidth),
                 static_cast<u16>(atlas.texHeight), GX_TF_RGBA8_PC, GX_CLAMP, GX_CLAMP,
                 GX_FALSE);
    GXInitTexObjLOD(&atlas.texObj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f,
                    GX_FALSE, GX_FALSE, GX_ANISO_1);
    atlas.valid = true;
    return &atlas;
}

bool labels_allowed() {
    if (!sConnected || !sGameplayReady || !sNameLabelsEnabled || !sRemoteModelEnabled ||
        dComIfGp_isPauseFlag() || dScnPly_c::isPause()) return false;
    dMsgObject_c* message = dMsgObject_getMsgObjectClass();
    return message == nullptr || !dMsgObject_isTalkNowCheck();
}

void setup_label_gx(NameLabelFontAtlas& atlas) {
    GXLoadPosMtxImm(j3dSys.getViewMtx(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXLoadTexObj(&atlas.texObj, GX_TEXMAP0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL,
                  GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_FALSE);
    GXSetAlphaCompare(GX_GREATER, 1, GX_AOP_OR, GX_GREATER, 1);
    GXSetCullMode(GX_CULL_NONE);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_CLR_RGBA, GX_RGBX8, 15);
}

void emit_glyph(const cXyz& origin, const cXyz& right, const cXyz& up, f32 scale,
                const ImFontGlyph& glyph, const JUtility::TColor& color) {
    const cXyz p0 = origin + right * (glyph.X0 * scale) - up * (glyph.Y0 * scale);
    const cXyz p1 = origin + right * (glyph.X1 * scale) - up * (glyph.Y0 * scale);
    const cXyz p2 = origin + right * (glyph.X1 * scale) - up * (glyph.Y1 * scale);
    const cXyz p3 = origin + right * (glyph.X0 * scale) - up * (glyph.Y1 * scale);
    const u16 u0 = u16(std::clamp(glyph.U0, 0.0f, 1.0f) * 32767.0f);
    const u16 v0 = u16(std::clamp(glyph.V0, 0.0f, 1.0f) * 32767.0f);
    const u16 u1 = u16(std::clamp(glyph.U1, 0.0f, 1.0f) * 32767.0f);
    const u16 v1 = u16(std::clamp(glyph.V1, 0.0f, 1.0f) * 32767.0f);
    GXPosition3f32(p0.x, p0.y, p0.z); GXColor1u32(color); GXTexCoord2u16(u0, v0);
    GXPosition3f32(p1.x, p1.y, p1.z); GXColor1u32(color); GXTexCoord2u16(u1, v0);
    GXPosition3f32(p2.x, p2.y, p2.z); GXColor1u32(color); GXTexCoord2u16(u1, v1);
    GXPosition3f32(p3.x, p3.y, p3.z); GXColor1u32(color); GXTexCoord2u16(u0, v1);
}

size_t bounded_text_length(const char* text, size_t maxCodeUnits) {
    if (text == nullptr) return 0;
    size_t length = 0;
    while (length < maxCodeUnits && text[length] != '\0') ++length;
    return length;
}

size_t count_visible_glyphs(const NameLabelFontAtlas& atlas, const char* text,
                            size_t codeUnits) {
    size_t count = 0;
    for (size_t i = 0; i < codeUnits; ++i) {
        const ImFontGlyph* glyph = atlas.font->FindGlyph(ImWchar(u8(text[i])));
        if (glyph != nullptr && glyph->Visible) ++count;
    }
    return count;
}

f32 measure_text(const NameLabelFontAtlas& atlas, const char* text, size_t codeUnits) {
    f32 width = 0.0f;
    for (size_t i = 0; i < codeUnits; ++i) {
        const ImFontGlyph* glyph = atlas.font->FindGlyph(ImWchar(u8(text[i])));
        width += glyph != nullptr ? glyph->AdvanceX : atlas.font->FallbackAdvanceX;
    }
    return width;
}

ImVec4 player_status_color(bool local, bool recent, std::string_view status) {
    if (local || recent || status == "connected") {
        return ImVec4(0.34f, 0.92f, 0.44f, 1.0f);
    }
    if (status == "connecting" || status == "joined" || status == "waiting") {
        return ImVec4(0.96f, 0.78f, 0.28f, 1.0f);
    }
    return ImVec4(0.95f, 0.35f, 0.32f, 1.0f);
}

void draw_player_status_dot(const ImVec4& color) {
    const float radius = ImGui::GetTextLineHeight() * 0.28f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 center(pos.x + radius, pos.y + ImGui::GetTextLineHeight() * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        center, radius, ImGui::GetColorU32(color), 16);
    ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetTextLineHeight()));
}

void draw_imgui_player_list() {
    if (!sConnected || !sPlayerListEnabled) return;
    struct Row {
        std::string name;
        std::string status;
        std::string area;
        bool local = false;
        bool recent = false;
    };
    std::vector<Row> rows;
    const char* localStage = dComIfGp_getStartStageName();
    rows.push_back({sLocalName.empty() ? "You" : sLocalName,
                    sLocalStatus.empty() ? "connected" : sLocalStatus,
                    localStage != nullptr && localStage[0] != '\0' ?
                        std::string(localStage) + " / room " +
                            std::to_string(int(dComIfGp_roomControl_getStayNo())) : "Unknown",
                    true, true});
    for (const auto& [peerId, peerName] : sNames) {
        const auto pose = sPoses.find(peerId);
        const bool valid = pose != sPoses.end() && pose->second.valid;
        const bool recent = valid && pose->second.ageTicks <= 90;
        rows.push_back({peerName.empty() ? peerId : peerName,
                        valid ? (recent ? "online" : "stale") : "waiting",
                        valid && !pose->second.stage.empty() ?
                            pose->second.stage + " / room " +
                                std::to_string(pose->second.room) : "Unknown",
                        false, recent});
    }
    if (rows.empty()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 windowPos(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                           viewport->WorkPos.y + 42.0f);
    const float width = std::clamp(viewport->WorkSize.x * 0.54f, 420.0f, 760.0f);
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.02f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.55f, 0.55f, 0.65f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Online Player List", nullptr, flags)) {
        const std::string title = (sRoom.empty() ? "Online" : sRoom) + " - " +
            std::to_string(rows.size()) + (rows.size() == 1 ? " player" : " players");
        const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleSize.x) * 0.5f);
        ImGui::TextUnformatted(title.c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("OnlinePlayers", 3,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoSavedSettings,
                              ImVec2(-1.0f, 0.0f))) {
            ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.22f);
            ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthStretch, 0.40f);
            ImGui::TableHeadersRow();
            for (const Row& row : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                draw_player_status_dot(player_status_color(row.local, row.recent, row.status));
                ImGui::SameLine();
                ImGui::TextUnformatted(row.name.c_str());
                if (row.local) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(you)");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.status.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.area.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void draw_imgui_progression_prompt() {
    if (!sConnected || !sProgressionPrompt.active) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workPos = viewport != nullptr ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
    const ImVec2 workSize = viewport != nullptr ? viewport->WorkSize : ImVec2(1280.0f, 720.0f);
    const ImVec2 windowSize(360.0f, 112.0f);
    ImGui::SetNextWindowPos(
        ImVec2(workPos.x + workSize.x - windowSize.x - 24.0f, workPos.y + 72.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.86f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.06f, 0.86f));
    if (ImGui::Begin("Multiplayer Progression Sync", nullptr, flags)) {
        const bool waiting = sProgressionPrompt.waiting;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          IM_COL32(245, 193, 51, 230), 6.0f, 0, 2.0f);
        ImGui::PushTextWrapPos(pos.x + 276.0f);
        ImGui::TextUnformatted(sProgressionPrompt.title.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.85f, 0.92f, 1.0f));
        ImGui::TextUnformatted(sProgressionPrompt.body.c_str());
        if (waiting) {
            ImGui::TextUnformatted("Please wait");
        } else {
            ImGui::Text("%.0fs", std::ceil(8.0f * sProgressionPrompt.remainingRatio));
        }
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        const ImVec2 ringCenter(pos.x + size.x - 46.0f, pos.y + size.y * 0.5f);
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float ringRadius = 20.0f;
        drawList->AddCircle(ringCenter, ringRadius, IM_COL32(255, 255, 255, 70), 48, 3.0f);
        drawList->AddCircle(ringCenter, ringRadius - 7.0f,
                            IM_COL32(255, 255, 255, 45), 48, 2.0f);
        if (waiting) {
            const float t = std::fmod(sProgressionPrompt.ageSeconds * 1.1f, 1.0f);
            drawList->PathArcTo(ringCenter, ringRadius, -0.5f * kPi + 2.0f * kPi * t,
                                -0.5f * kPi + 2.0f * kPi * (t + 0.72f), 48);
            drawList->PathStroke(IM_COL32(255, 176, 38, 255), 0, 5.0f);
        } else if (sProgressionPrompt.holdRatio > 0.0f) {
            drawList->PathArcTo(ringCenter, ringRadius, -0.5f * kPi,
                                -0.5f * kPi + 2.0f * kPi * sProgressionPrompt.holdRatio, 48);
            drawList->PathStroke(IM_COL32(255, 176, 38, 255), 0, 5.0f);
        }
        if (waiting) {
            drawList->AddText(ImVec2(ringCenter.x - 5.0f, ringCenter.y - 8.0f),
                              IM_COL32(255, 255, 255, 245), "...");
        } else {
            const ImVec2 barMin(pos.x, pos.y + size.y - 3.0f);
            drawList->AddRectFilled(
                barMin, ImVec2(pos.x + size.x * sProgressionPrompt.remainingRatio,
                               pos.y + size.y),
                IM_COL32(255, 176, 38, 210), 0.0f);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void draw_imgui_notifications() {
    if (!sConnected || sNotifications.empty()) return;
    const float dt = ImGui::GetIO().DeltaTime;
    for (Notification& notification : sNotifications) notification.ageSeconds += dt;
    std::erase_if(sNotifications, [](const Notification& notification) {
        return notification.ageSeconds >= notification.durationSeconds;
    });
    if (sNotifications.empty()) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workPos = viewport != nullptr ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
    ImGui::SetNextWindowPos(ImVec2(workPos.x + 16.0f, workPos.y + 44.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.58f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("Multiplayer Notices", nullptr, flags)) {
        for (const Notification& notification : sNotifications) {
            const float remaining = notification.durationSeconds - notification.ageSeconds;
            const float alpha = remaining < 1.0f ? remaining : 1.0f;
            if (!notification.playerName.empty()) {
                const PlayerColor color = notification.playerColor;
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                           (color.a / 255.0f) * alpha));
                ImGui::TextUnformatted(notification.playerName.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 0.0f);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.97f, 1.0f, alpha));
            ImGui::TextUnformatted(notification.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

void draw_host_imgui_overlays() {
    draw_imgui_player_list();
    draw_imgui_progression_prompt();
    draw_imgui_notifications();
}

void draw_text_run(NameLabelFontAtlas& atlas, const cXyz& origin, const cXyz& right,
                   const cXyz& up, f32 scale, const JUtility::TColor& color,
                   const char* text, size_t codeUnits) {
    cXyz cursorOrigin = origin;
    for (size_t i = 0; i < codeUnits; ++i) {
        const ImFontGlyph* glyph = atlas.font->FindGlyph(ImWchar(u8(text[i])));
        if (glyph != nullptr && glyph->Visible) {
            emit_glyph(cursorOrigin, right, up, scale, *glyph, color);
        }
        cursorOrigin += right * ((glyph != nullptr ? glyph->AdvanceX :
                                  atlas.font->FallbackAdvanceX) * scale);
    }
}

void draw_world_text(NameLabelFontAtlas& atlas, const cXyz& worldPos,
                     const JUtility::TColor& color, const char* text) {
    constexpr size_t kVerticesPerGlyph = 4;
    constexpr size_t kDrawPasses = 9;
    constexpr size_t kMaxCodeUnits = UINT16_MAX / (kVerticesPerGlyph * kDrawPasses);
    const size_t codeUnits = bounded_text_length(text, kMaxCodeUnits);
    const size_t visibleGlyphs = count_visible_glyphs(atlas, text, codeUnits);
    Mtx invView;
    if (visibleGlyphs == 0 || !MTXInverse(j3dSys.getViewMtx(), invView)) return;
    const cXyz right(invView[0][0], invView[1][0], invView[2][0]);
    const cXyz up(invView[0][1], invView[1][1], invView[2][1]);
    constexpr f32 kScale = 0.34f;
    constexpr f32 kOutlineOffset = 2.35f;
    const cXyz origin = worldPos - right * ((measure_text(atlas, text, codeUnits) * 0.5f - 2.5f) * kScale) +
                        up * (atlas.font->Ascent * kScale + 16.0f);
    const JUtility::TColor outline(0, 0, 0, color.a);
    static const cXyz kOffsets[] = {
        {-1.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f},  {-1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},   {1.0f, 1.0f, 0.0f},
    };
    GXBegin(GX_QUADS, GX_VTXFMT0,
            static_cast<u16>(visibleGlyphs * kDrawPasses * kVerticesPerGlyph));
    for (const cXyz& offset : kOffsets) {
        draw_text_run(atlas,
            origin + right * (offset.x * kOutlineOffset * kScale) -
                     up * (offset.y * kOutlineOffset * kScale),
            right, up, kScale, outline, text, codeUnits);
    }
    draw_text_run(atlas, origin, right, up, kScale, color, text, codeUnits);
    GXEnd();
}

void draw_name_labels() {
    if (!labels_allowed()) return;
    const char* localStage = dComIfGp_getStartStageName();
    camera_process_class* camera = dComIfGp_getCamera(0);
    NameLabelFontAtlas* atlas = get_font_atlas();
    if (localStage == nullptr || camera == nullptr || atlas == nullptr) return;

    struct Label { std::string peerId; std::string text; cXyz pos; f32 distance; };
    std::vector<Label> labels;
    for (const auto& [peerId, pose] : sPoses) {
        if (!pose.valid || pose.ageTicks > 30 || pose.stage != localStage ||
            !dComIfGp_roomControl_checkRoomDisp(pose.room)) continue;
        cXyz pos;
        if (!dusk::multiplayer::get_remote_link_dummy_label_position(peerId, &pos)) continue;
        cXyz cameraPos;
        mDoLib_pos2camera(&pos, &cameraPos);
        if (cameraPos.z >= -1.0f) continue;
        const auto name = sNames.find(peerId);
        const std::string text = name != sNames.end() && !name->second.empty() ?
                                     name->second : peerId;
        labels.push_back({peerId, text, pos, (pos - camera->view.lookat.eye).abs2()});
    }
    std::sort(labels.begin(), labels.end(),
              [](const Label& a, const Label& b) { return a.distance > b.distance; });
    if (labels.empty()) return;
    setup_label_gx(*atlas);
    for (const Label& label : labels) {
        const PlayerColor value = color_for_peer(label.peerId);
        draw_world_text(*atlas, label.pos,
                        JUtility::TColor(value.r, value.g, value.b, value.a),
                        label.text.c_str());
    }
    J3DShape::resetVcdVatCache();
}

void opaque_draw_list_post(ModContext*, void* args, void*, void*) {
    auto* list = mods::arg<dDlst_list_c*>(args, 0);
    auto* buffer = mods::arg<J3DDrawBuffer*>(args, 1);
    if (list == nullptr || buffer != list->mDrawBuffers[dDlst_list_c::DB_LIST_3D_LAST]) return;
    draw_name_labels();
    j3dSys.reinitGX();
    GXSetClipMode(GX_CLIP_ENABLE);
}

void meter_map_draw_post(ModContext*, void* args, void*, void*) {
    draw_minimap_markers(mods::arg<dMeterMap_c*>(args, 0));
}

void host_imgui_post_draw_post(ModContext*, void*, void*, void*) {
    using GetContextFn = ImGuiContext* (*)();
    const auto getHostContext = reinterpret_cast<GetContextFn>(
        HostImGuiGetCurrentContextSymbol::resolved_target());
    if (getHostContext == nullptr) return;
    ImGuiContext* const hostContext = getHostContext();
    if (hostContext == nullptr) return;

    // This DLL embeds the exact same ImGui release as the host. Point its
    // translation-unit-local GImGui at the host context only for this draw,
    // so commands join Aurora's external overlay pass and inherit its DPI,
    // viewport, fonts, and framebuffer scaling.
    ImGuiContext* const previousContext = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(hostContext);
    draw_host_imgui_overlays();
    ImGui::SetCurrentContext(previousContext);
}

}  // namespace

ModResult install_visual_hooks(ModError* error) {
    if (HostImGuiGetCurrentContextSymbol::resolved_target() == nullptr ||
        mods::hook::add_post<OpaqueDrawListHook>(&opaque_draw_list_post) != MOD_OK ||
        mods::hook::add_post<HostImGuiPostDrawHook>(&host_imgui_post_draw_post) != MOD_OK ||
        mods::hook::add_post<MeterMapDrawHook>(&meter_map_draw_post) != MOD_OK) {
        uninstall_visual_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE, "Online visual draw hooks are unavailable");
    }
    return MOD_OK;
}

void uninstall_visual_hooks() {
    mods::hook::uninstall<MeterMapDrawHook>();
    mods::hook::uninstall<HostImGuiPostDrawHook>();
    mods::hook::uninstall<OpaqueDrawListHook>();
    reset_visual_overlays();
    if (sFontAtlas != nullptr) {
        svc_resource->free(mod_ctx, &sFontAtlas->fontBuffer);
        svc_resource->free(mod_ctx, &sFontAtlas->uiFontBuffer);
        sFontAtlas.reset();
    }
}

void update_visual_overlays(
    bool connected, bool gameplayReady, bool nameLabelsEnabled, bool remoteModelEnabled,
    bool playerListEnabled, std::string_view room, std::string_view localStatus,
    std::string_view localName, uint8_t localColorSlot,
    const std::map<std::string, PeerPoseSnapshot>& poses,
    const std::map<std::string, std::string>& names,
    const std::map<std::string, uint8_t>& colorSlots,
    const ProgressionPromptView& progressionPrompt) {
    sConnected = connected;
    sGameplayReady = gameplayReady;
    sNameLabelsEnabled = nameLabelsEnabled;
    sRemoteModelEnabled = remoteModelEnabled;
    sPlayerListEnabled = playerListEnabled;
    sRoom = room;
    sLocalStatus = localStatus;
    sLocalName = localName;
    sLocalColorSlot = localColorSlot;
    sPoses = poses;
    sNames = names;
    sColorSlots = colorSlots;
    sProgressionPrompt = progressionPrompt;
}

void push_online_notification(std::string text, float durationSeconds) {
    if (text.empty()) return;
    sNotifications.push_back({{}, std::move(text), {}, 0.0f, durationSeconds});
    if (sNotifications.size() > 5) sNotifications.erase(sNotifications.begin());
}

void push_online_player_notification(std::string playerName, std::string text,
                                     uint8_t colorSlot, float durationSeconds) {
    if (playerName.empty() || text.empty()) return;
    sNotifications.push_back({std::move(playerName), std::move(text),
                              color_for_slot(colorSlot), 0.0f, durationSeconds});
    if (sNotifications.size() > 5) sNotifications.erase(sNotifications.begin());
}

void reset_visual_overlays() {
    sConnected = false;
    sGameplayReady = false;
    sPlayerListEnabled = false;
    sPoses.clear();
    sNames.clear();
    sColorSlots.clear();
    sRoom.clear();
    sLocalStatus.clear();
    sLocalName.clear();
    sLocalColorSlot = 0;
    sProgressionPrompt = {};
    sNotifications.clear();
}

}  // namespace dusklight_online::game
