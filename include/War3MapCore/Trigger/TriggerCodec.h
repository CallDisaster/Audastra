#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <span>

namespace War3Map::Trigger {

// 对项目既有 WTG v7/WCT v1 解析器进行 clean-room 重构，只依据
// War3TriggerCompilerCpp 已实现的格式行为，不包含 YDWE、HiveWE 或 w3x2lni 的实现代码。
class WtgWctCodec {
public:
    // 原创空 v7/v1 文档经同一解析器建立来源 seal，不放宽既有文档的验证资格。
    [[nodiscard]] Result<TriggerDocument> CreateEmpty(const UiCatalog& catalog) const;

    [[nodiscard]] Result<TriggerDocument> Parse(
        std::span<const std::byte> wtg,
        std::span<const std::byte> wct,
        const UiCatalog& catalog) const;

    [[nodiscard]] Status Validate(
        const TriggerDocument& document,
        const UiCatalog& catalog) const;

    // 只校验单个触发器可安全读取所需的部分：来源 seal、变量表、目标记录及其 ECA/参数子树。
    // 用于生成目标触发器的脚本预览/转换；不代替保存前的完整 Validate。
    [[nodiscard]] Status ValidateTrigger(
        const TriggerDocument& document,
        const UiCatalog& catalog,
        StableId triggerId) const;

    [[nodiscard]] Result<EncodedTriggerDocument> Serialize(
        const TriggerDocument& document,
        const UiCatalog& catalog) const;

    // 只用于内容身份（修改指纹）：写出与 Serialize 逐字节相同的结果，但不做函数库合法性校验。
    // 结构损坏（下标越界、缺 childId、无法编码的文本、无来源 seal）仍然失败；不得用于保存。
    [[nodiscard]] Result<EncodedTriggerDocument> EncodeForIdentity(const TriggerDocument& document) const;

    [[nodiscard]] TriggerInspection Inspect(
        const TriggerDocument& document) const noexcept;

    [[nodiscard]] Result<War3Map::TriggerPatch> BuildPatch(
        const TriggerDocument& document,
        const UiCatalog& catalog,
        std::filesystem::path wtgRelativePath,
        std::filesystem::path wctRelativePath,
        std::string summary) const;
};

} // namespace War3Map::Trigger
