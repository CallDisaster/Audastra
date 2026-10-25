#include "aj/Protocol.h"
#include <charconv>
#include <cmath>
#include <limits>

namespace audastra::japi {
namespace {
bool Lower(char c) { return c >= 'a' && c <= 'z'; }
bool Digit(char c) { return c >= '0' && c <= '9'; }
bool IdentifierChar(char c) { return Lower(c) || Digit(c) || c == '_' || c == '-'; }
bool Take(std::string_view& input, std::string_view& field) {
    const auto pos = input.find(';');
    if (pos == input.npos) return false;
    field = input.substr(0, pos);
    input.remove_prefix(pos + 1);
    return true;
}
bool Unsigned(std::string_view field, uint32_t& out, bool zero) {
    if (field.empty() || (field.size() > 1 && field[0] == '0')) return false;
    for (char c : field) if (!Digit(c)) return false;
    const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), out);
    return ec == std::errc{} && end == field.data() + field.size()
        && out <= INT32_MAX && (zero || out != 0);
}
bool Integer(std::string_view field, int32_t& out) {
    auto digits = field;
    if (digits.starts_with('-')) digits.remove_prefix(1);
    if (digits.empty()) return false;
    for (char c : digits) if (!Digit(c)) return false;
    const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), out);
    return ec == std::errc{} && end == field.data() + field.size();
}
bool Real(std::string_view field, float& out) {
    size_t p = field.starts_with('-') ? 1 : 0;
    size_t digits = 0;
    while (p < field.size() && Digit(field[p])) { ++p; ++digits; }
    if (p < field.size() && field[p] == '.') {
        ++p;
        while (p < field.size() && Digit(field[p])) { ++p; ++digits; }
    }
    if (!digits) return false;
    if (p < field.size() && (field[p] == 'e' || field[p] == 'E')) {
        ++p;
        if (p < field.size() && (field[p] == '+' || field[p] == '-')) ++p;
        const size_t start = p;
        while (p < field.size() && Digit(field[p])) ++p;
        if (start == p) return false;
    }
    if (p != field.size()) return false;
    const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), out,
        std::chars_format::general);
    return ec == std::errc{} && end == field.data() + field.size() && std::isfinite(out);
}
int32_t ParseValue(std::string_view field, Value& out, size_t textLimit, bool allowVoid) {
    if (field.size() < 2 || field[1] != ':') return AJ_FRAME;
    out.kind = TypeKind(field[0]);
    field.remove_prefix(2);
    switch (out.kind) {
    case AJ_VOID: return allowVoid && field.empty() ? AJ_OK : AJ_ARGUMENT;
    case AJ_INT: case AJ_HANDLE: return Integer(field, out.integer) ? AJ_OK : AJ_ARGUMENT;
    case AJ_BOOL:
        if (field != "0" && field != "1") return AJ_ARGUMENT;
        out.integer = field == "1"; return AJ_OK;
    case AJ_REAL: return Real(field, out.real) ? AJ_OK : AJ_ARGUMENT;
    case AJ_TEXT: return DecodeText(field, out.text, textLimit);
    default: return AJ_ARGUMENT;
    }
}
int Hex(char c) {
    if (Digit(c)) return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
}
bool ValidText(std::string_view text) {
    // Reject NUL/C0/DEL, overlong UTF-8, surrogates and out-of-range scalars.
    for (size_t i = 0; i < text.size();) {
        uint32_t c = static_cast<unsigned char>(text[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned n = 0; uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; minimum = 0x800; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; minimum = 0x10000; c &= 7; }
        else return false;
        if (i + n > text.size()) return false;
        while (n--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
bool ValidName(std::string_view name, size_t limit) {
    if (name.empty() || name.size() > limit || !Lower(name[0])) return false;
    for (char c : name) if (!IdentifierChar(c)) return false;
    return true;
}
bool ValidCommand(std::string_view name) {
    if (name.size() > 63) return false;
    const auto dot = name.find('.');
    if (dot == name.npos || !ValidName(name.substr(0, dot))) return false;
    name.remove_prefix(dot + 1);
    do {
        const auto next = name.find('.');
        if (!ValidName(name.substr(0, next), 63)) return false;
        if (next == name.npos) return true;
        name.remove_prefix(next + 1);
    } while (!name.empty());
    return false;
}
bool ValidFunction(std::string_view name) {
    if (name.empty() || name.size() > 63) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (!(Lower(c) || (c >= 'A' && c <= 'Z') || c == '_' || (i && Digit(c)))) return false;
    }
    return true;
}
bool ValidVersion(std::string_view version) {
    if (version.empty() || version.size() > 63) return false;
    for (char c : version)
        if (!(IdentifierChar(c) || (c >= 'A' && c <= 'Z') || c == '.' || c == '+')) return false;
    return true;
}
std::string EncodeText(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c == '%') out += "%25";
        else if (c == ';') out += "%3B";
        else out += c;
    }
    return out;
}
int32_t DecodeText(std::string_view text, std::string& out, size_t limit) {
    out.clear();
    if (text.size() > limit) return AJ_LIMIT;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ';') return AJ_FRAME;
        if (text[i] != '%') { out += text[i]; continue; }
        if (i + 2 >= text.size()) return AJ_ENCODING;
        const int a = Hex(text[i + 1]), b = Hex(text[i + 2]);
        if (a < 0 || b < 0) return AJ_ENCODING;
        out += static_cast<char>((a << 4) | b); i += 2;
    }
    return ValidText(out) ? AJ_OK : AJ_ENCODING;
}
int32_t ParseRequest(std::string_view wire, Request& out) {
    out = {};
    if (wire.size() > AJ_MAX_FRAME) return AJ_LIMIT;
    if (!wire.starts_with(Prefix)) return wire.starts_with("ajapi:") ? AJ_VERSION : AJ_FRAME;
    wire.remove_prefix(Prefix.size());
    std::string_view field;
    if (!Take(wire, field) || !Unsigned(field, out.epoch, true)) return AJ_FRAME;
    if (!Take(wire, field) || !Unsigned(field, out.id, false)) return AJ_FRAME;
    const auto pos = wire.find(';');
    out.command = wire.substr(0, pos);
    if (!ValidCommand(out.command)) return AJ_FRAME;
    if (pos == wire.npos) return AJ_OK;
    wire.remove_prefix(pos + 1);
    while (true) {
        if (out.arguments.size() == AJ_MAX_ARGUMENTS) return AJ_LIMIT;
        const auto end = wire.find(';');
        Value value;
        const auto status = ParseValue(wire.substr(0, end), value, AJ_MAX_INPUT_TEXT, false);
        if (status != AJ_OK) return status;
        out.arguments.push_back(std::move(value));
        if (end == wire.npos) break;
        wire.remove_prefix(end + 1);
    }
    return AJ_OK;
}
std::string EncodeResponse(const Response& r) {
    if (!r.consumed) return {};
    int32_t status = r.status;
    std::string token = "v:";
    if (status == AJ_OK) {
        const auto& v = r.value;
        token = std::string(1, TypeChar(v.kind)) + ':';
        switch (v.kind) {
        case AJ_VOID: break;
        case AJ_INT: case AJ_HANDLE: token += std::to_string(v.integer); break;
        case AJ_BOOL:
            if (v.integer != 0 && v.integer != 1) status = AJ_BACKEND;
            else token += std::to_string(v.integer);
            break;
        case AJ_REAL: {
            char number[64];
            const auto [end, ec] = std::to_chars(number, number + sizeof(number), v.real,
                std::chars_format::general, std::numeric_limits<float>::max_digits10);
            if (!std::isfinite(v.real) || ec != std::errc{}) status = AJ_BACKEND;
            else token.append(number, end);
            break;
        }
        case AJ_TEXT:
            if (!ValidText(v.text)) status = AJ_ENCODING;
            else {
                const auto encoded = EncodeText(v.text);
                if (encoded.size() > AJ_MAX_OUTPUT_TEXT) status = AJ_LIMIT;
                else token += encoded;
            }
            break;
        default: status = AJ_BACKEND;
        }
    }
    if (status != AJ_OK) token = "v:";
    return std::string(Prefix) + std::to_string(r.epoch) + ';' + std::to_string(r.id) + ';'
        + std::to_string(status) + ';' + token;
}
int32_t ParseResponse(std::string_view wire, Response& out) {
    out = {};
    if (wire.size() > AJ_MAX_FRAME) return AJ_LIMIT;
    if (!wire.starts_with(Prefix)) return AJ_FRAME;
    wire.remove_prefix(Prefix.size());
    std::string_view field;
    uint32_t status = 0;
    if (!Take(wire, field) || !Unsigned(field, out.epoch, true)
        || !Take(wire, field) || !Unsigned(field, out.id, true)
        || !Take(wire, field) || !Unsigned(field, status, true) || status > AJ_INTERNAL) return AJ_FRAME;
    out.status = static_cast<int32_t>(status);
    const auto parsed = ParseValue(wire, out.value, AJ_MAX_OUTPUT_TEXT, true);
    if (parsed != AJ_OK) return parsed;
    return out.status != AJ_OK && out.value.kind != AJ_VOID ? AJ_FRAME : AJ_OK;
}
char TypeChar(int32_t kind) {
    constexpr char types[] = "virsbd";
    return kind >= AJ_VOID && kind <= AJ_HANDLE ? types[kind] : '?';
}
int32_t TypeKind(char type) {
    for (int32_t i = AJ_VOID; i <= AJ_HANDLE; ++i) if (TypeChar(i) == type) return i;
    return -1;
}
const char* StatusText(int32_t status) {
    constexpr const char* names[] = {"ok", "invalid frame", "unsupported version", "limit exceeded",
        "invalid UTF-8/escape", "invalid argument", "unknown command", "wrong carrier",
        "invalid state", "wrong thread", "busy", "reentrant call", "unsupported capability",
        "backend rejected", "policy denied", "stale epoch/request", "dispatcher incomplete", "internal error"};
    return status >= AJ_OK && status <= AJ_INTERNAL ? names[status] : "unknown status";
}
}
