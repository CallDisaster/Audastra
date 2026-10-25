#include "War3MapCore/Services/TerrainPathingSave.h"
#include "TerrainPathingFinalOverlay.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/ObjectData/NativeTerrainDependencies.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>

namespace War3Map::Services {
namespace {
using TerrainPathingFinalDetail::FinalMemberOverlay;
using Reader = MapData::TerrainPathingResources::HeightSourceReader;
using Description = ObjectData::NativeTerrainDependencyDescription;
using Contribution = ObjectData::NativeTerrainDependencyContribution;
constexpr auto kSubsystem = "Terrain.FinalInputs";
constexpr std::size_t kTableBytes = 2u * 1024u * 1024u;
constexpr std::array<std::string_view, 6> kObjectInputs{"war3map.w3b", "war3map.w3d",
    "Units/DestructableData.slk", "Doodads/Doodads.slk", "Units/DestructableMetaData.slk", "Doodads/DoodadMetaData.slk"};

struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class T, class Function> Result<T> Guard(Function&& function) {
    try { return function(); }
    catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Result<T>::Failure(kSubsystem, "TERRAIN_FINAL_INPUT_BUDGET", "最终通行依赖探测分配失败，未接受部分结果。");
    } catch (const std::length_error&) {
        return Result<T>::Failure(kSubsystem, "TERRAIN_FINAL_INPUT_BUDGET", "最终通行依赖探测容量异常，未接受部分结果。");
    }
}
std::string Key(std::string_view name) { return Take(Resources::CanonicalImportPath(name)); }
void Charge(std::size_t& used, std::size_t value, std::size_t maximum, std::size_t multiple = 1) {
    if (!multiple || used > maximum || value > (maximum - used) / multiple)
        Fail("TERRAIN_FINAL_INPUT_BUDGET", "两侧通行依赖、解析和保存计划超过同一累计预算。");
    used += value * multiple;
}
void Validate(const MapData::TerrainSavePlanLimits& limits) {
    if (!limits.maximumBytes || limits.maximumBytes > MapData::TerrainSavePlanLimits{}.maximumBytes)
        Fail("TERRAIN_FINAL_INPUT_BUDGET", "最终通行依赖预算必须为正且只能收紧64 MiB上限。");
}
bool Independent(std::string_view name) {
    // 引擎中明确不写入地形静态层的成员域；任意资源名字不按后缀分类。
    constexpr std::array<std::string_view, 19> names{"WAR3MAP.W3U", "WAR3MAP.W3T", "WAR3MAP.W3A", "WAR3MAP.W3H",
        "WAR3MAP.W3Q", "WAR3MAPUNITS.DOO", "WAR3MAP.WTG", "WAR3MAP.WCT", "WAR3MAP.WTS", "WAR3MAP.J",
        "WAR3MAP.LUA", "SCRIPTS\\WAR3MAP.J", "SCRIPTS\\WAR3MAP.LUA", "WAR3MAP.IMP", "WAR3MAP.W3R",
        "WAR3MAP.W3C", "WAR3MAP.SHD", "WAR3MAP.MMP", "WAR3MAPMAP.BLP"};
    return name == "WAR3MAPMAP.TGA" || std::find(names.begin(), names.end(), name) != names.end();
}
bool MapOnly(std::string_view name) {
    return name == "WAR3MAP.W3E" || name == "WAR3MAP.WPM" || name == "WAR3MAP.W3I" || name == "WAR3MAP.DOO"
        || name == "WAR3MAP.W3B" || name == "WAR3MAP.W3D";
}
bool FixedInput(std::string_view name) {
    if (name == "WAR3MAP.W3E" || name == "WAR3MAP.WPM" || name == "WAR3MAP.W3I" || name == "WAR3MAP.DOO"
        || name == "TERRAINART\\TERRAIN.SLK" || name == "TERRAINART\\WATER.SLK" || name == "TERRAINART\\CLIFFTYPES.SLK") return true;
    return std::any_of(MapData::kClassicProfiles.begin(), MapData::kClassicProfiles.end(),
        [&](const auto& input) {
            const std::string_view path(input.path);
            return path.size() == name.size() && std::equal(path.begin(), path.end(), name.begin(),
                [](unsigned char a, unsigned char b) { return (a == '/' ? '\\' : std::toupper(a)) == b; });
        });
}
bool Same(const ArchiveExactMember& a, const ArchiveExactMember& b) {
    return a.present == b.present && a.bytes == b.bytes;
}
bool SettingsDiffer(const ArchiveExactMember& before, const ArchiveExactMember& after, std::size_t& used, std::size_t maximum) {
    if (Same(before, after)) return false;
    if (!before.present || !after.present) return true;
    Charge(used, before.bytes.size(), maximum, 16);
    Charge(used, after.bytes.size(), maximum, 16);
    const auto a = Take(MapData::MapSettings::Parse(before.bytes));
    const auto b = Take(MapData::MapSettings::Parse(after.bytes));
    return a.cameraBounds != b.cameraBounds || a.cameraMargins != b.cameraMargins
        || a.playableWidth != b.playableWidth || a.playableHeight != b.playableHeight || ((a.flags ^ b.flags) & 0x400u);
}

// 只保留本次实际探测的来源字节。map-only缺失绝不读游戏同名W3B/W3D。
// 游戏表和占地图使用与Core一致的tileset MPQ顺序；损坏高层不回退。
class Inputs {
public:
    Inputs(const Reader& sourceReader, const FinalMemberOverlay& overlay, const std::filesystem::path& gameRoot,
        std::size_t maximum, std::size_t& used)
        : reader_(sourceReader), overlay_(overlay), gameRoot_(gameRoot), maximum_(maximum), used_(used) {}

    void Capture(std::span<const std::string> names) {
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size(); limits.memberBytes = limits.totalBytes = Remaining();
        Charge(used_, names.size(), maximum_, sizeof(ArchiveExactMember) + 256);
        limits.memberBytes = limits.totalBytes = Remaining();
        auto members = Take(reader_(names, limits));
        if (members.size() != names.size()) Fail("TERRAIN_FINAL_INPUT_READ", "来源没有返回完整唯一成员集合。");
        for (std::size_t i = 0; i < members.size(); ++i) {
            auto& member = members[i];
            if (Key(member.name) != Key(names[i]) || (!member.present && !member.bytes.empty()))
                Fail("TERRAIN_FINAL_INPUT_READ", "来源成员名、顺序或缺失载荷不一致。");
            Charge(used_, member.bytes.capacity(), maximum_);
            if (member.name.capacity() + 1 > 256) Charge(used_, member.name.capacity() + 1 - 256, maximum_);
            if (Find(Key(member.name))) Fail("TERRAIN_FINAL_INPUT_READ", "来源成员被重复读取。");
            members_.push_back(std::move(member));
        }
    }
    void EnsureCaptured(std::span<const std::string> names) {
        std::vector<std::string> missing;
        for (const auto& name : names) if (!Find(Key(name))) missing.push_back(name);
        if (!missing.empty()) Capture(missing);
    }
    const ArchiveExactMember& Source(std::string_view name) {
        const auto key = Key(name);
        if (const auto* found = Find(key)) return *found;
        const std::array<std::string, 1> names{std::string(name)}; Capture(names);
        return *Find(key);
    }
    ArchiveExactMember Resolve(std::string_view name, bool final, bool mapOnly = false, bool table = false) {
        const auto key = Key(name);
        const auto* entry = final ? overlay_.Find(key) : nullptr;
        ArchiveExactMember result{std::string(name), false, {}};
        const auto maximum = table ? std::min(kTableBytes, Remaining()) : Remaining();
        if (entry) {
            if (entry->bytes) {
                if (entry->bytes->size() > maximum) Fail("TERRAIN_FINAL_INPUT_BUDGET", "最终依赖正文超过剩余或逐项预算。");
                Charge(used_, entry->bytes->size(), maximum_);
                result.present = true; result.bytes = *entry->bytes;
                Charge(used_, result.bytes.capacity() - entry->bytes->size(), maximum_);
            }
        } else {
            const auto& source = Source(name);
            if (source.bytes.size() > maximum) Fail("TERRAIN_FINAL_INPUT_BUDGET", "来源依赖正文超过剩余或逐项预算。");
            Charge(used_, source.bytes.size(), maximum_);
            result.present = source.present; result.bytes = source.bytes;
            Charge(used_, result.bytes.capacity() - source.bytes.size(), maximum_);
        }
        if (!result.present && !mapOnly) {
            if (!game_) game_ = Take(Game::GameDataSource::MountStrict(gameRoot_, tileset_));
            if (game_->HasFile(std::string(name))) {
                auto file = Take(game_->ReadFileBoundedWithSource(std::string(name), std::min(maximum, Remaining())));
                Charge(used_, file.bytes.capacity(), maximum_);
                result.present = true; result.bytes = std::move(file.bytes);
            }
        }
        return result;
    }
    void SetTileset(std::uint8_t value) noexcept { tileset_ = value; }
    [[nodiscard]] std::size_t Remaining() const {
        if (used_ >= maximum_) Fail("TERRAIN_FINAL_INPUT_BUDGET", "最终通行依赖没有剩余预算。");
        return maximum_ - used_;
    }
private:
    const ArchiveExactMember* Find(std::string_view key) const {
        for (const auto& member : members_) if (Key(member.name) == key) return &member;
        return nullptr;
    }
    const Reader& reader_;
    const FinalMemberOverlay& overlay_;
    const std::filesystem::path& gameRoot_;
    std::size_t maximum_, &used_;
    std::vector<ArchiveExactMember> members_;
    std::optional<Game::GameDataSource> game_;
    std::uint8_t tileset_ = 0;
};

void Normalize(Description& description) {
    std::sort(description.objects.begin(), description.objects.end(), [](const auto& a, const auto& b) {
        return std::tie(a.special, a.rawcode) < std::tie(b.special, b.rawcode);
    });
}
Description Describe(std::span<const std::string_view> ordinary, std::span<const std::string_view> special,
    std::span<const ArchiveExactMember> inputs, std::size_t& used, std::size_t maximum) {
    auto result = Take(ObjectData::InspectNativeTerrainDependencies(ordinary, special, inputs, maximum - used));
    // Inspector在剩余额度内验证本阶段解析峰值；返回后catalog/schema已销毁。
    // 后续两侧描述共存只保留实际返回容量，不能再次累加已释放的解析预留。
    Charge(used, result.retainedBytes, maximum); Normalize(result); return result;
}
bool UnknownChanged(const Description& before, const Description& after) {
    std::size_t a = 0, b = 0;
    while (a < before.objects.size() && b < after.objects.size()) {
        const auto& left = before.objects[a]; const auto& right = after.objects[b];
        const auto first = std::tie(left.special, left.rawcode), second = std::tie(right.special, right.rawcode);
        if (first < second) {
            if (!left.unknownFieldsSha256.empty()) return true;
            ++a;
        } else if (second < first) {
            if (!right.unknownFieldsSha256.empty()) return true;
            ++b;
        } else {
            if (left.unknownFieldsSha256 != right.unknownFieldsSha256) return true;
            ++a; ++b;
        }
    }
    for (; a < before.objects.size(); ++a) if (!before.objects[a].unknownFieldsSha256.empty()) return true;
    for (; b < after.objects.size(); ++b) if (!after.objects[b].unknownFieldsSha256.empty()) return true;
    return false;
}
bool SameContributions(const Description& before, const Description& after) {
    std::size_t a = 0, b = 0;
    for (;;) {
        while (a < before.objects.size() && before.objects[a].contribution == Contribution::Dynamic) ++a;
        while (b < after.objects.size() && after.objects[b].contribution == Contribution::Dynamic) ++b;
        if (a == before.objects.size() || b == after.objects.size())
            return a == before.objects.size() && b == after.objects.size();
        if (!(before.objects[a] == after.objects[b])) return false;
        ++a; ++b;
    }
}
struct FootprintNames {
    std::string first;
    std::optional<std::string> fallback;
    // 未支持声明只用于保守相关性探测，不能授权原版加载或绕过完整Core格式门。
    std::optional<std::string> unsupportedRaw;
};
FootprintNames Names(const ObjectData::NativeTerrainDependencyType& type) {
    auto name = Key(type.pathTexture);
    if (name.size() >= 260 || (!name.ends_with(".TGA") && (type.special || !name.ends_with(".BLP")))) {
        const auto slash = name.find_last_of('\\'), dot = name.find_last_of('.');
        const auto stem = dot != std::string::npos && (slash == std::string::npos || dot > slash)
            ? name.substr(0, dot) : name;
        return {stem + ".TGA", stem + ".BLP", std::move(name)};
    }
    if (!type.special && name.ends_with(".BLP")) name.replace(name.size() - 4, 4, ".TGA");
    FootprintNames result{std::move(name), {}, {}};
    if (!type.special && result.first.ends_with(".TGA")) {
        result.fallback = result.first; result.fallback->replace(result.fallback->size() - 4, 4, ".BLP");
    }
    return result;
}
bool AssetDiffer(const FootprintNames& names, Inputs& inputs, const FinalMemberOverlay& overlay) {
    if (names.unsupportedRaw) {
        const auto changed = [&](const auto& name) {
            if (!overlay.Find(name)) return false;
            const auto before = inputs.Resolve(name, false);
            const auto after = inputs.Resolve(name, true);
            return !Same(before, after);
        };
        return changed(*names.unsupportedRaw) || changed(names.first) || (names.fallback && changed(*names.fallback));
    }
    auto before = inputs.Resolve(names.first, false);
    auto after = inputs.Resolve(names.first, true);
    if (!before.present && names.fallback) before = inputs.Resolve(*names.fallback, false);
    if (!after.present && names.fallback) after = inputs.Resolve(*names.fallback, true);
    // 同名同字节的map/game切换不改变占地；写前仍从真实两侧重新读取。
    return !Same(before, after) || Key(before.name) != Key(after.name);
}
struct Probe { bool changed = false; std::size_t used = 0; };

Probe ProbeFinal(const Reader& sourceReader, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits, std::span<const MemberReplacement> finalMembers,
    std::span<const std::string> removals) {
    Validate(limits);
    const auto overlay = Take(FinalMemberOverlay::Prepare(finalMembers, removals, limits.maximumBytes));
    Probe result{false, overlay.MetadataBytes()};
    if (std::all_of(overlay.Entries().begin(), overlay.Entries().end(),
        [](const auto& entry) { return Independent(entry.name); })) return result;

    const auto objectInput = [](std::string_view name) {
        return std::any_of(kObjectInputs.begin(), kObjectInputs.end(), [&](auto path) { return Key(path) == name; });
    };
    std::vector<std::string> names{"war3map.w3e"};
    for (const auto& entry : overlay.Entries()) if ((FixedInput(entry.name) || objectInput(entry.name))
        && std::none_of(names.begin(), names.end(), [&](const auto& name) { return Key(name) == entry.name; })) names.push_back(entry.name);
    Charge(result.used, names.capacity(), limits.maximumBytes, sizeof(std::string) + 256);
    Inputs inputs(sourceReader, overlay, gameRoot, limits.maximumBytes, result.used); inputs.Capture(names);
    // 编译模板可能原字重现W3B/W3D等成员。实际字节未变时应在解析对象前剔除，
    // 避免单位生命值之类无关保存因原图不受本生成器支持而新增拒绝。
    const auto rawChanged = [&](const auto& entry) {
        if (Independent(entry.name)) return false;
        const bool captured = std::any_of(names.begin(), names.end(), [&](const auto& name) { return Key(name) == entry.name; });
        if (!captured) return true; // 任意资源待实际pathTex闭包检查，不按后缀授权。
        const auto& original = inputs.Source(entry.name);
        return entry.bytes ? !original.present || original.bytes != *entry.bytes : original.present;
    };
    if (std::none_of(overlay.Entries().begin(), overlay.Entries().end(), rawChanged)) return result;
    for (const auto& entry : overlay.Entries()) if (FixedInput(entry.name) && MapOnly(entry.name) && rawChanged(entry)) {
        auto before = inputs.Resolve(entry.name, false, true);
        auto after = inputs.Resolve(entry.name, true, true);
        const bool differs = entry.name == "WAR3MAP.W3I" ? SettingsDiffer(before, after, result.used, limits.maximumBytes) : !Same(before, after);
        if (differs) { result.changed = true; return result; }
    }
    const bool describeObjects = std::any_of(overlay.Entries().begin(), overlay.Entries().end(), [&](const auto& entry) {
        return !Independent(entry.name) && !FixedInput(entry.name) && rawChanged(entry);
    });
    const bool resourceFixed = std::any_of(overlay.Entries().begin(), overlay.Entries().end(), [&](const auto& entry) {
        return FixedInput(entry.name) && !MapOnly(entry.name) && rawChanged(entry);
    });
    if (!describeObjects && !resourceFixed) return result; // 仅地图说明等已证无关字段，不解析旧对象。
    const auto& terrain = inputs.Source("war3map.w3e");
    if (!terrain.present && !overlay.Find("WAR3MAP.W3E")) return result; // 局部物编归档没有静态地形。
    if (terrain.bytes.size() < 9 || terrain.bytes[0] != std::byte{'W'} || terrain.bytes[1] != std::byte{'3'}
        || terrain.bytes[2] != std::byte{'E'} || terrain.bytes[3] != std::byte{'!'})
        Fail("TERRAIN_FINAL_TERRAIN_HEADER", "实际来源无法提供地形集上下文，不能猜游戏资源winner。");
    inputs.SetTileset(std::to_integer<std::uint8_t>(terrain.bytes[8]));

    for (const auto& entry : overlay.Entries()) if (FixedInput(entry.name) && !MapOnly(entry.name) && rawChanged(entry)) {
        auto before = inputs.Resolve(entry.name, false, false, true);
        auto after = inputs.Resolve(entry.name, true, false, true);
        if (!Same(before, after)) { result.changed = true; return result; }
    }
    if (!describeObjects) return result;

    std::vector<std::string> objectSources{"war3map.doo"};
    for (const auto path : kObjectInputs) objectSources.emplace_back(path);
    Charge(result.used, objectSources.capacity(), limits.maximumBytes, sizeof(std::string) + 256);
    inputs.EnsureCaptured(objectSources);
    const auto& doo = inputs.Source("war3map.doo");
    if (!doo.present) return result;
    Charge(result.used, doo.bytes.size(), limits.maximumBytes, 32);
    MapData::PlacementReadLimits placementLimits; placementLimits.maxBytes = doo.bytes.size();
    const auto placements = Take(MapData::ParseDoodadPlacements(doo.bytes, placementLimits));
    if (placements.doodads.empty() && placements.specialDoodads.empty()) return result;
    std::vector<std::string_view> ordinary, special;
    Charge(result.used, placements.doodads.size() + placements.specialDoodads.size(), limits.maximumBytes, sizeof(std::string_view) * 2);
    ordinary.reserve(placements.doodads.size()); special.reserve(placements.specialDoodads.size());
    for (const auto& object : placements.doodads) ordinary.push_back(object.rawcode);
    for (const auto& object : placements.specialDoodads) special.push_back(object.rawcode);
    const auto unique = [](auto& values) { std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end()); };
    unique(ordinary); unique(special);
    std::vector<ArchiveExactMember> objectInputs; objectInputs.reserve(kObjectInputs.size());
    Charge(result.used, objectInputs.capacity(), limits.maximumBytes, sizeof(ArchiveExactMember) + 256);
    for (std::size_t i = 0; i < kObjectInputs.size(); ++i)
        objectInputs.push_back(inputs.Resolve(kObjectInputs[i], false, i < 2, true));
    auto before = Describe(ordinary, special, objectInputs, result.used, limits.maximumBytes);
    bool changedTables = false;
    for (std::size_t i = 0; i < kObjectInputs.size(); ++i) if (overlay.Find(Key(kObjectInputs[i]))) {
        auto next = inputs.Resolve(kObjectInputs[i], true, i < 2, true);
        changedTables |= !Same(objectInputs[i], next); objectInputs[i] = std::move(next);
    }
    if (changedTables) {
        auto after = Describe(ordinary, special, objectInputs, result.used, limits.maximumBytes);
        if (UnknownChanged(before, after))
            Fail("TERRAIN_FINAL_UNKNOWN_FIELD_CHANGED", "已放置静态对象的未知字段发生变化，不能把未理解的覆盖当作无关。");
        if (!SameContributions(before, after)) { result.changed = true; return result; }
    }
    // 描述两侧相等时，只读最终列表实际命中的占地候选；无关图标/模型无需解码或完整G0。
    std::vector<FootprintNames> checked;
    for (const auto& type : before.objects) {
        if (type.contribution == Contribution::Dynamic || type.pathTexture.empty()) continue;
        auto footprint = Names(type);
        if (!overlay.Find(footprint.first) && (!footprint.fallback || !overlay.Find(*footprint.fallback))
            && (!footprint.unsupportedRaw || !overlay.Find(*footprint.unsupportedRaw))) continue;
        if (std::any_of(checked.begin(), checked.end(), [&](const auto& value) {
            return value.first == footprint.first && value.fallback == footprint.fallback
                && value.unsupportedRaw == footprint.unsupportedRaw;
        })) continue;
        Charge(result.used, sizeof(FootprintNames) + footprint.first.capacity() + 1
            + (footprint.fallback ? footprint.fallback->capacity() + 1 : 0)
            + (footprint.unsupportedRaw ? footprint.unsupportedRaw->capacity() + 1 : 0), limits.maximumBytes, 2);
        checked.push_back(footprint);
        if (AssetDiffer(footprint, inputs, overlay)) { result.changed = true; return result; }
    }
    return result;
}

template<class Build> Result<std::optional<MapData::TerrainSavePlan>> PrepareFinal(const Reader& reader,
    const MapData::MapDataEditSession* frozenEdits, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits, std::span<const MemberReplacement> members,
    std::span<const std::string> removals, Build&& build) {
    using Output = Result<std::optional<MapData::TerrainSavePlan>>;
    return Guard<std::optional<MapData::TerrainSavePlan>>([&]() -> Output {
        Validate(limits);
        if (frozenEdits && MapData::TerrainSavePlan::HasTerrainChanges(*frozenEdits)) {
            auto plan = Take(build(limits, false));
            return Output::Success(std::optional<MapData::TerrainSavePlan>(std::move(plan)));
        }
        const auto probe = ProbeFinal(reader, gameRoot, limits, members, removals);
        if (!probe.changed) return Output::Success(std::optional<MapData::TerrainSavePlan>{});
        if (!frozenEdits || !frozenEdits->Terrain() || !frozenEdits->HasTerrainPathing())
            Fail("TERRAIN_FINAL_BASELINE_MISSING", "实际静态通行依赖发生变化，需要宿主编辑前已附入真实W3E/WPM的冻结会话。");
        // Probe内部的来源字节、表解析、描述与overlay均已销毁，没有owner逃逸。
        // 它和正式G0/G1捕获分别受同一64MiB峰值限制，不将已释放工作跨阶段累计。
        auto plan = Take(build(limits, true));
        if (!plan.SaveGate().Succeeded()) throw Failure{plan.SaveGate().diagnostics};
        return Output::Success(std::optional<MapData::TerrainSavePlan>(std::move(plan)));
    });
}
Status VerifyUnchanged(const Reader& reader, const MapData::MapDataEditSession* frozenEdits,
    const std::filesystem::path& gameRoot, const MapData::TerrainSavePlanLimits& limits,
    std::span<const MemberReplacement> members, std::span<const std::string> removals) {
    auto result = Guard<bool>([&]() -> Result<bool> {
        if ((frozenEdits && MapData::TerrainSavePlan::HasTerrainChanges(*frozenEdits))
            || ProbeFinal(reader, gameRoot, limits, members, removals).changed)
            Fail("TERRAIN_FINAL_INPUTS_CHANGED", "写前最终列表出现新的静态通行变化，必须重新准备保存；本次未临时重建。");
        return Result<bool>::Success(true);
    });
    return {std::move(result.diagnostics)};
}
} // namespace

Result<std::optional<MapData::TerrainSavePlan>> TerrainPathingSave::PrepareFinalForSave(
    const MapArchiveSession& source, const MapData::MapDataEditSession* frozenEdits,
    const std::filesystem::path& gameRoot, const MapData::TerrainSavePlanLimits& limits,
    std::span<const MemberReplacement> members, std::span<const std::string> removals) {
    const Reader reader = [&](auto names, const auto& readLimits) { return source.CaptureExactMembers(names, readLimits); };
    return PrepareFinal(reader, frozenEdits, gameRoot, limits, members, removals, [&](const auto& bounded, bool changed) {
        return PrepareWithDependencyDelta(source, *frozenEdits, gameRoot, bounded, members, removals, changed);
    });
}
Result<std::optional<MapData::TerrainSavePlan>> TerrainPathingSave::PrepareFinalForSave(
    const WorkspaceSnapshot& source, const MapData::MapDataEditSession* frozenEdits,
    const std::filesystem::path& gameRoot, const MapData::TerrainSavePlanLimits& limits,
    std::span<const MemberReplacement> members, std::span<const std::string> removals) {
    const Reader reader = [&](auto names, const auto& readLimits) {
        return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, readLimits);
    };
    return PrepareFinal(reader, frozenEdits, gameRoot, limits, members, removals, [&](const auto& bounded, bool changed) {
        return PrepareWithDependencyDelta(source, *frozenEdits, gameRoot, bounded, members, removals, changed);
    });
}
Status TerrainPathingSave::VerifyUnchangedFinalInputs(const MapArchiveSession& source,
    const MapData::MapDataEditSession* frozenEdits, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits, std::span<const MemberReplacement> members,
    std::span<const std::string> removals) {
    const Reader reader = [&](auto names, const auto& readLimits) { return source.CaptureExactMembers(names, readLimits); };
    return VerifyUnchanged(reader, frozenEdits, gameRoot, limits, members, removals);
}
Status TerrainPathingSave::VerifyUnchangedFinalInputs(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession* frozenEdits, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits, std::span<const MemberReplacement> members,
    std::span<const std::string> removals) {
    const Reader reader = [&](auto names, const auto& readLimits) {
        return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, readLimits);
    };
    return VerifyUnchanged(reader, frozenEdits, gameRoot, limits, members, removals);
}
} // namespace War3Map::Services
