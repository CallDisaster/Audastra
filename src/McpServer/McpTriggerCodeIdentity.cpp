#include "McpTriggerCodeIdentity.h"
#include "McpTriggerGraphJson.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace War3Map::Mcp {
bool HasCodeIdentityOperation(const QJsonObject& request) {
    for (const auto& value : request.value("operations").toArray()) {
        const auto op = value.toObject().value("op").toString();
        if (op == "rename_custom_trigger" || op == "copy_custom_trigger") return true;
    }
    return false;
}
QJsonObject CodeIdentityGraphSchema(QJsonObject schema) {
    auto properties = schema.value("properties").toObject(); auto operations = properties.value("operations").toObject();
    auto items = operations.value("items").toObject(); auto variants = items.value("oneOf").toArray();
    const QJsonObject string{{"type", "string"}}, id{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}};
    const QJsonObject raw{{"type", "object"}, {"additionalProperties", false}, {"required", QJsonArray{"text", "encoding", "raw_bytes_hex"}},
        {"properties", QJsonObject{{"text", string}, {"encoding", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"utf8", "utf8_bom", "gb18030", "latin1"}}}}, {"raw_bytes_hex", string}}}};
    for (const auto& op : {QStringLiteral("rename_custom_trigger"), QStringLiteral("copy_custom_trigger")}) {
        QJsonObject fields{{"op", QJsonObject{{"const", op}, {"type", "string"}}}, {"trigger_id", id}, {"expected_name", raw},
            {"expected_code_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}}, {"name", string}};
        QJsonArray required{"op", "trigger_id", "expected_name", "expected_code_sha256", "name"};
        if (op == "copy_custom_trigger") {
            fields.insert("category", QJsonObject{{"type", "object"}, {"additionalProperties", false}, {"required", QJsonArray{"kind", "id"}},
                {"properties", QJsonObject{{"kind", QJsonObject{{"const", "category"}, {"type", "string"}}}, {"id", id}}}});
            fields.insert("after_trigger_id", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}});
            fields.insert("expected_category_order", QJsonObject{{"type", "array"}, {"maxItems", 4096}, {"items", id}});
            required.append("category"); required.append("after_trigger_id"); required.append("expected_category_order");
        }
        variants.append(QJsonObject{{"type", "object"}, {"properties", fields}, {"required", required}, {"additionalProperties", false}});
    }
    items.insert("oneOf", variants); operations.insert("items", items); properties.insert("operations", operations);
    properties.insert("dry_run", QJsonObject{{"type", "boolean"}, {"description", "代码身份操作必须显式提供；true仅准备，不改变事务或撤销栈。"}});
    schema.insert("properties", properties); return schema;
}
QJsonObject CodeIdentitySummaryJson(const Services::TriggerCodeIdentitySummary& summary) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(Services::TriggerCodeIdentitySummaryJson(summary))).object();
}
Result<Trigger::TriggerGraphEditBatch> DecodeCodeIdentityGraphBatch(const QJsonObject& request,
    const Services::TriggerCodeIdentityContext& context, bool dryRun, Services::TriggerCodeIdentitySummary* summary,
    std::optional<Services::PreparedTriggerCodeIdentityEdit>* prepared) {
    using R = Result<Trigger::TriggerGraphEditBatch>;
    const auto operations = request.value("operations").toArray();
    if (operations.isEmpty()) return DecodeGraphBatch(request);
    const auto first = operations.first().toObject().value("op").toString();
    if (first != "rename_custom_trigger" && first != "copy_custom_trigger")
        return R::Failure("MCP", "TRIGGER_CODE_REQUEST_INVALID", "代码身份操作必须位于批次第一项，每批最多一项；其它操作可作为同一原子批次后缀。");
    QJsonArray tail;
    for (qsizetype i = 1; i < operations.size(); ++i) {
        const auto op = operations[i].toObject().value("op").toString();
        if (op == "rename_custom_trigger" || op == "copy_custom_trigger")
            return R::Failure("MCP", "TRIGGER_CODE_REQUEST_INVALID", "每批最多一个代码身份操作，请查询新身份后继续。");
        tail.append(operations[i]);
    }
    auto envelope = request; envelope.insert("operations", QJsonArray{operations.first()}); envelope.insert("dry_run", dryRun);
    auto parsed = Services::ParseTriggerCodeIdentityEdit(QJsonDocument(envelope).toJson(QJsonDocument::Compact).toStdString());
    if (!parsed.Succeeded()) return {{}, parsed.diagnostics};
    auto plan = Services::TriggerCodeIdentityEditing::Prepare(context, parsed.value->request);
    if (!plan.Succeeded()) return {{}, plan.diagnostics};
    Trigger::TriggerGraphEditBatch batch{parsed.value->request.expected, {plan.value->Operation()}};
    if (!tail.isEmpty()) {
        auto suffix = request; suffix.insert("operations", tail);
        auto decoded = DecodeGraphBatch(suffix); if (!decoded.Succeeded()) return decoded;
        batch.operations.insert(batch.operations.end(), decoded.value->operations.begin(), decoded.value->operations.end());
    }
    if (summary) *summary = plan.value->Summary();
    if (prepared) *prepared = *plan.value;
    return R::Success(std::move(batch));
}
}
