#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 地图自带界面：找出地图会读入哪些界面目录（toc）。游戏只经 toc 读入 fdf，toc 由地图脚本在运行时
// 加载，所以按脚本里加载 toc 的调用（字面量参数）与地图内的 .toc 文件确定，供界面库按地图层读入。
namespace War3Map::GameUi {

enum class ScriptLanguage : std::uint8_t { Jass, Lua };

struct MapScriptText {
    std::string path;  // 地图内路径，如 war3map.j
    ScriptLanguage language = ScriptLanguage::Jass;
    std::string text;
};

struct TocLoadCall {
    std::string function;    // 调用的函数名
    std::string path;        // 字面量参数（已解码）；参数不是字面量时为空
    std::string scriptFile;
    std::uint32_t line = 0;
};

// 脚本中加载 toc 的函数名（DzAPI 与 1.31+ 原生）。
[[nodiscard]] const std::vector<std::string_view>& TocLoadFunctions();

// 按脚本语言分词后查找加载 toc 的调用；注释与字符串里的同名文字不算。
[[nodiscard]] std::vector<TocLoadCall> FindTocLoadCalls(const MapScriptText& script);

struct MapTocPlan {
    // 依次读入的 toc：脚本按出现顺序加载的在前，地图里有但脚本未引用的在后（仍读入以便预览）。
    std::vector<std::string> tocs;
    std::vector<TocLoadCall> calls;
    std::vector<Diagnostic> diagnostics;
};

// mapFiles 为地图（含会话导入）内的文件路径。
[[nodiscard]] MapTocPlan PlanMapTocs(const std::vector<std::string>& mapFiles, const std::vector<MapScriptText>& scripts);

struct MapScriptPath {
    std::string_view path;
    ScriptLanguage language = ScriptLanguage::Jass;
};
// 地图脚本可能所在的路径（按游戏查找顺序）。
[[nodiscard]] const std::vector<MapScriptPath>& MapScriptPaths();

} // namespace War3Map::GameUi
