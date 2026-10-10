#pragma once

#include "AgentTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <functional>
#include <memory>

namespace War3Map::Agent {

// PI 辅助进程宿主。地图操作由注入的共用工具入口执行，本类不实现地图写入。
class AgentController final : public QObject {
    Q_OBJECT
public:
    explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;
    void SetSettings(const AgentSettings& settings);
    void SetActiveMap(QString title, QString identity, QString conversationIdentity = {});
    void SetToolDispatcher(std::function<QJsonObject(QString, QJsonObject)> dispatcher);
    void SetToolsProvider(std::function<QJsonArray()> provider);
    void SetSystemPrompt(QString prompt);
    void SetRuntimePaths(QString nodePath, QString workerPath);
    void SetCredentialDirectory(QString directory);
    void SetHistoryDirectory(QString directory);
    void RefreshConversations();
    [[nodiscard]] bool IsBusy() const;
    [[nodiscard]] qint64 RuntimeProcessId() const;
    // 仅显式反馈选项调用；读取当前已提交会话，不加载其他聊天或持久化内容。
    [[nodiscard]] QString CurrentFeedbackConversationSnippet(qsizetype maxUtf8Bytes = 16384) const;

public slots:
    void StartPrompt(QString text, QString providerId, QString modelId);
    void Stop();
    void NewConversation();
    void SwitchConversation(QString sessionId);
    void RenameConversation(QString sessionId, QString title);
    void ArchiveConversation(QString sessionId);

signals:
    void Event(const QJsonObject& event);
    void BusyChanged(bool busy);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Agent
