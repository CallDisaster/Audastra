#include "ArchiveMemberCommand.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Cli {
namespace {

constexpr std::uint64_t kMaxMemberBytes = 64ull * 1024 * 1024;
constexpr std::uint64_t kMaxTotalBytes = 256ull * 1024 * 1024;

void PrintDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
}

int InvalidArguments(std::string_view reason) {
    std::cerr << "Archive.MemberAdd: ARCHIVE_MEMBER_ADD_ARGUMENT_INVALID: " << reason << '\n'
        << "usage: archive-member-add <source-map> <expected-source-sha256> <new-candidate.w3x> <member> <file> [<member> <file> ...]\n"
        << "  只新增导入成员并更新 war3map.imp；不重建脚本，不覆盖已有成员或编辑器管理成员。\n";
    return 2;
}

std::optional<std::string> ParseSha(std::wstring_view text) {
    if (text.size() != 64) return {};
    std::string result;
    result.reserve(64);
    for (const auto c : text) {
        if (c >= L'0' && c <= L'9') result += static_cast<char>(c);
        else if (c >= L'a' && c <= L'f') result += static_cast<char>(c);
        else if (c >= L'A' && c <= L'F') result += static_cast<char>(c - L'A' + 'a');
        else return {};
    }
    return result;
}

std::optional<std::string> Utf8Argument(std::wstring_view text, std::size_t maximumBytes) {
    if (text.empty() || text.size() > maximumBytes) return {};
    const auto length = static_cast<int>(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || static_cast<std::size_t>(bytes) > maximumBytes) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), bytes, nullptr, nullptr) != bytes)
        return {};
    return result;
}

} // namespace

int RunArchiveMemberAdd(int argc, wchar_t** argv) {
    try {
        if (argc < 7 || (argc - 5) % 2 != 0) return InvalidArguments("需要 source-map、source-sha、candidate 和至少一组 member/file。");
        const auto expected = ParseSha(argv[3]);
        if (!expected) return InvalidArguments("expected-source-sha256 必须是 64 位十六进制。");
        const std::filesystem::path source(argv[2]);
        const std::filesystem::path target(argv[4]);
        std::error_code existsError;
        if (std::filesystem::exists(target, existsError) || existsError)
            return InvalidArguments("候选路径已存在；请换一个新的输出路径。");

        std::vector<std::pair<std::string, std::filesystem::path>> inputs;
        std::set<std::string> requested;
        for (int index = 5; index + 1 < argc; index += 2) {
            auto member = Utf8Argument(argv[index], 1024);
            if (!member) return InvalidArguments("member 路径为空、过长或不是有效 UTF-8。");
            const auto canonical = Resources::CanonicalImportPath(*member);
            if (!canonical.Succeeded() || !canonical.value) {
                PrintDiagnostics(canonical.diagnostics);
                return InvalidArguments("member 路径不是安全的归档相对路径： " + *member);
            }
            if (Resources::IsEditorManagedMemberPath(*member))
                return InvalidArguments("不能通过本命令写入编辑器管理的地图数据文件： " + *member);
            if (!requested.insert(*canonical.value).second)
                return InvalidArguments("同一 member 出现多次： " + *member);
            // 规范形式只用于比较；成员和导入清单保留作者写的大小写，统一为反斜杠。
            std::replace(member->begin(), member->end(), '/', '\\');
            inputs.emplace_back(*member, std::filesystem::path(argv[index + 1]));
        }

        auto opened = MapArchiveSession::Open(source);
        if (!opened.Succeeded() || !opened.value) {
            PrintDiagnostics(opened.diagnostics);
            std::cerr << "Archive.MemberAdd: ARCHIVE_OPEN_FAILED: 无法打开源地图。\n";
            return 2;
        }
        auto& archive = *opened.value;
        if (archive.SourceSha256() != *expected) {
            std::cerr << "Archive.MemberAdd: ARCHIVE_SOURCE_STALE: 源地图 SHA 与期望不一致。\n"
                << "  actual=" << archive.SourceSha256() << "\n  expected=" << *expected << '\n';
            return 2;
        }
        auto impBytes = archive.ReadMember("war3map.imp");
        if (!impBytes.Succeeded() || !impBytes.value) {
            PrintDiagnostics(impBytes.diagnostics);
            std::cerr << "Archive.MemberAdd: IMPORT_TABLE_MISSING: 源地图没有可解析的 war3map.imp。\n";
            return 2;
        }
        auto parsed = Resources::ImportTable::Parse(*impBytes.value);
        if (!parsed.Succeeded() || !parsed.value) {
            PrintDiagnostics(parsed.diagnostics);
            std::cerr << "Archive.MemberAdd: IMPORT_TABLE_INVALID: 原导入清单无法安全编辑。\n";
            return 2;
        }
        auto& table = *parsed.value;

        std::vector<MemberReplacement> replacements;
        replacements.reserve(inputs.size() + 1);
        std::uint64_t total = 0;
        for (const auto& [member, file] : inputs) {
            // 按名字精确核对：未列入文件清单的成员同样不能被覆盖。
            const std::array<std::string, 1> names{member};
            const auto exact = archive.CaptureExactMembers(names);
            if (!exact.Succeeded() || !exact.value || exact.value->size() != 1) {
                PrintDiagnostics(exact.diagnostics);
                std::cerr << "Archive.MemberAdd: MEMBER_PRESENCE_UNKNOWN: 无法确认源地图是否已有该成员： " << member << '\n';
                return 2;
            }
            if (exact.value->front().present) {
                std::cerr << "Archive.MemberAdd: MEMBER_EXISTS: 已存在成员，不能用本命令覆盖： " << member << '\n';
                return 2;
            }
            auto bytes = IO::ReadFileBytes(file, kMaxMemberBytes);
            if (!bytes.Succeeded() || !bytes.value) {
                PrintDiagnostics(bytes.diagnostics);
                std::cerr << "Archive.MemberAdd: MEMBER_FILE_UNREADABLE: 无法读取或超过 64 MiB： " << IO::PathUtf8(file) << '\n';
                return 2;
            }
            total += bytes.value->size();
            if (total > kMaxTotalBytes) return InvalidArguments("新增成员总字节超过 256 MiB。");
            const auto registered = table.Upsert(member, [&archive](std::string_view path) { return archive.HasMember(path); });
            if (!registered.Succeeded()) {
                PrintDiagnostics(registered.diagnostics);
                std::cerr << "Archive.MemberAdd: IMPORT_RECORD_FAILED: 写入导入记录失败： " << member << '\n';
                return 2;
            }
            replacements.push_back({member, std::move(*bytes.value)});
        }
        auto encoded = table.Serialize();
        if (!encoded.Succeeded() || !encoded.value) {
            PrintDiagnostics(encoded.diagnostics);
            std::cerr << "Archive.MemberAdd: IMPORT_TABLE_SERIALIZE_FAILED: 导入清单序列化失败。\n";
            return 2;
        }
        replacements.push_back({"war3map.imp", std::move(*encoded.value)});

        const auto preflight = archive.PreflightSave(replacements, {}, target);
        if (!preflight.Succeeded()) {
            PrintDiagnostics(preflight.diagnostics);
            std::cerr << "Archive.MemberAdd: PREFLIGHT_FAILED: 候选保存输入未通过。\n";
            return 2;
        }
        auto saved = archive.SaveCandidate(target, replacements, {});
        if (!saved.Succeeded() || !saved.value) {
            PrintDiagnostics(saved.diagnostics);
            std::cerr << "Archive.MemberAdd: SAVE_FAILED: 候选写出失败。\n";
            return 2;
        }

        std::cout << "candidate=" << IO::PathUtf8(saved.value->path) << '\n'
                  << "candidate_sha256=" << saved.value->sha256 << '\n'
                  << "member_count=" << saved.value->memberCount << '\n'
                  << "source_sha256=" << archive.SourceSha256() << '\n';
        for (const auto& [member, file] : inputs) {
            (void)file;
            std::cout << "added_member=" << member << '\n';
        }
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Archive.MemberAdd: ARCHIVE_MEMBER_ADD_FAILED: 未预期异常，源图未被修改。\n";
        return 2;
    }
}

} // namespace War3Map::Cli
