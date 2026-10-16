#include "AnimationInternal.h"
#include "AnimationParticles.h"
#include "AnimationPrem.h"
#include "AnimationPoseWarnings.h"
#include "AnimationVisibility.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include <stdexcept>

namespace War3Map::Render {
using namespace War3RenderCore;
namespace {
using TimingClock = std::chrono::steady_clock;
std::uint64_t ElapsedNs(TimingClock::time_point start) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(TimingClock::now()-start).count());
}
void AddMotionHash(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes=static_cast<const unsigned char*>(data);
    for (std::size_t i=0;i<size;++i) { hash^=bytes[i]; hash*=1099511628211ull; }
}
}
War3RenderSkinUpdateV2 AnimationFrame::Describe() const noexcept {
    return {sizeof(War3RenderSkinUpdateV2),static_cast<std::uint32_t>(instances.size()),
        static_cast<std::uint32_t>(groups.size()),static_cast<std::uint32_t>(alphas.size()),sceneVersion,contentVersion,
        sharedPoseRanges ? WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4_SHARED_RANGES : WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4,
        sizeof(War3RenderGroupMatrixV2),0,0,instances.data(),groups.data(),alphas.data()};
}
War3RenderDynamicColoredSkinUpdateV2 AnimationFrame::DescribeColored() const noexcept {
    War3RenderDynamicColoredSkinUpdateV2 result{}; result.base=Describe(); result.base.structSize=sizeof(result);
    result.colorFormat=WAR3_RENDER_DYNAMIC_MESH_RGB_V1;
    result.colorCount=static_cast<std::uint32_t>(colors.size()); result.colors=colors.data(); return result;
}
War3RenderMaterialSkinUpdateV2 AnimationFrame::DescribeMaterial() const noexcept {
    War3RenderMaterialSkinUpdateV2 result{}; result.base.base=Describe(); result.base.base.structSize=sizeof(result);
    if (dynamicColors) {
        result.base.colorFormat=WAR3_RENDER_DYNAMIC_MESH_RGB_V1;
        result.base.colorCount=static_cast<std::uint32_t>(colors.size()); result.base.colors=colors.data();
    }
    result.layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
    result.layerAlphaCount=static_cast<std::uint32_t>(layerAlphas.size());
    result.instanceSliceCount=static_cast<std::uint32_t>(layerSlices.size());
    result.layerAlphas=layerAlphas.empty() ? nullptr : layerAlphas.data();
    result.instanceSlices=layerSlices.empty() ? nullptr : layerSlices.data(); return result;
}
War3RenderMaterialStateSkinUpdateV2 AnimationFrame::DescribeMaterialState() const noexcept {
    War3RenderMaterialStateSkinUpdateV2 result{}; result.base=DescribeMaterial(); result.base.base.base.structSize=sizeof(result);
    result.materialStateFormat=WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
    result.materialStateCount=static_cast<std::uint32_t>(materialStates.size());
    result.materialInstanceSliceCount=static_cast<std::uint32_t>(materialStateSlices.size());
    result.materialStates=materialStates.empty() ? nullptr : materialStates.data();
    result.materialInstanceSlices=materialStateSlices.empty() ? nullptr : materialStateSlices.data(); return result;
}
War3RenderParticleUpdateV2 AnimationFrame::DescribeParticles(const War3RenderSkinUpdateV2* modelPose) const noexcept {
    return {sizeof(War3RenderParticleUpdateV2), static_cast<std::uint32_t>(particleVertices.size()),
        static_cast<std::uint32_t>(particleDraws.size()), 0, sceneVersion, contentVersion,
        particleVertices.data(), particleDraws.data(), modelPose};
}
War3RenderParticleMaterialUpdateV2 AnimationFrame::DescribeParticleMaterial(const War3RenderSkinUpdateV2* modelPose) const noexcept {
    War3RenderParticleMaterialUpdateV2 result{};
    result.base = DescribeParticles(modelPose); result.base.structSize = sizeof(result);
    result.materialFlagsFormat = WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1;
    result.materialFlagsCount = static_cast<std::uint32_t>(particleMaterialFlags.size());
    result.materialFlags = particleMaterialFlags.empty() ? nullptr : particleMaterialFlags.data();
    return result;
}
void AnimatedSceneDescriptor::Refresh(const AnimatedModelScene& data, const AnimationFrame& frame) {
    textures=DescribeModelScene(data.geometry); update=frame.Describe();
    scene={sizeof(scene),static_cast<std::uint32_t>(data.vertices.size()),static_cast<std::uint32_t>(data.geometry.indices.size()),
        static_cast<std::uint32_t>(data.geometry.layers.size()),static_cast<std::uint32_t>(data.meshes.size()),
        static_cast<std::uint32_t>(data.models.size()),static_cast<std::uint32_t>(textures.chains.size()),
        data.vertices.data(),data.geometry.indices.data(),data.geometry.layers.data(),data.meshes.data(),data.models.data(),
        textures.chains.data(),&update};
    if (data.dynamicColors) { coloredUpdate=frame.DescribeColored(); scene.initialPose=&coloredUpdate.base; }
    if (data.dynamicMaterialAlphas) { materialUpdate=frame.DescribeMaterial(); scene.initialPose=&materialUpdate.base.base; }
    if (data.dynamicMaterialStates) { materialStateUpdate=frame.DescribeMaterialState(); scene.initialPose=&materialStateUpdate.base.base.base; }
}
std::shared_ptr<WorldModelScene> AnimationPickingScene(const AnimatedModelScene& scene, const AnimationFrame& frame) {
    auto result=std::make_shared<WorldModelScene>();
    result->models=scene.geometry.models; result->modelBounds=scene.geometry.modelBounds;
    AnimationPickingInstances(scene,frame,*result);
    return result;
}
void AnimationPickingInstances(const AnimatedModelScene& scene, const AnimationFrame& frame, WorldModelScene& output) {
    output.instances.clear(); output.instanceRequests.clear();
    std::size_t noGeometry=0;
    for (std::size_t i=0;i<frame.instances.size();++i) if (!frame.poses.at(i).hidden && !frame.poses.at(i).outsideView) {
        const auto& pose=frame.poses[i];
        if (pose.noVisibleGeometry) {
            const auto particle=std::lower_bound(frame.particleLinks.begin(),frame.particleLinks.end(),pose.requestIndex,
                [](const auto& link,auto request) { return link.requestIndex<request; });
            const bool drawable=particle!=frame.particleLinks.end() && particle->requestIndex==pose.requestIndex &&
                particle->identity==pose.identity && particle->simulated && !particle->hidden && particle->vertexCount && particle->drawCount;
            if (!drawable) { ++noGeometry; continue; }
        }
        output.instances.push_back(frame.instances[i].instance);
        output.instanceRequests.push_back(frame.poses[i].requestIndex);
    }
    output.requestedInstances=scene.geometry.requestedInstances;
    output.skippedInstances=scene.geometry.skippedInstances+frame.hiddenInstances+noGeometry+
        static_cast<std::size_t>(std::count_if(frame.poses.begin(),frame.poses.end(),[](const auto& pose) { return pose.outsideView; }));
}
Status ModelAnimationService::PrepareStandStorage(State& s, std::uint64_t frameBudget, std::uint64_t activeBudget) {
    const auto& scene=*s.scene; const auto& geometry=scene.geometry;
    const auto fail=[](const char* code,const char* text) { return AnimationFailure(code,text); };
    auto stand=std::make_shared<AnimationFrame>();
    stand->sceneVersion=scene.sceneVersion; stand->documentRevision=scene.documentRevision;
    stand->dynamicMaterialAlphas=scene.dynamicMaterialAlphas; stand->dynamicColors=scene.dynamicColors;
    stand->dynamicMaterialStates=scene.dynamicMaterialStates;
    const auto mappingBytes=(geometry.instances.size()+scene.models.size())*sizeof(std::uint32_t);
    if (mappingBytes>frameBudget-s.frameBytes)
        return fail("ANIMATION_FRAME_BUDGET","Stand变体的实例映射超过帧预算；未截断地图实例。");
    s.frameBytes+=mappingBytes;
    s.modelSlots.resize(scene.models.size()); s.modelStandPoses.assign(scene.models.size(),UINT32_MAX);
    s.standPoseIndices.resize(geometry.instances.size());
    std::vector<std::uint32_t> counts(scene.models.size()),representatives,poseModels,poseSequences;
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> variants;
    for (std::uint32_t i=0;i<geometry.instances.size();++i) {
        const auto model=geometry.instances[i].model,request=geometry.instanceRequests[i];
        std::uint32_t sequence=UINT32_MAX;
        if (request!=kBakedInstance) {
            ++counts[model];
            const auto& spec=scene.specs[request];
            sequence=spec.action=="Stand" && !spec.sequenceIndex ? s.initial[request].sequenceIndex :
                FindAnimationSequence(scene.sequences[request],"Stand",spec.model.animationProperties).value_or(0);
        }
        const auto [found,inserted]=variants.try_emplace(std::make_pair(model,sequence),static_cast<std::uint32_t>(representatives.size()));
        if (inserted) {
            const auto metadataBytes=variants.size()*(3*sizeof(AnimationPoseLink)+2*sizeof(AnimationPickBounds)+3*sizeof(std::uint32_t));
            if (metadataBytes>(frameBudget-s.frameBytes)/2)
                return fail("ANIMATION_FRAME_BUDGET","实际Stand变体缓存元数据超过帧预算；未混用其他形态。");
            representatives.push_back(request); poseModels.push_back(model); poseSequences.push_back(sequence);
            if (s.modelStandPoses[model]==UINT32_MAX) s.modelStandPoses[model]=found->second;
        }
        s.standPoseIndices[i]=found->second;
    }
    s.standPoses.resize(representatives.size());
    for (std::uint32_t p=0;p<s.standPoses.size();++p) {
        const auto m=poseModels[p]; auto& link=s.standPoses[p]; const auto& model=scene.models[m];
        link.standPoseIndex=p;
        const auto assetIndex=s.modelAssets[m]; const auto* asset=assetIndex==UINT32_MAX ? nullptr : &s.assets[assetIndex];
        link.firstGroup=static_cast<std::uint32_t>(stand->groups.size()); link.groupCount=model.groupCount;
        link.firstAlpha=static_cast<std::uint32_t>(stand->alphas.size()); link.alphaCount=model.model.meshCount;
        if (scene.dynamicMaterialAlphas)
            link.materialAlphas={static_cast<std::uint32_t>(stand->layerAlphas.size()),scene.modelLayerRanges[m].count};
        if (scene.dynamicMaterialStates) link.materialStates=link.materialAlphas;
        const auto shapeCount=asset && asset->parsed ? asset->parsed->collisionShapes.size() : 0;
        if (link.groupCount>1048576-stand->groups.size() || link.alphaCount>1048576-stand->alphas.size() ||
            shapeCount>1048576-stand->collisionMatrices.size() || link.materialAlphas.count>1048576-stand->layerAlphas.size())
            return fail("ANIMATION_FRAME_BUDGET","共享 Stand 资产片段超出一次姿势池容量；未截断实例，保留静态场景。");
        link.collisionMatrices={static_cast<std::uint32_t>(stand->collisionMatrices.size()),static_cast<std::uint32_t>(shapeCount)};
        stand->collisionMatrices.resize(stand->collisionMatrices.size()+shapeCount);
        stand->groups.resize(stand->groups.size()+link.groupCount);
        stand->alphas.resize(stand->alphas.size()+link.alphaCount,1.0f);
        stand->colors.resize(stand->colors.size()+link.alphaCount,War3RenderMeshColorV2{{1,1,1}});
        stand->layerAlphas.resize(stand->layerAlphas.size()+link.materialAlphas.count);
        if (scene.dynamicMaterialStates) {
            stand->materialStates.resize(stand->layerAlphas.size());
            if (auto status=FillAnimationMaterialStates(scene,m,link.materialStates,stand->materialStates); !status.Succeeded()) return status;
        }
        if (auto status=FillAnimationLayerAlphas(scene,m,link.materialAlphas,stand->layerAlphas); !status.Succeeded()) return status;
        for (std::uint32_t k=0;k<link.groupCount;++k) stand->groups[link.firstGroup+k]={{1,0,0,0,1,0,0,0,1,0,0,0}};
        for (std::uint32_t k=0;k<link.alphaCount;++k)
            if (!scene.meshColors.empty()) stand->colors[link.firstAlpha+k]=scene.meshColors[model.model.firstMesh+k];
        link.evaluationEpoch=s.epoch; link.poseVersion=++s.staticPoseVersion;
        if (asset) link.asset={static_cast<std::uint64_t>(assetIndex)+1,scene.generation};
        if (asset && asset->compiled.Valid() && representatives[p]!=kBakedInstance) {
            const auto requestIndex=representatives[p];
            const auto sequence=poseSequences[p];
            const auto& selected=scene.sequences[requestIndex][sequence];
            PoseRequest request; request.instance=scene.specs[requestIndex].identity; request.asset=asset->compiled.Identity();
            request.assetIndex=asset->evaluatorIndex; request.sequenceIndex=sequence;
            request.sequenceElapsedMs=selected.end>=selected.start ? (std::uint64_t(selected.end)-selected.start)/2 : 0;
            link.staticSequenceIndex=sequence; link.staticElapsedMs=request.sequenceElapsedMs;
            const auto outcome=s.evaluator->Evaluate(std::span(&request,1));
            if (outcome.error!=Code::Ok) return AnimationFailure(CodeName(outcome.error),CodeMessage(outcome.error));
            const auto& receipt=s.evaluator->Receipts()[0]; link.asset=receipt.asset;
            link.evaluationEpoch=receipt.evaluationEpoch; link.poseVersion=receipt.poseVersion;
            link.requestIndex=requestIndex; link.identity=receipt.instance;
            if (receipt.error!=Code::Ok) {
                if (!AnimationLocalDataFailure(receipt.error)) return AnimationFailure(CodeName(receipt.error),CodeMessage(receipt.error));
                link.hidden=true; link.poseVersion=0; link.materialStateHash=0;
                const auto diagnostics=s.evaluator->Diagnostics(0);
                link.rejection=FirstAnimationPoseError(diagnostics,receipt.error);
                std::fill_n(stand->alphas.begin()+link.firstAlpha,link.alphaCount,0.0f);
                if (scene.dynamicMaterialAlphas) {
                    std::fill_n(stand->layerAlphas.begin()+link.materialAlphas.offset,link.materialAlphas.count,0.0f);
                    const auto issues=s.evaluator->MaterialIssues(0);
                    link.materialIssues={static_cast<std::uint32_t>(stand->materialIssues.size()),static_cast<std::uint32_t>(issues.size())};
                    for (const auto& issue:issues) stand->materialIssues.push_back({issue});
                    if (scene.dynamicMaterialStates) {
                        const auto textureIssues=s.evaluator->TextureIssues(0);
                        link.materialIssues.count+=static_cast<std::uint32_t>(textureIssues.size());
                        for (const auto& issue:textureIssues) stand->materialIssues.push_back({issue});
                    }
                }
            } else {
                if (auto status=CaptureAnimationPoseWarnings(link,receipt,request,s.evaluator->Diagnostics(0),true,
                    s.evaluator->MaterialAlphaPreviewWarnings(0));
                    !status.Succeeded()) return status;
                const auto groups=s.evaluator->Groups(0); const auto alphas=s.evaluator->Alphas(0); const auto colors=s.evaluator->Colors(0);
                if (groups.size()!=link.groupCount) return fail("ANIMATION_STATIC_MAPPING","共享 Stand 实际矩阵组数量不一致。");
                for (std::uint32_t k=0;k<link.groupCount;++k)
                    std::copy(groups[k].values.begin(),groups[k].values.end(),stand->groups[link.firstGroup+k].values);
                for (std::uint32_t k=0;k<link.alphaCount;++k) {
                    const auto geoset=geometry.meshGeosets[model.model.firstMesh+k];
                    if (geoset>=alphas.size() || geoset>=colors.size()) return fail("ANIMATION_STATIC_MAPPING","共享 Stand GEOA 来源越界。");
                    stand->alphas[link.firstAlpha+k]=alphas[geoset];
                    std::copy(colors[geoset].begin(),colors[geoset].end(),stand->colors[link.firstAlpha+k].rgb);
                }
                if (scene.dynamicMaterialAlphas) {
                    if (s.evaluator->LayerAlphas(0).size()!=asset->compiled.MaterialLayerCount())
                        return fail("ANIMATION_LAYER_ALPHA_MAPPING","共享 Stand 材质输出与编译布局不同。");
                    if (auto status=FillAnimationLayerAlphas(scene,m,link.materialAlphas,stand->layerAlphas,s.evaluator->LayerAlphas(0)); !status.Succeeded()) return status;
                }
                const auto issues=s.evaluator->GeosetIssues(0);
                if (scene.dynamicMaterialStates) {
                    if (s.evaluator->LayerTextureIds(0).size()!=asset->compiled.MaterialLayerCount())
                        return fail("ANIMATION_TEXTURE_MAPPING","共享Stand贴图输出与编译布局不同。");
                    if (auto status=FillAnimationMaterialStates(scene,m,link.materialStates,stand->materialStates,s.evaluator->LayerTextureIds(0),
                        s.evaluator->LayerUvTransforms(0)); !status.Succeeded()) return status;
                }
                if (issues.size()>1048576-stand->geosetIssues.size()) return fail("ANIMATION_FRAME_BUDGET","共享 Stand GEOA 诊断超过 owned 预算。");
                link.geosetIssues={static_cast<std::uint32_t>(stand->geosetIssues.size()),static_cast<std::uint32_t>(issues.size())};
                link.partial=!issues.empty();
                for (const auto& issue:issues) stand->geosetIssues.push_back({issue.geosetIndex,issue.diagnostic});
                if (asset->collisionNodes) {
                    const auto nodes=s.evaluator->Nodes(0);
                    constexpr std::array<unsigned,12> columns{0,1,2,4,5,6,8,9,10,12,13,14};
                    for (std::uint32_t k=0;k<link.collisionMatrices.count;++k) {
                        const auto node=(*asset->collisionNodes)[k]; if (node==NoIndex) continue;
                        if (node>=nodes.size()) return fail("ANIMATION_STATIC_MAPPING","共享 Stand 没有实际单节点矩阵。");
                        auto& output=stand->collisionMatrices[link.collisionMatrices.offset+k];
                        for (std::size_t column=0;column<columns.size();++column) output.matrix.values[column]=nodes[node].worldMatrix[columns[column]];
                        output.available=true;
                    }
                }
            }
        }
        link.noVisibleGeometry=!AnimationHasVisibleLayers(scene,*stand,link,m);
        if (!link.hidden) {
            link.matrixHash=AnimationBitsHash(stand->groups.data()+link.firstGroup,link.groupCount*sizeof(War3RenderGroupMatrixV2));
            link.alphaHash=AnimationBitsHash(stand->alphas.data()+link.firstAlpha,link.alphaCount*sizeof(float));
            link.colorHash=AnimationBitsHash(stand->colors.data()+link.firstAlpha,link.alphaCount*sizeof(War3RenderMeshColorV2));
            if (scene.dynamicMaterialAlphas) link.materialAlphaHash=AnimationBitsHash(stand->layerAlphas.data()+link.materialAlphas.offset,link.materialAlphas.count*sizeof(float));
            if (scene.dynamicMaterialStates) link.materialStateHash=AnimationBitsHash(stand->materialStates.data()+link.materialStates.offset,link.materialStates.count*sizeof(War3RenderMaterialStateV2));
        }
        auto instance=War3RenderSkinnedModelInstanceV2{}; instance.instance.model=m;
        instance.instance.transform[0]=instance.instance.transform[5]=instance.instance.transform[10]=1;
        instance.firstGroup=link.firstGroup; instance.firstAlpha=link.firstAlpha;
        stand->instances.push_back(instance); stand->poses.push_back(link); stand->poses.back().requestIndex=representatives[p];
        if (scene.dynamicMaterialAlphas) stand->layerSlices.push_back({link.materialAlphas.offset,link.materialAlphas.count});
        if (scene.dynamicMaterialStates) stand->materialStateSlices.push_back({link.materialStates.offset,link.materialStates.count});
    }
    if (auto status=PrepareAnimationPickBounds(scene,*stand); !status.Succeeded()) return status;
    s.scene->standPickBounds=stand->pickBounds;
    const auto standBytes=stand->groups.size()*sizeof(War3RenderGroupMatrixV2)+stand->alphas.size()*(sizeof(float)+sizeof(War3RenderMeshColorV2))+
        stand->collisionMatrices.size()*sizeof(AnimationCollisionMatrix)+stand->geosetIssues.size()*sizeof(AnimationGeosetIssue)+
        stand->layerAlphas.size()*sizeof(float)+stand->layerSlices.size()*sizeof(War3RenderMaterialLayerSliceV2)+stand->materialIssues.size()*sizeof(AnimationMaterialIssue)+
        stand->materialStates.size()*sizeof(War3RenderMaterialStateV2)+stand->materialStateSlices.size()*sizeof(War3RenderMaterialLayerSliceV2)+
        s.standPoses.size()*(3*sizeof(AnimationPoseLink)+2*sizeof(AnimationPickBounds)+3*sizeof(std::uint32_t))+
        scene.models.size()*sizeof(std::vector<std::uint32_t>);
    if (standBytes>(frameBudget-s.frameBytes)/2) return fail("ANIMATION_FRAME_BUDGET","全量身份与共享 Stand 真实内存超过预算；未截断实例。");
    s.frameBytes+=2*standBytes; // 缓存与候选 owned 副本一起核算。
    s.sharedGroupCount=static_cast<std::uint32_t>(stand->groups.size());
    s.sharedAlphaCount=static_cast<std::uint32_t>(stand->alphas.size());
    s.sharedLayerAlphaCount=static_cast<std::uint32_t>(stand->layerAlphas.size());
    s.maxCollisionMatrices=stand->collisionMatrices.size(); s.maxGeosetIssues=stand->geosetIssues.size();
    s.maxMaterialIssues=stand->materialIssues.size();
    std::deque<std::uint32_t> ready;
    for (std::uint32_t m=0;m<scene.models.size();++m) {
        const auto asset=s.modelAssets[m];
        if (counts[m] && asset!=UINT32_MAX && s.assets[asset].compiled.Valid()) ready.push_back(m);
    }
    std::uint64_t activeBytes=0;
    while (!ready.empty()) {
        const auto m=ready.front(); ready.pop_front(); const auto& base=s.standPoses[s.modelStandPoses[m]];
        const auto asset=s.modelAssets[m]; const auto geosets=s.assets[asset].compiled.GeosetCount();
        const auto bytes=std::uint64_t(base.groupCount)*sizeof(War3RenderGroupMatrixV2)+
            std::uint64_t(base.alphaCount)*(sizeof(float)+sizeof(War3RenderMeshColorV2))+
            std::uint64_t(base.collisionMatrices.count)*sizeof(AnimationCollisionMatrix)+std::uint64_t(geosets)*sizeof(AnimationGeosetIssue)+
            std::uint64_t(base.materialAlphas.count)*sizeof(float)+(scene.dynamicMaterialAlphas ? sizeof(AnimationMaterialIssue) : 0)+
            (scene.dynamicMaterialStates ? std::uint64_t(base.materialAlphas.count)*sizeof(War3RenderMaterialStateV2)+sizeof(AnimationMaterialIssue) : 0)+
            2*sizeof(AnimationPoseLink)+sizeof(std::uint32_t)+2*sizeof(std::pair<double,std::uint32_t>);
        if (bytes>activeBudget-activeBytes || bytes>frameBudget-s.frameBytes ||
            base.groupCount>1048576-s.sharedGroupCount || base.alphaCount>1048576-s.sharedAlphaCount ||
            base.collisionMatrices.count>1048576-s.maxCollisionMatrices || geosets>1048576-s.maxGeosetIssues) continue;
        if (base.materialAlphas.count>1048576-s.sharedLayerAlphaCount) continue;
        auto slot=base; slot.firstGroup=s.sharedGroupCount; slot.firstAlpha=s.sharedAlphaCount;
        slot.materialAlphas.offset=s.sharedLayerAlphaCount; slot.materialIssues={};
        if (scene.dynamicMaterialStates) slot.materialStates=slot.materialAlphas;
        slot.collisionMatrices.offset=static_cast<std::uint32_t>(s.maxCollisionMatrices);
        slot.geosetIssues={}; slot.staticForBudget=false; slot.hidden=false; slot.partial=false; slot.warnings={};
        s.modelSlots[m].push_back(static_cast<std::uint32_t>(s.activeSlots.size())); s.activeSlots.push_back(slot);
        s.sharedGroupCount+=base.groupCount; s.sharedAlphaCount+=base.alphaCount;
        s.sharedLayerAlphaCount+=base.materialAlphas.count;
        s.maxCollisionMatrices+=base.collisionMatrices.count; s.maxGeosetIssues+=geosets;
        if (scene.dynamicMaterialAlphas) ++s.maxMaterialIssues;
        if (scene.dynamicMaterialStates) ++s.maxMaterialIssues;
        activeBytes+=bytes; s.frameBytes+=bytes;
        if (s.modelSlots[m].size()<counts[m]) ready.push_back(m);
    }
    std::size_t eligible=0;
    for (std::uint32_t m=0;m<scene.models.size();++m) {
        const auto asset=s.modelAssets[m];
        if (asset!=UINT32_MAX && s.assets[asset].compiled.Valid()) eligible+=counts[m];
    }
    s.scene->budgetStaticInstances=eligible-s.activeSlots.size();
    s.standFrame=std::move(stand); return Status::Ok();
}
AnimationPlaybackPolicy ModelAnimationService::PlaybackPolicy() const noexcept { return state_->limits.playbackPolicy; }
Result<std::shared_ptr<const AnimationFrame>> ModelAnimationService::Prepare(const AnimationFrame* previous,
    const AnimationAdvance& advance) try {
    const auto prepareStarted=TimingClock::now();
    using Output=Result<std::shared_ptr<const AnimationFrame>>;
    const auto fail=[](const char* code,const char* text) { return Output::Failure("Render.Animation",code,text); };
    auto& s=*state_; const auto& scene=*s.scene; const auto& g=scene.geometry;
    if (advance.previewParticleBudget && (s.limits.playbackPolicy != AnimationPlaybackPolicy::InteractivePreview ||
        !advance.previewParticleBudget->maxAnchorSamples ||
        advance.previewParticleBudget->maxAnchorSamples > kMaximumPreviewAnchorSamples))
        return fail("ANIMATION_PREVIEW_BUDGET_INVALID", "单帧特效预算只适用于交互预览，且必须在锚样硬上限内。");
    if (previous && (previous->sceneVersion!=scene.sceneVersion || previous->documentRevision!=scene.documentRevision))
        return fail("ANIMATION_FRAME_STALE","旧帧不属于当前模型场景或地图修订。");
    auto frame=std::make_shared<AnimationFrame>();
    frame->sceneVersion=scene.sceneVersion; frame->documentRevision=scene.documentRevision;
    frame->previewParticleBudget=advance.previewParticleBudget;
    frame->timings.candidateInstances=g.instances.size();
    const auto stateStarted=TimingClock::now();
    frame->states=previous ? previous->states : s.initial;
    frame->worldCamera=advance.worldCamera ? advance.worldCamera : previous ? previous->worldCamera : std::nullopt;
    frame->worldView=advance.worldView ? advance.worldView : previous ? previous->worldView : std::nullopt;
    if (frame->worldView) frame->worldCamera=AnimationWorldCamera(frame->worldView->camera);
    if (auto check=AdvanceAnimationStates(frame->states,scene,advance); !check.Succeeded())
        return {std::nullopt,std::move(check.diagnostics)};
    frame->timings.stateAdvanceNs=ElapsedNs(stateStarted);
    const auto visibilityStarted=TimingClock::now();
    if (auto visibility=PrepareAnimationVisibility(s.visibility,scene,s.assets,s.requestAssets,*frame,advance.edits,s.maxVisibilityBytes); !visibility.Succeeded())
        return {std::nullopt,std::move(visibility.diagnostics)};
    frame->timings.visibilityNs=frame->worldView ? ElapsedNs(visibilityStarted) : 0;
    const auto poseStarted=TimingClock::now();
    frame->sharedPoseRanges=s.sharedPoseRanges;
    frame->dynamicMaterialAlphas=scene.dynamicMaterialAlphas; frame->dynamicColors=scene.dynamicColors;
    frame->dynamicMaterialStates=scene.dynamicMaterialStates;
    frame->instances.resize(g.instances.size()); frame->poses.resize(g.instances.size());
    if (scene.dynamicMaterialAlphas) frame->layerSlices.resize(g.instances.size());
    if (scene.dynamicMaterialStates) frame->materialStateSlices.resize(g.instances.size());
    frame->geosetIssues.reserve(static_cast<std::size_t>(s.maxGeosetIssues));
    frame->materialIssues.reserve(static_cast<std::size_t>(s.maxMaterialIssues));
    frame->collisionMatrices.reserve(static_cast<std::size_t>(s.maxCollisionMatrices));
    std::vector<std::uint32_t> slots;
    if (s.sharedPoseRanges) {
        // 每模型容量由真实字节分配。维护至多该模型容量的近处候选，不排序或复制整张地图。
        using Candidate=std::pair<double,std::uint32_t>;
        const auto less=[&](const Candidate& left,const Candidate& right) {
            if (left.first!=right.first) return left.first<right.first;
            if (frame->worldView) {
                const auto a=frame->states[g.instanceRequests[left.second]].identity.value;
                const auto b=frame->states[g.instanceRequests[right.second]].identity.value;
                if (a!=b) return a<b;
            }
            return left.second<right.second;
        };
        std::vector<std::vector<Candidate>> nearest(scene.models.size()); slots.assign(g.instances.size(),UINT32_MAX);
        for (std::uint32_t i=0;i<g.instances.size();++i) {
            const auto model=g.instances[i].model,request=g.instanceRequests[i]; const auto capacity=s.modelSlots[model].size();
            if (request==kBakedInstance || !capacity) continue;
            if (!frame->requestVisibility.empty() && !frame->requestVisibility[request]) continue;
            const auto& transform=frame->states[request].transform;
            double distance=0;
            if (frame->worldCamera) for (unsigned axis=0;axis<3;++axis) {
                const auto delta=double(transform[axis*4+3])-frame->worldCamera->eye[axis]; distance+=delta*delta;
            }
            // worldView 使用256世界单位粗距离桶，同桶按稳定实例身份排序；微小相机移动不抢换槽。
            const auto priority=frame->worldView ? std::floor(std::sqrt(distance)/256.0) : distance;
            auto& list=nearest[model]; const Candidate candidate{priority,i};
            if (list.size()<capacity) { list.push_back(candidate); std::push_heap(list.begin(),list.end(),less); }
            else if (less(candidate,list.front())) {
                std::pop_heap(list.begin(),list.end(),less); list.back()=candidate; std::push_heap(list.begin(),list.end(),less);
            }
        }
        for (std::uint32_t m=0;m<nearest.size();++m) {
            auto& list=nearest[m]; std::sort(list.begin(),list.end(),less);
            for (std::size_t k=0;k<list.size();++k) slots[list[k].second]=s.modelSlots[m][k];
        }
        frame->groups=s.standFrame->groups; frame->alphas=s.standFrame->alphas; frame->colors=s.standFrame->colors;
        frame->layerAlphas=s.standFrame->layerAlphas; frame->materialIssues=s.standFrame->materialIssues;
        frame->materialStates=s.standFrame->materialStates;
        frame->collisionMatrices=s.standFrame->collisionMatrices; frame->geosetIssues=s.standFrame->geosetIssues;
    }
    std::uint32_t groupCount=s.sharedPoseRanges ? s.sharedGroupCount : 0,alphaCount=s.sharedPoseRanges ? s.sharedAlphaCount : 0;
    std::uint32_t layerAlphaCount=s.sharedPoseRanges ? s.sharedLayerAlphaCount : 0;
    for (std::uint32_t i=0;i<g.instances.size();++i) {
        const auto m=g.instances[i].model, request=g.instanceRequests[i];
        const auto candidateAsset=s.modelAssets[m];
        const bool outside=request!=kBakedInstance && !frame->requestVisibility.empty() && !frame->requestVisibility[request];
        if (request!=kBakedInstance && candidateAsset!=UINT32_MAX && s.assets[candidateAsset].compiled.Valid()) {
            if (outside) ++frame->offscreenPoseInstances;
            else ++frame->timings.eligiblePoseInstances;
        }
        auto& link=frame->poses[i];
        if (s.sharedPoseRanges) {
            link=slots[i]==UINT32_MAX ? s.standPoses[s.standPoseIndices[i]] : s.activeSlots[slots[i]];
            link.standPoseIndex=s.standPoseIndices[i];
            const auto assetIndex=s.modelAssets[m];
            link.staticForBudget=!outside && request!=kBakedInstance && slots[i]==UINT32_MAX && assetIndex!=UINT32_MAX && s.assets[assetIndex].compiled.Valid();
        }
        link.outsideView=outside;
        auto& instance=frame->instances[i]; instance={g.instances[i],s.sharedPoseRanges ? link.firstGroup : groupCount,s.sharedPoseRanges ? link.firstAlpha : alphaCount};
        if (request!=kBakedInstance) {
            const auto& current=frame->states[request];
            std::copy(current.transform.begin(),current.transform.end(),instance.instance.transform);
            std::copy(current.tint.begin(),current.tint.end(),instance.instance.color);
        }
        link.requestIndex=request; link.renderedInstance=i;
        const auto& pick=scene.pickModels[m];
        if (pick.source && !s.sharedPoseRanges && !outside) {
            const auto count=pick.source->collisionShapes.size();
            if (count>s.maxCollisionMatrices-frame->collisionMatrices.size())
                return fail("ANIMATION_PICK_COLLISION_BUDGET","碰撞节点 owned 帧超出冻结预算。");
            link.collisionMatrices={static_cast<std::uint32_t>(frame->collisionMatrices.size()),static_cast<std::uint32_t>(count)};
            frame->collisionMatrices.resize(frame->collisionMatrices.size()+count);
        }
        if (!s.sharedPoseRanges) {
            link.firstGroup=groupCount; link.groupCount=scene.models[m].groupCount;
            link.firstAlpha=alphaCount; link.alphaCount=g.models[m].meshCount;
            if (scene.dynamicMaterialAlphas) {
                link.materialAlphas={layerAlphaCount,scene.modelLayerRanges[m].count}; layerAlphaCount+=link.materialAlphas.count;
            }
            groupCount+=link.groupCount; alphaCount+=link.alphaCount;
        }
        if (request!=kBakedInstance) link.identity=frame->states[request].identity;
        if (s.sharedPoseRanges && slots[i]==UINT32_MAX) RebindCachedAnimationPoseWarnings(link);
        else link.warnings={};
        if (scene.dynamicMaterialStates) {
            link.materialStates=link.materialAlphas;
            frame->materialStateSlices[i]={link.materialStates.offset,link.materialStates.count};
        }
        if (scene.dynamicMaterialAlphas) frame->layerSlices[i]={link.materialAlphas.offset,link.materialAlphas.count};
    }
    frame->groups.resize(groupCount); frame->alphas.resize(alphaCount,1.0f); frame->colors.resize(alphaCount,War3RenderMeshColorV2{{1,1,1}});
    frame->layerAlphas.resize(layerAlphaCount);
    if (scene.dynamicMaterialStates) {
        frame->materialStates.resize(layerAlphaCount);
        for (std::uint32_t i=0;i<frame->poses.size();++i) {
            const auto& link=frame->poses[i];
            if (s.sharedPoseRanges && slots[i]==UINT32_MAX) continue;
            if (auto status=FillAnimationMaterialStates(scene,frame->instances[i].instance.model,link.materialStates,frame->materialStates); !status.Succeeded())
                return {std::nullopt,std::move(status.diagnostics)};
            if (!s.sharedPoseRanges && link.outsideView && previous && previous->dynamicMaterialStates && i<previous->poses.size()) {
                const auto& old=previous->poses[i];
                if (old.identity==link.identity && old.requestIndex==link.requestIndex && old.materialStates.count==link.materialStates.count &&
                    old.materialStates.offset<=previous->materialStates.size() && old.materialStates.count<=previous->materialStates.size()-old.materialStates.offset)
                    std::copy_n(previous->materialStates.begin()+old.materialStates.offset,old.materialStates.count,frame->materialStates.begin()+link.materialStates.offset);
            }
        }
    }
    if (scene.dynamicMaterialAlphas) for (std::uint32_t i=0;i<frame->poses.size();++i) {
        const auto& link=frame->poses[i];
        if (s.sharedPoseRanges && slots[i]==UINT32_MAX) continue;
        if (auto status=FillAnimationLayerAlphas(scene,frame->instances[i].instance.model,link.materialAlphas,frame->layerAlphas); !status.Succeeded())
            return {std::nullopt,std::move(status.diagnostics)};
        if (!s.sharedPoseRanges && link.outsideView && previous && previous->dynamicMaterialAlphas && i<previous->poses.size()) {
            const auto& old=previous->poses[i];
            if (old.identity==link.identity && old.requestIndex==link.requestIndex && old.materialAlphas.count==link.materialAlphas.count &&
                old.materialAlphas.offset<=previous->layerAlphas.size() && old.materialAlphas.count<=previous->layerAlphas.size()-old.materialAlphas.offset)
                std::copy_n(previous->layerAlphas.begin()+old.materialAlphas.offset,old.materialAlphas.count,frame->layerAlphas.begin()+link.materialAlphas.offset);
        }
    }
    if (s.sharedPoseRanges) frame->collisionMatrices.resize(static_cast<std::size_t>(s.maxCollisionMatrices));
    const auto identityGroups=[&](const AnimationPoseLink& link) {
        for (std::uint32_t k=0;k<link.groupCount;++k)
            frame->groups[link.firstGroup+k]={{1,0,0,0,1,0,0,0,1,0,0,0}};
    };
    const auto appendDiagnostics=[&](std::span<const PoseDiagnostic> ds) {
        // 逐实例拒绝始终留在 link；共享摘要最多256条，不能靠截断掩盖哪个对象被隐藏。
        const auto n=std::min<std::size_t>(ds.size(),256-frame->diagnostics.size());
        frame->diagnostics.insert(frame->diagnostics.end(),ds.begin(),ds.begin()+static_cast<std::ptrdiff_t>(n));
        frame->omittedDiagnostics+=static_cast<std::uint64_t>(ds.size()-n);
    };
    std::uint32_t active=0;
    const auto flush=[&]() -> Status {
        if (!active) return {};
        frame->timings.poseEvaluationRequests+=active;
        const auto evaluationStarted=TimingClock::now();
        const auto outcome=s.evaluator->Evaluate(std::span(s.requests.data(),active));
        frame->timings.poseEvaluationNs+=ElapsedNs(evaluationStarted);
        if (outcome.error!=Code::Ok) return AnimationFailure(CodeName(outcome.error),CodeMessage(outcome.error));
        for (std::uint32_t k=0;k<active;++k) {
            const auto i=s.renderedIndices[k]; auto& link=frame->poses[i];
            const auto& receipt=s.evaluator->Receipts()[k];
            link.asset=receipt.asset; link.evaluationEpoch=receipt.evaluationEpoch; link.poseVersion=receipt.poseVersion;
            if (receipt.error!=Code::Ok) {
                if (!s.retainUnsupportedStatic || !AnimationLocalDataFailure(receipt.error)) {
                    if (scene.dynamicMaterialStates) {
                        const auto issues=s.evaluator->TextureIssues(k);
                        if (!issues.empty()) {
                            const auto& issue=issues.front(); const auto& spec=scene.specs[link.requestIndex];
                            return Status{{Diagnostic{DiagnosticSeverity::Error,"Render.Animation",CodeName(issue.error),
                                spec.hostKey+" material="+std::to_string(issue.materialIndex)+" layer="+std::to_string(issue.layerIndex)+
                                " sourceFrame="+std::to_string(issue.sourceFrame)+" TEXS/原值="+std::to_string(issue.sourceValueBits)+
                                "："+CodeMessage(issue.error),OriginSpan{spec.model.modelPath,issue.sourceByteOffset,4,{}}}}};
                        }
                    }
                    return AnimationFailure(CodeName(receipt.error),scene.specs[link.requestIndex].hostKey+"："+CodeMessage(receipt.error));
                }
                link.hidden=true; link.poseVersion=0; link.materialStateHash=0;
                const auto ds=s.evaluator->Diagnostics(k);
                link.rejection=FirstAnimationPoseError(ds,receipt.error);
                appendDiagnostics(ds);
                // 固定布局的不透明几何不能删；全部mesh alpha=0，有限矩阵仅作隐藏占位。
                identityGroups(link);
                std::fill_n(frame->alphas.begin()+link.firstAlpha,link.alphaCount,0.0f);
                if (scene.dynamicMaterialAlphas) {
                    std::fill_n(frame->layerAlphas.begin()+link.materialAlphas.offset,link.materialAlphas.count,0.0f);
                    const auto issues=s.evaluator->MaterialIssues(k);
                    if (issues.size()>s.maxMaterialIssues-frame->materialIssues.size())
                        return AnimationFailure("ANIMATION_MATERIAL_ISSUE_BUDGET","材质层原始诊断超过 owned 帧预算。");
                    link.materialIssues={static_cast<std::uint32_t>(frame->materialIssues.size()),static_cast<std::uint32_t>(issues.size())};
                    for (const auto& issue:issues) frame->materialIssues.push_back({issue});
                    if (scene.dynamicMaterialStates) {
                        const auto textureIssues=s.evaluator->TextureIssues(k);
                        if (textureIssues.size()>s.maxMaterialIssues-frame->materialIssues.size())
                            return AnimationFailure("ANIMATION_MATERIAL_ISSUE_BUDGET","KMTF原始字段诊断超过owned预算。");
                        link.materialIssues.count+=static_cast<std::uint32_t>(textureIssues.size());
                        for (const auto& issue:textureIssues) frame->materialIssues.push_back({issue});
                    }
                }
                ++frame->hiddenInstances;
                continue;
            }
            if (auto status=CaptureAnimationPoseWarnings(link,receipt,s.requests[k],s.evaluator->Diagnostics(k),false,
                s.evaluator->MaterialAlphaPreviewWarnings(k));
                !status.Succeeded()) return status;
            link.animated=true;
            ++frame->animatedInstances;
            const auto groups=s.evaluator->Groups(k); const auto alphas=s.evaluator->Alphas(k); const auto colors=s.evaluator->Colors(k);
            const auto& pick=scene.pickModels[frame->instances[i].instance.model];
            if (pick.collisionNodes) {
                const auto nodes=s.evaluator->Nodes(k);
                if (pick.collisionNodes->size()!=link.collisionMatrices.count)
                    return AnimationFailure("ANIMATION_PICK_COLLISION_MAPPING","碰撞 shape 与节点来源数量不一致。");
                constexpr std::array<unsigned,12> columns{0,1,2,4,5,6,8,9,10,12,13,14};
                for (std::uint32_t c=0;c<link.collisionMatrices.count;++c) {
                    const auto node=(*pick.collisionNodes)[c];
                    if (node==NoIndex) continue; // 没有实际节点输出不能拿组矩阵或 identity 代替。
                    if (node>=nodes.size()) return AnimationFailure("ANIMATION_PICK_COLLISION_MAPPING","碰撞节点没有当前完整姿势输出。");
                    auto& output=frame->collisionMatrices[link.collisionMatrices.offset+c];
                    for (std::size_t q=0;q<columns.size();++q) output.matrix.values[q]=nodes[node].worldMatrix[columns[q]];
                    output.available=true;
                }
            }
            const auto issues=s.evaluator->GeosetIssues(k);
            if (issues.size()>s.maxGeosetIssues-frame->geosetIssues.size())
                return AnimationFailure("ANIMATION_GEOSET_ISSUE_BUDGET","当前 GEOA 来源记录超出冻结帧预算。");
            link.geosetIssues={static_cast<std::uint32_t>(frame->geosetIssues.size()),static_cast<std::uint32_t>(issues.size())};
            link.partial=!issues.empty();
            for (const auto& issue:issues) {
                if (issue.geosetIndex>=alphas.size())
                    return AnimationFailure("ANIMATION_GEOSET_ISSUE_MAPPING","局部来源记录没有对应的原 geoset。");
                frame->geosetIssues.push_back({issue.geosetIndex,issue.diagnostic});
            }
            if (groups.size()!=link.groupCount) return AnimationFailure("ANIMATION_GROUP_MAPPING","实例矩阵组映射不一致。");
            for (std::size_t q=0;q<groups.size();++q)
                std::copy(groups[q].values.begin(),groups[q].values.end(),frame->groups[link.firstGroup+q].values);
            const auto& model=g.models[frame->instances[i].instance.model];
            if (scene.dynamicMaterialAlphas) {
                const auto assetIndex=s.modelAssets[frame->instances[i].instance.model];
                if (assetIndex==UINT32_MAX || s.evaluator->LayerAlphas(k).size()!=s.assets[assetIndex].compiled.MaterialLayerCount())
                    return AnimationFailure("ANIMATION_LAYER_ALPHA_MAPPING","材质输出与当前姿势资产布局不同。");
                if (auto status=FillAnimationLayerAlphas(scene,frame->instances[i].instance.model,link.materialAlphas,frame->layerAlphas,
                    s.evaluator->LayerAlphas(k)); !status.Succeeded()) return status;
            }
            if (scene.dynamicMaterialStates) {
                const auto assetIndex=s.modelAssets[frame->instances[i].instance.model];
                if (assetIndex==UINT32_MAX || s.evaluator->LayerTextureIds(k).size()!=s.assets[assetIndex].compiled.MaterialLayerCount())
                    return AnimationFailure("ANIMATION_TEXTURE_MAPPING","贴图输出与当前姿势资产布局不同。");
                if (auto status=FillAnimationMaterialStates(scene,frame->instances[i].instance.model,link.materialStates,frame->materialStates,
                    s.evaluator->LayerTextureIds(k),s.evaluator->LayerUvTransforms(k)); !status.Succeeded()) return status;
            }
            for (std::uint32_t mesh=0;mesh<model.meshCount;++mesh) {
                const auto geoset=g.meshGeosets[model.firstMesh+mesh];
                if (geoset>=alphas.size() || geoset>=colors.size()) return AnimationFailure("ANIMATION_COLOR_MAPPING","颜色/透明度片段缺少geoset。");
                frame->alphas[link.firstAlpha+mesh]=alphas[geoset];
                std::copy(colors[geoset].begin(),colors[geoset].end(),frame->colors[link.firstAlpha+mesh].rgb);
            }
            appendDiagnostics(s.evaluator->Diagnostics(k));
        }
        active=0; return {};
    };
    for (std::uint32_t i=0;i<g.instances.size();++i) {
        auto& link=frame->poses[i]; const auto m=g.instances[i].model, assetIndex=s.modelAssets[m];
        const auto* asset=assetIndex==UINT32_MAX ? nullptr : &s.assets[assetIndex];
        if (link.outsideView) {
            // 身份与时钟保留；离屏只保留安全片段，重入直接求当前时间，不重放历史。
            if (!s.sharedPoseRanges) identityGroups(link);
            if (asset) link.asset={static_cast<std::uint64_t>(assetIndex)+1,scene.generation};
            continue;
        }
        if (s.sharedPoseRanges && slots[i]==UINT32_MAX) {
            if (link.hidden) ++frame->hiddenInstances;
            else { ++frame->retainedStaticInstances; if (link.staticForBudget) ++frame->budgetStaticInstances; }
            continue;
        }
        if (!asset || !asset->compiled.Valid()) {
            identityGroups(link);
            for (std::uint32_t k=0;k<link.alphaCount;++k)
                if (!scene.meshColors.empty()) frame->colors[link.firstAlpha+k]=scene.meshColors[g.models[m].firstMesh+k];
            link.evaluationEpoch=s.epoch; link.poseVersion=++s.staticPoseVersion;
            if (asset) link.asset={static_cast<std::uint64_t>(assetIndex)+1,scene.generation};
            ++frame->retainedStaticInstances;
            continue;
        }
        const auto& current=frame->states[link.requestIndex];
        PoseRequest request; request.instance=current.identity; request.asset=asset->compiled.Identity();
        request.assetIndex=asset->evaluatorIndex; request.sequenceIndex=current.sequenceIndex;
        request.sequenceElapsedMs=current.elapsedMs; request.globalElapsedMs=current.globalMs;
        if (frame->worldCamera) {
            auto local=ToAnimationLocalCamera(*frame->worldCamera,current.transform);
            if (!local.Succeeded()) return {std::nullopt,std::move(local.diagnostics)};
            request.hasCamera=1; request.camera=*local.value;
        }
        s.requests[active]=request; s.renderedIndices[active]=i; ++active;
        if (active==s.batchWidth) if (auto result=flush(); !result.Succeeded()) return {std::nullopt,std::move(result.diagnostics)};
    }
    if (auto result=flush(); !result.Succeeded()) return {std::nullopt,std::move(result.diagnostics)};
    for (const auto& matrix:frame->groups) for (const auto f:matrix.values)
        if (!std::isfinite(f) || std::abs(f)>1e7f) return fail("ANIMATION_GROUP_INVALID","组矩阵不满足插件有限值预算。");
    frame->timings.posePackingNs=ElapsedNs(poseStarted)-frame->timings.poseEvaluationNs;
    frame->contentVersion=NextAnimationSerial();
    if (s.particles) {
        const auto particlesStarted=TimingClock::now();
        auto status=PrepareAnimationParticles(*s.particles,scene,s.assets,s.requestAssets,*frame,previous);
        frame->timings.particlesTotalNs=ElapsedNs(particlesStarted);
        if (!status.Succeeded()) return {std::nullopt,std::move(status.diagnostics)};
    }
    const auto fingerprintStarted=TimingClock::now();
    for (auto& link:frame->poses) {
        if (link.hidden || link.outsideView) link.warnings={}; // 粒子链可能在主姿势成功后隐藏整实例。
        if (link.outsideView) continue;
        if (s.sharedPoseRanges && !link.animated && !link.hidden) continue; // 实际缓存片段的指纹/可见度不逐实例重复计算。
        link.noVisibleGeometry=!AnimationHasVisibleLayers(scene,*frame,link,frame->instances[link.renderedInstance].instance.model);
        if (link.hidden) continue; // 占位矩阵/透明度没有成功姿势指纹。
        link.matrixHash=AnimationBitsHash(frame->groups.data()+link.firstGroup,link.groupCount*sizeof(War3RenderGroupMatrixV2));
        link.alphaHash=AnimationBitsHash(frame->alphas.data()+link.firstAlpha,link.alphaCount*sizeof(float));
        link.colorHash=AnimationBitsHash(frame->colors.data()+link.firstAlpha,link.alphaCount*sizeof(War3RenderMeshColorV2));
        if (scene.dynamicMaterialAlphas) link.materialAlphaHash=AnimationBitsHash(frame->layerAlphas.data()+link.materialAlphas.offset,link.materialAlphas.count*sizeof(float));
        if (scene.dynamicMaterialStates) link.materialStateHash=AnimationBitsHash(frame->materialStates.data()+link.materialStates.offset,link.materialStates.count*sizeof(War3RenderMaterialStateV2));
    }
    auto& measurement=frame->timings;
    measurement.outputMotionHash=measurement.particleStateHash=14695981039346656037ull;
    for (std::size_t i=0;i<frame->poses.size();++i) {
        const auto& pose=frame->poses[i];
        if (pose.hidden || pose.noVisibleGeometry || pose.outsideView) continue;
        AddMotionHash(measurement.outputMotionHash,&pose.matrixHash,sizeof(pose.matrixHash));
        AddMotionHash(measurement.outputMotionHash,&pose.alphaHash,sizeof(pose.alphaHash));
        AddMotionHash(measurement.outputMotionHash,&pose.colorHash,sizeof(pose.colorHash));
        if (scene.dynamicMaterialAlphas) AddMotionHash(measurement.outputMotionHash,&pose.materialAlphaHash,sizeof(pose.materialAlphaHash));
        if (scene.dynamicMaterialStates) AddMotionHash(measurement.outputMotionHash,&pose.materialStateHash,sizeof(pose.materialStateHash));
        const auto& instance=frame->instances[i].instance;
        AddMotionHash(measurement.outputMotionHash,instance.transform,sizeof(instance.transform));
        AddMotionHash(measurement.outputMotionHash,instance.color,sizeof(instance.color));
    }
    for (const auto& particle:frame->particleLinks) if (particle.simulated && !particle.hidden) {
        // StateHash 还含模拟时钟，只供诊断。有效动画不能拿该时钟替代实际几何变化。
        AddMotionHash(measurement.particleStateHash,&particle.stateHash,sizeof(particle.stateHash));
        if (particle.vertexCount && particle.drawCount)
            AddMotionHash(measurement.outputMotionHash,&particle.geometryHash,sizeof(particle.geometryHash));
    }
    if (!frame->states.empty()) {
        measurement.minElapsedMs=measurement.minGlobalMs=UINT64_MAX;
        for (const auto& state:frame->states) {
            measurement.minElapsedMs=std::min(measurement.minElapsedMs,state.elapsedMs);
            measurement.maxElapsedMs=std::max(measurement.maxElapsedMs,state.elapsedMs);
            measurement.minGlobalMs=std::min(measurement.minGlobalMs,state.globalMs);
            measurement.maxGlobalMs=std::max(measurement.maxGlobalMs,state.globalMs);
        }
    }
    measurement.fingerprintNs=ElapsedNs(fingerprintStarted);
    if (s.prem) {
        if (auto status = PrepareAnimationPrem(s, *frame, previous, advance); !status.Succeeded())
            return {std::nullopt, std::move(status.diagnostics)};
    }
    const auto boundsStarted=TimingClock::now();
    auto bounds=PrepareAnimationPickBounds(scene,*frame);
    measurement.pickBoundsNs=ElapsedNs(boundsStarted);
    if (!bounds.Succeeded()) return {std::nullopt,std::move(bounds.diagnostics)};
    measurement.prepareNs=ElapsedNs(prepareStarted);
    return Output::Success(std::move(frame));
} catch (const std::bad_alloc&) {
    return Result<std::shared_ptr<const AnimationFrame>>::Failure("Render.Animation","ANIMATION_ALLOCATION_FAILED","动画帧包分配失败；未发布半帧。");
} catch (const std::exception& e) {
    return Result<std::shared_ptr<const AnimationFrame>>::Failure("Render.Animation","ANIMATION_FRAME_FAILED",e.what());
}
} // namespace War3Map::Render
