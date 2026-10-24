#include "War3MapCore/Services/TerrainShadowCasterResources.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainShadowRebuild.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
constexpr const char* kSubsystem = "Terrain.ShadowCasters";
constexpr std::size_t kTableBytes = 2u * 1024u * 1024u;
// 每项先预收map节点、路径、SHA、读取策略和有界容器winner字符串，再读正文。
constexpr std::size_t kDependencyMetadataBytes = 1024 + 64 + 256;
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Check(Status result) { if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)}; }
void Charge(std::size_t& used, std::size_t count, std::size_t maximum, std::size_t multiple = 1) {
    if (used > maximum || !multiple || count > (maximum - used) / multiple)
        Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影模型捕获超过累计读取或工作预算。");
    used += count * multiple;
}
void Cancel(std::stop_token cancellation) {
    if (cancellation.stop_requested()) Fail("TERRAIN_SHADOW_CASTER_CANCELLED", "已取消阴影模型捕获，未返回部分集合。");
}
void Validate(const TerrainShadowCasterLimits& limits) {
    const TerrainShadowCasterLimits hard;
    if (!limits.maximumSourceBytes || limits.maximumSourceBytes > hard.maximumSourceBytes
        || !limits.maximumFileBytes || limits.maximumFileBytes > hard.maximumFileBytes
        || !limits.maximumWorkingBytes || limits.maximumWorkingBytes > hard.maximumWorkingBytes
        || !limits.maximumPlacements || limits.maximumPlacements > hard.maximumPlacements
        || !limits.maximumModels || limits.maximumModels > hard.maximumModels)
        Fail("TERRAIN_SHADOW_CASTER_LIMITS", "阴影模型捕获只能收紧非零硬上限。");
}
void Validate(const TerrainShadowCasterVerificationLimits& limits) {
    const TerrainShadowCasterVerificationLimits hard;
    if (limits.maximumSourceBytes > hard.maximumSourceBytes || limits.maximumFileBytes > hard.maximumFileBytes
        || limits.maximumWorkingBytes > hard.maximumWorkingBytes)
        Fail("TERRAIN_SHADOW_CASTER_LIMITS", "阴影来源复核只能收紧硬上限。");
    if (!limits.maximumSourceBytes || !limits.maximumFileBytes || !limits.maximumWorkingBytes)
        Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影来源复核没有剩余读取或临时工作预算。");
}
std::filesystem::path ActualGameRoot(const std::filesystem::path& gameRoot) {
    if (gameRoot.empty()) return {};
    std::error_code pathError;
    if (gameRoot.native().size() > 32767)
        Fail("TERRAIN_SHADOW_CASTER_GAME_ROOT", "阴影模型游戏来源路径超过有界支持范围。");
    auto identity = std::filesystem::canonical(gameRoot, pathError);
    if (pathError || !std::filesystem::is_directory(identity, pathError) || pathError)
        Fail("TERRAIN_SHADOW_CASTER_GAME_ROOT", "阴影模型不能固定实际游戏来源目录身份。");
    if (identity.native().size() > 32767)
        Fail("TERRAIN_SHADOW_CASTER_GAME_ROOT", "阴影模型规范游戏来源路径超过有界支持范围。");
    return identity;
}
std::string Canonical(std::string_view name) { return Take(Resources::CanonicalImportPath(name)); }
std::string_view TextBytes(std::span<const std::byte> bytes) {
    if (bytes.empty()) return {};
    std::string_view result(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (result.starts_with("\xef\xbb\xbf")) result.remove_prefix(3);
    return result;
}

// 复用归档exact读取、游戏MPQ优先级和只读来源租约。最终视图只替换明确命名
// 的成员；实际空文件不会退到游戏层，损坏/超额的最高层也不会降级。
struct Context {
    const MapArchiveSession& source;
    const TerrainShadowCasterLimits& limits;
    const std::filesystem::path& gameRoot;
    std::stop_token cancellation;
    Render::AnimationSourceGuard guard;
    std::optional<Game::GameDataSource> game;
    std::uint8_t tileset = 0;
    std::map<std::string, const MemberReplacement*, std::less<>> replacements;
    std::set<std::string, std::less<>> removals;
    std::map<std::string, ArchiveExactMember, std::less<>> files;
    std::map<std::string, TerrainShadowCasterDependency, std::less<>> facts;
    std::map<std::string, std::pair<bool, std::size_t>, std::less<>> readLimits;
    std::size_t sourceBytes = 0, workingBytes = 0;

    Context(const MapArchiveSession& archive, const std::filesystem::path& root,
        const TerrainShadowCasterLimits& budget, std::span<const MemberReplacement> output,
        std::span<const std::string> removed, std::stop_token stop)
        : source(archive), limits(budget), gameRoot(root), cancellation(stop) {
        Cancel(cancellation);
        if (output.size() > 16384 || removed.size() > 16384)
            Fail("TERRAIN_SHADOW_CASTER_BUDGET", "最终资源视图的成员数超过预算。");
        std::set<std::string, std::less<>> seen;
        std::size_t pathBytes = 0;
        for (const auto& item : output) {
            if (item.name.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_SHADOW_CASTER_BUDGET", "最终资源成员名超过路径预算。");
            // canonical、seen和最终索引可能同时驻留，必须在分配前预收。
            Charge(workingBytes, item.name.size() + 128, limits.maximumWorkingBytes, 4);
            const auto key = Resources::CanonicalImportPath(item.name);
            if (!key.Succeeded() || !key.value) continue; // 无关非ASCII文件由普通保存链验证。
            Charge(pathBytes, key.value->size(), ArchiveExactMemberLimits{}.totalPathBytes, 4);
            if (!seen.insert(*key.value).second)
                Fail("TERRAIN_SHADOW_CASTER_FINAL_CONFLICT", "最终资源视图含重复替换成员。");
            replacements.emplace(*key.value, &item);
        }
        for (const auto& name : removed) {
            if (name.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_SHADOW_CASTER_BUDGET", "最终删除成员名超过路径预算。");
            Charge(workingBytes, name.size() + 128, limits.maximumWorkingBytes, 4);
            const auto key = Resources::CanonicalImportPath(name);
            if (!key.Succeeded() || !key.value) continue;
            Charge(pathBytes, key.value->size(), ArchiveExactMemberLimits{}.totalPathBytes, 4);
            if (!seen.insert(*key.value).second)
                Fail("TERRAIN_SHADOW_CASTER_FINAL_CONFLICT", "最终资源成员同时替换/删除或重复删除。");
            removals.insert(*key.value);
        }
        Charge(workingBytes, (source.SourcePath().native().size() + 1) * sizeof(std::filesystem::path::value_type) + 512,
            limits.maximumWorkingBytes, 4);
        Check(guard.Watch(source.SourcePath()));
    }
    // 固定来源及全部已解析模型名按批捕获，避免每个DOO实例都对整份源图重新散列。
    // CaptureExactMembers自身最多256项；每批保留同一只读路径租约。
    void Phase(std::span<const std::string> names, std::size_t singleFile = 0) {
        const auto memberMaximum = std::min(singleFile ? singleFile : limits.maximumFileBytes, limits.maximumFileBytes);
        std::vector<std::string> wanted;
        const auto flush = [&] {
            if (wanted.empty()) return;
            Cancel(cancellation);
            ArchiveExactMemberLimits exact; exact.memberCount = 256;
            exact.memberBytes = memberMaximum;
            exact.totalBytes = std::min(limits.maximumSourceBytes - sourceBytes,
                (limits.maximumWorkingBytes - workingBytes) / 2);
            for (auto& file : Take(source.CaptureExactMembers(wanted, exact))) {
                Charge(sourceBytes, file.bytes.size(), limits.maximumSourceBytes);
                Charge(workingBytes, file.bytes.size(), limits.maximumWorkingBytes, 2);
                files.emplace(Canonical(file.name), std::move(file));
            }
            wanted.clear();
        };
        for (const auto& name : names) {
            Cancel(cancellation);
            if (name.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影模型路径超过预算。");
            const auto key = Canonical(name);
            if (files.contains(key)) continue;
            if (const auto replaced = replacements.find(key); replaced != replacements.end()) {
                if (replaced->second->data.size() > memberMaximum)
                    Fail("TERRAIN_SHADOW_CASTER_BUDGET", "最终阴影依赖超过单文件预算：" + name);
                Charge(sourceBytes, replaced->second->data.size(), limits.maximumSourceBytes);
                Charge(workingBytes, replaced->second->data.size(), limits.maximumWorkingBytes, 2);
                files.emplace(key, ArchiveExactMember{name, true, replaced->second->data});
            } else if (removals.contains(key)) files.emplace(key, ArchiveExactMember{name, false, {}});
            else { wanted.push_back(name); if (wanted.size() == 256) flush(); }
        }
        flush();
    }
    const ArchiveExactMember& Read(std::string_view name, bool gameFallback, std::size_t maximumFile = 0,
        bool metadataPrepaid = false) {
        Cancel(cancellation);
        if (name.size() > ArchiveExactMemberLimits{}.pathBytes)
            Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影模型路径超过预算。");
        auto pathWorking = workingBytes;
        Charge(pathWorking, name.size() + 128, limits.maximumWorkingBytes, 4);
        const auto key = Canonical(name);
        if (facts.contains(key)) return files.at(key);
        // 只有内部复核批次在Phase分配前为整批预收metadata。不是外部来源资格，
        // 也不能借此跳过实际成员读取或后面的完整winner/SHA检查。
        if (metadataPrepaid) {
            if (!files.contains(key))
                Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影依赖复核批次没有完整预捕获成员。");
        } else Charge(workingBytes, key.size() + kDependencyMetadataBytes, limits.maximumWorkingBytes, 4);
        if (!files.contains(key)) { const std::array<std::string, 1> names{std::string(name)}; Phase(names, maximumFile); }
        auto& file = files.at(key);
        const auto allowed = std::min({maximumFile ? maximumFile : limits.maximumFileBytes, limits.maximumFileBytes,
            limits.maximumSourceBytes - sourceBytes, (limits.maximumWorkingBytes - workingBytes) / 2});
        if (file.bytes.size() > (maximumFile ? maximumFile : limits.maximumFileBytes))
            Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影依赖超过读取器单文件预算：" + std::string(name));
        TerrainShadowCasterDependency identity;
        identity.name = key;
        identity.source = file.present ? TerrainShadowCasterSource::Map : TerrainShadowCasterSource::Missing;
        if (!file.present && gameFallback && !gameRoot.empty()) {
            if (!game) {
                Check(guard.WatchGameRoot(gameRoot));
                game = Take(Game::GameDataSource::MountStrict(gameRoot, tileset));
            }
            if (game->HasFile(std::string(name))) {
                auto read = Take(game->ReadFileBoundedWithSource(std::string(name), allowed));
                Charge(sourceBytes, read.bytes.size(), limits.maximumSourceBytes);
                Charge(workingBytes, read.bytes.size(), limits.maximumWorkingBytes, 2);
                file.present = true; file.bytes = std::move(read.bytes);
                identity.source = TerrainShadowCasterSource::Game; identity.gameContainer = std::move(read.archiveName);
            }
        }
        identity.bytes = file.bytes.size();
        if (identity.gameContainer.size() > 1024)
            Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影依赖的实际容器身份超过有界支持范围。");
        if (file.present) identity.sha256 = Take(Sha256(file.bytes));
        readLimits.emplace(key, std::pair{gameFallback, maximumFile});
        facts.emplace(key, std::move(identity));
        return file;
    }
    void Release(std::string_view name) {
        // 复核按项消费，累计读取不退还；正文与本项临时事实比较完即释放。
        const auto found = files.find(name);
        if (found == files.end()) return;
        workingBytes -= found->second.bytes.size() * 2 + (found->first.size() + kDependencyMetadataBytes) * 4;
        facts.erase(facts.find(name)); readLimits.erase(readLimits.find(name)); files.erase(found);
    }
    const ArchiveExactMember& Required(std::string_view name, bool gameFallback, std::size_t maximumFile = 0) {
        const auto& file = Read(name, gameFallback, maximumFile);
        if (!file.present || file.bytes.empty())
            Fail("TERRAIN_SHADOW_CASTER_SOURCE_MISSING", "阴影模型缺少实际非空来源：" + std::string(name));
        return file;
    }
    void ParseReserve(std::size_t bytes, std::size_t multiple = 64) {
        Charge(workingBytes, bytes, limits.maximumWorkingBytes, multiple);
    }
};

void VerifyDependencies(const MapArchiveSession& source, const std::filesystem::path& gameRoot,
    const std::filesystem::path& gameRootIdentity, std::uint8_t tileset,
    std::span<const TerrainShadowCasterDependency> dependencies,
    std::span<const std::pair<bool, std::size_t>> readLimits,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    const TerrainShadowCasterLimits& captureLimits, std::size_t residentBytes,
    const TerrainShadowCasterVerificationLimits& verificationLimits, std::stop_token cancellation,
    const char* staleCode) {
    Validate(verificationLimits);
    auto limits = captureLimits;
    limits.maximumSourceBytes = std::min(limits.maximumSourceBytes, verificationLimits.maximumSourceBytes);
    limits.maximumFileBytes = std::min(limits.maximumFileBytes, verificationLimits.maximumFileBytes);
    if (residentBytes >= limits.maximumWorkingBytes)
        Fail("TERRAIN_SHADOW_CASTER_BUDGET", "驻留阴影模型未留下来源复核的临时预算。");
    limits.maximumWorkingBytes = std::min(limits.maximumWorkingBytes - residentBytes, verificationLimits.maximumWorkingBytes);
    Context context(source, gameRoot, limits, replacements, removals, cancellation);
    context.ParseReserve((gameRoot.native().size() + gameRootIdentity.native().size() + 2)
        * sizeof(std::filesystem::path::value_type), 2);
    if (ActualGameRoot(gameRoot) != gameRootIdentity || dependencies.size() != readLimits.size())
        Fail(staleCode, "阴影模型的实际游戏目录身份或封存依赖策略已变化。");
    context.tileset = tileset;
    for (std::size_t first = 0; first < dependencies.size();) {
        Cancel(cancellation);
        const auto fileMaximum = [&](std::size_t index) {
            const auto requested = readLimits[index].second;
            return std::min(requested ? requested : limits.maximumFileBytes, limits.maximumFileBytes);
        };
        const auto maximumFile = fileMaximum(first);
        const auto available = limits.maximumWorkingBytes - context.workingBytes;
        const auto sourceAvailable = limits.maximumSourceBytes - context.sourceBytes;
        std::size_t end = first, metadata = 0, bodies = 0, transient = 0;
        // 先按封存的实际正文长度规划，不分配或重新解析owner。正文双缓冲、
        // 全批files/facts/readLimits节点、两份名字vector及canonical暂存均先预收。
        // 若实际文件变大，底层stored/decoded硬限仍在读取前拒绝，不按旧长度放行。
        while (end < dependencies.size() && end - first < ArchiveExactMemberLimits{}.memberCount
            && fileMaximum(end) == maximumFile) {
            const auto& dependency = dependencies[end];
            const auto nameBytes = dependency.name.size();
            if (nameBytes > ArchiveExactMemberLimits{}.pathBytes || dependency.bytes > maximumFile)
                Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影依赖复核超过单文件或路径预算。");
            const auto entryMetadata = (nameBytes + kDependencyMetadataBytes) * 4;
            const auto nextTransient = std::max(transient, (nameBytes + 128) * 4);
            if (nextTransient > available) break;
            const auto afterTransient = available - nextTransient;
            if (metadata > afterTransient || entryMetadata > afterTransient - metadata) break;
            const auto bodyMaximum = (afterTransient - metadata - entryMetadata) / 2;
            if (bodies > bodyMaximum || dependency.bytes > bodyMaximum - bodies
                || bodies > sourceAvailable || dependency.bytes > sourceAvailable - bodies) break;
            metadata += entryMetadata; bodies += dependency.bytes; transient = nextTransient; ++end;
        }
        if (end == first)
            Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影来源复核剩余额度容不下一份完整依赖及其索引。");
        // 这些预收在分配batch/Phase的vector和map之前发生；每批最多256项。
        context.ParseReserve(metadata, 1);
        std::vector<std::string> names; names.reserve(end - first);
        for (auto index = first; index < end; ++index) names.push_back(dependencies[index].name);
        context.Phase(names, maximumFile);
        for (auto index = first; index < end; ++index) {
            const auto& dependency = dependencies[index];
            const auto [fallback, originalMaximum] = readLimits[index];
            (void)context.Read(dependency.name, fallback, originalMaximum, true);
            if (context.facts.at(dependency.name) != dependency)
                Fail(staleCode, "阴影模型的最终成员或实际资源来源已变化：" + dependency.name);
        }
        // 整批比较完成再退还正文/metadata工作量；sourceBytes累计不退，
        // 下一批继续消费同一次复核的剩余额，不给每批重新发一份预算。
        for (auto index = first; index < end; ++index) context.Release(dependencies[index].name);
        first = end;
    }
    Cancel(cancellation); Check(context.guard.Verify()); Check(source.VerifySourceUnchanged());
    if (ActualGameRoot(gameRoot) != gameRootIdentity)
        Fail(staleCode, "阴影模型游戏来源路径在复核期间改变了实际目录。");
}

struct Table {
    Text::SlkCells cells;
    std::map<std::string, int, std::less<>> columns, rows;
    [[nodiscard]] bool Has(std::string_view id) const { return rows.contains(id); }
    [[nodiscard]] std::string_view Field(std::string_view id, std::string_view column) const {
        const auto row = rows.find(id), col = columns.find(column);
        if (row == rows.end() || col == columns.end())
            Fail("TERRAIN_SHADOW_CASTER_FIELD", "阴影对象缺少实际唯一字段：" + std::string(id) + "." + std::string(column));
        const auto& fields = cells.at(row->second);
        const auto found = fields.find(col->second);
        if (found == fields.end())
            Fail("TERRAIN_SHADOW_CASTER_FIELD", "阴影对象缺少实际字段值：" + std::string(id) + "." + std::string(column));
        return found->second;
    }
};
Table ReadTable(Context& context, std::string_view name, std::string_view idHeader) {
    const auto& file = context.Required(name, true, kTableBytes);
    context.ParseReserve(file.bytes.size());
    Table table; table.cells = Take(Text::ParseSlkCells(TextBytes(file.bytes), Text::SlkCellMode::CatalogUnique));
    const auto head = table.cells.find(1);
    if (head == table.cells.end() || !head->second.contains(1)
        || Text::LowerAscii(head->second.at(1)) != idHeader)
        Fail("TERRAIN_SHADOW_CASTER_TABLE_HEADER", "阴影对象表的首列无效：" + std::string(name));
    if (table.cells.size() > 4097)
        Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影对象表超过行数预算。");
    for (const auto& [column, title] : head->second)
        if (title.empty() || !table.columns.emplace(Text::LowerAscii(title), column).second)
            Fail("TERRAIN_SHADOW_CASTER_TABLE_COLUMN", "阴影对象表列名为空或重复：" + std::string(name));
    std::size_t cellCount = 0;
    for (const auto& [row, fields] : table.cells) {
        if (fields.size() > 262144 - cellCount)
            Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影对象表超过单元格预算。");
        cellCount += fields.size();
        for (const auto& [column, value] : fields)
            if (!head->second.contains(column) || value.size() > 4096)
                Fail("TERRAIN_SHADOW_CASTER_TABLE_COLUMN", "阴影对象表含未声明列或超长内容：" + std::string(name));
        if (row == 1) continue;
        const auto id = fields.find(1);
        if (id == fields.end() || id->second.size() != 4 || !table.rows.emplace(id->second, row).second)
            Fail("TERRAIN_SHADOW_CASTER_TABLE_ID", "阴影对象表的四字节ID缺失或重复：" + std::string(name));
    }
    return table;
}
void RequireNoOverrides(Context& context, std::string_view name, ObjectData::ObjectType type) {
    const auto& file = context.Read(name, false, kTableBytes);
    if (!file.present) return;
    context.ParseReserve(file.bytes.size());
    auto parsed = ObjectData::NativeObjectCodec(type).Parse(file.bytes);
    if (!parsed.Succeeded() || !parsed.document) throw Failure{std::move(parsed.diagnostics)};
    const auto* document = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get());
    if (!document || !document->IsStructurallySafe() || document->Version() != 2)
        Fail("TERRAIN_SHADOW_CASTER_NATIVE_FORMAT", "阴影对象表不属于完整经典v2格式：" + std::string(name));
    if (!document->Objects().empty())
        Fail("TERRAIN_SHADOW_CASTER_NATIVE_UNSUPPORTED", "阴影模型尚未解释非空原生覆盖，保留原阴影：" + std::string(name));
}
std::int32_t Integer(std::string_view value, std::string_view field, std::string_view rawcode) {
    std::int32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        Fail("TERRAIN_SHADOW_CASTER_FIELD", "阴影对象字段不是完整整数：" + std::string(rawcode) + "." + std::string(field));
    return result;
}
float Real(std::string_view value, std::string_view field, std::string_view rawcode) {
    float result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result))
        Fail("TERRAIN_SHADOW_CASTER_FIELD", "阴影对象字段不是完整有限数：" + std::string(rawcode) + "." + std::string(field));
    return result;
}
void Transform(const MapData::DoodadPlacement& placement) {
    if (!std::isfinite(placement.rotation)
        || !std::all_of(placement.position.begin(), placement.position.end(), [](float x) { return std::isfinite(x); })
        || !std::all_of(placement.scale.begin(), placement.scale.end(), [](float x) { return std::isfinite(x); }))
        Fail("TERRAIN_SHADOW_CASTER_TRANSFORM", "阴影模型实际DOO变换包含非有限值：" + placement.rawcode);
}
} // namespace

Result<std::shared_ptr<const TerrainShadowCasterResources>> TerrainShadowCasterResources::Capture(
    const MapArchiveSession& source, const std::filesystem::path& gameRoot,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    const TerrainShadowCasterLimits& limits, std::stop_token cancellation) {
    try {
        Validate(limits);
        auto gameRootIdentity = ActualGameRoot(gameRoot);
        Context context(source, gameRoot, limits, replacements, removals, cancellation);
        context.ParseReserve((gameRootIdentity.native().size() + 1) * sizeof(std::filesystem::path::value_type), 2);
        auto owner = std::shared_ptr<TerrainShadowCasterResources>(new TerrainShadowCasterResources);
        owner->sourceSha256_ = source.SourceSha256(); owner->gameRoot_ = gameRoot; owner->limits_ = limits;
        owner->gameRootIdentity_ = std::move(gameRootIdentity);
        const std::array<std::string, 2> initial{"war3map.w3e", "war3map.doo"}; context.Phase(initial);
        const auto& terrain = context.Required("war3map.w3e", false);
        context.ParseReserve(terrain.bytes.size());
        owner->terrain_ = Take(MapData::ParseMapTerrain(terrain.bytes)).Snapshot();
        context.tileset = owner->terrain_->header.mainTileset;
        const auto& doo = context.Read("war3map.doo", false);
        if (doo.present) {
            context.ParseReserve(doo.bytes.size(), 16);
            MapData::PlacementReadLimits placementLimits;
            placementLimits.maxBytes = std::min(limits.maximumFileBytes, placementLimits.maxBytes);
            placementLimits.maxPlacements = limits.maximumPlacements;
            placementLimits.maxNestedRecords = limits.maximumPlacements;
            auto placements = Take(MapData::ParseDoodadPlacements(doo.bytes, placementLimits));
            context.ParseReserve(placements.doodads.size(), sizeof(TerrainShadowCasterInstance));
            context.ParseReserve(placements.specialDoodads.size(), sizeof(MapData::SpecialDoodadPlacement));
            owner->special_ = std::move(placements.specialDoodads);
            if (!placements.doodads.empty()) {
                const std::array<std::string, 4> tables{"war3map.w3b", "war3map.w3d",
                    "Units/DestructableData.slk", "Doodads/Doodads.slk"}; context.Phase(tables, kTableBytes);
                RequireNoOverrides(context, "war3map.w3b", ObjectData::ObjectType::Destructable);
                RequireNoOverrides(context, "war3map.w3d", ObjectData::ObjectType::Doodad);
                const auto destructables = ReadTable(context, "Units/DestructableData.slk", "destructableid");
                const auto doodads = ReadTable(context, "Doodads/Doodads.slk", "doodid");
                std::map<std::string, std::size_t, std::less<>> modelIndex;
                for (std::size_t row = 0; row < placements.doodads.size(); ++row) {
                    Cancel(cancellation);
                    auto& placement = placements.doodads[row];
                    if (destructables.Has(placement.rawcode)) { ++owner->destructableCount_; continue; }
                    if (!doodads.Has(placement.rawcode))
                        Fail("TERRAIN_SHADOW_CASTER_OBJECT_UNKNOWN", "DOO对象不在真实可破坏物或装饰物目录中：" + placement.rawcode);
                    if (!Integer(doodads.Field(placement.rawcode, "shadow"), "shadow", placement.rawcode)) {
                        ++owner->nonShadowCount_; continue;
                    }
                    Transform(placement);
                    const auto defaultScale = Real(doodads.Field(placement.rawcode, "defscale"), "defScale", placement.rawcode);
                    const auto pitch = Real(doodads.Field(placement.rawcode, "maxpitch"), "maxPitch", placement.rawcode);
                    const auto roll = Real(doodads.Field(placement.rawcode, "maxroll"), "maxRoll", placement.rawcode);
                    const auto fixedRotation = Real(doodads.Field(placement.rawcode, "fixedrot"), "fixedRot", placement.rawcode);
                    const auto floating = Integer(doodads.Field(placement.rawcode, "floats"), "floats", placement.rawcode);
                    if (defaultScale < 0.01f || pitch != 0 || roll != 0 || fixedRotation >= 0 || floating != 0)
                        Fail("TERRAIN_SHADOW_CASTER_TRANSFORM_UNSUPPORTED", "阴影模型的缩放、倾斜或固定转角不在已接通的原版变换域：" + placement.rawcode);
                    const auto count = Integer(doodads.Field(placement.rawcode, "numvar"), "numVar", placement.rawcode);
                    if (count < 1 || count > 10 || placement.variation < 0 || placement.variation >= count)
                        Fail("TERRAIN_SHADOW_CASTER_VARIATION", "阴影模型变体不在已证的显式有效域中：" + placement.rawcode);
                    const auto file = doodads.Field(placement.rawcode, "file");
                    if (file.empty() || file == "_")
                        Fail("TERRAIN_SHADOW_CASTER_MODEL", "投影装饰物没有实际模型路径：" + placement.rawcode);
                    if (file.find('.') != std::string_view::npos)
                        Fail("TERRAIN_SHADOW_CASTER_MODEL_PATH_UNSUPPORTED", "阴影模型的显式扩展名或含点路径尚未接入原版解析规则：" + placement.rawcode);
                    // 5029A0先追加单字节'0'+variation，再剥扩展名。预览助手的
                    // 先去扩展名/十进制编号并不等价；这里仅接受无扩展名、0..9的已证域。
                    std::string path(file);
                    if (count > 1) path.push_back(static_cast<char>('0' + placement.variation));
                    path += ".mdx";
                    const auto key = Canonical(path);
                    auto found = modelIndex.find(key);
                    if (found == modelIndex.end()) {
                        if (owner->models_.size() >= limits.maximumModels)
                            Fail("TERRAIN_SHADOW_CASTER_BUDGET", "阴影模型唯一文件数超过预算。");
                        const auto index = owner->models_.size();
                        owner->models_.push_back({path, {}, {}});
                        found = modelIndex.emplace(key, index).first;
                    }
                    owner->casters_.push_back({row, found->second, std::move(placement), defaultScale, pitch, roll, fixedRotation, false, {}});
                }
                if (!owner->casters_.empty()) {
                    MapData::TerrainShadowRebuildLimits surfaceLimits;
                    surfaceLimits.maximumWork = std::min(surfaceLimits.maximumWork, limits.maximumWorkingBytes - context.workingBytes);
                    const auto checkedWork = Take(MapData::ValidateOrdinaryTerrainShadowSurface(
                        *owner->terrain_, owner->special_, surfaceLimits, cancellation));
                    context.ParseReserve(checkedWork, 1);
                    context.ParseReserve(owner->terrain_->vertices.size(), 1);
                    for (const auto& vertex : owner->terrain_->vertices) {
                        Cancel(cancellation);
                        if (vertex.HasWater())
                            Fail("TERRAIN_SHADOW_CASTER_WATER_UNSUPPORTED", "阴影模型最终Z尚不接受含水地形，未套用wire高度。");
                    }
                    // 复用既有矩阵数学；实际来源和原版域验证在此owner完成，
                    // 不能由调用者提供矩阵或“已验证”标志。DOO Z不符合实际表面时拒绝。
                    for (auto& caster : owner->casters_) {
                        Cancel(cancellation); context.ParseReserve(512, 1);
                        const auto& placement = caster.placement;
                        const auto height = MapData::SampleGroundHeight(*owner->terrain_, placement.position[0], placement.position[1],
                            MapData::TerrainHeightSource::RawWithRamps);
                        if (!height || !std::isfinite(*height) || *height != placement.position[2])
                            Fail("TERRAIN_SHADOW_CASTER_HEIGHT_UNSUPPORTED", "阴影模型wire Z与同份实际地形中心高度不同，不能猜测最终变换：" + placement.rawcode);
                        Render::ModelInstanceRequest request;
                        request.position = placement.position; request.angle = placement.rotation;
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            request.scale[axis] = placement.scale[axis] * caster.defaultScale;
                            if (!std::isfinite(request.scale[axis]))
                                Fail("TERRAIN_SHADOW_CASTER_TRANSFORM", "阴影模型实际组合缩放不能表示为有限数：" + placement.rawcode);
                        }
                        caster.nativeMatrix = Render::ModelInstanceTransform(request);
                        if (!std::all_of(caster.nativeMatrix.begin(), caster.nativeMatrix.end(), [](float value) { return std::isfinite(value); }))
                            Fail("TERRAIN_SHADOW_CASTER_TRANSFORM", "阴影模型实际矩阵包含非有限数：" + placement.rawcode);
                    }
                }
                std::vector<std::string> modelPaths; modelPaths.reserve(owner->models_.size());
                for (const auto& model : owner->models_) modelPaths.push_back(model.path);
                context.Phase(modelPaths);
                for (auto& output : owner->models_) {
                    const auto& model = context.Required(output.path, true);
                    // 捕获链已为原字和散列暂存预留两份；持久模型再保留一份实际原字。
                    context.ParseReserve(model.bytes.size(), 1);
                    output.bytes = model.bytes; output.sha256 = Take(Sha256(model.bytes));
                }
            }
        }
        Cancel(cancellation); Check(context.guard.Verify()); Check(source.VerifySourceUnchanged());
        context.ParseReserve(context.facts.size(), sizeof(TerrainShadowCasterDependency) + sizeof(std::pair<bool, std::size_t>));
        owner->inputs_.reserve(context.facts.size()); owner->inputReadLimits_.reserve(context.facts.size());
        for (auto& [name, dependency] : context.facts) {
            owner->inputReadLimits_.push_back(context.readLimits.at(name));
            owner->inputs_.push_back(std::move(dependency));
        }
        owner->sourceBytes_ = context.sourceBytes; owner->workingBytes_ = context.workingBytes;
        return Result<std::shared_ptr<const TerrainShadowCasterResources>>::Success(std::move(owner));
    } catch (Failure& failure) {
        Result<std::shared_ptr<const TerrainShadowCasterResources>> result;
        result.diagnostics = std::move(failure.diagnostics); return result;
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<const TerrainShadowCasterResources>>::Failure(kSubsystem,
            "TERRAIN_SHADOW_CASTER_MEMORY", "阴影模型捕获无法保留工作内存，未返回部分集合。");
    } catch (const std::length_error&) {
        return Result<std::shared_ptr<const TerrainShadowCasterResources>>::Failure(kSubsystem,
            "TERRAIN_SHADOW_CASTER_BUDGET", "阴影模型捕获容器超过大小上限。");
    }
}

Status TerrainShadowCasterResources::VerifySources(const MapArchiveSession& source,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    std::stop_token cancellation, const TerrainShadowCasterVerificationLimits& limits) const {
    if (source.SourceSha256() != sourceSha256_)
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_CASTER_SOURCE_STALE", "阴影模型捕获来自另一份源归档。");
    try {
        VerifyDependencies(source, gameRoot_, gameRootIdentity_, terrain_->header.mainTileset, inputs_, inputReadLimits_,
            replacements, removals, limits_, workingBytes_, limits, cancellation, "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_CASTER_MEMORY", "阴影模型来源复核分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_CASTER_BUDGET", "阴影模型来源复核容量异常。"); }
}
Status TerrainShadowCasterResources::VerifyReadback(const MapArchiveSession& saved, std::stop_token cancellation,
    const TerrainShadowCasterVerificationLimits& limits) const {
    try {
        VerifyDependencies(saved, gameRoot_, gameRootIdentity_, terrain_->header.mainTileset, inputs_, inputReadLimits_,
            {}, {}, limits_, workingBytes_, limits, cancellation, "TERRAIN_SHADOW_CASTER_READBACK_STALE");
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_CASTER_MEMORY", "阴影模型保存回读分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_CASTER_BUDGET", "阴影模型保存回读容量异常。"); }
}

} // namespace War3Map::Services
