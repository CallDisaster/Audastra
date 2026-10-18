#include "War3MapCore/Services/ArchiveMemberEdit.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <array>
#include <exception>

namespace War3Map::Services {
namespace {

using Receipt = Result<ArchiveMemberEditReceipt>;

// 先附上下层诊断，再追加本操作的结论，调用方按顺序输出即可定位。
Receipt Fail(const char* subsystem, std::vector<Diagnostic> causes, const char* code, std::string message) {
    causes.push_back({DiagnosticSeverity::Error, subsystem, code, std::move(message), std::nullopt});
    return {std::nullopt, std::move(causes)};
}

struct OpenedSource {
    std::optional<MapArchiveSession> archive;
    std::optional<Resources::ImportTable> table;
};

// 打开源图、核对 SHA、确认候选路径不存在并解析导入清单；两种操作共用。
Receipt OpenSource(const char* subsystem, const std::filesystem::path& source, std::string_view expectedSha256,
    const std::filesystem::path& target, OpenedSource& opened) {
    std::error_code existsError;
    if (target.empty() || std::filesystem::exists(target, existsError) || existsError)
        return Fail(subsystem, {}, "ARCHIVE_TARGET_EXISTS", "候选路径为空或已存在；请换一个新的输出路径。");
    auto archive = MapArchiveSession::Open(source);
    if (!archive.Succeeded() || !archive.value)
        return Fail(subsystem, std::move(archive.diagnostics), "ARCHIVE_OPEN_FAILED", "无法打开源地图。");
    if (archive.value->SourceSha256() != expectedSha256)
        return Fail(subsystem, {}, "ARCHIVE_SOURCE_STALE",
            "源地图 SHA 与期望不一致：actual=" + archive.value->SourceSha256() + " expected=" + std::string(expectedSha256));
    auto impBytes = archive.value->ReadMember("war3map.imp");
    if (!impBytes.Succeeded() || !impBytes.value)
        return Fail(subsystem, std::move(impBytes.diagnostics), "IMPORT_TABLE_MISSING", "源地图没有可解析的 war3map.imp。");
    auto parsed = Resources::ImportTable::Parse(*impBytes.value);
    if (!parsed.Succeeded() || !parsed.value)
        return Fail(subsystem, std::move(parsed.diagnostics), "IMPORT_TABLE_INVALID", "原导入清单无法安全编辑。");
    opened.archive.emplace(std::move(*archive.value));
    opened.table.emplace(std::move(*parsed.value));
    return Receipt::Success({});
}

// 按名字精确核对成员是否存在：未列入文件清单的成员同样计入。
std::optional<bool> MemberPresent(const MapArchiveSession& archive, const std::string& member, std::vector<Diagnostic>& diagnostics) {
    const std::array<std::string, 1> names{member};
    auto exact = archive.CaptureExactMembers(names);
    if (!exact.Succeeded() || !exact.value || exact.value->size() != 1) {
        diagnostics = std::move(exact.diagnostics);
        return std::nullopt;
    }
    return exact.value->front().present;
}

Receipt Publish(const char* subsystem, const MapArchiveSession& archive, const std::filesystem::path& target,
    std::vector<MemberReplacement> replacements, const std::vector<std::string>& removals, ArchiveMemberEditReceipt receipt) {
    auto preflight = archive.PreflightSave(replacements, removals, target);
    if (!preflight.Succeeded())
        return Fail(subsystem, std::move(preflight.diagnostics), "PREFLIGHT_FAILED", "候选保存输入未通过。");
    auto saved = archive.SaveCandidate(target, replacements, removals);
    if (!saved.Succeeded() || !saved.value)
        return Fail(subsystem, std::move(saved.diagnostics), "SAVE_FAILED", "候选写出失败。");
    receipt.candidate = saved.value->path;
    receipt.candidateSha256 = saved.value->sha256;
    receipt.memberCount = saved.value->memberCount;
    receipt.sourceSha256 = archive.SourceSha256();
    return Receipt::Success(std::move(receipt));
}

} // namespace

Result<std::string> NormalizeArchiveMemberPath(std::string_view member, std::set<std::string>& requested) {
    if (member.empty() || member.size() > 1024)
        return Result<std::string>::Failure("Archive.Member", "ARCHIVE_MEMBER_PATH_INVALID", "成员路径为空或超过 1024 字节。");
    std::string text(member);
    const auto canonical = Resources::CanonicalImportPath(text);
    if (!canonical.Succeeded() || !canonical.value) {
        auto failure = Result<std::string>::Failure("Archive.Member", "ARCHIVE_MEMBER_PATH_INVALID",
            "成员路径不是安全的归档相对路径：" + text);
        failure.diagnostics.insert(failure.diagnostics.begin(), canonical.diagnostics.begin(), canonical.diagnostics.end());
        return failure;
    }
    if (Resources::IsEditorManagedMemberPath(text))
        return Result<std::string>::Failure("Archive.Member", "ARCHIVE_MEMBER_MANAGED",
            "不能用导入操作改动编辑器管理的地图数据文件：" + text);
    if (!requested.insert(*canonical.value).second)
        return Result<std::string>::Failure("Archive.Member", "ARCHIVE_MEMBER_DUPLICATE", "同一成员出现多次：" + text);
    // 规范形式只用于比较；成员和导入清单保留作者写的大小写，统一为反斜杠。
    std::replace(text.begin(), text.end(), '/', '\\');
    return Result<std::string>::Success(std::move(text));
}

Result<ArchiveMemberEditReceipt> AddArchiveMembers(const std::filesystem::path& source, std::string_view expectedSourceSha256,
    const std::filesystem::path& target, const std::vector<ArchiveMemberAddition>& additions) {
    constexpr const char* kSubsystem = "Archive.MemberAdd";
    try {
        if (additions.empty() || additions.size() > ArchiveMemberEditLimits::maxMembers)
            return Fail(kSubsystem, {}, "ARCHIVE_MEMBER_ADD_ARGUMENT_INVALID", "一次新增 1..256 个成员。");
        std::set<std::string> requested;
        std::vector<std::string> members;
        for (const auto& addition : additions) {
            auto member = NormalizeArchiveMemberPath(addition.member, requested);
            if (!member.Succeeded()) return Fail(kSubsystem, std::move(member.diagnostics), "ARCHIVE_MEMBER_ADD_ARGUMENT_INVALID", "成员参数无效。");
            members.push_back(std::move(*member.value));
        }
        OpenedSource opened;
        auto openedStatus = OpenSource(kSubsystem, source, expectedSourceSha256, target, opened);
        if (!openedStatus.Succeeded()) return openedStatus;
        auto& archive = *opened.archive;
        auto& table = *opened.table;
        std::vector<MemberReplacement> replacements;
        replacements.reserve(additions.size() + 1);
        std::uint64_t total = 0;
        ArchiveMemberEditReceipt receipt;
        for (std::size_t index = 0; index < additions.size(); ++index) {
            const auto& member = members[index];
            std::vector<Diagnostic> causes;
            const auto present = MemberPresent(archive, member, causes);
            if (!present) return Fail(kSubsystem, std::move(causes), "MEMBER_PRESENCE_UNKNOWN", "无法确认源地图是否已有该成员：" + member);
            if (*present) return Fail(kSubsystem, {}, "MEMBER_EXISTS", "已存在成员，不能用新增操作覆盖：" + member);
            auto bytes = IO::ReadFileBytes(additions[index].file, ArchiveMemberEditLimits::maxMemberBytes);
            if (!bytes.Succeeded() || !bytes.value)
                return Fail(kSubsystem, std::move(bytes.diagnostics), "MEMBER_FILE_UNREADABLE",
                    "无法读取或超过 64 MiB：" + IO::PathUtf8(additions[index].file));
            total += bytes.value->size();
            if (total > ArchiveMemberEditLimits::maxTotalBytes)
                return Fail(kSubsystem, {}, "ARCHIVE_MEMBER_ADD_ARGUMENT_INVALID", "新增成员总字节超过 256 MiB。");
            auto registered = table.Upsert(member, [&archive](std::string_view path) { return archive.HasMember(path); });
            if (!registered.Succeeded())
                return Fail(kSubsystem, std::move(registered.diagnostics), "IMPORT_RECORD_FAILED", "写入导入记录失败：" + member);
            replacements.push_back({member, std::move(*bytes.value)});
            receipt.added.push_back(member);
        }
        auto encoded = table.Serialize();
        if (!encoded.Succeeded() || !encoded.value)
            return Fail(kSubsystem, std::move(encoded.diagnostics), "IMPORT_TABLE_SERIALIZE_FAILED", "导入清单序列化失败。");
        replacements.push_back({"war3map.imp", std::move(*encoded.value)});
        return Publish(kSubsystem, archive, target, std::move(replacements), {}, std::move(receipt));
    } catch (const std::exception&) {
        return Fail(kSubsystem, {}, "ARCHIVE_MEMBER_ADD_FAILED", "未预期异常，源图未被修改。");
    }
}

Result<ArchiveMemberEditReceipt> RemoveArchiveMembers(const std::filesystem::path& source, std::string_view expectedSourceSha256,
    const std::filesystem::path& target, const std::vector<std::string>& requestedMembers) {
    constexpr const char* kSubsystem = "Archive.MemberRemove";
    try {
        if (requestedMembers.empty() || requestedMembers.size() > ArchiveMemberEditLimits::maxMembers)
            return Fail(kSubsystem, {}, "ARCHIVE_MEMBER_REMOVE_ARGUMENT_INVALID", "一次删除 1..256 个成员。");
        std::set<std::string> requested;
        std::vector<std::string> members;
        for (const auto& requestedMember : requestedMembers) {
            auto member = NormalizeArchiveMemberPath(requestedMember, requested);
            if (!member.Succeeded()) return Fail(kSubsystem, std::move(member.diagnostics), "ARCHIVE_MEMBER_REMOVE_ARGUMENT_INVALID", "成员参数无效。");
            members.push_back(std::move(*member.value));
        }
        OpenedSource opened;
        auto openedStatus = OpenSource(kSubsystem, source, expectedSourceSha256, target, opened);
        if (!openedStatus.Succeeded()) return openedStatus;
        auto& archive = *opened.archive;
        auto& table = *opened.table;
        ArchiveMemberEditReceipt receipt;
        for (const auto& member : members) {
            std::vector<Diagnostic> causes;
            const auto present = MemberPresent(archive, member, causes);
            if (!present) return Fail(kSubsystem, std::move(causes), "MEMBER_PRESENCE_UNKNOWN", "无法确认源地图是否有该成员：" + member);
            if (!*present) return Fail(kSubsystem, {}, "MEMBER_MISSING", "源地图没有该成员：" + member);
            const auto before = table.Entries().size();
            auto removed = table.Remove(member, [&archive](std::string_view path) { return archive.HasMember(path); });
            if (!removed.Succeeded())
                return Fail(kSubsystem, std::move(removed.diagnostics), "IMPORT_RECORD_FAILED", "删除导入记录失败：" + member);
            receipt.removed.emplace_back(member, before - table.Entries().size());
        }
        std::vector<MemberReplacement> replacements;
        if (table.IsModified()) {
            auto encoded = table.Serialize();
            if (!encoded.Succeeded() || !encoded.value)
                return Fail(kSubsystem, std::move(encoded.diagnostics), "IMPORT_TABLE_SERIALIZE_FAILED", "导入清单序列化失败。");
            replacements.push_back({"war3map.imp", std::move(*encoded.value)});
        }
        return Publish(kSubsystem, archive, target, std::move(replacements), members, std::move(receipt));
    } catch (const std::exception&) {
        return Fail(kSubsystem, {}, "ARCHIVE_MEMBER_REMOVE_FAILED", "未预期异常，源图未被修改。");
    }
}

} // namespace War3Map::Services
