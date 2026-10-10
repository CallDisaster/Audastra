#include "War3MapCore/Text/SourceEncoding.h"
#include "War3MapCore/Text/Utf8Scalar.h"

#include <Windows.h>

#include <cstring>
#include <string>

namespace War3Map::Text {
namespace {

constexpr std::size_t MaxSourceBytes = 16 * 1024 * 1024;

Result<SourceScalar> Error(std::string code, std::string message,
    std::uint32_t offset, std::uint64_t length = 0) {
    auto result = Result<SourceScalar>::Failure("SourceEncoding", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{std::filesystem::path{}, offset, length, {}};
    return result;
}

Result<SourceScalar> Utf8(std::string_view text, std::uint32_t offset) {
    const auto scalar = Detail::ReadUtf8Scalar(text, offset);
    switch (scalar.error) {
    case Detail::Utf8ScalarError::None:
        return Result<SourceScalar>::Success({scalar.codepoint, offset, scalar.bytes});
    case Detail::Utf8ScalarError::Truncated:
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "UTF-8序列在来源末尾截断。", offset, scalar.examinedBytes);
    case Detail::Utf8ScalarError::InvalidContinuation:
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "UTF-8续字节无效。", offset, scalar.examinedBytes);
    case Detail::Utf8ScalarError::InvalidScalar:
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "UTF-8过长编码、代理码点或越界码点无效。", offset, scalar.examinedBytes);
    default:
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "UTF-8起始字节无效。", offset, 1);
    }
}

Result<SourceScalar> Gbk(std::string_view text, std::uint32_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    const auto length = static_cast<std::uint8_t>(first == 0x80 ? 1 : 2);
    if (first != 0x80 && (first < 0x81 || first > 0xFE)) {
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "GBK起始字节无效。", offset, 1);
    }
    if (text.size() - offset < length) {
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "GBK双字节序列在来源末尾截断。", offset, text.size() - offset);
    }
    if (length == 2) {
        const auto next = static_cast<unsigned char>(text[offset + 1]);
        if (next < 0x40 || next > 0xFE || next == 0x7F) {
            return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "GBK尾字节无效。", offset, 2);
        }
    }
    // CP936原生映射必须精确往返；0x80仅在映射为Euro并原样往返时允许。
    // 不使用ACP/GB18030，也不允许默认替代或best-fit映射。
    wchar_t wide = 0;
    if (MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, text.data() + offset, length, &wide, 1) != 1
        || wide == 0xFFFD || (wide >= 0xD800 && wide <= 0xDFFF) || (first == 0x80 && wide != 0x20AC)) {
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "GBK序列没有精确的CP936标量映射。", offset, length);
    }
    char encoded[2]{};
    BOOL usedDefault = FALSE;
    const int encodedBytes = WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, &wide, 1,
        encoded, 2, nullptr, &usedDefault);
    if (usedDefault || encodedBytes != length || std::memcmp(encoded, text.data() + offset, length) != 0) {
        return Error("SOURCE_ENCODING_SEQUENCE_INVALID", "GBK映射不能无替代地逐字节往返。", offset, length);
    }
    return Result<SourceScalar>::Success({static_cast<std::uint32_t>(wide), offset, length});
}

} // namespace

Result<SourceScalar> ReadSourceScalar(std::string_view text, std::uint32_t offset, SourceEncoding encoding) {
    if (encoding != SourceEncoding::Utf8 && encoding != SourceEncoding::Gbk) {
        return Error("SOURCE_ENCODING_INVALID", "来源编码枚举无效；不猜测编码。", offset);
    }
    if (text.size() > MaxSourceBytes) return Error("SOURCE_ENCODING_BUDGET", "来源文本超过16MiB标量读取预算。", offset);
    if (offset >= text.size()) return Error("SOURCE_ENCODING_OFFSET", "标量起始偏移不在来源文本范围内。", offset);
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return Result<SourceScalar>::Success({first, offset, 1});
    return encoding == SourceEncoding::Utf8 ? Utf8(text, offset) : Gbk(text, offset);
}

} // namespace War3Map::Text
