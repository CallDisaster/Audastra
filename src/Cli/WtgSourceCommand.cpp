#include "WtgSourceCommand.h"
#include "War3MapCore/Trigger/WtgSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <charconv>
#include <iostream>
#include <sstream>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Errors(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message;
        if (diagnostic.origin) std::cerr << " @" << diagnostic.origin->offset << '+' << diagnostic.origin->length;
        std::cerr << '\n';
    }
}
std::optional<std::size_t> Index(std::string_view value);
std::optional<WtgSourceLimits> ReadLimits(std::string_view maxNodes) {
    WtgSourceLimits limits;
    if (!maxNodes.empty()) {
        const auto value = Index(maxNodes);
        if (!value || !*value || *value > WtgSourceLimits::hardMaxNodes) {
            std::cerr << "WTG_SOURCE_LIMITS: --max-nodes must be 1.." << WtgSourceLimits::hardMaxNodes << '\n';
            return std::nullopt;
        }
        limits.maxNodes = *value;
    }
    return limits;
}
std::optional<WtgSource> Open(const std::filesystem::path& file, std::string_view sha,
    const std::filesystem::path& uiRoot, std::string_view catalogSha = {}, std::string_view maxNodes = {}) {
    const auto limits = ReadLimits(maxNodes);
    if (!limits) return std::nullopt;
    auto bytes = IO::ReadFileBytes(file, limits->maxBytes);
    if (!bytes.Succeeded()) { Errors(bytes.diagnostics); return std::nullopt; }
    const auto hash = Sha256(*bytes.value);
    if (!hash.Succeeded()) { Errors(hash.diagnostics); return std::nullopt; }
    if (*hash.value != sha) { std::cerr << "WTG_SOURCE_STALE: source SHA differs\n"; return std::nullopt; }
    auto catalog = UiCatalog::Load(uiRoot);
    if (!catalog.Succeeded()) { Errors(catalog.diagnostics); return std::nullopt; }
    if (!catalogSha.empty() && catalog.value->Sha256() != catalogSha) {
        std::cerr << "WTG_SOURCE_STALE: Catalog SHA differs\n"; return std::nullopt;
    }
    auto source = ParseWtgSource(*bytes.value, *catalog.value, *limits);
    if (!source.Succeeded()) { Errors(source.diagnostics); return std::nullopt; }
    return std::move(*source.value);
}
int Emit(const std::filesystem::path& file, std::string_view sha, const std::ostringstream& text) {
    const auto current = Sha256File(file);
    if (!current.Succeeded()) { Errors(current.diagnostics); return 1; }
    if (*current.value != sha) { std::cerr << "WTG_SOURCE_STALE: source changed while reading\n"; return 1; }
    std::cout << text.str();
    return 0;
}
std::optional<std::size_t> Index(std::string_view value) {
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}
void Header(std::ostringstream& out, const WtgSourceSnapshot& data) {
    out << "schema\tforge-wtg-source-tsv-v1\nsource_sha256\t" << data.sourceSha256
        << "\ncatalog_sha256\t" << data.catalogSha256 << "\nbase_version\t" << data.baseVersion
        << "\nsubversion_marker\t" << (data.subVersionMarker ? std::to_string(*data.subVersionMarker) : "none")
        << "\ngame_version_raw\t" << data.document.variablesHeader << "\neditable\tfalse\nwct_pairing\tunverified\n";
}
}

int RunWtgSourceInspect(const std::filesystem::path& file, std::string_view sha, const std::filesystem::path& uiRoot,
    std::string_view maxNodes) {
    auto source = Open(file, sha, uiRoot, {}, maxNodes);
    if (!source) return 1;
    const auto data = source->Snapshot();
    std::ostringstream out; Header(out, *data);
    if (!maxNodes.empty()) out << "parse_max_nodes\t" << ReadLimits(maxNodes)->maxNodes << '\n';
    out << "items\t" << data->items.size() << "\nvariables\t" << data->document.variables.size()
        << "\necas\t" << data->document.ecaNodes.size() << "\narguments\t" << data->document.argumentNodes.size() << '\n';
    const auto retained = Sha256(source->OriginalBytes());
    if (!retained.Succeeded()) { Errors(retained.diagnostics); return 1; }
    out << "retained_sha256\t" << *retained.value << '\n';
    if (data->subVersionMarker)
        for (const auto& group : data->counts)
            out << "count_group\t" << group.type << '\t' << group.declaredCount << '\t' << group.deletedIds.size()
                << '\t' << group.origin.offset << '\t' << group.origin.length << '\n';
    return Emit(file, sha, out);
}

int RunWtgSourceQuery(const std::filesystem::path& file, std::string_view sha, const std::filesystem::path& uiRoot,
    std::string_view catalogSha, std::string_view tableName, std::string_view startText, std::string_view limitText,
    std::string_view maxNodes, std::string_view triggerItem) {
    std::optional<WtgSourceTable> table;
    if (tableName == "items") table = WtgSourceTable::Items;
    else if (tableName == "variables") table = WtgSourceTable::Variables;
    else if (tableName == "deleted") table = WtgSourceTable::DeletedIds;
    else if (tableName == "ecas") table = WtgSourceTable::Ecas;
    else if (tableName == "arguments") table = WtgSourceTable::Arguments;
    const auto start = Index(startText), limit = Index(limitText);
    if (!table || !start || !limit || *limit == 0 || *limit > 128 || catalogSha.empty()) {
        std::cerr << "WTG_SOURCE_QUERY_ARGUMENT: invalid table/page/Catalog identity\n"; return 1;
    }
    const auto trigger = triggerItem.empty() ? std::optional<std::size_t>{} : Index(triggerItem);
    if (!triggerItem.empty() && (!trigger || (table != WtgSourceTable::Ecas && table != WtgSourceTable::Arguments))) {
        std::cerr << "WTG_SOURCE_QUERY_ARGUMENT: --trigger needs an items row number and the ecas or arguments table\n"; return 1;
    }
    auto source = Open(file, sha, uiRoot, catalogSha, maxNodes);
    if (!source) return 1;
    const auto page = trigger ? QueryWtgSourceTrigger(*source, sha, catalogSha, *trigger, *table, *start, *limit)
                              : QueryWtgSource(*source, sha, catalogSha, *table, *start, *limit);
    if (!page.Succeeded()) { Errors(page.diagnostics); return 1; }
    std::ostringstream out; Header(out, *source->Snapshot());
    if (!maxNodes.empty()) out << "parse_max_nodes\t" << ReadLimits(maxNodes)->maxNodes << '\n';
    out << "table\t" << tableName << '\n';
    if (trigger) out << "trigger_item\t" << *trigger << '\n';
    out << "total_rows\t" << page.value->totalRows << '\n';
    for (const auto& row : page.value->rows) {
        out << "row\t" << row.index << '\t' << row.sourceId << '\t' << row.origin.offset << '\t' << row.origin.length << '\n';
        for (const auto& field : row.fields)
            out << "field\t" << row.index << '\t' << field.name << '\t' << field.encoding << '\t' << field.value << '\n';
    }
    return Emit(file, sha, out);
}
