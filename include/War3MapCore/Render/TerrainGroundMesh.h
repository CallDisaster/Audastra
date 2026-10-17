#pragma once

#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"

#include <array>
#include <limits>
#include <optional>
#include <string>

namespace War3Map::Render {

inline constexpr std::uint32_t TerrainGroundHeightColor = std::numeric_limits<std::uint32_t>::max();

struct TerrainGroundMeshLimits {
    // 默认值也是硬上限，只能收紧；基础层超预算始终失败，不把粗格拉伸贴图。
    // 含过渡层：256×256 原格基础层 262144 顶点，另留过渡层余量。
    std::size_t maxVertices = 1048576;
    std::size_t maxIndices = 6291456;
    std::size_t maxTextures = 16;
    std::size_t maxTextureDimension = 2048;
    std::size_t maxTextureBytes = 16 * 1024 * 1024;
    std::size_t maxTextureChainBytes = 24 * 1024 * 1024;
    std::size_t maxRgbaBytes = 64 * 1024 * 1024;
    std::size_t maxBatches = 4096;
    // 输出载荷、保活 RGBA、块元数据和本函数 scratch；不含借入地形及分配器管理开销。
    std::size_t maxWorkingBytes = 128 * 1024 * 1024;
};

// 地表逐顶点光照（与原版地形相同的顶点光照：高度场法线 · 方向光 + 环境光）。
// 平地亮度为 flatShade；朝光坡面更亮（上限 1，渲染 ABI 的颜色范围）、背光坡面更暗，
// 悬崖回退格同样按坡度明暗。
// 顶点颜色 = ambient + diffuse × max(法线·光向, 0)，逐分量夹到 [0,1]。原版昼夜光照由
// SampleDayNight 给出；默认值为无 DNC 数据时的中性灰光（平地 0.9，背光面 0.35）。
struct TerrainLighting {
    bool enabled = true;
    std::array<float, 3> towardLight{-0.45f, 0.35f, 0.82f}; // 指向光源，不必归一化，z 须 > 0
    std::array<float, 3> ambient{0.35f, 0.35f, 0.35f};
    std::array<float, 3> diffuse{0.6698592f, 0.6698592f, 0.6698592f};
};

// 水面样式：取自 Water.slk 中当前 tileset 的一行（waterID = 主 tileset 字母 + "Sha"）。
struct TerrainWaterStyle {
    float heightOffset = 0; // height × 128，与 W3E waterBaseHeight 相加
    // RGBA 0..255：浅水/深水在各自深度区间两端的颜色与不透明度。
    std::array<std::uint8_t, 4> shallowMinimum{}, shallowMaximum{}, deepMinimum{}, deepMaximum{};
    // 明确选中的 Water.slk 帧；初始帧为 0，空纹理沿用历史白色回退并附带宿主诊断。
    std::shared_ptr<const Image::DecodedImage> texture;
    std::uint32_t frameIndex = 0, frameCount = 1;
    std::string frameSourcePath, frameSourceSha256;
};

struct TerrainGroundVertex {
    float x, y, z, red, green, blue, u, v;
};

struct TerrainGroundBatch {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t textureIndex = TerrainGroundHeightColor;
    bool alphaBlend = false; // 过渡层：按贴图 alpha 叠在之前的批次上
    bool translucent = false; // 水面：半透明，渲染器在不透明模型之后绘制、不写深度
};

struct TerrainGroundBlock {
    std::uint32_t tileX = 0, tileY = 0, tileWidth = 0, tileHeight = 0;
    std::array<float, 3> minimum{}, maximum{};
};

struct TerrainGroundFallbackCounts {
    std::size_t totalTiles = 0;
    // 各原因可重叠，不能相加代替 totalTiles。荒芜标记本身不再触发高度色回退。
    std::size_t blightTiles = 0; // 原格至少一角有荒芜标记；也包含因其它原因回退的格
    std::size_t mixedCliffLayerTiles = 0;
    std::size_t neighboringCliffTiles = 0;
    std::size_t invalidGroundIndexTiles = 0;
    std::size_t missingAtlasTiles = 0;
    std::size_t invalidAtlasTiles = 0;
};

enum class TerrainGroundBudgetPolicy {
    Strict,
    // 仅供世界预览宿主显式选择；完整保留基础层，按整类限制可选层，不表示完整原版保真。
    PreserveBasePreview
};

// 仅记录本次实际准入检查的失败域；未请求或成功显示的层没有省略原因。
// 资源回退与几何预览降级不同，不能仅凭reduced判断场景完整。
enum class TerrainGroundOmissionReason : std::uint32_t {
    MissingWaterStyle = 1u << 0,
    MissingBlightAtlas = 1u << 1,
    InvalidBlightAtlas = 1u << 2,
    TextureSlots = 1u << 3,
    TextureDimension = 1u << 4,
    TextureBytes = 1u << 5,
    TextureChainBytes = 1u << 6,
    RgbaBytes = 1u << 7,
    Vertices = 1u << 8,
    Indices = 1u << 9,
    Batches = 1u << 10,
    WorkingBytes = 1u << 11
};

struct TerrainGroundLayerCounts {
    std::size_t requested = 0, displayed = 0, omitted = 0;
    // 已证明全部可采样 texel 的 alpha 为零，不占绘制/资源预算；不属于质量省略。
    // requested = noContribution + displayed + omitted；目前仅合成单 mip 水面使用。
    std::size_t noContribution = 0;
    std::uint32_t omissionReasons = 0;
    // 水面/荒芜有请求时检查其资源前置条件；缺样式也属于已检查，不表示已分配纹理。
    bool resourceAdmissionChecked = false;
    // 最终实际准入尝试前的槽数；已省过渡后有界重试水面时不再沿用旧表槽数。
    std::size_t textureSlotsBeforeAdmission = 0;
};

struct TerrainGroundBudgetStats {
    TerrainGroundBudgetPolicy policy = TerrainGroundBudgetPolicy::Strict;
    // base 只统计未被悬崖模型覆盖的原格 quad；已覆盖格另见 hiddenTileCount，不占地表顶点预算。
    // transitions / blight 为可贴图格请求的遮罩 quad；water 为全部有水标记的格（含缺样式）。
    TerrainGroundLayerCounts base, transitions, blight, water;
    bool reduced = false; // 显式预览因几何/批次/整体载荷预算省略可选层；资源回退另行诊断。
    // A：全部请求base/transition依赖的原资源预检数，省略过渡也不能绕过其硬预算。
    std::size_t requestedGroundTextureSlots = 0;
    // B：最终保留ground槽及含水/荒芜的全部槽；Strict的requested/ground相同。
    std::size_t groundTextureSlots = 0, finalTextureSlots = 0, textureSlotLimit = 0;
    bool waterStyleAvailable = false, waterSourceTextureAvailable = false;
    // 这里只记录构建器的atlas布局/字节检查；原BLP来源及mask alpha由既有loader检查。
    bool blightAtlasAvailable = false, blightAtlasValidationPerformed = false, blightAtlasValid = false;
};

struct TerrainGroundMesh {
    std::uint32_t sourceWidth = 0, sourceHeight = 0;
    static constexpr std::uint32_t blockTileSize = 16;
    // 前 (原 tile 数 - hiddenTileCount)×4 个顶点为可见基础层，固定 SW/SE/NW/NE。
    // 无隐藏格时原 tile = vertexIndex / 4；有隐藏格时由 baseTiles 显式映射。
    // 其后每个过渡层 quad 也是四个顶点，所属原 tile 见 overlayTiles。
    // 只复制原始 XYZ，不调整坡道、悬崖或水面高度，不降采样。
    std::vector<TerrainGroundVertex> vertices;
    // 基础层按纹理稳定归组，地表过渡、荒芜和水面批次依次在后；每个 quad 为 (SW,SE,NW)、(SE,NE,NW)。
    std::vector<std::uint32_t> indices;
    std::vector<TerrainGroundBatch> batches;
    // textureIndex 索引此列表；相同 shared_ptr 只占一个槽。
    // 预览在原完整资源预检后，仅当原流程已整类省略过渡时回收无draw依赖的地表槽并统一remap。
    std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> textures;
    // 按块 row-major，末块可不足 16 格；本批 GPU 仍按纹理归组，没有宣称块剔除。
    std::vector<TerrainGroundBlock> blocks;
    // 仅有隐藏格时存储：按原 tile 升序，恰好为 hiddenTiles 的补集；不能省略、重复或重排。
    std::vector<std::uint32_t> baseTiles;
    // 第 i 个地表过渡或荒芜 quad（位于全部可见基础层顶点之后）所属的原 tile。
    std::vector<std::uint32_t> overlayTiles;
    // 水面 quad 排在过渡层之后，第 i 个所属的原 tile；XY 与原格相同，Z 为水面高度。
    std::vector<std::uint32_t> waterTiles;
    // 被已准备悬崖模型覆盖的原 tile（与 tile 同序，空表示无隐藏）：不分配地表顶点或索引。
    std::vector<std::uint8_t> hiddenTiles;
    std::size_t hiddenTileCount = 0;
    bool waterOmitted = false; // 缺样式、纹理槽不足或预览预算造成质量省略；不含已证明零贡献的格
    std::optional<std::uint32_t> waterTextureIndex; // 合成水面透明度 atlas 的纹理槽，供来源审计区分原始图集
    std::uint32_t waterFrameIndex = 0, waterFrameCount = 0; // 本网格实际选取的 Water.slk 帧身份
    std::string waterFrameSourcePath, waterFrameSourceSha256;
    std::size_t texturedTiles = 0;
    // 使用已验证 8x4 布局的荒芜 atlas 覆盖的格数；不含高度色/悬崖回退格。
    std::size_t blightTexturedTiles = 0;
    // 原版荒芜 atlas 不可用或纹理预算不足时，以原地表贴图轻度逐角色调表示的格数。
    std::size_t blightApproximationTiles = 0;
    std::size_t waterFlaggedTiles = 0; // 全部有水标记的原格，含零贡献与质量省略，保留源请求身份。
    std::size_t mixedGroundTextureTiles = 0; // 四角材质不同的格子（贴图格以过渡层混合）
    bool lit = false; // 顶点颜色已乘光照明暗
    TerrainGroundFallbackCounts fallback;
    TerrainGroundBudgetStats budget;
};

// groundAtlases 必须与 header.groundTilesets 同序同长度；null 明确表示资源不可用。
// 基础层取四角最小 ground 索引及 SW variation；其余每个更大的索引按所在角取 atlas 遮罩格
// （东南 1、西南 2、东北 4、西北 8），按索引升序作为 alpha 过渡层叠加。
// 荒芜 atlas 非空且为有效 8x4 时，按四角荒芜位在普通地表层之后绘制透明遮罩；全荒芜
// 格取右侧基础变体。atlas 不可用或纹理槽不足时仍保留原地表贴图，仅对荒芜角做轻度色调近似，
// 并分别记录 blightTexturedTiles / blightApproximationTiles。异 cliff layer、其八邻域、无效
// ground 索引、四角任一缺失或无效 atlas 仍逐 tile 保留高度色。
// 提供水面样式时，四角任一有水的格子追加一个半透明水面 quad（颜色按各角深度，透明度取四角平均，
// 量化为合成 4×4 atlas 的 16 级；每格是明确选择的水面帧乘该级透明度，不推测播放时钟）。
// 合成 atlas 仅 mip0；有基础层且选中 cell 的全部 texel alpha 为零时不发 quad，计入 noContribution。
// 全部基础格被覆盖时保留原水 draw，避免把只有水面的场景变成另有语义的空地形提交。
// Strict 保持预算/结构错误无部分结果；PreserveBasePreview 仅在相同硬上限内按整类尝试
// 水面 → 普通过渡 → 精确荒芜遮罩，装不下的一类全部省略，再尝试下一类；不截 tile 前缀。
// 基础层的每格 XYZ/UV、原有资源/结构校验与逐格高度色回退不变，省略数由 budget 精确报告。
// 保持原成功的过渡/水面决策；已省略过渡留下未用atlas时，回收其槽并只重试原资源拒绝的水面。
// 荒芜遮罩省略时沿用原底色近似。基础层及所需地表资源自身超预算仍失败。
// UV 左上原点，以实际尺寸半像素内缩；纹理 RGB 不做高度染色，基础层按不透明采样。
// 顶点颜色：贴图格为灰度明暗（光照关闭时为 1），高度色格为高度色 × 明暗；渲染器以颜色乘贴图。
// 法线取原格高度的中心差分（边缘单侧），与拼接后的显示高度一致，不修改 XYZ。
// coveredTiles（可空，与 tile 同序）标记已由悬崖模型覆盖的格：这些格不画地面、不参与回退计数，
// 其邻格也不再因“邻近悬崖”回退高度色；未覆盖的悬崖格（坡道、缺模型）仍按高度色显示。
// 纯 CPU，只读，不执行资源 I/O、GPU、地图编辑或撤销；宿主负责 whole-map 高度回退。
// effectiveGroundIndices 可空（沿用 raw）；非空必须与 vertices 同序，新派生索引必须合法。
// 未改变的未知 raw 值保持逐 tile 高度色回退；全部基础/过渡
// 材质读取共用它，不修改原始顶点或 palette。调用方负责派生语义与覆盖诊断。
[[nodiscard]] Result<TerrainGroundMesh> BuildTerrainGroundMesh(
    const MapData::TerrainGridSnapshot& terrain,
    std::span<const std::shared_ptr<const TerrainAtlasSnapshot>> groundAtlases,
    const TerrainGroundMeshLimits& limits = {}, const TerrainLighting& lighting = {},
    const TerrainWaterStyle* water = nullptr, std::span<const std::uint8_t> coveredTiles = {},
    std::shared_ptr<const TerrainAtlasSnapshot> blightAtlas = {},
    std::span<const std::uint8_t> effectiveGroundIndices = {},
    TerrainGroundBudgetPolicy budgetPolicy = TerrainGroundBudgetPolicy::Strict);

// 水面在给定深度（水面高度 − 地面高度，世界单位）处的 RGBA（0..1）。
// 深度 ≤ 64 在浅水两色间插值（10 以下取最小值），64–72 在深水两色间插值，再深取深水最大值。
[[nodiscard]] std::array<float, 4> TerrainWaterColor(const TerrainWaterStyle& style, float depth);

// 高度场在原格顶点 (x, y) 处的光照颜色（供网格与测试共用）；光照关闭时为白色。
[[nodiscard]] std::array<float, 3> TerrainVertexShade(const MapData::TerrainGridSnapshot& terrain,
    std::uint32_t x, std::uint32_t y, const TerrainLighting& lighting);

} // namespace War3Map::Render
