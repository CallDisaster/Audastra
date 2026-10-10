#include "McpProductTools.h"
#include "McpTools.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../../src/QtCommon/LocalConsentPreference.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Mcp;
void Expect(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void PersistentConsentDoesNotInferUnknownValues() {
    QTemporaryDir temporary;
    Expect(temporary.isValid(), "consent fixture temporary directory");
    const auto path = temporary.filePath(QStringLiteral("consent.ini"));
    const auto key = QStringLiteral("feedback/consent");
    const std::vector<QVariant> values{true, false, QStringLiteral("true"), QStringLiteral("false"),
        QStringLiteral("unknown-consent-value"), QStringLiteral("TRUE"), 1, QByteArray("true")};
    for (std::size_t index = 0; index < values.size(); ++index) {
        QSettings persisted(path, QSettings::IniFormat);
        persisted.setValue(key, values[index]);
        persisted.sync();
        Expect(persisted.status() == QSettings::NoError, "consent fixture persisted");
        QSettings reopened(path, QSettings::IniFormat);
        Expect(QtCommon::ReadExplicitLocalConsent(reopened, key) == (index == 0 || index == 2),
            "unknown persisted preference inferred as consent");
        reopened.beginGroup(QStringLiteral("unrelated"));
        reopened.setValue(key, true);
        Expect(!QtCommon::ReadExplicitLocalConsent(reopened, key), "group-scoped value inferred as consent");
    }
    QSettings absent(temporary.filePath(QStringLiteral("absent.ini")), QSettings::IniFormat);
    Expect(!QtCommon::ReadExplicitLocalConsent(absent, key), "absent preference inferred as consent");
}
void LocalOnlyDraftAndStrictSchema() {
    QTemporaryDir temporary;
    Expect(temporary.isValid(), "temporary directory");
    Feedback::PrivacyContext privacy;
    privacy.mapNames = {"私有作者地图.w3x"};
    const auto actualTemporary = QFileInfo(temporary.path()).canonicalFilePath();
    Expect(!actualTemporary.isEmpty(), "canonical temporary directory");
    auto opened = Feedback::FeedbackQueue::Open(std::filesystem::path(actualTemporary.toStdWString()) / L"queue", privacy);
    Expect(opened.Succeeded(), "queue open");
    const auto queue = *opened.value;
    const ProductToolHost host{[queue] { return Result<std::shared_ptr<Feedback::FeedbackQueue>>::Success(queue); }, [] { return 200000LL; }, {}};
    const QJsonObject args{{"issue", "internal_tool_error"}, {"error_code", "INTERNAL_TEST"},
        {"call_site", "save-service/verify"}, {"tool_name", "editor_save_map"},
        {"diagnostic", "无法打开 E:/中文用户/私有作者地图.w3x；api_key=sk-test-secret-do-not-send-1234567890"}};
    auto result = InvokeProductTool("feedback.draft", args, host);
    if (!result.value("ok").toBool())
        std::cerr << "fixture draft diagnostics: " << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << '\n';
    Expect(result.value("ok").toBool(), "draft");
    Expect(!result.value("consent").toBool() && !result.value("sent").toBool(), "default no consent/no send");
    const auto id = result.value("entry").toObject().value("report_id").toString();
    Expect(!id.isEmpty(), "stable report ID");
    auto duplicate = InvokeProductTool("feedback.draft", args, host);
    Expect(duplicate.value("deduplicated").toBool(), "dedup");
    Expect(duplicate.value("entry").toObject().value("occurrences").toString() == "2", "dedup counter");
    const auto preview = queue->GetPreview(id.toStdString());
    Expect(preview.Succeeded(), "preview");
    const auto& json = preview.value->delivery.reportJson;
    Expect(json.find("E:/") == std::string::npos && json.find("私有作者地图") == std::string::npos
        && json.find("sk-test-secret") == std::string::npos, "private data in report");
    auto list = InvokeProductTool("feedback.list", {{"offset", 0}, {"limit", 1}}, host);
    Expect(list.value("ok").toBool() && list.value("entries").toArray().size() == 1, "bounded queue list");
    auto status = InvokeProductTool("feedback.status", {{"report_id", id}}, host);
    Expect(status.value("ok").toBool() && !status.value("sent").toBool(), "status");
    for (const auto* forbidden : {"map_content", "trigger_content", "conversation", "api_key", "consent", "send"}) {
        auto bad = args; bad.insert(forbidden, "private-original-body");
        Expect(!InvokeProductTool("feedback.draft", bad, host).value("ok").toBool(), "unknown or sending field accepted");
    }
    auto mapError = args; mapError.insert("issue", "user_map_error");
    Expect(!InvokeProductTool("feedback.draft", mapError, host).value("ok").toBool(), "user map error accepted as app issue");
    Expect(!InvokeProductTool("feedback.list", {{"limit", 129}}, host).value("ok").toBool(), "unbounded page");
    Expect(!InvokeProductTool("feedback.list", {{"offset", 0.5}}, host).value("ok").toBool(), "fractional page");
    Expect(!InvokeProductTool("feedback.status", {{"report_id", 123}}, host).value("ok").toBool(), "numeric ID");
    Expect(queue->SetConsent(false).Succeeded(), "disable");
    Expect(queue->List().Succeeded() && queue->List().value->empty(), "disable clears unsent queue");
}
void UpdateAgentCannotInstall() {
    const auto status = InvokeProductTool("update.status", {}, {});
    Expect(status.value("ok").toBool() && !status.value("installation_available_to_agent").toBool(), "update read only status");
    Expect(status.value("check_state") == "not_checked", "unperformed check not latest");
    Expect(!InvokeProductTool("update.status", {{"install_dir", "C:/private"}}, {}).value("ok").toBool(), "update write parameters");
    const auto definitions = ProductToolDefinitions();
    Expect(definitions.size() == 4, "product tool count");
    for (const auto& tool : definitions) {
        Expect(tool.name != "update.apply" && tool.name != "feedback.send", "agent install/send exposed");
        Expect(!tool.inputSchema.value("additionalProperties").toBool(true), "strict tool schema");
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try { PersistentConsentDoesNotInferUnknownValues(); LocalOnlyDraftAndStrictSchema(); UpdateAgentCannotInstall(); std::cout << "product tools passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
