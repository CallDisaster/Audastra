#include "ImportQueryCommand.h"

#include "War3MapCore/Render/ModelAnimationCapture.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <set>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
constexpr std::size_t kJsonBytes = 256U * 1024U, kCursorBytes = 16U * 1024U;
constexpr char kHex[] = "0123456789abcdef";

class ResponseBudget final : public std::length_error {
public:
    ResponseBudget() : std::length_error("IMPORT_RESPONSE_BUDGET") {}
};

// 序列化期间即约束完整缓冲区；预留Windows文本stdout的CRLF，拒绝时不发布部分JSON。
class JsonBuffer final : public std::streambuf {
public:
    [[nodiscard]] const std::string& Bytes() const noexcept { return bytes_; }
protected:
    std::streamsize xsputn(const char* value, std::streamsize count) override {
        if (count < 0 || static_cast<std::uint64_t>(count) > kJsonBytes - 2 - bytes_.size())
            throw ResponseBudget{};
        bytes_.append(value, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char c = traits_type::to_char_type(value);
        xsputn(&c, 1);
        return value;
    }
private:
    std::string bytes_;
};

int Failure(const char* code, const char* message, int exitCode = 1) {
    std::cerr << "Imports.Cli: " << code << ": " << message << '\n';
    return exitCode;
}
int InvalidArguments(const char* reason) {
    Failure("IMPORT_ARGUMENT_INVALID", reason, 2);
    std::cerr << "usage: imports-query <archive-or-directory> --expected-source-sha256 <64lowerhex>"
        " [--expected-inventory-sha256 <64lowerhex>] [--query <text>] [--limit <1..128>] [--cursor <token>]\n"
        "       imports-get <archive-or-directory> --expected-source-sha256 <64lowerhex>"
        " --expected-inventory-sha256 <64lowerhex> --import-id <id>\n";
    return 2;
}
void PrintDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
}
bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::optional<std::uint64_t> Unsigned(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return {};
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return value;
}
bool ValidUtf8(std::string_view value) {
    return value.empty() || (value.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
        && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0) > 0);
}
std::optional<std::string> Utf8Argument(std::wstring_view value, std::size_t maximumBytes) {
    if (value.empty()) return std::string{};
    if (value.size() > maximumBytes || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
    const auto length = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || static_cast<std::size_t>(bytes) > maximumBytes) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        result.data(), bytes, nullptr, nullptr) != bytes) return {};
    return result;
}

// 复用现有CLI已链接的JSON转义；所有可能含原路径字节的文本先校验UTF-8。
void Text(std::ostream& out, std::string_view value) {
    if (!ValidUtf8(value)) { out << "null"; return; }
    // 单字段超过完整响应预算时无需建立更大的转义临时字符串。
    if (value.size() > kJsonBytes - 2) throw ResponseBudget{};
    out << Render::AnimationJsonString(value);
}
void OptionalText(std::ostream& out, const std::optional<std::string>& value) {
    if (value) Text(out, *value); else out << "null";
}
void Number(std::ostream& out, std::uint64_t value) { out << '"' << value << '"'; }
void OptionalNumber(std::ostream& out, const std::optional<std::uint64_t>& value) {
    if (value) Number(out, *value); else out << "null";
}
void Hex(std::ostream& out, std::string_view bytes) {
    if (bytes.size() > (kJsonBytes - 4) / 2) throw ResponseBudget{};
    out << '"';
    for (const unsigned char c : bytes) out << kHex[c >> 4] << kHex[c & 15];
    out << '"';
}
void OptionalHex(std::ostream& out, const std::optional<std::string>& value) {
    if (value) Hex(out, *value); else out << "null";
}
void Binding(std::ostream& out, const ImportInventoryBinding& binding) {
    out << "{\"project_identity\":"; Text(out, binding.documentId);
    out << ",\"document_generation\":"; Number(out, binding.generation);
    out << ",\"revision\":"; Text(out, binding.revision);
    out << ",\"source_sha256\":"; Text(out, binding.sourceSha256);
    out << ",\"asset_revision\":"; Text(out, binding.assetRevision);
    out << ",\"content_scope\":"; Text(out, binding.contentScope);
    out << '}';
}

// CLI令牌只编码Core游标的全部字段；长度前缀避免路径/query中的分隔符改变字段边界。
// 这不是授权签名；跨调用有效性由Core按完整来源/清单/绑定/query核验。
std::string CursorToken(const ImportInventoryCursor& cursor) {
    std::string bytes;
    for (const auto& field : {cursor.binding.documentId, std::to_string(cursor.binding.generation),
        cursor.binding.revision, cursor.binding.sourceSha256, cursor.binding.assetRevision,
        cursor.binding.contentScope, cursor.inventorySha256, cursor.query, std::to_string(cursor.nextIndex)}) {
        bytes += std::to_string(field.size()); bytes += ':'; bytes += field;
    }
    std::string token = "i1.";
    for (const unsigned char c : bytes) { token += kHex[c >> 4]; token += kHex[c & 15]; }
    return token;
}
std::optional<ImportInventoryCursor> ReadCursor(std::string_view token) {
    if (!token.starts_with("i1.") || token.size() > kCursorBytes || (token.size() - 3) % 2 != 0) return {};
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string bytes;
    for (std::size_t i = 3; i < token.size(); i += 2) {
        const int hi = nibble(token[i]), lo = nibble(token[i + 1]);
        if (hi < 0 || lo < 0) return {};
        bytes += static_cast<char>((hi << 4) | lo);
    }
    if (bytes.find('\0') != bytes.npos || !ValidUtf8(bytes)) return {};
    std::array<std::string, 9> fields;
    std::size_t position = 0;
    for (auto& field : fields) {
        const auto colon = bytes.find(':', position);
        if (colon == bytes.npos) return {};
        const auto length = Unsigned(std::string_view(bytes).substr(position, colon - position));
        if (!length || *length > 4096 || *length > bytes.size() - colon - 1) return {};
        position = colon + 1;
        field = bytes.substr(position, static_cast<std::size_t>(*length));
        position += static_cast<std::size_t>(*length);
    }
    const auto generation = Unsigned(fields[1]), next = Unsigned(fields[8]);
    if (position != bytes.size() || !generation || !next || fields[0].empty() || fields[2].empty()
        || fields[4].empty() || fields[5].empty() || !Sha(fields[3]) || !Sha(fields[6])) return {};
    return ImportInventoryCursor{{fields[0], *generation, fields[2], fields[3], fields[4], fields[5]},
        fields[6], fields[7], *next};
}
void Cursor(std::ostream& out, const ImportInventoryCursor& cursor) {
    out << "{\"binding\":"; Binding(out, cursor.binding);
    out << ",\"inventory_sha256\":"; Text(out, cursor.inventorySha256);
    out << ",\"query\":"; Text(out, cursor.query);
    out << ",\"next_index\":"; Number(out, cursor.nextIndex);
    out << '}';
}
void Description(std::ostream& out, const ImportInventoryDescription& description) {
    out << "{\"ok\":true,\"schema_version\":1,\"binding_scope\":\"source-sha-bound-readonly-cli\""
        ",\"generation_revision_scope\":\"content-identity-across-commands\",\"binding\":";
    Binding(out, description.binding);
    out << ",\"inventory_sha256\":"; Text(out, description.inventorySha256);
    out << ",\"source_kind\":"; Text(out, description.sourceKind);
    out << ",\"imp_state\":"; Text(out, description.impState);
    out << ",\"imp_source_path\":"; OptionalText(out, description.impSourcePath);
    out << ",\"imp_source_path_hex\":"; OptionalHex(out, description.impSourcePath);
    out << ",\"imp_sha256\":"; OptionalText(out, description.impSha256);
    out << ",\"imp_record_count\":"; Number(out, description.impRecordCount);
    out << ",\"physical_member_count\":"; Number(out, description.physicalMemberCount);
    out << ",\"queued_count\":"; Number(out, description.queuedCount);
    out << ",\"excluded_reserved_input_rows\":"; Number(out, description.excludedArchiveMetadataCount);
    out << ",\"archive_internal_metadata_enumerated\":false,\"coverage\":";
    Text(out, description.hasOpaqueArchiveMembers ? "partial" : "named-source-complete");
    out << ",\"has_opaque_archive_members\":" << (description.hasOpaqueArchiveMembers ? "true" : "false")
        << ",\"includes_unsaved_changes\":" << (description.includesUnsavedChanges ? "true" : "false")
        << ",\"read_only\":true,\"payloads_opaque\":true,\"text_available\":false,\"lower_layer_coverage\":\"unknown\""
        << ",\"metadata_scope\":\"source-member-bytes-and-current-path-overlay\"";
}
void Row(std::ostream& out, const ImportInventoryRow& row) {
    out << "{\"import_id\":"; Text(out, row.importId);
    out << ",\"kind\":"; Text(out, row.kind);
    out << ",\"raw_path\":"; Text(out, row.rawPath);
    out << ",\"raw_path_hex\":"; Hex(out, row.rawPath);
    out << ",\"flags\":";
    if (row.flags) out << static_cast<unsigned int>(*row.flags); else out << "null";
    out << ",\"source_path\":"; OptionalText(out, row.sourcePath);
    out << ",\"source_path_hex\":"; OptionalHex(out, row.sourcePath);
    out << ",\"archive_path\":"; OptionalText(out, row.archivePath);
    out << ",\"archive_path_hex\":"; OptionalHex(out, row.archivePath);
    out << ",\"source_layer\":"; Text(out, row.sourceLayer);
    out << ",\"state\":"; Text(out, row.state);
    out << ",\"overlay_state\":"; Text(out, row.overlayState);
    out << ",\"bytes\":"; OptionalNumber(out, row.bytes);
    out << ",\"sha256\":"; OptionalText(out, row.sha256);
    out << ",\"reported_bytes\":"; OptionalNumber(out, row.reportedBytes);
    out << ",\"override\":\"unknown\",\"payload_opaque\":true,\"candidates\":[";
    for (std::size_t i = 0; i < row.candidates.size(); ++i) {
        if (i) out << ',';
        const auto& candidate = row.candidates[i];
        out << "{\"path\":"; Text(out, candidate.name);
        out << ",\"path_hex\":"; Hex(out, candidate.name);
        out << ",\"state\":\"" << (candidate.present ? "present" : "missing") << "\",\"bytes\":";
        OptionalNumber(out, candidate.byteSize);
        out << ",\"sha256\":"; OptionalText(out, candidate.sha256);
        out << '}';
    }
    out << "],\"diagnostics\":[";
    for (std::size_t i = 0; i < row.diagnostics.size(); ++i) {
        if (i) out << ',';
        const auto& diagnostic = row.diagnostics[i];
        out << "{\"severity\":";
        Text(out, diagnostic.severity == DiagnosticSeverity::Error ? "error"
            : diagnostic.severity == DiagnosticSeverity::Warning ? "warning" : "info");
        out << ",\"subsystem\":"; Text(out, diagnostic.subsystem);
        out << ",\"code\":"; Text(out, diagnostic.code);
        out << ",\"message\":"; Text(out, diagnostic.message);
        out << '}';
    }
    out << "]}";
}

struct Arguments {
    bool get = false;
    std::string sourceSha, query;
    std::optional<std::string> inventorySha, importId;
    std::optional<ImportInventoryCursor> cursor;
    std::size_t limit = 64;
};
int Execute(const ImportInventorySnapshot& snapshot, const Arguments& arguments) {
    const auto& description = snapshot.Description();
    if (arguments.inventorySha && *arguments.inventorySha != description.inventorySha256)
        return Failure("IMPORT_REVISION_CONFLICT", "导入来源/IMP/资产覆盖快照已变化。");
    JsonBuffer buffer;
    std::ostream out(&buffer);
    out.imbue(std::locale::classic());
    out.exceptions(std::ios::badbit | std::ios::failbit);
    if (arguments.get) {
        const auto row = snapshot.Get(description.binding, *arguments.inventorySha, *arguments.importId);
        if (!row.Succeeded() || !row.value) { PrintDiagnostics(row.diagnostics); return 1; }
        Description(out, description);
        out << ",\"row\":"; Row(out, *row.value);
    } else {
        const auto page = snapshot.Query(description.binding, arguments.query, arguments.cursor, arguments.limit);
        if (!page.Succeeded() || !page.value) { PrintDiagnostics(page.diagnostics); return 1; }
        Description(out, page.value->description);
        out << ",\"query\":"; Text(out, arguments.query);
        out << ",\"rows\":[";
        for (std::size_t i = 0; i < page.value->rows.size(); ++i) {
            if (i) out << ',';
            Row(out, page.value->rows[i]);
        }
        out << "],\"returned\":"; Number(out, page.value->rows.size());
        out << ",\"known_row_count\":"; Number(out, page.value->knownRowCount);
        out << ",\"matched_total\":"; OptionalNumber(out, page.value->matchedTotal);
        out << ",\"examined\":"; Number(out, page.value->examined);
        out << ",\"scan_budget_exhausted\":" << (page.value->scanBudgetExhausted ? "true" : "false");
        out << ",\"truncated\":" << (page.value->next ? "true" : "false");
        out << ",\"next_cursor\":";
        if (page.value->next) Cursor(out, *page.value->next); else out << "null";
        out << ",\"next_cursor_token\":";
        if (page.value->next) Text(out, CursorToken(*page.value->next)); else out << "null";
    }
    out << '}';
    // 序列化后再次核验原来源；捕获至最后一次核验期间归档会话始终存活。
    const auto current = snapshot.VerifyCurrent();
    if (!current.Succeeded()) { PrintDiagnostics(current.diagnostics); return 1; }
    std::cout << buffer.Bytes() << '\n';
    return std::cout ? 0 : Failure("IMPORT_OUTPUT_FAILED", "标准输出写入失败。");
}
} // namespace

int RunImportQueryCommand(int argc, wchar_t** argv) try {
    if (!argv || argc < 3 || argc > 15 || !argv[1] || !argv[2] || !argv[2][0])
        return InvalidArguments("需要命令和来源路径，且参数数量有界。");
    const std::wstring_view command(argv[1]);
    if (command != L"imports-query" && command != L"imports-get") return InvalidArguments("未知导入命令。");
    Arguments arguments;
    arguments.get = command == L"imports-get";
    std::set<std::wstring_view> seen;
    for (int index = 3; index < argc; index += 2) {
        if (!argv[index] || index + 1 >= argc || !argv[index + 1]) return InvalidArguments("参数缺少值。");
        const std::wstring_view option(argv[index]);
        if (!seen.insert(option).second) return InvalidArguments("参数不能重复。");
        std::size_t maxBytes = 0;
        if (option == L"--expected-source-sha256" || option == L"--expected-inventory-sha256") maxBytes = 64;
        else if (!arguments.get && option == L"--query") maxBytes = 4096;
        else if (!arguments.get && option == L"--limit") maxBytes = 3;
        else if (!arguments.get && option == L"--cursor") maxBytes = kCursorBytes;
        else if (arguments.get && option == L"--import-id") maxBytes = 256;
        else return InvalidArguments("未知参数或参数不适用于该命令；不接受裸分页起点。");
        const auto value = Utf8Argument(argv[index + 1], maxBytes);
        if (!value) return InvalidArguments("参数超过UTF-8字节预算或包含无效UTF-16。");
        if (option == L"--expected-source-sha256" || option == L"--expected-inventory-sha256") {
            if (!Sha(*value)) return InvalidArguments("SHA必须是64位小写十六进制。");
            if (option == L"--expected-source-sha256") arguments.sourceSha = *value;
            else arguments.inventorySha = *value;
        } else if (option == L"--query") arguments.query = *value;
        else if (option == L"--limit") {
            const auto limit = Unsigned(*value);
            if (!limit || !*limit || *limit > 128) return InvalidArguments("页长必须是1至128的规范十进制整数。");
            arguments.limit = static_cast<std::size_t>(*limit);
        } else if (option == L"--import-id") {
            if (value->empty()) return InvalidArguments("import-id不能为空。");
            arguments.importId = *value;
        } else {
            arguments.cursor = ReadCursor(*value);
            if (!arguments.cursor) return InvalidArguments("游标令牌无效或超出预算。");
        }
    }
    if (arguments.sourceSha.empty()) return InvalidArguments("必须提供期望完整来源SHA。");
    if (arguments.get && (!arguments.inventorySha || !arguments.importId))
        return InvalidArguments("get必须同时提供期望清单SHA和import-id。");
    if (arguments.cursor && (!arguments.inventorySha || arguments.cursor->inventorySha256 != *arguments.inventorySha
        || arguments.cursor->binding.sourceSha256 != arguments.sourceSha || arguments.cursor->query != arguments.query))
        return InvalidArguments("续页必须提供与游标一致的来源SHA、清单SHA和query。");

    const std::filesystem::path path(argv[2]);
    std::error_code error;
    const bool directory = std::filesystem::is_directory(path, error);
    if (error) return Failure("IMPORT_SOURCE_OPEN", "无法确认来源路径类型。");
    const ImportInventoryBinding binding{std::string(directory ? "imports-cli-directory-" : "imports-cli-archive-")
        + arguments.sourceSha, 1, arguments.sourceSha, arguments.sourceSha, arguments.sourceSha, "committed-source"};
    if (directory) {
        auto workspace = DirectoryWorkspace::Open(path);
        if (!workspace.Succeeded() || !workspace.value) { PrintDiagnostics(workspace.diagnostics); return 1; }
        const auto captured = CaptureImportInventory(*workspace.value, binding);
        if (!captured.Succeeded() || !captured.value) { PrintDiagnostics(captured.diagnostics); return 1; }
        return Execute(**captured.value, arguments);
    }
    auto session = MapArchiveSession::Open(path);
    if (!session.Succeeded() || !session.value) { PrintDiagnostics(session.diagnostics); return 1; }
    const auto captured = CaptureImportInventory(*session.value, binding);
    if (!captured.Succeeded() || !captured.value) { PrintDiagnostics(captured.diagnostics); return 1; }
    return Execute(**captured.value, arguments);
} catch (const ResponseBudget&) {
    return Failure("IMPORT_RESPONSE_BUDGET", "完整导入元数据JSON超过256KiB，请缩小页长；未输出部分JSON。");
} catch (const std::exception&) {
    return Failure("IMPORT_QUERY_FAILED", "导入元数据查询失败，未发布成功回执。");
}
