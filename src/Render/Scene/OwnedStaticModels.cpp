#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <exception>
#include <limits>
#include <utility>

namespace War3Map::Render {
namespace {

std::string ResourceKey(std::string_view path) {
    std::string key(path);
    for (auto& c : key) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!key.empty() && (static_cast<unsigned char>(key.front()) < 32u || key.front() == '\x7f')) {
        // 原始请求先于reader进入缓存；即使caller伪造保留前缀，也只能属于ordinary-invalid空间。
        constexpr char hex[] = "0123456789abcdef";
        std::string escaped("\x01ordinary-resource-invalid-v1:");
        for (const char value : key) {
            const auto c = static_cast<unsigned char>(value);
            escaped.push_back(hex[c >> 4]); escaped.push_back(hex[c & 15u]);
        }
        return escaped;
    }
    return key;
}
bool Finite(float value) { return std::isfinite(value) && std::abs(value) <= 1e7f; }
void U32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::byte>((value >> (i * 8)) & 255u));
}
void Float(std::vector<std::byte>& bytes, float value) { U32(bytes, std::bit_cast<std::uint32_t>(value)); }

} // namespace

std::string ModelAssetKey(const ModelInstanceRequest& request) {
    // 普通路径的保留control前缀由ResourceKey独立转义；不能在reader验证前借到owned缓存。
    return request.ownedStaticAsset ? std::string("\x01owned-static-v1:") + std::string(request.ownedStaticAsset->IdentitySha256())
        : ResourceKey(request.modelPath);
}
std::string ModelTextureAssetKey(const ModelTextureRgba& texture) {
    return texture.ownedIdentity.empty() ? ResourceKey(texture.path) : std::string("\x01owned-texture-v1:") + texture.ownedIdentity;
}

Result<std::shared_ptr<const OwnedStaticModelAsset>> FreezeOwnedStaticModelAsset(
    std::span<const OwnedModelMesh> meshes, std::span<const ModelTextureRgba> textures,
    const WorldModelLimits& limits) try {
    using Output = Result<std::shared_ptr<const OwnedStaticModelAsset>>;
    const auto fail = [](const char* code, const char* text) { return Output::Failure("Render.OwnedStatic", code, text); };
    if (meshes.empty() || meshes.size() > limits.maxMeshes || meshes.size() > limits.maxLayers ||
        textures.size() > limits.maxTextures || !limits.maxModelBytes || !limits.maxTextureDimension)
        return fail("WORLD_OWNED_ASSET_INVALID", "辅助资产须有实际网格，并符合网格、层及贴图预算。");
    std::uint64_t bytes = sizeof(OwnedStaticModelAsset) + 64, vertices = 0, indices = 0, textureBytes = 0;
    const auto add = [&](std::uint64_t count) {
        if (bytes > limits.maxModelBytes || count > limits.maxModelBytes - bytes) return false;
        bytes += count; return true;
    };
    for (const auto& mesh : meshes) {
        if (mesh.vertices.empty() || mesh.vertices.size() > 65536 || mesh.indices.empty() || mesh.indices.size() % 3 ||
            mesh.filterMode > 6 || (mesh.flags & ~WAR3_RENDER_MODEL_LAYER_FLAGS) || !(mesh.alpha >= 0 && mesh.alpha <= 1) ||
            (mesh.textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE && mesh.textureIndex >= textures.size()))
            return fail("WORLD_OWNED_MESH_INVALID", "辅助网格的顶点、三角形、层或本地贴图引用无效。");
        for (const auto& v : mesh.vertices) {
            const float values[]{v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v};
            if (!std::all_of(std::begin(values), std::end(values), Finite))
                return fail("WORLD_OWNED_MESH_INVALID", "辅助网格包含非有限或超界顶点。");
        }
        if (std::any_of(mesh.indices.begin(), mesh.indices.end(), [&](std::uint16_t i) { return i >= mesh.vertices.size(); }))
            return fail("WORLD_OWNED_MESH_INVALID", "辅助网格索引越界。");
        vertices += mesh.vertices.size(); indices += mesh.indices.size();
        if (vertices > limits.maxVertices || indices > limits.maxIndices ||
            !add(sizeof(OwnedModelMesh)) || !add(mesh.vertices.size() * sizeof(War3RenderModelVertexV2)) ||
            !add(mesh.indices.size() * sizeof(std::uint16_t)))
            return fail("WORLD_OWNED_ASSET_BUDGET", "辅助资产实际几何字节超过预算。");
    }
    for (const auto& texture : textures) {
        const auto pixels = static_cast<std::uint64_t>(texture.width) * texture.height;
        if (!ModelTextureHasLegacyOwnedState(texture) || !texture.width || !texture.height || texture.width > limits.maxTextureDimension ||
            texture.height > limits.maxTextureDimension || pixels > std::numeric_limits<std::size_t>::max() / 4 ||
            texture.rgba.size() != pixels * 4 || texture.path.size() > 1024 || texture.path.find('\0') != std::string::npos)
            return fail("WORLD_OWNED_TEXTURE_INVALID", "辅助资产的owned RGBA尺寸、正文或来源描述无效。");
        if (texture.rgba.size() > limits.maxTextureBytes || textureBytes > limits.maxTotalTextureBytes ||
            texture.rgba.size() > limits.maxTotalTextureBytes - textureBytes ||
            !add(sizeof(ModelTextureRgba)) || !add(texture.rgba.size()) || !add(texture.path.size()) ||
            !add(64))
            return fail("WORLD_OWNED_ASSET_BUDGET", "辅助资产实际纹理字节超过预算。");
        textureBytes += texture.rgba.size();
    }
    // 按明确小端字段散列，不散列ABI padding、地址或caller给出的SHA/ownedIdentity。
    std::vector<std::byte> canonical;
    canonical.reserve(static_cast<std::size_t>(bytes));
    U32(canonical, 1); U32(canonical, static_cast<std::uint32_t>(meshes.size()));
    U32(canonical, static_cast<std::uint32_t>(textures.size()));
    for (const auto& mesh : meshes) {
        U32(canonical, static_cast<std::uint32_t>(mesh.vertices.size()));
        U32(canonical, static_cast<std::uint32_t>(mesh.indices.size()));
        U32(canonical, mesh.textureIndex); U32(canonical, mesh.filterMode); U32(canonical, mesh.flags); Float(canonical, mesh.alpha);
        for (const auto& v : mesh.vertices) {
            for (const auto value : {v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v}) Float(canonical, value);
        }
        for (const auto index : mesh.indices) {
            canonical.push_back(static_cast<std::byte>(index & 255u));
            canonical.push_back(static_cast<std::byte>(index >> 8));
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> textureSpans;
    textureSpans.reserve(textures.size());
    for (const auto& texture : textures) {
        const auto begin = canonical.size();
        U32(canonical, texture.width); U32(canonical, texture.height);
        for (const auto channel : texture.rgba) canonical.push_back(static_cast<std::byte>(channel));
        textureSpans.emplace_back(begin, canonical.size() - begin);
    }
    auto digest = Sha256(canonical);
    if (!digest.Succeeded()) return {std::nullopt, std::move(digest.diagnostics)};
    std::vector<std::string> textureIdentities;
    textureIdentities.reserve(textures.size());
    for (const auto& [offset, length] : textureSpans) {
        auto textureDigest = Sha256(std::span<const std::byte>(canonical).subspan(offset, length));
        if (!textureDigest.Succeeded()) return {std::nullopt, std::move(textureDigest.diagnostics)};
        textureIdentities.push_back(std::move(*textureDigest.value));
    }
    auto frozen = std::shared_ptr<OwnedStaticModelAsset>(new OwnedStaticModelAsset());
    frozen->identity_ = std::move(*digest.value);
    frozen->meshes_.assign(meshes.begin(), meshes.end()); frozen->textures_.reserve(textures.size());
    frozen->bytes_ = bytes;
    for (std::size_t i = 0; i < textures.size(); ++i) {
        // caller的ownedIdentity不借用、不复制；它没有设置资产身份的资格。
        frozen->textures_.push_back({textures[i].path, textures[i].width, textures[i].height, textures[i].rgba,
            std::move(textureIdentities[i])});
    }
    if (frozen->bytes_ > limits.maxModelBytes) return fail("WORLD_OWNED_ASSET_BUDGET", "辅助资产拥有的身份描述超过预算。");
    return Output::Success(std::move(frozen));
} catch (const std::exception&) {
    return Result<std::shared_ptr<const OwnedStaticModelAsset>>::Failure("Render.OwnedStatic",
        "WORLD_OWNED_ASSET_ALLOCATION", "辅助资产拥有字节分配或散列失败，未返回部分资产。");
}

Result<std::shared_ptr<const OwnedStaticModelAsset>> CreateCheckerBlockerAsset(
    std::optional<std::array<float, 2>> sourcePlanarFootprint, const WorldModelLimits& limits) {
    using Output = Result<std::shared_ptr<const OwnedStaticModelAsset>>;
    if (sourcePlanarFootprint && (!Finite((*sourcePlanarFootprint)[0]) || !Finite((*sourcePlanarFootprint)[1]) ||
        (*sourcePlanarFootprint)[0] <= 0 || (*sourcePlanarFootprint)[1] <= 0))
        return Output::Failure("Render.OwnedStatic", "WORLD_HELPER_FOOTPRINT_INVALID", "来源占地平面尺寸必须为有界正有限数。");
    const float x = sourcePlanarFootprint ? (*sourcePlanarFootprint)[0] / 2 : 4;
    const float y = sourcePlanarFootprint ? (*sourcePlanarFootprint)[1] / 2 : 4;
    const float z = sourcePlanarFootprint ? 32.0f : 8.0f; // 编辑器marker高度，不是原版占地数据。
    OwnedModelMesh mesh; mesh.textureIndex = 0;
    const auto face = [&](std::array<std::array<float, 3>, 4> points, std::array<float, 3> normal) {
        const auto base = static_cast<std::uint16_t>(mesh.vertices.size());
        constexpr std::array<std::array<float, 2>, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (std::size_t i = 0; i < points.size(); ++i) mesh.vertices.push_back({points[i][0], points[i][1], points[i][2],
            normal[0], normal[1], normal[2], uv[i][0], uv[i][1]});
        for (const auto index : {0, 1, 2, 0, 2, 3}) mesh.indices.push_back(static_cast<std::uint16_t>(base + index));
    };
    face({{{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}}}, {0, 0, -1});
    face({{{-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}}}, {0, 0, 1});
    face({{{-x, -y, 0}, {x, -y, 0}, {x, -y, z}, {-x, -y, z}}}, {0, -1, 0});
    face({{{x, -y, 0}, {x, y, 0}, {x, y, z}, {x, -y, z}}}, {1, 0, 0});
    face({{{x, y, 0}, {-x, y, 0}, {-x, y, z}, {x, y, z}}}, {0, 1, 0});
    face({{{-x, y, 0}, {-x, -y, 0}, {-x, -y, z}, {-x, y, z}}}, {-1, 0, 0});
    ModelTextureRgba texture; texture.width = texture.height = 8; texture.rgba.resize(8 * 8 * 4);
    for (std::uint32_t row = 0; row < 8; ++row) for (std::uint32_t col = 0; col < 8; ++col) {
        const bool purple = ((row / 2 + col / 2) & 1u) != 0;
        const auto pixel = (row * 8 + col) * 4;
        texture.rgba[pixel] = static_cast<std::uint8_t>(purple ? 198u : 12u);
        texture.rgba[pixel + 1] = static_cast<std::uint8_t>(purple ? 0u : 10u);
        texture.rgba[pixel + 2] = static_cast<std::uint8_t>(purple ? 232u : 16u); texture.rgba[pixel + 3] = 255;
    }
    auto output = FreezeOwnedStaticModelAsset(std::span(&mesh, 1), std::span(&texture, 1), limits);
    if (output.Succeeded()) {
        output.diagnostics.push_back({DiagnosticSeverity::Info, "Render.OwnedStatic", "WORLD_HELPER_PREVIEW_APPROXIMATE",
            "紫黑辅助模型为编辑器预览；高度、native锚点、旋转及物编缩放未验证，不声明原版占地一致。", std::nullopt});
        if (!sourcePlanarFootprint) output.diagnostics.push_back({DiagnosticSeverity::Warning, "Render.OwnedStatic",
            "WORLD_HELPER_FOOTPRINT_UNKNOWN", "来源占地未知；仅显示8单位小marker，不表示精确范围。", std::nullopt});
    }
    return output;
}

} // namespace War3Map::Render
