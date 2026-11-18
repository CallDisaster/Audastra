#include "AgentController.h"
#include "AgentSessionStore.h"
#include "AgentSettingsStore.h"
#include "CredentialStore.h"
#include "ExternalMcpManager.h"
#include "ProjectMemoryService.h"
#include "TokenMeter.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <Windows.h>

#include <cstdio>
#include <functional>
#include <memory>

using namespace War3Map::Agent;

namespace {

const QString kFakeKey = QStringLiteral("controller-fixture-secret-123");
bool portableRuntimeChild = false;

class QuietProcessErrors final {
public:
    QuietProcessErrors() : previous_(GetErrorMode()) {
        SetErrorMode(previous_ | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    ~QuietProcessErrors() { SetErrorMode(previous_); }
private:
    DWORD previous_;
};

class PortableChildJob final {
public:
    PortableChildJob() {
        name = QStringLiteral("Local\\Audastra-PortableAgent-%1-%2")
            .arg(GetCurrentProcessId()).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        handle_ = CreateJobObjectW(nullptr, reinterpret_cast<LPCWSTR>(name.utf16()));
        const auto createdError = GetLastError();
        if (!handle_ || createdError == ERROR_ALREADY_EXISTS) {
            error = QStringLiteral("Cannot create a unique portable child Job: %1").arg(createdError);
            if (handle_) CloseHandle(handle_);
            handle_ = nullptr;
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            error = QStringLiteral("Cannot configure portable child Job: %1").arg(GetLastError());
            CloseHandle(handle_); handle_ = nullptr;
        }
    }
    ~PortableChildJob() {
        if (handle_) { TerminateJobObject(handle_, 125); CloseHandle(handle_); }
    }
    bool Stop() { return handle_ && TerminateJobObject(handle_, 125); }
    bool WaitEmpty(int timeout) {
        QElapsedTimer elapsed; elapsed.start();
        do {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(handle_, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
                return false;
            if (accounting.ActiveProcesses == 0) return true;
            QTest::qWait(10);
        } while (elapsed.elapsed() < timeout);
        return false;
    }
    QString name, error;
private:
    HANDLE handle_ = nullptr;
};

bool JoinPortableChildJob(QString& error) {
    const auto name = qEnvironmentVariable("AUDASTRA_TEST_PORTABLE_JOB");
    if (name.isEmpty()) { error = QStringLiteral("Portable child Job is missing"); return false; }
    const auto job = OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
    if (!job) { error = QStringLiteral("Open portable child Job failed: %1").arg(GetLastError()); return false; }
    const bool joined = AssignProcessToJobObject(job, GetCurrentProcess()) != FALSE;
    const auto joinedError = GetLastError();
    // 父进程是唯一长期持有者；子进程不能使 kill-on-close 在父进程退出后失效。
    CloseHandle(job);
    qunsetenv("AUDASTRA_TEST_PORTABLE_JOB");
    if (!joined) error = QStringLiteral("Join portable child Job failed: %1").arg(joinedError);
    return joined;
}

bool CopyPortableFile(const QString& source, const QString& target, QString& error) {
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) { error = source + QStringLiteral(": ") + file.errorString(); return false; }
    QCryptographicHash original(QCryptographicHash::Sha256);
    if (!original.addData(&file)) { error = QStringLiteral("Cannot hash portable source: ") + source; return false; }
    if (!QFile::copy(source, target)) { error = QStringLiteral("Cannot copy portable file: ") + source; return false; }
    QFile copied(target);
    QCryptographicHash actual(QCryptographicHash::Sha256);
    if (!copied.open(QIODevice::ReadOnly) || !actual.addData(&copied) || actual.result() != original.result()) {
        error = QStringLiteral("Portable copied bytes differ: ") + target;
        return false;
    }
    return true;
}

QByteArray ProcessDiagnostic(const QProcess& process) {
    return QByteArray("exit=") + QByteArray::number(process.exitCode()) + " (0x"
        + QByteArray::number(static_cast<quint32>(process.exitCode()), 16) + ") status="
        + QByteArray::number(process.exitStatus()) + " state=" + QByteArray::number(process.state())
        + " error=" + process.errorString().toUtf8();
}

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
    void portableRuntimeUsesApplicationDirectoryWithoutOverrides();
    void stopClosesOnlyModelTaskAndKeepsFinishedToolChanges();
    void stopDuringToolWaitsForResultWithoutKillingTransaction();
    void mapSwitchStopsAndDoesNotReuseOtherMapHistory();
    void changedToolPolicyIsCheckedBeforeEveryDispatch();
    void newConversationPreservesStateUntilExplicitArchive();
    void modelSwitchContinuesSameSessionWithLoopbackOnly();
    void invalidTranscriptDoesNotSendModelRequests();
    void missingRuntimeAndInvalidOutputLeaveDispatcherUntouched();
    void failedRunAndCompactionNeverDropLongUserPrompt();
    void projectMemoryAndExternalMcpEndToEnd();

private:
    void Configure(AgentController& controller, const QString& directory, const QString& url);
    QString node;
    QString worker;
};

void AgentControllerTests::initTestCase() {
    node = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("agent-runtime/node.exe"));
    if (!QFileInfo::exists(node)) node = QStandardPaths::findExecutable(QStringLiteral("node"));
    worker = portableRuntimeChild
        ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("agent-runtime/worker.bundle.mjs"))
        : QFINDTESTDATA("../../tools/agent-runtime/worker.mjs");
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
    if (!portableRuntimeChild) controller.SetRuntimePaths(node, worker);
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
        if (portableRuntimeChild) {
            const auto* runtime = controller.findChild<QProcess*>();
            QVERIFY(runtime != nullptr);
            QCOMPARE(QFileInfo(runtime->program()).canonicalFilePath(), QFileInfo(node).canonicalFilePath());
            QCOMPARE(runtime->arguments().size(), 1);
            QCOMPARE(QFileInfo(runtime->arguments().front()).canonicalFilePath(), QFileInfo(worker).canonicalFilePath());
        }
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

void AgentControllerTests::portableRuntimeUsesApplicationDirectoryWithoutOverrides() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QuietProcessErrors quietErrors; // 子进程继承，冷加载失败也不能弹出系统模态框。
    PortableChildJob ownedJob;
    QVERIFY2(ownedJob.error.isEmpty(), qPrintable(ownedJob.error));
    const auto bin = directory.filePath(QStringLiteral("bin"));
    const auto deployed = QDir(bin).filePath(QStringLiteral("agent-runtime"));
    QVERIFY(QDir().mkpath(deployed));
    const auto childExecutable = QDir(bin).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName());
    QString copyError;
    QVERIFY2(CopyPortableFile(QCoreApplication::applicationFilePath(), childExecutable, copyError), qPrintable(copyError));
    QVERIFY2(CopyPortableFile(node, QDir(deployed).filePath(QStringLiteral("node.exe")), copyError), qPrintable(copyError));
    // 该测试宿主的 Qt/VC 导入依赖也须真正 app-local 部署。Svg 与 atomic_wait
    // 是宿主的直接导入；仅复制 Core/Test 或依赖父进程 PATH 都不能证明冷启动。
    const QStringList requiredDependencies{QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Network.dll"),
        QStringLiteral("Qt6Test.dll"), QStringLiteral("msvcp140.dll"), QStringLiteral("msvcp140_1.dll"),
        QStringLiteral("msvcp140_2.dll"), QStringLiteral("msvcp140_atomic_wait.dll"),
        QStringLiteral("vcruntime140.dll"), QStringLiteral("vcruntime140_1.dll")};
    for (const auto& dependency : requiredDependencies)
        QVERIFY2(CopyPortableFile(QDir(QCoreApplication::applicationDirPath()).filePath(dependency),
            QDir(bin).filePath(dependency), copyError), qPrintable(copyError));

    // 使用当前锁定依赖生成真实便携 worker，不复制 node_modules、不访问模型服务。
    const auto buildScript = QFINDTESTDATA("../../tools/agent-runtime/build.mjs");
    QVERIFY(!buildScript.isEmpty());
    QProcess bundle;
    bundle.setProgram(node);
    bundle.setArguments({buildScript, deployed});
    bundle.setWorkingDirectory(QFileInfo(buildScript).absolutePath());
    bundle.start();
    const bool bundleStarted = bundle.waitForStarted();
    const bool bundleFinished = bundleStarted && bundle.waitForFinished(15000);
    if (!bundleFinished && bundle.state() != QProcess::NotRunning) { bundle.kill(); bundle.waitForFinished(5000); }
    const auto bundleDiagnostic = ProcessDiagnostic(bundle) + "\n" + bundle.readAllStandardOutput() + bundle.readAllStandardError();
    QVERIFY2(bundleStarted && bundleFinished && bundle.exitStatus() == QProcess::NormalExit && bundle.exitCode() == 0,
        bundleDiagnostic.constData());
    QVERIFY(QFileInfo::exists(QDir(deployed).filePath(QStringLiteral("worker.bundle.mjs"))));
    QVERIFY(QFileInfo::exists(QDir(deployed).filePath(QStringLiteral("runtime-manifest.json"))));

    // 子进程的 applicationDir 是独立便携目录。生产控制器不注入运行时路径，
    // 仍执行原有完整回环工具/会话恢复测试，并核实际 QProcess 的程序与参数。
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("NODE_OPTIONS"));
    environment.remove(QStringLiteral("NODE_PATH"));
    environment.remove(QStringLiteral("QT_PLUGIN_PATH"));
    environment.remove(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"));
    const auto windows = environment.value(QStringLiteral("SystemRoot"));
    QVERIFY2(!windows.isEmpty(), "SystemRoot is required for the isolated portable process");
    environment.insert(QStringLiteral("PATH"), bin + QDir::listSeparator() + QDir(windows).filePath(QStringLiteral("System32"))
        + QDir::listSeparator() + windows);
    environment.insert(QStringLiteral("AUDASTRA_TEST_PORTABLE_JOB"), ownedJob.name);
    child.setProcessEnvironment(environment);
    child.setProgram(childExecutable);
    child.setWorkingDirectory(bin);
    const auto childLog = directory.filePath(QStringLiteral("portable-child.qtest.txt"));
    child.setArguments({QStringLiteral("--portable-agent-child"),
        QStringLiteral("realWorkerStreamsToolsAndRestoresCompleteSession"),
        QStringLiteral("-o"), childLog + QStringLiteral(",txt")});
    child.start();
    const bool started = child.waitForStarted();
    const bool finished = started && child.waitForFinished(25000);
    if (!finished) {
        ownedJob.Stop();
        if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(5000); }
    }
    const bool emptyAfterExit = ownedJob.WaitEmpty(1000);
    if (!emptyAfterExit) ownedJob.Stop();
    const bool cleaned = emptyAfterExit || ownedJob.WaitEmpty(5000);
    auto output = child.readAllStandardOutput() + child.readAllStandardError();
    QFile childReport(childLog);
    if (childReport.open(QIODevice::ReadOnly)) output += childReport.readAll();
    else output += "\nchild QTest log unavailable: " + childReport.errorString().toUtf8();
    const auto diagnostic = ProcessDiagnostic(child) + " started=" + QByteArray::number(started)
        + " finished=" + QByteArray::number(finished) + " empty_after_exit=" + QByteArray::number(emptyAfterExit)
        + " cleanup=" + QByteArray::number(cleaned) + "\n" + output;
    QVERIFY2(started && finished, diagnostic.constData());
    QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, diagnostic.constData());
    QVERIFY2(emptyAfterExit && cleaned, diagnostic.constData());
    QVERIFY2(output.contains("0 failed"), diagnostic.constData());
    qInfo().noquote() << "Portable child cold start: app-local DLLs; restricted PATH; default runtime; Job empty\n" << output;
}

void AgentControllerTests::failedRunAndCompactionNeverDropLongUserPrompt() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    SseFixture server; QVERIFY(server.Start());
    const QString longPrompt = QStringLiteral("长串核心需求：英雄每升一级全属性+5，三件铁剑自动合成玄铁剑，Boss血量低于30%开启狂暴触发器，绝不能丢失这段原始要求！");

    // 模拟第一轮执行完一次工具调用后中途运行失败（如服务端报错或中断），第二轮用户发“继续做完”，随后执行手动压缩“整理会话”。
    server.respond = [](const auto&, qsizetype index) -> QByteArray {
        if (index == 1) return ToolResponse(QStringLiteral("editor_state"), QStringLiteral("step-1"));
        if (index == 2) {
            const QJsonObject errChunk{{QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("simulated mid-run failure")}}}};
            return QByteArray("data: ") + QJsonDocument(errChunk).toJson(QJsonDocument::Compact) + "\n\ndata: [DONE]\n\n";
        }
        if (index == 3) return TextResponse(QStringLiteral("已按您的长串要求继续完成触发器创建。"));
        return TextResponse(QStringLiteral("<compacted-summary>\n## 原始需求与核心约束\n- 英雄每升一级全属性+5，三件铁剑自动合成玄铁剑，Boss血量低于30%开启狂暴触发器。\n</compacted-summary>"));
    };

    AgentController controller; Configure(controller, directory.path(), server.Url());
    controller.SetToolsProvider([] { return QJsonArray{Tool(QStringLiteral("editor_state"))}; });
    controller.SetToolDispatcher([](QString, QJsonObject) {
        return QJsonObject{{QStringLiteral("revision"), QStringLiteral("rev-mid-1")}};
    });
    QSignalSpy events(&controller, &AgentController::Event);

    // 1. 第一轮中途失败：核验磁盘已通过提交即落盘 + 回合检查点保存了长提示词与已完成工具步！
    controller.StartPrompt(longPrompt, QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QVERIFY(!LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool());

    const AgentSessionStore store(directory.filePath(QStringLiteral("history")));
    const auto afterFailure = store.List(QStringLiteral("map-A"));
    QCOMPARE(afterFailure.size(), qsizetype(1));
    QVERIFY(QJsonDocument(afterFailure.front().messages).toJson().contains(QStringLiteral("三件铁剑自动合成玄铁剑").toUtf8()));
    QVERIFY(QJsonDocument(afterFailure.front().messages).toJson().contains("rev-mid-1"));

    // 2. 第二轮发送“继续做完”：核验发给模型的上下文依然完整携带第一轮长需求，绝不被覆盖清空！
    events.clear();
    controller.StartPrompt(QStringLiteral("继续做完"), QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("run_end")), 15000);
    QVERIFY2(LastEvent(events, QStringLiteral("run_end")).value(QStringLiteral("success")).toBool(),
        qPrintable(RunDiagnostic(events, controller, server.bodies.size())));
    const QByteArray resumeRequest = QJsonDocument(server.bodies.back()).toJson(QJsonDocument::Compact);
    QVERIFY(resumeRequest.contains(QStringLiteral("三件铁剑自动合成玄铁剑").toUtf8()));
    QVERIFY(resumeRequest.contains(QStringLiteral("继续做完").toUtf8()));

    // 3. 手动压缩“整理会话”：核验模型表层已压缩，而持久 transcriptLog 依然 100% 保留原始长提示词气泡！
    events.clear();
    controller.CompactConversation(QStringLiteral("fixture-provider"), QStringLiteral("fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(HasEvent(events, QStringLiteral("compact_end")), 15000);
    QVERIFY(LastEvent(events, QStringLiteral("compact_end")).value(QStringLiteral("success")).toBool());
    QVERIFY(HasEvent(events, QStringLiteral("context_compacted")));

    const auto afterCompact = store.List(QStringLiteral("map-A"));
    QCOMPARE(afterCompact.size(), qsizetype(1));
    QVERIFY(QJsonDocument(afterCompact.front().transcriptLog).toJson().contains(longPrompt.toUtf8()));
    QVERIFY(controller.CurrentFeedbackConversationSnippet().contains(QStringLiteral("三件铁剑自动合成玄铁剑")));

    // 4. 单文件损坏容错：同目录下多出一个损坏文件时，正常会话依然可列出，不会整图消失！
    const QDir mapSessionDir(directory.filePath(QStringLiteral("history/sessions/") + AgentSessionStore::MapHash(QStringLiteral("map-A"))));
    QFile brokenSibling(mapSessionDir.filePath(QStringLiteral("00000000-0000-4000-8000-000000000099.json")));
    QVERIFY(brokenSibling.open(QIODevice::WriteOnly));
    brokenSibling.write("{corrupt-sibling");
    brokenSibling.close();
    QString listErr, listWarn;
    const auto resilientList = store.List(QStringLiteral("map-A"), &listErr, &listWarn);
    QVERIFY(listErr.isEmpty());
    QVERIFY(!listWarn.isEmpty());
    QCOMPARE(resilientList.size(), qsizetype(1));
}

void AgentControllerTests::projectMemoryAndExternalMcpEndToEnd() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(TokenMeter::EstimateText(QStringLiteral("中文触发器测试")) > 0);

    // 1. 地图同目录 <地图名>.memory.md 解析、只读门禁与读写注入测试
    const QString fakeMapPath = directory.filePath(QStringLiteral("Maps/SynthRPG.w3x"));
    QVERIFY(QDir().mkpath(QFileInfo(fakeMapPath).absolutePath()));
    QFile fakeMap(fakeMapPath); QVERIFY(fakeMap.open(QIODevice::WriteOnly)); fakeMap.write("HM3W"); fakeMap.close();

    ProjectMemoryService memory;
    memory.SetMapSourcePath(fakeMapPath);
    QCOMPARE(QFileInfo(memory.ProjectMemoryFilePath()).fileName(), QStringLiteral("SynthRPG.memory.md"));
    QVERIFY(!QFileInfo::exists(memory.ProjectMemoryFilePath()));

    // 只读档位拒绝 AI 写入记忆且不在磁盘生成空文件
    const auto denied = memory.InvokeTool(QStringLiteral("memory_write"),
        {{QStringLiteral("content"), QStringLiteral("不应写入")}}, false);
    QVERIFY(denied.value(QStringLiteral("isError")).toBool());
    QVERIFY(!QFileInfo::exists(memory.ProjectMemoryFilePath()));

    // 允许修改档位下成功写入 <地图名>.memory.md 并生成上下文段落
    const auto written = memory.InvokeTool(QStringLiteral("memory_write"),
        {{QStringLiteral("content"), QStringLiteral("- 习惯：所有刷怪触发器放入【刷怪系统】目录")}}, true);
    QVERIFY(written.value(QStringLiteral("ok")).toBool());
    QVERIFY(QFileInfo::exists(memory.ProjectMemoryFilePath()));
    QVERIFY(memory.BuildPromptSection().contains(QStringLiteral("刷怪系统")));

    // 2. 外部自定义 MCP 客户端（真实本机 stdio 子进程 + Streamable HTTP 双协议联调 + 最低权限门禁）
    const QString stdioScriptPath = directory.filePath(QStringLiteral("mock-mcp-stdio.mjs"));
    {
        QFile script(stdioScriptPath);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"(
import { createInterface } from 'node:readline';
const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
for await (const line of rl) {
  if (!line.trim()) continue;
  const req = JSON.parse(line);
  if (req.method === 'initialize') {
    process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id: req.id, result: {
      protocolVersion: '2025-03-26', capabilities: { tools: {} },
      serverInfo: { name: 'mock-stdio', version: '1.0' },
      instructions: '使用 ping_map 验证连通性'
    } }) + '\n');
  } else if (req.method === 'tools/list') {
    process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id: req.id, result: {
      tools: [{ name: 'ping_map', description: '测试连通', inputSchema: { type: 'object', properties: { tag: { type: 'string' } } } }]
    } }) + '\n');
  } else if (req.method === 'tools/call') {
    process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id: req.id, result: {
      content: [{ type: 'text', text: JSON.stringify({ pong: req.params.arguments.tag || 'ok' }) }]
    } }) + '\n');
  }
}
)");
        script.close();
    }

    QTcpServer httpMcpServer;
    QVERIFY(httpMcpServer.listen(QHostAddress::LocalHost));
    connect(&httpMcpServer, &QTcpServer::newConnection, this, [&httpMcpServer] {
        while (httpMcpServer.hasPendingConnections()) {
            auto* sock = httpMcpServer.nextPendingConnection();
            auto buf = std::make_shared<QByteArray>();
            connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
            connect(sock, &QTcpSocket::readyRead, sock, [sock, buf] {
                buf->append(sock->readAll());
                const auto hdrEnd = buf->indexOf("\r\n\r\n");
                if (hdrEnd < 0) return;
                qint64 len = 0;
                for (auto line : buf->left(hdrEnd).split('\n')) {
                    if (line.trimmed().toLower().startsWith("content-length:")) len = line.trimmed().mid(15).trimmed().toLongLong();
                }
                if (buf->size() < hdrEnd + 4 + len) return;
                const auto req = QJsonDocument::fromJson(buf->mid(hdrEnd + 4, len)).object();
                buf->clear();
                const QString method = req.value(QStringLiteral("method")).toString();
                QJsonObject resp{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), req.value(QStringLiteral("id"))}};
                if (method == QStringLiteral("initialize")) {
                    resp.insert(QStringLiteral("result"), QJsonObject{
                        {QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")},
                        {QStringLiteral("capabilities"), QJsonObject{}},
                        {QStringLiteral("serverInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("mock-http")}}},
                    });
                } else if (method == QStringLiteral("tools/list")) {
                    resp.insert(QStringLiteral("result"), QJsonObject{
                        {QStringLiteral("tools"), QJsonArray{QJsonObject{
                            {QStringLiteral("name"), QStringLiteral("query_doc")},
                            {QStringLiteral("description"), QStringLiteral("查询外部文档")},
                            {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), QJsonObject{}}}},
                        }}},
                    });
                } else if (method == QStringLiteral("tools/call")) {
                    resp.insert(QStringLiteral("result"), QJsonObject{
                        {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("http-doc-ok")}}}},
                    });
                } else {
                    const QByteArray accepted("HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    sock->write(accepted); sock->disconnectFromHost(); return;
                }
                const QByteArray payload = QJsonDocument(resp).toJson(QJsonDocument::Compact);
                sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sess-42\r\nContent-Length: "
                    + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
                sock->disconnectFromHost();
            });
        }
    });

    ExternalMcpManager mcpManager;
    AgentSettings mcpSettings;
    mcpSettings.allowEdits = false; // 初始只读档位
    ExternalMcpServerSettings stdioCfg;
    stdioCfg.id = QStringLiteral("srv-stdio");
    stdioCfg.name = QStringLiteral("war3_autotest");
    stdioCfg.transport = QStringLiteral("stdio");
    stdioCfg.command = node;
    stdioCfg.args = {stdioScriptPath};
    stdioCfg.minimumPermission = QStringLiteral("full"); // 需要完全访问

    ExternalMcpServerSettings httpCfg;
    httpCfg.id = QStringLiteral("srv-http");
    httpCfg.name = QStringLiteral("war3_docs");
    httpCfg.transport = QStringLiteral("http");
    httpCfg.url = QStringLiteral("http://127.0.0.1:%1/mcp").arg(httpMcpServer.serverPort());
    httpCfg.minimumPermission = QStringLiteral("read"); // 只读即可使用

    mcpSettings.externalMcpServers = {stdioCfg, httpCfg};
    mcpManager.ApplySettings(mcpSettings);

    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const auto st = mcpManager.Statuses();
        return st.size() == 2 && st[0].connected && st[1].connected;
    }(), 10000);

    // 只读档位下：只暴露 minimumPermission="read" 的 http 工具，拦截 "full" 的 stdio 工具
    QCOMPARE(mcpManager.EnabledToolDefinitions().size(), qsizetype(1));
    QVERIFY(mcpManager.InvokeTool(QStringLiteral("mcp__war3_autotest__ping_map"), {}).value(QStringLiteral("isError")).toBool());
    QVERIFY(!mcpManager.InvokeTool(QStringLiteral("mcp__war3_docs__query_doc"), {}).value(QStringLiteral("isError")).toBool());

    // 升至完全访问档位后：两台 MCP 服务器工具均正常暴露与调用
    mcpSettings.allowEdits = true;
    mcpSettings.allowSave = true;
    mcpSettings.allowCommands = true;
    mcpManager.ApplySettings(mcpSettings);
    QCOMPARE(mcpManager.EnabledToolDefinitions().size(), qsizetype(2));
    QVERIFY(mcpManager.CombinedServerInstructions().contains(QStringLiteral("ping_map")));
    const auto callOut = mcpManager.InvokeTool(QStringLiteral("mcp__war3_autotest__ping_map"),
        {{QStringLiteral("tag"), QStringLiteral("hello-forge")}});
    QVERIFY(!callOut.value(QStringLiteral("isError")).toBool());
    QVERIFY(QJsonDocument(callOut).toJson().contains("hello-forge"));

    // 一键重连验证
    mcpManager.Reconnect(QStringLiteral("srv-stdio"));
    QTRY_VERIFY_WITH_TIMEOUT(mcpManager.Statuses().front().connected, 10000);
    mcpManager.StopAll();
}

int main(int argc, char* argv[]) {
    bool child = false;
    for (int index = 1; index < argc; ++index)
        if (QByteArray(argv[index]) == "--portable-agent-child") child = true;
    if (child) {
        QString error;
        if (!JoinPortableChildJob(error)) { std::fprintf(stderr, "%s\n", error.toUtf8().constData()); return 125; }
    }
    QCoreApplication application(argc, argv);
    auto arguments = application.arguments();
    portableRuntimeChild = arguments.removeOne(QStringLiteral("--portable-agent-child"));
    AgentControllerTests tests;
    return QTest::qExec(&tests, arguments);
}
#include "AgentControllerTests.moc"
