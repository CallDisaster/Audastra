#include "McpImportTools.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Mcp;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QString Code(const QJsonObject& response) {
    const auto values = response.value("diagnostics").toArray();
    return values.isEmpty() ? QString{} : values.first().toObject().value("code").toString();
}
std::vector<std::byte> Bytes(std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data()); return {first, first + value.size()};
}
std::vector<std::byte> Imp(const std::vector<Resources::ImportRecord>& records) {
    std::vector<std::byte> result;
    const auto u32 = [&](std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) result.push_back(static_cast<std::byte>((value >> shift) & 255U)); };
    u32(1); u32(static_cast<std::uint32_t>(records.size()));
    for (const auto& entry : records) {
        result.push_back(static_cast<std::byte>(entry.flags));
        const auto path = Bytes(entry.path); result.insert(result.end(), path.begin(), path.end()); result.push_back(std::byte{});
    }
    return result;
}
struct Fixture {
    QTemporaryDir temporary;
    std::optional<DirectoryWorkspace> directory;
    ImportInventoryBinding binding;
    ImportInventoryOverlay overlay;
    ImportToolHost host;
    int captures = 0;
    std::vector<std::byte> imp;
    explicit Fixture(std::vector<Resources::ImportRecord> records = {{13, "asset.bin"}}) {
        Require(temporary.isValid(), "private metadata fixture directory");
        imp = Imp(records);
        Require(IO::WriteFileBytes(Root() / "war3map.imp", imp).Succeeded(), "write original IMP fixture");
        Require(IO::WriteFileBytes(Root() / "asset.bin", { }).Succeeded(), "write real empty imported member");
        const auto opened = DirectoryWorkspace::Open(Root()); Require(opened.Succeeded(), "capture original directory metadata");
        directory = *opened.value;
        binding = {"mcp-import-fixture", 2, "revision:1", directory->Snapshot().aggregateSha256, "asset:3", "gui-live-assets"};
        host.identity = [&] { return QJsonObject{{"project_identity", QString::fromStdString(binding.documentId)}, {"revision", QString::fromStdString(binding.revision)}}; };
        host.context = [&] { ++captures; return CaptureImportInventory(*directory, binding, overlay); };
    }
    std::filesystem::path Root() const { return std::filesystem::path(temporary.path().toStdWString()); }
    QJsonObject Call(const QString& name, const QJsonObject& arguments = {}) { return InvokeImportTool(name, arguments, host); }
    void Unchanged() const {
        Require(IO::ReadFileBytes(Root() / "war3map.imp").value == std::optional(imp), "IMP bytes unchanged by tool");
        Require(directory->VerifyUnchanged().Succeeded(), "all original directory bytes unchanged by tools");
    }
};

void SchemaAndMetadataContract() {
    const auto definitions = ImportToolDefinitions();
    Require(definitions.size() == 2 && definitions[0].name == "imports_list" && definitions[1].name == "imports_get", "only two metadata tools registered");
    for (const auto& definition : definitions) Require(definition.permission == ToolPermission::Query
        && definition.maxResponseBytes == 256U * 1024U && definition.maxRequestIdBytes == 1024, "query permission and real transport budgets");
    Fixture fixture;
    fixture.overlay = {true, {{"asset.bin", "renamed.bin"}}, {{"queued.bin", 400}}};
    const auto listed = fixture.Call("imports_list");
    Require(listed.value("ok").toBool() && listed.value("read_only").toBool() && listed.value("payloads_opaque").toBool()
        && !listed.value("text_available").toBool(), "metadata response with explicit opaque boundary");
    Require(listed.value("binding").toObject().value("source_sha256").toString() == QString::fromStdString(fixture.binding.sourceSha256)
        && listed.value("binding").toObject().value("asset_revision") == "asset:3", "complete source and overlay identity exported");
    QJsonObject target, pending;
    for (const auto value : listed.value("rows").toArray()) {
        const auto row = value.toObject();
        if (row.value("kind") == "imp-record") target = row;
        if (row.value("kind") == "queued-unfrozen") pending = row;
        Require(!row.contains("text") && !row.contains("data") && !row.contains("base64") && row.value("payload_opaque").toBool(), "no payload contents in any row");
    }
    Require(target.value("flags").toInt() == 13 && target.value("source_path") == "asset.bin"
        && target.value("archive_path") == "renamed.bin" && target.value("overlay_state") == "moved"
        && target.value("bytes") == "0" && target.value("sha256").isString(), "renamed metadata still proves original empty bytes");
    Require(pending.value("reported_bytes") == "400" && pending.value("bytes").isNull() && pending.value("sha256").isNull(), "queued reported size is not actual source identity");
    const QJsonObject request{{"project_identity", "mcp-import-fixture"}, {"expected_revision", "revision:1"},
        {"expected_inventory_sha256", listed.value("inventory_sha256")}, {"import_id", target.value("import_id")}};
    const auto detail = fixture.Call("imports_get", request);
    Require(detail.value("ok").toBool() && detail.value("row").toObject() == target, "get reuses exact same metadata service row");
    auto stale = request; stale["expected_inventory_sha256"] = QString(64, 'f');
    Require(Code(fixture.Call("imports_get", stale)) == "IMPORT_REVISION_CONFLICT", "get snapshot hash guard");
    stale = request; stale["expected_revision"] = "revision:0";
    Require(Code(fixture.Call("imports_get", stale)) == "IMPORT_REVISION_CONFLICT", "get revision guard");
    fixture.Unchanged();
}

void JsonValidationAndHostDrift() {
    Fixture fixture;
    const auto initialCaptures = fixture.captures;
    for (const auto* name : {"imports_add", "imports_remove", "imports_read_text"})
        Require(Code(fixture.Call(name)) == "IMPORT_TOOL_UNSUPPORTED", "write or text tool cannot reach source");
    Require(fixture.captures == initialCaptures, "unsupported tool never captures source");
    Require(Code(fixture.Call("imports_list", {{"file_path", "private.txt"}})) == "IMPORT_ARGUMENT_UNKNOWN", "unknown file argument rejected");
    Require(Code(fixture.Call("imports_list", {{"project_identity", "mcp-import-fixture"}})) == "IMPORT_IDENTITY_REQUIRED", "identity pair enforced");
    Require(Code(fixture.Call("imports_list", {{"query", false}})) == "IMPORT_ARGUMENT_TYPE", "query type not coerced");
    for (const auto value : QJsonArray{true, "2", 1.5, 0, 129, QJsonValue(QJsonValue::Null)})
        Require(Code(fixture.Call("imports_list", {{"page", QJsonObject{{"limit", value}}}})) == "IMPORT_QUERY_BUDGET", "strict page integer and bounds");
    Require(Code(fixture.Call("imports_list", {{"page", QJsonObject{{"start_index", "0"}}}})) == "IMPORT_ARGUMENT_UNKNOWN", "no unbound row offset");
    Require(Code(fixture.Call("imports_list", {{"page", QJsonObject{{"cursor", false}}}})) == "IMPORT_CURSOR_INVALID", "cursor type preserved");
    Require(Code(fixture.Call("imports_get", {{"import_id", "0"}})) == "IMPORT_ARGUMENT_REQUIRED", "get requires inventory identity");
    Require(Code(fixture.Call("imports_list", {{"query", QString(70000, 'x')}})) == "IMPORT_REQUEST_BUDGET", "whole request budget preflight");
    int reads = 0;
    fixture.host.identity = [&] {
        return QJsonObject{{"project_identity", "mcp-import-fixture"}, {"revision", ++reads == 1 ? "revision:1" : "revision:2"}};
    };
    Require(Code(fixture.Call("imports_list")) == "IMPORT_REVISION_CONFLICT", "post-query host revision drift refused");
    fixture.Unchanged();
}

void PaginationEncodingAndBudget() {
    std::vector<Resources::ImportRecord> records;
    for (unsigned index = 0; index < 129; ++index) records.push_back({13, "Item" + std::to_string(index)});
    Fixture fixture(records);
    QJsonObject request{{"query", "Item"}, {"page", QJsonObject{{"limit", 64}}}};
    std::set<QString> ids;
    QJsonObject firstCursor;
    do {
        const auto page = fixture.Call("imports_list", request);
        Require(page.value("ok").toBool() && page.value("matched_total") == "129", "truthful total for bounded full scan");
        for (const auto value : page.value("rows").toArray()) Require(ids.insert(value.toObject().value("import_id").toString()).second, "metadata pagination has no duplicate");
        if (page.value("next_cursor").isNull()) break;
        if (firstCursor.isEmpty()) firstCursor = page.value("next_cursor").toObject();
        request["page"] = QJsonObject{{"limit", 64}, {"cursor", page.value("next_cursor")}};
    } while (true);
    Require(ids.size() == 129, "129 metadata records without loss");
    Require(Code(fixture.Call("imports_list", {{"query", "item"}, {"page", QJsonObject{{"cursor", firstCursor}}}})) == "IMPORT_CURSOR_STALE", "cursor filter binding");
    auto changed = firstCursor; auto binding = changed.value("binding").toObject(); binding["document_generation"] = "3"; changed["binding"] = binding;
    Require(Code(fixture.Call("imports_list", {{"query", "Item"}, {"page", QJsonObject{{"cursor", changed}}}})) == "IMPORT_CURSOR_STALE", "cursor generation binding");
    changed = firstCursor; changed["next_index"] = "01";
    Require(Code(fixture.Call("imports_list", {{"query", "Item"}, {"page", QJsonObject{{"cursor", changed}}}})) == "IMPORT_CURSOR_INVALID", "cursor canonical decimal guard");
    fixture.Unchanged();
    Fixture encoded({{13, std::string("bad-\x81", 5)}});
    const auto raw = encoded.Call("imports_list", {{"query", "bad-"}}).value("rows").toArray().first().toObject();
    Require(raw.value("raw_path").isNull() && raw.value("raw_path_hex") == "6261642d81" && raw.value("state") == "unresolved", "nonUTF IMP path returns exact hex no lossy display");
    records.clear();
    for (unsigned index = 0; index < 128; ++index) records.push_back({13, std::string(1200, '"') + std::to_string(index)});
    Fixture large(records);
    const auto one = large.Call("imports_list", {{"page", QJsonObject{{"limit", 1}}}});
    Require(one.value("ok").toBool(), "single large but bounded metadata row valid");
    Require(Code(large.Call("imports_list", {{"page", QJsonObject{{"limit", 128}}}})) == "IMPORT_RESPONSE_BUDGET", "actual escaped full JSON budget enforced");
    large.Unchanged();
}

void RealDispatcherReadOnlyPolicy() {
    Fixture fixture;
    McpServer server;
    for (auto definition : ImportToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&, name](const QJsonObject& arguments) { return fixture.Call(name, arguments); };
        server.RegisterTool(std::move(definition));
    }
    ToolPolicy policy; policy.allowEdit = false; policy.allowSave = false; policy.allowExternal = false;
    server.SetToolPolicy(policy);
    const auto call = [&] {
        return server.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"imports_list","arguments":{}}})");
    };
    auto response = call();
    Require(response && !response->error && !response->result.toObject().value("isError").toBool(), "Query tools usable in actual readonly policy");
    const auto body = QJsonDocument::fromJson(response->result.toObject().value("content").toArray().first().toObject().value("text").toString().toUtf8()).object();
    Require(body.value("ok").toBool() && body.value("payloads_opaque").toBool(), "real dispatcher returns original metadata response");
    const auto captures = fixture.captures;
    policy.allowQuery = false; server.SetToolPolicy(policy); response = call();
    Require(response && response->error && fixture.captures == captures, "query denied by original dispatcher before host invocation");
    fixture.Unchanged();
}
} // namespace

int main() {
    try {
        SchemaAndMetadataContract(); JsonValidationAndHostDrift(); PaginationEncodingAndBudget(); RealDispatcherReadOnlyPolicy();
        std::cout << "PASS MCP import metadata: shared schema, strict JSON, cursor/encoding/response budget, original query policy\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
