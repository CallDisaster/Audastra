#include "McpObjectTools.h"
#include "McpServer.h"
#include "../services/ObjectCommandsFixture.h"

#include <QJsonDocument>
#include <QJsonArray>

#include <iostream>
#include <set>

namespace {
using namespace War3Map;
using namespace ObjectData;
using namespace Services;
using namespace Mcp;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
QString Code(const QJsonObject& value) {
    const auto rows = value.value("diagnostics").toArray();
    return rows.isEmpty() ? QString{} : rows.first().toObject().value("code").toString();
}
struct Fixture {
    Tests::ObjectCommandsFixture objects;
    McpServer server;
    std::uint64_t revision = 0;
    int commits = 0;
    bool rejectCommit = false;
    ObjectToolHost host;
    Fixture() {
        host.identity = [&] { return QJsonObject{{"project_identity", "synthetic-map"}, {"revision", QString::number(revision)}}; };
        host.context = [&](ObjectType type) { return Result<ObjectCommandContext>::Success(objects.Context(type)); };
        host.commit = [&](const ObjectCommandPlan& plan) {
            if (rejectCommit) return Status::Failure("Test", "ATOMIC_HOST_REJECTED", "保存前拒绝，原文档不变。");
            auto& document = objects.documents[static_cast<std::size_t>(plan.subject.type)];
            if (document->DocumentFingerprint() != plan.before.DocumentFingerprint())
                return Status::Failure("Test", "STALE_PLAN", "not current");
            *document = plan.after; ++revision; ++commits; return Status::Ok();
        };
        host.audit = [](const ObjectKey& key) {
            ObjectDeletionAssessment audit; audit.subject = key; audit.disposition = ObjectDeletionDisposition::BlockedReferences;
            IndexedObjectReference reference; reference.rawcode = key.rawcode; reference.targetTypes = {key.type}; reference.exactType = true;
            reference.memberName = "unit.ini"; reference.location = "[U002]/uabi"; reference.fieldId = "uabi";
            audit.objectReferences.definite.push_back(reference);
            audit.coverage.push_back({"war3map.j", ReferenceInputState::Partial, "动态引用尚未证明"});
            audit.reasons.push_back({"reference", "unit.ini", "对象被实际字段引用"});
            return audit;
        };
        for (auto definition : ObjectToolDefinitions()) {
            const auto name = definition.name;
            definition.handler = [&, name](const QJsonObject& arguments) -> QJsonValue { return InvokeObjectTool(name, arguments, host); };
            server.RegisterTool(std::move(definition));
        }
    }
    QJsonObject Request(ObjectType type, bool write = false) const {
        QJsonObject result{{"kind", QString::fromUtf8(ToString(type))}};
        if (write) { result.insert("project_identity", "synthetic-map"); result.insert("expected_revision", QString::number(revision)); }
        return result;
    }
    QJsonObject Invoke(const QString& name, const QJsonObject& arguments) {
        const QJsonObject request{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}};
        const auto response = server.HandleMessage(QJsonDocument(request).toJson(QJsonDocument::Compact));
        Require(response.has_value(), "tool has response");
        if (response->error) return {{"ok", false}, {"rpc_error", *response->error}};
        const auto content = response->result.toObject().value("content").toArray();
        Require(content.size() == 1, "tool returns one bounded payload");
        return QJsonDocument::fromJson(content.first().toObject().value("text").toString().toUtf8()).object();
    }
};
void SevenKindsAndSharedSchemas() {
    Fixture f;
    const auto definitions = ObjectToolDefinitions();
    Require(definitions.size() == 6, "all six A tools declared in one schema source");
    std::set<QString> names;
    for (const auto& tool : definitions) {
        names.insert(tool.name);
        Require(tool.inputSchema.value("additionalProperties").isBool() && !tool.inputSchema.value("additionalProperties").toBool(), "strict top-level schema");
        Require(!tool.description.isEmpty() && tool.maxResponseBytes != 0, "capability description and transport bound");
    }
    Require(names.contains("objects_set_fields") && names.contains("objects_delete") && names.contains("objects_field_options"), "A write/audit/options exposed");
    for (const auto& kind : Tests::ObjectCommandsFixture::kinds) {
        auto request = f.Request(kind.type);
        request.insert("page", QJsonObject{{"offset", 0}, {"limit", 1}});
        const auto listed = f.Invoke("objects_list", request);
        Require(listed.value("ok").toBool() && listed.value("total").toInt() == 1 && listed.value("items").toArray().size() == 1, "seven kinds bounded list");
        request = f.Request(kind.type); request.insert("id", kind.base);
        const auto fields = f.Invoke("objects_get", request);
        Require(fields.value("ok").toBool() && !fields.value("fields").toArray().isEmpty(), "seven kinds actual field metadata");
        request = f.Request(kind.type, true); request.insert("base_id", kind.base); request.insert("new_id", kind.custom); request.insert("name", "MCP 新名称");
        const auto created = f.Invoke("objects_create", request);
        Require(created.value("ok").toBool() && created.value("id").toString() == kind.custom && !created.value("saved").toBool(), "seven kinds shared command create");
    }
}
void SetOptionsRevisionAndAtomicity() {
    Fixture f;
    auto request = f.Request(ObjectType::Unit); request.insert("field", "uabi");
    const auto options = f.Invoke("objects_field_options", request);
    Require(options.value("ok").toBool() && options.value("ordered_list").toBool() && options.value("items").toArray()[0].toObject().value("value") == "A001", "reference options from actual ability context");
    request = f.Request(ObjectType::Unit); request.insert("field", "urac");
    Require(f.Invoke("objects_field_options", request).value("total").toInt() == 2, "fixed enum options");
    request = f.Request(ObjectType::Unit); request.insert("field", "uspe");
    Require(f.Invoke("objects_field_options", request).value("items").toArray()[0].toObject().value("value").isBool(), "boolean options use booleans");
    request = f.Request(ObjectType::Unit, true); request.insert("id", "u001");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "uhpm"}, {"value", 250}},
        QJsonObject{{"field", "uabi"}, {"value", QJsonArray{"A001", "A001"}}}});
    const auto oldRequest = request;
    const auto updated = f.Invoke("objects_set_fields", request);
    Require(updated.value("ok").toBool() && updated.value("revision").toString() == "1" && f.commits == 1, "batch publishes once with new revision");
    const auto snapshot = updated.value("changes").toObject().value("items").toArray().first().toObject();
    Require(snapshot.value("before").isObject() && snapshot.value("after").isObject(), "new override returns actual before/after objects");
    const auto before = snapshot.value("before").toObject().value("fields").toArray();
    Require(std::any_of(before.begin(), before.end(), [](const QJsonValue& row) { return row.toObject().value("field") == "uhpm" && row.toObject().value("value").toInt() == 100; }), "before includes inherited real value");
    Require(Code(f.Invoke("objects_set_fields", oldRequest)) == "OBJECT_REVISION_CONFLICT", "stale revision refuses");
    const auto fingerprint = f.objects.documents[0]->DocumentFingerprint();
    request = f.Request(ObjectType::Unit, true); request.insert("id", "u001");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "uhpm"}, {"value", 300}}, QJsonObject{{"field", "uabi"}, {"value", QJsonArray{"A999"}}}});
    Require(!f.Invoke("objects_set_fields", request).value("ok").toBool(), "illegal trailing reference refuses whole batch");
    Require(f.commits == 1 && f.objects.documents[0]->DocumentFingerprint() == fingerprint, "no partial mutation or revision change");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "uhpm"}, {"value", 300}, {"unexpected", true}}});
    Require(!f.Invoke("objects_set_fields", request).value("ok").toBool(), "nested extra input not silently discarded");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "uhpm"}, {"value", 300}}}); f.rejectCommit = true;
    Require(Code(f.Invoke("objects_set_fields", request)) == "ATOMIC_HOST_REJECTED", "host refusal propagated");
    Require(f.objects.documents[0]->DocumentFingerprint() == fingerprint, "host rejection preserved state");
    request = f.Request(ObjectType::Unit); request.insert("page", QJsonObject{{"limit", 129}});
    Require(!f.Invoke("objects_list", request).value("ok").toBool(), "page cap enforced beyond generic schema validation");
}
void PolicyAndDeletionCoverage() {
    Fixture f;
    auto request = f.Request(ObjectType::Unit, true); request.insert("base_id", "u001"); request.insert("new_id", "U001");
    ToolPolicy policy; policy.allowEdit = false; f.server.SetToolPolicy(policy);
    Require(!f.Invoke("objects_create", request).value("ok").toBool() && f.commits == 0, "read-only refuses at dispatcher before command");
    Require(f.Invoke("objects_list", f.Request(ObjectType::Unit)).value("ok").toBool(), "read-only query remains available");
    f.server.SetToolPolicy({}); Require(f.Invoke("objects_create", request).value("ok").toBool(), "edit permission restored");
    request = f.Request(ObjectType::Unit, true); request.insert("id", "U001");
    const auto removed = f.Invoke("objects_delete", request);
    Require(!removed.value("ok").toBool() && Code(removed) == "OBJECT_DELETE_REFERENCED", "referenced object delete refuses");
    const auto audit = removed.value("reference_audit").toObject();
    Require(audit.value("definite_references").toArray().size() == 1 && audit.value("coverage").toArray()[0].toObject().value("state") == "partial" && !audit.value("complete").toBool(), "deletion returns exact location and partial coverage");
    Require(f.objects.documents[0]->FindObject({ObjectType::Unit, "U001"}) != nullptr && f.commits == 1, "deletion does not change state");
}
void QueriedScalarAddressesRemainWritable() {
    Fixture f;
    for (const auto& kind : Tests::ObjectCommandsFixture::kinds) {
        auto request = f.Request(kind.type); request.insert("id", kind.base); request.insert("fields", QJsonArray{kind.nameField});
        const auto queried = f.Invoke("objects_get", request);
        Require(queried.value("ok").toBool() && queried.value("fields").toArray().size() == 1, "scalar read returned exact field");
        const auto field = queried.value("fields").toArray().first().toObject();
        Require(!field.contains("level") && !field.contains("variation"), "scalar address omits implicit native zero indices");
        QJsonObject change{{"field", field.value("field")}, {"value", "按查询地址修改"}};
        request = f.Request(kind.type, true); request.insert("id", kind.base); request.insert("changes", QJsonArray{change});
        const auto edited = f.Invoke("objects_set_fields", request);
        Require(edited.value("ok").toBool(), "scalar field can be written using queried address");
        const auto after = edited.value("changes").toObject().value("items").toArray().first().toObject().value("after").toObject();
        for (const auto& value : after.value("fields").toArray()) {
            const auto row = value.toObject();
            if (row.value("field") == kind.nameField) Require(!row.contains("level") && !row.contains("variation"), "change snapshots omit scalar zero indices too");
        }
    }
    auto request = f.Request(ObjectType::Ability); request.insert("id", "A001"); request.insert("fields", QJsonArray{"adat"});
    const auto levels = f.Invoke("objects_get", request).value("fields").toArray();
    Require(levels.size() == 2 && levels[0].toObject().value("level").toInt() == 1 && levels[1].toObject().value("level").toInt() == 2, "repeated field exposes positive exact levels");
    request = f.Request(ObjectType::Ability, true); request.insert("id", "A001");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "adat"}, {"level", 0}, {"value", 50}}});
    Require(Code(f.Invoke("objects_set_fields", request)) == "OBJECT_FIELD_ADDRESS_INVALID", "repeated zero level remains rejected");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "adat"}, {"level", 2}, {"value", 50}}});
    const auto edited = f.Invoke("objects_set_fields", request);
    Require(edited.value("ok").toBool(), "positive repeated level still writable");
    const auto change = edited.value("changes").toObject().value("items").toArray().first().toObject();
    Require(change.value("level").toInt() == 2 && change.value("before").toInt() == 20 && change.value("after").toInt() == 50, "repeated changes expose actual inherited before value and positive level");
}
}
int main() {
    try { SevenKindsAndSharedSchemas(); SetOptionsRevisionAndAtomicity(); PolicyAndDeletionCoverage(); QueriedScalarAddressesRemainWritable();
        std::cout << "McpObjectTools tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "McpObjectTools test failure: " << error.what() << '\n'; return 1; }
}
