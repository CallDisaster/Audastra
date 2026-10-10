#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Types.h"

#include <string>
#include <vector>

namespace War3Map::Trigger {

// WTG→JASS 触发器片段结果，不包含整图 config/main 或资源初始化。
struct JassCompileResult {
    std::string jassCode;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool Succeeded() const noexcept {
        for (const auto& d : diagnostics) {
            if (d.severity == DiagnosticSeverity::Error) return false;
        }
        return true;
    }
};

// 将已支持的触发器语义编译为 JASS 片段；不得直接作为 war3map.j 写回地图。
// 元数据驱动：从 UiCatalog 查找每个 ECA 节点的 script_name 生成对应调用。
// 未注册函数、无效参数与未支持语义记录 Error，并清空整个产出。
[[nodiscard]] JassCompileResult CompileTriggersToJass(
    const TriggerDocument& document,
    const UiCatalog& catalog);

} // namespace War3Map::Trigger
