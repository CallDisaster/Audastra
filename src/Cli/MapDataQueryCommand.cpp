#include "MapDataQueryCommand.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Services/MapDataQuery.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Services;

constexpr std::size_t kMaxJsonBytes = 1024 * 1024;

void PrintDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

bool LowerSha256(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string_view StateName(MapDataQueryState state) {
    switch (state) {
    case MapDataQueryState::Missing: return "missing";
    case MapDataQueryState::Ready: return "ready";
    case MapDataQueryState::Partial: return "partial";
    case MapDataQueryState::Invalid: return "invalid";
    case MapDataQueryState::Ambiguous: return "ambiguous";
    case MapDataQueryState::Unreadable: return "unreadable";
    case MapDataQueryState::OpaqueUnknown: return "opaque-unknown";
    }
    return "invalid";
}

std::string_view TextStateName(MapDataQueryTextState state) {
    switch (state) {
    case MapDataQueryTextState::Literal: return "literal";
    case MapDataQueryTextState::Resolved: return "resolved";
    case MapDataQueryTextState::ReferenceInvalid: return "reference-invalid";
    case MapDataQueryTextState::ReferenceMissing: return "reference-missing";
    case MapDataQueryTextState::ReferenceAmbiguous: return "reference-ambiguous";
    case MapDataQueryTextState::StringTableUnavailable: return "string-table-unavailable";
    case MapDataQueryTextState::CatalogUnavailable: return "catalog-unavailable";
    }
    return "invalid";
}

std::string_view FormName(MapDataQueryRowForm form) {
    switch (form) {
    case MapDataQueryRowForm::Record: return "record";
    case MapDataQueryRowForm::NormalDoodad: return "normal-doodad";
    case MapDataQueryRowForm::SpecialDoodad: return "special-doodad";
    }
    return "invalid";
}

bool ValidUtf8(std::string_view bytes) {
    if (bytes.empty()) return true;
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

void Quoted(std::ostream& out, std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) out << "\\u00" << digits[c >> 4] << digits[c & 15];
            else out << static_cast<char>(c);
            break;
        }
    }
    out << '"';
}

void Utf8OrNull(std::ostream& out, std::string_view bytes) {
    if (ValidUtf8(bytes)) Quoted(out, bytes);
    else out << "null";
}

void Hex(std::ostream& out, std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char c : bytes) out << digits[c >> 4] << digits[c & 15];
    out << '"';
}

void Binding(std::ostream& out, const MapDataQueryBinding& binding) {
    out << "{\"document_id\":";
    Quoted(out, binding.documentId);
    out << ",\"document_generation\":\"" << binding.generation
        << "\",\"revision\":\"" << binding.revision << "\",\"source_sha256\":";
    Quoted(out, binding.sourceSha256);
    out << '}';
}

void Member(std::ostream& out, const MapDataQueryMember& member) {
    out << "{\"kind\":";
    Quoted(out, MapData::MapDataKindId(member.kind));
    out << ",\"member_name\":";
    Quoted(out, member.memberName);
    out << ",\"state\":";
    Quoted(out, StateName(member.state));
    out << ",\"record_count\":";
    if (member.recordCount) out << '"' << *member.recordCount << '"';
    else out << "null";
    out << ",\"member_sha256\":";
    if (member.memberSha256) Quoted(out, *member.memberSha256);
    else out << "null";
    out << ",\"diagnostic_count\":\"" << member.diagnosticCount << "\"}";
}

void Text(std::ostream& out, const MapDataQueryText& value) {
    out << "{\"raw_hex\":";
    Hex(out, value.rawBytes);
    out << ",\"raw_utf8\":";
    Utf8OrNull(out, value.rawBytes);
    out << ",\"display_hex\":";
    if (value.displayBytes) Hex(out, *value.displayBytes);
    else out << "null";
    out << ",\"display_utf8\":";
    if (value.displayBytes) Utf8OrNull(out, *value.displayBytes);
    else out << "null";
    out << ",\"state\":";
    Quoted(out, TextStateName(value.state));
    out << '}';
}

void Field(std::ostream& out, const MapDataQueryField& field) {
    const bool isDefault = MapData::IsMapFieldDefault(*field.descriptor, field.value);
    out << "{\"id\":";
    Quoted(out, field.descriptor->id);
    out << ",\"type\":";
    Quoted(out, MapData::MapFieldTypeId(field.value.type));
    out << ",\"writable\":" << (field.writable ? "true" : "false") << ",\"raw\":";
    Quoted(out, MapData::FormatMapFieldRaw(field.value));
    out << ",\"is_default\":" << (isDefault ? "true" : "false") << ",\"value\":";
    if (isDefault) {
        out << "null";
    } else if (field.value.type == MapData::MapFieldType::Bytes) {
        Utf8OrNull(out, field.value.bytes);
    } else if (const auto number = MapData::MapFieldDisplayNumber(*field.descriptor, field.value)) {
        const auto precision = out.precision();
        out.precision(std::numeric_limits<double>::max_digits10);
        out << *number;
        out.precision(precision);
    } else {
        out << "null";
    }
    out << '}';
}

void Row(std::ostream& out, const MapDataQueryRow& row) {
    out << "{\"source_index\":\"" << row.sourceIndex << "\",\"locator_scope\":\"snapshot-revision\",\"form\":";
    Quoted(out, FormName(row.form));
    out << ",\"rawcode_hex\":";
    Hex(out, row.rawcodeBytes);
    out << ",\"rawcode_utf8\":";
    Utf8OrNull(out, row.rawcodeBytes);
    out << ",\"creation_number\":";
    if (row.creationNumber) out << *row.creationNumber;
    else out << "null";
    out << ",\"string_id\":";
    if (row.stringId) out << '"' << *row.stringId << '"';
    else out << "null";
    out << ",\"name\":";
    Text(out, row.name);
    out << ",\"owner\":";
    if (row.owner) out << *row.owner;
    else out << "null";
    out << ",\"position_world\":";
    if (row.positionWorld && std::all_of(row.positionWorld->begin(), row.positionWorld->end(),
        [](float value) { return std::isfinite(value); })) {
        out << '[' << (*row.positionWorld)[0] << ',' << (*row.positionWorld)[1] << ',' << (*row.positionWorld)[2] << ']';
    } else out << "null";
    out << ",\"position_world_raw_u32\":";
    if (row.positionWorld) {
        out << '[' << std::bit_cast<std::uint32_t>((*row.positionWorld)[0]) << ','
            << std::bit_cast<std::uint32_t>((*row.positionWorld)[1]) << ','
            << std::bit_cast<std::uint32_t>((*row.positionWorld)[2]) << ']';
    } else out << "null";
    out << ",\"grid_position\":";
    if (row.gridPosition) out << '[' << (*row.gridPosition)[0] << ',' << (*row.gridPosition)[1] << ']';
    else out << "null";
    out << ",\"resource_path_hex\":";
    if (row.resourcePathBytes) Hex(out, *row.resourcePathBytes);
    else out << "null";
    out << ",\"resource_path_utf8\":";
    if (row.resourcePathBytes) Utf8OrNull(out, *row.resourcePathBytes);
    else out << "null";
    out << ",\"row_key\":";
    Quoted(out, row.rowKey);
    out << ",\"fields\":[";
    for (std::size_t index = 0; index < row.fields.size(); ++index) {
        if (index != 0) out << ',';
        Field(out, row.fields[index]);
    }
    out << "]}";
}

} // namespace

int RunMapDataQuery(const std::filesystem::path& archivePath,
    std::string_view expectedSourceSha256, std::string_view kindText,
    std::string_view startIndexText, std::string_view limitText) {
    const auto kind = MapData::ParseMapDataKindId(kindText);
    const auto start = ParseUnsigned(startIndexText);
    const auto limit = ParseUnsigned(limitText);
    if (!LowerSha256(expectedSourceSha256) || !kind || !start || !limit || *limit == 0 || *limit > 128) {
        std::cerr << "MapData.Query: MAP_DATA_QUERY_ARGUMENT_INVALID: expected lowercase SHA-256, one of seven kinds, start index, and limit 1..128\n";
        return 2;
    }
    auto archive = MapArchiveSession::Open(archivePath);
    if (!archive.Succeeded() || !archive.value) { PrintDiagnostics(archive.diagnostics); return 1; }
    const auto& sourceSha256 = archive.value->SourceSha256();
    if (std::string_view(sourceSha256) != expectedSourceSha256) {
        std::cerr << "MapData.Query: MAP_DATA_QUERY_STALE: source SHA-256 changed\n";
        return 1;
    }
    MapDataQueryBinding binding{"archive-readonly-" + sourceSha256, 1, 0, sourceSha256};
    auto captured = CaptureMapDataSnapshot(MapDocument::FromArchive(*archive.value), binding,
        archive.value->HasOpaqueMembers());
    if (!captured.Succeeded() || !captured.value) { PrintDiagnostics(captured.diagnostics); return 1; }
    auto described = DescribeMapData(*captured.value, binding);
    if (!described.Succeeded() || !described.value) { PrintDiagnostics(described.diagnostics); return 1; }
    const MapDataQueryCursor cursor{binding, *kind, *start};
    auto page = QueryMapDataPage(*captured.value, binding, *kind, cursor, static_cast<std::size_t>(*limit));
    auto output = std::ostringstream{};
    output.imbue(std::locale::classic());
    output << std::setprecision(9) << std::boolalpha;
    output << "{\"ok\":" << (page.Succeeded() ? "true" : "false")
        << ",\"schema_version\":1,\"workspace_kind\":\"map-archive-readonly\""
        << ",\"binding_scope\":\"source-sha-bound-readonly-cli\""
        << ",\"generation_revision_scope\":\"single-command-open\",\"binding\":";
    Binding(output, binding);
    output << ",\"has_opaque_archive_members\":" << described.value->hasOpaqueArchiveMembers
        << ",\"members\":[";
    for (std::size_t i = 0; i < described.value->members.size(); ++i) {
        if (i) output << ',';
        Member(output, described.value->members[i]);
    }
    output << "],\"query\":{\"kind\":";
    Quoted(output, kindText);
    output << ",\"start_index\":\"" << *start << "\",\"limit\":" << *limit << '}';
    if (page.Succeeded() && page.value) {
        output << ",\"member\":";
        Member(output, page.value->member);
        output << ",\"logical_bytes\":\"" << page.value->logicalBytes << "\",\"rows\":[";
        for (std::size_t i = 0; i < page.value->rows.size(); ++i) {
            if (i) output << ',';
            Row(output, page.value->rows[i]);
        }
        output << "],\"next_cursor\":";
        if (page.value->next) {
            output << "{\"binding\":";
            Binding(output, page.value->next->binding);
            output << ",\"kind\":";
            Quoted(output, MapData::MapDataKindId(page.value->next->kind));
            output << ",\"next_index\":\"" << page.value->next->nextIndex << "\"}";
        } else output << "null";
    } else {
        const auto index = static_cast<std::size_t>(*kind);
        output << ",\"member\":";
        Member(output, described.value->members[index]);
        output << ",\"error_code\":";
        if (!page.diagnostics.empty()) Quoted(output, page.diagnostics.front().code);
        else Quoted(output, "MAP_DATA_QUERY_UNKNOWN_ERROR");
        output << ",\"rows\":null,\"next_cursor\":null";
    }
    output << '}';
    if (output.view().size() > kMaxJsonBytes) {
        std::cerr << "MapData.Query: MAP_DATA_QUERY_JSON_BUDGET: output exceeds 1 MiB\n";
        return 1;
    }
    const auto current = archive.value->VerifySourceUnchanged();
    if (!current.Succeeded()) { PrintDiagnostics(current.diagnostics); return 1; }
    std::cout << output.view() << '\n';
    if (!page.Succeeded()) PrintDiagnostics(page.diagnostics);
    return page.Succeeded() ? 0 : 1;
}

int RunMapFieldCatalog() {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"fields\":[";
    const auto fields = MapData::MapFieldCatalog();
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& field = fields[index];
        if (index != 0) output << ',';
        output << "{\"kind\":";
        Quoted(output, MapData::MapDataKindId(field.kind));
        output << ",\"id\":";
        Quoted(output, field.id);
        output << ",\"label\":";
        Quoted(output, field.label);
        output << ",\"type\":";
        Quoted(output, MapData::MapFieldTypeId(field.type));
        output << ",\"writable\":" << (field.writable ? "true" : "false") << ",\"constraint\":";
        Quoted(output, field.constraint);
        output << '}';
    }
    output << "]}\n";
    std::cout << output.str();
    return 0;
}
