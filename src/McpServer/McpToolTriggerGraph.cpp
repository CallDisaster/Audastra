#include "McpToolInternal.h"
#include "McpTriggerGraphJson.h"
#include "McpTriggerCodeIdentity.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include "War3MapCore/Trigger/LegacyTriggerReferences.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerFeatureText.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace War3Map::Mcp {
namespace {
using namespace Trigger;
constexpr std::size_t kResponseBytes = 1024U * 1024U;
constexpr std::size_t kRequestBytes = 4U * 1024U * 1024U;
constexpr std::size_t kRecords = 16384;

class GraphJsonError final : public std::runtime_error {
public:
    explicit GraphJsonError(std::string message, const char* diagnostic = "MCP_TRIGGER_GRAPH_REQUEST_INVALID")
        : std::runtime_error(std::move(message)), code(diagnostic) {}
    const char* code;
};

QJsonObject Object(const QJsonValue& value, std::initializer_list<const char*> required,
    std::initializer_list<const char*> optional = {}) {
    if (!value.isObject()) throw GraphJsonError("图请求中的记录必须是对象。");
    const auto object = value.toObject();
    for (const auto* key : required)
        if (!object.contains(QLatin1String(key))) throw GraphJsonError(std::string("缺少图字段：") + key);
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto matches = [&it](const char* key) { return it.key() == QLatin1String(key); };
        if (std::none_of(required.begin(), required.end(), matches) && std::none_of(optional.begin(), optional.end(), matches))
            throw GraphJsonError("图记录含未知字段：" + it.key().toStdString());
    }
    return object;
}

std::string String(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || value.toString().contains(QChar::Null))
        throw GraphJsonError(std::string(key) + " 必须是无 NUL 的字符串。");
    return value.toString().toStdString();
}
bool Boolean(const QJsonObject& object, const char* key) {
    if (!object.value(QLatin1String(key)).isBool()) throw GraphJsonError(std::string(key) + " 必须是布尔值。");
    return object.value(QLatin1String(key)).toBool();
}
std::int32_t Integer(const QJsonObject& object, const char* key) {
    const auto parsed = ParseInt32(object, key);
    if (!parsed.Succeeded()) throw GraphJsonError(std::string(key) + " 必须是 int32 整数。");
    return *parsed.value;
}
QJsonArray Array(const QJsonObject& object, const char* key) {
    if (!object.value(QLatin1String(key)).isArray()) throw GraphJsonError(std::string(key) + " 必须是数组。");
    return object.value(QLatin1String(key)).toArray();
}
std::string Sha(const QJsonObject& object, const char* key) {
    auto value = String(object, key);
    if (!IsLowerHexSha256(value)) throw GraphJsonError(std::string(key) + " 必须是完整小写 SHA-256。");
    return value;
}
StableId Id(const QJsonObject& object, const char* key, bool allowZero = false) {
    const auto value = ParseUnsignedId(object, key, allowZero);
    if (!value.Succeeded() || std::to_string(*value.value) != String(object, key))
        throw GraphJsonError(std::string(key) + " 必须是规范十进制 ID 字符串。");
    return *value.value;
}
TextEncoding Encoding(std::string_view text) {
    if (text == "utf8") return TextEncoding::Utf8;
    if (text == "utf8_bom") return TextEncoding::Utf8Bom;
    if (text == "gb18030") return TextEncoding::Gb18030;
    if (text == "latin1") return TextEncoding::Latin1;
    throw GraphJsonError("未知文本编码；只接受 utf8/utf8_bom/gb18030/latin1。");
}
const char* EncodingName(TextEncoding value) {
    switch (value) {
    case TextEncoding::Utf8: return "utf8";
    case TextEncoding::Utf8Bom: return "utf8_bom";
    case TextEncoding::Gb18030: return "gb18030";
    case TextEncoding::Latin1: return "latin1";
    }
    throw GraphJsonError("文档含未知文本编码。");
}
const char* RecordKind(GraphRecordKind kind) {
    switch (kind) {
    case GraphRecordKind::Category: return "category";
    case GraphRecordKind::Variable: return "variable";
    case GraphRecordKind::Trigger: return "trigger";
    case GraphRecordKind::Eca: return "eca";
    case GraphRecordKind::Argument: return "argument";
    }
    throw GraphJsonError("未知图记录种类。");
}
QJsonObject RefJson(GraphRecordKind kind, StableId id) {
    return {{"kind", RecordKind(kind)}, {"id", QString::number(id)}};
}
QJsonObject BindingJson(const TriggerGraphBinding& binding) {
    QJsonObject result{{"source_wtg_sha256", QString::fromStdString(binding.sourceWtgSha256)},
        {"source_wct_sha256", QString::fromStdString(binding.sourceWctSha256)},
        {"ui_catalog_sha256", QString::fromStdString(binding.uiCatalogSha256)},
        {"current_wtg_sha256", QString::fromStdString(binding.currentWtgSha256)},
        {"current_wct_sha256", QString::fromStdString(binding.currentWctSha256)}};
    if (!binding.mapObjectReferencesSha256.empty()) result["map_object_references_sha256"] = QString::fromStdString(binding.mapObjectReferencesSha256);
    return result;
}
QJsonObject ArgumentGuardJson(const GraphArgumentReplacementGuard& guard) {
    return {{"schema_version", static_cast<int>(guard.schemaVersion)},
        {"owner_eca_id", QString::number(guard.ownerEcaId)}, {"source_trigger_id", QString::number(guard.sourceTriggerId)},
        {"argument_ordinal", static_cast<qint64>(guard.argumentOrdinal)},
        {"ui_signature", QString::fromStdString(guard.uiSignature)}, {"expected_type", QString::fromStdString(guard.expectedType)},
        {"subtree_sha256", QString::fromStdString(guard.subtreeSha256)}};
}
GraphArgumentReplacementGuard ReadArgumentGuard(const QJsonValue& value) {
    const auto object = Object(value, {"schema_version", "owner_eca_id", "source_trigger_id", "argument_ordinal",
        "ui_signature", "expected_type", "subtree_sha256"});
    const auto version = Integer(object, "schema_version"), ordinal = Integer(object, "argument_ordinal");
    if (version != 1 || ordinal < 0) throw GraphJsonError("参数替换要求 schema_version=1，argument_ordinal>=0。");
    return {1, Id(object, "owner_eca_id"), Id(object, "source_trigger_id"), static_cast<std::uint32_t>(ordinal),
        String(object, "ui_signature"), String(object, "expected_type"), Sha(object, "subtree_sha256")};
}
QString Hex(std::span<const std::byte> bytes) {
    if (bytes.size() > kResponseBytes / 2) throw GraphJsonError("单个图字段超出回执预算。", "MCP_TRIGGER_GRAPH_RESPONSE_BUDGET");
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(bytes.data()),
        static_cast<qsizetype>(bytes.size())).toHex());
}
QJsonObject TextJson(const RawText& value) {
    if (value.text.size() > kResponseBytes) throw GraphJsonError("单个图文字超出回执预算。", "MCP_TRIGGER_GRAPH_RESPONSE_BUDGET");
    return {{"text", QString::fromStdString(value.text)}, {"encoding", EncodingName(value.encoding)},
        {"raw_bytes_hex", Hex(value.originalBytes)}};
}
QJsonObject OriginJson(const OriginSpan& value) {
    return {{"source", QString::fromStdWString(value.source.wstring())},
        {"offset", QString::number(value.offset)}, {"length", QString::number(value.length)},
        {"fingerprint", QString::fromStdString(value.fingerprint)}, {"member", "war3map.wtg"},
        {"state", value.length ? "source-span" : "unassigned"}};
}

class GraphReader {
public:
    std::string ClientKey(const QJsonObject& object, bool required) {
        if (!object.contains("client_key") && !required) return {};
        auto key = String(object, "client_key");
        if (key.empty() || key.size() > 4096) throw GraphJsonError("client_key 必须非空且不超过 4096 字节。");
        // 预留成功回执的完整创建映射，避免业务已发布后才因 stdio 字节预算失败。
        const auto bytes = static_cast<std::size_t>(QJsonDocument(QJsonObject{
            {QString::fromStdString(key), QString(20, '9')}}).toJson(QJsonDocument::Compact).size()) + 128;
        if (bytes > kResponseBytes / 4 - createdReplyBytes_)
            throw GraphJsonError("创建键映射超出成功回执预算。", "MCP_TRIGGER_GRAPH_REQUEST_BUDGET");
        createdReplyBytes_ += bytes;
        return key;
    }
    void Count(std::size_t depth = 0) {
        if (depth > 128 || ++records_ > kRecords)
            throw GraphJsonError("图请求超过 16384 条记录或 128 层预算。", "MCP_TRIGGER_GRAPH_REQUEST_BUDGET");
    }
    GraphRecordRef Ref(const QJsonValue& value) {
        Count();
        const auto object = Object(value, {"kind"}, {"id", "client_key"});
        GraphRecordRef ref;
        const auto kind = String(object, "kind");
        if (kind == "category") ref.kind = GraphRecordKind::Category;
        else if (kind == "variable") ref.kind = GraphRecordKind::Variable;
        else if (kind == "trigger") ref.kind = GraphRecordKind::Trigger;
        else if (kind == "eca") ref.kind = GraphRecordKind::Eca;
        else if (kind == "argument") ref.kind = GraphRecordKind::Argument;
        else throw GraphJsonError("未知记录 kind。");
        if (object.contains("id") == object.contains("client_key")) throw GraphJsonError("引用必须恰有 id 或 client_key。");
        if (object.contains("id")) {
            ref.id = Id(object, "id");
        } else {
            ref.clientKey = String(object, "client_key");
            if (ref.clientKey.empty() || ref.clientKey.size() > 4096) throw GraphJsonError("client_key 必须非空且不超过 4096 字节。");
        }
        return ref;
    }
    GraphParentExpectation Parent(const QJsonValue& value) {
        const auto object = Object(value, {"kind", "ordered_children"}, {"owner", "child_branch"});
        GraphParentExpectation parent;
        const auto kind = String(object, "kind");
        if (kind == "categories") parent.kind = GraphParentKind::Categories;
        else if (kind == "variables") parent.kind = GraphParentKind::Variables;
        else if (kind == "category_triggers") parent.kind = GraphParentKind::CategoryTriggers;
        else if (kind == "trigger_ecas") parent.kind = GraphParentKind::TriggerEcas;
        else if (kind == "eca_children") parent.kind = GraphParentKind::EcaChildren;
        else throw GraphJsonError("未知父容器 kind。");
        const bool top = parent.kind == GraphParentKind::Categories || parent.kind == GraphParentKind::Variables;
        if (top == object.contains("owner")) throw GraphJsonError("只有非顶层父容器必须提供 owner。");
        if (!top) parent.owner = Ref(object.value("owner"));
        if (object.contains("child_branch")) parent.childBranch = Integer(object, "child_branch");
        if (parent.childBranch && parent.kind != GraphParentKind::EcaChildren) throw GraphJsonError("child_branch 只用于 ECA 子分支。");
        for (const auto& child : Array(object, "ordered_children")) parent.orderedChildren.push_back(Ref(child));
        return parent;
    }
    GraphScalarValue Scalar(const QJsonValue& value) {
        const auto object = Object(value, {"kind", "value"});
        GraphScalarValue scalar{Integer(object, "kind"), String(object, "value")};
        if (scalar.kind != 0 && scalar.kind != 1 && scalar.kind != 3) throw GraphJsonError("scalar kind 只能为 0/1/3。");
        return scalar;
    }
    RawText Raw(const QJsonValue& value) {
        const auto object = Object(value, {"text", "encoding", "raw_bytes_hex"});
        RawText raw; raw.text = String(object, "text"); raw.encoding = Encoding(String(object, "encoding"));
        const auto hex = String(object, "raw_bytes_hex");
        if (hex.size() % 2) throw GraphJsonError("raw_bytes_hex 必须包含完整字节。");
        const auto nibble = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
            throw GraphJsonError("raw_bytes_hex 只接受小写十六进制。");
        };
        raw.originalBytes.reserve(hex.size() / 2);
        for (std::size_t index = 0; index < hex.size(); index += 2)
            raw.originalBytes.push_back(static_cast<std::byte>((nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
        return raw;
    }
    GraphArgumentValue Argument(const QJsonValue& value, std::size_t depth) {
        Count(depth);
        const auto object = Object(value, {}, {"scalar", "client_key", "call", "index"});
        // 函数参数可省略 scalar（默认空字面值即“由 call 提供”）；两者都没有才算缺值。
        if (!object.contains("scalar") && !object.contains("call")) throw GraphJsonError("参数须给出 scalar 或 call。");
        GraphArgumentValue result; if (object.contains("scalar")) result.scalar = Scalar(object.value("scalar"));
        result.clientKey = ClientKey(object, false);
        if (object.contains("call")) result.call = std::make_shared<const GraphEcaValue>(Eca(object.value("call"), depth + 1));
        if (object.contains("index")) result.index = std::make_shared<const GraphArgumentValue>(Argument(object.value("index"), depth + 1));
        return result;
    }
    GraphEcaValue Eca(const QJsonValue& value, std::size_t depth = 0) {
        Count(depth);
        const auto object = Object(value, {"function", "kind", "enabled", "arguments", "children"}, {"client_key", "child_branch"});
        GraphEcaValue result; result.function = String(object, "function"); result.enabled = Boolean(object, "enabled");
        const auto kind = String(object, "kind");
        if (kind == "event") result.kind = UiDefinitionKind::Event;
        else if (kind == "condition") result.kind = UiDefinitionKind::Condition;
        else if (kind == "action") result.kind = UiDefinitionKind::Action;
        else if (kind == "call") result.kind = UiDefinitionKind::Call;
        else throw GraphJsonError("ECA kind 只能为 event/condition/action/call。");
        result.clientKey = ClientKey(object, false);
        if (object.contains("child_branch")) result.childBranch = Integer(object, "child_branch");
        for (const auto& arg : Array(object, "arguments")) result.arguments.push_back(Argument(arg, depth + 1));
        for (const auto& child : Array(object, "children")) result.children.push_back(std::make_shared<const GraphEcaValue>(Eca(child, depth + 1)));
        return result;
    }
    AtomicRenameCodeAudit Audit(const QJsonValue& value) {
        Count();
        const auto object = Object(value, {"kind", "id", "encoding", "source_sha256", "ui_catalog_sha256",
            "old_name", "new_name", "old_symbol", "new_symbol", "reason"});
        AtomicRenameCodeAudit audit;
        const auto kind = String(object, "kind");
        if (kind == "global_custom_code") audit.kind = AtomicRenameCodeKind::GlobalCustomCode;
        else if (kind == "trigger_custom_code") audit.kind = AtomicRenameCodeKind::TriggerCustomCode;
        else if (kind == "argument_code") audit.kind = AtomicRenameCodeKind::ArgumentCode;
        else if (kind == "variable_initial_value") audit.kind = AtomicRenameCodeKind::VariableInitialValue;
        else throw GraphJsonError("未知不透明代码审计 kind。");
        audit.id = Id(object, "id", audit.kind == AtomicRenameCodeKind::GlobalCustomCode);
        if (audit.kind == AtomicRenameCodeKind::GlobalCustomCode && audit.id != 0) throw GraphJsonError("全局代码块审计 id 必须为 0。");
        audit.encoding = Encoding(String(object, "encoding"));
        audit.sourceSha256 = Sha(object, "source_sha256"); audit.uiCatalogSha256 = Sha(object, "ui_catalog_sha256");
        audit.oldName = String(object, "old_name"); audit.newName = String(object, "new_name");
        audit.oldSymbol = String(object, "old_symbol"); audit.newSymbol = String(object, "new_symbol");
        audit.reason = String(object, "reason");
        return audit;
    }
    RemovalCodeConfirmation RemovalAudit(const QJsonValue& value) {
        Count();
        const auto object = Object(value, {"review_sha256", "decision", "reason", "blocks"});
        RemovalCodeConfirmation audit{Sha(object, "review_sha256"), String(object, "decision"), String(object, "reason"), {}};
        for (const auto& entry : Array(object, "blocks")) {
            Count();
            const auto block = Object(entry, {"kind", "id", "encoding", "source_sha256", "evidence_sha256"});
            const auto kind = String(block, "kind");
            RemovalBlockIdentity identity;
            if (kind == "global_custom_code") identity.kind = AtomicRenameCodeKind::GlobalCustomCode;
            else if (kind == "trigger_custom_code") identity.kind = AtomicRenameCodeKind::TriggerCustomCode;
            else if (kind == "argument_code") identity.kind = AtomicRenameCodeKind::ArgumentCode;
            else if (kind == "variable_initial_value") identity.kind = AtomicRenameCodeKind::VariableInitialValue;
            else throw GraphJsonError("未知删除代码审计kind。");
            identity.id = Id(block, "id", identity.kind == AtomicRenameCodeKind::GlobalCustomCode);
            if (identity.kind == AtomicRenameCodeKind::GlobalCustomCode && identity.id) throw GraphJsonError("全局代码块id必须为0。");
            identity.encoding = Encoding(String(block, "encoding"));
            identity.sourceSha256 = Sha(block, "source_sha256"); identity.evidenceSha256 = Sha(block, "evidence_sha256");
            audit.blocks.push_back(std::move(identity));
        }
        return audit;
    }
    GraphField Field(std::string_view text) {
        if (text == "comment") return GraphField::Comment;
        if (text == "description") return GraphField::Description;
        if (text == "enabled") return GraphField::Enabled;
        if (text == "initially_on") return GraphField::InitiallyOn;
        if (text == "run_on_init") return GraphField::RunOnInit;
        if (text == "variable_type") return GraphField::VariableType;
        if (text == "variable_array") return GraphField::VariableArray;
        if (text == "variable_array_size") return GraphField::VariableArraySize;
        if (text == "variable_initialized") return GraphField::VariableInitialized;
        if (text == "variable_initial_value") return GraphField::VariableInitialValue;
        if (text == "argument_scalar") return GraphField::ArgumentScalar;
        throw GraphJsonError("未知图字段。");
    }
    GraphFieldValue FieldValue(const QJsonObject& object, const char* key, GraphField field) {
        if (field == GraphField::ArgumentScalar) return Scalar(object.value(QLatin1String(key)));
        if (field == GraphField::VariableArraySize) return Integer(object, key);
        if (field == GraphField::Description || field == GraphField::VariableType || field == GraphField::VariableInitialValue)
            return String(object, key);
        return Boolean(object, key);
    }
    TriggerGraphEditBatch Batch(const QJsonObject& request) {
        const auto binding = Object(request.value("expected"), {"source_wtg_sha256", "source_wct_sha256",
            "ui_catalog_sha256", "current_wtg_sha256", "current_wct_sha256"}, {"map_object_references_sha256"});
        TriggerGraphEditBatch batch;
        batch.expected = {Sha(binding, "source_wtg_sha256"), Sha(binding, "source_wct_sha256"),
            Sha(binding, "ui_catalog_sha256"), Sha(binding, "current_wtg_sha256"), Sha(binding, "current_wct_sha256")};
        if (binding.contains("map_object_references_sha256")) batch.expected.mapObjectReferencesSha256 = Sha(binding, "map_object_references_sha256");
        const auto operations = Array(request, "operations");
        if (operations.isEmpty() || operations.size() > 256) throw GraphJsonError("批次必须有 1..256 个操作。");
        for (const auto& value : operations) {
            Count();
            if (!value.isObject()) throw GraphJsonError("操作必须是对象。");
            const auto op = String(value.toObject(), "op");
            if (op == "create") {
                const auto object = Object(value, {"op", "client_key", "value", "parent"}, {"before"});
                GraphCreate created; created.clientKey = ClientKey(object, true); created.parent = Parent(object.value("parent"));
                if (object.contains("before")) created.before = Ref(object.value("before"));
                if (!object.value("value").isObject()) throw GraphJsonError("create value 必须是对象。");
                const auto kind = String(object.value("value").toObject(), "kind");
                if (kind == "category") {
                    const auto fields = Object(object.value("value"), {"kind", "name", "comment"});
                    created.value = GraphCategoryValue{String(fields, "name"), Boolean(fields, "comment")};
                } else if (kind == "variable") {
                    const auto fields = Object(object.value("value"), {"kind", "name", "type_name", "is_array", "array_size", "initialized", "initial_value"});
                    created.value = GraphVariableValue{String(fields, "name"), String(fields, "type_name"), Boolean(fields, "is_array"),
                        Integer(fields, "array_size"), Boolean(fields, "initialized"), String(fields, "initial_value")};
                } else if (kind == "trigger") {
                    const auto fields = Object(object.value("value"), {"kind", "name", "description", "enabled", "initially_on", "run_on_init", "comment"});
                    created.value = GraphTriggerValue{String(fields, "name"), String(fields, "description"), Boolean(fields, "enabled"),
                        Boolean(fields, "initially_on"), Boolean(fields, "run_on_init"), Boolean(fields, "comment")};
                } else throw GraphJsonError("create kind 只能为 category/variable/trigger。");
                batch.operations.emplace_back(std::move(created));
            } else if (op == "set") {
                const auto object = Object(value, {"op", "target", "field", "expected", "value"});
                GraphSet set; set.target = Ref(object.value("target")); set.field = Field(String(object, "field"));
                set.expected = FieldValue(object, "expected", set.field); set.value = FieldValue(object, "value", set.field);
                batch.operations.emplace_back(std::move(set));
            } else if (op == "rename") {
                const auto object = Object(value, {"op", "target", "expected_name", "name"}, {"code_audits"});
                GraphRename rename; rename.target = Ref(object.value("target")); rename.expectedName = Raw(object.value("expected_name"));
                rename.name = String(object, "name");
                if (object.contains("code_audits")) for (const auto& audit : Array(object, "code_audits")) rename.codeAudits.push_back(Audit(audit));
                batch.operations.emplace_back(std::move(rename));
            } else if (op == "move") {
                const auto object = Object(value, {"op", "target"}, {"from", "to", "before", "after"});
                if (!object.contains("from") && !object.contains("to")) {
                    // 轻量移动：只给 before 或 after 之一，父列表取自该锚点。
                    if (object.contains("before") == object.contains("after")) throw GraphJsonError("轻量 move 须恰给 before 或 after 之一。");
                    const auto after = object.contains("after");
                    batch.operations.emplace_back(GraphPlace{Ref(object.value("target")), Ref(object.value(after ? "after" : "before")), after});
                    continue;
                }
                if (!object.contains("from") || !object.contains("to") || object.contains("after"))
                    throw GraphJsonError("完整 move 须同时给 from 与 to，并只用 before 定位；或省略 from/to 用 before/after 轻量移动。");
                GraphMove move; move.target = Ref(object.value("target")); move.from = Parent(object.value("from")); move.to = Parent(object.value("to"));
                if (object.contains("before")) move.before = Ref(object.value("before"));
                batch.operations.emplace_back(std::move(move));
            } else if (op == "remove") {
                const auto object = Object(value, {"op", "target", "expected_name", "parent"}, {"code_audits"});
                GraphRemove remove{Ref(object.value("target")), String(object, "expected_name"), Parent(object.value("parent"))};
                if (object.contains("code_audits")) for (const auto& audit : Array(object, "code_audits")) remove.codeAudits.push_back(RemovalAudit(audit));
                batch.operations.emplace_back(std::move(remove));
            } else if (op == "insert_subtree") {
                const auto object = Object(value, {"op", "parent", "root"}, {"before"});
                GraphInsertSubtree insert; insert.parent = Parent(object.value("parent")); insert.root = std::make_shared<const GraphEcaValue>(Eca(object.value("root")));
                if (object.contains("before")) insert.before = Ref(object.value("before"));
                batch.operations.emplace_back(std::move(insert));
            } else if (op == "insert_text") {
                const auto object = Object(value, {"op", "parent", "text"}, {"before"});
                GraphInsertText insert; insert.parent = Parent(object.value("parent")); insert.text = String(object, "text");
                if (object.contains("before")) insert.before = Ref(object.value("before"));
                batch.operations.emplace_back(std::move(insert));
            } else if (op == "replace_trigger_text") {
                const auto object = Object(value, {"op", "trigger", "text"});
                batch.operations.emplace_back(GraphReplaceTriggerText{Ref(object.value("trigger")), String(object, "text")});
            } else if (op == "patch_trigger_text") {
                const auto object = Object(value, {"op", "trigger", "from_line", "to_line", "expected", "text"});
                const auto from = Integer(object, "from_line"), to = Integer(object, "to_line");
                if (from < 1 || to < from - 1) throw GraphJsonError("from_line 须 >= 1，to_line 须 >= from_line - 1。");
                batch.operations.emplace_back(GraphPatchTriggerText{Ref(object.value("trigger")), static_cast<std::size_t>(from),
                    static_cast<std::size_t>(to), String(object, "expected"), String(object, "text")});
            } else if (op == "apply_feature_text") {
                const auto object = Object(value, {"op", "category", "text"}, {"client_key", "after"});
                GraphApplyFeatureText feature{String(object, "category"), String(object, "text"),
                    object.contains("client_key") ? String(object, "client_key") : std::string{}, std::nullopt};
                if (object.contains("after")) feature.after = Ref(object.value("after"));
                batch.operations.emplace_back(std::move(feature));
            } else if (op == "replace_argument") {
                const auto object = Object(value, {"op", "target", "expected", "value"});
                GraphReplaceArgument replace;
                replace.target = Ref(object.value("target"));
                replace.expected = ReadArgumentGuard(object.value("expected"));
                replace.value = Argument(object.value("value"), 0);
                batch.operations.emplace_back(std::move(replace));
            } else throw GraphJsonError("未知图操作。");
        }
        return batch;
    }
private:
    std::size_t records_ = 0;
    std::size_t createdReplyBytes_ = 0;
};

QJsonArray IndexRefs(const TriggerDocument& document, const std::vector<std::uint32_t>& indices, bool arguments) {
    if (indices.size() > kResponseBytes / 32) throw GraphJsonError("单个容器引用超过分页回执预算。", "MCP_TRIGGER_GRAPH_RESPONSE_BUDGET");
    QJsonArray refs;
    for (const auto index : indices) {
        if (index >= (arguments ? document.argumentNodes.size() : document.ecaNodes.size()))
            throw GraphJsonError("文档有失效节点索引。", "MCP_TRIGGER_GRAPH_INVALID");
        refs.append(RefJson(arguments ? GraphRecordKind::Argument : GraphRecordKind::Eca,
            arguments ? document.argumentNodes[index].id : document.ecaNodes[index].id));
    }
    return refs;
}
QJsonValue OptionalIndex(const TriggerDocument& document, const std::optional<std::uint32_t>& index, bool argument) {
    if (!index) return QJsonValue(QJsonValue::Null);
    if (*index >= (argument ? document.argumentNodes.size() : document.ecaNodes.size()))
        throw GraphJsonError("文档有失效嵌套引用。", "MCP_TRIGGER_GRAPH_INVALID");
    return RefJson(argument ? GraphRecordKind::Argument : GraphRecordKind::Eca,
        argument ? document.argumentNodes[*index].id : document.ecaNodes[*index].id);
}
QJsonObject QueryDocumentImpl(const TriggerDocument& document, const UiCatalog& catalog,
    const MapObjectReferenceSnapshot* mapObjects, const QJsonObject& request, const GraphNameResolver& names = {}) {
    Object(request, {"table"}, {"start", "limit", "schema_version", "argument_ids", "variable_ids", "include_replacement_guard",
        "functions", "ui_layer", "trigger_id", "category_id", "type", "match", "plan", "operation_index"});
    if (request.contains("schema_version") && Integer(request, "schema_version") != 1) throw GraphJsonError("未知图 schema_version。");
    const auto table = String(request, "table");
    if (table == "remove_audit") {
        Object(request, {"table", "plan", "operation_index"}, {"schema_version"});
        if (!request.value("plan").isObject()) throw GraphJsonError("plan须为同一图批次对象。");
        const auto index = Integer(request, "operation_index");
        if (index < 0) throw GraphJsonError("operation_index须为非负整数。");
        const auto plan = DecodeGraphBatch(request.value("plan").toObject()); if (!plan.Succeeded()) return Failure(plan);
        const auto review = QueryDeclarationRemovalAudit(document, catalog, *plan.value, static_cast<std::size_t>(index), {}, mapObjects);
        if (!review.Succeeded()) return Failure(review);
        const auto rows = [](const auto& blocks) {
            QJsonArray result;
            for (const auto& block : blocks) {
                const auto& b = block.identity;
                const char* kind = b.kind == AtomicRenameCodeKind::GlobalCustomCode ? "global_custom_code"
                    : b.kind == AtomicRenameCodeKind::TriggerCustomCode ? "trigger_custom_code"
                    : b.kind == AtomicRenameCodeKind::ArgumentCode ? "argument_code" : "variable_initial_value";
                QJsonArray evidence;
                for (const auto& hit : block.evidence) evidence.append(QJsonObject{{"context", QString::fromStdString(hit.context)},
                    {"text", QString::fromStdString(hit.text)}, {"decoded_byte_offset", QString::number(hit.decodedByteOffset)},
                    {"snippet_truncated", hit.text.size() < hit.decodedByteLength},
                    {"decoded_byte_length", QString::number(hit.decodedByteLength)}, {"line", QString::number(hit.line)},
                    {"column", QString::number(hit.column)}, {"raw_byte_offset", QString::number(hit.rawByteOffset)},
                    {"raw_byte_length", QString::number(hit.rawByteLength)}, {"raw_range_exact", hit.rawRangeExact}, {"definite", hit.definite}});
                result.append(QJsonObject{{"identity", QJsonObject{{"kind", kind}, {"id", QString::number(b.id)}, {"encoding", EncodingName(b.encoding)},
                    {"source_sha256", QString::fromStdString(b.sourceSha256)}, {"evidence_sha256", QString::fromStdString(b.evidenceSha256)}}},
                    {"complete", block.complete}, {"dynamic", block.dynamic}, {"referenced", block.referenced},
                    {"incomplete_reason", QString::fromStdString(block.incompleteReason)}, {"evidence", evidence}});
            }
            return result;
        };
        QJsonArray symbols, typed;
        for (const auto& symbol : review.value->symbols) symbols.append(QString::fromStdString(symbol));
        for (const auto id : review.value->typedArgumentIds) typed.append(QString::number(id));
        QJsonObject result{{"ok", true}, {"schema_version", 1}, {"table", "remove_audit"}, {"binding", BindingJson(review.value->binding)},
            {"review_sha256", QString::fromStdString(review.value->reviewSha256)}, {"symbols", symbols}, {"typed_argument_ids", typed},
            {"complete", review.value->complete}, {"groupable_blocks", rows(review.value->groupableBlocks)},
            {"individual_blocks", rows(review.value->individualBlocks)}, {"external_dependencies_verified", false}};
        if (static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) > kResponseBytes)
            throw GraphJsonError("完整删除审计超过回执预算，未截断放行。", "MCP_TRIGGER_GRAPH_RESPONSE_BUDGET");
        return result;
    }
    if (table == "trigger_text") {
        // 整个触发器导出为紧凑 T 写法，可直接修改后用 replace_trigger_text 写回。
        Object(request, {"table", "trigger_id"}, {"schema_version"});
        const auto triggerId = Id(request, "trigger_id");
        const auto binding = CaptureTriggerGraphBinding(document, catalog, mapObjects);
        if (!binding.Succeeded()) return Failure(binding);
        const auto text = RenderTriggerText(document, catalog, triggerId, kResponseBytes / 2, nullptr, mapObjects);
        if (!text.Succeeded()) return Failure(text);
        const auto trigger = std::find_if(document.triggers.begin(), document.triggers.end(),
            [triggerId](const auto& row) { return row.id == triggerId; });
        return {{"ok", true}, {"schema_version", 1}, {"binding", BindingJson(*binding.value)}, {"table", "trigger_text"},
            {"trigger", RefJson(GraphRecordKind::Trigger, triggerId)}, {"name", QString::fromStdString(trigger->name.text)},
            {"text", QString::fromStdString(*text.value)}};
    }
    if (table == "feature_text") {
        // 一个类别导出为功能写法（触发器 + 它们引用的变量），可修改后用 apply_feature_text 写回。
        Object(request, {"table", "category_id"}, {"schema_version"});
        const auto categoryRef = Id(request, "category_id");
        const auto category = std::find_if(document.categories.begin(), document.categories.end(),
            [categoryRef](const auto& row) { return row.id == categoryRef; });
        if (category == document.categories.end()) throw GraphJsonError("找不到该类别。", "MCP_TRIGGER_GRAPH_RECORD_MISSING");
        const auto binding = CaptureTriggerGraphBinding(document, catalog, mapObjects);
        if (!binding.Succeeded()) return Failure(binding);
        const auto text = RenderFeatureText(document, catalog, category->categoryId, kResponseBytes / 2, mapObjects);
        if (!text.Succeeded()) return Failure(text);
        return {{"ok", true}, {"schema_version", 1}, {"binding", BindingJson(*binding.value)}, {"table", "feature_text"},
            {"category", RefJson(GraphRecordKind::Category, categoryRef)}, {"name", QString::fromStdString(category->name.text)},
            {"text", QString::fromStdString(*text.value)}};
    }
    if (table == "outline") {
        // 目录总览：类别按文档顺序，每个类别列出触发器名与编号；match 按名字筛选（类别名命中时列出全部触发器）。
        Object(request, {"table"}, {"schema_version", "match", "category_id"});
        const auto match = request.contains("match") ? QString::fromStdString(String(request, "match")) : QString{};
        const auto contains = [&](const std::string& text) {
            return match.isEmpty() || QString::fromStdString(text).contains(match, Qt::CaseInsensitive);
        };
        const auto only = request.contains("category_id") ? std::optional<StableId>(Id(request, "category_id")) : std::nullopt;
        QJsonArray rows;
        std::size_t listed = 0;
        for (const auto& category : document.categories) {
            if (only && category.id != *only) continue;
            const bool whole = !match.isEmpty() && contains(category.name.text);
            QJsonArray triggers;
            for (const auto& trigger : document.triggers) {
                if (trigger.categoryId != category.categoryId || (!whole && !contains(trigger.name.text))) continue;
                QJsonObject row{{"id", QString::number(trigger.id)}, {"name", QString::fromStdString(trigger.name.text)}};
                if (trigger.type == 1) row.insert("comment", true);
                if (!trigger.enabled) row.insert("enabled", false);
                if (trigger.initiallyClosed) row.insert("initially_on", false);
                triggers.append(row);
            }
            if (!match.isEmpty() && !whole && triggers.isEmpty()) continue;
            listed += static_cast<std::size_t>(triggers.size());
            rows.append(QJsonObject{{"id", QString::number(category.id)}, {"name", QString::fromStdString(category.name.text)},
                {"triggers", triggers}});
        }
        return {{"ok", true}, {"schema_version", 1}, {"table", "outline"}, {"categories", rows},
            {"trigger_count", QString::number(listed)}};
    }
    if (table == "legacy_trigger_references") {
        // 旧版本编辑器写法的触发器引用；operations 连同 binding 可直接交给编辑工具改正（与界面右键改正同一组修改）。
        Object(request, {"table"}, {"schema_version", "trigger_id"});
        const auto only = request.contains("trigger_id") ? std::optional<StableId>(Id(request, "trigger_id")) : std::nullopt;
        const auto binding = CaptureTriggerGraphBinding(document, catalog, mapObjects);
        if (!binding.Succeeded()) return Failure(binding);
        auto references = FindLegacyTriggerReferences(document, only);
        const bool truncated = references.size() > 128;
        if (truncated) references.resize(128);
        const auto scalar = [](const GraphFieldValue& value) {
            const auto& item = std::get<GraphScalarValue>(value);
            return QJsonObject{{"kind", item.kind}, {"value", QString::fromStdString(item.value)}};
        };
        QJsonArray rows, operations;
        for (const auto& reference : references) {
            rows.append(QJsonObject{{"argument", RefJson(GraphRecordKind::Argument, reference.argumentId)},
                {"trigger", RefJson(GraphRecordKind::Trigger, reference.ownerTriggerId)},
                {"target", RefJson(GraphRecordKind::Trigger, reference.targetTriggerId)},
                {"value", QString::fromStdString(reference.value)}, {"symbol", QString::fromStdString(reference.symbol)}});
        }
        for (const auto& operation : LegacyTriggerReferenceFixes(references)) {
            const auto& set = std::get<GraphSet>(operation);
            operations.append(QJsonObject{{"op", "set"}, {"target", RefJson(set.target.kind, set.target.id)},
                {"field", "argument_scalar"}, {"expected", scalar(set.expected)}, {"value", scalar(set.value)}});
        }
        return {{"ok", true}, {"schema_version", 1}, {"binding", BindingJson(*binding.value)}, {"table", "legacy_trigger_references"},
            {"references", rows}, {"operations", operations}, {"truncated", truncated}};
    }
    if (table == "presets") {
        // 函数库预设（固定选项），可按参数类型筛选；写紧凑 T 时直接写预设名。
        Object(request, {"table"}, {"schema_version", "type", "start", "limit"});
        const auto type = request.contains("type") ? String(request, "type") : std::string{};
        const auto presets = PresetsOfType(catalog, type);
        const auto start = request.contains("start") ? static_cast<std::size_t>(Integer(request, "start")) : 0U;
        const auto limit = request.contains("limit") ? static_cast<std::size_t>(Integer(request, "limit")) : 64U;
        QJsonArray rows;
        for (std::size_t i = start; i < presets.size() && i < start + limit; ++i)
            rows.append(QJsonObject{{"name", QString::fromStdString(presets[i].name)}, {"type", QString::fromStdString(presets[i].type)},
                {"display", QString::fromStdString(presets[i].display)}, {"script", QString::fromStdString(presets[i].script)}});
        return {{"ok", true}, {"schema_version", 1}, {"table", "presets"}, {"type", QString::fromStdString(type)},
            {"total", QString::number(presets.size())}, {"eof", start + limit >= presets.size()}, {"rows", rows}};
    }
    if (table == "function_examples") {
        // 函数在本图里的真实用法：所在触发器与该处的紧凑 T 原句，便于照着写参数与预设。
        Object(request, {"table", "functions"}, {"schema_version", "limit"});
        const auto perFunction = request.contains("limit") ? std::min<std::size_t>(static_cast<std::size_t>(Integer(request, "limit")), 16U) : 4U;
        std::set<std::string> wanted;
        for (const auto& value : request.value("functions").toArray()) wanted.insert(value.toString().toStdString());
        std::map<StableId, std::pair<std::string, std::vector<TriggerTextAnchor>>> rendered;
        std::map<std::string, QJsonArray> examples;
        std::size_t renders = 0, bytes = 0;
        bool truncated = false;
        for (const auto& eca : document.ecaNodes) {
            if (!wanted.contains(eca.name.text) || examples[eca.name.text].size() >= static_cast<qsizetype>(perFunction)) continue;
            auto found = rendered.find(eca.sourceTriggerId);
            if (found == rendered.end()) {
                if (renders >= 64 || bytes >= kResponseBytes / 2) { truncated = true; continue; }
                ++renders;
                std::vector<TriggerTextAnchor> anchors;
                auto text = RenderTriggerText(document, catalog, eca.sourceTriggerId, kResponseBytes / 4, &anchors, mapObjects);
                found = rendered.emplace(eca.sourceTriggerId, std::make_pair(text.Succeeded() && text.value ? std::move(*text.value)
                    : std::string{}, std::move(anchors))).first;
            }
            const auto& [text, anchors] = found->second;
            const auto anchor = std::find_if(anchors.begin(), anchors.end(), [&](const auto& row) { return row.ecaId == eca.id; });
            if (anchor == anchors.end()) continue;
            std::size_t begin = 0;
            for (std::size_t line = 1; line < anchor->line && begin != std::string::npos; ++line) {
                begin = text.find('\n', begin);
                if (begin != std::string::npos) ++begin;
            }
            if (begin == std::string::npos) continue;
            auto line = text.substr(begin, text.find('\n', begin) - begin);
            line.erase(0, line.find_first_not_of(' '));
            if (line.size() > 2048) line = line.substr(0, 2048) + "…";
            bytes += line.size();
            const auto trigger = std::find_if(document.triggers.begin(), document.triggers.end(),
                [&](const auto& row) { return row.id == eca.sourceTriggerId; });
            examples[eca.name.text].append(QJsonObject{{"trigger", RefJson(GraphRecordKind::Trigger, eca.sourceTriggerId)},
                {"trigger_name", trigger == document.triggers.end() ? QString{} : QString::fromStdString(trigger->name.text)},
                {"line", static_cast<qint64>(anchor->line)}, {"text", QString::fromStdString(line)}});
        }
        QJsonObject byFunction;
        for (const auto& name : wanted) byFunction.insert(QString::fromStdString(name), examples[name]);
        return {{"ok", true}, {"schema_version", 1}, {"table", "function_examples"}, {"examples", byFunction}, {"truncated", truncated}};
    }
    if (request.contains("type")) throw GraphJsonError("type 只适用于 presets 表。");
    if (request.contains("trigger_id")) throw GraphJsonError("trigger_id 只适用于 trigger_text 表。");
    if (request.contains("category_id")) throw GraphJsonError("category_id 只适用于 feature_text 表。");
    const bool exactArguments = request.contains("argument_ids"), exactVariables = request.contains("variable_ids");
    const bool exact = exactArguments || exactVariables;
    const bool guards = request.contains("include_replacement_guard") && Boolean(request, "include_replacement_guard");
    if ((exactArguments || guards) && table != "arguments") throw GraphJsonError("定点参数及替换资格只适用于 arguments 表。");
    if (exactVariables && table != "variables") throw GraphJsonError("variable_ids 只适用于 variables 表。");
    if (exactArguments && exactVariables) throw GraphJsonError("两类定点 ID 不能混用。");
    if (exact && (request.contains("start") || request.contains("limit"))) throw GraphJsonError("定点 ID 与分页 start/limit 不可混用。");
    if (guards && !exactArguments) throw GraphJsonError("替换资格必须使用 argument_ids 定点查询，最多16个。");
    // ECA 筛选：按函数名（精确，最多16个）或所属 UI 函数库层过滤，分页作用于筛选后的序列。
    const bool filterFunctions = request.contains("functions"), filterLayer = request.contains("ui_layer");
    const bool filtered = filterFunctions || filterLayer;
    if (filtered && table != "ecas") throw GraphJsonError("functions/ui_layer 筛选只适用于 ecas 表。");
    if (filtered && exact) throw GraphJsonError("筛选不能与定点 ID 混用。");
    std::set<std::string, std::less<>> functionNames;
    if (filterFunctions) {
        const auto requestedNames = Array(request, "functions");
        if (requestedNames.isEmpty() || requestedNames.size() > 16) throw GraphJsonError("functions 要求1..16个函数名。");
        for (const auto& name : requestedNames) {
            if (!name.isString() || name.toString().isEmpty()) throw GraphJsonError("functions 只接受非空函数名。");
            if (!functionNames.insert(name.toString().toStdString()).second) throw GraphJsonError("functions 不能重复。");
        }
    }
    const auto layer = filterLayer ? String(request, "ui_layer") : std::string{};
    if (filterLayer && layer.empty()) throw GraphJsonError("ui_layer 不能为空。");
    const auto startRaw = request.contains("start") ? Integer(request, "start") : 0;
    const auto limitRaw = request.contains("limit") ? Integer(request, "limit") : 64;
    if (startRaw < 0 || limitRaw < 1 || limitRaw > 128) throw GraphJsonError("分页要求 start>=0 且 limit=1..128。");
    const auto binding = CaptureTriggerGraphBinding(document, catalog, mapObjects);
    if (!binding.Succeeded()) return Failure(binding);
    auto total = table == "categories" ? document.categories.size() : table == "variables" ? document.variables.size()
        : table == "triggers" ? document.triggers.size() : table == "ecas" ? document.ecaNodes.size()
        : table == "arguments" ? document.argumentNodes.size()
        : table == "map_objects" ? (mapObjects ? mapObjects->References().size() : 0) : std::numeric_limits<std::size_t>::max();
    if (total == std::numeric_limits<std::size_t>::max()) throw GraphJsonError("未知图 table。");
    std::vector<std::size_t> indices;
    if (exact) {
        const char* idsKey = exactVariables ? "variable_ids" : "argument_ids";
        const auto requested = Array(request, idsKey);
        if (requested.isEmpty() || requested.size() > 16) throw GraphJsonError("定点查询要求1..16个唯一 ID。");
        std::map<StableId, std::size_t> requestedPositions;
        indices.resize(static_cast<std::size_t>(requested.size()), std::numeric_limits<std::size_t>::max());
        for (qsizetype slot = 0; slot < requested.size(); ++slot) {
            const auto id = Id(QJsonObject{{"id", requested[slot]}}, "id");
            if (!requestedPositions.emplace(id, static_cast<std::size_t>(slot)).second) throw GraphJsonError("定点查询 ID 不能重复。");
        }
        const auto recordCount = exactVariables ? document.variables.size() : document.argumentNodes.size();
        for (std::size_t index = 0; index < recordCount; ++index) {
            const auto id = exactVariables ? document.variables[index].id : document.argumentNodes[index].id;
            const auto found = requestedPositions.find(id);
            if (found == requestedPositions.end()) continue;
            auto& at = indices[found->second];
            if (at != std::numeric_limits<std::size_t>::max()) throw GraphJsonError("定点 ID 在文档内有歧义。", "MCP_TRIGGER_GRAPH_INVALID");
            at = index;
        }
        if (std::find(indices.begin(), indices.end(), std::numeric_limits<std::size_t>::max()) != indices.end())
            throw GraphJsonError("定点 ID 已不存在；重新查询当前图。",
                exactVariables ? "MCP_TRIGGER_GRAPH_VARIABLE_MISSING" : "MCP_TRIGGER_GRAPH_ARGUMENT_MISSING");
        total = indices.size();
    }
    std::vector<std::size_t> matches;
    if (filtered) {
        for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
            const auto& eca = document.ecaNodes[index];
            if (filterFunctions && !functionNames.contains(eca.name.text)) continue;
            if (filterLayer && eca.uiLayer != layer) continue;
            matches.push_back(index);
        }
        total = matches.size();
    }
    const auto start = static_cast<std::size_t>(startRaw), end = std::min(total, start + static_cast<std::size_t>(limitRaw));
    if (!exact) for (auto index = start; index < end; ++index) indices.push_back(filtered ? matches[index] : index);
    std::map<std::int32_t, QJsonArray> categoryChildren;
    TriggerTypeSystem typeSystem(catalog, mapObjects);
    std::map<std::size_t, std::string> argumentTypes;
    if (table == "arguments") {
        for (auto index : indices) argumentTypes.emplace(index, "");
        std::map<StableId, std::vector<LocalVariable>> locals;
        for (const auto& eca : document.ecaNodes) for (std::size_t position = 0; position < eca.arguments.size(); ++position) {
            auto found = argumentTypes.find(eca.arguments[position]);
            if (found == argumentTypes.end()) continue;
            auto scope = locals.find(eca.sourceTriggerId);
            if (scope == locals.end()) {
                const auto trigger = std::find_if(document.triggers.begin(), document.triggers.end(),
                    [&](const auto& candidate) { return candidate.id == eca.sourceTriggerId; });
                scope = locals.emplace(eca.sourceTriggerId, trigger == document.triggers.end() ? std::vector<LocalVariable>{}
                    : typeSystem.CollectLocals(document, *trigger)).first;
            }
            const auto type = typeSystem.ExpectedType(document, eca, position, scope->second);
            if (!found->second.empty() && found->second != type) throw GraphJsonError("参数拥有者类型有歧义。", "MCP_TRIGGER_GRAPH_INVALID");
            found->second = type;
        }
    }
    Services::NameResolutionContext fallbackNames;
    fallbackNames.triggers = &document; fallbackNames.mapObjects = mapObjects;
    const auto appendName = [&](QJsonObject& row, std::string_view raw, std::string_view type) {
        std::string baseType(type);
        const auto objectType = [](std::string_view value) {
            return value == "unitcode" || value == "itemcode" || value == "abilcode" || value == "heroskillcode"
                || value == "buffcode" || value == "techcode" || value == "doodadcode" || value == "destructablecode";
        };
        for (std::size_t depth = 0; depth < 64 && !objectType(baseType); ++depth) {
            const auto* definition = typeSystem.Find(baseType);
            if (!definition || definition->baseType.empty() || definition->baseType == baseType) break;
            baseType = definition->baseType;
        }
        const auto resolved = names ? names(raw, baseType) : Services::ResolveName(fallbackNames, raw, baseType);
        QJsonArray candidates;
        for (const auto& candidate : resolved.candidates) candidates.append(QString::fromStdString(candidate));
        row.insert("raw_value", QString::fromStdString(resolved.rawValue));
        row.insert("display_name", QString::fromStdString(resolved.displayName));
        row.insert("name_state", QString::fromStdString(std::string(Services::NameResolutionStateName(resolved.state))));
        row.insert("name_reason", QString::fromStdString(resolved.reason));
        row.insert("name_reference", QString::fromStdString(resolved.reference));
        row.insert("icon_path", QString::fromStdString(resolved.iconPath));
        row.insert("name_candidates", candidates);
    };
    if (table == "categories") {
        for (auto index = start; index < end; ++index) categoryChildren.emplace(document.categories[index].categoryId, QJsonArray{});
        for (const auto& trigger : document.triggers) if (auto found = categoryChildren.find(trigger.categoryId); found != categoryChildren.end()) {
            if (found->second.size() > static_cast<qsizetype>(kResponseBytes / 32))
                throw GraphJsonError("分类引用超过分页回执预算。", "MCP_TRIGGER_GRAPH_RESPONSE_BUDGET");
            found->second.append(RefJson(GraphRecordKind::Trigger, trigger.id));
        }
    }
    QJsonArray rows;
    std::size_t bytes = 0;
    for (const auto index : indices) {
        QJsonObject row;
        if (table == "map_objects") {
            const auto& record = mapObjects->References()[index];
            row = {{"symbol", QString::fromStdString(record.symbol)}, {"type", QString::fromStdString(record.type)},
                {"rawcode", QString::fromStdString(record.rawcode)}, {"creation_id", record.creationId},
                {"source_index", QString::number(record.sourceIndex)}, {"member", QString::fromStdString(record.memberName)},
                {"member_sha256", QString::fromStdString(record.memberSha256)},
                {"classification_source", QString::fromStdString(record.classificationSource)}, {"writable", false}, {"is_array", false}};
            appendName(row, record.symbol, record.type);
        } else if (table == "categories") {
            const auto& record = document.categories[index];
            row = {{"ref", RefJson(GraphRecordKind::Category, record.id)}, {"category_id", record.categoryId},
                {"name", TextJson(record.name)}, {"flags", QJsonObject{{"comment", record.comment}}},
                {"triggers", categoryChildren.at(record.categoryId)}, {"source_span", OriginJson(record.origin)}};
        } else if (table == "variables") {
            const auto& record = document.variables[index];
            row = GraphVariableRowJson(record);
            if (record.hasDefault) appendName(row, record.value.text, record.typeName.text);
        } else if (table == "triggers") {
            const auto& record = document.triggers[index];
            row = {{"ref", RefJson(GraphRecordKind::Trigger, record.id)}, {"name", TextJson(record.name)},
                {"description", TextJson(record.description)}, {"category_id", record.categoryId},
                {"flags", QJsonObject{{"type", record.type}, {"enabled", record.enabled}, {"custom_text", record.customTextFlag},
                    {"initially_closed", record.initiallyClosed}, {"initially_on", record.initiallyClosed == 0}, {"run_on_init", record.runOnInit}}},
                {"ecas", IndexRefs(document, record.ecas, false)}, {"source_span", OriginJson(record.origin)}};
            // 与实际脚本生成器共用名称规则，不能让 Agent 按中文字数猜符号。
            // 这是当前名称的编码事实，不是脚本已经生成或运行的证明。
            row.insert("name_derived_trigger_symbol", record.type == 1 ? QJsonValue(QJsonValue::Null)
                : QJsonValue(QString::fromStdString(TriggerDirectoryEditor::TriggerSymbol(record.name))));
            row.insert("script_symbol_state", record.type == 1 ? "comment-no-runtime-symbol" : "name-derived");
            if (record.customTextFlag) {
                const auto bytes = record.customText.ToBytes();
                if (!bytes.Succeeded()) throw GraphJsonError("自定义代码槽不能按原编码读取。", "TRIGGER_CODE_ENCODING_UNSUPPORTED");
                const auto hash = Sha256(*bytes.value);
                if (!hash.Succeeded()) throw GraphJsonError("无法取得自定义代码槽身份。", "TRIGGER_CODE_SOURCE_STALE");
                row.insert("custom_code_sha256", QString::fromStdString(*hash.value));
                row.insert("custom_code_bytes", QString::number(bytes.value->size()));
            }
        } else if (table == "ecas") {
            const auto& record = document.ecaNodes[index];
            row = {{"ref", RefJson(GraphRecordKind::Eca, record.id)}, {"source_trigger_id", QString::number(record.sourceTriggerId)},
                {"name", TextJson(record.name)}, {"flags", QJsonObject{{"type", record.type}, {"enabled", record.enabled}}},
                {"child_branch", record.childId ? QJsonValue(*record.childId) : QJsonValue(QJsonValue::Null)},
                {"arguments", IndexRefs(document, record.arguments, true)}, {"children", IndexRefs(document, record.children, false)},
                {"ui_layer", QString::fromStdString(record.uiLayer)}, {"ui_signature", QString::fromStdString(record.uiSignature)},
                {"source_span", OriginJson(record.origin)}};
            const auto owner = std::find_if(document.triggers.begin(), document.triggers.end(),
                [&](const auto& trigger) { return trigger.id == record.sourceTriggerId; });
            row.insert("source_trigger_name", owner == document.triggers.end() ? QJsonValue(QJsonValue::Null) : QJsonValue(TextJson(owner->name)));
        } else {
            const auto& record = document.argumentNodes[index];
            row = {{"ref", RefJson(GraphRecordKind::Argument, record.id)}, {"value", TextJson(record.value)},
                {"flags", QJsonObject{{"type", record.type}, {"insert_call", record.insertCall}, {"insert_index", record.insertIndex}}},
                {"call", OptionalIndex(document, record.callEca, false)}, {"index", OptionalIndex(document, record.indexArgument, true)},
                {"source_span", OriginJson(record.origin)}};
            row.insert("expected_type", QString::fromStdString(argumentTypes[index]));
            if (!record.callEca && record.type != 0) appendName(row, record.value.text, argumentTypes[index]);
            if (guards) {
                const auto guard = CaptureArgumentReplacementGuard(document, catalog, record.id, {}, mapObjects);
                row["replacement_guard"] = guard.Succeeded() ? QJsonValue(ArgumentGuardJson(*guard.value)) : QJsonValue(QJsonValue::Null);
                row["replacement_diagnostics"] = DiagnosticsJson(guard.diagnostics).value("diagnostics");
            }
        }
        const auto rowBytes = static_cast<std::size_t>(QJsonDocument(row).toJson(QJsonDocument::Compact).size());
        if (rowBytes > kResponseBytes - bytes) throw GraphJsonError("图分页回执超过 1 MiB，请缩小 limit。", "MCP_TRIGGER_GRAPH_RESPONSE_BUDGET");
        bytes += rowBytes; rows.append(row);
    }
    const auto next = start >= total ? total : end;
    return {{"ok", true}, {"schema_version", 1}, {"binding", BindingJson(*binding.value)},
        {"counts", QJsonObject{{"categories", QString::number(document.categories.size())}, {"variables", QString::number(document.variables.size())},
            {"triggers", QString::number(document.triggers.size())}, {"ecas", QString::number(document.ecaNodes.size())},
            {"arguments", QString::number(document.argumentNodes.size())}}},
        {"table", QString::fromStdString(table)}, {"query_mode", exactVariables ? "variable_ids" : exactArguments ? "argument_ids" : filtered ? "filter" : "page"}, {"start", startRaw}, {"total", QString::number(total)},
        {"rows", rows}, {"next", QString::number(next)}, {"eof", next == total}};
}

QJsonObject QueryPage(McpWorkspace& workspace, const QJsonObject& request) {
    const auto environment = workspace.EnvironmentStatus();
    if (!environment.Succeeded()) return Failure(environment);
    const auto capturedObjects = workspace.MapObjectReferences();
    if (request.value("table") == QStringLiteral("map_objects") && !capturedObjects.Succeeded())
        return Failure(capturedObjects);
    const auto* mapObjects = capturedObjects.Succeeded() && capturedObjects.value &&
        !(*capturedObjects.value)->References().empty() ? capturedObjects.value->get() : nullptr;
    auto result = QueryDocumentImpl(workspace.VisibleDocument(), workspace.Catalog(), mapObjects, request,
        CaptureWorkspaceNameResolver(workspace, workspace.VisibleDocument(), mapObjects));
    result.insert("revision", RevisionJson(workspace.Revision()));
    result.insert("content_scope", workspace.HasActiveTransaction() ? "visible-transaction" : "committed-directory");
    InsertMapObjectReferenceDiagnostics(result, capturedObjects.diagnostics);
    return result;
}

QJsonObject GraphReceipt(const TriggerGraphEditReceipt& receipt, McpWorkspace& workspace) {
    QJsonObject created;
    for (const auto& [key, ref] : receipt.created) created[QString::fromStdString(key)] = RefJson(ref.kind, ref.id);
    QJsonArray renames;
    for (const auto& rename : receipt.renames) {
        renames.append(QJsonObject{{"declaration_id", QString::number(rename.declarationId)},
            {"changed_argument_count", QString::number(rename.changedArgumentIds.size())},
            {"references_available_via", "query_trigger_graph"}, {"code_coverage", rename.codeCoverage == AtomicRenameCodeCoverage::NoOpaqueCode
                ? "NoOpaqueCode" : "CallerAssertedNotRuntimeProven"},
            {"externally_audited_blocks", QString::number(rename.externallyAuditedCode.size())}, {"external_dependencies_verified", false},
            {"old_symbol", QString::fromStdString(rename.oldSymbol)}, {"new_symbol", QString::fromStdString(rename.newSymbol)}});
    }
    QJsonArray replacements;
    for (const auto& replace : receipt.argumentReplacements)
        replacements.append(QJsonObject{{"argument_id", QString::number(replace.argumentId)},
            {"before_subtree_sha256", QString::fromStdString(replace.beforeSubtreeSha256)},
            {"after_subtree_sha256", QString::fromStdString(replace.afterSubtreeSha256)},
            {"removed_ecas", QString::number(replace.removedEcas)}, {"removed_arguments", QString::number(replace.removedArguments)}});
    QJsonArray removals;
    for (const auto& audit : receipt.removalAudits) removals.append(QJsonObject{{"review_sha256", QString::fromStdString(audit.reviewSha256)},
        {"block_count", QString::number(audit.blocks.size())}, {"coverage", "CallerAssertedNotRuntimeProven"}});
    return {{"ok", true}, {"schema_version", 1}, {"before", BindingJson(receipt.before)}, {"binding", BindingJson(receipt.after)},
        {"revision", RevisionJson(workspace.Revision())}, {"transaction_active", workspace.HasActiveTransaction()},
        {"applied_operations", QString::number(receipt.appliedOperations)}, {"created", created}, {"renames", renames},
        {"argument_replacements", replacements}, {"removal_audits", removals},
        {"external_dependencies_verified", false}, {"saved", false}, {"runtime_verified", false}};
}

QJsonObject Shape(QJsonObject properties, QJsonArray required = {}) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QJsonObject StringSchema() { return {{"type", "string"}}; }
QJsonObject ArraySchema(QJsonObject items) { return {{"type", "array"}, {"items", items}, {"maxItems", static_cast<int>(kRecords)}}; }
QJsonObject GraphSchema() {
    const auto string = StringSchema(), boolean = QJsonObject{{"type", "boolean"}}, integer = QJsonObject{{"type", "integer"}};
    const auto ref = Shape({{"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"category", "variable", "trigger", "eca", "argument"}}}},
        {"id", QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}}, {"client_key", string}}, {"kind"});
    const auto parent = Shape({{"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"categories", "variables", "category_triggers", "trigger_ecas", "eca_children"}}}},
        {"owner", ref}, {"ordered_children", ArraySchema(ref)}, {"child_branch", integer}}, {"kind", "ordered_children"});
    const auto encoding = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"utf8", "utf8_bom", "gb18030", "latin1"}}};
    const auto raw = Shape({{"text", string}, {"encoding", encoding}, {"raw_bytes_hex", QJsonObject{{"type", "string"}, {"pattern", "^([0-9a-f]{2})*$"}}}},
        {"text", "encoding", "raw_bytes_hex"});
    const auto scalar = Shape({{"kind", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{0, 1, 3}}}}, {"value", string}}, {"kind", "value"});
    const auto sha = QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    const auto binding = Shape({{"source_wtg_sha256", sha}, {"source_wct_sha256", sha}, {"ui_catalog_sha256", sha},
        {"current_wtg_sha256", sha}, {"current_wct_sha256", sha}, {"map_object_references_sha256", sha}}, {"source_wtg_sha256", "source_wct_sha256", "ui_catalog_sha256", "current_wtg_sha256", "current_wct_sha256"});
    const auto audit = GraphRenameCodeAuditSchema();
    const auto ecaRef = QJsonObject{{"$ref", "#/$defs/eca"}}, argumentRef = QJsonObject{{"$ref", "#/$defs/argument"}};
    const auto eca = Shape({{"client_key", string}, {"function", string}, {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"event", "condition", "action", "call"}}}},
        {"enabled", boolean}, {"child_branch", integer}, {"arguments", ArraySchema(argumentRef)}, {"children", ArraySchema(ecaRef)}},
        {"function", "kind", "enabled", "arguments", "children"});
    const auto argument = Shape({{"client_key", string}, {"scalar", scalar}, {"call", ecaRef}, {"index", argumentRef}});
    const auto argumentGuard = Shape({{"schema_version", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{1}}}},
        {"owner_eca_id", QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}},
        {"source_trigger_id", QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}},
        {"argument_ordinal", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 2147483647}}},
        {"ui_signature", string}, {"expected_type", string}, {"subtree_sha256", sha}},
        {"schema_version", "owner_eca_id", "source_trigger_id", "argument_ordinal", "ui_signature", "expected_type", "subtree_sha256"});
    const auto category = Shape({{"kind", QJsonObject{{"enum", QJsonArray{"category"}}}}, {"name", string}, {"comment", boolean}}, {"kind", "name", "comment"});
    const auto variable = Shape({{"kind", QJsonObject{{"enum", QJsonArray{"variable"}}}}, {"name", string}, {"type_name", string}, {"is_array", boolean},
        {"array_size", integer}, {"initialized", boolean}, {"initial_value", string}}, {"kind", "name", "type_name", "is_array", "array_size", "initialized", "initial_value"});
    const auto trigger = Shape({{"kind", QJsonObject{{"enum", QJsonArray{"trigger"}}}}, {"name", string}, {"description", string}, {"enabled", boolean},
        {"initially_on", boolean}, {"run_on_init", boolean}, {"comment", boolean}}, {"kind", "name", "description", "enabled", "initially_on", "run_on_init", "comment"});
    const auto op = [](const char* name) { return QJsonObject{{"type", "string"}, {"enum", QJsonArray{name}}}; };
    QJsonArray variants;
    variants.append(Shape({{"op", op("create")}, {"client_key", string}, {"value", QJsonObject{{"oneOf", QJsonArray{category, variable, trigger}}}},
        {"parent", parent}, {"before", ref}}, {"op", "client_key", "value", "parent"}));
    variants.append(Shape({{"op", op("set")}, {"target", ref}, {"field", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"comment", "description", "enabled", "initially_on", "run_on_init",
        "variable_type", "variable_array", "variable_array_size", "variable_initialized", "variable_initial_value", "argument_scalar"}}}},
        {"expected", QJsonObject{{"oneOf", QJsonArray{boolean, integer, string, scalar}}}}, {"value", QJsonObject{{"oneOf", QJsonArray{boolean, integer, string, scalar}}}}},
        {"op", "target", "field", "expected", "value"}));
    variants.append(Shape({{"op", op("rename")}, {"target", ref}, {"expected_name", raw}, {"name", string}, {"code_audits", ArraySchema(audit)}},
        {"op", "target", "expected_name", "name"}));
    // move：from+to(+before) 为带完整顺序 guard 的移动；省略 from/to 时只给 before 或 after 之一做轻量移动（触发器/类别）。
    variants.append(Shape({{"op", op("move")}, {"target", ref}, {"from", parent}, {"to", parent}, {"before", ref}, {"after", ref}}, {"op", "target"}));
    variants.append(Shape({{"op", op("remove")}, {"target", ref}, {"expected_name", string}, {"parent", parent},
        {"code_audits", ArraySchema(GraphRemovalCodeAuditSchema())}}, {"op", "target", "expected_name", "parent"}));
    variants.append(Shape({{"op", op("insert_subtree")}, {"parent", parent}, {"root", ecaRef}, {"before", ref}}, {"op", "parent", "root"}));
    variants.append(Shape({{"op", op("replace_argument")}, {"target", ref}, {"expected", argumentGuard}, {"value", argumentRef}},
        {"op", "target", "expected", "value"}));
    variants.append(Shape({{"op", op("insert_text")}, {"parent", parent}, {"text", string}, {"before", ref}}, {"op", "parent", "text"}));
    variants.append(Shape({{"op", op("replace_trigger_text")}, {"trigger", ref}, {"text", string}}, {"op", "trigger", "text"}));
    variants.append(Shape({{"op", op("apply_feature_text")}, {"category", string}, {"text", string}, {"client_key", string}, {"after", ref}},
        {"op", "category", "text"}));
    const QJsonObject line{{"type", "integer"}, {"minimum", 0}};
    variants.append(Shape({{"op", op("patch_trigger_text")}, {"trigger", ref}, {"from_line", line}, {"to_line", line},
        {"expected", string}, {"text", string}}, {"op", "trigger", "from_line", "to_line", "expected", "text"}));
    auto schema = BasicSchema({{"schema_version", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{1}}}}, {"expected", binding},
        {"operations", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"items", QJsonObject{{"oneOf", variants}}}}}},
        {"transaction_id", "expected_revision", "expected", "operations"});
    schema["$defs"] = QJsonObject{{"eca", eca}, {"argument", argument}};
    return CodeIdentityGraphSchema(std::move(schema));
}
} // namespace

GraphNameResolver CaptureWorkspaceNameResolver(McpWorkspace& workspace,
    const Trigger::TriggerDocument& document, const Trigger::MapObjectReferenceSnapshot* mapObjects) {
    std::array<Services::ProjectSearchObjectSource, 7> objects;
    const auto capturedObjects = workspace.ObjectContext(ObjectData::ObjectType::Unit);
    for (std::size_t index = 0; index < objects.size(); ++index) {
        objects[index].type = static_cast<ObjectData::ObjectType>(index);
        if (!capturedObjects.Succeeded()) continue;
        const auto* source = capturedObjects.value->documents[index];
        objects[index].state = source ? (source->IsStructurallySafe() ? Services::ProjectSearchState::Ready
            : Services::ProjectSearchState::Invalid) : Services::ProjectSearchState::Missing;
        if (source) objects[index].records = source->Objects();
    }
    const auto capturedMap = workspace.MapDataContext("name-resolution", workspace.Revision().generation);
    const auto map = capturedMap.Succeeded() ? capturedMap.value->document : std::shared_ptr<const MapDocument>{};
    const auto* standard = capturedObjects.Succeeded() ? capturedObjects.value->standard : nullptr;
    return [objects, map, standard, triggers = &document, mapObjects](std::string_view raw, std::string_view type) {
        Services::NameResolutionContext context;
        context.objects = objects; context.mapDocument = map.get(); context.standardObjects = standard;
        context.triggers = triggers; context.mapObjects = mapObjects;
        return Services::ResolveName(context, raw, type);
    };
}

QJsonObject GraphVariableRowJson(const Trigger::VariableRecord& record) {
    return {{"ref", RefJson(GraphRecordKind::Variable, record.id)}, {"name", TextJson(record.name)},
        {"type_name", TextJson(record.typeName)}, {"value", TextJson(record.value)},
        {"flags", QJsonObject{{"unknown", record.unknown}, {"is_array", record.isArray}, {"array_size", record.size}, {"initialized", record.hasDefault}}},
        {"source_span", OriginJson(record.origin)}};
}

QJsonObject GraphBindingJson(const Trigger::TriggerGraphBinding& binding) { return BindingJson(binding); }

QJsonObject GraphRenameCodeAuditSchema() {
    const auto string = StringSchema();
    const auto encoding = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"utf8", "utf8_bom", "gb18030", "latin1"}}};
    const auto sha = QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    return Shape({{"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"global_custom_code", "trigger_custom_code", "argument_code", "variable_initial_value"}}}},
        {"id", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}}}, {"encoding", encoding}, {"source_sha256", sha}, {"ui_catalog_sha256", sha},
        {"old_name", string}, {"new_name", string}, {"old_symbol", string}, {"new_symbol", string}, {"reason", string}},
        {"kind", "id", "encoding", "source_sha256", "ui_catalog_sha256", "old_name", "new_name", "old_symbol", "new_symbol", "reason"});
}

QJsonObject GraphRemovalCodeAuditSchema() {
    auto block = GraphRenameCodeAuditSchema();
    auto fields = block.value("properties").toObject();
    for (const auto* key : {"ui_catalog_sha256", "old_name", "new_name", "old_symbol", "new_symbol", "reason"}) fields.remove(key);
    fields.insert("evidence_sha256", fields.value("source_sha256"));
    block = Shape(fields, {"kind", "id", "encoding", "source_sha256", "evidence_sha256"});
    return Shape({{"review_sha256", fields.value("source_sha256")},
        {"decision", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"no_reference"}}}}, {"reason", StringSchema()},
        {"blocks", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 16384}, {"items", block}}}},
        {"review_sha256", "decision", "reason", "blocks"});
}
Result<std::vector<RemovalCodeConfirmation>> DecodeGraphRemovalCodeAudits(const QJsonArray& audits) {
    try {
        if (audits.size() > static_cast<qsizetype>(kRecords) || static_cast<std::size_t>(QJsonDocument(audits).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            throw GraphJsonError("删除审计超过请求预算。", "MCP_TRIGGER_GRAPH_REQUEST_BUDGET");
        GraphReader reader; std::vector<RemovalCodeConfirmation> result;
        for (const auto& audit : audits) result.push_back(reader.RemovalAudit(audit));
        return Result<std::vector<RemovalCodeConfirmation>>::Success(std::move(result));
    } catch (const GraphJsonError& failure) { return Result<std::vector<RemovalCodeConfirmation>>::Failure("MCP", failure.code, failure.what()); }
}

Result<std::vector<Trigger::AtomicRenameCodeAudit>> DecodeGraphRenameCodeAudits(const QJsonArray& audits) {
    try {
        if (audits.size() > static_cast<qsizetype>(kRecords)
            || static_cast<std::size_t>(QJsonDocument(audits).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            throw GraphJsonError("改名审计超过既有 Graph 请求预算。", "MCP_TRIGGER_GRAPH_REQUEST_BUDGET");
        GraphReader reader;
        std::vector<Trigger::AtomicRenameCodeAudit> result;
        result.reserve(static_cast<std::size_t>(audits.size()));
        for (const auto& audit : audits) result.push_back(reader.Audit(audit));
        return Result<std::vector<Trigger::AtomicRenameCodeAudit>>::Success(std::move(result));
    } catch (const GraphJsonError& failure) {
        return Result<std::vector<Trigger::AtomicRenameCodeAudit>>::Failure("MCP", failure.code, failure.what());
    }
}

Result<Trigger::TriggerGraphEditBatch> DecodeGraphBatch(const QJsonObject& request) {
    try {
        if (static_cast<std::size_t>(QJsonDocument(request).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            throw GraphJsonError("图请求超过 4 MiB 编码预算。", "MCP_TRIGGER_GRAPH_REQUEST_BUDGET");
        Object(request, {"expected", "operations"}, {"schema_version"});
        if (request.contains("schema_version") && Integer(request, "schema_version") != 1)
            throw GraphJsonError("未知图 schema_version。");
        GraphReader reader;
        return Result<Trigger::TriggerGraphEditBatch>::Success(reader.Batch(request));
    } catch (const GraphJsonError& failure) {
        return Result<Trigger::TriggerGraphEditBatch>::Failure("MCP", failure.code, failure.what());
    }
}

QJsonObject EcaFunctionFilterSchema() {
    return {{"type", "array"}, {"minItems", 1}, {"maxItems", 16}, {"uniqueItems", true},
        {"items", QJsonObject{{"type", "string"}, {"minLength", 1}}}};
}

QJsonObject GraphQueryProperties() {
    const auto id = QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}};
    return {{"schema_version", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{1}}}},
        {"table", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"categories", "variables", "triggers", "ecas", "arguments",
            "map_objects", "trigger_text", "feature_text", "presets", "function_examples", "outline", "legacy_trigger_references", "remove_audit"}}}},
        {"plan", GraphOperationSchema()}, {"operation_index", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
        {"start", QJsonObject{{"type", "integer"}, {"minimum", 0}}}, {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}},
        {"argument_ids", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 16}, {"uniqueItems", true}, {"items", id}}},
        {"variable_ids", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 16}, {"uniqueItems", true}, {"items", id}}},
        {"include_replacement_guard", QJsonObject{{"type", "boolean"}}}, {"functions", EcaFunctionFilterSchema()},
        {"ui_layer", QJsonObject{{"type", "string"}, {"minLength", 1}}}, {"trigger_id", id}, {"category_id", id},
        {"type", QJsonObject{{"type", "string"}, {"minLength", 1}}}, {"match", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}};
}

QJsonObject GraphOperationSchema() {
    const auto original = GraphSchema();
    auto properties = original.value("properties").toObject();
    properties.remove("transaction_id");
    properties.remove("expected_revision");
    auto schema = Shape(properties, {"expected", "operations"});
    schema.insert("$defs", original.value("$defs"));
    return schema;
}

QJsonObject QueryGraphDocument(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    const Trigger::MapObjectReferenceSnapshot* mapObjects, const QJsonObject& request, const GraphNameResolver& names) {
    try { return QueryDocumentImpl(document, catalog, mapObjects, request, names); }
    catch (const GraphJsonError& failure) { return Failure(failure.code, failure.what()); }
}

void RegisterTriggerGraphTools(McpServer& server) {
    ToolDefinition query;
    query.name = "query_trigger_graph";
    query.description = "分页查询 GUI 图；ecas 可用 functions（精确函数名）或 ui_layer（函数库层）筛选后再分页；arguments 可用 argument_ids 定点查询最多16项，并通过 include_replacement_guard 取得共用核心的原位替换资格；"
        "trigger_text 表配 trigger_id 把整个触发器导出为紧凑 T 写法；feature_text 表配 category_id 把整个类别（触发器与引用的变量）导出为功能写法；"
        "presets 表（可配 type）列出函数库预设；function_examples 表配 functions 给出这些函数在本图里的真实用法原句；"
        "outline 表一次列出全部类别和触发器的名字与编号，可用 match 按名字筛选。";
    query.inputSchema = BasicSchema(GraphQueryProperties(), {"table"});
    query.maxResponseBytes = kResponseBytes;
    query.handler = [&server](const QJsonObject& request) -> QJsonValue {
        QJsonObject error; auto* workspace = RequireWorkspace(server, error); if (!workspace) return error;
        try { return QueryPage(*workspace, request); }
        catch (const GraphJsonError& failure) { return Failure(failure.code, failure.what()); }
    };
    server.RegisterTool(std::move(query));
    ToolDefinition edit;
    edit.name = "edit_trigger_graph";
    edit.permission = ToolPermission::Edit;
    edit.description = "在单一目录事务内原子编辑 GUI 图：创建、字段、精确改名、顺序移动、删除、类型子树和受guard保护的参数原位替换。"
        "insert_text / replace_trigger_text 用紧凑 T 写法一次写入整段事件/条件/动作；apply_feature_text 一次写入一个功能的变量与多个触发器（after 指定新触发器插在哪个触发器之后，写法见 AGENT_GUIDE）；"
        "move 省略 from/to 时只给 before 或 after 即可把触发器/类别放到锚点旁。普通T代码保护不变；"
        "rename_custom_trigger/copy_custom_trigger仅编辑既有JASS自定义触发器，需显式dry_run，每批最多一个并置于第一项；其它操作可作为原子后缀。失败不发布前缀，复制不接受代码正文。";
    edit.inputSchema = GraphSchema(); edit.maxResponseBytes = kResponseBytes;
    edit.handler = [&server](const QJsonObject& request) -> QJsonValue {
        QJsonObject error; auto* workspace = RequireWorkspace(server, error); if (!workspace) return error;
        try {
            if (static_cast<std::size_t>(QJsonDocument(request).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
                throw GraphJsonError("图请求超过 4 MiB 编码预算。", "MCP_TRIGGER_GRAPH_REQUEST_BUDGET");
            Object(request, {"transaction_id", "expected_revision", "expected", "operations"}, {"schema_version", "dry_run"});
            if (request.contains("schema_version") && Integer(request, "schema_version") != 1) throw GraphJsonError("未知图 schema_version。");
            Object(request.value("expected_revision"), {"generation", "source_sha256", "ui_catalog_sha256"});
            const auto envelope = ParseEnvelope(request); if (!envelope.Succeeded()) return Failure(envelope);
            auto graphRequest = request;
            graphRequest.remove("transaction_id");
            graphRequest.remove("expected_revision");
            const bool codeIdentity = HasCodeIdentityOperation(graphRequest);
            if (codeIdentity && !request.value("dry_run").isBool()) return Failure("TRIGGER_CODE_REQUEST_INVALID", "代码身份编辑必须显式提供dry_run布尔值。");
            const bool dryRun = request.value("dry_run").toBool(false); graphRequest.remove("dry_run");
            Services::TriggerCodeIdentitySummary summary;
            Result<Trigger::TriggerGraphEditBatch> batch;
            if (codeIdentity) {
                auto context = workspace->CodeIdentityContext(); if (!context.Succeeded()) return Failure(context);
                batch = DecodeCodeIdentityGraphBatch(graphRequest, *context.value, dryRun, &summary);
            } else batch = DecodeGraphBatch(graphRequest);
            if (!batch.Succeeded()) return Failure(batch);
            const auto result = workspace->ApplyGraphEdit(envelope.value->first, envelope.value->second, *batch.value, {}, dryRun);
            if (!result.Succeeded()) return Failure(result);
            auto payload = GraphReceipt(*result.value, *workspace); payload.insert("dry_run", dryRun);
            payload.insert("diagnostics", DiagnosticsJson(result.diagnostics).value("diagnostics"));
            payload.insert("published", !dryRun && (result.value->before.currentWtgSha256 != result.value->after.currentWtgSha256 || result.value->before.currentWctSha256 != result.value->after.currentWctSha256));
            if (codeIdentity) payload.insert("code_identity", CodeIdentitySummaryJson(summary));
            return payload;
        } catch (const GraphJsonError& failure) { return Failure(failure.code, failure.what()); }
    };
    server.RegisterTool(std::move(edit));
}
} // namespace War3Map::Mcp
