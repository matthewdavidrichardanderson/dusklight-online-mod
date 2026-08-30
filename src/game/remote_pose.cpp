#include "dusklight_online/game/remote_pose.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dusklight_online::game {
namespace {

using nlohmann::json;
using namespace dusk::multiplayer;

constexpr uint8_t kPackedMatrixModeFloat32 = 0;
constexpr uint8_t kPackedMatrixModeQuantizedBasis = 1;
constexpr uint8_t kPackedModelWeightsIncluded = 1 << 0;
constexpr uint8_t kDeltaUnchanged = 0;
constexpr uint8_t kDeltaFull = 1;
constexpr uint8_t kDeltaMask = 2;
constexpr uint8_t kDeltaResidual = 3;
constexpr uint8_t kResidualUnchanged = 0;
constexpr uint8_t kResidualSmall = 1;
constexpr uint8_t kResidualFull = 2;
constexpr float kResidualTranslationScale = 16.0f;
constexpr size_t kMatrixHistoryLimit = 300;
std::map<std::string, std::map<uint32_t, std::string>> sMatrixHistory;

bool decode_base64(std::string_view text, std::string& out) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(alphabet[i])] = int8_t(i);
    if (text.empty() || text.size() % 4 != 0) return false;
    out.clear();
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        uint32_t bits = 0;
        int pads = 0;
        for (size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=' && i + 4 == text.size() && j >= 2) {
                ++pads;
                bits <<= 6;
            } else {
                const int value = table[static_cast<uint8_t>(c)];
                if (value < 0 || pads != 0) return false;
                bits = (bits << 6) | uint32_t(value);
            }
        }
        out.push_back(char((bits >> 16) & 0xff));
        if (pads < 2) out.push_back(char((bits >> 8) & 0xff));
        if (pads < 1) out.push_back(char(bits & 0xff));
    }
    return true;
}

bool packed_bytes(const json& source, std::string& out) {
    const auto it = source.find("data");
    if (it == source.end()) return false;
    if (it->is_binary()) {
        const auto& bytes = it->get_binary();
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }
    return it->is_string() && decode_base64(it->get_ref<const std::string&>(), out);
}

std::string history_key(const std::string& peerId, uint8_t packetType) {
    return peerId + '\x1f' + std::to_string(packetType);
}

std::optional<std::string> base_format_for_delta(const std::string& format) {
    if (format == "qrot16_trans32_womit_delta_bin_v1")
        return "qrot16_trans32_womit_bin_v1";
    if (format == "qrot16_trans32_womit_delta_v1")
        return "qrot16_trans32_womit_v1";
    if (format == "f32_pack_womit_delta_bin_v1") return "f32_pack_womit_bin_v1";
    if (format == "f32_pack_womit_delta_v1") return "f32_pack_womit_v1";
    return std::nullopt;
}

std::optional<std::string> delta_format_for_base(const std::string& format) {
    if (format == "qrot16_trans32_womit_bin_v1")
        return "qrot16_trans32_womit_delta_bin_v1";
    if (format == "qrot16_trans32_womit_v1")
        return "qrot16_trans32_womit_delta_v1";
    if (format == "f32_pack_womit_bin_v1") return "f32_pack_womit_delta_bin_v1";
    if (format == "f32_pack_womit_v1") return "f32_pack_womit_delta_v1";
    return std::nullopt;
}

bool is_delta_capable_base(const std::string& format) {
    return format == "qrot16_trans32_womit_bin_v1" ||
           format == "qrot16_trans32_womit_v1" ||
           format == "f32_pack_womit_bin_v1" || format == "f32_pack_womit_v1";
}

size_t matrix_bytes_for_format(const std::string& format) {
    if (format == "qrot16_trans32_womit_bin_v1" ||
        format == "qrot16_trans32_womit_v1") return 30;
    if (format == "f32_pack_womit_bin_v1" || format == "f32_pack_womit_v1") return 48;
    return 0;
}

bool read_bytes(const std::string& source, size_t& cursor, void* out, size_t size) {
    if (cursor > source.size() || size > source.size() - cursor) return false;
    std::memcpy(out, source.data() + cursor, size);
    cursor += size;
    return true;
}

template <typename T>
bool read_value(const std::string& source, size_t& cursor, T& out) {
    return read_bytes(source, cursor, &out, sizeof(out));
}

bool split_slots(const std::string& format, const std::string& packed,
                 std::string& header, std::vector<std::string>& slots) {
    const size_t matrixBytes = matrix_bytes_for_format(format);
    if (matrixBytes == 0) return false;
    size_t cursor = 0;
    char magic[4]{};
    uint8_t version = 0, count = 0;
    if (!read_bytes(packed, cursor, magic, 4) || std::memcmp(magic, "DMPM", 4) != 0 ||
        !read_value(packed, cursor, version) || !read_value(packed, cursor, count) ||
        version != 1 || count != 21) return false;
    header.assign(packed.data(), cursor);
    slots.clear();
    slots.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        const size_t start = cursor;
        uint8_t present = 0;
        if (!read_value(packed, cursor, present)) return false;
        if (present == 0) { slots.emplace_back(packed.data() + start, cursor - start); continue; }
        uint16_t joints = 0, weights = 0;
        uint8_t flags = 0;
        if (present != 1 || !read_value(packed, cursor, joints) ||
            !read_value(packed, cursor, weights) || !read_value(packed, cursor, flags)) return false;
        const bool hasWeights = (flags & kPackedModelWeightsIncluded) != 0 || weights == 0;
        const size_t matrices = 1 + size_t(joints) + (hasWeights ? size_t(weights) : 0);
        if (matrices > 1024 || cursor > packed.size() ||
            matrices * matrixBytes > packed.size() - cursor) return false;
        cursor += matrices * matrixBytes;
        slots.emplace_back(packed.data() + start, cursor - start);
    }
    return cursor == packed.size();
}

size_t slot_matrix_count(const std::string& slot, size_t matrixBytes) {
    if (slot.size() < 6 || uint8_t(slot[0]) != 1) return 0;
    size_t cursor = 1;
    uint16_t joints = 0, weights = 0;
    uint8_t flags = 0;
    if (!read_value(slot, cursor, joints) || !read_value(slot, cursor, weights) ||
        !read_value(slot, cursor, flags)) return 0;
    const bool hasWeights = (flags & kPackedModelWeightsIncluded) != 0 || weights == 0;
    const size_t count = 1 + size_t(joints) + (hasWeights ? size_t(weights) : 0);
    return count <= 1024 && slot.size() == 6 + count * matrixBytes ? count : 0;
}

template <typename T>
void append_value(std::string& target, const T& value) {
    target.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool build_mask_delta(const std::string& current, const std::string& baseline,
                      size_t matrixBytes, std::string& payload) {
    if (current.size() < 6 || baseline.size() < 6 ||
        current.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(current, matrixBytes);
    if (count == 0 || count != slot_matrix_count(baseline, matrixBytes)) return false;
    const size_t maskBytes = (count + 7) / 8;
    payload.assign(current.data(), 6);
    const size_t maskOffset = payload.size();
    payload.append(maskBytes, '\0');
    size_t changed = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = 6 + i * matrixBytes;
        if (std::memcmp(current.data() + offset, baseline.data() + offset,
                        matrixBytes) == 0) continue;
        payload[maskOffset + i / 8] = static_cast<char>(
            static_cast<uint8_t>(payload[maskOffset + i / 8]) |
            static_cast<uint8_t>(1u << (i % 8)));
        payload.append(current.data() + offset, matrixBytes);
        ++changed;
    }
    return changed != 0 && payload.size() < current.size();
}

bool build_small_residual(const char* current, const char* baseline,
                          std::string& payload) {
    for (size_t i = 0; i < 9; ++i) {
        int16_t now = 0, old = 0;
        std::memcpy(&now, current + i * sizeof(int16_t), sizeof(now));
        std::memcpy(&old, baseline + i * sizeof(int16_t), sizeof(old));
        const int delta = int(now) - int(old);
        if (delta < (std::numeric_limits<int8_t>::min)() ||
            delta > (std::numeric_limits<int8_t>::max)()) return false;
        append_value(payload, static_cast<int8_t>(delta));
    }
    for (size_t i = 0; i < 3; ++i) {
        float now = 0.0f, old = 0.0f;
        std::memcpy(&now, current + 18 + i * sizeof(float), sizeof(now));
        std::memcpy(&old, baseline + 18 + i * sizeof(float), sizeof(old));
        const float scaled = std::round((now - old) * kResidualTranslationScale);
        if (!std::isfinite(scaled) ||
            scaled < float((std::numeric_limits<int16_t>::min)()) ||
            scaled > float((std::numeric_limits<int16_t>::max)())) return false;
        append_value(payload, static_cast<int16_t>(scaled));
    }
    return true;
}

bool build_residual_delta(const std::string& current, const std::string& baseline,
                          size_t matrixBytes, std::string& payload) {
    if (matrixBytes != 30 || current.size() < 6 || baseline.size() < 6 ||
        current.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(current, matrixBytes);
    if (count == 0 || count != slot_matrix_count(baseline, matrixBytes)) return false;
    payload.assign(current.data(), 6);
    const size_t modesOffset = payload.size();
    payload.append(count, static_cast<char>(kResidualUnchanged));
    size_t changed = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = 6 + i * matrixBytes;
        const char* now = current.data() + offset;
        const char* old = baseline.data() + offset;
        if (std::memcmp(now, old, matrixBytes) == 0) continue;
        std::string residual;
        if (build_small_residual(now, old, residual) && residual.size() == 15) {
            payload[modesOffset + i] = static_cast<char>(kResidualSmall);
            payload += residual;
        } else {
            payload[modesOffset + i] = static_cast<char>(kResidualFull);
            payload.append(now, matrixBytes);
        }
        ++changed;
    }
    return changed != 0 && payload.size() < current.size();
}

bool build_slot_delta(const std::string& format, const std::string& current,
                      const std::string& baseline, uint32_t baselineSequence,
                      std::string& delta) {
    const size_t matrixBytes = matrix_bytes_for_format(format);
    std::string currentHeader, baselineHeader;
    std::vector<std::string> currentSlots, baselineSlots;
    if (matrixBytes == 0 ||
        !split_slots(format, current, currentHeader, currentSlots) ||
        !split_slots(format, baseline, baselineHeader, baselineSlots) ||
        currentHeader != baselineHeader || currentSlots.size() != baselineSlots.size()) {
        return false;
    }
    delta = "DMPD";
    append_value(delta, uint8_t{1});
    append_value(delta, static_cast<uint8_t>(currentSlots.size()));
    append_value(delta, baselineSequence);
    for (size_t i = 0; i < currentSlots.size(); ++i) {
        if (currentSlots[i] == baselineSlots[i]) {
            append_value(delta, kDeltaUnchanged);
            continue;
        }
        std::string payload;
        uint8_t mode = kDeltaFull;
        if (build_residual_delta(currentSlots[i], baselineSlots[i], matrixBytes, payload)) {
            mode = kDeltaResidual;
        } else if (build_mask_delta(currentSlots[i], baselineSlots[i], matrixBytes, payload)) {
            mode = kDeltaMask;
        } else {
            payload = currentSlots[i];
        }
        if (payload.size() > (std::numeric_limits<uint16_t>::max)()) return false;
        append_value(delta, mode);
        append_value(delta, static_cast<uint16_t>(payload.size()));
        delta += payload;
    }
    return true;
}

bool apply_mask_delta(const std::string& payload, const std::string& baseline,
                      size_t matrixBytes, std::string& current) {
    if (payload.size() < 6 || baseline.size() < 6 ||
        payload.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(baseline, matrixBytes);
    if (count == 0) return false;
    const size_t maskBytes = (count + 7) / 8;
    size_t cursor = 6 + maskBytes;
    if (payload.size() < cursor) return false;
    current = baseline;
    for (size_t i = 0; i < count; ++i) {
        if ((uint8_t(payload[6 + i / 8]) & uint8_t(1u << (i % 8))) == 0) continue;
        if (matrixBytes > payload.size() - cursor) return false;
        std::memcpy(current.data() + 6 + i * matrixBytes,
                    payload.data() + cursor, matrixBytes);
        cursor += matrixBytes;
    }
    return cursor == payload.size();
}

void apply_small_residual(const char* baseline, const char* residual, char* current) {
    for (size_t i = 0; i < 9; ++i) {
        int16_t value = 0;
        std::memcpy(&value, baseline + i * sizeof(int16_t), sizeof(value));
        value = int16_t(int(value) + int(*reinterpret_cast<const int8_t*>(residual + i)));
        std::memcpy(current + i * sizeof(int16_t), &value, sizeof(value));
    }
    for (size_t i = 0; i < 3; ++i) {
        float base = 0.0f;
        int16_t delta = 0;
        std::memcpy(&base, baseline + 18 + i * sizeof(float), sizeof(base));
        std::memcpy(&delta, residual + 9 + i * sizeof(int16_t), sizeof(delta));
        const float value = base + float(delta) / kResidualTranslationScale;
        std::memcpy(current + 18 + i * sizeof(float), &value, sizeof(value));
    }
}

bool apply_residual_delta(const std::string& payload, const std::string& baseline,
                          size_t matrixBytes, std::string& current) {
    if (matrixBytes != 30 || payload.size() < 6 || baseline.size() < 6 ||
        payload.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(baseline, matrixBytes);
    if (count == 0 || payload.size() < 6 + count) return false;
    current = baseline;
    size_t cursor = 6 + count;
    constexpr size_t smallBytes = 15;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t mode = uint8_t(payload[6 + i]);
        char* destination = current.data() + 6 + i * matrixBytes;
        if (mode == kResidualUnchanged) continue;
        if (mode == kResidualSmall) {
            if (smallBytes > payload.size() - cursor) return false;
            apply_small_residual(baseline.data() + 6 + i * matrixBytes,
                                 payload.data() + cursor, destination);
            cursor += smallBytes;
        } else if (mode == kResidualFull) {
            if (matrixBytes > payload.size() - cursor) return false;
            std::memcpy(destination, payload.data() + cursor, matrixBytes);
            cursor += matrixBytes;
        } else return false;
    }
    return cursor == payload.size();
}

bool apply_slot_delta(const std::string& deltaFormat, const std::string& delta,
                      const std::string& baseline, uint32_t expectedSequence,
                      std::string& current) {
    const auto baseFormat = base_format_for_delta(deltaFormat);
    if (!baseFormat) return false;
    const size_t matrixBytes = matrix_bytes_for_format(*baseFormat);
    std::string header;
    std::vector<std::string> baselineSlots;
    if (!split_slots(*baseFormat, baseline, header, baselineSlots)) return false;
    size_t cursor = 0;
    char magic[4]{};
    uint8_t version = 0, count = 0;
    uint32_t sequence = 0;
    if (!read_bytes(delta, cursor, magic, 4) || std::memcmp(magic, "DMPD", 4) != 0 ||
        !read_value(delta, cursor, version) || !read_value(delta, cursor, count) ||
        !read_value(delta, cursor, sequence) || version != 1 ||
        sequence != expectedSequence || count != baselineSlots.size()) return false;
    current = header;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t mode = 0;
        if (!read_value(delta, cursor, mode)) return false;
        if (mode == kDeltaUnchanged) { current += baselineSlots[i]; continue; }
        uint16_t bytes = 0;
        if ((mode != kDeltaFull && mode != kDeltaMask && mode != kDeltaResidual) ||
            !read_value(delta, cursor, bytes) || bytes > delta.size() - cursor) return false;
        if (mode == kDeltaFull) current.append(delta.data() + cursor, bytes);
        else {
            const std::string payload(delta.data() + cursor, bytes);
            std::string slot;
            const bool ok = mode == kDeltaMask ?
                apply_mask_delta(payload, baselineSlots[i], matrixBytes, slot) :
                apply_residual_delta(payload, baselineSlots[i], matrixBytes, slot);
            if (!ok) return false;
            current += slot;
        }
        cursor += bytes;
    }
    return cursor == delta.size();
}

float dequantize_basis(int16_t value) {
    return static_cast<float>(value) / 32767.0f;
}

bool read_quantized_matrix(const std::string& source, size_t& cursor, float* out) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            int16_t value = 0;
            if (!read_value(source, cursor, value)) return false;
            out[row * 4 + col] = dequantize_basis(value);
        }
    }
    for (int row = 0; row < 3; ++row) {
        if (!read_value(source, cursor, out[row * 4 + 3])) return false;
    }
    return true;
}

bool read_matrix(const std::string& source, size_t& cursor, float* out,
                 bool quantized, bool safeQuantized) {
    if (safeQuantized) {
        uint8_t mode = 0;
        if (!read_value(source, cursor, mode)) return false;
        if (mode == kPackedMatrixModeFloat32)
            return read_bytes(source, cursor, out, sizeof(float) * 12);
        if (mode != kPackedMatrixModeQuantizedBasis) return false;
        return read_quantized_matrix(source, cursor, out);
    }
    return quantized ? read_quantized_matrix(source, cursor, out) :
                       read_bytes(source, cursor, out, sizeof(float) * 12);
}

bool read_model(const std::string& packed, size_t& cursor,
                RemoteModelMatrixSnapshot& model, bool quantized,
                bool safeQuantized, bool omitWeights) {
    model = {};
    uint8_t present = 0;
    if (!read_value(packed, cursor, present)) return false;
    if (present == 0) return true;
    if (present != 1 || !read_value(packed, cursor, model.jointCount) ||
        !read_value(packed, cursor, model.weightCount)) return false;
    bool weightsIncluded = true;
    if (omitWeights) {
        uint8_t flags = 0;
        if (!read_value(packed, cursor, flags)) return false;
        weightsIncluded = (flags & kPackedModelWeightsIncluded) != 0 || model.weightCount == 0;
        model.weightsOmitted = !weightsIncluded && model.weightCount > 0;
    }
    const size_t jointFloats = size_t(model.jointCount) * 12;
    const size_t weightFloats = size_t(model.weightCount) * 12;
    if (jointFloats > 64 * 1024 * 12 || weightFloats > 64 * 1024 * 12) return false;
    model.joints.resize(jointFloats);
    model.weights.resize(weightsIncluded ? weightFloats : 0);
    if (!read_matrix(packed, cursor, model.base.data(), quantized, safeQuantized)) return false;
    for (size_t i = 0; i < model.jointCount; ++i)
        if (!read_matrix(packed, cursor, model.joints.data() + i * 12,
                         quantized, safeQuantized)) return false;
    if (weightsIncluded) {
        for (size_t i = 0; i < model.weightCount; ++i)
            if (!read_matrix(packed, cursor, model.weights.data() + i * 12,
                             quantized, safeQuantized)) return false;
    }
    model.valid = true;
    return true;
}

bool read_matrix_array(const json& source, std::array<float, 12>& out) {
    if (!source.is_array() || source.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) {
        if (!source[i].is_number()) return false;
        out[i] = source[i].get<float>();
    }
    return true;
}

RemoteModelMatrixSnapshot parse_json_model(const json& source) {
    RemoteModelMatrixSnapshot out;
    if (!source.is_object()) return out;
    const int joints = source.value("joint_count", -1);
    const int weights = source.value("weight_count", -1);
    if (joints < 0 || weights < 0 || joints > 65535 || weights > 65535) return out;
    out.jointCount = uint16_t(joints);
    out.weightCount = uint16_t(weights);
    const auto jointData = source.value("joints", json::array());
    const auto weightData = source.value("weights", json::array());
    if (!read_matrix_array(source.value("base", json::array()), out.base) ||
        !jointData.is_array() || jointData.size() != size_t(joints) * 12 ||
        !weightData.is_array() || weightData.size() != size_t(weights) * 12) return {};
    out.joints.reserve(jointData.size());
    out.weights.reserve(weightData.size());
    for (const auto& value : jointData) {
        if (!value.is_number()) return {};
        out.joints.push_back(value.get<float>());
    }
    for (const auto& value : weightData) {
        if (!value.is_number()) return {};
        out.weights.push_back(value.get<float>());
    }
    out.valid = true;
    return out;
}

RemoteLinkMatrixSnapshot parse_matrices(const json& source) {
    RemoteLinkMatrixSnapshot out;
    if (!source.is_object()) return out;
    const std::string format = source.value("format", "");
    const bool quantized = format == "qrot16_trans32_v1" ||
        format == "qrot16_trans32_bin_v1" ||
        format == "qrot16_trans32_womit_v1" ||
        format == "qrot16_trans32_womit_bin_v1";
    const bool safe = format == "qbasis16_trans32_safe_v1" ||
        format == "qbasis16_trans32_safe_bin_v1";
    const bool floating = format == "f32_pack_v1" || format == "f32_pack_bin_v1" ||
        format == "f32_pack_womit_v1" || format == "f32_pack_womit_bin_v1";
    const bool omit = format == "qrot16_trans32_womit_v1" ||
        format == "qrot16_trans32_womit_bin_v1" ||
        format == "f32_pack_womit_v1" || format == "f32_pack_womit_bin_v1";
    const bool packedFormat = quantized || safe || floating;
    if (packedFormat) {
        std::string packed;
        if (!packed_bytes(source, packed)) return out;
        size_t cursor = 0;
        char magic[4]{};
        uint8_t version = 0, count = 0;
        if (!read_bytes(packed, cursor, magic, 4) || std::memcmp(magic, "DMPM", 4) != 0 ||
            !read_value(packed, cursor, version) || !read_value(packed, cursor, count) ||
            version != 1 || count != 21) return {};
        RemoteModelMatrixSnapshot ignoredMidna[5];
        RemoteModelMatrixSnapshot* slots[] = {
            &out.body, &out.hat, &out.face, &out.hand, &out.sword, &out.sheath,
            &out.shield, &out.heldItem, &out.hookTip, &out.hookSubItem, &out.hookSubTip,
            &out.arrow, &out.kantera, &out.kanteraGlow, &out.itemActor, &out.rideActor,
            &ignoredMidna[0], &ignoredMidna[1], &ignoredMidna[2], &ignoredMidna[3],
            &ignoredMidna[4],
        };
        for (auto* slot : slots)
            if (!read_model(packed, cursor, *slot, quantized, safe, omit)) return {};
        if (cursor != packed.size()) return {};
    } else {
        out.body = parse_json_model(source.value("body", json::object()));
        out.hat = parse_json_model(source.value("hat", json::object()));
        out.face = parse_json_model(source.value("face", json::object()));
        out.hand = parse_json_model(source.value("hand", json::object()));
        out.sword = parse_json_model(source.value("sword", json::object()));
        out.sheath = parse_json_model(source.value("sheath", json::object()));
        out.shield = parse_json_model(source.value("shield", json::object()));
        out.heldItem = parse_json_model(source.value("held_item", json::object()));
        out.hookTip = parse_json_model(source.value("hook_tip", json::object()));
        out.hookSubItem = parse_json_model(source.value("hook_sub_item", json::object()));
        out.hookSubTip = parse_json_model(source.value("hook_sub_tip", json::object()));
        out.arrow = parse_json_model(source.value("arrow", json::object()));
        out.kantera = parse_json_model(source.value("kantera", json::object()));
        out.kanteraGlow = parse_json_model(source.value("kantera_glow", json::object()));
        out.itemActor = parse_json_model(source.value("item_actor", json::object()));
        out.rideActor = parse_json_model(source.value("ride_actor", json::object()));
    }
    out.midnaHairShape = 0;
    // A packed semantic pose may intentionally contain only small attachment
    // slots, or all-absent slots to explicitly clear old props. Successful
    // container decoding is therefore valid independently of the body slot.
    out.valid = packedFormat || out.body.valid;
    return out;
}

bool hydrate_model(RemoteModelMatrixSnapshot& model,
                   const RemoteModelMatrixSnapshot& previous) {
    if (!model.valid || !model.weightsOmitted) return true;
    if (!previous.valid || previous.weightCount != model.weightCount ||
        previous.weights.size() != size_t(model.weightCount) * 12) return false;
    model.weights = previous.weights;
    model.weightsOmitted = false;
    return true;
}

void hydrate_matrices(RemoteLinkMatrixSnapshot& value,
                      const RemoteLinkMatrixSnapshot& previous) {
    RemoteModelMatrixSnapshot* current[] = {
        &value.body, &value.hat, &value.face, &value.hand, &value.sword, &value.sheath,
        &value.shield, &value.heldItem, &value.hookTip, &value.hookSubItem, &value.hookSubTip,
        &value.arrow, &value.kantera, &value.kanteraGlow, &value.itemActor, &value.rideActor,
    };
    const RemoteModelMatrixSnapshot* old[] = {
        &previous.body, &previous.hat, &previous.face, &previous.hand, &previous.sword,
        &previous.sheath, &previous.shield, &previous.heldItem, &previous.hookTip,
        &previous.hookSubItem, &previous.hookSubTip, &previous.arrow, &previous.kantera,
        &previous.kanteraGlow, &previous.itemActor, &previous.rideActor,
    };
    for (size_t i = 0; i < std::size(current); ++i) hydrate_model(*current[i], *old[i]);
}

template <size_t N>
void parse_i16_array(const json& state, const char* key, std::array<int16_t, N>& out) {
    const auto it = state.find(key);
    if (it == state.end() || !it->is_array() || it->size() != N) return;
    for (size_t i = 0; i < N; ++i) {
        if (!(*it)[i].is_number_integer()) return;
        const int value = (*it)[i].get<int>();
        if (value < -32768 || value > 32767) return;
        out[i] = int16_t(value);
    }
}

std::vector<RemoteAudioEvent> parse_audio(const json& state, const char* key, bool active) {
    std::vector<RemoteAudioEvent> out;
    const auto it = state.find(key);
    if (it == state.end() || !it->is_array()) return out;
    for (const auto& entry : *it) {
        if (!entry.is_object() || out.size() >= 8) continue;
        RemoteAudioEvent event;
        event.sequence = entry.value("seq", 0U);
        event.soundId = entry.value("sound_id", 0U);
        event.mapInfo = entry.value("mapinfo", 0U);
        event.reverb = int8_t(std::clamp(entry.value("reverb", -1), -1, 127));
        event.sourceKind = uint8_t(std::clamp(entry.value("source", 0), 0, 255));
        event.level = active || entry.value("level", false);
        if (event.soundId != 0 && (active || event.sequence != 0)) out.push_back(event);
    }
    return out;
}

}  // namespace

bool decode_peer_pose(const json& message, const std::string& peerId,
                      const PeerPoseSnapshot* previous, PeerPoseSnapshot& pose,
                      std::string& error) {
    try {
        if (!message.is_object() || message.value("type", "") != "pose") {
            error = "not a pose message";
            return false;
        }
        const json state = message.value("state", json::object());
        if (!state.is_object()) { error = "pose state is not an object"; return false; }
        pose = {};
        pose.valid = true;
        pose.peerId = peerId;
        pose.sequence = message.value("sequence", 0U);
        const std::string visualMode = state.value("visual_mode", "");
        if (visualMode == "semantic_gameplay") {
            pose.visualMode = PeerPoseSnapshot::VisualMode::SemanticGameplay;
        } else if (visualMode == "hidden_unsupported") {
            pose.visualMode = PeerPoseSnapshot::VisualMode::HiddenUnsupported;
        } else {
            pose.visualMode = PeerPoseSnapshot::VisualMode::Unknown;
        }
        pose.visualUnsupportedReasons = state.value("visual_unsupported_reasons", 0U);
        pose.stage = state.value("stage", "");
        pose.room = state.value("room", -1);
        pose.layer = state.value("layer", -1);
        pose.x = state.value("x", 0.0f); pose.y = state.value("y", 0.0f);
        pose.z = state.value("z", 0.0f); pose.angleY = state.value("angle_y", 0);
        if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.z)) {
            error = "pose contains non-finite coordinates"; return false;
        }
        pose.finalGanondorfReady = state.value("final_ganondorf_ready", false);
        pose.procId = state.value("proc_id", 0); pose.procVar0 = state.value("proc_v0", 0);
        pose.procVar1 = state.value("proc_v1", 0); pose.procVar2 = state.value("proc_v2", 0);
        pose.procVar3 = state.value("proc_v3", 0); pose.procVar5 = state.value("proc_v5", 0);
        pose.cutType = state.value("cut_type", 0); pose.cutCount = state.value("cut_count", 0);
        pose.jumpCancelTurn = state.value("jump_cancel_turn", false);
        pose.manualSyncReady = state.value("manual_sync_ready", false);
        pose.underFrame = state.value("under_frame", 0.0f);
        pose.underBck0 = state.value("under_bck0", 0);
        pose.underBckArc0 = state.value("under_arc0", 0xFFFF);
        pose.underFrame0 = state.value("under_frame0", pose.underFrame);
        pose.underRate0 = state.value("under_rate0", 1.0f);
        pose.underRatio0 = state.value("under_ratio0", 1.0f);
        pose.underBck1 = state.value("under_bck1", 0);
        pose.underBckArc1 = state.value("under_arc1", 0xFFFF);
        pose.underFrame1 = state.value("under_frame1", 0.0f);
        pose.underRate1 = state.value("under_rate1", 1.0f);
        pose.underRatio1 = state.value("under_ratio1", 0.0f);
        pose.underBck2 = state.value("under_bck2", 0);
        pose.underBckArc2 = state.value("under_arc2", 0xFFFF);
        pose.underFrame2 = state.value("under_frame2", 0.0f);
        pose.underRate2 = state.value("under_rate2", 1.0f);
        pose.underRatio2 = state.value("under_ratio2", 0.0f);
        pose.upperBck0 = state.value("upper_bck0", 0);
        pose.upperBckArc0 = state.value("upper_arc0", 0xFFFF);
        pose.upperFrame0 = state.value("upper_frame0", 0.0f);
        pose.upperRate0 = state.value("upper_rate0", 1.0f);
        pose.upperRatio0 = state.value("upper_ratio0", 1.0f);
        pose.upperBck1 = state.value("upper_bck1", 0);
        pose.upperBckArc1 = state.value("upper_arc1", 0xFFFF);
        pose.upperFrame1 = state.value("upper_frame1", 0.0f);
        pose.upperRate1 = state.value("upper_rate1", 1.0f);
        pose.upperRatio1 = state.value("upper_ratio1", 0.0f);
        pose.upperBck2 = state.value("upper_bck2", 0);
        pose.upperBckArc2 = state.value("upper_arc2", 0xFFFF);
        pose.upperFrame2 = state.value("upper_frame2", 0.0f);
        pose.upperRate2 = state.value("upper_rate2", 1.0f);
        pose.upperRatio2 = state.value("upper_ratio2", 0.0f);
        pose.faceBck = state.value("face_bck", 0);
        pose.faceBckArc = state.value("face_bck_arc", 0xFFFF);
        pose.faceBckFrame = state.value("face_bck_frame", 0.0f);
        pose.faceBtp = state.value("face_btp", 0);
        pose.faceBtpArc = state.value("face_btp_arc", 0xFFFF);
        pose.faceBtpFrame = state.value("face_btp_frame", 0.0f);
        pose.faceBtk = state.value("face_btk", 0);
        pose.faceBtkArc = state.value("face_btk_arc", 0xFFFF);
        pose.faceBtkFrame = state.value("face_btk_frame", 0.0f);
        const float animationValues[] = {
            pose.underFrame, pose.underFrame0, pose.underRate0, pose.underRatio0,
            pose.underFrame1, pose.underRate1, pose.underRatio1, pose.underFrame2,
            pose.underRate2, pose.underRatio2, pose.upperFrame0, pose.upperRate0,
            pose.upperRatio0, pose.upperFrame1, pose.upperRate1, pose.upperRatio1,
            pose.upperFrame2, pose.upperRate2, pose.upperRatio2,
            pose.faceBckFrame, pose.faceBtpFrame, pose.faceBtkFrame,
        };
        if (std::any_of(std::begin(animationValues), std::end(animationValues),
                        [](float value) { return !std::isfinite(value); })) {
            error = "pose contains non-finite animation state";
            return false;
        }
        parse_i16_array(state, "hat_rot_a", pose.hatRotA);
        parse_i16_array(state, "hat_rot_b", pose.hatRotB);
        parse_i16_array(state, "hat_swing", pose.hatSwing);
        pose.hatShapeY = state.value("hat_shape_y", 0);
        pose.shapeAngleX = state.value("shape_angle_x", 0);
        pose.shapeAngleZ = state.value("shape_angle_z", 0);
        pose.bodyAngleX = state.value("body_angle_x", 0);
        pose.bodyAngleY = state.value("body_angle_y", 0);
        pose.bodyAngleZ = state.value("body_angle_z", 0);
        pose.bodyTwistY = state.value("body_twist_y", 0);
        pose.neckJointX = state.value("neck_joint_x", 0);
        pose.neckJointY = state.value("neck_joint_y", 0);
        pose.neckJointZ = state.value("neck_joint_z", 0);
        pose.lowerJointX = state.value("lower_joint_x", 0);
        pose.lowerJointZ = state.value("lower_joint_z", 0);
        pose.rootJointX = state.value("root_joint_x", 0);
        pose.rootJointZ = state.value("root_joint_z", 0);
        pose.blendMode = state.value("blend_mode", 0);
        pose.upperSavedRatio = state.value("upper_saved_ratio", 0.0f);
        pose.bodyRootValid = state.value("body_root_valid", false);
        pose.bodyRootX = state.value("body_root_x", 0.0f);
        pose.bodyRootY = state.value("body_root_y", 0.0f);
        pose.bodyRootZ = state.value("body_root_z", 0.0f);
        if (!std::isfinite(pose.upperSavedRatio) ||
            (pose.bodyRootValid &&
             (!std::isfinite(pose.bodyRootX) || !std::isfinite(pose.bodyRootY) ||
              !std::isfinite(pose.bodyRootZ)))) {
            error = "pose contains non-finite body animation state";
            return false;
        }
        parse_i16_array(state, "leg_ik_angles", pose.legIkAngles);
        parse_i16_array(state, "arm_ik_angles", pose.armIkAngles);
        parse_i16_array(state, "arm_rot_a", pose.armRotA);
        parse_i16_array(state, "arm_rot_b", pose.armRotB);
        parse_i16_array(state, "fishing_arm_1", pose.fishingArm1Angle);
        parse_i16_array(state, "fishing_arm_2", pose.fishingArm2Angle);
        pose.isWolf = state.value("is_wolf", false);
        pose.isTransforming = state.value("is_transforming", false);
        pose.transformFromWolf = state.value("transform_from_wolf", pose.isWolf);
        pose.transformToWolf = state.value("transform_to_wolf", !pose.transformFromWolf);
        pose.transformProcVar0 = state.value("transform_proc_v0", 0);
        pose.transformProcVar5 = state.value("transform_proc_v5", 0);
        pose.transformClothesWait = state.value("transform_clothes_wait", 0);
        pose.transformFrame = state.value("transform_frame", 0.0f);
        if (!std::isfinite(pose.transformFrame)) {
            error = "pose contains non-finite transformation state";
            return false;
        }
        pose.transformProcVar2 = state.value("transform_proc_v2", 0);
        pose.transformProcVar3 = state.value("transform_proc_v3", 0);
        pose.transformShapeX = state.value("transform_shape_x", 0);
        pose.equipItem = uint16_t(state.value("equip_item", 0xffff));
        pose.swordVariant = state.value("sword_variant", 0);
        pose.shieldVariant = state.value("shield_variant", 0);
        pose.clothesVariant = state.value("clothes_variant", 0);
        pose.swordDraw = state.value("sword_draw", false);
        pose.shieldDraw = state.value("shield_draw", false);
        pose.shieldGuardActive = state.value("shield_guard_active", false);
        pose.swordOut = state.value("sword_out", false);
        pose.swordHandAttached = state.value("sword_hand_attached", pose.swordOut);
        pose.shieldHandAttached = state.value("shield_hand_attached", false);
        pose.midnaDraw = false;
        pose.midnaMaskDraw = false;
        pose.midnaHandDraw = false;
        pose.midnaHairDraw = false;
        pose.midnaShadowForm = false;
        pose.heavyBoots = state.value("heavy_boots", false);
        pose.itemDraw = state.value("item_draw", false);
        pose.kanteraDraw = state.value("kantera_draw", false);
        pose.itemActorKind = state.value("item_actor_kind", 0);
        pose.itemActorBombExTime = state.value("item_actor_bomb_ex_time", -1);
        pose.itemActorBombFlash = state.value("item_actor_bomb_flash", -1);
        pose.rideActorKind = state.value("ride_actor_kind", 0);
        pose.linkMatrices = parse_matrices(state.value("link_matrices", json::object()));
        pose.linkMatricesFresh = pose.linkMatrices.valid;
        if (previous != nullptr && previous->valid) {
            if (pose.sequence <= previous->sequence) { error = "stale pose sequence"; return false; }
            if (pose.linkMatrices.valid) hydrate_matrices(pose.linkMatrices, previous->linkMatrices);
            else pose.linkMatrices = previous->linkMatrices;
        }
        pose.audioEvents = parse_audio(state, "audio_events", false);
        pose.activeAudioEvents = parse_audio(state, "active_audio_events", true);
        error.clear();
        return true;
    } catch (const json::exception& ex) {
        error = std::string("invalid pose: ") + ex.what();
        return false;
    }
}

bool enforce_semantic_pose_invariants(PeerPoseSnapshot& pose, std::string& error) {
    if (!pose.valid) {
        error = "semantic pose is invalid";
        return false;
    }
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::Unknown) {
        error = "semantic pose has no supported visual mode";
        return false;
    }
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::SemanticGameplay &&
        pose.visualUnsupportedReasons != 0) {
        error = "semantic pose has contradictory unsupported reasons";
        return false;
    }
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::HiddenUnsupported) {
        pose.linkMatrices = {};
        pose.linkMatricesFresh = false;
        error.clear();
        return true;
    }

    // Type-7 owns Link through semantic animation state. Only the inexpensive
    // attachment slots 4..15 may remain matrix-driven.
    pose.linkMatrices.body = {};
    pose.linkMatrices.hat = {};
    pose.linkMatrices.face = {};
    pose.linkMatrices.hand = {};
    pose.linkMatrices.midna = {};
    pose.linkMatrices.midnaMask = {};
    pose.linkMatrices.midnaHand = {};
    pose.linkMatrices.midnaHair = {};
    pose.linkMatrices.midnaGlow = {};
    pose.linkMatrices.midnaHairShape = 0;
    error.clear();
    return true;
}

bool merge_midna_pose(const json& message, PeerPoseSnapshot& pose, std::string& error) {
    (void)message;
    (void)pose;
    error.clear();
    return false;
}

bool expand_remote_matrix_delta(json& message, const std::string& peerId,
                                uint8_t packetType, uint32_t sequence,
                                std::string& error) {
    try {
        auto state = message.find("state");
        if (state == message.end() || !state->is_object()) return true;
        auto matrices = state->find("link_matrices");
        if (matrices == state->end() || !matrices->is_object()) return true;
        std::string format = matrices->value("format", "");
        const std::string key = history_key(peerId, packetType);
        if (const auto baseFormat = base_format_for_delta(format)) {
            const uint32_t baselineSequence = matrices->value("baseline_sequence", 0U);
            const auto history = sMatrixHistory.find(key);
            if (history == sMatrixHistory.end()) {
                error = "matrix delta has no baseline history";
                return false;
            }
            const auto baseline = history->second.find(baselineSequence);
            if (baseline == history->second.end()) {
                error = "matrix delta baseline is unavailable";
                return false;
            }
            std::string delta, expanded;
            if (!packed_bytes(*matrices, delta) ||
                !apply_slot_delta(format, delta, baseline->second, baselineSequence, expanded)) {
                error = "matrix delta payload is invalid";
                return false;
            }
            (*matrices)["format"] = *baseFormat;
            matrices->erase("baseline_sequence");
            std::vector<uint8_t> bytes(expanded.begin(), expanded.end());
            (*matrices)["data"] = json::binary(std::move(bytes));
            format = *baseFormat;
        }
        if (sequence != 0 && is_delta_capable_base(format)) {
            std::string packed;
            if (!packed_bytes(*matrices, packed)) {
                error = "matrix baseline payload is invalid";
                return false;
            }
            auto& history = sMatrixHistory[key];
            history[sequence] = std::move(packed);
            while (history.size() > kMatrixHistoryLimit) history.erase(history.begin());
        }
        error.clear();
        return true;
    } catch (const json::exception& ex) {
        error = std::string("invalid matrix delta: ") + ex.what();
        return false;
    }
}

bool prepare_remote_matrix_delta(json& message, const std::string& peerId,
                                 uint8_t packetType, uint32_t sequence,
                                 uint32_t baselineSequence, std::string& error) {
    try {
        auto state = message.find("state");
        if (state == message.end() || !state->is_object()) return true;
        auto matrices = state->find("link_matrices");
        if (matrices == state->end() || !matrices->is_object()) return true;
        const std::string format = matrices->value("format", "");
        const auto deltaFormat = delta_format_for_base(format);
        if (!deltaFormat) return true;

        std::string current;
        if (!packed_bytes(*matrices, current)) {
            error = "matrix baseline payload is invalid";
            return false;
        }
        auto& history = sMatrixHistory[history_key(peerId, packetType)];
        if (sequence != 0) {
            history[sequence] = current;
            while (history.size() > kMatrixHistoryLimit) history.erase(history.begin());
        }
        if (baselineSequence == 0) return true;
        const auto baseline = history.find(baselineSequence);
        if (baseline == history.end()) return true;

        std::string delta;
        if (!build_slot_delta(format, current, baseline->second, baselineSequence, delta) ||
            delta.size() >= current.size()) return true;
        (*matrices)["format"] = *deltaFormat;
        (*matrices)["baseline_sequence"] = baselineSequence;
        std::vector<uint8_t> bytes(delta.begin(), delta.end());
        (*matrices)["data"] = json::binary(std::move(bytes));
        error.clear();
        return true;
    } catch (const json::exception& ex) {
        error = std::string("invalid matrix baseline: ") + ex.what();
        return false;
    }
}

void clear_remote_matrix_history(const std::string& peerId) {
    if (peerId.empty()) {
        sMatrixHistory.clear();
        return;
    }
    for (auto it = sMatrixHistory.begin(); it != sMatrixHistory.end();) {
        if (it->first.rfind(peerId + '\x1f', 0) == 0) it = sMatrixHistory.erase(it);
        else ++it;
    }
}

}  // namespace dusklight_online::game
