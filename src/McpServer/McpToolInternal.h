#pragma once

#include "McpServer.h"
#include "McpWorkspace.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace War3Map::Mcp {

QJsonObject DiagnosticsJson(const std::vector<Diagnostic>& diagnostics);
// 查询结果附带的旁路诊断：只放前 3 条与总数，避免每次查询都带上整批同类警告。
void InsertMapObjectReferenceDiagnostics(QJsonObject& result, const std::vector<Diagnostic>& diagnostics);
QJsonObject Failure(std::string code, std::string message);
QJsonObject Failure(const Status& status);
template <typename T>
QJsonObject Failure(const Result<T>& result) {
    return DiagnosticsJson(result.diagnostics);
}
QJsonObject RevisionJson(const ProjectRevision& revision);
bool IsLowerHexSha256(std::string_view value);
Result<std::uint64_t> ParseUnsignedId(const QJsonObject& object, const char* field, bool allowZero = false);
Result<ProjectRevision> ParseRevision(const QJsonObject& parameters);
Result<std::int32_t> ParseInt32(const QJsonObject& object, const char* field);
Result<std::filesystem::path> ParseAbsolutePath(const QJsonObject& object, const char* field);
Result<std::filesystem::path> HostConfiguredSaveRoot(const char* name);
Result<Trigger::UiDefinitionKind> ParseRootKind(const QJsonObject& object);
QString KindName(Trigger::UiDefinitionKind kind);
McpWorkspace* RequireWorkspace(McpServer& server, QJsonObject& error);
Result<std::pair<TransactionId, ProjectRevision>> ParseEnvelope(const QJsonObject& parameters);
QJsonObject ReceiptJson(const Trigger::EcaAppendReceipt& receipt);
QJsonObject BasicSchema(QJsonObject properties = {}, QJsonArray required = {});

void RegisterReadOnlyTools(McpServer& server);
void RegisterArchiveIdentityTools(McpServer& server);
void RegisterArchiveMemberTools(McpServer& server);
void RegisterImportTools(McpServer& server);
void RegisterProjectSearchTools(McpServer& server);
void RegisterTerrainTools(McpServer& server);
void RegisterMapEditSaveTool(McpServer& server);
void RegisterMapCreateTool(McpServer& server);
void RegisterRevisionTools(McpServer& server);
void RegisterWarVKTool(McpServer& server);
void RegisterEditTools(McpServer& server);
void RegisterTriggerGraphTools(McpServer& server);

} // namespace War3Map::Mcp
