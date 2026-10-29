#include "GameUiTextFormat.h"

#include "War3MapCore/Text/UiNumberFormat.h"
#include "aj/Protocol.h"

namespace Audastra::Runtime32::GameUi {
namespace {

// 与文字参数的线路编码预算一致；不产生编码副本，也不截断输入或输出。
bool CountEncoded(std::string_view text, std::size_t& used) noexcept {
    for (const char character : text) {
        const std::size_t amount = character == '%' || character == ';' ? 3 : 1;
        if (amount > AJ_MAX_INPUT_TEXT - used) return false;
        used += amount;
    }
    return true;
}

} // namespace

std::optional<CompiledTextFormat> CompiledTextFormat::Parse(std::string_view format) {
    std::size_t encoded = 0;
    if (!CountEncoded(format, encoded) || !audastra::japi::ValidText(format)) return std::nullopt;
    CompiledTextFormat result;
    result.source_ = std::string(format);
    for (std::size_t position = 0; position < format.size();) {
        const auto start = position;
        if (format[position] != '{' && format[position] != '}') {
            do { ++position; }
            while (position < format.size() && format[position] != '{' && format[position] != '}');
            result.pieces_.push_back(Piece{start, position - start});
            continue;
        }
        const char bracket = format[position];
        if (position + 1 < format.size() && format[position + 1] == bracket) {
            result.pieces_.push_back(Piece{position, 1});
            position += 2;
            continue;
        }
        if (bracket != '{') return std::nullopt;
        ++position;
        if (position == format.size() || format[position] < '0' || format[position] > '7') return std::nullopt;
        const int argument = format[position++] - '0';
        int decimals = -1;
        if (position < format.size() && format[position] == ':') {
            ++position;
            if (position == format.size() || format[position] < '0' || format[position] > '6') return std::nullopt;
            decimals = format[position++] - '0';
        }
        if (position == format.size() || format[position] != '}') return std::nullopt;
        ++position;
        result.pieces_.push_back(Piece{0, 0, argument, decimals});
        result.required_ = static_cast<std::uint8_t>(result.required_ | (1u << argument));
    }
    return result;
}

std::optional<std::string> CompiledTextFormat::Render(const std::array<TextFormatArgument, 8>& arguments) const {
    std::string result;
    result.reserve(AJ_MAX_INPUT_TEXT);
    std::size_t encoded = 0;
    const auto append = [&](std::string_view text) {
        if (!CountEncoded(text, encoded)) return false;
        result.append(text);
        return true;
    };
    for (const auto& piece : pieces_) {
        if (piece.argument < 0) {
            if (!append(std::string_view(source_).substr(piece.offset, piece.size))) return std::nullopt;
            continue;
        }
        const auto& argument = arguments[static_cast<std::size_t>(piece.argument)];
        if (!argument.present) return std::nullopt;
        const auto text = argument.integer ? War3Map::Text::FormatUiInteger(argument.value, piece.decimals)
                                           : War3Map::Text::FormatUiReal(argument.real, piece.decimals);
        if (!text || !append(*text)) return std::nullopt;
    }
    if (!audastra::japi::ValidText(result)) return std::nullopt;
    return result;
}

} // namespace Audastra::Runtime32::GameUi
