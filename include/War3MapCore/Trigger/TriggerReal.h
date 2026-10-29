#pragma once

#include "War3MapCore/Types.h"
#include <optional>
#include <string>
#include <string_view>

namespace War3Map::Trigger {
// 有限 float32 的最短往返十进制；不使用经典脚本不接受的指数写法。
[[nodiscard]] std::optional<std::string> FormatTriggerReal(double value);
[[nodiscard]] std::optional<std::string> ShortestTriggerReal(std::string_view value);
// 仅诊断，不改写作者文本。九位是静态风险阈值，不是引擎精度保证。
[[nodiscard]] std::optional<Diagnostic> CheckTriggerRealLiteral(std::string_view value);
}
