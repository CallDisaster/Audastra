#include "LuaMapArchive.h"
#include "../RuntimeFiles.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <mutex>

namespace Audastra::Runtime32::OwnLua {
namespace {
constexpr std::uint64_t kMapBytes = 1024ull * 1024 * 1024;
constexpr std::size_t kModuleBytes = 4 * 1024 * 1024;
bool ValidMember(std::string_view name) {
    if (name.empty() || name.size() > 245 || !LuaMailboxUtf8(name)) return false;
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '/' || name[i] == '\\') {
            const auto part = name.substr(begin, i - begin);
            if (part.empty() || part == "." || part == "..") return false;
            begin = i + 1;
        } else {
            const auto ch = static_cast<unsigned char>(name[i]);
            if (ch < 0x20 || ch == 0x7f || ch == ':' || ch == '*' || ch == '?' || ch == '"'
                || ch == '<' || ch == '>' || ch == '|') return false;
        }
    }
    return true;
}
std::string DiagnosticText(const std::vector<War3Map::Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "MPQ 读取失败，未返回具体诊断。";
    return diagnostics.front().code + ": " + diagnostics.front().message;
}
}

struct LuaMapArchive::Impl {
    Handle file;
    War3Map::Mpq::MpqArchive archive;
    mutable std::mutex mutex;
    Impl(HANDLE input, War3Map::Mpq::MpqArchive value) : file(input), archive(std::move(value)) {}
};
LuaMapArchive::LuaMapArchive(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LuaMapArchive::~LuaMapArchive() = default;

LuaArchiveOpen LuaMapArchive::Open(const std::filesystem::path& map, std::string_view expectedSha256) {
    if (!LuaMailboxHex(expectedSha256, 64)) return {{}, "Lua 地图的预期 SHA256 无效。"};
    try {
        Handle file(OpenLockedFile(map));
        if (file.value == INVALID_HANDLE_VALUE) return {{}, "Lua 地图不是可冻结的普通本地文件。"};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > kMapBytes)
            return {{}, "Lua 地图超出 1 GiB 读取预算或为空。"};
        const auto actual = Sha256(file.value, kMapBytes);
        if (actual.empty() || actual != expectedSha256) return {{}, "Lua 地图与本次测试冻结的 SHA256 不符。"};
        War3Map::Mpq::MpqOpenLimits limits;
        limits.maxArchivePrefixBytes = 16u * 1024u * 1024u;
        limits.maxDeclaredArchiveBytes = kMapBytes;
        limits.maxStrictMemberCount = 65536;
        auto parsed = War3Map::Mpq::MpqArchive::OpenReadOnlyHandle(file.value, map, limits);
        if (!parsed.Succeeded()) return {{}, DiagnosticText(parsed.diagnostics)};
        auto state = std::make_unique<Impl>(file.value, std::move(*parsed.value));
        file.value = INVALID_HANDLE_VALUE;
        return {std::shared_ptr<LuaMapArchive>(new LuaMapArchive(std::move(state))), {}};
    } catch (const std::exception& error) { return {{}, std::string("无法打开 Lua 地图：") + error.what()}; }
    catch (...) { return {{}, "无法打开 Lua 地图：宿主发生未知异常。"}; }
}

LuaModuleRead LuaMapArchive::Read(std::string_view member, std::size_t maxBytes) const {
    if (!ValidMember(member) || !maxBytes || maxBytes > kModuleBytes)
        return {false, {}, "Lua 地图成员路径或读取预算无效。"};
    try {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->archive.HasFile(member)) return {};
        auto bytes = impl_->archive.ReadFileBounded(member, maxBytes);
        if (!bytes.Succeeded()) return {true, {}, DiagnosticText(bytes.diagnostics)};
        std::string text;
        if (!bytes.value->empty()) text.assign(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size());
        return {true, std::move(text), {}};
    } catch (const std::exception& error) { return {false, {}, std::string("无法读取 Lua 地图成员：") + error.what()}; }
    catch (...) { return {false, {}, "无法读取 Lua 地图成员：宿主发生未知异常。"}; }
}

} // namespace Audastra::Runtime32::OwnLua
