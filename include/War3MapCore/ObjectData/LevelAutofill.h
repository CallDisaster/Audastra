#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <vector>

// 原版物编“自动填充等级”的取值计算：只算出各等级的值，写入由调用方经正常字段编辑提交。
// 物编界面与 Agent 工具共用。
namespace War3Map::ObjectData {

enum class LevelAutofillKind : std::uint8_t { Integer, Real, Text };

struct LevelAutofill {
    std::uint32_t startLevel = 1;
    std::uint32_t endLevel = 1; // 含
    // 数值：开始等级取 base；之后每级 = 上一级 × previousFactor + 等级 × levelFactor + constant。
    double base = 0;
    double previousFactor = 1;
    double levelFactor = 0;
    double constant = 0;
    // 文本：每级取 baseText，其中的 levelToken（非空时）全部换成该等级数字。
    std::string baseText;
    std::string levelToken;
};

struct LevelAutofillValue {
    std::uint32_t level = 0;
    double number = 0;
    std::string text;
};

// 等级范围为 1..1000 且 start ≤ end；数值须有限，整数字段每级结果须是 int32 范围内的整数——
// 出现小数时报出等级与结果，不替作者取整。
[[nodiscard]] Result<std::vector<LevelAutofillValue>> ComputeLevelAutofill(const LevelAutofill& spec, LevelAutofillKind kind);

} // namespace War3Map::ObjectData
