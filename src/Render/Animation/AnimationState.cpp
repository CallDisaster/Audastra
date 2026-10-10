#include "AnimationInternal.h"
#include "AnimationVisibility.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace War3Map::Render {
namespace {
using Code = War3RenderCore::Code;
// 只放模型内容/当前采样的错误。来源、身份、容量、版本、宿主相机及请求错误不能降级。
constexpr Code kLocalDataErrors[]{
    Code::PoseModelUnsupported, Code::PoseSequenceUnsupported, Code::PoseNodeInvalid,
    Code::PoseNodeUnavailable, Code::PoseParentInvalid, Code::PoseParentUnavailable,
    Code::PosePivotUnavailable, Code::PoseNodeFlagsUnsupported, Code::PoseInterpolationUnsupported,
    Code::PoseGlobalSequenceInvalid, Code::PoseKeysInvalid, Code::PoseBoundaryUnsupported,
    Code::PoseNumberInvalid, Code::PoseRotationInvalid, Code::PoseCameraFlagsUnsupported,
    Code::SkinTopologyIncomplete, Code::SkinModelUnsupported, Code::SkinGeometryUnsupported,
    Code::SkinMatrixUnsupported, Code::SkinNormalInvalid, Code::GeoaUninterpreted,
    Code::GeoaNumberInvalid, Code::GeoaInterpolationUnsupported, Code::GeoaGlobalSequenceUnsupported,
    Code::GeoaSequenceUnsupported, Code::GeoaKeysInvalid, Code::GeoaBoundaryUnsupported,
    Code::PoseInheritanceSingular, Code::GeoaColorInvalid,
    Code::PoseZeroGlobalKeysUnsupported, Code::GeoaZeroGlobalKeysUnsupported,
    Code::PoseGlobalSpanUnsupported, Code::GeoaGlobalSpanUnsupported,
    Code::MaterialAlphaUninterpreted, Code::MaterialAlphaNumberInvalid, Code::MaterialAlphaInterpolationUnsupported,
    Code::MaterialAlphaGlobalSequenceUnsupported, Code::MaterialAlphaKeysInvalid, Code::MaterialAlphaBoundaryUnsupported,
    Code::MaterialAlphaGlobalSpanUnsupported,
    Code::MaterialTextureUninterpreted, Code::MaterialTextureInterpolationUnsupported,
    Code::MaterialTextureGlobalSequenceUnsupported, Code::MaterialTextureKeysInvalid,
    Code::MaterialTextureReferenceInvalid, Code::MaterialTextureGlobalSpanUnsupported
};
}
bool AnimationLocalDataFailure(Code code) noexcept {
    return std::find(std::begin(kLocalDataErrors), std::end(kLocalDataErrors), code) != std::end(kLocalDataErrors);
}
bool AnimationLocalDataFailure(std::string_view code) noexcept {
    constexpr std::string_view particleErrors[]{"PARTICLE_MAGIC", "PARTICLE_VERSION_UNSUPPORTED", "PARTICLE_TRUNCATED",
        "PARTICLE_SIZE_INVALID", "PARTICLE_NODE_SIZE", "PARTICLE_TRACK_DUPLICATE", "PARTICLE_TRACK_UNSUPPORTED",
        "PARTICLE_INTERPOLATION_UNSUPPORTED", "PARTICLE_FILTER_UNSUPPORTED", "PARTICLE_KEYS_INVALID", "PARTICLE_NUMBER_INVALID",
        "PARTICLE_TRACK_NUMBER", "PARTICLE_COLOR_INVALID",
        "PARTICLE_PARAMETER_UNSUPPORTED", "PARTICLE_ATLAS_INVALID", "PARTICLE_ATLAS_RANGE", "PARTICLE_SLOT_INVALID",
        "PARTICLE_GLOBAL_INVALID", "PARTICLE_MATERIAL_UNSUPPORTED", "PARTICLE_ANCHOR_UNAVAILABLE", "PARTICLE_TEXTURE_INVALID"};
    if (std::find(std::begin(particleErrors), std::end(particleErrors), code) != std::end(particleErrors)) return true;
    return std::any_of(std::begin(kLocalDataErrors), std::end(kLocalDataErrors), [&](Code value) {
        return code == War3RenderCore::CodeName(value);
    });
}
Status AnimationFailure(std::string code, std::string message) {
    return Status::Failure("Render.Animation", std::move(code), std::move(message));
}
std::uint64_t NextAnimationSerial() {
    static std::atomic<std::uint64_t> value{1};
    auto old = value.load(std::memory_order_relaxed);
    do {
        if (old == UINT64_MAX) throw std::overflow_error("ANIMATION_VERSION_EXHAUSTED");
    } while (!value.compare_exchange_weak(old, old + 1, std::memory_order_relaxed));
    return old;
}
std::string AnimationPathKey(std::string_view path) {
    std::string key(path);
    for (auto& c : key) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}
std::optional<std::uint32_t> FindAnimationSequence(std::span<const MdxSequence> sequences, std::string_view action,
    std::string_view requiredProperties) {
    const auto wanted = AnimationPathKey(action);
    if (wanted.empty()) return std::nullopt;
    if (!requiredProperties.empty()) {
        const auto words=[](std::string_view value) {
            std::set<std::string> result; std::string word;
            const auto flush=[&] { if (!word.empty()) { result.insert(std::move(word)); word.clear(); } };
            for (const unsigned char c:value) {
                if (c==',' || c=='-' || std::isspace(c)) flush();
                else word.push_back(static_cast<char>(std::tolower(c)));
            }
            flush(); return result;
        };
        auto required=words(action); const auto properties=words(requiredProperties);
        required.insert(properties.begin(),properties.end());
        std::optional<std::uint32_t> selected; std::size_t bestExtra=SIZE_MAX;
        for (std::uint32_t i=0;i<sequences.size();++i) {
            const auto candidate=words(sequences[i].name);
            if (!std::includes(candidate.begin(),candidate.end(),required.begin(),required.end())) continue;
            const auto extra=candidate.size()-required.size();
            if (!selected || extra<bestExtra) { selected=i; bestExtra=extra; }
        }
        // 模型缺少带附加属性的动作时只回到普通 Stand/Stand N，不能退成第 0 个动作，也不换成其他形态。
        if (selected) return selected;
    }
    const bool plainOnly = !requiredProperties.empty();
    std::optional<std::uint32_t> numbered, qualified;
    for (std::uint32_t i = 0; i < sequences.size(); ++i) {
        const auto name = AnimationPathKey(sequences[i].name);
        if (name == wanted) return i;
        if (!name.starts_with(wanted) || name.size() <= wanted.size()) continue;
        const auto tail = name[wanted.size()];
        if (tail != '-' && !std::isspace(static_cast<unsigned char>(tail))) continue;
        auto suffix = std::string_view(name).substr(wanted.size());
        while (!suffix.empty() && (suffix.front() == '-' || std::isspace(static_cast<unsigned char>(suffix.front()))))
            suffix.remove_prefix(1);
        if (!suffix.empty() && std::isdigit(static_cast<unsigned char>(suffix.front()))) {
            if (!numbered) numbered = i;
        } else if (!qualified) qualified = i;
    }
    return numbered ? numbered : (plainOnly ? std::nullopt : qualified);
}
Status CheckAnimationState(const AnimationInstanceState& s) {
    constexpr auto maxTime = std::uint64_t{1} << 53; // double 速度乘法前的整数精确范围
    if (!s.identity.value || !s.identity.generation || s.elapsedMs > maxTime || s.globalMs > maxTime ||
        !std::isfinite(s.speed) || s.speed < 0 || s.speed > 16 ||
        !std::isfinite(s.fractionalMs) || s.fractionalMs < 0 || s.fractionalMs >= 1)
        return AnimationFailure("ANIMATION_STATE_INVALID", "实例身份、时间或播放速度无效。");
    for (const auto f : s.transform) if (!std::isfinite(f) || std::abs(f) > 1e7f)
        return AnimationFailure("ANIMATION_TRANSFORM_INVALID", "实例变换包含非有限或超范围数值。");
    for (const auto f : s.tint) if (!(f >= 0 && f <= 1))
        return AnimationFailure("ANIMATION_COLOR_INVALID", "实例颜色须在 0..1 范围内。");
    return {};
}
Status AdvanceAnimationStates(std::vector<AnimationInstanceState>& states, const AnimatedModelScene& scene,
    const AnimationAdvance& a) {
    if (a.worldView) if (auto status=CheckAnimationView(*a.worldView); !status.Succeeded()) return status;
    if (a.deltaMs > 1000 || states.size() != scene.specs.size() || a.edits.size() > states.size())
        return AnimationFailure("ANIMATION_ADVANCE_BUDGET", "推进时间、编辑数或实例表不符合本场景预算。");
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    for (const auto& edit : a.edits) {
        const auto found = std::find_if(states.begin(), states.end(), [&](const auto& s) { return s.identity == edit.identity; });
        if (found == states.end() || !seen.emplace(edit.identity.value, edit.identity.generation).second)
            return AnimationFailure("ANIMATION_INSTANCE_STALE", "编辑指向未知、过期或重复的实例身份。");
        auto& s = *found;
        const auto i = static_cast<std::size_t>(found - states.begin());
        if (edit.sequenceIndex) {
            if (*edit.sequenceIndex >= scene.sequences[i].size())
                return AnimationFailure("ANIMATION_SEQUENCE_INVALID", "动作序号不在该模型的 SEQS 中。");
            s.sequenceIndex = *edit.sequenceIndex;
            const auto& sequence = scene.sequences[i][s.sequenceIndex];
            s.elapsedMs = sequence.end >= sequence.start ?
                (static_cast<std::uint64_t>(sequence.end) - sequence.start) / 2 : 0;
            s.fractionalMs = 0;
        }
        if (edit.elapsedMs) { s.elapsedMs = *edit.elapsedMs; s.fractionalMs = 0; }
        if (edit.globalMs) s.globalMs = *edit.globalMs;
        if (edit.speed) s.speed = *edit.speed;
        if (edit.playing) s.playing = *edit.playing;
        if (edit.transform) s.transform = *edit.transform;
        if (edit.tint) s.tint = *edit.tint;
    }
    for (std::size_t i = 0; i < states.size(); ++i) {
        auto& s = states[i];
        if (s.identity != scene.specs[i].identity) return AnimationFailure("ANIMATION_INSTANCE_STALE", "实例表身份已变化。");
        if (a.restartFinished && !s.playing && s.sequenceIndex < scene.sequences[i].size()) {
            const auto& seq = scene.sequences[i][s.sequenceIndex];
            if (seq.flags == 1 && seq.end >= seq.start && s.elapsedMs >= static_cast<std::uint64_t>(seq.end) - seq.start) {
                s.elapsedMs = 0; s.fractionalMs = 0;
            }
        }
        if (a.playAll) s.playing = *a.playAll;
        if (auto status = CheckAnimationState(s); !status.Succeeded()) return status;
        if (!(s.playing || a.stepPaused) || !a.deltaMs) continue;
        const auto scaled = static_cast<double>(a.deltaMs) * s.speed + s.fractionalMs;
        const auto whole = static_cast<std::uint64_t>(scaled);
        s.fractionalMs = scaled - static_cast<double>(whole);
        s.elapsedMs += whole; s.globalMs += whole;
        if (s.sequenceIndex < scene.sequences[i].size()) {
            const auto& seq = scene.sequences[i][s.sequenceIndex];
            if (seq.flags == 1 && seq.end >= seq.start && s.elapsedMs >= static_cast<std::uint64_t>(seq.end) - seq.start) {
                s.elapsedMs = static_cast<std::uint64_t>(seq.end) - seq.start;
                s.playing = false; s.fractionalMs = 0;
            }
        }
        if (auto status = CheckAnimationState(s); !status.Succeeded()) return status;
    }
    return {};
}
War3RenderCameraV2 FitAnimationSceneCamera(const AnimatedModelScene& scene) {
    std::array<float,3> low{1e30f,1e30f,1e30f}, high{-1e30f,-1e30f,-1e30f};
    for (const auto& instance:scene.geometry.instances) {
        const auto& box=scene.geometry.modelBounds.at(instance.model);
        for (unsigned corner=0;corner<8;++corner) {
            const float p[3]{box[(corner&1)?3:0],box[(corner&2)?4:1],box[(corner&4)?5:2]};
            for (std::size_t r=0;r<3;++r) {
                const auto v=instance.transform[r*4]*p[0]+instance.transform[r*4+1]*p[1]+instance.transform[r*4+2]*p[2]+instance.transform[r*4+3];
                low[r]=std::min(low[r],v); high[r]=std::max(high[r],v);
            }
        }
    }
    if (low[0]>high[0]) throw std::invalid_argument("ANIMATION_CAMERA_EMPTY_SCENE");
    const auto extent=std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2],20.0f});
    return {sizeof(War3RenderCameraV2),(low[0]+high[0])*0.5f,(low[1]+high[1])*0.5f,(low[2]+high[2])*0.5f,
        std::clamp(extent*2.4f,80.0f,100000.0f),-1.1f,0.5f};
}
War3RenderCore::LocalCamera AnimationWorldCamera(const War3RenderCameraV2& c) {
    const auto horizontal = c.distance * std::cos(c.pitchRadians);
    return {{c.targetX + horizontal * std::cos(c.yawRadians), c.targetY + horizontal * std::sin(c.yawRadians),
        c.targetZ + c.distance * std::sin(c.pitchRadians)}, {0, 0, 1}};
}
Result<War3RenderCore::LocalCamera> ToAnimationLocalCamera(const War3RenderCore::LocalCamera& c,
    const std::array<float, 12>& m) {
    // 通用 3x3 逆；非均匀缩放的相机方向由同一逆变换处理，不拿世界视点共用给所有实例。
    const double a=m[0], b=m[1], d=m[2], e=m[4], f=m[5], g=m[6], h=m[8], i=m[9], j=m[10];
    const double determinant=a*(f*j-g*i)-b*(e*j-g*h)+d*(e*i-f*h);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-15)
        return Result<War3RenderCore::LocalCamera>::Failure("Render.Animation", "ANIMATION_CAMERA_SINGULAR",
            "实例变换不可逆，无法建立模型局部相机。");
    const double inv[9]{(f*j-g*i)/determinant,(d*i-b*j)/determinant,(b*g-d*f)/determinant,
        (g*h-e*j)/determinant,(a*j-d*h)/determinant,(d*e-a*g)/determinant,
        (e*i-f*h)/determinant,(b*h-a*i)/determinant,(a*f-b*e)/determinant};
    War3RenderCore::LocalCamera out;
    for (std::size_t r=0;r<3;++r) {
        double eye=0, up=0;
        for (std::size_t k=0;k<3;++k) {
            eye += inv[r*3+k]*(static_cast<double>(c.eye[k])-m[k*4+3]);
            up += inv[r*3+k]*c.up[k];
        }
        out.eye[r]=static_cast<float>(eye); out.up[r]=static_cast<float>(up);
        if (!std::isfinite(out.eye[r]) || !std::isfinite(out.up[r]))
            return Result<War3RenderCore::LocalCamera>::Failure("Render.Animation", "ANIMATION_CAMERA_INVALID", "局部相机数值无效。");
    }
    return Result<War3RenderCore::LocalCamera>::Success(out);
}
std::uint64_t AnimationBitsHash(const void* data, std::size_t bytes) noexcept {
    auto hash = std::uint64_t{14695981039346656037ull};
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}
Status FillAnimationLayerAlphas(const AnimatedModelScene& scene, std::uint32_t model,
    War3RenderCore::Range32 range, std::span<float> output, std::span<const float> evaluated) {
    if (!scene.dynamicMaterialAlphas) return Status::Ok();
    if (model>=scene.modelLayerRanges.size() || range.offset>output.size() || range.count>output.size()-range.offset)
        return AnimationFailure("ANIMATION_LAYER_ALPHA_MAPPING","层透明度 owned 范围没有对应的实际模型。 ");
    const auto source=scene.modelLayerRanges[model];
    if (source.count!=range.count || source.offset>scene.geometry.layers.size() || source.count>scene.geometry.layers.size()-source.offset ||
        scene.drawLayerBindings.size()!=scene.geometry.layers.size())
        return AnimationFailure("ANIMATION_LAYER_ALPHA_MAPPING","模型 draw 层与冻结材质映射数量不同。 ");
    for (std::uint32_t i=0;i<range.count;++i) {
        const auto layer=source.offset+i; const auto& binding=scene.drawLayerBindings[layer];
        float alpha=scene.geometry.layers[layer].alpha;
        if (!evaluated.empty() && binding.materialLayer!=War3RenderCore::NoIndex) {
            if (binding.materialLayer>=evaluated.size())
                return AnimationFailure("ANIMATION_LAYER_ALPHA_MAPPING","当前姿势缺少原 material/layer 输出。 ");
            alpha=evaluated[binding.materialLayer]*binding.previewAlphaScale;
        }
        if (!std::isfinite(alpha)) return AnimationFailure("ANIMATION_LAYER_ALPHA_INVALID","材质层透明度不是有限值，未发布半帧。 ");
        output[range.offset+i]=alpha;
    }
    return Status::Ok();
}
Status FillAnimationMaterialStates(const AnimatedModelScene& scene, std::uint32_t model,
    War3RenderCore::Range32 range, std::span<War3RenderMaterialStateV2> output, std::span<const std::uint32_t> evaluated,
    std::span<const War3RenderCore::UvTransform> uvTransforms) {
    if (!scene.dynamicMaterialStates) return Status::Ok();
    if (!uvTransforms.empty() && uvTransforms.size()!=evaluated.size())
        return AnimationFailure("ANIMATION_TEXTURE_MAPPING","贴图坐标输出与贴图ID输出层数不同，未发布半帧。");
    if (model>=scene.modelLayerRanges.size() || range.offset>output.size() || range.count>output.size()-range.offset)
        return AnimationFailure("ANIMATION_TEXTURE_MAPPING","通用材质owned范围没有对应实际模型。");
    const auto source=scene.modelLayerRanges[model];
    if (source.count!=range.count || source.offset>scene.geometry.layers.size() || source.count>scene.geometry.layers.size()-source.offset ||
        scene.drawLayerBindings.size()!=scene.geometry.layers.size() || scene.geometry.modelTextureBindings.size()!=scene.models.size())
        return AnimationFailure("ANIMATION_TEXTURE_MAPPING","draw层与冻结常驻纹理映射数量不同。");
    const auto& textures=scene.geometry.modelTextureBindings[model];
    for (std::uint32_t i=0;i<range.count;++i) {
        const auto layer=source.offset+i; const auto& binding=scene.drawLayerBindings[layer];
        War3RenderMaterialStateV2 value{scene.geometry.layers[layer].textureIndex,binding.staticSamplerFlags,{1,0,0,0,1,0}};
        if (!evaluated.empty() && binding.materialLayer!=War3RenderCore::NoIndex) {
            if (binding.materialLayer>=evaluated.size() || !textures || evaluated[binding.materialLayer]>=textures->size() ||
                !(*textures)[evaluated[binding.materialLayer]].reachable)
                return AnimationFailure("ANIMATION_TEXTURE_MAPPING","当前姿势TEXS ID没有真实常驻绑定，未发布半帧。");
            const auto& texture=(*textures)[evaluated[binding.materialLayer]];
            value.textureIndex=texture.textureIndex; value.samplerFlags=texture.samplerFlags;
            if (!uvTransforms.empty()) {
                const auto& uv=uvTransforms[binding.materialLayer];
                if (!std::all_of(uv.begin(),uv.end(),[](float x) { return std::isfinite(x); }))
                    return AnimationFailure("ANIMATION_TEXTURE_MAPPING","贴图坐标变换不是有限值，未发布半帧。");
                std::copy(uv.begin(),uv.end(),value.uvTransform);
            }
        }
        if (value.samplerFlags>3 || (value.textureIndex!=WAR3_RENDER_MODEL_NO_TEXTURE && value.textureIndex>=scene.geometry.textures.size()))
            return AnimationFailure("ANIMATION_TEXTURE_MAPPING","材质常驻索引或sampler flags不属于当前静态资产。");
        output[range.offset+i]=value;
    }
    return Status::Ok();
}
bool AnimationHasVisibleLayers(const AnimatedModelScene& scene, const AnimationFrame& frame,
    const AnimationPoseLink& pose, std::uint32_t model) noexcept {
    if (!scene.dynamicMaterialAlphas)
        return std::any_of(frame.alphas.begin()+pose.firstAlpha,frame.alphas.begin()+pose.firstAlpha+pose.alphaCount,
            [](float alpha) { return alpha>0; });
    if (model>=scene.models.size() || model>=scene.modelLayerRanges.size() ||
        pose.materialAlphas.offset>frame.layerAlphas.size() || pose.materialAlphas.count>frame.layerAlphas.size()-pose.materialAlphas.offset)
        return false;
    const auto& meshes=scene.models[model].model; const auto layers=scene.modelLayerRanges[model];
    for (std::uint32_t mesh=0;mesh<meshes.meshCount;++mesh) {
        if (frame.alphas[pose.firstAlpha+mesh]<=0) continue;
        const auto& source=scene.geometry.meshes[meshes.firstMesh+mesh];
        if (source.firstLayer<layers.offset || source.firstLayer-layers.offset>pose.materialAlphas.count ||
            source.layerCount>pose.materialAlphas.count-(source.firstLayer-layers.offset)) return false;
        for (std::uint32_t k=0;k<source.layerCount;++k)
            if (frame.layerAlphas[pose.materialAlphas.offset+source.firstLayer-layers.offset+k]>0) return true;
    }
    return false;
}
} // namespace War3Map::Render
