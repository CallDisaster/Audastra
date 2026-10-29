#include "TriggerRemovalAuditCommand.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include <nlohmann/json.hpp>
#include <charconv>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <chrono>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
using Json = nlohmann::json;
std::string Utf8(const wchar_t* value) { const auto text = std::filesystem::path(value).u8string(); return {text.begin(), text.end()}; }
StableId Id(std::string_view value) {
    StableId id = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) throw std::invalid_argument("StableId必须为十进制整数。");
    return id;
}
const char* Kind(AtomicRenameCodeKind kind) {
    switch (kind) {
    case AtomicRenameCodeKind::GlobalCustomCode: return "global_custom_code";
    case AtomicRenameCodeKind::TriggerCustomCode: return "trigger_custom_code";
    case AtomicRenameCodeKind::ArgumentCode: return "argument_code";
    case AtomicRenameCodeKind::VariableInitialValue: return "variable_initial_value";
    }
    throw std::invalid_argument("未知代码块种类。");
}
Json Blocks(const std::vector<RemovalCodeBlock>& blocks) {
    Json result = Json::array();
    for (const auto& block : blocks) {
        const auto& id = block.identity; Json evidence = Json::array();
        for (const auto& hit : block.evidence) evidence.push_back({{"context", hit.context}, {"text", hit.text}, {"snippet_truncated", hit.text.size() < hit.decodedByteLength}, {"line", hit.line}, {"column", hit.column},
            {"source_file", hit.sourceFile}, {"source_line", hit.sourceLine},
            {"decoded_byte_offset", hit.decodedByteOffset}, {"decoded_byte_length", hit.decodedByteLength},
            {"raw_byte_offset", hit.rawByteOffset}, {"raw_byte_length", hit.rawByteLength}, {"raw_range_exact", hit.rawRangeExact}, {"definite", hit.definite}});
        result.push_back({{"identity", {{"kind", Kind(id.kind)}, {"id", std::to_string(id.id)}, {"encoding", TriggerEditEncodingName(id.encoding)},
            {"source_sha256", id.sourceSha256}, {"evidence_sha256", id.evidenceSha256}}}, {"complete", block.complete}, {"dynamic", block.dynamic},
            {"referenced", block.referenced}, {"incomplete_reason", block.incompleteReason}, {"evidence", evidence},
            {"sample_text", block.sampleText}, {"sample_truncated", block.sampleTruncated}});
    }
    return result;
}
Json Diagnostics(const std::vector<Diagnostic>& values) {
    Json result = Json::array(); for (const auto& d : values) result.push_back({{"code", d.code}, {"message", d.message}}); return result;
}
int Emit(const Json& output) {
    const auto text = output.dump();
    if (text.size() > 4 * 1024 * 1024) throw std::invalid_argument("完整审计回执超过4MiB，未提供截断结果。");
    std::cout << text << '\n';
    return output.at("ok").get<bool>() ? 0 : 1;
}
using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
Json PeakMemory() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return nullptr;
    return counters.PeakWorkingSetSize;
}
GraphRemove Remove(const TriggerDocument& document, bool trigger, StableId id) {
    GraphRemove op; op.target = {trigger ? GraphRecordKind::Trigger : GraphRecordKind::Variable, id, {}};
    if (trigger) {
        const auto at = std::find_if(document.triggers.begin(), document.triggers.end(), [&](const auto& r) { return r.id == id; });
        if (at == document.triggers.end()) throw std::invalid_argument("触发器ID不存在。");
        op.expectedName = at->name.text;
        const auto cat = std::find_if(document.categories.begin(), document.categories.end(), [&](const auto& r) { return r.categoryId == at->categoryId; });
        if (cat == document.categories.end()) throw std::invalid_argument("触发器分类不存在。");
        op.parent = {GraphParentKind::CategoryTriggers, {GraphRecordKind::Category, cat->id, {}}, {}, {}};
        for (const auto& row : document.triggers) if (row.categoryId == at->categoryId) op.parent.orderedChildren.push_back({GraphRecordKind::Trigger, row.id, {}});
    } else {
        const auto at = std::find_if(document.variables.begin(), document.variables.end(), [&](const auto& r) { return r.id == id; });
        if (at == document.variables.end()) throw std::invalid_argument("变量ID不存在。");
        op.expectedName = at->name.text; op.parent.kind = GraphParentKind::Variables;
        for (const auto& row : document.variables) op.parent.orderedChildren.push_back({GraphRecordKind::Variable, row.id, {}});
    }
    return op;
}
void Keys(const Json& value, std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) throw std::invalid_argument("删除确认字段不完整或含冗余字段。");
    for (auto key : keys) if (!value.contains(key)) throw std::invalid_argument("删除确认缺少字段。");
}
std::vector<RemovalCodeConfirmation> ReadConfirmations(const std::filesystem::path& path, const Services::MapTriggerQuerySources& source,
    const DeclarationRemovalReview& review) {
    if (std::filesystem::file_size(path) > 4 * 1024 * 1024) throw std::invalid_argument("删除确认文件超过4MiB。");
    std::ifstream stream(path, std::ios::binary); if (!stream) throw std::invalid_argument("无法读取删除确认文件。");
    std::vector<std::set<std::string>> keys;
    const auto root = Json::parse(stream, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 64) throw std::invalid_argument("删除确认JSON嵌套过深。");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) throw std::invalid_argument("删除确认JSON含重复键。");
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
    Keys(root, {"source_sha256", "catalog_sha256", "target_id", "expected_name", "code_audits"});
    if (root.at("source_sha256") != source.archive->SourceSha256() || root.at("catalog_sha256") != source.catalog.Sha256()
        || root.at("target_id") != std::to_string(review.target.id) || root.at("expected_name") != review.expectedName)
        throw std::invalid_argument("确认文件的地图、目录或目标身份已变化。");
    const auto& audits = root.at("code_audits");
    if (!audits.is_array() || audits.size() > 16385) throw std::invalid_argument("确认条数超出预算。");
    std::vector<RemovalCodeConfirmation> result;
    for (const auto& item : audits) {
        const bool grouped = item.contains("groupable_count") || item.contains("groupable_sha256");
        if (grouped) Keys(item, {"review_sha256", "decision", "reason", "groupable_count", "groupable_sha256"});
        else Keys(item, {"review_sha256", "decision", "reason", "blocks"});
        RemovalCodeConfirmation confirmation{item.at("review_sha256").get<std::string>(), item.at("decision").get<std::string>(), item.at("reason").get<std::string>(), {}};
        if (grouped) {
            if (!item.at("groupable_count").is_number_integer()) throw std::invalid_argument("整组数量必须为整数。");
            const auto count = item.at("groupable_count").get<std::int64_t>();
            if (count <= 0 || count > 262144) throw std::invalid_argument("整组数量超出预算。");
            confirmation.group = RemovalCodeConfirmation::Group{static_cast<std::size_t>(count), item.at("groupable_sha256").get<std::string>()};
            result.push_back(std::move(confirmation)); continue;
        }
        if (!item.at("blocks").is_array() || item.at("blocks").size() > 16384) throw std::invalid_argument("逐块确认超过预算。");
        for (const auto& b : item.at("blocks")) {
            Keys(b, {"kind", "id", "encoding", "source_sha256", "evidence_sha256"});
            std::optional<AtomicRenameCodeKind> kind;
            for (auto k : {AtomicRenameCodeKind::GlobalCustomCode, AtomicRenameCodeKind::TriggerCustomCode, AtomicRenameCodeKind::ArgumentCode, AtomicRenameCodeKind::VariableInitialValue})
                if (b.at("kind") == Kind(k)) kind = k;
            const auto encoding = ParseTriggerEditEncoding(b.at("encoding").get<std::string>());
            if (!kind || !encoding) throw std::invalid_argument("确认的代码块种类或编码无效。");
            confirmation.blocks.push_back({*kind, Id(b.at("id").get<std::string>()), *encoding,
                b.at("source_sha256").get<std::string>(), b.at("evidence_sha256").get<std::string>()});
        }
        result.push_back(std::move(confirmation));
    }
    return result;
}
}
int RunTriggerRemovalAudit(int argc, wchar_t** argv) {
    using namespace War3Map; using namespace War3Map::Trigger;
    if (argc < 7) { std::cerr << "用法：map-trigger-remove-audit <地图> <源SHA> <UI目录> <trigger|variable> <id|list> [--match <精确原名>] [--confirm-file <JSON>]\n"; return 2; }
    try {
        Services::MapTriggerTextQueryRequest request; request.sourceMap = argv[2]; request.expectedSourceSha256 = Utf8(argv[3]); request.uiRoot = argv[4];
        const auto kind = Utf8(argv[5]); if (kind != "trigger" && kind != "variable") throw std::invalid_argument("目标种类必须为trigger或variable。");
        const auto selector = Utf8(argv[6]); std::string match; std::filesystem::path confirmations, priorConfirmations;
        StableId priorTrigger = 0; std::filesystem::path toolRoot, gameRoot;
        for (int i = 7; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::invalid_argument("查询选项缺少值。");
            const auto option = Utf8(argv[i]);
            if (option == "--match" && match.empty()) match = Utf8(argv[i + 1]);
            else if (option == "--confirm-file" && confirmations.empty()) confirmations = argv[i + 1];
            else if (option == "--prior-trigger" && !priorTrigger) priorTrigger = Id(Utf8(argv[i + 1]));
            else if (option == "--prior-confirm-file" && priorConfirmations.empty()) priorConfirmations = argv[i + 1];
            else if (option == "--tool-root" && toolRoot.empty()) toolRoot = argv[i + 1];
            else if (option == "--game-root" && gameRoot.empty()) gameRoot = argv[i + 1];
            else throw std::invalid_argument("未知或重复的删除审计选项。");
        }
        auto source = Services::CaptureMapTriggerQuerySources(request);
        if (!source.Succeeded()) return Emit(Json{{"ok", false}, {"diagnostics", Diagnostics(source.diagnostics)}});
        auto& sources = *source.value;
        TriggerGraphEditLimits limits;
        if (!toolRoot.empty()) {
            auto snapshot = std::make_shared<Services::CodeCheckProjectSnapshot>();
            snapshot->binding = sources.archive->SourceSha256() + sources.catalog.Sha256();
            snapshot->sourcePath = request.sourceMap; snapshot->sourceSha256 = sources.archive->SourceSha256();
            snapshot->toolRoot = toolRoot; snapshot->uiRoot = request.uiRoot; snapshot->gameRoot = gameRoot;
            snapshot->catalog = std::make_shared<UiCatalog>(sources.catalog);
            snapshot->triggers = std::make_shared<TriggerDocument>(sources.document);
            for (std::size_t i = 0; i < snapshot->nativeObjects.size(); ++i) {
                const auto type = static_cast<ObjectData::ObjectType>(i);
                const auto member = ObjectData::NativeFileName(type);
                if (!sources.archive->HasMember(member)) continue;
                const auto bytes = sources.archive->ReadMember(member);
                if (!bytes.Succeeded()) return Emit(Json{{"ok", false}, {"diagnostics", Diagnostics(bytes.diagnostics)}, {"saved", false}});
                auto parsed = ObjectData::NativeObjectCodec(type).Parse(*bytes.value);
                if (!parsed.Succeeded()) return Emit(Json{{"ok", false}, {"diagnostics", Diagnostics(parsed.diagnostics)}, {"saved", false}});
                auto* native = dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get());
                if (!native) throw std::invalid_argument("物编来源不是原生文档。");
                snapshot->nativeObjects[i] = std::make_shared<ObjectData::NativeObjectDocument>(*native);
            }
            limits.removalPreprocessProvider = Services::MakeProjectRemovalPreprocessProvider(std::move(snapshot));
        }
        Json output{{"source_sha256", sources.archive->SourceSha256()}, {"catalog_sha256", sources.catalog.Sha256()}, {"saved", false}, {"runtime_verified", false}};
        if (priorTrigger || !priorConfirmations.empty()) {
            if (!priorTrigger || priorConfirmations.empty() || selector == "list" || selector == "inventory")
                throw std::invalid_argument("前序预演须同时给出触发器ID及原审计确认文件，仅用于目标审计。");
            auto prior = Remove(sources.document, true, priorTrigger);
            auto reviewed = QueryDeclarationRemovalAudit(sources.document, sources.catalog, prior, nullptr, &limits.removalPreprocessProvider);
            if (!reviewed.Succeeded()) { output["ok"] = false; output["diagnostics"] = Diagnostics(reviewed.diagnostics); return Emit(output); }
            prior.codeAudits = ReadConfirmations(priorConfirmations, sources, *reviewed.value);
            const auto start = Clock::now();
            const auto edited = ApplyTriggerGraphEditBatch(sources.document, sources.catalog, {reviewed.value->binding, {prior}}, limits);
            output["prior_preview_ms"] = Milliseconds(start); output["prior_preview_passed"] = edited.Succeeded();
            if (!edited.Succeeded()) { output["ok"] = false; output["diagnostics"] = Diagnostics(edited.diagnostics); return Emit(output); }
        }
        if (selector == "inventory") {
            if (!confirmations.empty() || !match.empty()) throw std::invalid_argument("分类统计不接受确认或目标名字。");
            const auto report = InspectOpaqueArgumentInventory(sources.document, sources.catalog);
            if (!report.Succeeded()) { output["ok"] = false; output["diagnostics"] = Diagnostics(report.diagnostics); return Emit(output); }
            Json rows = Json::array();
            for (const auto& row : report.value->classes) {
                Json samples = Json::array();
                for (const auto& sample : row.samples) samples.push_back({{"id", std::to_string(sample.id)}, {"bytes", sample.bytes}, {"text", sample.text}});
                rows.push_back({{"expected_type", row.expectedType}, {"literal_kind", row.literalKind}, {"validate_literal", row.validateLiteral},
                    {"opaque", row.opaque}, {"dispatch", row.dispatch}, {"count", row.count}, {"bytes", row.bytes}, {"short_count", row.shortCount}, {"samples", samples}});
            }
            output.update({{"ok", true}, {"statistics_only", true}, {"complete", true}, {"arguments", report.value->arguments},
                {"opaque_arguments", report.value->opaqueArguments}, {"total_bytes", report.value->totalBytes}, {"classes", rows}});
        } else if (selector == "list") {
            if (!confirmations.empty()) throw std::invalid_argument("list不接受删除确认。");
            Json rows = Json::array(); std::size_t count = 0;
            const auto add = [&](const auto& values) {
                for (const auto& row : values) if (match.empty() || row.name.text == match) {
                    ++count; if (rows.size() < 128) rows.push_back({{"id", std::to_string(row.id)}, {"name", row.name.text}});
                }
            };
            if (kind == "trigger") add(sources.document.triggers); else add(sources.document.variables);
            output["rows"] = rows; output["truncated"] = count > rows.size(); output["ok"] = true;
        } else {
            if (!match.empty()) throw std::invalid_argument("--match只用于list，审计须提供稳定ID。");
            const auto id = Id(selector); if (!id) throw std::invalid_argument("删除目标ID不能为0。");
            auto op = Remove(sources.document, kind == "trigger", id);
            const auto queryStart = Clock::now();
            const auto review = QueryDeclarationRemovalAudit(sources.document, sources.catalog, op, nullptr, &limits.removalPreprocessProvider);
            output["audit_ms"] = Milliseconds(queryStart); output["process_peak_working_set_bytes"] = PeakMemory();
            if (!review.Succeeded()) { output["ok"] = false; output["diagnostics"] = Diagnostics(review.diagnostics); return Emit(output); }
            const auto& r = *review.value; Json typed = Json::array();
            for (const auto value : r.typedArgumentIds) typed.push_back(std::to_string(value));
            output.update({{"ok", true}, {"target_id", std::to_string(id)}, {"expected_name", r.expectedName}, {"review_sha256", r.reviewSha256},
                {"complete", r.complete}, {"symbols", r.symbols}, {"typed_argument_ids", typed},
                {"groupable_count", r.groupableCount}, {"groupable_sha256", r.groupableSha256}, {"groupable_samples", Blocks(r.groupableSamples)},
                {"individual_blocks", Blocks(r.individualBlocks)}, {"external_dependencies_verified", r.externalDependenciesVerified},
                {"preprocess_identity_sha256", r.preprocessIdentitySha256}, {"preprocess_profile_sha256", r.preprocessProfileSha256}, {"included_file_count", r.includedFileCount}});
            if (!confirmations.empty()) {
                op.codeAudits = ReadConfirmations(confirmations, sources, r);
                const auto checkStart = Clock::now();
                const auto checked = CheckDeclarationRemovalAudit(sources.document, sources.catalog, op, nullptr, &limits.removalPreprocessProvider);
                output["check_ms"] = Milliseconds(checkStart);
                if (!checked.Succeeded()) {
                    output["preview_passed"] = false; output["diagnostics"] = Diagnostics(checked.diagnostics); output["ok"] = false;
                    output["process_peak_working_set_bytes"] = PeakMemory(); return Emit(output);
                }
                const auto previewStart = Clock::now();
                const auto edited = ApplyTriggerGraphEditBatch(sources.document, sources.catalog, {r.binding, {op}}, limits);
                output["preview_ms"] = Milliseconds(previewStart); output["process_peak_working_set_bytes"] = PeakMemory();
                output["preview_passed"] = edited.Succeeded(); output["diagnostics"] = Diagnostics(edited.diagnostics); output["ok"] = edited.Succeeded();
                output["code_coverage"] = "CallerAssertedNotRuntimeProven";
            }
        }
        const auto unchanged = sources.archive->VerifySourceUnchanged();
        if (!unchanged.Succeeded()) { output["ok"] = false; output["diagnostics"] = Diagnostics(unchanged.diagnostics); }
        return Emit(output);
    } catch (const std::exception& e) { std::cout << Json{{"ok", false}, {"code", "TRIGGER_REMOVE_QUERY_INVALID"}, {"message", e.what()}, {"saved", false}}.dump() << '\n'; return 1; }
}
