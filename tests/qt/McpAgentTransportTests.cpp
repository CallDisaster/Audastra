#include "McpServer.h"
#include "McpTools.h"
#include "McpResources.h"
#include "McpHttpTransport.h"
#include "McpTriggerGraphJson.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <filesystem>

using namespace War3Map::Mcp;

namespace {

const QByteArray kToken(48, 'x');

QByteArray Rpc(const QString& method, QJsonObject parameters = {}, int id = 1) {
    return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method},
        {"params", parameters}}).toJson(QJsonDocument::Compact);
}

QJsonArray ListTools(McpServer& server) {
    const auto reply = server.HandleMessage(Rpc(QStringLiteral("tools/list")));
    return reply ? reply->result.toObject().value("tools").toArray() : QJsonArray{};
}

bool HasTool(const QJsonArray& tools, const QString& name) {
    for (const auto& tool : tools) if (tool.toObject().value("name").toString() == name) return true;
    return false;
}

ToolDefinition DummyTool(const QString& name, ToolPermission permission, int& calls) {
    ToolDefinition tool;
    tool.name = name;
    tool.permission = permission;
    tool.inputSchema = {{"type", "object"}, {"additionalProperties", false}};
    tool.handler = [&calls](const QJsonObject&) -> QJsonValue { ++calls; return QJsonObject{{"ok", true}}; };
    return tool;
}

QByteArray RawExchange(quint16 port, const QByteArray& request, int timeoutMs = 2000) {
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QByteArray received;
    QObject::connect(&socket, &QTcpSocket::connected, &loop, [&] { socket.write(request); });
    QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&] { received += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    socket.connectToHost(QHostAddress::LocalHost, port);
    loop.exec();
    received += socket.readAll();
    return received;
}

QByteArray HttpRequest(quint16 port, const QByteArray& body, const QByteArray& extraHeaders = {},
    const QByteArray& method = "POST", const QByteArray& token = kToken) {
    QByteArray request = method + " /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n";
    request += "Authorization: Bearer " + token + "\r\n";
    request += "Accept: application/json, text/event-stream\r\nContent-Type: application/json\r\n";
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    request += extraHeaders + "\r\n" + body;
    return request;
}

QByteArray ResponseBody(const QByteArray& response) {
    const auto boundary = response.indexOf("\r\n\r\n");
    return boundary < 0 ? QByteArray{} : response.mid(boundary + 4);
}

bool Write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QString ResourceText(McpServer& server, const QString& uri) {
    const auto response = server.HandleMessage(Rpc(QStringLiteral("resources/read"), {{"uri", uri}}));
    if (!response || response->error) return {};
    return response->result.toObject().value("contents").toArray()[0].toObject().value("text").toString();
}

} // namespace

class McpAgentTransportTests final : public QObject {
    Q_OBJECT
private slots:
    void CachedToolCannotBypassPolicy();
    void ExistingWriteToolsHaveExplicitPermissions();
    void ProtocolAndNotificationsReflectRealChannel();
    void HostHandlerKeepsSchemaAndPolicy();
    void SkillResourcesOwnVersionedBytes();
    void GraphDecoderKeepsStrictDtoBoundary();
    void HttpPostsRequestsAndAcceptsNotifications();
    void HttpValidatesTokenOriginAndProtocol();
    void HttpHasBudgetsAndStopsCleanly();
};

void McpAgentTransportTests::CachedToolCannotBypassPolicy() {
    McpServer server;
    int queryCalls = 0, editCalls = 0, saveCalls = 0, externalCalls = 0;
    server.RegisterTool(DummyTool("query", ToolPermission::Query, queryCalls));
    server.RegisterTool(DummyTool("edit", ToolPermission::Edit, editCalls));
    server.RegisterTool(DummyTool("save", ToolPermission::Save, saveCalls));
    server.RegisterTool(DummyTool("external", ToolPermission::External, externalCalls));
    QCOMPARE(ListTools(server).size(), 4);
    ToolPolicy policy;
    policy.allowEdit = false;
    policy.allowSave = false;
    policy.allowExternal = false;
    server.SetToolPolicy(policy);
    QCOMPARE(ListTools(server).size(), 1);
    for (const auto* name : {"edit", "save", "external"}) {
        const auto response = server.HandleMessage(Rpc("tools/call", {{"name", name}, {"arguments", QJsonObject{}}}));
        QVERIFY(response && response->error);
        QCOMPARE(response->error->value("data").toObject().value("code").toString(), QStringLiteral("MCP_TOOL_DISABLED"));
    }
    QCOMPARE(editCalls + saveCalls + externalCalls, 0);
    policy.disabledTools.insert("query");
    server.SetToolPolicy(policy);
    QVERIFY(ListTools(server).isEmpty());
    QVERIFY(server.HandleMessage(Rpc("tools/call", {{"name", "query"}, {"arguments", QJsonObject{}}}))->error);
    QCOMPARE(queryCalls, 0);
    policy.disabledTools.clear();
    server.SetToolPolicy(policy);
    QVERIFY(!server.HandleMessage(Rpc("tools/call", {{"name", "query"}, {"arguments", QJsonObject{}}}))->error);
    QCOMPARE(queryCalls, 1);
}

void McpAgentTransportTests::ExistingWriteToolsHaveExplicitPermissions() {
    McpServer server;
    RegisterAllTools(server);
    ToolPolicy policy;
    policy.allowEdit = false;
    policy.allowSave = false;
    server.SetToolPolicy(policy);
    const auto tools = ListTools(server);
    QVERIFY(HasTool(tools, "query_trigger_graph"));
    QVERIFY(HasTool(tools, "get_revision"));
    for (const auto* name : {"begin_edit", "create_trigger", "append_root_eca", "append_argument_call",
        "set_scalar_argument", "edit_trigger_graph", "rollback_edit", "install_generated_warvk_module",
        "save_map_edit_candidate", "export_map_candidate", "save_terrain_blight_candidate", "create_map_candidate"})
        QVERIFY2(!HasTool(tools, QString::fromLatin1(name)), name);
}

void McpAgentTransportTests::ProtocolAndNotificationsReflectRealChannel() {
    McpServer server;
    int calls = 0;
    server.RegisterTool(DummyTool("edit", ToolPermission::Edit, calls));
    auto initialized = server.HandleMessage(Rpc("initialize"));
    QCOMPARE(initialized->result.toObject().value("protocolVersion").toString(), QStringLiteral("2024-11-05"));
    QVERIFY(!initialized->result.toObject().value("capabilities").toObject().value("tools").toObject().value("listChanged").toBool());
    for (const auto* version : {"2025-03-26", "2025-06-18", "2025-11-25"}) {
        initialized = server.HandleMessage(Rpc("initialize", {{"protocolVersion", version}}));
        QCOMPARE(initialized->result.toObject().value("protocolVersion").toString(), QString::fromLatin1(version));
    }
    initialized = server.HandleMessage(Rpc("initialize", {{"protocolVersion", "2099-01-01"}}));
    QCOMPARE(initialized->result.toObject().value("protocolVersion").toString(), QStringLiteral("2025-11-25"));
    QList<QByteArray> notifications;
    server.SetNotificationSink([&](const QByteArray& notification) { notifications.append(notification); });
    initialized = server.HandleMessage(Rpc("initialize", {{"protocolVersion", "2025-11-25"}}));
    QVERIFY(initialized->result.toObject().value("capabilities").toObject().value("tools").toObject().value("listChanged").toBool());
    QVERIFY(!server.HandleMessage(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    auto policy = server.Policy();
    policy.allowEdit = false;
    server.SetToolPolicy(policy);
    QCOMPARE(notifications.size(), 1);
    QCOMPARE(QJsonDocument::fromJson(notifications[0]).object().value("method").toString(), QStringLiteral("notifications/tools/list_changed"));
    server.SetToolPolicy(policy);
    QCOMPARE(notifications.size(), 1);
    server.SetNotificationSink({});
}

void McpAgentTransportTests::HostHandlerKeepsSchemaAndPolicy() {
    McpServer server;
    int oldCalls = 0, newCalls = 0;
    auto tool = DummyTool("edit", ToolPermission::Edit, oldCalls);
    tool.inputSchema = {{"type", "object"}, {"properties", QJsonObject{{"value", QJsonObject{{"type", "integer"}}}}},
        {"required", QJsonArray{"value"}}, {"additionalProperties", false}};
    server.RegisterTool(std::move(tool));
    QVERIFY(server.SetToolHandler("edit", [&](const QJsonObject&) -> QJsonValue { ++newCalls; return QJsonObject{{"ok", true}}; }));
    QVERIFY(!server.SetToolHandler("missing", [](const QJsonObject&) -> QJsonValue { return {}; }));
    QVERIFY(server.HandleMessage(Rpc("tools/call", {{"name", "edit"}, {"arguments", QJsonObject{{"value", "bad"}}}}))->error);
    QVERIFY(!server.HandleMessage(Rpc("tools/call", {{"name", "edit"}, {"arguments", QJsonObject{{"value", 3}}}}))->error);
    QCOMPARE(oldCalls, 0);
    QCOMPARE(newCalls, 1);
    auto policy = server.Policy(); policy.allowEdit = false; server.SetToolPolicy(policy);
    QVERIFY(server.HandleMessage(Rpc("tools/call", {{"name", "edit"}, {"arguments", QJsonObject{{"value", 3}}}}))->error);
    QCOMPARE(newCalls, 1);
}

void McpAgentTransportTests::SkillResourcesOwnVersionedBytes() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath("forge-editor-mcp/references"));
    const auto entryPath = root.path() + "/forge-editor-mcp/SKILL.md";
    const QByteArray entry = QStringLiteral("---\nname: forge-editor-mcp\ndescription: 地图工具\n---\n# 自然语言 T\n").toUtf8();
    QVERIFY(Write(entryPath, entry));
    QVERIFY(Write(root.path() + "/forge-editor-mcp/references/contract.md", "real contract\n"));
    McpServer server;
    RegisterAllResources(server, std::filesystem::path(root.path().toStdWString()));
    const auto manifest = QJsonDocument::fromJson(ResourceText(server, "forge://skills/manifest").toUtf8()).object();
    QCOMPARE(manifest.value("schema_version").toInt(), 1);
    QCOMPARE(manifest.value("skills").toArray().size(), 1);
    const auto skill = manifest.value("skills").toArray()[0].toObject();
    QCOMPARE(skill.value("files").toArray().size(), 2);
    QCOMPARE(skill.value("version").toString().size(), 64);
    const auto uri = skill.value("entry_uri").toString();
    QCOMPARE(ResourceText(server, uri).toUtf8(), entry);
    QVERIFY(Write(entryPath, entry + "updated\n"));
    QCOMPARE(ResourceText(server, uri).toUtf8(), entry);
    RegisterAllResources(server, std::filesystem::path(root.path().toStdWString()));
    const auto nextManifest = QJsonDocument::fromJson(ResourceText(server, "forge://skills/manifest").toUtf8()).object();
    const auto nextSkill = nextManifest.value("skills").toArray()[0].toObject();
    QVERIFY(nextSkill.value("version") != skill.value("version"));
    QVERIFY(server.HandleMessage(Rpc("resources/read", {{"uri", uri}}))->error);
    QVERIFY(server.HandleMessage(Rpc("resources/read", {{"uri", nextSkill.value("entry_uri").toString() + "/../other"}}))->error);
    QVERIFY(server.HandleMessage(Rpc("resources/read", {{"uri", "war3map://triggers"}}))->error);
}

void McpAgentTransportTests::GraphDecoderKeepsStrictDtoBoundary() {
    const auto schema = GraphOperationSchema();
    QVERIFY(!schema.value("properties").toObject().contains("transaction_id"));
    QVERIFY(schema.value("properties").toObject().contains("expected"));
    QVERIFY(schema.value("$defs").toObject().contains("eca"));
    const auto rejected = DecodeGraphBatch({{"expected", QJsonObject{}}, {"operations", QJsonArray{}}});
    QVERIFY(!rejected.Succeeded());
    QVERIFY(!DecodeGraphBatch({{"expected", QJsonObject{}}, {"operations", QJsonArray{}}, {"transaction_id", "1"}}).Succeeded());
    QVERIFY(!ParseRequest(R"({"jsonrpc":"2.0","id":true,"method":"ping"})"));
    QVERIFY(!ParseRequest(R"({"jsonrpc":"2.0","method":"ping","params":[]})"));
}

void McpAgentTransportTests::HttpPostsRequestsAndAcceptsNotifications() {
    McpServer server;
    McpHttpTransport transport(server);
    McpHttpOptions options; options.port = 0; options.bearerToken = kToken;
    QString error;
    QVERIFY2(transport.Start(options, &error), qPrintable(error));
    const auto response = RawExchange(transport.Port(), HttpRequest(transport.Port(),
        Rpc("initialize", {{"protocolVersion", "2025-11-25"}})));
    QVERIFY(response.startsWith("HTTP/1.1 200"));
    const auto result = QJsonDocument::fromJson(ResponseBody(response)).object().value("result").toObject();
    QCOMPARE(result.value("protocolVersion").toString(), QStringLiteral("2025-11-25"));
    QVERIFY(!result.value("capabilities").toObject().value("tools").toObject().value("listChanged").toBool());
    const auto notification = RawExchange(transport.Port(), HttpRequest(transport.Port(),
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    QVERIFY(notification.startsWith("HTTP/1.1 202"));
    QVERIFY(ResponseBody(notification).isEmpty());
    const auto get = RawExchange(transport.Port(), HttpRequest(transport.Port(), {}, {}, "GET"));
    QVERIFY(get.startsWith("HTTP/1.1 405"));
    QVERIFY(get.contains("Allow: POST\r\n"));
}

void McpAgentTransportTests::HttpValidatesTokenOriginAndProtocol() {
    McpServer server;
    int calls = 0; server.RegisterTool(DummyTool("query", ToolPermission::Query, calls));
    McpHttpTransport transport(server);
    McpHttpOptions options; options.port = 0; options.bearerToken = kToken;
    QVERIFY(transport.Start(options));
    const auto body = Rpc("tools/call", {{"name", "query"}, {"arguments", QJsonObject{}}});
    auto response = RawExchange(transport.Port(), HttpRequest(transport.Port(), body, {}, "POST", QByteArray(48, 'y')));
    QVERIFY(response.startsWith("HTTP/1.1 401"));
    response = RawExchange(transport.Port(), HttpRequest(transport.Port(), body, "Origin: https://evil.invalid\r\n"));
    QVERIFY(response.startsWith("HTTP/1.1 403"));
    response = RawExchange(transport.Port(), HttpRequest(transport.Port(), body, "MCP-Protocol-Version: 2099-01-01\r\n"));
    QVERIFY(response.startsWith("HTTP/1.1 400"));
    QCOMPARE(calls, 0);
    const auto origin = "Origin: http://127.0.0.1:" + QByteArray::number(transport.Port()) + "\r\n";
    response = RawExchange(transport.Port(), HttpRequest(transport.Port(), body, origin));
    QVERIFY(response.startsWith("HTTP/1.1 200"));
    QCOMPARE(calls, 1);
    auto request = HttpRequest(transport.Port(), body);
    request.replace("application/json, text/event-stream", "application/json");
    response = RawExchange(transport.Port(), request);
    QVERIFY(response.startsWith("HTTP/1.1 406"));
    QCOMPARE(calls, 1);
    const auto malformed = RawExchange(transport.Port(), HttpRequest(transport.Port(), R"({"jsonrpc":"2.0","method":"ping","params":[]})"));
    QVERIFY(malformed.startsWith("HTTP/1.1 400"));
}

void McpAgentTransportTests::HttpHasBudgetsAndStopsCleanly() {
    McpServer server;
    McpHttpTransport transport(server);
    McpHttpOptions options; options.port = 0; options.bearerToken = kToken;
    options.maxBodyBytes = 1024; options.receiveTimeoutMs = 100;
    QVERIFY(transport.Start(options));
    const auto port = transport.Port();
    McpServer otherServer;
    McpHttpTransport other(otherServer);
    auto conflicting = options; conflicting.port = port;
    QString error;
    QVERIFY(!other.Start(conflicting, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(transport.IsListening());
    const auto oversized = RawExchange(port, HttpRequest(port, QByteArray(1025, 'x')));
    QVERIFY(oversized.startsWith("HTTP/1.1 413"));
    const auto incomplete = RawExchange(port, "POST /mcp HTTP/1.1\r\n", 1000);
    QVERIFY(incomplete.startsWith("HTTP/1.1 408"));
    const auto duplicate = RawExchange(port, HttpRequest(port, Rpc("ping"), "Content-Length: 0\r\n"));
    QVERIFY(duplicate.startsWith("HTTP/1.1 400"));
    transport.Stop();
    QVERIFY(!transport.IsListening());
    QCOMPARE(transport.Port(), quint16(0));
    QVERIFY(transport.EndpointUrl().isEmpty());
    options.bearerToken = "short";
    QVERIFY(!transport.Start(options, &error));
    QVERIFY(!error.contains("short"));
}

QTEST_GUILESS_MAIN(McpAgentTransportTests)
#include "McpAgentTransportTests.moc"
