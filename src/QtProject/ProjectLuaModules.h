#pragma once

#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace War3Map::QtProject {
class EditorProject;

// 地图里 Lua 文件的冻结正文：require 名（lessons.hello）→ 正文。
using LuaModuleTexts = std::map<std::string, std::string, std::less<>>;

// 导入路径对应的 require 名：lessons\hello.lua → lessons.hello；不是 .lua 返回空。
[[nodiscard]] std::string LuaModuleName(const QString& path);

// 冻结地图里其他 Lua 文件的正文，供 require 补全与检查（编辑器与 MCP 共用）；current 为当前文件（不收入），可为空。
[[nodiscard]] std::shared_ptr<const LuaModuleTexts> CaptureLuaModules(const EditorProject& project, const QString& current = {});

} // namespace War3Map::QtProject
