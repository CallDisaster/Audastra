#include "War3MapCore/ObjectData/LevelAutofill.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace War3Map::ObjectData {
namespace {

constexpr const char* kDomain = "ObjectData.Autofill";

std::string Number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.9g", value);
    return buffer;
}

} // namespace

Result<std::vector<LevelAutofillValue>> ComputeLevelAutofill(const LevelAutofill& spec, LevelAutofillKind kind) {
    using Values = std::vector<LevelAutofillValue>;
    if (spec.startLevel < 1 || spec.endLevel > 1000 || spec.startLevel > spec.endLevel)
        return Result<Values>::Failure(kDomain, "AUTOFILL_LEVEL_RANGE", "等级范围须在 1..1000 内且开始等级不大于结束等级。");
    Values values;
    values.reserve(spec.endLevel - spec.startLevel + 1);
    if (kind == LevelAutofillKind::Text) {
        for (auto level = spec.startLevel; level <= spec.endLevel; ++level) {
            std::string text = spec.baseText;
            if (!spec.levelToken.empty()) {
                const auto number = std::to_string(level);
                for (std::size_t at = 0; (at = text.find(spec.levelToken, at)) != std::string::npos; at += number.size())
                    text.replace(at, spec.levelToken.size(), number);
            }
            values.push_back({level, 0, std::move(text)});
        }
        return Result<Values>::Success(std::move(values));
    }
    for (const double factor : {spec.base, spec.previousFactor, spec.levelFactor, spec.constant})
        if (!std::isfinite(factor)) return Result<Values>::Failure(kDomain, "AUTOFILL_NUMBER_INVALID", "基础值与各因素须是有限数字。");
    double previous = spec.base;
    for (auto level = spec.startLevel; level <= spec.endLevel; ++level) {
        const double value = level == spec.startLevel ? spec.base
            : previous * spec.previousFactor + static_cast<double>(level) * spec.levelFactor + spec.constant;
        if (!std::isfinite(value))
            return Result<Values>::Failure(kDomain, "AUTOFILL_NUMBER_INVALID", "第 " + std::to_string(level) + " 级结果超出数字范围。");
        if (kind == LevelAutofillKind::Integer) {
            if (value != std::trunc(value))
                return Result<Values>::Failure(kDomain, "AUTOFILL_INTEGER_FRACTION",
                    "整数字段第 " + std::to_string(level) + " 级结果 " + Number(value) + " 不是整数。");
            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                return Result<Values>::Failure(kDomain, "AUTOFILL_INTEGER_RANGE",
                    "整数字段第 " + std::to_string(level) + " 级结果 " + Number(value) + " 超出整数范围。");
        } else if (std::abs(value) > std::numeric_limits<float>::max()) {
            return Result<Values>::Failure(kDomain, "AUTOFILL_NUMBER_INVALID", "第 " + std::to_string(level) + " 级结果超出实数范围。");
        }
        values.push_back({level, value, kind == LevelAutofillKind::Integer
            ? std::to_string(static_cast<std::int32_t>(value)) : Number(static_cast<float>(value))});
        previous = value;
    }
    return Result<Values>::Success(std::move(values));
}

} // namespace War3Map::ObjectData
