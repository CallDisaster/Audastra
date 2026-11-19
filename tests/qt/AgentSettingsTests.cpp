#include "AgentSettingsDialog.h"
#include "AgentSettingsStore.h"
#include "ApplicationSettingsTransfer.h"
#include "CredentialStore.h"
#include "ExternalMcpManager.h"
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
#include <QProcess>
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
    void externalMcpSettingsAndJsonImportIsolateSecrets();
    void externalMcpImportKeepsSecretNamesAndRefusesUnsafeEntries();
    void externalMcpValidationRejectsPlaintextAndLoadSkipsUnsafeEntries();
    void externalMcpArgsSurviveUnrelatedDialogEdits();
};

void AgentSettingsTests::settingsRoundtripAndCredentialIsolation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AgentSettingsStore store(directory.filePath(QStringLiteral("设置/agent.json")));
    auto settings = Configured();
    settings.mcpEnabled = true;
    settings.mcpPort = 19876;
    settings.disabledTools = {QStringLiteral("save_map_edit_candidate")};
    settings.followChanges = true;
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
    QVERIFY(result.followChanges);
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

void AgentSettingsTests::externalMcpSettingsAndJsonImportIsolateSecrets() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString mcpJson = QStringLiteral(R"({
        "mcpServers": {
            "war3-autotest": {
                "command": "node",
                "args": ["autotest-mcp.mjs", "--headless"],
                "env": {
                    "MODE": "ci",
                    "WAR3_API_KEY": "extracted-stdio-secret-111"
                }
            },
            "local-http-mcp": {
                "url": "http://127.0.0.1:18900/mcp",
                "headers": {
                    "Authorization": "Bearer extracted-http-secret-222"
                }
            }
        }
    })");
    QHash<QString, QString> extracted;
    QString error;
    auto parsed = ExternalMcpManager::ParseMcpServersJson(mcpJson, &extracted, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(parsed.size(), qsizetype(2));
    QCOMPARE(extracted.size(), qsizetype(2));

    AgentSettings settings = Configured();
    settings.maxAutoSteps = 192;
    settings.externalMcpServers = parsed;
    settings.externalMcpServers[0].minimumPermission = QStringLiteral("read");
    settings.externalMcpServers[0].credentialRef = QStringLiteral("forge/external-mcp/ref-1");
    settings.externalMcpServers[1].minimumPermission = QStringLiteral("full");
    settings.externalMcpServers[1].credentialRef = QStringLiteral("forge/external-mcp/ref-2");

    AgentSettingsStore store(directory.filePath(QStringLiteral("mcp-settings.json")));
    QVERIFY2(store.Save(settings, &error), qPrintable(error));
    const QByteArray raw = Read(store.FilePath());
    QVERIFY(!raw.contains("extracted-stdio-secret-111"));
    QVERIFY(!raw.contains("extracted-http-secret-222"));

    const auto loaded = store.Load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.maxAutoSteps, 192);
    QCOMPARE(loaded.externalMcpServers.size(), qsizetype(2));

    AgentSettings perms;
    QVERIFY(ExternalMcpPermissionSatisfied(QStringLiteral("read"), perms));
    QVERIFY(!ExternalMcpPermissionSatisfied(QStringLiteral("edit"), perms));
    perms.allowEdits = true;
    QVERIFY(ExternalMcpPermissionSatisfied(QStringLiteral("edit"), perms));
    QVERIFY(!ExternalMcpPermissionSatisfied(QStringLiteral("save"), perms));
    perms.allowSave = true;
    QVERIFY(ExternalMcpPermissionSatisfied(QStringLiteral("save"), perms));
    QVERIFY(!ExternalMcpPermissionSatisfied(QStringLiteral("full"), perms));
    perms.allowCommands = true;
    QVERIFY(ExternalMcpPermissionSatisfied(QStringLiteral("full"), perms));

    // 禁止在 env 中直接塞入明文 token 字段
    auto unsafe = loaded;
    unsafe.externalMcpServers[0].env.push_back({QStringLiteral("token"), QStringLiteral("bad")});
    QVERIFY(!store.Save(unsafe, &error));
    QVERIFY(!error.isEmpty());

    // 思考强度获取（内置目录 / 接口获取 / 未识别手动指定）与持久化校验
    ModelSettings dsFlash;
    dsFlash.id = QStringLiteral("custom-ds-route");
    dsFlash.name = QStringLiteral("DeepSeek V4.1 Flash Ultrafast");
    const auto dsCap = ResolveThinkingCapability(dsFlash, QStringLiteral("openai-chat"), QStringLiteral("https://gateway.invalid/v1"));
    QVERIFY(dsCap.discovered);
    QCOMPARE(dsCap.levels, (QStringList{QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")}));
    QCOMPARE(dsCap.defaultLevel, QStringLiteral("high"));

    ModelSettings unknownModel;
    unknownModel.id = QStringLiteral("private-relay-alpha");
    unknownModel.name = QStringLiteral("Private Relay Alpha");
    const auto unknownCap = ResolveThinkingCapability(unknownModel, QStringLiteral("openai-chat"), QStringLiteral("https://gateway.invalid/v1"));
    QVERIFY(!unknownCap.discovered);
    QVERIFY(unknownCap.levels.isEmpty());

    unknownModel.thinkingLevelsCustomized = true;
    unknownModel.customThinkingLevels = {QStringLiteral("xhigh"), QStringLiteral("off"), QStringLiteral("low")};
    unknownModel.selectedThinkingLevel = QStringLiteral("xhigh");
    const auto customCap = ResolveThinkingCapability(unknownModel, QStringLiteral("openai-chat"), QStringLiteral("https://gateway.invalid/v1"));
    QVERIFY(customCap.discovered);
    QCOMPARE(customCap.levels, (QStringList{QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("xhigh")}));
    QCOMPARE(EffectiveThinkingLevel(unknownModel), QStringLiteral("xhigh"));

    // 网关 /v1/models 字典格式与 reasoning_efforts 自动提取
    LocalModelServer effortServer;
    QVERIFY(effortServer.Start());
    effortServer.responder = [](const auto&) {
        return Http(R"({"models":{"gw-reasoning-1":{"displayName":"Gateway Reasoning 1","limit":{"context":512000,"output":64000},"reasoning_efforts":["off","low","high","max"]}}})");
    };
    ProviderCatalog catalog;
    QList<ModelSettings> fetchedModels;
    connect(&catalog, &ProviderCatalog::ModelsReady, this, [&fetchedModels](const QString&, const QList<ModelSettings>& list) { fetchedModels = list; });
    QSignalSpy readySpy(&catalog, &ProviderCatalog::ModelsReady);
    catalog.Fetch(Provider(effortServer.Url()), QStringLiteral("fixture-key"));
    QTRY_COMPARE(readySpy.size(), qsizetype(1));
    QCOMPARE(fetchedModels.size(), qsizetype(1));
    QCOMPARE(fetchedModels.front().id, QStringLiteral("gw-reasoning-1"));
    QCOMPARE(fetchedModels.front().providerContext, qint64(512000));
    QCOMPARE(fetchedModels.front().maxOutputTokens, qint64(64000));
    QCOMPARE(fetchedModels.front().providerThinkingLevels,
        (QStringList{QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")}));

    settings.providers.front().models = {unknownModel, fetchedModels.front()};
    QVERIFY2(store.Save(settings, &error), qPrintable(error));
    const auto reloaded = store.Load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(reloaded.providers.front().models.size(), qsizetype(2));
    QVERIFY(reloaded.providers.front().models[0].thinkingLevelsCustomized);
    QCOMPARE(reloaded.providers.front().models[0].customThinkingLevels,
        (QStringList{QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("xhigh")}));
    QCOMPARE(reloaded.providers.front().models[0].selectedThinkingLevel, QStringLiteral("xhigh"));
    QCOMPARE(reloaded.providers.front().models[1].providerThinkingLevels,
        (QStringList{QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")}));
}

void AgentSettingsTests::externalMcpImportKeepsSecretNamesAndRefusesUnsafeEntries() {
    const QString json = QStringLiteral(R"({
        "mcpServers": {
            "github": {"command": "npx", "args": ["-y", "@modelcontextprotocol/server-github"],
                "env": {"GITHUB_PERSONAL_ACCESS_TOKEN": "fixture-github-pat-value"}},
            "database": {"command": "node", "args": ["db.mjs"],
                "env": {"DB_HOST": "localhost", "DB_PASSWORD": "fixture-db-password"}},
            "two-secrets": {"command": "node",
                "env": {"AWS_SECRET_ACCESS_KEY": "fixture-aws-secret", "AWS_SESSION_TOKEN": "fixture-aws-session"}},
            "arg-inline": {"command": "node", "args": ["server.js", "--token=fixture-inline-token"]},
            "arg-split": {"command": "node", "args": ["server.js", "--api-key", "fixture-split-key"]},
            "url-query": {"url": "https://mcp.example.invalid/mcp?api_key=fixture-query-key"},
            "custom-header": {"url": "https://mcp.example.invalid/mcp", "headers": {"X-API-Key": "fixture-header-key"}},
            "basic-auth": {"url": "https://mcp.example.invalid/mcp", "headers": {"Authorization": "Basic Zml4dHVyZTpiYXNpYw=="}},
            "bearer-auth": {"url": "https://mcp.example.invalid/mcp", "headers": {"Authorization": "Bearer fixture-bearer-token"}}
        }
    })");
    QHash<QString, QString> secrets;
    QString error;
    const auto parsed = ExternalMcpManager::ParseMcpServersJson(json, &secrets, &error);
    QCOMPARE(parsed.size(), qsizetype(4));
    // 无法安全转换的条目逐个拒绝并说明原因，错误文本不回显密钥值。
    for (const auto* refused : {"two-secrets", "arg-inline", "arg-split", "url-query", "custom-header"})
        QVERIFY2(error.contains(QLatin1String(refused)), qPrintable(error));
    for (const auto* value : {"fixture-aws-secret", "fixture-aws-session", "fixture-inline-token", "fixture-split-key",
             "fixture-query-key", "fixture-header-key"})
        QVERIFY2(!error.contains(QLatin1String(value)), qPrintable(error));

    QHash<QString, ExternalMcpServerSettings> byName;
    for (const auto& server : parsed) {
        QVERIFY(!server.enabled); // 导入后默认停用
        byName.insert(server.name, server);
    }
    // 密钥环境变量保留原名，值转入凭据；非密钥变量照常保留。
    const auto github = byName.value(QStringLiteral("github"));
    QCOMPARE(github.credentialEnv, QStringLiteral("GITHUB_PERSONAL_ACCESS_TOKEN"));
    QVERIFY(github.env.isEmpty());
    QCOMPARE(secrets.value(github.id), QStringLiteral("fixture-github-pat-value"));
    const auto database = byName.value(QStringLiteral("database"));
    QCOMPARE(database.credentialEnv, QStringLiteral("DB_PASSWORD"));
    QVERIFY(database.env == (QList<QPair<QString, QString>>{{QStringLiteral("DB_HOST"), QStringLiteral("localhost")}}));
    QCOMPARE(secrets.value(database.id), QStringLiteral("fixture-db-password"));
    // 非 Bearer 的 Authorization 保留完整值原样发送；Bearer 只存令牌。
    const auto basic = byName.value(QStringLiteral("basic-auth"));
    QVERIFY(basic.authorizationRaw);
    QCOMPARE(secrets.value(basic.id), QStringLiteral("Basic Zml4dHVyZTpiYXNpYw=="));
    const auto bearer = byName.value(QStringLiteral("bearer-auth"));
    QVERIFY(!bearer.authorizationRaw);
    QCOMPARE(secrets.value(bearer.id), QStringLiteral("fixture-bearer-token"));

    QTemporaryDir directory; QVERIFY(directory.isValid());
    AgentSettingsStore store(directory.filePath(QStringLiteral("mcp-import.json")));
    AgentSettings settings = Configured();
    settings.externalMcpServers = parsed;
    for (auto& server : settings.externalMcpServers) server.credentialRef = QStringLiteral("forge/external-mcp/") + server.id;
    QVERIFY2(store.Save(settings, &error), qPrintable(error));
    const QByteArray raw = Read(store.FilePath());
    for (const auto* value : {"fixture-github-pat-value", "fixture-db-password", "Zml4dHVyZTpiYXNpYw==", "fixture-bearer-token"})
        QVERIFY(!raw.contains(value));
    QString warning;
    const auto loaded = store.Load(&error, &warning);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(warning.isEmpty(), qPrintable(warning));
    QCOMPARE(loaded.externalMcpServers.size(), qsizetype(4));
    for (const auto& server : loaded.externalMcpServers) {
        const auto original = byName.value(server.name);
        QCOMPARE(server.credentialEnv, original.credentialEnv);
        QCOMPARE(server.authorizationRaw, original.authorizationRaw);
        QVERIFY(!server.enabled);
    }
}

void AgentSettingsTests::externalMcpValidationRejectsPlaintextAndLoadSkipsUnsafeEntries() {
    ExternalMcpServerSettings base;
    base.id = QStringLiteral("fixture-mcp");
    base.name = QStringLiteral("原创测试 MCP");
    base.transport = QStringLiteral("stdio");
    base.command = QStringLiteral("node");
    base.args = {QStringLiteral("server.mjs")};
    AgentSettings settings = Configured();
    settings.externalMcpServers = {base};
    QString error;
    QVERIFY2(AgentSettingsStore::Validate(settings, &error), qPrintable(error));
    const auto rejected = [&settings](const std::function<void(ExternalMcpServerSettings&)>& mutate) {
        auto candidate = settings;
        mutate(candidate.externalMcpServers.front());
        QString message;
        return !AgentSettingsStore::Validate(candidate, &message) && !message.isEmpty();
    };
    using Server = ExternalMcpServerSettings;
    // 地址规则与提供商地址一致：不能带查询参数或用户信息。
    QVERIFY(rejected([](Server& s) { s.transport = QStringLiteral("http"); s.url = QStringLiteral("https://mcp.example.invalid/mcp?api_key=fixture"); }));
    QVERIFY(rejected([](Server& s) { s.transport = QStringLiteral("http"); s.url = QStringLiteral("https://user:pw@mcp.example.invalid/mcp"); }));
    // 参数与环境变量中的明文密钥。
    QVERIFY(rejected([](Server& s) { s.args.push_back(QStringLiteral("--password=fixture-pass")); }));
    QVERIFY(rejected([](Server& s) { s.args.append(QStringList{QStringLiteral("--token"), QStringLiteral("fixture-token")}); }));
    QVERIFY(rejected([](Server& s) { s.args.push_back(QStringLiteral("Authorization: Bearer fixture-token-value")); }));
    QVERIFY(rejected([](Server& s) { s.env.push_back({QStringLiteral("DB_PASSWORD"), QStringLiteral("fixture")}); }));
    QVERIFY(rejected([](Server& s) { s.env.push_back({QStringLiteral("GITHUB_PAT"), QStringLiteral("fixture")}); }));
    QVERIFY(rejected([](Server& s) { s.env.push_back({QStringLiteral("OPTIONS"), QStringLiteral("sk-synthetic-fixture-value-0000")}); }));
    // 非 http 协议的连接串同样检查用户信息中的密码。
    QVERIFY(rejected([](Server& s) { s.args.push_back(QStringLiteral("postgresql://fixture:fixture-pass@db.example.invalid/maps")); }));
    // 与读取时相同的单值长度限制。
    QVERIFY(rejected([](Server& s) { s.args.push_back(QString(4097, QLatin1Char('a'))); }));
    QVERIFY(rejected([](Server& s) { s.env.push_back({QStringLiteral("MODE"), QString(4097, QLatin1Char('a'))}); }));
    QVERIFY(rejected([](Server& s) { s.credentialEnv = QStringLiteral("1BAD"); }));
    QVERIFY(rejected([](Server& s) { s.env.push_back({QStringLiteral("MY_SLOT"), QStringLiteral("x")}); s.credentialEnv = QStringLiteral("MY_SLOT"); }));
    // 文件路径、占位符与普通变量不是明文密钥。
    auto allowed = settings;
    allowed.externalMcpServers.front().args += QStringList{QStringLiteral("--key-file"), QStringLiteral("C:/certs/client.key"), // Synthetic
        QStringLiteral("--token"), QStringLiteral("${MCP_TOKEN}")};
    allowed.externalMcpServers.front().env.push_back({QStringLiteral("MODE"), QStringLiteral("ci")});
    allowed.externalMcpServers.front().credentialEnv = QStringLiteral("GITHUB_TOKEN");
    // 普通数量参数与指向文件的凭据路径不是明文密钥。
    allowed.externalMcpServers.front().args += QStringList{QStringLiteral("--max-tokens"), QStringLiteral("4096"),
        QStringLiteral("postgresql://db.example.invalid/maps")};
    allowed.externalMcpServers.front().env.push_back({QStringLiteral("MAX_TOKENS"), QStringLiteral("4096")});
    allowed.externalMcpServers.front().env.push_back({QStringLiteral("GOOGLE_APPLICATION_CREDENTIALS"), QStringLiteral("C:/keys/service.json")}); // Synthetic
    QVERIFY2(AgentSettingsStore::Validate(allowed, &error), qPrintable(error));

    // 超长参数在保存时即被拒绝，不会写出一份下次读取失败的设置文件。
    QTemporaryDir directory; QVERIFY(directory.isValid());
    AgentSettingsStore store(directory.filePath(QStringLiteral("agent.json")));
    QVERIFY2(store.Save(settings, &error), qPrintable(error));
    const QByteArray previous = Read(store.FilePath());
    auto tooLong = settings;
    tooLong.externalMcpServers.front().args.push_back(QString(4097, QLatin1Char('a')));
    QVERIFY(!store.Save(tooLong, &error));
    QCOMPARE(Read(store.FilePath()), previous);

    // 早期写入的不安全条目：逐条跳过并给出警告，其余设置照常读取，警告不回显明文。
    QJsonObject root = QJsonDocument::fromJson(previous).object();
    QJsonArray servers = root.value(QStringLiteral("external_mcp_servers")).toArray();
    QJsonObject unsafe = servers.first().toObject();
    unsafe.insert(QStringLiteral("id"), QStringLiteral("fixture-unsafe"));
    unsafe.insert(QStringLiteral("name"), QStringLiteral("早期条目"));
    unsafe.insert(QStringLiteral("env"), QJsonObject{{QStringLiteral("DB_PASSWORD"), QStringLiteral("fixture-plaintext")}});
    servers.append(unsafe);
    root.insert(QStringLiteral("external_mcp_servers"), servers);
    QVERIFY(Write(store.FilePath(), QJsonDocument(root).toJson()));
    QString warning;
    const auto tolerant = store.Load(&error, &warning);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(warning.contains(QStringLiteral("早期条目")));
    QVERIFY(!warning.contains(QStringLiteral("fixture-plaintext")));
    QCOMPARE(tolerant.externalMcpServers.size(), qsizetype(1));
    QCOMPARE(tolerant.externalMcpServers.front().id, base.id);
    QCOMPARE(tolerant.providers.size(), qsizetype(1));
    QVERIFY(tolerant.aiEnabled);
}

void AgentSettingsTests::externalMcpArgsSurviveUnrelatedDialogEdits() {
    auto initial = Configured();
    ExternalMcpServerSettings server;
    server.id = QStringLiteral("fixture-args");
    server.name = QStringLiteral("参数测试");
    server.transport = QStringLiteral("stdio");
    server.command = QStringLiteral("node");
    server.args = {QStringLiteral(R"({"a":1})"), QString{}, QStringLiteral("with space"),
        QStringLiteral(R"(say "hi")"), QStringLiteral("plain")};
    initial.externalMcpServers = {server};
    AgentSettingsDialog dialog(initial);
    auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("agentExternalMcpName"));
    auto* args = dialog.findChild<QLineEdit*>(QStringLiteral("agentExternalMcpArgs"));
    QVERIFY(name != nullptr);
    QVERIFY(args != nullptr);
    // 改其他字段不重新解析参数：JSON 参数与空参数原样保留。
    name->setText(QStringLiteral("参数测试改名"));
    QCOMPARE(dialog.Settings().externalMcpServers.front().name, QStringLiteral("参数测试改名"));
    QCOMPARE(dialog.Settings().externalMcpServers.front().args, server.args);
    // 参数栏显示的文本再次解析后与原参数一致。
    const QString shown = args->text();
    args->clear();
    args->setText(shown);
    QCOMPARE(dialog.Settings().externalMcpServers.front().args, server.args);
    // 引号规则与 QProcess::splitCommand 一致（splitCommand 不表示空参数）。
    QStringList nonEmpty = server.args;
    nonEmpty.removeAll(QString{});
    QCOMPARE(QProcess::splitCommand(shown), nonEmpty);
    dialog.reject();
}

QTEST_MAIN(AgentSettingsTests)
#include "AgentSettingsTests.moc"
