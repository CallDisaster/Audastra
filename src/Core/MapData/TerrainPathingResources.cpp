#include "War3MapCore/MapData/TerrainPathingResources.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/TerrainTable.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::MapData {
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
    return Guard<TerrainPathingResources>([&] {
        TerrainPathingResources result;
        result.tileset_ = before.header.mainTileset;
        if (!gameRoot.empty()) {
            std::error_code error;
            result.gameRoot_ = std::filesystem::absolute(gameRoot, error).lexically_normal();
            if (error) Fail("TERRAIN_PATHING_RESOURCE_ROOT", "无法固定通行输入的游戏来源路径。");
        }
        const auto needs = Required(before, after);
        std::vector<std::string> names;
        if (!needs.ground.empty()) names.emplace_back("TerrainArt/Terrain.slk");
        if (needs.water) {
            names.emplace_back("TerrainArt/Water.slk");
            names.emplace_back("UI/MiscData.txt");
            names.emplace_back("UI/WorldEditData.txt");
        }
        if (names.empty()) return result;
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size(); limits.memberBytes = std::min(kTableBytes, maximumBytes);
        limits.totalBytes = std::min(names.size() * kTableBytes, maximumBytes);
        const auto captured = Take(source.CaptureExactMembers(names, limits));
        std::optional<Game::GameDataSource> game;
        Text::ProfileSections configuration;
        for (std::size_t i = 0; i < captured.size(); ++i) {
            auto bytes = captured[i].bytes;
            Dependency dependency;
            dependency.name = Canonical(names[i]); dependency.mapPresent = captured[i].present;
            if (!dependency.mapPresent) {
                if (result.gameRoot_.empty()) Fail("TERRAIN_PATHING_RESOURCE_MISSING", "通行语义需要真实地图或游戏资源：" + names[i]);
                if (!game) game = Take(Game::GameDataSource::MountStrict(result.gameRoot_, result.tileset_));
                auto file = Take(game->ReadFileBoundedWithSource(names[i], std::min(kTableBytes, maximumBytes - result.sourceBytes_)));
                bytes = std::move(file.bytes); dependency.gameArchive = std::move(file.archiveName);
            }
            if (bytes.size() > maximumBytes - result.sourceBytes_) Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "通行表与配置超过保存计划剩余预算。");
            result.sourceBytes_ += bytes.size(); dependency.bytes = bytes.size(); dependency.sha256 = Take(Sha256(bytes));
            result.dependencies_.push_back(std::move(dependency));
            if (names[i] == "TerrainArt/Terrain.slk") {
                const auto table = Table(names[i], "tileid", bytes);
                for (const auto& row : table.Rows()) if (needs.ground.contains(row.id)) {
                    result.ground_.emplace(row.id, std::array{Boolean(Field(row, "walkable"), "walkable"),
                        Boolean(Field(row, "flyable"), "flyable"), Boolean(Field(row, "buildable"), "buildable")});
                }
                if (result.ground_.size() != needs.ground.size())
                    Fail("TERRAIN_PATHING_PROPERTY_MISSING", "Terrain.slk 缺少旧/新地面材质的完整通行行。");
            } else if (names[i] == "TerrainArt/Water.slk") {
                const auto table = Table(names[i], "waterid", bytes);
                const auto id = std::string(1, static_cast<char>(result.tileset_)) + "Sha";
                const auto row = std::find_if(table.Rows().begin(), table.Rows().end(), [&](const auto& item) { return item.id == id; });
                if (row == table.Rows().end()) Fail("TERRAIN_PATHING_PROPERTY_MISSING", "Water.slk 缺少当前地形集的浅水行。");
                // 原版记录先存float再乘128；不以视觉深水阈值64替代通行阈值。
                result.waterOffset_ = Number(Field(*row, "height"), "Water.height") * 128.0f;
            } else {
                auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
                for (auto& [section, fields] : Text::ParseProfile(text))
                    for (auto& [key, value] : fields) configuration[section][key] = std::move(value);
            }
        }
        if (needs.water) {
            const auto water = configuration.find("water");
            if (water == configuration.end() || !water->second.contains("deeplevelpathing"))
                Fail("TERRAIN_PATHING_PROPERTY_MISSING", "当前配置缺少 Water.DeepLevelPathing，不能猜测水深通行状态。");
            result.deepThreshold_ = Number(water->second.at("deeplevelpathing"), "Water.DeepLevelPathing");
            if (result.deepThreshold_ < 0) Fail("TERRAIN_PATHING_PROPERTY_INVALID", "DeepLevelPathing 不能为负。");
            result.waterConfigured_ = true;
        }
        return result;
    });
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
    if (!waterConfigured_ || before.IsRamp() || after.IsRamp()
        || (before.groundHeightRaw & 0xc000) != 0 || (after.groundHeightRaw & 0xc000) != 0) return false;
    const auto state = [&](const TerrainVertex& vertex) {
        const double ground = (static_cast<double>(vertex.groundHeightRaw) - 8192.0) / 4.0
            + 128.0 * (static_cast<int>(vertex.CliffLevel()) - 2);
        const double surface = (static_cast<double>(vertex.waterHeightAndFlagsRaw & 0x3fff) - 8192.0) / 4.0 + waterOffset_;
        const int depth = ground > surface ? 0 : ground <= surface - deepThreshold_ ? 2 : 1;
        return std::pair{depth, surface > ground};
    };
    return state(before) == state(after);
}

Status TerrainPathingResources::VerifyFrozenOutput(std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals) const {
    auto checked = Guard<bool>([&] {
        std::optional<Game::GameDataSource> game;
        for (const auto& dependency : dependencies_) {
            const MemberReplacement* replacement = nullptr;
            for (const auto& member : replacements) if (Canonical(member.name) == dependency.name) {
                if (replacement) Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "通行输入被重复覆盖：" + dependency.name);
                replacement = &member;
            }
            for (const auto& removed : removals) if (Canonical(removed) == dependency.name && dependency.mapPresent)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "通行不变保存不能同时删除生效输入：" + dependency.name);
            if (replacement && (!dependency.mapPresent || replacement->data.size() != dependency.bytes
                || Take(Sha256(replacement->data)) != dependency.sha256))
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "最终通行输入与准备时不同：" + dependency.name);
            if (dependency.mapPresent) continue;
            if (!game) game = Take(Game::GameDataSource::MountStrict(gameRoot_, tileset_));
            const auto actual = Take(game->ReadFileBoundedWithSource(dependency.name, dependency.bytes));
            if (actual.archiveName != dependency.gameArchive || actual.bytes.size() != dependency.bytes
                || Take(Sha256(actual.bytes)) != dependency.sha256)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "游戏通行输入的实际来源或内容已经改变：" + dependency.name);
        }
        return true;
    });
    return {std::move(checked.diagnostics)};
}

Status TerrainPathingResources::VerifyReadback(const MapArchiveSession& saved, std::size_t maximumBytes) const {
    auto checked = Guard<bool>([&] {
        if (sourceBytes_ > maximumBytes)
            Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "地形成员回读后剩余额度不足以复核通行输入。");
        std::vector<std::string> names;
        for (const auto& dependency : dependencies_) names.push_back(dependency.name);
        if (names.empty()) return true;
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size(); limits.memberBytes = std::min(kTableBytes, maximumBytes);
        limits.totalBytes = sourceBytes_;
        const auto actual = Take(saved.CaptureExactMembers(names, limits));
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto& expected = dependencies_[i];
            if (actual[i].present != expected.mapPresent || (actual[i].present
                && (actual[i].bytes.size() != expected.bytes || Take(Sha256(actual[i].bytes)) != expected.sha256)))
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "保存回读的通行输入不同：" + expected.name);
        }
        const auto game = VerifyFrozenOutput({}, {});
        if (!game.Succeeded()) throw Failure{game.diagnostics};
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
    return bytes;
}

} // namespace War3Map::MapData
