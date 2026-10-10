#pragma once
#include "War3MapCore/Types.h"
#include <array>
#include <functional>
#include <span>

namespace War3Map::Render::Particles {
using Vec3 = std::array<float, 3>;
using Matrix = std::array<float, 12>; // 行主序模型到世界 3x4；不沿用骨骼组矩阵的列主序布局。
inline constexpr Matrix Identity{1,0,0,0,0,1,0,0,0,0,1,0};
inline constexpr std::uint32_t FixedStepMs = 10;
inline constexpr std::uint64_t MaxClockMs = std::uint64_t{1} << 53; // 与模型动画时间的精确整数范围一致。
enum class Kind : std::uint32_t { Particle2, Ribbon };
enum class Filter : std::uint32_t { Blend, Additive, Modulate, Modulate2x, AlphaKey, AddAlpha, Opaque };
enum class Parameter : std::uint32_t { Rate, Speed, Variation, Latitude, Gravity, Length, Width, Visibility,
    Above, Below, Alpha, Color, TextureSlot, Count };
struct Key { std::int32_t frame = 0; Vec3 value{}, in{}, out{}; };
struct KeyRange { std::uint32_t first = 0, count = 0; bool ascending = true; };
struct Track {
    std::uint32_t interpolation = 0, globalSequence = UINT32_MAX, width = 1;
    std::vector<Key> keys; // 源文件顺序，不排序。
    // 非全局轨迹按 1.27a 原版逐动作区间取键（SEQS 文件顺序共享游标，见 War3RenderCore/GameKeyRanges.h）。
    // Read 在有 SEQS 时为每个动作填一项；空表示没有 SEQS 的合成资产，此时键必须严格递增。
    std::vector<KeyRange> ranges;
};
struct Interval { std::uint32_t first = 0, last = 0, repeat = 1; };
struct Emitter {
    Kind kind = Kind::Particle2;
    std::uint32_t nodeId = 0, flags = 0, textureId = 0, replaceableId = 0, materialId = UINT32_MAX;
    std::uint32_t rows = 1, columns = 1, headOrTail = 0, squirt = 0, textureSlot = 0;
    std::int32_t priority = 0;
    Filter filter = Filter::Blend;
    float rate = 0, speed = 0, variation = 0, latitude = 0, gravity = 0, life = 0;
    float length = 0, width = 0, tailLength = 0, middle = 0.5f, above = 0, below = 0, alpha = 1;
    Vec3 ribbonColor{1,1,1};
    std::array<Vec3, 3> colors{{{1,1,1},{1,1,1},{1,1,1}}};
    std::array<float, 3> alphas{1,1,0}, scales{1,1,1};
    std::array<Interval, 4> intervals{};
    std::array<Track, static_cast<std::size_t>(Parameter::Count)> tracks;
    std::uint32_t sourceIndex = 0; // Read提供的同kind源序号，跨同类块，包含被隔离记录。
};
struct SkippedEmitter {
    Kind kind = Kind::Particle2;
    std::uint32_t sourceIndex = 0;
    std::optional<std::uint32_t> nodeId;
    std::string code, message; // 原始局部诊断；不是修复后的合法发射器。
};
struct RecoveredVisibilityKey {
    std::uint32_t keyIndex = 0; // 原始序列中同帧边的后一键；没有删除或重排源键。
    std::int32_t frame = 0;
    std::uint64_t frameByteOffset = 0;
    std::uint32_t valueBits = 0;
};
struct RecoveredVisibilityTrack {
    Kind kind = Kind::Particle2;
    std::uint32_t sourceIndex = 0, nodeId = 0, sourceKeyCount = 0;
    std::uint64_t sourceByteOffset = 0; // 原MDX中KP2V tag的绝对偏移。
    std::vector<RecoveredVisibilityKey> duplicateKeys;
};
// 预览模式下接受的非全局逆序键（严格模式仍拒绝）；记录第一条逆序边与轨迹标签位置。
struct GameOrderTrack {
    Kind kind = Kind::Particle2;
    Parameter parameter = Parameter::Rate;
    std::uint32_t sourceIndex = 0, nodeId = 0;
    std::uint64_t sourceByteOffset = 0;
    std::int32_t left = 0, right = 0;
};
struct SequenceSpan { std::uint32_t start = 0, end = 0; };
struct Asset {
    std::vector<Emitter> emitters;
    std::vector<SequenceSpan> sequences; // SEQS 文件顺序；Timeline::sequenceIndex 指向这里。
    std::vector<std::uint32_t> globalSequences;
    std::uint32_t premCount = 0;
    std::vector<SkippedEmitter> skippedEmitters;
    // 仅PRE2/KP2V/None/global=-1/squirt=0的同帧逐位同值预览恢复，不是原版兼容证明。
    // 一次读取产出来源汇总，求值与模拟不重复生成诊断，也不计入skippedEmitters。
    std::vector<RecoveredVisibilityTrack> recoveredVisibilityTracks;
    std::vector<GameOrderTrack> gameOrderTracks;
};
struct ReadLimits {
    std::uint64_t maxBytes = 32ull * 1024 * 1024;
    std::uint32_t maxEmitters = 256, maxKeys = 65536;
};
enum class ReadMode { Strict, PreviewSkipInvalidEmitters };
// 不改变 MdxModel/RenderCore。只读 PRE2/RIBB 负载；Node 的图与 TRS 仍由现有读取器提供。
// 预览隔离仅跳过安全边界内已知局部内容错误；外层结构、未知布局和资源预算仍整次拒绝。
// 同帧同值的窄KP2V恢复保留全部源键与原预算；Strict仍要求严格递增。
// 预览模式的其他非全局逆序键按原版逐动作区间取值（GameOrderTrack 记录来源），全局轨迹仍须递增。
[[nodiscard]] Result<Asset> Read(std::span<const std::byte> bytes, ReadLimits limits = {},
    ReadMode mode = ReadMode::Strict);
struct Timeline {
    std::uint32_t start = 0, end = 0, sequenceIndex = 0;
    bool loop = true;
    // 同一显式时刻的两个时钟；global(elapsed)=globalReference+(elapsed-elapsedReference)。
    std::uint64_t elapsedReferenceMs = 0, globalReferenceMs = 0;
    Matrix world = Identity;
    // 世界预览只推进实际可表示的当前区间；默认严格回放仍验证从零开始的完整历史。
    bool boundedWorldPreview = false;
};
// AnchorSampler与属性轨迹共用映射。负历史global明确拒绝，不用模运算伪造节点历史。
[[nodiscard]] Result<std::uint64_t> GlobalTimeAt(const Timeline& timeline, std::uint64_t elapsedMs);
// 相位身份可为负，但不代表允许采样负global；每个实际采样时刻仍由GlobalTimeAt校验。
[[nodiscard]] Result<std::int64_t> GlobalPhase(const Timeline& timeline);
struct Anchor { Matrix model = Identity; Vec3 pivot{}; };
// 每个固定步一次提供全部发射器的模型局部节点变换。错误必须向上传播，不能填单位矩阵继续。
using AnchorSampler = std::function<Status(std::uint64_t, std::span<Anchor>)>;
struct Limits {
    std::uint32_t particles = 65536, ribbonSegments = 32768, perEmitter = 4096;
    std::uint32_t maxReplaySteps = 6000; // 单次重放至多60秒；不是每帧无限追赶。
};
struct Particle { Vec3 position{}, velocity{}; float age = 0, life = 0, gravity = 0; };
struct Segment { Vec3 above{}, below{}; float age = 0; std::array<float,4> color{1,1,1,1}; std::uint32_t cell = 0; };
struct EmitterState {
    std::uint64_t random = 0, spawned = 0, dropped = 0;
    std::uint32_t quota = 0;
    double accumulator = 0;
    std::int64_t lastBurst = -1;
    std::vector<Particle> particles;
    std::vector<Segment> segments;
};
struct State {
    std::uint64_t tickMs = 0, timeMs = 0, instance = 0, generation = 0;
    std::uint32_t sequenceIndex = 0;
    std::optional<std::int64_t> globalStartMs; // global-elapsed；严格回放必须非负，世界可只保留相位身份。
    bool boundedWorldPreview = false;
    std::vector<EmitterState> emitters;
};
struct Counts {
    std::uint32_t particles = 0, ribbonSegments = 0;
    std::uint64_t spawned = 0, suppressed = 0;
    bool budgetLimited = false;
};
// quota 由宿主分配；世界模式只给当前可见且获准模拟的实例分配活跃配额。
[[nodiscard]] Result<State> Create(const Asset& asset, std::uint64_t instance, std::uint64_t generation,
    std::uint32_t sequenceIndex, std::span<const std::uint32_t> quotas, const Limits& limits = {});
// 只在10ms网格上推进；残余毫秒由几何采样解析外推，不变更随机数或发射累计。
// 调用方用候选副本推进，失败/取消不提交；往回跳转、切动作或改全局相位须重新Create并从0重放。
// 默认即使没有全局轨迹也要求[0,target]的global可表示；有界世界模式只访问非负global的实际尾段。
[[nodiscard]] Status Advance(const Asset& asset, State& state, const Timeline& timeline,
    std::uint64_t targetMs, const AnchorSampler& sample, const Limits& limits = {});
// 世界预览冻结恢复：只老化/移除已有粒子和条带，不取锚点、不补发、不改变随机流。
// 同时推进到目标固定网格，下一帧从当前时刻正常发射；严格回放仍使用 Advance。
[[nodiscard]] Status FastForwardFrozen(const Asset& asset, State& state, const Timeline& timeline,
    std::uint64_t targetMs);
[[nodiscard]] Counts Count(const State& state) noexcept;
[[nodiscard]] Vec3 Sample(const Track& track, Vec3 fallback, const Asset& asset, const Timeline& timeline, std::uint64_t ms);
struct Vertex { Vec3 position{}; std::array<float,2> uv{}; std::array<float,4> color{1,1,1,1}; };
struct Draw { std::uint32_t first = 0, count = 0, emitter = 0; Filter filter = Filter::Blend; };
struct Geometry { std::vector<Vertex> vertices; std::vector<Draw> draws; };
// 全部动态顶点为世界空间；头/尾用同一粒子状态，RIBB为相邻历史边连接，绝不连跨实例条带。
[[nodiscard]] Result<Geometry> Build(const Asset& asset, const State& state, const Timeline& timeline,
    const Vec3& eye, const Vec3& up, std::uint32_t maxVertices = 1048576);
} // namespace War3Map::Render::Particles
