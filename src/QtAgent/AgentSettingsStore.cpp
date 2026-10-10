#include "AgentSettingsStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <cmath>
#include <utility>

namespace War3Map::Agent {
namespace {

constexpr qint64 kMaximumSettingsBytes = 2 * 1024 * 1024;
constexpr qint64 kMaximumTokens = 999999999999999;

bool Fail(QString* error, const QString& message) {
    if (error != nullptr) *error = message;
    return false;
}

bool ReadBool(const QJsonObject& object, const QString& name, bool fallback, bool& result) {
    const auto value = object.value(name);
    if (value.isUndefined()) { result = fallback; return true; }
    if (!value.isBool()) return false;
    result = value.toBool();
    return true;
}

bool ReadTokens(const QJsonObject& object, const QString& name, qint64& result) {
    const auto value = object.value(name);
    if (value.isUndefined()) { result = 0; return true; }
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > static_cast<double>(kMaximumTokens)
        || std::floor(number) != number) return false;
    result = value.toInteger();
    return true;
}

bool ReadText(const QJsonObject& object, const QString& name, QString& result) {
    const auto value = object.value(name);
    if (!value.isUndefined() && !value.isString()) return false;
    result = value.toString();
    return result.size() <= 16384;
}

bool ContainsSecretField(const QJsonObject& object) {
    for (const auto& key : object.keys()) {
        const auto lower = key.toLower();
        if (lower == QStringLiteral("api_key") || lower == QStringLiteral("apikey")
            || lower == QStringLiteral("secret") || lower == QStringLiteral("password")
            || lower == QStringLiteral("authorization") || lower == QStringLiteral("token")) return true;
    }
    return false;
}

QJsonObject EncodeModel(const ModelSettings& model) {
    return {{QStringLiteral("id"), model.id}, {QStringLiteral("name"), model.name},
        {QStringLiteral("enabled"), model.enabled}, {QStringLiteral("context_override"), model.contextOverride},
        {QStringLiteral("provider_context"), model.providerContext}, {QStringLiteral("catalog_context"), model.catalogContext},
        {QStringLiteral("max_output_tokens"), model.maxOutputTokens}, {QStringLiteral("tools_supported"), model.toolsSupported},
        {QStringLiteral("images_supported"), model.imagesSupported}};
}

bool DecodeModel(const QJsonObject& object, ModelSettings& model) {
    return !ContainsSecretField(object) && ReadText(object, QStringLiteral("id"), model.id)
        && ReadText(object, QStringLiteral("name"), model.name)
        && ReadBool(object, QStringLiteral("enabled"), true, model.enabled)
        && ReadBool(object, QStringLiteral("tools_supported"), true, model.toolsSupported)
        && ReadBool(object, QStringLiteral("images_supported"), false, model.imagesSupported)
        && ReadTokens(object, QStringLiteral("context_override"), model.contextOverride)
        && ReadTokens(object, QStringLiteral("provider_context"), model.providerContext)
        && ReadTokens(object, QStringLiteral("catalog_context"), model.catalogContext)
        && ReadTokens(object, QStringLiteral("max_output_tokens"), model.maxOutputTokens);
}

} // namespace

AgentSettingsStore::AgentSettingsStore(QString filePath)
    : filePath_(filePath.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
            .filePath(QStringLiteral("agent/settings.json")) : std::move(filePath)) {}

QString AgentSettingsStore::FilePath() const { return filePath_; }

bool AgentSettingsStore::Validate(const AgentSettings& settings, QString* error) {
    if (error != nullptr) error->clear();
    if (settings.mcpPort == 0) return Fail(error, QStringLiteral("MCP 端口必须在 1–65535 之间。"));
    if (settings.providers.size() > 128) return Fail(error, QStringLiteral("提供商设置数量超过读取预算。"));
    QSet<QString> providerIds;
    for (const auto& provider : settings.providers) {
        if (provider.id.isEmpty() || provider.id.size() > 16384 || providerIds.contains(provider.id))
            return Fail(error, QStringLiteral("提供商身份为空或重复。"));
        providerIds.insert(provider.id);
        if (provider.name.size() > 16384 || provider.credentialRef.size() > 16384 || provider.website.size() > 16384)
            return Fail(error, QStringLiteral("提供商文本超过长度预算。"));
        if (provider.protocol != QStringLiteral("openai-chat") && provider.protocol != QStringLiteral("openai-responses")
            && provider.protocol != QStringLiteral("anthropic") && provider.protocol != QStringLiteral("gemini"))
            return Fail(error, QStringLiteral("不支持所选 API 协议。"));
        if (!provider.baseUrl.isEmpty()) {
            const QUrl url(provider.baseUrl);
            if (provider.baseUrl.size() > 16384 || !url.isValid() || url.host().isEmpty()
                || (url.scheme() != QStringLiteral("https") && (url.scheme() != QStringLiteral("http")
                    || (url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0 && !QHostAddress(url.host()).isLoopback())))
                || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
                return Fail(error, QStringLiteral("API 地址须为 HTTP(S) 基础地址，不能包含用户名、密钥参数或片段。"));
        }
        if (!provider.website.isEmpty()) {
            const QUrl url(provider.website);
            if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
                || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")))
                return Fail(error, QStringLiteral("提供商网站须为有效的 HTTP(S) 地址。"));
        }
        if (provider.models.size() > 10000) return Fail(error, QStringLiteral("模型设置数量超过读取预算。"));
        QSet<QString> modelIds;
        for (const auto& model : provider.models) {
            if (model.id.isEmpty() || model.id.size() > 16384 || model.name.size() > 16384 || modelIds.contains(model.id))
                return Fail(error, QStringLiteral("同一提供商的模型 ID 为空、重复或过长。"));
            modelIds.insert(model.id);
            for (const qint64 count : {model.contextOverride, model.providerContext, model.catalogContext, model.maxOutputTokens}) {
                if (count < 0 || count > kMaximumTokens)
                    return Fail(error, QStringLiteral("模型 token 数须为非负整数，不能超过 15 位。"));
            }
        }
    }
    QSet<QString> tools;
    for (const auto& tool : settings.disabledTools) {
        if (tool.isEmpty() || tool.size() > 16384 || tools.contains(tool))
            return Fail(error, QStringLiteral("工具开关中存在空名称、重复名称或过长名称。"));
        tools.insert(tool);
    }
    return true;
}

AgentSettings AgentSettingsStore::Load(QString* error) const {
    if (error != nullptr) error->clear();
    AgentSettings settings;
    if (!QFileInfo::exists(filePath_)) return settings;
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumSettingsBytes) {
        Fail(error, QStringLiteral("无法读取 AI 设置，或设置文件超过 2 MiB；原文件保留。"));
        return {};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        Fail(error, QStringLiteral("AI 设置 JSON 损坏；原文件保留。"));
        return {};
    }
    const auto object = document.object();
    if (ContainsSecretField(object) || object.value(QStringLiteral("version")).toInt(-1) != 1) {
        Fail(error, QStringLiteral("AI 设置版本不支持，或包含明文凭据字段；不会导入密钥。"));
        return {};
    }
    if (!ReadBool(object, QStringLiteral("ai_enabled"), false, settings.aiEnabled)
        || !ReadBool(object, QStringLiteral("reduced_motion"), false, settings.reducedMotion)
        || !ReadBool(object, QStringLiteral("allow_edits"), false, settings.allowEdits)
        || !ReadBool(object, QStringLiteral("allow_save"), false, settings.allowSave)
        || !ReadBool(object, QStringLiteral("allow_commands"), false, settings.allowCommands)
        || !ReadBool(object, QStringLiteral("mcp_enabled"), false, settings.mcpEnabled)) {
        Fail(error, QStringLiteral("AI 设置中的开关类型无效。")); return {};
    }
    const auto portValue = object.value(QStringLiteral("mcp_port"));
    if (!portValue.isUndefined()) {
        const double port = portValue.toDouble(-1);
        if (!portValue.isDouble() || port < 1 || port > 65535 || std::floor(port) != port) {
            Fail(error, QStringLiteral("MCP 端口必须为 1–65535 的整数。")); return {};
        }
        settings.mcpPort = static_cast<quint16>(port);
    }
    const auto providersValue = object.value(QStringLiteral("providers"));
    if (!providersValue.isUndefined() && !providersValue.isArray()) {
        Fail(error, QStringLiteral("提供商设置必须为列表。")); return {};
    }
    for (const auto& providerValue : providersValue.toArray()) {
        if (!providerValue.isObject()) { Fail(error, QStringLiteral("提供商设置条目无效。")); return {}; }
        const auto encoded = providerValue.toObject();
        ProviderSettings provider;
        if (ContainsSecretField(encoded) || !ReadText(encoded, QStringLiteral("id"), provider.id)
            || !ReadText(encoded, QStringLiteral("name"), provider.name)
            || !ReadText(encoded, QStringLiteral("website"), provider.website)
            || !ReadText(encoded, QStringLiteral("base_url"), provider.baseUrl)
            || !ReadText(encoded, QStringLiteral("credential_ref"), provider.credentialRef)
            || !ReadBool(encoded, QStringLiteral("enabled"), true, provider.enabled)) {
            Fail(error, QStringLiteral("提供商字段无效，或包含明文凭据字段。")); return {};
        }
        if (encoded.contains(QStringLiteral("protocol")) && !ReadText(encoded, QStringLiteral("protocol"), provider.protocol)) {
            Fail(error, QStringLiteral("提供商协议字段无效。")); return {};
        }
        const auto modelsValue = encoded.value(QStringLiteral("models"));
        if (!modelsValue.isUndefined() && !modelsValue.isArray()) {
            Fail(error, QStringLiteral("模型设置必须为列表。")); return {};
        }
        for (const auto& modelValue : modelsValue.toArray()) {
            ModelSettings model;
            if (!modelValue.isObject() || !DecodeModel(modelValue.toObject(), model)) {
                Fail(error, QStringLiteral("模型字段或 token 数无效。")); return {};
            }
            provider.models.push_back(std::move(model));
        }
        settings.providers.push_back(std::move(provider));
    }
    const auto disabled = object.value(QStringLiteral("disabled_tools"));
    if (!disabled.isUndefined() && !disabled.isArray()) {
        Fail(error, QStringLiteral("工具开关必须为列表。")); return {};
    }
    for (const auto& tool : disabled.toArray()) {
        if (!tool.isString()) { Fail(error, QStringLiteral("工具名称必须为文本。")); return {}; }
        settings.disabledTools.push_back(tool.toString());
    }
    if (!Validate(settings, error)) return {};
    return settings;
}

bool AgentSettingsStore::Save(const AgentSettings& settings, QString* error) const {
    if (!Validate(settings, error)) return false;
    QJsonArray providers;
    for (const auto& provider : settings.providers) {
        QJsonArray models;
        for (const auto& model : provider.models) models.append(EncodeModel(model));
        providers.append(QJsonObject{{QStringLiteral("id"), provider.id}, {QStringLiteral("name"), provider.name},
            {QStringLiteral("website"), provider.website}, {QStringLiteral("base_url"), provider.baseUrl},
            {QStringLiteral("protocol"), provider.protocol}, {QStringLiteral("credential_ref"), provider.credentialRef},
            {QStringLiteral("enabled"), provider.enabled}, {QStringLiteral("models"), models}});
    }
    QJsonArray disabled;
    for (const auto& tool : settings.disabledTools) disabled.append(tool);
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("ai_enabled"), settings.aiEnabled}, {QStringLiteral("reduced_motion"), settings.reducedMotion},
        {QStringLiteral("providers"), providers}, {QStringLiteral("allow_edits"), settings.allowEdits},
        {QStringLiteral("allow_save"), settings.allowSave}, {QStringLiteral("allow_commands"), settings.allowCommands},
        {QStringLiteral("mcp_enabled"), settings.mcpEnabled}, {QStringLiteral("mcp_port"), settings.mcpPort},
        {QStringLiteral("disabled_tools"), disabled}}).toJson(QJsonDocument::Indented);
    if (bytes.size() > kMaximumSettingsBytes) return Fail(error, QStringLiteral("设置超过 2 MiB，未写入。"));
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return Fail(error, QStringLiteral("无法创建 AI 设置目录。"));
    QSaveFile file(filePath_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return Fail(error, QStringLiteral("无法原子写入 AI 设置；上一份设置保留。"));
    return true;
}

} // namespace War3Map::Agent
