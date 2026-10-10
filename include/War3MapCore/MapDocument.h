#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/LniMapSettings.h"
#include "War3MapCore/Workspace.h"

#include <array>

namespace War3Map {
namespace MapData::detail { struct MapDataFieldEditingAccess; }

enum class MapDataKind { Settings, Units, Doodads, Regions, Cameras, Sounds, Strings, Count };
enum class MapDataState { Missing, Ready, Partial, Invalid, Ambiguous, Unreadable };

struct MapMemberState {
    MapDataKind kind;
    std::string memberName;
    MapDataState state = MapDataState::Missing;
    // 冲突时保留所有输入，不从多个同名来源中任取一个。
    std::vector<MapArchiveMember> sources;
    std::vector<Diagnostic> diagnostics;
    std::optional<std::size_t> recordCount;
};

// 地图基础数据的只读文档。解析失败局限于对应成员；原归档和工程继续保留其原字节。
// 此轮不接管既有物编/触发器命令，不提供基础数据回写或游戏初始化生成器。
class MapDocument {
public:
    [[nodiscard]] static MapDocument FromArchive(const MapArchiveSession& archive);
    // 仅按已知名字读取基础成员，不要求归档拥有完整 listfile。此入口不授予保存能力。
    [[nodiscard]] static Result<MapDocument> OpenReadOnlyArchive(const std::filesystem::path& path);
    [[nodiscard]] static MapDocument FromMembers(std::span<const MapArchiveMember> members,
        const std::filesystem::path& source = {});
    // root/ 与 map/ 同名成员、原生 W3I 与 table/w3i.ini 同时存在均视为冲突。
    // 读取必须匹配 DirectoryWorkspace 的内容指纹。
    [[nodiscard]] static Result<MapDocument> FromDirectory(const DirectoryWorkspace& workspace);

    [[nodiscard]] const std::array<MapMemberState, 7>& Members() const noexcept { return members_; }
    [[nodiscard]] const MapMemberState& Member(MapDataKind kind) const;
    [[nodiscard]] const std::filesystem::path& SourcePath() const noexcept { return source_; }
    [[nodiscard]] std::vector<Diagnostic> Diagnostics() const;
    // 仅检查本轮所需的输入数据，不代表 config/main 或外部工具链已实现。
    [[nodiscard]] Status InitializationSourceStatus() const;
    [[nodiscard]] Result<std::string> ResolveString(std::string_view value) const;

    [[nodiscard]] const auto& Settings() const noexcept { return settings_; }
    // 非空时完整性与原始文本来自 LNI，不能将其当作原生 W3I 的逐字节往返证据。
    [[nodiscard]] const auto& LniSettings() const noexcept { return lniSettings_; }
    [[nodiscard]] const auto& Units() const noexcept { return units_; }
    [[nodiscard]] const auto& Doodads() const noexcept { return doodads_; }
    [[nodiscard]] const auto& Regions() const noexcept { return regions_; }
    [[nodiscard]] const auto& Cameras() const noexcept { return cameras_; }
    [[nodiscard]] const auto& Sounds() const noexcept { return sounds_; }
    [[nodiscard]] const auto& Strings() const noexcept { return strings_; }

private:
    friend struct MapData::detail::MapDataFieldEditingAccess;
    MapDocument();
    void ParseMembers();
    std::filesystem::path source_;
    std::array<MapMemberState, 7> members_;
    std::optional<MapData::MapSettings> settings_;
    std::optional<MapData::LniMapSettings> lniSettings_;
    std::optional<MapData::UnitPlacements> units_;
    std::optional<MapData::DoodadPlacements> doodads_;
    std::optional<MapData::RegionsDocument> regions_;
    std::optional<MapData::CamerasDocument> cameras_;
    std::optional<MapData::SoundsDocument> sounds_;
    std::optional<MapData::StringTableDocument> strings_;
};

} // namespace War3Map
