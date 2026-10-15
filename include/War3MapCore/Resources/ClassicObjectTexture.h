#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace War3Map::Resources {

// 仅供 classic 物编 texFile 替换入口使用。去掉文件名的扩展名后依次尝试 TGA、BLP；
// 普通 MDX TEXS 路径、队伍色和队伍光晕不采用该规则。返回路径保留大小写，规范化分隔符。
[[nodiscard]] Result<std::array<std::string, 2>> ClassicObjectTextureCandidates(std::string_view path);

// 只有明确的缺失才允许换后缀；来源冲突、读取失败、预算或空诊断都不能降级。
[[nodiscard]] bool ClassicObjectTextureMissing(std::span<const Diagnostic> diagnostics) noexcept;

template <typename Resource>
struct ClassicObjectTextureRead {
    std::string path; // 实际读取的候选；解码、来源身份和冻结缓存必须继续使用此路径。
    Resource resource;
};

// 每个候选都交回同一只读解析器，先完成该候选的完整来源查找，再考虑下一后缀。
// 不自行访问磁盘/归档；Resource 可保留宿主提供的来源层、SHA 与其它冻结身份。
template <typename Resource, typename Reader>
[[nodiscard]] Result<ClassicObjectTextureRead<Resource>> ReadClassicObjectTexture(
    std::string_view path, Reader&& read) {
    using Output = Result<ClassicObjectTextureRead<Resource>>;
    auto candidates = ClassicObjectTextureCandidates(path);
    if (!candidates.Succeeded()) return {std::nullopt, std::move(candidates.diagnostics)};
    std::vector<Diagnostic> missing;
    for (auto& candidate : *candidates.value) {
        auto resource = read(candidate);
        if (resource.Succeeded()) {
            auto output = Output::Success({std::move(candidate), std::move(*resource.value)});
            output.diagnostics = std::move(resource.diagnostics);
            return output;
        }
        if (!ClassicObjectTextureMissing(resource.diagnostics))
            return {std::nullopt, std::move(resource.diagnostics)};
        missing.insert(missing.end(), std::make_move_iterator(resource.diagnostics.begin()),
            std::make_move_iterator(resource.diagnostics.end()));
    }
    return {std::nullopt, std::move(missing)};
}

} // namespace War3Map::Resources
