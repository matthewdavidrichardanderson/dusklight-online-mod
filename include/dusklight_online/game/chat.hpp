#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dusklight_online::game {

inline constexpr size_t kMaxChatLineCodepoints = 50;
inline constexpr size_t kMaxChatLines = 4;
// Four full lines of four-byte UTF-8 code points plus their separators.
inline constexpr size_t kMaxChatTextBytes =
    kMaxChatLineCodepoints * kMaxChatLines * 4 + (kMaxChatLines - 1);

struct WrappedChatInput {
    std::string text;
    size_t cursorByte = 0;
};

inline size_t chat_utf8_codepoint_bytes(std::string_view input, size_t index) {
    const uint8_t first = static_cast<uint8_t>(input[index]);
    if (first < 0x80) return 1;
    if (first >= 0xc2 && first <= 0xdf) return 2;
    if (first >= 0xe0 && first <= 0xef) return 3;
    if (first >= 0xf0 && first <= 0xf4) return 4;
    return 1;
}

inline WrappedChatInput wrap_chat_input(std::string_view input, size_t cursorByte) {
    WrappedChatInput wrapped;
    wrapped.text.reserve(std::min(input.size() + kMaxChatLines - 1,
                                  kMaxChatTextBytes));
    cursorByte = std::min(cursorByte, input.size());

    size_t inputIndex = 0;
    size_t line = 0;
    size_t column = 0;
    while (inputIndex < input.size() && line < kMaxChatLines) {
        if (inputIndex == cursorByte) wrapped.cursorByte = wrapped.text.size();

        const char character = input[inputIndex];
        if (character == '\r' || character == '\n') {
            const size_t consumed = character == '\r' && inputIndex + 1 < input.size() &&
                    input[inputIndex + 1] == '\n' ? 2 : 1;
            inputIndex += consumed;
            if (line + 1 >= kMaxChatLines) break;
            wrapped.text.push_back('\n');
            ++line;
            column = 0;
            if (inputIndex <= cursorByte) wrapped.cursorByte = wrapped.text.size();
            continue;
        }

        const size_t codepointBytes = std::min(
            chat_utf8_codepoint_bytes(input, inputIndex), input.size() - inputIndex);
        if (column == kMaxChatLineCodepoints) {
            if (line + 1 >= kMaxChatLines) break;
            wrapped.text.push_back('\n');
            ++line;
            column = 0;
            if (inputIndex == cursorByte) wrapped.cursorByte = wrapped.text.size();
        }
        if (wrapped.text.size() + codepointBytes > kMaxChatTextBytes) break;
        wrapped.text.append(input.substr(inputIndex, codepointBytes));
        inputIndex += codepointBytes;
        ++column;
        if (inputIndex <= cursorByte) wrapped.cursorByte = wrapped.text.size();
    }
    if (cursorByte >= inputIndex) wrapped.cursorByte = wrapped.text.size();
    return wrapped;
}

inline bool normalize_chat_text(std::string_view input, std::string& output) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\n')) {
        input.remove_prefix(1);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\n')) {
        input.remove_suffix(1);
    }
    if (input.empty() || input.size() > kMaxChatTextBytes) return false;

    const auto* bytes = reinterpret_cast<const uint8_t*>(input.data());
    size_t index = 0;
    size_t line = 0;
    size_t column = 0;
    while (index < input.size()) {
        const uint8_t first = bytes[index];
        if (first < 0x80) {
            if (first == '\n') {
                if (++line >= kMaxChatLines) return false;
                column = 0;
                ++index;
                continue;
            }
            // Only LF is layout-significant. Reject the remaining C0 controls
            // and DEL rather than allowing them to affect logs or terminals.
            if (first < 0x20 || first == 0x7f ||
                ++column > kMaxChatLineCodepoints) return false;
            ++index;
            continue;
        }

        size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > input.size()) return false;
        for (size_t continuation = 1; continuation < length; ++continuation) {
            if ((bytes[index + continuation] & 0xc0) != 0x80) return false;
        }
        // Reject overlong encodings, UTF-16 surrogates, and values above
        // U+10FFFF.
        if (first == 0xe0 && bytes[index + 1] < 0xa0) return false;
        if (first == 0xed && bytes[index + 1] >= 0xa0) return false;
        if (first == 0xf0 && bytes[index + 1] < 0x90) return false;
        if (first == 0xf4 && bytes[index + 1] >= 0x90) return false;
        if (++column > kMaxChatLineCodepoints) return false;
        index += length;
    }

    output.assign(input);
    return true;
}

}  // namespace dusklight_online::game
