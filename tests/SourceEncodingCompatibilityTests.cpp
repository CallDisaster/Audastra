#include "War3MapCore/Text/SourceEncoding.h"
#include "War3MapCore/Text/Utf8Scalar.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace War3Map::Text;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

void SourceCompatibility() {
    struct Valid { std::string bytes; std::uint32_t codepoint; std::uint8_t size; };
    for (const auto& fixture : std::vector<Valid>{{"A", 65, 1}, {std::string("\0", 1), 0, 1}, {"中", 0x4e2d, 3},
        {"😀", 0x1f600, 4}, {std::string("\xef\xbb\xbf", 3), 0xfeff, 3}}) {
        const auto helper = Detail::ReadUtf8Scalar(fixture.bytes, 0);
        const auto actual = ReadSourceScalar(fixture.bytes, 0, SourceEncoding::Utf8);
        Require(helper.Valid() && helper.codepoint == fixture.codepoint && helper.bytes == fixture.size,
            "shared helper changed valid scalar");
        Require(actual.Succeeded() && actual.value && actual.value->offset == 0 && actual.value->codepoint == helper.codepoint
            && actual.value->bytes == helper.bytes, "SourceEncoding delegation changed valid scalar contract");
    }
    struct Invalid { std::string bytes; const char* message; std::uint64_t length; };
    for (const auto& fixture : std::vector<Invalid>{{std::string("\xc0", 1), "UTF-8起始字节无效。", 1},
        {std::string("\xe4\xb8", 2), "UTF-8序列在来源末尾截断。", 2},
        {std::string("\xe4X\xad", 3), "UTF-8续字节无效。", 2},
        {std::string("\xed\xa0\x80", 3), "UTF-8过长编码、代理码点或越界码点无效。", 3}}) {
        const auto actual = ReadSourceScalar("A" + fixture.bytes, 1, SourceEncoding::Utf8);
        Require(!actual.Succeeded() && !actual.value && actual.diagnostics.size() == 1, "bad scalar accepted");
        const auto& diagnostic = actual.diagnostics.front();
        Require(diagnostic.code == "SOURCE_ENCODING_SEQUENCE_INVALID" && diagnostic.message == fixture.message
            && diagnostic.origin && diagnostic.origin->offset == 1 && diagnostic.origin->length == fixture.length,
            "delegation changed exact old error code/message/offset/length");
    }
    Require(ReadSourceScalar("", 0, SourceEncoding::Utf8).diagnostics.front().code == "SOURCE_ENCODING_OFFSET", "offset gate changed");
    Require(ReadSourceScalar("A", 0, static_cast<SourceEncoding>(999)).diagnostics.front().code == "SOURCE_ENCODING_INVALID", "enum gate changed");
    const std::string huge(16u * 1024u * 1024u + 1u, 'A');
    Require(ReadSourceScalar(huge, 0, SourceEncoding::Utf8).diagnostics.front().code == "SOURCE_ENCODING_BUDGET", "16MiB gate changed");
    const auto gbk = ReadSourceScalar(std::string("\xd6\xd0", 2), 0, SourceEncoding::Gbk);
    Require(gbk.Succeeded() && gbk.value && gbk.value->codepoint == 0x4e2d && gbk.value->bytes == 2, "unchanged CP936 path changed");
}

} // namespace

int main() {
    try {
        SourceCompatibility();
        std::cout << "PASS shared UTF8 source compatibility\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL shared UTF8 source compatibility " << error.what() << '\n';
        return 1;
    }
}
