#include "War3MapCore/Resources/TerrainTable.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace War3Map::Resources {
namespace {

constexpr std::string_view kSubsystem = "Resources.TerrainTable";
// 表头、重复行、列名及rawcode规则改变时须升版；不能复用旧的资格身份。
constexpr std::string_view kParserVersion = "forge-terrain-table-catalog-strict-v1";

struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(std::string code, std::string message) {
    throw Failure{{{DiagnosticSeverity::Error, std::string(kSubsystem),
        std::move(code), std::move(message), std::nullopt}}};
}

template <typename T>
T Take(Result<T> result) {
    if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

void ValidateLimits(const TerrainTableLimits& limits) {
    const TerrainTableLimits hard;
    if (limits.maxTableBytes > hard.maxTableBytes || limits.maxRows > hard.maxRows
        || limits.maxCells > hard.maxCells || limits.maxCellBytes > hard.maxCellBytes)
        Fail("TERRAIN_TABLE_LIMITS", "地形表限额只能收紧，不能放宽硬上限。");
}

bool Rawcode(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return c >= 33 && c <= 126;
    });
}

std::string Where(const TerrainTableRow& row) {
    return row.table.canonicalPath + " 行 " + std::to_string(row.row) + "（" + row.id + "）";
}

// 原TerrainMaterials::ReadRows的严格语义集中在这里；只解释表结构，保留未知业务列。
std::vector<TerrainTableRow> ParseRows(std::span<const std::byte> bytes, const ResourceLocation& location,
    std::string_view idColumn, const TerrainTableLimits& limits) {
    ValidateLimits(limits);
    const auto& path = location.canonicalPath;
    if (bytes.size() > limits.maxTableBytes)
        Fail("TERRAIN_TABLE_BUDGET", path + " 超出单表字节预算。");
    std::string_view text;
    if (!bytes.empty()) text = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    auto cells = Take(Text::ParseSlkCells(text, Text::SlkCellMode::CatalogStrict));
    if (cells.size() - 1 > limits.maxRows)
        Fail("TERRAIN_TABLE_BUDGET", path + " 超出表行数预算。");
    std::size_t cellCount = 0;
    for (const auto& [rowIndex, row] : cells) {
        (void)rowIndex;
        if (row.size() > limits.maxCells - cellCount)
            Fail("TERRAIN_TABLE_BUDGET", path + " 超出单元格数量预算。");
        cellCount += row.size();
        for (const auto& [column, value] : row) {
            (void)column;
            if (value.size() > limits.maxCellBytes)
                Fail("TERRAIN_TABLE_BUDGET", path + " 单元格文本超出预算。");
        }
    }
    const auto& headers = cells.at(1);
    if (Text::LowerAscii(headers.at(1)) != idColumn)
        Fail("TERRAIN_TABLE_HEADER", path + " 第一列标题不是 " + std::string(idColumn));
    std::map<int, std::string> names;
    std::set<std::string> uniqueNames;
    for (const auto& [column, value] : headers) {
        auto name = Text::LowerAscii(value);
        if (name.empty() || !uniqueNames.insert(name).second)
            Fail("TERRAIN_TABLE_HEADER", path + " 包含空白或重复列名。");
        names.emplace(column, std::move(name));
    }
    std::vector<TerrainTableRow> rows;
    std::set<std::string> ids;
    for (const auto& [rowIndex, row] : cells) {
        if (rowIndex == 1) continue;
        const auto id = row.find(1);
        if (id == row.end() || !Rawcode(id->second))
            Fail("TERRAIN_TABLE_ID", path + " 行 " + std::to_string(rowIndex) + " 缺少四字节 ASCII rawcode。");
        if (!ids.insert(id->second).second)
            Fail("TERRAIN_TABLE_DUPLICATE", path + " 重复 rawcode：" + id->second);
        TerrainTableRow entry{id->second, location, rowIndex, {}};
        for (const auto& [column, value] : row) {
            const auto name = names.find(column);
            if (name == names.end())
                Fail("TERRAIN_TABLE_HEADER", Where(entry) + " 存在未声明列的单元格。");
            entry.fields.emplace(name->second, value);
        }
        rows.push_back(std::move(entry));
    }
    return rows;
}

std::string Identity(const ResourceLocation& location, std::size_t byteCount, std::string_view sourceSha256,
    std::string_view idColumn, const TerrainTableLimits& limits) {
    std::string manifest;
    const auto append = [&](std::string_view value) {
        manifest += std::to_string(value.size());
        manifest += ':';
        manifest += value;
    };
    append(kParserVersion);
    append(location.canonicalPath);
    append(std::to_string(static_cast<int>(location.layer)));
    append(std::to_string(location.priority));
    append(location.sourceId);
    append(location.sourcePath);
    append(std::to_string(byteCount));
    append(sourceSha256);
    append(idColumn);
    append(std::to_string(limits.maxTableBytes));
    append(std::to_string(limits.maxRows));
    append(std::to_string(limits.maxCells));
    append(std::to_string(limits.maxCellBytes));
    return Take(Sha256Text(manifest));
}

bool SameLocation(const ResourceLocation& a, const ResourceLocation& b) {
    return a.canonicalPath == b.canonicalPath && a.layer == b.layer && a.priority == b.priority
        && a.sourceId == b.sourceId && a.sourcePath == b.sourcePath;
}

template <typename T, typename Operation>
Result<T> Checked(Operation operation) {
    try {
        return Result<T>::Success(operation());
    } catch (Failure& failure) {
        Result<T> result;
        result.diagnostics = std::move(failure.diagnostics);
        return result;
    } catch (const std::bad_alloc&) {
        return Result<T>::Failure(std::string(kSubsystem), "TERRAIN_TABLE_ALLOCATION", "地形表捕获内存分配失败。");
    } catch (const std::length_error&) {
        return Result<T>::Failure(std::string(kSubsystem), "TERRAIN_TABLE_ALLOCATION", "地形表捕获超出容器容量。");
    }
}

} // namespace

Result<TerrainTableSnapshot> CaptureTerrainTable(const ResourceResolver& resolver,
    std::string_view path, std::string_view idColumn, const TerrainTableLimits& limits) {
    return Checked<TerrainTableSnapshot>([&] {
        ValidateLimits(limits);
        auto resource = Take(resolver.Read(path));
        auto rows = ParseRows(resource.bytes, resource.location, idColumn, limits);
        TerrainTableSnapshot snapshot;
        snapshot.location_ = std::move(resource.location);
        snapshot.bytes_ = std::move(resource.bytes);
        snapshot.sourceSha256_ = Take(Sha256(snapshot.bytes_));
        snapshot.idColumn_ = idColumn;
        snapshot.limits_ = limits;
        snapshot.identitySha256_ = Identity(snapshot.location_, snapshot.bytes_.size(), snapshot.sourceSha256_,
            snapshot.idColumn_, snapshot.limits_);
        snapshot.rows_ = std::move(rows);
        return snapshot;
    });
}

Result<std::vector<TerrainTableRow>> ReplayTerrainTable(const TerrainTableSnapshot& snapshot) {
    return Checked<std::vector<TerrainTableRow>>([&] {
        ValidateLimits(snapshot.Limits());
        const auto sourceSha256 = Take(Sha256(snapshot.Bytes()));
        if (sourceSha256 != snapshot.SourceSha256()
            || Identity(snapshot.Location(), snapshot.Bytes().size(), sourceSha256, snapshot.IdColumn(), snapshot.Limits())
                != snapshot.IdentitySha256())
            Fail("TERRAIN_TABLE_SNAPSHOT_INVALID", "地形表快照的原字节或捕获身份不一致，拒绝内存重放。");
        return ParseRows(snapshot.Bytes(), snapshot.Location(), snapshot.IdColumn(), snapshot.Limits());
    });
}

Status VerifyTerrainTableSnapshot(const TerrainTableSnapshot& snapshot, const ResourceResolver& resolver) {
    auto current = CaptureTerrainTable(resolver, snapshot.Location().canonicalPath, snapshot.IdColumn(), snapshot.Limits());
    if (!current.Succeeded()) return {std::move(current.diagnostics)};
    const auto& actual = *current.value;
    if (!SameLocation(snapshot.Location(), actual.Location())
        || snapshot.SourceSha256() != actual.SourceSha256() || snapshot.IdentitySha256() != actual.IdentitySha256()
        || !std::equal(snapshot.Bytes().begin(), snapshot.Bytes().end(), actual.Bytes().begin(), actual.Bytes().end()))
        return Status::Failure(std::string(kSubsystem), "TERRAIN_TABLE_SOURCE_CHANGED",
            "地形表的生效来源、遮蔽身份或原字节已变化；须重新捕获材质事实。");
    return {std::move(current.diagnostics)};
}

} // namespace War3Map::Resources
