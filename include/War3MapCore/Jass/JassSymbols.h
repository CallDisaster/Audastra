#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Jass {

// 同一次检查中的来源序号与原token字节位置构成身份；同名局部变量不会共用身份。
struct SymbolKey {
    std::uint32_t file = 0, offset = 0;
    [[nodiscard]] bool operator==(const SymbolKey&) const = default;
};
enum class SymbolKind : std::uint8_t { Type, Global, Native, Function, Parameter, Local };
enum class SymbolUse : std::uint8_t { Read, Write, Call, FunctionReference };
struct SymbolParameter {
    std::string type, name;
    [[nodiscard]] bool operator==(const SymbolParameter&) const = default;
};
struct SymbolPosition {
    SymbolKey key;
    std::uint32_t length = 0, line = 0, byteColumn = 1;
    [[nodiscard]] bool operator==(const SymbolPosition&) const = default;
};
struct SymbolDefinition {
    SymbolPosition position;
    SymbolKind kind = SymbolKind::Global;
    std::string name, type;
    std::optional<SymbolKey> scope;
    bool array = false, constant = false;
    std::vector<SymbolParameter> parameters; // 同次真实Parser函数/原生参数摘要。
    [[nodiscard]] bool operator==(const SymbolDefinition&) const = default;
};
struct SymbolReference {
    SymbolPosition position;
    SymbolUse use = SymbolUse::Read;
    std::optional<SymbolKey> target;
    [[nodiscard]] bool operator==(const SymbolReference&) const = default;
};
struct SymbolScope {
    SymbolKey function;
    std::uint32_t firstLine = 0, lastLine = 0;
    [[nodiscard]] bool operator==(const SymbolScope&) const = default;
};
struct SymbolIndex {
    // completed涵盖变量读写、函数调用和function引用；不收集类型名的使用点。
    // 达到显示预算的部分子集可受并发领取顺序影响；完整结果的身份/顺序保持稳定。
    bool completed = false;
    std::string reason;
    std::vector<std::string> sources;
    std::vector<SymbolDefinition> definitions;
    std::vector<SymbolReference> references;
    std::vector<SymbolScope> scopes;
};

} // namespace War3Map::Jass
