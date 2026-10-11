#include "NativeSignature.h"

namespace Audastra::Runtime32::OwnLua {
namespace {
bool Letter(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
std::optional<NativeType> ReadType(std::string_view text, std::size_t& offset, bool allowNothing) {
    if (offset >= text.size()) return {};
    NativeType result;
    switch (text[offset++]) {
    case 'V': if (!allowNothing) return {}; result.kind = NativeKind::Nothing; break;
    case 'I': result.kind = NativeKind::Integer; break;
    case 'R': result.kind = NativeKind::Real; break;
    case 'B': result.kind = NativeKind::Boolean; break;
    case 'S': result.kind = NativeKind::String; break;
    case 'C': result.kind = NativeKind::Code; break;
    case 'H': {
        const auto end = text.find(';', offset);
        if (end == std::string_view::npos || !NativeIdentifier(text.substr(offset, end - offset))) return {};
        result.kind = NativeKind::Handle;
        result.handleType = text.substr(offset, end - offset);
        offset = end + 1;
        break;
    }
    default: return {};
    }
    return result;
}
}

bool NativeIdentifier(std::string_view text) noexcept {
    if (text.empty() || text.size() > 128 || !Letter(text.front())) return false;
    for (const char ch : text) if (!Letter(ch) && (ch < '0' || ch > '9')) return false;
    return true;
}

std::optional<NativeSignature> ParseNativeSignature(std::string_view text) {
    if (text.size() < 3 || text.size() > 1024 || text.front() != '(') return {};
    NativeSignature result;
    std::size_t offset = 1;
    while (offset < text.size() && text[offset] != ')') {
        if (result.arguments.size() == 32) return {};
        auto type = ReadType(text, offset, false);
        if (!type) return {};
        result.arguments.push_back(std::move(*type));
    }
    if (offset >= text.size() || text[offset++] != ')') return {};
    auto returns = ReadType(text, offset, true);
    if (!returns) return {};
    // 经典游戏的实际登记表允许标量返回类型附带一个终结分号。
    // 只识别已核实的四种标量；参数、handle 终结符和额外尾文本仍严格校验。
    const bool scalar = returns->kind == NativeKind::Integer || returns->kind == NativeKind::Real
        || returns->kind == NativeKind::Boolean || returns->kind == NativeKind::String;
    if (scalar && offset + 1 == text.size() && text[offset] == ';') ++offset;
    if (offset != text.size()) return {};
    result.returns = std::move(*returns);
    return result;
}

} // namespace Audastra::Runtime32::OwnLua
