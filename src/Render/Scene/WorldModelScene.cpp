#include "War3MapCore/Render/WorldModelScene.h"
#include "War3RenderCore/MaterialTextures.h"

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Render/MdxPose.h"
#include "War3MapCore/Resources/ClassicObjectTexture.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace War3Map::Render {
namespace {

constexpr std::string_view kSubsystem = "Render.WorldModels";
constexpr std::size_t kListedPaths = 12; // 每类警告最多列出的路径数
bool InstanceBudget(std::uint64_t count, const WorldModelLimits& limits) {
    return count<=UINT32_MAX && (!limits.maxInstances || count<=limits.maxInstances) &&
        count<=limits.maxInstanceBytes/(sizeof(War3RenderModelInstanceV2)+sizeof(std::uint32_t));
}

std::string PathKey(std::string_view path) {
    std::string key(path);
    for (auto& character : key) {
        if (character == '/') character = '\\';
        else character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return key;
}

bool Finite(std::span<const float> values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value) && std::abs(value) <= 1e7f; });
}

// 2×2 盒式缩小（奇数边保留最后一行/列），直到不超过上限。
void Shrink(ModelTextureRgba& texture, std::uint32_t maximum) {
    while (texture.width > maximum || texture.height > maximum) {
        const auto width = std::max(1u, texture.width / 2), height = std::max(1u, texture.height / 2);
        std::vector<std::uint8_t> output(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                std::uint32_t sum = 0, count = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const auto sx = std::min(texture.width - 1, x * 2 + dx), sy = std::min(texture.height - 1, y * 2 + dy);
                    sum += texture.rgba[(static_cast<std::size_t>(sy) * texture.width + sx) * 4 + channel]; ++count;
                }
                output[(static_cast<std::size_t>(y) * width + x) * 4 + channel] = static_cast<std::uint8_t>((sum + count / 2) / count);
            }
        }
        texture.width = width; texture.height = height; texture.rgba = std::move(output);
    }
}

struct ParsedModel {
    std::shared_ptr<const MdxModel> model;
    std::shared_ptr<const War3RenderCore::CompiledMaterialTextures> materialTextures;
    bool animated = false;
    bool teamDependent = false;        // 使用替换贴图 1/2
    bool replaceableDependent = false; // 使用替换贴图 ≥ 3
};

class Builder {
public:
    Builder(const ModelFileReader& read, const WorldModelLimits& limits,
        std::optional<ModelSceneRigidPoseRequest> rigidPose, ModelGeometryProvider assets = {},
        ModelDecodedTextureProvider decoded = {})
        : read_(read), limits_(limits), rigidPose_(rigidPose), assets_(std::move(assets)), decoded_(std::move(decoded)) { scene_.rigidPose = rigidPose; }

    Status Add(const ModelInstanceRequest& request, std::uint32_t requestIndex) {
        ++scene_.requestedInstances;
        if (!Finite(request.position) || !Finite(request.scale) || !Finite(std::span<const float>(&request.angle, 1))
            || std::any_of(request.tint.begin(), request.tint.end(), [](float value) { return !(value >= 0 && value <= 1); }))
            return Status::Failure(std::string(kSubsystem), "WORLD_MODEL_INSTANCE_INVALID", "摆放实例的坐标、缩放或颜色不是有效数值。");
        if (request.ownedStaticAsset) return AddOwned(request, requestIndex);
        const auto path = ModelAssetKey(request);
        auto parsed = parsed_.find(path);
        if (parsed == parsed_.end()) parsed = parsed_.emplace(path, Parse(request.modelPath)).first;
        if (!parsed->second.model) {
            if (rigidPose_) return Status::Failure(std::string(kSubsystem), "WORLD_MODEL_POSE_UNAVAILABLE",
                "请求刚性姿态的模型无法读取或解析：" + request.modelPath);
            ++scene_.skippedInstances; ++failedModels_[request.modelPath]; return {};
        }
        std::string variant = path;
        if (parsed->second.teamDependent) variant += "|team" + std::to_string(request.teamColor);
        if (parsed->second.replaceableDependent) variant += "|" + PathKey(request.replaceableTexture);
        if (request.previewGhost) variant += "|preview-ghost";
        auto model = models_.find(variant);
        if (model == models_.end()) {
            auto appended = Append(*parsed->second.model, request, parsed->second.animated, parsed->second.materialTextures.get());
            if (!appended.Succeeded()) return Status{std::move(appended.diagnostics)};
            model = models_.emplace(variant, std::move(*appended.value)).first;
        }
        if (!model->second || !InstanceBudget(scene_.instances.size()+1,limits_)) {
            if (rigidPose_) return Status::Failure(std::string(kSubsystem), "WORLD_MODEL_POSE_BUDGET",
                "请求刚性姿态的场景超出模型或实例显示预算：" + request.modelPath);
            ++scene_.skippedInstances; ++budgetModels_[request.modelPath]; return {};
        }
        War3RenderModelInstanceV2 instance{*model->second, {}, {request.tint[0], request.tint[1], request.tint[2]}};
        const auto transform = ModelInstanceTransform(request);
        std::copy(transform.begin(), transform.end(), instance.transform);
        scene_.instances.push_back(instance);
        scene_.instanceRequests.push_back(requestIndex);
        if (parsed->second.model->rigidStaticPreviewSource) ++scene_.rigidStaticPreviewInstances;
        return {};
    }

    Status AddBaked(std::span<const BakedModelMesh> baked) {
        if (baked.empty()) return {};
        std::uint64_t vertices = 0, indices = 0;
        for (const auto& mesh : baked) {
            if (mesh.samplerFlags > 3 || mesh.vertices.size() > 65536 || mesh.indices.size() % 3 != 0
                || std::any_of(mesh.indices.begin(), mesh.indices.end(), [&](std::uint16_t i) { return i >= mesh.vertices.size(); })
                || std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [](const War3RenderModelVertexV2& v) {
                    const float values[]{v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v};
                    return !Finite(values);
                }))
                return Status::Failure(std::string(kSubsystem), "WORLD_BAKED_MESH_INVALID", "烘焙网格的索引或坐标无效。");
            vertices += mesh.vertices.size(); indices += mesh.indices.size();
        }
        if (scene_.models.size() >= limits_.maxModels || !InstanceBudget(scene_.instances.size()+1,limits_)
            || vertices > limits_.maxVertices - scene_.vertices.size() || indices > limits_.maxIndices - scene_.indices.size()
            || baked.size() > limits_.maxMeshes - scene_.meshes.size() || baked.size() > limits_.maxLayers - scene_.layers.size()) {
            scene_.warnings.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), "WORLD_BAKED_BUDGET",
                "悬崖等烘焙网格超出模型显示预算，未显示。", std::nullopt});
            return {};
        }
        const auto emittedLayers = static_cast<std::size_t>(std::count_if(baked.begin(), baked.end(),
            [](const auto& mesh) { return !mesh.indices.empty(); }));
        if (auto status = PrepareSamplerAppend(emittedLayers, true); !status.Succeeded()) return status;
        War3RenderModelV2 entry{static_cast<std::uint32_t>(scene_.meshes.size()), 0};
        const ModelInstanceRequest none;
        for (const auto& mesh : baked) {
            if (mesh.indices.empty()) continue;
            scene_.meshes.push_back({static_cast<std::uint32_t>(scene_.indices.size()), static_cast<std::uint32_t>(mesh.indices.size()),
                static_cast<std::uint32_t>(scene_.vertices.size()), static_cast<std::uint32_t>(scene_.layers.size()), 1});
            if (assets_) {
                scene_.vertexGroups.insert(scene_.vertexGroups.end(), mesh.vertices.size(), 0);
                scene_.meshGeosets.push_back(UINT32_MAX);
            }
            scene_.vertices.insert(scene_.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
            scene_.indices.insert(scene_.indices.end(), mesh.indices.begin(), mesh.indices.end());
            MdxTexture texture;
            texture.path = mesh.texturePath;
            scene_.layers.push_back({Texture(texture, none), 0, 0, 1.0f});
            scene_.layerSamplerFlags.push_back(mesh.samplerFlags);
            ++entry.meshCount;
        }
        scene_.models.push_back(entry);
        scene_.modelAssetKeys.emplace_back();
        scene_.modelTextureBindings.push_back(nullptr);
        if (assets_) scene_.modelPaths.emplace_back();
        scene_.instances.push_back({static_cast<std::uint32_t>(scene_.models.size() - 1),
            {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {1, 1, 1}});
        scene_.instanceRequests.push_back(kBakedInstance);
        return {};
    }

    Result<WorldModelScene> Finish() {
        if (samplerActive_ && scene_.layerSamplerFlags.size() != scene_.layers.size())
            return Result<WorldModelScene>::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_INVALID",
                "来源 sampler 与实际发布的模型层数量不同。");
        if (samplerBudgetExceeded_ || !MaterialStorageFits(textureBytes_, textureBindingBytes_, scene_.layerSamplerFlags.capacity()))
            return Result<WorldModelScene>::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_BUDGET",
                "静态 sampler、绑定与真实贴图的常驻或增长峰值超过原预算。");
        // 实例按模型稳定排序，请求序号随之重排。
        std::vector<std::uint32_t> order(scene_.instances.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [this](std::uint32_t left, std::uint32_t right) {
            return scene_.instances[left].model < scene_.instances[right].model;
        });
        std::vector<War3RenderModelInstanceV2> instances; instances.reserve(order.size());
        std::vector<std::uint32_t> requests; requests.reserve(order.size());
        for (const auto index : order) { instances.push_back(scene_.instances[index]); requests.push_back(scene_.instanceRequests[index]); }
        scene_.instances = std::move(instances); scene_.instanceRequests = std::move(requests);
        ComputeBounds();
        Summarize(failedModels_, "WORLD_MODEL_UNAVAILABLE", "模型无法读取或解析，相关实例未显示");
        Summarize(budgetModels_, "WORLD_MODEL_BUDGET", "超出模型显示预算，相关实例未显示");
        Summarize(failedTextures_, "WORLD_MODEL_TEXTURE_UNAVAILABLE", "贴图不可用，相关层按白色显示");
        Summarize(budgetTextures_, "WORLD_MODEL_TEXTURE_BUDGET", "贴图数量、文件大小或解码后总字节数超过预算，未上传");
        return Result<WorldModelScene>::Success(std::move(scene_));
    }

private:
    // 所有材质所有者共用一份预算；retiredCapacity仅在reserve时与新分配同时驻留。
    bool MaterialStorageFits(std::uint64_t textureBytes, std::uint64_t bindingBytes,
        std::uint64_t samplerCapacity, std::uint64_t retiredCapacity = 0) const {
        auto remaining = limits_.maxTotalTextureBytes;
        const auto take = [&](std::uint64_t bytes) {
            if (bytes > remaining) return false;
            remaining -= bytes; return true;
        };
        return take(textureBytes) && take(bindingBytes)
            && take(samplerCapacity * sizeof(std::uint32_t)) && take(retiredCapacity * sizeof(std::uint32_t))
            && (!(samplerCapacity || retiredCapacity)
                || take(sizeof(War3RenderStaticSamplerModelSceneV2) - sizeof(War3RenderModelSceneV2)));
    }

    Status PrepareSamplerAppend(std::size_t layers, bool explicitSource) {
        if (!layers || (!samplerActive_ && !explicitSource)) return {};
        const auto target = scene_.layers.size() + layers;
        if (target > limits_.maxLayers)
            return Status::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_INVALID", "来源 sampler 层数超过几何门禁。");
        const auto previousCapacity = scene_.layerSamplerFlags.capacity();
        if (target > previousCapacity) {
            const auto requested = std::max(target, std::min<std::size_t>(limits_.maxLayers,
                previousCapacity + std::max<std::size_t>(previousCapacity, 1)));
            if (!MaterialStorageFits(textureBytes_, textureBindingBytes_, requested, previousCapacity))
                return Status::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_BUDGET", "来源 sampler 增长峰值超过原材质预算。");
            // Builder尚未发布scene，也没有共享副本；此处不存在额外COW分配。
            scene_.layerSamplerFlags.reserve(requested);
            if (!MaterialStorageFits(textureBytes_, textureBindingBytes_, scene_.layerSamplerFlags.capacity(), previousCapacity))
                return Status::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_BUDGET", "来源 sampler 实际分配峰值超过原材质预算。");
        }
        if (!samplerActive_) scene_.layerSamplerFlags.resize(scene_.layers.size(), 3);
        samplerActive_ = true;
        return {};
    }

    ParsedModel Parse(const std::string& path) {
        ParsedModel result;
        if (assets_) {
            auto asset = assets_(PathKey(path));
            result.model = std::move(asset.model);
            result.animated = asset.animated;
            result.materialTextures = std::move(asset.materialTextures);
        } else {
            auto bytes = read_(path);
            if (!bytes.Succeeded() || bytes.value->size() > limits_.maxModelBytes) return result;
            auto model = ParseMdx(*bytes.value);
            if (!model.Succeeded() && limits_.allowRigidStaticPreview && !model.diagnostics.empty()
                && model.diagnostics.front().code == "MDX_VALUE_INVALID") {
                auto preview = ParseMdxRigidStaticPreview(*bytes.value);
                if (preview.Succeeded()) model = std::move(preview);
            }
            if (!model.Succeeded()) return result;
            result.model = std::make_shared<const MdxModel>(std::move(*model.value));
        }
        if (!result.model) return result;
        if (result.model->rigidStaticPreviewSource) {
            if (!limits_.allowRigidStaticPreview) {
                scene_.warnings.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem),
                    "WORLD_MODEL_RIGID_PREVIEW_POLICY_REQUIRED", path + "：未明确启用无控制器静态预览，资产未显示。", std::nullopt});
                result.model.reset(); return result;
            }
            ++scene_.rigidStaticPreviewAssets;
            const auto origins = result.model->rigidStaticPreviewSource->Geosets();
            std::string message = path + "：预览近似，无控制器静态局部 identity；geosets=" + std::to_string(origins.size()) + "。";
            constexpr std::size_t listedGeosets = 8;
            for (std::size_t i = 0; i < std::min(origins.size(), listedGeosets); ++i) {
                const auto& origin = origins[i];
                message += " geoset=" + std::to_string(i) + " GNDX(offset=" + std::to_string(origin.vertexGroupsCountOffset)
                    + ",count=" + std::to_string(origin.vertexCount) + ") MTGC(offset=" + std::to_string(origin.matrixGroupsCountOffset)
                    + ",count=0) MATS(offset=" + std::to_string(origin.matrixIndicesCountOffset) + ",count=0)。";
            }
            if (origins.size() > listedGeosets) message += "其余字段位置保留在只读来源档案。";
            message += "原字节未修改；不是原版兼容或保存资格。";
            std::optional<OriginSpan> origin;
            if (!origins.empty()) origin = OriginSpan{path, origins[0].matrixGroupsCountOffset, 4, {}};
            scene_.warnings.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem),
                "WORLD_MODEL_RIGID_STATIC_PREVIEW", std::move(message), std::move(origin)});
        }
        for (const auto& texture : result.model->textures) {
            if (texture.replaceableId == 1 || texture.replaceableId == 2) result.teamDependent = true;
            else if (texture.replaceableId != 0) result.replaceableDependent = true;
        }
        return result;
    }

    std::uint32_t Texture(const MdxTexture& texture, const ModelInstanceRequest& request) {
        const auto requestedPath = ModelTexturePath(texture, request);
        if (requestedPath.empty()) { ++scene_.untexturedLayers; return WAR3_RENDER_MODEL_NO_TEXTURE; }
        // 同名普通 TEXS 与物编替换入口可能命中不同后缀，不能共用请求缓存。
        const bool objectReplacement = texture.replaceableId >= 3;
        const auto requestKey = std::string(objectReplacement ? "object:" : "exact:") + PathKey(requestedPath);
        if (const auto found = textureRequests_.find(requestKey); found != textureRequests_.end()) {
            if (found->second.index == WAR3_RENDER_MODEL_NO_TEXTURE) {
                ++scene_.untexturedLayers;
                if (found->second.budgetExceeded) ++budgetTextures_[requestedPath]; else ++failedTextures_[requestedPath];
            }
            return found->second.index;
        }
        auto index = WAR3_RENDER_MODEL_NO_TEXTURE;
        auto path = requestedPath;
        Result<std::vector<std::byte>> bytes;
        if (objectReplacement) {
            auto resolved = Resources::ReadClassicObjectTexture<std::vector<std::byte>>(path, read_);
            if (resolved.Succeeded()) {
                path = std::move(resolved.value->path);
                bytes.value = std::move(resolved.value->resource);
            }
            bytes.diagnostics = std::move(resolved.diagnostics);
        } else bytes = read_(path);
        ModelTextureRgba resourceKey; resourceKey.path = path;
        const auto key = ModelTextureAssetKey(resourceKey);
        // 解析后按实际资源去重：请求规则不同，但成功读到同一资源时只占一份上传预算。
        if (bytes.Succeeded() && bytes.value->size() <= limits_.maxTextureBytes) {
            if (const auto found = textures_.find(key); found != textures_.end()) {
                textureRequests_.emplace(requestKey, TextureRequest{found->second, false});
                return found->second;
            }
        }
        bool budgetExceeded = false;
        if (bytes.Succeeded() && (bytes.value->size() > limits_.maxTextureBytes || scene_.textures.size() >= limits_.maxTextures)) {
            budgetExceeded = true;
        } else if (bytes.Succeeded()) {
            // 解码的分配门只扣实际RGBA；联合来源/采样元数据由MaterialStorageFits归因。
            const auto remaining = limits_.maxTotalTextureBytes - std::min(limits_.maxTotalTextureBytes, textureBytes_);
            const auto cached = decoded_ ? decoded_(path, *bytes.value, limits_.maxTextureDimension) : nullptr;
            const bool usable = cached && cached->ownedIdentity.empty() && PathKey(cached->path) == PathKey(path)
                && cached->mipPolicyVersion == ModelTextureMipPolicyVersion
                && ModelTextureValid(*cached, limits_.maxTextureDimension) && ModelTextureRgbaBytes(*cached) <= remaining;
            auto image = usable ? Result<ModelTextureRgba>::Success(*cached)
                : DecodeModelTexture(path, *bytes.value, limits_.maxTextureDimension, remaining);
            if (image.Succeeded()) {
                auto decoded = std::move(*image.value);
                const auto rgbaBytes = ModelTextureRgbaBytes(decoded);
                const bool rgbaFits = ModelTextureValid(decoded, limits_.maxTextureDimension)
                    && rgbaBytes <= limits_.maxTotalTextureBytes && textureBytes_ <= limits_.maxTotalTextureBytes - rgbaBytes;
                if (rgbaFits && MaterialStorageFits(textureBytes_ + rgbaBytes, textureBindingBytes_, scene_.layerSamplerFlags.capacity())) {
                    textureBytes_ += rgbaBytes;
                    // 场景保有界摘要，完整来源/截止诊断仍由纹理owned字段保存。
                    for (const auto& diagnostic : decoded.mipDiagnostics)
                        if (scene_.warnings.size() < 64) scene_.warnings.push_back(diagnostic);
                    index = static_cast<std::uint32_t>(scene_.textures.size());
                    scene_.textures.push_back(std::move(decoded));
                } else {
                    budgetExceeded = true;
                    // 仅纯RGBA本可容纳时将超额归因于来源元数据；纯纹理超限保留原预算诊断。
                    samplerBudgetExceeded_ |= rgbaFits && (samplerActive_ || textureBindingBytes_ != 0);
                }
            } else {
                for (const auto& diagnostic : image.diagnostics) {
                    budgetExceeded |= diagnostic.code == "WORLD_MODEL_TEXTURE_BUDGET"
                        || diagnostic.code == "BLP_MIP_INPUT_BUDGET" || diagnostic.code == "BLP_MIP_RGBA_BUDGET"
                        || diagnostic.code == "BLP_MIP_RETAINED_BUDGET" || diagnostic.code == "BLP_MIP_MIP_BUDGET"
                        || diagnostic.code == "BLP_MIP_WORKING_BUDGET" || diagnostic.code == "BLP_MIP_JPEG_BUDGET";
                    if (scene_.warnings.size() < 64) {
                        auto warning = diagnostic; warning.severity = DiagnosticSeverity::Warning;
                        scene_.warnings.push_back(std::move(warning));
                    }
                }
            }
        }
        if (budgetExceeded) ++budgetTextures_[requestedPath];
        textureRequests_.emplace(requestKey, TextureRequest{index, budgetExceeded});
        if (index != WAR3_RENDER_MODEL_NO_TEXTURE) textures_.emplace(key, index);
        if (index == WAR3_RENDER_MODEL_NO_TEXTURE) {
            ++scene_.untexturedLayers;
            if (!budgetExceeded) ++failedTextures_[requestedPath];
        }
        return index;
    }

    Status AddOwned(const ModelInstanceRequest& request, std::uint32_t requestIndex) {
        if (rigidPose_) return Status::Failure(std::string(kSubsystem), "WORLD_OWNED_POSE_UNSUPPORTED",
            "原创静态辅助资产没有MDX动作；不能将显式刚性姿态请求冒称成功。");
        const auto key = ModelAssetKey(request);
        const auto variant = key + (request.previewGhost ? "|preview-ghost" : "");
        auto model = models_.find(variant);
        if (model == models_.end()) {
            auto appended = AppendOwned(request);
            if (!appended.Succeeded()) return Status{std::move(appended.diagnostics)};
            model = models_.emplace(variant, *appended.value).first;
            scene_.warnings.push_back({DiagnosticSeverity::Info, std::string(kSubsystem), "WORLD_MODEL_OWNED_STATIC_HELPER",
                request.modelPath + "：编辑器原创静态辅助资产，不读取或解析MDX，不声明原版模型兼容。", std::nullopt});
        }
        if (!model->second || !InstanceBudget(scene_.instances.size() + 1, limits_)) {
            ++scene_.skippedInstances; ++budgetModels_[request.modelPath]; return {};
        }
        War3RenderModelInstanceV2 instance{*model->second, {}, {request.tint[0], request.tint[1], request.tint[2]}};
        const auto transform = ModelInstanceTransform(request);
        std::copy(transform.begin(), transform.end(), instance.transform);
        scene_.instances.push_back(instance); scene_.instanceRequests.push_back(requestIndex);
        return {};
    }

    Result<std::optional<std::uint32_t>> AppendOwned(const ModelInstanceRequest& request) {
        using Output = Result<std::optional<std::uint32_t>>;
        const auto& asset = *request.ownedStaticAsset;
        std::uint64_t vertices = 0, indices = 0, textures = 0, textureBytes = 0;
        std::set<std::string> newTextures;
        for (const auto& mesh : asset.Meshes()) { vertices += mesh.vertices.size(); indices += mesh.indices.size(); }
        for (const auto& texture : asset.Textures()) {
            if (!ModelTextureHasLegacyOwnedState(texture) || !ModelTextureValid(texture, limits_.maxTextureDimension) ||
                ModelTextureRgbaBytes(texture) > limits_.maxTextureBytes) return Output::Success(std::nullopt);
            const auto key = ModelTextureAssetKey(texture);
            if (!textures_.contains(key) && newTextures.insert(key).second) { ++textures; if (!ChargeModelTextureRgba(texture, textureBytes, limits_.maxTotalTextureBytes)) return Output::Success(std::nullopt); }
        }
        if (asset.OwnedBytes() > limits_.maxModelBytes || scene_.models.size() >= limits_.maxModels ||
            scene_.vertices.size() > limits_.maxVertices || vertices > limits_.maxVertices - scene_.vertices.size() ||
            scene_.indices.size() > limits_.maxIndices || indices > limits_.maxIndices - scene_.indices.size() ||
            scene_.meshes.size() > limits_.maxMeshes || asset.Meshes().size() > limits_.maxMeshes - scene_.meshes.size() ||
            scene_.layers.size() > limits_.maxLayers || asset.Meshes().size() > limits_.maxLayers - scene_.layers.size() ||
            scene_.textures.size() > limits_.maxTextures || textures > limits_.maxTextures - scene_.textures.size())
            return Output::Success(std::nullopt);
        if (auto status = PrepareSamplerAppend(asset.Meshes().size(), false); !status.Succeeded())
            return {std::nullopt, std::move(status.diagnostics)};
        if (!MaterialStorageFits(textureBytes_ + textureBytes, textureBindingBytes_, scene_.layerSamplerFlags.capacity())) {
            if (!samplerActive_ && !textureBindingBytes_) return Output::Success(std::nullopt);
            return Output::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_BUDGET", "原创模型贴图与来源 sampler 超过原材质预算。");
        }
        std::vector<std::uint32_t> textureMap;
        for (const auto& texture : asset.Textures()) {
            const auto key = ModelTextureAssetKey(texture);
            auto found = textures_.find(key);
            if (found == textures_.end()) {
                found = textures_.emplace(key, static_cast<std::uint32_t>(scene_.textures.size())).first;
                scene_.textures.push_back(texture); textureBytes_ += ModelTextureRgbaBytes(texture);
            }
            textureMap.push_back(found->second);
        }
        const War3RenderModelV2 model{static_cast<std::uint32_t>(scene_.meshes.size()), static_cast<std::uint32_t>(asset.Meshes().size())};
        for (const auto& mesh : asset.Meshes()) {
            scene_.meshes.push_back({static_cast<std::uint32_t>(scene_.indices.size()), static_cast<std::uint32_t>(mesh.indices.size()),
                static_cast<std::uint32_t>(scene_.vertices.size()), static_cast<std::uint32_t>(scene_.layers.size()), 1});
            scene_.vertices.insert(scene_.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
            scene_.indices.insert(scene_.indices.end(), mesh.indices.begin(), mesh.indices.end());
            scene_.layers.push_back({mesh.textureIndex == WAR3_RENDER_MODEL_NO_TEXTURE ? WAR3_RENDER_MODEL_NO_TEXTURE : textureMap.at(mesh.textureIndex),
                request.previewGhost ? 2u : mesh.filterMode, mesh.flags, mesh.alpha * (request.previewGhost ? 0.45f : 1.0f)});
            if (samplerActive_) scene_.layerSamplerFlags.push_back(3);
            if (mesh.textureIndex == WAR3_RENDER_MODEL_NO_TEXTURE) ++scene_.untexturedLayers;
            if (assets_) { scene_.vertexGroups.insert(scene_.vertexGroups.end(), mesh.vertices.size(), 0); scene_.meshGeosets.push_back(UINT32_MAX); }
        }
        scene_.models.push_back(model); scene_.modelAssetKeys.push_back(ModelAssetKey(request));
        scene_.modelTextureBindings.push_back(nullptr);
        if (assets_) scene_.modelPaths.push_back(PathKey(request.modelPath));
        return Output::Success(std::optional<std::uint32_t>{static_cast<std::uint32_t>(scene_.models.size() - 1)});
    }

    // 追加一份模型几何；预算不足时整体不追加并返回空。
    Result<std::optional<std::uint32_t>> Append(const MdxModel& model, const ModelInstanceRequest& request, bool animated,
        const War3RenderCore::CompiledMaterialTextures* materialTextures) {
        std::uint64_t vertices = 0, indices = 0, meshes = 0, layers = 0, posedMeshes = 0;
        bool explicitSampler = false;
        std::vector<float> geosetAlpha;
        geosetAlpha.reserve(model.geosets.size());
        std::vector<std::optional<MdxRigidSkinnedGeoset>> posedGeosets;
        if (rigidPose_) posedGeosets.resize(model.geosets.size());
        std::optional<Diagnostic> firstUnsupportedPose;
        std::vector<float> materialAlphas;
        std::vector<std::uint32_t> firstMaterialLayers;
        if (rigidPose_ && rigidPose_->sequenceIndex >= model.sequences.size())
            return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                "WORLD_MODEL_POSE_SEQUENCE_INVALID", "模型不存在请求的动作序号：" + request.modelPath);
        if (rigidPose_) {
            auto sampled=SampleMdxLayerAlphas(model,{0,rigidPose_->sequenceIndex,rigidPose_->sequenceElapsedMs,rigidPose_->globalElapsedMs});
            if (!sampled.Succeeded()) return {std::nullopt,std::move(sampled.diagnostics)};
            materialAlphas=std::move(*sampled.value);
            std::uint32_t first=0;
            for (const auto& material:model.materials) {
                firstMaterialLayers.push_back(first); first+=static_cast<std::uint32_t>(material.layers.size());
            }
            if (materialAlphas.size()!=first) return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                "WORLD_MODEL_POSE_MATERIAL_MAPPING","参考层透明度与原 material/layer 数量不同："+request.modelPath);
        }
        for (std::size_t geosetIndex = 0; geosetIndex < model.geosets.size(); ++geosetIndex) {
            const auto& geoset = model.geosets[geosetIndex];
            float alpha = animated || geoset.visibleInStand ? 1.0f : 0.0f;
            if (rigidPose_) {
                const auto sampled = SampleMdxGeosetAlpha(model, {geosetIndex, rigidPose_->sequenceIndex,
                    rigidPose_->sequenceElapsedMs, rigidPose_->globalElapsedMs});
                if (!sampled.Succeeded()) {
                    const auto& diagnostic = sampled.diagnostics.front();
                    return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem), diagnostic.code,
                        request.modelPath + " geoset " + std::to_string(geosetIndex) + "：" + diagnostic.message);
                }
                alpha = *sampled.value;
            }
            if (rigidPose_ && alpha > 0 && !model.materials[geoset.materialId].layers.empty()
                && geoset.skippedPrimitiveGroups != 0)
                return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                    "WORLD_MODEL_POSE_PRIMITIVE_UNSUPPORTED",
                    "显示的 geoset 含尚未渲染的图元组，不能作为完整姿态几何：" + request.modelPath);
            if (rigidPose_ && alpha > 0 && !geoset.indices.empty()
                && !model.materials[geoset.materialId].layers.empty()) {
                const MdxRigidSkinRequest skinRequest{geosetIndex, rigidPose_->sequenceIndex,
                    rigidPose_->sequenceElapsedMs, rigidPose_->globalElapsedMs, rigidPose_->camera};
                auto sampled = model.rigidStaticPreviewSource ? SampleMdxRigidStaticPreviewGeoset(model, skinRequest)
                    : SampleMdxRigidSkinnedGeoset(model, skinRequest);
                if (!sampled.Succeeded()) {
                    const auto& diagnostic = sampled.diagnostics.front();
                    const auto message = request.modelPath + " geoset " + std::to_string(geosetIndex) + "：" + diagnostic.message;
                    if (diagnostic.code != "MDX_POSE_ROTATION_SPLINE_UNSUPPORTED")
                        return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem), diagnostic.code, message);
                    if (!firstUnsupportedPose)
                        firstUnsupportedPose = Diagnostic{DiagnosticSeverity::Warning, std::string(kSubsystem),
                            diagnostic.code, message, std::nullopt};
                    scene_.warnings.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), diagnostic.code,
                        message + "；该 geoset 未显示。", std::nullopt});
                    alpha = 0;
                } else {
                    for (const auto& warning : sampled.diagnostics) {
                        const auto message = request.modelPath + "：" + warning.message;
                        const auto duplicate = std::any_of(scene_.warnings.begin(), scene_.warnings.end(),
                            [&](const Diagnostic& previous) {
                                return previous.code == warning.code && previous.message == message;
                            });
                        if (!duplicate && warning.code != "MDX_RIGID_STATIC_PREVIEW") {
                            auto origin = warning.origin;
                            if (origin) origin->source = request.modelPath;
                            scene_.warnings.push_back({DiagnosticSeverity::Warning,
                                std::string(kSubsystem), warning.code, message, std::move(origin)});
                        }
                    }
                    posedGeosets[geosetIndex] = std::move(*sampled.value);
                }
            }
            geosetAlpha.push_back(alpha);
            if (!(alpha > 0) || geoset.indices.empty()) continue;
            for (const auto& layer : model.materials[geoset.materialId].layers) {
                const auto flags = model.textures[layer.textureId].flags;
                if (flags > 3) return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                    "WORLD_MODEL_SAMPLER_UNSUPPORTED", "实际模型层的来源 TEXS sampler 含未知标志：" + request.modelPath);
                explicitSampler |= flags != 3;
            }
            vertices += geoset.positions.size(); indices += geoset.indices.size();
            ++meshes; layers += model.materials[geoset.materialId].layers.size();
            if (!model.materials[geoset.materialId].layers.empty()) ++posedMeshes;
        }
        if (rigidPose_ && posedMeshes == 0 && firstUnsupportedPose)
            return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                firstUnsupportedPose->code, firstUnsupportedPose->message);
        if (rigidPose_ && posedMeshes == 0)
            return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                "WORLD_MODEL_POSE_GEOMETRY_UNAVAILABLE", "模型没有可显示的刚性 geoset：" + request.modelPath);
        if (scene_.models.size() >= limits_.maxModels || scene_.vertices.size() > limits_.maxVertices
            || scene_.indices.size() > limits_.maxIndices || scene_.meshes.size() > limits_.maxMeshes
            || scene_.layers.size() > limits_.maxLayers || vertices > limits_.maxVertices - scene_.vertices.size()
            || indices > limits_.maxIndices - scene_.indices.size() || meshes > limits_.maxMeshes - scene_.meshes.size()
            || layers > limits_.maxLayers - scene_.layers.size())
            return Result<std::optional<std::uint32_t>>::Success(std::nullopt);
        std::shared_ptr<const std::vector<ModelTextureBinding>> textureBindings;
        if (materialTextures) {
            const auto bytes=static_cast<std::uint64_t>(model.textures.size())*sizeof(ModelTextureBinding);
            if (bytes>limits_.maxModelBytes || !MaterialStorageFits(textureBytes_, textureBindingBytes_ + bytes, scene_.layerSamplerFlags.capacity()))
                return Result<std::optional<std::uint32_t>>::Success(std::nullopt);
            auto bindings=std::make_shared<std::vector<ModelTextureBinding>>(model.textures.size());
            const auto actualBytes = std::uint64_t(bindings->capacity()) * sizeof(ModelTextureBinding);
            if (!MaterialStorageFits(textureBytes_, textureBindingBytes_ + actualBytes, scene_.layerSamplerFlags.capacity()))
                return Result<std::optional<std::uint32_t>>::Success(std::nullopt);
            textureBindingBytes_+=actualBytes;
            for (std::uint32_t layer=0;layer<materialTextures->MaterialLayerCount();++layer) {
                const auto layout=materialTextures->MaterialLayerLayouts()[layer];
                for (const auto textureId:materialTextures->ReachableTextureIds(layer)) {
                    if (textureId>=model.textures.size()) return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                        "ANIMATION_TEXTURE_MAPPING","KMTF 可达 TEXS ID 越界："+request.modelPath);
                    auto& binding=(*bindings)[textureId];
                    if (binding.reachable) continue;
                    const auto& texture=model.textures[textureId];
                    if (texture.flags>3) return Result<std::optional<std::uint32_t>>::Failure(std::string(kSubsystem),
                        "ANIMATION_TEXTURE_SAMPLER_UNSUPPORTED","KMTF 可达 TEXS 含未支持的 sampler flags："+request.modelPath);
                    binding={Texture(texture,request),texture.flags,true};
                    if (binding.textureIndex==WAR3_RENDER_MODEL_NO_TEXTURE && !ModelTexturePath(texture,request).empty()) {
                        const auto& source=model.materials.at(layout.materialIndex).layers.at(layout.layerIndex);
                        scene_.warnings.push_back({DiagnosticSeverity::Warning,std::string(kSubsystem),
                            "ANIMATION_REACHABLE_TEXTURE_UNAVAILABLE",request.modelPath+" material="+std::to_string(layout.materialIndex)+
                            " layer="+std::to_string(layout.layerIndex)+" TEXS="+std::to_string(textureId)+
                            " 真实可达贴图未能常驻："+ModelTexturePath(texture,request)+"；保留未贴图状态，不能签收动态资源完整显示。",
                            OriginSpan{request.modelPath,source.textureSourceByteOffset,4,{}}});
                    }
                }
            }
            textureBindings=std::move(bindings);
        }
        if (auto status = PrepareSamplerAppend(static_cast<std::size_t>(layers), explicitSampler); !status.Succeeded())
            return {std::nullopt, std::move(status.diagnostics)};
        War3RenderModelV2 entry{static_cast<std::uint32_t>(scene_.meshes.size()), 0};
        for (std::size_t geosetIndex = 0; geosetIndex < model.geosets.size(); ++geosetIndex) {
            const auto& geoset = model.geosets[geosetIndex];
            const auto& material = model.materials[geoset.materialId];
            if (!(geosetAlpha[geosetIndex] > 0) || geoset.indices.empty() || material.layers.empty()) continue;
            const auto* skin = rigidPose_ ? &*posedGeosets[geosetIndex] : nullptr;
            War3RenderModelMeshV2 mesh{static_cast<std::uint32_t>(scene_.indices.size()),
                static_cast<std::uint32_t>(geoset.indices.size()), static_cast<std::uint32_t>(scene_.vertices.size()),
                static_cast<std::uint32_t>(scene_.layers.size()), static_cast<std::uint32_t>(material.layers.size())};
            for (std::size_t i = 0; i < geoset.positions.size(); ++i) {
                const auto& p = skin ? skin->positions[i] : geoset.positions[i];
                const auto n = skin ? skin->normals[i]
                    : (i < geoset.normals.size() ? geoset.normals[i] : std::array<float, 3>{0, 0, 1});
                const auto uv = i < geoset.uvs.size() ? geoset.uvs[i] : std::array<float, 2>{0, 0};
                scene_.vertices.push_back({p[0], p[1], p[2], n[0], n[1], n[2], uv[0], uv[1]});
                if (assets_) scene_.vertexGroups.push_back(animated ? geoset.vertexGroups.at(i) : 0);
            }
            scene_.indices.insert(scene_.indices.end(), geoset.indices.begin(), geoset.indices.end());
            std::uint32_t materialLayer=0;
            for (const auto& layer : material.layers) {
                const std::uint32_t flags = ((layer.shadingFlags & 0x1u) ? WAR3_RENDER_MODEL_UNSHADED : 0u)
                    | ((layer.shadingFlags & 0x20u) ? WAR3_RENDER_MODEL_UNFOGGED : 0u)
                    | ((layer.shadingFlags & 0x80u) ? WAR3_RENDER_MODEL_NO_DEPTH_WRITE : 0u);
                scene_.layers.push_back({Texture(model.textures[layer.textureId], request),
                    request.previewGhost ? 2u : std::min(static_cast<std::uint32_t>(layer.filterMode), 6u), flags,
                    std::clamp((rigidPose_ ? materialAlphas[firstMaterialLayers[geoset.materialId]+materialLayer] : layer.alpha) * geosetAlpha[geosetIndex]
                        * (request.previewGhost ? 0.45f : 1.0f), 0.0f, 1.0f)});
                if (samplerActive_) scene_.layerSamplerFlags.push_back(model.textures[layer.textureId].flags);
                ++materialLayer;
            }
            scene_.meshes.push_back(mesh);
            if (assets_) scene_.meshGeosets.push_back(static_cast<std::uint32_t>(geosetIndex));
            ++entry.meshCount;
        }
        scene_.models.push_back(entry);
        scene_.modelTextureBindings.push_back(std::move(textureBindings));
        scene_.modelAssetKeys.push_back(ModelAssetKey(request));
        if (assets_) scene_.modelPaths.push_back(PathKey(request.modelPath));
        return Result<std::optional<std::uint32_t>>::Success(
            std::optional<std::uint32_t>{static_cast<std::uint32_t>(scene_.models.size() - 1)});
    }

    // 每个模型的局部 AABB（只统计网格实际引用的顶点）；无几何的模型为零盒。
    void ComputeBounds() {
        scene_.modelBounds.assign(scene_.models.size(), {});
        for (std::size_t m = 0; m < scene_.models.size(); ++m) {
            std::array<float, 6> box{FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
            const auto& model = scene_.models[m];
            for (std::uint32_t k = 0; k < model.meshCount; ++k) {
                const auto& mesh = scene_.meshes[model.firstMesh + k];
                for (std::uint32_t i = 0; i < mesh.indexCount; ++i) {
                    const auto& v = scene_.vertices[mesh.baseVertex + scene_.indices[mesh.firstIndex + i]];
                    const float p[3]{v.x, v.y, v.z};
                    for (int axis = 0; axis < 3; ++axis) { box[axis] = std::min(box[axis], p[axis]); box[axis + 3] = std::max(box[axis + 3], p[axis]); }
                }
            }
            if (box[0] <= box[3]) scene_.modelBounds[m] = box;
        }
    }

    void Summarize(const std::map<std::string, std::size_t>& paths, const char* code, const char* message) {
        if (paths.empty()) return;
        std::string text = std::string(message) + "：";
        std::size_t listed = 0;
        for (const auto& [path, count] : paths) {
            if (listed++ == kListedPaths) { text += " 等共 " + std::to_string(paths.size()) + " 个"; break; }
            text += (listed > 1 ? "、" : "") + path + "（" + std::to_string(count) + "）";
        }
        scene_.warnings.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), code, std::move(text), std::nullopt});
    }

    const ModelFileReader& read_;
    const WorldModelLimits& limits_;
    std::optional<ModelSceneRigidPoseRequest> rigidPose_;
    ModelGeometryProvider assets_;
    ModelDecodedTextureProvider decoded_;
    WorldModelScene scene_;
    std::unordered_map<std::string, ParsedModel> parsed_;
    std::unordered_map<std::string, std::optional<std::uint32_t>> models_;
    struct TextureRequest { std::uint32_t index; bool budgetExceeded; };
    std::unordered_map<std::string, TextureRequest> textureRequests_;
    std::unordered_map<std::string, std::uint32_t> textures_;
    std::map<std::string, std::size_t> failedModels_, budgetModels_, failedTextures_, budgetTextures_;
    std::uint64_t textureBytes_ = 0;
    std::uint64_t textureBindingBytes_ = 0;
    bool samplerActive_ = false, samplerBudgetExceeded_ = false;
};

} // namespace

std::string ModelTexturePath(const MdxTexture& texture, const ModelInstanceRequest& request) {
    if (texture.replaceableId == 0) return texture.path;
    if (texture.replaceableId == 1) return TeamColorTexture(request.teamColor);
    if (texture.replaceableId == 2) return TeamGlowTexture(request.teamColor);
    return request.replaceableTexture;
}
std::uint64_t ModelTextureRgbaBytes(const ModelTextureRgba& texture) noexcept {
    std::uint64_t bytes = texture.rgba.size();
    for (const auto& mip : texture.additionalMips) {
        if (mip.rgba.size() > UINT64_MAX - bytes) return UINT64_MAX;
        bytes += mip.rgba.size();
    }
    return bytes;
}

std::uint64_t ModelDiagnosticStorageBytes(const std::vector<Diagnostic>& diagnostics) noexcept {
    if (diagnostics.capacity()>UINT64_MAX/sizeof(Diagnostic)) return UINT64_MAX;
    std::uint64_t bytes=std::uint64_t(diagnostics.capacity())*sizeof(Diagnostic);
    const auto add=[&](std::uint64_t value) { if (value>UINT64_MAX-bytes) return false; bytes+=value;return true; };
    for (const auto& diagnostic:diagnostics) {
        if (!add(diagnostic.subsystem.capacity()) || !add(diagnostic.code.capacity()) || !add(diagnostic.message.capacity()))
            return UINT64_MAX;
        if (diagnostic.origin) {
            const auto capacity=diagnostic.origin->source.native().capacity();
            if (capacity>UINT64_MAX/sizeof(std::filesystem::path::value_type)
                || !add(std::uint64_t(capacity)*sizeof(std::filesystem::path::value_type))
                || !add(diagnostic.origin->fingerprint.capacity())) return UINT64_MAX;
        }
    }
    return bytes;
}

std::uint64_t ModelTextureStorageBytes(const ModelTextureRgba& texture) noexcept {
    std::uint64_t bytes = texture.rgba.capacity();
    const auto add = [&](std::uint64_t value) {
        if (value > UINT64_MAX - bytes) return false;
        bytes += value; return true;
    };
    if (texture.additionalMips.capacity() > UINT64_MAX / sizeof(Image::DecodedImage)
        || !add(std::uint64_t(texture.additionalMips.capacity()) * sizeof(Image::DecodedImage))
        || !add(ModelDiagnosticStorageBytes(texture.mipDiagnostics))) return UINT64_MAX;
    for (const auto& mip : texture.additionalMips) if (!add(mip.rgba.capacity())) return UINT64_MAX;
    return bytes;
}

bool ModelTextureHasLegacyOwnedState(const ModelTextureRgba& texture) noexcept {
    return texture.additionalMips.empty() && texture.mipOrigin == ModelTextureMipOrigin::GeneratedFromTop
        && texture.mipPolicyVersion == 0 && texture.sourceWidth == 0 && texture.sourceHeight == 0
        && texture.sourceBaseMip == 0 && texture.mipStopReason == Image::BlpMipChainStopReason::Complete
        && texture.mipStopLevel == 0 && !texture.mipHasLaterDeclarations && texture.mipDiagnostics.empty();
}

bool ModelTextureValid(const ModelTextureRgba& texture, std::uint32_t maximum) noexcept {
    if (!maximum || !texture.width || !texture.height || texture.width > maximum || texture.height > maximum
        || std::uint64_t(texture.width) * texture.height > UINT64_MAX / 4
        || std::uint64_t(texture.width) * texture.height * 4 != texture.rgba.size()
        || texture.additionalMips.size() > 15 || texture.mipDiagnostics.size() > 64
        || (texture.mipPolicyVersion && texture.mipPolicyVersion != ModelTextureMipPolicyVersion)) return false;
    if ((texture.mipOrigin!=ModelTextureMipOrigin::GeneratedFromTop && texture.mipOrigin!=ModelTextureMipOrigin::BlpSource)
        || (!texture.mipPolicyVersion && !ModelTextureHasLegacyOwnedState(texture))) return false;
    if (std::any_of(texture.mipDiagnostics.begin(), texture.mipDiagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    })) return false;
    auto width = texture.width, height = texture.height;
    for (const auto& mip : texture.additionalMips) {
        if (width == 1 && height == 1) return false;
        width = std::max(1u, width / 2); height = std::max(1u, height / 2);
        if (mip.width != width || mip.height != height || !mip.Valid()) return false;
    }
    if (!texture.additionalMips.empty() && (texture.mipOrigin != ModelTextureMipOrigin::BlpSource
        || texture.mipPolicyVersion != ModelTextureMipPolicyVersion)) return false;
    if (texture.mipPolicyVersion == ModelTextureMipPolicyVersion
        && texture.mipOrigin == ModelTextureMipOrigin::GeneratedFromTop) {
        using Stop = Image::BlpMipChainStopReason;
        const bool legacy = texture.mipStopLevel == 0 && texture.mipStopReason == Stop::Complete
            && !texture.mipHasLaterDeclarations;
        const bool sourceTopOnly = texture.mipStopLevel == 1 && texture.mipStopReason == Stop::UndeclaredMip
            && !texture.mipHasLaterDeclarations && (texture.width!=1 || texture.height!=1)
            && texture.sourceWidth == texture.width && texture.sourceHeight == texture.height;
        if (!texture.additionalMips.empty() || texture.sourceBaseMip!=0 || !texture.sourceWidth || !texture.sourceHeight
            || (!legacy && !sourceTopOnly)) return false;
    }
    if (texture.mipOrigin == ModelTextureMipOrigin::BlpSource) {
        using Stop = Image::BlpMipChainStopReason;
        if (texture.mipPolicyVersion != ModelTextureMipPolicyVersion || texture.sourceBaseMip != 0
            || texture.sourceWidth != texture.width || texture.sourceHeight != texture.height
            || texture.sourceWidth>512 || texture.sourceHeight>512
            || texture.mipStopLevel != texture.additionalMips.size() + 1
            || (texture.additionalMips.empty() && (texture.width != 1 || texture.height != 1))) return false;
        const bool terminal=width==1 && height==1;
        const bool truncated=std::any_of(texture.mipDiagnostics.begin(),texture.mipDiagnostics.end(),[](const auto& diagnostic) {
            return diagnostic.subsystem=="Render.WorldModels" && diagnostic.code=="WORLD_MODEL_SOURCE_MIP_TRUNCATED";
        });
        switch (texture.mipStopReason) {
        case Stop::Complete: if (!terminal) return false; break;
        case Stop::UndeclaredMip:
            if (terminal || (texture.mipHasLaterDeclarations && !truncated)) return false;
            break;
        case Stop::InvalidMipRange:
            if (terminal || texture.additionalMips.empty() || !truncated) return false;
            break;
        case Stop::DecodeFailure: {
            bool knownFormat=false;
            for (const auto& diagnostic:texture.mipDiagnostics) {
                if (diagnostic.subsystem=="Image.BlpMipChain" && diagnostic.code=="BLP_MIP_JPEG") knownFormat=true;
                else if (!(diagnostic.subsystem=="Image.BlpMipChain" && diagnostic.code=="BLP_MIP_JPEG_TRAILING_DATA")
                    && !(diagnostic.subsystem=="Render.WorldModels" && diagnostic.code=="WORLD_MODEL_SOURCE_MIP_TRUNCATED")) return false;
            }
            if (terminal || texture.additionalMips.empty() || !knownFormat || !truncated) return false;
            break;
        }
        default: return false; // 预算、未知枚举及Core故障不得从缓存重新准入。
        }
    }
    return ModelTextureRgbaBytes(texture) != UINT64_MAX && ModelTextureStorageBytes(texture) != UINT64_MAX;
}

bool ChargeModelTextureRgba(const ModelTextureRgba& texture, std::uint64_t& bytes, std::uint64_t limit) noexcept {
    const auto count = ModelTextureRgbaBytes(texture);
    if (!ModelTextureValid(texture, UINT32_MAX) || bytes > limit || count > limit - bytes) return false;
    bytes += count; return true;
}

Result<ModelTextureRgba> DecodeModelTexture(std::string path, std::span<const std::byte> bytes,
    std::uint32_t maxDimension, std::uint64_t remainingRgbaBytes) {
    using Output = Result<ModelTextureRgba>;
    const auto fail = [](const char* code, const char* message) {
        return Output::Failure("Render.WorldModels", code, message);
    };
    if (!maxDimension || !remainingRgbaBytes || bytes.size() > 16u * 1024u * 1024u)
        return fail("WORLD_MODEL_TEXTURE_BUDGET", "贴图尺寸、读取或剩余RGBA预算无效。");
    // 仅按固定头的尺寸选择政策；分类不代表格式/像素有效，不捕获解码错误来回退。
    const auto u32 = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (unsigned part = 0; part < 4; ++part)
            value |= std::uint32_t(std::to_integer<std::uint8_t>(bytes[at + part])) << (part * 8);
        return value;
    };
    const bool blp1 = bytes.size() >= 20 && bytes[0] == std::byte{'B'} && bytes[1] == std::byte{'L'}
        && bytes[2] == std::byte{'P'} && bytes[3] == std::byte{'1'};
    const bool sourceChain = blp1 && u32(12) && u32(16)
        && u32(12) <= std::min(maxDimension, 512u) && u32(16) <= std::min(maxDimension, 512u);
    ModelTextureRgba decoded;
    decoded.path = std::move(path);
    decoded.mipPolicyVersion = ModelTextureMipPolicyVersion;
    if (!sourceChain) {
        // 保留旧4096源、TGA与caller较小上限的DecodeTexture+Shrink能力，不套generic2048门。
        auto image = Image::DecodeTexture(decoded.path, bytes);
        if (!image.Succeeded()) return {std::nullopt, std::move(image.diagnostics)};
        if (!image.value->Valid()) return fail("WORLD_MODEL_TEXTURE_INVALID", "解码贴图无效。");
        auto width = image.value->width, height = image.value->height;
        while (width > maxDimension || height > maxDimension) {
            width = std::max(1u, width / 2); height = std::max(1u, height / 2);
        }
        if (std::uint64_t(width) * height * 4 > remainingRgbaBytes)
            return fail("WORLD_MODEL_TEXTURE_BUDGET", "派生贴图超过本次剩余RGBA预算。");
        decoded.width = decoded.sourceWidth = image.value->width;
        decoded.height = decoded.sourceHeight = image.value->height;
        decoded.rgba.resize(image.value->rgba.size());
        std::transform(image.value->rgba.begin(), image.value->rgba.end(), decoded.rgba.begin(),
            [](std::byte value) { return static_cast<std::uint8_t>(value); });
        Shrink(decoded, maxDimension);
    } else {
        Image::BlpMipChainLimits limits;
        limits.maxDimension = std::min(maxDimension, 512u);
        limits.maxRgbaBytes = static_cast<std::size_t>(std::min<std::uint64_t>(limits.maxRgbaBytes, remainingRgbaBytes));
        limits.maxRetainedRgbaBytes = static_cast<std::size_t>(std::min<std::uint64_t>(limits.maxRetainedRgbaBytes, remainingRgbaBytes));
        auto chain = Image::DecodeBlpMipChain(bytes, limits);
        if (!chain.Succeeded()) return {std::nullopt, std::move(chain.diagnostics)};
        const auto& stop = chain.value->stop;
        using Stop = Image::BlpMipChainStopReason;
        const bool budget = stop.reason == Stop::TotalPixelBudget || stop.reason == Stop::RetainedRgbaBudget
            || stop.reason == Stop::WorkingBudget || stop.reason == Stop::JpegBudget;
        if (budget) {
            auto failure = fail("WORLD_MODEL_TEXTURE_BUDGET", "原始mip链超过本次预算，不发布预算截断链。");
            failure.diagnostics.insert(failure.diagnostics.end(), stop.diagnostics.begin(), stop.diagnostics.end());
            return failure;
        }
        const bool ordinaryEnd = stop.reason == Stop::Complete
            || (stop.reason == Stop::UndeclaredMip && !stop.hasLaterDeclarations);
        // 只允许明确可定位的格式损坏；Core像素/分配/未知host故障不能靠message猜成坏素材。
        const bool knownFormat = stop.reason == Stop::DecodeFailure && !stop.diagnostics.empty()
            && std::all_of(stop.diagnostics.begin(), stop.diagnostics.end(), [](const auto& diagnostic) {
                return diagnostic.subsystem == "Image.BlpMipChain" && diagnostic.code == "BLP_MIP_JPEG";
            });
        const bool damaged = stop.reason == Stop::InvalidMipRange
            || (stop.reason == Stop::UndeclaredMip && stop.hasLaterDeclarations) || knownFormat;
        if (!ordinaryEnd && (!damaged || chain.value->validatedMipCount < 2)) {
            auto failure = fail("WORLD_MODEL_SOURCE_MIP_REJECTED", "原始mip链损坏只剩单级，或故障身份不明确；不回退生成链。");
            failure.diagnostics.insert(failure.diagnostics.end(), stop.diagnostics.begin(), stop.diagnostics.end());
            return failure;
        }
        decoded.width = decoded.sourceWidth = chain.value->image.width;
        decoded.height = decoded.sourceHeight = chain.value->image.height;
        decoded.rgba.resize(chain.value->image.rgba.size());
        std::transform(chain.value->image.rgba.begin(), chain.value->image.rgba.end(), decoded.rgba.begin(),
            [](std::byte value) { return static_cast<std::uint8_t>(value); });
        decoded.additionalMips = std::move(chain.value->additionalMips);
        decoded.mipOrigin = decoded.additionalMips.empty() && (decoded.width != 1 || decoded.height != 1)
            ? ModelTextureMipOrigin::GeneratedFromTop : ModelTextureMipOrigin::BlpSource;
        decoded.mipStopReason = stop.reason; decoded.mipStopLevel = stop.level;
        decoded.mipHasLaterDeclarations = stop.hasLaterDeclarations;
        decoded.mipDiagnostics = std::move(chain.diagnostics);
        if (damaged) {
            for (auto& diagnostic : chain.value->stop.diagnostics) {
                diagnostic.severity = DiagnosticSeverity::Warning;
                decoded.mipDiagnostics.push_back(std::move(diagnostic));
            }
            decoded.mipDiagnostics.push_back({DiagnosticSeverity::Warning, "Render.WorldModels", "WORLD_MODEL_SOURCE_MIP_TRUNCATED",
                "原始mip在第" + std::to_string(stop.level) + "级遇到已定位格式损坏，仅保留实际多级前缀。", std::nullopt});
        } else if (decoded.mipOrigin == ModelTextureMipOrigin::GeneratedFromTop) {
            decoded.mipDiagnostics.push_back({DiagnosticSeverity::Info, "Render.WorldModels", "WORLD_MODEL_SOURCE_MIP0_GENERATED",
                "源文件明确只有mip0；低级由既有provider生成，不声称保留原始低mip。", std::nullopt});
        }
    }
    if (!ModelTextureValid(decoded, maxDimension) || ModelTextureRgbaBytes(decoded) > remainingRgbaBytes)
        return fail("WORLD_MODEL_TEXTURE_INVALID", "解码后的完整贴图链或预算无效。");
    auto result = Output::Success(std::move(decoded));
    result.diagnostics = result.value->mipDiagnostics;
    return result;
}
std::array<float, 12> ModelInstanceTransform(const ModelInstanceRequest& request) {
    const float c = std::cos(request.angle), s = std::sin(request.angle);
    return {c * request.scale[0], -s * request.scale[1], 0, request.position[0],
        s * request.scale[0], c * request.scale[1], 0, request.position[1],
        0, 0, request.scale[2], request.position[2]};
}

std::string TeamColorTexture(std::uint32_t index) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "ReplaceableTextures\\TeamColor\\TeamColor%02u.blp", std::min(index, 15u));
    return buffer;
}

std::string TeamGlowTexture(std::uint32_t index) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "ReplaceableTextures\\TeamGlow\\TeamGlow%02u.blp", std::min(index, 15u));
    return buffer;
}

Result<WorldModelScene> BuildWorldModelScene(std::span<const ModelInstanceRequest> requests,
    const ModelFileReader& read, const WorldModelLimits& limits, std::span<const BakedModelMesh> baked,
    std::optional<ModelSceneRigidPoseRequest> rigidPose) {
    if (!read) return Result<WorldModelScene>::Failure(std::string(kSubsystem), "WORLD_MODEL_READER", "没有提供模型读取器。");
    if (rigidPose && requests.empty())
        return Result<WorldModelScene>::Failure(std::string(kSubsystem), "WORLD_MODEL_POSE_EMPTY",
            "请求刚性姿态时没有摆放模型实例。");
    if (rigidPose && rigidPose->camera && requests.size() != 1)
        return Result<WorldModelScene>::Failure(std::string(kSubsystem), "WORLD_MODEL_POSE_CAMERA_SHARED",
            "模型局部相机预览只支持单个实例；多个摆放实例需要各自的局部视点。");
    Builder builder(read, limits, rigidPose);
    // 烘焙网格（悬崖）先占预算：它是地形的一部分，优先于摆放物显示。
    if (auto status = builder.AddBaked(baked); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto status = builder.Add(requests[i], static_cast<std::uint32_t>(i));
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
    }
    return builder.Finish();
}

Result<WorldModelScene> BuildAnimatedModelGeometry(std::span<const ModelInstanceRequest> requests,
    const ModelFileReader& read, const ModelGeometryProvider& assets, const WorldModelLimits& limits,
    std::span<const BakedModelMesh> baked, const ModelDecodedTextureProvider& decoded) {
    if (!read || !assets) return Result<WorldModelScene>::Failure(std::string(kSubsystem),
        "WORLD_MODEL_READER", "没有提供资源或已编译模型读取器。");
    Builder builder(read, limits, std::nullopt, assets, decoded);
    if (auto status = builder.AddBaked(baked); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto status = builder.Add(requests[i], static_cast<std::uint32_t>(i));
        if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
    }
    return builder.Finish();
}

bool ReplaceModelInstances(WorldModelScene& scene, std::span<const ModelInstanceRequest> previous,
    std::span<const ModelInstanceRequest> next, const WorldModelLimits& limits) {
    if (!scene.vertexGroups.empty() || scene.instanceRequests.size() != scene.instances.size()) return false;
    const auto variantKey = [](const ModelInstanceRequest& request) {
        return ModelAssetKey(request) + "|" + std::to_string(request.teamColor) + "|"
            + PathKey(request.replaceableTexture) + (request.previewGhost ? "|preview-ghost" : "");
    };
    // 变体 → 已显示的模型序号；空表示此前请求过但不可用。
    std::unordered_map<std::string, std::optional<std::uint32_t>> variants;
    std::vector<War3RenderModelInstanceV2> instances;
    std::vector<std::uint32_t> requests;
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const auto source = scene.instanceRequests[i];
        if (source == kBakedInstance) { instances.push_back(scene.instances[i]); requests.push_back(kBakedInstance); continue; }
        if (source >= previous.size()) return false;
        variants[variantKey(previous[source])] = scene.instances[i].model;
    }
    for (const auto& request : previous) variants.try_emplace(variantKey(request), std::nullopt);
    std::size_t skipped = 0;
    for (std::size_t r = 0; r < next.size(); ++r) {
        const auto& request = next[r];
        if (!Finite(request.position) || !Finite(request.scale) || !Finite(std::span<const float>(&request.angle, 1))
            || std::any_of(request.tint.begin(), request.tint.end(), [](float value) { return !(value >= 0 && value <= 1); }))
            return false;
        const auto found = variants.find(variantKey(request));
        if (found == variants.end()) return false;
        if (!found->second) { ++skipped; continue; }
        if (!InstanceBudget(instances.size()+1,limits)) return false;
        War3RenderModelInstanceV2 instance{*found->second, {}, {request.tint[0], request.tint[1], request.tint[2]}};
        const auto transform = ModelInstanceTransform(request);
        std::copy(transform.begin(), transform.end(), instance.transform);
        instances.push_back(instance);
        requests.push_back(static_cast<std::uint32_t>(r));
    }
    std::vector<std::uint32_t> order(instances.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&](std::uint32_t left, std::uint32_t right) { return instances[left].model < instances[right].model; });
    scene.instances.clear(); scene.instanceRequests.clear();
    for (const auto index : order) { scene.instances.push_back(instances[index]); scene.instanceRequests.push_back(requests[index]); }
    scene.requestedInstances = next.size();
    scene.skippedInstances = skipped;
    return true;
}

bool ReplaceBakedMeshes(WorldModelScene& outputScene, std::span<const BakedModelMesh> baked, const WorldModelLimits& limits) {
    const auto& scene = std::as_const(outputScene); // 失败前的读取不能触发 COW 分离。
    if (scene.instanceRequests.size() != scene.instances.size() || scene.modelBounds.size() != scene.models.size()
        || (!scene.modelAssetKeys.empty() && scene.modelAssetKeys.size() != scene.models.size())
        || (!scene.modelTextureBindings.empty() && scene.modelTextureBindings.size() != scene.models.size())
        || (!scene.modelPaths.empty() && scene.modelPaths.size() != scene.models.size())
        || (!scene.layerSamplerFlags.empty() && (scene.layerSamplerFlags.size() != scene.layers.size()
            || std::any_of(scene.layerSamplerFlags.cbegin(), scene.layerSamplerFlags.cend(), [](auto flags) { return flags > 3; })))
        || scene.textures.size() > limits.maxTextures || baked.size() > limits.maxMeshes) return false;

    // BuildWorldModelScene 把烘焙模型放在 0 号，实例也排在首位；只接受这种布局。
    bool hadBaked = false;
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        if (scene.instanceRequests[i] == kBakedInstance) {
            if (hadBaked || i != 0 || scene.models.empty() || scene.instances[i].model != 0) return false;
            hadBaked = true;
        } else if (scene.instances[i].model >= scene.models.size()) return false;
    }
    const std::size_t oldModels = hadBaked ? 1 : 0;
    const std::size_t oldMeshes = hadBaked ? scene.models[0].meshCount : 0;
    if (oldMeshes > scene.meshes.size() || (hadBaked && scene.models[0].firstMesh != 0)) return false;

    // 模型、网格及数据数组必须保持构建时的紧密布局；否则平移偏移量没有可信含义。
    std::size_t nextMesh = oldMeshes;
    for (std::size_t m = oldModels; m < scene.models.size(); ++m) {
        const auto& model = scene.models[m];
        if (model.firstMesh != nextMesh || model.meshCount > scene.meshes.size() - nextMesh) return false;
        nextMesh += model.meshCount;
    }
    if (nextMesh != scene.meshes.size()) return false;
    std::size_t nextIndex = 0, nextLayer = 0, previousVertex = 0;
    for (const auto& mesh : scene.meshes) {
        if (mesh.firstIndex != nextIndex || mesh.firstLayer != nextLayer
            || mesh.baseVertex < previousVertex || mesh.baseVertex > scene.vertices.size()
            || mesh.indexCount > scene.indices.size() - nextIndex
            || mesh.layerCount > scene.layers.size() - nextLayer) return false;
        nextIndex += mesh.indexCount;
        nextLayer += mesh.layerCount;
        previousVertex = mesh.baseVertex;
    }
    if (nextIndex != scene.indices.size() || nextLayer != scene.layers.size()
        || (scene.meshes.empty() && !scene.vertices.empty())
        || (!scene.meshes.empty() && scene.meshes[0].baseVertex != 0)) return false;
    const std::size_t oldVertices = oldMeshes < scene.meshes.size()
        ? scene.meshes[oldMeshes].baseVertex : scene.vertices.size();
    const std::size_t oldIndices = oldMeshes < scene.meshes.size()
        ? scene.meshes[oldMeshes].firstIndex : scene.indices.size();
    const std::size_t oldLayers = oldMeshes < scene.meshes.size()
        ? scene.meshes[oldMeshes].firstLayer : scene.layers.size();
    if (oldMeshes > 0 && scene.meshes[oldMeshes - 1].baseVertex > oldVertices) return false;
    for (std::size_t i = oldModels; i < scene.instances.size(); ++i) {
        if (scene.instances[i].model < oldModels) return false;
    }

    std::size_t newVertices = 0, newIndices = 0, newMeshes = 0;
    for (const auto& mesh : baked) {
        if (mesh.samplerFlags > 3 || mesh.vertices.size() > 65536 || mesh.indices.size() % 3 != 0
            || (mesh.indices.empty() && !mesh.vertices.empty())
            || std::any_of(mesh.indices.begin(), mesh.indices.end(), [&](std::uint16_t i) { return i >= mesh.vertices.size(); })
            || std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [](const War3RenderModelVertexV2& v) {
                const float values[]{v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v};
                return !Finite(values);
            })) return false;
        if (mesh.indices.empty()) continue;
        if (newVertices > limits.maxVertices || mesh.vertices.size() > limits.maxVertices - newVertices
            || newIndices > limits.maxIndices || mesh.indices.size() > limits.maxIndices - newIndices) return false;
        newVertices += mesh.vertices.size(); newIndices += mesh.indices.size(); ++newMeshes;
    }
    const auto fits = [](std::size_t added, std::size_t kept, std::size_t maximum) {
        return kept <= maximum && added <= maximum - kept;
    };
    const std::size_t keptVertices = scene.vertices.size() - oldVertices;
    const std::size_t keptIndices = scene.indices.size() - oldIndices;
    const std::size_t keptLayers = scene.layers.size() - oldLayers;
    const std::size_t keptMeshes = scene.meshes.size() - oldMeshes;
    const std::size_t keptModels = scene.models.size() - oldModels;
    const std::size_t keptInstances = scene.instances.size() - oldModels;
    const std::size_t newModels = newMeshes != 0 ? 1 : 0;
    if (!fits(newVertices, keptVertices, limits.maxVertices) || !fits(newIndices, keptIndices, limits.maxIndices)
        || !fits(newMeshes, keptLayers, limits.maxLayers) || !fits(newMeshes, keptMeshes, limits.maxMeshes)
        || !fits(newModels, keptModels, limits.maxModels)
        || !InstanceBudget(newModels+keptInstances,limits)) return false;

    std::uint64_t samplerStorage = std::uint64_t(scene.layerSamplerFlags.capacity()) * sizeof(std::uint32_t)
        + std::uint64_t(newMeshes + keptLayers) * sizeof(std::uint32_t)
        + sizeof(War3RenderStaticSamplerModelSceneV2) - sizeof(War3RenderModelSceneV2);
    for (const auto& bindings : scene.modelTextureBindings)
        if (bindings) samplerStorage += std::uint64_t(bindings->capacity()) * sizeof(ModelTextureBinding);
    for (const auto& texture : scene.textures) {
        if (!ModelTextureValid(texture, limits.maxTextureDimension)
            || !ChargeModelTextureRgba(texture, samplerStorage, limits.maxTotalTextureBytes)) return false;
    }
    if (samplerStorage > limits.maxTotalTextureBytes) return false;

    std::unordered_map<std::string, std::uint32_t> textureByPath;
    textureByPath.reserve(scene.textures.size());
    for (std::size_t i = 0; i < scene.textures.size(); ++i)
        textureByPath.try_emplace(ModelTextureAssetKey(scene.textures[i]), static_cast<std::uint32_t>(i));
    std::vector<War3RenderModelVertexV2> vertices; vertices.reserve(newVertices + keptVertices);
    std::vector<std::uint16_t> indices; indices.reserve(newIndices + keptIndices);
    std::vector<War3RenderModelLayerV2> layers; layers.reserve(newMeshes + keptLayers);
    std::vector<std::uint32_t> samplerFlags; samplerFlags.reserve(newMeshes + keptLayers);
    std::vector<War3RenderModelMeshV2> meshes; meshes.reserve(newMeshes + keptMeshes);
    std::size_t newUntextured = 0;
    for (const auto& mesh : baked) {
        if (mesh.indices.empty()) continue;
        auto texture = WAR3_RENDER_MODEL_NO_TEXTURE;
        if (!mesh.texturePath.empty()) {
            ModelTextureRgba resourceKey; resourceKey.path = mesh.texturePath;
            const auto found = textureByPath.find(ModelTextureAssetKey(resourceKey));
            if (found == textureByPath.end()) return false;
            texture = found->second;
        }
        if (texture == WAR3_RENDER_MODEL_NO_TEXTURE) ++newUntextured;
        meshes.push_back({static_cast<std::uint32_t>(indices.size()), static_cast<std::uint32_t>(mesh.indices.size()),
            static_cast<std::uint32_t>(vertices.size()), static_cast<std::uint32_t>(layers.size()), 1});
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
        layers.push_back({texture, 0, 0, 1.0f});
        samplerFlags.push_back(mesh.samplerFlags);
    }
    const auto oldUntextured = static_cast<std::size_t>(std::count_if(scene.layers.begin(),
        scene.layers.begin() + static_cast<std::ptrdiff_t>(oldLayers), [](const auto& layer) {
            return layer.textureIndex == WAR3_RENDER_MODEL_NO_TEXTURE;
        }));
    if (scene.untexturedLayers < oldUntextured) return false;

    for (std::size_t k = oldMeshes; k < scene.meshes.size(); ++k) {
        auto mesh = scene.meshes[k];
        if (mesh.baseVertex < oldVertices || mesh.firstIndex < oldIndices || mesh.firstLayer < oldLayers) return false;
        mesh.baseVertex = static_cast<std::uint32_t>(newVertices + (mesh.baseVertex - oldVertices));
        mesh.firstIndex = static_cast<std::uint32_t>(newIndices + (mesh.firstIndex - oldIndices));
        mesh.firstLayer = static_cast<std::uint32_t>(newMeshes + (mesh.firstLayer - oldLayers));
        meshes.push_back(mesh);
    }
    vertices.insert(vertices.end(), scene.vertices.begin() + static_cast<std::ptrdiff_t>(oldVertices), scene.vertices.end());
    indices.insert(indices.end(), scene.indices.begin() + static_cast<std::ptrdiff_t>(oldIndices), scene.indices.end());
    layers.insert(layers.end(), scene.layers.begin() + static_cast<std::ptrdiff_t>(oldLayers), scene.layers.end());
    if (scene.layerSamplerFlags.empty()) samplerFlags.resize(newMeshes + keptLayers, 3);
    else samplerFlags.insert(samplerFlags.end(), scene.layerSamplerFlags.cbegin() + static_cast<std::ptrdiff_t>(oldLayers), scene.layerSamplerFlags.cend());
    if (samplerFlags.capacity() > newMeshes + keptLayers) {
        const auto extra = std::uint64_t(samplerFlags.capacity() - newMeshes - keptLayers) * sizeof(std::uint32_t);
        if (extra > limits.maxTotalTextureBytes - samplerStorage) return false;
    }

    std::vector<War3RenderModelV2> models; models.reserve(newModels + keptModels);
    std::vector<std::array<float, 6>> bounds; bounds.reserve(newModels + keptModels);
    std::vector<std::string> assetKeys, modelPaths;
    std::vector<std::shared_ptr<const std::vector<ModelTextureBinding>>> textureBindings;
    if (!scene.modelTextureBindings.empty()) {
        textureBindings.resize(newModels);
        textureBindings.insert(textureBindings.end(),scene.modelTextureBindings.cbegin()+static_cast<std::ptrdiff_t>(oldModels),scene.modelTextureBindings.cend());
    }
    if (!scene.modelAssetKeys.empty()) {
        assetKeys.reserve(newModels + keptModels); if (newModels) assetKeys.emplace_back();
        assetKeys.insert(assetKeys.end(), scene.modelAssetKeys.cbegin() + static_cast<std::ptrdiff_t>(oldModels), scene.modelAssetKeys.cend());
    }
    if (!scene.modelPaths.empty()) {
        modelPaths.reserve(newModels + keptModels); if (newModels) modelPaths.emplace_back();
        modelPaths.insert(modelPaths.end(), scene.modelPaths.cbegin() + static_cast<std::ptrdiff_t>(oldModels), scene.modelPaths.cend());
    }
    if (newModels) {
        models.push_back({0, static_cast<std::uint32_t>(newMeshes)});
        std::array<float, 6> box{FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (std::size_t v = 0; v < newVertices; ++v) {
            const float p[3]{vertices[v].x, vertices[v].y, vertices[v].z};
            for (int axis = 0; axis < 3; ++axis) {
                box[axis] = std::min(box[axis], p[axis]);
                box[axis + 3] = std::max(box[axis + 3], p[axis]);
            }
        }
        bounds.push_back(box);
    }
    for (std::size_t m = oldModels; m < scene.models.size(); ++m) {
        models.push_back({static_cast<std::uint32_t>(newMeshes + (scene.models[m].firstMesh - oldMeshes)),
            scene.models[m].meshCount});
        bounds.push_back(scene.modelBounds[m]);
    }

    std::vector<War3RenderModelInstanceV2> instances; instances.reserve(newModels + keptInstances);
    std::vector<std::uint32_t> requests; requests.reserve(newModels + keptInstances);
    if (newModels) {
        instances.push_back({0, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {1, 1, 1}});
        requests.push_back(kBakedInstance);
    }
    for (std::size_t i = oldModels; i < scene.instances.size(); ++i) {
        auto instance = scene.instances[i];
        instance.model = static_cast<std::uint32_t>(newModels + (instance.model - oldModels));
        instances.push_back(instance);
        requests.push_back(scene.instanceRequests[i]);
    }
    outputScene.vertices = std::move(vertices); outputScene.indices = std::move(indices); outputScene.layers = std::move(layers);
    outputScene.layerSamplerFlags = std::move(samplerFlags);
    outputScene.meshes = std::move(meshes); outputScene.models = std::move(models); outputScene.modelBounds = std::move(bounds);
    outputScene.modelAssetKeys = std::move(assetKeys); outputScene.modelPaths = std::move(modelPaths);
    outputScene.modelTextureBindings = std::move(textureBindings);
    outputScene.instances = std::move(instances); outputScene.instanceRequests = std::move(requests);
    outputScene.untexturedLayers = scene.untexturedLayers - oldUntextured + newUntextured;
    return true;
}

ModelSceneDescriptor DescribeModelTextures(std::span<const ModelTextureRgba> textures) {
    ModelSceneDescriptor descriptor;
    std::size_t totalMips = 0;
    for (const auto& texture : textures) {
        const auto count = texture.additionalMips.size() + 1;
        if (!ModelTextureValid(texture, UINT32_MAX) || count > descriptor.mips.max_size() - totalMips) {
            descriptor.validStaticSamplers = false; return descriptor;
        }
        totalMips += count;
    }
    // 先完成全部描述符分配，再建立指针；后续push不能使已有chain悬空。
    descriptor.mips.reserve(totalMips);
    for (const auto& texture : textures) {
        descriptor.mips.push_back({sizeof(War3RenderMipRgbaV2), texture.width, texture.height, texture.rgba.size(), texture.rgba.data()});
        for (const auto& mip : texture.additionalMips)
            descriptor.mips.push_back({sizeof(War3RenderMipRgbaV2), mip.width, mip.height, mip.rgba.size(),
                reinterpret_cast<const std::uint8_t*>(mip.rgba.data())});
    }
    descriptor.chains.reserve(textures.size());
    std::size_t offset = 0;
    for (const auto& texture : textures) {
        const auto count = static_cast<std::uint32_t>(texture.additionalMips.size() + 1);
        descriptor.chains.push_back({sizeof(War3RenderTextureMipChainV2), count, count - 1, 1, 1, descriptor.mips.data() + offset});
        offset += count;
    }
    return descriptor;
}
ModelSceneDescriptor DescribeModelScene(const WorldModelScene& scene) {
    auto descriptor = DescribeModelTextures(scene.textures);
    const auto count = [](const auto& values) { return static_cast<std::uint32_t>(values.size()); };
    descriptor.scene = {sizeof(War3RenderModelSceneV2), count(scene.vertices), count(scene.indices), count(scene.layers),
        count(scene.meshes), count(scene.models), count(scene.instances), count(scene.textures),
        scene.vertices.data(), scene.indices.data(), scene.layers.data(), scene.meshes.data(), scene.models.data(),
        scene.instances.data(), descriptor.chains.data()};
    descriptor.validStaticSamplers = descriptor.validStaticSamplers && (scene.layerSamplerFlags.empty()
        || (scene.layerSamplerFlags.size() == scene.layers.size()
            && std::none_of(scene.layerSamplerFlags.begin(), scene.layerSamplerFlags.end(), [](auto flags) { return flags > 3; })));
    if (descriptor.validStaticSamplers && std::any_of(scene.layerSamplerFlags.begin(), scene.layerSamplerFlags.end(), [](auto flags) { return flags != 3; })) {
        descriptor.samplerFlags = scene.layerSamplerFlags;
        descriptor.samplerScene = {descriptor.scene, WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1,
            count(descriptor.samplerFlags), std::as_const(descriptor.samplerFlags).data()};
        descriptor.samplerScene.base.structSize = sizeof(descriptor.samplerScene);
    }
    return descriptor;
}

Status CheckStaticModelSamplerSupport(const War3RenderPluginApiV2* api, War3RendererHandle renderer,
    const WorldModelScene& scene) {
    if (!scene.layerSamplerFlags.empty() && (scene.layerSamplerFlags.size() != scene.layers.size()
        || std::any_of(scene.layerSamplerFlags.begin(), scene.layerSamplerFlags.end(), [](auto flags) { return flags > 3; })))
        return Status::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_INVALID", "静态 sampler 与实际 draw 层不一致。");
    if (std::none_of(scene.layerSamplerFlags.begin(), scene.layerSamplerFlags.end(), [](auto flags) { return flags != 3; })) return {};
    return CheckStaticModelSamplerCapability(api, renderer);
}
Status CheckStaticModelSamplerCapability(const War3RenderPluginApiV2* api, War3RendererHandle renderer) {
    War3RenderStaticSamplerCapabilitiesV2 capabilities{sizeof(capabilities)};
    if (!api || api->structSize < offsetof(War3RenderPluginApiV2, getStaticModelSamplerCapabilities) + sizeof(api->getStaticModelSamplerCapabilities)
        || !api->getStaticModelSamplerCapabilities || api->getStaticModelSamplerCapabilities(renderer, &capabilities) != 0
        || capabilities.structSize != sizeof(capabilities) || capabilities.samplerFormat != WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1
        || capabilities.supportedSamplerFlags != WAR3_RENDER_STATIC_SAMPLER_FLAGS_MASK || capabilities.reserved)
        return Status::Failure(std::string(kSubsystem), "WORLD_STATIC_SAMPLER_UNSUPPORTED",
            "渲染插件未声明来源静态 sampler 能力，保留此前配对地形与悬崖。");
    return {};
}

} // namespace War3Map::Render
