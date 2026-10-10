#include "War3MapCore/Render/WorldModelScene.h"
#include "War3RenderCore/MaterialTextures.h"

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Render/MdxPose.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

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
            if (mesh.vertices.size() > 65536 || mesh.indices.size() % 3 != 0
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

    WorldModelScene Finish() {
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
        return std::move(scene_);
    }

private:
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
        const auto path = ModelTexturePath(texture, request);
        if (path.empty()) { ++scene_.untexturedLayers; return WAR3_RENDER_MODEL_NO_TEXTURE; }
        ModelTextureRgba resourceKey; resourceKey.path = path;
        const auto key = ModelTextureAssetKey(resourceKey);
        if (const auto found = textures_.find(key); found != textures_.end()) {
            if (found->second == WAR3_RENDER_MODEL_NO_TEXTURE) {
                ++scene_.untexturedLayers;
                if (budgetTextures_.contains(key)) ++budgetTextures_[key]; else ++failedTextures_[path];
            }
            return found->second;
        }
        auto index = WAR3_RENDER_MODEL_NO_TEXTURE;
        auto bytes = read_(path);
        bool budgetExceeded = false;
        if (bytes.Succeeded() && (bytes.value->size() > limits_.maxTextureBytes || scene_.textures.size() >= limits_.maxTextures)) {
            budgetExceeded = true;
        } else if (bytes.Succeeded()) {
            const auto cached = decoded_ ? decoded_(path, *bytes.value, limits_.maxTextureDimension) : nullptr;
            const bool usable = cached && cached->ownedIdentity.empty() && PathKey(cached->path) == PathKey(path)
                && cached->width && cached->height && cached->width <= limits_.maxTextureDimension
                && cached->height <= limits_.maxTextureDimension
                && std::uint64_t(cached->width) * cached->height * 4 == cached->rgba.size();
            auto image = usable ? Result<ModelTextureRgba>::Success(*cached)
                : DecodeModelTexture(path, *bytes.value, limits_.maxTextureDimension);
            if (image.Succeeded()) {
                auto decoded = std::move(*image.value);
                if (textureBytes_ <= limits_.maxTotalTextureBytes && decoded.rgba.size() <= limits_.maxTotalTextureBytes - textureBytes_) {
                    textureBytes_ += decoded.rgba.size();
                    index = static_cast<std::uint32_t>(scene_.textures.size());
                    scene_.textures.push_back(std::move(decoded));
                } else budgetExceeded = true;
            }
        }
        if (budgetExceeded) ++budgetTextures_[key];
        textures_.emplace(key, index);
        if (index == WAR3_RENDER_MODEL_NO_TEXTURE) {
            ++scene_.untexturedLayers;
            if (!budgetExceeded) ++failedTextures_[path];
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
            if (texture.width > limits_.maxTextureDimension || texture.height > limits_.maxTextureDimension ||
                texture.rgba.size() > limits_.maxTextureBytes) return Output::Success(std::nullopt);
            const auto key = ModelTextureAssetKey(texture);
            if (!textures_.contains(key) && newTextures.insert(key).second) { ++textures; textureBytes += texture.rgba.size(); }
        }
        if (asset.OwnedBytes() > limits_.maxModelBytes || scene_.models.size() >= limits_.maxModels ||
            scene_.vertices.size() > limits_.maxVertices || vertices > limits_.maxVertices - scene_.vertices.size() ||
            scene_.indices.size() > limits_.maxIndices || indices > limits_.maxIndices - scene_.indices.size() ||
            scene_.meshes.size() > limits_.maxMeshes || asset.Meshes().size() > limits_.maxMeshes - scene_.meshes.size() ||
            scene_.layers.size() > limits_.maxLayers || asset.Meshes().size() > limits_.maxLayers - scene_.layers.size() ||
            scene_.textures.size() > limits_.maxTextures || textures > limits_.maxTextures - scene_.textures.size() ||
            textureBytes_ > limits_.maxTotalTextureBytes || textureBytes > limits_.maxTotalTextureBytes - textureBytes_)
            return Output::Success(std::nullopt);
        std::vector<std::uint32_t> textureMap;
        for (const auto& texture : asset.Textures()) {
            const auto key = ModelTextureAssetKey(texture);
            auto found = textures_.find(key);
            if (found == textures_.end()) {
                found = textures_.emplace(key, static_cast<std::uint32_t>(scene_.textures.size())).first;
                scene_.textures.push_back(texture); textureBytes_ += texture.rgba.size();
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
            if (bytes>limits_.maxModelBytes || textureBindingBytes_>limits_.maxTotalTextureBytes ||
                bytes>limits_.maxTotalTextureBytes-textureBindingBytes_)
                return Result<std::optional<std::uint32_t>>::Success(std::nullopt);
            textureBindingBytes_+=bytes;
            auto bindings=std::make_shared<std::vector<ModelTextureBinding>>(model.textures.size());
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
                const std::uint32_t flags = ((layer.shadingFlags & 0x1u) ? 1u : 0u) | ((layer.shadingFlags & 0x80u) ? 4u : 0u);
                scene_.layers.push_back({Texture(model.textures[layer.textureId], request),
                    request.previewGhost ? 2u : std::min(static_cast<std::uint32_t>(layer.filterMode), 6u), flags,
                    std::clamp((rigidPose_ ? materialAlphas[firstMaterialLayers[geoset.materialId]+materialLayer] : layer.alpha) * geosetAlpha[geosetIndex]
                        * (request.previewGhost ? 0.45f : 1.0f), 0.0f, 1.0f)});
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
    std::unordered_map<std::string, std::uint32_t> textures_;
    std::map<std::string, std::size_t> failedModels_, budgetModels_, failedTextures_, budgetTextures_;
    std::uint64_t textureBytes_ = 0;
    std::uint64_t textureBindingBytes_ = 0;
};

} // namespace

std::string ModelTexturePath(const MdxTexture& texture, const ModelInstanceRequest& request) {
    if (texture.replaceableId == 0) return texture.path;
    if (texture.replaceableId == 1) return TeamColorTexture(request.teamColor);
    if (texture.replaceableId == 2) return TeamGlowTexture(request.teamColor);
    return request.replaceableTexture;
}
Result<ModelTextureRgba> DecodeModelTexture(std::string path, std::span<const std::byte> bytes,
    std::uint32_t maxDimension) {
    if (!maxDimension) return Result<ModelTextureRgba>::Failure("Render.WorldModels", "WORLD_MODEL_TEXTURE_BUDGET", "贴图尺寸预算不能为零。");
    auto image = Image::DecodeBlp(bytes);
    if (!image.Succeeded()) return {std::nullopt, std::move(image.diagnostics)};
    if (!image.value->Valid()) return Result<ModelTextureRgba>::Failure("Render.WorldModels", "WORLD_MODEL_TEXTURE_INVALID", "解码贴图无效。");
    ModelTextureRgba decoded{std::move(path), image.value->width, image.value->height, {}};
    decoded.rgba.resize(image.value->rgba.size());
    std::transform(image.value->rgba.begin(), image.value->rgba.end(), decoded.rgba.begin(),
        [](std::byte value) { return static_cast<std::uint8_t>(value); });
    Shrink(decoded, maxDimension);
    return Result<ModelTextureRgba>::Success(std::move(decoded));
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
    return Result<WorldModelScene>::Success(builder.Finish());
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
    return Result<WorldModelScene>::Success(builder.Finish());
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

bool ReplaceBakedMeshes(WorldModelScene& scene, std::span<const BakedModelMesh> baked, const WorldModelLimits& limits) {
    if (scene.instanceRequests.size() != scene.instances.size() || scene.modelBounds.size() != scene.models.size()
        || (!scene.modelAssetKeys.empty() && scene.modelAssetKeys.size() != scene.models.size())
        || (!scene.modelTextureBindings.empty() && scene.modelTextureBindings.size() != scene.models.size())
        || (!scene.modelPaths.empty() && scene.modelPaths.size() != scene.models.size())
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
        if (mesh.vertices.size() > 65536 || mesh.indices.size() % 3 != 0
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

    std::unordered_map<std::string, std::uint32_t> textureByPath;
    textureByPath.reserve(scene.textures.size());
    for (std::size_t i = 0; i < scene.textures.size(); ++i)
        textureByPath.try_emplace(ModelTextureAssetKey(scene.textures[i]), static_cast<std::uint32_t>(i));
    std::vector<War3RenderModelVertexV2> vertices; vertices.reserve(newVertices + keptVertices);
    std::vector<std::uint16_t> indices; indices.reserve(newIndices + keptIndices);
    std::vector<War3RenderModelLayerV2> layers; layers.reserve(newMeshes + keptLayers);
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
    scene.vertices = std::move(vertices); scene.indices = std::move(indices); scene.layers = std::move(layers);
    scene.meshes = std::move(meshes); scene.models = std::move(models); scene.modelBounds = std::move(bounds);
    scene.modelAssetKeys = std::move(assetKeys); scene.modelPaths = std::move(modelPaths);
    scene.modelTextureBindings = std::move(textureBindings);
    scene.instances = std::move(instances); scene.instanceRequests = std::move(requests);
    scene.untexturedLayers = scene.untexturedLayers - oldUntextured + newUntextured;
    return true;
}

ModelSceneDescriptor DescribeModelTextures(std::span<const ModelTextureRgba> textures) {
    ModelSceneDescriptor descriptor;
    descriptor.mips.reserve(textures.size());
    for (const auto& texture : textures)
        descriptor.mips.push_back({sizeof(War3RenderMipRgbaV2), texture.width, texture.height, texture.rgba.size(), texture.rgba.data()});
    descriptor.chains.reserve(textures.size());
    for (const auto& mip : descriptor.mips)
        descriptor.chains.push_back({sizeof(War3RenderTextureMipChainV2), 1, 0, 1, 1, &mip});
    return descriptor;
}
ModelSceneDescriptor DescribeModelScene(const WorldModelScene& scene) {
    auto descriptor = DescribeModelTextures(scene.textures);
    const auto count = [](const auto& values) { return static_cast<std::uint32_t>(values.size()); };
    descriptor.scene = {sizeof(War3RenderModelSceneV2), count(scene.vertices), count(scene.indices), count(scene.layers),
        count(scene.meshes), count(scene.models), count(scene.instances), count(scene.textures),
        scene.vertices.data(), scene.indices.data(), scene.layers.data(), scene.meshes.data(), scene.models.data(),
        scene.instances.data(), descriptor.chains.data()};
    return descriptor;
}

} // namespace War3Map::Render
