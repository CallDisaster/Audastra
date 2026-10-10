#pragma once

#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/LuaMapPipeline.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"

namespace War3Map::Script {

// 已检查脚本和生成物编的保存准备结果。纯内存，不写地图，也不宣告模板语义已证明。
// 模板执行者的来源、版本、权限与完整性由生产执行适配器负责；这里检查输出格式和提交边界。
struct ScriptSaveBundle {
    std::vector<MemberReplacement> members;
    std::array<std::unique_ptr<ObjectData::NativeObjectDocument>, 7> nativeBaselines;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, 7> editableBaselines;
};

// 要求整个流水线成功；最多七类经典 native 成员，共64MiB，脚本最多64MiB。
// 非白名单/重名/未知版本/不安全结构/无法建立可编辑基线时不返回部分bundle。
[[nodiscard]] Result<ScriptSaveBundle> PrepareScriptSaveBundle(
    const PipelineResult& pipeline, std::string_view scriptMember);

// 完整Lua由独立类型检查结果进入同一个成员合并/归档事务；不生成或替换物编。
[[nodiscard]] Result<ScriptSaveBundle> PrepareScriptSaveBundle(
    const LuaMapPipelineResult& pipeline, std::string_view scriptMember);

// 仅供需要规范 LNI 整类替换的调用方使用的保守预检。
// currentEditable 必须来自本次保存冻结的完整七类快照；nullptr 只表示该类原本不存在，
// 不能用 nullptr 代替未捕获/未知文档。仅检查 bundle 实际生成的类别，输入均不修改。
// 只放行无地图自定义父链、无 opaque/非 native 值、无额外文本的规范 LNI；
// MapSaveService 的正式保存路径已改用基于冻结源 native 的三方保留合并。
// 本服务不证明模板执行语义、授权或来源身份；bundle 必须为未改动的 Prepare 输出。
[[nodiscard]] Status PreflightScriptSaveBaselineReplacement(
    const ScriptSaveBundle& bundle,
    const std::array<const ObjectData::LniObjectDocument*, 7>& currentEditable);

// 合并前核所有名称/重复/替换删除冲突，预留容量后只做noexcept移动；失败不修改输入。
// 已有同名待保存版本被最终生成版本替换，不重复追加。最终生成成员不能被同时删除。
// 不复制与本次生成无关的大型导入字节。成功消耗generated，调用方才切换对应基线。
[[nodiscard]] Status MergeScriptSaveMembers(std::vector<MemberReplacement>& pending,
    const std::vector<std::string>& removals, std::vector<MemberReplacement>& generated);

} // namespace War3Map::Script
