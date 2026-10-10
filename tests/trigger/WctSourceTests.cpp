#include "War3MapCore/Trigger/WctSource.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Trigger;
namespace {
void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
struct Bytes {
    std::vector<std::byte> data;
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
    void Text(std::string_view text) { for (const auto c : text) data.push_back(static_cast<std::byte>(c)); }
    void Block(std::string_view text) { U32(static_cast<std::uint32_t>(text.size())); Text(text); }
};
Bytes Fixture(std::optional<std::uint32_t> marker, std::uint32_t version) {
    Bytes bytes;
    if (marker) bytes.U32(*marker);
    bytes.U32(version);
    if (version == 1) { bytes.Text("comment\r\n"); bytes.data.push_back(std::byte{}); bytes.Block(std::string_view("g\0h", 3)); }
    if (marker != 0x80000004U) bytes.U32(3);
    bytes.Block(""); bytes.Block("abc"); bytes.Block(std::string_view("x\0y\0", 4));
    return bytes;
}
void Reject(const std::vector<std::byte>& bytes, std::string_view code, WctSourceLimits limits = {}) {
    const auto parsed = ReadWctSource(bytes, limits);
    Require(!parsed.Succeeded() && !parsed.value && std::any_of(parsed.diagnostics.begin(), parsed.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; }), "expected exact rejection");
}
}
int main() {
    try {
        for (const auto marker : {std::optional<std::uint32_t>{}, std::optional<std::uint32_t>{0x80000001U}, std::optional<std::uint32_t>{0x80000004U}}) {
            for (std::uint32_t version : {0U, 1U}) {
                auto source = Fixture(marker, version);
                const auto parsed = ReadWctSource(source.data); Require(parsed.Succeeded(), "known profile rejected");
                const auto& doc = *parsed.value;
                Require(doc.Header().version == version && doc.Header().subversionMarker == marker && doc.Blocks().size() == 3, "header or block count");
                Require(doc.Header().declaredBlockCount.has_value() == (marker != 0x80000004U), "count presence");
                Require(doc.Blocks()[0].payload.length == 0 && !doc.Blocks()[0].endsWithNul, "empty block");
                Require(doc.Blocks()[1].payload.length == 3 && !doc.Blocks()[1].containsNul, "not a NUL terminated field");
                Require(doc.Blocks()[2].payload.length == 4 && doc.Blocks()[2].containsNul && doc.Blocks()[2].endsWithNul, "interior NUL truncated");
                Require(std::equal(source.data.begin(), source.data.end(), doc.OriginalBytes().begin(), doc.OriginalBytes().end()), "original bytes differ");
                const auto page = QueryWctSourceBlocks(doc, doc.SourceSha256(), 0, 2);
                Require(page.Succeeded() && page.value->blocks.size() == 2 && page.value->nextIndex == 2, "bounded page");
                const auto chunk = ReadWctSourceChunk(doc, doc.SourceSha256(), 2, 1, 2);
                Require(chunk.Succeeded() && chunk.value->bytes == std::vector<std::byte>{std::byte{}, std::byte{'y'}} && chunk.value->nextOffset == 3, "byte chunk");
                Require(!QueryWctSourceBlocks(doc, std::string(64, '0'), 0).Succeeded(), "stale fingerprint");
                Require(!QueryWctSourceBlocks(doc, doc.SourceSha256(), 0, 129).Succeeded(), "page budget");
                Require(!ReadWctSourceChunk(doc, doc.SourceSha256(), 2, 5).Succeeded(), "chunk beyond EOF");
                Require(!ReadWctSourceChunk(doc, doc.SourceSha256(), 3, 0).Succeeded(), "bad slot");
                const auto global = ReadWctSourceChunk(doc, doc.SourceSha256(), {}, 0);
                Require(global.Succeeded() == (version == 1), "global block presence");
                if (version == 1) Require(global.value->bytes.size() == 3 && global.value->bytes[1] == std::byte{}, "global interior NUL");
                source.data[0] = std::byte{0x55}; Require(doc.OriginalBytes()[0] != std::byte{0x55}, "source alias remained mutable");
            }
        }
        const auto source = Fixture({}, 1).data;
        for (std::size_t size = 0; size < source.size(); ++size)
            Require(!ReadWctSource(std::span(source).first(size)).Succeeded(), "truncated counted profile accepted");
        auto tail = source; tail.push_back(std::byte{}); Reject(tail, "WCT_SOURCE_TRAILING_BYTES");
        Bytes bad; bad.U32(0x80000005U); Reject(bad.data, "WCT_SOURCE_VERSION_UNSUPPORTED");
        bad.data.clear(); bad.U32(0); bad.U32(0xffffffffU); Reject(bad.data, "WCT_SOURCE_NEGATIVE_COUNT");
        bad.data.clear(); bad.U32(0); bad.U32(1); bad.U32(0xffffffffU); Reject(bad.data, "WCT_SOURCE_NEGATIVE_LENGTH");
        auto eof = Fixture(0x80000004U, 0).data; eof.push_back(std::byte{1}); Reject(eof, "WCT_SOURCE_TRUNCATED");
        WctSourceLimits limits; limits.maxBlocks = 2; Reject(source, "WCT_SOURCE_COUNT_BUDGET", limits);
        limits = {}; limits.maxBlockBytes = 2; Reject(source, "WCT_SOURCE_BLOCK_BUDGET", limits);
        limits = {}; limits.maxBytes = source.size() - 1; Reject(source, "WCT_SOURCE_BYTES_BUDGET", limits);
        limits = {}; limits.maxCommentBytes = 1; Reject(source, "WCT_SOURCE_COMMENT_UNTERMINATED", limits);
        limits = {}; ++limits.maxBytes; Reject(source, "WCT_SOURCE_LIMIT_INVALID", limits);
        std::cout << "WCT six profiles, exact byte ranges, counted truncation and bound queries passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
