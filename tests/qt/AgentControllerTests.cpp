#include "AgentController.h"
#include "AgentSessionStore.h"
#include "CredentialStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>

using namespace War3Map::Agent;

namespace {

const QString kFakeKey = QStringLiteral("controller-fixture-secret-123");

QByteArray Chunk(const QJsonArray& choices) {
    return QByteArray("data: ") + QJsonDocument(QJsonObject{{QStringLiteral("id"), QStringLiteral("fixture-stream")},
        {QStringLiteral("object"), QStringLiteral("chat.completion.chunk")}, {QStringLiteral("created"), 1},
        {QStringLiteral("model"), QStringLiteral("fixture")}, {QStringLiteral("choices"), choices}}).toJson(QJsonDocument::Compact) + "\n\n";
}

QByteArray TextResponse(const QString& text) {
    const QJsonObject first{{QStringLiteral("index"), 0},
        {QStringLiteral("delta"), QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), text}}},
        {QStringLiteral("finish_reason"), QJsonValue{QJsonValue::Null}}};
    const QJsonObject final{{QStringLiteral("index"), 0}, {QStringLiteral("delta"), QJsonObject{}}, {QStringLiteral("finish_reason"), QStringLiteral("stop")}};
    return Chunk({first}) + Chunk({final}) + "data: [DONE]\n\n";
}

QByteArray ToolResponse(const QString& name, const QString& id) {
    const QJsonObject call{{QStringLiteral("index"), 0}, {QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), QStringLiteral("{}")}}}};
    const QJsonObject first{{QStringLiteral("index"), 0}, {QStringLiteral("delta"), QJsonObject{
        {QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("tool_calls"), QJsonArray{call}}}},
        {QStringLiteral("finish_reason"), QJsonValue{QJsonValue::Null}}};
    const QJsonObject final{{QStringLiteral("index"), 0}, {QStringLiteral("delta"), QJsonObject{}}, {QStringLiteral("finish_reason"), QStringLiteral("tool_calls")}};
    return Chunk({first}) + Chunk({final}) + "data: [DONE]\n\n";
}

QJsonObject Tool(const QString& name) {
    return {{QStringLiteral("name"), name}, {QStringLiteral("description"), QStringLiteral("原创测试工具")},
        {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"), QJsonObject{}}, {QStringLiteral("additionalProperties"), false}}}};
}

class SseFixture final : public QObject {
public:
    SseFixture() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer, handled] {
                    buffer->append(socket->readAll());
                    if (*handled) return;
                    const auto boundary = buffer->indexOf("\r\n\r\n");
                    if (boundary < 0) return;
                    const auto header = buffer->left(boundary);
                    qint64 length = -1;
                    for (auto line : header.split('\n')) {
                        line = line.trimmed();
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    }
                    if (length < 0 || buffer->size() - boundary - 4 < length) return;
                    *handled = true; headers.push_back(header);
                    const auto body = QJsonDocument::fromJson(buffer->mid(boundary + 4, length)).object();
                    bodies.push_back(body);
                    const auto bytes = respond ? respond(body, bodies.size()) : TextResponse(QStringLiteral("已完成"));
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                    socket->write(bytes);
                    if (!holdOpen) socket->disconnectFromHost();
                });
            }
        });
    }
    bool Start() { return server.listen(QHostAddress::LocalHost); }
    QString Url() const { return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()); }
    QTcpServer server;
    QList<QJsonObject> bodies;
    QList<QByteArray> headers;
    std::function<QByteArray(const QJsonObject&, qsizetype)> respond;
    bool holdOpen = false;
};

AgentSettings Settings(const QString& url) {
    AgentSettings settings;
    settings.aiEnabled = true; settings.allowEdits = true; settings.allowSave = true;
    ProviderSettings provider;
    provider.id = QStringLiteral("fixture-provider"); provider.name = QStringLiteral("独立回环测试");
    provider.baseUrl = url; provider.credentialRef = QStringLiteral("fixture-credential");
    ModelSettings model; model.id = QStringLiteral("fixture"); provider.models = {model}; settings.providers = {provider};
    return settings;
}

bool HasEvent(const QSignalSpy& events, const QString& type) {
    for (const auto& arguments : events) if (arguments.front().toJsonObject().value(QStringLiteral("type")).toString() == type) return true;
    return false;
}

QJsonObject LastEvent(const QSignalSpy& events, const QString& type) {
    for (auto iterator = events.crbegin(); iterator != events.crend(); ++iterator) {
        const auto event = iterator->front().toJsonObject();
        if (event.value(QStringLiteral("type")).toString() == type) return event;
    }
    return {};
}

QString RunDiagnostic(const QSignalSpy& events, const AgentController& controller, qsizetype requests) {
    // 只报告协议状态，不输出 Authorization、请求正文或模型返回的完整内容。
    QJsonArray summary;
    for (const auto& arguments : events) {
        const auto event = arguments.front().toJsonObject();
        const auto type = event.value(QStringLiteral("type")).toString();
        QJsonObject item{{QStringLiteral("type"), type}};
        if (type == QStringLiteral("error") || type == QStringLiteral("run_end")) {
            auto text = event.value(QStringLiteral("text")).toString();
            text.replace(kFakeKey, QStringLiteral("[fixture credential redacted]"));
            item.insert(QStringLiteral("text"), text.left(512));
            if (type == QStringLiteral("run_end")) {
                item.insert(QStringLiteral("success"), event.value(QStringLiteral("success")));
                item.insert(QStringLiteral("cancelled"), event.value(QStringLiteral("cancelled")));
            }
        } else if (type == QStringLiteral("session_state")) {
            const auto messages = event.value(QStringLiteral("messages")).toArray();
            item.insert(QStringLiteral("message_count"), messages.size());
            QJsonArray shape;
            for (const auto& value : messages) {
                const auto message = value.toObject();
                QJsonObject messageShape{{QStringLiteral("role"), message.value(QStringLiteral("role"))},
                    {QStringLiteral("stopReason"), message.value(QStringLiteral("stopReason"))}};
                if (message.contains(QStringLiteral("toolCallId")))
                    messageShape.insert(QStringLiteral("has_tool_call_id"), !message.value(QStringLiteral("toolCallId")).toString().isEmpty());
                QJsonArray blockTypes;
                for (const auto& block : message.value(QStringLiteral("content")).toArray())
                    blockTypes.append(block.toObject().value(QStringLiteral("type")));
                messageShape.insert(QStringLiteral("blocks"), blockTypes);
                shape.append(messageShape);
                if (shape.size() == 20) break;
            }
            item.insert(QStringLiteral("message_shape"), shape);
        }
        summary.append(item);
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("requests"), requests},
        {QStringLiteral("busy"), controller.IsBusy()}, {QStringLiteral("runtime_pid"), controller.RuntimeProcessId()},
        {QStringLiteral("events"), summary}}).toJson(QJsonDocument::Compact));
}

QByteArray AllFiles(const QString& directory) {
    QByteArray result;
    QDirIterator files(directory, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (file.open(QIODevice::ReadOnly)) result += file.readAll();
    }
    return result;
}

} // namespace

class AgentControllerTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void realWorkerStreamsToolsAndRestoresCompleteSession();
    void stopClosesOnlyModelTaskAndKeepsFinishedToolChanges();
    void stopDuringToolWaitsForResultWithoutKillingTransaction();
    void mapSwitchStopsAndDoesNotReuseOtherMapHistory();
    void changedToolPolicyIsCheckedBeforeEveryDispatch();
    void newConversationPreservesStateUntilExplicitArchive();
    void modelSwitchContinuesSameSessionWithLoopbackOnly();
    void invalidTranscriptDoesNotSendModelRequests();
    void missingRuntimeAndInvalidOutputLeaveDispatcherUntouched();

private:
    void Configure(AgentController& controller, const QString& directory, const QString& url);
    QString node;
    QString worker;
};

void AgentControllerTests::initTestCase() {
    node = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("agent-runtime/node.exe"));
    if (!QFileInfo::exists(node)) node = QStandardPaths::findExecutable(QStringLiteral("node"));
    worker = QFINDTESTDATA("../../tools/agent-runtime/worker.mjs");
    QVERIFY2(!node.isEmpty() && QFileInfo(node).isFile(), "实际 Node 运行时缺失，不能以模拟进程替代。");
    QVERIFY2(!worker.isEmpty() && QFileInfo(worker).isFile(), "实际 PI worker 缺失，不能跳过真实工具循环。");
}

void AgentControllerTests::Configure(AgentController& controller, const QString& directory, const QString& url) {
    const auto credentialDirectory = QDir(directory).filePath(QStringLiteral("credentials"));
    CredentialStore credentials(credentialDirectory);
    QString error;
    QVERIFY2(credentials.Write(QStringLiteral("fixture-credential"), kFakeKey, &error), qPrintable(error));
    controller.SetCredentialDirectory(credentialDirectory);
    controller.SetHistoryDirectory(QDir(directory).filePath(QStringLiteral("history")));
    controller.SetRuntimePaths(node, worker);
    controller.SetSettings(Settings(url));
    controller.SetActiveMap(QStringLiteral("独立测试地图"), QStringLiteral("map-A"));
    controller.SetSystemPrompt(QStringLiteral("调用工具取得真实测试事实，不伪造编辑结果。"));
}

void AgentControllerTests::realWorkerStreamsToolsAndRestoresCompleteSession() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    server.respond = [](const auto&, qsizetype index) {
        return index == 1 ? ToolResponse(QStringLiteral("editor_state"), QStringLiteral("state-call"))
            : TextResponse(QStringLiteral("已回读 revision-7，密钥 controller-fixture-secret-123 不得记录。"));
    };
    int dispatched = 0;
    {
        AgentController controller; Configure(controller, directory.path(), server.Url());
        controller.SetToolsProvider([] { return QJsonArray{Tool(QStringLiteral("editor_state"))}; });
        controller.SetToolDispatcher([&dispatched](QString name, QJsonObject) {
            ++dispatched;
            if (name != QStringLiteral("editor_state")) return QJsonObject{{QStringLiteral("isError"), true}};
            return QJsonObject{{QStringLiteral("revision"), QStringLiteral("revision-7")}, {QStringLiteral("changes"), QJsonObject{
                {QStringLiteral("trigger_nodes"), 2}, {QStringLiteral("items"), QJsonArray{QJsonObject{
                    {QStringLiteral("title"), QStringLiteral("测试触发器")}, {QStringLiteral("kind"), QStringLiteral("trigger")},
                    {QStringLiteral("object_id"), QStringLiteral("trigger-7")}}}}}}};
        });
        QSignalSpy events(&controller, &AgentController::Event);
        controller.StartPrompt(QStringLiteral("任务一：回读地图"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
        QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
        QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
            qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
        QVERIFY(!controller.IsBusy()); QCOMPARE(dispatched, 1);
        QVERIFY(HasEvent(events, QStringLiteral("assistant_delta")));
        QVERIFY(HasEvent(events, QStringLiteral("changes")));
        const auto transcript = LastEvent(events, QStringLiteral("session_state")).value(QStringLiteral("messages")).toArray();
        QVERIFY(!transcript.isEmpty());
        const auto system = transcript.at(0).toObject();
        QCOMPARE(system.value(QStringLiteral("role")).toString(), QStringLiteral("system"));
        QVERIFY(system.value(QStringLiteral("content")).isString());
        const auto declarations = system.value(QStringLiteral("toolsAdded")).toArray();
        QCOMPARE(declarations.size(), qsizetype(1));
        QCOMPARE(declarations.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("editor_state"));
        QVERIFY(declarations.at(0).toObject().value(QStringLiteral("parameters")).isObject());
        QCOMPARE(server.bodies.size(), qsizetype(2));
        const auto secondBody = QJsonDocument(server.bodies[1]).toJson(QJsonDocument::Compact);
        QVERIFY(secondBody.contains("revision-7"));
        QVERIFY(server.headers.front().toLower().contains("authorization: bearer controller-fixture-secret-123"));
        const auto snippet = controller.CurrentFeedbackConversationSnippet();
        QVERIFY(snippet.contains(QStringLiteral("任务一：回读地图")));
        QVERIFY(snippet.contains(QStringLiteral("已回读 revision-7")));
        QVERIFY(!snippet.contains(QStringLiteral("toolsAdded")));
        QVERIFY(!snippet.contains(QStringLiteral("trigger_nodes")));
        QVERIFY(!snippet.contains(kFakeKey));
        QVERIFY(snippet.toUtf8().size() <= 16384);
        const auto shortSnippet = controller.CurrentFeedbackConversationSnippet(37);
        QVERIFY(shortSnippet.toUtf8().size() <= 37);
        QVERIFY(!shortSnippet.contains(QChar::ReplacementCharacter));
        QVERIFY(controller.CurrentFeedbackConversationSnippet(16385).isEmpty());
        for (const auto& event : events) QVERIFY(!QJsonDocument(event.front().toJsonObject()).toJson().contains(kFakeKey.toUtf8()));
    }
    const auto recorded = AllFiles(directory.filePath(QStringLiteral("history")));
    QVERIFY(!recorded.isEmpty()); QVERIFY(!recorded.contains(kFakeKey.toUtf8()));
    const AgentSessionStore sessions(directory.filePath(QStringLiteral("history")));
    const auto saved = sessions.List(QStringLiteral("map-A"));
    QCOMPARE(saved.size(), qsizetype(1));
    QFile savedFile(sessions.SessionPath(QStringLiteral("map-A"), saved.front().id));
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    const auto savedState = QJsonDocument::fromJson(savedFile.readAll()).object();
    savedFile.close();
    QCOMPARE(savedState.value(QStringLiteral("version")).toInt(), 2);
    QCOMPARE(savedState.value(QStringLiteral("session_id")).toString(), saved.front().id);
    QCOMPARE(QFileInfo(savedFile).absolutePath(), directory.filePath(QStringLiteral("history/sessions/") + AgentSessionStore::MapHash(QStringLiteral("map-A"))));
    QVERIFY(savedState.value(QStringLiteral("messages")).toArray().first().toObject().value(QStringLiteral("forge_hidden")).toBool());
    {
        AgentController restored;
        QSignalSpy events(&restored, &AgentController::Event);
        Configure(restored, directory.path(), server.Url());
        restored.StartPrompt(QStringLiteral("任务二：延续上一轮"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
        QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
        QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
            qPrintable(RunDiagnostic(events, restored, server.bodies.size())));
        QCOMPARE(server.bodies.size(), qsizetype(3));
        const auto resumed = QJsonDocument(server.bodies.back()).toJson(QJsonDocument::Compact);
        QVERIFY(resumed.contains(QStringLiteral("任务一").toUtf8()));
        QVERIFY(resumed.contains("revision-7"));
        QVERIFY(!resumed.contains(kFakeKey.toUtf8()));
        QVERIFY(HasEvent(events, QStringLiteral("history_restored")));
        const auto restoredState = LastEvent(events, QStringLiteral("history_restored"));
        QCOMPARE(restoredState.value(QStringLiteral("messages")).toArray().at(0).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
        int restoredUserMessages = 0;
        for (const auto& value : restoredState.value(QStringLiteral("ui_events")).toArray())
            if (value.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("user_message")) ++restoredUserMessages;
        QCOMPARE(restoredUserMessages, 1); // 系统提示不伪装成用户发言。
        bool removedDeclaration = false;
        for (const auto& value : LastEvent(events, QStringLiteral("session_state")).value(QStringLiteral("messages")).toArray()) {
            const auto message = value.toObject();
            if (message.value(QStringLiteral("role")).toString() != QStringLiteral("system")) continue;
            for (const auto& removed : message.value(QStringLiteral("toolsRemoved")).toArray())
                if (removed.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("editor_state")) removedDeclaration = true;
        }
        QVERIFY(removedDeclaration); // 继续任务的工具集合变化也能完整持久化。
        QVERIFY(!LastEvent(events, QStringLiteral("history_restored")).value(QStringLiteral("ui_events")).toArray().isEmpty());
    }
}

void AgentControllerTests::stopClosesOnlyModelTaskAndKeepsFinishedToolChanges() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    server.respond = [&server](const auto&, qsizetype index) {
        server.holdOpen = index > 1;
        return index == 1 ? ToolResponse(QStringLiteral("editor_edit"), QStringLiteral("edit-call"))
            : Chunk({QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), QStringLiteral("仍在流式输出")}}},
                {QStringLiteral("finish_reason"), QJsonValue{QJsonValue::Null}}}});
    };
    AgentController controller; Configure(controller, directory.path(), server.Url());
    controller.SetToolsProvider([] { return QJsonArray{Tool(QStringLiteral("editor_edit"))}; });
    int edits = 0;
    controller.SetToolDispatcher([&edits](QString, QJsonObject) { ++edits; return QJsonObject{{QStringLiteral("revision"), QStringLiteral("edited-1")}}; });
    QSignalSpy events(&controller, &AgentController::Event);
    controller.StartPrompt(QStringLiteral("实际工具后停止"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_COMPARE_WITH_TIMEOUT(server.bodies.size(), qsizetype(2), 15000);
    QCOMPARE(edits, 1); QVERIFY(controller.IsBusy());
    controller.Stop();
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 10000);
    QVERIFY(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("cancelled")).toBool());
    QVERIFY(!controller.IsBusy()); QCOMPARE(edits, 1);
    QVERIFY(AllFiles(directory.filePath(QStringLiteral("history/events"))).contains("edited-1"));
}

void AgentControllerTests::mapSwitchStopsAndDoesNotReuseOtherMapHistory() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    server.respond = [](const auto&, qsizetype) { return TextResponse(QStringLiteral("地图独立会话")); };
    AgentController controller; Configure(controller, directory.path(), server.Url());
    QSignalSpy events(&controller, &AgentController::Event);
    controller.StartPrompt(QStringLiteral("仅属于地图 A 的旧任务"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
        qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
    controller.SetActiveMap(QStringLiteral("测试地图 B"), QStringLiteral("map-B")); events.clear();
    controller.StartPrompt(QStringLiteral("地图 B 新任务"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QCOMPARE(server.bodies.size(), qsizetype(2));
    QVERIFY(!QJsonDocument(server.bodies.back()).toJson().contains(QStringLiteral("地图 A 的旧任务").toUtf8()));
    server.holdOpen = true;
    server.respond = [](const auto&, qsizetype) { return Chunk({QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("delta"),
        QJsonObject{{QStringLiteral("content"), QStringLiteral("进行中")}}}, {QStringLiteral("finish_reason"), QJsonValue{QJsonValue::Null}}}}); };
    events.clear(); controller.StartPrompt(QStringLiteral("地图 B 长任务"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_COMPARE_WITH_TIMEOUT(server.bodies.size(), qsizetype(3), 15000);
    controller.SetActiveMap(QStringLiteral("测试地图 C"), QStringLiteral("map-C"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 10000);
    QVERIFY(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("cancelled")).toBool());
}

void AgentControllerTests::stopDuringToolWaitsForResultWithoutKillingTransaction() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    server.respond = [](const auto&, qsizetype index) {
        return index == 1 ? ToolResponse(QStringLiteral("editor_save"), QStringLiteral("save-call")) : TextResponse(QStringLiteral("保存后停止"));
    };
    AgentController controller; Configure(controller, directory.path(), server.Url());
    controller.SetToolsProvider([] { return QJsonArray{Tool(QStringLiteral("editor_save"))}; });
    bool finished = false;
    controller.SetToolDispatcher([&](QString, QJsonObject) {
        controller.Stop(); // 模拟原子事务期间用户提出停止。
        if (controller.RuntimeProcessId() <= 0 || !controller.IsBusy()) return QJsonObject{{QStringLiteral("isError"), true}};
        finished = true;
        return QJsonObject{{QStringLiteral("saved"), true}, {QStringLiteral("identity"), QStringLiteral("committed-save-1")}};
    });
    QSignalSpy events(&controller, &AgentController::Event);
    controller.StartPrompt(QStringLiteral("完成保存后停止"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QVERIFY(finished); QVERIFY(!controller.IsBusy());
    QVERIFY(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("cancelled")).toBool());
    QVERIFY(AllFiles(directory.filePath(QStringLiteral("history/events"))).contains("committed-save-1"));
}

void AgentControllerTests::changedToolPolicyIsCheckedBeforeEveryDispatch() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    server.respond = [](const auto&, qsizetype index) {
        if (index == 1) return ToolResponse(QStringLiteral("editor_state"), QStringLiteral("query-call"));
        if (index == 2) return ToolResponse(QStringLiteral("editor_edit"), QStringLiteral("denied-edit-call"));
        return TextResponse(QStringLiteral("已收到关闭工具的结果"));
    };
    AgentController controller; Configure(controller, directory.path(), server.Url());
    controller.SetToolsProvider([] { return QJsonArray{Tool(QStringLiteral("editor_state")), Tool(QStringLiteral("editor_edit"))}; });
    int queries = 0, edits = 0;
    controller.SetToolDispatcher([&](QString name, QJsonObject) {
        if (name == QStringLiteral("editor_state")) {
            ++queries;
            auto changed = Settings(server.Url()); changed.disabledTools = {QStringLiteral("editor_edit")};
            controller.SetSettings(changed);
            return QJsonObject{{QStringLiteral("revision"), QStringLiteral("checked-1")}};
        }
        ++edits; return QJsonObject{};
    });
    QSignalSpy events(&controller, &AgentController::Event);
    controller.StartPrompt(QStringLiteral("先查后改，期间关闭修改工具"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QCOMPARE(queries, 1); QCOMPARE(edits, 0); QCOMPARE(server.bodies.size(), qsizetype(3));
    QVERIFY(QJsonDocument(server.bodies.back()).toJson().contains("AGENT_TOOL_DISABLED"));
}

void AgentControllerTests::missingRuntimeAndInvalidOutputLeaveDispatcherUntouched() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    AgentController controller; Configure(controller, directory.path(), QStringLiteral("http://127.0.0.1:1/v1"));
    int dispatched = 0;
    controller.SetToolDispatcher([&dispatched](QString, QJsonObject) { ++dispatched; return QJsonObject{}; });
    controller.SetRuntimePaths(directory.filePath(QStringLiteral("missing-node.exe")), worker);
    QSignalSpy events(&controller, &AgentController::Event);
    controller.StartPrompt(QStringLiteral("缺运行时"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QVERIFY(HasEvent(events, QStringLiteral("error"))); QVERIFY(!controller.IsBusy()); QCOMPARE(dispatched, 0);
    QFile malformed(directory.filePath(QStringLiteral("malformed.mjs")));
    QVERIFY(malformed.open(QIODevice::WriteOnly));
    const QByteArray program("process.stdout.write('not-json\\n'); setTimeout(() => {}, 2000);\n");
    QCOMPARE(malformed.write(program), qint64(program.size())); malformed.close();
    controller.SetRuntimePaths(node, malformed.fileName()); events.clear();
    controller.StartPrompt(QStringLiteral("坏 IPC"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 10000);
    QVERIFY(!LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool());
    QVERIFY(!controller.IsBusy()); QCOMPARE(dispatched, 0);
}

void AgentControllerTests::newConversationPreservesStateUntilExplicitArchive() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    AgentController controller; Configure(controller, directory.path(), server.Url());
    QSignalSpy events(&controller, &AgentController::Event);
    controller.StartPrompt(QStringLiteral("旧会话任务"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
        qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
    const auto previousAudit = QDir(directory.filePath(QStringLiteral("history/events"))).entryList(QDir::Files);
    QCOMPARE(previousAudit.size(), qsizetype(1));
    const AgentSessionStore store(directory.filePath(QStringLiteral("history")));
    const auto previousSessions = store.List(QStringLiteral("map-A"));
    QCOMPARE(previousSessions.size(), qsizetype(1));
    const auto previousId = previousSessions.front().id;
    QFile previousFile(store.SessionPath(QStringLiteral("map-A"), previousId));
    QVERIFY(previousFile.open(QIODevice::ReadOnly));
    const auto previousBytes = previousFile.readAll(); previousFile.close();
    controller.NewConversation(); QVERIFY(HasEvent(events, QStringLiteral("conversation_reset")));
    QCOMPARE(store.List(QStringLiteral("map-A")).size(), qsizetype(2));
    const auto archivedPath = directory.filePath(QStringLiteral("history/archives/") + AgentSessionStore::MapHash(QStringLiteral("map-A")));
    QVERIFY(QDir(archivedPath).entryList(QDir::Files).isEmpty());
    events.clear();
    controller.StartPrompt(QStringLiteral("新会话任务"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
        qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
    QCOMPARE(server.bodies.size(), qsizetype(2));
    QVERIFY(!QJsonDocument(server.bodies.back()).toJson().contains(QStringLiteral("旧会话任务").toUtf8()));
    QVERIFY(!HasEvent(events, QStringLiteral("history_restored")));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("history/events/") + previousAudit.front())));
    QCOMPARE(store.List(QStringLiteral("map-A")).size(), qsizetype(2));
    QVERIFY(previousFile.open(QIODevice::ReadOnly)); QCOMPARE(previousFile.readAll(), previousBytes); previousFile.close();
    events.clear();
    controller.SwitchConversation(previousId);
    QVERIFY(HasEvent(events, QStringLiteral("history_restored")));
    const auto restored = QJsonDocument(LastEvent(events, QStringLiteral("history_restored"))).toJson();
    QVERIFY(restored.contains(QStringLiteral("旧会话任务").toUtf8()));
    QVERIFY(!restored.contains(QStringLiteral("新会话任务").toUtf8()));
    QCOMPARE(server.bodies.size(), qsizetype(2));
    QVERIFY(!controller.IsBusy());
    controller.ArchiveConversation(previousId);
    QCOMPARE(store.List(QStringLiteral("map-A")).size(), qsizetype(1));
    QVERIFY(!QFileInfo::exists(previousFile.fileName()));
    const auto archivedFiles = QDir(archivedPath).entryList(QDir::Files);
    QCOMPARE(archivedFiles.size(), qsizetype(1));
    QFile archived(QDir(archivedPath).filePath(archivedFiles.front()));
    QVERIFY(archived.open(QIODevice::ReadOnly)); QCOMPARE(archived.readAll(), previousBytes);
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("history/events/") + previousAudit.front())));
    QCOMPARE(server.bodies.size(), qsizetype(2));
}

void AgentControllerTests::modelSwitchContinuesSameSessionWithLoopbackOnly() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    // 真实 PI worker 仅访问本机原创 SSE 夹具，不使用外部站点或作者凭据。
    SseFixture server; QVERIFY(server.Start());
    const AgentSessionStore store(directory.filePath(QStringLiteral("history")));
    QString sessionId;
    {
        AgentController controller; Configure(controller, directory.path(), server.Url());
        auto settings = Settings(server.Url());
        ModelSettings secondModel; secondModel.id = QStringLiteral("fixture-second");
        settings.providers[0].models.append(secondModel);
        controller.SetSettings(settings);
        QSignalSpy events(&controller, &AgentController::Event);
        controller.StartPrompt(QStringLiteral("模型甲任务：保留这条上下文"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
        QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
        QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
            qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
        const auto first = store.List(QStringLiteral("map-A"));
        QCOMPARE(first.size(), qsizetype(1)); sessionId = first.front().id;
        events.clear();
        controller.StartPrompt(QStringLiteral("模型乙任务：继续同一会话"), QStringLiteral("fixture-provider"), QStringLiteral("fixture-second"));
        QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
        QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
            qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
        QCOMPARE(server.bodies.size(), qsizetype(2));
        QCOMPARE(server.bodies.back().value(QStringLiteral("model")).toString(), QStringLiteral("fixture-second"));
        const auto request = QJsonDocument(server.bodies.back()).toJson();
        QVERIFY(request.contains(QStringLiteral("模型甲任务").toUtf8()));
        QVERIFY(request.contains(QStringLiteral("模型乙任务").toUtf8()));
        const auto second = store.List(QStringLiteral("map-A"));
        QCOMPARE(second.size(), qsizetype(1)); QCOMPARE(second.front().id, sessionId);
        bool firstModelRecorded = false, secondModelRecorded = false;
        for (const auto& value : second.front().messages) {
            const auto message = value.toObject();
            const auto text = AgentMessageText(message);
            if (text.contains(QStringLiteral("模型甲任务"))) {
                QCOMPARE(message.value(QStringLiteral("forge_model_id")).toString(), QStringLiteral("fixture"));
                firstModelRecorded = true;
            }
            if (text.contains(QStringLiteral("模型乙任务"))) {
                QCOMPARE(message.value(QStringLiteral("forge_model_id")).toString(), QStringLiteral("fixture-second"));
                secondModelRecorded = true;
            }
        }
        QVERIFY(firstModelRecorded); QVERIFY(secondModelRecorded);
    }
    AgentController restored;
    QSignalSpy events(&restored, &AgentController::Event);
    Configure(restored, directory.path(), server.Url());
    auto settings = Settings(server.Url());
    ModelSettings secondModel; secondModel.id = QStringLiteral("fixture-second");
    settings.providers[0].models.append(secondModel); restored.SetSettings(settings);
    QVERIFY(HasEvent(events, QStringLiteral("history_restored")));
    restored.StartPrompt(QStringLiteral("重启宿主后继续模型乙"), QStringLiteral("fixture-provider"), QStringLiteral("fixture-second"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
        qPrintable(RunDiagnostic(events, restored, server.bodies.size())));
    QCOMPARE(server.bodies.size(), qsizetype(3));
    const auto request = QJsonDocument(server.bodies.back()).toJson();
    QVERIFY(request.contains(QStringLiteral("模型甲任务").toUtf8()));
    QVERIFY(request.contains(QStringLiteral("模型乙任务").toUtf8()));
    const auto sessions = store.List(QStringLiteral("map-A"));
    QCOMPARE(sessions.size(), qsizetype(1)); QCOMPARE(sessions.front().id, sessionId);
}

void AgentControllerTests::invalidTranscriptDoesNotSendModelRequests() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    {
        AgentController controller; Configure(controller, directory.path(), server.Url());
        QSignalSpy events(&controller, &AgentController::Event);
        controller.StartPrompt(QStringLiteral("用于校验完整上下文"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
        QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
        QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
            qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
    }
    const QDir sessions(directory.filePath(QStringLiteral("history/sessions/") + AgentSessionStore::MapHash(QStringLiteral("map-A"))));
    const auto files = sessions.entryList(QDir::Files);
    QCOMPARE(files.size(), qsizetype(1));
    QFile file(sessions.filePath(files.front()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto original = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    QCOMPARE(original.value(QStringLiteral("version")).toInt(), 2);
    const auto messages = original.value(QStringLiteral("messages")).toArray();
    QVERIFY(!messages.isEmpty());
    for (int malformed = 0; malformed < 4; ++malformed) {
        auto altered = messages;
        if (malformed < 2) {
            auto system = altered.at(0).toObject();
            if (malformed == 0) system.insert(QStringLiteral("content"), 17);
            else system.insert(QStringLiteral("toolsAdded"), QJsonArray{QJsonObject{
                {QStringLiteral("name"), QStringLiteral("editor_state")}, {QStringLiteral("description"), QStringLiteral("坏声明")},
                {QStringLiteral("parameters"), QStringLiteral("not-a-schema")}}});
            altered[0] = system;
        } else if (malformed == 2) altered.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("stopReason"), QStringLiteral("toolUse")}, {QStringLiteral("content"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("toolCall")}, {QStringLiteral("id"), QStringLiteral("unpaired-call")},
                {QStringLiteral("name"), QStringLiteral("editor_state")}, {QStringLiteral("arguments"), QJsonObject{}}}}}});
        else altered.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("unrecognized")}, {QStringLiteral("content"), QStringLiteral("未知角色")}});
        auto state = original; state.insert(QStringLiteral("messages"), altered);
        const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
        AgentController controller; Configure(controller, directory.path(), server.Url());
        QSignalSpy events(&controller, &AgentController::Event);
        controller.StartPrompt(QStringLiteral("拒绝损坏会话"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
        QVERIFY(HasEvent(events, QStringLiteral("error"))); QVERIFY(!controller.IsBusy());
        QCOMPARE(controller.RuntimeProcessId(), qint64(0)); QCOMPARE(server.bodies.size(), qsizetype(1));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes); file.close();
    }
}

QTEST_GUILESS_MAIN(AgentControllerTests)
#include "AgentControllerTests.moc"
