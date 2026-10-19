#include "War3MapCore/Update/SemanticVersion.h"
#include "War3MapCore/Update/UpdateArchive.h"
#include "War3MapCore/Update/UpdateManifest.h"
#include "War3MapCore/Update/UpdatePlan.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "Core/Serialization/StrictJson.h"
#include "Core/Update/UpdatePath.h"

#include <Windows.h>
#include <winioctl.h>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

using namespace War3Map;
using namespace War3Map::Update;
using Json = nlohmann::json;
int failures = 0;
int checks = 0;

void Check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<typename T>
bool Code(const Result<T>& result, std::string_view code) {
    return !result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
template<typename T>
void PrintDiagnostics(const Result<T>& result, std::string_view fixture) {
    for (const auto& diagnostic : result.diagnostics) {
        std::cerr << "FIXTURE " << fixture << ": " << diagnostic.code << ": " << diagnostic.message;
        if (diagnostic.origin) std::cerr << " origin=" << IO::PathUtf8(diagnostic.origin->source);
        std::cerr << '\n';
    }
}
void PrintFixtureIdentity(const std::filesystem::path& path) {
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::cerr << "FIXTURE identity open-error=" << GetLastError() << " path=" << IO::PathUtf8(path) << '\n';
        return;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool stat = GetFileInformationByHandle(handle, &info) != FALSE;
    const auto size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::wstring finalPath(size ? size : 1, L'\0');
    const auto count = size ? GetFinalPathNameByHandleW(handle, finalPath.data(), size,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    CloseHandle(handle);
    if (count && count < size) finalPath.resize(count); else finalPath.clear();
    std::cerr << "FIXTURE identity stat=" << stat << " attr=" << info.dwFileAttributes << " links=" << info.nNumberOfLinks
        << " path=" << IO::PathUtf8(path) << " final=" << IO::PathUtf8(finalPath) << '\n';
}
std::span<const std::byte> Bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
std::string Hash(std::string_view text) {
    const auto hash = Sha256Text(text);
    if (!hash.Succeeded()) throw std::runtime_error("fixture SHA failed");
    return *hash.value;
}
UpdateFileEntry File(std::string path, std::string_view contents) {
    return {std::move(path), static_cast<std::uint64_t>(contents.size()), Hash(contents)};
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    if (!IO::WriteFileBytes(path, bytes).Succeeded()) throw std::runtime_error("fixture write failed");
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, Bytes(text)); }

class TempDirectory {
public:
    TempDirectory() {
        // 本机TEMP是8.3别名，标准temp_directory_path还保留末尾分隔符。
        // 测试夹具先通过句柄确定实际长路径；产品对reparse/别名的拒绝合同不放宽。
        const auto temporary = std::filesystem::temp_directory_path();
        const auto parentHandle = CreateFileW(temporary.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (parentHandle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open actual temporary fixture parent");
        const auto size = GetFinalPathNameByHandleW(parentHandle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        std::wstring finalPath(size ? size : 1, L'\0');
        const auto count = size ? GetFinalPathNameByHandleW(parentHandle, finalPath.data(), size,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
        CloseHandle(parentHandle);
        if (!count || count >= size) throw std::runtime_error("cannot resolve actual temporary fixture parent");
        finalPath.resize(count);
        if (!finalPath.starts_with(L"\\\\?\\") || finalPath.starts_with(L"\\\\?\\UNC\\"))
            throw std::runtime_error("temporary fixture parent must resolve to a local disk");
        finalPath.erase(0, 4);
        parent_ = std::filesystem::path(finalPath).lexically_normal();
        while (parent_.filename().empty() && parent_ != parent_.root_path()) parent_ = parent_.parent_path();
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = parent_ / (L"AudastraUpdateTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks));
        if (!path_.is_absolute() || path_.parent_path() != parent_ || !std::filesystem::create_directory(path_))
            throw std::runtime_error("cannot create exclusive temporary fixture directory");
    }
    ~TempDirectory() {
        // 只移除本测试独占创建的子目录；不递归删除计算出的外部路径或复用作者包。
        if (path_.is_absolute() && path_.parent_path() == parent_
            && path_.filename().native().starts_with(L"AudastraUpdateTests-")) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path parent_, path_;
};

struct Signer {
    TrustedUpdateKey key;
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secret{};
    explicit Signer(unsigned char variation = 0) {
        // 原创可重复测试seed，只在测试程序使用，不能编入发布公钥配置。
        std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<unsigned char>(0x41 + i + variation);
        if (crypto_sign_seed_keypair(reinterpret_cast<unsigned char*>(key.bytes.data()), secret.data(), seed.data()) != 0)
            throw std::runtime_error("fixture signing key failed");
        key.configured = true;
    }
    std::array<std::byte, crypto_sign_BYTES> Sign(std::string_view raw) const {
        std::array<std::byte, crypto_sign_BYTES> signature{};
        unsigned long long length = 0;
        if (crypto_sign_detached(reinterpret_cast<unsigned char*>(signature.data()), &length,
            reinterpret_cast<const unsigned char*>(raw.data()), static_cast<unsigned long long>(raw.size()), secret.data()) != 0
            || length != signature.size()) throw std::runtime_error("fixture sign failed");
        return signature;
    }
    Result<VerifiedUpdateManifest> Verify(std::string_view raw) const {
        const auto signature = Sign(raw);
        return VerifyAndParseUpdateManifest(Bytes(raw), signature, key);
    }
};

Json Manifest(std::string version, const std::vector<UpdateFileEntry>& files, std::uint64_t zipBytes = 30,
    std::string zipHash = Hash("original package fixture"), std::string minimum = "0.1.0") {
    Json rows = Json::array();
    for (const auto& file : files) rows.push_back({{"path", file.path}, {"bytes", file.bytes}, {"sha256", file.sha256}});
    return {{"format", "audastra-update"}, {"schema", 1}, {"product", "Audastra"}, {"platform", "windows-x64"},
        {"version", std::move(version)}, {"minimum_direct_upgrade", std::move(minimum)}, {"source_commit", std::string(40, 'a')},
        {"zip", {{"filename", "new.zip"}, {"bytes", zipBytes}, {"sha256", std::move(zipHash)}}},
        {"release_notes", "原创离线更新测试"}, {"files", std::move(rows)}};
}

std::vector<std::byte> Zip(const std::vector<std::pair<std::string, std::string>>& files, bool compress = true) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0)) throw std::runtime_error("fixture ZIP writer init failed");
    for (const auto& [path, contents] : files) {
        if (!mz_zip_writer_add_mem(&archive, path.c_str(), contents.data(), contents.size(),
            compress ? MZ_BEST_COMPRESSION : MZ_NO_COMPRESSION)) {
            mz_zip_writer_end(&archive);
            throw std::runtime_error("fixture ZIP writer add failed");
        }
    }
    void* memory = nullptr;
    size_t bytes = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &memory, &bytes)) {
        mz_zip_writer_end(&archive);
        throw std::runtime_error("fixture ZIP writer finalize failed");
    }
    const auto* begin = static_cast<const std::byte*>(memory);
    std::vector<std::byte> result(begin, begin + bytes);
    mz_free(memory);
    mz_zip_writer_end(&archive);
    return result;
}
std::uint32_t Little32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return 0;
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}
std::size_t Central(std::span<const std::byte> bytes) {
    for (std::size_t i = 0; i + 46 <= bytes.size(); ++i) if (Little32(bytes, i) == 0x02014b50) return i;
    throw std::runtime_error("fixture central header missing");
}
void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}

bool CreateJunction(const std::filesystem::path& junction, const std::filesystem::path& target) {
    struct MountPointBuffer {
        DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        WORD dataLength = 0, reserved = 0;
        WORD substituteOffset = 0, substituteLength = 0, printOffset = 0, printLength = 0;
        wchar_t path[2048]{};
    } buffer;
    const auto substitute = L"\\??\\" + target.native();
    const auto print = target.native();
    if (substitute.size() + print.size() + 2 > std::size(buffer.path)) return false;
    buffer.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    buffer.printOffset = static_cast<WORD>(buffer.substituteLength + sizeof(wchar_t));
    buffer.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    buffer.dataLength = static_cast<WORD>(8 + buffer.printOffset + buffer.printLength + sizeof(wchar_t));
    std::memcpy(buffer.path, substitute.data(), buffer.substituteLength);
    std::memcpy(reinterpret_cast<std::byte*>(buffer.path) + buffer.printOffset, print.data(), buffer.printLength);
    if (!std::filesystem::create_directory(junction)) return false;
    const auto handle = CreateFileW(junction.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const bool succeeded = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, buffer.dataLength + 8,
        nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return succeeded;
}

void SemVerTests() {
    const std::array ordered{"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "2.0.0"};
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        const auto a = ParseSemanticVersion(ordered[i - 1]), b = ParseSemanticVersion(ordered[i]);
        Check(a.Succeeded() && b.Succeeded() && CompareSemanticVersions(*a.value, *b.value) < 0,
            "SemVer prerelease/core precedence");
    }
    const auto a = ParseSemanticVersion("1.0.0+build.1"), b = ParseSemanticVersion("1.0.0+build.2");
    Check(a.Succeeded() && b.Succeeded() && CompareSemanticVersions(*a.value, *b.value) == 0, "build ignored");
    const auto huge = ParseSemanticVersion("9999999999999999999999999999999999999.0.0");
    const auto small = ParseSemanticVersion("999999999999999999999999999999999999.9.9");
    Check(huge.Succeeded() && small.Succeeded() && CompareSemanticVersions(*huge.value, *small.value) > 0, "core integers avoid overflow");
    const auto hugePre = ParseSemanticVersion("1.0.0-9999999999999999999999999999999");
    const auto smallPre = ParseSemanticVersion("1.0.0-999999999999999999999999999999");
    Check(hugePre.Succeeded() && smallPre.Succeeded() && CompareSemanticVersions(*hugePre.value, *smallPre.value) > 0,
        "prerelease integers avoid overflow");
    for (const auto* invalid : {"", "v1.0.0", "1.0", "1.0.0.0", "01.0.0", "1.01.0", "1.0.01", "1.0.0-01",
            "1.0.0-", "1.0.0+", "1.0.0-a..b", "1.0.0+a..b", "1.0.0+a+b", " 1.0.0", "1.0.0 ", "-1.0.0"}) {
        Check(!ParseSemanticVersion(invalid).Succeeded(), "invalid SemVer rejected");
    }
    Check(!ParseSemanticVersion(std::string(129, '1')).Succeeded(), "SemVer text bounded");
}

void JsonAndManifestTests(const Signer& signer) {
    using Serialization::ParseStrictJson;
    Check(!ParseStrictJson(Bytes("{\"x\":1,\"\\u0078\":2}")).Succeeded(), "decoded duplicate key rejected");
    Check(!ParseStrictJson(Bytes("{\"x\":\"\\ud800\"}")).Succeeded(), "unpaired surrogate rejected");
    const auto invalidUtf8 = std::string("{\"x\":\"") + std::string("\xff", 1) + "\"}";
    Check(!ParseStrictJson(Bytes(invalidUtf8)).Succeeded(), "invalid UTF8 rejected");
    Check(!ParseStrictJson(Bytes("{}{} ")).Succeeded(), "trailing JSON rejected");
    Check(!ParseStrictJson(Bytes("/*comment*/{}")).Succeeded(), "comments rejected");
    Check(!ParseStrictJson(Bytes(std::string("\xef\xbb\xbf{}"))).Succeeded(), "BOM rejected");
    Serialization::StrictJsonLimits limits;
    limits.maxDepth = 2;
    Check(!ParseStrictJson(Bytes("[[[]]]"), limits).Succeeded(), "JSON depth bounded");
    limits.maxNodes = 2;
    Check(!ParseStrictJson(Bytes("[1,2]"), limits).Succeeded(), "JSON node count bounded");
    limits = {};
    limits.maxStringBytes = 2;
    Check(!ParseStrictJson(Bytes("\"abc\""), limits).Succeeded(), "JSON string bounded");
    auto json = Manifest("0.2.0", {File("bin/app.exe", "new")});
    const auto raw = json.dump();
    auto signature = signer.Sign(raw);
    const auto valid = VerifyAndParseUpdateManifest(Bytes(raw), signature, signer.key);
    Check(valid.Succeeded(), "original fixture signature valid");
    if (!valid.Succeeded()) return;
    Check(valid.value->ManifestSha256() == Hash(raw), "raw manifest hash identity");
    Check(Code(VerifyAndParseUpdateManifest(Bytes(raw), signature, TrustedUpdateKey{}), "UPDATE_TRUST_NOT_CONFIGURED"), "no production trust rejects");
    Check(Code(VerifyAndParseUpdateManifest(Bytes(raw), {}, signer.key), "UPDATE_SIGNATURE_MISSING_OR_INVALID"), "missing signature rejects");
    Check(Code(VerifyAndParseUpdateManifest(Bytes(raw), std::span(signature).first(63), signer.key),
        "UPDATE_SIGNATURE_MISSING_OR_INVALID"), "short signature rejects");
    signature[0] ^= std::byte{1};
    Check(Code(VerifyAndParseUpdateManifest(Bytes(raw), signature, signer.key), "UPDATE_SIGNATURE_INVALID"), "wrong signature rejects");
    signature = signer.Sign(raw);
    Check(Code(VerifyAndParseUpdateManifest(Bytes(raw + "\n"), signature, signer.key), "UPDATE_SIGNATURE_INVALID"), "raw newline change rejects");
    const Signer other(7);
    Check(Code(VerifyAndParseUpdateManifest(Bytes(raw), signature, other.key), "UPDATE_SIGNATURE_INVALID"), "wrong trusted key rejects");
    Check(!ParseTrustedUpdateKeyHex("abc").Succeeded(), "short key rejected");
    Check(!ParseTrustedUpdateKeyHex(std::string(64, 'z')).Succeeded(), "nonhex key rejected");
    Check(ParseTrustedUpdateKeyHex("").Succeeded() && !ParseTrustedUpdateKeyHex("").value->configured, "unconfigured key explicit");
    const auto oversized = std::string(kMaxUpdateManifestBytes + 1, ' ');
    const auto oversizedSignature = signer.Sign(oversized);
    Check(Code(VerifyAndParseUpdateManifest(Bytes(oversized), oversizedSignature, signer.key), "UPDATE_MANIFEST_BUDGET"),
        "manifest byte budget before parser");
    auto modified = json;
    modified["unknown"] = true;
    Check(!signer.Verify(modified.dump()).Succeeded(), "unknown signed schema field rejects");
    modified = json;
    modified["zip"]["bytes"] = 1.5;
    Check(!signer.Verify(modified.dump()).Succeeded(), "fractional size rejected");
    modified = json;
    modified["zip"]["bytes"] = -1;
    Check(!signer.Verify(modified.dump()).Succeeded(), "negative size rejected");
    auto overflow = raw;
    const auto at = overflow.find("\"bytes\":30");
    if (at == std::string::npos) throw std::runtime_error("fixture integer missing");
    overflow.replace(at, std::string("\"bytes\":30").size(), "\"bytes\":18446744073709551616");
    Check(!signer.Verify(overflow).Succeeded(), "overflow size rejected");
    modified = json;
    modified["files"].push_back(modified["files"][0]);
    Check(!signer.Verify(modified.dump()).Succeeded(), "duplicate file rejected");
    modified = json;
    auto row = modified["files"][0];
    row["path"] = "BIN/APP.exe";
    modified["files"].push_back(row);
    Check(!signer.Verify(modified.dump()).Succeeded(), "case collision rejected");
    modified = json;
    row = modified["files"][0];
    row["path"] = "BIN/another.exe";
    modified["files"].push_back(row);
    Check(!signer.Verify(modified.dump()).Succeeded(), "shared directory case alias rejected");
    modified = json;
    modified["files"].push_back({{"path", "bin"}, {"bytes", 0}, {"sha256", Hash("")}});
    Check(!signer.Verify(modified.dump()).Succeeded(), "file-parent collision rejected");
    modified = json;
    modified["minimum_direct_upgrade"] = "0.3.0";
    Check(!signer.Verify(modified.dump()).Succeeded(), "minimum above target rejects");
    Check(CheckUpdate(*valid.value, "0.1.0").value->updateAvailable, "new stable detected");
    Check(!CheckUpdate(*valid.value, "0.2.0+local").value->updateAvailable, "equal precedence not update");
    Check(CheckUpdate(*valid.value, "0.0.9").value->updateAvailable
        && !CheckUpdate(*valid.value, "0.0.9").value->directlyUpgradeable, "minimum upgrade boundary");
    const auto beta = signer.Verify(Manifest("0.3.0-beta.2", {File("bin/app.exe", "new")}).dump());
    Check(beta.Succeeded() && !CheckUpdate(*beta.value, "0.2.0").value->updateAvailable
        && CheckUpdate(*beta.value, "0.2.0", UpdateChannel::Beta).value->updateAvailable, "channel prerelease boundary");
    for (const auto* path : {"../evil", "/absolute", "C:/escape", "C:escape", "//server/share", "bin\\x.exe", "bin/a:stream",
            "bin//a", "bin/./a", "bin/../a", "bin/NUL.txt", "COM1", "CON .txt", "aux", "bin/a.", "bin/a ", "bin/conout$"})
        Check(!ValidateUpdateRelativePath(path).Succeeded(), "unsafe signed path rejected");
}

void PackageTests(const Signer& signer) {
    TempDirectory temp;
    const std::string contents(16384, 'A');
    auto archive = Zip({{"bin/app.exe", contents}});
    Check(archive.size() > 10 && std::to_integer<unsigned char>(archive[8]) == 8, "fixture is actual Deflate ZIP");
    const auto package = temp.Path() / L"new.zip";
    Write(package, archive);
    auto hash = Sha256(archive);
    auto signedManifest = signer.Verify(Manifest("0.2.0", {File("bin/app.exe", contents)}, archive.size(), *hash.value).dump());
    Check(signedManifest.Succeeded(), "package signed fixture constructed");
    if (!signedManifest.Succeeded()) return;
    const auto verified = VerifyUpdatePackage(*signedManifest.value, package);
    if (!verified.Succeeded()) {
        PrintDiagnostics(verified, "positive-package");
        PrintFixtureIdentity(package);
        PrintFixtureIdentity(temp.Path());
        PrintFixtureIdentity(temp.Path().root_path());
    }
    Check(verified.Succeeded() && verified.value->files.size() == 1 && verified.value->expandedBytes == contents.size(), "Deflate size/SHA/CRC full verification");
    auto wrongSize = signer.Verify(Manifest("0.2.0", {File("bin/app.exe", contents)}, archive.size() + 1, *hash.value).dump());
    Check(Code(VerifyUpdatePackage(*wrongSize.value, package), "UPDATE_PACKAGE_SIZE_MISMATCH"), "package size mismatch");
    auto wrongHash = signer.Verify(Manifest("0.2.0", {File("bin/app.exe", contents)}, archive.size(), Hash("wrong")).dump());
    Check(Code(VerifyUpdatePackage(*wrongHash.value, package), "UPDATE_PACKAGE_SHA_MISMATCH"), "package SHA mismatch");

    const auto verifyArchive = [&](std::vector<std::byte> bytes, std::vector<UpdateFileEntry> files) {
        Write(package, bytes);
        const auto digest = Sha256(bytes);
        const auto manifest = signer.Verify(Manifest("0.2.0", files, bytes.size(), *digest.value).dump());
        if (!manifest.Succeeded()) throw std::runtime_error("archive test manifest invalid");
        return VerifyUpdatePackage(*manifest.value, package);
    };
    Check(verifyArchive(Zip({{"bin/app.exe", contents}}, false), {File("bin/app.exe", contents)}).Succeeded(), "stored ZIP accepted");
    Check(Code(verifyArchive(Zip({{"bin/app.exe", contents}, {"extra.txt", "injected"}}), {File("bin/app.exe", contents)}),
        "UPDATE_ZIP_EXTRA_MEMBER"), "extra ZIP member rejected");
    Check(Code(verifyArchive(Zip({{"bin/app.exe", contents}, {"BIN/APP.exe", contents}}), {File("bin/app.exe", contents)}),
        "UPDATE_ZIP_PATH_COLLISION"), "ZIP case collision rejected");
    Check(Code(verifyArchive(Zip({{"bin/app.exe", contents}, {"bin/app.exe", contents}}), {File("bin/app.exe", contents)}),
        "UPDATE_ZIP_PATH_COLLISION"), "ZIP duplicate rejected");
    Check(Code(verifyArchive(archive, {File("bin/app.exe", std::string(contents.size(), 'B'))}), "UPDATE_ZIP_MEMBER_SHA_MISMATCH"), "inner SHA mismatch");
    Check(Code(verifyArchive(archive, {File("bin/app.exe", "small")}), "UPDATE_ZIP_MEMBER_SIZE_MISMATCH"), "expanded size mismatch/bomb denied");
    Check(Code(verifyArchive(archive, {File("bin/app.exe", contents), File("missing", "x")}), "UPDATE_ZIP_MISSING_MEMBER"), "missing signed member");
    auto changedName = archive;
    changedName.at(30) = std::byte{'B'};
    Check(Code(verifyArchive(changedName, {File("bin/app.exe", contents)}), "UPDATE_ZIP_HEADERS_INVALID"), "local-central names mismatch");
    auto changedCrc = archive;
    const auto wrongCrc = Little32(changedCrc, 14) ^ 0x01000000u;
    Put32(changedCrc, 14, wrongCrc);
    Put32(changedCrc, Central(changedCrc) + 16, wrongCrc);
    const auto incoherentCrc = verifyArchive(changedCrc, {File("bin/app.exe", contents)});
    PrintDiagnostics(incoherentCrc, "incoherent-CRC-header");
    Check(Code(incoherentCrc, "UPDATE_ZIP_HEADERS_INVALID"), "incoherent local CRC with zero descriptor-header sizes rejected");
    // miniz原创writer使用bit3和16字节data descriptor，local CRC/size保持0。
    // 前一夹具改local CRC会先破坏header一致性；本夹具只同步改central/descriptor，
    // 让原decoded-CRC断言确实进入完整解压CRC门禁，而不改产品检查或原断言代码。
    changedCrc = archive;
    const auto crcCentral = Central(changedCrc);
    if (crcCentral < 16 || Little32(changedCrc, crcCentral - 16) != 0x08074b50)
        throw std::runtime_error("original CRC fixture must have a 16-byte data descriptor");
    const auto coherentWrongCrc = Little32(changedCrc, crcCentral + 16) ^ 0x01000000u;
    Put32(changedCrc, crcCentral + 16, coherentWrongCrc);
    Put32(changedCrc, crcCentral - 12, coherentWrongCrc);
    const auto decodedCrc = verifyArchive(changedCrc, {File("bin/app.exe", contents)});
    if (!Code(decodedCrc, "UPDATE_ZIP_CONTENT_INVALID")) PrintDiagnostics(decodedCrc, "decoded-CRC");
    Check(Code(decodedCrc, "UPDATE_ZIP_CONTENT_INVALID"), "decoded CRC mismatch rejected");
    auto symlink = archive;
    const auto central = Central(symlink);
    symlink[central + 5] = std::byte{3};
    Put32(symlink, central + 38, 0xa1ff0000);
    Check(Code(verifyArchive(symlink, {File("bin/app.exe", contents)}), "UPDATE_ZIP_MEMBER_TYPE_REJECTED"), "Unix symlink rejected");
    Check(!verifyArchive(Zip({{"../escape", "x"}}), {File("bin/app.exe", contents)}).Succeeded(), "ZIP traversal rejected");
    Check(!verifyArchive(Zip({{"bin/a:stream", "x"}}), {File("bin/app.exe", contents)}).Succeeded(), "ZIP ADS rejected");
    Write(package, archive);
    Check(Code(VerifyUpdatePackage(*signedManifest.value, L"\\\\server\\share\\new.zip"), "UPDATE_LOCAL_PATH_INVALID"), "UNC package rejected before network");
    Write(std::filesystem::path(package.native() + L":fixture"), "stream");
    Check(Code(VerifyUpdatePackage(*signedManifest.value, package), "UPDATE_ADS_REJECTED"), "actual NTFS ADS package rejected");
}

void PathAncestorStreamTests(const Signer& signer) {
    TempDirectory temp;
    const auto install = temp.Path() / L"install";
    const auto package = temp.Path() / L"cache/new.zip";
    const auto archive = Zip({{"bin/app.exe", "original new program"}});
    Write(package, archive);
    Write(install / L"user.txt", "original user content");
    const auto digest = Sha256(archive);
    const auto target = signer.Verify(Manifest("0.2.0", {File("bin/app.exe", "original new program")},
        archive.size(), *digest.value).dump());
    Check(target.Succeeded(), "ancestor ADS signed fixture valid");
    if (!target.Succeeded()) return;
    // 只写原创独占夹具的stream；不读取或修改本机既有祖先stream内容。
    const auto ancestorStream = std::filesystem::path(temp.Path().native() + L":fixture-unrelated");
    Write(ancestorStream, "original ancestor metadata");
    Check(VerifyUpdatePackage(*target.value, package).Succeeded(), "unrelated ancestor ADS does not block safe package");
    Check(BuildUpdateInstallPlan(*target.value, nullptr, install, "0.1.0").Succeeded(),
        "unrelated ancestor ADS does not block safe install inventory");
    const auto installStream = std::filesystem::path(install.native() + L":fixture-target");
    Write(installStream, "original target directory metadata");
    Check(Code(BuildUpdateInstallPlan(*target.value, nullptr, install, "0.1.0"), "UPDATE_ADS_REJECTED"),
        "actual target install directory ADS rejected");
    Check(DeleteFileW(installStream.c_str()) != FALSE, "only original target fixture ADS removed");
    std::filesystem::create_directory(install / L"user-directory");
    Write(std::filesystem::path((install / L"user-directory").native() + L":fixture-child"), "original child metadata");
    Check(Code(BuildUpdateInstallPlan(*target.value, nullptr, install, "0.1.0"), "UPDATE_ADS_REJECTED"),
        "actual inventory child directory ADS rejected");
    Check(Sha256File(install / L"user.txt").value == std::optional<std::string>(Hash("original user content")),
        "ADS refusal leaves original user file bytes intact");
}

void PathDirectoryLeaseTests() {
    TempDirectory temp;
    const auto ancestor = temp.Path() / L"leased-parent", directory = ancestor / L"leaf",
        movedDirectory = ancestor / L"leaf-moved", movedAncestor = temp.Path() / L"parent-moved";
    Write(directory / L"user.txt", "original directory lease user bytes");
    auto directoryLease = Detail::LockSafeDirectory(directory);
    Check(directoryLease.Succeeded(), "real directory and ancestor no-delete lease acquired");
    if (!directoryLease.Succeeded()) return;
    const bool directoryMoved = MoveFileExW(directory.c_str(), movedDirectory.c_str(), 0) != FALSE;
    const auto directoryError = directoryMoved ? ERROR_SUCCESS : GetLastError();
    Check(!directoryMoved && directoryError == ERROR_SHARING_VIOLATION,
        "held directory lease actually prevents native target rename with sharing violation");
    if (directoryMoved && !MoveFileExW(movedDirectory.c_str(), directory.c_str(), 0))
        throw std::runtime_error("fixture lease target restore failed");
    const bool ancestorMoved = MoveFileExW(ancestor.c_str(), movedAncestor.c_str(), 0) != FALSE;
    const auto ancestorError = ancestorMoved ? ERROR_SUCCESS : GetLastError();
    Check(!ancestorMoved && ancestorError == ERROR_SHARING_VIOLATION,
        "held child lease actually prevents native ancestor rename with sharing violation");
    if (ancestorMoved && !MoveFileExW(movedAncestor.c_str(), ancestor.c_str(), 0))
        throw std::runtime_error("fixture lease ancestor restore failed");
    auto fileLease = Detail::OpenSafeReadFile(directory / L"user.txt", 1024);
    Check(fileLease.Succeeded(), "safe file reader reuses real ancestor directory leases");
    directoryLease.value.reset();
    if (!fileLease.Succeeded()) return;
    const bool fileParentMoved = MoveFileExW(ancestor.c_str(), movedAncestor.c_str(), 0) != FALSE;
    const auto fileParentError = fileParentMoved ? ERROR_SUCCESS : GetLastError();
    Check(!fileParentMoved && fileParentError == ERROR_SHARING_VIOLATION,
        "file read lease alone prevents ancestor swap until natural handle release");
    if (fileParentMoved && !MoveFileExW(movedAncestor.c_str(), ancestor.c_str(), 0))
        throw std::runtime_error("fixture file parent restore failed");
    fileLease.value.reset();
    Check(MoveFileExW(ancestor.c_str(), movedAncestor.c_str(), 0) != FALSE, "native ancestor rename succeeds once owned leases are released");
    Check(Sha256File(movedAncestor / L"leaf/user.txt").value == std::optional<std::string>(Hash("original directory lease user bytes")),
        "actual lease refusal and later rename preserve original user bytes");
}

std::filesystem::path ZonePath(const std::filesystem::path& path) {
    return std::filesystem::path(path.native() + Detail::kZoneIdentifierSuffix);
}
bool OriginalZone(const Detail::OriginStreamReadLease& lease, std::string_view bytes) {
    return lease.stream.Valid() && lease.zoneIdentifier
        && lease.zoneIdentifier->bytes == std::vector<std::byte>(Bytes(bytes).begin(), Bytes(bytes).end())
        && lease.zoneIdentifier->sha256 == Hash(bytes)
        && Detail::OriginMatches(lease, &*lease.zoneIdentifier) && !Detail::OriginMatches(lease, nullptr);
}

void OriginMetadataFormatTests() {
    TempDirectory temp;
    const auto file = temp.Path() / L"original.txt", zone = ZonePath(file);
    Write(file, "original default stream");
    {
        const auto absent = Detail::OpenSafeReadFile(file, 1024);
        Check(absent.Succeeded() && !absent.value->origin.zoneIdentifier && !absent.value->origin.stream.Valid()
            && Detail::OriginMatches(absent.value->origin, nullptr), "absent origin metadata stays absent with no invented lease");
    }
    const auto accepted = [&](std::string_view text) {
        Write(zone, text);
        const auto lease = Detail::OpenSafeReadFile(file, 1024);
        Check(lease.Succeeded() && OriginalZone(lease.value->origin, text),
            "supported original Zone metadata accepted with exact raw bytes and SHA");
    };
    for (const char id : {'0', '1', '2', '3', '4'})
        accepted(std::string("[ZoneTransfer]\r\nZoneId=") + id + "\r\nHostUrl=https://origin.invalid/original?a=1&b=2\r\n");
    accepted(" \t[zonetransfer]\n ZoNeId \t= 3\t\nReferrerUrl=https://referrer.invalid/original\n");
    accepted(std::string("\xef\xbb\xbf") + "[ZoneTransfer]\r\nZoneId=3\r\n");
    const auto mixedCase = temp.Path() / L"case-original.txt";
    const std::string mixedCaseBytes = "[ZoneTransfer]\r\nZoneId=3\r\n";
    Write(mixedCase, "original case-insensitive stream fixture");
    Write(std::filesystem::path(mixedCase.native() + L":zOnE.iDeNtIfIeR:$DATA"), mixedCaseBytes);
    {
        const auto lease = Detail::OpenSafeReadFile(mixedCase, 1024);
        Check(lease.Succeeded() && OriginalZone(lease.value->origin, mixedCaseBytes),
            "only the fixed origin stream name is accepted case-insensitively");
    }
    std::string utf16("\xff\xfe", 2);
    for (const char c : std::string("[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://origin.invalid/wide\r\n")) {
        utf16.push_back(c); utf16.push_back('\0');
    }
    accepted(utf16);
    const auto wideBytes = [](std::wstring_view text) {
        std::string result("\xff\xfe", 2);
        for (const auto c : text) { result.push_back(static_cast<char>(c & 0xff)); result.push_back(static_cast<char>(c >> 8)); }
        return result;
    };
    accepted(wideBytes(L"[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://origin.invalid/\u4e2d\u6587\r\n"));
    // \u89e3\u538b\u5de5\u5177\u5199\u5165\u7684\u7ed3\u5c3eNUL\uff1a\u672c\u673a\u8def\u5f84\u6765\u6e90\u3001\u65e0\u7ed3\u5c3e\u6362\u884c\u3002
    accepted(std::string("[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=C:\\Download\\fixture.zip") + '\0');
    accepted(std::string("[ZoneTransfer]\nZoneId=3\n") + '\0' + '\0');
    accepted(wideBytes(L"[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=C:\\Download\\fixture.zip") + std::string(2, '\0'));
    const std::string prefix = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=";
    const auto limit = static_cast<std::size_t>(Detail::kMaxZoneIdentifierBytes);
    Check(limit > prefix.size(), "origin metadata boundary fixture has room for a URL value");
    if (limit <= prefix.size()) return;
    const auto bounded = prefix + std::string(limit - prefix.size(), 'a');
    accepted(std::string_view(bounded).substr(0, limit - 1));
    accepted(bounded); // 16KiB为包含式上限；+1必须拒绝。
    const std::vector<std::string> rejected{
        "", "[ZoneTransfer]\r\n", "[ZoneTransfer]\r\nHostUrl=https://origin.invalid/no-zone\r\n",
        "ZoneId=3\r\n", "[Other]\r\nZoneId=3\r\n", "[ZoneTransfer]\r\nZoneId=3\r\nZoneId=4\r\n",
        "[ZoneTransfer]\r\nZoneId=3\r\nzoneid=3\r\n", "[ZoneTransfer]\r\nZoneId=3\r\n[ZoneTransfer]\r\nZoneId=3\r\n",
        "[ZoneTransfer]\r\nZoneId=3\r\n[Other]\r\nEntry=value\r\n", "[ZoneTransfer]\r\nZoneId=5\r\n",
        "[ZoneTransfer]\r\nZoneId=-1\r\n", "[ZoneTransfer]\r\nZoneId=00\r\n", "[ZoneTransfer]\r\nZoneId=\r\n",
        "[ZoneTransfer]\r\nZoneId=x\r\n", std::string(1, '\0'),
        std::string("[ZoneTransfer]\nZoneId=3\n") + '\0' + "HostUrl=https://origin.invalid/inner-nul\n",
        wideBytes(L"[ZoneTransfer]\r\nZoneId=3\r\n") + std::string(2, '\0') + wideBytes(L"x").substr(2),
        std::string("[ZoneTransfer]\nZoneId=3\nHostUrl=") + char(1),
        std::string("[ZoneTransfer]\nZoneId=3\nHostUrl=") + char(127),
        wideBytes(L"[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=\x0001"),
        wideBytes(L"[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=\x007f"),
        wideBytes(L"[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=\xd800"), bounded + 'a', utf16 + 'x'};
    for (const auto& text : rejected) {
        Write(zone, text);
        Check(Code(Detail::OpenSafeReadFile(file, 1024), "UPDATE_ORIGIN_METADATA_INVALID"),
            "empty ambiguous malformed or oversized Zone metadata rejected before a safe read");
    }
    Check(Sha256File(file).value == std::optional<std::string>(Hash("original default stream")),
        "origin metadata format checks never modify the original default stream");
}

void OriginNamedStreamLeaseTests() {
    TempDirectory temp;
    const auto directory = temp.Path() / L"origin-directory", file = directory / L"origin-file.txt";
    Write(file, "original named-stream lease data");
    const std::string original = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://origin.invalid/lease\r\n";
    const std::string changed = "[ZoneTransfer]\r\nZoneId=4\r\nHostUrl=https://origin.invalid/changed\r\n";
    Write(ZonePath(directory), original); Write(ZonePath(file), original);
    const auto cannotMutate = [&](const std::filesystem::path& path) {
        const auto stream = ZonePath(path);
        const HANDLE writer = CreateFileW(stream.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        const auto openError = writer == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        bool written = false;
        if (writer != INVALID_HANDLE_VALUE) {
            DWORD bytes = 0;
            written = WriteFile(writer, changed.data(), static_cast<DWORD>(changed.size()), &bytes, nullptr) != FALSE;
            CloseHandle(writer);
        }
        Check(writer == INVALID_HANDLE_VALUE && openError == ERROR_SHARING_VIOLATION && !written,
            "named origin read lease actually refuses a native writer instead of relying on the default-stream handle");
        const bool removed = DeleteFileW(stream.c_str()) != FALSE;
        const auto deleteError = removed ? ERROR_SUCCESS : GetLastError();
        Check(!removed && deleteError == ERROR_SHARING_VIOLATION,
            "named origin read lease actually refuses native stream deletion");
    };
    {
        const auto held = Detail::LockSafeDirectory(directory);
        Check(held.Succeeded() && OriginalZone(held.value->origin, original), "directory owns its actual named-stream read lease");
        if (held.Succeeded()) cannotMutate(directory);
    }
    Write(ZonePath(directory), changed);
    {
        const auto held = Detail::LockSafeDirectory(directory);
        Check(held.Succeeded() && OriginalZone(held.value->origin, changed), "released directory lease allows a new original metadata snapshot");
    }
    {
        const auto held = Detail::OpenSafeReadFile(file, 1024);
        Check(held.Succeeded() && OriginalZone(held.value->origin, original)
            && OriginalZone(held.value->parent.origin, changed), "file and its parent hold separate original metadata leases");
        if (held.Succeeded()) cannotMutate(file);
    }
    Write(ZonePath(file), changed);
    {
        const auto held = Detail::OpenSafeReadFile(file, 1024);
        Check(held.Succeeded() && OriginalZone(held.value->origin, changed), "released file lease allows changed origin bytes to be read and rehashed");
    }
    const auto stream = ZonePath(file);
    Detail::NativeHandle preexistingWriter(CreateFileW(stream.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    Check(preexistingWriter.Valid(), "original preexisting named-stream writer acquired");
    if (preexistingWriter.Valid()) {
        DWORD count = 0;
        Check(WriteFile(preexistingWriter.Get(), changed.data(), static_cast<DWORD>(changed.size()), &count, nullptr) != FALSE
            && count == changed.size(), "preexisting native writer fixture can actually write its original named stream");
        Check(Code(Detail::OpenSafeReadFile(file, 1024), "UPDATE_STREAM_INSPECTION_FAILED"),
            "an already-open native named-stream writer prevents granting a false exclusive read lease");
    }
    preexistingWriter = Detail::NativeHandle{};
    Check(DeleteFileW(stream.c_str()) != FALSE, "native stream deletion succeeds after every owned lease is released");
    const auto absent = Detail::OpenSafeReadFile(file, 1024);
    Check(absent.Succeeded() && !absent.value->origin.zoneIdentifier && Detail::OriginMatches(absent.value->origin, nullptr),
        "deleted metadata is recorded as absent instead of retaining a stale prior hash");
    Check(Sha256File(file).value == std::optional<std::string>(Hash("original named-stream lease data")),
        "named-stream refusal release and deletion leave default-stream content unchanged");
}

void OriginPackageAndPlanTests(const Signer& signer) {
    TempDirectory temp;
    const std::string zone = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://origin.invalid/package\r\n";
    const auto cache = temp.Path() / L"cache", package = cache / L"original.zip",
        manifestPath = cache / L"original.json", signaturePath = cache / L"original.sig",
        install = temp.Path() / L"install", userDirectory = install / L"user", userFile = userDirectory / L"original.txt";
    const auto archive = Zip({{"bin/app.exe", "original new program"}});
    const auto digest = Sha256(archive);
    const auto raw = Manifest("0.2.0", {File("bin/app.exe", "original new program")}, archive.size(), *digest.value).dump();
    const auto signature = signer.Sign(raw);
    const auto target = signer.Verify(raw);
    const auto baseline = signer.Verify(Manifest("0.1.0", {File("bin/app.exe", "original old program")}).dump());
    Check(target.Succeeded() && baseline.Succeeded(), "original MOTW package and baseline signatures constructed");
    if (!target.Succeeded() || !baseline.Succeeded()) return;
    Write(package, archive); Write(manifestPath, raw); Write(signaturePath, signature);
    Write(install / L"bin/app.exe", "original old program"); Write(userFile, "original user bytes");
    const auto plainPlan = BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0");
    Check(plainPlan.Succeeded(), "original no-origin inventory plan available for metadata identity comparison");
    for (const auto& path : {cache, package, manifestPath, signaturePath, install, userDirectory, userFile, install / L"bin/app.exe"})
        Write(ZonePath(path), zone);
    {
        auto manifestLease = Detail::OpenSafeReadFile(manifestPath, 1024 * 1024);
        auto signatureLease = Detail::OpenSafeReadFile(signaturePath, 64);
        Check(manifestLease.Succeeded() && signatureLease.Succeeded(), "signed manifest and signature accept bounded original Zone metadata");
        if (manifestLease.Succeeded() && signatureLease.Succeeded()) {
            const auto readHeld = [](Detail::FileReadLease& lease) {
                std::vector<std::byte> bytes(static_cast<std::size_t>(lease.bytes)); DWORD count = 0;
                if (!ReadFile(lease.file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
                    throw std::runtime_error("original leased signed-input read failed");
                return bytes;
            };
            const auto manifestBytes = readHeld(*manifestLease.value), signatureBytes = readHeld(*signatureLease.value);
            Check(OriginalZone(manifestLease.value->origin, zone) && OriginalZone(signatureLease.value->origin, zone)
                && OriginalZone(manifestLease.value->parent.origin, zone)
                && VerifyAndParseUpdateManifest(manifestBytes, signatureBytes, signer.key).Succeeded(),
                "MOTW signed inputs retain exact metadata and still pass the real signature parser");
        }
    }
    const auto verified = VerifyUpdatePackage(*target.value, package);
    Check(verified.Succeeded() && verified.value->sha256 == *digest.value,
        "legal package and parent Zone metadata do not alter signed ZIP SHA or verification");
    const auto plan = BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0");
    Check(plan.Succeeded() && plainPlan.Succeeded() && plan.value->inventorySha256 != plainPlan.value->inventorySha256,
        "legal root directory program and user Zone metadata enter the real inventory identity");
    if (plan.Succeeded()) {
        const auto user = std::find_if(plan.value->files.begin(), plan.value->files.end(), [](const auto& entry) { return entry.path == "user/original.txt"; });
        Check(user != plan.value->files.end() && user->action == UpdateFileAction::PreserveUserFile
            && user->currentSha256 == Hash("original user bytes"), "origin-marked user bytes remain a preserved user file");
        Write(ZonePath(userFile), "[ZoneTransfer]\r\nZoneId=4\r\n");
        const auto changed = BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0");
        Check(changed.Succeeded() && changed.value->inventorySha256 != plan.value->inventorySha256,
            "changing only original user metadata changes the actual inventory hash");
        Check(DeleteFileW(ZonePath(userFile).c_str()) != FALSE, "only original user Zone fixture deleted for absence comparison");
        const auto absent = BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0");
        Check(absent.Succeeded() && changed.Succeeded() && absent.value->inventorySha256 != changed.value->inventorySha256,
            "Zone metadata absence differs from presence in the actual inventory");
        Write(ZonePath(userFile), zone);
    }
    for (const auto& path : {package, manifestPath, install, userDirectory, userFile}) {
        const auto unknown = std::filesystem::path(path.native() + L":original-unknown");
        Write(unknown, "original unknown stream alongside valid Zone");
        if (path == package) Check(Code(VerifyUpdatePackage(*target.value, package), "UPDATE_ADS_REJECTED"),
            "legal Zone plus unknown package ADS remains refused");
        else if (path == manifestPath) Check(Code(Detail::OpenSafeReadFile(manifestPath, 1024 * 1024), "UPDATE_ADS_REJECTED"),
            "legal Zone plus unknown signed-input ADS remains refused");
        else Check(Code(BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0"), "UPDATE_ADS_REJECTED"),
            "legal Zone plus unknown root child-directory or user-file ADS remains refused");
        Check(DeleteFileW(unknown.c_str()) != FALSE, "only original unknown stream fixture removed");
    }
    Check(VerifyUpdatePackage(*target.value, package).Succeeded()
        && BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0").Succeeded(),
        "unknown stream refusal does not destroy legal Zone metadata or valid update inputs");
    const auto wrong = signer.Verify(Manifest("0.2.0", {File("bin/app.exe", "original new program")}, archive.size(), Hash("wrong signed ZIP")).dump());
    Check(wrong.Succeeded() && Code(VerifyUpdatePackage(*wrong.value, package), "UPDATE_PACKAGE_SHA_MISMATCH"),
        "legal origin metadata never relaxes the signed package SHA gate");
    Check(Sha256File(userFile).value == std::optional<std::string>(Hash("original user bytes"))
        && Sha256File(install / L"bin/app.exe").value == std::optional<std::string>(Hash("original old program")),
        "all original origin planning and refusal cases leave user and program default bytes intact");
}

void PlanTests(const Signer& signer) {
    TempDirectory temp;
    const auto install = temp.Path() / L"install";
    Write(install / L"bin/app.exe", "old");
    Write(install / L"docs/custom.txt", "author file");
    Write(install / L"settings.json", "local settings");
    Write(install / L"logs/session.log", "local log");
    Write(install / L"feedback/queued.json", "private queue");
    Write(install / L"obsolete.dll", "old library");
    std::filesystem::create_directories(install / L"empty-user-directory");
    const auto baseline = signer.Verify(Manifest("0.1.0", {File("bin/app.exe", "old"), File("obsolete.dll", "old library")}).dump());
    const auto target = signer.Verify(Manifest("0.2.0", {File("bin/app.exe", "new"), File("licenses/new.txt", "license")}).dump());
    Check(baseline.Succeeded() && target.Succeeded(), "plan fixtures valid");
    if (!baseline.Succeeded() || !target.Succeeded()) return;
    const auto plan = BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0");
    if (!plan.Succeeded()) {
        PrintDiagnostics(plan, "positive-plan");
        PrintFixtureIdentity(install);
    }
    Check(plan.Succeeded(), "trusted baseline produces plan");
    if (!plan.Succeeded()) return;
    const auto find = [&](std::string_view name) {
        return std::find_if(plan.value->files.begin(), plan.value->files.end(), [&](const auto& file) { return file.path == name; });
    };
    Check(find("bin/app.exe") != plan.value->files.end() && find("bin/app.exe")->action == UpdateFileAction::ReplaceProgramFile,
        "owned unmodified program replaced");
    Check(find("licenses/new.txt") != plan.value->files.end() && find("licenses/new.txt")->action == UpdateFileAction::CreateProgramFile,
        "new program file created");
    for (const auto* user : {"docs/custom.txt", "settings.json", "logs/session.log", "feedback/queued.json"})
        Check(find(user) != plan.value->files.end() && find(user)->action == UpdateFileAction::PreserveUserFile, "user content preserved");
    Check(find("obsolete.dll") != plan.value->files.end() && find("obsolete.dll")->action == UpdateFileAction::PreserveObsoleteProgramFile,
        "obsolete program not silently deleted");
    Check(std::find(plan.value->directories.begin(), plan.value->directories.end(), "empty-user-directory") != plan.value->directories.end(),
        "empty user directory retained");
    Check(Sha256File(install / L"bin/app.exe").value == std::optional<std::string>(Hash("old"))
        && !std::filesystem::exists(install / L"licenses/new.txt"), "plan makes no writes");
    const auto repeated = BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0");
    Check(repeated.Succeeded() && repeated.value->inventorySha256 == plan.value->inventorySha256, "stable inventory identity");
    Check(Code(BuildUpdateInstallPlan(*target.value, nullptr, install, "0.1.0"), "UPDATE_BASELINE_UNTRUSTED"), "unsigned old manifest cannot authorize overwrite");
    const auto collision = signer.Verify(Manifest("0.2.0", {File("docs/custom.txt", "overwrite")}).dump());
    Check(Code(BuildUpdateInstallPlan(*collision.value, &*baseline.value, install, "0.1.0"), "UPDATE_USER_FILE_CONFLICT"), "new program/user collision refused");
    const auto prefix = signer.Verify(Manifest("0.2.0", {File("settings.json/child", "overwrite")}).dump());
    Check(Code(BuildUpdateInstallPlan(*prefix.value, &*baseline.value, install, "0.1.0"), "UPDATE_USER_PATH_CONFLICT"), "file as new parent refused");
    const auto folder = signer.Verify(Manifest("0.2.0", {File("docs", "overwrite")}).dump());
    Check(Code(BuildUpdateInstallPlan(*folder.value, &*baseline.value, install, "0.1.0"), "UPDATE_USER_PATH_CONFLICT"), "directory overwritten by file refused");
    Write(install / L"bin/app.exe", "author changed program");
    Check(Code(BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0"), "UPDATE_PROGRAM_FILE_MODIFIED"), "modified owned file refused");
    Write(install / L"bin/app.exe", "old");
    const Signer other(11);
    const auto wrongPublisher = other.Verify(Manifest("0.1.0", {File("bin/app.exe", "old")}).dump());
    Check(Code(BuildUpdateInstallPlan(*target.value, &*wrongPublisher.value, install, "0.1.0"), "UPDATE_BASELINE_IDENTITY_MISMATCH"), "baseline publisher mismatch refused");
    Write(install / L"docs/custom.txt:secret", "hidden stream");
    Check(Code(BuildUpdateInstallPlan(*target.value, &*baseline.value, install, "0.1.0"), "UPDATE_ADS_REJECTED"), "actual user ADS refused without loss");
    Check(Code(BuildUpdateInstallPlan(*target.value, &*baseline.value, L"\\\\server\\share\\install", "0.1.0"), "UPDATE_LOCAL_PATH_INVALID"), "UNC install refused before network");
    const auto empty = temp.Path() / L"empty-install";
    std::filesystem::create_directory(empty);
    Check(BuildUpdateInstallPlan(*target.value, nullptr, empty, "0.1.0").Succeeded(), "no collision can add signed program files");
    const auto linked = temp.Path() / L"linked-install";
    std::filesystem::create_directory(linked);
    Write(linked / L"original", "author hard link");
    Check(CreateHardLinkW((linked / L"alias").c_str(), (linked / L"original").c_str(), nullptr) != FALSE, "original hardlink fixture created");
    Check(Code(BuildUpdateInstallPlan(*target.value, nullptr, linked, "0.1.0"), "UPDATE_REPARSE_OR_PATH_REJECTED"), "hardlinked user files refused");
    const auto reparseInstall = temp.Path() / L"reparse-install";
    const auto linkedTarget = temp.Path() / L"junction-target";
    std::filesystem::create_directory(reparseInstall);
    Write(linkedTarget / L"author.txt", "junction target remains intact");
    const auto junction = reparseInstall / L"redirect";
    const bool created = CreateJunction(junction, linkedTarget);
    Check(created, "original reparse junction fixture created");
    if (created) {
        Check(Code(BuildUpdateInstallPlan(*target.value, nullptr, reparseInstall, "0.1.0"), "UPDATE_INVENTORY_PATH_REJECTED"),
            "reparse child refused without traversal");
        Check(Code(BuildUpdateInstallPlan(*target.value, nullptr, junction, "0.1.0"), "UPDATE_REPARSE_OR_PATH_REJECTED"),
            "reparse install root refused");
        Check(Sha256File(linkedTarget / L"author.txt").value == std::optional<std::string>(Hash("junction target remains intact")),
            "junction target bytes unchanged");
        Check(RemoveDirectoryW(junction.c_str()) != FALSE, "only original junction removed");
    }
}

} // namespace

int main() {
    try {
        if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
        const Signer signer;
        SemVerTests();
        JsonAndManifestTests(signer);
        PackageTests(signer);
        PathAncestorStreamTests(signer);
        PathDirectoryLeaseTests();
        OriginMetadataFormatTests();
        OriginNamedStreamLeaseTests();
        OriginPackageAndPlanTests(signer);
        PlanTests(signer);
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: unexpected fixture failure: " << error.what() << '\n';
    }
    std::cout << "Update Core checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
