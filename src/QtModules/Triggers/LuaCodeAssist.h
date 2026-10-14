#pragma once

#include "ProjectLuaModules.h"
#include "Widgets/CodeViewTypes.h"
#include "War3MapCore/Lua/LuaApiCatalog.h"

#include <QHash>
#include <QPointer>
#include <map>
#include <memory>
#include <string>
#include <vector>

class QWidget;

namespace War3Map::Modules {

using QtProject::LuaModuleTexts;
using QtProject::LuaModuleName;
using QtProject::CaptureLuaModules;

// 把 Lua 代码分析接到代码编辑器。位置在 QString 下标与 UTF-8 字节之间换算；
// notes 为函数库说明（按函数名），追加在 common.j 注释之后；style 提供悬停文字的配色；
// shared 为同一份地图脚本里的其它代码块（WCT 头部与其它触发器），其中的全局在当前代码里可见。
[[nodiscard]] UiKit::CodeAssist MakeLuaCodeAssist(std::shared_ptr<const Lua::ApiCatalog> catalog,
    std::shared_ptr<const LuaModuleTexts> modules, std::string currentModule,
    QHash<QString, QString> notes = {}, QPointer<QWidget> style = {}, const std::vector<std::string>& shared = {});

} // namespace War3Map::Modules
