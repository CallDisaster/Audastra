#include "ProviderCatalog.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <utility>

namespace War3Map::Agent {
namespace {

// 模型目录协议依据：
// https://developers.openai.com/api/reference/resources/models/methods/list
// https://platform.claude.com/docs/en/api/models/list
// https://ai.google.dev/api/models
// https://ai.google.dev/gemini-api/docs/api-key （x-goog-api-key）

constexpr qsizetype kPageBudget = 2 * 1024 * 1024;
constexpr qsizetype kTotalBudget = 8 * 1024 * 1024;
constexpr qsizetype kModelBudget = 10000;
constexpr int kPageCountBudget = 64;

qint64 PositiveTokens(const QJsonValue& value) {
    const double number = value.toDouble(0);
    return value.isDouble() && std::isfinite(number) && number > 0 && number <= 999999999999999.0
        && std::floor(number) == number ? value.toInteger() : 0;
}

bool IsLocal(const QUrl& url) {
    return url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
        || QHostAddress(url.host()).isLoopback();
}

} // namespace

ProviderCatalog::ProviderCatalog(QObject* parent) : QObject(parent) {
    network_ = new QNetworkAccessManager(this);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { Fail(tr("获取模型超时，已取消请求；现有模型设置保留。")); });
}

ProviderCatalog::~ProviderCatalog() { Clear(); }

bool ProviderCatalog::IsBusy() const { return busy_; }

QUrl ProviderCatalog::Endpoint(const ProviderSettings& provider, QString* error) {
    if (error != nullptr) error->clear();
    QUrl url(provider.baseUrl);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || (url.scheme() != QStringLiteral("https") && (url.scheme() != QStringLiteral("http") || !IsLocal(url)))) {
        if (error != nullptr) *error = tr("请填写 HTTPS API 基础地址；本机接口可使用 HTTP，地址不能包含凭据参数。");
        return {};
    }
    if (provider.protocol != QStringLiteral("openai-chat") && provider.protocol != QStringLiteral("openai-responses")
        && provider.protocol != QStringLiteral("anthropic") && provider.protocol != QStringLiteral("gemini")) {
        if (error != nullptr) *error = tr("此 API 协议尚未提供模型目录接口，可手工添加模型 ID。");
        return {};
    }
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path.isEmpty()) path = provider.protocol == QStringLiteral("gemini") ? QStringLiteral("/v1beta") : QStringLiteral("/v1");
    if (!path.endsWith(QStringLiteral("/models"))) path += QStringLiteral("/models");
    url.setPath(path);
    return url;
}

void ProviderCatalog::Fetch(const ProviderSettings& provider, const QString& secret, int timeoutMs) {
    if (busy_) Cancel();
    QString error;
    const auto endpoint = Endpoint(provider, &error);
    if (endpoint.isEmpty()) { emit Failed(provider.id, error); return; }
    for (const auto character : secret) {
        if (character.unicode() < 32 || character.unicode() == 127) {
            emit Failed(provider.id, tr("密钥含控制字符，请重新填写。")); return;
        }
    }
    if (provider.id.isEmpty() || secret.size() > 32768) {
        emit Failed(provider.id, tr("提供商身份或密钥长度无效。")); return;
    }
    provider_ = provider;
    endpoint_ = endpoint;
    secret_ = secret;
    totalBytes_ = 0;
    pages_ = 0;
    busy_ = true;
    timer_->start(std::clamp(timeoutMs, 1, 120000));
    emit BusyChanged(true);
    RequestPage();
}

void ProviderCatalog::RequestPage(const QString& cursor) {
    if (++pages_ > kPageCountBudget) { Fail(tr("站点模型分页超过 64 页，停止读取。")); return; }
    QUrl url = endpoint_;
    QUrlQuery query;
    if (provider_.protocol == QStringLiteral("anthropic")) {
        query.addQueryItem(QStringLiteral("limit"), QStringLiteral("1000"));
        if (!cursor.isEmpty()) query.addQueryItem(QStringLiteral("after_id"), cursor);
    } else if (provider_.protocol == QStringLiteral("gemini")) {
        query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("1000"));
        if (!cursor.isEmpty()) query.addQueryItem(QStringLiteral("pageToken"), cursor);
    } else if (!cursor.isEmpty()) {
        query.addQueryItem(QStringLiteral("after"), cursor);
    }
    url.setQuery(query);
    QNetworkRequest request(url);
    // 禁止携带 Authorization 自动跟随重定向；站点地址变化需用户明确更新。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    if (provider_.protocol == QStringLiteral("anthropic")) {
        request.setRawHeader("anthropic-version", "2023-06-01");
        if (!secret_.isEmpty()) request.setRawHeader("x-api-key", secret_.toUtf8());
    } else if (provider_.protocol == QStringLiteral("gemini")) {
        // 官方支持头部认证，避免 key 出现在 URL、代理历史或错误文本中。
        if (!secret_.isEmpty()) request.setRawHeader("x-goog-api-key", secret_.toUtf8());
    } else if (!secret_.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + secret_.toUtf8());
    }
    response_.clear();
    reply_ = network_->get(request);
    reply_->setReadBufferSize(kPageBudget);
    auto* reply = reply_;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { Consume(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { FinishPage(reply); });
}

void ProviderCatalog::Consume(QNetworkReply* reply) {
    if (!busy_ || reply != reply_) return;
    const auto bytes = reply->readAll();
    totalBytes_ += bytes.size();
    response_.append(bytes);
    if (response_.size() > kPageBudget || totalBytes_ > kTotalBudget)
        Fail(tr("模型目录超过读取预算；已取消请求，现有设置保留。"));
}

void ProviderCatalog::FinishPage(QNetworkReply* reply) {
    if (!busy_ || reply != reply_) return;
    Consume(reply);
    if (!busy_ || reply != reply_) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300 || reply->error() != QNetworkReply::NoError) {
        // 不回显响应正文或站点 error 字段，它们可能含站点回传的凭据。
        const QString message = status > 0
            ? tr("站点返回 HTTP %1；请检查 API 地址、协议和密钥。现有模型设置保留。").arg(status)
            : tr("无法连接站点或验证 TLS；现有模型设置保留。");
        Fail(message);
        return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(response_, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        Fail(tr("站点返回的模型目录不是有效 JSON。")); return;
    }
    const auto object = document.object();
    const bool gemini = provider_.protocol == QStringLiteral("gemini");
    const auto arrayValue = object.value(gemini ? QStringLiteral("models") : QStringLiteral("data"));
    if (!arrayValue.isArray()) { Fail(tr("模型目录缺少协议要求的列表字段。")); return; }
    for (const auto& value : arrayValue.toArray()) {
        if (!value.isObject()) { Fail(tr("模型目录包含无效条目。")); return; }
        const auto entry = value.toObject();
        ModelSettings model;
        model.id = entry.value(gemini ? QStringLiteral("name") : QStringLiteral("id")).toString();
        if (gemini && model.id.startsWith(QStringLiteral("models/"))) model.id.remove(0, 7);
        if (model.id.isEmpty() || model.id.size() > 16384) { Fail(tr("模型目录包含空 ID 或过长 ID。")); return; }
        if (modelIds_.contains(model.id)) continue;
        if (gemini) {
            const auto methods = entry.value(QStringLiteral("supportedGenerationMethods")).toArray();
            bool generation = false;
            for (const auto& method : methods) if (method.toString() == QStringLiteral("generateContent")) generation = true;
            if (!generation) continue;
            model.name = entry.value(QStringLiteral("displayName")).toString(model.id);
            model.providerContext = PositiveTokens(entry.value(QStringLiteral("inputTokenLimit")));
            model.maxOutputTokens = PositiveTokens(entry.value(QStringLiteral("outputTokenLimit")));
        } else {
            model.name = entry.value(QStringLiteral("display_name")).toString(entry.value(QStringLiteral("name")).toString(model.id));
            model.providerContext = PositiveTokens(entry.value(QStringLiteral("context_window")));
            if (model.providerContext == 0) model.providerContext = PositiveTokens(entry.value(QStringLiteral("context_length")));
            if (model.providerContext == 0) model.providerContext = PositiveTokens(entry.value(QStringLiteral("max_input_tokens")));
            model.maxOutputTokens = PositiveTokens(entry.value(QStringLiteral("max_tokens")));
            const auto imageCapability = entry.value(QStringLiteral("capabilities")).toObject()
                .value(QStringLiteral("image_input")).toObject().value(QStringLiteral("supported"));
            if (imageCapability.isBool()) model.imagesSupported = imageCapability.toBool();
        }
        model.enabled = false; // 读取到模型后由作者勾选；不自动启用整个站点。
        modelIds_.insert(model.id);
        models_.push_back(std::move(model));
        if (models_.size() > kModelBudget) { Fail(tr("模型目录超过 10000 项，停止读取。")); return; }
    }
    QString cursor;
    if (gemini) cursor = object.value(QStringLiteral("nextPageToken")).toString();
    else if (object.value(QStringLiteral("has_more")).toBool()) {
        cursor = object.value(QStringLiteral("last_id")).toString();
        if (cursor.isEmpty()) { Fail(tr("模型分页声称仍有后页，但缺少下一页身份。")); return; }
    }
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    reply_ = nullptr;
    if (!cursor.isEmpty()) {
        if (cursor.size() > 16384 || cursors_.contains(cursor)) { Fail(tr("模型分页游标重复或过长，停止读取。")); return; }
        cursors_.insert(cursor);
        RequestPage(cursor);
        return;
    }
    std::sort(models_.begin(), models_.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto id = provider_.id;
    const auto result = models_;
    Clear();
    emit BusyChanged(false);
    emit ModelsReady(id, result);
}

void ProviderCatalog::Fail(const QString& message) {
    if (!busy_) return;
    const auto id = provider_.id;
    Clear();
    emit BusyChanged(false);
    emit Failed(id, message);
}

void ProviderCatalog::Cancel() {
    if (busy_) Fail(tr("已取消获取模型；现有设置保留。"));
}

void ProviderCatalog::Clear() {
    timer_->stop();
    if (reply_ != nullptr) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    secret_.fill(QChar(0));
    secret_.clear();
    response_.clear();
    models_.clear();
    modelIds_.clear();
    cursors_.clear();
    busy_ = false;
}

} // namespace War3Map::Agent
