#pragma once

#include "War3MapCore/Resources/ResourceResolver.h"

#include <map>
#include <span>

namespace War3Map::Resources {

struct TerrainTableLimits {
    // 默认值也是硬上限；读取回调本身仍负责限制实际文件读取。
    std::size_t maxTableBytes = 2 * 1024 * 1024;
    std::size_t maxRows = 4096;
    std::size_t maxCells = 262144;
    std::size_t maxCellBytes = 4096;
};

struct TerrainTableRow {
    std::string id; // rawcode 区分大小写。
    ResourceLocation table;
    int row = 0; // SYLK 行号，不是文本字节偏移。
    std::map<std::string, std::string, std::less<>> fields; // 小写列名，原始值和未知列保留。
};

class TerrainTableSnapshot;

// 只捕获指定表的实际读取依赖；不读取图片、不解释通行位，也不是保存放行凭据。
[[nodiscard]] Result<TerrainTableSnapshot> CaptureTerrainTable(
    const ResourceResolver& resolver, std::string_view path, std::string_view idColumn,
    const TerrainTableLimits& limits = {});

class TerrainTableSnapshot {
public:
    [[nodiscard]] const ResourceLocation& Location() const noexcept { return location_; }
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::string& SourceSha256() const noexcept { return sourceSha256_; }
    [[nodiscard]] const std::string& IdentitySha256() const noexcept { return identitySha256_; }
    [[nodiscard]] const std::string& IdColumn() const noexcept { return idColumn_; }
    [[nodiscard]] const TerrainTableLimits& Limits() const noexcept { return limits_; }
    [[nodiscard]] const std::vector<TerrainTableRow>& Rows() const noexcept { return rows_; }

private:
    TerrainTableSnapshot() = default;
    ResourceLocation location_;
    std::vector<std::byte> bytes_;
    std::string sourceSha256_;
    std::string identitySha256_;
    std::string idColumn_;
    TerrainTableLimits limits_;
    std::vector<TerrainTableRow> rows_;
    friend Result<TerrainTableSnapshot> CaptureTerrainTable(
        const ResourceResolver&, std::string_view, std::string_view, const TerrainTableLimits&);
};

// 重放只消费快照原字节；复核重新解析同一路径的生效来源和字节。
// 不证明未访问的低层来源/整棵资源树，也不建立外部长期执行租约。
[[nodiscard]] Result<std::vector<TerrainTableRow>> ReplayTerrainTable(const TerrainTableSnapshot& snapshot);
[[nodiscard]] Status VerifyTerrainTableSnapshot(
    const TerrainTableSnapshot& snapshot, const ResourceResolver& resolver);

} // namespace War3Map::Resources
