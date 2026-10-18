#include "War3MapCore/Services/ArchiveMemberEdit.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <array>
#include <exception>
#include <span>

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

std::string UpperKey(std::string_view name) {
    std::string key(name);
    for (auto& c : key) {
        if (c == '/') c = '\\';
        else if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return key;
}

// 查找用的归一化文本：小写 ASCII、斜杠统一为反斜杠、连续反斜杠（JASS 转义）按一个算。
std::string NormalizePathText(std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto byte : bytes) {
        auto c = static_cast<char>(byte);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        else if (c == '/') c = '\\';
        if (c == '\\' && !text.empty() && text.back() == '\\') continue;
        text.push_back(c);
    }
    return text;
}

bool WordCharacter(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }
bool PathCharacter(char c) { return WordCharacter(c) || c == '-' || c == '.' || c == '\\'; }

// 只读这些成员找路径；.ai 等不透明导入不读。
std::optional<const char*> MentionKind(const std::string& key) {
    if (key == "WAR3MAP.J" || key == "SCRIPTS\\WAR3MAP.J" || key == "WAR3MAP.LUA" || key == "SCRIPTS\\WAR3MAP.LUA") return "script";
    const auto endsWith = [&key](std::string_view suffix) { return key.ends_with(suffix); };
    if (key.find('\\') == std::string::npos && (key.starts_with("WAR3MAP.") || key.starts_with("WAR3MAPSKIN.")))
        for (const auto* extension : {".W3U", ".W3T", ".W3B", ".W3D", ".W3A", ".W3H", ".W3Q"})
            if (endsWith(extension)) return "object-data";
    if (key == "WAR3MAP.W3I" || key == "WAR3MAP.WTS" || key == "WAR3MAPMISC.TXT" || key == "WAR3MAPSKIN.TXT" ||
        key == "WAR3MAPEXTRA.TXT") return "map-data";
    if (Resources::IsEditorManagedMemberPath(key)) return std::nullopt;
    if (endsWith(".MDX") || endsWith(".MDL")) return "model";
    if (endsWith(".J") || endsWith(".LUA")) return "script";
    for (const auto* extension : {".TXT", ".FDF", ".TOC", ".SLK", ".INI"})
        if (endsWith(extension)) return "ui-text";
    return std::nullopt;
}

std::size_t CountMentions(const std::string& text, const std::vector<std::string>& needles) {
    std::size_t count = 0;
    for (const auto& needle : needles)
        for (auto position = text.find(needle); position != std::string::npos; position = text.find(needle, position + 1)) {
            const auto end = position + needle.size();
            if ((position == 0 || !PathCharacter(text[position - 1])) && (end == text.size() || !WordCharacter(text[end]))) ++count;
        }
    return count;
}

} // namespace

std::vector<std::vector<ArchivePathMention>> FindArchivePathMentions(std::span<const MapArchiveMember> members,
    std::span<const std::string> targets) {
    std::set<std::string> targetKeys;
    std::vector<std::vector<std::string>> needles;
    for (const auto& target : targets) {
        auto key = UpperKey(target);
        targetKeys.insert(key);
        auto lower = key;
        for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        std::vector<std::string> forms{lower};
        // 物编和脚本常写 .mdl，游戏按 .mdx 读取，两种都算。
        if (lower.ends_with(".mdx") || lower.ends_with(".mdl"))
            forms.push_back(lower.substr(0, lower.size() - 1) + (lower.back() == 'x' ? 'l' : 'x'));
        needles.push_back(std::move(forms));
    }
    std::vector<std::vector<ArchivePathMention>> result(targets.size());
    for (const auto& member : members) {
        const auto key = UpperKey(member.name);
        if (targetKeys.contains(key)) continue;
        const auto kind = MentionKind(key);
        if (!kind) continue;
        const auto text = NormalizePathText(member.data);
        for (std::size_t index = 0; index < targets.size(); ++index)
            if (const auto count = CountMentions(text, needles[index]); count != 0)
                result[index].push_back({member.name, *kind, count});
    }
    return result;
}

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
    const std::filesystem::path& target, const std::vector<std::string>& requestedMembers, const ArchiveMemberRemoveOptions& options) {
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
        }
        receipt.mentions = FindArchivePathMentions(archive.Members(), members);
        if (!options.allowReferenced) {
            std::string found;
            std::size_t listed = 0;
            for (std::size_t index = 0; index < members.size(); ++index)
                for (const auto& mention : receipt.mentions[index])
                    if (listed++ < 16)
                        found += "\n  " + members[index] + " ← " + mention.member + "（" + mention.kind + " ×" + std::to_string(mention.count) + "）";
            if (listed != 0)
                return Fail(kSubsystem, {}, "MEMBER_REFERENCED", "要删除的文件路径仍出现在 " + std::to_string(listed) +
                    " 处，确认要删时加 allow_referenced（CLI 为 --allow-referenced）：" + found);
        }
        for (const auto& member : members) {
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
