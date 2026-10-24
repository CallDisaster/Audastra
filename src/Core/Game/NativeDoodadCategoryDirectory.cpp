#include "War3MapCore/Game/NativeDoodadCategoryDirectory.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace War3Map::Game {
namespace {
constexpr auto kSubsystem = "Terrain.DoodadCategoryDirectory";
constexpr std::size_t kProfileBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaximumDirectoryKeys = 4096;
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
std::string ClassicAnsiPath(const std::filesystem::path& path) {
#ifdef _WIN32
    // 原版使用GetModuleFileNameA/GetFileAttributesA/_fullpath，不能把UTF-16长度冒充ANSI容量。
    const auto codepage = GetACP();
    const bool utf8 = codepage == CP_UTF8;
    BOOL replaced = FALSE;
    const auto& wide = path.native();
    const int size = WideCharToMultiByte(codepage, utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
        wide.c_str(), -1, nullptr, 0, nullptr, utf8 ? nullptr : &replaced);
    if (size <= 0 || size > 260 || replaced)
        Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动路径不属于可逆经典ANSI MAX_PATH域。");
    std::string ansi(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(codepage, utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
            wide.c_str(), -1, ansi.data(), size, nullptr, utf8 ? nullptr : &replaced) || replaced)
        Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动路径发生ANSI默认字符或best-fit替换。");
    const int characters = MultiByteToWideChar(codepage, 0, ansi.c_str(), -1, nullptr, 0);
    if (characters <= 0) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动路径不能从ANSI回读。");
    std::wstring roundtrip(static_cast<std::size_t>(characters), L'\0');
    if (!MultiByteToWideChar(codepage, 0, ansi.c_str(), -1, roundtrip.data(), characters))
        Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动路径ANSI回读失败。");
    roundtrip.pop_back();
    if (roundtrip != wide) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动路径ANSI回读身份不同。");
    ansi.pop_back();
    return ansi;
#else
    const auto text = IO::GenericPathUtf8(path);
    if (text.size() >= 260 || std::any_of(text.begin(), text.end(), [](unsigned char c) { return c > 127; }))
        Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "当前平台只重建经典ASCII MAX_PATH来源。");
    return text;
#endif
}
void NoLink(const std::filesystem::path& path, const std::filesystem::file_status& status) {
    if (std::filesystem::is_symlink(status))
        Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动目录不支持符号链接：" + IO::PathUtf8(path));
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动目录入口不可检查或属于重解析点：" + IO::PathUtf8(path));
#endif
}
std::filesystem::path CanonicalRoot(const std::filesystem::path& configured) {
    if (configured.empty()) Fail("TERRAIN_CATEGORY_SOURCE_REQUIRED", "C类占地需要配置游戏目录以重建启动类别来源。");
    std::error_code error;
    const auto absolute = std::filesystem::absolute(configured, error).lexically_normal();
    if (error) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "无法解析启动目录。");
    auto prefix = absolute.root_path();
    for (const auto& component : absolute.relative_path()) {
        prefix /= component;
        const auto status = std::filesystem::symlink_status(prefix, error);
        if (error || !std::filesystem::is_directory(status))
            Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动目录链不是可检查的普通目录：" + IO::PathUtf8(prefix));
        NoLink(prefix, status);
    }
    const auto canonical = std::filesystem::canonical(absolute, error);
    if (error || IO::GenericPathUtf8(canonical).size() > 512)
        Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动目录不能建立有界canonical身份。");
    ClassicAnsiPath(canonical);
    return canonical;
}
// 按Windows ASCII不区分大小写查找每一层；重复别名、目录代替文件及链接均为unknown。
// 查不到名字才是known absent，不能把拒绝访问或悬空入口当成缺失。
std::optional<std::filesystem::path> FindFile(const std::filesystem::path& root, std::string_view relative) {
    auto current = root;
    std::size_t start = 0;
    while (start < relative.size()) {
        const auto slash = relative.find('/', start);
        const auto part = relative.substr(start, slash == std::string_view::npos ? slash : slash - start);
        const auto direct = current / IO::PathFromUtf8(part);
        const auto ansi = ClassicAnsiPath(direct);
#ifdef _WIN32
        const auto directAttributes = GetFileAttributesA(ansi.c_str());
        if (directAttributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND
            && GetLastError() != ERROR_PATH_NOT_FOUND)
            Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "原版ANSI路径probe不能证明来源缺失：" + IO::PathUtf8(direct));
        const bool directPresent = directAttributes != INVALID_FILE_ATTRIBUTES;
#endif
        std::optional<std::filesystem::path> found;
        std::error_code error;
        std::size_t entries = 0;
        for (std::filesystem::directory_iterator it(current, error), end; !error && it != end; it.increment(error)) {
            if (++entries > 65536)
                Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动目录单层枚举超过支持上限。");
            if (Text::LowerAscii(IO::PathUtf8(it->path().filename())) != Text::LowerAscii(part)) continue;
            if (found) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动目录存在大小写别名冲突：" + std::string(relative));
            found = it->path();
        }
        if (error) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "不能检查启动来源absence：" + IO::PathUtf8(current));
        if (!found) {
#ifdef _WIN32
            if (directPresent) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "ANSI路径probe与目录枚举身份不同。");
#endif
            return {};
        }
#ifdef _WIN32
        if (!directPresent || !std::filesystem::equivalent(direct, *found, error) || error)
            Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "ANSI路径probe与唯一枚举项不一致。");
#endif
        const auto status = std::filesystem::symlink_status(*found, error);
        if (error) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "不能检查启动来源入口：" + IO::PathUtf8(*found));
        NoLink(*found, status);
        const bool last = slash == std::string_view::npos;
        if ((last && !std::filesystem::is_regular_file(status)) || (!last && !std::filesystem::is_directory(status)))
            Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动来源路径类型不明确：" + IO::PathUtf8(*found));
        current = *found;
        ClassicAnsiPath(current);
        if (last) return current;
        start = slash + 1;
    }
    Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动来源路径为空。");
}

#ifdef _WIN32
class InputLease {
public:
    explicit InputLease(const std::filesystem::path& path) : handle_(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle_ == INVALID_HANDLE_VALUE || GetFileType(handle_) != FILE_TYPE_DISK
            || !GetFileInformationByHandle(handle_, &info)
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT))) {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "不能只读锁定普通启动来源：" + IO::PathUtf8(path));
        }
        size_ = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }
    ~InputLease() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    InputLease(const InputLease&) = delete;
    InputLease& operator=(const InputLease&) = delete;
    HANDLE Handle() const noexcept { return handle_; }
    std::uint64_t Size() const noexcept { return size_; }
    std::vector<std::byte> Read(std::size_t count) {
        if (count > size_) Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动来源短于所需输入。");
        std::vector<std::byte> bytes(count);
        std::size_t offset = 0;
        while (offset < count) {
            DWORD got = 0;
            const auto request = static_cast<DWORD>(std::min(std::size_t{65536}, count - offset));
            if (!ReadFile(handle_, bytes.data() + offset, request, &got, nullptr) || got == 0 || got > request)
                Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动来源有界快照读取失败。");
            offset += got;
        }
        return bytes;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
};
std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}
#endif

// 409FA0/40A190的相关状态：行首section与//，不trim，首个'='分隔，raw-empty不插入。
// section/key按ASCII忽略大小写合并；第一次有效key拼写保留。值内容不参与目录首字节。
template<class Reserve>
void MergeCategories(std::span<const std::byte> bytes, std::map<std::string, std::string>& keys, Reserve&& reserve) {
    if (std::find(bytes.begin(), bytes.end(), std::byte{}) != bytes.end())
        Fail("TERRAIN_CATEGORY_PROFILE_ENCODING", "启动profile含NUL或不支持的宽字符编码。");
    auto text = bytes.empty() ? std::string_view{}
        : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    if (text.starts_with("\xff\xfe") || text.starts_with("\xfe\xff"))
        Fail("TERRAIN_CATEGORY_PROFILE_ENCODING", "启动profile不是支持的原版字节文本。");
    bool target = false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto end = text.find_first_of("\r\n", offset);
        const auto line = text.substr(offset, end == std::string_view::npos ? end : end - offset);
        offset = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty() || line.starts_with("//")) continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            // 原状态机对没有闭括号的行保留上一section。
            if (close != std::string_view::npos)
                target = Text::LowerAscii(line.substr(1, close - 1)) == "doodadcategories";
            continue;
        }
        if (!target) continue;
        const auto equal = line.find('=');
        if (equal == std::string_view::npos || equal + 1 == line.size()) continue;
        const auto key = line.substr(0, equal);
        if (key.size() > 1024 || std::any_of(key.begin(), key.end(), [](unsigned char c) { return c < 32 || c > 126; }))
            Fail("TERRAIN_CATEGORY_PROFILE_ENCODING", "类别目录key超出已证明的ASCII比较域。");
        reserve(2 * (key.size() + 1)); // 本行规范key和lookup临时字符串先计费。
        auto canonical = Text::LowerAscii(key);
        if (keys.contains(canonical)) continue;
        if (keys.size() >= kMaximumDirectoryKeys)
            Fail("TERRAIN_CATEGORY_BUDGET", "类别目录key数量超出有界支持域。");
        reserve(160 + 4 * (key.size() + 1)); // 树节点、原始/规范key及最终vector拥有内容。
        keys.emplace(std::move(canonical), key);
    }
}
} // namespace

Result<NativeDoodadCategoryDirectory> NativeDoodadCategoryDirectory::Capture(
    const std::filesystem::path& gameRoot, NativeEditorStartupRules rules, std::size_t maximumBytes) {
    return CaptureWithProfiles(gameRoot, rules, maximumBytes, {});
}

Result<NativeDoodadCategoryDirectory> NativeDoodadCategoryDirectory::CaptureWithProfiles(
    const std::filesystem::path& gameRoot, NativeEditorStartupRules rules, std::size_t maximumBytes,
    const ProfileVisitor& visitor) {
    try {
        if (rules != NativeEditorStartupRules::Classic127aExpansion)
            Fail("TERRAIN_CATEGORY_RULES_UNSUPPORTED", "不支持的启动规则版本。");
        NativeDoodadCategoryDirectory output;
        const auto reserve = [&](std::size_t bytes) {
            if (output.sourceBytes_ > maximumBytes || output.workBytes_ > maximumBytes - output.sourceBytes_
                || bytes > maximumBytes - output.sourceBytes_ - output.workBytes_)
                Fail("TERRAIN_CATEGORY_BUDGET", "启动类别来源或解析工作超过同次剩余预算。");
            output.workBytes_ += bytes;
        };
        const auto sourceRead = [&](std::size_t bytes) {
            reserve(bytes);
            output.workBytes_ -= bytes;
            output.sourceBytes_ += bytes;
        };
        // 固定来源描述符、路径/SHA拥有内容、解析树及归档入口；不靠vector增长漏计。
        reserve(2 * sizeof(output) + MapData::kClassicProfiles.size() * (sizeof(ProfileSource) + 2048)
            + GameDataSource::CoreArchiveNames().size() * 2048 + 4096);
        output.rules_ = rules;
        std::error_code error;
        output.configuredRoot_ = std::filesystem::absolute(gameRoot, error).lexically_normal();
        output.canonicalRoot_ = CanonicalRoot(gameRoot);
        std::vector<std::optional<Mpq::MpqArchive>> archives;
        archives.reserve(GameDataSource::CoreArchiveNames().size());
        output.archivePaths_.reserve(GameDataSource::CoreArchiveNames().size());
        for (const auto name : GameDataSource::CoreArchiveNames()) {
            const auto path = FindFile(output.canonicalRoot_, name);
            output.archivePaths_.push_back(path ? IO::GenericPathUtf8(*path) : std::string{});
            if (path) {
#ifdef _WIN32
                // 同一deny-write HANDLE核头并交给MPQ reader复制，不能预检后换文件再打开。
                InputLease lease(*path);
                reserve(512); // 32字节头读取、probe及固定状态。
                sourceRead(68); // 自身32字节头 + reader的4字节probe/32字节头。
                auto header = lease.Read(32);
                if (U32(header, 0) != 0x1a51504d)
                    Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动核心包需要位于文件起点的经典MPQ头。");
                const auto hashCount = U32(header, 24), blockCount = U32(header, 28);
                if (!hashCount || !blockCount || hashCount > (1u << 20) || blockCount > (1u << 20))
                    Fail("TERRAIN_CATEGORY_BUDGET", "启动核心包表数超出受支持域。");
                // 原表读取、解密副本、拥有表及活跃块范围排序均按实际表数预收。
                // prefix=0且skip-listfile，解析不会再隐式扫描或读取无关清单。
                reserve(std::size_t{hashCount} * 48 + std::size_t{blockCount} * 80);
                const auto tableBytes = std::size_t{hashCount + blockCount} * 16;
                sourceRead(tableBytes);
                Mpq::MpqOpenLimits limits;
                limits.loadListfile = false;
                limits.maxArchivePrefixBytes = 0;
                limits.maxHashTableEntries = hashCount;
                limits.maxBlockTableEntries = blockCount;
                archives.emplace_back(Take(Mpq::MpqArchive::OpenReadOnlyHandle(lease.Handle(), *path, limits)));
#else
                Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "当前平台没有启动归档只读HANDLE租约实现。");
#endif
            } else archives.emplace_back();
        }
        output.profiles_.reserve(MapData::kClassicProfiles.size());
        std::map<std::string, std::string> keys;
        for (const auto& input : MapData::kClassicProfiles) {
            ProfileSource source; source.logicalPath = input.path;
            std::vector<std::byte> bytes;
            const auto logical = std::string_view(input.path);
            // 正常editor mode=1加载全部56项，包括flag=0；basename在完整相对路径之前。
            auto loose = FindFile(output.canonicalRoot_, logical.substr(logical.find_last_of('/') + 1));
            if (loose) source.winner = "loose-basename:" + IO::GenericPathUtf8(*loose);
            else {
                loose = FindFile(output.canonicalRoot_, logical);
                if (loose) source.winner = "loose-relative:" + IO::GenericPathUtf8(*loose);
            }
            if (loose) {
#ifdef _WIN32
                InputLease lease(*loose);
                if (lease.Size() > kProfileBytes)
                    Fail("TERRAIN_CATEGORY_BUDGET", "loose启动profile无法有界读取。");
                sourceRead(static_cast<std::size_t>(lease.Size()));
                reserve(static_cast<std::size_t>(lease.Size()));
                bytes = lease.Read(static_cast<std::size_t>(lease.Size()));
#else
                Fail("TERRAIN_CATEGORY_SOURCE_UNKNOWN", "当前平台没有启动loose只读HANDLE租约实现。");
#endif
            } else {
                for (std::size_t i = archives.size(); i-- > 0;) {
                    if (!archives[i]) continue;
                    const auto info = archives[i]->GetFileInfo(input.path);
                    if (!info) continue;
                    // MPQ扇区偏移表可使存储大小大于解码大小，不能以剩余预算的一半
                    // 同时限制两个不同尺寸。先按真实块表分别预收，失败仍拒绝winner。
                    if (info->compressedSize > kProfileBytes || info->fileSize > kProfileBytes)
                        Fail("TERRAIN_CATEGORY_BUDGET", "MPQ启动profile的存储或解码大小超过单项预算。");
                    sourceRead(info->compressedSize);
                    reserve(info->fileSize);
                    bytes = Take(archives[i]->ReadFileBounded(input.path,
                        std::max<std::size_t>(info->compressedSize, info->fileSize)));
                    source.winner = "mpq:" + output.archivePaths_[i];
                    break;
                }
            }
            if (!source.winner.empty()) {
                source.bytes = bytes.size();
                source.sha256 = Take(Sha256(bytes));
                // 逐字节解析工作已在实际读取前预收，包含无关section及重复读取。
                MergeCategories(bytes, keys, reserve);
                if (visitor) {
                    auto status = visitor(logical, bytes);
                    if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)};
                }
            }
            output.profiles_.push_back(std::move(source));
        }
        output.categoryKeys_.reserve(keys.size());
        for (auto& [canonical, key] : keys) { (void)canonical; output.categoryKeys_.push_back(std::move(key)); }
        if (CanonicalRoot(output.configuredRoot_) != output.canonicalRoot_)
            Fail("TERRAIN_CATEGORY_SOURCE_CHANGED", "启动根目录在捕获时改变。");
        return Result<NativeDoodadCategoryDirectory>::Success(std::move(output));
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Result<NativeDoodadCategoryDirectory>::Failure(kSubsystem, "TERRAIN_CATEGORY_BUDGET", "启动类别来源分配失败。");
    } catch (const std::length_error&) {
        return Result<NativeDoodadCategoryDirectory>::Failure(kSubsystem, "TERRAIN_CATEGORY_BUDGET", "启动类别来源超过容器容量。");
    } catch (const std::filesystem::filesystem_error&) {
        return Result<NativeDoodadCategoryDirectory>::Failure(kSubsystem, "TERRAIN_CATEGORY_SOURCE_UNKNOWN", "启动类别来源文件系统状态不明确。");
    }
}

std::uint8_t NativeDoodadCategoryDirectory::CategoryCSnapCells() const noexcept {
    return std::any_of(categoryKeys_.begin(), categoryKeys_.end(), [](const auto& key) {
        return !key.empty() && key.front() == 'C';
    }) ? 4 : 2;
}

Status NativeDoodadCategoryDirectory::VerifySources(std::size_t maximumBytes) const {
    auto current = Capture(configuredRoot_, rules_, maximumBytes);
    if (!current.Succeeded() || !current.value) return {std::move(current.diagnostics)};
    if (current.value->canonicalRoot_ != canonicalRoot_ || current.value->archivePaths_ != archivePaths_
        || current.value->profiles_ != profiles_ || current.value->categoryKeys_ != categoryKeys_)
        return Status::Failure(kSubsystem, "TERRAIN_CATEGORY_SOURCE_CHANGED", "启动类别来源、内容或较高来源absence已经改变。");
    return Status::Ok();
}

std::size_t NativeDoodadCategoryDirectory::OwnedCapacityBytes() const noexcept {
    auto bytes = sizeof(*this) + archivePaths_.capacity() * sizeof(std::string)
        + profiles_.capacity() * sizeof(ProfileSource) + categoryKeys_.capacity() * sizeof(std::string)
        + (configuredRoot_.native().capacity() + canonicalRoot_.native().capacity() + 2) * sizeof(std::filesystem::path::value_type);
    for (const auto& path : archivePaths_) bytes += path.capacity() + 1;
    for (const auto& source : profiles_)
        bytes += source.logicalPath.capacity() + source.winner.capacity() + source.sha256.capacity() + 3;
    for (const auto& key : categoryKeys_) bytes += key.capacity() + 1;
    return bytes;
}

} // namespace War3Map::Game
