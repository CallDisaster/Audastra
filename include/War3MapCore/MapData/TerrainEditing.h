#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// 地形编辑：W3E v11 写出、按原始字段重算派生值、笔刷（纯函数，界面与 Agent 共用）。
// 撤销与保存由 MapDataEditSession 负责；这里只修改传入的地形快照。
namespace War3Map::MapData {

// 原样写出头与每顶点 7 字节原始字段；位置与水基础高度由原始字段推导，不写回。
[[nodiscard]] Result<std::vector<std::byte>> SerializeMapTerrain(const TerrainGridSnapshot& terrain);

// 由原始字段重算 [x0,x1)×[y0,y1) 顶点的位置与水基础高度，再重算包围盒与未解析纹理计数。
void RefreshTerrainDerived(TerrainGridSnapshot& terrain, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1);

enum class TerrainHeightSource { StoredPositions, RawWithRamps };

// 地面高度（与渲染拾取同一三角剖分）；越界或非有限输入为空。
// 显示副本已经抬升坡道，默认直接采样 position；编辑会话应显式选择 RawWithRamps。
// native结构编辑须传已捕获的最大层级，使用完整R/S偏移且遇未知邻域返回空。
// 无规则的兼容调用仍只识别既有直坡中点，不授予复杂坡道编辑/保存资格。
[[nodiscard]] std::optional<float> SampleGroundHeight(const TerrainGridSnapshot& terrain, double x, double y,
    TerrainHeightSource source = TerrainHeightSource::StoredPositions,
    std::optional<std::uint8_t> maximumCliffLevel = {});

enum class TerrainBrushKind {
    Texture,      // 地面贴图：设定贴图并按静态地图统计权重随机变体
    Blight,       // 荒芜：置位或清除（value 为 0 时清除）
    Raise,        // 升高：每个笔刷点按余弦衰减加 strength 世界单位
    Lower,        // 降低
    Plateau,      // 高原：把地面原始高度设为第一个笔刷点处的值
    Smooth,       // 平滑：与 3×3 邻域平均高度按 strength（0..1）混合
    CliffLevel,   // 悬崖：把层级设为第一个笔刷点处层级 + levelDelta（-1/0/+1），相邻层级差限制在 2 以内
    Noise,        // 噪波：一笔内每个顶点只施加一次确定性有界高度扰动
    CliffTexture, // 仅替换悬崖类型，保留各顶点层级与地高
    WaterHeight,  // 仅调整已有水面的基础高度，不生成浅水/深水或修改通行性
    Ramp,         // 一级干地直坡：整段三点创建/移除，value 决定是否创建，不使用尺寸或形状
    Boundary,     // 边界标志：仅修改 W3E 0x80；保留外缘与 WPM，原生保存仍须通过派生数据门
    PathingRestriction, // WPM 附加限制草稿：仅由 MapDataEditSession 应用，不改变 W3E
    CornerRamp,   // 一级干地九点外角；单点canonical输入，须走实际规则的独立结构事务。
    RemoveWater,  // 移除水面：仅清 HasWater，保留地高、水原始高度与其它原始字段。
};

enum class TerrainBrushShape { Circle, Square };

// 在拖动开始时由原始地形快照采样；预览与提交复用同一锚点。
// 未提供时，兼容路径仍从笔划中第一个落图点采样。
struct TerrainStrokeAnchor {
    std::uint16_t groundHeightRaw = 0;
    std::uint8_t cliffLevel = 2;
};

struct TerrainBrush {
    TerrainBrushKind kind = TerrainBrushKind::Texture;
    TerrainBrushShape shape = TerrainBrushShape::Circle;
    // 以顶点计的半径：0 只影响最近的一个顶点，最大 16。
    std::uint32_t radius = 1;
    std::string groundTexture;       // Texture：地面贴图 rawcode（不在表中时追加，表最多 16 项）
    std::uint8_t cliffTexture = 0;   // CliffLevel：悬崖贴图表索引
    int levelDelta = 0;              // CliffLevel：-1、0、+1
    bool value = true;               // Blight/Ramp/Boundary：置位或清除
    float strength = 32.0f;          // Raise/Lower：世界单位；Smooth：0..1
    std::uint32_t seed = 0;          // Texture 变体随机种子，同一输入得到同一结果
    std::optional<TerrainStrokeAnchor> anchor; // Plateau/CliffLevel：拖动起点的固定基准
    // 指定时优先于 radius：经典编辑器尺寸 1..8 对应顶点半径 size-1。
    // 圆形严格裁剪到该半径；方形每边覆盖 2*size-1 个顶点。
    std::optional<std::uint32_t> size;
    std::optional<std::uint8_t> variation; // Texture：0..17；未指定时使用坐标种子
    bool randomizeVariation = false;     // Texture：同贴图也重选变体；默认保留旧协议行为
    float waterHeight = 0;               // WaterHeight：W3E 基础世界高度，范围 -2048..2047.75
    std::uint8_t pathingRestrictions = 0x02; // PathingRestriction：0x02/0x04/0x08 的非空组合
};

struct TerrainStrokePoint { double x = 0, y = 0; };

// 地面单位能否站在该点所在的格：越界、悬崖面（四角层级不同且不全是坡道角）或边界格不可站立。
// 只看地形本身，不读 WPM 与可破坏物；深水规则未取证，不在此判定。
[[nodiscard]] bool TerrainGroundStandable(const TerrainGridSnapshot& terrain, double x, double y);
// 从该点所在格向外逐圈搜索，返回距离最近的可站立格中心；maxTiles 圈内没有时为空。
[[nodiscard]] std::optional<TerrainStrokePoint> NearestTerrainGroundStandable(const TerrainGridSnapshot& terrain,
    double x, double y, std::uint32_t maxTiles = 16);

struct TerrainBrushFootprint {
    double centerX = 0, centerY = 0, radius = 0; // 世界坐标；radius 包含外侧半格
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // 已裁剪的顶点半开范围
};

// 界面范围预览和 Core 笔刷共用吸附/尺寸规则；无效输入或地图外落点返回空。
[[nodiscard]] std::optional<TerrainBrushFootprint> GetTerrainBrushFootprint(
    const TerrainGridSnapshot& terrain, const TerrainBrush& brush, const TerrainStrokePoint& point);

// 从笔划起点取原始高度和悬崖层级；越界、非有限坐标或无效快照明确报错。
[[nodiscard]] Result<TerrainStrokeAnchor> CaptureTerrainStrokeAnchor(
    const TerrainGridSnapshot& terrain, const TerrainStrokePoint& point);

struct TerrainStrokeResult {
    // 实际改动的顶点范围 [x0,x1)×[y0,y1)；没有改动时为空范围。
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool headerChanged = false;
    std::size_t levelPropagationWork = 0; // 本笔累计的层级传播队列/邻域及逐采样刷新访问。
    [[nodiscard]] bool Empty() const noexcept { return x0 >= x1 || y0 >= y1; }
};

inline constexpr std::size_t TerrainLevelPropagationMaximumWork = 16u * 1024u * 1024u;
struct OwnedTerrainStroke {
    TerrainGridSnapshot terrain;
    TerrainStrokeResult result;
};
// 先在独立副本中落笔；传播预算只能收紧硬上限，失败不返回已修改前缀。
[[nodiscard]] Result<OwnedTerrainStroke> PrepareTerrainStroke(
    const TerrainGridSnapshot& terrain, const TerrainBrush& brush, std::span<const TerrainStrokePoint> points,
    std::size_t maximumLevelPropagationWork = TerrainLevelPropagationMaximumWork);

// 依次在每个点上施加笔刷，直接修改 terrain（含派生值）。输入非法时不修改并返回错误。
// 地图边界顶点只接受贴图、荒芜、边界标志与移除水面，不改高度与悬崖。
[[nodiscard]] Result<TerrainStrokeResult> ApplyTerrainStroke(
    TerrainGridSnapshot& terrain, const TerrainBrush& brush, std::span<const TerrainStrokePoint> points);

enum class TerrainTextureTable { Ground, Cliff };

// 替换既有槽位或在尾部追加一个槽位；原始顶点索引不改。
// 地面最多 16 项，悬崖最多 15 项（索引 15 为哨兵）；拒绝重复 rawcode。
[[nodiscard]] Result<TerrainStrokeResult> ReplaceTerrainTextureSlot(TerrainGridSnapshot& terrain,
    TerrainTextureTable table, std::uint32_t slot, const std::string& rawcode);

} // namespace War3Map::MapData
