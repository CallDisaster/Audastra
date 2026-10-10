#include "AgentSettingsDialog.h"
#include "AgentSettingsStore.h"
#include "ApplicationSettingsTransfer.h"
#include "CredentialStore.h"
#include "ProviderCatalog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>

using namespace War3Map::Agent;

namespace {

ProviderSettings Provider(const QString& base = QStringLiteral("https://api.example.invalid/v1")) {
    ProviderSettings provider;
    provider.id = QStringLiteral("fixture-provider");
    provider.name = QStringLiteral("原创测试站点");
    provider.baseUrl = base;
    return provider;
}

AgentSettings Configured() {
    AgentSettings settings;
    settings.aiEnabled = true;
    settings.allowEdits = true;
    auto provider = Provider();
    provider.credentialRef = QStringLiteral("fixture-reference");
    ModelSettings model;
    model.id = QStringLiteral("fixture-model");
    model.name = QStringLiteral("测试模型");
    model.contextOverride = 400000;
    model.providerContext = 200000;
    provider.models.push_back(model);
    settings.providers.push_back(provider);
    return settings;
}

QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool Write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray Http(const QByteArray& body, int status = 200) {
    return QByteArray("HTTP/1.1 ") + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: "
        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// 只监听回环的原创协议夹具；没有访问收费 API 或读取作者密钥。
class LocalModelServer final : public QObject {
public:
    LocalModelServer() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled] {
                    buffer->append(socket->readAll());
                    if (*handled || !buffer->contains("\r\n\r\n")) return;
                    *handled = true;
                    requests.push_back(*buffer);
                    const auto response = responder ? responder(*buffer) : QByteArray{};
                    if (!response.isEmpty()) { socket->write(response); socket->disconnectFromHost(); }
                });
            }
        });
    }
    bool Start() { return server.listen(QHostAddress::LocalHost); }
    QString Url() const { return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()); }
    QTcpServer server;
    QList<QByteArray> requests;
    std::function<QByteArray(const QByteArray&)> responder;
};

} // namespace

class AgentSettingsTests final : public QObject {
    Q_OBJECT
private slots:
    void settingsRoundtripAndCredentialIsolation();
    void invalidSaveKeepsPreviousSettings();
    void malformedAndPlaintextCredentialsAreRejected();
    void contextPriorityAndUnknownDefault();
    void encryptedCredentialsAreUserAndReferenceBound();
    void realOpenAiDirectoryProtocolWithUnknownContext();
    void geminiDirectoryUsesHeaderAndGenerationModels();
    void anthropicDirectoryPagination();
    void paginationLoopsAndRedirectsAreRejected();
    void cancelTimeoutAndErrorRedaction();
    void nativeDialogAcceptsImeAndModelOverrides();
    void failedDialogCommitRollsBackOnlyNewCredentials();
    void toolSwitchesPreserveUnavailableDisabledNames();
    void portableSettingsRoundTripAndStrictWhitelist();
    void portableProvidersKeepOnlyMatchingLocalCredentials();
    void dialogImportStagesUntilHostCommitAndPreservesSecrets();
};

void AgentSettingsTests::settingsRoundtripAndCredentialIsolation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentSettingsStore store(directory.filePath(QStringLiteral("设置/agent.json")));
    auto settings = Configured();
    settings.mcpEnabled = true;
    settings.mcpPort = 19876;
    settings.disabledTools = {QStringLiteral("save_map_edit_candidate")};
    QString error;
    QVERIFY2(store.Save(settings, &error), qPrintable(error));
    const auto encoded = Read(store.FilePath());
    QVERIFY(!encoded.isEmpty());
    QVERIFY(!encoded.contains("api_key"));
    QVERIFY(!encoded.contains("password"));
    QVERIFY(!encoded.contains("authorization"));
    const auto result = store.Load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(result.aiEnabled);
    QVERIFY(result.allowEdits);
    QCOMPARE(result.mcpPort, quint16(19876));
    QCOMPARE(result.disabledTools, settings.disabledTools);
    QCOMPARE(result.providers.size(), qsizetype(1));
    QCOMPARE(result.providers.front().credentialRef, QStringLiteral("fixture-reference"));
    QCOMPARE(result.providers.front().models.front().contextOverride, qint64(400000));
    QCOMPARE(result.providers.front().models.front().providerContext, qint64(200000));
}

void AgentSettingsTests::invalidSaveKeepsPreviousSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentSettingsStore store(directory.filePath(QStringLiteral("agent.json")));
    auto settings = Configured();
    QString error;
    QVERIFY(store.Save(settings, &error));
    const auto previous = Read(store.FilePath());
    settings.providers.front().models.push_back(settings.providers.front().models.front());
    QVERIFY(!store.Save(settings, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(Read(store.FilePath()), previous);
    settings = Configured();
    settings.providers.front().baseUrl = QStringLiteral("https://fixture.invalid/v1?api_key=fixture-secret");
    QVERIFY(!store.Save(settings, &error));
    QCOMPARE(Read(store.FilePath()), previous);
    settings = Configured();
    settings.providers.front().models.front().contextOverride = -1;
    QVERIFY(!store.Save(settings, &error));
    QCOMPARE(Read(store.FilePath()), previous);
}

void AgentSettingsTests::malformedAndPlaintextCredentialsAreRejected() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentSettingsStore store(directory.filePath(QStringLiteral("agent.json")));
    QString error;
    QVERIFY(Write(store.FilePath(), "{broken"));
    QVERIFY(!store.Load(&error).aiEnabled);
    QVERIFY(!error.isEmpty());
    QCOMPARE(Read(store.FilePath()), QByteArray("{broken"));
    const QByteArray unsafe = R"({"version":1,"ai_enabled":true,"providers":[{"id":"p","api_key":"fixture-secret"}]})";
    QVERIFY(Write(store.FilePath(), unsafe));
    QVERIFY(!store.Load(&error).aiEnabled);
    QVERIFY(!error.contains(QStringLiteral("fixture-secret")));
    QVERIFY(!error.isEmpty());
    QCOMPARE(Read(store.FilePath()), unsafe);
    QVERIFY(Write(store.FilePath(), R"({"version":1,"mcp_port":8.5})"));
    static_cast<void>(store.Load(&error));
    QVERIFY(!error.isEmpty());
}

void AgentSettingsTests::contextPriorityAndUnknownDefault() {
    ModelSettings model;
    const auto unknown = ResolveContextWindow(model);
    QCOMPARE(unknown.tokens, qint64(262144));
    QVERIFY(unknown.estimated);
    QVERIFY(!unknown.source.isEmpty());
    model.catalogContext = 32000;
    const auto catalog = ResolveContextWindow(model);
    QCOMPARE(catalog.tokens, qint64(32000));
    QVERIFY(!catalog.estimated);
    model.providerContext = 64000;
    const auto provider = ResolveContextWindow(model);
    QCOMPARE(provider.tokens, qint64(64000));
    QVERIFY(provider.source != catalog.source);
    model.contextOverride = 123456;
    const auto overridden = ResolveContextWindow(model);
    QCOMPARE(overridden.tokens, qint64(123456));
    QVERIFY(overridden.source != provider.source);
    model.contextOverride = 0;
    QCOMPARE(ResolveContextWindow(model).tokens, qint64(64000));
}

void AgentSettingsTests::encryptedCredentialsAreUserAndReferenceBound() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    CredentialStore store(directory.path());
    const QString value = QStringLiteral("fixture-key-独立测试-1234");
    QString error;
#ifdef Q_OS_WIN
    QVERIFY2(store.Write(QStringLiteral("original-ref"), value, &error), qPrintable(error));
    QVERIFY(store.Exists(QStringLiteral("original-ref")));
    QCOMPARE(store.Read(QStringLiteral("original-ref"), &error), value);
    QVERIFY(error.isEmpty());
    const auto files = QDir(directory.path()).entryList(QStringList{QStringLiteral("*.bin")}, QDir::Files);
    QCOMPARE(files.size(), qsizetype(1));
    const auto encrypted = Read(directory.filePath(files.front()));
    QVERIFY(!encrypted.contains(value.toUtf8()));
    QVERIFY(store.Write(QStringLiteral("second-ref"), QStringLiteral("fixture-second-key"), &error));
    const auto after = QDir(directory.path()).entryList(QStringList{QStringLiteral("*.bin")}, QDir::Files);
    for (const auto& file : after) if (!files.contains(file)) QVERIFY(Write(directory.filePath(file), encrypted));
    QVERIFY(store.Read(QStringLiteral("second-ref"), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.Read(QStringLiteral("original-ref"), &error), value);
    QVERIFY(store.Remove(QStringLiteral("original-ref"), &error));
    QVERIFY(!store.Exists(QStringLiteral("original-ref")));
#else
    QVERIFY(!store.Write(QStringLiteral("original-ref"), value, &error));
    QVERIFY(!error.isEmpty());
#endif
}

void AgentSettingsTests::realOpenAiDirectoryProtocolWithUnknownContext() {
    LocalModelServer server;
    QVERIFY(server.Start());
    server.responder = [](const auto&) { return Http(R"({"data":[{"id":"fixture-chat","object":"model","owned_by":"fixture"},{"id":"fixture-large","context_window":750000}]})"); };
    ProviderCatalog catalog;
    QList<ModelSettings> result;
    connect(&catalog, &ProviderCatalog::ModelsReady, this, [&result](const QString&, const QList<ModelSettings>& models) { result = models; });
    QSignalSpy ready(&catalog, &ProviderCatalog::ModelsReady);
    QSignalSpy failed(&catalog, &ProviderCatalog::Failed);
    catalog.Fetch(Provider(server.Url()), QStringLiteral("fixture-only-key"));
    QTRY_COMPARE(ready.size(), qsizetype(1));
    QCOMPARE(failed.size(), qsizetype(0));
    QCOMPARE(server.requests.size(), qsizetype(1));
    QVERIFY(server.requests.front().startsWith("GET /v1/models HTTP/1.1"));
    QVERIFY(server.requests.front().toLower().contains("authorization: bearer fixture-only-key"));
    QCOMPARE(result.size(), qsizetype(2));
    QCOMPARE(result.front().id, QStringLiteral("fixture-chat"));
    QVERIFY(!result.front().enabled);
    QCOMPARE(ResolveContextWindow(result.front()).tokens, qint64(262144));
    QVERIFY(ResolveContextWindow(result.front()).estimated);
    QCOMPARE(result.back().providerContext, qint64(750000));
}

void AgentSettingsTests::geminiDirectoryUsesHeaderAndGenerationModels() {
    LocalModelServer server;
    QVERIFY(server.Start());
    server.responder = [](const auto&) { return Http(R"({"models":[{"name":"models/fixture-generate","displayName":"Fixture Generate","inputTokenLimit":1048576,"outputTokenLimit":8192,"supportedGenerationMethods":["generateContent"]},{"name":"models/fixture-embed","supportedGenerationMethods":["embedContent"]}]})"); };
    auto provider = Provider(server.Url()); provider.protocol = QStringLiteral("gemini");
    ProviderCatalog catalog;
    QList<ModelSettings> result;
    connect(&catalog, &ProviderCatalog::ModelsReady, this, [&result](const QString&, const QList<ModelSettings>& models) { result = models; });
    QSignalSpy ready(&catalog, &ProviderCatalog::ModelsReady);
    catalog.Fetch(provider, QStringLiteral("fixture-google-key"));
    QTRY_COMPARE(ready.size(), qsizetype(1));
    QCOMPARE(result.size(), qsizetype(1));
    QCOMPARE(result.front().id, QStringLiteral("fixture-generate"));
    QCOMPARE(result.front().providerContext, qint64(1048576));
    QCOMPARE(result.front().maxOutputTokens, qint64(8192));
    QVERIFY(server.requests.front().toLower().contains("x-goog-api-key: fixture-google-key"));
    QVERIFY(!server.requests.front().left(server.requests.front().indexOf("\r\n")).contains("fixture-google-key"));
}

void AgentSettingsTests::anthropicDirectoryPagination() {
    LocalModelServer server;
    QVERIFY(server.Start());
    server.responder = [](const auto& request) {
        return request.contains("after_id=fixture-one")
            ? Http(R"({"data":[{"id":"fixture-two","display_name":"Fixture Two","max_input_tokens":200000}],"has_more":false,"last_id":"fixture-two"})")
            : Http(R"({"data":[{"id":"fixture-one","display_name":"Fixture One"}],"has_more":true,"last_id":"fixture-one"})");
    };
    auto provider = Provider(server.Url()); provider.protocol = QStringLiteral("anthropic");
    ProviderCatalog catalog;
    QList<ModelSettings> result;
    connect(&catalog, &ProviderCatalog::ModelsReady, this, [&result](const QString&, const QList<ModelSettings>& models) { result = models; });
    QSignalSpy ready(&catalog, &ProviderCatalog::ModelsReady);
    catalog.Fetch(provider, QStringLiteral("fixture-anthropic-key"));
    QTRY_COMPARE(ready.size(), qsizetype(1));
    QCOMPARE(server.requests.size(), qsizetype(2));
    QCOMPARE(result.size(), qsizetype(2));
    QVERIFY(server.requests.front().toLower().contains("anthropic-version: 2023-06-01"));
    QVERIFY(server.requests.front().toLower().contains("x-api-key: fixture-anthropic-key"));
    QCOMPARE(result.back().providerContext, qint64(200000));
}

void AgentSettingsTests::paginationLoopsAndRedirectsAreRejected() {
    LocalModelServer server;
    QVERIFY(server.Start());
    server.responder = [](const auto&) { return Http(R"({"data":[{"id":"fixture"}],"has_more":true,"last_id":"fixture"})"); };
    auto provider = Provider(server.Url()); provider.protocol = QStringLiteral("anthropic");
    ProviderCatalog catalog;
    QSignalSpy ready(&catalog, &ProviderCatalog::ModelsReady);
    QSignalSpy failed(&catalog, &ProviderCatalog::Failed);
    catalog.Fetch(provider, QStringLiteral("fixture-only-key"));
    QTRY_COMPARE(failed.size(), qsizetype(1));
    QCOMPARE(server.requests.size(), qsizetype(2));
    QCOMPARE(ready.size(), qsizetype(0));
    QVERIFY(!catalog.IsBusy());
    failed.clear(); server.requests.clear();
    server.responder = [](const auto&) { return QByteArray("HTTP/1.1 302 Found\r\nLocation: https://fixture.invalid/capture\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); };
    catalog.Fetch(provider, QStringLiteral("fixture-only-key"));
    QTRY_COMPARE(failed.size(), qsizetype(1));
    QCOMPARE(server.requests.size(), qsizetype(1));
    QCOMPARE(ready.size(), qsizetype(0));
}

void AgentSettingsTests::cancelTimeoutAndErrorRedaction() {
    LocalModelServer server;
    QVERIFY(server.Start());
    ProviderCatalog catalog;
    QSignalSpy ready(&catalog, &ProviderCatalog::ModelsReady);
    QSignalSpy failed(&catalog, &ProviderCatalog::Failed);
    catalog.Fetch(Provider(server.Url()), QStringLiteral("fixture-only-key"), 10000);
    catalog.Cancel();
    QCOMPARE(failed.size(), qsizetype(1));
    QVERIFY(!catalog.IsBusy());
    failed.clear();
    catalog.Fetch(Provider(server.Url()), QStringLiteral("fixture-only-key"), 30);
    QTRY_COMPARE(failed.size(), qsizetype(1));
    QVERIFY(!catalog.IsBusy());
    failed.clear();
    server.responder = [](const auto&) { return Http(R"({"error":"fixture-only-key"})", 401); };
    catalog.Fetch(Provider(server.Url()), QStringLiteral("fixture-only-key"));
    QTRY_COMPARE(failed.size(), qsizetype(1));
    QVERIFY(!failed.front().at(1).toString().contains(QStringLiteral("fixture-only-key")));
    QVERIFY(failed.front().at(1).toString().contains(QStringLiteral("401")));
    QCOMPARE(ready.size(), qsizetype(0));
}

void AgentSettingsTests::nativeDialogAcceptsImeAndModelOverrides() {
    AgentSettingsDialog dialog(Configured());
    auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("agentProviderName"));
    auto* context = dialog.findChild<QLineEdit*>(QStringLiteral("agentModelContextOverride"));
    QVERIFY(name != nullptr); QVERIFY(context != nullptr);
    name->selectAll();
    QInputMethodEvent committed; committed.setCommitString(QStringLiteral("中文测试提供商"));
    QApplication::sendEvent(name, &committed);
    context->setText(QStringLiteral("123456"));
    bool saved = false;
    dialog.SetCommitHandler([&saved](const AgentSettings& candidate, QString*) {
        saved = candidate.providers.front().name == QStringLiteral("中文测试提供商")
            && candidate.providers.front().models.front().contextOverride == 123456;
        return saved;
    });
    dialog.accept();
    QVERIFY(saved);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.Settings().providers.front().name, QStringLiteral("中文测试提供商"));
}

void AgentSettingsTests::failedDialogCommitRollsBackOnlyNewCredentials() {
#ifdef Q_OS_WIN
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    CredentialStore credentials(directory.path());
    QString error;
    QVERIFY(credentials.Write(QStringLiteral("fixture-reference"), QStringLiteral("fixture-old-key"), &error));
    AgentSettingsDialog dialog(Configured());
    dialog.SetCredentialStore(&credentials);
    auto* secret = dialog.findChild<QLineEdit*>(QStringLiteral("agentProviderSecret"));
    QVERIFY(secret != nullptr);
    secret->setText(QStringLiteral("fixture-new-key"));
    dialog.SetCommitHandler([](const AgentSettings&, QString* message) { *message = QStringLiteral("fixture-commit-refused"); return false; });
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QCOMPARE(credentials.Read(QStringLiteral("fixture-reference"), &error), QStringLiteral("fixture-old-key"));
    QCOMPARE(QDir(directory.path()).entryList(QStringList{QStringLiteral("*.bin")}, QDir::Files).size(), qsizetype(1));
    const auto* message = dialog.findChild<QLabel*>(QStringLiteral("agentSettingsMessage"));
    QVERIFY(message != nullptr);
    QCOMPARE(message->text(), QStringLiteral("fixture-commit-refused"));
    dialog.reject();
    QCOMPARE(credentials.Read(QStringLiteral("fixture-reference"), &error), QStringLiteral("fixture-old-key"));
#endif
}

void AgentSettingsTests::toolSwitchesPreserveUnavailableDisabledNames() {
    auto initial = Configured();
    initial.disabledTools = {QStringLiteral("future-tool")};
    AgentSettingsDialog dialog(initial);
    dialog.SetAvailableTools({QStringLiteral("query_trigger_graph"), QStringLiteral("edit_trigger_graph")});
    auto* tools = dialog.findChild<QListWidget*>(QStringLiteral("agentMcpTools"));
    QVERIFY(tools != nullptr);
    QCOMPARE(tools->count(), 3);
    for (int row = 0; row < tools->count(); ++row) {
        if (tools->item(row)->data(Qt::UserRole).toString() == QStringLiteral("edit_trigger_graph")) tools->item(row)->setCheckState(Qt::Unchecked);
    }
    QVERIFY(dialog.Settings().disabledTools.contains(QStringLiteral("future-tool")));
    QVERIFY(dialog.Settings().disabledTools.contains(QStringLiteral("edit_trigger_graph")));
    QVERIFY(!dialog.Settings().disabledTools.contains(QStringLiteral("query_trigger_graph")));
}

void AgentSettingsTests::portableSettingsRoundTripAndStrictWhitelist() {
    const ApplicationPreferences application{QStringLiteral("Q:/NewMachine/Warcraft"), QStringLiteral("Q:/NewMachine/YDWE"),
        QStringLiteral("未安装的原创字体"), QStringLiteral("graphite")};
    auto settings = Configured(); settings.mcpEnabled = true; settings.allowCommands = true;
    settings.disabledTools = {QStringLiteral("private-tool")};
    QString error;
    const auto bytes = ApplicationSettingsTransfer::Encode(application, settings, &error);
    QVERIFY2(!bytes.isEmpty(), qPrintable(error));
    for (const auto forbidden : {"credential_ref", "fixture-reference", "mcp_enabled", "allow_edits", "allow_commands", "disabled_tools"})
        QVERIFY(!bytes.contains(forbidden));
    const auto imported = ApplicationSettingsTransfer::Decode(bytes, &error);
    QVERIFY2(imported.has_value(), qPrintable(error));
    QCOMPARE(imported->application.gameRoot, application.gameRoot);
    QCOMPARE(imported->application.uiDataRoot, application.uiDataRoot);
    QCOMPARE(imported->application.uiFontFamily, application.uiFontFamily);
    QCOMPARE(imported->application.themeId, application.themeId);
    QCOMPARE(imported->providers.front().models.front().contextOverride, qint64(400000));
    QVERIFY(imported->providers.front().credentialRef.isEmpty());
    auto portable = settings; portable.providers = imported->providers;
    QCOMPARE(ApplicationSettingsTransfer::Encode(imported->application, portable, &error), bytes);
    const auto root = QJsonDocument::fromJson(bytes).object();
    const auto rejected = [&](QJsonObject candidate) {
        QString diagnostic;
        return !ApplicationSettingsTransfer::Decode(QJsonDocument(candidate).toJson(), &diagnostic) && !diagnostic.isEmpty();
    };
    for (const auto field : {"api_key", "credential_ref", "allow_save", "mcp_port", "unknown"}) {
        auto candidate = root; candidate.insert(QLatin1String(field), QStringLiteral("must-not-be-imported")); QVERIFY(rejected(candidate));
    }
    { auto candidate = root; candidate.insert("version", 2); QVERIFY(rejected(candidate)); }
    { auto candidate = root; candidate.insert("version", 1.5); QVERIFY(rejected(candidate)); }
    { auto candidate = root; candidate.remove("application"); QVERIFY(rejected(candidate)); }
    for (const auto field : {"credential_ref", "secret", "authorization", "allow_edits"}) {
        auto candidate = root; auto providers = candidate.value("providers").toArray(); auto provider = providers.first().toObject();
        provider.insert(QLatin1String(field), "private-value"); providers[0] = provider; candidate.insert("providers", providers); QVERIFY(rejected(candidate));
    }
    for (const auto address : {"https://name:secret@example.invalid/v1", "https://example.invalid/v1?api_key=secret", "https://example.invalid/v1#secret"}) {
        auto candidate = root; auto providers = candidate.value("providers").toArray(); auto provider = providers.first().toObject();
        provider.insert("base_url", QLatin1String(address)); providers[0] = provider; candidate.insert("providers", providers); QVERIFY(rejected(candidate));
        provider = root.value("providers").toArray().first().toObject(); provider.insert("website", QLatin1String(address));
        providers[0] = provider; candidate.insert("providers", providers); QVERIFY(rejected(candidate));
    }
    { auto candidate = root; auto providers = candidate.value("providers").toArray(); providers.append(providers.first());
        candidate.insert("providers", providers); QVERIFY(rejected(candidate)); }
    { auto candidate = root; QJsonArray providers;
        for (int i = 0; i < 129; ++i) providers.append(root.value("providers").toArray().first());
        candidate.insert("providers", providers); QVERIFY(rejected(candidate)); }
    for (const auto field : {"api_key", "context_override", "enabled"}) {
        auto candidate = root; auto providers = candidate.value("providers").toArray(); auto provider = providers.first().toObject();
        auto models = provider.value("models").toArray(); auto model = models.first().toObject();
        model.insert(QLatin1String(field), "invalid-type-or-secret"); models[0] = model; provider.insert("models", models);
        providers[0] = provider; candidate.insert("providers", providers); QVERIFY(rejected(candidate));
    }
    QVERIFY(!ApplicationSettingsTransfer::Decode(QByteArray(ApplicationSettingsTransfer::MaximumBytes + 1, ' '), &error));
    QVERIFY(!ApplicationSettingsTransfer::Decode("[]", &error));
    QVERIFY(!ApplicationSettingsTransfer::Decode("{", &error));
    auto badApplication = application; badApplication.gameRoot += QChar(0);
    QVERIFY(ApplicationSettingsTransfer::Encode(badApplication, settings, &error).isEmpty());
    auto unsafe = settings; unsafe.providers.front().website = QStringLiteral("https://example.invalid/?token=secret");
    QVERIFY(ApplicationSettingsTransfer::Encode(application, unsafe, &error).isEmpty());
}

void AgentSettingsTests::portableProvidersKeepOnlyMatchingLocalCredentials() {
    auto local = Configured(); local.aiEnabled = false; local.allowEdits = true; local.allowSave = true;
    local.allowCommands = false; local.reducedMotion = true; local.mcpEnabled = true; local.mcpPort = 19321;
    local.disabledTools = {QStringLiteral("unavailable-tool")};
    auto imported = local.providers; imported.front().credentialRef = QStringLiteral("untrusted-ref");
    auto merged = ApplicationSettingsTransfer::MergeProviders(local, imported);
    QCOMPARE(merged.providers.front().credentialRef, QStringLiteral("fixture-reference"));
    QVERIFY(!merged.aiEnabled); QVERIFY(merged.allowEdits); QVERIFY(merged.allowSave); QVERIFY(!merged.allowCommands);
    QVERIFY(merged.reducedMotion); QVERIFY(merged.mcpEnabled); QCOMPARE(merged.mcpPort, quint16(19321));
    QCOMPARE(merged.disabledTools, local.disabledTools);
    imported.front().baseUrl += QStringLiteral("/changed");
    QVERIFY(ApplicationSettingsTransfer::MergeProviders(local, imported).providers.front().credentialRef.isEmpty());
    imported = local.providers; imported.front().protocol = QStringLiteral("openai-responses");
    QVERIFY(ApplicationSettingsTransfer::MergeProviders(local, imported).providers.front().credentialRef.isEmpty());
    imported = local.providers; imported.front().id += QStringLiteral("-new");
    QVERIFY(ApplicationSettingsTransfer::MergeProviders(local, imported).providers.front().credentialRef.isEmpty());
}

void AgentSettingsTests::dialogImportStagesUntilHostCommitAndPreservesSecrets() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    CredentialStore credentials(directory.filePath(QStringLiteral("credentials")));
    QString error;
#ifdef Q_OS_WIN
    QVERIFY(credentials.Write(QStringLiteral("fixture-reference"), QStringLiteral("old-local-secret"), &error));
#endif
    auto original = Configured(); original.mcpEnabled = true; original.mcpPort = 19322;
    original.disabledTools = {QStringLiteral("retained-local-tool")};
    const ApplicationPreferences application{QStringLiteral("Q:/OtherComputer/Game"), QStringLiteral("Q:/OtherComputer/UI"),
        QStringLiteral("原创字体未安装"), QStringLiteral("graphite")};
    auto importedSettings = original; importedSettings.providers.front().name = QStringLiteral("导入提供商");
    const auto bytes = ApplicationSettingsTransfer::Encode(application, importedSettings, &error); QVERIFY(!bytes.isEmpty());
    int commits = 0;
    {
        AgentSettingsDialog cancelled(original); cancelled.SetCredentialStore(&credentials);
        cancelled.SetApplicationPreferences({}); cancelled.SetCommitHandler([&](const AgentSettings&, QString*) { ++commits; return true; });
        QVERIFY2(cancelled.ImportSettingsData(bytes, &error), qPrintable(error));
        QCOMPARE(cancelled.ApplicationSettings().gameRoot, application.gameRoot);
        QCOMPARE(cancelled.UiFontFamily(), application.uiFontFamily);
        QCOMPARE(cancelled.Settings().providers.front().name, QStringLiteral("导入提供商"));
        QCOMPARE(cancelled.Settings().providers.front().credentialRef, QStringLiteral("fixture-reference"));
        QCOMPARE(cancelled.Settings().mcpPort, original.mcpPort); QCOMPARE(cancelled.Settings().disabledTools, original.disabledTools);
        const auto* preview = cancelled.findChild<QLabel*>(QStringLiteral("settingsTransferSummary")); QVERIFY(preview);
        QVERIFY(preview->text().contains(QStringLiteral("已暂存导入")));
        QVERIFY(preview->text().contains(QStringLiteral("1 个提供商，1 个模型")));
        cancelled.findChild<QLineEdit*>(QStringLiteral("agentProviderBaseUrl"))->setText(QStringLiteral("https://post-import-change.example.invalid/v1"));
        QVERIFY(cancelled.Settings().providers.front().credentialRef.isEmpty());
        cancelled.reject(); QCOMPARE(commits, 0);
    }
    {
        AgentSettingsDialog clearing(original); clearing.SetCredentialStore(&credentials); clearing.SetApplicationPreferences({});
        auto* clear = clearing.findChild<QPushButton*>(QStringLiteral("agentClearSecret")); QVERIFY(clear); clear->click();
        QVERIFY(!clearing.ImportSettingsData(bytes, &error)); QVERIFY(error.contains(QStringLiteral("清除")));
        clearing.reject();
    }
    AgentSettingsDialog dialog(original); dialog.SetCredentialStore(&credentials); dialog.SetApplicationPreferences({});
    auto* secret = dialog.findChild<QLineEdit*>(QStringLiteral("agentProviderSecret")); QVERIFY(secret);
    secret->setText(QStringLiteral("pending-secret"));
    QVERIFY(!dialog.ImportSettingsData(bytes, &error)); QVERIFY(error.contains(QStringLiteral("尚未保存")));
    QCOMPARE(secret->text(), QStringLiteral("pending-secret"));
    QCOMPARE(dialog.Settings().providers.front().name, original.providers.front().name);
    secret->clear();
    // 未提交的endpoint改动不应把旧credentialRef带入相同的新导入endpoint。
    auto* endpoint = dialog.findChild<QLineEdit*>(QStringLiteral("agentProviderBaseUrl")); QVERIFY(endpoint);
    importedSettings.providers.front().baseUrl = QStringLiteral("https://changed.example.invalid/v1");
    endpoint->setText(importedSettings.providers.front().baseUrl);
    const auto changed = ApplicationSettingsTransfer::Encode(application, importedSettings, &error);
    QVERIFY2(dialog.ImportSettingsData(changed, &error), qPrintable(error));
    QVERIFY(dialog.Settings().providers.front().credentialRef.isEmpty());
    const auto exportBytes = dialog.ExportSettingsData(&error); QVERIFY(!exportBytes.contains("fixture-reference"));
    bool allowCommit = false;
    dialog.SetCommitHandler([&](const AgentSettings& candidate, QString* message) {
        ++commits;
        if (dialog.ApplicationSettings().gameRoot != application.gameRoot || !candidate.providers.front().credentialRef.isEmpty()) return false;
        if (!allowCommit) { *message = QStringLiteral("host-rollback-fixture"); return false; }
        return true;
    });
    dialog.accept(); QCOMPARE(commits, 1); QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("agentSettingsMessage"))->text(), QStringLiteral("host-rollback-fixture"));
    allowCommit = true; dialog.accept(); QCOMPARE(commits, 2); QCOMPARE(dialog.result(), int(QDialog::Accepted));
#ifdef Q_OS_WIN
    QCOMPARE(credentials.Read(QStringLiteral("fixture-reference"), &error), QStringLiteral("old-local-secret"));
#endif
}

QTEST_MAIN(AgentSettingsTests)
#include "AgentSettingsTests.moc"
