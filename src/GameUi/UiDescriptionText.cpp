#include "UiDescriptionText.h"

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Text/UiNumberFormat.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace War3Map::GameUi {
namespace detail {

std::optional<UiValueReference> ParseUiValueReference(std::string_view text) {
    UiValueReference reference;
    const std::size_t open = text.find('[');
    if (open == std::string_view::npos) {
        if (!IsValidUiId(text)) return std::nullopt;
        reference.id = std::string(text);
        return reference;
    }
    if (text.back() != ']') return std::nullopt;
    const std::string_view id = text.substr(0, open);
    const std::string_view inside = text.substr(open + 1, text.size() - open - 2);
    if (!IsValidUiId(id)) return std::nullopt;
    reference.id = std::string(id);
    if (inside.empty()) {
        reference.perInstance = true;
        return reference;
    }
    std::uint32_t index = 0;
    const auto [end, error] = std::from_chars(inside.data(), inside.data() + inside.size(), index);
    if (error != std::errc() || end != inside.data() + inside.size()) return std::nullopt;
    reference.index = index;
    return reference;
}

std::optional<std::vector<UiTextPiece>> ParseUiTextTemplate(std::string_view text, std::string* error) {
    std::vector<UiTextPiece> pieces;
    std::string literal;
    const auto fail = [&](std::string message) -> std::optional<std::vector<UiTextPiece>> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '}') {
            if (i + 1 < text.size() && text[i + 1] == '}') {
                literal.push_back('}');
                ++i;
                continue;
            }
            return fail("单独的 } 要写成 }}");
        }
        if (c != '{') {
            literal.push_back(c);
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '{') {
            literal.push_back('{');
            ++i;
            continue;
        }
        const std::size_t close = text.find('}', i + 1);
        if (close == std::string_view::npos) return fail("{ 没有对应的 }");
        std::string_view inside = text.substr(i + 1, close - i - 1);
        UiTextPiece piece;
        if (const std::size_t colon = inside.find(':'); colon != std::string_view::npos) {
            const std::string_view digits = inside.substr(colon + 1);
            int decimals = 0;
            const auto [end, parseError] = std::from_chars(digits.data(), digits.data() + digits.size(), decimals);
            if (parseError != std::errc() || end != digits.data() + digits.size() || decimals < 0 || decimals > 6) {
                return fail("{" + std::string(inside) + "} 的小数位数应为 0 到 6");
            }
            piece.decimals = decimals;
            inside = inside.substr(0, colon);
        }
        piece.reference = ParseUiValueReference(inside);
        if (!piece.reference) return fail("{" + std::string(inside) + "} 不是取值（编号、编号[]、编号[下标]）");
        if (!literal.empty()) pieces.push_back(UiTextPiece{std::move(literal), std::nullopt, std::nullopt});
        literal.clear();
        pieces.push_back(std::move(piece));
        i = close;
    }
    if (!literal.empty()) pieces.push_back(UiTextPiece{std::move(literal), std::nullopt, std::nullopt});
    return pieces;
}

std::string FormatUiNumber(double value) {
    if (value == 0.0 || !std::isfinite(value)) return "0";
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc()) return "0";
    return std::string(buffer.data(), end);
}

} // namespace detail

namespace {

// 预览与游戏、运行库同一格式：实数先转成游戏里的 binary32 再按位数格式化，整数不经实数。
std::string NumberText(UiValueType type, const std::string& value, std::optional<int> decimals) {
    std::optional<std::string> text;
    if (type == UiValueType::Real) {
        float number = 0.0f;
        std::from_chars(value.data(), value.data() + value.size(), number);
        text = Text::FormatUiReal(number, decimals.value_or(-1));
    } else {
        std::int32_t number = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc() || end != value.data() + value.size()) return value;
        text = Text::FormatUiInteger(number, decimals.value_or(-1));
    }
    return text ? *text : value;
}

} // namespace

std::optional<std::string> UiValueText(const UiDescription& description, const UiInstanceState& state, std::string_view id,
    std::optional<std::uint32_t> index) {
    const UiValue* value = FindUiValue(description, id);
    if (!value) return std::nullopt;
    if ((value->size == 0) != !index.has_value()) return std::nullopt;
    if (index && *index >= value->size) return std::nullopt;
    const std::string key = index ? std::string(id) + "[" + std::to_string(*index) + "]" : std::string(id);
    if (const auto found = state.values.find(key); found != state.values.end()) {
        if (auto normalized = NormalizeUiValue(value->type, found->second)) return normalized;
    }
    return value->defaultValue;
}

std::string FormatUiText(const UiDescription& description, const UiInstanceState& state, std::string_view text,
    std::optional<std::uint32_t> instance) {
    const auto pieces = detail::ParseUiTextTemplate(text, nullptr);
    if (!pieces) return std::string(text);
    std::string result;
    for (const auto& piece : *pieces) {
        if (!piece.reference) {
            result += piece.literal;
            continue;
        }
        const auto& reference = *piece.reference;
        const std::optional<std::uint32_t> index = reference.perInstance ? instance : reference.index;
        if (reference.perInstance && !instance) continue;
        const auto value = UiValueText(description, state, reference.id, index);
        if (!value) continue;
        const UiValue* spec = FindUiValue(description, reference.id);
        if (spec->type == UiValueType::Real || spec->type == UiValueType::Integer) result += NumberText(spec->type, *value, piece.decimals);
        else result += *value;
    }
    return result;
}

} // namespace War3Map::GameUi
