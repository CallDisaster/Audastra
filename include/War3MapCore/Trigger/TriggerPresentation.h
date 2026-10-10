#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace War3Map::Trigger {

class TriggerTypeSystem;

struct ReferencePresentation {
    std::string rawValue, displayName, iconPath, issue, reference, state;
    std::vector<std::string> candidates;
};

// 语句中一个顶层参数在 text 中的位置（UTF-8 字节偏移），界面据此把参数做成可点击的记号。
struct PresentationArgumentSpan {
    std::size_t begin = 0;
    std::size_t length = 0;
    std::size_t position = 0; // 在 EcaNode::arguments 中的序号
    std::string uiType;       // 定义中的参数类型；定义外参数为空
    bool empty = false;       // 值为空（未设置）
    bool call = false;        // 由函数调用提供
    std::string rawValue, displayName, iconPath, issue, reference, state;
    std::vector<std::string> candidates;
};

struct EcaPresentation {
    // 主视图只展示由当前 UI Catalog 生成的自然语言；原始函数名用于诊断和检查器。
    std::string text;
    std::string rawFunctionName;
    bool definitionAvailable = false;
    bool templateComplete = false;
    std::size_t renderedArgumentCount = 0;
    std::vector<PresentationArgumentSpan> arguments;
};

// 呈现可选项：物体 rawcode → 显示名解析（如 "hfoo" → "步兵"）。
// 返回空串表示未命中（保持显示四字码）。Core 不持有物体数据，由调用方注入。
struct PresentationOptions {
    std::function<std::string(std::string_view rawcode)> objectNameResolver;
    std::function<ReferencePresentation(std::string_view rawValue, std::string_view uiType)> referenceResolver;
    const TriggerTypeSystem* typeSystem = nullptr; // 宿主持有，避免列表逐行重建目录。
    bool showRawValues = false;
};

// UI TXT 中的 ${...} 是位置参数。此服务只消费动态 Catalog，不包含第三方函数白名单。
[[nodiscard]] EcaPresentation PresentEca(
    const TriggerDocument& document,
    std::uint32_t ecaIndex,
    const UiCatalog& catalog);
[[nodiscard]] EcaPresentation PresentEca(
    const TriggerDocument& document,
    std::uint32_t ecaIndex,
    const UiCatalog& catalog,
    const PresentationOptions& options);

[[nodiscard]] EcaPresentation PresentEca(
    const TriggerDocument& document,
    const EcaNode& eca,
    const UiCatalog& catalog);
[[nodiscard]] EcaPresentation PresentEca(
    const TriggerDocument& document,
    const EcaNode& eca,
    const UiCatalog& catalog,
    const PresentationOptions& options);

[[nodiscard]] std::string PresentArgument(
    const TriggerDocument& document,
    std::uint32_t argumentIndex,
    const UiCatalog& catalog);

// 函数库定义的作者说明，供代码悬停与补全：标题和提示（新格式 comment / 旧格式 Hint），
// 已按字符串表解析并去除颜色控制码；未提供的项为空。
struct DefinitionNote {
    std::string title;
    std::string hint;
};
[[nodiscard]] DefinitionNote PresentDefinitionNote(const UiDefinition& definition, const UiCatalog& catalog);

} // namespace War3Map::Trigger
