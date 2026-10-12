// MPQ 只读读取器的合成单元测试（不依赖外部地图文件）。
// 校验点：
//   1. 经典加密表首值 == 0x55C636E2（对齐 StormLib/w3x2lni/HiveWE）。
//   2. HashString 确定性与自研 Encrypt/DecryptBlock 互逆。
//   3. Inflate 对手工构造的 zlib(stored) 流正确解压。
//   4. 端到端：手工构造含单个加密文件的合法 MPQ，经 MpqArchive 读回内容一致。
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Mpq/ArchiveCandidateService.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/SlkCells.h"

#include "Mpq/MpqCrypt.h"
#include "Mpq/MpqDecompress.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using War3Map::Mpq::MpqArchive;
using War3Map::Mpq::MpqCrypt;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempDirectory {
public:
    TempDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("War3MapEditor64-mpq-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void PutU32(std::vector<std::byte>& buf, std::size_t at, std::uint32_t value) {
    std::memcpy(buf.data() + at, &value, 4);
}

// 与 MpqCrypt::DecryptBlock 互逆的测试侧加密（种子更新使用明文，与解密一致）。
void EncryptBlock(std::vector<std::byte>& data, std::size_t at, std::size_t byteLen,
                  std::uint32_t key) {
    const auto& crypt = MpqCrypt::Instance();
    const std::size_t count = byteLen / 4;
    std::uint32_t seed = 0xEEEEEEEE;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t plain = 0;
        std::memcpy(&plain, data.data() + at + i * 4, 4);
        seed += crypt.At(0x400 + (key & 0xFF));
        const std::uint32_t cipher = plain ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = plain + seed + (seed << 5) + 3;
        std::memcpy(data.data() + at + i * 4, &cipher, 4);
    }
}

void TestCryptTable() {
    const auto& crypt = MpqCrypt::Instance();
    Require(crypt.At(0) == 0x55C636E2u,
            "crypt table[0] must equal the canonical 0x55C636E2");
    // 哈希确定性与大小写/斜杠归一。
    Require(crypt.HashString("war3map.w3u", MpqCrypt::HashType::NameA) ==
                crypt.HashString("WAR3MAP.W3U", MpqCrypt::HashType::NameA),
            "HashString must be case-insensitive");
    Require(crypt.HashString("path/to/file", MpqCrypt::HashType::NameA) ==
                crypt.HashString("path\\to\\file", MpqCrypt::HashType::NameA),
            "HashString must treat '/' and '\\\\' identically");
}

void TestEncryptDecryptRoundTrip() {
    const auto& crypt = MpqCrypt::Instance();
    std::vector<std::byte> data(16);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(0x11 * (i + 1));
    }
    const std::vector<std::byte> original = data;
    const std::uint32_t key = crypt.HashString("(block table)", MpqCrypt::HashType::FileKey);
    EncryptBlock(data, 0, data.size(), key);
    Require(data != original, "encryption must alter data");
    crypt.DecryptBlock(data.data(), data.size(), key);
    Require(data == original, "DecryptBlock must invert EncryptBlock");
}

void TestInflateStoredBlock() {
    // zlib 头 0x78 0x01 + DEFLATE stored 块（BFINAL=1,BTYPE=0）+ LEN/NLEN + "Hi"。
    const std::vector<std::byte> stream{
        std::byte{0x78}, std::byte{0x01}, std::byte{0x01}, std::byte{0x02},
        std::byte{0x00}, std::byte{0xFD}, std::byte{0xFF}, std::byte{0x48},
        std::byte{0x69}};
    auto result = War3Map::Mpq::Inflate(stream, 2);
    Require(result.Succeeded() && result.value.has_value(), "inflate stored block must succeed");
    Require(result.value->size() == 2, "inflated size must be 2");
    Require((*result.value)[0] == std::byte{0x48} && (*result.value)[1] == std::byte{0x69},
            "inflated content must be \"Hi\"");
}

void TestDeflateInflateRoundTrip() {
    // 多组数据做 Deflate→Inflate 往返一致性验证。
    struct Case {
        std::string name;
        std::vector<std::byte> data;
    };
    std::vector<Case> cases;

    // 空数据
    cases.push_back({"empty", {}});
    // 小数据
    cases.push_back({"small", {std::byte{0x48}, std::byte{0x69}}});
    // 重复数据（LZ77 应能压缩）
    std::vector<std::byte> repetitive(1024, std::byte{0xAB});
    cases.push_back({"repetitive", repetitive});
    // 伪随机数据
    std::vector<std::byte> randomish(4096);
    std::uint32_t seed = 12345;
    for (auto& b : randomish) {
        seed = seed * 1103515245 + 12345;
        b = static_cast<std::byte>((seed >> 16) & 0xFF);
    }
    cases.push_back({"random", randomish});
    // 混合：前半重复后半随机
    std::vector<std::byte> mixed(2048);
    std::fill(mixed.begin(), mixed.begin() + 1024, std::byte{0x42});
    seed = 99999;
    for (std::size_t i = 1024; i < mixed.size(); ++i) {
        seed = seed * 1103515245 + 12345;
        mixed[i] = static_cast<std::byte>((seed >> 16) & 0xFF);
    }
    cases.push_back({"mixed", mixed});

    for (const auto& testCase : cases) {
        const auto compressed = War3Map::Mpq::Deflate(testCase.data);
        Require(!compressed.empty(),
                "Deflate must produce output for: " + testCase.name);
        auto inflated = War3Map::Mpq::Inflate(
            compressed, static_cast<std::uint32_t>(testCase.data.size()));
        Require(inflated.Succeeded() && inflated.value.has_value(),
                "Inflate must succeed for: " + testCase.name);
        Require(inflated.value->size() == testCase.data.size(),
                "roundtrip size mismatch for: " + testCase.name);
        Require(*inflated.value == testCase.data,
                "roundtrip content mismatch for: " + testCase.name);
    }
}

// 构造仅含单个加密、未压缩文件的最小合法 MPQ format v0 归档。
std::filesystem::path BuildSyntheticMpq(
    const std::filesystem::path& dir,
    const std::string& fileName,
    const std::string& content) {
    const auto& crypt = MpqCrypt::Instance();
    constexpr std::uint32_t kHashCount = 4;
    constexpr std::uint32_t kBlockCount = 1;
    constexpr std::uint32_t kHeaderSize = 32;
    const std::uint32_t dataPos = kHeaderSize;

    // 内容按 4 字节对齐（加密只处理完整 dword）。
    std::vector<std::byte> fileData(content.size());
    std::memcpy(fileData.data(), content.data(), content.size());
    while (fileData.size() % 4 != 0) {
        fileData.push_back(std::byte{0});
    }

    const std::uint32_t hashPos = dataPos + static_cast<std::uint32_t>(fileData.size());
    const std::uint32_t blockPos = hashPos + kHashCount * 16;
    const std::uint32_t total = blockPos + kBlockCount * 16;

    std::vector<std::byte> image(total, std::byte{0});
    // 头
    PutU32(image, 0x00, 0x1A51504D);
    PutU32(image, 0x04, kHeaderSize);
    PutU32(image, 0x08, total);
    // 0x0C: 低 16 位 formatVersion=0，高 16 位 blockSizeShift=3（扇区 512<<3=4096）。
    PutU32(image, 0x0C, 0x00030000u);
    PutU32(image, 0x10, hashPos);
    PutU32(image, 0x14, blockPos);
    PutU32(image, 0x18, kHashCount);
    PutU32(image, 0x1C, kBlockCount);

    // 文件数据（加密：key = HashString(baseName,3)）。
    std::memcpy(image.data() + dataPos, fileData.data(), fileData.size());
    const auto separator = fileName.find_last_of("\\/");
    const std::string_view baseName(fileName.data() +
        (separator == std::string::npos ? 0 : separator + 1));
    const std::uint32_t fileKey = crypt.HashString(baseName, MpqCrypt::HashType::FileKey);
    EncryptBlock(image, dataPos, fileData.size(), fileKey);

    // 哈希表：全部初始化为空桶。
    for (std::uint32_t i = 0; i < kHashCount; ++i) {
        const std::size_t at = hashPos + i * 16;
        PutU32(image, at + 0, 0xFFFFFFFF);
        PutU32(image, at + 4, 0xFFFFFFFF);
        PutU32(image, at + 8, 0xFFFFFFFF); // locale+platform
        PutU32(image, at + 12, 0xFFFFFFFF); // blockIndex=empty
    }
    const std::uint32_t bucket =
        crypt.HashString(fileName, MpqCrypt::HashType::TableOffset) & (kHashCount - 1);
    const std::size_t entry = hashPos + bucket * 16;
    PutU32(image, entry + 0, crypt.HashString(fileName, MpqCrypt::HashType::NameA));
    PutU32(image, entry + 4, crypt.HashString(fileName, MpqCrypt::HashType::NameB));
    PutU32(image, entry + 8, 0);   // locale=0, platform=0
    PutU32(image, entry + 12, 0);  // blockIndex=0
    EncryptBlock(image, hashPos, kHashCount * 16,
                 crypt.HashString("(hash table)", MpqCrypt::HashType::FileKey));

    // 块表：单个加密文件（kFileExists | kFileEncrypted）。
    PutU32(image, blockPos + 0, dataPos);
    PutU32(image, blockPos + 4, static_cast<std::uint32_t>(fileData.size()));
    PutU32(image, blockPos + 8, static_cast<std::uint32_t>(content.size()));
    PutU32(image, blockPos + 12, 0x80000000u | 0x00010000u);
    EncryptBlock(image, blockPos, kBlockCount * 16,
                 crypt.HashString("(block table)", MpqCrypt::HashType::FileKey));

    const std::filesystem::path outPath = dir / "synthetic.w3x";
    std::ofstream out(outPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.data()),
              static_cast<std::streamsize>(image.size()));
    Require(static_cast<bool>(out), "failed to write synthetic MPQ");
    return outPath;
}

void TestSyntheticArchiveRoundTrip() {
    TempDirectory temp;
    const std::string name = "war3map.w3u";
    const std::string content = "UNIT-DATA-123456"; // 16 字节，4 对齐，无尾部明文歧义
    const auto archivePath = BuildSyntheticMpq(temp.Path(), name, content);

    auto opened = MpqArchive::OpenReadOnly(archivePath);
    Require(opened.Succeeded() && opened.value.has_value(), "synthetic MPQ must open");
    MpqArchive& archive = *opened.value;

    Require(archive.HasFile(name), "archive must contain the file");
    Require(archive.HasFile("WAR3MAP.W3U"), "lookup must be case-insensitive");
    Require(!archive.HasFile("missing.txt"), "absent file must not be found");

    auto data = archive.ReadFile(name);
    Require(data.Succeeded() && data.value.has_value(), "reading encrypted file must succeed");
    const std::string got(
        reinterpret_cast<const char*>(data.value->data()), data.value->size());
    Require(got == content,
            "decrypted content mismatch: got \"" + got + "\" expected \"" + content + "\"");
}

void TestWriterRoundTrip() {
    TempDirectory temp;
    const std::filesystem::path outPath = temp.Path() / "written.w3x";

    // 构造测试文件：压缩 + 未压缩 + 空文件。
    std::vector<War3Map::Mpq::MpqBuildFile> files;
    {
        War3Map::Mpq::MpqBuildFile f;
        f.name = "war3map.w3u";
        f.data.resize(8192);
        for (std::size_t i = 0; i < f.data.size(); ++i) {
            f.data[i] = static_cast<std::byte>((i * 7 + 13) & 0xFF);
        }
        f.compress = true;
        files.push_back(std::move(f));
    }
    {
        War3Map::Mpq::MpqBuildFile f;
        f.name = "war3map.wtg";
        f.data.resize(256);
        for (std::size_t i = 0; i < f.data.size(); ++i) {
            f.data[i] = static_cast<std::byte>(0xAA);
        }
        f.compress = true;
        files.push_back(std::move(f));
    }
    {
        War3Map::Mpq::MpqBuildFile f;
        f.name = "empty.dat";
        f.compress = false;
        files.push_back(std::move(f));
    }

    // 带 512 字节 HM3W 前置头。
    std::vector<std::byte> header(512, std::byte{0});
    header[0] = std::byte{'H'};
    header[1] = std::byte{'M'};
    header[2] = std::byte{'3'};
    header[3] = std::byte{'W'};

    auto writeStatus = War3Map::Mpq::WriteMpqArchive(outPath, files, header);
    Require(writeStatus.Succeeded(), "WriteMpqArchive must succeed");

    // 读回验证。
    auto opened = MpqArchive::OpenReadOnly(outPath);
    Require(opened.Succeeded() && opened.value.has_value(),
            "written MPQ must be openable");
    MpqArchive& archive = *opened.value;

    for (const auto& f : files) {
        Require(archive.HasFile(f.name),
                "written archive must contain: " + f.name);
        auto data = archive.ReadFile(f.name);
        Require(data.Succeeded() && data.value.has_value(),
                "reading back must succeed: " + f.name);
        Require(data.value->size() == f.data.size(),
                "size mismatch for: " + f.name);
        Require(*data.value == f.data,
                "content mismatch for: " + f.name);
    }

    // listfile 应包含所有文件名。
    auto listFiles = archive.ListFiles();
    Require(listFiles.size() >= files.size(),
            "listfile must contain at least all written files");
}

std::vector<std::byte> TextBytes(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

std::string HashBytes(const std::vector<std::byte>& bytes) {
    auto hash = War3Map::Sha256(bytes);
    Require(hash.Succeeded() && hash.value.has_value(), "SHA-256 must succeed");
    return *hash.value;
}

std::string HashFile(const std::filesystem::path& path) {
    auto hash = War3Map::Sha256File(path);
    Require(hash.Succeeded() && hash.value.has_value(), "file SHA-256 must succeed");
    return *hash.value;
}

std::string DiagnosticsText(const std::vector<War3Map::Diagnostic>& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics) {
        if (!result.empty()) {
            result += "; ";
        }
        result += diagnostic.code + ":" + diagnostic.message;
    }
    return result;
}

std::vector<std::byte> ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Require(static_cast<bool>(input), "test file must open");
    const auto size = input.tellg();
    Require(size >= 0, "test file size must be valid");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    Require(static_cast<bool>(input), "test file must be fully readable");
    return bytes;
}

void WriteAll(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(output), "synthetic mutation must be written");
}

std::uint32_t GetU32(const std::vector<std::byte>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

struct MutableMpqTables {
    std::size_t archiveBase = 0;
    std::size_t hashAbsolute = 0;
    std::size_t blockAbsolute = 0;
    std::uint32_t hashCount = 0;
    std::uint32_t blockCount = 0;
    std::vector<std::byte> hash;
    std::vector<std::byte> block;
};

MutableMpqTables ReadMutableTables(const std::vector<std::byte>& image) {
    constexpr std::uint32_t kMpqSignature = 0x1A51504D;
    std::size_t archiveBase = image.size();
    for (std::size_t offset = 0; offset + 32 <= image.size(); offset += 512) {
        if (GetU32(image, offset) == kMpqSignature) {
            archiveBase = offset;
            break;
        }
    }
    Require(archiveBase != image.size(), "writer fixture must contain an aligned MPQ header");

    MutableMpqTables tables;
    tables.archiveBase = archiveBase;
    tables.hashCount = GetU32(image, archiveBase + 0x18);
    tables.blockCount = GetU32(image, archiveBase + 0x1C);
    tables.hashAbsolute = archiveBase + GetU32(image, archiveBase + 0x10);
    tables.blockAbsolute = archiveBase + GetU32(image, archiveBase + 0x14);
    const std::size_t hashBytes = static_cast<std::size_t>(tables.hashCount) * 16;
    const std::size_t blockBytes = static_cast<std::size_t>(tables.blockCount) * 16;
    Require(tables.hashAbsolute <= image.size() &&
                hashBytes <= image.size() - tables.hashAbsolute,
            "encrypted hash table must be bounded by fixture bytes");
    Require(tables.blockAbsolute <= image.size() &&
                blockBytes <= image.size() - tables.blockAbsolute,
            "encrypted block table must be bounded by fixture bytes");
    tables.hash.assign(
        image.begin() + tables.hashAbsolute,
        image.begin() + tables.hashAbsolute + hashBytes);
    tables.block.assign(
        image.begin() + tables.blockAbsolute,
        image.begin() + tables.blockAbsolute + blockBytes);
    const auto& crypt = MpqCrypt::Instance();
    crypt.DecryptBlock(
        tables.hash.data(), tables.hash.size(),
        crypt.HashString("(hash table)", MpqCrypt::HashType::FileKey));
    crypt.DecryptBlock(
        tables.block.data(), tables.block.size(),
        crypt.HashString("(block table)", MpqCrypt::HashType::FileKey));
    return tables;
}

void WriteMutableTables(std::vector<std::byte>& image, const MutableMpqTables& tables) {
    auto hash = tables.hash;
    auto block = tables.block;
    const auto& crypt = MpqCrypt::Instance();
    crypt.EncryptBlock(
        hash.data(), hash.size(),
        crypt.HashString("(hash table)", MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(
        block.data(), block.size(),
        crypt.HashString("(block table)", MpqCrypt::HashType::FileKey));
    std::memcpy(image.data() + tables.hashAbsolute, hash.data(), hash.size());
    std::memcpy(image.data() + tables.blockAbsolute, block.data(), block.size());
}

std::size_t FindHashEntryForBlock(
    const MutableMpqTables& tables, std::uint32_t blockIndex) {
    for (std::uint32_t i = 0; i < tables.hashCount; ++i) {
        if (GetU32(tables.hash, static_cast<std::size_t>(i) * 16 + 12) == blockIndex) {
            return i;
        }
    }
    throw std::runtime_error("expected live hash entry was not found");
}

bool IsEmptyHashEntry(const MutableMpqTables& tables, std::size_t index) {
    return GetU32(tables.hash, index * 16 + 12) == 0xFFFFFFFFu;
}

bool HasDiagnostic(
    const std::vector<War3Map::Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

std::size_t CountSubstring(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string_view::npos;
         at += needle.size()) {
        ++count;
    }
    return count;
}

class JsonSyntaxParser {
public:
    explicit JsonSyntaxParser(std::string_view text) : text_(text) {}

    bool ParseDocument() {
        SkipWhitespace();
        return ParseValue(0) && (SkipWhitespace(), offset_ == text_.size());
    }

private:
    void SkipWhitespace() {
        while (offset_ < text_.size() &&
               (text_[offset_] == ' ' || text_[offset_] == '\t' ||
                text_[offset_] == '\r' || text_[offset_] == '\n')) {
            ++offset_;
        }
    }

    bool Consume(char expected) {
        if (offset_ >= text_.size() || text_[offset_] != expected) {
            return false;
        }
        ++offset_;
        return true;
    }

    bool ParseValue(std::size_t depth) {
        if (depth > 64) {
            return false;
        }
        SkipWhitespace();
        if (offset_ >= text_.size()) {
            return false;
        }
        switch (text_[offset_]) {
        case '{': return ParseObject(depth + 1);
        case '[': return ParseArray(depth + 1);
        case '"': return ParseString();
        case 't': return ParseLiteral("true");
        case 'f': return ParseLiteral("false");
        case 'n': return ParseLiteral("null");
        default: return ParseNumber();
        }
    }

    bool ParseObject(std::size_t depth) {
        ++offset_;
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            if (!ParseString()) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':') || !ParseValue(depth)) {
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseArray(std::size_t depth) {
        ++offset_;
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            if (!ParseValue(depth)) {
                return false;
            }
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseString() {
        if (!Consume('"')) {
            return false;
        }
        while (offset_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[offset_++]);
            if (c == '"') {
                return true;
            }
            if (c < 0x20) {
                return false;
            }
            if (c != '\\') {
                continue;
            }
            if (offset_ >= text_.size()) {
                return false;
            }
            const char escape = text_[offset_++];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' ||
                escape == 'f' || escape == 'n' || escape == 'r' || escape == 't') {
                continue;
            }
            if (escape != 'u' || offset_ + 4 > text_.size()) {
                return false;
            }
            for (std::size_t i = 0; i < 4; ++i) {
                if (!std::isxdigit(static_cast<unsigned char>(text_[offset_ + i]))) {
                    return false;
                }
            }
            offset_ += 4;
        }
        return false;
    }

    bool ParseLiteral(std::string_view literal) {
        if (text_.substr(offset_, literal.size()) != literal) {
            return false;
        }
        offset_ += literal.size();
        return true;
    }

    bool ParseNumber() {
        const std::size_t start = offset_;
        if (offset_ < text_.size() && text_[offset_] == '-') {
            ++offset_;
        }
        if (offset_ >= text_.size()) {
            return false;
        }
        if (text_[offset_] == '0') {
            ++offset_;
        } else if (text_[offset_] >= '1' && text_[offset_] <= '9') {
            while (offset_ < text_.size() &&
                   text_[offset_] >= '0' && text_[offset_] <= '9') {
                ++offset_;
            }
        } else {
            return false;
        }
        if (offset_ < text_.size() && text_[offset_] == '.') {
            ++offset_;
            const std::size_t digits = offset_;
            while (offset_ < text_.size() &&
                   text_[offset_] >= '0' && text_[offset_] <= '9') {
                ++offset_;
            }
            if (digits == offset_) {
                return false;
            }
        }
        if (offset_ < text_.size() &&
            (text_[offset_] == 'e' || text_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < text_.size() &&
                (text_[offset_] == '+' || text_[offset_] == '-')) {
                ++offset_;
            }
            const std::size_t digits = offset_;
            while (offset_ < text_.size() &&
                   text_[offset_] >= '0' && text_[offset_] <= '9') {
                ++offset_;
            }
            if (digits == offset_) {
                return false;
            }
        }
        return offset_ > start;
    }

    std::string_view text_;
    std::size_t offset_ = 0;
};

std::string NameBytesHex(std::string_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char c : value) {
        result.push_back(kHex[(c >> 4) & 0x0F]);
        result.push_back(kHex[c & 0x0F]);
    }
    return result;
}

void RequireManifestMember(
    std::string_view manifest,
    std::string_view canonicalName,
    std::string_view originalName,
    std::string_view status,
    const std::vector<std::byte>& source,
    const std::vector<std::byte>& candidate) {
    std::string escapedCanonical(canonicalName);
    std::size_t slash = 0;
    while ((slash = escapedCanonical.find('\\', slash)) != std::string::npos) {
        escapedCanonical.insert(slash, 1, '\\');
        slash += 2;
    }
    const std::string expected =
        "{\"canonicalName\": \"" + escapedCanonical +
        "\", \"nameBytesHex\": \"" + NameBytesHex(originalName) +
        "\", \"status\": \"" + std::string(status) +
        "\", \"sourceSha256\": \"" + HashBytes(source) +
        "\", \"candidateSha256\": \"" + HashBytes(candidate) + "\"}";
    Require(manifest.find(expected) != std::string_view::npos,
            "manifest member entry must bind name bytes, status, and both exact SHA values: " +
                std::string(canonicalName));
}

struct CandidateFixture {
    std::filesystem::path source;
    std::filesystem::path output;
    std::filesystem::path manifest;
    std::vector<std::byte> originalWtg;
    std::vector<std::byte> originalWct;
    std::vector<std::byte> replacementWtg;
    std::vector<std::byte> replacementWct;
    std::vector<std::byte> prefix;
};

CandidateFixture BuildCandidateFixture(const std::filesystem::path& root) {
    const auto sourceDir = root / "source";
    const auto candidateDir = root / "candidate";
    std::filesystem::create_directories(sourceDir);
    std::filesystem::create_directories(candidateDir);
    CandidateFixture fixture;
    fixture.source = sourceDir / "source.w3x";
    fixture.output = candidateDir / "candidate.w3x";
    fixture.manifest = std::filesystem::path(fixture.output.wstring() + L".manifest.json");
    fixture.originalWtg = TextBytes("synthetic-wtg-original");
    fixture.originalWct = TextBytes("synthetic-wct-original");
    fixture.replacementWtg = TextBytes("synthetic-wtg-replacement");
    fixture.replacementWct = TextBytes("synthetic-wct-replacement");
    fixture.prefix.resize(1024);
    for (std::size_t i = 0; i < fixture.prefix.size(); ++i) {
        fixture.prefix[i] = static_cast<std::byte>((i * 17 + 3) & 0xFF);
    }
    std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"war3map.wtg", fixture.originalWtg, true},
        {"war3map.wct", fixture.originalWct, true},
        {"scripts\\keep.bin", TextBytes("unchanged-member"), false}};
    auto status = War3Map::Mpq::WriteMpqArchive(fixture.source, files, fixture.prefix);
    Require(status.Succeeded(), "synthetic candidate source must be written");
    return fixture;
}

War3Map::Mpq::ArchiveCandidateRequest MakeCandidateRequest(
    const CandidateFixture& fixture,
    std::uint64_t transactionId = 17) {
    War3Map::Mpq::ArchiveCandidateRequest request;
    request.sourceArchivePath = fixture.source;
    request.expectedSourceSha256 = HashFile(fixture.source);
    request.uiCatalogSha256 = std::string(64, 'a');
    request.transactionId = transactionId;
    request.outputArchivePath = fixture.output;
    request.manifestPath = fixture.manifest;
    request.wtg.data = fixture.replacementWtg;
    request.wtg.expectedOriginalSha256 = HashBytes(fixture.originalWtg);
    request.wct.data = fixture.replacementWct;
    request.wct.expectedOriginalSha256 = HashBytes(fixture.originalWct);
    request.finalGuard = [] { return War3Map::Status::Ok(); };
    return request;
}

void RequireCandidateTempsAbsent(
    const War3Map::Mpq::ArchiveCandidateRequest& request,
    std::string_view context) {
    Require(!std::filesystem::exists(War3Map::Mpq::ArchiveCandidateTempPath(request)),
            std::string(context) + ": archive temp must be removed");
    Require(!std::filesystem::exists(War3Map::Mpq::ArchiveCandidateManifestTempPath(request)),
            std::string(context) + ": manifest temp must be removed");
}

template <typename Mutator>
void RequireMutatedCandidateRejected(
    const std::filesystem::path& root,
    std::uint64_t transactionId,
    std::string_view expectedDiagnostic,
    std::string_view context,
    Mutator&& mutate) {
    const auto fixture = BuildCandidateFixture(root);
    auto image = ReadAll(fixture.source);
    auto tables = ReadMutableTables(image);
    mutate(image, tables);
    WriteMutableTables(image, tables);
    WriteAll(fixture.source, image);
    const auto request = MakeCandidateRequest(fixture, transactionId);
    const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
    Require(!result.Succeeded(), std::string(context) + ": mutation must fail closed");
    Require(HasDiagnostic(result.diagnostics, expectedDiagnostic),
            std::string(context) + ": expected diagnostic " +
                std::string(expectedDiagnostic) + ", got " + DiagnosticsText(result.diagnostics));
    Require(!std::filesystem::exists(fixture.output),
            std::string(context) + ": rejected archive must not publish");
    Require(!std::filesystem::exists(fixture.manifest),
            std::string(context) + ": rejected manifest must not publish");
    RequireCandidateTempsAbsent(request, context);
}

void TestArchiveCandidateSuccess() {
    TempDirectory temp;
    const auto fixture = BuildCandidateFixture(temp.Path());
    const auto sourceBefore = ReadAll(fixture.source);
    auto result = War3Map::Mpq::ExportArchiveCandidate(MakeCandidateRequest(fixture));
    Require(result.Succeeded() && result.value.has_value(),
            "strict synthetic archive candidate must export successfully: " +
                DiagnosticsText(result.diagnostics));
    Require(ReadAll(fixture.source) == sourceBefore, "source archive bytes must remain unchanged");
    Require(std::filesystem::exists(result.value->outputArchivePath),
            "published canonical candidate path must exist");
    Require(result.value->candidateSha256 == HashFile(result.value->outputArchivePath),
            "result candidate SHA must bind the published archive");
    Require(result.value->wtgSha256 == HashBytes(fixture.replacementWtg) &&
                result.value->wctSha256 == HashBytes(fixture.replacementWct),
            "result trigger SHA values must bind exact replacement bytes");

    auto opened = MpqArchive::OpenReadOnly(result.value->outputArchivePath);
    const std::string openDiagnostic = opened.diagnostics.empty()
        ? std::string{} : opened.diagnostics.front().code + ":" + opened.diagnostics.front().message;
    Require(opened.Succeeded() && opened.value.has_value(),
            "candidate must reopen: " + openDiagnostic);
    auto prefix = opened.value->ReadArchivePrefix();
    Require(prefix.Succeeded() && prefix.value == fixture.prefix,
            "candidate must preserve the exact non-512-sized-assumption prefix");
    auto wtg = opened.value->ReadFile("war3map.wtg");
    auto wct = opened.value->ReadFile("war3map.wct");
    auto keep = opened.value->ReadFile("scripts\\keep.bin");
    Require(wtg.Succeeded() && wtg.value == fixture.replacementWtg,
            "WTG replacement must be exact");
    Require(wct.Succeeded() && wct.value == fixture.replacementWct,
            "WCT replacement must be exact");
    Require(keep.Succeeded() && keep.value == TextBytes("unchanged-member"),
            "non-trigger member must remain byte-identical");
    const auto manifest = ReadAll(result.value->manifestPath);
    const std::string manifestText(
        reinterpret_cast<const char*>(manifest.data()), manifest.size());
    Require(JsonSyntaxParser(manifestText).ParseDocument(),
            "candidate sidecar must be a complete syntactically valid JSON document");
    Require(manifestText.find("\"candidateKind\": \"mpq-side-write\"") != std::string::npos,
            "manifest must identify side-write candidate");
    Require(manifestText.find("\"distributionStatus\": \"blocked\"") != std::string::npos,
            "manifest must block distribution");
    Require(manifestText.find("\"deploymentAuthorized\": false") != std::string::npos &&
                manifestText.find("\"mapArchiveIntegrationAuthorized\": false") !=
                    std::string::npos &&
                manifestText.find("\"runnableMapClaimed\": false") != std::string::npos &&
                manifestText.find("\"warcraftExecuted\": false") != std::string::npos,
            "manifest must explicitly deny deployment, integration, runnable-map, and runtime claims");
    Require(manifestText.find("\"memberCount\": 4") != std::string::npos &&
                CountSubstring(manifestText, "\"canonicalName\"") == 4 &&
                CountSubstring(manifestText, "\"status\": \"replaced\"") == 2 &&
                CountSubstring(manifestText, "\"status\": \"unchanged\"") == 2,
            "manifest must contain the complete four-member change inventory");
    Require(manifestText.find("\"canonicalName\": \"WAR3MAP.WTG\"") != std::string::npos &&
                manifestText.find("\"canonicalName\": \"WAR3MAP.WCT\"") != std::string::npos &&
                manifestText.find("\"canonicalName\": \"SCRIPTS\\\\KEEP.BIN\"") !=
                    std::string::npos &&
                manifestText.find("\"canonicalName\": \"(LISTFILE)\"") != std::string::npos,
            "manifest must name every source member canonically");
    Require(manifestText.find(result.value->candidateSha256) != std::string::npos &&
                manifestText.find(HashFile(fixture.source)) != std::string::npos &&
                manifestText.find(std::string(64, 'a')) != std::string::npos,
            "manifest must bind candidate, source, and Catalog SHA-256 values");
    const auto keepBytes = TextBytes("unchanged-member");
    const auto listfileBytes =
        TextBytes("war3map.wtg\r\nwar3map.wct\r\nscripts\\keep.bin\r\n");
    RequireManifestMember(
        manifestText, "WAR3MAP.WTG", "war3map.wtg", "replaced",
        fixture.originalWtg, fixture.replacementWtg);
    RequireManifestMember(
        manifestText, "WAR3MAP.WCT", "war3map.wct", "replaced",
        fixture.originalWct, fixture.replacementWct);
    RequireManifestMember(
        manifestText, "SCRIPTS\\KEEP.BIN", "scripts\\keep.bin", "unchanged",
        keepBytes, keepBytes);
    RequireManifestMember(
        manifestText, "(LISTFILE)", "(listfile)", "unchanged",
        listfileBytes, listfileBytes);
    const std::string expectedReplacements =
        "\"war3map.wtg\": {\"originalSha256\": \"" + HashBytes(fixture.originalWtg) +
        "\", \"candidateSha256\": \"" + HashBytes(fixture.replacementWtg) +
        "\"},\n    \"war3map.wct\": {\"originalSha256\": \"" +
        HashBytes(fixture.originalWct) + "\", \"candidateSha256\": \"" +
        HashBytes(fixture.replacementWct) + "\"}";
    Require(manifestText.find(expectedReplacements) != std::string::npos,
            "replacements object must cross-bind the same WTG/WCT source and candidate hashes");
    RequireCandidateTempsAbsent(MakeCandidateRequest(fixture), "successful publish");
}

void TestArchiveCandidatePreconditions() {
    TempDirectory temp;
    auto fixture = BuildCandidateFixture(temp.Path());
    auto badSha = MakeCandidateRequest(fixture);
    badSha.expectedSourceSha256 = std::string(64, '0');
    Require(!War3Map::Mpq::ExportArchiveCandidate(badSha).Succeeded(),
            "source SHA mismatch must fail closed");

    auto missingGuard = MakeCandidateRequest(fixture);
    missingGuard.finalGuard = {};
    Require(!War3Map::Mpq::ExportArchiveCandidate(missingGuard).Succeeded(),
            "empty final Catalog guard must fail closed");

    auto badTriggerBinding = MakeCandidateRequest(fixture);
    badTriggerBinding.wtg.expectedOriginalSha256 = std::string(64, '0');
    Require(!War3Map::Mpq::ExportArchiveCandidate(badTriggerBinding).Succeeded(),
            "WTG transaction baseline mismatch must fail closed");

    auto nonIsolated = MakeCandidateRequest(fixture);
    nonIsolated.outputArchivePath = fixture.source.parent_path() / "nested-candidate.w3x";
    nonIsolated.manifestPath =
        std::filesystem::path(nonIsolated.outputArchivePath.wstring() + L".manifest.json");
    Require(!War3Map::Mpq::ExportArchiveCandidate(nonIsolated).Succeeded(),
            "candidate tree overlapping the source tree must fail closed");

    const auto sentinel = TextBytes("do-not-overwrite");
    WriteAll(fixture.output, sentinel);
    Require(!War3Map::Mpq::ExportArchiveCandidate(MakeCandidateRequest(fixture, 18)).Succeeded(),
            "existing output must fail closed");
    Require(ReadAll(fixture.output) == sentinel, "existing output must not be truncated");
    std::filesystem::remove(fixture.output);

    WriteAll(fixture.manifest, sentinel);
    Require(!War3Map::Mpq::ExportArchiveCandidate(MakeCandidateRequest(fixture, 19)).Succeeded(),
            "existing sidecar must fail closed");
    Require(ReadAll(fixture.manifest) == sentinel, "existing sidecar must not be truncated");
    std::filesystem::remove(fixture.manifest);

    auto tempRequest = MakeCandidateRequest(fixture, 20);
    const auto archiveTemp = War3Map::Mpq::ArchiveCandidateTempPath(tempRequest);
    WriteAll(archiveTemp, sentinel);
    Require(!War3Map::Mpq::ExportArchiveCandidate(tempRequest).Succeeded(),
            "existing archive temp must fail closed");
    Require(ReadAll(archiveTemp) == sentinel, "existing archive temp must not be removed");

    std::filesystem::remove(archiveTemp);
    const auto manifestTemp = War3Map::Mpq::ArchiveCandidateManifestTempPath(tempRequest);
    WriteAll(manifestTemp, sentinel);
    Require(!War3Map::Mpq::ExportArchiveCandidate(tempRequest).Succeeded(),
            "existing manifest temp must fail closed");
    Require(ReadAll(manifestTemp) == sentinel, "existing manifest temp must not be removed");
}

void TestArchiveCandidatePublishRaces() {
    TempDirectory temp;
    const auto sentinel = TextBytes("external-sentinel-must-survive");

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "guard-reject");
        auto request = MakeCandidateRequest(fixture, 31);
        request.finalGuard = [] {
            return War3Map::Status::Failure(
                "Test", "CATALOG_GUARD_REJECTED", "synthetic final guard rejected");
        };
        const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
        Require(!result.Succeeded() && HasDiagnostic(result.diagnostics, "CATALOG_GUARD_REJECTED"),
                "final Catalog guard rejection must propagate");
        Require(!std::filesystem::exists(fixture.output) &&
                    !std::filesystem::exists(fixture.manifest),
                "guard rejection must not publish either artifact");
        RequireCandidateTempsAbsent(request, "guard rejection");
    }

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "archive-race");
        auto request = MakeCandidateRequest(fixture, 32);
        request.finalGuard = [path = fixture.output, sentinel] {
            WriteAll(path, sentinel);
            return War3Map::Status::Ok();
        };
        const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
        Require(!result.Succeeded() && HasDiagnostic(result.diagnostics, "PUBLISH_NO_REPLACE_FAILED"),
                "archive target race must fail no-replace publication");
        Require(ReadAll(fixture.output) == sentinel,
                "archive target race must preserve the external file identity and bytes");
        Require(!std::filesystem::exists(fixture.manifest),
                "archive target race must roll back the service-owned published sidecar");
        RequireCandidateTempsAbsent(request, "archive target race");
    }

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "manifest-race");
        auto request = MakeCandidateRequest(fixture, 33);
        request.finalGuard = [path = fixture.manifest, sentinel] {
            WriteAll(path, sentinel);
            return War3Map::Status::Ok();
        };
        const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
        Require(!result.Succeeded() && HasDiagnostic(result.diagnostics, "PUBLISH_NO_REPLACE_FAILED"),
                "manifest target race must fail no-replace publication");
        Require(ReadAll(fixture.manifest) == sentinel,
                "manifest target race must preserve the external file identity and bytes");
        Require(!std::filesystem::exists(fixture.output),
                "manifest target race must not publish the archive");
        RequireCandidateTempsAbsent(request, "manifest target race");
    }

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "source-lease");
        auto request = MakeCandidateRequest(fixture, 34);
        bool sourceWriteDenied = false;
        request.finalGuard = [path = fixture.source, &sourceWriteDenied] {
            HANDLE write = CreateFileW(
                path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            sourceWriteDenied = write == INVALID_HANDLE_VALUE;
            if (write != INVALID_HANDLE_VALUE) {
                CloseHandle(write);
                return War3Map::Status::Failure(
                    "Test", "SOURCE_LEASE_BROKEN", "source unexpectedly accepted a writer");
            }
            return War3Map::Status::Ok();
        };
        const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
        Require(result.Succeeded() && sourceWriteDenied,
                "source must remain write-denied through the final guard");
    }

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "directory-lease");
        const auto movedDirectory =
            fixture.output.parent_path().parent_path() / "candidate-retargeted";
        auto request = MakeCandidateRequest(fixture, 35);
        bool parentRenameDenied = false;
        request.finalGuard = [directory = fixture.output.parent_path(), movedDirectory,
                              &parentRenameDenied] {
            const BOOL moved = MoveFileExW(
                directory.c_str(), movedDirectory.c_str(), MOVEFILE_WRITE_THROUGH);
            parentRenameDenied = moved == FALSE;
            if (moved != FALSE) {
                return War3Map::Status::Failure(
                    "Test", "TARGET_DIRECTORY_LEASE_BROKEN",
                    "candidate parent unexpectedly accepted a rename");
            }
            return War3Map::Status::Ok();
        };
        const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
        Require(result.Succeeded() && parentRenameDenied,
                "candidate parent must remain rename-denied through the commit point");
        Require(std::filesystem::exists(fixture.output) &&
                    std::filesystem::exists(fixture.manifest) &&
                    !std::filesystem::exists(movedDirectory),
                "directory lease must keep both artifacts in the originally bound directory");
    }

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "ancestor-lease");
        const auto boundTree = fixture.output.parent_path().parent_path();
        const auto movedTree = boundTree.parent_path() / "ancestor-retargeted";
        auto request = MakeCandidateRequest(fixture, 36);
        bool ancestorRenameDenied = false;
        request.finalGuard = [boundTree, movedTree, &ancestorRenameDenied] {
            const BOOL moved = MoveFileExW(
                boundTree.c_str(), movedTree.c_str(), MOVEFILE_WRITE_THROUGH);
            ancestorRenameDenied = moved == FALSE;
            if (moved != FALSE) {
                return War3Map::Status::Failure(
                    "Test", "TARGET_ANCESTOR_LEASE_BROKEN",
                    "candidate ancestor unexpectedly accepted a rename");
            }
            return War3Map::Status::Ok();
        };
        const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
        Require(result.Succeeded() && ancestorRenameDenied,
                "every renamable candidate ancestor must remain locked through commit");
        Require(std::filesystem::exists(fixture.output) &&
                    std::filesystem::exists(fixture.manifest) &&
                    !std::filesystem::exists(movedTree),
                "ancestor lease chain must keep both artifacts under the bound path");
    }
}

void TestWriterFailClosedContracts() {
    TempDirectory temp;
    std::vector<War3Map::Mpq::MpqBuildFile> duplicate{
        {"same/path.bin", TextBytes("a"), false},
        {"SAME\\PATH.BIN", TextBytes("b"), false}};
    Require(!War3Map::Mpq::WriteMpqArchive(temp.Path() / "duplicate.mpq", duplicate).Succeeded(),
            "case/slash duplicate member must be rejected");
    std::vector<War3Map::Mpq::MpqBuildFile> unsafe{
        {"../escape.bin", TextBytes("x"), false}};
    Require(!War3Map::Mpq::WriteMpqArchive(temp.Path() / "unsafe.mpq", unsafe).Succeeded(),
            "unsafe member path must be rejected");
    std::vector<War3Map::Mpq::MpqBuildFile> wildcard{
        {"unsafe*.bin", TextBytes("x"), false}};
    Require(!War3Map::Mpq::WriteMpqArchive(temp.Path() / "wildcard.mpq", wildcard).Succeeded(),
            "Windows wildcard member name must be rejected");
    std::vector<War3Map::Mpq::MpqBuildFile> listfile{
        {"(listfile)", TextBytes("x"), false}};
    Require(!War3Map::Mpq::WriteMpqArchive(temp.Path() / "listfile.mpq", listfile).Succeeded(),
            "caller-supplied listfile must be rejected");
    std::vector<std::byte> badPrefix(513);
    Require(!War3Map::Mpq::WriteMpqArchive(
        temp.Path() / "prefix.mpq", unsafe, badPrefix).Succeeded(),
        "non-512-aligned prefix must be rejected");
    const auto existing = temp.Path() / "existing.mpq";
    const auto sentinel = TextBytes("existing");
    WriteAll(existing, sentinel);
    std::vector<War3Map::Mpq::MpqBuildFile> valid{
        {"ok.bin", TextBytes("ok"), false}};
    Require(!War3Map::Mpq::WriteMpqArchive(existing, valid).Succeeded(),
            "writer must not truncate existing output");
    Require(ReadAll(existing) == sentinel, "writer must preserve existing output bytes");
}

void TestStrictMissingListfileAndBounds() {
    TempDirectory temp;
    const auto noListfile = BuildSyntheticMpq(temp.Path(), "war3map.wtg", "0123456789ABCDEF");
    auto opened = MpqArchive::OpenReadOnly(noListfile);
    Require(opened.Succeeded() && opened.value.has_value(), "synthetic no-listfile archive opens loosely");
    Require(!opened.value->ListFilesStrict().Succeeded(), "strict inventory must reject missing listfile");

    const auto fixture = BuildCandidateFixture(temp.Path() / "bounded");
    auto bytes = ReadAll(fixture.source);
    PutU32(bytes, fixture.prefix.size() + 0x08,
           (std::numeric_limits<std::uint32_t>::max)());
    WriteAll(fixture.source, bytes);
    Require(!MpqArchive::OpenReadOnly(fixture.source).Succeeded(),
            "declared archiveSize beyond EOF must be rejected");
}

void TestStrictEncryptedTableMutations() {
    TempDirectory temp;

    RequireMutatedCandidateRejected(
        temp.Path() / "balanced-alias", 41, "BLOCK_NAME_DUPLICATE",
        "balanced alias plus orphan block",
        [](auto&, MutableMpqTables& tables) {
            const std::size_t wct = FindHashEntryForBlock(tables, 1);
            PutU32(tables.hash, wct * 16 + 12, 0);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "duplicate-name-hash", 42, "LISTFILE_HASH_AMBIGUOUS",
        "duplicate name hash pair",
        [](auto&, MutableMpqTables& tables) {
            const std::size_t first = FindHashEntryForBlock(tables, 0);
            const std::size_t second = FindHashEntryForBlock(tables, 1);
            PutU32(tables.hash, second * 16, GetU32(tables.hash, first * 16));
            PutU32(tables.hash, second * 16 + 4, GetU32(tables.hash, first * 16 + 4));
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "locale", 43, "HASH_ENTRY_UNSUPPORTED",
        "non-neutral locale hash entry",
        [](auto&, MutableMpqTables& tables) {
            const std::size_t keep = FindHashEntryForBlock(tables, 2);
            PutU32(tables.hash, keep * 16 + 8, 1);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "invalid-block-index", 44, "HASH_ENTRY_UNSUPPORTED",
        "hash entry with invalid block index",
        [](auto&, MutableMpqTables& tables) {
            const std::size_t keep = FindHashEntryForBlock(tables, 2);
            PutU32(tables.hash, keep * 16 + 12, tables.blockCount);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "unreachable-hash", 45, "HASH_ENTRY_UNREACHABLE",
        "hash entry moved outside its probe chain",
        [](auto&, MutableMpqTables& tables) {
            const std::size_t original = FindHashEntryForBlock(tables, 2);
            std::size_t destination = tables.hashCount;
            std::size_t emptySeen = 0;
            for (std::size_t distance = 1; distance < tables.hashCount; ++distance) {
                const std::size_t candidate = (original + distance) % tables.hashCount;
                if (IsEmptyHashEntry(tables, candidate)) {
                    ++emptySeen;
                    if (emptySeen == 2) {
                        destination = candidate;
                        break;
                    }
                }
            }
            Require(destination != tables.hashCount,
                    "fixture hash table must have two empty relocation buckets");
            std::copy_n(
                tables.hash.begin() + original * 16, 16,
                tables.hash.begin() + destination * 16);
            std::fill_n(tables.hash.begin() + original * 16, 16, std::byte{0xFF});
            PutU32(tables.hash, original * 16 + 12, 0xFFFFFFFEu);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "empty-hides-data", 46, "EMPTY_FILE_SIZE_INVALID",
        "empty file hiding non-empty archive bytes",
        [](auto&, MutableMpqTables& tables) {
            PutU32(tables.block, 2 * 16 + 8, 0);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "uint32-filesize", 47, "MEMBER_DECODED_BUDGET_EXCEEDED",
        "uint32 maximum stored file size",
        [](auto&, MutableMpqTables& tables) {
            PutU32(tables.block, 2 * 16 + 8,
                   (std::numeric_limits<std::uint32_t>::max)());
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "sector-gap", 48, "SECTOR_OFFSET_INVALID",
        "compressed sector offset gap",
        [](auto& image, MutableMpqTables& tables) {
            const std::uint32_t filePos = GetU32(tables.block, 0);
            const std::size_t firstOffset = tables.archiveBase + filePos;
            PutU32(image, firstOffset, GetU32(image, firstOffset) + 1);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "unknown-flags", 49, "FILE_FLAGS_UNSUPPORTED",
        "unknown file flag",
        [](auto&, MutableMpqTables& tables) {
            PutU32(tables.block, 2 * 16 + 12,
                   GetU32(tables.block, 2 * 16 + 12) | 1u);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "format-v1", 50, "FORMAT_UNSUPPORTED",
        "unsupported extended MPQ format",
        [](auto& image, MutableMpqTables& tables) {
            PutU32(image, tables.archiveBase + 0x0C,
                   GetU32(image, tables.archiveBase + 0x0C) | 1u);
        });
}

void TestArchiveCandidateResourceBudgets() {
    TempDirectory temp;

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "declared-archive-budget");
        HANDLE source = CreateFileW(
            fixture.source.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(source != INVALID_HANDLE_VALUE,
                "declared archive budget fixture must open by handle");
        War3Map::Mpq::MpqOpenLimits limits;
        limits.maxDeclaredArchiveBytes = 32;
        const auto opened = MpqArchive::OpenReadOnlyHandle(source, fixture.source, limits);
        CloseHandle(source);
        Require(!opened.Succeeded() &&
                    HasDiagnostic(opened.diagnostics, "DECLARED_ARCHIVE_BUDGET_EXCEEDED"),
                "declared archive budget must reject immediately after header parsing");
    }

    RequireMutatedCandidateRejected(
        temp.Path() / "single-member-budget", 51,
        "MEMBER_DECODED_BUDGET_EXCEEDED", "single decoded member budget",
        [](auto&, MutableMpqTables& tables) {
            constexpr std::size_t kKeepBlock = 2;
            PutU32(
                tables.block, kKeepBlock * 16 + 8,
                static_cast<std::uint32_t>(
                    War3Map::Mpq::kArchiveCandidateMaxMemberBytes + 1));
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "total-decoded-budget", 52,
        "SOURCE_DECODED_BUDGET_EXCEEDED", "total decoded source budget",
        [](auto&, MutableMpqTables& tables) {
            constexpr std::uint32_t kFiftyMiB = 50u * 1024u * 1024u;
            for (std::size_t block = 0; block < 3; ++block) {
                PutU32(tables.block, block * 16 + 8, kFiftyMiB);
            }
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "listfile-budget", 53,
        "LISTFILE_UNREADABLE", "listfile pre-parse budget",
        [](auto&, MutableMpqTables& tables) {
            Require(tables.blockCount == 4,
                    "candidate fixture must contain the generated listfile block");
            constexpr std::size_t kListfileBlock = 3;
            constexpr std::uint32_t kOverFourMiB = 4u * 1024u * 1024u + 1u;
            PutU32(tables.block, kListfileBlock * 16 + 8, kOverFourMiB);
        });

    RequireMutatedCandidateRejected(
        temp.Path() / "table-entry-budget", 54,
        "TABLE_COUNT_LIMIT_EXCEEDED", "parse-time hash table budget",
        [](auto& image, MutableMpqTables& tables) {
            constexpr std::uint32_t kOverCandidateHashLimit = 131072;
            PutU32(
                image, tables.archiveBase + 0x18,
                kOverCandidateHashLimit);
        });

    {
        const auto fixture = BuildCandidateFixture(temp.Path() / "prefix-scan-budget");
        const auto original = ReadAll(fixture.source);
        Require(original.size() > fixture.prefix.size(),
                "prefix budget fixture must contain MPQ bytes");
        const std::size_t oversizedPrefix = static_cast<std::size_t>(
            War3Map::Mpq::kArchiveCandidateMaxPrefixBytes + 512);
        std::vector<std::byte> shifted(
            oversizedPrefix + original.size() - fixture.prefix.size(), std::byte{0});
        std::copy(
            original.begin() + static_cast<std::ptrdiff_t>(fixture.prefix.size()),
            original.end(), shifted.begin() + static_cast<std::ptrdiff_t>(oversizedPrefix));
        WriteAll(fixture.source, shifted);
        const auto request = MakeCandidateRequest(fixture, 55);
        const auto result = War3Map::Mpq::ExportArchiveCandidate(request);
        Require(!result.Succeeded() &&
                    HasDiagnostic(result.diagnostics, "ARCHIVE_PREFIX_SCAN_LIMIT_EXCEEDED"),
                "oversized prefix must fail during bounded header scan: " +
                    DiagnosticsText(result.diagnostics));
        Require(!std::filesystem::exists(fixture.output) &&
                    !std::filesystem::exists(fixture.manifest),
                "prefix scan rejection must not publish artifacts");
        RequireCandidateTempsAbsent(request, "prefix scan budget");
    }
}

// 部分编辑器把 (listfile) 自身也写进清单；严格清单不能把它再补一次后误报重名。
void TestStrictListfileListsItself() {
    TempDirectory temp;
    const auto archivePath = BuildSyntheticMpq(temp.Path(), "(listfile)", "(listfile)\r\n");
    auto opened = MpqArchive::OpenReadOnly(archivePath);
    Require(opened.Succeeded() && opened.value.has_value(), "self-listing archive must open");
    const auto strict = opened.value->ListFilesStrict();
    Require(strict.Succeeded() && strict.value && strict.value->size() == 1,
            "a listfile that lists itself is a valid strict inventory with one member");
}

void TestStrictStaleListfileName() {
    TempDirectory temp;
    // 原创清单含已经不存在的metadata。不存在和多个hash是不同根因；均不准入严格写出。
    // 该最小加密构造器要求正文按dword对齐；16字节清单避免先命中加密尾字节拒绝。
    const auto path = BuildSyntheticMpq(temp.Path(), "(listfile)", "(attributes)\r\nx\n");
    auto archive = MpqArchive::OpenReadOnly(path);
    Require(archive.Succeeded(), "stale listfile archive still opens read-only");
    const auto strict = archive.value->ListFilesStrict();
    Require(!strict.Succeeded() && !strict.diagnostics.empty()
        && strict.diagnostics.front().code == "LISTFILE_MEMBER_MISSING",
        "missing member has precise diagnostic: " + DiagnosticsText(strict.diagnostics));
    const auto preserved = ReadAll(path);
    Require(!archive.value->HasFile("(attributes)") && ReadAll(path) == preserved, "missing metadata is not fabricated or changed");
}

void TestStrictCanonicalListfileCollision() {
    TempDirectory temp;
    const auto archivePath = temp.Path() / "canonical-collision.w3x";
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"alpha.bin", TextBytes("alpha"), false},
        {"bravo.bin", TextBytes("bravo"), false}};
    Require(War3Map::Mpq::WriteMpqArchive(archivePath, files).Succeeded(),
            "canonical collision fixture must be written");

    auto image = ReadAll(archivePath);
    const auto tables = ReadMutableTables(image);
    constexpr std::size_t kListfileBlock = 2;
    const std::uint32_t listfilePos = GetU32(tables.block, kListfileBlock * 16);
    const std::uint32_t compressedSize = GetU32(tables.block, kListfileBlock * 16 + 4);
    const std::string original = "alpha.bin\r\nbravo.bin\r\n";
    Require(compressedSize == original.size() + 8 &&
                GetU32(image, tables.archiveBase + listfilePos) == 8,
            "small synthetic listfile must use one raw compressed sector");
    const std::string replacement = "ALPHA.BIN";
    const std::size_t secondName = tables.archiveBase + listfilePos + 8 + 11;
    std::memcpy(image.data() + secondName, replacement.data(), replacement.size());
    WriteAll(archivePath, image);

    auto opened = MpqArchive::OpenReadOnly(archivePath);
    Require(opened.Succeeded() && opened.value.has_value(),
            "canonical-collision archive should open before strict inventory");
    const auto strict = opened.value->ListFilesStrict();
    // 重复行本身只是冗余；这里失败的根因是 bravo.bin 没有任何名字，仍不准入严格清单。
    Require(!strict.Succeeded() && HasDiagnostic(strict.diagnostics, "ANONYMOUS_BLOCK_REJECTED")
                && !HasDiagnostic(strict.diagnostics, "LISTFILE_NAME_DUPLICATE"),
            "a case-only duplicate that leaves another block unnamed still fails as anonymous block: " + DiagnosticsText(strict.diagnostics));
    opened.value.reset();
    std::filesystem::rename(archivePath, temp.Path() / "war3patch.mpq");
    auto game = War3Map::Game::GameDataSource::Mount(temp.Path());
    Require(game.Succeeded(), "game catalog should mount the colliding listfile archive");
    const auto inventory = game.value->ListFilesWithSource();
    Require(inventory.Succeeded() && inventory.value,
            "read-only catalog should report partial names without failing all assets");
    Require(HasDiagnostic(inventory.diagnostics, "GAME_LISTFILE_NAME_CONFLICT"),
            "game catalog must report canonical listfile conflicts");
    Require(inventory.value->empty(), "ambiguous game names must not be offered as resources");
}

void TestStrictListfileRedundantDuplicate() {
    // 原创两成员归档，把清单原地改成 "a\nA\nb\n"：名字哈希不分大小写，重复行指向同一成员，
    // 每个块仍有名字，严格清单应成功、按首次写法保留并给出警告。
    TempDirectory temp;
    const auto archivePath = temp.Path() / "redundant-listfile.w3x";
    const std::vector<War3Map::Mpq::MpqBuildFile> files{{"a", TextBytes("alpha"), false}, {"b", TextBytes("bravo"), false}};
    Require(War3Map::Mpq::WriteMpqArchive(archivePath, files).Succeeded(), "redundant listfile fixture must be written");
    auto image = ReadAll(archivePath);
    const auto tables = ReadMutableTables(image);
    constexpr std::size_t kListfileBlock = 2;
    const std::uint32_t listfilePos = GetU32(tables.block, kListfileBlock * 16);
    const std::string original = "a\r\nb\r\n", replacement = "a\nA\nb\n";
    Require(GetU32(tables.block, kListfileBlock * 16 + 4) == original.size() + 8
                && std::memcmp(image.data() + tables.archiveBase + listfilePos + 8, original.data(), original.size()) == 0,
            "small synthetic listfile must be one raw sector with the expected names");
    std::memcpy(image.data() + tables.archiveBase + listfilePos + 8, replacement.data(), replacement.size());
    WriteAll(archivePath, image);

    auto opened = MpqArchive::OpenReadOnly(archivePath);
    Require(opened.Succeeded() && opened.value.has_value(), "redundant listfile archive opens");
    const auto strict = opened.value->ListFilesStrict();
    Require(strict.Succeeded() && strict.value && HasDiagnostic(strict.diagnostics, "LISTFILE_NAME_DUPLICATE"),
            "redundant duplicate is a warning, not a failure: " + DiagnosticsText(strict.diagnostics));
    std::vector<std::string> names;
    for (const auto& file : *strict.value) names.push_back(file.name);
    std::sort(names.begin(), names.end());
    Require(names == std::vector<std::string>{"(listfile)", "a", "b"}, "each member listed once with its first spelling");
}

void TestBoundedArchiveReads() {
    using namespace War3Map::Mpq;
    TempDirectory temp;
    const auto path = temp.Path() / "bounded.mpq";
    const auto single = TextBytes("single-unit-data");
    std::vector<std::byte> stored(9000, std::byte{0x51});
    std::vector<std::byte> compressed(8192, std::byte{0x32});
    auto zlib = Deflate(compressed);
    zlib.insert(zlib.begin(), std::byte{0x02});
    const std::vector<MpqBuildFile> files{
        {"Paths\\Single.bin", {}, false, MpqRawBlock{
            single, static_cast<std::uint32_t>(single.size()), kFileExists | kFileSingleUnit, 0}},
        {"stored.bin", stored, false},
        {"compressed.bin", compressed, true},
        {"single-z.bin", {}, false, MpqRawBlock{
            zlib, static_cast<std::uint32_t>(compressed.size()),
            kFileExists | kFileSingleUnit | kFileCompress, 0}},
        {"tiny.bin", TextBytes("tiny"), true},
        {"empty.bin", {}, false}};
    Require(WriteMpqArchive(path, files).Succeeded(), "bounded fixture must be written");
    auto opened = MpqArchive::OpenReadOnly(path);
    Require(opened.Succeeded() && opened.value, "bounded fixture must open");
    const std::vector<std::vector<std::byte>> expected{
        single, stored, compressed, compressed, TextBytes("tiny"), {}};
    const auto blocks = opened.value->BlockTableEntries();
    Require(blocks[4].compressedSize > blocks[4].fileSize,
            "tiny compressed fixture must exercise stored bytes larger than decoded bytes");
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto budget = (std::max)(blocks[i].compressedSize, blocks[i].fileSize);
        const auto full = opened.value->ReadFileBounded(files[i].name, budget);
        Require(full.Succeeded() && full.value && *full.value == expected[i],
                "exact stored/decoded budget must return the complete member: " + files[i].name);
        if (budget != 0) {
            const auto limited = opened.value->ReadFileBounded(files[i].name, budget - 1);
            Require(!limited.Succeeded() && !limited.value &&
                        HasDiagnostic(limited.diagnostics, "FILE_READ_BUDGET_EXCEEDED"),
                    "one byte below either size must fail without partial bytes: " + files[i].name);
            const auto head = opened.value->ReadFileHead(files[i].name, 2);
            Require(head.Succeeded() && head.value && head.value->size() == 2,
                    "the old head API must keep its prefix-reading behavior");
        }
    }
    const auto canonical = opened.value->ReadFileBounded("paths/single.BIN", single.size());
    Require(canonical.Succeeded() && *canonical.value == single,
            "bounded lookup must preserve case and slash normalization");
    const auto zero = opened.value->ReadFileBounded("empty.bin", 0);
    Require(zero.Succeeded() && zero.value && zero.value->empty(),
            "zero budget must accept an actual empty member");
    const auto missing = opened.value->ReadFileBounded("missing.bin", 0);
    Require(!missing.Succeeded() && !missing.value &&
                HasDiagnostic(missing.diagnostics, "FILE_NOT_FOUND"),
            "missing member must remain distinct from empty member");
    opened.value.reset();

    // 仅改小归档的加密块表；不构造/分配宣称的 2 GiB 数据。
    const auto original = ReadAll(path);
    for (const std::size_t index : {std::size_t{1}, std::size_t{3}}) {
        auto image = original;
        auto tables = ReadMutableTables(image);
        PutU32(tables.block, index * 16 + 8, 0x7FFFFFFFu);
        WriteMutableTables(image, tables);
        const auto mutatedPath = temp.Path() / ("declared-large-" + std::to_string(index) + ".mpq");
        WriteAll(mutatedPath, image);
        auto mutated = MpqArchive::OpenReadOnly(mutatedPath);
        Require(mutated.Succeeded() && mutated.value, "large decoded declaration may open metadata");
        const auto read = mutated.value->ReadFileBounded(files[index].name, 16384);
        Require(!read.Succeeded() && !read.value &&
                    HasDiagnostic(read.diagnostics, "FILE_READ_BUDGET_EXCEEDED"),
                "declared expansion must fail before stored-size validation or decompression");
    }

    const std::vector<MpqBuildFile> invalidTable{{"short-table.bin", {}, false, MpqRawBlock{
        {std::byte{0x00}}, 1, kFileExists | kFileCompress, kMpqWriterSectorSize}}};
    const auto invalidPath = temp.Path() / "short-table.mpq";
    Require(WriteMpqArchive(invalidPath, invalidTable).Succeeded(), "short-table fixture write");
    auto invalid = MpqArchive::OpenReadOnly(invalidPath);
    Require(invalid.Succeeded() && invalid.value, "short-table metadata may open");
    const auto shortTable = invalid.value->ReadFileBounded("short-table.bin", 1);
    Require(!shortTable.Succeeded() && !shortTable.value &&
                HasDiagnostic(shortTable.diagnostics, "SECTOR_OFFSET_INVALID"),
            "sector table must fit the bounded member before its allocation/read");
}

void TestEmptySectorChecksumLayout() {
    using namespace War3Map::Mpq;
    TempDirectory temp;
    const std::string member = "models/synthetic.bin";
    std::vector<std::byte> original(5000);
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<std::byte>(i % 19);
    // 两个数据扇区，另一个末偏移描述明确为空的校验区。两扇区都真正压缩。
    std::vector<std::byte> raw(16);
    PutU32(raw, 0, 16);
    for (std::size_t sector = 0; sector < 2; ++sector) {
        const auto start = sector * kMpqWriterSectorSize;
        const auto source = std::span(original).subspan(start, (std::min)(original.size() - start,
            static_cast<std::size_t>(kMpqWriterSectorSize)));
        const auto compressed = Deflate(source);
        Require(compressed.size() + 1 < source.size(), "checksum-layout fixture must exercise compressed sectors");
        raw.push_back(std::byte{0x02}); raw.insert(raw.end(), compressed.begin(), compressed.end());
        PutU32(raw, (sector + 1) * 4, static_cast<std::uint32_t>(raw.size()));
    }
    PutU32(raw, 12, static_cast<std::uint32_t>(raw.size()));
    const auto write = [&](const std::string& name, std::vector<std::byte> bytes, std::uint32_t flags) {
        const auto path = temp.Path() / (name + ".mpq");
        const std::vector<MpqBuildFile> files{{member, {}, false, MpqRawBlock{
            std::move(bytes), static_cast<std::uint32_t>(original.size()), flags, kMpqWriterSectorSize}}};
        Require(WriteMpqArchive(path, files).Succeeded(), "write original checksum-layout fixture");
        return path;
    };
    const auto flags = kFileExists | kFileCompress | kFileSectorCrc;
    for (const bool encrypted : {false, true}) {
        auto bytes = raw;
        if (encrypted) {
            const auto key = MpqCrypt::Instance().HashString("synthetic.bin", MpqCrypt::HashType::FileKey);
            for (std::size_t sector = 0; sector < 2; ++sector) {
                const auto start = GetU32(raw, sector * 4), end = GetU32(raw, (sector + 1) * 4);
                EncryptBlock(bytes, start, end - start, key + static_cast<std::uint32_t>(sector));
            }
            EncryptBlock(bytes, 0, 16, key - 1);
        }
        auto archive = MpqArchive::OpenReadOnly(write(encrypted ? "encrypted-empty" : "empty", std::move(bytes),
            flags | (encrypted ? kFileEncrypted : 0)));
        Require(archive.Succeeded(), "empty checksum fixture opens");
        const auto read = archive.value->ReadFileBounded(member, original.size());
        Require(read.Succeeded() && *read.value == original, "explicit empty checksum layout must decode complete original bytes");
        const auto head = archive.value->ReadFileHead(member, 17);
        Require(head.Succeeded() && *head.value == std::vector<std::byte>(original.begin(), original.begin() + 17),
            "empty checksum layout must retain prefix reading");
        const auto strict = archive.value->ListFilesStrict();
        Require(!strict.Succeeded() && HasDiagnostic(strict.diagnostics, "SECTOR_CRC_UNSUPPORTED"),
            "read compatibility must not enable unchecked strict side-write");
        const auto bounded = archive.value->ReadFileBounded(member, original.size() - 1);
        Require(!bounded.Succeeded() && HasDiagnostic(bounded.diagnostics, "FILE_READ_BUDGET_EXCEEDED"),
            "checksum flag must not bypass decoded size budget");
    }
    const auto rejects = [&](const std::string& name, std::vector<std::byte> bytes, std::uint32_t blockFlags,
        const std::string& code) {
        const auto archive = MpqArchive::OpenReadOnly(write(name, std::move(bytes), blockFlags));
        Require(archive.Succeeded(), "invalid checksum fixture metadata may open");
        const auto read = archive.value->ReadFileBounded(member, original.size());
        Require(!read.Succeeded() && HasDiagnostic(read.diagnostics, code), name + " must reject full reading");
        const auto head = archive.value->ReadFileHead(member, 1);
        Require(!head.Succeeded() && HasDiagnostic(head.diagnostics, code), name + " must reject prefix reading too");
    };
    auto invalid = raw; PutU32(invalid, 0, 17);
    rejects("table-gap", invalid, flags, "SECTOR_OFFSET_INVALID");
    invalid = raw; PutU32(invalid, 8, GetU32(invalid, 4));
    rejects("empty-data-sector", invalid, flags, "SECTOR_OFFSET_INVALID");
    invalid = raw; PutU32(invalid, 8, static_cast<std::uint32_t>(invalid.size() + 1));
    rejects("checksum-offset-outside-member", invalid, flags, "SECTOR_OFFSET_INVALID");
    invalid = raw; PutU32(invalid, 12, static_cast<std::uint32_t>(invalid.size() - 1));
    rejects("checksum-end-outside-block", invalid, flags, "SECTOR_OFFSET_INVALID");
    rejects("extra-offset-without-flag", raw, flags & ~kFileSectorCrc, "SECTOR_OFFSET_INVALID");
    rejects("truncated-offset-table", std::vector<std::byte>(12), flags, "SECTOR_OFFSET_INVALID");
    invalid = raw; invalid.insert(invalid.end(), 8, std::byte{});
    PutU32(invalid, 12, static_cast<std::uint32_t>(invalid.size()));
    rejects("nonempty-checksums", invalid, flags, "SECTOR_CRC_UNSUPPORTED");
}

void TestGameBoundedReadPriority() {
    using namespace War3Map::Mpq;
    TempDirectory temp;
    const std::vector<MpqBuildFile> base{
        {"Textures\\Shared.blp", TextBytes("base"), false},
        {"Textures\\Broken.blp", TextBytes("valid lower layer"), false},
        {"base-only.bin", TextBytes("base only"), false}};
    const auto patchPayload = TextBytes("highest priority patch payload");
    const std::vector<MpqBuildFile> patch{
        {"textures/shared.BLP", patchPayload, false},
        {"Textures\\Broken.blp", {}, false, MpqRawBlock{
            {std::byte{0x10}, std::byte{0x01}}, 32,
            kFileExists | kFileSingleUnit | kFileCompress, 0}}};
    Require(WriteMpqArchive(temp.Path() / "war3.mpq", base).Succeeded(), "bounded base write");
    Require(WriteMpqArchive(temp.Path() / "war3patch.mpq", patch).Succeeded(), "bounded patch write");
    auto game = War3Map::Game::GameDataSource::Mount(temp.Path());
    Require(game.Succeeded() && game.value, "bounded game source must mount");
    const auto limited = game.value->ReadFileBounded("TEXTURES/SHARED.BLP", 8);
    Require(!limited.Succeeded() && !limited.value &&
                HasDiagnostic(limited.diagnostics, "FILE_READ_BUDGET_EXCEEDED"),
            "over-budget patch must not return an in-budget base member");
    const auto exact = game.value->ReadFileBounded("Textures\\Shared.blp", patchPayload.size());
    Require(exact.Succeeded() && exact.value && *exact.value == patchPayload,
            "in-budget complete patch must retain authoritative priority");
    const auto sourcedPatch = game.value->ReadFileBoundedWithSource("Textures\\Shared.blp", patchPayload.size());
    Require(sourcedPatch.Succeeded() && sourcedPatch.value && sourcedPatch.value->bytes == patchPayload
                && sourcedPatch.value->archiveName == "war3patch.mpq",
            "sourced patch bytes and archive must share the winning lookup");
    const auto broken = game.value->ReadFileBounded("Textures\\Broken.blp", 32);
    Require(!broken.Succeeded() && !broken.value &&
                HasDiagnostic(broken.diagnostics, "SECTOR_UNSUPPORTED_COMPRESSION"),
            "corrupt/unsupported highest-priority member must not return a valid lower layer");
    const auto baseOnly = game.value->ReadFileBounded("base-only.bin", 9);
    Require(baseOnly.Succeeded() && baseOnly.value && *baseOnly.value == TextBytes("base only"),
            "absence in patch must still permit lookup in base");
    const auto sourcedBase = game.value->ReadFileBoundedWithSource("base-only.bin", 9);
    Require(sourcedBase.Succeeded() && sourcedBase.value && sourcedBase.value->bytes == TextBytes("base only")
                && sourcedBase.value->archiveName == "war3.mpq",
            "base-only bytes must report the base archive");
    const auto missing = game.value->ReadFileBounded("missing.bin", 64);
    Require(!missing.Succeeded() && !missing.value &&
                HasDiagnostic(missing.diagnostics, "file-not-found"),
            "missing across all mounted archives must retain game-source diagnostic");
}

void TestGameStrictMount() {
    using War3Map::Game::GameDataSource;
    TempDirectory temp;
    const std::vector<War3Map::Mpq::MpqBuildFile> files{{"same.bin", TextBytes("base"), false}};
    Require(War3Map::Mpq::WriteMpqArchive(temp.Path() / "war3.mpq", files).Succeeded(),
            "strict mount base fixture write");
    auto missingPatch = GameDataSource::MountStrict(temp.Path());
    Require(missingPatch.Succeeded() && missingPatch.value && missingPatch.value->MountedCount() == 1,
            "strict mount must allow absent optional archives");
    const auto base = missingPatch.value->ReadFileBounded("same.bin", 4);
    Require(base.Succeeded() && base.value && *base.value == TextBytes("base"),
            "a confirmed absent patch allows the base source");

    const auto patchPath = temp.Path() / "war3patch.mpq";
    WriteAll(patchPath, TextBytes("present but not an MPQ"));
    const auto corrupt = GameDataSource::MountStrict(temp.Path());
    Require(!corrupt.Succeeded() && !corrupt.value &&
                HasDiagnostic(corrupt.diagnostics, "archive-open-failed"),
            "strict mount must not skip a present corrupt patch and expose base");
    const auto compatible = GameDataSource::Mount(temp.Path());
    Require(compatible.Succeeded() && compatible.value && compatible.value->MountedCount() == 1,
            "legacy best-effort mount must retain its compatibility contract");
    std::filesystem::remove(patchPath);
    std::filesystem::create_directory(patchPath);
    const auto directory = GameDataSource::MountStrict(temp.Path());
    Require(!directory.Succeeded() && !directory.value &&
                HasDiagnostic(directory.diagnostics, "archive-not-file"),
            "an existing non-file archive path is not an absent archive");
    std::filesystem::remove(patchPath);
    Require(War3Map::Mpq::WriteMpqArchive(patchPath, files).Succeeded(), "locked patch fixture write");
    const HANDLE locked = CreateFileW(patchPath.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "test must exclusively lease the patch");
    const auto inaccessible = GameDataSource::MountStrict(temp.Path());
    CloseHandle(locked);
    Require(!inaccessible.Succeeded() && !inaccessible.value &&
                HasDiagnostic(inaccessible.diagnostics, "archive-open-failed"),
            "a sharing-denied patch must not be mistaken for absent and bypassed");
}

void TestGameDataSourceKnownInventory() {
    TempDirectory temp;
    const std::vector<War3Map::Mpq::MpqBuildFile> baseFiles{
        {"Sounds\\Shared.wav", TextBytes("base"), false},
        {"Sounds\\BaseOnly.mp3", TextBytes("music"), false}};
    const std::vector<War3Map::Mpq::MpqBuildFile> patchFiles{
        {"sounds/shared.WAV", TextBytes("patch"), false},
        {"Sounds\\PatchOnly.wav", TextBytes("patch only"), false}};
    Require(War3Map::Mpq::WriteMpqArchive(temp.Path() / "war3.mpq", baseFiles).Succeeded(),
            "base fixture write must succeed");
    Require(War3Map::Mpq::WriteMpqArchive(temp.Path() / "war3patch.mpq", patchFiles).Succeeded(),
            "patch fixture write must succeed");
    auto game = War3Map::Game::GameDataSource::Mount(temp.Path());
    Require(game.Succeeded(), "fixture game root must mount");
    const auto listed = game.value->ListFilesWithSource();
    Require(listed.Succeeded() && listed.value && listed.value->size() == 3,
            "catalog must merge real known names across mounted archives");
    const auto find = [&](std::string_view key) -> const War3Map::Game::GameResourceFile* {
        for (const auto& file : *listed.value) {
            const auto canonical = War3Map::Resources::CanonicalImportPath(file.path);
            if (canonical.Succeeded() && canonical.value && *canonical.value == key) return &file;
        }
        return nullptr;
    };
    const auto* shared = find("SOUNDS\\SHARED.WAV");
    const auto* baseOnly = find("SOUNDS\\BASEONLY.MP3");
    const auto* patchOnly = find("SOUNDS\\PATCHONLY.WAV");
    Require(shared && shared->archiveName == "war3patch.mpq", "patch must win name/source priority");
    Require(baseOnly && baseOnly->archiveName == "war3.mpq", "base-only source must be exact");
    Require(patchOnly && patchOnly->archiveName == "war3patch.mpq", "patch-only source must be exact");
    auto bytes = game.value->ReadFile(shared->path);
    Require(bytes.Succeeded() && bytes.value && *bytes.value == TextBytes("patch"),
            "catalog source must agree with actual game read precedence");

    const auto noListfile = BuildSyntheticMpq(temp.Path(), "Sounds\\BaseOnly.mp3", "0123456789ABCDEF");
    std::filesystem::rename(noListfile, temp.Path() / "war3x.mpq");
    auto partial = War3Map::Game::GameDataSource::Mount(temp.Path());
    Require(partial.Succeeded(), "game root with one unlisted archive must still mount");
    const auto partialList = partial.value->ListFilesWithSource();
    Require(partialList.Succeeded() && partialList.value && partialList.value->size() == 3,
            "missing listfile must not erase other known resources");
    Require(HasDiagnostic(partialList.diagnostics, "GAME_LISTFILE_MISSING"),
            "missing listfile must have an explicit catalog diagnostic");
    const auto source = std::find_if(partialList.value->begin(), partialList.value->end(),
        [](const auto& file) { return file.path == "Sounds\\BaseOnly.mp3"; });
    Require(source != partialList.value->end() && source->archiveName == "war3x.mpq",
            "listed lower path must still report an unlisted higher archive when HasFile resolves it");
}

void TestOwnedTilesetArchive() {
    using War3Map::Game::GameDataSource;
    TempDirectory temp;
    const auto nestedPath = temp.Path() / "fixture.mpq";
    const std::vector<War3Map::Mpq::MpqBuildFile> inner{{"same.bin", TextBytes("city"), false}};
    Require(War3Map::Mpq::WriteMpqArchive(nestedPath, inner).Succeeded(), "nested fixture write");
    auto bytes = ReadAll(nestedPath);
    auto owned = MpqArchive::OpenReadOnlyBytes(bytes);
    Require(owned.Succeeded(), "owned archive uses existing MPQ parser");
    const auto expectedSha = War3Map::Sha256(bytes);
    const std::vector<War3Map::Mpq::MpqBuildFile> outer{
        {"same.bin", TextBytes("base"), false}, {"Y.mpq", bytes, true}};
    Require(War3Map::Mpq::WriteMpqArchive(temp.Path() / "war3.mpq", outer).Succeeded(), "outer write");
    bytes.assign(1, std::byte{0});
    auto read = owned.value->ReadFileBounded("same.bin", 4);
    Require(read.Succeeded() && *read.value == TextBytes("city"), "archive owns immutable input copy");
    Require(!MpqArchive::OpenReadOnlyBytes(bytes).Succeeded(), "truncated memory archive rejected");
    const auto plain = GameDataSource::MountStrict(temp.Path());
    const auto missing = GameDataSource::MountStrict(temp.Path(), 'L');
    auto city = GameDataSource::MountStrict(temp.Path(), 'Y');
    Require(plain.Succeeded() && missing.Succeeded() && city.Succeeded(), "optional tileset mounts");
    Require(HasDiagnostic(missing.diagnostics, "tileset-archive-absent"), "absent tileset is reported");
    Require(*plain.value->ReadFile("same.bin").value == TextBytes("base")
        && *missing.value->ReadFile("same.bin").value == TextBytes("base"), "default and absent preserve base");
    auto sourced = city.value->ReadFileBoundedWithSource("same.bin", 4);
    Require(sourced.Succeeded() && sourced.value->bytes == TextBytes("city")
        && sourced.value->archiveName == "war3.mpq|Y.mpq|sha256:" + *expectedSha.value,
        "tileset texture and full parent/content provenance");
    Require(!city.value->ReadFileBounded("same.bin", 3).Succeeded(), "nested member budget never falls back");
    const std::vector<War3Map::Mpq::MpqBuildFile> patch{{"same.bin", TextBytes("patch"), false}};
    Require(War3Map::Mpq::WriteMpqArchive(temp.Path() / "war3patch.mpq", patch).Succeeded(), "patch fixture");
    const auto patched = GameDataSource::MountStrict(temp.Path(), 'Y');
    Require(patched.Succeeded() && *patched.value->ReadFile("same.bin").value == TextBytes("patch"),
        "retail patch priority 8 wins over tileset priority 6");
    std::filesystem::remove(temp.Path() / "war3patch.mpq");
    const std::vector<War3Map::Mpq::MpqBuildFile> corrupt{{"Y.mpq", TextBytes("invalid"), false}};
    Require(War3Map::Mpq::WriteMpqArchive(temp.Path() / "war3patch.mpq", corrupt).Succeeded(), "corrupt fixture");
    Require(!GameDataSource::MountStrict(temp.Path(), 'Y').Succeeded(), "corrupt selected tileset refuses lower Y");
    Require(!GameDataSource::MountStrict(temp.Path(), '/').Succeeded(), "tileset is not a path fragment");
}

void TestSoundInfoVerifiedDiscovery() {
    TempDirectory temp;
    const std::string path = "Abilities\\Spells\\Human\\Blizzard\\BlizzardTarget1.wav";
    const auto noListfile = BuildSyntheticMpq(temp.Path(), path, "0123456789ABCDEF");
    std::filesystem::rename(noListfile, temp.Path() / "war3.mpq");
    const std::string slk =
        "ID;P\r\nB;X3;Y3\r\n"
        "C;X1;Y1;K\"SoundName\"\r\n"
        "C;X2;K\"FileNames\"\r\n"
        "C;X3;K\"DirectoryBase\"\r\n"
        "C;X1;Y2;K\"Blizzard\"\r\n"
        "C;X2;K\"BlizzardTarget1.wav,Missing.wav\"\r\n"
        "C;X3;K\"Abilities\\Spells\\Human\\Blizzard\"\r\n"
        "C;X1;Y3;K\"Unsafe\"\r\n"
        "C;X2;K\"..\\Escape.wav\"\r\n"
        "C;X3;K\"Abilities\\Spells\\Human\\Blizzard\"\r\nE\r\n";
    const std::vector<War3Map::Mpq::MpqBuildFile> tables{
        {"UI\\SoundInfo\\AbilitySounds.slk", TextBytes(slk), false}};
    Require(War3Map::Mpq::WriteMpqArchive(temp.Path() / "war3patch.mpq", tables).Succeeded(),
        "SoundInfo metadata fixture must write");
    auto game = War3Map::Game::GameDataSource::Mount(temp.Path());
    Require(game.Succeeded(), "SoundInfo fixture game root must mount");
    const auto listed = game.value->ListFilesWithSource();
    Require(listed.Succeeded() && HasDiagnostic(listed.diagnostics, "GAME_LISTFILE_MISSING"),
        "base archive without listfile must be reported");
    const auto sounds = game.value->DiscoverSoundFilesFromSoundInfo();
    Require(sounds.Succeeded() && sounds.value && sounds.value->size() == 1,
        "only a SoundInfo path verified by HasFile may enter the sound directory");
    Require(sounds.value->front().path == path && sounds.value->front().archiveName == "war3.mpq",
        "SoundInfo path must insert the missing separator and report the real base MPQ");
    Require(HasDiagnostic(sounds.diagnostics, "SOUNDINFO_PATH_NOT_FOUND") &&
        HasDiagnostic(sounds.diagnostics, "SOUNDINFO_PATH_UNSAFE"),
        "missing and unsafe SoundInfo paths must be diagnosed, never invented as resources");
}

void TestSharedSlkParserModes() {
    const std::string valid = "ID;P\r\nC;X1;Y1;K\"Header\"\r\n"
        "C;X1;Y2;K\"V\"\"Q\"\r\nE\r\n";
    const auto strict = War3Map::Text::ParseSlkCells(valid, War3Map::Text::SlkCellMode::CatalogStrict);
    const auto legacy = War3Map::Text::ParseSlkCells(valid, War3Map::Text::SlkCellMode::SoundQueriesLegacy);
    Require(strict.Succeeded() && strict.value && strict.value->at(2).at(1) == "V\"Q",
        "strict catalog parsing must decode paired SYLK quotes");
    Require(legacy.Succeeded() && legacy.value && legacy.value->at(2).at(1) == "V\"\"Q",
        "legacy script label parsing must retain historical quoted-cell behavior");
    const std::string incomplete = "C;X0;Y1;K\"Header\"\n";
    Require(!War3Map::Text::ParseSlkCells(incomplete,
        War3Map::Text::SlkCellMode::CatalogStrict).Succeeded(),
        "strict catalog must reject missing ID/E and zero coordinates");
    const auto old = War3Map::Text::ParseSlkCells(incomplete,
        War3Map::Text::SlkCellMode::SoundQueriesLegacy);
    Require(old.Succeeded() && old.value && old.value->at(1).at(0) == "Header",
        "legacy script mode must preserve old permissive coordinates/header behavior");
}

// 可选：若设置了环境变量 WAR3_TEST_MPQ 指向真实 w3x/w3m，则做端到端验证。
// 不设置时跳过，保证 ctest 的 hermetic 性。
void TestRealMapIfProvided() {
    const char* envPath = std::getenv("WAR3_TEST_MPQ");
    if (envPath == nullptr || envPath[0] == '\0') {
        std::cout << "  (real-map check skipped: set WAR3_TEST_MPQ to enable)\n";
        return;
    }
    const std::filesystem::path mapPath(envPath);
    auto opened = MpqArchive::OpenReadOnly(mapPath);
    Require(opened.Succeeded() && opened.value.has_value(),
            "failed to open real map: " + mapPath.string());
    MpqArchive& archive = *opened.value;
    std::cout << "  real map: " << mapPath.filename().string()
              << "  blocks=" << archive.FileCount()
              << "  listfile=" << archive.ListFiles().size() << "\n";

    // war3map.w3i（地图信息）几乎所有地图都含有，用它验证真实解密/解压。
    if (archive.HasFile("war3map.w3i")) {
        auto info = archive.ReadFile("war3map.w3i");
        Require(info.Succeeded() && info.value.has_value() && !info.value->empty(),
                "war3map.w3i must decode to non-empty content");
        std::uint32_t version = 0;
        std::memcpy(&version, info.value->data(), 4);
        std::cout << "    war3map.w3i decoded " << info.value->size()
                  << " bytes, format version=" << version << "\n";
        Require(version >= 8 && version <= 40,
                "war3map.w3i format version out of expected range");
    }
    // 若含 (listfile)，随机验证第一个可读文件解压成功。
    for (const std::string& fileName : archive.ListFiles()) {
        if (archive.HasFile(fileName)) {
            auto data = archive.ReadFile(fileName);
            Require(data.Succeeded(),
                    "listed file must decode: " + fileName);
            std::cout << "    first listed file OK: " << fileName
                      << " (" << (data.value ? data.value->size() : 0) << " bytes)\n";
            break;
        }
    }
}

// 真实地图零编辑往返：打开→全文件读出→重写为新 MPQ→重新打开→逐文件比对。
void TestRealMapRepackRoundTrip() {
    const std::filesystem::path fixture =
        std::filesystem::path(MPQ_TEST_FIXTURES) / "roundtrip.w3x";
    if (!std::filesystem::exists(fixture)) {
        std::cout << "  (repack roundtrip skipped: fixture not found)\n";
        return;
    }

    // 读取 HM3W 前置头。
    std::ifstream src(fixture, std::ios::binary);
    std::vector<std::byte> header(512);
    src.read(reinterpret_cast<char*>(header.data()), 512);
    src.close();

    // 打开源归档。
    auto opened = MpqArchive::OpenReadOnly(fixture);
    Require(opened.Succeeded() && opened.value.has_value(),
            "fixture map must open");
    MpqArchive& archive = *opened.value;

    // 枚举所有文件（listfile + 已知内部名）。
    std::vector<std::string> names = archive.ListFiles();
    std::erase_if(names, [](const std::string& name) {
        return _stricmp(name.c_str(), "(listfile)") == 0;
    });
    static const char* kKnownNames[] = {
        "war3map.w3i", "war3map.wtg", "war3map.wct", "war3map.wts",
        "war3map.w3u", "war3map.w3t", "war3map.w3a", "war3map.w3b",
        "war3map.w3d", "war3map.w3h", "war3map.w3q", "war3mapunits.doo"};
    for (const char* known : kKnownNames) {
        if (archive.HasFile(known) &&
            !std::any_of(names.begin(), names.end(), [known](const std::string& name) {
                return _stricmp(name.c_str(), known) == 0;
            })) {
            names.push_back(known);
        }
    }
    Require(!names.empty(), "fixture must contain at least one file");

    // 读出所有文件。
    std::vector<War3Map::Mpq::MpqBuildFile> buildFiles;
    for (const std::string& name : names) {
        auto data = archive.ReadFile(name);
        Require(data.Succeeded() && data.value.has_value(),
                "must read: " + name);
        War3Map::Mpq::MpqBuildFile bf;
        bf.name = name;
        bf.data = std::move(*data.value);
        bf.compress = true;
        buildFiles.push_back(std::move(bf));
    }

    // 写入新 MPQ。
    TempDirectory temp;
    const std::filesystem::path outPath = temp.Path() / "repacked.w3x";
    auto writeStatus = War3Map::Mpq::WriteMpqArchive(outPath, buildFiles, header);
    Require(writeStatus.Succeeded(),
            "repack write must succeed: " + DiagnosticsText(writeStatus.diagnostics));

    // 重新打开并逐文件比对。
    auto reopened = MpqArchive::OpenReadOnly(outPath);
    Require(reopened.Succeeded() && reopened.value.has_value(),
            "repacked MPQ must open");
    MpqArchive& repacked = *reopened.value;

    std::size_t verified = 0;
    for (const auto& bf : buildFiles) {
        Require(repacked.HasFile(bf.name),
                "repacked must contain: " + bf.name);
        auto data = repacked.ReadFile(bf.name);
        Require(data.Succeeded() && data.value.has_value(),
                "repacked read must succeed: " + bf.name);
        Require(*data.value == bf.data,
                "repacked content mismatch: " + bf.name);
        ++verified;
    }
    std::cout << "  repack roundtrip OK: " << verified << " files verified\n";
}

// 可选：若设置 WAR3_GAME_ROOT 指向魔兽安装目录，验证游戏 MPQ 挂载与图标读取。
void TestGameDataSourceIfProvided() {
    const char* rootEnv = std::getenv("WAR3_GAME_ROOT");
    if (rootEnv == nullptr || rootEnv[0] == '\0') {
        std::cout << "  (game-data check skipped: set WAR3_GAME_ROOT to enable)\n";
        return;
    }
    const std::filesystem::path root(rootEnv);

    // 校验。
    auto validation = War3Map::Game::GameDataSource::Validate(root);
    std::cout << "  game root validation:\n" << validation.Summary();
    Require(validation.Usable(), "game root must be usable (war3.mpq + war3x.mpq)");

    // 挂载。
    auto mounted = War3Map::Game::GameDataSource::Mount(root);
    Require(mounted.Succeeded() && mounted.value.has_value(),
            "game data source must mount");
    auto& source = *mounted.value;
    std::cout << "  mounted archives: " << source.MountedCount() << "\n";
    if (source.HasFile("Y.mpq")) {
        auto city = War3Map::Game::GameDataSource::MountStrict(root, 'Y');
        Require(city.Succeeded(), "real city tileset mounts");
        const auto cliff = city.value->ReadFileBoundedWithSource("ReplaceableTextures\\Cliff\\Cliff1.blp", 16 * 1024 * 1024);
        Require(cliff.Succeeded(), "real city cliff reads with exact source");
        const auto digest = War3Map::Sha256(cliff.value->bytes);
        Require(digest.Succeeded(), "real city cliff SHA");
        std::cout << "  city-cliff source=" << cliff.value->archiveName << " sha256=" << *digest.value << '\n';
    }
    const auto discoveredSounds = source.DiscoverSoundFilesFromSoundInfo();
    Require(discoveredSounds.Succeeded() && discoveredSounds.value && !discoveredSounds.value->empty(),
        "game SoundInfo catalog must discover at least one verified audio resource");
    std::cout << "  verified SoundInfo audio resources: " << discoveredSounds.value->size() << "\n";

    // 读取一个典型图标：BTNPillage.blp（TC_NOTHING 分类图标）。
    const std::string iconPath = "ReplaceableTextures\\CommandButtons\\BTNPillage.blp";
    if (source.HasFile(iconPath)) {
        auto blp = source.ReadFile(iconPath);
        Require(blp.Succeeded() && blp.value.has_value() && !blp.value->empty(),
                "BTNPillage.blp must decode to non-empty content");
        // BLP 文件魔数应为 "BLP1" 或 "BLP2"。
        Require(blp.value->size() >= 4, "BLP too short");
        const auto* magic = reinterpret_cast<const char*>(blp.value->data());
        const bool isBlp = std::string_view(magic, 4) == "BLP1" ||
                           std::string_view(magic, 4) == "BLP2";
        Require(isBlp, "BTNPillage.blp must have BLP1/BLP2 magic");
        std::cout << "    BTNPillage.blp OK: " << blp.value->size()
                  << " bytes, magic=" << std::string_view(magic, 4) << "\n";

        // 解码为 RGBA 并验证尺寸。
        auto decoded = War3Map::Image::DecodeBlp(*blp.value);
        Require(decoded.Succeeded() && decoded.value.has_value(),
                "BTNPillage.blp must decode to RGBA");
        Require(decoded.value->Valid(), "decoded image must be valid RGBA");
        Require(decoded.value->width == 64 && decoded.value->height == 64,
                "BTNPillage should be 64x64");
        std::cout << "    decoded RGBA: " << decoded.value->width << "x"
                  << decoded.value->height << "\n";
    } else {
        std::cout << "    (BTNPillage.blp not present in this game version)\n";
    }
}

} // namespace

int main() {
    try {
        TestCryptTable();
        TestEncryptDecryptRoundTrip();
        TestInflateStoredBlock();
        TestDeflateInflateRoundTrip();
        TestSyntheticArchiveRoundTrip();
        TestWriterRoundTrip();
        TestArchiveCandidateSuccess();
        TestArchiveCandidatePreconditions();
        TestArchiveCandidatePublishRaces();
        TestWriterFailClosedContracts();
        TestStrictMissingListfileAndBounds();
        TestStrictEncryptedTableMutations();
        TestArchiveCandidateResourceBudgets();
        TestStrictListfileListsItself();
        TestStrictStaleListfileName();
        TestStrictCanonicalListfileCollision();
        TestStrictListfileRedundantDuplicate();
        TestBoundedArchiveReads();
        TestEmptySectorChecksumLayout();
        TestGameBoundedReadPriority();
        TestGameStrictMount();
        TestOwnedTilesetArchive();
        TestGameDataSourceKnownInventory();
        TestSoundInfoVerifiedDiscovery();
        TestSharedSlkParserModes();
        TestRealMapRepackRoundTrip();
        TestRealMapIfProvided();
        TestGameDataSourceIfProvided();
        std::cout << "War3MapEditor64 MPQ reader tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "TEST FAILURE: " << ex.what() << '\n';
        return 1;
    }
}
