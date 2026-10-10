#include "ApplicationSettingsTransfer.h"
#include "AgentSettingsStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <utility>

namespace War3Map::Agent {
namespace {
bool Fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}
bool TextValid(const QString& text, qsizetype limit) {
    return text.size() <= limit && std::none_of(text.begin(), text.end(), [](QChar value) {
        return value.unicode() < 0x20 || value.unicode() == 0x7f;
    });
}
bool ExactKeys(const QJsonObject& object, const QStringList& keys) {
    return object.size() == keys.size() && std::all_of(keys.begin(), keys.end(), [&](const QString& key) { return object.contains(key); });
}
bool Text(const QJsonObject& object, const char* key, QString& result, qsizetype limit = 16384) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || !TextValid(value.toString(), limit)) return false;
    result = value.toString(); return true;
}
bool Boolean(const QJsonObject& object, const char* key, bool& result) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isBool()) return false;
    result = value.toBool(); return true;
}
bool Tokens(const QJsonObject& object, const char* key, qint64& result) {
    const auto value = object.value(QLatin1String(key)); const auto count = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(count) || count < 0 || count > 999999999999999.0 || std::floor(count) != count) return false;
    result = value.toInteger(); return true;
}
bool Validate(const ApplicationPreferences& application, const QList<ProviderSettings>& providers, QString* error) {
    if (!TextValid(application.gameRoot, 16384) || !TextValid(application.uiDataRoot, 16384)
        || !TextValid(application.uiFontFamily, 256) || !TextValid(application.themeId, 128))
        return Fail(error, QStringLiteral("目录、字体或主题字段过长，或含控制字符。"));
    AgentSettings check; check.providers = providers;
    if (!AgentSettingsStore::Validate(check, error)) return false;
    qsizetype models = 0;
    for (const auto& provider : providers) {
        models += provider.models.size();
        for (const auto& text : {provider.id, provider.name, provider.website, provider.baseUrl, provider.protocol})
            if (!TextValid(text, 16384)) return Fail(error, QStringLiteral("提供商字段过长或含控制字符。"));
        for (const auto& model : provider.models)
            if (!TextValid(model.id, 16384) || !TextValid(model.name, 16384))
                return Fail(error, QStringLiteral("模型名称过长或含控制字符。"));
        // 网站链接也不承载查询参数/片段，避免把密钥URL当作普通描述导出。
        const QUrl site(provider.website);
        if (site.hasQuery() || site.hasFragment() || !site.userInfo().isEmpty())
            return Fail(error, QStringLiteral("迁移文件中的提供商地址不能包含身份信息、查询参数或片段。"));
    }
    if (models > 10000) return Fail(error, QStringLiteral("迁移文件最多包含10000个模型。"));
    return true;
}
QJsonObject Model(const ModelSettings& model) {
    return {{"id", model.id}, {"name", model.name}, {"enabled", model.enabled}, {"context_override", model.contextOverride},
        {"provider_context", model.providerContext}, {"catalog_context", model.catalogContext}, {"max_output_tokens", model.maxOutputTokens},
        {"tools_supported", model.toolsSupported}, {"images_supported", model.imagesSupported}};
}
bool Model(const QJsonObject& object, ModelSettings& model) {
    return ExactKeys(object, {"id", "name", "enabled", "context_override", "provider_context", "catalog_context", "max_output_tokens", "tools_supported", "images_supported"})
        && Text(object, "id", model.id) && Text(object, "name", model.name) && Boolean(object, "enabled", model.enabled)
        && Tokens(object, "context_override", model.contextOverride) && Tokens(object, "provider_context", model.providerContext)
        && Tokens(object, "catalog_context", model.catalogContext) && Tokens(object, "max_output_tokens", model.maxOutputTokens)
        && Boolean(object, "tools_supported", model.toolsSupported) && Boolean(object, "images_supported", model.imagesSupported);
}
} // namespace

QByteArray ApplicationSettingsTransfer::Encode(const ApplicationPreferences& application, const AgentSettings& settings, QString* error) {
    if (error) error->clear();
    if (!Validate(application, settings.providers, error)) return {};
    QJsonArray providers;
    for (const auto& provider : settings.providers) {
        QJsonArray models;
        for (const auto& model : provider.models) models.append(Model(model));
        providers.append(QJsonObject{{"id", provider.id}, {"name", provider.name}, {"website", provider.website},
            {"base_url", provider.baseUrl}, {"protocol", provider.protocol}, {"enabled", provider.enabled}, {"models", models}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"format", "audastra.application-settings"}, {"version", 1},
        {"application", QJsonObject{{"game_root", application.gameRoot}, {"ui_data_root", application.uiDataRoot},
            {"ui_font_family", application.uiFontFamily}, {"theme", application.themeId}}}, {"providers", providers}}).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumBytes) { Fail(error, QStringLiteral("设置迁移文件超过2MiB，未导出。")); return {}; }
    return bytes;
}

std::optional<ApplicationSettingsImport> ApplicationSettingsTransfer::Decode(const QByteArray& bytes, QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& message) -> std::optional<ApplicationSettingsImport> { Fail(error, message); return std::nullopt; };
    if (bytes.isEmpty() || bytes.size() > MaximumBytes) return fail(QStringLiteral("设置迁移文件为空或超过2MiB。"));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return fail(QStringLiteral("设置迁移文件不是有效的JSON对象。"));
    const auto root = document.object();
    if (!ExactKeys(root, {"format", "version", "application", "providers"})
        || root.value("format").toString() != QStringLiteral("audastra.application-settings")
        || !root.value("version").isDouble() || root.value("version").toDouble() != 1)
        return fail(QStringLiteral("设置迁移版本不支持，或含未允许字段；不会导入密钥、凭据引用、权限或MCP配置。"));
    ApplicationSettingsImport result;
    const auto application = root.value("application").toObject();
    if (!root.value("application").isObject() || !ExactKeys(application, {"game_root", "ui_data_root", "ui_font_family", "theme"})
        || !Text(application, "game_root", result.application.gameRoot) || !Text(application, "ui_data_root", result.application.uiDataRoot)
        || !Text(application, "ui_font_family", result.application.uiFontFamily, 256) || !Text(application, "theme", result.application.themeId, 128))
        return fail(QStringLiteral("应用设置字段无效或包含未允许字段。"));
    if (!root.value("providers").isArray() || root.value("providers").toArray().size() > 128)
        return fail(QStringLiteral("提供商必须是至多128项的列表。"));
    qsizetype models = 0;
    for (const auto& entry : root.value("providers").toArray()) {
        const auto object = entry.toObject(); ProviderSettings provider;
        if (!entry.isObject() || !ExactKeys(object, {"id", "name", "website", "base_url", "protocol", "enabled", "models"})
            || !Text(object, "id", provider.id) || !Text(object, "name", provider.name) || !Text(object, "website", provider.website)
            || !Text(object, "base_url", provider.baseUrl) || !Text(object, "protocol", provider.protocol) || !Boolean(object, "enabled", provider.enabled)
            || !object.value("models").isArray()) return fail(QStringLiteral("提供商字段无效，或含密钥、凭据引用等未允许字段。"));
        models += object.value("models").toArray().size();
        if (models > 10000) return fail(QStringLiteral("迁移文件最多包含10000个模型。"));
        for (const auto& item : object.value("models").toArray()) {
            ModelSettings model;
            if (!item.isObject() || !Model(item.toObject(), model)) return fail(QStringLiteral("模型字段、类型或token数量无效，或含未允许字段。"));
            provider.models.append(std::move(model));
        }
        result.providers.append(std::move(provider));
    }
    if (!Validate(result.application, result.providers, error)) return std::nullopt;
    return result;
}

AgentSettings ApplicationSettingsTransfer::MergeProviders(const AgentSettings& local, const QList<ProviderSettings>& imported) {
    auto result = local; result.providers = imported;
    for (auto& provider : result.providers) {
        provider.credentialRef.clear();
        const auto matching = std::find_if(local.providers.begin(), local.providers.end(), [&](const auto& previous) {
            return previous.id == provider.id && previous.baseUrl == provider.baseUrl && previous.protocol == provider.protocol;
        });
        if (matching != local.providers.end()) provider.credentialRef = matching->credentialRef;
    }
    return result;
}

} // namespace War3Map::Agent
