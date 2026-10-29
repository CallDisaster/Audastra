#include "War3MapCore/Trigger/TriggerReal.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace War3Map::Trigger {
namespace {
std::string_view Trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == value.npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
bool Risk(std::string_view value) {
    if (value.starts_with('-')) value.remove_prefix(1);
    const auto dot = value.find('.');
    if (dot != value.npos && value.size() - dot - 1 > 9) return true;
    std::string digits;
    for (const char c : value) if (c >= '0' && c <= '9') digits += c;
    const auto first = digits.find_first_not_of('0');
    const auto end = digits.find_last_not_of('0');
    return first != digits.npos && end - first + 1 > 9;
}
std::string Plain(std::string text) {
    const auto e = text.find_first_of("eE");
    if (e != text.npos) {
        int exponent = 0;
        auto power = std::string_view(text).substr(e + 1);
        if (power.starts_with('+')) power.remove_prefix(1);
        const auto parsed = std::from_chars(power.data(), power.data() + power.size(), exponent);
        if (parsed.ec != std::errc{}) return {};
        const bool negative = text.front() == '-';
        std::string mantissa = text.substr(negative ? 1 : 0, e - (negative ? 1 : 0));
        const auto dot = mantissa.find('.');
        const int point = static_cast<int>(dot == mantissa.npos ? mantissa.size() : dot) + exponent;
        if (dot != mantissa.npos) mantissa.erase(dot, 1);
        if (point <= 0) text = "0." + std::string(static_cast<std::size_t>(-point), '0') + mantissa;
        else if (static_cast<std::size_t>(point) >= mantissa.size())
            text = mantissa + std::string(static_cast<std::size_t>(point) - mantissa.size(), '0') + ".0";
        else { text = mantissa; text.insert(static_cast<std::size_t>(point), "."); }
        if (negative) text.insert(0, "-");
    }
    if (text.find('.') == text.npos) text += ".0";
    return text;
}
}
std::optional<std::string> FormatTriggerReal(double value) {
    const float number = static_cast<float>(value);
    if (!std::isfinite(value) || !std::isfinite(number) || (value != 0 && number == 0)) return {};
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number);
    if (converted.ec != std::errc{}) return {};
    auto text = Plain(std::string(buffer, converted.ptr));
    float back = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), back);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || std::bit_cast<std::uint32_t>(number) != std::bit_cast<std::uint32_t>(back)) return {};
    return text;
}
std::optional<std::string> ShortestTriggerReal(std::string_view value) {
    value = Trim(value);
    if (!TriggerTypeSystem::IsRealLiteral(value)) return {};
    float number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{}) return {};
    return FormatTriggerReal(number);
}
std::optional<Diagnostic> CheckTriggerRealLiteral(std::string_view value) {
    const auto trimmed = Trim(value);
    if (!TriggerTypeSystem::IsRealLiteral(trimmed) || !Risk(trimmed)) return {};
    std::string message = "实数文本过长，经典脚本解析可能偏离预期。";
    const auto shorter = ShortestTriggerReal(trimmed);
    if (shorter && shorter->size() < trimmed.size() && !Risk(*shorter)) message += "建议短写为 " + *shorter + "。";
    else message += "没有不再触发风险提示的等值短写建议，请核对数值。";
    return Diagnostic{DiagnosticSeverity::Warning, "Trigger.Value", "TRIGGER_REAL_LITERAL_PRECISION_RISK", std::move(message)};
}
}
