#pragma once

#include "War3MapCore/MapData/MapSettings.h"

namespace War3Map::MapData {

// 只读取经典 v25 的中文/英文 LNI 数据，不执行 Lua，也不生成原生 W3I。
// settings 的原生 OriginalBytes 为空；完整性与原始文本以本结构为准。
struct LniMapSettings {
    MapSettings settings;
    std::vector<std::byte> originalBytes;
    bool fullyParsed = false;
    std::vector<std::string> unknownFields;
};

[[nodiscard]] Result<LniMapSettings> ParseLniMapSettings(std::span<const std::byte> data);

} // namespace War3Map::MapData
