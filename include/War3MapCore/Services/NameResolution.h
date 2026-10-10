#pragma once

#include "War3MapCore/Services/ProjectSearch.h"

namespace War3Map { class MapDocument; }
namespace War3Map::Trigger { class MapObjectReferenceSnapshot; class TriggerTypeSystem; }

namespace War3Map::Services {

enum class NameResolutionState {
    Resolved, Literal, MissingDefinition, MissingString, Ambiguous, Unsupported, Invalid,
};

// 名称只用于展示；rawValue 保留协议/文档中的实际值，任何解析都不改写来源。
struct NameResolution {
    std::string rawValue, displayName, iconPath, reference;
    NameResolutionState state = NameResolutionState::Literal;
    std::string reason;
    std::vector<std::string> candidates;
};

// 调用期间借用当前有效 IR。显式 strings 包含待保存 WTS overlay，并优先于旧地图。
// 服务不持有静态缓存；宿主缓存时须覆盖文档身份、目录和各编辑修订。
struct NameResolutionContext {
    std::span<const ProjectSearchObjectSource> objects;
    const ObjectData::StandardObjectCatalog* standardObjects = nullptr;
    const MapDocument* mapDocument = nullptr;
    const MapData::StringTableDocument* strings = nullptr;
    const Trigger::TriggerDocument* triggers = nullptr;
    const Trigger::MapObjectReferenceSnapshot* mapObjects = nullptr;
    const Trigger::TriggerTypeSystem* types = nullptr;
    bool preserveStringFormatting = false; // 基础地图正文查询保留颜色码、换行；名称展示默认清理。
};

[[nodiscard]] std::string_view NameResolutionStateName(NameResolutionState state) noexcept;
// 完整 TRIGSTR 十进制 ID；接受匹配的外层引号，补零不改变身份。
[[nodiscard]] std::optional<std::uint32_t> StringReferenceId(std::string_view rawValue) noexcept;
[[nodiscard]] NameResolution ResolveObjectName(const NameResolutionContext& context,
    const ObjectData::ObjectKey& key);
[[nodiscard]] NameResolution ResolveName(const NameResolutionContext& context,
    std::string_view rawValue, std::string_view uiType = {});

} // namespace War3Map::Services
