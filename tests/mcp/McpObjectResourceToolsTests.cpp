#include "McpObjectResourceTools.h"
#include <QJsonArray>
#include <iostream>
#include <stdexcept>
namespace {
using namespace War3Map;
using namespace War3Map::Mcp;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QString Code(const QJsonObject& body) { const auto notes = body.value("diagnostics").toArray(); return notes.isEmpty() ? QString{} : notes.first().toObject().value("code").toString(); }
void Contract() {
    const auto definitions = ObjectResourceToolDefinitions(); Require(definitions.size() == 3, "three shared tools");
    for (const auto& tool : definitions) {
        Require(tool.permission == ToolPermission::Query && tool.maxResponseBytes == 256 * 1024, "read-only bounded surface");
        Require(!tool.inputSchema.value("additionalProperties").toBool(true), "strict schema");
    }
    std::string revision = "resources:1";
    auto catalog = std::make_shared<ObjectResourceCatalog>(); catalog->revision = revision; catalog->identity = "catalog:1";
    for (int i = 0; i < 5; ++i) {
        ObjectResourceCandidate row; row.id = std::to_string(i); row.title = "Unit" + row.id;
        row.value = "model" + row.id + ".mdx"; row.kind = ObjectResourceKind::Model; catalog->rows.push_back(row);
    }
    int loads = 0, reads = 0; bool drift = false;
    ObjectResourceToolHost host;
    host.identity = [&] { return QJsonObject{{"project_identity", "project:1"}, {"revision", "17"}, {"resource_revision", QString::fromStdString(revision)}}; };
    host.load = [&](auto) { ++loads; if (drift) revision = "resources:2"; return Result<std::shared_ptr<const ObjectResourceCatalog>>::Success(catalog); };
    host.field = [](auto, auto) { return Result<ObjectResourceField>::Success({ObjectResourceKind::Model, true, true}); };
    host.inspect = [&](auto value, auto kind) {
        ++reads; ObjectResourceInfo info; info.requestedValue = std::string(value); info.kind = kind;
        info.source.sourceId = "map"; info.bytes = 37; info.sequences.push_back({"Stand", 0, 1000, true, true});
        return Result<ObjectResourceInfo>::Success(info);
    };
    auto state = InvokeObjectResourceTool("object_resource_state", {}, host);
    Require(state.value("ok").toBool() && loads == 0 && reads == 0, "state is cold and read-only");
    QJsonObject arguments{{"project_identity", "project:1"}, {"expected_revision", "17"}, {"resource_revision", "resources:1"},
        {"origin", "object"}, {"object_type", "unit"}, {"field_id", "umdl"}, {"limit", 2}};
    auto first = InvokeObjectResourceTool("object_resource_query", arguments, host);
    Require(first.value("ok").toBool() && first.value("rows").toArray().size() == 2 && first.value("ordered_list").toBool(), "field-derived kind and first page");
    Require(!first.value("rows").toArray().first().toObject().value("source_verified").toBool(), "listing is not actual source proof");
    arguments.insert("cursor", first.value("cursor"));
    auto second = InvokeObjectResourceTool("object_resource_query", arguments, host);
    Require(second.value("ok").toBool() && second.value("rows").toArray().first().toObject().value("id") == "2", "next page follows sealed query");
    arguments.insert("search", "changed");
    Require(!InvokeObjectResourceTool("object_resource_query", arguments, host).value("ok").toBool(), "cursor cannot cross query");
    arguments.remove("cursor"); arguments.remove("search"); arguments.insert("kind", "icon");
    Require(Code(InvokeObjectResourceTool("object_resource_query", arguments, host)) == "OBJECT_RESOURCE_ARGUMENT_INVALID", "field kind cannot be forged");
    arguments.remove("kind"); arguments.insert("limit", 129); const auto before = loads;
    Require(!InvokeObjectResourceTool("object_resource_query", arguments, host).value("ok").toBool() && loads == before, "invalid budget rejected before loading");
    arguments.insert("limit", 2); arguments.insert("resource_revision", "old");
    Require(Code(InvokeObjectResourceTool("object_resource_query", arguments, host)) == "OBJECT_RESOURCE_STALE" && loads == before, "old identity rejected before loading");
    arguments.insert("resource_revision", "resources:1"); drift = true;
    Require(Code(InvokeObjectResourceTool("object_resource_query", arguments, host)) == "OBJECT_RESOURCE_HOST_CHANGED", "drift discards page");
    QJsonObject inspect{{"project_identity", "project:1"}, {"expected_revision", "17"}, {"resource_revision", "resources:2"}, {"kind", "model"}, {"value", "model.mdx"}};
    auto info = InvokeObjectResourceTool("object_resource_inspect", inspect, host);
    Require(info.value("ok").toBool() && reads == 1 && info.value("sequences").toArray().size() == 1 &&
        info.value("source_verified").toBool() && !info.value("payload_included").toBool(), "inspection returns metadata only");
    inspect.insert("path", "unauthorized");
    Require(Code(InvokeObjectResourceTool("object_resource_inspect", inspect, host)) == "OBJECT_RESOURCE_ARGUMENT_INVALID" && reads == 1, "no host paths or unknown arguments");
}
}
int main() {
    try { Contract(); std::cout << "Object resource tools passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
