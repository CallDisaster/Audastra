#include "ArchiveMemberCommand.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Services/ArchiveExpand.h"
#include "War3MapCore/Services/ArchiveMemberEdit.h"

#include <Windows.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Cli {
namespace {

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

int InvalidRemoveArguments(std::string_view reason) {
    std::cerr << "Archive.MemberRemove: ARCHIVE_MEMBER_REMOVE_ARGUMENT_INVALID: " << reason << '\n'
        << "usage: archive-member-remove <source-map> <expected-source-sha256> <new-candidate.w3x> <member> [<member> ...] [--allow-referenced]\n"
        << "  只删除已有导入成员和它在 war3map.imp 里的记录；不重建脚本。路径仍出现在脚本、物编、模型或 UI 文本里时拒绝，\n"
        << "  确认要删时加 --allow-referenced。找不到出现处不等于没有引用（动态拼接的路径查不出）。\n";
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

int Report(const Result<Services::ArchiveMemberEditReceipt>& result) {
    if (!result.Succeeded() || !result.value) {
        PrintDiagnostics(result.diagnostics);
        return 2;
    }
    const auto& receipt = *result.value;
    std::cout << "candidate=" << IO::PathUtf8(receipt.candidate) << '\n'
              << "candidate_sha256=" << receipt.candidateSha256 << '\n'
              << "member_count=" << receipt.memberCount << '\n'
              << "source_sha256=" << receipt.sourceSha256 << '\n';
    for (const auto& member : receipt.added) std::cout << "added_member=" << member << '\n';
    if (!receipt.removed.empty()) std::cout << "references_checked=false\n";
    for (std::size_t index = 0; index < receipt.removed.size(); ++index) {
        const auto& [member, records] = receipt.removed[index];
        std::cout << "removed_member=" << member << " import_records_removed=" << records << '\n';
        if (index < receipt.mentions.size())
            for (const auto& mention : receipt.mentions[index])
                std::cout << "mentioned_in=" << mention.member << " kind=" << mention.kind << " count=" << mention.count
                          << " removed_member=" << member << '\n';
    }
    return 0;
}

} // namespace

int RunArchiveMemberAdd(int argc, wchar_t** argv) {
    try {
        if (argc < 7 || (argc - 5) % 2 != 0) return InvalidArguments("需要 source-map、source-sha、candidate 和至少一组 member/file。");
        const auto expected = ParseSha(argv[3]);
        if (!expected) return InvalidArguments("expected-source-sha256 必须是 64 位十六进制。");
        std::vector<Services::ArchiveMemberAddition> additions;
        for (int index = 5; index + 1 < argc; index += 2) {
            auto member = Utf8Argument(argv[index], 1024);
            if (!member) return InvalidArguments("member 路径为空、过长或不是有效 UTF-8。");
            additions.push_back({std::move(*member), std::filesystem::path(argv[index + 1])});
        }
        return Report(Services::AddArchiveMembers(argv[2], *expected, argv[4], additions));
    } catch (const std::exception&) {
        std::cerr << "Archive.MemberAdd: ARCHIVE_MEMBER_ADD_FAILED: 未预期异常，源图未被修改。\n";
        return 2;
    }
}

int RunArchiveMemberRemove(int argc, wchar_t** argv) {
    try {
        if (argc < 6) return InvalidRemoveArguments("需要 source-map、source-sha、candidate 和至少一个 member。");
        const auto expected = ParseSha(argv[3]);
        if (!expected) return InvalidRemoveArguments("expected-source-sha256 必须是 64 位十六进制。");
        std::vector<std::string> members;
        Services::ArchiveMemberRemoveOptions options;
        for (int index = 5; index < argc; ++index) {
            if (std::wstring_view(argv[index]) == L"--allow-referenced") { options.allowReferenced = true; continue; }
            auto member = Utf8Argument(argv[index], 1024);
            if (!member) return InvalidRemoveArguments("member 路径为空、过长或不是有效 UTF-8。");
            members.push_back(std::move(*member));
        }
        if (members.empty()) return InvalidRemoveArguments("至少需要一个 member。");
        return Report(Services::RemoveArchiveMembers(argv[2], *expected, argv[4], members, options));
    } catch (const std::exception&) {
        std::cerr << "Archive.MemberRemove: ARCHIVE_MEMBER_REMOVE_FAILED: 未预期异常，源图未被修改。\n";
        return 2;
    }
}

int RunArchiveExpand(int argc, wchar_t** argv) {
    const auto invalid = [](std::string_view reason) {
        std::cerr << "Archive.Expand: ARCHIVE_EXPAND_ARGUMENT_INVALID: " << reason << '\n'
            << "usage: archive-expand <source-map> <expected-source-sha256> <new-directory> [--data-only]\n"
            << "  把地图包展开成新目录，供 war3-map-mcp --workspace 打开；源图只读。\n";
        return 2;
    };
    try {
        if (argc != 5 && argc != 6) return invalid("需要 source-map、source-sha 和新目录。");
        Services::ArchiveExpandOptions options;
        if (argc == 6) {
            if (std::wstring_view(argv[5]) != L"--data-only") return invalid("只接受 --data-only 选项。");
            options.dataOnly = true;
        }
        const auto expected = ParseSha(argv[3]);
        if (!expected) return invalid("expected-source-sha256 必须是 64 位十六进制。");
        const auto result = Services::ExpandArchive(argv[2], *expected, argv[4], options);
        if (!result.Succeeded() || !result.value) {
            PrintDiagnostics(result.diagnostics);
            return 2;
        }
        const auto& receipt = *result.value;
        std::size_t imports = 0;
        std::cout << "directory=" << IO::PathUtf8(receipt.directory) << '\n'
                  << "source_sha256=" << receipt.sourceSha256 << '\n'
                  << "written_members=" << receipt.writtenMembers << '\n'
                  << "written_bytes=" << receipt.writtenBytes << '\n'
                  << "opaque_members=" << receipt.opaqueMembers << '\n'
                  << "trigger_pair=" << (receipt.triggerPairPresent ? "true" : "false") << '\n';
        for (const auto& skipped : receipt.skipped) {
            if (skipped.reason == "import") { ++imports; continue; }
            std::cout << "skipped_member=" << skipped.member << " reason=" << skipped.reason << '\n';
        }
        if (options.dataOnly) std::cout << "skipped_imports=" << imports << '\n';
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Archive.Expand: ARCHIVE_EXPAND_FAILED: 未预期异常，源图未被修改。\n";
        return 2;
    }
}

} // namespace War3Map::Cli
