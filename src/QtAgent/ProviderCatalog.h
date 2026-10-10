#pragma once

#include "AgentTypes.h"

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace War3Map::Agent {

// 有界、异步的真实模型目录读取。任何失败都不会用假列表替代。
class ProviderCatalog final : public QObject {
    Q_OBJECT
public:
    explicit ProviderCatalog(QObject* parent = nullptr);
    ~ProviderCatalog() override;
    void Fetch(const ProviderSettings& provider, const QString& secret, int timeoutMs = 15000);
    void Cancel();
    [[nodiscard]] bool IsBusy() const;
    [[nodiscard]] static QUrl Endpoint(const ProviderSettings& provider, QString* error = nullptr);

signals:
    void ModelsReady(const QString& providerId, const QList<War3Map::Agent::ModelSettings>& models);
    void Failed(const QString& providerId, const QString& message);
    void BusyChanged(bool busy);

private:
    void RequestPage(const QString& cursor = {});
    void FinishPage(QNetworkReply* reply);
    void Consume(QNetworkReply* reply);
    void Fail(const QString& message);
    void Clear();

    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QTimer* timer_ = nullptr;
    ProviderSettings provider_;
    QString secret_;
    QUrl endpoint_;
    QByteArray response_;
    QList<ModelSettings> models_;
    QSet<QString> modelIds_;
    QSet<QString> cursors_;
    qsizetype totalBytes_ = 0;
    int pages_ = 0;
    bool busy_ = false;
};

} // namespace War3Map::Agent
