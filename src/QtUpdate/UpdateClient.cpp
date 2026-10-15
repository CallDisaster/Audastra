#include "UpdateClient.h"

#include "../QtCommon/ApplicationPaths.h"
#include "AudastraVersion.generated.h"
#include "War3MapCore/Update/SemanticVersion.h"
#include "War3MapCore/Update/UpdateInstaller.h"

#include <QDate>
#include <QDir>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <compare>
#include <cmath>
#include <exception>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace War3Map::QtUpdate {
namespace {
constexpr qsizetype kReleaseBudget = 2 * 1024 * 1024;
constexpr char kAutomaticKey[] = "update/automaticEnabled";
constexpr char kChannelKey[] = "update/channel";
constexpr char kSkipKey[] = "update/skippedVersion";
constexpr char kAttemptKey[] = "update/lastAutomaticCheckDate";
// GitHub 资源 CDN 经代理时常见十几秒停顿或断线；包下载只要有进展就重新计数。
constexpr int kMetadataRetries = 3;
constexpr int kPackageRetries = 8;

bool RetryableNetworkError(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::RemoteHostClosedError: case QNetworkReply::HostNotFoundError: case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::TimeoutError: case QNetworkReply::OperationCanceledError: case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError: case QNetworkReply::UnknownNetworkError: case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError: case QNetworkReply::InternalServerError: case QNetworkReply::ServiceUnavailableError:
    case QNetworkReply::UnknownServerError:
        return true;
    default:
        return false;
    }
}

QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::span<const std::byte> Bytes(const QByteArray& value) {
    return {reinterpret_cast<const std::byte*>(value.constData()), static_cast<std::size_t>(value.size())};
}
bool Loopback(const QUrl& url) {
    // 测试不解析任意主机名，避免DNS把非回环站点映射成本机而绕过注入限制。
    return url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 || QHostAddress(url.host()).isLoopback();
}
bool SafeRoot(const QUrl& url) {
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
bool SameOrigin(const QUrl& a, const QUrl& b) {
    return a.scheme() == b.scheme() && a.host().compare(b.host(), Qt::CaseInsensitive) == 0 && a.port() == b.port();
}
QString DiagnosticCode(const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
    for (const auto& diagnostic : diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error) return Text(diagnostic.code);
    return fallback;
}
QString DiagnosticMessage(const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
    for (const auto& diagnostic : diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error) return Text(diagnostic.message);
    return fallback;
}
bool WriteExact(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path); file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QString ActualDirectory(const QString& path) {
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    const auto handle = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    const auto size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::wstring actual(size && size <= 32768 ? size : 1, L'\0');
    const auto count = size && size <= 32768 ? GetFinalPathNameByHandleW(handle, actual.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    CloseHandle(handle);
    if (!count || count >= size) return {};
    actual.resize(count);
    if (!actual.starts_with(L"\\\\?\\") || actual.starts_with(L"\\\\?\\UNC\\")) return {};
    actual.erase(0, 4);
    return QDir::fromNativeSeparators(QString::fromStdWString(actual));
#else
    return QDir(path).canonicalPath();
#endif
}
} // namespace

UpdateClient::UpdateClient(QObject* parent) : QObject(parent) {
    status_.currentVersion = Text(BuildInfo::Version);
    releaseOwner_ = Text(BuildInfo::ReleaseOwner); releaseRepository_ = Text(BuildInfo::ReleaseRepository);
    publicKeyHex_ = Text(BuildInfo::UpdatePublicKeyHex);
    const QRegularExpression name(QStringLiteral("^[A-Za-z0-9_.-]{1,100}$"));
    if (!name.match(releaseOwner_).hasMatch() || !name.match(releaseRepository_).hasMatch())
        configurationError_ = tr("此构建未配置有效的 GitHub 发布仓库。");
    else apiRoot_ = QUrl(QStringLiteral("https://api.github.com/repos/%1/%2").arg(releaseOwner_, releaseRepository_));
    stagingRoot_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!stagingRoot_.isEmpty()) stagingRoot_ += QStringLiteral("/updates");
    settings_ = new QSettings(this);
    Initialize();
}

UpdateClient::UpdateClient(const LoopbackUpdateSource& source, QSettings& settings, QObject* parent)
    : QObject(parent), settings_(&settings), apiRoot_(source.apiRoot), publicKeyHex_(source.trustedPublicKeyHex),
      stagingRoot_(source.stagingDirectory), installationRoot_(source.installationDirectory),
      loopback_(true), timeoutMs_(std::clamp(source.requestTimeoutMs, 1, 120000)) {
    status_.currentVersion = source.currentVersion;
    if (!SafeRoot(apiRoot_) || !Loopback(apiRoot_) || (apiRoot_.scheme() != QStringLiteral("http") && apiRoot_.scheme() != QStringLiteral("https")))
        configurationError_ = tr("测试更新源必须是明确的回环 HTTP/HTTPS 地址，不能含凭据或参数。");
    if (!settings.group().isEmpty()) configurationError_ = tr("更新测试设置必须使用独立文件的根 group。");
    Initialize();
}

void UpdateClient::Initialize() {
    auto path = apiRoot_.path(); while (path.endsWith(QLatin1Char('/'))) path.chop(1); apiRoot_.setPath(path);
    qRegisterMetaType<UpdateStatus>(); qRegisterMetaType<PreparedUpdate>(); qRegisterMetaType<UpdateState>(); qRegisterMetaType<UpdatePreferences>();
    preferences_ = ReadPreferences(*settings_); checkPreferences_ = preferences_; status_.channel = preferences_.channel;
    if (!Update::ParseSemanticVersion(status_.currentVersion.toStdString()).Succeeded())
        configurationError_ = tr("当前构建版本不是有效的语义化版本，无法比较更新。");
    network_ = new QNetworkAccessManager(this);
    requestTimer_ = new QTimer(this); requestTimer_->setSingleShot(true);
    automaticTimer_ = new QTimer(this); automaticTimer_->setSingleShot(true);
    deadlineTimer_ = new QTimer(this); deadlineTimer_->setSingleShot(true);
    retryTimer_ = new QTimer(this); retryTimer_->setSingleShot(true);
    connect(requestTimer_, &QTimer::timeout, this, [this] { RetryOrFail(QStringLiteral("UPDATE_NETWORK_TIMEOUT"), tr("检查或下载更新超时，请稍后重试。")); });
    connect(retryTimer_, &QTimer::timeout, this, [this] { Request(stageUrl_, stage_); });
    connect(deadlineTimer_, &QTimer::timeout, this, [this] { Fail(QStringLiteral("UPDATE_OPERATION_TIMEOUT"), tr("更新操作超过允许的总时间，已停止请求。")); });
    connect(automaticTimer_, &QTimer::timeout, this, [this] { (void)Check(false); });
}

UpdateClient::~UpdateClient() {
    ClearReply();
    if (baselineCancelled_) baselineCancelled_->store(true);
    if (baselineVerifier_) {
        disconnect(baselineVerifier_, nullptr, this, nullptr);
        baselineVerifier_->wait();
        delete baselineVerifier_; baselineVerifier_ = nullptr;
    }
    if (verifier_) {
        disconnect(verifier_, nullptr, this, nullptr);
        verifier_->wait();
        delete verifier_; verifier_ = nullptr;
    }
}

bool UpdateClient::IsBusy() const noexcept {
    return verifier_ || baselineVerifier_ || status_.state == UpdateState::Checking
        || status_.state == UpdateState::Downloading || status_.state == UpdateState::Verifying;
}
bool UpdateClient::AutomaticCheckEnabled() const { return preferences_.automaticCheckEnabled; }
Update::UpdateChannel UpdateClient::Channel() const { return preferences_.channel; }
QString UpdateClient::SkippedVersion() const { return preferences_.skippedVersion; }

UpdatePreferences UpdateClient::ReadPreferences(QSettings& settings) {
    UpdatePreferences preferences;
    if (!settings.group().isEmpty()) return preferences;
    const auto automatic = settings.value(kAutomaticKey, true);
    if (automatic.metaType().id() == QMetaType::Bool) preferences.automaticCheckEnabled = automatic.toBool();
    else if (automatic.metaType().id() == QMetaType::QString && automatic.toString() == QStringLiteral("false"))
        preferences.automaticCheckEnabled = false;
    preferences.channel = settings.value(kChannelKey, QStringLiteral("stable")).toString() == QStringLiteral("beta")
        ? Update::UpdateChannel::Beta : Update::UpdateChannel::Stable;
    const auto skipped = settings.value(kSkipKey).toString();
    if (skipped.isEmpty() || Update::ParseSemanticVersion(skipped.toStdString()).Succeeded()) preferences.skippedVersion = skipped;
    return preferences;
}

bool UpdateClient::ValidatePreferences(const UpdatePreferences& preferences, QString* error) {
    if (error) error->clear();
    if (preferences.channel != Update::UpdateChannel::Stable && preferences.channel != Update::UpdateChannel::Beta) {
        if (error) *error = tr("更新渠道无效。");
        return false;
    }
    if (!preferences.skippedVersion.isEmpty() && !Update::ParseSemanticVersion(preferences.skippedVersion.toStdString()).Succeeded()) {
        if (error) *error = tr("跳过的更新版本不是有效的语义化版本。");
        return false;
    }
    return true;
}

bool UpdateClient::WritePreferences(QSettings& settings, const UpdatePreferences& preferences, QString* error) {
    if (!ValidatePreferences(preferences, error)) return false;
    if (!settings.group().isEmpty() || settings.status() != QSettings::NoError) {
        if (error) *error = tr("更新偏好必须写入可用的本机设置根组。");
        return false;
    }
    settings.setValue(kAutomaticKey, preferences.automaticCheckEnabled);
    settings.setValue(kChannelKey, preferences.channel == Update::UpdateChannel::Beta ? QStringLiteral("beta") : QStringLiteral("stable"));
    settings.setValue(kSkipKey, preferences.skippedVersion);
    return true;
}

bool UpdateClient::ApplyCommittedPreferences(const UpdatePreferences& preferences, QString* error) {
    if (!ValidatePreferences(preferences, error)) return false;
    const QPointer<UpdateClient> alive(this);
    const auto previous = preferences_;
    preferences_ = preferences;
    if (!preferences_.automaticCheckEnabled) automaticTimer_->stop();
    if (previous.channel != preferences_.channel && status_.channel != preferences_.channel) {
        if (verifier_ || baselineVerifier_) resetChannelAfterVerification_ = true;
        if (IsBusy()) {
            Cancel();
            // StatusChanged 的直接槽可以销毁客户端，不能回到已析构的偏好/状态成员。
            if (!alive) return true;
        }
        status_.availableVersion.clear(); status_.releaseNotes.clear(); status_.canDownload = false;
        assets_.clear(); prepared_.reset();
        if (!verifier_ && !baselineVerifier_) {
            ClearStaging(); verifiedManifest_.reset(); status_.channel = preferences_.channel;
            Publish(UpdateState::Idle, tr("渠道已切换，可重新检查更新。"));
            if (!alive) return true;
        }
    }
    emit PreferencesChanged();
    return true;
}

bool UpdateClient::WritePreference(const QString& key, const QVariant& value, QString* error) {
    if (error) error->clear();
    // 单独的新QSettings避免旧对象sticky错误污染重试，只写本模块的单个键。
    QSettings stored(settings_->fileName(), settings_->format()); stored.setFallbacksEnabled(false);
    const bool had = stored.contains(key); const auto before = stored.value(key);
    stored.setValue(key, value); stored.sync();
    if (stored.status() != QSettings::NoError) {
        QSettings restore(settings_->fileName(), settings_->format()); restore.setFallbacksEnabled(false);
        if (had) restore.setValue(key, before); else restore.remove(key);
        restore.sync();
        if (error) *error = tr("无法保存更新设置，请检查设置目录的写入权限。");
        return false;
    }
    settings_->sync();
    return true;
}

bool UpdateClient::SetAutomaticCheckEnabled(bool enabled, QString* error) {
    if (!WritePreference(QString::fromLatin1(kAutomaticKey), enabled, error)) return false;
    auto preferences = preferences_; preferences.automaticCheckEnabled = enabled;
    return ApplyCommittedPreferences(preferences, error);
}
bool UpdateClient::SetChannel(Update::UpdateChannel channel, QString* error) {
    if (IsBusy()) { if (error) *error = tr("请等待当前检查或取消下载后再切换渠道。"); return false; }
    if (channel != Update::UpdateChannel::Stable && channel != Update::UpdateChannel::Beta) {
        if (error) *error = tr("更新渠道无效。"); return false;
    }
    if (!WritePreference(QString::fromLatin1(kChannelKey), channel == Update::UpdateChannel::Beta ? QStringLiteral("beta") : QStringLiteral("stable"), error)) return false;
    auto preferences = preferences_; preferences.channel = channel;
    return ApplyCommittedPreferences(preferences, error);
}
bool UpdateClient::SkipThisVersion(QString* error) {
    if (status_.state != UpdateState::Available || status_.availableVersion.isEmpty()) {
        if (error) *error = tr("没有可跳过的更新版本。"); return false;
    }
    if (!WritePreference(QString::fromLatin1(kSkipKey), status_.availableVersion, error)) return false;
    preferences_.skippedVersion = status_.availableVersion;
    status_.canDownload = false;
    Publish(UpdateState::Skipped, tr("已跳过版本 %1；下个版本发布后再提示。").arg(status_.availableVersion));
    emit PreferencesChanged(); return true;
}
bool UpdateClient::SkipThisVersionInSession(QString* error) {
    if (error) error->clear();
    if (status_.state != UpdateState::Available || status_.availableVersion.isEmpty()) {
        if (error) *error = tr("没有可跳过的更新版本。"); return false;
    }
    status_.canDownload = false;
    Publish(UpdateState::Skipped, tr("本次检查已跳过版本 %1。").arg(status_.availableVersion));
    return true;
}
void UpdateClient::ScheduleAutomaticCheck(int delayMs) {
    if (AutomaticCheckEnabled()) automaticTimer_->start(std::clamp(delayMs, 0, 600000));
}
QUrl UpdateClient::CheckEndpoint() const { return Endpoint(Channel()); }
QUrl UpdateClient::Endpoint(Update::UpdateChannel channel) const {
    auto endpoint = apiRoot_;
    QString path = endpoint.path(); while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    endpoint.setPath(path + (channel == Update::UpdateChannel::Stable ? QStringLiteral("/releases/latest") : QStringLiteral("/releases")));
    if (channel == Update::UpdateChannel::Beta) { QUrlQuery query; query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("100")); endpoint.setQuery(query); }
    return endpoint;
}

bool UpdateClient::Check(bool manual) { return CheckUsing(preferences_, manual); }
bool UpdateClient::CheckWithPreferences(const UpdatePreferences& preferences) { return CheckUsing(preferences, true); }
bool UpdateClient::CheckUsing(const UpdatePreferences& preferences, bool manual) {
    if (IsBusy()) return false;
    QString preferenceError;
    if (!ValidatePreferences(preferences, &preferenceError)) { Fail(QStringLiteral("UPDATE_PREFERENCES"), preferenceError); return false; }
    if (!manual) {
        if (!AutomaticCheckEnabled() || settings_->value(kAttemptKey).toString() == QDate::currentDate().toString(Qt::ISODate)) return false;
        QString error;
        if (!WritePreference(QString::fromLatin1(kAttemptKey), QDate::currentDate().toString(Qt::ISODate), &error)) {
            Fail(QStringLiteral("UPDATE_SETTINGS_WRITE"), error); return false;
        }
    }
    ClearStaging(); verifiedManifest_.reset(); prepared_.reset(); assets_.clear();
    checkPreferences_ = preferences; status_.channel = preferences.channel; attempts_ = 0;
    status_.manual = manual; status_.availableVersion.clear(); status_.releaseNotes.clear(); status_.canDownload = false;
    if (!configurationError_.isEmpty()) { Fail(QStringLiteral("UPDATE_CONFIGURATION"), configurationError_); return false; }
    Publish(UpdateState::Checking, tr("正在检查 %1更新…").arg(checkPreferences_.channel == Update::UpdateChannel::Stable ? tr("稳定版") : tr("测试版")));
    if (status_.state == UpdateState::Checking) {
        deadlineTimer_->start(loopback_ ? std::clamp(timeoutMs_ * 4, 1000, 30000) : 60000);
        Request(Endpoint(checkPreferences_.channel), RequestStage::Releases);
    }
    return true;
}

void UpdateClient::Publish(UpdateState state, const QString& message, const QString& code) {
    status_.state = state; status_.message = message; status_.errorCode = code;
    emit StatusChanged(status_);
}
void UpdateClient::ClearReply() {
    if (requestTimer_) requestTimer_->stop();
    if (retryTimer_) retryTimer_->stop();
    if (reply_) { auto* reply = reply_; reply_ = nullptr; disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater(); }
}
bool UpdateClient::ClearStaging() {
    if (packageFile_) { packageFile_->cancelWriting(); packageFile_.reset(); }
    const bool removed = !staging_ || !staging_->isValid() || !staging_->autoRemove() || staging_->remove();
    staging_.reset(); manifestBytes_.clear(); signatureBytes_.clear(); received_ = 0;
    return removed;
}
void UpdateClient::Fail(const QString& code, const QString& message) {
    deadlineTimer_->stop();
    ClearReply(); ClearStaging(); verifiedManifest_.reset(); prepared_.reset(); status_.canDownload = false;
    Publish(UpdateState::Failed, message, code);
}
void UpdateClient::Cancel() {
    if (!IsBusy()) return;
    deadlineTimer_->stop();
    ClearReply(); status_.canDownload = false;
    if (baselineVerifier_) {
        baselineCancelled_->store(true);
        cancellingVerification_ = true;
        Publish(UpdateState::Checking, tr("正在结束安装目录核对，随后取消更新。"));
        return;
    }
    if (verifier_) { cancellingVerification_ = true; Publish(UpdateState::Verifying, tr("正在结束包校验，随后取消更新。")); return; }
    const bool removed = ClearStaging(); verifiedManifest_.reset();
    Publish(UpdateState::Cancelled, removed ? tr("已取消更新；下载的临时文件已清理。")
        : tr("已取消更新；部分临时文件可能仍在应用数据目录，未继续安装。"), removed ? QString{} : QStringLiteral("UPDATE_STAGING_CLEANUP"));
}

void UpdateClient::FinishCancelledVerification() {
    cancellingVerification_ = false;
    const bool removed = ClearStaging(); verifiedManifest_.reset(); prepared_.reset(); status_.canDownload = false;
    if (resetChannelAfterVerification_) {
        resetChannelAfterVerification_ = false;
        status_.channel = preferences_.channel;
        Publish(UpdateState::Idle, tr("渠道已切换，可重新检查更新。"));
        return;
    }
    Publish(UpdateState::Cancelled, removed ? tr("已取消更新；下载的临时文件已清理。")
        : tr("已取消更新；部分临时文件可能仍在应用数据目录，未继续安装。"), removed ? QString{} : QStringLiteral("UPDATE_STAGING_CLEANUP"));
}

bool UpdateClient::UrlAllowed(const QUrl& url, bool redirect) const {
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment()) return false;
    const auto query = QUrlQuery(url).queryItems();
    if (!query.empty()) {
        // GitHub公共资产CDN需要短期sig/jwt运输签名；初始API只接受固定分页参数。
        // 不允许用户令牌、密码或API key由响应URL进入请求或错误文本。
        if (!redirect || loopback_) {
            if (url.path() != apiRoot_.path() + QStringLiteral("/releases") || query.size() != 1
                || query.front().first != QStringLiteral("per_page") || query.front().second != QStringLiteral("100")) return false;
        } else {
            if (!url.host().endsWith(QStringLiteral(".githubusercontent.com"))) return false;
            for (const auto& [name, value] : query) {
                Q_UNUSED(value);
                const auto lower = name.toLower();
                if (lower.contains(QStringLiteral("token")) || lower.contains(QStringLiteral("secret")) || lower.contains(QStringLiteral("password"))
                    || lower.contains(QStringLiteral("credential")) || lower.contains(QStringLiteral("auth")) || lower == QStringLiteral("api_key")
                    || lower == QStringLiteral("apikey") || lower == QStringLiteral("key") || lower.endsWith(QStringLiteral("_key"))) return false;
            }
        }
    }
    if (loopback_) return Loopback(url) && SameOrigin(url, apiRoot_);
    if (url.scheme() != QStringLiteral("https") || (url.port() != -1 && url.port() != 443)) return false;
    if (redirect) {
        const QString host = url.host().toLower();
        return host == QStringLiteral("github.com") || host == QStringLiteral("api.github.com")
            || host == QStringLiteral("release-assets.githubusercontent.com") || host == QStringLiteral("objects.githubusercontent.com")
            || host == QStringLiteral("github-releases.githubusercontent.com");
    }
    if (url.host() == QStringLiteral("api.github.com")) return url.path().startsWith(apiRoot_.path() + QStringLiteral("/releases"));
    return url.host() == QStringLiteral("github.com") && !url.hasQuery()
        && url.path().startsWith(QStringLiteral("/%1/%2/releases/download/").arg(releaseOwner_, releaseRepository_));
}

void UpdateClient::Request(const QUrl& url, RequestStage stage, int redirects) {
    if (!UrlAllowed(url, redirects != 0)) { Fail(QStringLiteral("UPDATE_SOURCE_URL"), tr("更新来源或重定向地址不符合当前发布源配置。")); return; }
    ClearReply();
    if (redirects == 0) {
        // 新阶段从零计数；重试回到阶段起点（GitHub 下载地址），重新取得短期 CDN 签名。
        if (stage != stage_ || url != stageUrl_) attempts_ = 0;
        stageUrl_ = url; attemptStart_ = received_;
    }
    stage_ = stage; redirects_ = redirects; response_.clear(); responseStarted_ = false;
    QNetworkRequest request(url);
    if (stage == RequestStage::Package && received_ > 0)
        request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(received_) + '-');
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setRawHeader("Accept", stage == RequestStage::Releases ? "application/vnd.github+json" : "application/octet-stream");
    request.setRawHeader("User-Agent", "Audastra-Update");
    if (!loopback_ && stage == RequestStage::Releases) request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setTransferTimeout(timeoutMs_);
    reply_ = network_->get(request); reply_->setReadBufferSize(128 * 1024);
    auto* reply = reply_;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { Consume(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { FinishRequest(reply); });
    requestTimer_->start(timeoutMs_);
}

void UpdateClient::Consume(QNetworkReply* reply) {
    if (reply != reply_) return;
    requestTimer_->start(timeoutMs_);
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto bytes = reply->readAll();
    // 错误正文不缓存、不回显，HTTP错误始终在FinishRequest按状态码明确归因。
    if (http != 200 && http != 206 && http > 0) return;
    if (stage_ == RequestStage::Package) {
        if (http != 200 && http != 206) return;
        const auto expected = static_cast<qint64>(verifiedManifest_->Manifest().packageBytes);
        if (!responseStarted_) {
            responseStarted_ = true;
            if (http == 206) {
                // 只接受从已收字节到包尾的续传；其他区间无法拼成签名清单声明的整包。
                const auto range = reply->rawHeader("Content-Range");
                const auto wanted = QByteArray("bytes ") + QByteArray::number(received_) + '-' + QByteArray::number(expected - 1) + '/' + QByteArray::number(expected);
                if (received_ == 0 || range != wanted) { Fail(QStringLiteral("UPDATE_PACKAGE_RANGE"), tr("下载服务器返回的续传区间与已下载部分不符，已停止下载。")); return; }
            } else if (received_ > 0 && !OpenPackageFile()) return; // 服务器不支持续传时从头重下。
            if (attempts_ > 0) Publish(UpdateState::Downloading, tr("正在下载版本 %1…").arg(status_.availableVersion));
            if (status_.state != UpdateState::Downloading) return;
        }
        if (received_ > expected - bytes.size()) { Fail(QStringLiteral("UPDATE_PACKAGE_SIZE"), tr("下载包超过签名清单声明的大小，已停止下载。")); return; }
        if (!packageFile_ || packageFile_->write(bytes) != bytes.size()) { Fail(QStringLiteral("UPDATE_DOWNLOAD_WRITE"), tr("无法写入更新临时目录，请检查磁盘空间与权限。")); return; }
        received_ += bytes.size(); emit DownloadProgress(received_, expected);
    } else {
        const qsizetype budget = stage_ == RequestStage::Releases ? kReleaseBudget : stage_ == RequestStage::Signature ? 64 : Update::kMaxUpdateManifestBytes;
        if (response_.size() > budget - bytes.size()) { Fail(QStringLiteral("UPDATE_RESPONSE_LIMIT"), tr("更新响应超过允许的大小，已停止读取。")); return; }
        response_.append(bytes);
    }
}

void UpdateClient::FinishRequest(QNetworkReply* reply) {
    if (reply != reply_) return;
    Consume(reply); if (reply != reply_) return;
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (http >= 300 && http < 400 && !redirect.isEmpty()) {
        if (redirects_ >= 5) { Fail(QStringLiteral("UPDATE_REDIRECT_LIMIT"), tr("更新下载重定向次数过多，已停止请求。")); return; }
        const auto next = reply->url().resolved(redirect); const auto stage = stage_; const int count = redirects_ + 1;
        Request(next, stage, count); return;
    }
    const bool success = http == 200 || (http == 206 && stage_ == RequestStage::Package);
    if (!success || reply->error() != QNetworkReply::NoError) {
        const bool timeout = reply->error() == QNetworkReply::TimeoutError || reply->error() == QNetworkReply::OperationCanceledError;
        // 传输中断时状态码可能已是 200/206；4xx 与来源校验错误不重试。
        const bool transport = RetryableNetworkError(reply->error()) && (http == 0 || http == 200 || http == 206);
        if (transport || http >= 500) {
            if (timeout) RetryOrFail(QStringLiteral("UPDATE_NETWORK_TIMEOUT"), tr("检查或下载更新超时，请稍后重试。"));
            else if (http >= 500) RetryOrFail(QStringLiteral("UPDATE_HTTP_ERROR"), tr("更新服务器返回 HTTP %1，请稍后重试。").arg(http));
            else RetryOrFail(QStringLiteral("UPDATE_NETWORK_ERROR"), tr("无法连接更新服务器或验证 TLS，请检查网络后重试。"));
        }
        else if (timeout) Fail(QStringLiteral("UPDATE_NETWORK_TIMEOUT"), tr("检查或下载更新超时，请稍后重试。"));
        else if (http == 404) Fail(QStringLiteral("UPDATE_HTTP_404"), tr("暂时无法检查或下载更新：发布仓库可能尚未公开，或此版本资源不存在（HTTP 404）。"));
        else if (http == 403 || http == 429) Fail(QStringLiteral("UPDATE_RATE_LIMIT"), tr("GitHub 暂时拒绝请求或请求已限流（HTTP %1），请稍后重试。").arg(http));
        else if (http > 0) Fail(QStringLiteral("UPDATE_HTTP_ERROR"), tr("更新服务器返回 HTTP %1，请稍后重试。").arg(http));
        else Fail(QStringLiteral("UPDATE_NETWORK_ERROR"), tr("无法连接更新服务器或验证 TLS，请检查网络后重试。"));
        return;
    }
    const bool hasNext = reply->rawHeader("Link").contains("rel=\"next\"");
    const auto body = response_; const auto stage = stage_; ClearReply();
    if (stage == RequestStage::Releases) { deadlineTimer_->stop(); (void)ReadRelease(body, hasNext); return; }
    if (stage == RequestStage::Manifest) { manifestBytes_ = body; Request(assets_[QStringLiteral("update-manifest.json.sig")].url, RequestStage::Signature); return; }
    if (stage == RequestStage::Signature) { signatureBytes_ = body; FinishSignature(); return; }
    if (!verifiedManifest_ || received_ != static_cast<qint64>(verifiedManifest_->Manifest().packageBytes)) {
        Fail(QStringLiteral("UPDATE_PACKAGE_SIZE"), tr("下载包的大小与签名清单不符，不能安装。")); return;
    }
    if (!packageFile_ || !packageFile_->commit()) { Fail(QStringLiteral("UPDATE_DOWNLOAD_WRITE"), tr("无法完成更新包写入，不能安装。")); return; }
    packageFile_.reset(); StartPackageVerification();
}

bool UpdateClient::ReadRelease(const QByteArray& bytes, bool hasNextPage) {
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(bytes, &parse);
    const bool stable = checkPreferences_.channel == Update::UpdateChannel::Stable;
    if (parse.error != QJsonParseError::NoError || (stable ? !document.isObject() : !document.isArray())) {
        Fail(QStringLiteral("UPDATE_RELEASE_JSON"), tr("发布服务器返回的数据不是有效的版本 JSON。")); return false;
    }
    if (!stable && hasNextPage) { Fail(QStringLiteral("UPDATE_RELEASE_PAGE_LIMIT"), tr("测试版发布列表超过检查预算，无法确认最新版本。")); return false; }
    const QJsonArray releases = stable ? QJsonArray{document.object()} : document.array();
    if (releases.size() > 100) { Fail(QStringLiteral("UPDATE_RELEASE_PAGE_LIMIT"), tr("发布列表超过 100 项，无法确认最新版本。")); return false; }
    QJsonObject chosen; std::optional<Update::SemanticVersion> newest;
    for (const auto& value : releases) {
        if (!value.isObject()) { Fail(QStringLiteral("UPDATE_RELEASE_INVALID"), tr("发布列表包含无效条目。")); return false; }
        const auto release = value.toObject();
        if (!release.value("draft").isBool() || !release.value("prerelease").isBool() || !release.value("tag_name").isString()) {
            Fail(QStringLiteral("UPDATE_RELEASE_INVALID"), tr("发布条目缺少版本、草稿或测试版标识。")); return false;
        }
        if (release.value("draft").toBool()) continue;
        QString version = release.value("tag_name").toString(); if (version.startsWith(QLatin1Char('v'))) version.remove(0, 1);
        const auto parsed = Update::ParseSemanticVersion(version.toStdString());
        if (!parsed.Succeeded() || !parsed.value) { Fail(QStringLiteral("UPDATE_RELEASE_VERSION"), tr("发布版本不是有效的语义化版本。")); return false; }
        if (stable && (release.value("prerelease").toBool() || parsed.value->HasPrerelease())) continue;
        if (!newest || Update::CompareSemanticVersions(*parsed.value, *newest) == std::strong_ordering::greater) { newest = *parsed.value; chosen = release; }
    }
    if (!newest) { Fail(QStringLiteral("UPDATE_RELEASE_UNAVAILABLE"), tr("当前渠道还没有可用的公开发布，暂时无法确认最新版本。")); return false; }
    if ((!chosen.value("body").isNull() && !chosen.value("body").isString()) || !chosen.value("assets").isArray()) {
        Fail(QStringLiteral("UPDATE_RELEASE_INVALID"), tr("发布说明或资源列表格式无效。")); return false;
    }
    const auto notes = chosen.value("body").toString();
    if (notes.toUtf8().size() > 65536 || chosen.value("assets").toArray().size() > 256) {
        Fail(QStringLiteral("UPDATE_RESPONSE_LIMIT"), tr("发布说明或资源列表超过检查预算。")); return false;
    }
    for (const auto& value : chosen.value("assets").toArray()) {
        if (!value.isObject()) { Fail(QStringLiteral("UPDATE_RELEASE_INVALID"), tr("发布资源条目无效。")); return false; }
        const auto asset = value.toObject(); const auto name = asset.value("name").toString();
        const auto size = asset.value("size").toInteger(-1); const auto number = asset.value("size").toDouble(-1);
        const QUrl url(asset.value("browser_download_url").toString());
        if (name.isEmpty() || name.size() > 256 || assets_.contains(name) || !asset.value("size").isDouble() || size < 0
            || !std::isfinite(number) || std::floor(number) != number || number != static_cast<double>(size)
            || size > static_cast<qint64>(Update::kMaxUpdatePackageBytes) || !UrlAllowed(url, false)) {
            Fail(QStringLiteral("UPDATE_RELEASE_ASSET"), tr("发布资源的名称、大小或下载地址无效。")); return false;
        }
        assets_.insert(name, {url, size});
    }
    status_.availableVersion = Text(newest->Text()); status_.releaseNotes = notes;
    const auto current = Update::ParseSemanticVersion(status_.currentVersion.toStdString());
    if (Update::CompareSemanticVersions(*newest, *current.value) != std::strong_ordering::greater) {
        Publish(UpdateState::UpToDate, tr("当前版本 %1 已是此渠道的最新版本。").arg(status_.currentVersion)); return true;
    }
    if (!status_.manual && checkPreferences_.skippedVersion == status_.availableVersion) {
        Publish(UpdateState::Skipped, tr("版本 %1 已跳过，下个版本发布后再提示。").arg(status_.availableVersion)); return true;
    }
    const auto key = Update::ParseTrustedUpdateKeyHex(publicKeyHex_.toStdString());
    const bool trusted = key.Succeeded() && key.value && key.value->configured;
    const bool signedAssets = assets_.contains(QStringLiteral("update-manifest.json")) && assets_.contains(QStringLiteral("update-manifest.json.sig"));
    status_.canDownload = false;
    if (!trusted || !signedAssets) {
        Publish(UpdateState::Available, !trusted ? tr("发现版本 %1；此构建未配置有效的更新验证公钥，暂不能安装。").arg(status_.availableVersion)
            : tr("发现版本 %1；发布缺少签名更新清单，暂不能安装。").arg(status_.availableVersion));
        return true;
    }
    StartInstalledBaselineVerification();
    return true;
}

void UpdateClient::StartInstalledBaselineVerification() {
    // 与安装准备使用同一安装根、当前构建版本和可信公钥。只读散列全部在后台进行；
    // 此结果仅用于检查阶段的提示，安装准备及独立安装器仍各自重新验证。
    const auto root = loopback_ ? installationRoot_ : QtCommon::InstallationRoot();
    const auto version = status_.currentVersion, keyHex = publicKeyHex_;
    auto result = std::make_shared<Result<Update::UpdateBaselineLocation>>();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    baselineCancelled_ = cancelled;
    baselineVerifier_ = QThread::create([root, version, keyHex, result, cancelled] {
        if (cancelled->load()) return;
        try {
            const auto key = Update::ParseTrustedUpdateKeyHex(keyHex.toStdString());
            if (!key.Succeeded() || !key.value || !key.value->configured) {
                *result = Result<Update::UpdateBaselineLocation>::Failure("QtUpdate", "UPDATE_TRUST_NOT_CONFIGURED",
                    "未配置可信发布公钥，无法核对当前安装目录。");
            } else if (root.isEmpty()) {
                *result = Result<Update::UpdateBaselineLocation>::Failure("QtUpdate", "UPDATE_BASELINE_NOT_FOUND",
                    "无法定位当前安装目录的签名版本基线。");
            } else {
                *result = Update::DiscoverInstalledUpdateBaseline(std::filesystem::path(root.toStdWString()),
                    version.toStdString(), *key.value);
            }
        } catch (...) {
            *result = Result<Update::UpdateBaselineLocation>::Failure("QtUpdate", "UPDATE_BASELINE_DISCOVERY_FAILED",
                "无法完成当前安装目录的只读核对。");
        }
    });
    auto* worker = baselineVerifier_;
    connect(worker, &QThread::finished, this, [this, worker, result, cancelled] {
        if (baselineVerifier_ != worker) return;
        baselineVerifier_ = nullptr; baselineCancelled_.reset();
        if (cancelled->load() || cancellingVerification_) { FinishCancelledVerification(); return; }
        if (!result->Succeeded() || !result->value) {
            status_.canDownload = false;
            Publish(UpdateState::Available,
                tr("发现版本 %1；当前安装目录缺少可信版本基线，或程序文件与签名清单不一致，不能自动下载安装。"
                   "请下载最新完整包，解压到新的文件夹使用，设置会自动沿用。\n\n%2")
                    .arg(status_.availableVersion, DiagnosticMessage(result->diagnostics, tr("安装目录核对失败。"))),
                DiagnosticCode(result->diagnostics, QStringLiteral("UPDATE_BASELINE_DISCOVERY_FAILED")));
            return;
        }
        status_.canDownload = true;
        Publish(UpdateState::Available, tr("发现新版本 %1，可以下载更新。").arg(status_.availableVersion));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    // StatusChanged 可同步触发取消、切换渠道或销毁客户端；工作指针先登记，
    // 取消标记由后台读取，销毁后不再启动已被析构回收的线程。
    const QPointer<UpdateClient> alive(this);
    Publish(UpdateState::Checking, tr("发现版本 %1，正在核对当前安装目录…").arg(status_.availableVersion));
    if (alive) worker->start();
}

bool UpdateClient::Download() {
    if (IsBusy() || status_.state != UpdateState::Available || !status_.canDownload) return false;
    if (stagingRoot_.isEmpty() || !QDir().mkpath(stagingRoot_)) { Fail(QStringLiteral("UPDATE_STAGING_DIRECTORY"), tr("无法创建应用数据中的更新临时目录。")); return false; }
    // 环境可能给出8.3 AppData别名；仅解析自有下载根，随后Core仍逐组件核对实际普通文件。
    stagingRoot_ = ActualDirectory(stagingRoot_);
    if (stagingRoot_.isEmpty()) { Fail(QStringLiteral("UPDATE_STAGING_DIRECTORY"), tr("无法确认更新临时目录的本机身份。")); return false; }
    staging_ = std::make_unique<QTemporaryDir>(QDir(stagingRoot_).filePath(QStringLiteral("download-XXXXXX")));
    if (!staging_->isValid()) { Fail(QStringLiteral("UPDATE_STAGING_DIRECTORY"), tr("无法创建独立的更新下载目录。")); return false; }
    attempts_ = 0; Publish(UpdateState::Downloading, tr("正在下载并验证更新清单…"));
    if (status_.state != UpdateState::Downloading) return true;
    emit DownloadProgress(0, 0);
    if (status_.state == UpdateState::Downloading) {
        deadlineTimer_->start(loopback_ ? std::clamp(timeoutMs_ * 8, 1000, 30000) : 30 * 60 * 1000);
        Request(assets_[QStringLiteral("update-manifest.json")].url, RequestStage::Manifest);
    }
    return true;
}

void UpdateClient::FinishSignature() {
    const auto key = Update::ParseTrustedUpdateKeyHex(publicKeyHex_.toStdString());
    if (!key.Succeeded() || !key.value) { Fail(DiagnosticCode(key.diagnostics, QStringLiteral("UPDATE_TRUST_KEY")), DiagnosticMessage(key.diagnostics, tr("更新公钥配置无效。"))); return; }
    auto verified = Update::VerifyAndParseUpdateManifest(Bytes(manifestBytes_), Bytes(signatureBytes_), *key.value);
    if (!verified.Succeeded() || !verified.value) { Fail(DiagnosticCode(verified.diagnostics, QStringLiteral("UPDATE_SIGNATURE")), DiagnosticMessage(verified.diagnostics, tr("更新清单签名校验失败，不能安装。"))); return; }
    if (Text(verified.value->Manifest().version) != status_.availableVersion) { Fail(QStringLiteral("UPDATE_RELEASE_MANIFEST_VERSION"), tr("签名清单版本与刚检查的发布版本不同，请重新检查。")); return; }
    const auto check = Update::CheckUpdate(*verified.value, status_.currentVersion.toStdString(), checkPreferences_.channel);
    if (!check.Succeeded() || !check.value || !check.value->updateAvailable || !check.value->directlyUpgradeable) {
        Fail(DiagnosticCode(check.diagnostics, QStringLiteral("UPDATE_DIRECT_UPGRADE")), check.value ? Text(check.value->reason) : DiagnosticMessage(check.diagnostics, tr("当前版本不能直接升级到此包。"))); return;
    }
    const auto& manifest = verified.value->Manifest(); const auto filename = Text(manifest.packageFilename); const auto packageBytes = manifest.packageBytes;
    if (!assets_.contains(filename) || assets_[filename].bytes != static_cast<qint64>(manifest.packageBytes)) {
        Fail(QStringLiteral("UPDATE_PACKAGE_ASSET"), tr("发布中的更新包名称或大小与签名清单不符。")); return;
    }
    if (!WriteExact(staging_->filePath(QStringLiteral("update-manifest.json")), manifestBytes_)
        || !WriteExact(staging_->filePath(QStringLiteral("update-manifest.json.sig")), signatureBytes_)) {
        Fail(QStringLiteral("UPDATE_DOWNLOAD_WRITE"), tr("无法保存已验证的更新清单。")); return;
    }
    verifiedManifest_ = std::move(*verified.value);
    if (!OpenPackageFile()) return;
    Publish(UpdateState::Downloading, tr("正在下载版本 %1…").arg(status_.availableVersion));
    if (status_.state != UpdateState::Downloading) return;
    emit DownloadProgress(0, static_cast<qint64>(packageBytes));
    if (status_.state == UpdateState::Downloading) Request(assets_[filename].url, RequestStage::Package);
}

bool UpdateClient::OpenPackageFile() {
    if (packageFile_) packageFile_->cancelWriting();
    packageFile_ = std::make_unique<QSaveFile>(staging_->filePath(QStringLiteral("package.zip"))); packageFile_->setDirectWriteFallback(false);
    received_ = 0; attemptStart_ = 0;
    if (!packageFile_->open(QIODevice::WriteOnly)) { Fail(QStringLiteral("UPDATE_DOWNLOAD_WRITE"), tr("无法写入更新包临时文件。")); return false; }
    if (verifiedManifest_) emit DownloadProgress(0, static_cast<qint64>(verifiedManifest_->Manifest().packageBytes));
    return true;
}

void UpdateClient::RetryOrFail(const QString& code, const QString& message) {
    const bool package = stage_ == RequestStage::Package;
    if (package && received_ > attemptStart_) attempts_ = 0; // 本次有进展，只累计连续无进展的失败。
    if (attempts_ >= (package ? kPackageRetries : kMetadataRetries)) { Fail(code, message); return; }
    ++attempts_;
    ClearReply(); response_.clear();
    if (status_.state == UpdateState::Downloading)
        Publish(UpdateState::Downloading, tr("网络中断，正在重试（第 %1 次）…").arg(attempts_));
    if (status_.state != UpdateState::Downloading && status_.state != UpdateState::Checking) return;
    retryTimer_->start(loopback_ ? 10 : std::min(1000 << (attempts_ - 1), 8000));
}

void UpdateClient::StartPackageVerification() {
    deadlineTimer_->stop();
    Publish(UpdateState::Verifying, tr("正在校验更新包的大小、SHA-256 和程序文件清单…"));
    if (status_.state != UpdateState::Verifying || !verifiedManifest_ || !staging_) return;
    const auto path = staging_->filePath(QStringLiteral("package.zip")); const auto manifest = *verifiedManifest_;
    auto result = std::make_shared<Result<Update::UpdatePackageVerification>>();
    verifier_ = QThread::create([manifest, path, result] {
        try { *result = Update::VerifyUpdatePackage(manifest, std::filesystem::path(path.toStdWString())); }
        catch (...) { *result = Result<Update::UpdatePackageVerification>::Failure("QtUpdate", "UPDATE_PACKAGE_VERIFICATION", "更新包校验异常，不能安装。"); }
    });
    auto* worker = verifier_;
    connect(worker, &QThread::finished, this, [this, worker, result] {
        if (verifier_ != worker) return;
        verifier_ = nullptr;
        if (cancellingVerification_) {
            FinishCancelledVerification(); return;
        }
        if (!result->Succeeded() || !result->value) { Fail(DiagnosticCode(result->diagnostics, QStringLiteral("UPDATE_PACKAGE_VERIFICATION")), DiagnosticMessage(result->diagnostics, tr("更新包校验失败，不能安装。"))); return; }
        PreparedUpdate ready{staging_->filePath(QStringLiteral("package.zip")), staging_->filePath(QStringLiteral("update-manifest.json")),
            staging_->filePath(QStringLiteral("update-manifest.json.sig")), status_.availableVersion, Text(result->value->manifestSha256), Text(result->value->sha256)};
        prepared_ = ready; status_.canDownload = false;
        Publish(UpdateState::Ready, tr("更新包已通过签名和文件校验，可以安装并重启。"));
        if (status_.state == UpdateState::Ready && prepared_) emit PackagePrepared(ready);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

bool UpdateClient::RetainPreparedForInstaller(const PreparedUpdate& update, QString* error) {
    if (error) error->clear();
    if (status_.state != UpdateState::Ready || !prepared_ || !staging_ || !staging_->isValid() || *prepared_ != update) {
        if (error) *error = tr("安装请求与本客户端当前已验证的更新包身份不一致，未保留临时文件。");
        return false;
    }
    staging_->setAutoRemove(false);
    return true;
}

} // namespace War3Map::QtUpdate
