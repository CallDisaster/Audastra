#pragma once

#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Script/SlkTemplateSession.h"
#include "War3MapCore/Script/SlkDataset.h"

namespace War3Map::Script {

struct SlkNativeFrontendLimits {
    // 默认值同时是硬上限；调用方只能收紧，0 不提供此项预算。
    std::size_t values = 2000000; // 真实默认约25.4万字段，原始default+old双图；构造前逐项计账
    std::size_t stringBytes = 16 * 1024 * 1024; // 包括字段键和 WTS 值
    std::size_t wtsEntries = 100000;
};

struct SlkNativeFrontendInput {
    SlkSessionBindings bindings;
    SlkNativeFrontendLimits limits;
    // 必须来自调用方已验证的所选 YDWE default 与 metadata；普通字符串显示目录不能代替。
    // 本组件继续校验对象身份、字段适用性和 Lua 值类型，不负责从资源读取/解析 typed default。
    const ObjectData::ObjectFieldSchema* schema = nullptr;
    std::array<std::vector<SlkSessionObject>, 7> standardDefaults;
    // 新入口直接使用 canonical common/byCode，不从展示/编辑 schema 反推。
    // 与 schema、standardDefaults 互斥；dataSetIdentity 必须等于 dataset.manifestSha256，
    // metadata/default/W3I SHA 和 revision 逐项匹配其读取回执。
    const SlkTypedDataset* canonicalDataset = nullptr;
    std::array<const ObjectData::NativeObjectDocument*, 7> nativeDocuments{};
    const MapData::StringTableDocument* strings = nullptr;
    // 输入扫描必须证明没有地图 SLK/LNI/TXT 或其他未建模物编来源；默认不推断为不存在。
    bool verifiedNativeOnlySources = false;
    std::vector<Diagnostic> sourceValidationDiagnostics;
};

enum class SlkNativeFrontendCoverage { ExistingNativeFrontendOnly };
struct SlkNativeFrontendMaterialization {
    SlkNativeFrontendCoverage coverage = SlkNativeFrontendCoverage::ExistingNativeFrontendOnly;
    // existingObjects 已构造，createdObjects 未构造；所有类始终 Unverified，不能用于模板放行。
    SlkTemplateSession session;
    std::array<std::vector<std::string>, 7> generatedObjectIds; // mark_obj 的 old 身份，包含空字符串 W2lo
    std::size_t appliedModifications = 0;
    std::size_t resolvedStringReferences = 0;
    std::string canonicalDatasetManifest; // canonical 分支才填写；不是完整性凭据
};

// 原生 v2 frontend 的有界子集。复用 NativeObjectDocument 的真实存储类型与字段地址，
// 对最终 Serialize 字节核对已绑定 SHA；不会改变 native 文档、默认对象或 WTS。
// old 端字符串逐个替换嵌入式 TRIGSTR_数字，不递归；缺失引用/重复 WTS ID 拒绝，不提前转换 bool。
// 原始等级 0 按 1；检查 data 索引、唯一字段地址、适用的 metadata 和默认数组。
// 支持稠密默认数组与 sparse native 覆盖经 profile/SLK 规则补齐；所得 sparse 表、
// 自定义父链、重复字段地址、未知 metadata 明确拒绝。legacy schema 入口还拒绝未证明的
// codemapping/doodad变体；canonical入口按common→byCode两轮消费，按byCode优先选择fill，
// 支持尾NUL补足的1–4字节metadata ID，保留缺失_code与原始typed默认字段。
// 先汇总 native 级数字段再填充，不按 modification 序列逐次做 new 端的 fill。
// 成功只有 ExistingNativeFrontendOnly，附带 new/完整来源验证未完成的诊断；Session 仍 Unverified。
// 硬上限：20000 个默认/原生对象、300000 条覆盖、2000000 个值/字段/数组容器、16 MiB 字符串、256 级；
// WTS 最多 100000 条与 64 MiB 原字节，索引前检查；成功返回不修改输入 limits。
// metadata 最多 10000 条，字段/适用 ID/原生存储匹配预算最多 50000000 次。
[[nodiscard]] Result<SlkNativeFrontendMaterialization> MaterializeSlkNativeFrontend(
    const SlkNativeFrontendInput& input);

} // namespace War3Map::Script
