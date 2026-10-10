#pragma once

#include "War3MapCore/Script/MapInitializationPlan.h"
#include "War3MapCore/MapData/GeneratedObjectSymbol.h"
#include "War3MapCore/MapData/PlacementInitializationRules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace War3Map::Script::init_detail {

inline constexpr std::string_view kSettings = "war3map.w3i";
inline constexpr std::string_view kUnits = "war3mapUnits.doo";
inline constexpr std::string_view kDoodads = "war3map.doo";
inline constexpr std::string_view kRegions = "war3map.w3r";
inline constexpr std::string_view kCameras = "war3map.w3c";
inline constexpr std::string_view kSounds = "war3map.w3s";

struct PlanFailure { Diagnostic diagnostic; };

[[noreturn]] inline void Reject(std::string code, std::string message, std::string_view member,
    std::optional<std::size_t> index = {}) {
    const std::string location = std::string(member) + (index ? " 记录[" + std::to_string(*index) + "]" : "");
    // sourceIndex 是已解析记录编号，不伪装成原归档内的字节偏移。
    throw PlanFailure{{DiagnosticSeverity::Error, "Script.MapInitialization", std::move(code),
        location + "：" + std::move(message), OriginSpan{std::string(member), 0, 0, {}}}};
}

inline bool AsciiAlnum(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9');
}

inline bool Identifier(std::string_view value) {
    return !value.empty() && (value.front() == '_' || (AsciiAlnum(value.front())
        && !(value.front() >= '0' && value.front() <= '9')))
        && std::all_of(value.begin(), value.end(), [](char c) { return AsciiAlnum(c) || c == '_'; });
}

inline void CheckString(std::string_view value, std::string_view field, std::string_view member,
    std::optional<std::size_t> index = {}) {
    if (value.find('\0') != std::string_view::npos)
        Reject("MAP_INIT_STRING_INVALID", std::string(field) + " 含内嵌 NUL，不能与成员字符串一致。", member, index);
}

inline void CheckFinite(std::initializer_list<float> values, std::string_view field,
    std::string_view member, std::optional<std::size_t> index = {}) {
    if (std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); }))
        Reject("MAP_INIT_VALUE_NONFINITE", std::string(field) + " 含非有限浮点数。", member, index);
}

inline bool EmptyRawcode(const MapData::SettingsRawcode& value) {
    return std::all_of(value.begin(), value.end(), [](std::byte byte) { return byte == std::byte{}; });
}

inline void CheckRawcode(std::string_view value, std::string_view field, std::string_view member,
    std::size_t index) {
    if (value.size() != 4)
        Reject("MAP_INIT_RAWCODE_INVALID", std::string(field) + " 不是四字节对象 ID。", member, index);
}

inline std::string NumberedSymbol(std::string_view kind, std::string_view rawcode,
    std::int32_t creationId, std::string_view member, std::size_t index) {
    auto symbol = MapData::GeneratedObjectSymbol(kind, rawcode, creationId);
    if (!symbol.Succeeded() || !symbol.value)
        Reject("MAP_INIT_RAWCODE_INVALID", "rawcode和创建编号不能形成安全的来源对象符号。", member, index);
    return std::move(*symbol.value);
}

inline void CheckTransform(const MapData::PlacementTransform& value, std::string_view member,
    std::size_t index) {
    CheckRawcode(value.rawcode, "rawcode", member, index);
    CheckFinite({value.position[0], value.position[1], value.position[2], value.rotation,
        value.scale[0], value.scale[1], value.scale[2]}, "位置、角度或缩放", member, index);
    if (value.skinId)
        Reject("MAP_INIT_SKIN_UNSUPPORTED", "初始化计划尚未实现 skinId，不能只创建基础类型。", member, index);
}

inline bool HasDropChoices(const std::vector<MapData::PlacementItemSet>& sets) {
    return std::any_of(sets.begin(), sets.end(), [](const auto& set) { return !set.items.empty(); });
}

template <typename Callback>
auto ReadQuery(Callback&& callback, std::string_view name, std::string_view member, std::size_t index) {
    try {
        return callback();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        Reject("MAP_INIT_QUERY_FAILED", "有效物编/资源查询 " + std::string(name) + " 失败；未采用猜测默认值。", member, index);
    }
}

} // namespace War3Map::Script::init_detail
