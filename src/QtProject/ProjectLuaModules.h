#pragma once

#include "War3MapCore/Types.h"

#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace War3Map::QtProject {
class EditorProject;

// 地图里 Lua 文件的冻结正文：require 名（lessons.hello）→ 正文。
using LuaModuleTexts = std::map<std::string, std::string, std::less<>>;

// 导入路径对应的 require 名：lessons\hello.lua → lessons.hello；不是 .lua 返回空。
[[nodiscard]] std::string LuaModuleName(const QString& path);

// 冻结地图里其他 Lua 文件的正文，供 require 补全与检查（编辑器与 MCP 共用）；current 为当前文件（不收入），可为空。
[[nodiscard]] std::shared_ptr<const LuaModuleTexts> CaptureLuaModules(const EditorProject& project, const QString& current = {});

// 地图里的 Lua 文件：编辑器面板与 MCP 共用的列表、读取和写入。
struct LuaScriptEntry {
    QString path;
    QString state; // 原内容 / 待导入 / 工程草稿 / 来源歧义
    qint64 size = 0;
};
[[nodiscard]] std::vector<LuaScriptEntry> ListLuaScripts(const EditorProject& project);

struct LuaScriptText {
    QString text;
    std::string sha256; // 当前字节的身份，写入时用来确认没有被别处改过
};
// 只接受 UTF-8 正文；超过 Lua 资源上限或编码不对时失败。
[[nodiscard]] Result<LuaScriptText> ReadLuaScript(const EditorProject& project, const QString& path);

// 新建或整体替换一个 Lua 文件，作为一次可撤销的资源编辑。expectedSha256 非空时必须与当前字节一致；
// 替换时沿用原文件的 CRLF 换行。返回空串表示成功，否则是原因。
[[nodiscard]] QString WriteLuaScript(EditorProject& project, const QString& path, QString text,
    const std::string& expectedSha256, const QString& label);

} // namespace War3Map::QtProject
