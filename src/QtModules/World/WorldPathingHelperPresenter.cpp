#include "WorldPathingHelperPresenter.h"

#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Modules {
namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t ElapsedNs(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
struct StageTimer {
    std::uint64_t& output;
    Clock::time_point start = Clock::now();
    ~StageTimer() { output += ElapsedNs(start); }
};
void Part(std::string& identity, std::string_view value) {
    identity += std::to_string(value.size()) + ":" + std::string(value);
}
void PathPart(std::string& identity, const std::filesystem::path& value) {
    const auto path = value.generic_u8string();
    Part(identity, std::string_view(reinterpret_cast<const char*>(path.data()), path.size()));
}
void AddUnique(std::vector<Diagnostic>& output, const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        if (std::none_of(output.begin(), output.end(), [&](const auto& prior) {
            return prior.code == diagnostic.code && prior.message == diagnostic.message;
        })) output.push_back(diagnostic);
}
} // namespace

Result<std::string> WorldPathingHelperResourceIdentity(const WorldTerrainSources& sources,
    std::uint64_t catalogRevision) {
    std::string identity = "world-pathing-helper-resource-v1;";
    PathPart(identity, sources.archivePath); Part(identity, sources.archiveSha); PathPart(identity, sources.gameRoot);
    Part(identity, std::to_string(catalogRevision));
    Part(identity, sources.mainTileset ? std::to_string(*sources.mainTileset) : "unknown-tileset");
    for (const auto& entry : sources.entries) {
        Part(identity, entry.path); Part(identity, entry.archivePath); PathPart(identity, entry.diskPath);
    }
    return Sha256Text(identity);
}
Services::PlacementPresentationResourceReader WorldPathingHelperResourceReader(std::shared_ptr<WorldResourceReader> source) {
    return [source = std::move(source)](std::string_view path, std::size_t maximum) -> Result<Resources::ResolvedResource> {
        if (!source) return Result<Resources::ResolvedResource>::Failure("World.Helper", "WORLD_HELPER_READER_UNAVAILABLE", "辅助物资源来源不可用。");
        auto location = source->Resolver().Locate(path);
        if (!location.Succeeded()) return {std::nullopt, std::move(location.diagnostics)};
        auto owned = source->ReadAnimationResource(path, maximum); // 实际读取预算/高层失败/来源guard共用。
        if (!owned.Succeeded()) return {std::nullopt, std::move(owned.diagnostics)};
        location.value->sourceId = owned.value->sourceLayer; // 包含真实game archive或地图/session层。
        return Result<Resources::ResolvedResource>::Success({std::move(*location.value), std::move(owned.value->bytes)});
    };
}
Result<std::string> WorldPathingHelperCandidateIdentity(std::span<const WorldPlacementPresentationCandidate> candidates,
    const Services::WorldSelectionVersion& version, std::string_view resourceIdentity) {
    Services::PlacementPresentationContext context{version, std::string(resourceIdentity), {}};
    std::string identity = "world-pathing-helper-candidates-v1;";
    for (const auto& candidate : candidates) {
        if (!candidate.fields) return Result<std::string>::Failure("World.Helper", "WORLD_HELPER_INPUT_INVALID", "辅助物缺有效字段。");
        auto row = Services::QueryPlacementPresentation(context, version, resourceIdentity,
            {candidate.object, candidate.owner, version, *candidate.fields});
        if (!row.Succeeded()) return {std::nullopt, std::move(row.diagnostics)};
        Part(identity, std::to_string(candidate.requestIndex)); Part(identity, row.value->inputIdentity);
    }
    return Sha256Text(identity);
}
Result<WorldPathingHelperPrepared> PrepareWorldPathingHelpers(std::vector<Render::ModelInstanceRequest>& requests,
    std::span<const WorldPlacementPresentationCandidate> candidates,
    const Services::PlacementPresentationContext& context, std::shared_ptr<WorldResourceReader> sourceOwner,
    std::shared_ptr<const WorldPathingHelperCache> previous) {
    using Output = Result<WorldPathingHelperPrepared>;
    const auto totalStart = Clock::now();
    WorldPathingHelperPrepared prepared;
    prepared.version = context.version;
    prepared.records.reserve(candidates.size());
    prepared.timings.candidateCount = candidates.size();
    if (context.resourceIdentity.empty()) return Output::Failure("World.Helper", "WORLD_HELPER_SOURCE_INVALID", "辅助物未绑定冻结资源身份。");
    {
        StageTimer timer{prepared.timings.mappingNs};
        std::set<std::uint32_t> assigned;
        for (const auto& candidate : candidates)
            if (candidate.requestIndex >= requests.size() || !candidate.fields || !assigned.insert(candidate.requestIndex).second)
                return Output::Failure("World.Helper", "WORLD_HELPER_REQUEST_INVALID", "辅助物有效字段与原请求索引不符，未提交部分重映射。");
    }
    if (sourceOwner) {
        StageTimer timer{prepared.timings.verifyNs};
        const auto current = sourceOwner->VerifyCurrentSource();
        if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
    }
    if (previous && previous->resourceIdentity == context.resourceIdentity && previous->sourceOwner) {
        StageTimer timer{prepared.timings.verifyNs};
        const auto current = previous->sourceOwner->VerifyCurrentSource();
        if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
    }
    std::shared_ptr<WorldPathingHelperCache> cache;
    {
        StageTimer timer{prepared.timings.cacheNs};
        const bool sameSources = previous && previous->resourceIdentity == context.resourceIdentity;
        const bool newReader = sourceOwner && (!sameSources || previous->sourceOwner != sourceOwner);
        if (sameSources && !newReader) cache = std::make_shared<WorldPathingHelperCache>(*previous);
        else {
            cache = std::make_shared<WorldPathingHelperCache>();
            // 新reader必须重取实际访问闭包；只保留shared资产，避免先深拷旧TGA再立即清空。
            if (sameSources) cache->assets = previous->assets;
            cache->sourceOwner = sourceOwner;
        }
        cache->resourceIdentity = context.resourceIdentity;
    }
    prepared.helperRequests.assign(requests.size(), 0);
    std::set<std::string> usedAssets;
    auto query = context;
    query.resourceReader = [cache, &prepared, reader = context.resourceReader](std::string_view path, std::size_t maximum) {
        const auto key = std::string(path);
        if (const auto found = cache->resources.find(key); found != cache->resources.end()) {
            ++prepared.timings.resourceCacheHits;
            return found->second;
        }
        ++prepared.timings.resourceReads;
        auto resource = reader ? reader(path, maximum)
            : Result<Resources::ResolvedResource>::Failure("World.Helper", "WORLD_HELPER_READER_UNAVAILABLE", "辅助物冻结reader不可用。");
        constexpr std::size_t budget = 64ull * 1024 * 1024; // 实际owned资源字节预算，不限制地图实例数。
        if (resource.Succeeded()) {
            if (resource.value->bytes.size() > maximum || resource.value->bytes.size() > budget - cache->resourceBytes)
                resource = Result<Resources::ResolvedResource>::Failure("World.Helper", "WORLD_HELPER_RESOURCE_BUDGET", "辅助物来源字节超过owned缓存预算，未猜占地。");
            else cache->resourceBytes += resource.value->bytes.size();
        }
        cache->resources.emplace(key, resource); return resource;
    };
    Services::PlacementPresentationBatch batch(query);
    for (const auto& candidate : candidates) {
        const auto classifyStart = Clock::now();
        auto fact = std::make_shared<WorldPathingHelperFact>();
        fact->object = candidate.object; fact->fields = candidate.fields;
        prepared.records.push_back({candidate.requestIndex, candidate.owner, fact});
        Services::PlacementPresentationInput input{candidate.object, candidate.owner, context.version, *candidate.fields};
        // 默认对象没有需要保留的辅助物行；共享服务仍逐owner核验来源/类型/预算，
        // 但不再为全图普通对象构造随即丢弃的完整输入SHA和来源副本。
        auto classification = Services::ClassifyPlacementPresentation(query, context.version, context.resourceIdentity, input);
        prepared.timings.classifyNs += ElapsedNs(classifyStart);
        if (!classification.Succeeded()) {
            fact->unavailableClassification = true; fact->diagnostics = classification.diagnostics;
            ++prepared.unavailableClassifications; AddUnique(prepared.diagnostics, classification.diagnostics); continue;
        }
        if (*classification.value != Services::PlacementPresentationKind::PathingBlockerHelper) {
            if (*classification.value == Services::PlacementPresentationKind::Unknown) {
                fact->unavailableClassification = true; ++prepared.unavailableClassifications;
            }
            continue;
        }
        fact->helper = true;
        ++prepared.timings.helperCount;
        ++prepared.timings.queryCount;
        const auto queryStart = Clock::now();
        Services::PlacementPresentationQueryTimings queryTimings;
        auto row = batch.Query(context.version, context.resourceIdentity, input, &queryTimings);
        prepared.timings.queryNs += ElapsedNs(queryStart);
        prepared.timings.queryValidateNs += queryTimings.validateNs;
        prepared.timings.queryRowCopyNs += queryTimings.rowCopyNs;
        prepared.timings.queryCanonicalNs += queryTimings.identityCanonicalNs;
        prepared.timings.queryHashNs += queryTimings.identityHashNs;
        prepared.timings.queryResourceNs += queryTimings.resourceNs;
        prepared.timings.queryFootprintNs += queryTimings.footprintNs;
        if (!row.Succeeded()) {
            fact->unavailableClassification = true; fact->diagnostics = row.diagnostics;
            ++prepared.unavailableClassifications; AddUnique(prepared.diagnostics, row.diagnostics); continue;
        }
        if (row.value->kind != Services::PlacementPresentationKind::PathingBlockerHelper) {
            fact->helper = false;
            if (row.value->kind == Services::PlacementPresentationKind::Unknown) {
                fact->unavailableClassification = true; ++prepared.unavailableClassifications;
            }
            continue;
        }
        StageTimer assetTimer{prepared.timings.assetNs};
        fact->diagnostics = row.value->resourceDiagnostics;
        AddUnique(prepared.diagnostics, row.value->resourceDiagnostics);
        std::optional<std::array<float, 2>> footprint;
        if (row.value->footprintState == Services::PlacementFootprintState::Available && row.value->footprint)
            footprint = std::array{row.value->footprint->worldWidth, row.value->footprint->worldHeight};
        else { fact->unknownFootprint = true; ++prepared.unknownFootprints; }
        std::string assetKey = "checker-source-v1;";
        Part(assetKey, row.value->textureSha256.value_or("unknown-texture"));
        if (row.value->textureSource) {
            Part(assetKey, row.value->textureSource->sourceId); Part(assetKey, row.value->textureSource->sourcePath);
        }
        if (footprint) for (auto axis : *footprint) Part(assetKey, std::to_string(std::bit_cast<std::uint32_t>(axis)));
        auto asset = cache->assets.find(assetKey);
        if (asset == cache->assets.end()) {
            auto created = Render::CreateCheckerBlockerAsset(footprint);
            if (!created.Succeeded()) {
                fact->unavailableClassification = true; AddUnique(fact->diagnostics, created.diagnostics);
                ++prepared.unavailableClassifications; AddUnique(prepared.diagnostics, created.diagnostics); continue;
            }
            AddUnique(fact->diagnostics, created.diagnostics);
            AddUnique(prepared.diagnostics, created.diagnostics);
            asset = cache->assets.emplace(std::move(assetKey), std::move(*created.value)).first;
            ++prepared.timings.assetsCreated;
        } else ++prepared.timings.assetsReused;
        usedAssets.insert(asset->first);
        auto& request = requests[candidate.requestIndex]; request.ownedStaticAsset = asset->second;
        request.scale = {1, 1, 1}; request.tint = {1, 1, 1}; request.replaceableTexture.clear();
        prepared.helperRequests[candidate.requestIndex] = 1;
        fact->assetKey = asset->first; fact->asset = asset->second;
        fact->row = std::make_shared<const Services::PlacementPresentationRow>(std::move(*row.value));
        prepared.helpers.push_back({candidate.requestIndex, fact->row, candidate.owner});
    }
    if (cache->sourceOwner) {
        StageTimer timer{prepared.timings.verifyNs};
        const auto current = cache->sourceOwner->VerifyCurrentSource();
        if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
    }
    // 当前请求未再使用的旧版本不留在下一份cache；已安装服务仍由自己的shared资产保活。
    std::erase_if(cache->assets, [&](const auto& entry) { return !usedAssets.contains(entry.first); });
    prepared.cache = std::move(cache);
    prepared.timings.totalNs = ElapsedNs(totalStart);
    return Output::Success(std::move(prepared));
}

namespace {
bool SameFields(const Services::PlacementPresentationFields& a, const Services::PlacementPresentationFields& b) {
    const auto fact = [](const auto& left, const auto& right) {
        return left.state == right.state && left.value == right.value && left.reason == right.reason;
    };
    if (!fact(a.category, b.category) || !fact(a.modelPath, b.modelPath) || !fact(a.pathingTexture, b.pathingTexture)
        || !fact(a.useClickHelper, b.useClickHelper) || a.origins.size() != b.origins.size()) return false;
    for (std::size_t i = 0; i < a.origins.size(); ++i) {
        const auto& left = a.origins[i]; const auto& right = b.origins[i];
        if (left.field != right.field || left.objectRawcode != right.objectRawcode || left.sourceId != right.sourceId
            || left.sourcePath != right.sourcePath || left.byteOffset != right.byteOffset) return false;
    }
    return true;
}
bool SameDiagnostics(const std::vector<Diagnostic>& a, const std::vector<Diagnostic>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& left = a[i]; const auto& right = b[i];
        if (left.severity != right.severity || left.subsystem != right.subsystem || left.code != right.code
            || left.message != right.message || left.origin.has_value() != right.origin.has_value()) return false;
        if (left.origin && (left.origin->source != right.origin->source || left.origin->offset != right.origin->offset
            || left.origin->length != right.origin->length || left.origin->fingerprint != right.origin->fingerprint)) return false;
    }
    return true;
}
bool SameResource(const Result<Resources::ResolvedResource>& a, const Result<Resources::ResolvedResource>& b) {
    if (!a.Succeeded() || !b.Succeeded() || !SameDiagnostics(a.diagnostics, b.diagnostics)) return false;
    const auto& left = a.value->location; const auto& right = b.value->location;
    return left.canonicalPath == right.canonicalPath && left.layer == right.layer && left.priority == right.priority
        && left.sourceId == right.sourceId && left.sourcePath == right.sourcePath && a.value->bytes == b.value->bytes;
}
using FieldComparisons = std::unordered_map<const Services::PlacementPresentationFields*,
    std::unordered_map<const Services::PlacementPresentationFields*, bool>>;
bool SameCandidateFact(const WorldPathingHelperFact& fact, const WorldPlacementPresentationCandidate& candidate,
    FieldComparisons& comparisons) {
    if (fact.object.type != candidate.object.type || fact.object.rawcode != candidate.object.rawcode
        || (fact.helper && (!fact.row || !fact.asset))) return false;
    if (fact.fields == candidate.fields) return true;
    auto& matches = comparisons[fact.fields.get()];
    auto [entry, inserted] = matches.try_emplace(candidate.fields.get(), false);
    if (inserted) entry->second = SameFields(*fact.fields, *candidate.fields);
    return entry->second;
}
Status VerifyReaders(const std::shared_ptr<WorldResourceReader>& first,
    const std::shared_ptr<WorldResourceReader>& second) {
    if (first) {
        const auto current = first->VerifyCurrentSource();
        if (!current.Succeeded()) return current;
    }
    return second && second != first ? second->VerifyCurrentSource() : Status{};
}
} // namespace

Result<Services::WorldSelectionVersion> BindWorldPathingHelperVersion(
    const Services::WorldSelectionVersion& captured,
    const Services::WorldInstanceRegistry& before, const Services::WorldInstanceRegistry& after) {
    using Output = Result<Services::WorldSelectionVersion>;
    const auto& oldBinding = before.Binding(); const auto& nextBinding = after.Binding();
    if (captured.document.documentId.empty() || !captured.document.generation
        || captured.document.sourceSha256 != oldBinding.sourceIdentity || captured.document.sourceSha256 != nextBinding.sourceIdentity
        || captured.document.revision < oldBinding.mapRevision || captured.document.revision > nextBinding.mapRevision
        || oldBinding.sourceGeneration != nextBinding.sourceGeneration || oldBinding.editSessionIdentity != nextBinding.editSessionIdentity
        || oldBinding.editRevision > nextBinding.editRevision)
        return Output::Failure("World.Helper", "WORLD_HELPER_INSTANCE_STALE", "选择快照与实例增量不属于同一来源或修订区间。");
    auto current = captured;
    current.document.revision = nextBinding.mapRevision;
    // editorRevision仍标识捕获时的选择状态，不猜下一次WorldSelection::Bind的编号。
    return Output::Success(std::move(current));
}

Result<WorldPathingHelperPrepared> PrepareWorldPathingHelperDelta(
    std::vector<Render::ModelInstanceRequest>& requests, std::span<const WorldPlacementPresentationCandidate> candidates,
    const Services::PlacementPresentationContext& context, std::shared_ptr<WorldResourceReader> sourceOwner,
    std::shared_ptr<const WorldPathingHelperPrepared> previous,
    const Services::WorldInstanceRegistry& before, const Services::WorldInstanceRegistry& after,
    std::span<const War3RenderCore::InstanceIdentity> renderIdentities) {
    using Output = Result<WorldPathingHelperPrepared>;
    const auto start = Clock::now();
    const auto failure = [](const char* code, const char* message) { return Output::Failure("World.Helper", code, message); };
    WorldPathingHelperTimings overhead;
    const auto previousCache = previous ? previous->cache : nullptr;
    bool canReuse = previousCache && sourceOwner && previousCache->sourceOwner && context.resourceReader
        && previousCache->resourceIdentity == context.resourceIdentity
        && previous->version.document.documentId == context.version.document.documentId
        && previous->version.document.generation == context.version.document.generation
        && previous->version.catalogRevision == context.version.catalogRevision;
    // 失败查找没有负租约证明，必须完整查询；稳定owner列表也不能跳过此门禁。
    if (canReuse) for (const auto& [path, resource] : previousCache->resources) {
        (void)path;
        if (!resource.Succeeded()) { canReuse = false; break; }
    }
    bool sameRecords = canReuse && previous->records.size() == candidates.size();
    FieldComparisons fieldPairs;
    std::unordered_map<std::uint64_t, const WorldPathingHelperRecord*> prior;
    {
        StageTimer timer{overhead.mappingNs};
        // 两个只读registry只能由Create/Prepare产生；Prepare仍完整校验成员SHA和不可变谱系。
        const auto proof = before.DeltaTo(after);
        if (!proof.Succeeded()) return {std::nullopt, proof.diagnostics};
        if (requests.size() != renderIdentities.size() || context.version.document.revision != after.Binding().mapRevision
            || context.version.document.sourceSha256 != after.Binding().sourceIdentity)
            return failure("WORLD_HELPER_INSTANCE_STALE", "辅助物当前版本与已验证实例注册表不符。");
        std::vector<std::uint8_t> assigned(requests.size(), 0);
        for (const auto& candidate : candidates) {
            if (!candidate.owner || !candidate.fields || candidate.requestIndex >= requests.size()
                || assigned[candidate.requestIndex])
                return failure("WORLD_HELPER_REQUEST_INVALID", "辅助物增量缺少正式owner或含重复请求。");
            assigned[candidate.requestIndex] = 1;
            const auto identity = after.Find(*candidate.owner);
            if (!identity || *identity != renderIdentities[candidate.requestIndex])
                return failure("WORLD_HELPER_INSTANCE_STALE", "辅助物当前行不属于同一注册表身份。");
        }
        if (previous) {
            if (previous->version.document.revision != before.Binding().mapRevision
                || previous->version.document.sourceSha256 != before.Binding().sourceIdentity)
                return failure("WORLD_HELPER_INSTANCE_STALE", "辅助物旧事实不属于已安装注册表。");
            // 普通单位追加不改变可破坏物候选。直接核对完整owner与事实，避免为数千条
            // 未变辅助物构造prior/reusable/fresh三套索引；重排或任一证据变化仍走通用差集。
            for (std::size_t i = 0; sameRecords && i < candidates.size(); ++i) {
                const auto& candidate = candidates[i]; const auto& record = previous->records[i];
                const auto identity = record.currentOwner ? before.Find(*record.currentOwner) : std::nullopt;
                sameRecords = identity && *identity == renderIdentities[candidate.requestIndex]
                    && record.currentOwner == candidate.owner && record.requestIndex == candidate.requestIndex
                    && record.requestIndex < previous->helperRequests.size() && record.fact && record.fact->fields;
                // 严格按文件/行递增同时证明旧owner唯一；其它合法顺序由完整索引核验。
                if (sameRecords && i) {
                    const auto& left = *candidates[i - 1].owner; const auto& right = *candidate.owner;
                    sameRecords = left.file < right.file || (left.file == right.file && left.index < right.index);
                }
                if (sameRecords) sameRecords = SameCandidateFact(*record.fact, candidate, fieldPairs);
            }
            if (!sameRecords) {
                prior.reserve(previous->records.size());
                for (const auto& record : previous->records) {
                    const auto identity = record.currentOwner ? before.Find(*record.currentOwner) : std::nullopt;
                    if (!identity || !record.fact || !record.fact->fields || record.requestIndex >= previous->helperRequests.size()
                        || !prior.emplace(identity->value, &record).second)
                        return failure("WORLD_HELPER_INSTANCE_STALE", "辅助物旧绑定缺少唯一且仍有效的实例身份。");
                }
            }
        }
    }
    std::shared_ptr<WorldPathingHelperCache> captured;
    if (canReuse) {
        StageTimer timer{overhead.verifyNs};
        const auto currentSources = VerifyReaders(previousCache->sourceOwner, sourceOwner);
        if (!currentSources.Succeeded()) return {std::nullopt, currentSources.diagnostics};
        captured = std::make_shared<WorldPathingHelperCache>();
        captured->sourceOwner = sourceOwner; captured->resourceIdentity = context.resourceIdentity;
        captured->assets = previousCache->assets;
        for (const auto& [path, original] : previousCache->resources) {
            ++overhead.resourceReads;
            auto current = context.resourceReader(path, Services::PlacementPresentationLimits{}.maxTextureBytes);
            if (!SameResource(original, current))
                return failure("WORLD_HELPER_SOURCE_STALE", "辅助物实际查找来源或字节改变，必须重新加载而非复用旧事实。");
            captured->resourceBytes += current.value->bytes.size();
            captured->resources.emplace(path, std::move(current));
        }
    }
    const auto finish = [&](WorldPathingHelperPrepared prepared) -> Output {
        {
            StageTimer timer{overhead.verifyNs};
            // 后验放在整个准备结束处；正项实际重读仍在上面，旧/新来源都不能被路径替换。
            const auto current = VerifyReaders(canReuse ? previousCache->sourceOwner : nullptr,
                prepared.cache ? prepared.cache->sourceOwner : nullptr);
            if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
        }
        prepared.timings.mappingNs += overhead.mappingNs; prepared.timings.verifyNs += overhead.verifyNs;
        prepared.timings.assetNs += overhead.assetNs; prepared.timings.resourceReads += overhead.resourceReads;
        prepared.timings.totalNs = ElapsedNs(start);
        return Output::Success(std::move(prepared));
    };
    if (sameRecords) {
        WorldPathingHelperPrepared prepared;
        {
            StageTimer timer{overhead.assetNs};
            prepared = *previous;
            prepared.version = context.version; prepared.cache = std::move(captured); prepared.timings = {};
            prepared.timings.candidateCount = candidates.size();
            prepared.helperRequests.resize(requests.size(), 0);
            for (const auto& record : prepared.records) {
                const auto& fact = *record.fact;
                if (fact.helper) ++prepared.timings.helperCount;
                if (!fact.asset) continue;
                ++prepared.timings.assetsReused;
                auto& request = requests[record.requestIndex]; request.ownedStaticAsset = fact.asset;
                request.scale = {1, 1, 1}; request.tint = {1, 1, 1}; request.replaceableTexture.clear();
            }
        }
        return finish(std::move(prepared));
    }
    std::vector<WorldPlacementPresentationCandidate> changed;
    std::unordered_map<std::uint32_t, std::shared_ptr<const WorldPathingHelperFact>> reusable;
    {
        StageTimer timer{overhead.mappingNs};
        changed.reserve(candidates.size()); reusable.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            const auto identity = renderIdentities[candidate.requestIndex];
            const auto found = prior.find(identity.value);
            bool retain = canReuse && found != prior.end();
            if (retain) retain = SameCandidateFact(*found->second->fact, candidate, fieldPairs);
            if (retain) reusable.emplace(candidate.requestIndex, found->second->fact);
            else changed.push_back(candidate);
        }
    }
    WorldPathingHelperPrepared prepared;
    if (changed.empty() && canReuse) {
        prepared.version = context.version; prepared.cache = captured;
    } else {
        auto fallback = previousCache;
        if (!canReuse && previousCache) {
            // 即使调用者传回同一reader也不能复用失败查找；仅保留资产和旧来源guard。
            auto assetsOnly = std::make_shared<WorldPathingHelperCache>();
            assetsOnly->resourceIdentity = previousCache->resourceIdentity;
            assetsOnly->sourceOwner = previousCache->sourceOwner;
            assetsOnly->assets = previousCache->assets;
            fallback = std::move(assetsOnly);
        }
        auto fresh = PrepareWorldPathingHelpers(requests, changed, context, sourceOwner, canReuse ? captured : fallback);
        if (!fresh.Succeeded()) return fresh;
        prepared = std::move(*fresh.value);
    }
    auto freshRecords = std::move(prepared.records);
    std::unordered_map<std::uint32_t, const WorldPathingHelperRecord*> freshByRequest;
    for (const auto& record : freshRecords) freshByRequest.emplace(record.requestIndex, &record);
    auto cache = std::make_shared<WorldPathingHelperCache>(*prepared.cache);
    prepared.helpers.clear(); prepared.records.clear(); prepared.diagnostics.clear();
    prepared.helpers.reserve(candidates.size()); prepared.records.reserve(candidates.size());
    prepared.helperRequests.assign(requests.size(), 0);
    prepared.unknownFootprints = prepared.unavailableClassifications = 0;
    prepared.timings.candidateCount = candidates.size(); prepared.timings.helperCount = 0;
    {
        StageTimer timer{overhead.assetNs};
        std::unordered_set<const Render::OwnedStaticModelAsset*> usedAssets;
        for (const auto& candidate : candidates) {
            const auto reused = reusable.find(candidate.requestIndex);
            const auto fresh = freshByRequest.find(candidate.requestIndex);
            const auto fact = reused != reusable.end() ? reused->second
                : fresh != freshByRequest.end() ? fresh->second->fact : nullptr;
            if (!fact) return failure("WORLD_HELPER_INSTANCE_STALE", "辅助物增量缺少当前候选事实。");
            const auto old = prior.find(renderIdentities[candidate.requestIndex].value);
            if (old != prior.end() && old->second->fact->asset != fact->asset)
                return failure("WORLD_HELPER_INSTANCE_REBUILD_REQUIRED", "存活辅助物的资产或分类已变化，普通实例增量不能替换旧资产。");
            prepared.records.push_back({candidate.requestIndex, candidate.owner, fact});
            AddUnique(prepared.diagnostics, fact->diagnostics);
            if (fact->helper) ++prepared.timings.helperCount;
            if (fact->unavailableClassification) ++prepared.unavailableClassifications;
            if (fact->unknownFootprint) ++prepared.unknownFootprints;
            if (!fact->asset) continue;
            if (reused != reusable.end()) ++prepared.timings.assetsReused;
            if (usedAssets.insert(fact->asset.get()).second) cache->assets.emplace(fact->assetKey, fact->asset);
            auto& request = requests[candidate.requestIndex]; request.ownedStaticAsset = fact->asset;
            request.scale = {1, 1, 1}; request.tint = {1, 1, 1}; request.replaceableTexture.clear();
            prepared.helperRequests[candidate.requestIndex] = 1;
            prepared.helpers.push_back({candidate.requestIndex, fact->row, candidate.owner});
        }
        std::erase_if(cache->assets, [&](const auto& entry) { return !usedAssets.contains(entry.second.get()); });
    }
    prepared.cache = std::move(cache);
    return finish(std::move(prepared));
}
} // namespace War3Map::Modules
