#pragma once

#include "War3MapCore/Update/UpdateArchive.h"

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QSettings;
class QTemporaryDir;
class QThread;
class QTimer;

namespace War3Map::QtUpdate {

enum class UpdateState { Idle, Checking, UpToDate, Available, Skipped, Downloading, Verifying, Ready, Failed, Cancelled };

struct UpdatePreferences {
    bool automaticCheckEnabled = true;
    Update::UpdateChannel channel = Update::UpdateChannel::Stable;
    QString skippedVersion;
    [[nodiscard]] bool operator==(const UpdatePreferences&) const = default;
};

struct UpdateStatus {
    UpdateState state = UpdateState::Idle;
    Update::UpdateChannel channel = Update::UpdateChannel::Stable;
    QString currentVersion;
    QString availableVersion;
    QString releaseNotes;
    QString message;
    QString errorCode;
    bool manual = true;
    bool canDownload = false;
};

// 只表示独立下载包已校验；安装器仍须重新验签、验包和核对安装目录。
struct PreparedUpdate {
    QString packagePath;
    QString manifestPath;
    QString signaturePath;
    QString version;
    QString manifestSha256;
    QString packageSha256;
    [[nodiscard]] bool operator==(const PreparedUpdate&) const = default;
};

// 显式测试入口只接受同一回环 origin，不读取正式公钥或写正式应用设置。
struct LoopbackUpdateSource {
    QUrl apiRoot;
    QString currentVersion;
    QString trustedPublicKeyHex;
    QString stagingDirectory;
    int requestTimeoutMs = 1000;
};

class UpdateClient final : public QObject {
    Q_OBJECT
public:
    explicit UpdateClient(QObject* parent = nullptr);
    UpdateClient(const LoopbackUpdateSource& testSource, QSettings& testSettings, QObject* parent = nullptr);
    ~UpdateClient() override;

    [[nodiscard]] const UpdateStatus& Status() const noexcept { return status_; }
    [[nodiscard]] bool IsBusy() const noexcept;
    [[nodiscard]] bool AutomaticCheckEnabled() const;
    [[nodiscard]] Update::UpdateChannel Channel() const;
    [[nodiscard]] QString SkippedVersion() const;
    [[nodiscard]] const UpdatePreferences& Preferences() const noexcept { return preferences_; }
    [[nodiscard]] const std::optional<PreparedUpdate>& Prepared() const noexcept { return prepared_; }
    [[nodiscard]] QUrl CheckEndpoint() const;

    [[nodiscard]] static UpdatePreferences ReadPreferences(QSettings& rootSettings);
    [[nodiscard]] static bool ValidatePreferences(const UpdatePreferences& preferences, QString* error = nullptr);
    // 只设置三个更新偏好键，不sync；宿主统一持久化并按自己的快照回滚。
    [[nodiscard]] static bool WritePreferences(QSettings& rootSettings, const UpdatePreferences& preferences, QString* error = nullptr);
    // 宿主确认统一保存成功后应用运行时偏好；此入口不写设置文件。
    [[nodiscard]] bool ApplyCommittedPreferences(const UpdatePreferences& preferences, QString* error = nullptr);

    bool SetAutomaticCheckEnabled(bool enabled, QString* error = nullptr);
    bool SetChannel(Update::UpdateChannel channel, QString* error = nullptr);
    bool SkipThisVersion(QString* error = nullptr);
    // 设置页的检查窗口使用暂存值，仅改变本次检查状态，不写已提交偏好。
    bool SkipThisVersionInSession(QString* error = nullptr);
    bool CheckWithPreferences(const UpdatePreferences& preferences);
    void ScheduleAutomaticCheck(int delayMs = 30000);
    bool Check(bool manual = true);
    bool Download();
    void Cancel();
    // 独立updater已经成功启动、宿主已接受正常关闭后才保留其输入。
    [[nodiscard]] bool RetainPreparedForInstaller(const PreparedUpdate& update, QString* error = nullptr);

signals:
    void StatusChanged(const War3Map::QtUpdate::UpdateStatus& status);
    void PreferencesChanged();
    void DownloadProgress(qint64 received, qint64 total);
    void PackagePrepared(const War3Map::QtUpdate::PreparedUpdate& update);

private:
    enum class RequestStage { Releases, Manifest, Signature, Package };
    struct Asset { QUrl url; qint64 bytes = 0; };
    void Initialize();
    bool CheckUsing(const UpdatePreferences& preferences, bool manual);
    QUrl Endpoint(Update::UpdateChannel channel) const;
    bool WritePreference(const QString& key, const QVariant& value, QString* error);
    void Publish(UpdateState state, const QString& message, const QString& code = {});
    void Fail(const QString& code, const QString& message);
    void ClearReply();
    bool ClearStaging();
    void Request(const QUrl& url, RequestStage stage, int redirects = 0);
    void Consume(QNetworkReply* reply);
    void FinishRequest(QNetworkReply* reply);
    bool UrlAllowed(const QUrl& url, bool redirect) const;
    bool ReadRelease(const QByteArray& bytes, bool hasNextPage);
    void FinishSignature();
    void StartPackageVerification();

    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QTimer* requestTimer_ = nullptr;
    QTimer* automaticTimer_ = nullptr;
    QTimer* deadlineTimer_ = nullptr;
    QThread* verifier_ = nullptr;
    QSettings* settings_ = nullptr;
    QUrl apiRoot_;
    QString releaseOwner_;
    QString releaseRepository_;
    QString publicKeyHex_;
    QString stagingRoot_;
    QString configurationError_;
    bool loopback_ = false;
    bool cancellingVerification_ = false;
    int timeoutMs_ = 15000;
    RequestStage stage_ = RequestStage::Releases;
    int redirects_ = 0;
    QByteArray response_;
    QByteArray manifestBytes_;
    QByteArray signatureBytes_;
    QMap<QString, Asset> assets_;
    std::unique_ptr<QTemporaryDir> staging_;
    std::unique_ptr<QSaveFile> packageFile_;
    std::optional<Update::VerifiedUpdateManifest> verifiedManifest_;
    std::optional<PreparedUpdate> prepared_;
    qint64 received_ = 0;
    UpdatePreferences preferences_;
    UpdatePreferences checkPreferences_;
    UpdateStatus status_;
};

} // namespace War3Map::QtUpdate

Q_DECLARE_METATYPE(War3Map::QtUpdate::UpdateState)
Q_DECLARE_METATYPE(War3Map::QtUpdate::UpdateStatus)
Q_DECLARE_METATYPE(War3Map::QtUpdate::PreparedUpdate)
Q_DECLARE_METATYPE(War3Map::QtUpdate::UpdatePreferences)
