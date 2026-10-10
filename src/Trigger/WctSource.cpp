#include "War3MapCore/Trigger/WctSource.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <new>

namespace War3Map::Trigger {
struct WctSourceDocument::Storage {
    WctSourceHeader header;
    std::string sha256;
    std::vector<WctSourceBlock> blocks;
    std::vector<std::byte> original;
};
namespace {
struct Rejection { Diagnostic diagnostic; };
[[noreturn]] void Reject(std::string code, std::string message, std::size_t offset) {
    throw Rejection{{DiagnosticSeverity::Error, "Trigger.WctSource", std::move(code), std::move(message),
        OriginSpan{"war3map.wct", offset, 0, {}}}};
}
class Reader {
public:
    Reader(std::span<const std::byte> bytes, const WctSourceLimits& limits) : bytes_(bytes), limits_(limits) {}
    std::size_t Position() const { return offset_; }
    std::size_t Remaining() const { return bytes_.size() - offset_; }
    std::uint32_t U32() {
        if (Remaining() < 4) Reject("WCT_SOURCE_TRUNCATED", "没有完整的 32 位字段。", offset_);
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) result |= std::to_integer<std::uint32_t>(bytes_[offset_++]) << (8 * i);
        return result;
    }
    WctSourceRange Comment() {
        const auto available = bytes_.subspan(offset_, std::min(Remaining(), limits_.maxCommentBytes));
        const auto terminator = std::find(available.begin(), available.end(), std::byte{});
        if (terminator == available.end()) Reject("WCT_SOURCE_COMMENT_UNTERMINATED",
            "全局注释在字节范围/预算内没有 NUL 终止符。", offset_);
        WctSourceRange result{offset_, static_cast<std::size_t>(terminator - available.begin()) + 1};
        offset_ += result.length; return result;
    }
    WctSourceBlock Block() {
        WctSourceBlock result; result.lengthFieldOffset = offset_;
        const auto length = U32();
        if (length > 0x7fffffffU) Reject("WCT_SOURCE_NEGATIVE_LENGTH", "代码块字节长度为负数。", result.lengthFieldOffset);
        if (length > limits_.maxBlockBytes) Reject("WCT_SOURCE_BLOCK_BUDGET", "代码块超过字节预算。", result.lengthFieldOffset);
        if (length > Remaining()) Reject("WCT_SOURCE_TRUNCATED", "代码块声明长度超过剩余成员字节。", result.lengthFieldOffset);
        result.payload = {offset_, length};
        const auto payload = bytes_.subspan(offset_, length);
        result.containsNul = std::find(payload.begin(), payload.end(), std::byte{}) != payload.end();
        result.endsWithNul = !payload.empty() && payload.back() == std::byte{};
        offset_ += length; return result;
    }
private:
    std::span<const std::byte> bytes_;
    const WctSourceLimits& limits_;
    std::size_t offset_ = 0;
};
template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("Trigger.WctSource", std::move(code), std::move(message));
}
}

const WctSourceHeader& WctSourceDocument::Header() const noexcept { return storage_->header; }
const std::string& WctSourceDocument::SourceSha256() const noexcept { return storage_->sha256; }
std::span<const WctSourceBlock> WctSourceDocument::Blocks() const noexcept { return storage_->blocks; }
std::span<const std::byte> WctSourceDocument::OriginalBytes() const noexcept { return storage_->original; }

Result<WctSourceDocument> ReadWctSource(std::span<const std::byte> bytes, const WctSourceLimits& limits) {
    try {
        const WctSourceLimits hard;
        if (limits.maxBytes > hard.maxBytes || limits.maxBlocks > hard.maxBlocks
            || limits.maxBlockBytes > hard.maxBlockBytes || limits.maxCommentBytes > hard.maxCommentBytes)
            return Failure<WctSourceDocument>("WCT_SOURCE_LIMIT_INVALID", "调用方只能收紧读取预算。");
        if (bytes.size() > limits.maxBytes) return Failure<WctSourceDocument>("WCT_SOURCE_BYTES_BUDGET", "成员超过总字节预算。");
        Reader reader(bytes, limits);
        auto result = std::make_shared<WctSourceDocument::Storage>();
        auto& header = result->header;
        auto version = reader.U32();
        if (version == 0x80000001U || version == 0x80000004U) {
            header.subversionMarker = version; version = reader.U32();
        }
        if (version != 0 && version != 1) Reject("WCT_SOURCE_VERSION_UNSUPPORTED", "未识别的 WCT 基础版本或次版本标记。", 0);
        header.version = version;
        if (version == 1) { header.comment = reader.Comment(); header.globalCode = reader.Block(); }
        if (header.subversionMarker != 0x80000004U) {
            const auto countOffset = reader.Position();
            const auto count = reader.U32();
            if (count > 0x7fffffffU) Reject("WCT_SOURCE_NEGATIVE_COUNT", "代码块数量为负数。", countOffset);
            if (count > limits.maxBlocks) Reject("WCT_SOURCE_COUNT_BUDGET", "代码块数量超过预算。", countOffset);
            if (count > reader.Remaining() / 4) Reject("WCT_SOURCE_TRUNCATED", "代码块数量超过最小可用长度字段数。", countOffset);
            header.declaredBlockCount = count;
            result->blocks.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) result->blocks.push_back(reader.Block());
            if (reader.Remaining()) Reject("WCT_SOURCE_TRAILING_BYTES", "声明块数之后仍有未知尾部，不猜测额外代码块。", reader.Position());
        } else {
            // 次版本 04 无总数量字段，每次严格消费一个长度块；空块也必须消耗 4 字节。
            while (reader.Remaining()) {
                if (result->blocks.size() >= limits.maxBlocks) Reject("WCT_SOURCE_COUNT_BUDGET", "代码块数量超过预算。", reader.Position());
                result->blocks.push_back(reader.Block());
            }
        }
        const auto hash = Sha256(bytes);
        if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
        result->sha256 = *hash.value;
        result->original.assign(bytes.begin(), bytes.end());
        return Result<WctSourceDocument>::Success(WctSourceDocument(std::move(result)));
    } catch (const Rejection& rejection) { return {std::nullopt, {rejection.diagnostic}}; }
    catch (const std::bad_alloc&) { return Failure<WctSourceDocument>("WCT_SOURCE_ALLOCATION_FAILED", "读取 WCT 来源时内存不足。"); }
}

Result<WctSourcePage> QueryWctSourceBlocks(const WctSourceDocument& document,
    std::string_view expectedSourceSha256, std::size_t startIndex, std::size_t limit) {
    if (expectedSourceSha256 != document.SourceSha256()) return Failure<WctSourcePage>("WCT_SOURCE_STALE", "代码块查询未绑定当前成员指纹。");
    if (limit == 0 || limit > 128 || startIndex > document.Blocks().size())
        return Failure<WctSourcePage>("WCT_SOURCE_PAGE_RANGE", "代码块页范围无效；单页只能查询 1..128 项。");
    const auto page = document.Blocks().subspan(startIndex, std::min(limit, document.Blocks().size() - startIndex));
    WctSourcePage result{document.SourceSha256(), startIndex, {page.begin(), page.end()}, {}};
    if (startIndex + page.size() < document.Blocks().size()) result.nextIndex = startIndex + page.size();
    return Result<WctSourcePage>::Success(std::move(result));
}

Result<WctSourceChunk> ReadWctSourceChunk(const WctSourceDocument& document,
    std::string_view expectedSourceSha256, std::optional<std::size_t> blockIndex,
    std::size_t payloadOffset, std::size_t limit) {
    if (expectedSourceSha256 != document.SourceSha256()) return Failure<WctSourceChunk>("WCT_SOURCE_STALE", "代码块读取未绑定当前成员指纹。");
    if ((blockIndex && *blockIndex >= document.Blocks().size()) || (!blockIndex && !document.Header().globalCode))
        return Failure<WctSourceChunk>("WCT_SOURCE_BLOCK_MISSING", "此来源没有指定的代码块。");
    const auto& block = blockIndex ? document.Blocks()[*blockIndex] : *document.Header().globalCode;
    if (limit == 0 || limit > 65536 || payloadOffset > block.payload.length)
        return Failure<WctSourceChunk>("WCT_SOURCE_CHUNK_RANGE", "代码块字节范围无效；单次只能读取 1..65536 字节。");
    const auto bytes = document.OriginalBytes().subspan(block.payload.offset + payloadOffset,
        std::min(limit, block.payload.length - payloadOffset));
    WctSourceChunk result{document.SourceSha256(), blockIndex, payloadOffset, {bytes.begin(), bytes.end()}, {}};
    if (payloadOffset + bytes.size() < block.payload.length) result.nextOffset = payloadOffset + bytes.size();
    return Result<WctSourceChunk>::Success(std::move(result));
}
} // namespace War3Map::Trigger
