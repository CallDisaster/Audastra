#include "AnimationParticles.h"
#include "War3MapCore/Resources/ClassicObjectTexture.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace War3Map::Render {
using namespace War3RenderCore;
namespace {
// 独立函数避免 MSVC 14.50 在捕获局部 constexpr 表的 lambda 上产生 C1001。
Status PrepareParticleMaterial(const MdxModel& model, Particles::Emitter& e, float& alpha, bool& unfogged) {
    constexpr Particles::Filter filters[]{Particles::Filter::Opaque, Particles::Filter::AlphaKey,
        Particles::Filter::Blend, Particles::Filter::Additive, Particles::Filter::AddAlpha,
        Particles::Filter::Modulate, Particles::Filter::Modulate2x};
    if (e.kind == Particles::Kind::Ribbon) {
        if (e.materialId >= model.materials.size())
            return AnimationFailure("PARTICLE_MATERIAL_UNSUPPORTED", "条带材质索引不存在。");
        const auto& m = model.materials[e.materialId];
        if (m.layers.size() != 1 || m.flags || m.priorityPlane || m.hasUninterpretedData)
            return AnimationFailure("PARTICLE_MATERIAL_UNSUPPORTED", "条带只支持无未知尾部的单层静态材质。");
        const auto& layer = m.layers.front();
        if (layer.animatedTexture || layer.animatedAlpha || layer.textureAnimationId != UINT32_MAX ||
            layer.coordId || layer.hasUninterpretedData || (layer.shadingFlags & ~0x31u) ||
            !(layer.shadingFlags & 1u) || !std::isfinite(layer.alpha) || layer.alpha < 0 || layer.alpha > 1)
            return AnimationFailure("PARTICLE_MATERIAL_UNSUPPORTED", "条带动态材质、光照、UV 或深度标志尚未完整表达。");
        const auto filter = static_cast<std::uint32_t>(layer.filterMode);
        if (filter >= std::size(filters)) return AnimationFailure("PARTICLE_MATERIAL_UNSUPPORTED", "未知条带过滤模式。");
        e.textureId = layer.textureId; e.filter = filters[filter]; alpha = layer.alpha;
        unfogged = (layer.shadingFlags & 0x20u) != 0;
    }
    if (!e.replaceableId && e.textureId >= model.textures.size())
        return AnimationFailure("PARTICLE_TEXTURE_INVALID", "粒子贴图索引不存在，未使用白色替代。");
    return {};
}
bool OnlyMissingResource(std::span<const Diagnostic> diagnostics) {
    bool error = false;
    for (const auto& d : diagnostics) if (d.severity == DiagnosticSeverity::Error) {
        error = true;
        if (d.code != "RESOURCE_NOT_FOUND" && d.code != "file-not-found" && d.code != "WORLD_ASSET_MISSING") return false;
    }
    return error;
}
bool ValidTextureResource(const AnimationResource& resource, std::uint64_t maximum) {
    return resource.bytes.size() <= maximum && !resource.sourceLayer.empty() && resource.digest.size() == 64
        && std::all_of(resource.digest.begin(), resource.digest.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
} // namespace
Status ReadAnimationParticleAsset(AnimationAsset& a, std::span<const std::byte> bytes,
    const AnimationLimits& limits) {
    const bool retainUnsupportedStatic = limits.retainUnsupportedStatic;
    auto& capability = a.particleCapability;
    ForEachMdxNode(*a.parsed, [&](const MdxNode& n) {
        capability.particle2Count += n.kind == MdxNodeKind::ParticleEmitter2;
        capability.ribbonCount += n.kind == MdxNodeKind::Ribbon;
        capability.premCount += n.kind == MdxNodeKind::ParticleEmitter;
    });
    if (!capability.particle2Count && !capability.ribbonCount && !capability.premCount) return {};
    if (!limits.enableParticles) { capability.rejectionCode = "PARTICLE_PREVIEW_DISABLED"; return {}; }
    if (capability.premCount) capability.rejectionCode = "PARTICLE_PREM_UNSUPPORTED";
    if (!capability.particle2Count && !capability.ribbonCount) return {};
    if (!a.compiled.Valid()) return AnimationFailure("PARTICLE_ANCHOR_UNAVAILABLE", "粒子需要完整的已编译节点图。");
    // 原版取键顺序与隔离坏发射器是两个独立选择。模型动作窗口必须能使用原版顺序，
    // 同时保留无效数值、全局序列、材质和结构的失败，不静默退回部分显示。
    const auto mode = retainUnsupportedStatic ? Particles::ReadMode::PreviewSkipInvalidEmitters :
        limits.playbackPolicy == AnimationPlaybackPolicy::InteractivePreview
            ? Particles::ReadMode::PreviewGameOrder : Particles::ReadMode::Strict;
    auto read = Particles::Read(bytes, {}, mode);
    if (!read.Succeeded()) return {std::move(read.diagnostics)};
    auto particles = std::make_shared<Particles::Asset>(std::move(*read.value));
    std::unordered_map<std::uint32_t, std::uint32_t> nodes;
    for (std::uint32_t i = 0; i < a.compiled.NodeCount(); ++i) {
        const auto node = a.compiled.Node(i); nodes.emplace(node.objectId, i);
        a.cameraDependent |= (node.flags & 0xF8u) != 0;
    }
    // 这里只验证已界定发射器的材质内容与索引；锚点、资源读取和预算仍走原拒绝路径。
    std::vector<std::uint32_t> emitterNodes;
    std::vector<float> layerAlpha;
    std::vector<std::uint32_t> materialFlags;
    emitterNodes.reserve(particles->emitters.size()); layerAlpha.reserve(particles->emitters.size());
    materialFlags.reserve(particles->emitters.size());
    std::size_t retained = 0;
    bool unfoggedPreview = false;
    for (auto& e : particles->emitters) {
        const auto node = nodes.find(e.nodeId);
        if (node == nodes.end()) return AnimationFailure("PARTICLE_ANCHOR_UNAVAILABLE", "发射器没有对应的完整父链节点。");
        float alpha = 1; bool unfogged = false;
        auto material = PrepareParticleMaterial(*a.parsed, e, alpha, unfogged);
        if (!material.Succeeded()) {
            if (!retainUnsupportedStatic) return material;
            const auto& diagnostic = material.diagnostics.front();
            particles->skippedEmitters.push_back({e.kind, e.sourceIndex, e.nodeId, diagnostic.code, diagnostic.message});
            // 读取时的窄恢复不等于最终可显示；材质隔离后撤销同一来源轨迹的恢复记录。
            std::erase_if(particles->recoveredVisibilityTracks, [&](const auto& recovery) {
                if (recovery.kind != e.kind || recovery.sourceIndex != e.sourceIndex) return false;
                std::erase_if(read.diagnostics, [&](const auto& issue) {
                    return issue.code == "PARTICLE_VISIBILITY_DUPLICATE_PREVIEW" && issue.origin
                        && issue.origin->offset == recovery.sourceByteOffset;
                });
                return true;
            });
            std::erase_if(particles->gameOrderTracks, [&](const auto& order) {
                if (order.kind != e.kind || order.sourceIndex != e.sourceIndex) return false;
                std::erase_if(read.diagnostics, [&](const auto& issue) {
                    return issue.code == "PARTICLE_KEYS_GAME_ORDER_PREVIEW" && issue.origin
                        && issue.origin->offset == order.sourceByteOffset;
                });
                return true;
            });
            read.diagnostics.push_back({DiagnosticSeverity::Warning, diagnostic.subsystem, diagnostic.code,
                std::string(e.kind == Particles::Kind::Particle2 ? "PRE2" : "RIBB") + " 源记录 " +
                std::to_string(e.sourceIndex) + " 已隔离：" + diagnostic.message, diagnostic.origin});
            continue;
        }
        emitterNodes.push_back(node->second); layerAlpha.push_back(alpha);
        materialFlags.push_back(unfogged ? WAR3_PARTICLE_UNFOGGED : 0u);
        unfoggedPreview |= unfogged;
        if (&e != &particles->emitters[retained]) particles->emitters[retained] = std::move(e);
        ++retained;
    }
    particles->emitters.resize(retained);
    // 验证失败不会把节点、透明度或部分发射器安装到资产；有效记录保持源次序。
    a.emitterNodes = std::move(emitterNodes); a.particleLayerAlpha = std::move(layerAlpha);
    a.particleMaterialFlags = std::make_shared<const std::vector<std::uint32_t>>(std::move(materialFlags));
    a.particleUnfoggedPreview |= unfoggedPreview;
    if (!particles->skippedEmitters.empty()) {
        auto issues = std::make_shared<std::vector<AnimationParticleIssue>>();
        issues->reserve(particles->skippedEmitters.size());
        for (const auto& skipped : particles->skippedEmitters)
            issues->push_back({skipped.kind == Particles::Kind::Particle2 ? "PRE2" : "RIBB",
                skipped.sourceIndex, skipped.nodeId, skipped.code, skipped.message});
        capability.skippedEmitters = std::move(issues);
        if (capability.rejectionCode.empty() || particles->emitters.empty())
            capability.rejectionCode = capability.skippedEmitters->front().code;
    }
    capability.available = !particles->emitters.empty();
    // PRE2/RIBB 读取器不实现 PREM，但启用的独立 PREM 主机会随后给出逐发射器能力结果。
    // 只撤掉读取器这条笼统提示；主机拒绝、子资产失败及其它粒子诊断保持原样。
    if (limits.enablePremPreview) std::erase_if(read.diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == "PARTICLE_PREM_UNSUPPORTED";
    });
    for (auto& diagnostic : read.diagnostics) {
        if ((diagnostic.code != "PARTICLE_VISIBILITY_DUPLICATE_PREVIEW"
                && diagnostic.code != "PARTICLE_KEYS_GAME_ORDER_PREVIEW") || !diagnostic.origin) continue;
        diagnostic.origin->source = std::filesystem::path(
            std::u8string(a.source.virtualPath.begin(), a.source.virtualPath.end()));
        diagnostic.origin->fingerprint = a.source.contentDigest;
        diagnostic.message += " 来源层：" + a.source.sourceLayer;
    }
    a.particles = std::move(particles);
    return {std::move(read.diagnostics)};
}

Result<std::shared_ptr<AnimationParticleHost>> CreateAnimationParticles(AnimatedModelScene& scene,
    std::span<const AnimationAsset> assets, std::span<const std::uint32_t> requestAssets,
    std::span<const CompiledModel> compiled, const AnimationResourceReader& read,
    const AnimationLimits& limits, std::uint64_t epoch, std::uint64_t fixedFrameBytes) {
    using Out = Result<std::shared_ptr<AnimationParticleHost>>;
    const auto fail = [](const char* code, const char* message) { return Out::Failure("Render.Animation", code, message); };
    const auto& p = limits.particles;
    if (p.maxParticles > 65536 || p.maxRibbonSegments > 32768 || p.maxPerEmitter > 4096 ||
        !p.maxAnchorSamples || p.maxAnchorSamples > 6000 || !p.maxAnchorNodeVisits || p.maxAnchorNodeVisits > 4194304 ||
        p.maxVertices > 1048576 || p.maxDraws > 65536 || !p.maxWorldFrameSteps || p.maxWorldFrameSteps>128)
        return fail("ANIMATION_PARTICLE_BUDGET", "粒子预算超出场景硬上限。");
    auto host = std::make_shared<AnimationParticleHost>();
    host->limits = p; host->maxFrameBytes = limits.maxFrameBytes; host->fixedFrameBytes = fixedFrameBytes;
    host->retainUnsupportedStatic = limits.retainUnsupportedStatic;
    host->playbackPolicy = limits.playbackPolicy;
    host->simulation = {p.maxParticles, p.maxRibbonSegments, p.maxPerEmitter, p.maxAnchorSamples};
    std::unordered_map<std::string, std::uint32_t> textures;
    std::unordered_map<std::string, std::string> objectTexturePaths;
    std::unordered_set<std::string> missingTextures; // 同一模型的多个实例不重复走全部来源查找
    std::uint64_t textureBytes = 0, modelTextureBytes = 0;
    const auto& geometry=std::as_const(scene.geometry);
    const auto& residentTextures=std::as_const(scene.particleTextures);
    for (const auto& t : geometry.textures)
        if (!ChargeModelTextureRgba(t,modelTextureBytes,limits.geometry.maxTotalTextureBytes))
            return fail("ANIMATION_PARTICLE_TEXTURE_BUDGET","模型完整mip链超过总预算。");
    for (std::uint32_t i=0;i<residentTextures.size();++i) {
        textures.emplace(AnimationPathKey(residentTextures[i].path),i);
        if (!ChargeModelTextureRgba(residentTextures[i],textureBytes,64ull*1024*1024))
            return fail("ANIMATION_PARTICLE_TEXTURE_BUDGET","常驻粒子完整mip链超过预算。");
    }
    if (textures.size()>256 || textureBytes>64ull*1024*1024 ||
        modelTextureBytes>limits.geometry.maxTotalTextureBytes || textureBytes>limits.geometry.maxTotalTextureBytes-modelTextureBytes)
        return fail("ANIMATION_PARTICLE_TEXTURE_BUDGET", "驻留模型和粒子解码贴图超过总预算。");
    std::uint32_t particleEmitters = 0, ribbonEmitters = 0, maxNodes = 1, maxGroups = 1, maxGeosets = 1;
    std::uint64_t bindingBytes=sizeof(AnimationParticleCheckpoint)+scene.specs.size()*sizeof(std::uint32_t);
    // 表随冻结资产常驻，所有实例只持有同一const owner；按唯一实际拥有表收费一次。
    scene.particleUnfogged = false;
    std::uint64_t materialTableBytes=0;
    if (host->fixedFrameBytes>limits.maxFrameBytes)
        return fail("ANIMATION_FRAME_BUDGET", "粒子固定身份存储超过帧预算。");
    {
        // 临时指针索引排序去重；不对大量资产做平方扫描，也不复制不可变payload。
        const auto tableCount=std::count_if(assets.begin(),assets.end(),[](const auto& asset) { return bool(asset.particleMaterialFlags); });
        std::vector<const std::vector<std::uint32_t>*> tables;
        if (std::uint64_t(tableCount)*sizeof(tables.front())>limits.maxFrameBytes-host->fixedFrameBytes)
            return fail("ANIMATION_FRAME_BUDGET", "粒子材质表去重索引超过工作预算。");
        tables.reserve(static_cast<std::size_t>(tableCount));
        const auto indexBytes=std::uint64_t(tables.capacity())*sizeof(tables.front());
        for (const auto& asset:assets) if (asset.particleMaterialFlags) tables.push_back(asset.particleMaterialFlags.get());
        std::sort(tables.begin(),tables.end(),std::less<>{});
        const auto uniqueEnd=std::unique(tables.begin(),tables.end());
        for (auto i=tables.begin();i!=uniqueEnd;++i)
            materialTableBytes+=sizeof(**i)+std::uint64_t((*i)->capacity())*sizeof(std::uint32_t);
        if (indexBytes>limits.maxFrameBytes-host->fixedFrameBytes ||
            materialTableBytes>limits.maxFrameBytes-host->fixedFrameBytes-indexBytes)
            return fail("ANIMATION_FRAME_BUDGET", "粒子不可变材质表与临时去重索引超过帧预算。");
    }
    host->fixedFrameBytes+=materialTableBytes;
    if (bindingBytes>limits.maxFrameBytes-host->fixedFrameBytes && limits.retainUnsupportedStatic) {
        for (auto& capability:scene.particleCapabilities) if (capability.available) {
            capability.available=false; capability.rejectionCode="ANIMATION_PARTICLE_INSTANCE_BUDGET";
        }
        host->omitRejectedLinks=true; scene.particleInstanceCount=0; return Out::Success(std::move(host));
    }
    const auto rejected=std::count_if(scene.particleCapabilities.begin(),scene.particleCapabilities.end(),[](const auto& c) {
        return !c.available && (c.particle2Count || c.ribbonCount || c.premCount);
    });
    const auto rejectedBytes=static_cast<std::uint64_t>(rejected)*sizeof(AnimationParticleLink);
    if (bindingBytes<=limits.maxFrameBytes-host->fixedFrameBytes && rejectedBytes<=limits.maxFrameBytes-host->fixedFrameBytes-bindingBytes)
        bindingBytes+=rejectedBytes;
    else host->omitRejectedLinks=true;
    for (std::uint32_t r = 0; r < requestAssets.size(); ++r) {
        const auto& a = assets[requestAssets[r]];
        if (!a.particleCapability.available || !a.unavailableCode.empty()) continue;
        const auto emitterCount=a.particles->emitters.size();
        if (!a.particleMaterialFlags || a.particleMaterialFlags->size()!=emitterCount)
            return fail("ANIMATION_PARTICLE_MAPPING", "不可变粒子材质表与已接受发射器数量不符。");
        // 这里只收费实际绑定/两帧回执与共享检查点索引；尚未获准模拟的地图实例不预分配粒子池。
        const auto cost=2*sizeof(AnimationParticleBinding)+2*sizeof(AnimationParticleLink)+
            2*sizeof(std::shared_ptr<const AnimationParticleState>)+2*sizeof(std::uint8_t)+
            sizeof(AnimationParticleAdmission)+3*sizeof(std::size_t)+sizeof(double)+
            emitterCount*(3*sizeof(std::uint32_t)+sizeof(float));
        if (bindingBytes>limits.maxFrameBytes-host->fixedFrameBytes ||
            cost>limits.maxFrameBytes-host->fixedFrameBytes-bindingBytes) {
            if (!limits.retainUnsupportedStatic)
                return fail("ANIMATION_PARTICLE_BUDGET","实际发射器/身份内存超过模拟预算。");
            auto& capability=scene.particleCapabilities[r]; capability.available=false;
            capability.rejectionCode="ANIMATION_PARTICLE_INSTANCE_BUDGET"; continue;
        }
        bindingBytes+=cost;
        AnimationParticleBinding binding; binding.requestIndex = r; binding.assetIndex = requestAssets[r];
        std::uint32_t bindingParticles = 0, bindingRibbons = 0;
        bool textureMissing = false;
        for (const auto& e : a.particles->emitters) {
            if (e.kind == Particles::Kind::Particle2) ++bindingParticles; else ++bindingRibbons;
            MdxTexture texture;
            if (e.replaceableId) texture.replaceableId = e.replaceableId;
            else texture = a.parsed->textures[e.textureId];
            auto path = ModelTexturePath(texture, std::as_const(scene).specs[r].model);
            if (path.empty()) return fail("PARTICLE_TEXTURE_INVALID", "粒子没有可解析贴图路径，未填白。");
            const bool objectReplacement = texture.replaceableId >= 3;
            const auto requestKey = std::string(objectReplacement ? "object:" : "exact:") + AnimationPathKey(path);
            if (missingTextures.contains(requestKey)) { textureMissing = true; break; }
            std::optional<AnimationResource> selectedResource;
            if (objectReplacement) {
                if (const auto found = objectTexturePaths.find(requestKey); found != objectTexturePaths.end()) path = found->second;
                else {
                    auto selected = Resources::ReadClassicObjectTexture<AnimationResource>(path,
                        [&](std::string_view candidate) { return read(candidate, limits.geometry.maxTextureBytes); });
                    if (!selected.Succeeded()) {
                        if (!limits.retainUnsupportedStatic || !OnlyMissingResource(selected.diagnostics))
                            return {std::nullopt, std::move(selected.diagnostics)};
                        missingTextures.insert(requestKey); textureMissing = true; break;
                    }
                    path = std::move(selected.value->path);
                    selectedResource = std::move(selected.value->resource);
                    if (!ValidTextureResource(*selectedResource, limits.geometry.maxTextureBytes))
                        return fail("ANIMATION_TEXTURE_SOURCE", "粒子贴图缺少真实来源、SHA-256 或超过读取预算。");
                    objectTexturePaths.emplace(requestKey, path);
                }
            }
            const auto key = AnimationPathKey(path);
            auto found = textures.find(key);
            if (found == textures.end()) {
                if (textures.size() >= 256) return fail("ANIMATION_PARTICLE_TEXTURE_BUDGET", "粒子场景贴图超过 256。");
                ModelTextureRgba decoded;
                const auto existing = std::find_if(geometry.textures.begin(), geometry.textures.end(),
                    [&](const auto& t) { return AnimationPathKey(t.path) == key; });
                const auto remaining=std::min(64ull*1024*1024-textureBytes,
                    limits.geometry.maxTotalTextureBytes-modelTextureBytes-textureBytes);
                if (existing != geometry.textures.end()) {
                    if (!ModelTextureValid(*existing,limits.geometry.maxTextureDimension) || ModelTextureRgbaBytes(*existing)>remaining)
                        return fail("ANIMATION_PARTICLE_TEXTURE_BUDGET","复制模型完整mip链前粒子配额不足。");
                    decoded = *existing;
                }
                else {
                    auto resource = selectedResource ? Result<AnimationResource>::Success(std::move(*selectedResource))
                        : read(path, limits.geometry.maxTextureBytes);
                    if (!resource.Succeeded()) {
                        // 预览模式下贴图在所有来源都不存在：游戏同样画不出这组粒子，只停这个实例的粒子并记原因，
                        // 不让一张缺失贴图拖垮整个动画场景（模型、选取圈都会随之消失）。
                        if (!limits.retainUnsupportedStatic || !OnlyMissingResource(resource.diagnostics))
                            return {std::nullopt, std::move(resource.diagnostics)};
                        missingTextures.insert(requestKey); textureMissing = true; break;
                    }
                    if (!ValidTextureResource(*resource.value, limits.geometry.maxTextureBytes))
                        return fail("ANIMATION_TEXTURE_SOURCE", "粒子贴图缺少真实来源、SHA-256 或超过读取预算。");
                    auto image = DecodeModelTexture(path, resource.value->bytes, limits.geometry.maxTextureDimension,remaining);
                    if (!image.Succeeded()) return {std::nullopt, std::move(image.diagnostics)};
                    decoded = std::move(*image.value);
                    scene.sources.push_back({key, resource.value->sourceLayer, resource.value->digest});
                }
                if (!ChargeModelTextureRgba(decoded,textureBytes,64ull*1024*1024)
                    || textureBytes>limits.geometry.maxTotalTextureBytes-modelTextureBytes)
                    return fail("ANIMATION_PARTICLE_TEXTURE_BUDGET", "模型和粒子解码贴图超过总预算。");
                for (const auto& diagnostic:decoded.mipDiagnostics)
                    if (scene.geometry.warnings.size()<64) scene.geometry.warnings.push_back(diagnostic);
                found = textures.emplace(key, static_cast<std::uint32_t>(scene.particleTextures.size())).first;
                scene.particleTextures.push_back(std::move(decoded));
            }
            binding.textures.push_back(found->second);
        }
        if (textureMissing) {
            bindingBytes -= cost;
            auto& capability = scene.particleCapabilities[r]; capability.available = false;
            capability.rejectionCode = "PARTICLE_TEXTURE_MISSING"; continue;
        }
        particleEmitters += bindingParticles; ribbonEmitters += bindingRibbons;
        binding.layerAlpha = a.particleLayerAlpha;
        binding.materialFlags = a.particleMaterialFlags;
        scene.particleUnfogged |= a.particleUnfoggedPreview;
        binding.quotas.resize(a.particles->emitters.size());
        maxNodes = std::max(maxNodes, a.compiled.NodeCount()); maxGroups = std::max(maxGroups, a.compiled.GroupCount());
        maxGeosets = std::max(maxGeosets, a.compiled.GeosetCount()); host->bindings.push_back(std::move(binding));
    }
    host->fixedFrameBytes += bindingBytes;
    scene.particleInstanceCount=host->bindings.size();
    if (host->fixedFrameBytes > limits.maxFrameBytes)
        return fail("ANIMATION_FRAME_BUDGET", "粒子身份回执和检查点超过帧预算。");
    if (host->bindings.empty()) return Out::Success(std::move(host));
    BatchLimits batch; batch.maxInstances = 1; batch.maxNodes = maxNodes; batch.maxGroups = maxGroups;
    batch.maxGeosets = maxGeosets; batch.outputMode = PoseOutputMode::FullNodes;
    const auto geosetPolicy = limits.retainUnsupportedStatic ? GeosetFailurePolicy::PreviewHideInvalidGeosets : GeosetFailurePolicy::Strict;
    auto evaluator = PoseBatchEvaluator::Create(compiled, epoch, batch, geosetPolicy, {}, {}, {}, AnimationNodeKeyPolicy(limits));
    if (!evaluator.Succeeded()) return {std::nullopt, std::move(evaluator.diagnostics)};
    host->evaluator = std::make_unique<PoseBatchEvaluator>(std::move(*evaluator.value));
    host->fixedFrameBytes += host->evaluator->StorageBytes() + 2ull * 256 * sizeof(Particles::Anchor);
    if (host->fixedFrameBytes>limits.maxFrameBytes)
        return fail("ANIMATION_FRAME_BUDGET","粒子绑定与单实例锚点求值器的实际存储超过帧预算。");
    // 统一按声明次序给配额；内存不足时降低共同的每发射器上限，不让每实例重领场景预算。
    const auto measure = [&](std::uint32_t cap, bool assign) {
        std::uint64_t stateBytes = host->bindings.size() * sizeof(AnimationParticleState), vertices = 0;
        for (auto& binding : host->bindings) {
            const auto& emitters = assets[binding.assetIndex].particles->emitters;
            for (std::size_t i = 0; i < emitters.size(); ++i) {
                const bool particle = emitters[i].kind == Particles::Kind::Particle2;
                const auto fair = particle ? p.maxParticles / std::max(1u, particleEmitters) : p.maxRibbonSegments / std::max(1u, ribbonEmitters);
                const auto quota = std::min(cap, fair);
                if (assign) { binding.quotas[i] = quota; binding.quotaLimited |= quota < p.maxPerEmitter; }
                stateBytes += sizeof(Particles::EmitterState) + static_cast<std::uint64_t>(quota) *
                    (particle ? sizeof(Particles::Particle) : sizeof(Particles::Segment));
                vertices += static_cast<std::uint64_t>(quota) * (particle && emitters[i].headOrTail == 2 ? 12u : 6u);
            }
        }
        if (assign) {
            host->stateBytes = stateBytes;
            host->maxDynamicVertices = static_cast<std::uint32_t>(std::min<std::uint64_t>(vertices, p.maxVertices));
            host->maxDynamicDraws = std::min(particleEmitters + ribbonEmitters, p.maxDraws);
        }
        // 旧检查点、候选状态，以及合并帧/临时几何的容量增长都计入；这是活跃计算预算，不是RSS。
        return host->fixedFrameBytes + 2 * stateBytes + 3 * std::min<std::uint64_t>(vertices, p.maxVertices) * sizeof(Particles::Vertex)
            + 2ull * (particleEmitters + ribbonEmitters) * (sizeof(War3RenderParticleDrawV2)+sizeof(std::uint32_t));
    };
    if (measure(0, false) > limits.maxFrameBytes) {
        if (!limits.retainUnsupportedStatic) return fail("ANIMATION_FRAME_BUDGET", "粒子检查点和节点暂存已超过帧预算。");
        // 世界可见集合尚未提交：只保留绑定，帧内存准入再决定实际模拟对象。
        // 不能因为全图潜在 EmitterState 的总数而剔除任何可见实例。
        (void)measure(0,true); return Out::Success(std::move(host));
    }
    std::uint32_t low = 0, high = p.maxPerEmitter;
    while (low < high) {
        const auto mid = low + (high - low + 1) / 2;
        if (measure(mid, false) <= limits.maxFrameBytes) low = mid; else high = mid - 1;
    }
    (void)measure(low, true);
    return Out::Success(std::move(host));
}
} // namespace War3Map::Render
