#pragma once

#include "War3MapCore/Types.h"

#include <span>
#include <string_view>

namespace War3Map::Render {

// 模型字节已按来源读到、但解析器不能解释其内容（坏头、截断、越界、未支持的块）。
// 只有存在 error 且全部属于这一类时才为真；读取、来源、预算错误都不在其中。
[[nodiscard]] inline bool IsMdxParseFailure(std::span<const Diagnostic> diagnostics) noexcept {
    bool error = false;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Error) continue;
        error = true;
        const std::string_view code = diagnostic.code;
        if (code != "MDX_MAGIC" && code != "MDX_VERSION_UNSUPPORTED" && code != "MDX_TRUNCATED"
            && code != "MDX_SIZE_INVALID" && code != "MDX_TAG_MISMATCH" && code != "MDX_VALUE_INVALID"
            && code != "MDX_NUMBER_INVALID" && code != "MDX_REFERENCE_INVALID"
            && code != "MDX_COLLISION_TYPE_UNSUPPORTED" && code != "MDX_GEOA_INTERPOLATION_UNSUPPORTED")
            return false;
    }
    return error;
}

} // namespace War3Map::Render
