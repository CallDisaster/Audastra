#include "War3MapCore/MapData/TerrainPathingResources.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/MapData/PlacementElevationInvariant.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainBoundsEditing.h"
#include "War3MapCore/MapData/TerrainEditInspection.h"
#include "War3MapCore/Resources/TerrainTable.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
// v1 wire先写W3E、三张地形表和56项启动profile，历史上随后还写入地图Misc缺失。
// 成功的旧owner只可能得到下面的缺失tuple；它仅保留wire兼容，绝非实际来源证明。
static_assert(kClassicProfiles.size() == 56, "v1 dependency wire requires an explicit migration when the startup chain changes");
constexpr std::size_t kWaterV1StartupDependencies = 60;
constexpr std::string_view kWaterV1ReservedMisc = "war3mapmisc.txt";
constexpr std::string_view kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
}

std::size_t TerrainPathingResources::WaterBrushDependencyBudgetBytes() const noexcept {
    if (!waterBrushRules_ || waterBrushInputSha_.empty() || waterBrushDoodadsSha_.empty()) return 0;
    std::size_t maximum = 1024 + kWaterV1ReservedMisc.size() + kEmptySha256.size() + 64;
    if (dependencies_.size() < kWaterV1StartupDependencies || dependencies_.size() >= 4096) return 0;
    for (const auto& dependency : dependencies_) {
        for (const auto* value : {&dependency.name, &dependency.sha256, &dependency.gameArchive}) {
            if (value->size() > 1024 * 1024 - maximum) return 0;
            maximum += value->size();
        }
        if (64 > 1024 * 1024 - maximum) return 0;
        maximum += 64;
    }
    return maximum;
}

Result<std::string> TerrainPathingResources::WaterBrushDependencyFingerprint() const try {
    if (!waterBrushRules_ || waterBrushInputSha_.empty() || waterBrushDoodadsSha_.empty())
        return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_JOURNAL_RULES_MISSING", "水刷记录缺少实际规则或完整DOO依赖。");
    const auto maximum = WaterBrushDependencyBudgetBytes();
    if (!maximum)
        return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "水刷依赖摘要超过有界记录预算。");
    // 长度前缀避免字符串拼接歧义；真实依赖按捕获链排序，历史常量槽不进入依赖集合。
    std::vector<std::byte> wire;
    wire.reserve(maximum);
    const auto number = [&wire](std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) wire.push_back(std::byte((value >> (i * 8)) & 255));
    };
    const auto text = [&wire, &number](std::string_view value) {
        number(value.size()); const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        wire.insert(wire.end(), bytes.begin(), bytes.end());
    };
    text("terrain-water-dependencies-v1"); text(waterBrushInputSha_); number(waterBrushInputBytes_);
    text(waterBrushDoodadsSha_); number(waterBrushRules_->enforceWaterLimits); number(dependencies_.size() + 1);
    std::size_t index = 0;
    for (const auto& dependency : dependencies_) {
        text(dependency.name); text(dependency.sha256); text(dependency.gameArchive); number(dependency.bytes);
        number(dependency.mapPresent); number(dependency.mapOnly); number(dependency.gameAbsent);
        if (++index == kWaterV1StartupDependencies) {
            text(kWaterV1ReservedMisc); text(kEmptySha256); text(""); number(0);
            number(false); number(false); number(true);
        }
    }
    return Sha256(wire);
} catch (const std::bad_alloc&) {
    return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "水刷依赖摘要分配失败。");
} catch (const std::length_error&) {
    return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BUDGET", "水刷依赖摘要超出容器预算。");
}
namespace {
constexpr auto kSubsystem = "Terrain.PathingResources";
constexpr std::size_t kTableBytes = 2 * 1024 * 1024;
struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class T, class F> Result<T> Guard(F&& operation) {
    try { return Result<T>::Success(operation()); }
    catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Result<T>::Failure(kSubsystem, "TERRAIN_PATHING_RESOURCE_BUDGET", "通行输入分配失败，未返回部分资格。"); }
    catch (const std::length_error&) { return Result<T>::Failure(kSubsystem, "TERRAIN_PATHING_RESOURCE_BUDGET", "通行输入超过容器容量，未返回部分资格。"); }
}
std::string Canonical(std::string_view name) {
    auto key = Text::LowerAscii(name);
    std::replace(key.begin(), key.end(), '\\', '/');
    return key;
}
bool SameOrdinaryDoodadWire(std::span<const std::byte> before, const DoodadPlacements& original,
    std::span<const std::byte> after, const DoodadPlacements& current) {
    const auto prefix = [](std::span<const std::byte> bytes, const DoodadPlacements& parsed) {
        // 调用者已用完整经典reader读到EOF；特殊尾是version/count及每项16字节。
        // 直接比较其前全部原字，包含头、普通数量、嵌套掉落物和浮点位，不复制或重写记录。
        if (parsed.recordLayout != PlacementRecordLayout::Classic
            || !IsClassicPlacementVersion(parsed.version, parsed.subversion) || parsed.specialVersion != 0
            || bytes.size() < 8 || parsed.specialDoodads.size() > (bytes.size() - 8) / 16)
            return std::span<const std::byte>{};
        return bytes.first(bytes.size() - 8 - parsed.specialDoodads.size() * 16);
    };
    const auto oldPrefix = prefix(before, original), newPrefix = prefix(after, current);
    return !oldPrefix.empty() && std::equal(oldPrefix.begin(), oldPrefix.end(), newPrefix.begin(), newPrefix.end());
}
std::string_view Ground(const TerrainHeader& header, const TerrainVertex& vertex) {
    const auto index = vertex.GroundTextureIndex();
    return index < header.groundTilesets.size() ? std::string_view(header.groundTilesets[index]) : std::string_view{};
}
struct Needs {
    std::set<std::string, std::less<>> ground;
    bool water = false;
};
Needs Required(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after) {
    Needs needs;
    if (before.vertices.size() != after.vertices.size()) return needs;
    // 未使用的新槽位也必须存在于真实资源表；未使用的旧槽位不参与通行等价比较。
    for (std::size_t i = 0; i < after.header.groundTilesets.size(); ++i) {
        if (i >= before.header.groundTilesets.size()
            || after.header.groundTilesets[i] != before.header.groundTilesets[i])
            needs.ground.insert(after.header.groundTilesets[i]);
    }
    for (std::size_t i = 0; i < before.vertices.size(); ++i) {
        const auto& a = before.vertices[i]; const auto& b = after.vertices[i];
        const auto oldGround = Ground(before.header, a), newGround = Ground(after.header, b);
        if (oldGround != newGround) { needs.ground.emplace(oldGround); needs.ground.emplace(newGround); }
        if ((a.HasWater() || b.HasWater()) && (a.groundHeightRaw != b.groundHeightRaw
            || a.waterHeightAndFlagsRaw != b.waterHeightAndFlagsRaw || a.HasWater() != b.HasWater()
            || a.CliffLevel() != b.CliffLevel())) needs.water = true;
    }
    return needs;
}
std::string_view Field(const Resources::TerrainTableRow& row, const char* name) {
    const auto found = row.fields.find(name);
    if (found == row.fields.end()) Fail("TERRAIN_PATHING_PROPERTY_MISSING", row.id + " 缺少通行字段 " + name);
    return found->second;
}
bool Boolean(std::string_view text, std::string_view field) {
    text = Text::TrimCWhitespace(text);
    if (text != "0" && text != "1") Fail("TERRAIN_PATHING_PROPERTY_INVALID", std::string(field) + " 必须是明确的 0 或 1，不能猜测缺省值。");
    return text == "1";
}
float Number(std::string_view text, std::string_view field) {
    text = Text::TrimCWhitespace(text);
    float result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(result)
        || std::abs(result) > 65536) Fail("TERRAIN_PATHING_PROPERTY_INVALID", std::string(field) + " 不是受支持的有限数值。");
    return result;
}
Resources::TerrainTableSnapshot Table(const std::string& path, std::string_view id,
    const std::vector<std::byte>& bytes) {
    Resources::ResourceResolver resolver;
    Resources::ResourceSource source;
    source.id = "captured-terrain-pathing-table";
    source.layer = Resources::ResourceLayer::SessionOverride;
    source.probe = [&path](std::string_view) {
        return Result<Resources::ResourceProbe>::Success({Resources::ResourceProbeState::Found, path});
    };
    source.read = [&bytes](std::string_view) { return Result<std::vector<std::byte>>::Success(bytes); };
    const auto added = resolver.AddSource(std::move(source));
    if (!added.Succeeded()) throw Failure{added.diagnostics};
    return Take(Resources::CaptureTerrainTable(resolver, path, id));
}
std::vector<ArchiveExactMember> DirectoryMembers(const WorkspaceSnapshot& expected,
    std::span<const std::string> names, const ArchiveExactMemberLimits& limits, bool allowSettingsLni = false) {
    // 用现有目录快照与只读SHA租约读取；不按外部提供的files向量拼接任意路径。
    auto directory = Take(DirectoryWorkspace::Open(expected.root));
    if (directory.Snapshot().aggregateSha256 != expected.aggregateSha256)
        Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "目录通行来源已不属于准备时的快照。");
    if (names.size() > limits.memberCount)
        Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "目录通行成员数量超过捕获预算。");
    std::vector<ArchiveExactMember> output;
    std::size_t total = 0;
    for (const auto& name : names) {
        const auto key = Canonical(name);
        const WorkspaceFile* selected = nullptr;
        for (const auto& file : directory.Snapshot().files) {
            const auto path = Canonical(IO::GenericPathUtf8(file.relativePath));
            if ((!allowSettingsLni && key == "war3map.w3i" && path == "table/w3i.ini")
                || (key == "war3map.w3b" && (path == "table/destructable.ini" || path == "table/doodad.ini"))
                || (key == "war3mapmisc.txt" && path == "table/misc.ini"))
                Fail("TERRAIN_NATIVE_SOURCE_LAYOUT", "目录含尚未绑定到原生通行输入的LNI覆盖：" + path);
            if (path != key && path != "map/" + key) continue;
            if (selected) Fail("TERRAIN_PATHING_RESOURCE_AMBIGUOUS", "目录通行输入有多个同名来源：" + name);
            selected = &file;
        }
        ArchiveExactMember member{name, selected != nullptr, {}};
        if (selected) {
            if (selected->size > limits.memberBytes || selected->size > limits.totalBytes - total)
                Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "目录通行输入超过捕获预算：" + name);
            member.bytes = Take(ReadVerifiedFile(directory.Snapshot().root / selected->relativePath,
                selected->sha256, std::min(limits.memberBytes, limits.totalBytes - total)));
            if (member.bytes.size() != selected->size)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "目录通行输入长度已改变：" + name);
            total += member.bytes.size();
        }
        output.push_back(std::move(member));
    }
    const auto checked = directory.VerifyUnchanged();
    if (!checked.Succeeded()) throw Failure{checked.diagnostics};
    return output;
}
std::vector<std::byte> TerrainWire(const TerrainGridSnapshot& grid, std::size_t maximumBytes) {
    const auto tables = grid.header.groundTilesets.size() + grid.header.cliffTilesets.size();
    if (tables > (std::numeric_limits<std::size_t>::max() - 37) / 4)
        Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "原版通行地形表长度溢出。");
    const auto header = 37 + tables * 4;
    if (header > maximumBytes || grid.vertices.size() > (maximumBytes - header) / 7)
        Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "原版通行地形身份超过捕获预算。");
    return Take(SerializeMapTerrain(grid));
}
void SameScalar(std::optional<float>& target, float value, const char* name) {
    if (target && *target != value)
        Fail("TERRAIN_PATHING_PROPERTY_AMBIGUOUS", std::string(name) + " 存在不同的重复定义，不能以后值覆盖原版首值。");
    target = value;
}
void RejectLooseInput(const std::filesystem::path& root, std::string_view name) {
    if (root.empty()) return;
    // 不读取HKCU来猜AllowLocalFiles；已知同名loose入口一律拒绝，包括悬空链接。
    // 每级检查可区分真正缺失与非目录父项/无法检查，不能把检查错误当作不存在。
    auto current = root;
    const auto relative = std::filesystem::u8path(name);
    for (auto item = relative.begin(); item != relative.end(); ++item) {
        current /= *item;
        std::error_code error;
        const auto state = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) return;
        if (error) Fail("TERRAIN_WATER_RULE_SOURCE", "无法检查经典规则的loose来源：" + IO::GenericPathUtf8(current));
        if (!std::filesystem::exists(state)) return;
        auto next = item; ++next;
        if (next == relative.end() || !std::filesystem::is_directory(state))
            Fail("TERRAIN_WATER_RULE_SOURCE", "经典规则存在未纳入MPQ来源链的loose入口：" + IO::GenericPathUtf8(current));
    }
}
} // namespace

bool TerrainPathingResources::Needed(const TerrainGridSnapshot& before, const TerrainGridSnapshot& after) {
    const auto needs = Required(before, after);
    return !needs.ground.empty() || needs.water || before.header.groundTilesets != after.header.groundTilesets
        || before.header.usesCustomTileset != after.header.usesCustomTileset
        || (before.vertices.size() == after.vertices.size() && !std::equal(before.vertices.begin(), before.vertices.end(),
            after.vertices.begin(), [](const auto& a, const auto& b) { return a.CliffLevel() == b.CliffLevel(); }));
}

Result<TerrainPathingResources> TerrainPathingResources::Capture(const MapArchiveSession& source,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    return CaptureInputs([&](auto names, const auto& limits) { return source.CaptureExactMembers(names, limits); },
        before, after, gameRoot, maximumBytes, false);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureNative(const MapArchiveSession& source,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    return CaptureInputs([&](auto names, const auto& limits) { return source.CaptureExactMembers(names, limits); },
        before, after, gameRoot, maximumBytes, true);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureNative(const WorkspaceSnapshot& source,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    return CaptureInputs([&](auto names, const auto& limits) {
        return Guard<std::vector<ArchiveExactMember>>([&] { return DirectoryMembers(source, names, limits); });
    }, before, after, gameRoot, maximumBytes, true);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureNativeWithObjects(const MapArchiveSession& source,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, const NativeObjectValidator& validator, const std::vector<std::byte>* currentDoodads) {
    return CaptureInputs([&](auto names, const auto& limits) { return source.CaptureExactMembers(names, limits); },
        before, after, gameRoot, maximumBytes, true, &validator, currentDoodads);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureNativeWithObjects(const WorkspaceSnapshot& source,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, const NativeObjectValidator& validator, const std::vector<std::byte>* currentDoodads) {
    return CaptureInputs([&](auto names, const auto& limits) {
        return Guard<std::vector<ArchiveExactMember>>([&] { return DirectoryMembers(source, names, limits); });
    }, before, after, gameRoot, maximumBytes, true, &validator, currentDoodads);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureWaterBrushRules(const MapArchiveSession& source,
    const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
    const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes) {
    return CaptureWaterBrushInputs([&](auto names, const auto& limits) { return source.CaptureExactMembers(names, limits); },
        sourceTerrain, currentTerrain, gameRoot, enforceWaterLimits, maximumBytes);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureWaterBrushRules(const WorkspaceSnapshot& source,
    const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
    const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes) {
    return CaptureWaterBrushInputs([&](auto names, const auto& limits) {
        return Guard<std::vector<ArchiveExactMember>>([&] { return DirectoryMembers(source, names, limits); });
    }, sourceTerrain, currentTerrain, gameRoot, enforceWaterLimits, maximumBytes);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureWaterBrushRules(const MapArchiveSession& source,
    const TerrainGridSnapshot& before, const std::filesystem::path& gameRoot,
    bool enforceWaterLimits, std::size_t maximumBytes) {
    return CaptureWaterBrushRules(source, before, before, gameRoot, enforceWaterLimits, maximumBytes);
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureWaterBrushRules(const WorkspaceSnapshot& source,
    const TerrainGridSnapshot& before, const std::filesystem::path& gameRoot,
    bool enforceWaterLimits, std::size_t maximumBytes) {
    return CaptureWaterBrushRules(source, before, before, gameRoot, enforceWaterLimits, maximumBytes);
}

Result<std::vector<ArchiveExactMember>> TerrainPathingResources::ReadDirectoryMembers(const WorkspaceSnapshot& source,
    std::span<const std::string> names, const ArchiveExactMemberLimits& limits, bool allowSettingsLni) {
    return Guard<std::vector<ArchiveExactMember>>([&] { return DirectoryMembers(source, names, limits, allowSettingsLni); });
}

Status TerrainPathingResources::CaptureWaterSpecialInputs(const MemberReader& reader,
    const TerrainGridSnapshot& terrain, const DoodadPlacements& doodads, const NativeObjectValidator& validator) {
    auto captured = Guard<bool>([&] {
        if (!waterBrushRules_) Fail("TERRAIN_WATER_RULE_OWNER", "特殊水刷输入缺少规则owner。");
        PlacementReadLimits limits;
        limits.maxBytes = std::min(limits.maxBytes, (capturedMaximumBytes_ - sourceBytes_) / 32);
        const auto projection = Take(MeasureClassicPlacementWire(doodads, {}, limits));
        objectWorkBytes_ += projection.byteLength * 32;
        {
            const auto wire = Take(doodads.Serialize(limits));
            waterBrushDoodadsSha_ = Take(Sha256(wire));
        }
        if (doodads.specialDoodads.empty()) return true;
        // 普通DOO由会话保存门继续核对；这里只请求特殊类型，不要求无关普通对象具备水刷占地资格。
        std::vector<ArchiveExactMember> sources;
        const std::array<std::string, 3> mapNames{"war3map.w3b", "war3map.w3d", "war3map.doo"};
        ArchiveExactMemberLimits mapLimits; mapLimits.memberCount = mapNames.size();
        mapLimits.memberBytes = mapLimits.totalBytes = capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_;
        auto mapMembers = Take(reader(mapNames, mapLimits));
        if (mapMembers.size() != mapNames.size()) Fail("TERRAIN_WATER_SPECIAL_SOURCE", "特殊类型来源捕获不完整。");
        for (auto& member : mapMembers) {
            if (member.bytes.size() > capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_) Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "特殊类型来源超过预算。");
            sourceBytes_ += member.bytes.size();
            dependencies_.push_back({Canonical(member.name), Take(Sha256(member.bytes)), {}, member.bytes.size(), member.present, true, false});
            if (Canonical(member.name) == "war3map.doo") {
                if (!member.present) Fail("TERRAIN_WATER_SPECIAL_SOURCE", "特殊水刷缺少实际来源DOO。");
                nativeDoodads_ = std::move(member.bytes);
            } else sources.push_back(std::move(member));
        }
        const std::array<std::string, 3> tables{"Units/DestructableData.slk", "Doodads/Doodads.slk", "Units/DestructableMetaData.slk"};
        auto status = CaptureResourceMembers(reader, tables, tables.size(), [&](std::size_t index, bool present, std::vector<std::byte>&& bytes) {
            sources.push_back({tables[index], present, std::move(bytes)});
        });
        if (!status.Succeeded()) throw Failure{status.diagnostics};
        std::vector<std::string_view> rawcodes; rawcodes.reserve(doodads.specialDoodads.size());
        for (const auto& object : doodads.specialDoodads) rawcodes.push_back(object.rawcode);
        auto inspected = validator({}, rawcodes, sources, capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_);
        if (!inspected.value && inspected.diagnostics.size() == 1
            && inspected.diagnostics.front().severity == DiagnosticSeverity::Error
            && inspected.diagnostics.front().subsystem == "Terrain.NativeObjects"
            && inspected.diagnostics.front().code == "TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED") {
            const std::array<std::string, 1> extra{"Doodads/DoodadMetaData.slk"};
            status = CaptureResourceMembers(reader, extra, 1, [&](std::size_t, bool present, std::vector<std::byte>&& bytes) {
                sources.push_back({extra[0], present, std::move(bytes)});
            });
            if (!status.Succeeded()) throw Failure{status.diagnostics};
            inspected = validator({}, rawcodes, sources, capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_);
        }
        const auto objects = Take(std::move(inspected));
        if (objects.workBytes > capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_) Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "特殊类型解析超过预算。");
        objectWorkBytes_ += objects.workBytes;
        status = CaptureSpecialFootprints(reader, terrain, doodads.specialDoodads, objects.specialFootprints,
            waterBrushRules_->maximumCliffLevel, 1);
        if (!status.Succeeded()) throw Failure{status.diagnostics};
        return true;
    });
    return {captured.diagnostics};
}

Status TerrainPathingResources::VerifyWaterBrushDoodads(const DoodadPlacements& doodads, std::span<const std::byte> source) const {
    auto checked = Guard<bool>([&] {
        if (waterBrushDoodadsSha_.empty()) {
            if (!doodads.specialDoodads.empty()) Fail("TERRAIN_WATER_SPECIAL_DOODAD_FACTS", "Core兼容规则未捕获特殊模型的实际类型与贴图。");
        } else {
            PlacementReadLimits limits; limits.maxBytes = std::min(limits.maxBytes, capturedMaximumBytes_ / 32);
            (void)Take(MeasureClassicPlacementWire(doodads, {}, limits));
            if (Take(Sha256(Take(doodads.Serialize(limits)))) != waterBrushDoodadsSha_)
                Fail("TERRAIN_WATER_SPECIAL_SOURCE", "水刷当前DOO已改变，不能使用旧形状。");
            if (!nativeDoodads_.empty() && !source.empty()
                && !std::equal(source.begin(), source.end(), nativeDoodads_.begin(), nativeDoodads_.end()))
                Fail("TERRAIN_WATER_SPECIAL_SOURCE", "水刷实际DOO来源不属于当前会话基线。");
        }
        return true;
    });
    return {checked.diagnostics};
}

Status TerrainPathingResources::CaptureResourceMembers(const MemberReader& reader,
    std::span<const std::string> names, std::size_t requiredCount, const ResourceVisitor& visitor) {
    auto checked = Guard<bool>([&] {
        if (sourceBytes_ > capturedMaximumBytes_ || objectWorkBytes_ > capturedMaximumBytes_ - sourceBytes_
            || requiredCount > names.size()
            || names.size() > std::numeric_limits<std::size_t>::max() / kTableBytes)
            Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "通行资源成员列表或累计额度无效。");
        const auto remaining = [&] { return capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_; };
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size();
        limits.memberBytes = std::min(kTableBytes, remaining());
        limits.totalBytes = std::min(names.size() * kTableBytes, remaining());
        auto captured = Take(reader(names, limits));
        if (captured.size() != names.size())
            Fail("TERRAIN_PATHING_RESOURCE_MISSING", "精确资源捕获未返回完整成员集合。");
        // 地图成员整批保留后才读取游戏回退；两类调用共享同一存储/解码预算。
        for (const auto& member : captured) {
            if (member.bytes.size() > remaining())
                Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "通行表与配置超过保存计划剩余预算。");
            sourceBytes_ += member.bytes.size();
        }
        std::optional<Game::GameDataSource> game;
        for (std::size_t i = 0; i < captured.size(); ++i) {
            auto& member = captured[i];
            auto bytes = std::move(member.bytes);
            bool present = member.present;
            Dependency dependency;
            dependency.name = Canonical(names[i]); dependency.mapPresent = member.present;
            if (!member.present) {
                if (gameRoot_.empty())
                    Fail("TERRAIN_PATHING_RESOURCE_MISSING", "资源回退来源未知，不能把未知当缺失：" + names[i]);
                if (!game) game = Take(Game::GameDataSource::MountStrict(gameRoot_, tileset_));
                if (i >= requiredCount && !game->HasFile(names[i])) {
                    dependency.gameAbsent = true;
                } else {
                    auto file = Take(game->ReadFileBoundedWithSource(names[i],
                        std::min(kTableBytes, remaining())));
                    bytes = std::move(file.bytes); dependency.gameArchive = std::move(file.archiveName);
                    if (bytes.size() > remaining())
                        Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "通行表与配置超过保存计划剩余预算。");
                    sourceBytes_ += bytes.size(); present = true;
                }
            }
            dependency.bytes = bytes.size(); dependency.sha256 = Take(Sha256(bytes));
            const auto previous = std::find_if(dependencies_.begin(), dependencies_.end(),
                [&](const auto& value) { return value.name == dependency.name; });
            if (previous == dependencies_.end()) {
                dependencies_.push_back(std::move(dependency));
            } else if (previous->bytes != dependency.bytes || previous->sha256 != dependency.sha256
                || previous->mapPresent != dependency.mapPresent || previous->mapOnly != dependency.mapOnly
                || previous->gameAbsent != dependency.gameAbsent || previous->gameArchive != dependency.gameArchive) {
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "同次捕获的通行输入来源或内容不一致：" + dependency.name);
            }
            // 多类规则可共用同一输入；每次读取仍计预算并供各自解析，回读只请求一个确定身份。
            // 仍按原顺序逐项读取winner；严格对象适配可移走需要共同解析的原字节，
            // 其读取量和解析工作均计入同一次捕获预算。
            visitor(i, present, std::move(bytes));
        }
        return true;
    });
    return {std::move(checked.diagnostics)};
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureInputs(const MemberReader& reader,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, bool native,
    const NativeObjectValidator* validator, const std::vector<std::byte>* currentDoodads) {
    return Guard<TerrainPathingResources>([&] {
        TerrainPathingResources result;
        result.tileset_ = before.header.mainTileset;
        result.capturedMaximumBytes_ = maximumBytes;
        if (!gameRoot.empty()) {
            std::error_code error;
            result.gameRoot_ = std::filesystem::absolute(gameRoot, error).lexically_normal();
            if (error) Fail("TERRAIN_PATHING_RESOURCE_ROOT", "无法固定通行输入的游戏来源路径。");
        }
        auto needs = Required(before, after);
        std::optional<MapSettings> settings;
        std::optional<DoodadPlacements> doodads;
        std::set<std::string, std::less<>> cliffs;
        bool nativeOverrides = false;
        bool nativeRamp = false;
        std::vector<ArchiveExactMember> objectSources;
        if (native) {
            if (before.header.version != 11 || after.header.version != 11
                || before.header.width != after.header.width || before.header.height != after.header.height
                || before.header.offset != after.header.offset || before.header.mainTileset != after.header.mainTileset
                || before.header.cliffTilesets != after.header.cliffTilesets
                || before.vertices.size() != after.vertices.size())
                Fail("TERRAIN_NATIVE_SOURCE_SHAPE", "原版通行重建要求旧/新经典地形尺寸、偏移与地形集不变。");
            for (const auto* grid : {&before, &after}) for (const auto& vertex : grid->vertices) {
                nativeRamp = nativeRamp || vertex.IsRamp();
            }
            for (std::size_t i = 0; i < before.vertices.size(); ++i) {
                const auto oldSlot = before.vertices[i].CliffTextureIndex(), newSlot = after.vertices[i].CliffTextureIndex();
                if (oldSlot == newSlot) continue;
                const bool oldKnown = oldSlot < 15 && oldSlot < before.header.cliffTilesets.size();
                const bool newKnown = newSlot < 15 && newSlot < after.header.cliffTilesets.size();
                if ((!oldKnown || !newKnown) && !IsNativeTerrainCliffSentinelTransition(before, after, i))
                    Fail("TERRAIN_NATIVE_CLIFF_SLOT", "悬崖槽变化需要实际有效槽，或已证明平坦安全的15号哨兵过渡：顶点" + std::to_string(i));
                // 平坦15号哨兵没有材质行；只捕获实际有效一侧，不伪造旧/新资源。
                // 哨兵侧的完整邻域仍由共享判定核实，G0/G1和旧WPM逐格比较不变。
                if (oldKnown) cliffs.insert(before.header.cliffTilesets[oldSlot]);
                if (newKnown) cliffs.insert(after.header.cliffTilesets[newSlot]);
            }
            const std::vector<std::string> metadata{"war3map.w3e", "war3map.w3i", "war3map.doo", "war3map.w3b"};
            ArchiveExactMemberLimits limits;
            limits.memberCount = metadata.size(); limits.memberBytes = limits.totalBytes = maximumBytes;
            auto captured = Take(reader(metadata, limits));
            for (std::size_t i = 0; i < captured.size(); ++i) {
                auto& member = captured[i];
                if (member.bytes.size() > maximumBytes - result.sourceBytes_)
                    Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "原版通行元数据超过捕获预算。");
                result.sourceBytes_ += member.bytes.size();
                Dependency dependency;
                dependency.name = Canonical(metadata[i]); dependency.mapPresent = member.present; dependency.mapOnly = true;
                dependency.bytes = member.bytes.size(); dependency.sha256 = Take(Sha256(member.bytes));
                result.dependencies_.push_back(std::move(dependency));
                if (i == 0) {
                    // 两份身份缓冲分开存活，不让其伴随整个元数据捕获生命周期。
                    {
                        const auto wire = TerrainWire(before, maximumBytes);
                        if (!member.present || member.bytes != wire)
                            Fail("TERRAIN_NATIVE_SOURCE_IDENTITY", "旧地形快照不属于本次实际来源W3E。");
                        result.nativeTerrainSha_[0] = Take(Sha256(wire));
                        result.nativeTerrainBytes_[0] = wire.size();
                    }
                    {
                        const auto wire = TerrainWire(after, maximumBytes);
                        result.nativeTerrainSha_[1] = Take(Sha256(wire));
                        result.nativeTerrainBytes_[1] = wire.size();
                    }
                } else if (i == 1) {
                    if (!member.present) Fail("TERRAIN_NATIVE_W3I_MISSING", "原版通行需要实际来源W3I。");
                    settings = Take(MapSettings::Parse(member.bytes));
                    if (!settings->FullyParsed() || settings->formatVersion != 25)
                        Fail("TERRAIN_NATIVE_W3I_LAYOUT", "原版通行需要完整classic v25 W3I。");
                } else if (i == 2) {
                    if (!member.present) Fail("TERRAIN_NATIVE_DOODADS_MISSING", "尚未证明缺失DOO等价于完整空对象集合。");
                    PlacementReadLimits placementLimits; placementLimits.maxBytes = maximumBytes;
                    doodads = Take(ParseDoodadPlacements(member.bytes, placementLimits));
                    if (!doodads->specialDoodads.empty() && !validator)
                        Fail("TERRAIN_NATIVE_SPECIAL_UNSUPPORTED", "war3map.doo 特殊地形记录0（地格"
                            + std::to_string(doodads->specialDoodads.front().position[0]) + ","
                            + std::to_string(doodads->specialDoodads.front().position[1]) + "）尚未接入原版通行重建。");
                    result.nativeDoodads_ = std::move(member.bytes);
                } else {
                    nativeOverrides = member.present;
                    if (validator) objectSources.push_back(std::move(member));
                }
            }
            for (const auto* grid : {&before, &after}) for (std::size_t i = 0; i < grid->vertices.size(); ++i) {
                const auto id = Ground(grid->header, grid->vertices[i]);
                if (id.empty()) Fail("TERRAIN_NATIVE_GROUND_SLOT", "原版通行顶点地面索引越界：" + std::to_string(i));
                needs.ground.emplace(id);
                needs.water |= grid->vertices[i].HasWater();
            }
            if (!doodads->doodads.empty() && !validator)
                Fail(nativeOverrides ? "TERRAIN_NATIVE_OBJECT_OVERRIDE_UNSUPPORTED" : "TERRAIN_NATIVE_OBJECT_ADAPTER_REQUIRED",
                    "非空普通DOO需要严格对象解析适配，不能只凭可破坏物单表证明类别和静态贡献。");
        }
        std::optional<DoodadPlacements> editedDoodads;
        const DoodadPlacements* afterDoodads = doodads ? &*doodads : nullptr;
        if (native) {
            result.nativeDoodadsFrozen_ = currentDoodads != nullptr;
            if (currentDoodads) {
                if (currentDoodads->size() > maximumBytes - result.sourceBytes_)
                    Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "当前DOO身份超过捕获预算。");
                result.sourceBytes_ += currentDoodads->size();
                result.nativeAfterDoodads_ = *currentDoodads;
                editedDoodads = Take(ParseDoodadPlacements(*currentDoodads)); afterDoodads = &*editedDoodads;
            } else result.nativeAfterDoodads_ = result.nativeDoodads_;
        }
        const bool typedObjects = native && (!doodads->doodads.empty() || !doodads->specialDoodads.empty()
            || !afterDoodads->specialDoodads.empty()) && validator;
        if (typedObjects) {
            // 有无W3B均核对实际对象类别；W3D缺失也固定身份，不能用新导入遮蔽分类。
            const std::array<std::string, 1> names{"war3map.w3d"};
            ArchiveExactMemberLimits limits;
            limits.memberCount = 1; limits.memberBytes = limits.totalBytes = maximumBytes - result.sourceBytes_;
            auto captured = Take(reader(names, limits));
            if (captured.size() != 1 || captured[0].bytes.size() > maximumBytes - result.sourceBytes_)
                Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "W3D分类来源不完整或超过累计预算。");
            auto& member = captured[0];
            result.sourceBytes_ += member.bytes.size();
            result.dependencies_.push_back({Canonical(names[0]), Take(Sha256(member.bytes)), {}, member.bytes.size(), member.present, true, false});
            objectSources.push_back(std::move(member));
        }
        std::vector<std::string> names;
        if (!needs.ground.empty()) names.emplace_back("TerrainArt/Terrain.slk");
        if (needs.water) names.emplace_back("TerrainArt/Water.slk");
        if (!cliffs.empty()) names.emplace_back("TerrainArt/CliffTypes.slk");
        const bool camera = native;
        if (needs.water || camera) {
            names.emplace_back("UI/MiscData.txt");
            names.emplace_back("UI/WorldEditData.txt");
        }
        if (typedObjects) {
            names.emplace_back("Units/DestructableData.slk");
            names.emplace_back("Doodads/Doodads.slk");
            names.emplace_back("Units/DestructableMetaData.slk");
        }
        if (names.empty()) return result;
        std::optional<float> deepThreshold;
        std::array<std::optional<float>, 4> cameraMargins;
        std::optional<float> waterHeight;
        bool waterImpassable = false;
        const auto visitResource = [&](std::size_t i, bool present, std::vector<std::byte>&& bytes) {
            if (typedObjects && (names[i] == "Units/DestructableData.slk"
                    || names[i] == "Doodads/Doodads.slk" || names[i] == "Units/DestructableMetaData.slk")) {
                objectSources.push_back({names[i], present, std::move(bytes)});
                return;
            }
            if (names[i] == "TerrainArt/Terrain.slk") {
                const auto table = Table(names[i], "tileid", bytes);
                for (const auto& row : table.Rows()) if (needs.ground.contains(row.id)) {
                    result.ground_.emplace(row.id, std::array{Boolean(Field(row, "walkable"), "walkable"),
                        Boolean(Field(row, "flyable"), "flyable"), Boolean(Field(row, "buildable"), "buildable")});
                }
                if (result.ground_.size() != needs.ground.size())
                    Fail("TERRAIN_PATHING_PROPERTY_MISSING", "Terrain.slk 缺少旧/新地面材质的完整通行行。");
                if (native) for (const auto& [rawcode, fields] : result.ground_)
                    result.nativeGround_.push_back({rawcode, fields[0], fields[1], fields[2]});
            } else if (names[i] == "TerrainArt/Water.slk") {
                const auto table = Table(names[i], "waterid", bytes);
                const auto id = std::string(1, static_cast<char>(result.tileset_)) + "Sha";
                const auto row = std::find_if(table.Rows().begin(), table.Rows().end(), [&](const auto& item) { return item.id == id; });
                if (row == table.Rows().end()) Fail("TERRAIN_PATHING_PROPERTY_MISSING", "Water.slk 缺少当前地形集的浅水行。");
                // 原版记录先存float再乘128；不以视觉深水阈值64替代通行阈值。
                waterHeight = Number(Field(*row, "height"), "Water.height");
                if (native) waterImpassable = Boolean(Field(*row, "impassable"), "Water.impassable");
            } else if (names[i] == "TerrainArt/CliffTypes.slk") {
                const auto table = Table(names[i], "cliffid", bytes);
                for (const auto& id : cliffs) if (std::none_of(table.Rows().begin(), table.Rows().end(),
                    [&](const auto& row) { return row.id == id; }))
                    Fail("TERRAIN_PATHING_PROPERTY_MISSING", "CliffTypes.slk缺少悬崖槽变化的实际行：" + id);
            } else {
                auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
                Text::VisitProfileFields(text, [&](std::string_view section, std::string_view key, std::string_view value) {
                    if (camera && section == "cameramargins") {
                        constexpr std::array<std::string_view, 4> keys{"left", "right", "bottom", "top"};
                        for (std::size_t n = 0; n < keys.size(); ++n) if (key == keys[n]) {
                            const auto margin = Number(value, "CameraMargins");
                            if (margin < 0 || std::trunc(margin) != margin)
                                Fail("TERRAIN_PATHING_PROPERTY_INVALID", "CameraMargins必须是有界非负整数，与原版int getter一致。");
                            SameScalar(cameraMargins[n], margin, "CameraMargins");
                        }
                    }
                    if (!needs.water || section != "water" || key != "deeplevelpathing") return;
                    const auto threshold = Number(value, "Water.DeepLevelPathing");
                    if (threshold < 0) Fail("TERRAIN_PATHING_PROPERTY_INVALID", "DeepLevelPathing 不能为负。");
                    // 原版读取追加值链的首值。来源合并尚有未决项，不能用后值覆盖冲突后声称通行等价。
                    if (deepThreshold && *deepThreshold != threshold)
                        Fail("TERRAIN_PATHING_PROPERTY_AMBIGUOUS", "Water.DeepLevelPathing 存在不同的重复定义，无法确认水深通行阈值。");
                    deepThreshold = threshold;
                });
            }
        };
        const auto resources = result.CaptureResourceMembers(reader, names, names.size(), visitResource);
        if (!resources.Succeeded()) throw Failure{resources.diagnostics};
        if (typedObjects) {
            // 在reserve之前检查列表额度；validator的累计工作预留包括这块空间。
            const auto records = doodads->doodads.size() + doodads->specialDoodads.size() + afterDoodads->specialDoodads.size();
            if (records > (maximumBytes - result.sourceBytes_) / (sizeof(std::string_view) * 4))
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "摆放分类列表超过剩余解析预算。");
            std::vector<std::string_view> rawcodes;
            rawcodes.reserve(doodads->doodads.size());
            for (const auto& object : doodads->doodads) rawcodes.emplace_back(object.rawcode);
            std::vector<std::string_view> specialRawcodes;
            specialRawcodes.reserve(doodads->specialDoodads.size() + afterDoodads->specialDoodads.size());
            for (const auto& object : doodads->specialDoodads) specialRawcodes.emplace_back(object.rawcode);
            for (const auto& object : afterDoodads->specialDoodads) specialRawcodes.emplace_back(object.rawcode);
            auto validated = (*validator)(rawcodes, specialRawcodes, objectSources, maximumBytes - result.sourceBytes_);
            // 旧五项域不增加依赖；只有可信adapter的精确单一请求才补捕获新域所需表。
            // 首轮解析临时对象已释放，补捕获后按新的剩余额度从头重验，最多一次。
            if (!validated.value && validated.diagnostics.size() == 1
                && validated.diagnostics.front().severity == DiagnosticSeverity::Error
                && validated.diagnostics.front().subsystem == "Terrain.NativeObjects"
                && validated.diagnostics.front().code == "TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED") {
                const std::array<std::string, 1> extra{"Doodads/DoodadMetaData.slk"};
                const auto captured = result.CaptureResourceMembers(reader, extra, extra.size(),
                    [&](std::size_t, bool present, std::vector<std::byte>&& bytes) {
                        objectSources.push_back({extra[0], present, std::move(bytes)});
                    });
                if (!captured.Succeeded()) throw Failure{captured.diagnostics};
                validated = (*validator)(rawcodes, specialRawcodes, objectSources, maximumBytes - result.sourceBytes_);
            }
            auto objectPathing = Take(std::move(validated));
            result.objectWorkBytes_ = objectPathing.workBytes;
            if (result.objectWorkBytes_ > maximumBytes - result.sourceBytes_)
                Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "严格物编解析超过保存计划剩余预算。");
            if (!objectPathing.footprints.empty()) {
                const bool walkable = std::any_of(objectPathing.footprints.begin(), objectPathing.footprints.end(),
                    [](const auto& type) { return type.contribution == TerrainNativeFootprintContribution::WalkableTerrainSuppression; });
                // special删除仅重建地形覆盖，不写普通桥的投影来源。普通段仍逐字冻结，
                // 含桥Z和其它普通对象的Z；尾段变化的保存资格另由会话私有水刷谱系核验。
                if (walkable && result.nativeDoodads_ != result.nativeAfterDoodads_
                    && !SameOrdinaryDoodadWire(result.nativeDoodads_, *doodads, result.nativeAfterDoodads_, *afterDoodads))
                    Fail("TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED", "含可行走对象的地形保存须保持完整普通DOO原字不变；特殊尾段仅允许会话可信水刷事务。");
                const auto captured = result.CaptureNativeFootprints(reader, before.header,
                    doodads->doodads, objectPathing.footprints);
                if (!captured.Succeeded()) throw Failure{captured.diagnostics};
            }
            if (!specialRawcodes.empty()) {
                const bool nonempty = std::any_of(objectPathing.specialFootprints.begin(), objectPathing.specialFootprints.end(),
                    [](const auto& type) { return !type.pathTexture.empty(); });
                if (nonempty && !result.nativeMaximumCliffLevel_)
                    result.nativeMaximumCliffLevel_ = Take(result.CaptureNativeCliffLimit(reader));
                const auto maximumLevel = nonempty ? *result.nativeMaximumCliffLevel_ : std::uint8_t{0};
                if (!doodads->specialDoodads.empty()) {
                    const auto captured = result.CaptureSpecialFootprints(reader, before,
                        doodads->specialDoodads, objectPathing.specialFootprints, maximumLevel, 0);
                    if (!captured.Succeeded()) throw Failure{captured.diagnostics};
                }
                if (!afterDoodads->specialDoodads.empty()) {
                    const auto captured = result.CaptureSpecialFootprints(reader, after,
                        afterDoodads->specialDoodads, objectPathing.specialFootprints, maximumLevel, 1);
                    if (!captured.Succeeded()) throw Failure{captured.diagnostics};
                }
            }
        }
        // 必须在严格物编工作预留赋值之后捕获；有special时复用其同一M，
        // 无special也不能把缺省0当配置值。配置依赖与容器预留均保留在owner中。
        if (nativeRamp && !result.nativeMaximumCliffLevel_)
            result.nativeMaximumCliffLevel_ = Take(result.CaptureNativeCliffLimit(reader));
        if (needs.water) {
            if (!deepThreshold)
                Fail("TERRAIN_PATHING_PROPERTY_MISSING", "当前配置缺少 Water.DeepLevelPathing，不能猜测水深通行状态。");
            result.waterRules_ = TerrainNativeWaterRules{*waterHeight, *deepThreshold, waterImpassable};
        }
        if (native) {
            for (const auto& margin : cameraMargins) if (!margin)
                Fail("TERRAIN_NATIVE_CAMERA_CONFIG", "原版世界可玩范围需要实际CameraMargins四边配置。");
            std::array<float, 4> margins{};
            for (std::size_t i = 0; i < margins.size(); ++i) margins[i] = *cameraMargins[i];
            result.nativeBounds_ = Take(ComputeTerrainNativePlayableBounds(before.header, *settings, margins));
            result.nativeCameraMargins_ = margins;
        }
        return result;
    });
}

Result<TerrainPathingResources> TerrainPathingResources::CaptureWaterBrushInputs(const MemberReader& reader,
    const TerrainGridSnapshot& sourceTerrain, const TerrainGridSnapshot& currentTerrain,
    const std::filesystem::path& gameRoot, bool enforceWaterLimits, std::size_t maximumBytes) {
    return Guard<TerrainPathingResources>([&] {
        TerrainPathingResources result;
        result.tileset_ = sourceTerrain.header.mainTileset;
        result.capturedMaximumBytes_ = maximumBytes;
        if (!gameRoot.empty()) {
            std::error_code error;
            result.gameRoot_ = std::filesystem::absolute(gameRoot, error).lexically_normal();
            if (error) Fail("TERRAIN_PATHING_RESOURCE_ROOT", "无法固定水刷规则的游戏来源路径。");
        }
        if (sourceTerrain.header.version != 11 || currentTerrain.header.version != 11
            || sourceTerrain.header.width != currentTerrain.header.width
            || sourceTerrain.header.height != currentTerrain.header.height
            || sourceTerrain.header.offset != currentTerrain.header.offset
            || sourceTerrain.header.mainTileset != currentTerrain.header.mainTileset
            || currentTerrain.header.groundTilesets.empty() || currentTerrain.header.groundTilesets.size() > 256
            || currentTerrain.header.cliffTilesets.empty() || currentTerrain.header.cliffTilesets.size() > 256)
            Fail("TERRAIN_WATER_RULE_SOURCE", "水刷规则需要经典W3E及有界的实际地面/悬崖palette。");
        const std::vector<std::string> metadata{"war3map.w3e"};
        ArchiveExactMemberLimits metadataLimits;
        metadataLimits.memberCount = 1; metadataLimits.memberBytes = metadataLimits.totalBytes = maximumBytes;
        auto actual = Take(reader(metadata, metadataLimits));
        if (actual.size() != 1 || !actual.front().present)
            Fail("TERRAIN_WATER_RULE_SOURCE", "水刷规则需要实际来源W3E。");
        {
            const auto wire = TerrainWire(sourceTerrain, maximumBytes);
            if (wire != actual.front().bytes)
                Fail("TERRAIN_WATER_RULE_SOURCE", "水刷sourceTerrain快照不属于本次实际来源W3E。");
        }
        if (actual.front().bytes.size() > maximumBytes)
            Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "水刷规则的W3E身份超过捕获预算。");
        Dependency terrain;
        terrain.name = "war3map.w3e"; terrain.mapPresent = terrain.mapOnly = true;
        terrain.bytes = actual.front().bytes.size(); terrain.sha256 = Take(Sha256(actual.front().bytes));
        result.sourceBytes_ = terrain.bytes; result.dependencies_.push_back(std::move(terrain));
        // W3E仅验证这次规则捕获的原始来源，不建立after或最终保存资格。
        actual.clear();
        {
            const auto wire = TerrainWire(currentTerrain, maximumBytes);
            result.waterBrushInputSha_ = Take(Sha256(wire));
            result.waterBrushInputBytes_ = wire.size();
        }

        std::vector<std::string> names{"TerrainArt/Terrain.slk", "TerrainArt/Water.slk", "TerrainArt/CliffTypes.slk"};
        for (const auto& input : kClassicProfiles) names.emplace_back(input.path);
        // 原版4D1D40从独立的启动profile读取这些规则；地图Misc对象518D60只替换
        // 自己的this+4，不合并到该profile。war3mapMisc.txt不是此计算的依赖。
        for (const auto& name : names) RejectLooseInput(result.gameRoot_, name);
        std::vector<TerrainWaterGroundRule> ground;
        std::vector<std::string> cliffs;
        float waterOffset = 0;
        constexpr std::array<const char*, 4> fields{"Terrain.MaxSlope", "Terrain.MinHeight", "Terrain.MaxHeight", "Water.DeepLevelPathing"};
        std::array<std::optional<float>, 4> values;
        std::array<bool, 4> unguarded{};
        const auto visitResource = [&](std::size_t i, bool present, const std::vector<std::byte>& bytes) {
            if (i == 0) {
                const auto table = Table(names[i], "tileid", bytes);
                std::set<std::string, std::less<>> ids;
                for (const auto& id : currentTerrain.header.groundTilesets) if (ids.insert(id).second) {
                    const auto row = std::find_if(table.Rows().begin(), table.Rows().end(),
                        [&](const auto& item) { return item.id == id; });
                    if (row == table.Rows().end())
                        Fail("TERRAIN_PATHING_PROPERTY_MISSING", "Terrain.slk缺少水刷地面palette行：" + id);
                    // 荒芜资格使用实际buildable，不能误用显示优先级blightPri。
                    ground.push_back({id, Boolean(Field(*row, "buildable"), "Terrain.buildable")});
                }
                return;
            }
            if (i == 1) {
                const auto table = Table(names[i], "waterid", bytes);
                const auto id = std::string(1, static_cast<char>(result.tileset_)) + "Sha";
                const auto row = std::find_if(table.Rows().begin(), table.Rows().end(), [&](const auto& item) { return item.id == id; });
                if (row == table.Rows().end())
                    Fail("TERRAIN_PATHING_PROPERTY_MISSING", "Water.slk缺少水刷当前地形集浅水行。");
                // height先存F32，乘128再落F32；深水阈值不乘128。
                waterOffset = Number(Field(*row, "height"), "Water.height") * 128.0f;
                return;
            }
            if (i == 2) {
                const auto table = Table(names[i], "cliffid", bytes);
                for (const auto& id : currentTerrain.header.cliffTilesets) {
                    if (std::none_of(table.Rows().begin(), table.Rows().end(), [&](const auto& item) { return item.id == id; }))
                        Fail("TERRAIN_PATHING_PROPERTY_MISSING", "CliffTypes.slk缺少水刷悬崖palette行：" + id);
                    if (std::find(cliffs.begin(), cliffs.end(), id) == cliffs.end()) cliffs.push_back(id);
                }
                return;
            }
            if (!present) return;
            std::string_view text;
            if (!bytes.empty()) text = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
            if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
            if (text.find('\0') != std::string_view::npos)
                Fail("TERRAIN_WATER_RULE_SOURCE", "经典规则profile含未知NUL/编码，不能跳过潜在定义：" + names[i]);
            Text::VisitProfileFields(text, [&](std::string_view section, std::string_view key, std::string_view value) {
                std::optional<std::size_t> slot;
                if (section == "terrain") {
                    if (key == "maxslope") slot = 0;
                    else if (key == "minheight") slot = 1;
                    else if (key == "maxheight") slot = 2;
                } else if (section == "water" && key == "deeplevelpathing") slot = 3;
                if (!slot) return;
                // 经典getter读取追加链首值。本有限域要求所有出现均为同一单scalar；
                // CSV/非法值由完整数值解析拒绝，故不能被后续合法定义掩盖。
                SameScalar(values[*slot], Number(value, fields[*slot]), fields[*slot]);
                unguarded[*slot] |= kClassicProfiles[i - 3].alwaysLoad;
            });
        };
        const auto resources = result.CaptureResourceMembers(reader, names, 3, visitResource);
        if (!resources.Succeeded()) throw Failure{resources.diagnostics};
        for (std::size_t i = 0; i < fields.size(); ++i) if (!values[i] || !unguarded[i])
            Fail("TERRAIN_WATER_RULE_SOURCE", std::string(fields[i]) + "未由flag1固定链完整提供；不能猜测未知加载guard。");
        if (*values[0] < 0 || *values[0] > 90 || *values[1] > *values[2] || *values[3] < 0)
            Fail("TERRAIN_PATHING_PROPERTY_INVALID", "实际水刷Terrain/Water规则不在有限支持域内。");
        // 源MaxHeight是F32，随后double计算并向零截断；经典规则没有clamp14。
        const double maximumLevel = std::trunc((static_cast<double>(*values[2]) - static_cast<double>(-256.0f)) / 128.0);
        if (maximumLevel < 0 || maximumLevel > 14)
            Fail("TERRAIN_PATHING_PROPERTY_INVALID", "实际MaxHeight派生悬崖上限超出水刷0..14域，不能截断为兼容默认值。");
        result.waterBrushRules_.emplace(*values[0], *values[1], *values[2], waterOffset, *values[3],
            static_cast<std::uint8_t>(maximumLevel), enforceWaterLimits, std::move(ground), std::move(cliffs));
        return result;
    });
}

Result<std::uint8_t> TerrainPathingResources::CaptureNativeCliffLimit(const MemberReader& reader) {
    return Guard<std::uint8_t>([&] {
        std::vector<std::string> names;
        names.reserve(kClassicProfiles.size());
        for (const auto& profile : kClassicProfiles) names.emplace_back(profile.path);
        // 与水刷使用同一完整启动profile集合；地图独立Misc不覆盖原版地形构造字段。
        constexpr std::size_t perSource = sizeof(ArchiveExactMember) + 2 * sizeof(Dependency) + 8 * 260;
        if (sourceBytes_ > capturedMaximumBytes_ || objectWorkBytes_ > capturedMaximumBytes_ - sourceBytes_
            || names.size() > (capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_) / perSource)
            Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "原版地形层级配置超过剩余捕获预算。");
        objectWorkBytes_ += names.size() * perSource;
        std::optional<float> maximum;
        bool unconditional = false;
        auto status = CaptureResourceMembers(reader, names, 0, [&](std::size_t i, bool present, std::vector<std::byte>&& bytes) {
            if (!present) return;
            std::string_view text;
            if (!bytes.empty()) text = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
            if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
            if (text.find('\0') != std::string_view::npos)
                Fail("TERRAIN_SPECIAL_RULE_SOURCE", "原版地形配置含未知NUL/编码：" + names[i]);
            Text::VisitProfileFields(text, [&](std::string_view section, std::string_view key, std::string_view value) {
                if (section != "terrain" || key != "maxheight") return;
                SameScalar(maximum, Number(value, "Terrain.MaxHeight"), "Terrain.MaxHeight");
                unconditional |= kClassicProfiles[i].alwaysLoad;
            });
        });
        if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)};
        if (!maximum || !unconditional)
            Fail("TERRAIN_SPECIAL_RULE_SOURCE", "原版地形需要固定加载链中明确的Terrain.MaxHeight。");
        const auto level = std::trunc((static_cast<double>(*maximum) + 256.0) / 128.0);
        if (level < 0 || level > 14)
            Fail("TERRAIN_SPECIAL_RULE_SOURCE", "实际原版地形层级上限超出0..14支持域。");
        return static_cast<std::uint8_t>(level);
    });
}

Status TerrainPathingResources::VerifyWaterBrushInput(const TerrainGridSnapshot& sourceTerrain,
    const TerrainGridSnapshot& currentTerrain) const {
    auto checked = Guard<bool>([&] {
        if (!waterBrushRules_ || waterBrushInputSha_.empty())
            Fail("TERRAIN_WATER_RULES_UNAVAILABLE", "此owner未从实际来源捕获水刷编辑规则。");
        const auto source = std::find_if(dependencies_.begin(), dependencies_.end(),
            [](const auto& dependency) { return dependency.name == "war3map.w3e" && dependency.mapOnly && dependency.mapPresent; });
        if (source == dependencies_.end())
            Fail("TERRAIN_WATER_RULES_UNAVAILABLE", "水刷规则缺少实际来源W3E身份。");
        {
            const auto wire = TerrainWire(sourceTerrain, capturedMaximumBytes_);
            if (wire.size() != source->bytes || Take(Sha256(wire)) != source->sha256)
                Fail("TERRAIN_WATER_RULE_SOURCE_CHANGED", "Session原始地形不属于本次实际来源W3E。");
        }
        {
            const auto wire = TerrainWire(currentTerrain, capturedMaximumBytes_);
            if (wire.size() != waterBrushInputBytes_ || Take(Sha256(wire)) != waterBrushInputSha_)
                Fail("TERRAIN_WATER_RULE_INPUT_CHANGED", "Session当前草稿不属于本笔规则捕获时的冻结输入。");
        }
        return true;
    });
    return {std::move(checked.diagnostics)};
}

bool TerrainPathingResources::EquivalentGround(std::string_view before, std::string_view after) const {
    if (before.empty() || after.empty()) return false;
    if (before == after) return true;
    const auto a = ground_.find(before), b = ground_.find(after);
    return a != ground_.end() && b != ground_.end() && a->second == b->second;
}

bool TerrainPathingResources::EquivalentWater(const TerrainVertex& before, const TerrainVertex& after) const {
    if (before.HasWater() != after.HasWater()) return false;
    if (!before.HasWater()) return true;
    if (!waterRules_) return false;
    const auto a = ClassifyNativeTerrainWater(before, *waterRules_);
    const auto b = ClassifyNativeTerrainWater(after, *waterRules_);
    return a.Succeeded() && b.Succeeded() && a.value && b.value && *a.value == *b.value;
}

Result<std::vector<std::byte>> TerrainPathingResources::GenerateNative(const TerrainGridSnapshot& terrain,
    std::size_t maximumBytes) const {
    // 兼容调用只适用于DOO集合不变；正式保存始终显式选择完整旧/新阶段。
    return Guard<std::vector<std::byte>>([&] {
        const auto wire = TerrainWire(terrain, maximumBytes);
        const auto sha = Take(Sha256(wire));
        if (nativeDoodads_ != nativeAfterDoodads_ || nativeAfterBounds_)
            Fail("TERRAIN_NATIVE_STAGE_REQUIRED", "DOO或世界范围发生变化，须显式选择旧或新阶段。");
        return Take(GenerateNative(terrain, sha == nativeTerrainSha_[0] ? TerrainNativeStage::Before : TerrainNativeStage::After, maximumBytes));
    });
}

Result<std::vector<std::byte>> TerrainPathingResources::GenerateNative(const TerrainGridSnapshot& terrain,
    TerrainNativeStage stage, std::size_t maximumBytes) const {
    return Guard<std::vector<std::byte>>([&] {
        const auto index = stage == TerrainNativeStage::Before ? 0u : 1u;
        if (!nativeBounds_ || (stage != TerrainNativeStage::Before && stage != TerrainNativeStage::After))
            Fail("TERRAIN_NATIVE_INPUT_UNAVAILABLE", "未捕获完整原版阶段输入。");
        {
            const auto wire = TerrainWire(terrain, maximumBytes);
            if (wire.size() != nativeTerrainBytes_[index] || Take(Sha256(wire)) != nativeTerrainSha_[index])
                Fail("TERRAIN_NATIVE_SOURCE_IDENTITY", "请求W3E不属于本次捕获的指定旧/新阶段。");
        }
        TerrainNativePathingInput input;
        input.terrain = &terrain;
        input.playableBounds = stage == TerrainNativeStage::After && nativeAfterBounds_ ? *nativeAfterBounds_ : *nativeBounds_;
        input.groundRules = nativeGround_; input.water = waterRules_;
        input.staticAttributes = nativeStaticAttributes_;
        input.specialDoodadCount = nativeSpecialCount_[index]; input.specialAttributes = nativeSpecialAttributes_[index];
        input.maximumCliffLevel = nativeMaximumCliffLevel_;
        return Take(GenerateTerrainNativePathing(input, maximumBytes));
    });
}

Status TerrainPathingResources::VerifyFrozenOutput(std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals) const {
    if (waterBrushRules_)
        return Status::Failure(kSubsystem, "TERRAIN_WATER_RULES_EDIT_ONLY", "水刷规则捕获只用于编辑；最终输出必须重新CaptureNative旧/新修订。");
    auto checked = Guard<bool>([&] {
        auto remaining = capturedMaximumBytes_;
        for (const auto& dependency : dependencies_) {
            const MemberReplacement* replacement = nullptr;
            for (const auto& member : replacements) if (Canonical(member.name) == dependency.name) {
                if (replacement) Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "通行输入被重复覆盖：" + dependency.name);
                replacement = &member;
            }
            for (const auto& removed : removals) if (Canonical(removed) == dependency.name && dependency.mapPresent)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "通行不变保存不能同时删除生效输入：" + dependency.name);
            if (nativeBounds_ && dependency.name == "war3map.w3e") {
                const auto bytes = replacement ? replacement->data.size() : dependency.bytes;
                const auto sha = replacement ? Take(Sha256(replacement->data)) : dependency.sha256;
                if (bytes != nativeTerrainBytes_[1] || sha != nativeTerrainSha_[1])
                    Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "最终W3E不属于本次捕获的新地形修订。");
            } else if (nativeBounds_ && dependency.name == "war3map.w3i" && nativeSettingsOutputSha_) {
                const auto bytes = replacement ? replacement->data.size() : dependency.bytes;
                const auto sha = replacement ? Take(Sha256(replacement->data)) : dependency.sha256;
                if (bytes != nativeSettingsOutputBytes_ || sha != *nativeSettingsOutputSha_)
                    Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "最终W3I遗漏或改变了保存计划冻结的完整说明修改。");
            } else if (nativeBounds_ && dependency.name == "war3map.doo" && dependency.mapPresent && nativeDoodadsFrozen_) {
                const auto& actual = replacement ? replacement->data : nativeDoodads_;
                if (actual != nativeAfterDoodads_) Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "最终DOO遗漏或改变了冻结的特殊集合。");
            } else if (replacement && nativeBounds_ && dependency.name == "war3map.doo" && dependency.mapPresent) {
                if (nativeDoodadsFrozen_) {
                    if (replacement->data != nativeAfterDoodads_) Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "最终DOO不是冻结的新阶段集合。");
                } else {
                    const auto checked = VerifyDoodadPlacementElevationOnly(nativeDoodads_, replacement->data, capturedMaximumBytes_);
                    if (!checked.Succeeded()) throw Failure{checked.diagnostics};
                }
            } else if (replacement && (!dependency.mapPresent || replacement->data.size() != dependency.bytes
                    || Take(Sha256(replacement->data)) != dependency.sha256)) {
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "最终通行输入与准备时不同：" + dependency.name);
            }
            if (dependency.mapPresent) remaining -= dependency.bytes;
        }
        const auto game = VerifyGameDependencies(remaining);
        if (!game.Succeeded()) throw Failure{game.diagnostics};
        return true;
    });
    return {std::move(checked.diagnostics)};
}

Status TerrainPathingResources::VerifyReadback(const MapArchiveSession& saved, std::size_t maximumBytes) const {
    return VerifyReadbackMembers([&](auto names, const auto& limits) { return saved.CaptureExactMembers(names, limits); }, maximumBytes);
}

Status TerrainPathingResources::VerifyReadback(const WorkspaceSnapshot& saved, std::size_t maximumBytes) const {
    return VerifyReadbackMembers([&](auto names, const auto& limits) {
        return Guard<std::vector<ArchiveExactMember>>([&] { return DirectoryMembers(saved, names, limits); });
    }, maximumBytes);
}

Status TerrainPathingResources::VerifySourceInputs(const MapArchiveSession& source, std::size_t maximumBytes) const {
    return VerifyReadbackMembers([&](auto names, const auto& limits) { return source.CaptureExactMembers(names, limits); }, maximumBytes, true);
}

Status TerrainPathingResources::VerifySourceInputs(const WorkspaceSnapshot& source, std::size_t maximumBytes) const {
    return VerifyReadbackMembers([&](auto names, const auto& limits) {
        return Guard<std::vector<ArchiveExactMember>>([&] { return DirectoryMembers(source, names, limits); });
    }, maximumBytes, true);
}

Status TerrainPathingResources::VerifyReadbackMembers(const MemberReader& reader, std::size_t maximumBytes,
    bool sourceInputs) const {
    auto checked = Guard<bool>([&] {
        if (sourceBytes_ > maximumBytes)
            Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "地形成员回读后剩余额度不足以复核通行输入。");
        std::vector<std::string> names;
        for (const auto& dependency : dependencies_) names.push_back(dependency.name);
        if (names.empty()) return true;
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size();
        limits.memberBytes = nativeBounds_ || waterBrushRules_ ? maximumBytes : std::min(kTableBytes, maximumBytes);
        // 读取上限同时约束存储和解码大小；不能以解码长度代替调用者预算。
        limits.totalBytes = maximumBytes;
        const auto actual = Take(reader(names, limits));
        auto remaining = maximumBytes;
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto& expected = dependencies_[i];
            if (actual[i].present != expected.mapPresent)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "保存回读的通行输入不同：" + expected.name);
            if (actual[i].present) {
                if (!sourceInputs && nativeBounds_ && expected.name == "war3map.doo") {
                    if (nativeDoodadsFrozen_) {
                        if (actual[i].bytes != nativeAfterDoodads_) Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "回读DOO不是冻结的新阶段集合。");
                    } else {
                        const auto status = VerifyDoodadPlacementElevationOnly(nativeDoodads_, actual[i].bytes, maximumBytes);
                        if (!status.Succeeded()) throw Failure{status.diagnostics};
                    }
                } else {
                    const bool terrainOutput = !sourceInputs && nativeBounds_ && expected.name == "war3map.w3e";
                    const bool settingsOutput = !sourceInputs && nativeBounds_ && expected.name == "war3map.w3i" && nativeSettingsOutputSha_;
                    const auto size = terrainOutput ? nativeTerrainBytes_[1]
                        : settingsOutput ? nativeSettingsOutputBytes_ : expected.bytes;
                    const auto& sha = terrainOutput ? nativeTerrainSha_[1]
                        : settingsOutput ? *nativeSettingsOutputSha_ : expected.sha256;
                    if (actual[i].bytes.size() != size || Take(Sha256(actual[i].bytes)) != sha)
                        Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "保存回读的通行输入不同：" + expected.name);
                }
            }
            remaining -= actual[i].bytes.size();
        }
        const auto game = VerifyGameDependencies(remaining);
        if (!game.Succeeded()) throw Failure{game.diagnostics};
        return true;
    });
    return {std::move(checked.diagnostics)};
}

Status TerrainPathingResources::VerifyGameDependencies(std::size_t remainingBytes) const {
    auto checked = Guard<bool>([&] {
        std::optional<Game::GameDataSource> game;
        for (const auto& dependency : dependencies_) {
            if (waterBrushRules_ && !dependency.mapOnly) RejectLooseInput(gameRoot_, dependency.name);
            if (dependency.mapPresent || dependency.mapOnly) continue;
            if (!game) game = Take(Game::GameDataSource::MountStrict(gameRoot_, tileset_));
            if (dependency.gameAbsent) {
                if (game->HasFile(dependency.name))
                    Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "准备时缺失的游戏规则输入已出现：" + dependency.name);
                continue;
            }
            const auto actual = Take(game->ReadFileBoundedWithSource(dependency.name,
                std::min(kTableBytes, remainingBytes)));
            if (actual.archiveName != dependency.gameArchive || actual.bytes.size() != dependency.bytes
                || Take(Sha256(actual.bytes)) != dependency.sha256)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "游戏通行输入的实际来源或内容已经改变：" + dependency.name);
            remainingBytes -= actual.bytes.size();
        }
        if (nativeCategoryDirectory_) {
            const auto status = nativeCategoryDirectory_->VerifySources(remainingBytes);
            if (!status.Succeeded()) throw Failure{status.diagnostics};
        }
        return true;
    });
    return {std::move(checked.diagnostics)};
}

std::size_t TerrainPathingResources::OwnedCapacityBytes() const noexcept {
    std::size_t bytes = sizeof(*this) + dependencies_.capacity() * sizeof(Dependency)
        + (gameRoot_.native().capacity() + 1) * sizeof(std::filesystem::path::value_type);
    for (const auto& value : dependencies_)
        bytes += value.name.capacity() + value.sha256.capacity() + value.gameArchive.capacity() + 3;
    for (const auto& [name, properties] : ground_) { (void)properties; bytes += name.capacity() + 1 + 128; }
    bytes += nativeGround_.capacity() * sizeof(TerrainNativeGroundRule) + nativeDoodads_.capacity()
        + nativeStaticAttributes_.capacity() + nativeSpecialAttributes_[0].capacity() + nativeSpecialAttributes_[1].capacity()
        + nativeAfterDoodads_.capacity() + waterBrushDoodadsSha_.capacity() + waterSpecialShapes_.capacity() * sizeof(TerrainWaterSpecialDoodadShape);
    for (const auto& shape : waterSpecialShapes_) bytes += shape.rawcode.capacity() + shape.relativeCliffLevels.capacity() + 1;
    for (const auto& rule : nativeGround_) bytes += rule.rawcode.capacity() + 1;
    for (const auto& sha : nativeTerrainSha_) bytes += sha.capacity() + 1;
    if (nativeSettingsOutputSha_) bytes += nativeSettingsOutputSha_->capacity() + 1;
    bytes += waterBrushInputSha_.capacity() + 1;
    if (nativeCategoryDirectory_) bytes += nativeCategoryDirectory_->OwnedCapacityBytes();
    if (waterBrushRules_) {
        bytes += waterBrushRules_->groundRules.capacity() * sizeof(TerrainWaterGroundRule)
            + waterBrushRules_->availableCliffTilesets.capacity() * sizeof(std::string);
        for (const auto& rule : waterBrushRules_->groundRules) bytes += rule.rawcode.capacity() + 1;
        for (const auto& id : waterBrushRules_->availableCliffTilesets) bytes += id.capacity() + 1;
    }
    return bytes;
}

} // namespace War3Map::MapData
