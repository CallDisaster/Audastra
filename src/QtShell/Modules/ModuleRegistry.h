#pragma once

#include "Modules/EditorModule.h"

#include <memory>
#include <vector>

namespace War3Map::Shell {

// 模块注册表：登记顺序即切换器顺序。新增模块只需在组装处登记，不改 Shell。
class ModuleRegistry final {
public:
    // 重复 ID 视为组装错误，返回 false 且不登记。
    bool Register(std::unique_ptr<EditorModule> module);

    [[nodiscard]] EditorModule* Find(const ModuleId& id) const;
    [[nodiscard]] std::vector<ModuleId> Ids() const;
    [[nodiscard]] std::vector<ModuleId> AvailableIds() const;
    [[nodiscard]] const std::vector<std::unique_ptr<EditorModule>>& Modules() const noexcept { return modules_; }

private:
    std::vector<std::unique_ptr<EditorModule>> modules_;
};

} // namespace War3Map::Shell
