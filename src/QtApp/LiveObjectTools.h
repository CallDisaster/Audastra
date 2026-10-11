#pragma once

#include <QJsonObject>
#include <QString>
#include "War3MapCore/Services/ObjectCommands.h"

#include <functional>

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::QtApp {

// 在所属线程调用；持有空文档等上下文的生命周期，复用物编宿主的完整来源检查。
using LiveObjectContextProvider = std::function<Result<Services::ObjectCommandContext>(ObjectData::ObjectType)>;
[[nodiscard]] LiveObjectContextProvider MakeLiveObjectContextProvider(
    QtProject::EditorProject& project, const QJsonObject& currentIdentity);

// 可信 GUI 适配：不访问控件，只使用 EditorProject 和原有对象撤销命令。
[[nodiscard]] QJsonObject InvokeLiveObjectTool(QtProject::EditorProject& project,
    const QString& name, const QJsonObject& arguments, const QJsonObject& currentIdentity);

} // namespace War3Map::QtApp
