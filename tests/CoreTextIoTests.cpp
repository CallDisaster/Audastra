#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Text/TextUtil.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

class ScratchDirectory {
public:
    ScratchDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("forge-core-text-io-" + std::to_string(stamp));
        if (!std::filesystem::create_directory(path_)) throw std::runtime_error("scratch directory collision");
    }
    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

void TestText() {
    using namespace War3Map::Text;
    Require(TrimHorizontal(" \tA\r\n ") == "A\r\n", "horizontal trim preserves CR/LF");
    Require(TrimLineWhitespace(" \t\r\nA\r\n ") == "A", "line trim removes four explicit characters");
    Require(TrimLineWhitespace("\vA\f") == "\vA\f", "line trim preserves VT/FF");
    Require(TrimLineWhitespaceCopy(" \t\r\nA\r\n ") == "A", "line trim copy owns its result");
    Require(TrimCWhitespace("\vA\f") == "A", "C whitespace includes VT/FF");
    Require(TrimCWhitespaceCopy("\vA\f") == "A", "copy trim owns its result");
    Require(TrimAsciiControlSpaceCopy(std::string_view("\0\x1f" "A\x1f", 4)) == "A",
        "control-space trim includes NUL and unit separator");
    Require(TrimLineWhitespace(" \t\r\n").empty(), "all whitespace yields empty view");
    Require(LowerAscii("AZaz09_\xC3\x84") == "azaz09_\xC3\x84", "ASCII lower leaves UTF-8 unchanged");
    Require(UpperAscii("AZaz09_\xC3\xA4") == "AZAZ09_\xC3\xA4", "ASCII upper leaves UTF-8 unchanged");
    Require(LowerCType("A_Z") == "a_z" && UpperCType("a_z") == "A_Z",
        "C locale byte casing retains existing ASCII spellings");
    const auto fields = SplitPlain(",a,,", ',');
    Require(fields.size() == 4 && fields[0].empty() && fields[1] == "a"
        && fields[2].empty() && fields[3].empty(), "plain split keeps empty fields");
    Require(SplitPlain("", ',').size() == 1, "empty text has one empty field");
}

void TestFileBytes() {
    using namespace War3Map::IO;
    ScratchDirectory scratch;
    const auto file = scratch.Path() / std::filesystem::path(u8"样例.bin");
    const std::array bytes{std::byte{0}, std::byte{0xff}, std::byte{0x41}, std::byte{0}};
    Require(WriteFileBytes(file, bytes).Succeeded(), "binary write");
    const auto read = ReadFileBytes(file, bytes.size());
    Require(read.Succeeded() && *read.value == std::vector<std::byte>(bytes.begin(), bytes.end()),
        "binary roundtrip including NUL");
    const auto capped = ReadFileBytes(file, bytes.size() - 1);
    Require(!capped.Succeeded() && capped.diagnostics[0].code == "FILE_SIZE_LIMIT"
        && capped.diagnostics[0].origin->source == file, "capacity diagnostic and path");
    Require(WriteFileBytes(file, {}).Succeeded(), "empty write");
    const auto empty = ReadFileBytes(file, 0);
    Require(empty.Succeeded() && empty.value->empty(), "empty roundtrip");
    const auto missing = ReadFileBytes(scratch.Path() / "missing");
    Require(!missing.Succeeded() && missing.diagnostics[0].origin.has_value(), "missing file diagnostic");
    Require(!ReadFileBytes(scratch.Path()).Succeeded(), "directory is not a file");
    Require(!WriteFileBytes(scratch.Path(), bytes).Succeeded(), "directory cannot be opened for writing");
    Require(PathUtf8(std::filesystem::path(u8"样例.bin")) == "\xE6\xA0\xB7\xE4\xBE\x8B.bin",
        "path UTF-8 encoding");
}

} // namespace

int main() {
    try {
        TestText();
        TestFileBytes();
        std::cout << "Core text and file byte tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
