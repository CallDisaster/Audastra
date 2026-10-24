#include "WorldTerrainAssets.h"
#include "EditorProject.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace War3Map::Modules {
namespace {
constexpr std::size_t kResourceLimit = 16 * 1024 * 1024;
using OwnedBytes = std::shared_ptr<const std::vector<std::byte>>;
struct WeakOwnedVersion { std::weak_ptr<const std::vector<std::byte>> owner; std::uint64_t version; };
struct StrongOwnedVersion { OwnedBytes owner; std::uint64_t version; };
struct OwnedInput { OwnedBytes owner; std::size_t entry; std::uint64_t version = 0; };
struct OwnedIdentityFailure { const char* code; const char* message; };
[[noreturn]] void IdentityBudgetFailure() {
    throw OwnedIdentityFailure{"WORLD_ASSET_IDENTITY_BUDGET", "资源版本记录所需的内存超过本次预算，请减少待导入资源或关闭不用的地图后重试。"};
}
bool OwnedLess(const OwnedBytes& a, const OwnedBytes& b) {
    const std::owner_less<OwnedBytes> owners;
    if (owners(a, b)) return true;
    if (owners(b, a)) return false;
    // 只比较本次强保活的view；地址不进入持久键或weak registry的比较器。
    return std::less<const std::vector<std::byte>*>{}(a.get(), b.get());
}
bool SameOwned(const OwnedBytes& a, const OwnedBytes& b) {
    return a == b && !a.owner_before(b) && !b.owner_before(a);
}
Result<std::size_t> AssignOwnedVersions(std::vector<WorldAssetEntry>& entries, std::size_t limit) try {
    if (limit > WorldOwnedIdentityLimits{}.maxMetadataBytes) IdentityBudgetFailure();
    const auto inputCount = static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
        [](const auto& entry) { return static_cast<bool>(entry.ownedPayload); }));
    struct Registry {
        std::mutex mutex;
        std::vector<WeakOwnedVersion> rows; // 持久状态只有weak+版本；活owner永不淘汰。
        std::uint64_t nextVersion = 1;
    };
    static Registry registry;
    const std::scoped_lock lock(registry.mutex);
    std::vector<StrongOwnedVersion> live;
    std::vector<OwnedInput> inputs; // 同时存排序索引与逐输入版本结果，不另开未计费数组。
    std::vector<WeakOwnedVersion> next;
    std::size_t observed = 0;
    const auto capacityBytes = [&] {
        std::size_t total = 0;
        const auto add = [&](std::size_t count, std::size_t element) {
            if (count > std::numeric_limits<std::size_t>::max() / element) IdentityBudgetFailure();
            const auto bytes = count * element;
            if (bytes > limit || total > limit - bytes) IdentityBudgetFailure();
            total += bytes;
        };
        add(registry.rows.capacity(), sizeof(WeakOwnedVersion));
        add(live.capacity(), sizeof(StrongOwnedVersion));
        add(inputs.capacity(), sizeof(OwnedInput));
        add(next.capacity(), sizeof(WeakOwnedVersion));
        return total;
    };
    const auto observe = [&] { observed = std::max(observed, capacityBytes()); };
    const auto reserve = [&](auto& vector, std::size_t count) {
        if (count > vector.max_size()) IdentityBudgetFailure();
        const auto current = capacityBytes();
        using Value = typename std::remove_reference_t<decltype(vector)>::value_type;
        const auto extra = count > vector.capacity() ? count - vector.capacity() : 0;
        if (extra > (limit - current) / sizeof(Value)) IdentityBudgetFailure();
        // 三个工作vector都只从空表reserve一次；前置需求估算不称为实测完整峰值。
        vector.reserve(count);
        observe(); // 实际capacity含旧/新registry和全部工作表，分配后再核才可发布。
    };
    observe();
    if (!inputCount) return Result<std::size_t>::Success(observed); // 空输入仍核算已驻留的weak registry容量。
    reserve(live, registry.rows.size());
    for (const auto& row : registry.rows) {
        auto held = row.owner.lock();
        if (held) live.push_back({std::move(held), row.version});
        else if (!row.owner.expired())
            throw OwnedIdentityFailure{"WORLD_ASSET_IDENTITY", "导入资源的内存来源无效，请重新导入该资源。"};
    }
    reserve(inputs, inputCount);
    for (std::size_t index = 0; index < entries.size(); ++index) if (const auto& payload = entries[index].ownedPayload) {
        const auto held = std::weak_ptr<const std::vector<std::byte>>(payload).lock();
        if (!held || !SameOwned(held, payload))
            throw OwnedIdentityFailure{"WORLD_ASSET_IDENTITY", "导入资源的内存来源无效，请重新导入该资源。"};
        inputs.push_back({payload, index});
    }
    std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) { return OwnedLess(a.owner, b.owner); });
    // 上轮merge有序，清除expired保持live顺序；不能每次重排整个registry。
    const auto merge = [&](auto visitor) {
        std::size_t old = 0, input = 0;
        while (old < live.size() || input < inputs.size()) {
            if (input == inputs.size() || (old < live.size() && OwnedLess(live[old].owner, inputs[input].owner))) {
                visitor(live[old].owner, live[old].version, input, input); ++old;
            } else {
                const auto first = input++;
                while (input < inputs.size() && SameOwned(inputs[first].owner, inputs[input].owner)) ++input;
                const auto version = old < live.size() && SameOwned(live[old].owner, inputs[first].owner)
                    ? live[old++].version : 0;
                visitor(inputs[first].owner, version, first, input);
            }
        }
    };
    std::size_t mergedCount = 0;
    merge([&](const OwnedBytes&, std::uint64_t, std::size_t, std::size_t) {
        if (mergedCount == next.max_size()) IdentityBudgetFailure();
        ++mergedCount;
    });
    reserve(next, mergedCount);
    merge([&](const OwnedBytes& owner, std::uint64_t version, std::size_t first, std::size_t end) {
        if (!version) {
            if (registry.nextVersion == std::numeric_limits<std::uint64_t>::max())
                throw OwnedIdentityFailure{"WORLD_ASSET_IDENTITY_EXHAUSTED", "资源版本记录已耗尽，请重新启动编辑器。"};
            version = registry.nextVersion++;
        }
        next.push_back({owner, version});
        for (auto index = first; index < end; ++index) inputs[index].version = version;
    });
    observe();
    for (const auto& input : inputs) entries[input.entry].ownedPayloadVersion = input.version;
    registry.rows.swap(next); // 最后一次性发布；旧表此刻仍在next内，峰值费用已计。
    return Result<std::size_t>::Success(observed);
} catch (const OwnedIdentityFailure& failure) {
    return Result<std::size_t>::Failure("World", failure.code, failure.message);
} catch (const std::bad_alloc&) {
    return Result<std::size_t>::Failure("World", "WORLD_ASSET_IDENTITY_ALLOCATION", "无法为资源版本记录分配内存，请关闭不用的地图后重试。");
} catch (const std::length_error&) {
    return Result<std::size_t>::Failure("World", "WORLD_ASSET_IDENTITY_BUDGET", "资源版本记录超出容量限制，未完成资源读取。");
}
struct ReadLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    explicit ReadLease(const std::filesystem::path& path)
        // 世界视图会缓存资源读取器；允许经过校验的保存流程 rename 替换来源路径，
        // 现有句柄仍锚定旧文件，MapDocumentChanged 后整张地形重新装载。
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(handle, &identity)
            || (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
        }
    }
    ~ReadLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
};
bool SameFile(const ReadLease& a, const ReadLease& b) noexcept {
    if (a.handle == INVALID_HANDLE_VALUE || b.handle == INVALID_HANDLE_VALUE) return false;
    const auto& first = a.identity; const auto& second = b.identity;
    return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber
        && first.nFileIndexHigh == second.nFileIndexHigh && first.nFileIndexLow == second.nFileIndexLow
        && first.nFileSizeHigh == second.nFileSizeHigh && first.nFileSizeLow == second.nFileSizeLow
        && CompareFileTime(&first.ftLastWriteTime, &second.ftLastWriteTime) == 0;
}
std::uint64_t ResourceStampNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
Result<std::vector<std::byte>> ReadDisk(const std::filesystem::path& path, std::size_t maximumBytes = kResourceLimit) {
    ReadLease lease(path);
    LARGE_INTEGER size{};
    if (lease.handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(lease.handle, &size) ||
        size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > std::min(maximumBytes, kResourceLimit))
        return Result<std::vector<std::byte>>::Failure("World", "WORLD_ASSET_READ", "导入地形资源无法锁定或超过读取预算。");
    const auto hash = Sha256Handle(lease.handle);
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    return ReadVerifiedFile(path, *hash.value, std::min(maximumBytes, kResourceLimit));
}

std::string ResourcePathText(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

Result<Resources::ResourceReadResult> WithReadProvenance(Result<std::vector<std::byte>> read,
    Resources::ResourceReadOrigin origin, std::string container, std::string member, std::string context) {
    if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
    Result<Resources::ResourceReadResult> result;
    result.value = Resources::ResourceReadResult{std::move(*read.value),
        Resources::ResourceReadProvenance{origin, std::move(container), std::move(member), std::move(context)}};
    result.diagnostics = std::move(read.diagnostics);
    return result;
}

Result<std::string> BlightTexturePath(Resources::ResourceResolver& resolver, std::uint8_t mainTileset) {
    auto table = resolver.Read("UI\\WorldEditData.txt");
    if (!table.Succeeded()) return {std::nullopt, std::move(table.diagnostics)};
    if (table.value->bytes.size() > 2 * 1024 * 1024)
        return Result<std::string>::Failure("World", "WORLD_BLIGHT_TABLE_BUDGET", "WorldEditData 超过 2 MiB。");
    std::string_view text(reinterpret_cast<const char*>(table.value->bytes.data()), table.value->bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto sections = Text::ParseProfile(text);
    const auto section = sections.find("tilesets");
    const auto key = Text::LowerAscii(std::string(1, static_cast<char>(mainTileset)));
    const auto entry = section == sections.end() ? nullptr : [&]() -> const std::string* {
        const auto found = section->second.find(key);
        return found == section->second.end() ? nullptr : &found->second;
    }();
    if (!entry)
        return Result<std::string>::Failure("World", "WORLD_BLIGHT_TILESET_UNDEFINED", "WorldEditData 未定义当前主地形的荒芜材质。");
    const auto fields = Text::SplitPlain(*entry, ',');
    if (fields.size() != 2 || Text::TrimCWhitespace(fields[0]).empty() || Text::TrimCWhitespace(fields[1]).empty())
        return Result<std::string>::Failure("World", "WORLD_BLIGHT_TILESET_INVALID", "WorldEditData 的荒芜材质路径格式无效。");
    std::string path(Text::TrimCWhitespace(fields[1]));
    if (!Text::LowerAscii(path).ends_with(".blp")) path += ".blp";
    auto canonical = Resources::CanonicalImportPath(path);
    if (!canonical.Succeeded()) return {std::nullopt, std::move(canonical.diagnostics)};
    return Result<std::string>::Success(std::move(*canonical.value));
}

bool HasBlightMaskAlpha(const Render::TerrainAtlasSnapshot& atlas) {
    if (atlas.columns != 8 || !atlas.image.Valid()) return false;
    // 仅 1–14 是局部遮罩；每格必须同时有透明与不透明像素。否则 alpha 混合会
    // 将底下的原地表整块盖住，不能仅凭 8×4 尺寸将任意 BLP 当作荒芜图集。
    for (std::uint32_t mask = 1; mask < 15; ++mask) {
        const auto* cell = atlas.Cell(mask % 4, mask / 4);
        if (!cell || cell->width == 0 || cell->height == 0) return false;
        bool transparent = false, opaque = false;
        for (std::uint32_t y = cell->y; y < cell->y + cell->height; ++y) {
            for (std::uint32_t x = cell->x; x < cell->x + cell->width; ++x) {
                const auto alpha = std::to_integer<std::uint8_t>(
                    atlas.image.rgba[(static_cast<std::size_t>(y) * atlas.image.width + x) * 4 + 3]);
                transparent |= alpha < 128;
                opaque |= alpha >= 128;
            }
        }
        if (!transparent || !opaque) return false;
    }
    return true;
}
} // namespace

bool SameWorldTerrainSources(const WorldTerrainSources& a, const WorldTerrainSources& b) {
    return a.archivePath == b.archivePath && a.archiveSha == b.archiveSha && a.gameRoot == b.gameRoot
        && a.readOnlyArchive == b.readOnlyArchive
        && a.mainTileset == b.mainTileset && a.entries.size() == b.entries.size()
        && std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(), [](const auto& x, const auto& y) {
            // 两侧快照均保活immutable owner，比较期间不会发生释放后的地址复用。
            return x.path == y.path && x.archivePath == y.archivePath && x.diskPath == y.diskPath
                && x.size == y.size && x.ambiguous == y.ambiguous && x.ownedPayload == y.ownedPayload
                && x.ownedPayloadVersion == y.ownedPayloadVersion;
        });
}

Result<WorldTerrainSources> CaptureWorldTerrainSources(const QtProject::EditorProject& project, std::size_t maxEntries,
    const WorldOwnedIdentityLimits& identityLimits) {
    if (!project.archiveSession_ && !project.readOnlyArchive_) {
        if (!project.sourceArchivePath_.empty())
            return Result<WorldTerrainSources>::Failure("World", "WORLD_ASSET_SOURCE_CHANGED",
                "地图没有可验证的资源来源绑定，请重新打开地图。");
        return Result<WorldTerrainSources>::Failure("World", "WORLD_ASSET_DIRECTORY_UNSUPPORTED",
            "目录工程地形资源来源尚未接入；当前显示高度预览。");
    }
    WorldTerrainSources result;
    if (project.archiveSession_) {
        result.archivePath = project.archiveSession_->SourcePath();
        result.archiveSha = project.archiveSession_->SourceSha256();
    } else {
        const auto verified = project.readOnlyArchive_->VerifyUnchanged();
        if (!verified.Succeeded()) return {std::nullopt, verified.diagnostics};
        std::error_code error;
        if (project.sourceArchivePath_.empty()
            || !std::filesystem::equivalent(project.sourceArchivePath_, project.readOnlyArchive_->SourcePath(), error) || error)
            return Result<WorldTerrainSources>::Failure("World", "WORLD_ASSET_SOURCE_CHANGED", "地图路径与只读资源绑定不一致。");
        result.archivePath = project.readOnlyArchive_->SourcePath();
        result.archiveSha = project.readOnlyArchive_->SourceSha256();
        result.readOnlyArchive = project.readOnlyArchive_;
    }
    result.gameRoot = project.war3GameRoot_;
    if (project.mapEdits_) {
        if (const auto terrain = project.mapEdits_->Terrain()) result.mainTileset = terrain->header.mainTileset;
    }
    const auto entries = project.AssetEntries(maxEntries == std::numeric_limits<std::size_t>::max() ? maxEntries : maxEntries + 1);
    if (entries.size() > maxEntries)
        return Result<WorldTerrainSources>::Failure("World", "WORLD_ASSET_METADATA_BUDGET",
            "资源覆盖清单超过查询预算；未截断后回退到游戏资源。");
    for (const auto& entry : entries) {
        WorldAssetEntry captured{entry.path.toStdString(), entry.originalPath.toStdString(), entry.diskPath.toStdWString(),
            static_cast<std::int64_t>(entry.size), entry.ambiguous, entry.ownedPayload};
        result.entries.push_back(std::move(captured));
    }
    auto identity = AssignOwnedVersions(result.entries, identityLimits.maxMetadataBytes);
    if (!identity.Succeeded()) return {std::nullopt, std::move(identity.diagnostics)};
    result.ownedIdentityMetadataCapacityBytes = *identity.value;
    return Result<WorldTerrainSources>::Success(std::move(result));
}

// 后台线程独占：地图只读租约 + 归档 + 导入快照 + 游戏包，按“会话导入 → 地图 → 游戏”解析。
struct WorldResourceReader::State {
    WorldTerrainSources sources;
    std::optional<ReadLease> lease;
    std::optional<Mpq::MpqArchive> archive;
    std::map<std::string, WorldAssetEntry, std::less<>> entries;
    // 地图 (listfile) 列出的成员；不在其中的成员只能按名字哈希找到（加密/优化地图常删去清单）。
    std::set<std::string, std::less<>> listedArchive;
    std::optional<Game::GameDataSource> game;
    Resources::ResourceResolver resolver;
    Render::AnimationSourceGuard animationGuard;
    std::shared_ptr<const Render::AnimationSourceGuard> heightGuard; // 冻结后随高度owner发布；复用仅复核文件身份。
    std::string heightRuleFingerprint;
    bool animationReads = false;
};

WorldResourceReader::WorldResourceReader() : state_(std::make_unique<State>()) {}
WorldResourceReader::~WorldResourceReader() = default;
Resources::ResourceResolver& WorldResourceReader::Resolver() noexcept { return state_->resolver; }
bool WorldResourceReader::MatchesSources(const WorldTerrainSources& sources) const {
    return SameWorldTerrainSources(state_->sources, sources);
}

Status WorldResourceReader::VerifyCurrentSource() const {
    if (state_->sources.readOnlyArchive) {
        const auto verified = state_->sources.readOnlyArchive->VerifyUnchanged();
        if (!verified.Succeeded()) return verified;
    }
    ReadLease current(state_->sources.archivePath);
    if (current.handle == INVALID_HANDLE_VALUE || !state_->lease)
        return Status::Failure("World", "WORLD_ASSET_SOURCE_CHANGED", "地图资源来源已不可验证，请重新加载地形。");
    if (!SameFile(*state_->lease, current)) return Status::Failure("World", "WORLD_ASSET_SOURCE_CHANGED",
        "地图资源来源已被替换，请重新加载地形。");
    const auto animation = state_->animationGuard.Verify();
    return animation.Succeeded() && state_->heightGuard ? state_->heightGuard->Verify() : animation;
}

Result<std::vector<std::byte>> WorldResourceReader::ReadMapArchiveMember(std::string_view path, std::size_t maxBytes) {
    if (!state_->archive || !state_->archive->HasFile(path))
        return Result<std::vector<std::byte>>::Failure("World", "WORLD_MAP_MEMBER_MISSING", "地图归档里没有 " + std::string(path));
    return state_->archive->ReadFileBounded(path, maxBytes);
}

Result<std::vector<ArchiveExactMember>> WorldResourceReader::ReadHeightMembers(
    std::span<const std::string> names, const ArchiveExactMemberLimits& limits) const try {
    using Output = Result<std::vector<ArchiveExactMember>>;
    if (!state_->archive || names.size() > limits.memberCount)
        return Output::Failure("World", "WORLD_HEIGHT_RULE_SOURCE", "坡道高度规则缺少已锁定地图来源或成员预算。");
    std::vector<ArchiveExactMember> output;
    output.reserve(names.size());
    std::size_t used = 0;
    for (const auto& name : names) {
        const auto key = Resources::CanonicalImportPath(name);
        if (!key.Succeeded() || !key.value) return {{}, key.diagnostics};
        const auto found = state_->entries.find(*key.value);
        const bool present = found != state_->entries.end()
            || (!state_->listedArchive.contains(*key.value) && state_->archive->HasFile(*key.value));
        ArchiveExactMember member{name, present, {}};
        if (present) {
            const auto remaining = std::min(limits.memberBytes, limits.totalBytes - used);
            Result<std::vector<std::byte>> read;
            if (found != state_->entries.end() && found->second.ownedPayload) {
                if (found->second.ownedPayload->size() > remaining)
                    return Output::Failure("World", "WORLD_HEIGHT_RULE_BUDGET", "会话坡道高度规则超过读取预算。");
                read = Result<std::vector<std::byte>>::Success(*found->second.ownedPayload);
            } else if (found != state_->entries.end() && !found->second.diskPath.empty())
                read = ReadDisk(found->second.diskPath, remaining);
            else read = state_->archive->ReadFileBounded(
                found == state_->entries.end() ? *key.value : found->second.archivePath, remaining);
            if (!read.Succeeded() || !read.value) return {{}, read.diagnostics};
            if (read.value->size() > remaining)
                return Output::Failure("World", "WORLD_HEIGHT_RULE_BUDGET", "坡道高度规则超过累计读取预算。");
            used += read.value->size(); member.bytes = std::move(*read.value);
        }
        output.push_back(std::move(member));
    }
    return Output::Success(std::move(output));
} catch (const std::bad_alloc&) {
    return Result<std::vector<ArchiveExactMember>>::Failure("World", "WORLD_HEIGHT_RULE_BUDGET", "坡道高度规则来源读取分配失败。");
}

Result<std::shared_ptr<const WorldTerrainHeightRules>> WorldResourceReader::CaptureHeightRules(std::uint8_t tileset) {
    using Output = Result<std::shared_ptr<const WorldTerrainHeightRules>>;
    const auto current = VerifyCurrentSource();
    if (!current.Succeeded()) return {{}, current.diagnostics};
    auto guard = std::make_shared<Render::AnimationSourceGuard>();
    const auto map = guard->Watch(state_->sources.archivePath);
    if (!map.Succeeded()) return {{}, map.diagnostics};
    bool needsGame = false;
    // 和Core同一固定profile清单；owned覆盖由sources保活，磁盘覆盖持有拒写租约。
    // 游戏同名loose入口在捕获时拒绝，此处登记其缺失，后来出现即使缓存失效。
    for (const auto& profile : MapData::kClassicProfiles) {
        const auto key = Resources::CanonicalImportPath(profile.path);
        if (!key.Succeeded() || !key.value) return {{}, key.diagnostics};
        if (!state_->sources.gameRoot.empty()) {
            const auto absent = guard->Watch(state_->sources.gameRoot / std::filesystem::u8path(profile.path), true);
            if (!absent.Succeeded()) return {{}, absent.diagnostics};
        }
        const auto found = state_->entries.find(*key.value);
        needsGame |= found == state_->entries.end()
            && (state_->listedArchive.contains(*key.value) || !state_->archive->HasFile(*key.value));
        if (found != state_->entries.end() && !found->second.ownedPayload && !found->second.diskPath.empty()) {
            const auto file = guard->Watch(found->second.diskPath);
            if (!file.Succeeded()) return {{}, file.diagnostics};
        }
    }
    if (needsGame && !state_->sources.gameRoot.empty()) {
        const auto game = guard->WatchGameRoot(state_->sources.gameRoot);
        if (!game.Succeeded()) return {{}, game.diagnostics};
    }
    auto captured = MapData::TerrainPathingResources::CaptureHeightRules(
        [this](auto names, const auto& limits) { return ReadHeightMembers(names, limits); },
        tileset, state_->sources.gameRoot, 64u * 1024u * 1024u);
    if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
    const auto content = captured.value->VerifyHeightRuleSources(
        [this](auto names, const auto& limits) { return ReadHeightMembers(names, limits); }, 64u * 1024u * 1024u);
    if (!content.Succeeded()) return {{}, content.diagnostics};
    const auto fingerprint = captured.value->HeightRuleDependencyFingerprint();
    if (!fingerprint.Succeeded() || !fingerprint.value) return {{}, fingerprint.diagnostics};
    const auto stable = guard->Verify();
    if (!stable.Succeeded()) return {{}, stable.diagnostics};
    auto owner = std::shared_ptr<const WorldTerrainHeightRules>(
        new WorldTerrainHeightRules(std::move(*captured.value), guard));
    state_->heightGuard = std::move(guard);
    state_->heightRuleFingerprint = *fingerprint.value;
    const auto verified = VerifyHeightRules(*owner);
    if (!verified.Succeeded()) return {{}, verified.diagnostics};
    return Output::Success(std::move(owner));
}

Status WorldResourceReader::VerifyHeightRules(const WorldTerrainHeightRules& rules) const {
    const auto current = VerifyCurrentSource();
    if (!current.Succeeded()) return current;
    const auto fingerprint = rules.rules_.HeightRuleDependencyFingerprint();
    if (!fingerprint.Succeeded() || !fingerprint.value) return {fingerprint.diagnostics};
    if (state_->heightRuleFingerprint.empty() || *fingerprint.value != state_->heightRuleFingerprint
        || rules.sources_ != state_->heightGuard)
        return Status::Failure("World", "WORLD_HEIGHT_RULE_SOURCE", "坡道高度规则并非此世界来源读取器捕获，未复用其它地图的规则。");
    return rules.VerifyCurrentSource();
}

Result<Render::AnimationResource> WorldResourceReader::ReadAnimationResource(std::string_view path, std::uint64_t maxBytes) {
    using Output = Result<Render::AnimationResource>;
    if (maxBytes > kResourceLimit) maxBytes = kResourceLimit;
    state_->animationReads = true;
    const auto location = state_->resolver.Locate(path);
    if (!location.Succeeded()) return {std::nullopt, location.diagnostics};
    std::vector<std::byte> bytes;
    std::string layer;
    if (location.value->layer == Resources::ResourceLayer::Game) {
        if (!state_->game) return Output::Failure("World", "WORLD_GAME_SOURCE_MISSING", "游戏资源未挂载。");
        auto read = state_->game->ReadFileBoundedWithSource(location.value->sourcePath, static_cast<std::size_t>(maxBytes));
        if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
        bytes = std::move(read.value->bytes);
        const auto root = state_->sources.gameRoot.generic_u8string();
        layer = "game:" + std::string(reinterpret_cast<const char*>(root.data()), root.size()) + "|" + read.value->archiveName;
    } else {
        const auto entry = state_->entries.find(location.value->canonicalPath);
        if (entry != state_->entries.end() && entry->second.ownedPayload
            && entry->second.ownedPayload->size() > maxBytes)
            return Output::Failure("World", "WORLD_ANIMATION_RESOURCE_BUDGET", "动画资源超过读取预算。");
        if (entry != state_->entries.end() && !entry->second.ownedPayload && !entry->second.diskPath.empty()) {
            auto guard = state_->animationGuard.Watch(entry->second.diskPath);
            if (!guard.Succeeded()) return {std::nullopt, std::move(guard.diagnostics)};
        }
        auto read = state_->resolver.Read(path);
        if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
        if (read.value->bytes.size() > maxBytes)
            return Output::Failure("World", "WORLD_ANIMATION_RESOURCE_BUDGET", "动画资源超过读取预算。");
        bytes = std::move(read.value->bytes);
        layer = read.value->location.sourceId;
        if (entry != state_->entries.end() && entry->second.ownedPayload) layer += "|session:owned";
        else if (entry != state_->entries.end() && !entry->second.diskPath.empty()) {
            const auto disk = entry->second.diskPath.generic_u8string();
            layer += "|session:" + std::string(reinterpret_cast<const char*>(disk.data()), disk.size());
        } else layer += "|map-archive";
    }
    auto current = state_->animationGuard.Verify();
    if (!current.Succeeded()) return {std::nullopt, std::move(current.diagnostics)};
    auto digest = Sha256(bytes);
    if (!digest.Succeeded()) return {std::nullopt, std::move(digest.diagnostics)};
    return Output::Success({std::move(bytes), std::move(layer), std::move(*digest.value)});
}

Result<std::unique_ptr<WorldResourceReader>> WorldResourceReader::Open(const WorldTerrainSources& sources,
    const WorldResourceReader* verifiedSource, OpenTimings* timings) {
    using Reader = std::unique_ptr<WorldResourceReader>;
    using namespace Resources;
    if (timings) *timings = {};
    const auto openStarted = ResourceStampNs();
    struct FinishTiming {
        OpenTimings* output;
        std::uint64_t start;
        ~FinishTiming() { if (output) output->totalNs = ResourceStampNs() - start; }
    } finish{timings, openStarted};
    const auto fail = [](std::string code, std::string message) { return Result<Reader>::Failure("World", std::move(code), std::move(message)); };
    Reader reader(new WorldResourceReader);
    auto& state = *reader->state_;
    state.sources = sources;
    state.lease.emplace(sources.archivePath);
    if (state.lease->handle == INVALID_HANDLE_VALUE) return fail("WORLD_ASSET_SOURCE", "地图来源无法取得只读资源租约。");
    if (sources.readOnlyArchive) {
        const auto verified = sources.readOnlyArchive->VerifySourceHandle(state.lease->handle);
        if (!verified.Succeeded()) return {std::nullopt, verified.diagnostics};
        if (sources.archiveSha != sources.readOnlyArchive->SourceSha256())
            return fail("WORLD_ASSET_SOURCE_CHANGED", "地图资源摘要与只读绑定不一致。");
        if (timings) timings->verifiedArchiveReused = true;
    } else if (verifiedSource) {
        // 两份GENERIC_READ租约都不共享写访问：原文件从完整SHA验证到新租约取得始终
        // 禁止内容写入。比较冻结身份而不是路径存在，rename替换即便字节相同也拒绝。
        const auto& verified = *verifiedSource->state_;
        if (verified.sources.archiveSha != sources.archiveSha || !verified.lease
            || !SameFile(*verified.lease, *state.lease))
            return fail("WORLD_ASSET_SOURCE_CHANGED", "地图来源与已验证的只读租约不一致，请重新加载地形。");
        if (timings) timings->verifiedArchiveReused = true;
    } else {
        const auto hashStarted = ResourceStampNs();
        const auto hash = Sha256Handle(state.lease->handle);
        if (timings) {
            timings->hashNs = ResourceStampNs() - hashStarted;
            timings->hashedBytes = (std::uint64_t(state.lease->identity.nFileSizeHigh) << 32)
                | state.lease->identity.nFileSizeLow;
        }
        if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
        if (*hash.value != sources.archiveSha) return fail("WORLD_ASSET_SOURCE_CHANGED", "地图来源已变化，请重新加载地形。");
    }
    const auto archiveStarted = ResourceStampNs();
    auto archive = Mpq::MpqArchive::OpenReadOnlyHandle(state.lease->handle, sources.archivePath);
    if (timings) timings->archiveNs = ResourceStampNs() - archiveStarted;
    if (!archive.Succeeded()) return {std::nullopt, archive.diagnostics};
    state.archive.emplace(std::move(*archive.value));
    for (const auto& name : state.archive->RawListfileNames())
        if (const auto key = CanonicalImportPath(name); key.Succeeded()) state.listedArchive.insert(*key.value);
    for (const auto& entry : sources.entries) {
        if (entry.ambiguous) return fail("WORLD_ASSET_CONFLICT", "地图资源快照包含歧义路径，地图资源未启用。");
        if (entry.size < 0 || (entry.ownedPayload && static_cast<std::uint64_t>(entry.size) != entry.ownedPayload->size()))
            return fail("WORLD_ASSET_SIZE", "地图资源快照的大小与不可变正文不一致。");
        if (entry.ownedPayload && !entry.ownedPayloadVersion)
            return fail("WORLD_ASSET_IDENTITY", "地图资源快照缺少已冻结的正文缓存版本。");
        const auto key = CanonicalImportPath(entry.path);
        if (!key.Succeeded()) continue; // 不相关的不可规范化名字不可能命中严格资源虚拟路径。
        const auto previous = state.entries.find(*key.value);
        if (previous == state.entries.end()) state.entries.emplace(*key.value, entry);
        else if (static_cast<bool>(previous->second.ownedPayload || !previous->second.diskPath.empty())
            != static_cast<bool>(entry.ownedPayload || !entry.diskPath.empty())) {
            // 与生产保存一致：会话待导入覆盖原归档（包括改名后的目标）。同层冲突仍拒绝。
            if (entry.ownedPayload || !entry.diskPath.empty()) previous->second = entry;
        } else return fail("WORLD_ASSET_CONFLICT", "地图资源同层存在大小写或路径冲突，地图资源未启用。");
    }
    State* shared = &state;
    ResourceSource mapSource;
    mapSource.id = "world-map-" + sources.archiveSha;
    mapSource.layer = ResourceLayer::MapImport;
    mapSource.priority = ResourcePriority::MapArchive;
    // 清单之外的成员与游戏一样按名字哈希读取；清单列出但工程里已移除或改名的成员不回退到原归档。
    const auto unlisted = [shared](std::string_view path) {
        return !shared->listedArchive.contains(path) && shared->archive->HasFile(path);
    };
    mapSource.probe = [shared, unlisted](std::string_view path) {
        return Result<ResourceProbe>::Success(shared->entries.contains(path) || unlisted(path)
            ? ResourceProbe{ResourceProbeState::Found, std::string(path)} : ResourceProbe{});
    };
    mapSource.readWithProvenance = [shared, unlisted](std::string_view path) -> Result<ResourceReadResult> {
        const auto found = shared->entries.find(path);
        if (found == shared->entries.end()) {
            if (unlisted(path)) return WithReadProvenance(shared->archive->ReadFileBounded(path, kResourceLimit),
                ResourceReadOrigin::MapArchive, ResourcePathText(shared->sources.archivePath),
                std::string(path), shared->sources.archiveSha);
            return Result<ResourceReadResult>::Failure("World", "WORLD_ASSET_MISSING", "资源快照内没有所请求的地图成员。");
        }
        if (found->second.ownedPayload) {
            if (found->second.ownedPayload->size() > kResourceLimit)
                return Result<ResourceReadResult>::Failure("World", "WORLD_ASSET_READ", "导入地形资源超过 16 MiB。");
            // 空正文仍是明确的会话覆盖，不回退磁盘、旧地图或游戏包。
            return WithReadProvenance(Result<std::vector<std::byte>>::Success(*found->second.ownedPayload),
                ResourceReadOrigin::SessionOwned, "owned-version:" + std::to_string(found->second.ownedPayloadVersion),
                std::string(path), shared->sources.archiveSha);
        }
        if (!found->second.diskPath.empty()) return WithReadProvenance(ReadDisk(found->second.diskPath),
            ResourceReadOrigin::SessionFile, ResourcePathText(found->second.diskPath), std::string(path), shared->sources.archiveSha);
        return WithReadProvenance(shared->archive->ReadFileBounded(found->second.archivePath, kResourceLimit),
            ResourceReadOrigin::MapArchive, ResourcePathText(shared->sources.archivePath),
            found->second.archivePath, shared->sources.archiveSha);
    };
    if (const auto added = state.resolver.AddSource(std::move(mapSource)); !added.Succeeded()) return {std::nullopt, added.diagnostics};
    if (!sources.gameRoot.empty()) {
        ResourceSource gameSource;
        gameSource.id = "world-game-patch-local-expansion-base";
        if (sources.mainTileset) gameSource.id += "|tileset:" + std::string(1, static_cast<char>(*sources.mainTileset));
        gameSource.layer = ResourceLayer::Game;
        gameSource.priority = ResourcePriority::GameBase;
        gameSource.probe = [shared](std::string_view path) {
            // 只有高层未命中才访问游戏包；完整地图覆盖不受无关游戏目录故障影响。
            if (!shared->game) {
                // 嵌套包是父归档内的 owned 快照；静态贴图和动画都绑定父归档身份。
                auto guard = shared->animationGuard.WatchGameRoot(shared->sources.gameRoot);
                if (!guard.Succeeded()) return Result<ResourceProbe>{std::nullopt, std::move(guard.diagnostics)};
                auto mounted = shared->sources.mainTileset
                    ? Game::GameDataSource::MountStrict(shared->sources.gameRoot, *shared->sources.mainTileset)
                    : Game::GameDataSource::MountStrict(shared->sources.gameRoot);
                if (!mounted.Succeeded()) return Result<ResourceProbe>{std::nullopt, mounted.diagnostics};
                guard = shared->animationGuard.Verify();
                if (!guard.Succeeded()) return Result<ResourceProbe>{std::nullopt, std::move(guard.diagnostics)};
                shared->game = std::move(*mounted.value);
            }
            return Result<ResourceProbe>::Success(shared->game->HasFile(std::string(path))
                ? ResourceProbe{ResourceProbeState::Found, std::string(path)} : ResourceProbe{});
        };
        gameSource.readWithProvenance = [shared](std::string_view path) -> Result<ResourceReadResult> {
            // bytes与container都来自同一次命中查找；不再次Locate或根据gameRoot猜winning包。
            auto read = shared->game->ReadFileBoundedWithSource(std::string(path), kResourceLimit);
            if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
            Result<ResourceReadResult> result;
            result.value = ResourceReadResult{std::move(read.value->bytes), ResourceReadProvenance{
                ResourceReadOrigin::GameArchive, std::move(read.value->archiveName), std::string(path),
                ResourcePathText(shared->sources.gameRoot)}};
            result.diagnostics = std::move(read.diagnostics);
            return result;
        };
        if (const auto added = state.resolver.AddSource(std::move(gameSource)); !added.Succeeded()) return {std::nullopt, added.diagnostics};
    }
    const auto current = reader->VerifyCurrentSource();
    if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
    return Result<Reader>::Success(std::move(reader));
}

Result<std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>> LoadWorldTerrainAtlases(
    const WorldTerrainSources& sources, const MapData::TerrainHeader& terrain,
    std::optional<Render::TerrainWaterStyle>* water,
    std::shared_ptr<const Render::TerrainAtlasSnapshot>* blight,
    std::optional<Render::WaterMaterialDefinition>* waterDefinition) {
    if (water) water->reset();
    if (blight) blight->reset();
    if (waterDefinition) waterDefinition->reset();
    auto opened = WorldResourceReader::Open(sources);
    if (!opened.Succeeded()) return {std::nullopt, opened.diagnostics};
    return LoadWorldTerrainAtlases((*opened.value)->Resolver(), terrain, water, blight, waterDefinition);
}

Result<std::vector<std::optional<Render::CliffTypeSource>>> LoadWorldCliffTypes(
    Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain) {
    const auto catalog = Render::LoadTerrainMaterialCatalog(resolver);
    if (!catalog.Succeeded()) return {std::nullopt, catalog.diagnostics};
    return Render::ResolveTerrainCliffTypes(*catalog.value, terrain);
}

Result<WorldTerrainNames> LoadWorldTerrainNames(Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain) {
    const auto catalog = Render::LoadTerrainMaterialCatalog(resolver);
    if (!catalog.Succeeded()) return {std::nullopt, catalog.diagnostics};
    const auto nameOf = [](const Render::TerrainMaterialRow& row) {
        for (const auto* column : {"name", "comment"})
            if (const auto found = row.fields.find(column); found != row.fields.end() && !found->second.empty()) return found->second;
        return std::string{};
    };
    WorldTerrainNames names;
    for (const auto& id : terrain.groundTilesets) {
        const auto row = catalog.value->ground.find(id);
        names.ground.push_back(row == catalog.value->ground.end() ? std::string{} : nameOf(row->second.source));
    }
    for (const auto& id : terrain.cliffTilesets) {
        const auto row = catalog.value->cliffs.find(id);
        names.cliffs.push_back(row == catalog.value->cliffs.end() ? std::string{} : nameOf(row->second.source));
    }
    return Result<WorldTerrainNames>::Success(std::move(names));
}

Result<std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>> LoadWorldTerrainAtlases(
    Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain,
    std::optional<Render::TerrainWaterStyle>* water,
    std::shared_ptr<const Render::TerrainAtlasSnapshot>* blight,
    std::optional<Render::WaterMaterialDefinition>* waterDefinition) {
    if (water) water->reset();
    if (blight) blight->reset();
    if (waterDefinition) waterDefinition->reset();
    using Atlases = std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>;
    const auto fail = [](std::string code, std::string message) { return Result<Atlases>::Failure("World", std::move(code), std::move(message)); };
    if (terrain.groundTilesets.empty() || terrain.groundTilesets.size() > 16)
        return fail("WORLD_ATLAS_COUNT", "地表纹理数量不在 1–16 范围内。");
    const auto catalog = Render::LoadTerrainMaterialCatalog(resolver);
    if (!catalog.Succeeded()) return {std::nullopt, catalog.diagnostics};
    Atlases atlases;
    std::vector<Diagnostic> unavailable;
    const auto recordUnavailable = [&](std::string_view path, const std::vector<Diagnostic>& diagnostics,
        std::string_view effect = "（相关格保留高度色）") {
        for (auto diagnostic : diagnostics) {
            diagnostic.severity = DiagnosticSeverity::Warning;
            diagnostic.message = std::string(path) + "：" + diagnostic.message + std::string(effect);
            unavailable.push_back(std::move(diagnostic));
        }
    };
    std::size_t bytes = 0;
    std::map<std::string, std::shared_ptr<const Render::TerrainAtlasSnapshot>, std::less<>> cache;
    for (const auto& id : terrain.groundTilesets) {
        const auto material = catalog.value->ground.find(id);
        if (material == catalog.value->ground.end()) {
            recordUnavailable(id, {{DiagnosticSeverity::Warning, "World", "WORLD_ATLAS_UNKNOWN",
                "地图地表纹理在 Terrain.slk 中没有定义。", std::nullopt}});
            atlases.push_back(nullptr);
            continue;
        }
        const auto& path = material->second.texturePath;
        if (const auto old = cache.find(path); old != cache.end()) { atlases.push_back(old->second); continue; }
        auto atlas = Render::LoadTerrainTextureAtlas(resolver, path);
        if (!atlas.Succeeded()) {
            recordUnavailable(path, atlas.diagnostics);
            cache.emplace(path, nullptr);
            atlases.push_back(nullptr);
            continue;
        }
        const auto rgbaSize = (*atlas.value)->RgbaByteCount();
        if (rgbaSize > 64 * 1024 * 1024 - bytes) return fail("WORLD_ATLAS_BUDGET", "地表 RGBA 总量超过 64 MiB。");
        bytes += rgbaSize;
        cache.emplace(path, *atlas.value);
        atlases.push_back(std::move(*atlas.value));
    }
    if (water || waterDefinition) {
        const auto row = catalog.value->water.find(std::string(1, static_cast<char>(terrain.mainTileset)) + "Sha");
        if (row != catalog.value->water.end()) {
            const auto& definition = row->second;
            if (water) {
                Render::TerrainWaterStyle style{static_cast<float>(definition.heightOffsetWorld),
                    definition.shallowMinimum, definition.shallowMaximum, definition.deepMinimum, definition.deepMaximum, {}};
                style.frameCount = definition.frameCount;
                // 初始帧固定为 0；失败仍保留历史白色回退，但必须显示资源诊断。
                auto frame = Render::LoadTerrainWaterFrame(definition, 0, resolver);
                if (!frame.Succeeded()) recordUnavailable(definition.texturePrefix + "00.blp", frame.diagnostics,
                    "（水面按白色显示）");
                else {
                    style.texture = std::make_shared<const Image::DecodedImage>(std::move(frame.value->image));
                    style.frameSourcePath = frame.value->resource.canonicalPath;
                    style.frameSourceSha256 = std::move(frame.value->sourceSha256);
                }
                *water = std::move(style);
            }
            if (waterDefinition) *waterDefinition = definition;
        }
    }
    if (blight) {
        auto path = BlightTexturePath(resolver, terrain.mainTileset);
        if (!path.Succeeded()) recordUnavailable("UI\\WorldEditData.txt", path.diagnostics,
            "（荒芜保留原地表贴图近似色调）");
        else {
            auto atlas = Render::LoadTerrainTextureAtlas(resolver, *path.value);
            if (!atlas.Succeeded()) recordUnavailable(*path.value, atlas.diagnostics,
                "（荒芜保留原地表贴图近似色调）");
            else if ((*atlas.value)->columns != 8)
                recordUnavailable(*path.value, {{DiagnosticSeverity::Warning, "World", "WORLD_BLIGHT_ATLAS_LAYOUT",
                    "荒芜 atlas 不是已验证的 8×4 遮罩及基础变体布局。", std::nullopt}},
                    "（荒芜保留原地表贴图近似色调）");
            else if (!HasBlightMaskAlpha(**atlas.value))
                recordUnavailable(*path.value, {{DiagnosticSeverity::Warning, "World", "WORLD_BLIGHT_MASK_ALPHA",
                    "荒芜图集的局部遮罩没有可验证的透明与不透明像素。", std::nullopt}},
                    "（荒芜保留原地表贴图近似色调）");
            else *blight = std::move(*atlas.value);
        }
    }
    return {std::move(atlases), std::move(unavailable)};
}

WorldLighting LoadWorldLighting(Resources::ResourceResolver& resolver, char mainTileset,
    std::uint8_t lightEnvironment, float hours) {
    WorldLighting output;
    const auto note = [&output](std::string message) {
        output.notes.push_back({DiagnosticSeverity::Warning, "World", "WORLD_LIGHTING", std::move(message), std::nullopt});
    };
    Text::ProfileSections worldEditData;
    if (auto data = resolver.Read("UI\\WorldEditData.txt"); data.Succeeded()) {
        std::string_view text(reinterpret_cast<const char*>(data.value->bytes.data()), data.value->bytes.size());
        if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        worldEditData = Text::ParseProfile(text);
    } else {
        note("UI\\WorldEditData.txt 不可用，昼夜光照按内置 Lordaeron 模型选择。");
    }
    for (const bool unit : {false, true}) {
        const auto path = Render::DayNightModelPath(worldEditData, mainTileset, lightEnvironment, unit);
        auto& target = unit ? output.unit : output.terrain;
        auto bytes = resolver.Read(path);
        if (!bytes.Succeeded()) { note(path + " 不可用，使用默认光照。"); continue; }
        auto model = Render::ParseMdx(bytes.value->bytes);
        if (!model.Succeeded()) { note(path + " 解析失败，使用默认光照。"); continue; }
        target = Render::SampleDayNight(*model.value, hours);
        if (!target) note(path + " 没有平行光或 Stand 动作，使用默认光照。");
    }
    return output;
}
} // namespace War3Map::Modules
