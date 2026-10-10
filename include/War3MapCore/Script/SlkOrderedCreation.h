#pragma once

#include "War3MapCore/Script/SlkTemplateMaterializer.h"
#include "War3MapCore/Script/SlkTemplatePlan.h"

namespace War3Map::Script {

struct SlkOrderedCreationLimits {
    // 默认同时为硬上限；仅可收紧。字段、容器、复制值和每次fill产生的值累计计账。
    std::size_t abilities = 4096;
    std::size_t writes = 65536;
    std::size_t values = 2000000;
    std::size_t stringBytes = 16 * 1024 * 1024;
};
struct SlkOrderedCreationInput {
    const SlkTypedDataset* dataset = nullptr;
    const SlkNativeFrontendMaterialization* oldFrontend = nullptr;
    // 由可信上游按完整Lua chunk语义交付的已求值计划；不是分别Parse模板块的等价承诺。
    // 目标template.lua可跨块共享local甚至do/end；其解析/来源闭合仍在本单元之外。
    // 已求值对象无相互读取，writes保持各自执行顺序。
    std::span<const SlkTemplatePlan> orderedPlans;
    SlkOrderedCreationLimits limits;
};
enum class SlkOrderedCreationCoverage { OrderedAbilityCreationOnly };
struct SlkOrderedCreation {
    SlkOrderedCreationCoverage coverage = SlkOrderedCreationCoverage::OrderedAbilityCreationOnly;
    SlkSessionBindings bindings;
    std::vector<SlkSessionObject> createdAbilities;
    std::size_t appliedWrites = 0;
    std::size_t filledValues = 0;
    std::vector<std::string> unresolvedSemantics;
};

// 固定目标slk_lib的纯内存静态ability new/标量按序写入子集，不运行Lua，不读取/写地图。
// 从原始dataset default复制；common优先解析字段；levels写入立即执行common→byCode fill。
// 保留integer/double/string/bool；不调用to_type、around_number或WTS。支持裸repeat首级、
// 显式级号、metadata indexed字段及裸indexed首项；未知字段/内部写入/稀疏表/字符串截断拒绝。
// new前拒绝标准/未移走old对象/跨计划ID冲突；generated old由W2lo字符串标记识别。
// 输入必须绑定canonical old manifest和固定库SHA；公开可变图、计划与来源摘要的真实性仍由
// 后续可信生产者证明。本单元不创建Complete Session，也不返回WholeSessionNoObjectWrites。
// 成功附带SLK_CREATION_SESSION_UNVERIFIED；失败没有部分created对象。
[[nodiscard]] Result<SlkOrderedCreation> MaterializeSlkOrderedAbilityCreation(
    const SlkOrderedCreationInput& input);

} // namespace War3Map::Script
