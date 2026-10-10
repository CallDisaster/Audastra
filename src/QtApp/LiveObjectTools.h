#pragma once

#include <QJsonObject>
#include <QString>

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::QtApp {

// 可信 GUI 适配：不访问控件，只使用 EditorProject 和原有对象撤销命令。
[[nodiscard]] QJsonObject InvokeLiveObjectTool(QtProject::EditorProject& project,
    const QString& name, const QJsonObject& arguments, const QJsonObject& currentIdentity);

} // namespace War3Map::QtApp
