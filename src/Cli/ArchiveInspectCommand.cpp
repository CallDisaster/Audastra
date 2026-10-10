#include "ArchiveInspectCommand.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Render/ModelAnimationCapture.h"

#include <Windows.h>

#include <exception>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr std::size_t kMaximumMembers = 256;

void PrintDiagnostics(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
}

int InvalidArguments(const char* reason) {
    std::cerr << "Archive.Inspect: ARCHIVE_INSPECT_ARGUMENT_INVALID: " << reason << '\n'
        << "usage: archive-session-inspect <map-archive> [--expected-source-sha256 <64hex>] [--member <internal-path> ...]\n";
    return 2;
}

std::optional<std::string> SourceShaArgument(std::wstring_view text) {
    if (text.size() != 64) return {};
    std::string result; result.reserve(text.size());
    for (const auto c : text) {
        if (c >= L'0' && c <= L'9') result += static_cast<char>(c);
        else if (c >= L'a' && c <= L'f') result += static_cast<char>(c);
        else if (c >= L'A' && c <= L'F') result += static_cast<char>(c - L'A' + L'a');
        else return {};
    }
    return result;
}

std::optional<std::string> Utf8Argument(std::wstring_view text, std::size_t maximumBytes) {
    if (text.empty() || text.size() > maximumBytes
        || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
    const auto length = static_cast<int>(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || static_cast<std::size_t>(bytes) > maximumBytes) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
        result.data(), bytes, nullptr, nullptr) != bytes) return {};
    return result;
}

void PrintLegacy(const War3Map::MapArchiveSession& session) {
    std::cout << "sourceSha256=" << session.SourceSha256()
        << "\nnamedMemberCount=" << session.Members().size()
        << "\nopaqueMemberCount=" << session.OpaqueMemberCount() << '\n';
    for (const auto& metadata : session.MetadataMembers()) {
        std::cout << "metadataMember=" << metadata
            << (session.HasOpaqueMembers() ? " (kept as source bytes)\n" : " (dropped on rebuild)\n");
    }
    std::cout << "mapInitializationGenerated=false\noriginalEditorValidated=false\nruntimeValidated=false\n";
}
}

int RunArchiveInspectCommand(int argc, wchar_t** argv) try {
    if (!argv || argc < 3 || !argv[1] || !argv[2]
        || std::wstring_view(argv[1]) != L"archive-session-inspect")
        return InvalidArguments("需要命令及归档路径。");
    if (argc > 5 + 2 * static_cast<int>(kMaximumMembers))
        return InvalidArguments("请求参数过多；最多256个成员。");
    std::optional<std::string> expected;
    std::vector<std::string> names;
    const War3Map::ArchiveExactMemberLimits limits;
    for (int index = 3; index < argc; index += 2) {
        if (!argv[index] || index + 1 >= argc || !argv[index + 1])
            return InvalidArguments("参数缺少值。");
        const std::wstring_view option(argv[index]);
        if (option == L"--expected-source-sha256") {
            if (expected) return InvalidArguments("期望源SHA只能声明一次。");
            expected = SourceShaArgument(argv[index + 1]);
            if (!expected) return InvalidArguments("期望源SHA必须是64位十六进制。");
        } else if (option == L"--member") {
            if (names.size() == kMaximumMembers) return InvalidArguments("最多256个成员。");
            auto name = Utf8Argument(argv[index + 1], limits.pathBytes);
            if (!name) return InvalidArguments("成员路径为空、超过UTF-8字节预算或包含无效UTF-16。");
            names.push_back(std::move(*name));
        } else return InvalidArguments("未知参数。");
    }
    if (argc != 3 && !expected) return InvalidArguments("精确成员查询必须提供期望源SHA。");

    auto session = War3Map::MapArchiveSession::Open(argv[2]);
    if (!session.Succeeded() || !session.value) { PrintDiagnostics(session.diagnostics); return 1; }
    if (argc == 3) { PrintLegacy(*session.value); return 0; }
    const auto identity = session.value->DescribeExactMemberIdentities(*expected, names, limits);
    if (!identity.Succeeded() || !identity.value) { PrintDiagnostics(identity.diagnostics); return 1; }

    // CLI已链接RenderSupport，复用其公开JSON字符串转义；不再复制第三套转义器或引入Qt依赖。
    const auto quoted = War3Map::Render::AnimationJsonString;
    std::ostringstream json; json.imbue(std::locale::classic());
    json << "{\"source_sha256\":" << quoted(identity.value->sourceSha256) << ",\"members\":[";
    bool comma = false;
    for (const auto& member : identity.value->members) {
        if (comma) json << ',';
        comma = true;
        json << "{\"name\":" << quoted(member.name)
            << ",\"state\":\"" << (member.present ? "present" : "missing") << "\",\"bytes\":";
        if (member.byteSize) json << quoted(std::to_string(*member.byteSize)); else json << "null";
        json << ",\"sha256\":";
        if (member.sha256) json << quoted(*member.sha256); else json << "null";
        json << '}';
    }
    json << "]}";
    std::cout << json.str() << '\n'; // 完整查询/序列化成功后才向stdout发布JSON。
    return 0;
} catch (const std::exception&) {
    std::cerr << "Archive.Inspect: ARCHIVE_INSPECT_FAILED: 归档身份查询失败，未输出部分身份。\n";
    return 1;
}
