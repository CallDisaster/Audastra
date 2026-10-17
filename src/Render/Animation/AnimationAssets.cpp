#include "AnimationInternal.h"
#include "AnimationPrem.h"
#include "AnimationParticles.h"
#include "AnimationVisibility.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <set>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace War3Map::Render {
using namespace War3RenderCore;
namespace {
template<class Predicate> bool OnlyErrors(std::span<const Diagnostic> diagnostics, Predicate accepts) {
    bool error = false;
    for (const auto& d : diagnostics) if (d.severity == DiagnosticSeverity::Error) {
        error = true;
        if (!accepts(std::string_view(d.code))) return false;
    }
    return error;
}
bool MissingResource(std::span<const Diagnostic> diagnostics) {
    return OnlyErrors(diagnostics, [](std::string_view code) {
        return code == "RESOURCE_NOT_FOUND" || code == "file-not-found" || code == "WORLD_ASSET_MISSING" ||
            code == "missing" || code == "MISSING";
    });
}
bool ModelParseFailure(std::span<const Diagnostic> diagnostics) {
    return OnlyErrors(diagnostics, [](std::string_view code) {
        return code == "MDX_MAGIC" || code == "MDX_VERSION_UNSUPPORTED" || code == "MDX_TRUNCATED" ||
            code == "MDX_SIZE_INVALID" || code == "MDX_TAG_MISMATCH" || code == "MDX_VALUE_INVALID" ||
            code == "MDX_NUMBER_INVALID" || code == "MDX_REFERENCE_INVALID" ||
            code == "MDX_COLLISION_TYPE_UNSUPPORTED" || code == "MDX_GEOA_INTERPOLATION_UNSUPPORTED";
    });
}
std::string FirstError(std::span<const Diagnostic> diagnostics) {
    for (const auto& d : diagnostics) if (d.severity == DiagnosticSeverity::Error) return d.code;
    return {};
}
bool PreviewColorFailure(GeosetColorError error) {
    return error == GeosetColorError::Uninterpreted || error == GeosetColorError::InvalidNumber;
}
template<class Target, class Source> void CopyStaticGeometry(Target& target, const Source& source) {
    auto& g = target.geometry; const auto& a = source.geometry;
    g.vertices = a.vertices; g.indices = a.indices; g.layers = a.layers; g.meshes = a.meshes;
    g.layerSamplerFlags = a.layerSamplerFlags;
    g.models = a.models; g.modelBounds = a.modelBounds; g.textures = a.textures;
    g.vertexGroups = a.vertexGroups; g.meshGeosets = a.meshGeosets;
    g.modelPaths = a.modelPaths; g.modelAssetKeys = a.modelAssetKeys;
    g.modelTextureBindings = a.modelTextureBindings;
    g.untexturedLayers = a.untexturedLayers; g.rigidStaticPreviewAssets = a.rigidStaticPreviewAssets;
    g.warnings = a.warnings;
    target.vertices = source.vertices; target.meshes = source.meshes; target.models = source.models;
    target.meshColors = source.meshColors; target.drawLayerBindings = source.drawLayerBindings;
    target.modelLayerRanges = source.modelLayerRanges; target.pickModels = source.pickModels;
    target.pickMeshGroups = source.pickMeshGroups; target.pickGroupBounds = source.pickGroupBounds;
    target.sources = source.sources;
    target.dynamicColors = source.dynamicColors; target.dynamicMaterialAlphas = source.dynamicMaterialAlphas;
    target.dynamicMaterialStates = source.dynamicMaterialStates;
}
template<class T> void Append(SharedSceneVector<T>& target, const SharedSceneVector<T>& source) {
    if (!source.empty()) target.insert(target.cend(), source.cbegin(), source.cend());
}
Status PromoteMaterialTextureBindings(AnimatedModelScene& scene, std::span<const AnimationAsset> assets,
    std::span<const std::uint32_t> modelAssets, const WorldModelLimits& limits) {
    if (!scene.dynamicMaterialStates) return Status::Ok();
    const auto& catalog=std::as_const(scene); const auto& g=catalog.geometry;
    const auto fail=[](const char* code,const char* text) { return AnimationFailure(code,text); };
    if (modelAssets.size()!=catalog.models.size() || g.modelTextureBindings.size()!=catalog.models.size())
        return fail("ANIMATION_TEXTURE_MAPPING","静态材质元数据与冻结模型目录数量不同。");
    auto remaining=limits.maxTotalTextureBytes;
    const auto take=[&](std::uint64_t bytes) {
        if (bytes>remaining) return false;
        remaining-=bytes; return true;
    };
    bool fits=take(std::uint64_t(g.layerSamplerFlags.capacity())*sizeof(std::uint32_t));
    if (!g.layerSamplerFlags.empty()) fits &= take(sizeof(War3RenderStaticSamplerModelSceneV2)-sizeof(War3RenderModelSceneV2));
    for (const auto& texture:g.textures)
        fits &= ModelTextureValid(texture,limits.maxTextureDimension) && take(ModelTextureRgbaBytes(texture));
    for (const auto& binding:g.modelTextureBindings) if (binding)
        fits &= take(std::uint64_t(binding->capacity())*sizeof(ModelTextureBinding));
    if (!fits) return fail("ANIMATION_DELTA_TEXTURE_BUDGET","静态材质提升的常驻来源元数据超过原预算。");
    // false→true：从常驻draw层绑定真实TEXS；正式预取也走此函数，COW工作留在准备阶段。
    for (std::uint32_t m=0;m<catalog.models.size();++m) {
        const auto index=modelAssets[m];
        if (index==UINT32_MAX) continue;
        if (index>=assets.size()) return fail("ANIMATION_TEXTURE_OWNER_INVALID","静态材质来源超出冻结资产目录。");
        if (!assets[index].parsed || g.modelTextureBindings[m]) continue;
        const auto& asset=assets[index];
        if (asset.materialTextures && asset.materialTextures->HasAnimatedTexture())
            return fail("ANIMATION_TEXTURE_MAPPING","动态KMTF资产缺少已冻结的全部可达贴图绑定。");
        const auto requestedBytes=std::uint64_t(asset.parsed->textures.size())*sizeof(ModelTextureBinding);
        if (requestedBytes>remaining)
            return fail("ANIMATION_DELTA_TEXTURE_BUDGET","静态材质提升的新绑定超过原预算。");
        auto bindings=std::make_shared<std::vector<ModelTextureBinding>>(asset.parsed->textures.size());
        if (!take(std::uint64_t(bindings->capacity())*sizeof(ModelTextureBinding)))
            return fail("ANIMATION_DELTA_TEXTURE_BUDGET","静态材质提升的实际绑定分配超过原预算。");
        const auto model=catalog.models[m].model;
        for (std::uint32_t k=0;k<model.meshCount;++k) {
            const auto mesh=model.firstMesh+k,geoset=g.meshGeosets[mesh];
            const auto material=asset.parsed->geosets.at(geoset).materialId;
            const auto& layers=asset.parsed->materials.at(material).layers;
            if (layers.size()!=g.meshes[mesh].layerCount) return fail("ANIMATION_TEXTURE_MAPPING","存活静态材质层映射变化。");
            for (std::uint32_t layer=0;layer<layers.size();++layer) {
                const auto id=layers[layer].textureId,draw=g.meshes[mesh].firstLayer+layer;
                if (id>=bindings->size() || asset.parsed->textures[id].flags>3)
                    return fail("ANIMATION_TEXTURE_SAMPLER_UNSUPPORTED","存活真实TEXS sampler flags不支持。");
                auto& binding=(*bindings)[id]; const auto resident=g.layers[draw].textureIndex;
                if (binding.reachable && binding.textureIndex!=resident)
                    return fail("ANIMATION_TEXTURE_MAPPING","同一几何变体的静态TEXS出现不同常驻身份。");
                binding={resident,asset.parsed->textures[id].flags,true};
                scene.drawLayerBindings[draw].staticSamplerFlags=binding.samplerFlags;
            }
        }
        scene.geometry.modelTextureBindings[m]=std::move(bindings);
    }
    return Status::Ok();
}
template<class State> AnimationResourceReader FrozenAssetReader(State& state,
    AnimationResourceReader rawRead, bool recordAccesses, std::uint64_t additionalAssetBytes = 0) {
    return [&state, rawRead = std::move(rawRead), recordAccesses, additionalAssetBytes,
        observed = std::set<std::pair<std::string, std::uint64_t>>{}]
        (std::string_view path, std::uint64_t budget) mutable -> Result<AnimationResource> {
        const auto key = AnimationPathKey(path);
        const auto remember = [&](std::shared_ptr<const AnimationResource> resource, std::vector<Diagnostic> diagnostics = {}) {
            if (recordAccesses && observed.emplace(key, budget).second)
                state.staticResourceAccesses.push_back({key, budget, std::move(resource), std::move(diagnostics)});
        };
        const auto found = state.resources.find(key);
        if (found != state.resources.end()) {
            if (found->second->bytes.size() > budget) return Result<AnimationResource>::Failure(
                "Render.Animation", "ANIMATION_RESOURCE_BUDGET", "已冻结资源超过本次读取预算。");
            remember(found->second);
            return Result<AnimationResource>::Success(*found->second);
        }
        auto value = rawRead(path, budget);
        if (!value.Succeeded()) { remember({}, value.diagnostics); return value; }
        if (value.value->bytes.size() > budget) return Result<AnimationResource>::Failure(
            "Render.Animation", "ANIMATION_RESOURCE_BUDGET", "实际资源超过本次读取预算。");
        if (additionalAssetBytes && (state.sourceBytes > state.limits.maxAssetBytes
            || additionalAssetBytes > state.limits.maxAssetBytes - state.sourceBytes
            || state.resourceBytes > state.limits.maxAssetBytes - state.sourceBytes - additionalAssetBytes
            || value.value->bytes.size() > state.limits.maxAssetBytes - state.sourceBytes - additionalAssetBytes - state.resourceBytes))
            return Result<AnimationResource>::Failure("Render.Animation", "ANIMATION_ASSET_BUDGET",
                "新资源连同实际 PREM 子服务已拥有资产超过原预算，未挤占其资源额度。");
        if (state.resourceBytes > state.limits.maxAssetBytes ||
            value.value->bytes.size() > state.limits.maxAssetBytes - state.resourceBytes)
            return Result<AnimationResource>::Failure("Render.Animation", "ANIMATION_ASSET_BUDGET", "冻结资源的实际原字节超过资产内存预算。");
        state.resourceBytes += value.value->bytes.size();
        auto frozen = std::make_shared<const AnimationResource>(std::move(*value.value));
        state.resources.emplace(key, frozen); remember(frozen);
        return Result<AnimationResource>::Success(*frozen);
    };
}
}
AnimationStaticGeometry AnimationStaticGeometry::Capture(const AnimatedModelScene& source) {
    AnimationStaticGeometry result; CopyStaticGeometry(result, source); return result;
}
AnimatedModelScene AnimationStaticGeometry::Materialize() const {
    AnimatedModelScene result; CopyStaticGeometry(result, *this); return result;
}
void AnimationStaticGeometry::ApplyTo(AnimatedModelScene& target) const {
    CopyStaticGeometry(target, *this);
}
Status AppendAnimationStaticGeometry(AnimatedModelScene& target, const AnimatedModelScene& added,
    const WorldModelLimits& limits) {
    if (auto budget=CheckAnimationStaticAppend(target,added,limits); !budget.Succeeded()) return budget;
    auto& g=target.geometry; const auto& a=added.geometry;
    std::vector<std::uint32_t> samplerFlags;
    const bool explicitSamplers=!g.layerSamplerFlags.empty() || !a.layerSamplerFlags.empty();
    if (explicitSamplers) {
        samplerFlags.reserve(g.layers.size()+a.layers.size());
        if (g.layerSamplerFlags.empty()) samplerFlags.resize(g.layers.size(),3);
        else samplerFlags.insert(samplerFlags.end(),g.layerSamplerFlags.cbegin(),g.layerSamplerFlags.cend());
        if (a.layerSamplerFlags.empty()) samplerFlags.resize(g.layers.size()+a.layers.size(),3);
        else samplerFlags.insert(samplerFlags.end(),a.layerSamplerFlags.cbegin(),a.layerSamplerFlags.cend());
    }
    if (auto budget=CheckAnimationStaticAppend(target,added,limits,samplerFlags.capacity()); !budget.Succeeded()) return budget;
    const auto vertexBase=static_cast<std::uint32_t>(g.vertices.size());
    const auto indexBase=static_cast<std::uint32_t>(g.indices.size());
    const auto layerBase=static_cast<std::uint32_t>(g.layers.size());
    const auto meshBase=static_cast<std::uint32_t>(g.meshes.size());
    const auto pickBase=static_cast<std::uint32_t>(target.pickGroupBounds.size());
    std::map<std::string,std::uint32_t> textures;
    for (std::uint32_t i=0;i<g.textures.size();++i) textures.emplace(ModelTextureAssetKey(std::as_const(g.textures)[i]),i);
    std::vector<std::uint32_t> textureMap;
    for (const auto& texture:a.textures) {
        const auto key=ModelTextureAssetKey(texture); auto found=textures.find(key);
        if (found==textures.end()) {
            found=textures.emplace(key,static_cast<std::uint32_t>(g.textures.size())).first;
            g.textures.push_back(texture);
        }
        textureMap.push_back(found->second);
    }
    Append(g.vertices,a.vertices); Append(g.indices,a.indices); Append(g.vertexGroups,a.vertexGroups);
    Append(g.meshGeosets,a.meshGeosets); Append(g.modelPaths,a.modelPaths); Append(g.modelAssetKeys,a.modelAssetKeys); Append(g.modelBounds,a.modelBounds);
    for (const auto& source:a.modelTextureBindings) {
        if (!source) { g.modelTextureBindings.push_back(nullptr); continue; }
        auto bindings=std::make_shared<std::vector<ModelTextureBinding>>(*source);
        for (auto& binding:*bindings) if (binding.reachable && binding.textureIndex!=WAR3_RENDER_MODEL_NO_TEXTURE)
            binding.textureIndex=textureMap.at(binding.textureIndex);
        g.modelTextureBindings.push_back(std::move(bindings));
    }
    if (explicitSamplers) g.layerSamplerFlags=std::move(samplerFlags);
    for (auto layer:a.layers) {
        if (layer.textureIndex!=WAR3_RENDER_MODEL_NO_TEXTURE) layer.textureIndex=textureMap.at(layer.textureIndex);
        g.layers.push_back(layer);
    }
    for (auto mesh:a.meshes) {
        mesh.baseVertex+=vertexBase; mesh.firstIndex+=indexBase; mesh.firstLayer+=layerBase; g.meshes.push_back(mesh);
    }
    for (auto model:a.models) { model.firstMesh+=meshBase; g.models.push_back(model); }
    Append(target.vertices,added.vertices); Append(target.meshColors,added.meshColors);
    Append(target.drawLayerBindings,added.drawLayerBindings);
    for (auto range:added.modelLayerRanges) { range.offset+=layerBase; target.modelLayerRanges.push_back(range); }
    Append(target.pickModels,added.pickModels); Append(target.pickGroupBounds,added.pickGroupBounds);
    for (auto mesh:added.meshes) {
        mesh.mesh.baseVertex+=vertexBase; mesh.mesh.firstIndex+=indexBase; mesh.mesh.firstLayer+=layerBase;
        target.meshes.push_back(mesh);
    }
    for (auto model:added.models) { model.model.firstMesh+=meshBase; target.models.push_back(model); }
    for (auto range:added.pickMeshGroups) { range.offset+=pickBase; target.pickMeshGroups.push_back(range); }
    g.untexturedLayers+=a.untexturedLayers;
    g.warnings.insert(g.warnings.end(),a.warnings.begin(),a.warnings.end());
    target.dynamicColors|=added.dynamicColors;
    target.dynamicMaterialAlphas|=added.dynamicMaterialAlphas;
    target.dynamicMaterialStates|=added.dynamicMaterialStates;
    std::set<std::tuple<std::string,std::string,std::string>> sources;
    for (const auto& s:target.sources) sources.emplace(s.virtualPath,s.sourceLayer,s.contentDigest);
    for (const auto& s:added.sources) if (sources.emplace(s.virtualPath,s.sourceLayer,s.contentDigest).second) target.sources.push_back(s);
    return {};
}
Status CheckAnimationStaticAppend(const AnimatedModelScene& current,
    const AnimatedModelScene& added, const WorldModelLimits& limits, std::uint64_t mergedSamplerCapacity) {
    const auto& g = current.geometry; const auto& a = added.geometry;
    const auto fits = [](std::uint64_t present, std::uint64_t extra, std::uint64_t maximum) {
        return present <= maximum && extra <= maximum - present;
    };
    if ((!g.layerSamplerFlags.empty() && g.layerSamplerFlags.size()!=g.layers.size()) ||
        (!a.layerSamplerFlags.empty() && a.layerSamplerFlags.size()!=a.layers.size()) ||
        !fits(g.vertices.size(), a.vertices.size(), limits.maxVertices) ||
        !fits(g.indices.size(), a.indices.size(), limits.maxIndices) ||
        !fits(g.layers.size(), a.layers.size(), limits.maxLayers) ||
        !fits(g.meshes.size(), a.meshes.size(), limits.maxMeshes) ||
        !fits(g.models.size(), a.models.size(), limits.maxModels))
        return AnimationFailure("ANIMATION_DELTA_ASSET_BUDGET", "新增资产的累计几何超过已配置内存/索引预算，驻留资产未改动。");
    std::set<std::string> textures;
    std::uint64_t bytes = 0;
    if (!g.layerSamplerFlags.empty() || !a.layerSamplerFlags.empty())
        bytes = (std::uint64_t(g.layerSamplerFlags.capacity()) + a.layerSamplerFlags.capacity()
            + (mergedSamplerCapacity ? mergedSamplerCapacity : g.layers.size() + a.layers.size())) * sizeof(std::uint32_t);
    for (const auto& bindings:g.modelTextureBindings)
        if (bindings) bytes+=std::uint64_t(bindings->capacity())*sizeof(ModelTextureBinding);
    for (const auto& bindings:a.modelTextureBindings)
        if (bindings) bytes+=std::uint64_t(bindings->capacity())*sizeof(ModelTextureBinding);
    if (bytes > limits.maxTotalTextureBytes)
        return AnimationFailure("ANIMATION_DELTA_TEXTURE_BUDGET", "sampler 常驻及合并暂存超过原材质预算。");
    const auto addTextures = [&](const auto& values) {
        for (const auto& texture : values) {
            if (!ModelTextureValid(texture,limits.maxTextureDimension)) return false;
            if (!textures.insert(ModelTextureAssetKey(texture)).second) continue;
            if (!fits(bytes, ModelTextureRgbaBytes(texture), limits.maxTotalTextureBytes) ||
                textures.size() > limits.maxTextures) return false;
            bytes += ModelTextureRgbaBytes(texture);
        }
        return true;
    };
    if (!addTextures(g.textures) || !addTextures(a.textures))
        return AnimationFailure("ANIMATION_DELTA_TEXTURE_BUDGET", "增量后的实际解码纹理超过资产预算。");
    return {};
}
Result<AnimationInstanceState> MakeAnimationInitialState(const AnimationInstanceSpec& input,
    const AnimationAsset& asset, bool retainUnsupported, std::vector<Diagnostic>& warnings) {
    const auto fail=[](const char* code,const char* text) { return Result<AnimationInstanceState>::Failure("Render.Animation",code,text); };
    const auto sequences=asset.parsed ? std::span<const MdxSequence>(asset.parsed->sequences) : std::span<const MdxSequence>{};
    if (asset.parsed && input.sequenceIndex && *input.sequenceIndex >= sequences.size())
        return fail("ANIMATION_SEQUENCE_INVALID", "显式动作序号不在已解析模型的 SEQS 中，静态保留不能掩盖宿主输入错误。");
    auto selected = input.sequenceIndex ? input.sequenceIndex :
        FindAnimationSequence(sequences,input.action,input.model.animationProperties);
    if (asset.compiled.Valid() && (!selected || *selected >= sequences.size())) {
        if (!retainUnsupported || input.sequenceIndex)
            return fail(input.model.animationProperties.empty() ? "ANIMATION_SEQUENCE_INVALID" : "ANIMATION_REQUIRED_SEQUENCE_MISSING",
                (input.model.modelPath + " 的 SEQS 中不存在动作 " + input.action +
                " 与所需动画属性 [" + input.model.animationProperties + "]。").c_str());
        // 没有 Stand 时只选确实存在的首动作，并保留提示；不是编造一个默认骨骼姿势。
        selected = 0;
        warnings.push_back({DiagnosticSeverity::Warning,"Render.Animation",
            input.model.animationProperties.empty() ? "ANIMATION_FIRST_SEQUENCE" : "ANIMATION_REQUIRED_SEQUENCE_MISSING",
            input.model.modelPath + " 没有动作 " + input.action + " 与所需动画属性 [" + input.model.animationProperties +
            "]，世界浏览保留实际 SEQS[0]，不能视为所需形态已显示。",std::nullopt});
    }
    AnimationInstanceState initial;
    initial.identity = input.identity; initial.sequenceIndex = selected.value_or(0);
    const auto* sequence = asset.compiled.Valid() ? &sequences[initial.sequenceIndex] : nullptr;
    initial.elapsedMs = input.elapsedMs.value_or(sequence && sequence->end >= sequence->start ?
        (static_cast<std::uint64_t>(sequence->end) - sequence->start)/2 : 0);
    initial.globalMs = input.globalMs; initial.playing = input.playing; initial.speed = input.speed;
    initial.transform = ModelInstanceTransform(input.model); initial.tint = input.model.tint;
    if (auto check = CheckAnimationState(initial); !check.Succeeded()) return {std::nullopt,std::move(check.diagnostics)};
    return Result<AnimationInstanceState>::Success(std::move(initial));
}
ModelAnimationService::ModelAnimationService(std::unique_ptr<State> state) : state_(std::move(state)) {}
ModelAnimationService::~ModelAnimationService() = default;
std::shared_ptr<const AnimatedModelScene> ModelAnimationService::Scene() const noexcept { return state_->scene; }

Result<std::shared_ptr<ModelAnimationService>> ModelAnimationService::Create(AnimationSession session,
    std::span<const AnimationInstanceSpec> inputs, const AnimationResourceReader& read,
    const AnimationLimits& limits, std::span<const BakedModelMesh> baked) {
    return CreateWithResident(std::move(session),inputs,read,limits,baked,nullptr);
}
Result<std::shared_ptr<ModelAnimationService>> ModelAnimationService::CreateWithResident(AnimationSession session,
    std::span<const AnimationInstanceSpec> inputs, const AnimationResourceReader& rawRead,
    const AnimationLimits& limits, std::span<const BakedModelMesh> baked, const State* resident,
    std::shared_ptr<const AnimationAssetSnapshot> preloaded) try {
    auto built = BuildStaticAssets(std::move(session), inputs, rawRead, limits, baked, resident, std::move(preloaded));
    if (!built.Succeeded()) return {std::nullopt, std::move(built.diagnostics)};
    auto state = std::move(*built.value);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        auto initial = MakeAnimationInitialState(inputs[i], state->assets[state->requestAssets[i]],
            limits.retainUnsupportedStatic, state->scene->geometry.warnings);
        if (!initial.Succeeded()) return {std::nullopt, std::move(initial.diagnostics)};
        state->initial.push_back(std::move(*initial.value));
    }
    state->staticResourceAccesses = decltype(state->staticResourceAccesses){};
    if (auto storage = InitializeStorage(*state, limits, rawRead); !storage.Succeeded())
        return {std::nullopt, std::move(storage.diagnostics)};
    return Result<std::shared_ptr<ModelAnimationService>>::Success(
        std::shared_ptr<ModelAnimationService>(new ModelAnimationService(std::move(state))));
} catch (const std::bad_alloc&) {
    return Result<std::shared_ptr<ModelAnimationService>>::Failure("Render.Animation", "ANIMATION_ALLOCATION_FAILED", "动画资产内存分配失败。");
} catch (const std::exception& e) {
    return Result<std::shared_ptr<ModelAnimationService>>::Failure("Render.Animation", "ANIMATION_BUILD_FAILED", e.what());
}
Result<std::unique_ptr<ModelAnimationService::State>> ModelAnimationService::BuildStaticAssets(AnimationSession session,
    std::span<const AnimationInstanceSpec> inputs, const AnimationResourceReader& rawRead,
    const AnimationLimits& limits, std::span<const BakedModelMesh> baked, const State* resident,
    std::shared_ptr<const AnimationAssetSnapshot> preloaded) try {
    using Output = Result<std::unique_ptr<State>>;
    const auto fail = [](const char* code, const char* text) { return Output::Failure("Render.Animation", code, text); };
    if (limits.playbackPolicy != AnimationPlaybackPolicy::StrictReplay &&
        limits.playbackPolicy != AnimationPlaybackPolicy::InteractivePreview)
        return fail("ANIMATION_PLAYBACK_POLICY_INVALID", "未知动画时间求值策略，未推测为交互预览。");
    if (!rawRead || !session.generation || session.sourceIdentity.empty() || (inputs.empty() && baked.empty()) ||
        inputs.size()>=UINT32_MAX || limits.maxFrameBytes>512ull*1024*1024 ||
        !limits.maxFrameBytes || !limits.maxAssetBytes || limits.maxActivePoseBytes>64ull*1024*1024)
        return fail("ANIMATION_SESSION_INVALID", "来源代次、读取器或实例/内存预算无效。");
    auto state = std::make_unique<State>();
    state->limits=limits;
    if (resident) {
        state->assets=resident->assets; state->assetPaths=resident->assetPaths;
        state->resources=resident->resources; state->sourceBytes=resident->sourceBytes; state->resourceBytes=resident->resourceBytes;
        state->collisionNodesRequired=resident->collisionNodesRequired;
    }
    const auto read = FrozenAssetReader(*state, rawRead, true);
    state->retainUnsupportedStatic = limits.retainUnsupportedStatic;
    state->maxVisibilityBytes = limits.maxVisibilityBytes;
    state->scene = std::make_shared<AnimatedModelScene>();
    auto& scene = *state->scene;
    scene.sceneVersion = NextAnimationSerial(); state->epoch = NextAnimationSerial();
    scene.generation = session.generation; scene.sourceIdentity = std::move(session.sourceIdentity);
    scene.documentRevision = session.documentRevision;
    scene.specs.assign(inputs.begin(), inputs.end()); scene.sequences.resize(inputs.size());
    scene.particlesEnabled = limits.enableParticles; scene.premEnabled = limits.enablePremPreview;
    scene.particleCapabilities.resize(inputs.size());
    std::unordered_map<std::string, std::uint32_t> cached;
    for (std::uint32_t i=0;i<state->assetPaths.size();++i) cached.emplace(state->assetPaths[i],i);
    std::set<std::pair<std::uint64_t,std::uint64_t>> identities;
    std::vector<ModelInstanceRequest> requests; requests.reserve(inputs.size());
    std::vector<std::uint32_t> geometryRequests;
    std::vector<Diagnostic> warnings;
    std::uint64_t sourceBytes = state->sourceBytes;
    if (inputs.size()>limits.maxFrameBytes/(sizeof(AnimationInstanceState)+sizeof(std::uint8_t)))
        return fail("ANIMATION_FRAME_BUDGET","全量实例状态实际内存超过帧预算；保留原静态场景，未截断地图实例。");
    scene.requestGeometryRequired.reserve(inputs.size());
    for (std::size_t r = 0; r < inputs.size(); ++r) {
        const auto& input = inputs[r];
        if (!input.identity.value || input.identity.generation != session.generation || input.hostKey.empty() ||
            !identities.emplace(input.identity.value,input.identity.generation).second)
            return fail("ANIMATION_INSTANCE_IDENTITY", "实例需非零独立身份、宿主键及当前来源代次。");
        const auto key = ModelAssetKey(input.model);
        auto found = cached.find(key);
        if (found == cached.end()) {
            if (state->assets.size() >= 4096) return fail("ANIMATION_ASSET_BUDGET", "模型资产数超过 4096。");
            AnimationAsset asset;
            if (input.model.ownedStaticAsset) {
                const auto bytes = input.model.ownedStaticAsset->OwnedBytes();
                if (sourceBytes > limits.maxAssetBytes || bytes > limits.maxAssetBytes - sourceBytes)
                    return fail("ANIMATION_ASSET_BUDGET", "编辑器owned静态辅助资产实际拥有字节超过资产预算。");
                sourceBytes += bytes; asset.drawable = true; asset.ownedStaticAsset = input.model.ownedStaticAsset;
                // 这是显式原创辅助资产，不是MDX解析失败后的静态降级；没有骨架/粒子求值。
            } else {
                auto resource = read(input.model.modelPath, limits.geometry.maxModelBytes);
                if (!resource.Succeeded()) {
                    if (!limits.retainUnsupportedStatic || !MissingResource(resource.diagnostics))
                        return {std::nullopt, std::move(resource.diagnostics)};
                    asset.unavailableCode = FirstError(resource.diagnostics);
                }
                if (resource.Succeeded()) {
                    if (resource.value->sourceLayer.empty() || resource.value->digest.size() != 64 ||
                        !std::all_of(resource.value->digest.begin(),resource.value->digest.end(),[](unsigned char c) { return std::isxdigit(c) != 0; }) ||
                        resource.value->bytes.size() > limits.geometry.maxModelBytes ||
                        resource.value->bytes.size() > limits.maxAssetBytes - sourceBytes)
                        return fail("ANIMATION_ASSET_SOURCE", "资源必须有实际来源层、SHA-256 及有界读取内容。");
                    sourceBytes += resource.value->bytes.size();
                    asset.source = {key, scene.sourceIdentity + "|" + resource.value->sourceLayer, resource.value->digest};
                    std::shared_ptr<const MdxModel> cachedParsed;
                    if (preloaded && preloaded->allowRigidStaticPreview_ == limits.allowRigidStaticPreview) {
                        const auto foundModel = preloaded->models_.find(AnimationPathKey(input.model.modelPath));
                        if (foundModel != preloaded->models_.end()
                            && foundModel->second.sourceLayer == resource.value->sourceLayer
                            && foundModel->second.digest == resource.value->digest)
                            cachedParsed = foundModel->second.parsed;
                    }
                    Result<MdxModel> parsed;
                    if (!cachedParsed) parsed = ParseMdx(resource.value->bytes);
                    else if (cachedParsed->rigidStaticPreviewSource) {
                        if (resource.value->bytes.size() > limits.maxAssetBytes - sourceBytes)
                            return fail("ANIMATION_ASSET_BUDGET", "静态预览保留原始字节后超过资产预算。");
                        sourceBytes += resource.value->bytes.size();
                    }
                    // 原默认读取不变；局部预览资格失败仍保留原严格诊断，不扩展解析错误白名单。
                    if (!cachedParsed && !parsed.Succeeded() && limits.allowRigidStaticPreview && FirstError(parsed.diagnostics) == "MDX_VALUE_INVALID") {
                        auto preview = ParseMdxRigidStaticPreview(resource.value->bytes);
                        if (preview.Succeeded()) {
                            if (resource.value->bytes.size() > limits.maxAssetBytes - sourceBytes)
                                return fail("ANIMATION_ASSET_BUDGET", "静态预览保留原始字节后超过资产预算。");
                            sourceBytes += resource.value->bytes.size();
                            parsed = std::move(preview);
                        }
                    }
                    if (!cachedParsed && !parsed.Succeeded()) {
                        if (!limits.retainUnsupportedStatic || !ModelParseFailure(parsed.diagnostics))
                            return {std::nullopt, std::move(parsed.diagnostics)};
                        asset.unavailableCode = FirstError(parsed.diagnostics);
                    } else {
                        asset.parsed = cachedParsed ? std::move(cachedParsed)
                            : std::make_shared<const MdxModel>(std::move(*parsed.value));
                        ReadAnimationVisibilityBounds(asset, resource.value->bytes);
                        const AssetIdentity assetIdentity{static_cast<std::uint64_t>(state->assets.size()+1), session.generation};
                        const auto compileBudget = std::min<std::uint64_t>(limits.maxAssetBytes, 128ull*1024*1024);
                        auto build = asset.parsed->rigidStaticPreviewSource
                            ? CompileMdxRigidStaticPreview(*asset.parsed, assetIdentity, asset.source, compileBudget)
                            : CompileMdxModel(*asset.parsed, assetIdentity, asset.source, compileBudget);
                        if (build.Succeeded() && !asset.parsed->sequences.empty()) {
                            asset.compiled = std::move(*build.value);
                            // KMTF 或 TXAN 引用都需要材质贴图 sidecar；坏/缺 TXAN 由 sidecar 保持静态并报告。
                            const bool textureAnimation=std::any_of(asset.parsed->materials.begin(),asset.parsed->materials.end(),
                                [](const auto& material) { return std::any_of(material.layers.begin(),material.layers.end(),
                                    [](const auto& layer) { return layer.animatedTexture || layer.textureAnimationId!=UINT32_MAX; }); });
                            if (textureAnimation) {
                                auto textures=CompileMdxMaterialTextures(*asset.parsed,asset.compiled,compileBudget);
                                if (!textures.Succeeded()) return {std::nullopt,std::move(textures.diagnostics)};
                                if (sourceBytes>limits.maxAssetBytes || textures.value->StorageBytes()>limits.maxAssetBytes-sourceBytes)
                                    return fail("ANIMATION_ASSET_BUDGET","冻结材质贴图轨迹超过资产预算。");
                                sourceBytes+=textures.value->StorageBytes();
                                for (auto diagnostic:textures.diagnostics) {
                                    diagnostic.message=input.model.modelPath+"："+diagnostic.message;
                                    warnings.push_back(std::move(diagnostic));
                                }
                                asset.materialTextures=std::make_shared<const CompiledMaterialTextures>(std::move(*textures.value));
                            }
                        } else {
                            if (!limits.retainUnsupportedStatic || (!build.Succeeded() &&
                                !OnlyErrors(build.diagnostics, [](std::string_view code) { return AnimationLocalDataFailure(code); }))) {
                                if (build.Succeeded()) return fail("ANIMATION_SEQUENCE_UNAVAILABLE", "模型没有 SEQS 动作，不能声明动画可用。");
                                return {std::nullopt, std::move(build.diagnostics)};
                            }
                            const bool materialAnimation=std::any_of(asset.parsed->materials.begin(),asset.parsed->materials.end(),
                                [](const auto& material) { return std::any_of(material.layers.begin(),material.layers.end(),
                                    [](const auto& layer) { return layer.animatedAlpha || layer.animatedTexture; }); });
                            if (materialAnimation) asset.unavailableCode=build.Succeeded() ? "ANIMATION_SEQUENCE_UNAVAILABLE" : FirstError(build.diagnostics);
                        }
                        // 显式预览且骨架编译有效时，只隐藏白名单坏 GEOA，保留完整几何及真实资源检查。
                        bool drawable = false;
                        for (const auto& geoset : asset.parsed->geosets) {
                            if (geoset.animation) {
                                const auto& a = *geoset.animation;
                                const auto color = CompileGeosetColor(a.flags, a.staticColor, a.hasUninterpretedData);
                                if (color.error != GeosetColorError::None &&
                                    !(limits.retainUnsupportedStatic && asset.compiled.Valid() && PreviewColorFailure(color.error))) {
                                    asset.unavailableCode = color.error == GeosetColorError::Uninterpreted ?
                                        "MDX_GEOA_UNINTERPRETED" : "MDX_GEOA_COLOR_INVALID";
                                    break;
                                }
                            }
                            drawable |= (asset.compiled.Valid() || geoset.visibleInStand) && !geoset.indices.empty() &&
                                !asset.parsed->materials[geoset.materialId].layers.empty();
                        }
                        asset.drawable = drawable;
                        auto particleStatus = ReadAnimationParticleAsset(asset, resource.value->bytes, limits);
                        if (!particleStatus.Succeeded()) {
                            if (!limits.retainUnsupportedStatic || !OnlyErrors(particleStatus.diagnostics,
                                [](std::string_view code) { return AnimationLocalDataFailure(code); }))
                                return {std::nullopt, std::move(particleStatus.diagnostics)};
                            asset.unavailableCode = FirstError(particleStatus.diagnostics);
                        } else {
                            for (auto& diagnostic : particleStatus.diagnostics) {
                                diagnostic.message = input.model.modelPath + "：" + diagnostic.message;
                                warnings.push_back(std::move(diagnostic));
                            }
                        }
                        if (asset.unavailableCode.empty() && !drawable && !asset.particleCapability.available)
                            asset.unavailableCode = asset.particleCapability.rejectionCode.empty() ? "ANIMATION_EMPTY_MODEL" : asset.particleCapability.rejectionCode;
                        if (!asset.unavailableCode.empty()) {
                            asset.particleCapability.available = false;
                            asset.particleCapability.rejectionCode = asset.unavailableCode;
                        }
                        if (limits.enableParticles && !limits.enablePremPreview && asset.particleCapability.premCount)
                            warnings.push_back({DiagnosticSeverity::Warning,"Render.Animation","PARTICLE_PREM_UNSUPPORTED",
                                input.model.modelPath + "：旧式 PREM 未实现，未将其他已显示部分称为完整效果。",std::nullopt});
                        if (limits.enableParticles && asset.particleUnfoggedPreview)
                            warnings.push_back({DiagnosticSeverity::Warning,"Render.Animation","PARTICLE_UNFOGGED_REQUIRES_CAPABILITY",
                                input.model.modelPath + "：RIBB Unfogged需要插件声明逐draw粒子材质能力；实际呈现以同帧完成回执为准。",std::nullopt});
                        if (!asset.unavailableCode.empty() && !limits.retainUnsupportedStatic)
                            return fail(asset.unavailableCode.c_str(), "模型颜色或完整网格无法表达，未使用白色或部分几何替代。");
                        if (asset.unavailableCode.empty()) {
                            if (asset.compiled.Valid()) {
                                for (std::uint32_t n=0;n<asset.compiled.NodeCount();++n)
                                    asset.cameraDependent|=(asset.compiled.Node(n).flags&0xF8u)!=0;
                                if (!asset.parsed->collisionShapes.empty()) {
                                    auto indices=std::make_shared<std::vector<std::uint32_t>>(asset.parsed->collisionShapes.size(),NoIndex);
                                    std::unordered_map<std::uint32_t,std::uint32_t> nodes;
                                    for (std::uint32_t n=0;n<asset.compiled.NodeCount();++n) nodes.emplace(asset.compiled.Node(n).objectId,n);
                                    for (std::size_t c=0;c<indices->size();++c) {
                                        const auto node=nodes.find(asset.parsed->collisionShapes[c].nodeId);
                                        if (node!=nodes.end()) (*indices)[c]=node->second;
                                    }
                                    asset.collisionNodes=std::move(indices); state->collisionNodesRequired=true;
                                }
                            } else warnings.push_back({DiagnosticSeverity::Warning,"Render.Animation","ANIMATION_STATIC_RETAINED",
                                input.model.modelPath + "：" + (build.Succeeded() ? "ANIMATION_SEQUENCE_UNAVAILABLE" : FirstError(build.diagnostics)) +
                                "；整模型保留旧静态显示，不称为动画成功。",std::nullopt});
                        }
                    }
                }
            }
            if (!asset.unavailableCode.empty()) warnings.push_back({DiagnosticSeverity::Warning,"Render.Animation",
                asset.unavailableCode, input.model.modelPath + "：整个资产及其摆放未显示。",std::nullopt});
            found = cached.emplace(key,static_cast<std::uint32_t>(state->assets.size())).first;
            state->assetPaths.push_back(key);
            asset.originalParticleRejectionCode = asset.particleCapability.rejectionCode;
            state->assets.push_back(std::move(asset));
        }
        state->requestAssets.push_back(found->second);
        auto& asset = state->assets[found->second];
        scene.requestGeometryRequired.push_back(asset.drawable ? 1 : 0);
        scene.particleCapabilities[r] = asset.particleCapability;
        if (asset.particleCapability.available) ++scene.particleInstanceCount;
        if (!asset.unavailableCode.empty()) scene.unavailableInstances.push_back({input.identity,
            {static_cast<std::uint64_t>(found->second) + 1, session.generation}, static_cast<std::uint32_t>(r), asset.unavailableCode});
        if (asset.parsed) scene.sequences[r] = asset.parsed->sequences;
        // 合法纯粒子不制造空网格，也不计作几何丢失；原始请求身份始终保留在 states/specs。
        if (asset.drawable || !asset.particleCapability.available) {
            requests.push_back(input.model); geometryRequests.push_back(static_cast<std::uint32_t>(r));
        }
    }
    // 复用原材质/队伍色/替换贴图逻辑；每个资源由实际来源读取器提供。不在每帧调用。
    std::vector<Diagnostic> resourceFailures;
    auto geometryLimits = limits.geometry;
    geometryLimits.allowRigidStaticPreview = limits.allowRigidStaticPreview;
    // 追加准备继承常驻场景的材质模式；缺图变体也须为最终合并场景冻结缺失sidecar。
    scene.dynamicMaterialStates=(resident && resident->scene->dynamicMaterialStates) ||
        std::any_of(state->assets.begin(),state->assets.end(),[](const auto& asset) {
            return asset.unavailableCode.empty() && AnimatedMaterialState(asset.materialTextures.get());
        }) || std::any_of(baked.begin(), baked.end(), [](const auto& mesh) { return mesh.samplerFlags != 3; });
    // 只在显式材质模式下冻结缺失sidecar；迟激活与原KMTF/TXAN共用身份和预算。
    const auto freezeMaterialTextures = [&]() -> Status {
        for (auto& asset:state->assets) if (asset.compiled.Valid() &&
            asset.unavailableCode.empty() && !asset.materialTextures) {
            auto textures=CompileMdxMaterialTextures(*asset.parsed,asset.compiled,std::min<std::uint64_t>(limits.maxAssetBytes,128ull*1024*1024));
            if (!textures.Succeeded()) return {std::move(textures.diagnostics)};
            if (sourceBytes>limits.maxAssetBytes || textures.value->StorageBytes()>limits.maxAssetBytes-sourceBytes)
                return AnimationFailure("ANIMATION_ASSET_BUDGET","混合场景冻结材质贴图布局超过资产预算。");
            sourceBytes+=textures.value->StorageBytes();
            asset.materialTextures=std::make_shared<const CompiledMaterialTextures>(std::move(*textures.value));
        }
        return Status::Ok();
    };
    if (scene.dynamicMaterialStates)
        if (auto status=freezeMaterialTextures(); !status.Succeeded()) return {std::nullopt,std::move(status.diagnostics)};
    auto geometry = BuildAnimatedModelGeometry(requests, [&](std::string_view path) -> Result<std::vector<std::byte>> {
        auto data = read(path,limits.geometry.maxTextureBytes);
        if (!data.Succeeded()) {
            if (!MissingResource(data.diagnostics)) {
                if (data.diagnostics.empty()) resourceFailures.push_back({DiagnosticSeverity::Error,"Render.Animation",
                    "ANIMATION_RESOURCE_INVALID","资源读取器失败时没有提供诊断。",std::nullopt});
                else resourceFailures.insert(resourceFailures.end(), data.diagnostics.begin(), data.diagnostics.end());
            }
            return {std::nullopt,std::move(data.diagnostics)};
        }
        if (data.value->sourceLayer.empty() || data.value->digest.size()!=64 ||
            !std::all_of(data.value->digest.begin(),data.value->digest.end(),[](unsigned char c){return std::isxdigit(c)!=0;})) {
            auto failure=Result<std::vector<std::byte>>::Failure("Render.Animation","ANIMATION_TEXTURE_SOURCE","贴图缺少实际来源或SHA-256。");
            resourceFailures.insert(resourceFailures.end(),failure.diagnostics.begin(),failure.diagnostics.end());
            return failure;
        }
        if (data.value->bytes.size() > limits.geometry.maxTextureBytes) {
            auto failure = Result<std::vector<std::byte>>::Failure("Render.Animation","ANIMATION_TEXTURE_BUDGET","贴图超出读取预算。");
            resourceFailures.insert(resourceFailures.end(), failure.diagnostics.begin(), failure.diagnostics.end());
            return failure;
        }
        scene.sources.push_back({AnimationPathKey(path),data.value->sourceLayer,data.value->digest});
        return Result<std::vector<std::byte>>::Success(std::move(data.value->bytes));
    }, [&](std::string_view path) {
        const auto it = cached.find(std::string(path));
        if (it == cached.end()) return ModelGeometryAsset{};
        const auto& a = state->assets[it->second];
        return a.unavailableCode.empty() ? ModelGeometryAsset{a.parsed,a.compiled.Valid(),
            scene.dynamicMaterialStates ? a.materialTextures : nullptr} : ModelGeometryAsset{};
    }, geometryLimits,baked, [&](std::string_view path, std::span<const std::byte>, std::uint32_t maximum)
        -> std::shared_ptr<const ModelTextureRgba> {
        if (!preloaded || preloaded->maxTextureDimension_ != maximum
            || preloaded->mipPolicyVersion_ != ModelTextureMipPolicyVersion) return {};
        const auto key = AnimationPathKey(path);
        const auto frozen = preloaded->textures_.find(key);
        const auto current = state->resources.find(key); // 正式读取器刚提供的本次来源，不能按路径单独复用。
        if (frozen == preloaded->textures_.end() || current == state->resources.end()
            || frozen->second.sourceLayer != current->second->sourceLayer
            || frozen->second.digest != current->second->digest) return {};
        return std::shared_ptr<const ModelTextureRgba>(preloaded, &preloaded->decoded_.at(frozen->second.index));
    });
    if (!resourceFailures.empty()) return {std::nullopt,std::move(resourceFailures)};
    if (!geometry.Succeeded()) return {std::nullopt,std::move(geometry.diagnostics)};
    for (const auto& d : geometry.value->warnings) if (d.code == "WORLD_MODEL_BUDGET" || d.code == "WORLD_BAKED_BUDGET" ||
        d.code == "WORLD_MODEL_TEXTURE_BUDGET") return fail(d.code.c_str(), d.message.c_str());
    if (!scene.dynamicMaterialStates && !geometry.value->layerSamplerFlags.empty()) {
        const auto& source = std::as_const(*geometry.value);
        for (std::size_t layer=0;layer<source.layers.size();++layer)
            if (source.layers[layer].textureIndex!=WAR3_RENDER_MODEL_NO_TEXTURE && source.layerSamplerFlags[layer]!=3) {
                // 白色fallback没有纹理采样；实际常驻的非3源值才要求新的skin材质能力。
                scene.dynamicMaterialStates=true;
                if (auto status=freezeMaterialTextures(); !status.Succeeded()) return {std::nullopt,std::move(status.diagnostics)};
                break;
            }
    }
    scene.geometry = std::move(*geometry.value);
    for (auto& request : scene.geometry.instanceRequests) if (request != kBakedInstance) request = geometryRequests.at(request);
    scene.geometry.requestedInstances = inputs.size();
    scene.geometry.warnings.insert(scene.geometry.warnings.end(),warnings.begin(),warnings.end());
    if ((!limits.retainUnsupportedStatic && scene.geometry.skippedInstances) ||
        (!resident && scene.geometry.instances.empty() && !scene.particleInstanceCount))
        return fail("ANIMATION_GEOMETRY_UNAVAILABLE", "未生成全部请求实例的可绘制几何。");
    const auto& g = scene.geometry;
    scene.vertices.reserve(g.vertices.size());
    for (std::size_t i=0;i<g.vertices.size();++i) scene.vertices.push_back({g.vertices[i],g.vertexGroups.at(i)});
    scene.meshes.resize(g.meshes.size());
    scene.pickMeshGroups.resize(g.meshes.size());
    scene.drawLayerBindings.resize(g.layers.size()); scene.modelLayerRanges.resize(g.models.size());
    if (!g.layerSamplerFlags.empty()) for (std::size_t i=0;i<g.layers.size();++i)
        scene.drawLayerBindings[i].staticSamplerFlags=g.layerSamplerFlags[i];
    std::vector<std::uint8_t> ghostModels(g.models.size());
    for (std::size_t i=0;i<g.instances.size();++i) if (g.instanceRequests[i]!=kBakedInstance)
        ghostModels[g.instances[i].model]=scene.specs[g.instanceRequests[i]].model.previewGhost ? 1 : 0;
    std::uint64_t pickBytes=g.meshes.size()*sizeof(Range32)+g.models.size()*(sizeof(AnimationPickModel)+sizeof(Range32))+
        g.layers.size()*sizeof(AnimationDrawLayerBinding)+g.layerSamplerFlags.capacity()*sizeof(std::uint32_t);
    scene.meshColors.assign(g.meshes.size(), War3RenderMeshColorV2{{1,1,1}});
    for (std::size_t m=0;m<g.models.size();++m) {
        const auto found = cached.find(g.modelAssetKeys[m]);
        const auto assetIndex = found == cached.end() ? UINT32_MAX : found->second;
        state->modelAssets.push_back(assetIndex);
        const auto* a = assetIndex == UINT32_MAX ? nullptr : &state->assets[assetIndex];
        scene.pickModels.push_back({a ? a->parsed : nullptr,a ? a->collisionNodes : nullptr,a ? a->ownedStaticAsset : nullptr});
        const bool animated = a && a->compiled.Valid();
        const auto& model = g.models[m];
        if (!model.meshCount) return fail("ANIMATION_EMPTY_MODEL","模型没有可绘制网格，不能提交空动画模型。");
        scene.models.push_back({model,animated ? a->compiled.GroupCount() : model.meshCount});
        const auto firstLayer=g.meshes[model.firstMesh].firstLayer;
        const auto& lastMesh=g.meshes[model.firstMesh+model.meshCount-1];
        scene.modelLayerRanges[m]={firstLayer,lastMesh.firstLayer+lastMesh.layerCount-firstLayer};
        std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> materialLayers;
        if (animated) {
            scene.dynamicMaterialAlphas|=a->compiled.HasAnimatedMaterialAlpha();
            scene.dynamicMaterialStates|=AnimatedMaterialState(a->materialTextures.get());
            const auto layouts=a->compiled.MaterialLayerLayouts();
            for (std::uint32_t layer=0;layer<layouts.size();++layer)
                materialLayers.emplace(std::make_pair(layouts[layer].materialIndex,layouts[layer].layerIndex),layer);
        }
        for (std::uint32_t k=0;k<model.meshCount;++k) {
            const auto mesh = model.firstMesh+k;
            const auto range = animated ? a->compiled.Geosets()[g.meshGeosets[mesh]].groups : Range32{k,1};
            scene.meshes[mesh] = {g.meshes[mesh],range.offset,range.count};
            const auto& input=g.meshes[mesh];
            if (input.firstIndex>g.indices.size() || input.indexCount>g.indices.size()-input.firstIndex)
                return fail("ANIMATION_PICK_MAPPING","点选 mesh 的实际索引片段越界。");
            std::map<std::uint32_t,std::array<float,6>> boxes;
            for (std::uint32_t q=0;q<input.indexCount;++q) {
                const auto vertex=static_cast<std::uint64_t>(input.baseVertex)+g.indices[input.firstIndex+q];
                if (vertex>=scene.vertices.size()) return fail("ANIMATION_PICK_MAPPING","点选实际顶点越界。");
                const auto& v=scene.vertices[static_cast<std::size_t>(vertex)];
                if (v.group>=range.count) return fail("ANIMATION_PICK_MAPPING","点选顶点组号不属于实际 mesh。");
                const auto [box,inserted]=boxes.emplace(v.group,std::array<float,6>{v.vertex.x,v.vertex.y,v.vertex.z,v.vertex.x,v.vertex.y,v.vertex.z});
                if (inserted) {
                    pickBytes+=sizeof(AnimationPickGroupBounds);
                    if (pickBytes>limits.maxAssetBytes-sourceBytes)
                        return fail("ANIMATION_PICK_METADATA_BUDGET","点选固定组边界超过原资产预算。");
                } else {
                    const float xyz[]{v.vertex.x,v.vertex.y,v.vertex.z};
                    for (unsigned axis=0;axis<3;++axis) { box->second[axis]=std::min(box->second[axis],xyz[axis]); box->second[axis+3]=std::max(box->second[axis+3],xyz[axis]); }
                }
            }
            scene.pickMeshGroups[mesh]={static_cast<std::uint32_t>(scene.pickGroupBounds.size()),static_cast<std::uint32_t>(boxes.size())};
            for (const auto& [group,box]:boxes) scene.pickGroupBounds.push_back({group,box});
            if (a && a->parsed) {
                const auto geoset = g.meshGeosets.at(mesh);
                if (geoset >= a->parsed->geosets.size())
                    return fail("ANIMATION_COLOR_MAPPING", "网格没有对应的原始 geoset，未猜测颜色。");
                const auto material=a->parsed->geosets[geoset].materialId;
                if (material>=a->parsed->materials.size() || g.meshes[mesh].layerCount!=a->parsed->materials[material].layers.size())
                    return fail("ANIMATION_LAYER_ALPHA_MAPPING","实际 draw 层与原 material/layer 数量不同。");
                for (std::uint32_t layer=0;layer<g.meshes[mesh].layerCount;++layer) {
                    auto& binding=scene.drawLayerBindings[g.meshes[mesh].firstLayer+layer];
                    binding.previewAlphaScale=ghostModels[m] ? 0.45f : 1.0f;
                    if (scene.dynamicMaterialStates) {
                        const auto id=a->parsed->materials[material].layers[layer].textureId;
                        if (id>=a->parsed->textures.size() || a->parsed->textures[id].flags>3)
                            return fail("ANIMATION_TEXTURE_SAMPLER_UNSUPPORTED","真实静态TEXS sampler flags尚未支持，未猜测环绕。");
                        binding.staticSamplerFlags=a->parsed->textures[id].flags;
                    }
                    if (animated) {
                        const auto foundLayer=materialLayers.find({material,layer});
                        if (foundLayer==materialLayers.end()) return fail("ANIMATION_LAYER_ALPHA_MAPPING","编译材质布局缺少实际 draw 层。");
                        binding.materialLayer=foundLayer->second;
                    }
                }
                const auto& animation = a->parsed->geosets[geoset].animation;
                const auto color = animated ? a->compiled.GeosetColors()[geoset] : animation ?
                    CompileGeosetColor(animation->flags,animation->staticColor,animation->hasUninterpretedData) : GeosetColor{};
                if (color.error != GeosetColorError::None) {
                    if (!(limits.retainUnsupportedStatic && animated && PreviewColorFailure(color.error)))
                        return fail(color.error == GeosetColorError::Uninterpreted ? "MDX_GEOA_UNINTERPRETED" : "MDX_GEOA_COLOR_INVALID",
                            "GEOA 颜色无法完整表达；不使用白色、静态兜底或贴图烘焙代替。");
                    std::fill_n(scene.meshColors[mesh].rgb,3,0.0f); // 当前帧 alpha=0 才是隐藏依据；不是成功颜色。
                } else std::copy(color.rgb.begin(),color.rgb.end(),scene.meshColors[mesh].rgb);
                if (animated && animation && (animation->flags & 2u) && animation->hasColorTrack)
                    scene.dynamicColors = true;
            }
        }
    }
    for (const auto& a:state->assets) if (a.collisionNodes) pickBytes+=a.collisionNodes->size()*sizeof(std::uint32_t);
    // 显式通用材质尾部包含 alpha；KMTF-only模型也消费真实静态层alpha，不能重复预乘。
    scene.dynamicMaterialAlphas|=scene.dynamicMaterialStates;
    if (auto status=PromoteMaterialTextureBindings(scene,state->assets,state->modelAssets,limits.geometry); !status.Succeeded())
        return {std::nullopt,std::move(status.diagnostics)};
    for (const auto& bindings:std::as_const(scene.geometry).modelTextureBindings)
        if (bindings) pickBytes+=bindings->capacity()*sizeof(ModelTextureBinding);
    if (pickBytes>limits.maxAssetBytes-sourceBytes)
        return fail("ANIMATION_PICK_METADATA_BUDGET","点选固定来源和节点索引超过原资产预算。");
    state->sourceBytes=sourceBytes;
    for (const auto& a:state->assets) if (!a.source.virtualPath.empty()) scene.sources.push_back(a.source);
    return Output::Success(std::move(state));
} catch (const std::bad_alloc&) {
    return Result<std::unique_ptr<State>>::Failure("Render.Animation","ANIMATION_ALLOCATION_FAILED","动画资产内存分配失败。");
} catch (const std::exception& e) {
    return Result<std::unique_ptr<State>>::Failure("Render.Animation","ANIMATION_BUILD_FAILED",e.what());
}
Result<std::shared_ptr<const AnimationPreparedAssets>> ModelAnimationService::PrepareStaticAssets(
    const ModelInstanceRequest& request, const AnimationResourceReader& read,
    std::shared_ptr<const AnimationAssetSnapshot> preloaded) const try {
    using Output = Result<std::shared_ptr<const AnimationPreparedAssets>>;
    if (request.previewGhost) return Output::Failure("Render.Animation", "ANIMATION_PREPARED_GHOST_REQUEST",
        "正式静态预取不接收ghost材质请求，预览与正式材质必须分开准备。");
    const auto& base = *state_->scene;
    AnimationInstanceSpec spec; spec.identity = {1, base.generation}; spec.hostKey = "static-asset-preparation";
    spec.model = request; spec.model.position = {}; spec.model.angle = 0; spec.model.scale = {1, 1, 1};
    spec.model.tint = {1, 1, 1}; spec.model.selection.reset(); spec.model.animationProperties.clear();
    auto built = BuildStaticAssets({base.generation, base.documentRevision, base.sourceIdentity}, std::span(&spec, 1),
        read, state_->limits, {}, state_.get(), std::move(preloaded));
    if (!built.Succeeded()) return {std::nullopt, std::move(built.diagnostics)};
    const auto& prepared = **built.value;
    if (auto budget = CheckAnimationStaticAppend(base, *prepared.scene, state_->limits.geometry); !budget.Succeeded())
        return {std::nullopt, std::move(budget.diagnostics)};
    auto result = std::shared_ptr<AnimationPreparedAssets>(new AnimationPreparedAssets);
    result->baseSceneVersion_ = base.sceneVersion; result->baseDocumentRevision_ = base.documentRevision;
    result->generation_ = base.generation; result->sourceIdentity_ = base.sourceIdentity;
    result->assetKey_ = ModelAssetKey(request); result->replacementKey_ = AnimationPathKey(request.replaceableTexture);
    result->teamColor_ = request.teamColor; result->assetIndex_ = prepared.requestAssets.at(0);
    result->newAsset_ = result->assetIndex_ == state_->assets.size();
    result->asset_ = prepared.assets.at(result->assetIndex_); result->asset_.evaluatorIndex = UINT32_MAX;
    if (prepared.scene->dynamicMaterialStates) {
        // 首次启用KMTF也会为存活静态资产冻结sidecar；这些元数据须与新增资产一起带回正式catalog。
        // 只共享不可变编译结果，不捕获实例或时钟，也不在正式点击时重新编译/读取。
        result->materialTextures_.reserve(prepared.assets.size());
        for (const auto& asset:prepared.assets) result->materialTextures_.push_back(asset.materialTextures);
    }
    result->collisionNodesRequired_ = prepared.collisionNodesRequired;
    result->addedGeometry_ = AnimationStaticGeometry::Capture(*prepared.scene);
    result->modelAssets_ = prepared.modelAssets;
    if (!prepared.scene->geometry.instances.empty()) result->modelIndex_ = prepared.scene->geometry.instances.at(0).model;
    result->sourceBytes_ = prepared.sourceBytes - state_->sourceBytes;
    result->resourceBytes_ = prepared.resourceBytes - state_->resourceBytes;
    // exact base中的旧资源已经冻结，来源租约仍由宿主前后Verify负责；
    // 这里只留下新静态请求实际触及的新正项/负项，不对整张地图重读。
    for (const auto& access : prepared.staticResourceAccesses)
        if (!access.resource || !state_->resources.contains(access.path)) result->accesses_.push_back(access);
    for (const auto& [path, resource] : prepared.resources)
        if (!state_->resources.contains(path)) result->resources_.emplace(path, resource);
    std::erase_if(result->addedGeometry_.sources, [&](const auto& source) {
        return source.virtualPath != result->assetKey_ && !result->resources_.contains(AnimationPathKey(source.virtualPath));
    });
    // 唯一静态追加逻辑在预取阶段完成旧世界缓冲的COW分离；包不持有原State或实例。
    // 正式点击只共享这份不可变合并结果，仍要复核exact catalog、实际来源和累计预算。
    AnimatedModelScene merged; CopyStaticGeometry(merged, base);
    if (auto budget=AppendAnimationStaticGeometry(merged, result->addedGeometry_.Materialize(),state_->limits.geometry); !budget.Succeeded())
        return {std::nullopt,std::move(budget.diagnostics)};
    if (merged.dynamicMaterialStates) {
        auto modelAssets=state_->modelAssets;
        modelAssets.insert(modelAssets.end(),prepared.modelAssets.begin(),prepared.modelAssets.end());
        if (auto promoted=PromoteMaterialTextureBindings(merged,prepared.assets,modelAssets,state_->limits.geometry); !promoted.Succeeded())
            return {std::nullopt,std::move(promoted.diagnostics)};
    }
    result->mergedGeometry_ = AnimationStaticGeometry::Capture(merged);
    return Output::Success(std::move(result));
} catch (const std::bad_alloc&) {
    return Result<std::shared_ptr<const AnimationPreparedAssets>>::Failure("Render.Animation", "ANIMATION_ALLOCATION_FAILED",
        "正式静态预取内存分配失败，驻留场景未改变。");
} catch (const std::exception& e) {
    return Result<std::shared_ptr<const AnimationPreparedAssets>>::Failure("Render.Animation", "ANIMATION_PREPARED_BUILD_FAILED", e.what());
}
Status ModelAnimationService::InitializeStorage(State& state, const AnimationLimits& limits,
    const AnimationResourceReader& read, AnimationInstanceDeltaTimings* timings) {
    using Clock = std::chrono::steady_clock;
    auto stageStart = Clock::now();
    const auto record = [&](std::uint64_t AnimationInstanceDeltaTimings::* field) {
        const auto end = Clock::now();
        if (timings) timings->*field += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - stageStart).count());
        stageStart = end;
    };
    auto& scene=*state.scene; const auto& catalog=std::as_const(scene); const auto& g=catalog.geometry;
    const auto fail=[](const char* code,const char* text) { return AnimationFailure(code,text); };
    if (catalog.requestGeometryRequired.size()!=catalog.specs.size() || state.requestAssets.size()!=catalog.specs.size())
        return fail("ANIMATION_GEOMETRY_COVERAGE","实际资产的逐请求几何需求未完整冻结。");
    for (std::size_t r=0;r<catalog.specs.size();++r) {
        const auto asset=state.requestAssets[r];
        if (asset>=state.assets.size() || catalog.requestGeometryRequired[r]>1
            || (catalog.requestGeometryRequired[r]!=0)!=state.assets[asset].drawable)
            return fail("ANIMATION_GEOMETRY_COVERAGE","逐请求几何需求与实际解析资产不一致。");
    }
    auto identity=EnsureAnimationCatalogIdentityIndex(scene,state.catalogIdentity,limits.maxFrameBytes);
    if (!identity.Succeeded()) return {std::move(identity.diagnostics)};
    state.catalogIdentity=std::move(*identity.value);
    if (auto promoted=PromoteMaterialTextureBindings(scene,state.assets,state.modelAssets,limits.geometry); !promoted.Succeeded()) return promoted;
    std::vector<CompiledModel> compiled;
    std::vector<CompiledMaterialTextures> textureAssets;
    std::uint32_t maxNodes=1,maxGroups=1,maxGeosets=1,maxMaterialLayers=1;
    state.maxGeosetIssues=state.maxCollisionMatrices=state.maxMaterialIssues=0;
    state.sharedPoseRanges=false; state.evaluator.reset(); state.particles.reset(); state.visibility.reset();
    state.standFrame.reset(); state.standPoses.clear(); state.standPoseIndices.clear();
    state.modelStandPoses.clear(); state.activeSlots.clear(); state.modelSlots.clear();
    state.sharedGroupCount=state.sharedAlphaCount=state.sharedLayerAlphaCount=0;
    scene.animatedInstances=scene.retainedStaticInstances=scene.budgetStaticInstances=0;
    // 粒子贴图属于驻留静态资产；重绑实例只重建工作区，不重解码或清掉纹理池。
    for (auto& asset:state.assets) if (asset.compiled.Valid() && asset.unavailableCode.empty()) {
        asset.evaluatorIndex=static_cast<std::uint32_t>(compiled.size());
        compiled.push_back(asset.compiled);
        if (scene.dynamicMaterialStates) {
            if (!asset.materialTextures || !asset.materialTextures->Matches(asset.compiled))
                return fail("ANIMATION_TEXTURE_OWNER_INVALID","材质贴图sidecar不属于当前冻结模型。");
            textureAssets.push_back(*asset.materialTextures);
        }
        maxNodes=std::max(maxNodes,asset.compiled.NodeCount());
        maxGroups=std::max(maxGroups,asset.compiled.GroupCount());
        maxGeosets=std::max(maxGeosets,asset.compiled.GeosetCount());
        maxMaterialLayers=std::max(maxMaterialLayers,asset.compiled.MaterialLayerCount());
    }
    std::uint64_t groups=0,alphas=0,layerAlphas=0;
    for (std::size_t i=0;i<g.instances.size();++i) {
        const auto m = g.instances[i].model;
        groups += catalog.models[m].groupCount; alphas += g.models[m].meshCount;
        if (scene.dynamicMaterialAlphas) layerAlphas+=catalog.modelLayerRanges[m].count;
        const auto a = state.modelAssets[m];
        if (a != UINT32_MAX && state.assets[a].compiled.Valid()) {
            ++scene.animatedInstances;
            if (limits.retainUnsupportedStatic) state.maxGeosetIssues += state.assets[a].compiled.GeosetCount();
            if (scene.dynamicMaterialAlphas && limits.retainUnsupportedStatic) ++state.maxMaterialIssues;
            if (scene.dynamicMaterialStates && limits.retainUnsupportedStatic) ++state.maxMaterialIssues;
        }
        else ++scene.retainedStaticInstances;
        if (a != UINT32_MAX && state.assets[a].parsed) state.maxCollisionMatrices+=state.assets[a].parsed->collisionShapes.size();
    }
    const auto baseFrameBytes=AnimationRequestGeometryBytes(catalog)+AnimationParticleMaterialOwnerBytes(state.assets.capacity())+
        g.instances.size()*(sizeof(War3RenderSkinnedModelInstanceV2)+sizeof(AnimationPoseLink))+
        scene.specs.size()*sizeof(AnimationInstanceState)+g.instances.size()*sizeof(AnimationPickBounds)+
        (scene.dynamicMaterialAlphas ? g.instances.size()*sizeof(War3RenderMaterialLayerSliceV2) : 0)+
        (scene.dynamicMaterialStates ? g.instances.size()*sizeof(War3RenderMaterialLayerSliceV2) : 0);
    state.frameBytes=baseFrameBytes+groups*sizeof(War3RenderGroupMatrixV2)+
        alphas*(sizeof(float)+sizeof(War3RenderMeshColorV2))+state.maxGeosetIssues*sizeof(AnimationGeosetIssue)+
        state.maxCollisionMatrices*sizeof(AnimationCollisionMatrix)+layerAlphas*sizeof(float)+state.maxMaterialIssues*sizeof(AnimationMaterialIssue)+
        (scene.dynamicMaterialStates ? layerAlphas*sizeof(War3RenderMaterialStateV2) : 0);
    if (groups>1048576 || alphas>1048576 || layerAlphas>1048576 || state.maxGeosetIssues>1048576 || state.maxCollisionMatrices>1048576 ||
        state.frameBytes>limits.maxFrameBytes) {
        if (!limits.retainUnsupportedStatic || baseFrameBytes>=limits.maxFrameBytes)
            return fail("ANIMATION_FRAME_BUDGET","全量实例 owned 帧实际内存超过预算；未截掉地图实例，应保留静态场景。");
        const auto scheduleBytes=g.instances.size()*sizeof(std::uint32_t)+scene.models.size()*sizeof(std::vector<std::pair<double,std::uint32_t>>);
        if (scheduleBytes>=limits.maxFrameBytes-baseFrameBytes)
            return fail("ANIMATION_FRAME_BUDGET","全量实例调度索引实际内存超过预算；未截断地图实例。");
        state.sharedPoseRanges=true; state.frameBytes=baseFrameBytes+scheduleBytes;
    }
    record(&AnimationInstanceDeltaTimings::storageLayoutNs);
    if (!compiled.empty()) {
        // 至多64实例，按最大单模型预热；过大时减小批宽，不增加64MiB求值器上限。
        std::uint32_t width=64;
        for (;;) {
            BatchLimits batch;
            batch.maxInstances=width; batch.maxNodes=maxNodes*width; batch.maxGroups=maxGroups*width;
            batch.maxGeosets=maxGeosets*width; batch.outputMode=state.collisionNodesRequired ? PoseOutputMode::FullNodes : PoseOutputMode::GroupsAndAlphas;
            const auto policy=limits.retainUnsupportedStatic ? GeosetFailurePolicy::PreviewHideInvalidGeosets : GeosetFailurePolicy::Strict;
            const auto nodePolicy=AnimationNodeKeyPolicy(limits);
            const auto materialPolicy=scene.dynamicMaterialAlphas ? AnimationMaterialAlphaKeyPolicy(limits)
                : MaterialAlphaKeySamplingPolicy::Strict;
            auto evaluator=scene.dynamicMaterialStates ? PoseBatchEvaluator::Create(compiled,state.epoch,batch,policy,
                MaterialChannelOptions{1,maxMaterialLayers*width},TextureChannelOptions{1,maxMaterialLayers*width},textureAssets,nodePolicy,materialPolicy) :
                scene.dynamicMaterialAlphas ? PoseBatchEvaluator::Create(compiled,state.epoch,batch,policy,
                MaterialChannelOptions{1,maxMaterialLayers*width},{},{},nodePolicy,materialPolicy) :
                PoseBatchEvaluator::Create(compiled,state.epoch,batch,policy,{},{},{},nodePolicy,materialPolicy);
            const bool ownsWorkspace=state.collisionNodesRequired || state.sharedPoseRanges || scene.dynamicMaterialAlphas;
            if (evaluator.Succeeded() && (!ownsWorkspace || evaluator.value->StorageBytes()<=limits.maxFrameBytes-state.frameBytes)) {
                if (ownsWorkspace) state.frameBytes+=evaluator.value->StorageBytes();
                state.evaluator=std::make_unique<PoseBatchEvaluator>(std::move(*evaluator.value));
                state.batchWidth=width; break;
            }
            if (width==1) {
                if (evaluator.Succeeded()) return fail("ANIMATION_FRAME_BUDGET","碰撞节点批内输出和 owned 帧超过原帧预算。");
                return {std::move(evaluator.diagnostics)};
            }
            width/=2;
        }
    }
    record(&AnimationInstanceDeltaTimings::storageEvaluatorNs);
    if (state.sharedPoseRanges) {
        if (auto storage=PrepareStandStorage(state,limits.maxFrameBytes,limits.maxActivePoseBytes); !storage.Succeeded())
            return storage;
    }
    record(&AnimationInstanceDeltaTimings::storageStandNs);
    if (limits.enablePremPreview) {
        for (auto& asset : state.assets) {
            asset.particleCapability.premPreparedCount = 0;
            asset.particleCapability.rejectionCode = asset.originalParticleRejectionCode;
        }
        if (auto status = CreateAnimationPrem(state, FrozenAssetReader(state, read, false)); !status.Succeeded()) return status;
        if (state.prem && state.prem->capabilities) {
            for (const auto& capability : *state.prem->capabilities) {
                if (capability.prepared && capability.parentAssetIndex < state.assets.size())
                    ++state.assets[capability.parentAssetIndex].particleCapability.premPreparedCount;
            }
            for (std::size_t assetIndex = 0; assetIndex < state.assets.size(); ++assetIndex) {
                auto& asset = state.assets[assetIndex];
                auto& capability = asset.particleCapability;
                const bool disabledOtherKinds = capability.rejectionCode == "PARTICLE_PREVIEW_DISABLED"
                    && (capability.particle2Count || capability.ribbonCount);
                if (!capability.premCount || disabledOtherKinds || (!capability.rejectionCode.empty()
                    && capability.rejectionCode != "PARTICLE_PREM_UNSUPPORTED"
                    && capability.rejectionCode != "PARTICLE_PREVIEW_DISABLED")) continue;
                const auto rejected = std::find_if(state.prem->capabilities->begin(), state.prem->capabilities->end(),
                    [&](const auto& item) { return item.parentAssetIndex == assetIndex && !item.prepared && !item.rejectionCode.empty(); });
                capability.rejectionCode = capability.premPreparedCount == capability.premCount ? ""
                    : capability.premPreparedCount ? "PREM_BASIC_PARTIAL"
                    : rejected != state.prem->capabilities->end() ? rejected->rejectionCode : "PARTICLE_PREM_UNSUPPORTED";
            }
            for (std::size_t r = 0; r < state.requestAssets.size(); ++r) {
                const auto& prepared = state.assets[state.requestAssets[r]].particleCapability;
                auto& capability = scene.particleCapabilities[r]; capability.premPreparedCount = prepared.premPreparedCount;
                capability.rejectionCode = prepared.rejectionCode;
            }
        }
    }
    if (limits.enableParticles) {
        auto particles = CreateAnimationParticles(scene, state.assets, state.requestAssets, compiled,
            FrozenAssetReader(state, read, false, state.prem ? state.prem->extraAssetBytes : 0), limits, state.epoch, state.frameBytes);
        if (!particles.Succeeded()) return {std::move(particles.diagnostics)};
        state.particles = std::move(*particles.value);
    }
    if (state.prem) {
        const auto resident = state.sourceBytes + state.resourceBytes;
        state.prem->budget.parentAssetBytesAfter = resident;
        if (resident > limits.maxAssetBytes || state.prem->extraAssetBytes > limits.maxAssetBytes - resident)
            return fail("ANIMATION_ASSET_BUDGET", "全部粒子读取完成后的父资源与 PREM 子资产超过原预算。");
    }
    record(&AnimationInstanceDeltaTimings::storageParticlesNs);
    return Status::Ok();
}
} // namespace War3Map::Render
