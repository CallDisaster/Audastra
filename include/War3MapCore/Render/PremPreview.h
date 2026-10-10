#pragma once

#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Types.h"
#include "War3RenderCore/PoseTypes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace War3Map::Render::Prem {

// 仅基础模型粒子预览，不改变模型/地图字节，不提供保存或原版物理等价资格。
// root 负责资源优先级、.mdl 别名、子几何/动画/递归门禁和实际完成帧。
struct SourceBinding {
    War3RenderCore::AssetIdentity parentAsset;
    War3RenderCore::AssetSource parentSource, childSource;
};

class Emitter {
public:
    [[nodiscard]] const MdxModel& Model() const noexcept { return *model_; }
    [[nodiscard]] const MdxPremEmitter& Source() const noexcept { return source_; }
    [[nodiscard]] const SourceBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] std::uint32_t CompiledNodeIndex() const noexcept { return nodeIndex_; }
    [[nodiscard]] std::uint32_t ChildModelIndex() const noexcept { return childIndex_; }
private:
    std::shared_ptr<const MdxModel> model_;
    MdxPremEmitter source_;
    SourceBinding binding_;
    std::uint32_t nodeIndex_ = UINT32_MAX, childIndex_ = UINT32_MAX;
    Emitter() = default;
    friend Result<std::shared_ptr<const Emitter>> CreateEmitter(
        std::shared_ptr<const MdxModel>, std::uint32_t, std::uint32_t, std::uint32_t, SourceBinding);
};

// model 必须为宿主已冻结的 typed ParseMdx 结果；不在这里重新解析或读取资源。
// compiledNodeIndex 由真实 compiled.Nodes() 映射；每次采样仍核对 NodePose.objectId/pivot。
[[nodiscard]] Result<std::shared_ptr<const Emitter>> CreateEmitter(
    std::shared_ptr<const MdxModel> model, std::uint32_t sourceIndex,
    std::uint32_t compiledNodeIndex, std::uint32_t childModelIndex, SourceBinding binding);

struct Request {
    std::shared_ptr<const Emitter> emitter;
    War3RenderCore::InstanceIdentity identity;
    std::uint32_t requestIndex = UINT32_MAX, sequenceIndex = 0;
    std::uint64_t elapsedMs = 0, globalMs = 0;
    std::array<float, 12> parentTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; // row-major3x4
};

// 必须在 birthElapsed/birthGlobal 下严格求真实父节点；worldMatrix 为 column-major4x4、
// 已合成节点父链但仍是模型局部空间。服务再乘 parentTransform 一次。
using NodeSampler = std::function<Result<War3RenderCore::NodePose>(
    const Request&, std::uint64_t birthElapsed, std::uint64_t birthGlobal)>;

struct Limits {
    std::uint32_t maxRequests = 256, maxLiveChildren = 256;
    std::uint32_t maxBirths = 512, maxAnchorVisits = 512;
    std::uint32_t maxVisibilityKeyVisits = 65536;
    std::uint64_t maxFrameBytes = 256 * 1024;
};

struct Child {
    std::shared_ptr<const Emitter> emitter; // 路径/来源字符串共用不可变绑定，不按 child 复制。
    War3RenderCore::InstanceIdentity parentIdentity;
    std::uint32_t parentRequestIndex = UINT32_MAX, parentSequenceIndex = 0;
    std::uint64_t birthOrdinal = 0, birthElapsedMs = 0, birthGlobalMs = 0, ageMs = 0;
    std::array<float, 12> transform{}; // row-major3x4；节点/轴心/父实例已经恰好各应用一次。
};

struct Issue {
    std::shared_ptr<const Emitter> emitter;
    War3RenderCore::InstanceIdentity parentIdentity;
    std::uint32_t parentRequestIndex = UINT32_MAX;
    const char* code = ""; // 静态字符串；不替代宿主实际核心诊断。
    std::uint64_t rejectedBirths = 0;
};

struct Frame {
    std::vector<Child> children;
    std::vector<Issue> issues; // 每 request 至多一条，保真 owner，不按 nodeId 猜归属。
    std::uint64_t unsupportedRequests = 0, suppressedChildren = 0, clockRejectedChildren = 0;
    std::uint64_t candidateBirths = 0, evaluatedBirths = 0, anchorVisits = 0, visibilityKeyVisits = 0;
    War3RenderCore::PoseQuality quality = War3RenderCore::PoseQuality::PreviewApproximate;
};

// 无跨帧状态：以静态可见区间积分直接定位当前 lifespan 窗口的出生序号，不重放全历史。
// 同时钟/暂停完全重复；倒退或切动作重新构造同源候选，不使用负 delta，也不补无界历史。
// 可见1ms格在起点发射：ceil所需可见毫秒数的末格；随机角度取原弧度。
// gravity 在父模型局部Z轴，最后乘父实例；不播放子自身动画，不宣称原版随机/物理等价。
// 仅 flags0x9000、六静态参数、nonglobal None KPEV；其它轨迹/flags 局部拒绝。
// maxRequests4096、live4096、birth/anchor16384、key visits1048576、frame1MiB 为硬帽，只能收紧。
// elapsed/global <= UINT32_MAX；birthGlobal = global-(elapsed-birth) 下溢时拒绝该 request 的出生锚点。
[[nodiscard]] Result<Frame> Build(std::span<const Request> requests, const NodeSampler& sampler,
    const Limits& limits = {}, std::stop_token stop = {});

} // namespace War3Map::Render::Prem
