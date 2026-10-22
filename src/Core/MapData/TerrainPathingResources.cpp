#include "War3MapCore/MapData/TerrainPathingResources.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/PlacementElevationInvariant.h"
#include "War3MapCore/MapData/TerrainEditing.h"
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
    std::span<const std::string> names, const ArchiveExactMemberLimits& limits) {
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
            if ((key == "war3map.w3i" && path == "table/w3i.ini")
                || (key == "war3map.w3b" && (path == "table/destructable.ini" || path == "table/doodad.ini")))
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

Result<TerrainPathingResources> TerrainPathingResources::CaptureInputs(const MemberReader& reader,
    const TerrainGridSnapshot& before, const TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, bool native) {
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
        bool nativeOverrides = false;
        if (native) {
            if (before.header.version != 11 || after.header.version != 11
                || before.header.width != after.header.width || before.header.height != after.header.height
                || before.header.offset != after.header.offset || before.header.mainTileset != after.header.mainTileset)
                Fail("TERRAIN_NATIVE_SOURCE_SHAPE", "原版通行重建要求旧/新经典地形尺寸、偏移与地形集不变。");
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
                    if (!doodads->specialDoodads.empty())
                        Fail("TERRAIN_NATIVE_SPECIAL_UNSUPPORTED", "war3map.doo 特殊地形记录0（地格"
                            + std::to_string(doodads->specialDoodads.front().position[0]) + ","
                            + std::to_string(doodads->specialDoodads.front().position[1]) + "）尚未接入原版通行重建。");
                    result.nativeDoodads_ = std::move(member.bytes);
                } else nativeOverrides = member.present;
            }
            for (const auto* grid : {&before, &after}) for (std::size_t i = 0; i < grid->vertices.size(); ++i) {
                const auto id = Ground(grid->header, grid->vertices[i]);
                if (id.empty()) Fail("TERRAIN_NATIVE_GROUND_SLOT", "原版通行顶点地面索引越界：" + std::to_string(i));
                needs.ground.emplace(id);
                needs.water |= grid->vertices[i].HasWater();
            }
            if (!doodads->doodads.empty() && nativeOverrides)
                Fail("TERRAIN_NATIVE_OBJECT_OVERRIDE_UNSUPPORTED", "非空普通DOO存在war3map.w3b覆盖；尚未从有效物编winner证明静态贡献。");
        }
        std::vector<std::string> names;
        if (!needs.ground.empty()) names.emplace_back("TerrainArt/Terrain.slk");
        if (needs.water) names.emplace_back("TerrainArt/Water.slk");
        const bool camera = native;
        if (needs.water || camera) {
            names.emplace_back("UI/MiscData.txt");
            names.emplace_back("UI/WorldEditData.txt");
        }
        if (native && !doodads->doodads.empty()) names.emplace_back("Units/DestructableData.slk");
        if (names.empty()) return result;
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size(); limits.memberBytes = std::min(kTableBytes, maximumBytes - result.sourceBytes_);
        limits.totalBytes = std::min(names.size() * kTableBytes, maximumBytes - result.sourceBytes_);
        auto captured = Take(reader(names, limits));
        // 地图成员已整批读入；游戏回退前必须扣除全部已保留的解码字节。
        for (const auto& member : captured) {
            if (member.bytes.size() > maximumBytes - result.sourceBytes_)
                Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "通行表与配置超过保存计划剩余预算。");
            result.sourceBytes_ += member.bytes.size();
        }
        std::optional<Game::GameDataSource> game;
        std::optional<float> deepThreshold;
        std::array<std::optional<float>, 4> cameraMargins;
        std::optional<float> waterHeight;
        bool waterImpassable = false;
        for (std::size_t i = 0; i < captured.size(); ++i) {
            auto bytes = std::move(captured[i].bytes);
            Dependency dependency;
            dependency.name = Canonical(names[i]); dependency.mapPresent = captured[i].present;
            if (!dependency.mapPresent) {
                if (result.gameRoot_.empty()) Fail("TERRAIN_PATHING_RESOURCE_MISSING", "通行语义需要真实地图或游戏资源：" + names[i]);
                if (!game) game = Take(Game::GameDataSource::MountStrict(result.gameRoot_, result.tileset_));
                auto file = Take(game->ReadFileBoundedWithSource(names[i], std::min(kTableBytes, maximumBytes - result.sourceBytes_)));
                bytes = std::move(file.bytes); dependency.gameArchive = std::move(file.archiveName);
                if (bytes.size() > maximumBytes - result.sourceBytes_)
                    Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "通行表与配置超过保存计划剩余预算。");
                result.sourceBytes_ += bytes.size();
            }
            dependency.bytes = bytes.size(); dependency.sha256 = Take(Sha256(bytes));
            result.dependencies_.push_back(std::move(dependency));
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
            } else if (names[i] == "Units/DestructableData.slk") {
                const auto table = Table(names[i], "destructableid", bytes);
                for (std::size_t rowIndex = 0; rowIndex < doodads->doodads.size(); ++rowIndex) {
                    const auto& object = doodads->doodads[rowIndex];
                    const auto row = std::find_if(table.Rows().begin(), table.Rows().end(), [&](const auto& item) { return item.id == object.rawcode; });
                    if (row == table.Rows().end())
                        Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "war3map.doo 普通记录" + std::to_string(rowIndex)
                            + "未由实际DestructableData胜出分类，不能忽略其静态像素。");
                    if (Boolean(Field(*row, "walkable"), "Destructable.walkable"))
                        Fail("TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "war3map.doo 普通记录" + std::to_string(rowIndex)
                            + "为walkable破坏物；需要实际pathTex/pathTexDeath三通道贡献。");
                }
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
        }
        if (needs.water) {
            if (!deepThreshold)
                Fail("TERRAIN_PATHING_PROPERTY_MISSING", "当前配置缺少 Water.DeepLevelPathing，不能猜测水深通行状态。");
            result.waterRules_ = TerrainNativeWaterRules{*waterHeight, *deepThreshold, waterImpassable};
        }
        if (native) {
            TerrainNativePlayableBounds bounds;
            const auto& grid = before.header;
            for (const auto& margin : cameraMargins) if (!margin)
                Fail("TERRAIN_NATIVE_CAMERA_CONFIG", "原版世界可玩范围需要实际CameraMargins四边配置。");
            const auto world = [&](std::size_t i) { return *cameraMargins[i] * 128.0; };
            const auto expandLow = [&](double base, std::size_t i) {
                return static_cast<float>(static_cast<double>(static_cast<float>(base + world(i))) - world(i));
            };
            const auto expandHigh = [&](double base, std::size_t i) {
                return static_cast<float>(static_cast<double>(static_cast<float>(base - world(i))) + world(i));
            };
            if ((settings->flags & 0x400u) != 0) {
                for (const auto margin : settings->cameraMargins) if (margin < 0)
                    Fail("TERRAIN_NATIVE_PLAYABLE_BOUNDS", "W3I可玩边距不能为负。");
                // 5E69A0先将补足边距与CameraMargins组合并存F32相机角，
                // 5E72C0再扩回F32查询矩形；不能仅在double中约去两次边距。
                bounds = {expandLow(grid.offset[0] + settings->cameraMargins[0] * 128.0, 0),
                    expandLow(grid.offset[1] + settings->cameraMargins[2] * 128.0, 2),
                    expandHigh(grid.offset[0] + (static_cast<double>(grid.width - 1) - settings->cameraMargins[1]) * 128.0, 1),
                    expandHigh(grid.offset[1] + (static_cast<double>(grid.height - 1) - settings->cameraMargins[3]) * 128.0, 3)};
            } else {
                bounds = {static_cast<float>(settings->cameraBounds[0] - world(0)),
                    static_cast<float>(settings->cameraBounds[1] - world(2)),
                    static_cast<float>(settings->cameraBounds[2] + world(1)),
                    static_cast<float>(settings->cameraBounds[3] + world(3))};
            }
            // 5E72C0使用world浮点矩形；不借用小地图clamp+tile取整。
            if (!std::isfinite(bounds.left) || !std::isfinite(bounds.bottom) || !std::isfinite(bounds.right)
                || !std::isfinite(bounds.top) || bounds.left >= bounds.right || bounds.bottom >= bounds.top)
                Fail("TERRAIN_NATIVE_PLAYABLE_BOUNDS", "W3I实际世界可玩矩形无效或为空。");
            result.nativeBounds_ = bounds;
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
    if (!waterRules_) return false;
    const auto a = ClassifyNativeTerrainWater(before, *waterRules_);
    const auto b = ClassifyNativeTerrainWater(after, *waterRules_);
    return a.Succeeded() && b.Succeeded() && a.value && b.value && *a.value == *b.value;
}

Result<std::vector<std::byte>> TerrainPathingResources::GenerateNative(const TerrainGridSnapshot& terrain,
    std::size_t maximumBytes) const {
    return Guard<std::vector<std::byte>>([&] {
        if (!nativeBounds_) Fail("TERRAIN_NATIVE_INPUT_UNAVAILABLE", "此资源捕获只授予原WPM等价保留，未捕获完整生成输入。");
        {
            const auto wire = TerrainWire(terrain, maximumBytes);
            const auto sha = Take(Sha256(wire));
            if ((wire.size() != nativeTerrainBytes_[0] || sha != nativeTerrainSha_[0])
                && (wire.size() != nativeTerrainBytes_[1] || sha != nativeTerrainSha_[1]))
                Fail("TERRAIN_NATIVE_SOURCE_IDENTITY", "请求地形不属于本次捕获的旧/新W3E修订。");
        }
        TerrainNativePathingInput input;
        input.terrain = &terrain; input.playableBounds = *nativeBounds_;
        input.groundRules = nativeGround_; input.water = waterRules_;
        // 本批只接受已证明selector2处处为零的完整对象集合。Units普通层4/8
        // 不进入保存；DOO分类与walkable=false由CaptureNative的实际表证明。
        return Take(GenerateTerrainNativePathing(input, maximumBytes));
    });
}

Status TerrainPathingResources::VerifyFrozenOutput(std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals) const {
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
            } else if (replacement && nativeBounds_ && dependency.name == "war3map.doo" && dependency.mapPresent) {
                const auto checked = VerifyDoodadPlacementElevationOnly(nativeDoodads_, replacement->data, capturedMaximumBytes_);
                if (!checked.Succeeded()) throw Failure{checked.diagnostics};
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

Status TerrainPathingResources::VerifyReadbackMembers(const MemberReader& reader, std::size_t maximumBytes) const {
    auto checked = Guard<bool>([&] {
        if (sourceBytes_ > maximumBytes)
            Fail("TERRAIN_PATHING_RESOURCE_BUDGET", "地形成员回读后剩余额度不足以复核通行输入。");
        std::vector<std::string> names;
        for (const auto& dependency : dependencies_) names.push_back(dependency.name);
        if (names.empty()) return true;
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size(); limits.memberBytes = nativeBounds_ ? maximumBytes : std::min(kTableBytes, maximumBytes);
        // 读取上限同时约束存储和解码大小；不能以解码长度代替调用者预算。
        limits.totalBytes = maximumBytes;
        const auto actual = Take(reader(names, limits));
        auto remaining = maximumBytes;
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto& expected = dependencies_[i];
            if (actual[i].present != expected.mapPresent)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "保存回读的通行输入不同：" + expected.name);
            if (actual[i].present) {
                if (nativeBounds_ && expected.name == "war3map.doo") {
                    const auto status = VerifyDoodadPlacementElevationOnly(nativeDoodads_, actual[i].bytes, maximumBytes);
                    if (!status.Succeeded()) throw Failure{status.diagnostics};
                } else {
                    const auto size = nativeBounds_ && expected.name == "war3map.w3e" ? nativeTerrainBytes_[1] : expected.bytes;
                    const auto& sha = nativeBounds_ && expected.name == "war3map.w3e" ? nativeTerrainSha_[1] : expected.sha256;
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
            if (dependency.mapPresent || dependency.mapOnly) continue;
            if (!game) game = Take(Game::GameDataSource::MountStrict(gameRoot_, tileset_));
            const auto actual = Take(game->ReadFileBoundedWithSource(dependency.name,
                std::min(kTableBytes, remainingBytes)));
            if (actual.archiveName != dependency.gameArchive || actual.bytes.size() != dependency.bytes
                || Take(Sha256(actual.bytes)) != dependency.sha256)
                Fail("TERRAIN_PATHING_RESOURCE_CHANGED", "游戏通行输入的实际来源或内容已经改变：" + dependency.name);
            remainingBytes -= actual.bytes.size();
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
    bytes += nativeGround_.capacity() * sizeof(TerrainNativeGroundRule) + nativeDoodads_.capacity();
    for (const auto& rule : nativeGround_) bytes += rule.rawcode.capacity() + 1;
    for (const auto& sha : nativeTerrainSha_) bytes += sha.capacity() + 1;
    return bytes;
}

} // namespace War3Map::MapData
