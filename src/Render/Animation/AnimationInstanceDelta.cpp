#include "AnimationInternal.h"
#include "AnimationParticles.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace War3Map::Render {
// 索引拥有它所描述的不可变请求表；没有借用字符串视图、地址键缓存或旧State链。
// 新候选按实际增删/改名更新有序request号；只有完整绑定相同才可直接复用。
struct AnimationCatalogIdentityIndex {
    SharedSceneVector<AnimationInstanceSpec> specs;
    std::shared_ptr<const std::vector<std::uint32_t>> byIdentity, byHostKey;
    std::uint64_t sceneVersion = 0, documentRevision = 0, generation = 0;
    std::string sourceIdentity;
    bool uniqueHostKeys = false;
    explicit AnimationCatalogIdentityIndex(const AnimatedModelScene& scene)
        : specs(scene.specs), sceneVersion(scene.sceneVersion), documentRevision(scene.documentRevision),
          generation(scene.generation), sourceIdentity(scene.sourceIdentity) {}
    [[nodiscard]] bool Matches(const AnimatedModelScene& scene) const noexcept {
        return specs.SharesStorageWith(scene.specs) && sceneVersion == scene.sceneVersion &&
            documentRevision == scene.documentRevision && generation == scene.generation &&
            sourceIdentity == scene.sourceIdentity;
    }
};
namespace {
using Identity=War3RenderCore::InstanceIdentity;
using Key=std::pair<std::uint64_t,std::uint64_t>;
using Clock=std::chrono::steady_clock;
class DeltaPhaseTimer {
public:
    explicit DeltaPhaseTimer(std::uint64_t* output):output_(output),start_(Clock::now()) {}
    ~DeltaPhaseTimer() { Switch(nullptr); }
    void Switch(std::uint64_t* output) {
        const auto end=Clock::now();
        if (output_) *output_+=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end-start_).count());
        output_=output; start_=end;
    }
private:
    std::uint64_t* output_;
    Clock::time_point start_;
};
Key IdentityKey(Identity id) { return {id.value,id.generation}; }
using IdentityIndexResult=Result<std::shared_ptr<const AnimationCatalogIdentityIndex>>;
bool FitsIdentityIndex(std::size_t count,std::uint64_t maxBytes) {
    return count<UINT32_MAX && count<=maxBytes/(2*sizeof(std::uint32_t));
}
bool UniqueHostKeys(const AnimatedModelScene& scene,const std::vector<std::uint32_t>& order) {
    return std::adjacent_find(order.begin(),order.end(),[&](auto a,auto b) {
        return scene.specs[a].hostKey==scene.specs[b].hostKey;
    })==order.end();
}
IdentityIndexResult AdvanceIdentityIndex(const AnimationCatalogIdentityIndex& previous,
    const AnimatedModelScene& scene,std::span<const std::uint32_t> oldToNew,std::uint32_t survivors,
    std::span<const std::uint8_t> changed,std::span<const std::uint32_t> renamed,std::uint64_t maxBytes) {
    if (!FitsIdentityIndex(scene.specs.size(),maxBytes)) return IdentityIndexResult::Failure(
        "Render.Animation","ANIMATION_DELTA_BUDGET","实例身份索引超过配置内存/编号范围，未截断实例。");
    auto next=std::make_shared<AnimationCatalogIdentityIndex>(scene);
    const bool sameRoster=survivors==previous.specs.size() && survivors==scene.specs.size();
    if (sameRoster) next->byIdentity=previous.byIdentity;
    else {
        auto order=std::make_shared<std::vector<std::uint32_t>>(); order->reserve(scene.specs.size());
        for (const auto old:*previous.byIdentity) if (oldToNew[old]!=UINT32_MAX) order->push_back(oldToNew[old]);
        const auto retained=order->size();
        for (auto request=survivors;request<scene.specs.size();++request) order->push_back(request);
        const auto byIdentity=[&](auto a,auto b) { return IdentityKey(scene.specs[a].identity)<IdentityKey(scene.specs[b].identity); };
        const auto added=order->begin()+static_cast<std::ptrdiff_t>(retained);
        std::sort(added,order->end(),byIdentity);
        std::inplace_merge(order->begin(),added,order->end(),byIdentity);
        next->byIdentity=std::move(order);
    }
    if (sameRoster && renamed.empty()) {
        next->byHostKey=previous.byHostKey; next->uniqueHostKeys=previous.uniqueHostKeys;
    } else {
        auto order=std::make_shared<std::vector<std::uint32_t>>(); order->reserve(scene.specs.size());
        for (const auto old:*previous.byHostKey) {
            const auto request=oldToNew[old];
            if (request!=UINT32_MAX && !(changed[request]&1u)) order->push_back(request);
        }
        const auto retained=order->size();
        order->insert(order->end(),renamed.begin(),renamed.end());
        for (auto request=survivors;request<scene.specs.size();++request) order->push_back(request);
        const auto byHost=[&](auto a,auto b) { return scene.specs[a].hostKey<scene.specs[b].hostKey; };
        const auto added=order->begin()+static_cast<std::ptrdiff_t>(retained);
        std::sort(added,order->end(),byHost);
        std::inplace_merge(order->begin(),added,order->end(),byHost);
        next->uniqueHostKeys=UniqueHostKeys(scene,*order); next->byHostKey=std::move(order);
    }
    if (!next->uniqueHostKeys) return IdentityIndexResult::Failure(
        "Render.Animation","ANIMATION_DELTA_ROW_DUPLICATE","重映射后的宿主行键重复。");
    return IdentityIndexResult::Success(std::move(next));
}
std::string VariantKey(const AnimationInstanceSpec& spec,const AnimationAsset& asset,std::string key) {
    bool team=false,replacement=false;
    if (asset.parsed) for (const auto& texture:asset.parsed->textures) {
        team|=texture.replaceableId==1 || texture.replaceableId==2;
        replacement|=texture.replaceableId>=3;
    }
    if (team) key+="|team"+std::to_string(spec.model.teamColor);
    if (replacement) key+="|"+AnimationPathKey(spec.model.replaceableTexture);
    if (spec.model.previewGhost) key+="|preview-ghost";
    return key;
}
std::string VariantKey(const AnimationInstanceSpec& spec,const AnimationAsset& asset) {
    return VariantKey(spec,asset,ModelAssetKey(spec.model));
}
enum class CatalogScene { ReadOnlyCapture, OwnedForEdit };
template<class State> std::unique_ptr<State> CloneCatalog(const State& source,
    CatalogScene sceneMode=CatalogScene::OwnedForEdit) {
    auto out=std::make_unique<State>();
    // 已完成初始化的scene在Prepare中只读；捕获可共享整个目录。
    // 实际增量/重基有独占scene，静态表按写分离；State、求值器和粒子workspace始终独立。
    out->scene=sceneMode==CatalogScene::ReadOnlyCapture ? source.scene : std::make_shared<AnimatedModelScene>(*source.scene);
    out->catalogIdentity=source.catalogIdentity;
    out->assets=source.assets; out->assetPaths=source.assetPaths; out->resources=source.resources;
    out->sourceBytes=source.sourceBytes; out->resourceBytes=source.resourceBytes; out->limits=source.limits;
    out->requestAssets=source.requestAssets; out->modelAssets=source.modelAssets; out->initial=source.initial;
    out->collisionNodesRequired=source.collisionNodesRequired;
    out->retainUnsupportedStatic=source.retainUnsupportedStatic; out->maxVisibilityBytes=source.maxVisibilityBytes;
    out->epoch=NextAnimationSerial();
    return out;
}
template<class State> AnimationResourceReader FrozenReader(const State& source) {
    return [&source](std::string_view path,std::uint64_t budget) -> Result<AnimationResource> {
        const auto found=source.resources.find(AnimationPathKey(path));
        if (found==source.resources.end()) return Result<AnimationResource>::Failure(
            "Render.Animation","RESOURCE_NOT_FOUND","增量档案没有已捕获的资源："+std::string(path));
        if (found->second->bytes.size()>budget) return Result<AnimationResource>::Failure(
            "Render.Animation","ANIMATION_RESOURCE_BUDGET","冻结资源超过本次读取预算。");
        return Result<AnimationResource>::Success(*found->second);
    };
}
void MergeEdits(std::vector<AnimationEdit>& target,std::span<const AnimationEdit> edits) {
    for (const auto& edit:edits) {
        const auto found=std::find_if(target.begin(),target.end(),[&](const auto& item) { return item.identity==edit.identity; });
        if (found==target.end()) { target.push_back(edit); continue; }
        if (edit.sequenceIndex) found->sequenceIndex=edit.sequenceIndex;
        if (edit.elapsedMs) found->elapsedMs=edit.elapsedMs;
        if (edit.globalMs) found->globalMs=edit.globalMs;
        if (edit.speed) found->speed=edit.speed;
        if (edit.playing) found->playing=edit.playing;
        if (edit.transform) found->transform=edit.transform;
        if (edit.tint) found->tint=edit.tint;
    }
}
}

Result<std::shared_ptr<const AnimationCatalogIdentityIndex>> EnsureAnimationCatalogIdentityIndex(
    const AnimatedModelScene& scene,std::shared_ptr<const AnimationCatalogIdentityIndex> previous,
    std::uint64_t maxBytes) {
    if (!FitsIdentityIndex(scene.specs.size(),maxBytes)) return IdentityIndexResult::Failure(
        "Render.Animation","ANIMATION_DELTA_BUDGET","实例身份索引超过配置内存/编号范围，未截断实例。");
    if (previous && previous->Matches(scene)) return IdentityIndexResult::Success(std::move(previous));
    auto index=std::make_shared<AnimationCatalogIdentityIndex>(scene);
    auto identities=std::make_shared<std::vector<std::uint32_t>>(); identities->reserve(scene.specs.size());
    for (std::uint32_t i=0;i<scene.specs.size();++i) identities->push_back(i);
    const auto byIdentity=[&](auto a,auto b) { return IdentityKey(scene.specs[a].identity)<IdentityKey(scene.specs[b].identity); };
    if (!std::is_sorted(identities->begin(),identities->end(),byIdentity))
        std::sort(identities->begin(),identities->end(),byIdentity);
    if (std::adjacent_find(identities->begin(),identities->end(),[&](auto a,auto b) {
        return scene.specs[a].identity==scene.specs[b].identity;
    })!=identities->end()) return IdentityIndexResult::Failure(
        "Render.Animation","ANIMATION_DELTA_INSTANCE_STALE","驻留请求表含重复实例身份。");
    auto hosts=std::make_shared<std::vector<std::uint32_t>>(*identities);
    std::sort(hosts->begin(),hosts->end(),[&](auto a,auto b) { return scene.specs[a].hostKey<scene.specs[b].hostKey; });
    index->uniqueHostKeys=UniqueHostKeys(scene,*hosts);
    index->byIdentity=std::move(identities); index->byHostKey=std::move(hosts);
    return IdentityIndexResult::Success(std::move(index));
}

Result<std::shared_ptr<const ModelAnimationService>> ModelAnimationService::CaptureInstanceCatalog() const try {
    return Result<std::shared_ptr<const ModelAnimationService>>::Success(
        std::shared_ptr<const ModelAnimationService>(new ModelAnimationService(CloneCatalog(*state_,CatalogScene::ReadOnlyCapture))));
} catch (const std::exception&) {
    return Result<std::shared_ptr<const ModelAnimationService>>::Failure("Render.Animation",
        "ANIMATION_CATALOG_CAPTURE_FAILED", "捕获只读实例档案失败，驻留世界未改变。");
}

Result<std::shared_ptr<const AnimationInstanceDeltaPlan>> ModelAnimationService::PrepareInstanceCatalog(
    AnimationInstanceDelta delta,const AnimationResourceReader& read,
    std::shared_ptr<const AnimationAssetSnapshot> preloaded,AnimationInstanceDeltaTimings* timings,
    std::shared_ptr<const AnimationPreparedAssets> prepared) const try {
    if (timings) *timings={};
    DeltaPhaseTimer total(timings ? &timings->totalNs : nullptr);
    DeltaPhaseTimer phase(timings ? &timings->identityNs : nullptr);
    using Out=Result<std::shared_ptr<const AnimationInstanceDeltaPlan>>;
    const auto fail=[](const char* code,const char* message) { return Out::Failure("Render.Animation",code,message); };
    const auto& old=*state_->scene;
    if (!read || delta.session.generation!=old.generation || delta.session.sourceIdentity!=old.sourceIdentity)
        return fail("ANIMATION_DELTA_SOURCE_STALE","实例增量的来源身份或代次与驻留场景不同。");
    const auto bytes=std::uint64_t(old.specs.size()+delta.additions.size())*sizeof(AnimationInstanceSpec);
    if (bytes>state_->limits.maxFrameBytes || delta.additions.size()>=UINT32_MAX-old.specs.size())
        return fail("ANIMATION_DELTA_BUDGET","实例增量的实际请求表超过内存/索引范围，未截断地图实例。");
    auto indexed=EnsureAnimationCatalogIdentityIndex(old,state_->catalogIdentity,state_->limits.maxFrameBytes);
    if (!indexed.Succeeded()) return {std::nullopt,std::move(indexed.diagnostics)};
    const auto& index=**indexed.value; const auto& original=*index.byIdentity;
    const auto findOriginal=[&](const Key& key) {
        const auto found=std::lower_bound(original.begin(),original.end(),key,[&](std::uint32_t request,const Key& target) {
            return IdentityKey(old.specs[request].identity)<target;
        });
        return found!=original.end() && IdentityKey(old.specs[*found].identity)==key ? *found : UINT32_MAX;
    };
    std::vector<std::uint8_t> removed(old.specs.size(),0);
    for (const auto id:delta.removals) {
        const auto request=findOriginal(IdentityKey(id));
        if (request==UINT32_MAX || removed[request])
            return fail("ANIMATION_DELTA_INSTANCE_STALE","移除请求含未知或重复身份。");
        removed[request]=1;
    }
    auto plan=std::make_shared<AnimationInstanceDeltaPlan>();
    plan->baseSceneVersion_=old.sceneVersion; plan->baseDocumentRevision_=old.documentRevision;
    plan->oldToNewRequests_.assign(old.specs.size(),UINT32_MAX);
    phase.Switch(timings ? &timings->cloneNs : nullptr);
    auto candidate=CloneCatalog(*state_); auto& scene=*candidate->scene;
    phase.Switch(timings ? &timings->identityNs : nullptr);
    scene.sceneVersion=NextAnimationSerial(); scene.documentRevision=delta.session.documentRevision;
    // 同一目录的静态请求/动作表只在真正修改时分离。删除才原地稳定压紧，
    // 单纯追加不再把每个幸存行移出后搬回；没有行变更的重基继续共享两张只读表。
    const auto& previousAssets=state_->requestAssets;
    scene.unavailableInstances.clear();
    const auto requestCapacity=old.specs.size()-delta.removals.size()+delta.additions.size();
    if (!delta.additions.empty()) {
        scene.specs.reserve(requestCapacity); scene.sequences.reserve(requestCapacity);
        scene.particleCapabilities.reserve(requestCapacity); candidate->requestAssets.reserve(requestCapacity);
        candidate->initial.reserve(requestCapacity);
    }
    if (!delta.removals.empty()) {
        // 可写引用只在本块存活，绝不跨越计划发布或新的scene快照。
        auto& specs=static_cast<std::vector<AnimationInstanceSpec>&>(scene.specs);
        auto& sequences=static_cast<std::vector<std::vector<MdxSequence>>&>(scene.sequences);
        std::uint32_t next=0;
        for (std::uint32_t i=0;i<old.specs.size();++i) if (!removed[i]) {
            plan->oldToNewRequests_[i]=next;
            if (next!=i) {
                specs[next]=std::move(specs[i]);
                sequences[next]=std::move(sequences[i]);
                scene.particleCapabilities[next]=std::move(scene.particleCapabilities[i]);
                candidate->requestAssets[next]=candidate->requestAssets[i];
                candidate->initial[next]=std::move(candidate->initial[i]);
            }
            ++next;
        }
        specs.resize(next); sequences.resize(next); scene.particleCapabilities.resize(next);
        candidate->requestAssets.resize(next); candidate->initial.resize(next);
    } else {
        for (std::uint32_t i=0;i<old.specs.size();++i) plan->oldToNewRequests_[i]=i;
    }
    const auto survivorCount=static_cast<std::uint32_t>(scene.specs.size());
    std::map<Key,std::uint32_t> addedRequests;
    for (const auto& added:delta.additions) {
        const auto key=IdentityKey(added.identity);
        if (!added.identity.value || added.identity.generation!=old.generation || added.hostKey.empty() ||
            findOriginal(key)!=UINT32_MAX || !addedRequests.emplace(key,static_cast<std::uint32_t>(scene.specs.size())).second)
            return fail("ANIMATION_DELTA_INSTANCE_IDENTITY","新增实例必须有独立当前代次身份与宿主键，不能复用被删身份。");
        scene.specs.push_back(added);
    }
    const auto findCurrent=[&](const Key& key) {
        const auto previous=findOriginal(key);
        if (previous!=UINT32_MAX) return plan->oldToNewRequests_[previous];
        const auto added=addedRequests.find(key);
        return added==addedRequests.end() ? UINT32_MAX : added->second;
    };
    std::vector<std::uint8_t> changed(scene.specs.size(),0);
    std::vector<std::uint32_t> renamed; renamed.reserve(std::min(delta.rows.size(),std::size_t(survivorCount)));
    for (const auto& row:delta.rows) {
        const auto request=findCurrent(IdentityKey(row.identity));
        if (row.hostKey.empty() || request==UINT32_MAX || (changed[request]&1u))
            return fail("ANIMATION_DELTA_ROW_STALE","行重映射含空键、未知或重复身份。");
        changed[request]=static_cast<std::uint8_t>(changed[request]|1u);
        scene.specs[request].hostKey=row.hostKey;
        if (request<survivorCount) renamed.push_back(request);
    }
    for (const auto& edit:delta.edits) {
        const auto request=findCurrent(IdentityKey(edit.identity));
        if (request==UINT32_MAX || (changed[request]&2u))
            return fail("ANIMATION_DELTA_EDIT_STALE","实例编辑含未知、移除或重复身份。");
        changed[request]=static_cast<std::uint8_t>(changed[request]|2u);
    }
    // 来源校验仍由正常保存/资产链执行；这里只复用有拥有者且完整绑定的已验身份次序。
    // 真正的新行、移除和改名已逐项验证，只排序改变的尾部，再与保序幸存行归并。
    auto nextIndex=AdvanceIdentityIndex(index,scene,plan->oldToNewRequests_,survivorCount,changed,renamed,state_->limits.maxFrameBytes);
    if (!nextIndex.Succeeded()) return {std::nullopt,std::move(nextIndex.diagnostics)};
    candidate->catalogIdentity=std::move(*nextIndex.value);
    phase.Switch(timings ? &timings->assetNs : nullptr);
    std::map<std::string,std::uint32_t> variants;
    std::vector<std::uint32_t> requestModels(old.specs.size(),UINT32_MAX);
    std::vector<bool> indexedModels(old.geometry.models.size(),false);
    for (std::size_t i=0;i<old.geometry.instances.size();++i) {
        const auto request=old.geometry.instanceRequests[i];
        if (request==kBakedInstance) continue;
        const auto model=old.geometry.instances[i].model;
        requestModels[request]=model;
        if (indexedModels[model]) continue;
        indexedModels[model]=true;
        const auto asset=state_->requestAssets[request];
        variants.emplace(VariantKey(old.specs[request],state_->assets[asset],state_->assetPaths[asset]),model);
    }
    // 纯粒子或不可绘制目录也属于已加载结果，不能误当缺失而重新读资产。
    for (std::uint32_t i=0;i<old.specs.size();++i) if (requestModels[i]==UINT32_MAX) {
        const auto asset=state_->requestAssets[i];
        variants.try_emplace(VariantKey(old.specs[i],state_->assets[asset],state_->assetPaths[asset]),UINT32_MAX);
    }
    std::map<std::string,std::uint32_t> paths;
    for (std::uint32_t i=0;i<candidate->assetPaths.size();++i) paths.emplace(candidate->assetPaths[i],i);
    std::vector<AnimationInstanceSpec> missing;
    std::set<std::string> loading;
    for (const auto& spec:delta.additions) {
        const auto found=paths.find(ModelAssetKey(spec.model));
        const auto key=found==paths.end() ? ModelAssetKey(spec.model)+"|"+
            std::to_string(spec.model.teamColor)+"|"+AnimationPathKey(spec.model.replaceableTexture)+(spec.model.previewGhost ? "|ghost" : "") :
            VariantKey(spec,candidate->assets[found->second],found->first);
        if ((found==paths.end() || !variants.contains(key)) && loading.insert(key).second) missing.push_back(spec);
    }
    if (prepared) {
        const auto miss = [&](const char* reason) { if (timings) timings->preparedAssetMissReason = reason; };
        if (prepared->baseSceneVersion_ != old.sceneVersion || prepared->baseDocumentRevision_ != old.documentRevision ||
            prepared->generation_ != old.generation || prepared->sourceIdentity_ != old.sourceIdentity) {
            miss("ANIMATION_PREPARED_CATALOG_STALE");
        } else {
            const auto matched = std::find_if(missing.begin(), missing.end(), [&](const auto& spec) {
                return !spec.model.previewGhost && ModelAssetKey(spec.model) == prepared->assetKey_ &&
                    spec.model.teamColor == prepared->teamColor_ &&
                    AnimationPathKey(spec.model.replaceableTexture) == prepared->replacementKey_;
            });
            if (matched == missing.end()) {
                miss(missing.empty() ? "ANIMATION_PREPARED_NOT_NEEDED" : "ANIMATION_PREPARED_REQUEST_MISMATCH");
            } else {
                // 只复核本包实际访问的新来源；旧resident来源由宿主的前后Verify继续保护。
                // 来源变更不能靠包内旧字节掩盖，也不能在未知遮蔽变化时回退低层。
                DeltaPhaseTimer preparedPhase(timings ? &timings->preparedSourceVerifyNs : nullptr);
                for (const auto& access : prepared->accesses_) {
                    auto actual = read(access.path, access.budget);
                    if (access.resource) {
                        if (!actual.Succeeded()) {
                            miss("ANIMATION_PREPARED_SOURCE_REJECTED");
                            if (!actual.diagnostics.empty()) return {std::nullopt, std::move(actual.diagnostics)};
                            return fail("ANIMATION_PREPARED_SOURCE_REJECTED", "正式静态预取的实际来源复核失败。");
                        }
                        if (actual.value->bytes.size() > access.budget ||
                            actual.value->sourceLayer != access.resource->sourceLayer ||
                            actual.value->digest != access.resource->digest || actual.value->bytes != access.resource->bytes) {
                            miss("ANIMATION_PREPARED_SOURCE_CHANGED");
                            return fail("ANIMATION_PREPARED_SOURCE_CHANGED", "正式静态预取后资源层或原字节变化，未发布过期资产。");
                        }
                    } else {
                        const bool same = !actual.Succeeded() && actual.diagnostics.size() == access.diagnostics.size() &&
                            std::equal(actual.diagnostics.begin(), actual.diagnostics.end(), access.diagnostics.begin(),
                                [](const auto& left, const auto& right) {
                                    return left.severity == right.severity && left.subsystem == right.subsystem && left.code == right.code;
                                });
                        if (!same) {
                            miss("ANIMATION_PREPARED_SOURCE_CHANGED");
                            return fail("ANIMATION_PREPARED_SOURCE_CHANGED", "正式静态预取的缺失来源状态已变化，未使用过期负缓存。");
                        }
                    }
                }
                preparedPhase.Switch(timings ? &timings->preparedGeometryApplyNs : nullptr);
                if (prepared->assetIndex_ > candidate->assets.size() ||
                    (prepared->newAsset_ && prepared->assetIndex_ != candidate->assets.size()) ||
                    (!prepared->newAsset_ && (prepared->assetIndex_ >= candidate->assets.size() ||
                        candidate->assetPaths[prepared->assetIndex_] != prepared->assetKey_)) ||
                    (prepared->asset_.compiled.Valid() && prepared->asset_.compiled.Identity() !=
                        War3RenderCore::AssetIdentity{std::uint64_t(prepared->assetIndex_) + 1, scene.generation}))
                    return fail("ANIMATION_PREPARED_IDENTITY_STALE", "正式静态预取的资产编号不属于原catalog，未重绑编译身份。");
                if (prepared->mergedGeometry_.dynamicMaterialStates) {
                    const auto expectedAssets=candidate->assets.size()+(prepared->newAsset_ ? 1u : 0u);
                    if (prepared->materialTextures_.size()!=expectedAssets)
                        return fail("ANIMATION_TEXTURE_OWNER_INVALID", "正式预取的材质元数据不覆盖同一资产目录。");
                    for (std::size_t i=0;i<expectedAssets;++i) {
                        const auto& asset=i<candidate->assets.size() ? candidate->assets[i] : prepared->asset_;
                        const auto& textures=prepared->materialTextures_[i];
                        if ((asset.compiled.Valid() && asset.unavailableCode.empty() && !textures) ||
                            (textures && !textures->Matches(asset.compiled)))
                            return fail("ANIMATION_TEXTURE_OWNER_INVALID", "正式预取的材质元数据与原编译资产身份不同。");
                    }
                } else if (!prepared->materialTextures_.empty()) {
                    return fail("ANIMATION_TEXTURE_OWNER_INVALID", "非材质状态模式不能携带另一模式的预取元数据。");
                }
                if (candidate->sourceBytes > state_->limits.maxAssetBytes ||
                    prepared->sourceBytes_ > state_->limits.maxAssetBytes - candidate->sourceBytes ||
                    candidate->resourceBytes > state_->limits.maxAssetBytes ||
                    prepared->resourceBytes_ > state_->limits.maxAssetBytes - candidate->resourceBytes)
                    return fail("ANIMATION_ASSET_BUDGET", "正式静态预取实际拥有的原字节超过当前资产预算。");
                const auto added = prepared->addedGeometry_.Materialize();
                if (auto budget = CheckAnimationStaticAppend(scene, added, state_->limits.geometry); !budget.Succeeded())
                    return {std::nullopt, std::move(budget.diagnostics)};
                const auto modelBase = static_cast<std::uint32_t>(scene.models.size());
                // 来源和当前预算均已复核。仅替换静态缓冲，保留上方已编辑的实例/行映射表；
                // 完整几何合并和COW复制在PrepareStaticAssets完成，正式点击不再复制旧世界。
                prepared->mergedGeometry_.ApplyTo(scene);
                preparedPhase.Switch(nullptr);
                if (prepared->newAsset_) {
                    candidate->assets.push_back(prepared->asset_); candidate->assetPaths.push_back(prepared->assetKey_);
                }
                for (std::size_t i=0;i<prepared->materialTextures_.size();++i)
                    candidate->assets[i].materialTextures=prepared->materialTextures_[i];
                for (const auto& [path, resource] : prepared->resources_) candidate->resources.emplace(path, resource);
                candidate->sourceBytes += prepared->sourceBytes_; candidate->resourceBytes += prepared->resourceBytes_;
                candidate->collisionNodesRequired |= prepared->collisionNodesRequired_;
                candidate->modelAssets.insert(candidate->modelAssets.end(), prepared->modelAssets_.begin(), prepared->modelAssets_.end());
                variants[VariantKey(*matched, candidate->assets[prepared->assetIndex_])] =
                    prepared->modelIndex_ == UINT32_MAX ? UINT32_MAX : modelBase + prepared->modelIndex_;
                paths[prepared->assetKey_] = prepared->assetIndex_;
                missing.erase(matched); plan->assetsChanged_ = true;
                if (timings) timings->preparedAssetHit = true;
            }
        }
    }
    if (!missing.empty()) {
        auto extra=BuildStaticAssets(delta.session,missing,read,state_->limits,{},candidate.get(),std::move(preloaded));
        if (!extra.Succeeded()) return {std::nullopt,std::move(extra.diagnostics)};
        const auto& add=**extra.value;
        const auto modelBase=static_cast<std::uint32_t>(scene.models.size());
        if (auto budget = CheckAnimationStaticAppend(scene, *add.scene, state_->limits.geometry); !budget.Succeeded())
            return {std::nullopt, std::move(budget.diagnostics)};
        AppendAnimationStaticGeometry(scene,*add.scene);
        candidate->assets=add.assets; candidate->assetPaths=add.assetPaths; candidate->resources=add.resources;
        candidate->sourceBytes=add.sourceBytes; candidate->resourceBytes=add.resourceBytes;
        candidate->collisionNodesRequired=add.collisionNodesRequired;
        candidate->modelAssets.insert(candidate->modelAssets.end(),add.modelAssets.begin(),add.modelAssets.end());
        for (std::size_t i=0;i<add.scene->geometry.instances.size();++i) {
            const auto r=add.scene->geometry.instanceRequests[i];
            if (r!=kBakedInstance) variants[VariantKey(add.scene->specs[r],add.assets[add.requestAssets[r]])]=modelBase+add.scene->geometry.instances[i].model;
        }
        for (std::uint32_t i=0;i<add.scene->specs.size();++i) variants.try_emplace(
            VariantKey(add.scene->specs[i],add.assets[add.requestAssets[i]]),UINT32_MAX);
        paths.clear();
        for (std::uint32_t i=0;i<candidate->assetPaths.size();++i) paths.emplace(candidate->assetPaths[i],i);
        plan->assetsChanged_=true;
    }
    std::uint64_t decodedBytes=0;
    for (const auto& texture:std::as_const(scene.geometry.textures)) decodedBytes+=texture.rgba.size();
    if (scene.geometry.textures.size()>state_->limits.geometry.maxTextures || decodedBytes>state_->limits.geometry.maxTotalTextureBytes)
        return fail("ANIMATION_DELTA_TEXTURE_BUDGET","增量后的实际解码纹理超过资产预算。");
    phase.Switch(timings ? &timings->orderingNs : nullptr);
    std::vector<std::pair<War3RenderModelInstanceV2,std::uint32_t>> instances;
    instances.reserve(old.geometry.instances.size()+delta.additions.size());
    for (std::size_t i=0;i<old.geometry.instances.size();++i) {
        const auto request=old.geometry.instanceRequests[i];
        if (request==kBakedInstance) instances.emplace_back(old.geometry.instances[i],kBakedInstance);
        else if (plan->oldToNewRequests_[request]!=UINT32_MAX)
            instances.emplace_back(old.geometry.instances[i],plan->oldToNewRequests_[request]);
    }
    const auto retainedRendered=instances.size();
    scene.geometry.skippedInstances=0; scene.particleInstanceCount=0;
    for (std::uint32_t r=0;r<scene.specs.size();++r) {
        const auto& spec=std::as_const(scene).specs[r];
        const bool survivor=r<survivorCount;
        const auto a=survivor ? candidate->requestAssets[r] : paths.at(ModelAssetKey(spec.model));
        const auto& asset=candidate->assets[a];
        if (asset.particleCapability.available) ++scene.particleInstanceCount;
        if (!asset.unavailableCode.empty()) scene.unavailableInstances.push_back({spec.identity,{std::uint64_t(a)+1,scene.generation},r,asset.unavailableCode});
        if (survivor) continue;
        candidate->requestAssets.push_back(a);
        scene.sequences.push_back(asset.parsed ? asset.parsed->sequences : std::vector<MdxSequence>{});
        scene.particleCapabilities.push_back(asset.particleCapability);
        auto initial=MakeAnimationInitialState(spec,asset,state_->retainUnsupportedStatic,scene.geometry.warnings);
        if (!initial.Succeeded()) return {std::nullopt,std::move(initial.diagnostics)};
        candidate->initial.push_back(std::move(*initial.value));
        const auto model=variants.at(VariantKey(spec,asset,candidate->assetPaths[a]));
        if (model==UINT32_MAX) { if (!asset.particleCapability.available) ++scene.geometry.skippedInstances; continue; }
        War3RenderModelInstanceV2 instance{}; instance.model=model;
        const auto transform=ModelInstanceTransform(spec.model);
        std::copy(transform.begin(),transform.end(),instance.transform);
        std::copy(spec.model.tint.begin(),spec.model.tint.end(),instance.color);
        instances.emplace_back(instance,r);
    }
    for (std::uint32_t i=0;i<old.specs.size();++i) if (plan->oldToNewRequests_[i]!=UINT32_MAX &&
        requestModels[i]==UINT32_MAX && !candidate->assets[previousAssets[i]].particleCapability.available)
        ++scene.geometry.skippedInstances;
    const auto byModel=[](const auto& a,const auto& b) { return a.first.model<b.first.model; };
    const auto newBegin=instances.begin()+static_cast<std::ptrdiff_t>(retainedRendered);
    if (std::is_sorted(instances.begin(),newBegin,byModel)) {
        // 保留旧模型序，只排序新增尾部；稳定归并保证同模型旧实例在新增之前。
        std::stable_sort(newBegin,instances.end(),byModel);
        std::inplace_merge(instances.begin(),newBegin,instances.end(),byModel);
    } else {
        // 早期/外部目录未排序时维持原完整稳定排序语义，不假定旧序永远有效。
        std::stable_sort(instances.begin(),instances.end(),byModel);
    }
    scene.geometry.instances.clear(); scene.geometry.instanceRequests.clear();
    for (const auto& [instance,request]:instances) {
        scene.geometry.instances.push_back(instance); scene.geometry.instanceRequests.push_back(request);
    }
    scene.geometry.requestedInstances=scene.specs.size();
    phase.Switch(timings ? &timings->storageNs : nullptr);
    if (auto status=InitializeStorage(*candidate,state_->limits,read,timings); !status.Succeeded())
        return {std::nullopt,std::move(status.diagnostics)};
    phase.Switch(timings ? &timings->identityNs : nullptr);
    plan->prepared_=std::shared_ptr<ModelAnimationService>(new ModelAnimationService(std::move(candidate)));
    plan->delta_=std::move(delta);
    return Out::Success(std::move(plan));
} catch (const std::bad_alloc&) {
    return Result<std::shared_ptr<const AnimationInstanceDeltaPlan>>::Failure("Render.Animation","ANIMATION_DELTA_ALLOCATION_FAILED","增量候选内存分配失败，驻留场景未改动。");
} catch (const std::exception& e) {
    return Result<std::shared_ptr<const AnimationInstanceDeltaPlan>>::Failure("Render.Animation","ANIMATION_DELTA_BUILD_FAILED",e.what());
}

Result<AnimationInstanceDeltaResult> ModelAnimationService::PrepareInstanceDelta(const AnimationFrame& latest,
    const AnimationInstanceDeltaPlan& plan,const AnimationAdvance& advance,AnimationInstanceDeltaTimings* timings) const try {
    if (timings) *timings={};
    DeltaPhaseTimer total(timings ? &timings->totalNs : nullptr);
    DeltaPhaseTimer phase(timings ? &timings->identityNs : nullptr);
    using Out=Result<AnimationInstanceDeltaResult>;
    const auto fail=[](const char* code,const char* message) { return Out::Failure("Render.Animation",code,message); };
    const auto& old=*state_->scene;
    if (!plan.prepared_ || plan.baseSceneVersion_!=old.sceneVersion || plan.baseDocumentRevision_!=old.documentRevision ||
        latest.sceneVersion!=old.sceneVersion || latest.documentRevision!=old.documentRevision || latest.states.size()!=old.specs.size() ||
        plan.oldToNewRequests_.size()!=old.specs.size() || latest.poses.size()!=old.geometry.instances.size() ||
        latest.instances.size()!=latest.poses.size())
        return fail("ANIMATION_DELTA_FRAME_STALE","增量档案或最新接受帧不属于当前驻留场景。");
    for (std::size_t i=0;i<latest.states.size();++i) if (latest.states[i].identity!=old.specs[i].identity)
        return fail("ANIMATION_DELTA_INSTANCE_STALE","最新帧的请求表身份与驻留场景不同。");
    phase.Switch(timings ? &timings->cloneNs : nullptr);
    auto candidate=CloneCatalog(*plan.prepared_->state_);
    phase.Switch(timings ? &timings->storageNs : nullptr);
    if (auto status=InitializeStorage(*candidate,candidate->limits,FrozenReader(*candidate),timings); !status.Succeeded())
        return {std::nullopt,std::move(status.diagnostics)};
    phase.Switch(timings ? &timings->stateRemapNs : nullptr);
    AnimationFrame seed; seed.sceneVersion=candidate->scene->sceneVersion; seed.documentRevision=candidate->scene->documentRevision;
    seed.contentVersion=latest.contentVersion; seed.worldView=latest.worldView; seed.worldCamera=latest.worldCamera;
    seed.states=candidate->initial;
    for (std::size_t i=0;i<latest.states.size();++i) if (plan.oldToNewRequests_[i]!=UINT32_MAX)
        seed.states[plan.oldToNewRequests_[i]]=latest.states[i];
    candidate->initial=seed.states;
    AnimationInstanceDeltaResult result; result.oldToNewRequests=plan.oldToNewRequests_; result.assetsChanged=plan.assetsChanged_;
    phase.Switch(timings ? &timings->particleRemapNs : nullptr);
    if (candidate->particles) {
        auto checkpoint=std::make_shared<AnimationParticleCheckpoint>();
        checkpoint->sceneVersion=seed.sceneVersion; checkpoint->contentVersion=seed.contentVersion;
        checkpoint->instances.resize(candidate->particles->bindings.size()); checkpoint->frozen.resize(checkpoint->instances.size(),1);
        if (state_->particles) {
            if (!latest.particleCheckpoint || latest.particleCheckpoint->sceneVersion!=latest.sceneVersion ||
                latest.particleCheckpoint->contentVersion!=latest.contentVersion ||
                latest.particleCheckpoint->instances.size()!=state_->particles->bindings.size() ||
                latest.particleCheckpoint->frozen.size()!=state_->particles->bindings.size())
                return fail("ANIMATION_PARTICLE_CHECKPOINT_STALE","最新粒子检查点与接受帧不一致。");
            std::map<std::uint32_t,std::uint32_t> bindings;
            for (std::uint32_t b=0;b<candidate->particles->bindings.size();++b) bindings.emplace(candidate->particles->bindings[b].requestIndex,b);
            result.oldToNewParticleBindings.assign(state_->particles->bindings.size(),UINT32_MAX);
            for (std::uint32_t b=0;b<state_->particles->bindings.size();++b) {
                const auto& previous=state_->particles->bindings[b]; const auto r=plan.oldToNewRequests_[previous.requestIndex];
                const auto found=bindings.find(r); if (r==UINT32_MAX || found==bindings.end()) continue;
                const auto next=found->second;
                if (candidate->assets[candidate->particles->bindings[next].assetIndex].compiled.Identity()!=state_->assets[previous.assetIndex].compiled.Identity())
                    return fail("ANIMATION_DELTA_PARTICLE_ASSET_STALE","存活粒子绑定的资产身份改变，未把旧RNG状态交给新资产。");
                result.oldToNewParticleBindings[b]=next; checkpoint->instances[next]=latest.particleCheckpoint->instances[b];
                checkpoint->frozen[next]=latest.particleCheckpoint->frozen[b];
                if (checkpoint->instances[next]) checkpoint->stateBytes+=checkpoint->instances[next]->storageBytes;
            }
        }
        seed.particleCheckpoint=std::move(checkpoint);
    }
    phase.Switch(timings ? &timings->firstFrameNs : nullptr);
    result.service=std::shared_ptr<ModelAnimationService>(new ModelAnimationService(std::move(candidate)));
    auto combined=advance; combined.edits=plan.delta_.edits; MergeEdits(combined.edits,advance.edits);
    auto first=result.service->Prepare(&seed,combined);
    if (!first.Succeeded()) return {std::nullopt,std::move(first.diagnostics)};
    result.frame=std::move(*first.value);
    phase.Switch(timings ? &timings->renderedRemapNs : nullptr);
    const auto requestCount=result.service->state_->scene->specs.size();
    const auto mappingBytes=(std::uint64_t(requestCount)+latest.poses.size())*sizeof(std::uint32_t)+std::uint64_t(old.specs.size());
    if (requestCount>=UINT32_MAX || latest.poses.size()>=UINT32_MAX || result.frame->poses.size()>=UINT32_MAX ||
        mappingBytes>state_->limits.maxFrameBytes)
        return fail("ANIMATION_DELTA_BUDGET","实例渲染重映射超过内存或索引范围，未截断请求。");
    result.oldToNewRendered.assign(latest.poses.size(),UINT32_MAX);
    std::vector<std::uint32_t> rendered(requestCount,UINT32_MAX);
    std::vector<std::uint8_t> previousRendered(old.specs.size(),0);
    for (std::uint32_t i=0;i<result.frame->poses.size();++i) {
        const auto request=result.frame->poses[i].requestIndex;
        if (request==kBakedInstance) continue;
        if (request>=rendered.size() || rendered[request]!=UINT32_MAX)
            return fail("ANIMATION_DELTA_RENDER_MAPPING_INVALID","新渲染帧含越界或重复请求，未发布部分映射。");
        rendered[request]=i;
    }
    for (std::size_t i=0;i<latest.poses.size();++i) {
        const auto r=latest.poses[i].requestIndex;
        if (r==kBakedInstance) { if (i<result.frame->poses.size() && result.frame->poses[i].requestIndex==kBakedInstance) result.oldToNewRendered[i]=static_cast<std::uint32_t>(i); continue; }
        if (r>=previousRendered.size() || previousRendered[r] || latest.poses[i].identity!=old.specs[r].identity)
            return fail("ANIMATION_DELTA_RENDER_MAPPING_INVALID","旧渲染帧含越界、重复或错误身份请求，未发布部分映射。");
        previousRendered[r]=1;
        const auto next=plan.oldToNewRequests_[r];
        if (next==UINT32_MAX) continue;
        if (next>=rendered.size())
            return fail("ANIMATION_DELTA_RENDER_MAPPING_INVALID","存活请求不属于候选渲染目录。");
        if (rendered[next]!=UINT32_MAX) result.oldToNewRendered[i]=rendered[next];
    }
    return Out::Success(std::move(result));
} catch (const std::bad_alloc&) {
    return Result<AnimationInstanceDeltaResult>::Failure("Render.Animation","ANIMATION_DELTA_ALLOCATION_FAILED","增量重基内存分配失败，已接受帧未改动。");
} catch (const std::exception& e) {
    return Result<AnimationInstanceDeltaResult>::Failure("Render.Animation","ANIMATION_DELTA_PREPARE_FAILED",e.what());
}
}
