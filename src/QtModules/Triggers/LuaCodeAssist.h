#pragma once

#include "Widgets/CodeViewTypes.h"
#include "War3MapCore/Lua/LuaApiCatalog.h"

#include <QHash>
#include <QPointer>
#include <map>
#include <memory>
#include <string>

class QWidget;

namespace War3Map::Modules {

// 地图里 Lua 文件的冻结正文：require 名（lessons.hello）→ 正文。
using LuaModuleTexts = std::map<std::string, std::string, std::less<>>;

// 导入路径对应的 require 名：lessons\hello.lua → lessons.hello；不是 .lua 返回空。
[[nodiscard]] std::string LuaModuleName(const QString& path);

// 把 Lua 代码分析接到代码编辑器。位置在 QString 下标与 UTF-8 字节之间换算；
// notes 为函数库说明（按函数名），追加在 common.j 注释之后；style 提供悬停文字的配色。
[[nodiscard]] UiKit::CodeAssist MakeLuaCodeAssist(std::shared_ptr<const Lua::ApiCatalog> catalog,
    std::shared_ptr<const LuaModuleTexts> modules, std::string currentModule,
    QHash<QString, QString> notes = {}, QPointer<QWidget> style = {});

} // namespace War3Map::Modules
