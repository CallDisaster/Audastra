#pragma once

#include "War3MapCore/Types.h"

#include <string_view>

namespace War3Map::Text {

enum class SourceEncoding { Utf8, Gbk };

struct SourceScalar {
    std::uint32_t codepoint = 0;
    std::uint32_t offset = 0;
    std::uint8_t bytes = 0;
    [[nodiscard]] bool operator==(const SourceScalar&) const = default;
};

// 只读取声明编码的一个标量；来源上限16MiB，不猜编码或改写原字节。
// NUL/BOM/Ctrl-Z原保；不授予identifier、改名或脚本保存资格。
// 序列错误OriginSpan从原offset开始，length仅覆盖已检查/现有的原字节。
[[nodiscard]] Result<SourceScalar> ReadSourceScalar(
    std::string_view text, std::uint32_t offset, SourceEncoding encoding);

} // namespace War3Map::Text
