#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace War3Map::Modules {
class GameUiModule;

// 界面工作区的 MCP/Agent 适配：查询与操作都走 GameUiModule 的公共接口，与界面共用同一份状态。
// 参数已由 Mcp::InvokeGameUiTool 校验；这里只处理取值语义。
class GameUiToolService final {
public:
    using ModuleProvider = std::function<GameUiModule*()>;
    explicit GameUiToolService(ModuleProvider provider);
    [[nodiscard]] QJsonObject Invoke(const QString& name, const QJsonObject& arguments);

private:
    ModuleProvider provider_;
    bool waiting_ = false;  // 正在等模拟准备：不嵌套等待
};

} // namespace War3Map::Modules
