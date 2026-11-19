#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace War3Map::Agent {

// 会话身份只绑定地图；messages 为发给模型的平衡表层，transcriptLog 为只增不减的完整用户/工具/压缩历史。
struct AgentSession {
    QString id;
    QString mapIdentity;
    QString title;
    QString createdAt;
    QString updatedAt;
    QString providerId;
    QString modelId;
    QJsonArray messages;
    QString migratedFrom;
    QJsonArray transcriptLog;
    QJsonObject lastUsage;
};

[[nodiscard]] bool IsCompleteAgentTranscript(const QJsonArray& messages);
[[nodiscard]] bool IsHiddenAgentMessage(const QJsonObject& message);
[[nodiscard]] QString AgentMessageText(const QJsonObject& message);

// 有界原子存储，删除只归档；旧版单文件按身份只读复制，保留作者原历史。
class AgentSessionStore final {
public:
    explicit AgentSessionStore(QString directory);
    [[nodiscard]] static QString MapHash(const QString& mapIdentity);
    [[nodiscard]] static QString LegacyBinding(const QString& map, const QString& provider, const QString& model);
    [[nodiscard]] QString SessionPath(const QString& mapIdentity, const QString& id) const;
    [[nodiscard]] QList<AgentSession> List(const QString& mapIdentity, QString* error = nullptr,
        QString* warning = nullptr) const;
    [[nodiscard]] std::optional<AgentSession> Read(const QString& mapIdentity, const QString& id,
        QString* error = nullptr) const;
    [[nodiscard]] std::optional<AgentSession> Create(const QString& mapIdentity, QString* error = nullptr) const;
    // writtenLog 返回实际写出的 transcript_log（超预算时为裁剪后的版本），调用方据此替换内存副本。
    [[nodiscard]] bool Save(const AgentSession& session, QString* error = nullptr, QJsonArray* writtenLog = nullptr) const;
    [[nodiscard]] bool Rename(const QString& mapIdentity, const QString& id, const QString& title,
        QString* error = nullptr) const;
    [[nodiscard]] bool Archive(const QString& mapIdentity, const QString& id, QString* error = nullptr) const;
    [[nodiscard]] bool MigrateLegacy(const QString& mapIdentity, QString* error = nullptr,
        const QString& legacyMapIdentity = {}) const;

private:
    QString directory_;
};

} // namespace War3Map::Agent
