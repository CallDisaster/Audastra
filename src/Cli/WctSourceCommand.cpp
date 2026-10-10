#include "WctSourceCommand.h"
#include "War3MapCore/Trigger/WctSource.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Hash.h"
#include <charconv>
#include <iostream>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Errors(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message;
        if (diagnostic.origin) std::cerr << " @" << diagnostic.origin->offset;
        std::cerr << '\n';
    }
}
std::optional<std::size_t> Size(std::string_view text) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    return value;
}
std::optional<WctSourceDocument> Load(const std::filesystem::path& path, std::string_view sha) {
    const auto bytes = IO::ReadFileBytes(path, WctSourceLimits{}.maxBytes);
    if (!bytes.Succeeded()) { Errors(bytes.diagnostics); return {}; }
    auto parsed = ReadWctSource(*bytes.value);
    if (!parsed.Succeeded()) { Errors(parsed.diagnostics); return {}; }
    if (parsed.value->SourceSha256() != sha) { std::cerr << "WCT_SOURCE_STALE: expected member SHA differs\n"; return {}; }
    return std::move(*parsed.value);
}
void Block(std::string_view key, const WctSourceBlock& block) {
    std::cout << "block\t" << key << '\t' << block.lengthFieldOffset << '\t' << block.payload.offset << '\t'
        << block.payload.length << '\t' << (block.containsNul ? "true" : "false") << '\t' << (block.endsWithNul ? "true" : "false") << '\n';
}
}
int RunWctSourceInspect(const std::filesystem::path& path, std::string_view sha,
    std::string_view startText, std::string_view limitText) {
    const auto start = Size(startText), limit = Size(limitText);
    if (!start || !limit) { std::cerr << "invalid page arguments\n"; return 1; }
    const auto document = Load(path, sha); if (!document) return 1;
    const auto page = QueryWctSourceBlocks(*document, sha, *start, *limit);
    if (!page.Succeeded()) { Errors(page.diagnostics); return 1; }
    const auto retainedHash = Sha256(document->OriginalBytes());
    if (!retainedHash.Succeeded()) { Errors(retainedHash.diagnostics); return 1; }
    if (*retainedHash.value != sha) { std::cerr << "WCT_SOURCE_RETAINED_BYTES_MISMATCH\n"; return 1; }
    const auto& header = document->Header();
    std::cout << "schema\tforge-wct-source-tsv-v1\nsource_sha256\t" << document->SourceSha256()
        << "\nversion\t" << header.version << "\nsubversion_marker\t"
        << (header.subversionMarker ? std::to_string(*header.subversionMarker) : "none")
        << "\ndeclared_count\t" << (header.declaredBlockCount ? std::to_string(*header.declaredBlockCount) : "absent")
        << "\nblock_count\t" << document->Blocks().size() << "\noriginal_bytes\t" << document->OriginalBytes().size()
        << "\nretained_sha256\t" << *retainedHash.value
        << "\neditable\tfalse\nlanguage\tunknown\nwtg_pairing\tunverified\n";
    if (header.comment) std::cout << "comment\t" << header.comment->offset << '\t' << header.comment->length << '\n';
    if (header.globalCode) Block("global", *header.globalCode);
    for (std::size_t i = 0; i < page.value->blocks.size(); ++i) Block(std::to_string(*start + i), page.value->blocks[i]);
    std::cout << "next_index\t" << (page.value->nextIndex ? std::to_string(*page.value->nextIndex) : "none") << '\n';
    return 0;
}
int RunWctSourceChunk(const std::filesystem::path& path, std::string_view sha,
    std::string_view blockText, std::string_view offsetText, std::string_view limitText) {
    const auto block = blockText == "global" ? std::optional<std::size_t>{} : Size(blockText);
    const auto offset = Size(offsetText), limit = Size(limitText);
    if ((!block && blockText != "global") || !offset || !limit) { std::cerr << "invalid chunk arguments\n"; return 1; }
    const auto document = Load(path, sha); if (!document) return 1;
    const auto chunk = ReadWctSourceChunk(*document, sha, block, *offset, *limit);
    if (!chunk.Succeeded()) { Errors(chunk.diagnostics); return 1; }
    std::cout << "schema\tforge-wct-chunk-tsv-v1\nsource_sha256\t" << chunk.value->sourceSha256
        << "\nblock\t" << blockText << "\npayload_offset\t" << chunk.value->payloadOffset << "\nbytes_hex\t";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : chunk.value->bytes) { const auto v = std::to_integer<unsigned>(byte); std::cout << digits[v >> 4] << digits[v & 15]; }
    std::cout << "\nnext_offset\t" << (chunk.value->nextOffset ? std::to_string(*chunk.value->nextOffset) : "none") << '\n';
    return 0;
}
