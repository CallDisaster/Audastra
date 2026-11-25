#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::MapData {

// 外部摆放清单 sonne-placements/v1 的格式读取：导入文件、自定义装饰物/单位类型和摆放实例。
// 只做格式与取值校验；类型建立、地图范围和提交由 Services::PlanPlacementImport 负责。
inline constexpr std::string_view PlacementManifestFormatV1 = "sonne-placements/v1";
inline constexpr std::string_view PlacementManifestArchivePrefix = "Sonne\\";
inline constexpr std::size_t PlacementManifestMaximumInstances = 100000;
inline constexpr std::size_t PlacementManifestMaximumTypes = 4096;
inline constexpr std::size_t PlacementManifestMaximumFiles = 4096;
inline constexpr std::size_t PlacementManifestMaximumFileBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t PlacementManifestMaximumTotalFileBytes = 1024u * 1024u * 1024u;
inline constexpr std::size_t PlacementManifestMaximumBytes = 64u * 1024u * 1024u;

enum class PlacementManifestKind : std::uint8_t { Doodad, Unit };

struct PlacementManifestFile {
    std::string source;      // 相对清单目录
    std::string archivePath; // 包内路径，Sonne\ 开头，反斜杠分隔
    std::vector<std::byte> bytes;
};

struct PlacementManifestType {
    std::string key;
    PlacementManifestKind kind = PlacementManifestKind::Doodad;
    std::string model;
    std::string name;                // 缺省为 key
    std::optional<std::string> base; // 单位必填
    bool followGround = false;       // 仅装饰物：false 时写入保持 Z 标志 0x04
};

struct PlacementManifestInstance {
    std::uint32_t type = 0; // types 下标
    std::array<float, 3> position{};
    float facingDegrees = 0; // 从正东逆时针
    std::array<float, 3> scale{1, 1, 1};
    std::int32_t variation = 0;
};

struct PlacementManifest {
    std::vector<PlacementManifestFile> files; // Parse 只给路径；Read 同时装入字节
    std::vector<PlacementManifestType> types;
    std::vector<PlacementManifestInstance> instances;
};

[[nodiscard]] Result<PlacementManifest> ParsePlacementManifest(std::span<const std::byte> json);
// 读取清单并装入 files 的字节（相对清单所在目录，单个与总量均有上限）。
[[nodiscard]] Result<PlacementManifest> ReadPlacementManifest(const std::filesystem::path& manifest);

} // namespace War3Map::MapData
