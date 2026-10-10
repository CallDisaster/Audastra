#include "War3MapCore/ProjectStore.h"

#include "War3MapCore/Hash.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <exception>
#include <memory>
#include <set>

namespace War3Map {
namespace {

class FileHandle {
public:
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (!Valid()) return;
        if (remove_) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            (void)SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        CloseHandle(handle_);
    }
    [[nodiscard]] bool Valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE Get() const { return handle_; }
    void RemoveOnClose() { remove_ = true; }
    void Keep() { remove_ = false; }
private:
    HANDLE handle_;
    bool remove_ = false;
};

using SaveResult = Result<ProjectSaveResult>;

SaveResult Failure(std::string code, std::string message,
    const std::filesystem::path& source = {}) {
    auto result = SaveResult::Failure("ProjectStore", std::move(code), std::move(message));
    if (!source.empty()) result.diagnostics.back().origin = OriginSpan{source};
    return result;
}

SaveResult ForwardFailure(std::vector<Diagnostic> diagnostics) {
    SaveResult result;
    result.diagnostics = std::move(diagnostics);
    return result;
}

void AddRecoveryPath(SaveResult& result, const std::filesystem::path& path, std::string code,
    std::string message) {
    result.diagnostics.push_back({DiagnosticSeverity::Warning, "ProjectStore", std::move(code),
        std::move(message), OriginSpan{path}});
}

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool Inside(const std::filesystem::path& path, const std::filesystem::path& directory) {
    if (SamePath(path, directory)) return true;
    auto prefix = directory.wstring();
    if (prefix.empty()) return false;
    if (prefix.back() != L'\\' && prefix.back() != L'/') prefix.push_back(L'\\');
    const auto name = path.wstring();
    return name.size() > prefix.size() && _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0;
}

std::uint64_t FileId(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

Result<std::filesystem::path> FinalPath(HANDLE handle) {
    const auto length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!length) return Result<std::filesystem::path>::Failure(
        "ProjectStore", "PATH_UNAVAILABLE", "无法确定工程文件的实际路径。");
    std::wstring buffer(length, L'\0');
    const auto count = GetFinalPathNameByHandleW(handle, buffer.data(), length, FILE_NAME_NORMALIZED);
    if (!count || count >= length) return Result<std::filesystem::path>::Failure(
        "ProjectStore", "PATH_UNAVAILABLE", "无法读取完整的工程文件路径。");
    buffer.resize(count);
    if (buffer.starts_with(L"\\\\?\\UNC\\")) buffer = L"\\\\" + buffer.substr(8);
    else if (buffer.starts_with(L"\\\\?\\")) buffer.erase(0, 4);
    return Result<std::filesystem::path>::Success(std::filesystem::path(buffer).lexically_normal());
}

bool SafeFilename(const std::filesystem::path& path) {
    const auto name = path.filename().wstring();
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') return false;
    if (std::any_of(name.begin(), name.end(), [](wchar_t c) {
        return c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"'
            || c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*';
    })) return false;
    auto stem = name.substr(0, name.find(L'.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return std::towupper(c); });
    static const std::set<std::wstring> reserved{L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2",
        L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2",
        L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    return !reserved.contains(stem) && _wcsicmp(path.extension().c_str(), L".forge") == 0;
}

std::string CanonicalMember(std::string_view name) {
    std::string result(name);
    for (auto& c : result) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// 只读取清单的格式身份，其余值仍做完整 JSON 语法检查。不会执行脚本，
// 也不替代恢复入口对源地图、定义和覆盖文档的语义验证。
class ManifestIdentityReader {
public:
    explicit ManifestIdentityReader(std::span<const std::byte> bytes)
        : text_(reinterpret_cast<const char*>(bytes.data()), bytes.size()) {}

    bool Read() {
        if (text_.empty() || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text_.data(), static_cast<int>(text_.size()), nullptr, 0)) return false;
        if (!Object(0)) return false;
        Space();
        return position_ == text_.size() && format_ == "forge-project" && version_ == "1"
            && (sourceKind_ == "archive" || sourceKind_ == "directory");
    }
    const std::string& SourceKind() const { return sourceKind_; }

private:
    void Space() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t'
            || text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
    }
    bool Take(char c) {
        Space();
        if (position_ == text_.size() || text_[position_] != c) return false;
        ++position_;
        return true;
    }
    bool Hex(std::uint32_t& result) {
        result = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (position_ == text_.size()) return false;
            const char c = text_[position_++];
            const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (value < 0) return false;
            result = result * 16 + static_cast<unsigned>(value);
        }
        return true;
    }
    static void Utf8(std::string& result, std::uint32_t code) {
        if (code < 0x80) result.push_back(static_cast<char>(code));
        else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    bool String(std::string& result) {
        if (!Take('"')) return false;
        while (position_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') return true;
            if (c < 32) return false;
            if (c != '\\') { result.push_back(static_cast<char>(c)); continue; }
            if (position_ == text_.size()) return false;
            switch (text_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t code = 0;
                if (!Hex(code)) return false;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (text_.substr(position_, 2) != "\\u") return false;
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!Hex(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
                } else if (code >= 0xDC00 && code <= 0xDFFF) return false;
                Utf8(result, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Number(std::string* result = nullptr) {
        Space();
        const auto start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ == text_.size()) return false;
        if (text_[position_] == '0') ++position_;
        else {
            if (text_[position_] < '1' || text_[position_] > '9') return false;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        }
        for (unsigned part = 0; part < 2; ++part) {
            if (position_ == text_.size()) break;
            const char c = text_[position_];
            if ((part == 0 && c != '.') || (part == 1 && c != 'e' && c != 'E')) continue;
            ++position_;
            if (part == 1 && position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const auto digits = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (digits == position_) return false;
        }
        if (result) result->assign(text_.substr(start, position_ - start));
        return true;
    }
    bool Value(unsigned depth) {
        Space();
        if (depth > 64 || ++values_ > 100000 || position_ == text_.size()) return false;
        if (text_[position_] == '{') return Object(depth);
        if (text_[position_] == '[') {
            ++position_;
            if (Take(']')) return true;
            do { if (!Value(depth + 1)) return false; } while (Take(','));
            return Take(']');
        }
        if (text_[position_] == '"') { std::string ignored; return String(ignored); }
        for (const auto literal : {std::string_view("null"), std::string_view("true"), std::string_view("false")}) {
            if (text_.substr(position_, literal.size()) == literal) { position_ += literal.size(); return true; }
        }
        return Number();
    }
    bool Object(unsigned depth) {
        if (depth > 64 || !Take('{')) return false;
        std::set<std::string> keys;
        if (Take('}')) return true;
        do {
            std::string key;
            if (!String(key) || !keys.insert(key).second || !Take(':')) return false;
            if (depth == 0 && key == "format") { if (!String(format_)) return false; }
            else if (depth == 0 && key == "version") { if (!Number(&version_)) return false; }
            else if (depth == 0 && key == "sourceKind") { if (!String(sourceKind_)) return false; }
            else if (!Value(depth + 1)) return false;
        } while (Take(','));
        return Take('}');
    }

    std::string_view text_;
    std::size_t position_ = 0, values_ = 0;
    std::string format_, version_, sourceKind_;
};

Status ProjectEnvelope(std::span<const MapArchiveMember> members) {
    bool manifest = false, archiveBaseline = false, directoryBaseline = false;
    std::string sourceKind;
    for (const auto& member : members) {
        const auto name = CanonicalMember(member.name);
        if (name == "project.json") {
            if (manifest || member.data.empty() || member.data.size() > 1024 * 1024) return Status::Failure(
                "ProjectStore", "PROJECT_MANIFEST_INVALID", "工程清单为空、重复或超过 1 MiB。");
            manifest = true;
            ManifestIdentityReader identity(member.data);
            if (!identity.Read()) return Status::Failure("ProjectStore", "PROJECT_MANIFEST_INVALID",
                "工程清单不是有效的 forge-project v1 JSON，或来源类型不受支持。");
            sourceKind = identity.SourceKind();
        } else if (name == "baseline/source.w3x" && !member.data.empty()) archiveBaseline = true;
        else if (name.starts_with("baseline/workspace/") && name.size() > 19) directoryBaseline = true;
        else if (!name.starts_with("overlay/") && !name.starts_with("definitions/") && !name.starts_with("imports/")) {
            return Status::Failure("ProjectStore", "PROJECT_ENVELOPE_INVALID",
                "工程容器包含不属于工程格式的根成员：" + member.name);
        }
    }
    if (!manifest || (sourceKind == "archive" ? (!archiveBaseline || directoryBaseline) : (!directoryBaseline || archiveBaseline)))
        return Status::Failure("ProjectStore", "PROJECT_ENVELOPE_INVALID",
        "工程必须包含 project.json 和完整来源基线，不能将普通地图作为工程覆盖。");
    return Status::Ok();
}

Result<std::filesystem::path> PinTargetDirectory(const std::filesystem::path& target,
    std::vector<std::unique_ptr<FileHandle>>& leases) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(target, ec).lexically_normal();
    if (ec) return Result<std::filesystem::path>::Failure(
        "ProjectStore", "TARGET_PATH_INVALID", "无法解析工程目标路径。");
    auto directory = std::make_unique<FileHandle>(CreateFileW(absolute.parent_path().c_str(),
        FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!directory->Valid()) return Result<std::filesystem::path>::Failure(
        "ProjectStore", "TARGET_PARENT_INVALID", "工程保存目录不存在、不可访问或正在移动。");
    auto parent = FinalPath(directory->Get());
    if (!parent.Succeeded()) return parent;
    leases.push_back(std::move(directory));
    for (auto ancestor = parent.value->parent_path(); !ancestor.empty();) {
        const auto next = ancestor.parent_path();
        if (next.empty() || SamePath(ancestor, next)) break;
        auto lease = std::make_unique<FileHandle>(CreateFileW(ancestor.c_str(),
            FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!lease->Valid()) return Result<std::filesystem::path>::Failure(
            "ProjectStore", "TARGET_ANCESTOR_BUSY", "无法锁定工程保存目录的完整路径。");
        leases.push_back(std::move(lease));
        ancestor = next;
    }
    const auto verified = FinalPath(leases.front()->Get());
    if (!verified.Succeeded() || !SamePath(*verified.value, *parent.value)) return Result<std::filesystem::path>::Failure(
        "ProjectStore", "TARGET_PARENT_CHANGED", "工程保存目录在锁定时发生变化。");
    return Result<std::filesystem::path>::Success(*parent.value / absolute.filename());
}

Status PinProtectedSources(const ProjectSaveOptions& options, const std::filesystem::path& target,
    std::vector<std::unique_ptr<FileHandle>>& leases,
    std::vector<BY_HANDLE_FILE_INFORMATION>& identities) {
    for (const auto& path : options.protectedSourcePaths) {
        if (path.empty()) continue;
        auto lease = std::make_unique<FileHandle>(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!lease->Valid()) return Status::Failure("ProjectStore", "PROTECTED_SOURCE_UNAVAILABLE",
            "无法确认受保护来源的实际身份，工程保存未执行。");
        const auto resolved = FinalPath(lease->Get());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!resolved.Succeeded() || !GetFileInformationByHandle(lease->Get(), &info)) return Status::Failure(
            "ProjectStore", "PROTECTED_SOURCE_UNAVAILABLE", "无法确认受保护来源的路径或文件身份。");
        if (SamePath(target, *resolved.value)
            || ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Inside(target, *resolved.value))) {
            return Status::Failure("ProjectStore", "PROTECTED_SOURCE_TARGET",
                "工程目标位于只读源文件或源目录中，不允许覆盖来源。");
        }
        identities.push_back(info);
        leases.push_back(std::move(lease));
    }
    return Status::Ok();
}

std::wstring UniqueSuffix() {
    static std::atomic<std::uint64_t> sequence{0};
    return std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())
        + L"-" + std::to_wstring(sequence.fetch_add(1));
}

std::vector<std::byte> RenameRequest(const std::filesystem::path& target) {
    const auto name = target.wstring();
    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
    return storage;
}

// 仅以调用方自有句柄改名。请求内存在移动旧目标前准备，回滚时不再分配。
// 永不替换已有目标，也不先删除目标。
bool RenameWithoutReplacement(HANDLE handle, std::span<std::byte> request) {
    return SetFileInformationByHandle(handle, FileRenameInfo, request.data(), static_cast<DWORD>(request.size())) != FALSE;
}

} // namespace

namespace {
Result<ProjectSaveResult> SaveControlled(const std::filesystem::path& requestedTarget,
    std::span<const MapArchiveMember> members, const ProjectSaveOptions& options) {
    // backupMoved 为真后，所有异常路径都尝试恢复旧名；若出现并发新目标则保留备份。
    std::unique_ptr<FileHandle> previous, candidate;
    std::vector<std::unique_ptr<FileHandle>> directoryLeases, sourceLeases;
    std::filesystem::path target, backup, staging;
    std::vector<std::byte> targetRename;
    bool backupMoved = false;
    auto rollback = [&](SaveResult result) {
        if (backupMoved && previous && previous->Valid()) {
            if (RenameWithoutReplacement(previous->Get(), targetRename)) {
                backupMoved = false;
            } else {
                if (candidate && candidate->Valid()) candidate->Keep();
                AddRecoveryPath(result, backup, "PROJECT_BACKUP_RETAINED",
                    "无法在不覆盖现有目标的前提下恢复旧文件；完整旧工程保留在此备份，请从该路径恢复。");
                if (candidate && candidate->Valid()) {
                    const auto actual = FinalPath(candidate->Get());
                    AddRecoveryPath(result, actual.Succeeded() ? *actual.value : staging,
                        "PROJECT_CANDIDATE_RETAINED", "已验证的新工程另行保留，未提交保存基线。");
                }
            }
        }
        return result;
    };
    try {
        if (options.control && !options.control->Checkpoint(ProjectSaveStage::Build))
            return ForwardFailure(ProjectSaveCancelledStatus().diagnostics);
        if (!SafeFilename(requestedTarget)) return Failure("TARGET_PATH_INVALID", "工程目标必须是合法的 .forge 文件路径。", requestedTarget);
        if (options.expectedTargetSha256 && (options.expectedTargetSha256->size() != 64
            || !std::all_of(options.expectedTargetSha256->begin(), options.expectedTargetSha256->end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            }))) return Failure("EXPECTED_SHA_INVALID", "更新工程必须提供上一次验证成功的完整 SHA-256。", requestedTarget);
        const auto envelope = ProjectEnvelope(members);
        if (!envelope.Succeeded()) return ForwardFailure(envelope.diagnostics);
        auto resolvedTarget = PinTargetDirectory(requestedTarget, directoryLeases);
        if (!resolvedTarget.Succeeded()) return ForwardFailure(std::move(resolvedTarget.diagnostics));
        target = *resolvedTarget.value;
        std::vector<BY_HANDLE_FILE_INFORMATION> sourceIdentities;
        const auto protection = PinProtectedSources(options, target, sourceLeases, sourceIdentities);
        if (!protection.Succeeded()) return ForwardFailure(protection.diagnostics);

        if (!options.expectedTargetSha256) {
            const auto written = WriteVerifiedArchive(target, members, {}, options.control.get());
            if (!written.Succeeded()) return ForwardFailure(written.diagnostics);
            return SaveResult::Success({written.value->path, written.value->sha256, written.value->memberCount, {}});
        }

        // 先验证既有文件确实是完整工程。该读取不持有句柄，随后再次独占读取 SHA。
        const auto existing = MapArchiveSession::Open(target, options.control.get());
        if (options.control && options.control->CancellationRequested()) return ForwardFailure(ProjectSaveCancelledStatus().diagnostics);
        if (!existing.Succeeded()) return Failure("PROJECT_TARGET_UNAVAILABLE",
            "更新目标已丢失、被占用或不是可完整读取的工程；不会改为新建保存。", target);
        if (!existing.value->Prefix().empty() || !ProjectEnvelope(existing.value->Members()).Succeeded()) return Failure(
            "PROJECT_TARGET_NOT_PROJECT", "现有目标不是编辑工程，禁止以工程保存覆盖地图或其他文件。", target);

        previous = std::make_unique<FileHandle>(CreateFileW(target.c_str(), GENERIC_READ | DELETE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!previous->Valid()) return Failure("PROJECT_TARGET_LOCKED",
            "无法锁定工程更新目标；文件可能正在编辑、被打开或已发生变化。", target);
        BY_HANDLE_FILE_INFORMATION identity{};
        if (!GetFileInformationByHandle(previous->Get(), &identity)
            || (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY))
            || identity.nNumberOfLinks != 1) return Failure("PROJECT_TARGET_IDENTITY_INVALID",
                "工程更新目标必须是可写的普通独立文件，不能使用只读文件、目录、链接或重解析点。", target);
        for (const auto& sourceIdentity : sourceIdentities) {
            if (!(sourceIdentity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                && sourceIdentity.dwVolumeSerialNumber == identity.dwVolumeSerialNumber
                && FileId(sourceIdentity) == FileId(identity)) return Failure("PROTECTED_SOURCE_TARGET",
                    "工程更新目标与只读来源是同一个文件，不允许覆盖。", target);
        }
        const auto actualTarget = FinalPath(previous->Get());
        const auto oldSha = Sha256Handle(previous->Get(), options.control.get());
        if (options.control && options.control->CancellationRequested()) return ForwardFailure(ProjectSaveCancelledStatus().diagnostics);
        if (!actualTarget.Succeeded() || !SamePath(*actualTarget.value, target) || !oldSha.Succeeded()) return Failure(
            "PROJECT_TARGET_IDENTITY_INVALID", "无法确认锁定目标的路径和内容。", target);
        auto expected = *options.expectedTargetSha256;
        std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (*oldSha.value != expected || *oldSha.value != existing.value->SourceSha256()) return Failure(
            "PROJECT_TARGET_CHANGED", "工程已被外部修改，当前内容与上次保存基线不同；请先另存或重新打开。", target);

        const auto suffix = UniqueSuffix();
        staging = target.parent_path() / (L".forge-project-stage-" + suffix + L".forge");
        backup = target.parent_path() / (target.filename().wstring() + L".backup-" + suffix + L".forge");
        // 自有 stage 的改名不是最终发布边界，取消窗口一直保留到移动旧目标前。
        const auto written = WriteVerifiedArchive(staging, members, {}, options.control.get(), false);
        if (!written.Succeeded()) return ForwardFailure(written.diagnostics);
        candidate = std::make_unique<FileHandle>(CreateFileW(staging.c_str(), GENERIC_READ | DELETE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!candidate->Valid()) return Failure("PROJECT_CANDIDATE_LOCKED",
            "已验证候选随后被占用，旧工程未改变；候选文件保留供检查。", staging);
        BY_HANDLE_FILE_INFORMATION candidateIdentity{};
        const auto stagePath = FinalPath(candidate->Get());
        const auto stageSha = Sha256Handle(candidate->Get(), options.control.get());
        if (options.control && options.control->CancellationRequested()) {
            auto cancelled = ForwardFailure(ProjectSaveCancelledStatus().diagnostics);
            AddRecoveryPath(cancelled, staging, "PROJECT_CANDIDATE_RETAINED",
                "取消发生在候选身份复核期间；未删除尚未完整重核身份的 stage，旧恢复点保持不变。");
            return cancelled;
        }
        if (!GetFileInformationByHandle(candidate->Get(), &candidateIdentity)
            || (candidateIdentity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            || candidateIdentity.nNumberOfLinks != 1 || !stagePath.Succeeded() || !SamePath(*stagePath.value, staging)
            || !stageSha.Succeeded() || *stageSha.value != written.value->sha256) return Failure(
                "PROJECT_CANDIDATE_CHANGED", "工程候选在锁定前发生变化，旧工程未改变；候选保留供检查。", staging);
        candidate->RemoveOnClose();
        // 结果内存及路径在移动旧目标前准备好，避免成功发布后分配失败。
        auto success = SaveResult::Success({target, written.value->sha256, written.value->memberCount, backup});
        targetRename = RenameRequest(target);
        auto backupRename = RenameRequest(backup);
        const auto finalSha = Sha256Handle(previous->Get(), options.control.get());
        if (options.control && options.control->CancellationRequested()) return ForwardFailure(ProjectSaveCancelledStatus().diagnostics);
        const auto finalPath = FinalPath(previous->Get());
        if (!finalSha.Succeeded() || *finalSha.value != expected || !finalPath.Succeeded() || !SamePath(*finalPath.value, target)) {
            return Failure("PROJECT_TARGET_CHANGED", "发布前目标验证失败，旧工程未改变。", target);
        }
        if (options.control && !options.control->BeginPublication()) return ForwardFailure(ProjectSaveCancelledStatus().diagnostics);
        if (!RenameWithoutReplacement(previous->Get(), backupRename)) return Failure("PROJECT_BACKUP_FAILED",
            "无法保留完整旧工程备份，更新未执行。", backup);
        backupMoved = true;
        if (options.control) options.control->PublicationStep(1);
        if (!RenameWithoutReplacement(candidate->Get(), targetRename)) return rollback(Failure("PROJECT_PUBLISH_FAILED",
            "无法发布新工程；目标可能被并发创建，正在尝试保留原工程。", target));
        // 句柄在整个发布阶段禁止外部写入/删除，成功改名后内容与路径不会被转向。
        candidate->Keep();
        backupMoved = false;
        return success;
    } catch (const std::exception&) {
        return rollback(Failure("PROJECT_SAVE_FAILED", "工程保存失败，未提交新的保存基线。", target));
    } catch (...) {
        return rollback(Failure("PROJECT_SAVE_FAILED", "工程保存发生未知异常，未提交新的保存基线。", target));
    }
}
} // namespace

Result<ProjectSaveResult> ProjectStore::Save(const std::filesystem::path& requestedTarget,
    std::span<const MapArchiveMember> members, const ProjectSaveOptions& options) {
    if (options.control && !options.control->CanRun()) return Failure("PROJECT_SAVE_CONTROL_USED",
        "恢复控制器已经结束，不能复用于另一份快照。", requestedTarget);
    Result<ProjectSaveResult> cancelled;
    if (options.control) cancelled = ForwardFailure(ProjectSaveCancelledStatus().diagnostics);
    auto result = SaveControlled(requestedTarget, members, options);
    if (options.control && options.control->Complete()) {
        result.value.reset();
        if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const Diagnostic& item) {
            return item.code == kProjectSaveCancelledCode;
        })) {
            result = std::move(cancelled);
        }
    }
    return result;
}

} // namespace War3Map
