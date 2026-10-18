#include "War3MapCore/Render/ClassicMinimapCandidate.h"
#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapTerrain.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace War3Map::Render {
namespace {

constexpr const char* kSubsystem = "ClassicMinimapCandidate";
constexpr std::size_t kFixedReserve = 64u * 1024u;
constexpr std::size_t kHardWorkingBytes = 128u * 1024u * 1024u;
constexpr std::string_view kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct Failure { std::string code; std::string message; };

struct InputView {
    const TerrainMinimapInput& terrain;
    const ClassicMinimapObjectDomain& objectDomain;
    std::uint64_t terrainSourceRevision = 0;
    const TerrainMinimapPostprocessSettings& postprocess;
    const ClassicMinimapObjectAssets* captured = nullptr; // 仅下方typed入口可提供，非public解锁参数
    std::size_t additionalRetainedBytes = 0;
};

[[noreturn]] void Fail(const char* code, const char* message) { throw Failure{code, message}; }

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        if (!result.diagnostics.empty()) throw Failure{result.diagnostics.front().code,
            result.diagnostics.front().message};
        throw Failure{"CLASSIC_MINIMAP_DEPENDENCY", "依赖组件失败且没有稳定诊断。"};
    }
    return std::move(*result.value);
}

void Add(std::size_t& total, std::size_t value) {
    if (value > std::numeric_limits<std::size_t>::max() - total)
        Fail("CLASSIC_MINIMAP_SIZE_OVERFLOW", "候选工作集加法溢出。");
    total += value;
}

void AddMul(std::size_t& total, std::size_t count, std::size_t size) {
    if (size && count > std::numeric_limits<std::size_t>::max() / size)
        Fail("CLASSIC_MINIMAP_SIZE_OVERFLOW", "候选工作集乘法溢出。");
    Add(total, count * size);
}

bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool Text(std::string_view value, std::size_t maximum, bool required = true) {
    return (!required || !value.empty()) && value.size() <= maximum && value.find('\0') == std::string_view::npos;
}

void ValidateLimits(const ClassicMinimapCandidateLimits& limits) {
    if (!limits.maxWorkingBytes || limits.maxWorkingBytes > kHardWorkingBytes || !limits.maxW3eBytes
        || limits.maxW3eBytes > 4u * 1024u * 1024u || !limits.maxTiles
        || limits.maxTiles > 65536 || !limits.maxRawRgbaBytes || limits.maxRawRgbaBytes > 64u * 1024u * 1024u
        || !limits.maxBlpBytes || limits.maxBlpBytes > 1024u * 1024u || !limits.maxSourceTextBytes
        || limits.maxSourceTextBytes > 64u * 1024u)
        Fail("CLASSIC_MINIMAP_LIMITS", "候选预算只能在非零硬上限内收紧。");
}

void ValidateObjectState(ClassicMinimapMemberState state, std::string_view hash, std::size_t bytes,
    std::size_t maximumText, const ClassicMinimapObjectAssets* captured) {
    if (state == ClassicMinimapMemberState::ParsedEmpty) {
        if (!captured) Fail("CLASSIC_MINIMAP_OBJECT_CAPTURE_REQUIRED", "ParsedEmpty只能由sealed实际捕获入口提供。");
        if (!bytes || !Sha(hash) || hash.size() > maximumText)
            Fail("CLASSIC_MINIMAP_OBJECT_DOMAIN_IDENTITY", "ParsedEmpty必须保留实际非零成员长度与SHA。");
        return;
    }
    if (state != ClassicMinimapMemberState::Unknown && state != ClassicMinimapMemberState::Missing
        && state != ClassicMinimapMemberState::Empty)
        Fail("CLASSIC_MINIMAP_OBJECT_DOMAIN_INVALID", "对象成员状态枚举无效。");
    if (state == ClassicMinimapMemberState::Unknown)
        Fail("CLASSIC_MINIMAP_OBJECT_DOMAIN_UNKNOWN", "对象成员状态未知，不能把未知当作空层。");
    if (state == ClassicMinimapMemberState::Missing) {
        if (bytes || !hash.empty()) Fail("CLASSIC_MINIMAP_OBJECT_DOMAIN_IDENTITY", "Missing对象成员不能带正文尺寸或摘要。");
        return;
    }
    if (bytes || hash != kEmptySha256 || hash.size() > maximumText)
        Fail("CLASSIC_MINIMAP_OBJECT_DOMAIN_IDENTITY", "Empty对象成员必须是零字节且SHA256(empty)。");
}

void ValidateObjectDomain(const InputView& input, const ClassicMinimapCandidateLimits& limits) {
    const auto& expected = input.terrain.binding;
    const auto& domain = input.objectDomain;
    if (!Sha(expected.sourceSha256) || !Sha(expected.terrainSha256)
        || !Text(expected.documentId, 128) || !expected.documentGeneration)
        Fail("CLASSIC_MINIMAP_BINDING", "地形来源绑定不完整。");
    if (domain.sourceSha256 != expected.sourceSha256 || domain.documentId != expected.documentId
        || domain.documentGeneration != expected.documentGeneration || domain.revision != expected.revision
        || domain.terrainSourceRevision != input.terrainSourceRevision)
        Fail("CLASSIC_MINIMAP_OBJECT_DOMAIN_STALE", "对象域与当前地形来源、会话代际或修订不一致。");
    ValidateObjectState(domain.unitsState, domain.unitsMemberSha256, domain.unitsMemberBytes, limits.maxSourceTextBytes, input.captured);
    ValidateObjectState(domain.doodadsState, domain.doodadsMemberSha256, domain.doodadsMemberBytes, limits.maxSourceTextBytes, input.captured);
}

void AddTextCaps(std::size_t& bytes, const std::string& text) {
    Add(bytes, text.capacity()); Add(bytes, 1);
}

std::size_t TerrainInputCapacity(const TerrainMinimapInput& terrain) {
    std::size_t bytes = sizeof(terrain);
    AddTextCaps(bytes, terrain.binding.documentId); AddTextCaps(bytes, terrain.binding.sourceSha256);
    AddTextCaps(bytes, terrain.binding.terrainSha256); Add(bytes, terrain.currentW3e.capacity());
    if (terrain.sourceW3i) Add(bytes, terrain.sourceW3i->capacity());
    AddMul(bytes, terrain.groundAtlases.capacity(), sizeof(std::shared_ptr<const TerrainAtlasSnapshot>));
    for (std::size_t i = 0; i < terrain.groundAtlases.size(); ++i) {
        const auto& pointer = terrain.groundAtlases[i];
        if (!pointer) Fail("CLASSIC_MINIMAP_ATLAS", "地面材质缺少冻结atlas。");
        if (std::find(terrain.groundAtlases.begin(), terrain.groundAtlases.begin() + static_cast<std::ptrdiff_t>(i), pointer)
            != terrain.groundAtlases.begin() + static_cast<std::ptrdiff_t>(i)) continue;
        const auto& atlas = *pointer;
        Add(bytes, sizeof(atlas)); AddTextCaps(bytes, atlas.source.canonicalPath);
        AddTextCaps(bytes, atlas.source.sourceId); AddTextCaps(bytes, atlas.source.sourcePath);
        if (atlas.source.readProvenance) {
            Add(bytes, sizeof(Resources::ResourceReadProvenance));
            AddTextCaps(bytes, atlas.source.readProvenance->containerIdentity);
            AddTextCaps(bytes, atlas.source.readProvenance->memberPath);
            AddTextCaps(bytes, atlas.source.readProvenance->sourceContext);
        }
        AddTextCaps(bytes, atlas.sourceSha256); Add(bytes, atlas.image.rgba.capacity());
        AddMul(bytes, atlas.cells.capacity(), sizeof(TerrainAtlasCell));
        AddMul(bytes, atlas.additionalMips.capacity(), sizeof(Image::DecodedImage));
        for (const auto& mip : atlas.additionalMips) Add(bytes, mip.rgba.capacity());
    }
    return bytes;
}

std::size_t RetainedInputBytes(const InputView& input) {
    std::size_t bytes = kFixedReserve + sizeof(input);
    Add(bytes, sizeof(input.objectDomain)); Add(bytes, sizeof(input.postprocess));
    Add(bytes, TerrainInputCapacity(input.terrain));
    Add(bytes, input.additionalRetainedBytes);
    AddTextCaps(bytes, input.objectDomain.sourceSha256); AddTextCaps(bytes, input.objectDomain.documentId);
    AddTextCaps(bytes, input.objectDomain.unitsMemberSha256); AddTextCaps(bytes, input.objectDomain.doodadsMemberSha256);
    if (input.postprocess.noiseSamples) AddMul(bytes, input.postprocess.noiseSamples->capacity(), sizeof(std::uint16_t));
    return bytes;
}

void ValidateAtlas(const TerrainAtlasSnapshot& atlas, std::size_t sourceTextLimit) {
    if (!Sha(atlas.sourceSha256) || !atlas.sourceBytes || !atlas.image.Valid()
        || !Text(atlas.source.canonicalPath, 4096) || !Text(atlas.source.sourceId, 128)
        || !Text(atlas.source.sourcePath, 4096) || (atlas.columns != 4 && atlas.columns != 8)
        || atlas.image.width != atlas.sourceWidth || atlas.image.height != atlas.sourceHeight
        || !atlas.image.width || atlas.image.height % 4 || atlas.image.width != atlas.image.height / 4 * atlas.columns
        || atlas.cells.size() != static_cast<std::size_t>(atlas.columns) * 4
        || atlas.validatedMipCount != atlas.additionalMips.size() + 1)
        Fail("CLASSIC_MINIMAP_ATLAS", "地面atlas的来源、尺寸、cell或mip身份无效。");
    if (atlas.sourceSha256.size() > sourceTextLimit || atlas.image.rgba.capacity() > 64u * 1024u * 1024u)
        Fail("CLASSIC_MINIMAP_BUDGET", "地面atlas正文或来源摘要超过候选预算。");
    const auto cell = atlas.image.height / 4;
    for (std::uint32_t row = 0; row < 4; ++row) for (std::uint32_t column = 0; column < atlas.columns; ++column) {
        const auto& actual = atlas.cells[static_cast<std::size_t>(row) * atlas.columns + column];
        if (actual.column != column || actual.row != row || actual.x != column * cell || actual.y != row * cell
            || actual.width != cell || actual.height != cell)
            Fail("CLASSIC_MINIMAP_ATLAS", "地面atlas的cell矩形与冻结像素布局不一致。");
    }
}

std::string ObjectDomainIdentity(const ClassicMinimapObjectDomain& domain, std::string_view policy) {
    std::string text;
    text.reserve(512);
    text.append(policy).append("|").append(domain.sourceSha256).append("|")
        .append(domain.documentId).append("|").append(std::to_string(domain.documentGeneration)).append("|")
        .append(std::to_string(domain.revision)).append("|").append(std::to_string(domain.terrainSourceRevision)).append("|")
        .append(std::to_string(static_cast<unsigned>(domain.unitsState))).append("|").append(domain.unitsMemberSha256).append("|")
        .append(std::to_string(domain.unitsMemberBytes)).append("|").append(std::to_string(static_cast<unsigned>(domain.doodadsState))).append("|")
        .append(domain.doodadsMemberSha256).append("|").append(std::to_string(domain.doodadsMemberBytes));
    return Take(Sha256Text(text));
}

void SetRgba(Image::DecodedImage& image, std::size_t pixel, const std::array<std::uint8_t, 4>& rgba) {
    for (std::size_t channel = 0; channel < 4; ++channel) image.rgba[pixel * 4 + channel] = static_cast<std::byte>(rgba[channel]);
}

std::array<std::uint8_t, 4> GroundColor(const std::vector<std::array<std::uint8_t, 4>>& colors,
    std::uint8_t index, bool boundary) {
    auto color = colors[index];
    if (boundary) for (std::size_t channel = 0; channel < 3; ++channel)
        color[channel] = static_cast<std::uint8_t>(static_cast<unsigned>(color[channel]) * 25u / 255u);
    color[3] = 255;
    return color;
}

Result<ClassicMinimapCandidate> FailureResult(std::string_view code, std::string_view message) {
    return Result<ClassicMinimapCandidate>::Failure(kSubsystem, std::string(code), std::string(message));
}

Result<ClassicMinimapCandidate> Build(const InputView& input,
    const ClassicMinimapCandidateLimits& limits) {
    try {
        ValidateLimits(limits);
        if (input.terrain.rangePolicy != TerrainMinimapRangePolicy::FullW3e)
            Fail("CLASSIC_MINIMAP_PLAYABLE_RECT_UNRESOLVED", "第一版只生成FullW3e，W3I playable crop语义仍未证实。");
        if (input.terrain.generation)
            Fail("CLASSIC_MINIMAP_GENERATION_UNSUPPORTED", "Classic候选暂不消费未接通的S0 generation配置。");
        if (input.terrain.sourceW3i)
            Fail("CLASSIC_MINIMAP_W3I_UNEXPECTED", "FullW3e候选不静默忽略额外W3I来源。");
        ValidateObjectDomain(input, limits);
        const auto retained = RetainedInputBytes(input);
        if (retained > limits.maxWorkingBytes) Fail("CLASSIC_MINIMAP_BUDGET", "冻结来源保活已超过128MiB候选预算。");
        if (input.terrain.currentW3e.size() > limits.maxW3eBytes)
            Fail("CLASSIC_MINIMAP_W3E_BUDGET", "当前W3E超过Classic候选读取上限。");
        std::size_t reserveExtra = kFixedReserve;
        AddMul(reserveExtra, limits.maxTiles, 8); // 两份原图：输出与postprocess调用输入
        Add(reserveExtra, 4u * 256u * 256u * 4u); // 两内容pingpong及两个256画布
        Add(reserveExtra, 16u * 1024u * 1024u); Add(reserveExtra, limits.maxBlpBytes);
        // Hash::Sha256无control时会复制整个当前W3E作chunk；必须在首次SHA之前预收。
        // 后续图像/BLP散列的有界临时正文另包含在较大的JPEG阶段预留中。
        Add(reserveExtra, input.terrain.currentW3e.size());
        if (retained > limits.maxWorkingBytes || reserveExtra > limits.maxWorkingBytes - retained)
            Fail("CLASSIC_MINIMAP_BUDGET", "解析前预收原图、postprocess与JPEG工作集后预算不足。");
        const auto terrainSha = Take(Sha256(input.terrain.currentW3e));
        if (terrainSha != input.terrain.binding.terrainSha256)
            Fail("CLASSIC_MINIMAP_TERRAIN_STALE", "当前W3E字节与绑定terrain SHA不同。");
        MapData::TerrainReadLimits read;
        read.maxBytes = input.terrain.currentW3e.size();
        read.maxTiles = limits.maxTiles;
        read.maxDecodedBytes = limits.maxWorkingBytes - retained - reserveExtra;
        const auto parsed = Take(MapData::ParseMapTerrain(input.terrain.currentW3e, read));
        const auto grid = parsed.Snapshot();
        if (!grid || grid->header.version != 11 || grid->header.width < 2 || grid->header.height < 2
            || grid->header.groundTilesets.empty() || grid->header.groundTilesets.size() > 16)
            Fail("CLASSIC_MINIMAP_TERRAIN", "FullW3e候选只接受有界classic v11地面网格。");
        const auto tileWidth = grid->header.width - 1, tileHeight = grid->header.height - 1;
        if (tileHeight && tileWidth > std::numeric_limits<std::size_t>::max() / tileHeight)
            Fail("CLASSIC_MINIMAP_SIZE_OVERFLOW", "W3E tile数量乘法溢出。");
        const auto tileCount = static_cast<std::size_t>(tileWidth) * tileHeight;
        if (tileCount > limits.maxTiles || tileCount > limits.maxRawRgbaBytes / 4)
            Fail("CLASSIC_MINIMAP_BUDGET", "逐格原图超过候选预算。");
        if (grid->header.height && grid->header.width > std::numeric_limits<std::size_t>::max() / grid->header.height)
            Fail("CLASSIC_MINIMAP_SIZE_OVERFLOW", "W3E vertex数量乘法溢出。");
        const auto vertexCount = static_cast<std::size_t>(grid->header.width) * grid->header.height;
        if (grid->tiles.size() != tileCount || grid->vertices.size() != vertexCount)
            Fail("CLASSIC_MINIMAP_TERRAIN", "W3E解析网格尺寸与声明头不一致。");
        // 统一候选口径：冻结输入、W3E解析副本、逐格RGBA、两个256画布、JPEG工作区/输出
        // 必须在同一硬帽内；子组件仍会按自身实际capacity再次拒绝，而不会扩大此帽。
        auto peak = retained;
        Add(peak, sizeof(MapData::MapTerrain)); Add(peak, sizeof(MapData::TerrainGridSnapshot));
        AddMul(peak, grid->vertices.capacity(), sizeof(MapData::TerrainVertex));
        AddMul(peak, grid->tiles.capacity(), sizeof(MapData::TerrainTile));
        for (const auto* table : {&grid->header.groundTilesets, &grid->header.cliffTilesets}) {
            AddMul(peak, table->capacity(), sizeof(std::string));
            for (const auto& rawcode : *table) AddTextCaps(peak, rawcode);
        }
        Add(peak, parsed.OriginalBytes().capacity());
        AddMul(peak, tileCount, 4); // postprocess调用输入的独立原图copy
        AddMul(peak, tileCount, 4);
        Add(peak, 2u * 256u * 256u * 4u);
        Add(peak, std::min<std::size_t>(16u * 1024u * 1024u, limits.maxWorkingBytes));
        Add(peak, limits.maxBlpBytes);
        Add(peak, sizeof(ClassicMinimapCandidate));
        if (peak > limits.maxWorkingBytes) Fail("CLASSIC_MINIMAP_BUDGET", "解析、逐格图、后处理与JPEG峰值超过候选预算。");
        if (input.terrain.groundAtlases.size() != grid->header.groundTilesets.size())
            Fail("CLASSIC_MINIMAP_ATLAS", "ground atlas必须与W3E ground tileset按槽位同序。");
        std::vector<std::array<std::uint8_t, 4>> colors;
        colors.reserve(input.terrain.groundAtlases.size());
        for (const auto& pointer : input.terrain.groundAtlases) {
            if (!pointer) Fail("CLASSIC_MINIMAP_ATLAS", "ground atlas缺失。");
            ValidateAtlas(*pointer, limits.maxSourceTextBytes);
            colors.push_back(Take(ComputeTerrainMinimapRepresentativeColor(pointer->image, TerrainMinimapTextureRole::Ground)).rgba);
        }
        ClassicMinimapCandidate output;
        output.manifest.sourceSha256 = input.terrain.binding.sourceSha256;
        output.manifest.terrainSha256 = terrainSha;
        output.manifest.objectDomainPolicy = input.captured ? ClassicMinimapObjectAssetFacts::capturePolicy
            : ClassicMinimapManifest::objectDomainDeclaration;
        output.manifest.objectDomainSha256 = ObjectDomainIdentity(input.objectDomain, output.manifest.objectDomainPolicy);
        if (input.captured) output.manifest.objectCaptureIdentitySha256 = input.captured->IdentitySha256();
        output.manifest.sourceWidth = tileWidth; output.manifest.sourceHeight = tileHeight;
        output.manifest.sourceTiles = tileCount; output.manifest.sourceRectangle = {0, 0, tileWidth, tileHeight};
        output.raw.width = tileWidth; output.raw.height = tileHeight;
        output.raw.rgba.resize(tileCount * 4);
        for (std::uint32_t y = 0; y < tileHeight; ++y) for (std::uint32_t x = 0; x < tileWidth; ++x) {
            const auto tileIndex = static_cast<std::size_t>(y) * tileWidth + x;
            const auto& corners = grid->tiles[tileIndex].vertices;
            if (corners[0] >= grid->vertices.size() || corners[1] >= grid->vertices.size()
                || corners[2] >= grid->vertices.size() || corners[3] >= grid->vertices.size())
                Fail("CLASSIC_MINIMAP_TERRAIN", "W3E tile角点索引越界。");
            const auto& sw = grid->vertices[corners[0]];
            const auto& se = grid->vertices[corners[1]];
            const auto& nw = grid->vertices[corners[2]];
            const auto& ne = grid->vertices[corners[3]];
            for (const auto* corner : {&sw, &se, &nw, &ne}) {
                if (corner->HasWater()) Fail("CLASSIC_MINIMAP_WATER", "实际tile含水标志，水色/深水阈值未解析。");
                if (corner->UnknownWaterFlags() || corner->UnknownTextureFlags())
                    Fail("CLASSIC_MINIMAP_UNKNOWN_FLAGS", "实际tile含未解释的W3E标志。");
                if (corner->IsRamp() || corner->CliffLevel() != sw.CliffLevel())
                    Fail("CLASSIC_MINIMAP_CLIFF_UNRESOLVED", "实际tile含悬崖/坡道邻域，悬崖例外条件未解析。");
            }
            if (sw.IsBlight()) output.manifest.ignoredBlightSeen = true;
            if (sw.GroundVariation()) output.manifest.ignoredVariationSeen = true;
            const auto index = sw.GroundTextureIndex();
            if (index >= colors.size()) Fail("CLASSIC_MINIMAP_GROUND", "实际SW地面槽超出冻结atlas目录。");
            // W3E 的 Boundary 与 MapEdge 都进入 +12 的 0xC0 外缘语义，均使用已知 A230 黑混合。
            const bool boundary = sw.IsBoundary() || sw.IsMapEdge();
            if (boundary) ++output.manifest.boundaryTiles;
            SetRgba(output.raw, tileIndex, GroundColor(colors, index, boundary));
        }
        output.manifest.rawRgbaSha256 = Take(Sha256(output.raw.rgba));
        TerrainMinimapPostprocessLimits postLimits;
        postLimits.maxWorkingBytes = limits.maxWorkingBytes;
        auto post = Take(PostprocessTerrainMinimap(
            {output.raw.width, output.raw.height, output.raw.rgba}, input.postprocess, postLimits));
        output.manifest.postprocessFacts = post.facts;
        if (input.postprocess.noiseSamples)
            output.manifest.noiseSamplesSha256 = Take(Sha256(std::as_bytes(std::span(*input.postprocess.noiseSamples))));
        output.preNoise = std::move(post.preNoise); output.finalImage = std::move(post.finalImage);
        output.manifest.preNoiseRgbaSha256 = Take(Sha256(output.preNoise.rgba));
        output.manifest.finalRgbaSha256 = Take(Sha256(output.finalImage.rgba));
        Image::BlpJpegEncodeLimits jpegLimits;
        jpegLimits.maxWorkingBytes = std::min<std::size_t>(jpegLimits.maxWorkingBytes, limits.maxWorkingBytes);
        jpegLimits.maxOutputBytes = limits.maxBlpBytes;
        auto encoded = Take(Image::EncodeBlpJpegSingleMip(output.finalImage, jpegLimits));
        if (encoded.quality != 40 || encoded.width != 256 || encoded.height != 256 || encoded.bytes.capacity() > limits.maxBlpBytes)
            Fail("CLASSIC_MINIMAP_JPEG", "JPEG候选编码身份或容量不符合q40单mip合同。");
        output.manifest.blpSha256 = Take(Sha256(encoded.bytes));
        output.encoded = std::move(encoded);
        return Result<ClassicMinimapCandidate>::Success(std::move(output));
    } catch (const Failure& failure) {
        return FailureResult(failure.code, failure.message);
    } catch (const std::bad_alloc&) {
        return FailureResult("CLASSIC_MINIMAP_ALLOCATION", "候选分配失败，不返回部分结果。");
    } catch (const std::length_error&) {
        return FailureResult("CLASSIC_MINIMAP_ALLOCATION", "候选容器容量异常，不返回部分结果。");
    }
}

} // namespace

Result<ClassicMinimapCandidate> BuildClassicMinimapCandidate(const ClassicMinimapCandidateInput& input,
    const ClassicMinimapCandidateLimits& limits) {
    return Build({input.terrain, input.objectDomain, input.terrainSourceRevision, input.postprocess}, limits);
}

Result<ClassicMinimapCandidate> BuildClassicMinimapCandidate(const ClassicMinimapObjectAssets& assets,
    const TerrainMinimapPostprocessSettings& postprocess, const ClassicMinimapCandidateLimits& limits) {
    try {
        ValidateLimits(limits);
        if (!assets.captured_ || !Sha(assets.identitySha256_) || !Sha(assets.terrain_.IdentitySha256()))
            Fail("CLASSIC_MINIMAP_OBJECT_CAPTURE_STALE", "sealed资产缺少有效捕获身份或已被move。");
        // 不复制TerrainMinimapInput/W3E/atlas或settings。额外计入组合快照持有的表/依赖与对象见证。
        const auto inputCapacity = TerrainInputCapacity(assets.terrain_.Input());
        if (inputCapacity > assets.facts_.retainedTerrainCapacityBytes)
            Fail("CLASSIC_MINIMAP_OBJECT_CAPTURE_STALE", "sealed资产容量账与当前拥有的Terrain输入不同。");
        auto extra = assets.facts_.retainedTerrainCapacityBytes - inputCapacity;
        Add(extra, assets.facts_.retainedAuxiliaryCapacityBytes);
        return Build({assets.terrain_.Input(), assets.domain_, assets.terrain_.Binding().terrainSourceRevision, postprocess,
            &assets, extra}, limits);
    } catch (const Failure& failure) {
        return FailureResult(failure.code, failure.message);
    } catch (const std::bad_alloc&) {
        return FailureResult("CLASSIC_MINIMAP_ALLOCATION", "候选分配失败，不返回部分结果。");
    } catch (const std::length_error&) {
        return FailureResult("CLASSIC_MINIMAP_ALLOCATION", "候选容器容量异常，不返回部分结果。");
    }
}

} // namespace War3Map::Render
