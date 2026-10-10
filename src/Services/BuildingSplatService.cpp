#include "War3MapCore/Services/BuildingSplatService.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <set>
#include <tuple>

namespace War3Map::Services {
namespace {
constexpr std::string_view TablePath = "SPLATS\\UBERSPLATDATA.SLK";
struct ParseFailure { std::string code, text; };
[[noreturn]] void Fail(std::string code, std::string text) { throw ParseFailure{std::move(code), std::move(text)}; }
void IdentityPart(std::string& identity, std::string_view value) {
    identity += std::to_string(value.size()); identity += ':'; identity += value;
}
std::string Where(int row, std::string_view field) {
    return "UberSplat表行 " + std::to_string(row) + " 字段 " + std::string(field);
}
template<typename T> T Number(const BuildingSplatDefinition& definition, std::string_view field) {
    const auto found = definition.rawFields.find(std::string(field));
    if (found == definition.rawFields.end()) Fail("BUILDING_SPLAT_FIELD_MISSING", Where(static_cast<int>(definition.tableRow), field) + " 缺失。");
    const auto value = Text::TrimCWhitespace(found->second);
    T parsed{}; const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
        || !std::isfinite(static_cast<double>(parsed)))
        Fail("BUILDING_SPLAT_FIELD_INVALID", Where(static_cast<int>(definition.tableRow), field) + " 不是有效有限数。");
    return parsed;
}
std::array<std::uint32_t, 4> RawLifecycleColor(const BuildingSplatDefinition& definition, std::string_view prefix) {
    std::array<std::uint32_t, 4> result{};
    constexpr std::array<std::string_view, 4> channels{"r", "g", "b", "a"};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const auto field = std::string(prefix) + std::string(channels[i]);
        result[i] = Number<std::uint32_t>(definition, field);
    }
    return result;
}
bool Rawcode(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}
std::optional<std::size_t> InputLogicalBytes(const BuildingSplatUnitInput& input) {
    if (input.fields.origins.size() > 64 || input.fields.uubs.size() > 4096 || input.fields.reason.size() > 4096)
        return std::nullopt;
    std::size_t bytes = sizeof(BuildingSplatRow) + input.owner.rawcode.size()
        + input.version.document.documentId.size() + input.version.document.sourceSha256.size()
        + input.fields.uubs.size() + input.fields.reason.size();
    for (const auto& origin : input.fields.origins) {
        if (origin.field.size() > 4096 || origin.objectRawcode.size() > 4096
            || origin.sourceId.size() > 4096 || origin.sourcePath.size() > 4096) return std::nullopt;
        bytes += sizeof(origin) + origin.field.size() + origin.objectRawcode.size() + origin.sourceId.size() + origin.sourcePath.size();
    }
    return bytes;
}
bool HasOrigin(const BuildingSplatEffectiveFields& fields, std::string_view field) {
    return std::any_of(fields.origins.begin(), fields.origins.end(), [&](const auto& origin) {
        return origin.field == field && Rawcode(origin.objectRawcode) && !origin.sourceId.empty() && !origin.sourcePath.empty();
    });
}
}
Result<std::shared_ptr<const BuildingSplatCatalog>> ParseBuildingSplatCatalog(
    Resources::ResolvedResource resource, const BuildingSplatCatalogLimits& limits) try {
    using Output = Result<std::shared_ptr<const BuildingSplatCatalog>>;
    const BuildingSplatCatalogLimits hard;
    if (limits.maxTableBytes > hard.maxTableBytes || limits.maxRows > hard.maxRows
        || limits.maxCells > hard.maxCells || limits.maxCellBytes > hard.maxCellBytes)
        return Output::Failure("BuildingSplat", "BUILDING_SPLAT_LIMIT_INVALID", "贴画目录预算只能收紧。");
    if (resource.bytes.size() > limits.maxTableBytes)
        return Output::Failure("BuildingSplat", "BUILDING_SPLAT_TABLE_BUDGET", "贴画目录原字节超出预算。");
    const auto canonical = Resources::CanonicalImportPath(resource.location.canonicalPath);
    if (!canonical.Succeeded() || *canonical.value != TablePath || resource.location.sourceId.empty()
        || resource.location.sourcePath.empty() || resource.location.sourceId.size() > 4096 || resource.location.sourcePath.size() > 4096)
        return Output::Failure("BuildingSplat", "BUILDING_SPLAT_SOURCE_INVALID", "目录必须绑定实际Splats表路径与来源。");
    auto sha = Sha256(resource.bytes); if (!sha.Succeeded()) return {std::nullopt, std::move(sha.diagnostics)};
    std::string_view text(reinterpret_cast<const char*>(resource.bytes.data()), resource.bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    auto parsed = Text::ParseSlkCells(text, Text::SlkCellMode::CatalogStrict);
    if (!parsed.Succeeded()) return {std::nullopt, std::move(parsed.diagnostics)};
    const auto& cells = *parsed.value;
    if (cells.size() - 1 > limits.maxRows) Fail("BUILDING_SPLAT_TABLE_BUDGET", "贴画目录行数超出预算。");
    std::size_t count = 0;
    for (const auto& [row, values] : cells) {
        (void)row;
        if (values.size() > limits.maxCells - count) Fail("BUILDING_SPLAT_TABLE_BUDGET", "贴画目录单元格超出预算。");
        count += values.size();
        for (const auto& [column, value] : values) {
            (void)column;
            if (value.size() > limits.maxCellBytes) Fail("BUILDING_SPLAT_TABLE_BUDGET", "贴画目录单元格文本超出预算。");
        }
    }
    std::map<int, std::string> columns; std::set<std::string> names;
    for (const auto& [column, value] : cells.at(1)) {
        auto name = Text::LowerAscii(value);
        if (name.empty() || !names.insert(name).second) Fail("BUILDING_SPLAT_HEADER_INVALID", "贴画目录有空白或重复列名。");
        columns.emplace(column, std::move(name));
    }
    if (columns.at(1) != "name") Fail("BUILDING_SPLAT_HEADER_INVALID", "贴画目录首列不是Name。");
    auto catalog = std::make_shared<BuildingSplatCatalog>();
    catalog->source = std::move(resource.location); catalog->tableSha256 = std::move(*sha.value);
    catalog->originalBytes = std::move(resource.bytes);
    for (const auto& [row, values] : cells) {
        if (row == 1) continue;
        const auto key = values.find(1);
        if (key == values.end() || !Rawcode(key->second)) Fail("BUILDING_SPLAT_KEY_INVALID", Where(row, "name") + " 缺少四字节key。");
        BuildingSplatDefinition definition; definition.key = key->second; definition.tableRow = static_cast<std::uint32_t>(row);
        for (const auto& [column, value] : values) {
            const auto name = columns.find(column);
            if (name == columns.end()) Fail("BUILDING_SPLAT_HEADER_INVALID", Where(row, "column") + " 有未声明列。");
            definition.rawFields.emplace(name->second, value);
        }
        const auto dir = definition.rawFields.find("dir"), file = definition.rawFields.find("file");
        if (dir == definition.rawFields.end() || file == definition.rawFields.end() || file->second.empty())
            Fail("BUILDING_SPLAT_TEXTURE_INVALID", Where(row, "dir/file") + " 缺少纹理路径。");
        auto path = dir->second.empty() ? file->second : dir->second + '/' + file->second;
        if (!Text::LowerAscii(path).ends_with(".blp")) path += ".blp";
        auto texture = Resources::CanonicalImportPath(path);
        if (!texture.Succeeded()) Fail("BUILDING_SPLAT_TEXTURE_INVALID", Where(row, "dir/file") + " 纹理路径不安全。");
        definition.texturePath = std::move(*texture.value);
        definition.scale = Number<float>(definition, "scale"); definition.birth = Number<float>(definition, "birthtime");
        definition.pause = Number<float>(definition, "pausetime"); definition.decay = Number<float>(definition, "decay");
        definition.tableBlendMode = Number<std::int32_t>(definition, "blendmode");
        definition.start = RawLifecycleColor(definition, "start"); definition.middle = RawLifecycleColor(definition, "middle"); definition.end = RawLifecycleColor(definition, "end");
        if (!catalog->definitions.emplace(definition.key, std::move(definition)).second)
            Fail("BUILDING_SPLAT_KEY_DUPLICATE", Where(row, "name") + " 重复key。");
    }
    std::string identity = "BuildingSplatCatalog/" + std::to_string(BuildingSplatCatalog::schemaVersion) + ':';
    IdentityPart(identity, catalog->tableSha256); IdentityPart(identity, *canonical.value);
    IdentityPart(identity, catalog->source.sourceId); IdentityPart(identity, catalog->source.sourcePath);
    IdentityPart(identity, std::to_string(static_cast<int>(catalog->source.layer)));
    IdentityPart(identity, std::to_string(catalog->source.priority));
    auto digest = Sha256Text(identity); if (!digest.Succeeded()) return {std::nullopt, std::move(digest.diagnostics)};
    catalog->identitySha256 = std::move(*digest.value);
    return Output::Success(std::move(catalog));
} catch (const ParseFailure& error) {
    return Result<std::shared_ptr<const BuildingSplatCatalog>>::Failure("BuildingSplat", error.code, error.text);
} catch (const std::exception&) {
    return Result<std::shared_ptr<const BuildingSplatCatalog>>::Failure("BuildingSplat", "BUILDING_SPLAT_PARSE_FAILED", "贴画目录读取失败，未返回部分目录。");
}

Result<BuildingSplatPage> QueryBuildingSplatFootprints(std::shared_ptr<const BuildingSplatCatalog> catalog,
    const BuildingSplatQueryContext& context, const WorldSelectionVersion& expected, std::string_view expectedCatalogIdentity,
    const BuildingSplatQueryOptions& options, const std::optional<BuildingSplatCursor>& cursor) try {
    using Output = Result<BuildingSplatPage>;
    if (!catalog || catalog->identitySha256.empty() || catalog->identitySha256 != expectedCatalogIdentity || context.version != expected
        || context.inputIdentity.empty() || context.inputIdentity.size() > 4096 || context.version.document.documentId.empty()
        || context.version.document.documentId.size() > 4096 || !context.version.document.generation
        || context.version.document.sourceSha256.empty() || context.version.document.sourceSha256.size() > 4096)
        return Output::Failure("BuildingSplat", "BUILDING_SPLAT_QUERY_STALE", "贴画目录、有效对象输入或文档修订不匹配。");
    if (!options.limit || options.limit > 128 || !options.maxLogicalBytes || options.maxLogicalBytes > 1024 * 1024)
        return Output::Failure("BuildingSplat", "BUILDING_SPLAT_QUERY_LIMIT", "贴画查询预算无效。");
    if (cursor && (cursor->version != expected || cursor->catalogIdentity != expectedCatalogIdentity
        || cursor->inputIdentity != context.inputIdentity || cursor->nextInput > context.units.size()))
        return Output::Failure("BuildingSplat", "BUILDING_SPLAT_CURSOR_STALE", "贴画继续页身份已失效。");
    const auto start = cursor ? static_cast<std::size_t>(cursor->nextInput) : 0;
    BuildingSplatPage page; page.version = expected; page.catalog = std::move(catalog);
    std::set<std::tuple<MapData::PlacementFile, std::uint32_t, std::string, std::int32_t>> owners;
    auto next = start;
    for (; next < context.units.size() && page.rows.size() < options.limit; ++next) {
        const auto& input = context.units[next];
        if (input.version != expected || input.owner.file != MapData::PlacementFile::Units || !Rawcode(input.owner.rawcode)
            || !owners.emplace(input.owner.file, input.owner.index, input.owner.rawcode, input.owner.creationId).second)
            return Output::Failure("BuildingSplat", "BUILDING_SPLAT_OWNER_STALE", "贴画owner类型、来源修订或页内身份重复，未返回部分结果。");
        const auto bytes = InputLogicalBytes(input);
        if (!bytes) return Output::Failure("BuildingSplat", "BUILDING_SPLAT_INPUT_BUDGET", "贴画有效字段来源文本超出预算。");
        // 固定预留诊断与footprint载荷；不将输入预算当地图总数限制。
        const auto needed = *bytes + sizeof(BuildingSplatFootprint) + 256;
        if (needed > options.maxLogicalBytes - page.logicalBytes) {
            if (page.rows.empty()) return Output::Failure("BuildingSplat", "BUILDING_SPLAT_QUERY_BUDGET", "单个贴画行超出查询输出预算。");
            break;
        }
        BuildingSplatRow row; row.owner = input.owner; row.version = expected; row.inputIndex = next;
        row.splatKey = input.fields.uubs; row.origins = input.fields.origins;
        if (input.fields.configuration == BuildingSplatConfiguration::NotApplicableToNonBuilding
            && input.fields.complete && input.fields.uubs.empty() && !HasOrigin(input.fields, "uubs")
            && HasOrigin(input.fields, "ubdg") && HasOrigin(input.fields, "hero") && HasOrigin(input.fields, "creep")
            && HasOrigin(input.fields, "uubs-applicability-schema")) {
            row.availability = BuildingSplatAvailability::Absent;
            row.reasonCode = "BUILDING_SPLAT_NOT_APPLICABLE";
            row.reason = input.fields.reason;
        } else if (input.fields.configuration != BuildingSplatConfiguration::EffectiveField
            || !input.fields.complete || !HasOrigin(input.fields, "uubs")) {
            row.reasonCode = "BUILDING_SPLAT_DATA_UNAVAILABLE";
            row.reason = input.fields.reason.empty() ? "有效uubs或父链来源不完整。" : input.fields.reason;
        } else if (row.splatKey.empty() || row.splatKey == "_") {
            row.availability = BuildingSplatAvailability::Absent;
        } else if (const auto found = page.catalog->definitions.find(row.splatKey); found == page.catalog->definitions.end()) {
            row.reasonCode = "BUILDING_SPLAT_KEY_UNKNOWN";
            row.reason = "有效uubs不在当前Splats目录中。";
        } else if (!HasOrigin(input.fields, "uclr") || !HasOrigin(input.fields, "uclg") || !HasOrigin(input.fields, "uclb")) {
            row.reasonCode = "BUILDING_SPLAT_TINT_UNAVAILABLE";
            row.reason = "对象tint来源不完整，未填白色默认值。";
        } else if (std::any_of(input.fields.tint.begin(), input.fields.tint.end(), [](std::uint32_t value) { return value > 255; })) {
            row.reasonCode = "BUILDING_SPLAT_TINT_INVALID";
            row.reason = "有效对象tint必须在0..255内；未截断或归一生命周期表原值。";
        } else {
            const auto scale = found->second.scale;
            if (!std::all_of(input.position.begin(), input.position.end(), [](float value) { return std::isfinite(value) && std::abs(value) <= 1000000; })
                || scale <= 0 || scale > 1000000) {
                row.reasonCode = "BUILDING_SPLAT_FOOTPRINT_UNAVAILABLE";
                row.reason = "位置或独立表尺寸超出当前有界footprint支持。";
            }
            else {
                BuildingSplatFootprint footprint;
                footprint.minimum = {input.position[0] - scale, input.position[1] - scale};
                footprint.maximum = {input.position[0] + scale, input.position[1] + scale};
                footprint.fullWidth = footprint.fullHeight = 2 * scale;
                footprint.color = {static_cast<std::uint8_t>(input.fields.tint[0]), static_cast<std::uint8_t>(input.fields.tint[1]),
                    static_cast<std::uint8_t>(input.fields.tint[2]), 255};
                row.availability = BuildingSplatAvailability::Available; row.footprint = footprint;
            }
        }
        page.logicalBytes += needed; page.rows.push_back(std::move(row));
    }
    if (next < context.units.size()) page.next = BuildingSplatCursor{expected, page.catalog->identitySha256, context.inputIdentity, next};
    return Output::Success(std::move(page));
} catch (const std::exception&) {
    return Result<BuildingSplatPage>::Failure("BuildingSplat", "BUILDING_SPLAT_QUERY_FAILED", "贴画查询失败，未返回部分结果。");
}
} // namespace War3Map::Services
