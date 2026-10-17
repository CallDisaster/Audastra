#include "War3MapCore/Render/TerrainMinimapCandidate.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace War3Map::Render {
namespace {

constexpr TerrainMinimapLimits kHard{};
constexpr TerrainGroundMeshLimits kMeshHard{};
constexpr std::uint32_t kSize = 256;
constexpr std::size_t kOutputBytes = kSize * kSize * 4;
constexpr std::size_t kEncodingWorkReserve = Image::BlpOpaqueQuantizeLimits{}.maxWorkingBytes;
constexpr std::size_t kEncodedOutputReserve = Image::BlpOpaqueQuantizeLimits{}.maxOutputBytes;
constexpr std::size_t kHashReserve = 1024 * 1024;
constexpr std::size_t kManifestReserve = 1024 * 1024;
constexpr std::size_t kMaximumHashBytes = 256 * 1024 * 1024;
constexpr const char* kSubsystem = "TerrainMinimapCandidate";

struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{{{DiagnosticSeverity::Error, kSubsystem, code, message, {}}}};
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

void Check(Status status) {
    if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)};
}

void Charge(std::size_t& bytes, std::size_t count, std::size_t element = 1) {
    if (element && count > (std::numeric_limits<std::size_t>::max() - bytes) / element)
        Fail("TERRAIN_MINIMAP_BUDGET", "内存账乘加溢出。");
    bytes += count * element;
}

bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool Text(std::string_view value, std::size_t maximum, bool required = true) {
    return (!required || !value.empty()) && value.size() <= maximum && value.find('\0') == std::string_view::npos;
}

void ChargeText(std::size_t& bytes, const std::string& value) { Charge(bytes, value.capacity()); Charge(bytes, 1); }

void ChargeBinding(std::size_t& bytes, const TerrainInspectionBinding& binding) {
    ChargeText(bytes, binding.documentId); ChargeText(bytes, binding.sourceSha256); ChargeText(bytes, binding.terrainSha256);
}

void ChargeLocation(std::size_t& bytes, const Resources::ResourceLocation& source) {
    ChargeText(bytes, source.canonicalPath); ChargeText(bytes, source.sourceId); ChargeText(bytes, source.sourcePath);
}

class Digest {
public:
    explicit Digest(std::size_t& observed) : stream_(kMaximumHashBytes), observed_(observed) {
        // Sha256Stream 惰性分配 CNG object；空 Update 初始化 provider/hash，不改变摘要字节。
        Check(stream_.Update({})); Observe();
    }
    void Bytes(std::span<const std::byte> bytes) { Check(stream_.Update(bytes)); Observe(); }
    void U32(std::uint32_t value) {
        std::array<std::byte, 4> bytes{};
        for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::byte>((value >> (i * 8)) & 255);
        Bytes(bytes);
    }
    void U64(std::uint64_t value) {
        U32(static_cast<std::uint32_t>(value)); U32(static_cast<std::uint32_t>(value >> 32));
    }
    void F32(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void F64(double value) { U64(std::bit_cast<std::uint64_t>(value)); }
    void String(std::string_view value) {
        U64(value.size()); Bytes(std::as_bytes(std::span(value.data(), value.size())));
    }
    std::string Finish() {
        auto result = stream_.Finish(); Observe(); return Take(std::move(result));
    }
private:
    void Observe() {
        const auto bytes = stream_.ScratchBytes();
        observed_ = std::max(observed_, bytes);
        if (bytes > kHashReserve) Fail("TERRAIN_MINIMAP_BUDGET", "SHA 工作缓冲超过预留预算。");
    }
    Sha256Stream stream_;
    std::size_t& observed_;
};

std::string HashBytes(std::span<const std::byte> bytes, std::size_t& scratch) {
    Digest digest(scratch); digest.Bytes(bytes); return digest.Finish();
}

void HashBinding(Digest& digest, const TerrainInspectionBinding& binding) {
    digest.String(binding.documentId); digest.U64(binding.documentGeneration); digest.U64(binding.revision);
    digest.String(binding.sourceSha256); digest.String(binding.terrainSha256);
}

void HashLocation(Digest& digest, const Resources::ResourceLocation& location) {
    digest.String(location.canonicalPath); digest.U32(static_cast<std::uint32_t>(location.layer));
    digest.U32(std::bit_cast<std::uint32_t>(location.priority)); digest.String(location.sourceId); digest.String(location.sourcePath);
}

struct Counts { std::size_t vertices = 0, tiles = 0, palette = 0; };

std::size_t ValidateInputs(const TerrainMinimapInput& input, const TerrainMinimapLimits& limits) {
    if (!limits.maxTiles || limits.maxTiles > kHard.maxTiles || !limits.maxW3eBytes || limits.maxW3eBytes > kHard.maxW3eBytes
        || !limits.maxRetainedInputBytes || limits.maxRetainedInputBytes > kHard.maxRetainedInputBytes
        || !limits.maxWorkingBytes || limits.maxWorkingBytes > kHard.maxWorkingBytes)
        Fail("TERRAIN_MINIMAP_LIMITS", "候选预算必须为正且只能收紧硬上限。");
    if (!Text(input.binding.documentId, 128) || !Sha(input.binding.sourceSha256) || !Sha(input.binding.terrainSha256))
        Fail("TERRAIN_MINIMAP_BINDING", "候选需要完整的冻结文档及当前 W3E SHA 身份。");
    if (input.groundAtlases.empty() || input.groundAtlases.size() > 16)
        Fail("TERRAIN_MINIMAP_ATLAS", "候选需要有界、与地面表同序的冻结 atlas 快照。");
    std::size_t retained = sizeof(input), rgba = 0;
    ChargeBinding(retained, input.binding); Charge(retained, input.currentW3e.capacity());
    Charge(retained, input.groundAtlases.capacity(), sizeof(input.groundAtlases.front()));
    for (std::size_t slot = 0; slot < input.groundAtlases.size(); ++slot) {
        const auto& pointer = input.groundAtlases[slot];
        if (!pointer) Fail("TERRAIN_MINIMAP_ATLAS", "候选不接受缺失 atlas 或高度色回退。");
        if (std::find(input.groundAtlases.begin(), input.groundAtlases.begin() + static_cast<std::ptrdiff_t>(slot), pointer)
            != input.groundAtlases.begin() + static_cast<std::ptrdiff_t>(slot)) continue;
        const auto& atlas = *pointer;
        const auto& source = atlas.source;
        if (!Text(source.canonicalPath, 4096) || !Text(source.sourceId, 128) || !Text(source.sourcePath, 4096)
            || !Sha(atlas.sourceSha256) || !atlas.sourceBytes
            || static_cast<unsigned>(source.layer) > static_cast<unsigned>(Resources::ResourceLayer::Other))
            Fail("TERRAIN_MINIMAP_ATLAS_IDENTITY", "atlas 缺少有界的声明来源与编码源身份。");
        const auto w = atlas.image.width, h = atlas.image.height;
        if ((atlas.columns != 4 && atlas.columns != 8) || !w || !h || w > kMeshHard.maxTextureDimension
            || h > kMeshHard.maxTextureDimension || h % 4 || w != h / 4 * atlas.columns
            || atlas.sourceWidth != w || atlas.sourceHeight != h || !atlas.image.Valid()
            || atlas.cells.size() != atlas.columns * 4 || atlas.additionalMips.size() > 11
            || atlas.validatedMipCount != atlas.additionalMips.size() + 1)
            Fail("TERRAIN_MINIMAP_ATLAS", "atlas 布局、声明原链长度或 mip0 尺寸无效。");
        const auto cellSize = h / 4;
        for (std::uint32_t row = 0; row < 4; ++row) for (std::uint32_t column = 0; column < atlas.columns; ++column) {
            const auto& cell = atlas.cells[static_cast<std::size_t>(row) * atlas.columns + column];
            if (cell.column != column || cell.row != row || cell.x != column * cellSize || cell.y != row * cellSize
                || cell.width != cellSize || cell.height != cellSize)
                Fail("TERRAIN_MINIMAP_ATLAS", "atlas cell 元数据与冻结像素布局不一致。");
        }
        std::size_t chainBytes = atlas.image.rgba.size();
        auto mw = w, mh = h;
        for (const auto& mip : atlas.additionalMips) {
            if (mw == 1 && mh == 1) Fail("TERRAIN_MINIMAP_ATLAS", "原链在 1x1 后仍有额外 mip。");
            mw = std::max(1u, mw / 2); mh = std::max(1u, mh / 2);
            if (mip.width != mw || mip.height != mh || !mip.Valid())
                Fail("TERRAIN_MINIMAP_ATLAS", "原 mip 链存在缺级或尺寸不一致。");
            Charge(chainBytes, mip.rgba.size());
        }
        if (chainBytes > kMeshHard.maxTextureChainBytes) Fail("TERRAIN_MINIMAP_BUDGET", "单 atlas 原链超过 GroundMesh 硬预算。");
        Charge(rgba, chainBytes);
        Charge(retained, sizeof(atlas)); ChargeLocation(retained, source); ChargeText(retained, atlas.sourceSha256);
        Charge(retained, atlas.cells.capacity(), sizeof(TerrainAtlasCell));
        Charge(retained, atlas.additionalMips.capacity(), sizeof(Image::DecodedImage));
        Charge(retained, atlas.image.rgba.capacity());
        for (const auto& mip : atlas.additionalMips) Charge(retained, mip.rgba.capacity());
    }
    if (rgba > kMeshHard.maxRgbaBytes || retained > limits.maxRetainedInputBytes)
        Fail("TERRAIN_MINIMAP_BUDGET", "冻结输入的实际 capacity 保活账超过预算。");
    return retained;
}

struct Layer {
    std::uint32_t firstVertex = 0, texture = 0, cell = 0;
    std::array<double, 2> lod{};
};
struct TileLayers { std::array<Layer, 4> layers{}; std::uint32_t count = 0; };

void Estimate(const TerrainMinimapInput& input, const Counts& counts, TerrainMinimapMemoryFacts& memory,
    const TerrainMinimapLimits& limits) {
    // 在 Parse/完整 mesh 前按原 tile 最坏 1 base + 3 transition 计费。Overlay 内部增长
    // scratch 按每层 32 字节，另计公开映射；不借 256 输出把大网格工作集隐去。
    std::size_t extra = sizeof(MapData::MapTerrain) + sizeof(MapData::TerrainGridSnapshot)
        + kHashReserve + kEncodingWorkReserve + 8192;
    Charge(extra, input.currentW3e.size(), 2); // Parse 原字节 + 内部重新 Serialize
    Charge(extra, counts.vertices, sizeof(MapData::TerrainVertex));
    Charge(extra, counts.tiles, sizeof(MapData::TerrainTile));
    Charge(extra, counts.palette, sizeof(std::string) + 64);
    Charge(extra, counts.tiles, 16 * sizeof(TerrainGroundVertex) + 24 * sizeof(std::uint32_t)
        + 3 * 32 + 3 * sizeof(std::uint32_t) + 5 + 4 + sizeof(TileLayers));
    // block 数不超过 tile 数；采用向上整除的行列上界。
    Charge(extra, (counts.tiles + 15) / 16 + 16, sizeof(TerrainGroundBlock));
    Charge(extra, 32, sizeof(TerrainGroundBatch));
    Charge(extra, 16, sizeof(std::shared_ptr<const TerrainAtlasSnapshot>));
    memory.estimatedExtraPeakBytes = extra;
    memory.estimatedOutputBytes = sizeof(TerrainMinimapCandidate) + kOutputBytes + kEncodedOutputReserve + kManifestReserve;
    memory.estimatedPeakBytes = memory.retainedInputCapacityBytes;
    Charge(memory.estimatedPeakBytes, extra); Charge(memory.estimatedPeakBytes, memory.estimatedOutputBytes);
    if (memory.estimatedPeakBytes > limits.maxWorkingBytes)
        Fail("TERRAIN_MINIMAP_BUDGET", "原始网格、最坏全部层与输入保活的构建前工作集估算超过预算。");
}

std::size_t TerrainCapacity(const MapData::MapTerrain& terrain) {
    const auto grid = terrain.Snapshot();
    std::size_t bytes = sizeof(terrain) + sizeof(*grid);
    Charge(bytes, terrain.OriginalBytes().capacity()); Charge(bytes, grid->vertices.capacity(), sizeof(MapData::TerrainVertex));
    Charge(bytes, grid->tiles.capacity(), sizeof(MapData::TerrainTile));
    for (const auto* table : {&grid->header.groundTilesets, &grid->header.cliffTilesets}) {
        Charge(bytes, table->capacity(), sizeof(std::string));
        for (const auto& item : *table) ChargeText(bytes, item);
    }
    return bytes;
}

std::size_t MeshCapacity(const TerrainGroundMesh& mesh) {
    std::size_t bytes = sizeof(mesh);
    Charge(bytes, mesh.vertices.capacity(), sizeof(TerrainGroundVertex)); Charge(bytes, mesh.indices.capacity(), sizeof(std::uint32_t));
    Charge(bytes, mesh.batches.capacity(), sizeof(TerrainGroundBatch)); Charge(bytes, mesh.blocks.capacity(), sizeof(TerrainGroundBlock));
    Charge(bytes, mesh.textures.capacity(), sizeof(std::shared_ptr<const TerrainAtlasSnapshot>));
    Charge(bytes, mesh.baseTiles.capacity(), sizeof(std::uint32_t)); Charge(bytes, mesh.overlayTiles.capacity(), sizeof(std::uint32_t));
    Charge(bytes, mesh.waterTiles.capacity(), sizeof(std::uint32_t)); Charge(bytes, mesh.hiddenTiles.capacity());
    ChargeText(bytes, mesh.waterFrameSourcePath); ChargeText(bytes, mesh.waterFrameSourceSha256);
    return bytes;
}

void CheckDomain(const MapData::TerrainGridSnapshot& grid) {
    if (grid.header.version != 11) Fail("TERRAIN_MINIMAP_VERSION", "第一版候选仅支持 classic W3E v11。");
    if (grid.header.groundTilesets.empty() || grid.header.groundTilesets.size() > 16)
        Fail("TERRAIN_MINIMAP_PALETTE", "第一版候选需要 1..16 个地面材质槽。");
    const auto level = grid.vertices.front().CliffLevel();
    for (const auto& vertex : grid.vertices) {
        if (vertex.IsBoundary()) Fail("TERRAIN_MINIMAP_BOUNDARY", "第一版候选不解释 Boundary 边界标志。");
        if (vertex.IsMapEdge()) Fail("TERRAIN_MINIMAP_MAP_EDGE", "第一版候选不解释 MapEdge 外缘标志。");
        if (vertex.UnknownWaterFlags() || vertex.UnknownTextureFlags())
            Fail("TERRAIN_MINIMAP_UNKNOWN_FLAGS", "候选不忽略未知地形标志。");
        if (vertex.IsRamp()) Fail("TERRAIN_MINIMAP_RAMP", "第一版候选不近似坡道。");
        if (vertex.HasWater()) Fail("TERRAIN_MINIMAP_WATER", "第一版候选不近似水面。");
        if (vertex.IsBlight()) Fail("TERRAIN_MINIMAP_BLIGHT", "第一版候选不近似荒芜。");
        if (vertex.CliffLevel() != level)
            Fail("TERRAIN_MINIMAP_NOT_FLAT", "候选需要统一 cliff layer，不近似悬崖层级变化。");
        if (vertex.GroundTextureIndex() >= grid.header.groundTilesets.size()
            || (vertex.CliffTextureIndex() != 15 && vertex.CliffTextureIndex() >= grid.header.cliffTilesets.size()))
            Fail("TERRAIN_MINIMAP_PALETTE", "候选不解释未解析的地面或悬崖材质索引。");
        if (vertex.GroundVariation() > 17)
            Fail("TERRAIN_MINIMAP_VARIATION", "第一版候选不推测 18..31 地面变体的语义。");
    }
}

const Image::DecodedImage& Mip(const TerrainAtlasSnapshot& atlas, std::uint32_t level) {
    return level == 0 ? atlas.image : atlas.additionalMips[level - 1];
}

TerrainMinimapAtlasFacts AtlasFacts(const TerrainAtlasSnapshot& atlas, std::size_t& scratch) {
    TerrainMinimapAtlasFacts facts;
    facts.declaredSource = atlas.source; facts.declaredEncodedSha256 = atlas.sourceSha256;
    facts.declaredEncodedBytes = atlas.sourceBytes; facts.sourceWidth = atlas.sourceWidth; facts.sourceHeight = atlas.sourceHeight;
    facts.columns = atlas.columns; facts.declaredValidatedMipCount = atlas.validatedMipCount; facts.cells = atlas.cells;
    facts.originalMaxMip = static_cast<std::uint32_t>(atlas.additionalMips.size());
    auto cell = atlas.image.height / 4;
    // 奇数 cell 的下一层会破坏逐 cell 整数边界；即使全 atlas 还有 mip 也必须在此止步。
    while (cell > 1 && cell % 2 == 0) { ++facts.cellSafeMaxMip; cell /= 2; }
    facts.usableMaxMip = std::min(facts.originalMaxMip, facts.cellSafeMaxMip);
    facts.mips.reserve(atlas.additionalMips.size() + 1);
    for (std::uint32_t level = 0; level <= facts.originalMaxMip; ++level) {
        const auto& mip = Mip(atlas, level);
        facts.mips.push_back({mip.width, mip.height, mip.rgba.size(), mip.rgba.capacity(), HashBytes(mip.rgba, scratch)});
    }
    return facts;
}

std::uint32_t FindCell(const TerrainGroundMesh& mesh, std::uint32_t first, const TerrainAtlasSnapshot& atlas) {
    const auto& sw = mesh.vertices[first]; const auto& se = mesh.vertices[first + 1];
    const auto& nw = mesh.vertices[first + 2]; const auto& ne = mesh.vertices[first + 3];
    if (sw.u != nw.u || se.u != ne.u || sw.v != se.v || nw.v != ne.v || sw.u > se.u || nw.v > sw.v)
        Fail("TERRAIN_MINIMAP_MESH", "实际 quad UV 不符合可验证的 atlas 矩形。");
    // 仅用实际 mesh UV 验证并查找 cell，不复制变体或 corner-mask 选择规则。
    const auto left = static_cast<double>(sw.u) * atlas.image.width - 0.5;
    const auto right = static_cast<double>(se.u) * atlas.image.width - 0.5;
    const auto top = static_cast<double>(nw.v) * atlas.image.height - 0.5;
    const auto bottom = static_cast<double>(sw.v) * atlas.image.height - 0.5;
    constexpr double tolerance = 0.001;
    for (std::size_t i = 0; i < atlas.cells.size(); ++i) {
        const auto& cell = atlas.cells[i];
        if (std::abs(left - cell.x) < tolerance && std::abs(right - (cell.x + cell.width - 1)) < tolerance
            && std::abs(top - cell.y) < tolerance && std::abs(bottom - (cell.y + cell.height - 1)) < tolerance)
            return static_cast<std::uint32_t>(i);
    }
    Fail("TERRAIN_MINIMAP_MESH", "实际 quad UV 无法唯一对应冻结 atlas 的完整 cell。");
}

double RequestedLod(const TerrainGroundVertex& origin, const TerrainGroundVertex& east,
    const TerrainGroundVertex& north, const TerrainAtlasSnapshot& atlas, double tileStep) {
    const auto footprint = [&](const TerrainGroundVertex& end) {
        return std::hypot((static_cast<double>(end.u) - origin.u) * atlas.image.width,
            (static_cast<double>(end.v) - origin.v) * atlas.image.height) * tileStep;
    };
    return std::log2(std::max(1.0, std::max(footprint(east), footprint(north))));
}

std::vector<TileLayers> IndexLayers(const MapData::TerrainGridSnapshot& grid, const TerrainGroundMesh& mesh,
    TerrainMinimapManifest& manifest) {
    const auto tiles = grid.tiles.size(), quads = mesh.vertices.size() / 4;
    if (mesh.sourceWidth != grid.header.width || mesh.sourceHeight != grid.header.height || mesh.vertices.size() % 4
        || quads != tiles + mesh.overlayTiles.size() || mesh.indices.size() != quads * 6 || mesh.hiddenTileCount
        || !mesh.hiddenTiles.empty() || !mesh.waterTiles.empty() || mesh.waterFlaggedTiles || mesh.waterOmitted
        || mesh.waterTextureIndex || mesh.blightApproximationTiles || mesh.blightTexturedTiles || mesh.lit
        || mesh.fallback.totalTiles || mesh.texturedTiles != tiles || mesh.budget.policy != TerrainGroundBudgetPolicy::Strict
        || mesh.budget.reduced || mesh.budget.base.displayed != tiles || mesh.budget.base.omitted
        || mesh.budget.transitions.displayed != mesh.budget.transitions.requested || mesh.budget.transitions.omitted
        || mesh.budget.blight.requested || mesh.budget.water.requested
        || (!mesh.baseTiles.empty() && mesh.baseTiles.size() != tiles))
        Fail("TERRAIN_MINIMAP_MESH", "候选要求完整、无回退、无隐藏与无可选层省略的 Strict unlit 地表网格。");
    std::vector<TileLayers> indexed(tiles);
    std::vector<std::uint8_t> seen(quads);
    const auto tileStep = static_cast<double>(std::max(grid.header.width - 1, grid.header.height - 1)) / kSize;
    std::size_t next = 0;
    bool blended = false;
    for (const auto& batch : mesh.batches) {
        if (batch.firstIndex != next || !batch.indexCount || batch.indexCount % 6
            || batch.indexCount > mesh.indices.size() - next || batch.textureIndex >= mesh.textures.size()
            || batch.translucent || (!batch.alphaBlend && blended))
            Fail("TERRAIN_MINIMAP_MESH", "实际批次必须完整有序覆盖索引并且只含 base/straight-alpha 地表层。");
        blended = blended || batch.alphaBlend;
        for (std::size_t at = next; at < next + batch.indexCount; at += 6) {
            const auto first = mesh.indices[at];
            if (first % 4 || first >= mesh.vertices.size() || mesh.vertices.size() - first < 4)
                Fail("TERRAIN_MINIMAP_MESH", "quad 首顶点不是四个连续顶点的起点。");
            const std::array<std::uint32_t, 6> pattern{first, first + 1, first + 2, first + 1, first + 3, first + 2};
            if (!std::equal(pattern.begin(), pattern.end(), mesh.indices.begin() + static_cast<std::ptrdiff_t>(at)))
                Fail("TERRAIN_MINIMAP_MESH", "quad 三角必须保持 SW/SE/NW 与 SE/NE/NW。");
            const auto quad = first / 4;
            if (seen[quad]++) Fail("TERRAIN_MINIMAP_MESH", "实际索引重复引用同一 quad。");
            // 索引已按纹理归组，at/6 绝不能作为原 tile。
            const auto tile = quad < tiles ? (mesh.baseTiles.empty() ? quad : mesh.baseTiles[quad])
                : mesh.overlayTiles[quad - tiles];
            if (tile >= tiles || batch.alphaBlend != (quad >= tiles))
                Fail("TERRAIN_MINIMAP_MESH", "quad 的基础/过渡身份或原 tile 映射无效。");
            auto& target = indexed[tile];
            if ((!batch.alphaBlend && target.count) || (batch.alphaBlend && !target.count) || target.count >= 4)
                Fail("TERRAIN_MINIMAP_MESH", "每个 tile 必须先有恰好一个 base 且总层数不超过四层。");
            for (std::size_t corner = 0; corner < 4; ++corner) {
                const auto& vertex = mesh.vertices[first + corner];
                const auto& position = grid.vertices[grid.tiles[tile].vertices[corner]].position;
                if (position != std::array<float, 3>{vertex.x, vertex.y, vertex.z}
                    || vertex.red != 1 || vertex.green != 1 || vertex.blue != 1 || !std::isfinite(vertex.u) || !std::isfinite(vertex.v))
                    Fail("TERRAIN_MINIMAP_MESH", "quad 必须保留原始 XYZ、白色 unlit 顶点与有限 UV。");
            }
            const auto& atlas = *mesh.textures[batch.textureIndex];
            Layer layer{first, batch.textureIndex, FindCell(mesh, first, atlas)};
            const auto& sw = mesh.vertices[first]; const auto& se = mesh.vertices[first + 1];
            const auto& nw = mesh.vertices[first + 2]; const auto& ne = mesh.vertices[first + 3];
            auto& facts = manifest.atlases[batch.textureIndex];
            // 第二个三角在 NE 处取向西/向南导数，长度与屏幕像素足迹同样有定义。
            const std::array<double, 2> requested{RequestedLod(sw, se, nw, atlas, tileStep), RequestedLod(ne, nw, se, atlas, tileStep)};
            for (std::size_t triangle = 0; triangle < 2; ++triangle) {
                facts.maximumRequestedLod = std::max(facts.maximumRequestedLod, requested[triangle]);
                facts.missingOriginalMipsLimitedSampling = facts.missingOriginalMipsLimitedSampling
                    || (requested[triangle] > facts.originalMaxMip && facts.originalMaxMip < facts.cellSafeMaxMip);
                facts.cellBoundaryLimitedSampling = facts.cellBoundaryLimitedSampling || requested[triangle] > facts.cellSafeMaxMip;
                layer.lod[triangle] = std::min(requested[triangle], static_cast<double>(facts.usableMaxMip));
            }
            target.layers[target.count++] = layer;
        }
        next += batch.indexCount;
    }
    if (next != mesh.indices.size() || std::find(seen.begin(), seen.end(), 0) != seen.end()
        || std::any_of(indexed.begin(), indexed.end(), [](const auto& tile) { return !tile.count; }))
        Fail("TERRAIN_MINIMAP_MESH", "实际 draw 未完整、唯一覆盖每个基础 tile 与过渡 quad。");
    manifest.memory.rasterCapacityBytes = indexed.capacity() * sizeof(TileLayers) + seen.capacity();
    return indexed;
}

std::string MeshHash(const TerrainGroundMesh& mesh, std::size_t& scratch) {
    Digest hash(scratch); hash.String("TerrainMinimapMesh-v1"); hash.U32(mesh.sourceWidth); hash.U32(mesh.sourceHeight);
    hash.U64(mesh.vertices.size());
    for (const auto& v : mesh.vertices) {
        for (const auto f : {v.x, v.y, v.z, v.red, v.green, v.blue, v.u, v.v}) hash.F32(f);
    }
    hash.U64(mesh.indices.size()); for (const auto index : mesh.indices) hash.U32(index);
    hash.U64(mesh.batches.size());
    for (const auto& batch : mesh.batches) {
        hash.U32(batch.firstIndex); hash.U32(batch.indexCount); hash.U32(batch.textureIndex);
        hash.U32(batch.alphaBlend ? 1u : 0u); hash.U32(batch.translucent ? 1u : 0u);
    }
    for (const auto* mapping : {&mesh.baseTiles, &mesh.overlayTiles}) {
        hash.U64(mapping->size()); for (const auto tile : *mapping) hash.U32(tile);
    }
    return hash.Finish();
}

std::array<double, 4> Bilinear(const TerrainAtlasSnapshot& atlas, const TerrainAtlasCell& cell,
    std::uint32_t level, double u, double v) {
    const auto& mip = Mip(atlas, level);
    const auto divisor = std::uint32_t{1} << level;
    const auto left = cell.x / divisor, top = cell.y / divisor;
    const auto right = (cell.x + cell.width) / divisor - 1, bottom = (cell.y + cell.height) / divisor - 1;
    const auto x = std::clamp(u * mip.width - 0.5, static_cast<double>(left), static_cast<double>(right));
    const auto y = std::clamp(v * mip.height - 0.5, static_cast<double>(top), static_cast<double>(bottom));
    const auto x0 = static_cast<std::uint32_t>(std::floor(x)), y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = std::min(x0 + 1, right), y1 = std::min(y0 + 1, bottom);
    const auto texel = [&](std::uint32_t px, std::uint32_t py, std::size_t channel) {
        return std::to_integer<unsigned>(mip.rgba[(static_cast<std::size_t>(py) * mip.width + px) * 4 + channel]);
    };
    std::array<double, 4> result{};
    for (std::size_t c = 0; c < 4; ++c)
        result[c] = std::lerp(std::lerp(static_cast<double>(texel(x0, y0, c)), static_cast<double>(texel(x1, y0, c)), x - x0),
            std::lerp(static_cast<double>(texel(x0, y1, c)), static_cast<double>(texel(x1, y1, c)), x - x0), y - y0);
    return result;
}

std::array<double, 4> Sample(const TerrainAtlasSnapshot& atlas, const Layer& layer, double lod, double u, double v) {
    const auto low = static_cast<std::uint32_t>(std::floor(lod)), high = static_cast<std::uint32_t>(std::ceil(lod));
    auto result = Bilinear(atlas, atlas.cells[layer.cell], low, u, v);
    if (low != high) {
        const auto second = Bilinear(atlas, atlas.cells[layer.cell], high, u, v);
        for (std::size_t c = 0; c < 4; ++c) result[c] = std::lerp(result[c], second[c], lod - low);
    }
    return result;
}

void Raster(const MapData::TerrainGridSnapshot& grid, const TerrainGroundMesh& mesh,
    const std::vector<TileLayers>& tiles, TerrainMinimapCandidate& result) {
    const auto width = grid.header.width - 1, height = grid.header.height - 1;
    const auto scale = static_cast<double>(kSize) / std::max(width, height);
    const double fitWidth = width * scale, fitHeight = height * scale;
    const double left = (kSize - fitWidth) / 2, top = (kSize - fitHeight) / 2;
    result.manifest.contentRectangle = {left, top, fitWidth, fitHeight};
    result.image = {kSize, kSize, std::vector<std::byte>(kOutputBytes, std::byte{0})};
    for (std::uint32_t py = 0; py < kSize; ++py) for (std::uint32_t px = 0; px < kSize; ++px) {
        const double centerX = px + 0.5, centerY = py + 0.5;
        auto* output = result.image.rgba.data() + (static_cast<std::size_t>(py) * kSize + px) * 4;
        output[3] = std::byte{255};
        if (centerX < left || centerX >= left + fitWidth || centerY < top || centerY >= top + fitHeight) continue;
        const auto x = (centerX - left) / scale, y = (top + fitHeight - centerY) / scale;
        const auto tx = std::min(static_cast<std::uint32_t>(x), width - 1), ty = std::min(static_cast<std::uint32_t>(y), height - 1);
        const auto fx = std::clamp(x - tx, 0.0, 1.0), fy = std::clamp(y - ty, 0.0, 1.0);
        const auto& tile = tiles[static_cast<std::size_t>(ty) * width + tx];
        const auto triangle = fx + fy <= 1 ? 0u : 1u;
        const std::array<std::uint32_t, 3> corners = triangle == 0
            ? std::array<std::uint32_t, 3>{0, 1, 2} : std::array<std::uint32_t, 3>{1, 3, 2};
        const std::array<double, 3> weights = triangle == 0
            ? std::array<double, 3>{1 - fx - fy, fx, fy} : std::array<double, 3>{1 - fy, fx + fy - 1, 1 - fx};
        std::array<double, 3> color{};
        for (std::uint32_t i = 0; i < tile.count; ++i) {
            const auto& layer = tile.layers[i];
            double u = 0, v = 0;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto& vertex = mesh.vertices[layer.firstVertex + corners[corner]];
                u += weights[corner] * vertex.u; v += weights[corner] * vertex.v;
            }
            const auto sampled = Sample(*mesh.textures[layer.texture], layer, layer.lod[triangle], u, v);
            auto& facts = result.manifest.atlases[layer.texture];
            facts.maximumSampledLod = std::max(facts.maximumSampledLod, layer.lod[triangle]);
            const auto alpha = i == 0 ? 1.0 : std::clamp(sampled[3] / 255.0, 0.0, 1.0);
            for (std::size_t c = 0; c < 3; ++c) color[c] = std::lerp(color[c], std::clamp(sampled[c], 0.0, 255.0), alpha);
        }
        for (std::size_t c = 0; c < 3; ++c) output[c] = static_cast<std::byte>(std::lround(std::clamp(color[c], 0.0, 255.0)));
    }
}

std::size_t OutputCapacity(const TerrainMinimapCandidate& result) {
    const auto& m = result.manifest;
    std::size_t bytes = sizeof(result); Charge(bytes, result.image.rgba.capacity());
    Charge(bytes, result.encoded.bytes.capacity()); ChargeBinding(bytes, m.binding);
    for (const auto* text : {&m.currentW3eSha256, &m.reserializedW3eSha256, &m.meshSha256,
        &m.outputRgbaSha256, &m.outputBlpSha256, &m.manifestSha256})
        ChargeText(bytes, *text);
    Charge(bytes, m.atlases.capacity(), sizeof(TerrainMinimapAtlasFacts));
    for (const auto& a : m.atlases) {
        ChargeLocation(bytes, a.declaredSource); ChargeText(bytes, a.declaredEncodedSha256);
        Charge(bytes, a.cells.capacity(), sizeof(TerrainAtlasCell)); Charge(bytes, a.mips.capacity(), sizeof(TerrainMinimapMipFacts));
        for (const auto& mip : a.mips) ChargeText(bytes, mip.decodedRgbaSha256);
    }
    return bytes;
}

std::string ManifestHash(TerrainMinimapManifest& m) {
    Digest hash(m.memory.maximumObservedHashScratchBytes);
    hash.String("TerrainMinimapManifest-v2"); hash.String(m.samplingPolicy); hash.String(m.encodingPolicy); HashBinding(hash, m.binding);
    hash.String(m.currentW3eSha256); hash.String(m.reserializedW3eSha256);
    for (const auto value : m.worldExtent) hash.F64(value);
    for (const auto value : m.contentRectangle) hash.F64(value);
    hash.U32(kSize); hash.U32(kSize); hash.U64(kOutputBytes);
    hash.U64(m.sourceTiles); hash.U64(m.meshQuads); hash.U64(m.meshBatches); hash.String(m.meshSha256); hash.String(m.outputRgbaSha256);
    hash.String(m.outputBlpSha256); hash.U32(static_cast<std::uint32_t>(m.paletteMethod)); hash.U32(m.paletteQuantized ? 1u : 0u);
    hash.U32(m.paletteColorCount); hash.U32(m.occupiedHistogramBins); hash.U32(m.encodedMipCount);
    hash.U64(m.encodedTotalMipPixels); hash.U64(m.encodedBlpBytes);
    hash.U64(m.atlases.size());
    for (const auto& a : m.atlases) {
        HashLocation(hash, a.declaredSource); hash.String(a.declaredEncodedSha256); hash.U64(a.declaredEncodedBytes);
        hash.U32(a.sourceWidth); hash.U32(a.sourceHeight); hash.U32(a.columns); hash.U32(a.rows); hash.U32(a.declaredValidatedMipCount);
        hash.U64(a.mips.size());
        for (const auto& mip : a.mips) {
            hash.U32(mip.width); hash.U32(mip.height); hash.U64(mip.rgbaBytes); hash.U64(mip.rgbaCapacityBytes); hash.String(mip.decodedRgbaSha256);
        }
        hash.U64(a.cells.size());
        for (const auto& cell : a.cells) {
            hash.U32(cell.column); hash.U32(cell.row); hash.U32(cell.x); hash.U32(cell.y); hash.U32(cell.width); hash.U32(cell.height);
        }
        hash.U32(a.originalMaxMip); hash.U32(a.cellSafeMaxMip); hash.U32(a.usableMaxMip);
        hash.F64(a.maximumRequestedLod); hash.F64(a.maximumSampledLod);
        hash.U32(a.missingOriginalMipsLimitedSampling ? 1u : 0u); hash.U32(a.cellBoundaryLimitedSampling ? 1u : 0u);
    }
    const auto& memory = m.memory;
    for (const auto bytes : {memory.retainedInputCapacityBytes, memory.estimatedExtraPeakBytes, memory.estimatedOutputBytes,
        memory.estimatedPeakBytes, memory.parsedTerrainCapacityBytes, memory.reserializedW3eCapacityBytes, memory.meshCapacityBytes,
        memory.rasterCapacityBytes, memory.outputCapacityBytes, memory.observedContainerCapacityBytes, memory.maximumObservedHashScratchBytes}) hash.U64(bytes);
    for (const auto fact : {m.archiveSourceIndependentlyVerified, m.encodedAtlasSourcesIndependentlyVerified,
        m.resourceWinnersIndependentlyVerified, m.nativeEditorOrGameVerified, m.formalSaveAuthorized}) hash.U32(fact ? 1u : 0u);
    return hash.Finish();
}

} // namespace

Result<TerrainMinimapCandidate> BuildTerrainMinimapCandidate(const TerrainMinimapInput& input, const TerrainMinimapLimits& limits) {
    try {
        TerrainMinimapCandidate result;
        auto& manifest = result.manifest; auto& memory = manifest.memory;
        memory.retainedInputCapacityBytes = ValidateInputs(input, limits);
        if (input.currentW3e.size() > limits.maxW3eBytes)
            Fail("TERRAIN_MINIMAP_BUDGET", "当前 W3E 超过候选读取预算。");
        memory.estimatedOutputBytes = sizeof(TerrainMinimapCandidate) + kOutputBytes + kEncodedOutputReserve + kManifestReserve;
        std::size_t beforeParse = memory.retainedInputCapacityBytes;
        Charge(beforeParse, memory.estimatedOutputBytes); Charge(beforeParse, kHashReserve);
        Charge(beforeParse, kEncodingWorkReserve);
        Charge(beforeParse, input.currentW3e.size()); // 后续 Serialize 同时保活
        if (beforeParse >= limits.maxWorkingBytes)
            Fail("TERRAIN_MINIMAP_BUDGET", "冻结输入、输出与解析预留已经超过工作预算。");
        manifest.binding = input.binding;
        manifest.currentW3eSha256 = HashBytes(input.currentW3e, memory.maximumObservedHashScratchBytes);
        if (manifest.currentW3eSha256 != input.binding.terrainSha256)
            Fail("TERRAIN_MINIMAP_IDENTITY", "实际当前 W3E 字节与冻结修订身份不一致。");
        MapData::TerrainReadLimits read;
        read.maxBytes = limits.maxW3eBytes;
        // 非空矩形 tile 数 T 的顶点数至多 2T+2（最薄的 1×T 情形）。
        read.maxVertices = std::min(read.maxVertices, 2 * limits.maxTiles + 2); read.maxTiles = limits.maxTiles;
        read.maxDecodedBytes = std::min(read.maxDecodedBytes, limits.maxWorkingBytes - beforeParse);
        const auto terrain = Take(MapData::ParseMapTerrain(input.currentW3e, read));
        const auto grid = terrain.Snapshot();
        if (grid->header.groundTilesets.size() != input.groundAtlases.size())
            Fail("TERRAIN_MINIMAP_ATLAS", "冻结 atlas 数量与当前 W3E 地面表不同。");
        CheckDomain(*grid);
        const Counts counts{grid->vertices.size(), grid->tiles.size(), grid->header.groundTilesets.size() + grid->header.cliffTilesets.size()};
        Estimate(input, counts, memory, limits);
        const auto serialized = Take(MapData::SerializeMapTerrain(*grid));
        manifest.reserializedW3eSha256 = HashBytes(serialized, memory.maximumObservedHashScratchBytes);
        if (serialized != input.currentW3e || manifest.reserializedW3eSha256 != manifest.currentW3eSha256)
            Fail("TERRAIN_MINIMAP_IDENTITY", "内部重新序列化不能逐字节保持当前 W3E 身份。");
        memory.parsedTerrainCapacityBytes = TerrainCapacity(terrain); memory.reserializedW3eCapacityBytes = serialized.capacity();
        std::size_t estimatedParsed = sizeof(MapData::MapTerrain) + sizeof(MapData::TerrainGridSnapshot);
        Charge(estimatedParsed, input.currentW3e.size(), 2);
        Charge(estimatedParsed, counts.vertices, sizeof(MapData::TerrainVertex));
        Charge(estimatedParsed, counts.tiles, sizeof(MapData::TerrainTile));
        Charge(estimatedParsed, counts.palette, sizeof(std::string) + 64);
        std::size_t actualParsed = memory.parsedTerrainCapacityBytes;
        Charge(actualParsed, memory.reserializedW3eCapacityBytes);
        if (actualParsed > estimatedParsed) {
            // 即使某个 STL 的 reserve 给出更大容量，也在完整 mesh 分配前补足估算或拒绝。
            Charge(memory.estimatedExtraPeakBytes, actualParsed - estimatedParsed);
            Charge(memory.estimatedPeakBytes, actualParsed - estimatedParsed);
            if (memory.estimatedPeakBytes > limits.maxWorkingBytes)
                Fail("TERRAIN_MINIMAP_BUDGET", "解析/重序列化实际 capacity 使完整 mesh 前预算超限。");
        }
        const auto mesh = Take(BuildTerrainGroundMesh(*grid, input.groundAtlases, {}, TerrainLighting{false}, nullptr, {}, {}, {}, TerrainGroundBudgetPolicy::Strict));
        memory.meshCapacityBytes = MeshCapacity(mesh);
        manifest.atlases.reserve(mesh.textures.size());
        for (const auto& atlas : mesh.textures) manifest.atlases.push_back(AtlasFacts(*atlas, memory.maximumObservedHashScratchBytes));
        const auto layers = IndexLayers(*grid, mesh, manifest);
        manifest.sourceTiles = grid->tiles.size(); manifest.meshQuads = mesh.vertices.size() / 4; manifest.meshBatches = mesh.batches.size();
        manifest.worldExtent = {grid->header.offset[0], grid->header.offset[1],
            static_cast<double>(grid->header.offset[0]) + (grid->header.width - 1) * 128.0,
            static_cast<double>(grid->header.offset[1]) + (grid->header.height - 1) * 128.0};
        manifest.meshSha256 = MeshHash(mesh, memory.maximumObservedHashScratchBytes);
        Raster(*grid, mesh, layers, result);
        manifest.outputRgbaSha256 = HashBytes(result.image.rgba, memory.maximumObservedHashScratchBytes);
        // mesh/解析输入仍在保活，因此在 Parse 和 mesh 前已经预留完整编码工作集。
        // 编码器的工作上限和输出容量独立于旧无损入口，不能把有损结果称作无损。
        result.encoded = Take(Image::EncodeBlpOpaqueQuantized(result.image));
        manifest.outputBlpSha256 = HashBytes(result.encoded.bytes, memory.maximumObservedHashScratchBytes);
        manifest.paletteMethod = result.encoded.method; manifest.paletteQuantized = result.encoded.quantized;
        manifest.paletteColorCount = result.encoded.paletteColorCount; manifest.occupiedHistogramBins = result.encoded.occupiedHistogramBins;
        manifest.encodedMipCount = result.encoded.mipCount; manifest.encodedTotalMipPixels = result.encoded.totalMipPixels;
        manifest.encodedBlpBytes = result.encoded.bytes.size();
        // 为最后摘要预留其最终 64 字符容量，内存账和 manifest hash 无自引用。
        manifest.manifestSha256.reserve(64);
        memory.outputCapacityBytes = OutputCapacity(result);
        memory.observedContainerCapacityBytes = memory.retainedInputCapacityBytes;
        for (const auto bytes : {memory.parsedTerrainCapacityBytes, memory.reserializedW3eCapacityBytes, memory.meshCapacityBytes,
            memory.rasterCapacityBytes, memory.outputCapacityBytes}) Charge(memory.observedContainerCapacityBytes, bytes);
        if (memory.outputCapacityBytes > memory.estimatedOutputBytes || memory.observedContainerCapacityBytes > limits.maxWorkingBytes
            || memory.observedContainerCapacityBytes > memory.estimatedPeakBytes)
            Fail("TERRAIN_MINIMAP_BUDGET", "实际可观察 capacity 超过构建前估算或候选硬预算。");
        const auto identity = ManifestHash(manifest);
        manifest.manifestSha256.assign(identity.data(), identity.size()); // 保持上面已计费的预留容量
        return Result<TerrainMinimapCandidate>::Success(std::move(result));
    } catch (Failure& failure) {
        Result<TerrainMinimapCandidate> result; result.diagnostics = std::move(failure.diagnostics); return result;
    } catch (const std::bad_alloc&) {
        return Result<TerrainMinimapCandidate>::Failure(kSubsystem, "TERRAIN_MINIMAP_ALLOCATION", "候选内存分配失败，不返回部分像素。");
    } catch (const std::length_error&) {
        return Result<TerrainMinimapCandidate>::Failure(kSubsystem, "TERRAIN_MINIMAP_ALLOCATION", "候选容器容量异常，不返回部分像素。");
    }
}

} // namespace War3Map::Render
