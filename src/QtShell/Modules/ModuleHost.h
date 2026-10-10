#pragma once

#include "Modules/EditorModule.h"
#include <cstdint>

namespace War3Map::Shell {

// Shell 向模块提供的服务。模块之间不互相 include，跨模块交互都经由这里。
class ModuleHost {
public:
    virtual ~ModuleHost() = default;

    // 跳到目标模块并定位；记录返回栈。
    virtual void Navigate(const NavigationTarget& target) = 0;
    // 切到模块（浮动窗口提到最前、右侧组激活、主区显示）。
    virtual void ActivateModule(const ModuleId& id) = 0;
    virtual void ShowStatusMessage(const QString& text, int timeoutMs = 0) = 0;
    // 模块的撤销栈变了（例如物体编辑器切换了类别），Shell 重新路由顶栏撤销 / 重做。
    virtual void UndoStackChanged(const ModuleId& id) = 0;
    [[nodiscard]] virtual std::uint64_t NavigationEpoch() const { return 0; }
};

} // namespace War3Map::Shell
