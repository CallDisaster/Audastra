#pragma once

#include "LuaTestMailbox.h"
#include <string_view>

namespace Audastra::Runtime32 {

inline bool LuaMailboxUtf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto ch = static_cast<unsigned char>(text[i++]);
        if (ch == 0) return false;
        if (ch < 0x80) continue;
        std::uint32_t code = 0, minimum = 0;
        std::size_t extra = 0;
        if (ch >= 0xc2 && ch <= 0xdf) { code = ch & 0x1f; minimum = 0x80; extra = 1; }
        else if (ch >= 0xe0 && ch <= 0xef) { code = ch & 0xf; minimum = 0x800; extra = 2; }
        else if (ch >= 0xf0 && ch <= 0xf4) { code = ch & 7; minimum = 0x10000; extra = 3; }
        else return false;
        if (extra > text.size() - i) return false;
        for (std::size_t j = 0; j < extra; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

inline bool LuaMailboxHex(std::string_view text, std::size_t bytes) noexcept {
    if (text.size() != bytes) return false;
    for (const char ch : text) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}

inline bool LuaMailboxHeader(const LuaTestMailbox& mailbox) noexcept {
    return mailbox.magic == kLuaMailboxMagic && mailbox.version == kLuaMailboxVersion
        && mailbox.structBytes == sizeof(LuaTestMailbox)
        && mailbox.sessionNonce[32] == 0 && mailbox.mapSha256[64] == 0
        && LuaMailboxHex({mailbox.sessionNonce, 32}, 32) && LuaMailboxHex({mailbox.mapSha256, 64}, 64);
}

inline bool LuaMailboxChunkName(std::string_view text) noexcept {
    if (text.empty() || text.size() >= kLuaReloadChunkCapacity || !LuaMailboxUtf8(text)) return false;
    for (const char ch : text) if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) return false;
    return true;
}

// 输入已通过 UTF-8 校验时，在容量边界前保留完整字符，结果不包括终止 NUL。
inline std::size_t LuaMailboxUtf8Prefix(std::string_view text, std::size_t capacity) noexcept {
    if (text.size() <= capacity) return text.size();
    while (capacity && (static_cast<unsigned char>(text[capacity]) & 0xc0) == 0x80) --capacity;
    return capacity;
}

} // namespace Audastra::Runtime32
