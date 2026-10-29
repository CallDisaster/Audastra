#include "McpVariableTools.h"
#include "McpTriggerGraphJson.h"
#include "McpServer.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
using namespace War3Map::Mcp;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
QString Code(const QJsonObject& result) {
    const auto rows = result.value("diagnostics").toArray();
    return rows.isEmpty() ? QString{} : rows.first().toObject().value("code").toString();
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << text;
    Require(stream.good(), "write original synthetic catalog");
}
struct Fixture {
    QTemporaryDir temporary;
    UiCatalog catalog;
    TriggerDocument document;
    VariableToolHost host;
    bool independent = false, active = true, rejectPublish = false;
    std::uint64_t generation = 0;
    int attempts = 0, previews = 0, publishes = 0;
    QJsonObject hostChanges;
    explicit Fixture(bool transaction = false) : independent(transaction) {
        Require(temporary.isValid(), "synthetic temporary catalog");
        const auto root = std::filesystem::path(temporary.path().toStdWString()) / "ui";
        Write(root / "config", "base\n");
        Write(root / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\n"
            "string=0,1,1,String,string\nunit=0,1,1,Unit,unit\nlocalonly=0,0,0,Local,string\n"
            "[TriggerParams]\nFixedOne=0,integer,1,One\n");
        Write(root / "base/action.txt", "[StoreInteger]\n[[.args]]\ntype=integer\n");
        for (const auto* name : {"event.txt", "condition.txt", "call.txt"}) Write(root / "base" / name, {});
        auto loaded = UiCatalog::Load(root); Require(loaded.Succeeded(), "load original type catalog"); catalog = std::move(*loaded.value);
        auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "create trusted paired source seal"); document = std::move(*empty.value);
        const auto category = TriggerDirectoryEditor::AddCategory(document, "业务"); Require(category.Succeeded(), "synthetic category");
        Require(TriggerDocumentEditor::AddTrigger(document, *category.value, "OriginalBusiness").Succeeded(), "synthetic trigger");
        host.identity = [&] {
            QJsonObject identity{{"project_identity", "synthetic-variable-map"},
                {"revision", QStringLiteral("epoch:%1:active:%2").arg(generation).arg(active ? 1 : 0)}};
            if (independent) {
                identity.insert("transaction_active", active);
                identity.insert("transaction_id", active ? QStringLiteral("7") : QStringLiteral("0"));
                identity.insert("transaction_revision", QJsonObject{{"generation", "0"}, {"source_sha256", QString(64, 'a')},
                    {"ui_catalog_sha256", QString::fromStdString(catalog.Sha256())}});
            }
            return identity;
        };
        host.context = [&] { return Result<VariableToolContext>::Success(VariableToolContext{&document, &catalog, {}}); };
        host.apply = [&](const TriggerGraphEditBatch& batch, const VariablePrepublishValidator& validate) {
            ++attempts;
            if (independent && !active)
                return Result<TriggerGraphEditReceipt>::Failure("Test", "VARIABLE_ACTIVE_TRANSACTION_REQUIRED", "没有活动事务。");
            TriggerDocument candidate(document);
            auto edited = ApplyTriggerGraphEditBatch(candidate, catalog, batch);
            if (!edited.Succeeded()) return edited;
            ++previews;
            const auto accepted = validate(document, candidate, *edited.value, hostChanges);
            if (!accepted.Succeeded()) return Result<TriggerGraphEditReceipt>{{}, accepted.diagnostics};
            if (rejectPublish) return Result<TriggerGraphEditReceipt>::Failure("Test", "HOST_PUBLICATION_DENIED", "候选发布前拒绝。");
            const bool changed = edited.value->before.currentWtgSha256 != edited.value->after.currentWtgSha256
                || edited.value->before.currentWctSha256 != edited.value->after.currentWctSha256;
            if (changed || independent) { document = std::move(candidate); ++generation; ++publishes; }
            return edited;
        };
    }
    QJsonObject IdentityArguments() const {
        const auto identity = host.identity();
        return {{"project_identity", identity.value("project_identity")}, {"expected_revision", identity.value("revision")}};
    }
    QJsonObject CreateRequest(const char* name = "Counter", const char* type = "integer") const {
        auto request = IdentityArguments(); request.insert("name", name); request.insert("type_name", type);
        request.insert("initialized", true); request.insert("initial_value", "42"); return request;
    }
    StableId Create(const char* name = "Counter", const char* type = "integer") {
        const auto result = InvokeVariableTool("variables_create", CreateRequest(name, type), host);
        Require(result.value("ok").toBool(), "facade creates a real typed variable");
        bool valid = false; const auto id = result.value("id").toString().toULongLong(&valid);
        Require(valid && id && result.value("before").isNull()
            && result.value("after").toObject().value("ref").toObject().value("id") == result.value("id"), "generated stable identity and complete create receipt");
        return id;
    }
    QJsonObject For(StableId id) const { auto request = IdentityArguments(); request.insert("id", QString::number(id)); return request; }
    EncodedTriggerDocument Bytes() const {
        const auto bytes = WtgWctCodec{}.Serialize(document, catalog); Require(bytes.Succeeded(), "current paired bytes"); return *bytes.value;
    }
    void Reparse() {
        const auto bytes = Bytes(); const auto parsed = WtgWctCodec{}.Parse(bytes.wtg, bytes.wct, catalog);
        Require(parsed.Succeeded(), "seal authored opaque fixture"); document = *parsed.value; ++generation;
    }
};
void SameBytes(const Fixture& fixture, const EncodedTriggerDocument& before, int publishes) {
    const auto after = fixture.Bytes(); Require(before.wtg == after.wtg && before.wct == after.wct
        && fixture.publishes == publishes, "failed operation preserves both paired bytes and publication count");
}

void SchemasAndSuccessfulHosts() {
    const auto definitions = VariableToolDefinitions(); Require(definitions.size() == 6, "one source defines exactly six variable tools");
    std::set<QString> names;
    for (const auto& definition : definitions) {
        names.insert(definition.name);
        Require(!definition.inputSchema.value("additionalProperties").toBool(true)
            && definition.maxResponseBytes == 1024U * 1024U && definition.maxRequestIdBytes == 4096,
            "strict schemas and explicit body/transport limits");
    }
    Require(names == std::set<QString>{"variables_list", "variables_get", "variables_create", "variables_update", "variables_rename", "variables_delete"}, "six final public names");
    for (const bool independent : {false, true}) {
        Fixture f(independent); const auto id = f.Create();
        auto get = f.For(id); const auto row = InvokeVariableTool("variables_get", get, f.host);
        Require(row.value("ok").toBool() && row.value("variable").toObject().value("value").toObject().value("text") == "42", "exact get returns stored literal and RawText");
        auto update = f.For(id); update.insert("changes", QJsonObject{{"initial_value", "73"}});
        const auto edited = InvokeVariableTool("variables_update", update, f.host);
        Require(edited.value("ok").toBool() && edited.value("changed").toBool()
            && edited.value("before").toObject().value("value").toObject().value("text") == "42"
            && edited.value("after").toObject().value("value").toObject().value("text") == "73", "both hosts expose complete actual before and after");
        Require(!edited.value("saved").toBool() && !edited.value("runtime_verified").toBool(), "edit does not claim save or runtime");
        if (independent) Require(edited.value("transaction_active").toBool()
            && edited.value("transaction_id") == "7" && edited.value("transaction_revision").isObject(), "facade preserves the original transaction envelope");
        const auto generation = f.generation;
        const auto publications = f.publishes;
        update = f.For(id); update.insert("changes", QJsonObject{{"initial_value", "73"}});
        const auto noChange = InvokeVariableTool("variables_update", update, f.host);
        Require(noChange.value("ok").toBool() && !noChange.value("changed").toBool()
            && f.generation == generation + (independent ? 1 : 0) && f.publishes == publications + (independent ? 1 : 0), "no-op follows each original host generation/Undo policy");
        auto rename = f.For(id); rename.insert("name", "Renamed");
        const auto renamed = InvokeVariableTool("variables_rename", rename, f.host);
        Require(renamed.value("ok").toBool() && !renamed.value("external_dependencies_verified").toBool()
            && renamed.value("renames").toArray().first().toObject().value("code_coverage") == "no-opaque-code", "atomic rename keeps evidence boundary");
        const auto removed = InvokeVariableTool("variables_delete", f.For(id), f.host);
        Require(removed.value("ok").toBool() && removed.value("after").isNull() && f.document.variables.empty(), "unreferenced variable removes through Graph command");
    }
}

void PaginationAndExactIds() {
    Fixture f; const auto first = f.Create("First"); const auto second = f.Create("Second");
    auto page = QJsonObject{{"page", QJsonObject{{"offset", 1}, {"limit", 1}}}};
    const auto listed = InvokeVariableTool("variables_list", page, f.host);
    Require(listed.value("ok").toBool() && listed.value("total") == "2" && listed.value("rows").toArray().size() == 1
        && listed.value("rows").toArray().first().toObject().value("ref").toObject().value("id") == QString::number(second), "list preserves document order and bounded page");
    constexpr StableId large = 9007199254740993ULL;
    f.document.variables.front().id = large;
    const auto exact = InvokeVariableTool("variables_get", QJsonObject{{"id", QString::number(large)}}, f.host);
    Require(exact.value("ok").toBool() && exact.value("variable").toObject().value("ref").toObject().value("id") == QString::number(large), "StableId above double precision remains exact");
    const auto graph = QueryGraphDocument(f.document, f.catalog, nullptr,
        {{"table", "variables"}, {"variable_ids", QJsonArray{QString::number(second), QString::number(large)}}});
    Require(graph.value("ok").toBool() && graph.value("query_mode") == "variable_ids"
        && graph.value("rows").toArray().first().toObject().value("ref").toObject().value("id") == QString::number(second), "shared Graph exact query preserves requested order");
    f.document.variables.back().typeName.text = "string";
    f.document.variables.back().hasDefault = 1;
    f.document.variables.back().value.text = "TRIGSTR_8";
    MapData::StringTableDocument strings;
    strings.entries.push_back({8, "来自当前字符串表", 0, 0, 0});
    Services::NameResolutionContext names;
    names.triggers = &f.document; names.strings = &strings;
    const auto named = QueryGraphDocument(f.document, f.catalog, nullptr,
        {{"table", "variables"}, {"variable_ids", QJsonArray{QString::number(second)}}},
        [&](std::string_view raw, std::string_view type) { return Services::ResolveName(names, raw, type); });
    const auto namedRow = named.value("rows").toArray().first().toObject();
    Require(named.value("ok").toBool() && namedRow.value("raw_value") == "TRIGSTR_8"
        && namedRow.value("display_name") == "来自当前字符串表" && namedRow.value("name_state") == "resolved",
        "shared MCP graph preserves raw value beside current resolved WTS name");
    f.host.context = [&] {
        VariableToolContext context{&f.document, &f.catalog, {}};
        context.nameResolver = [&](std::string_view raw, std::string_view type) { return Services::ResolveName(names, raw, type); };
        return Result<VariableToolContext>::Success(std::move(context));
    };
    const auto facade = InvokeVariableTool("variables_get", {{"id", QString::number(second)}}, f.host);
    Require(facade.value("ok").toBool() && facade.value("variable").toObject().value("raw_value") == "TRIGSTR_8"
        && facade.value("variable").toObject().value("display_name") == "来自当前字符串表", "variable facade passes current host names to common graph query");
    for (const auto& value : QJsonArray{0, "0", "01", "-1", "18446744073709551616", static_cast<double>(large)})
        Require(Code(InvokeVariableTool("variables_get", {{"id", value}}, f.host)) == "VARIABLE_ID_INVALID", "invalid or numeric stable identity rejects");
    Require(!InvokeVariableTool("variables_get", {{"id", "1"}}, f.host).value("ok").toBool(), "missing ID rejects");
    Require(!QueryGraphDocument(f.document, f.catalog, nullptr, {{"table", "variables"}, {"variable_ids", QJsonArray{QString::number(large)}}, {"limit", 1}}).value("ok").toBool(), "exact IDs do not mix with paging");
    Require(!QueryGraphDocument(f.document, f.catalog, nullptr, {{"table", "variables"}, {"variable_ids", QJsonArray{QString::number(large), QString::number(large)}}}).value("ok").toBool(), "duplicate requested IDs reject");
    f.document.variables.back().id = large;
    Require(!InvokeVariableTool("variables_get", {{"id", QString::number(large)}}, f.host).value("ok").toBool(), "duplicate actual IDs reject rather than select first");
    (void)first;
}

void ValidationAndAtomicity() {
    Fixture f(true); const auto id = f.Create(); const auto original = f.Bytes(); const auto publications = f.publishes;
    auto update = f.For(id); const auto stale = update; update.insert("changes", QJsonObject{});
    Require(Code(InvokeVariableTool("variables_update", update, f.host)) == "VARIABLE_CHANGES_EMPTY", "empty update rejects");
    update = f.For(id); update.insert("changes", QJsonObject{{"is_array", true}, {"array_size", 8193}});
    Require(!InvokeVariableTool("variables_update", update, f.host).value("ok").toBool(), "late array bound failure rolls back preceding GraphSet"); SameBytes(f, original, publications);
    update = f.For(id); update.insert("changes", QJsonObject{{"initial_value", "1 + 2"}});
    Require(Code(InvokeVariableTool("variables_update", update, f.host)) == "TRIGGER_LITERAL_INVALID", "initial values cannot inject expressions"); SameBytes(f, original, publications);
    auto create = f.CreateRequest("Other", "localonly"); Require(!InvokeVariableTool("variables_create", create, f.host).value("ok").toBool(), "non-global catalog type rejects"); SameBytes(f, original, publications);
    update = f.For(id); update.insert("changes", QJsonObject{{"name", "Bypass"}});
    Require(Code(InvokeVariableTool("variables_update", update, f.host)) == "VARIABLE_ARGUMENT_UNKNOWN", "rename cannot bypass atomic name command");
    update = f.For(id); update.insert("changes", QJsonObject{{"is_array", 1}});
    Require(Code(InvokeVariableTool("variables_update", update, f.host)) == "VARIABLE_ARGUMENT_TYPE", "boolean fields do not accept integers");
    update = f.For(id); update.insert("changes", QJsonObject{{"is_array", true}, {"array_size", 8192}});
    Require(InvokeVariableTool("variables_update", update, f.host).value("ok").toBool(), "maximum legal array size accepted");
    update = f.For(id); update.insert("changes", QJsonObject{{"is_array", false}});
    Require(!InvokeVariableTool("variables_update", update, f.host).value("ok").toBool(), "scalar switch does not guess a new length");
    update = f.For(id); update.insert("changes", QJsonObject{{"is_array", false}, {"array_size", 1}, {"initialized", false}});
    const auto changed = InvokeVariableTool("variables_update", update, f.host);
    Require(changed.value("ok").toBool() && f.document.variables.front().value.text == "42" && !f.document.variables.front().hasDefault, "unprovided initial value survives flag update");
    auto old = stale; old.insert("changes", QJsonObject{{"initial_value", "73"}});
    Require(Code(InvokeVariableTool("variables_update", old, f.host)) == "VARIABLE_REVISION_CONFLICT", "old token in the same active transaction rejects");
    Require(Code(InvokeVariableTool("variables_list", {{"project_identity", "synthetic-variable-map"}}, f.host)) == "VARIABLE_REVISION_CONFLICT", "optional query identity must be a pair");
    f.active = false; const auto inactive = f.Bytes(); const auto count = f.publishes;
    Require(Code(InvokeVariableTool("variables_create", f.CreateRequest("NoTransaction"), f.host)) == "VARIABLE_ACTIVE_TRANSACTION_REQUIRED", "standalone host requires its original active transaction"); SameBytes(f, inactive, count);
}

void TypedReferencesAndOpaqueCode() {
    Fixture f; const auto id = f.Create();
    const auto added = TriggerDocumentEditor::AppendRootEca(f.document, f.catalog, f.document.triggers.front().id, UiDefinitionKind::Action, "StoreInteger");
    Require(added.Succeeded(), "actual typed reference fixture");
    const auto argument = f.document.argumentNodes.front().id;
    Require(TriggerDocumentEditor::SetScalarArgument(f.document, argument, 1, "Counter").Succeeded(), "actual declared variable reference");
    ++f.generation;
    auto rename = f.For(id); rename.insert("name", "Renamed");
    const auto renamed = InvokeVariableTool("variables_rename", rename, f.host);
    Require(renamed.value("ok").toBool() && f.document.argumentNodes.front().value.text == "Renamed"
        && renamed.value("renames").toArray().first().toObject().value("changed_argument_ids").toArray() == QJsonArray{QString::number(argument)}, "rename updates exact typed reference and reports its stable identity");
    const auto before = f.Bytes(); const auto publications = f.publishes;
    Require(!InvokeVariableTool("variables_delete", f.For(id), f.host).value("ok").toBool(), "typed-reference deletion refuses");
    auto arrayUpdate = f.For(id); arrayUpdate.insert("changes", QJsonObject{{"is_array", true}});
    Require(!InvokeVariableTool("variables_update", arrayUpdate, f.host).value("ok").toBool(), "array change cannot invalidate an existing scalar reference"); SameBytes(f, before, publications);
    auto update = f.For(id); update.insert("changes", QJsonObject{{"type_name", "string"}});
    Require(!InvokeVariableTool("variables_update", update, f.host).value("ok").toBool(), "type change invalidating original T refuses atomically"); SameBytes(f, before, publications);
    Fixture opaque; opaque.Create();
    opaque.document.wct.customCode.text = "call LegacyOpaque()\r\n"; opaque.Reparse();
    const auto opaqueId = opaque.document.variables.front().id;
    const auto count = opaque.publishes;
    const auto original = opaque.Bytes();
    rename = opaque.For(opaqueId); rename.insert("name", "Renamed");
    Require(Code(InvokeVariableTool("variables_rename", rename, opaque.host)) == "TRIGGER_RENAME_CODE_AUDIT_REQUIRED", "opaque rename preserves strict audit gate");
    Require(Code(InvokeVariableTool("variables_delete", opaque.For(opaqueId), opaque.host)) == "TRIGGER_REMOVE_CODE_AUDIT_REQUIRED", "opaque deletion requires bound code review");
    rename.insert("code_audits", QJsonArray{QJsonObject{{"approved", true}}});
    Require(!InvokeVariableTool("variables_rename", rename, opaque.host).value("ok").toBool(), "generic audit boolean cannot bypass existing DTO"); SameBytes(opaque, original, count);
    const auto codeBytes = opaque.document.wct.customCode.ToBytes(); Require(codeBytes.Succeeded(), "actual opaque code bytes");
    const auto codeHash = Sha256(*codeBytes.value); Require(codeHash.Succeeded(), "actual opaque code hash");
    QJsonObject audit{{"kind", "global_custom_code"}, {"id", "0"}, {"encoding", "utf8"},
        {"source_sha256", QString::fromStdString(*codeHash.value)}, {"ui_catalog_sha256", QString::fromStdString(opaque.catalog.Sha256())},
        {"old_name", "Counter"}, {"new_name", "Renamed"}, {"old_symbol", "udg_Counter"}, {"new_symbol", "udg_Renamed"},
        {"reason", "已人工审查原创测试代码只调用无关函数，本次声明改名保持该代码不变。"}};
    auto staleAudit = audit; staleAudit.insert("source_sha256", QString(64, '0'));
    rename.insert("code_audits", QJsonArray{staleAudit});
    Require(Code(InvokeVariableTool("variables_rename", rename, opaque.host)) == "TRIGGER_RENAME_CODE_AUDIT_STALE", "complete audit with wrong actual bytes still refuses"); SameBytes(opaque, original, count);
    rename.insert("code_audits", QJsonArray{audit});
    const auto audited = InvokeVariableTool("variables_rename", rename, opaque.host);
    Require(audited.value("ok").toBool() && audited.value("renames").toArray().first().toObject().value("code_coverage") == "caller-asserted-not-runtime-proven"
        && !audited.value("external_dependencies_verified").toBool() && opaque.Bytes().wct == original.wct,
        "bound audit uses the original decoder while keeping WCT bytes and evidence boundary");
    const auto binding = CaptureTriggerGraphBinding(opaque.document, opaque.catalog); Require(binding.Succeeded(), "capture deletion identity");
    QJsonArray children;
    for (const auto& row : opaque.document.variables) children.append(QJsonObject{{"kind", "variable"}, {"id", QString::number(row.id)}});
    QJsonObject operation{{"op", "remove"}, {"target", QJsonObject{{"kind", "variable"}, {"id", QString::number(opaqueId)}}},
        {"expected_name", "Renamed"}, {"parent", QJsonObject{{"kind", "variables"}, {"ordered_children", children}}}};
    const auto query = QueryGraphDocument(opaque.document, opaque.catalog, nullptr, QJsonObject{{"table", "remove_audit"},
        {"plan", QJsonObject{{"expected", GraphBindingJson(*binding.value)}, {"operations", QJsonArray{operation}}}}, {"operation_index", 0}});
    Require(query.value("ok").toBool() && query.value("complete").toBool() && query.value("individual_blocks").toArray().isEmpty(), "shared JSON query returns authoritative grouping");
    Require(query.value("groupable_count").toInt() > 0 && !query.contains("groupable_blocks")
        && query.value("groupable_samples").toArray().size() <= 20, "bounded summary still requires explicit confirmation");
    auto remove = opaque.For(opaqueId);
    remove.insert("code_audits", QJsonArray{QJsonObject{{"review_sha256", query.value("review_sha256")}, {"decision", "no_reference"},
        {"reason", "reviewed exact original zero-hit group"}, {"groupable_count", query.value("groupable_count")},
        {"groupable_sha256", query.value("groupable_sha256")}}});
    const auto audits = remove.value("code_audits").toArray();
    auto invalid = audits.at(0).toObject(); invalid.insert("blocks", QJsonArray{});
    Require(!DecodeGraphRemovalCodeAudits(QJsonArray{invalid}).Succeeded(), "JSON summary and list cannot coexist");
    invalid = audits.at(0).toObject(); invalid.insert("groupable_count", true);
    Require(!DecodeGraphRemovalCodeAudits(QJsonArray{invalid}).Succeeded(), "JSON group count is an integer, not a boolean");
    remove.insert("code_audits", QJsonArray{audits.at(0), audits.at(0)});
    Require(Code(InvokeVariableTool("variables_delete", remove, opaque.host)) == "TRIGGER_REMOVE_CODE_AUDIT_INVALID", "duplicate whole group rejected before publication");
    remove.insert("code_audits", audits);
    const auto deleted = InvokeVariableTool("variables_delete", remove, opaque.host);
    Require(deleted.value("ok").toBool() && opaque.document.variables.empty() && opaque.Bytes().wct == original.wct,
        "variable adapter consumes the same grouped proof, preserving original code");
}

void BeforePublicationBudgetAndHostRefusal() {
    Fixture f; const auto id = f.Create(); auto original = f.Bytes(); auto publications = f.publishes;
    f.hostChanges = {{"huge", QString(150000, '"')}};
    auto update = f.For(id); update.insert("changes", QJsonObject{{"initial_value", "73"}});
    Require(Code(InvokeVariableTool("variables_update", update, f.host)) == "VARIABLE_RESPONSE_BUDGET", "actual escaped complete host payload rejects before publication"); SameBytes(f, original, publications);
    f.hostChanges = {}; f.rejectPublish = true;
    Require(Code(InvokeVariableTool("variables_update", update, f.host)) == "HOST_PUBLICATION_DENIED", "host final publication refusal propagates"); SameBytes(f, original, publications);
    f.rejectPublish = false;
    f.document.variables.front().hasDefault = 0; f.document.variables.front().value.text.assign(300000, 'x');
    f.document.variables.front().value.originalBytes.clear(); ++f.generation; original = f.Bytes(); publications = f.publishes;
    update = f.For(id); update.insert("changes", QJsonObject{{"is_array", true}});
    Require(Code(InvokeVariableTool("variables_update", update, f.host)) == "VARIABLE_RESPONSE_BUDGET", "large historical before record refuses before constructing full response"); SameBytes(f, original, publications);
}

void DispatcherPermissionsAndSharedRegistration() {
    Fixture f; McpServer first, second;
    const auto registerHost = [&](McpServer& server) {
        for (auto definition : VariableToolDefinitions()) {
            const auto name = definition.name;
            definition.handler = [&, name](const QJsonObject& request) -> QJsonValue { return InvokeVariableTool(name, request, f.host); };
            server.RegisterTool(std::move(definition));
        }
    };
    registerHost(first); registerHost(second);
    const auto firstList = first.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    const auto secondList = second.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    Require(firstList && secondList && firstList->result == secondList->result, "two host registrations expose identical definitions");
    ToolPolicy policy; policy.allowEdit = false; first.SetToolPolicy(policy);
    const auto response = first.HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "variables_create"}, {"arguments", f.CreateRequest()}}}}).toJson(QJsonDocument::Compact));
    Require(response && response->error && f.attempts == 0 && f.publishes == 0, "read-only dispatcher rejects before invoking variable host");
}
} // namespace

int main() {
    try {
        SchemasAndSuccessfulHosts(); PaginationAndExactIds(); ValidationAndAtomicity();
        TypedReferencesAndOpaqueCode(); BeforePublicationBudgetAndHostRefusal(); DispatcherPermissionsAndSharedRegistration();
        std::cout << "MCP variable facade tests passed.\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
