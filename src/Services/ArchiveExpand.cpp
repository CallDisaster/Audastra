#include "War3MapCore/Services/ArchiveExpand.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <exception>
#include <map>
#include <random>

namespace War3Map::Services {
namespace {

using Receipt = Result<ArchiveExpandReceipt>;
constexpr const char* kSubsystem = "Archive.Expand";

Receipt Fail(std::vector<Diagnostic> causes, const char* code, std::string message) {
    causes.push_back({DiagnosticSeverity::Error, kSubsystem, code, std::move(message), std::nullopt});
    return {std::nullopt, std::move(causes)};
}

// 同级临时目录：名字带随机后缀，只删除本次自己创建的目录。
class StagingDirectory {
public:
    explicit StagingDirectory(const std::filesystem::path& target) {
        std::random_device random;
        for (int attempt = 0; attempt < 8 && path_.empty(); ++attempt) {
            auto candidate = target;
            candidate += L".expanding-" + std::to_wstring(random());
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error) && !error) path_ = std::move(candidate);
        }
    }
    ~StagingDirectory() {
        if (path_.empty() || committed_) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }
    void Commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    bool committed_ = false;
};

bool IsMetadataMember(std::string_view name) { return !name.empty() && name.front() == '('; }

} // namespace

Result<ArchiveExpandReceipt> ExpandArchive(const std::filesystem::path& source, std::string_view expectedSourceSha256,
    const std::filesystem::path& target, const ArchiveExpandOptions& options) {
    try {
        std::error_code pathError;
        if (target.empty() || std::filesystem::exists(target, pathError) || pathError)
            return Fail({}, "ARCHIVE_EXPAND_TARGET_EXISTS", "展开目录为空或已存在；请换一个新的目录路径。");
        const auto parent = target.parent_path();
        if (parent.empty() || !std::filesystem::is_directory(parent, pathError) || pathError)
            return Fail({}, "ARCHIVE_EXPAND_PARENT_MISSING", "展开目录的上级目录不存在：" + IO::PathUtf8(parent));
        auto archive = MapArchiveSession::Open(source);
        if (!archive.Succeeded() || !archive.value)
            return Fail(std::move(archive.diagnostics), "ARCHIVE_OPEN_FAILED", "无法打开源地图。");
        if (archive.value->SourceSha256() != expectedSourceSha256)
            return Fail({}, "ARCHIVE_SOURCE_STALE",
                "源地图 SHA 与期望不一致：actual=" + archive.value->SourceSha256() + " expected=" + std::string(expectedSourceSha256));

        ArchiveExpandReceipt receipt;
        receipt.sourceSha256 = archive.value->SourceSha256();
        receipt.opaqueMembers = archive.value->OpaqueMemberCount();
        // 先按不区分大小写的规范路径分组：Windows 目录里大小写不同的两个成员会互相覆盖，二者都不写。
        std::map<std::string, std::size_t> canonicalUses;
        std::vector<std::optional<std::string>> canonical;
        for (const auto& member : archive.value->Members()) {
            auto path = Resources::CanonicalImportPath(member.name);
            canonical.push_back(path.Succeeded() && path.value ? std::optional<std::string>(*path.value) : std::nullopt);
            if (canonical.back()) ++canonicalUses[*canonical.back()];
        }
        StagingDirectory staging(target);
        if (staging.Path().empty())
            return Fail({}, "ARCHIVE_EXPAND_STAGING_FAILED", "无法在上级目录创建临时展开目录。");
        bool wtg = false, wct = false;
        const auto members = archive.value->Members();
        for (std::size_t index = 0; index < members.size(); ++index) {
            const auto& member = members[index];
            if (IsMetadataMember(member.name)) { receipt.skipped.push_back({member.name, "archive-metadata"}); continue; }
            if (!canonical[index]) { receipt.skipped.push_back({member.name, "unsafe-path"}); continue; }
            if (canonicalUses[*canonical[index]] > 1) { receipt.skipped.push_back({member.name, "case-collision"}); continue; }
            if (options.dataOnly && !Resources::IsEditorManagedMemberPath(member.name)) {
                receipt.skipped.push_back({member.name, "import"});
                continue;
            }
            std::string relative = member.name;
            for (auto& character : relative) if (character == '\\') character = '/';
            const auto file = staging.Path() / IO::PathFromUtf8(relative);
            std::error_code directoryError;
            std::filesystem::create_directories(file.parent_path(), directoryError);
            if (directoryError)
                return Fail({}, "ARCHIVE_EXPAND_WRITE_FAILED", "无法创建目录：" + IO::PathUtf8(file.parent_path()));
            auto written = IO::WriteFileBytes(file, member.data);
            if (!written.Succeeded())
                return Fail(std::move(written.diagnostics), "ARCHIVE_EXPAND_WRITE_FAILED", "写出成员失败：" + member.name);
            ++receipt.writtenMembers;
            receipt.writtenBytes += member.data.size();
            if (*canonical[index] == "WAR3MAP.WTG") wtg = true;
            if (*canonical[index] == "WAR3MAP.WCT") wct = true;
        }
        // 改名前再确认源图没有在展开期间被替换，避免目录与回执里的 SHA 不对应。
        auto unchanged = archive.value->VerifySourceUnchanged();
        if (!unchanged.Succeeded())
            return Fail(std::move(unchanged.diagnostics), "ARCHIVE_SOURCE_STALE", "展开期间源地图已变化。");
        std::error_code renameError;
        std::filesystem::rename(staging.Path(), target, renameError);
        if (renameError)
            return Fail({}, "ARCHIVE_EXPAND_PUBLISH_FAILED", "临时目录无法改名为目标目录：" + renameError.message());
        staging.Commit();
        receipt.directory = target;
        receipt.triggerPairPresent = wtg && wct;
        return Receipt::Success(std::move(receipt));
    } catch (const std::exception&) {
        return Fail({}, "ARCHIVE_EXPAND_FAILED", "未预期异常，源图未被修改。");
    }
}

} // namespace War3Map::Services
