#pragma once

#include "War3MapCore/Services/ObjectCommands.h"
#include <functional>

namespace War3Map::QtProject {
class EditorProject;

// 仅在工程所属线程调用。provider 持有临时空表和名称快照的生命周期；返回值为借用视图。
// 同一个 provider 只适用于创建时的工程修订；关闭或切换地图后须重新创建。
using ObjectContextProvider = std::function<Result<Services::ObjectCommandContext>(ObjectData::ObjectType)>;
[[nodiscard]] ObjectContextProvider MakeObjectContextProvider(EditorProject& project);

} // namespace War3Map::QtProject
