#pragma once

#include "War3MapCore/Script/ScriptPipeline.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Script::detail {

// YDWE 模板 import(名)(文本) / import(名)(相对路径, true) 的写入规则。
// 内置求值器与 Lua 模板沙盒共用；同名后者覆盖前者，路径只读且不越出 importRoot。
class TemplateImportSink {
public:
    TemplateImportSink(const std::optional<std::filesystem::path>& root, std::vector<MemberReplacement>& imports,
        std::vector<Diagnostic>& diagnostics)
        : root_(root), imports_(imports), diagnostics_(diagnostics) {}

    // 规范化并检查地图内成员名；失败时返回空并写出 error。
    [[nodiscard]] std::optional<std::string> MemberName(std::string name, std::string& error) const;
    // 写入文本或读取地图目录下的文件；root 为空时只核对写法（只读检查场景）。失败返回 false 并写出 error。
    [[nodiscard]] bool Add(const std::string& member, std::string_view content, bool path, std::string& error);

private:
    const std::optional<std::filesystem::path>& root_;
    std::vector<MemberReplacement>& imports_;
    std::vector<Diagnostic>& diagnostics_;
    std::size_t bytes_ = 0;
};

} // namespace War3Map::Script::detail
