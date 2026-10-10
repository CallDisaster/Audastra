#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

// 生成整图脚本（预处理前的 WE + YDTrigger 等价输出）；地图只读。
// 给出 compileDir 时继续走注入 → 预处理 → 模板 → vJass 翻译 → 语法检查流水线，阶段文件写入该目录。
// engines：空为内置实现；--external 全部使用外部程序（Wave、jasshelper、pjass）；--cross-check 内置检查后再用 pjass 对照。
int RunScriptBuild(const std::filesystem::path& toolRoot, const std::filesystem::path& gameRoot,
    const std::filesystem::path& mapPath, const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& compileDir = std::nullopt, std::wstring_view engines = {});

// 调试：只对已有的 Wave 输出做模板求值（技能命令查询使用地图物编）。
int RunScriptTemplate(const std::filesystem::path& toolRoot, const std::filesystem::path& gameRoot,
    const std::filesystem::path& mapPath, const std::filesystem::path& inputPath, const std::filesystem::path& outputPath);

#include <string>
// 调试：列出原版物体目录中某物体的全部字段。
int RunObjectField(const std::filesystem::path& gameRoot, const std::string& type, const std::string& rawcode);
