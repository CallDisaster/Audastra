#include "McpToolInternal.h"

#include <algorithm>
#include <utility>

namespace War3Map::Mcp {
void RegisterRevisionTools(McpServer& server) {
    {
        ToolDefinition tool;
        tool.name = "get_revision";
        tool.description = "获取项目 revision、事务状态与候选导出能力";
        tool.inputSchema = BasicSchema();
        tool.handler = [&server](const QJsonObject&) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            const Status environment = workspace->EnvironmentStatus();
            QJsonObject result{
                {"ok", environment.Succeeded()},
                {"revision", RevisionJson(workspace->Revision())},
                {"identity_checked", true},
                {"identity_current", environment.Succeeded()},
                {"transaction_active", workspace->HasActiveTransaction()},
                {"legacy_write_tools_enabled", false},
                {"archive_export_implemented", false},
                {"workspace_archive_save_implemented", true},
                {"workspace_archive_save_tool", "save_map_edit_candidate"},
                {"candidate_kind", "expanded-directory"},
                {"warvk_generated_module_install", "pinned-v1-only"},
            };
            if (workspace->HasActiveTransaction()) {
                result["transaction_id"] =
                    QString::number(workspace->ActiveTransactionId());
            }
            if (!environment.Succeeded()) {
                result["diagnostics"] = DiagnosticsJson(
                    environment.diagnostics).value("diagnostics");
            }
            return result;
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "begin_edit";
        tool.permission = ToolPermission::Edit;
        tool.description = "按完整 expected_revision 获取进程/工作区单写租约";
        tool.inputSchema = BasicSchema({}, {"expected_revision"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto revision = ParseRevision(parameters);
            if (!revision.Succeeded()) return Failure(revision);
            auto started = workspace->BeginEdit(*revision.value);
            if (!started.Succeeded()) return Failure(started);
            return QJsonObject{
                {"ok", true},
                {"transaction_id", QString::number(*started.value)},
                {"base_revision", RevisionJson(*revision.value)},
            };
        };
        server.RegisterTool(std::move(tool));
    }

}

void RegisterEditTools(McpServer& server) {
    {
        ToolDefinition tool;
        tool.name = "create_trigger";
        tool.permission = ToolPermission::Edit;
        tool.description = "在活动 overlay 中新建普通 GUI 触发器";
        tool.inputSchema = BasicSchema({}, {
            "transaction_id", "expected_revision", "category_id", "name"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            if (!envelope.Succeeded()) return Failure(envelope);
            auto categoryId = ParseInt32(parameters, "category_id");
            if (!categoryId.Succeeded()) return Failure(categoryId);
            if (!parameters.value("name").isString()) {
                return Failure("MCP_STRING_REQUIRED", "name 必须是字符串。");
            }
            auto created = workspace->CreateTrigger(
                envelope.value->first,
                envelope.value->second,
                *categoryId.value,
                parameters.value("name").toString().toStdString());
            if (!created.Succeeded()) return Failure(created);
            return QJsonObject{
                {"ok", true}, {"trigger_id", QString::number(*created.value)}};
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "append_root_eca";
        tool.permission = ToolPermission::Edit;
        tool.description = "按当前 Catalog 定义向普通触发器追加根 event/condition/action";
        tool.inputSchema = BasicSchema({
            {"kind", QJsonObject{
                {"type", "string"},
                {"enum", QJsonArray{"event", "condition", "action"}},
            }},
        }, {
            "transaction_id", "expected_revision", "trigger_id", "kind", "function_name"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            auto triggerId = ParseUnsignedId(parameters, "trigger_id");
            auto kind = ParseRootKind(parameters);
            if (!envelope.Succeeded()) return Failure(envelope);
            if (!triggerId.Succeeded()) return Failure(triggerId);
            if (!kind.Succeeded()) return Failure(kind);
            if (!parameters.value("function_name").isString()) {
                return Failure("MCP_STRING_REQUIRED", "function_name 必须是字符串。");
            }
            auto appended = workspace->AppendRootEca(
                envelope.value->first,
                envelope.value->second,
                *triggerId.value,
                *kind.value,
                parameters.value("function_name").toString().toStdString());
            if (!appended.Succeeded()) return Failure(appended);
            return ReceiptJson(*appended.value);
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "append_argument_call";
        tool.permission = ToolPermission::Edit;
        tool.description = "把 Catalog 中返回类型匹配的 call 插入普通参数";
        tool.inputSchema = BasicSchema({}, {
            "transaction_id", "expected_revision", "argument_id",
            "function_name"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            auto argumentId = ParseUnsignedId(parameters, "argument_id");
            if (!envelope.Succeeded()) return Failure(envelope);
            if (!argumentId.Succeeded()) return Failure(argumentId);
            if (!parameters.value("function_name").isString()) {
                return Failure(
                    "MCP_STRING_REQUIRED",
                    "function_name 必须是字符串。");
            }
            auto appended = workspace->AppendArgumentCall(
                envelope.value->first,
                envelope.value->second,
                *argumentId.value,
                parameters.value("function_name").toString().toStdString());
            if (!appended.Succeeded()) return Failure(appended);
            return ReceiptJson(*appended.value);
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "set_scalar_argument";
        tool.permission = ToolPermission::Edit;
        tool.description = "设置普通非嵌套参数：value_kind 0=预设（TriggerParams 名）、1=变量（udg_名，类型须一致）、"
            "3=直接值（整数/实数/true|false/文本/自定义代码/物编对象编号，如技能写 A04W 不加引号）";
        tool.inputSchema = BasicSchema({
            {"value_kind", QJsonObject{
                {"type", "integer"},
                {"enum", QJsonArray{0, 1, 3}},
            }},
        }, {
            "transaction_id", "expected_revision", "argument_id", "value_kind", "value"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            auto argumentId = ParseUnsignedId(parameters, "argument_id");
            auto valueKind = ParseInt32(parameters, "value_kind");
            if (!envelope.Succeeded()) return Failure(envelope);
            if (!argumentId.Succeeded()) return Failure(argumentId);
            if (!valueKind.Succeeded()) return Failure(valueKind);
            if (!parameters.value("value").isString()) {
                return Failure("MCP_STRING_REQUIRED", "value 必须是字符串。");
            }
            const Status status = workspace->SetScalarArgument(
                envelope.value->first,
                envelope.value->second,
                *argumentId.value,
                *valueKind.value,
                parameters.value("value").toString().toStdString());
            if (!status.Succeeded()) return Failure(status);
            return QJsonObject{{"ok", true}};
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "preview_edit";
        tool.description = "只读预览活动 overlay 的 WTG/WCT 双文件语义差异";
        tool.inputSchema = BasicSchema({}, {"transaction_id", "expected_revision"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            if (!envelope.Succeeded()) return Failure(envelope);
            auto preview = workspace->PreviewEdit(
                envelope.value->first, envelope.value->second);
            if (!preview.Succeeded()) return Failure(preview);
            QJsonArray changes;
            for (const SemanticChange& change : preview.value->changes) {
                changes.append(QJsonObject{
                    {"kind", QString::fromUtf8(change.kind)},
                    {"path", QString::fromStdWString(change.path.generic_wstring())},
                    {"summary", QString::fromUtf8(change.summary)},
                    {"before_sha256", QString::fromUtf8(change.beforeSha256)},
                    {"after_sha256", QString::fromUtf8(change.afterSha256)},
                });
            }
            return QJsonObject{{"ok", true}, {"changes", changes}};
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "validate_edit";
        tool.description = "验证来源未变化、Catalog identity、WTG/WCT seal 与 L0 overlay";
        tool.inputSchema = BasicSchema({}, {"transaction_id", "expected_revision"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            if (!envelope.Succeeded()) return Failure(envelope);
            const ValidationReport report = workspace->ValidateEdit(
                envelope.value->first, envelope.value->second);
            QJsonObject result = DiagnosticsJson(report.diagnostics);
            result["ok"] = report.satisfied;
            result["satisfied"] = report.satisfied;
            result["highest_proven_level"] =
                static_cast<int>(report.highestProvenLevel);
            return result;
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "export_map_candidate";
        tool.permission = ToolPermission::Save;
        tool.description = "原子导出不覆盖的展开目录候选；.w3x/.w3m 在无真保存服务时拒绝";
        tool.inputSchema = BasicSchema({}, {
            "transaction_id", "expected_revision", "output_path"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            if (!envelope.Succeeded()) return Failure(envelope);
            if (!parameters.value("output_path").isString() ||
                parameters.value("output_path").toString().isEmpty()) {
                return Failure("MCP_OUTPUT_PATH_REQUIRED", "output_path 必须是非空字符串。");
            }
            const std::filesystem::path output(
                parameters.value("output_path").toString().toStdWString());
            const Status status = workspace->ExportCandidate(
                envelope.value->first, envelope.value->second, output);
            if (!status.Succeeded()) return Failure(status);
            return QJsonObject{
                {"ok", true},
                {"candidate_kind", "expanded-directory"},
                {"editor_true_save", false},
                {"output_path", QString::fromStdWString(output.wstring())},
                {"revision", RevisionJson(workspace->Revision())},
            };
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "rollback_edit";
        tool.permission = ToolPermission::Edit;
        tool.description = "丢弃活动 overlay 并释放单写租约，不改源目录";
        tool.inputSchema = BasicSchema({}, {"transaction_id", "expected_revision"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto envelope = ParseEnvelope(parameters);
            if (!envelope.Succeeded()) return Failure(envelope);
            const Status status = workspace->Rollback(
                envelope.value->first, envelope.value->second);
            if (!status.Succeeded()) return Failure(status);
            return QJsonObject{{"ok", true}, {"revision", RevisionJson(workspace->Revision())}};
        };
        server.RegisterTool(std::move(tool));
    }
}

} // namespace War3Map::Mcp
