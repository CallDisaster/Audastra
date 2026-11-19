#include "AgentTypes.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace War3Map::Agent {
namespace {

QString StripVendorPrefix(QString key) {
    key = key.trimmed().toLower();
    const int slash = key.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0 && slash + 1 < key.size()) key = key.mid(slash + 1);
    return key;
}

QString CanonicalSlug(const QString& raw) {
    QString slug = StripVendorPrefix(raw);
    slug.replace(QLatin1Char('_'), QLatin1Char('-'));
    slug.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("-"));
    return slug;
}

bool MatchCatalogLevels(const QString& slug, const QString& protocol, const QString& baseUrl,
    QStringList& outLevels, QString& outDefault) {
    if (slug.isEmpty()) return false;
    const QString host = baseUrl.trimmed().toLower();
    const bool isDeepSeekRoute = host.contains(QStringLiteral("deepseek")) || slug.contains(QStringLiteral("deepseek"));

    // 1. DeepSeek 系列（对齐 DSH llm-deepseek/model-info.ts 与 pi-ai deepseek.json）
    if (isDeepSeekRoute) {
        if (slug.contains(QStringLiteral("v4-pro")) || slug.contains(QStringLiteral("v4.1-pro"))) {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")};
            outDefault = QStringLiteral("high");
            return true;
        }
        if (slug.contains(QStringLiteral("v4")) || slug.contains(QStringLiteral("deepseek-flash"))
            || (slug.contains(QStringLiteral("deepseek")) && slug.contains(QStringLiteral("flash")))) {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")};
            outDefault = QStringLiteral("high");
            return true;
        }
        if (slug.contains(QStringLiteral("reasoner")) || slug.contains(QStringLiteral("deepseek-r1"))
            || slug == QStringLiteral("r1") || slug.startsWith(QStringLiteral("r1-"))) {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")};
            outDefault = QStringLiteral("high");
            return true;
        }
        if (slug == QStringLiteral("deepseek-chat") || slug.contains(QStringLiteral("v3.2"))
            || slug.contains(QStringLiteral("v3-2"))) {
            outLevels = {QStringLiteral("off"), QStringLiteral("high")};
            return true;
        }
        if (slug.contains(QStringLiteral("v3")) || slug.contains(QStringLiteral("coder"))) {
            outLevels.clear();
            return true; // 已知早期非思考模型
        }
    }

    // 2. OpenAI / Codex 系列（对齐 pi-ai openai.json）
    if (slug.startsWith(QStringLiteral("gpt-6")) || slug.startsWith(QStringLiteral("gpt-5.6"))
        || slug.startsWith(QStringLiteral("gpt-daybreak")) || slug.startsWith(QStringLiteral("daybreak-"))) {
        if (slug.contains(QStringLiteral("-pro")) || slug.contains(QStringLiteral("astra")) || slug.contains(QStringLiteral("6.1-sol"))) {
            outLevels = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max")};
        } else {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max")};
        }
        return true;
    }
    if (slug.startsWith(QStringLiteral("gpt-5.2")) || slug.startsWith(QStringLiteral("gpt-5.3"))
        || slug.startsWith(QStringLiteral("gpt-5.4")) || slug.startsWith(QStringLiteral("gpt-5.5"))) {
        if (slug.contains(QStringLiteral("-pro"))) {
            outLevels = {QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh")};
        } else if (slug.contains(QStringLiteral("codex-spark"))) {
            outLevels = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh")};
        } else {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh")};
        }
        return true;
    }
    if (slug.startsWith(QStringLiteral("gpt-5.1"))) {
        outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
        return true;
    }
    if (slug == QStringLiteral("gpt-5") || slug.startsWith(QStringLiteral("gpt-5-"))) {
        if (slug.contains(QStringLiteral("chat"))) {
            outLevels.clear();
        } else if (slug.contains(QStringLiteral("-pro"))) {
            outLevels = {QStringLiteral("high")};
        } else {
            outLevels = {QStringLiteral("minimal"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
        }
        return true;
    }
    if (slug == QStringLiteral("o1") || slug.startsWith(QStringLiteral("o1-"))
        || slug == QStringLiteral("o3") || slug.startsWith(QStringLiteral("o3-"))
        || slug == QStringLiteral("o4") || slug.startsWith(QStringLiteral("o4-"))) {
        outLevels = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
        return true;
    }
    if (slug.startsWith(QStringLiteral("gpt-4")) || slug.startsWith(QStringLiteral("gpt-3.5"))) {
        outLevels.clear();
        return true;
    }

    // 3. Anthropic Claude 系列（对齐 pi-ai anthropic.json）
    if (slug.startsWith(QStringLiteral("claude-")) || slug.startsWith(QStringLiteral("fable-"))) {
        if (slug.contains(QStringLiteral("3-5-")) || slug.contains(QStringLiteral("3.5-"))
            || slug.contains(QStringLiteral("3-haiku")) || slug.contains(QStringLiteral("3-opus"))
            || slug.contains(QStringLiteral("3-sonnet"))) {
            outLevels.clear();
            return true;
        }
        if (slug.contains(QStringLiteral("5-5")) || slug.contains(QStringLiteral("5.5"))
            || slug.contains(QStringLiteral("fable")) || slug.contains(QStringLiteral("opus-5"))
            || slug.contains(QStringLiteral("sonnet-5")) || slug.contains(QStringLiteral("opus-4-7"))
            || slug.contains(QStringLiteral("opus-4.7")) || slug.contains(QStringLiteral("opus-4-8"))
            || slug.contains(QStringLiteral("opus-4.8"))) {
            outLevels = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max")};
            return true;
        }
        if (slug.contains(QStringLiteral("4-6")) || slug.contains(QStringLiteral("4.6"))) {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("max")};
            return true;
        }
        outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
        return true;
    }

    // 4. Google Gemini / Gemma 系列（对齐 pi-ai google.json）
    if (slug.startsWith(QStringLiteral("gemini-")) || slug.startsWith(QStringLiteral("gemma-4"))
        || slug.startsWith(QStringLiteral("gemma4"))) {
        if (slug.startsWith(QStringLiteral("gemini-1.")) || (slug.startsWith(QStringLiteral("gemini-2.0")) && !slug.contains(QStringLiteral("thinking")))) {
            outLevels.clear();
            return true;
        }
        if (slug.startsWith(QStringLiteral("gemma"))) {
            outLevels = {QStringLiteral("minimal"), QStringLiteral("high")};
            return true;
        }
        if (slug.startsWith(QStringLiteral("gemini-3")) && (slug.contains(QStringLiteral("pro")) || slug.startsWith(QStringLiteral("gemini-3.7")) || slug.startsWith(QStringLiteral("gemini-3.8")))) {
            outLevels = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
            return true;
        }
        if (slug.startsWith(QStringLiteral("gemini-3")) || slug == QStringLiteral("gemini-flash-latest") || slug == QStringLiteral("gemini-flash-lite-latest")) {
            outLevels = {QStringLiteral("minimal"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
            return true;
        }
        if (slug.startsWith(QStringLiteral("gemini-2.5-pro"))) {
            outLevels = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
            return true;
        }
        outLevels = {QStringLiteral("off"), QStringLiteral("minimal"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
        return true;
    }

    // 5. Qwen / Zhipu GLM / Grok / Kimi / MiniMax 推理系列
    if (slug.startsWith(QStringLiteral("qwq")) || slug.startsWith(QStringLiteral("qwen3"))
        || slug.startsWith(QStringLiteral("glm-4.5")) || slug.startsWith(QStringLiteral("glm-4.6"))
        || slug.startsWith(QStringLiteral("glm-5")) || slug.startsWith(QStringLiteral("glm-z1"))
        || slug.contains(QStringLiteral("kimi-k2-thinking")) || slug.startsWith(QStringLiteral("minimax-m2"))) {
        outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
        return true;
    }
    if (slug.startsWith(QStringLiteral("grok-3-mini")) || slug.startsWith(QStringLiteral("grok-4"))) {
        outLevels = {QStringLiteral("low"), QStringLiteral("high")};
        return true;
    }
    if (slug.contains(QStringLiteral("thinking")) || slug.contains(QStringLiteral("reasoner"))) {
        if (protocol == QStringLiteral("anthropic")) {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("max")};
        } else {
            outLevels = {QStringLiteral("off"), QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
        }
        return true;
    }
    return false;
}

} // namespace

ContextWindow ResolveContextWindow(const ModelSettings& model) {
    if (model.contextOverride > 0) return {model.contextOverride,
        QCoreApplication::translate("AgentSettings", "用户指定"), false};
    if (model.providerContext > 0) return {model.providerContext,
        QCoreApplication::translate("AgentSettings", "提供商声明"), false};
    if (model.catalogContext > 0) return {model.catalogContext,
        QCoreApplication::translate("AgentSettings", "内置默认"), false};
    return {262144, QCoreApplication::translate("AgentSettings", "256K 估算"), true};
}

const QStringList& AllThinkingLevels() {
    static const QStringList kLevels{
        QStringLiteral("off"),
        QStringLiteral("minimal"),
        QStringLiteral("low"),
        QStringLiteral("medium"),
        QStringLiteral("high"),
        QStringLiteral("xhigh"),
        QStringLiteral("max"),
    };
    return kLevels;
}

bool IsValidThinkingLevel(const QString& level) {
    return AllThinkingLevels().contains(level);
}

QString NormalizeThinkingLevelToken(const QString& raw) {
    const QString token = raw.trimmed().toLower();
    if (token.isEmpty() || token == QStringLiteral("default") || token == QStringLiteral("auto")
        || token == QStringLiteral("默认")) return {};
    if (token == QStringLiteral("off") || token == QStringLiteral("none")
        || token == QStringLiteral("disabled") || token == QStringLiteral("关闭")) return QStringLiteral("off");
    if (token == QStringLiteral("minimal") || token == QStringLiteral("极低")) return QStringLiteral("minimal");
    if (token == QStringLiteral("low") || token == QStringLiteral("低")) return QStringLiteral("low");
    if (token == QStringLiteral("medium") || token == QStringLiteral("med") || token == QStringLiteral("中")) return QStringLiteral("medium");
    if (token == QStringLiteral("high") || token == QStringLiteral("高")) return QStringLiteral("high");
    if (token == QStringLiteral("xhigh") || token == QStringLiteral("extra_high")
        || token == QStringLiteral("extra-high") || token == QStringLiteral("超高")) return QStringLiteral("xhigh");
    if (token == QStringLiteral("max") || token == QStringLiteral("maximum") || token == QStringLiteral("最高")) return QStringLiteral("max");
    return {};
}

QStringList NormalizeThinkingLevels(const QStringList& rawLevels) {
    QStringList normalized;
    for (const auto& raw : rawLevels) {
        const QString token = NormalizeThinkingLevelToken(raw);
        if (!token.isEmpty() && !normalized.contains(token)) normalized.push_back(token);
    }
    QStringList ordered;
    for (const auto& canonical : AllThinkingLevels()) {
        if (normalized.contains(canonical)) ordered.push_back(canonical);
    }
    return ordered;
}

QString ThinkingLevelShortLabel(const QString& level) {
    if (level.isEmpty() || level == QStringLiteral("default")) return QStringLiteral("默认");
    if (level == QStringLiteral("off")) return QStringLiteral("关闭");
    if (level == QStringLiteral("minimal")) return QStringLiteral("极低");
    if (level == QStringLiteral("low")) return QStringLiteral("低");
    if (level == QStringLiteral("medium")) return QStringLiteral("中");
    if (level == QStringLiteral("high")) return QStringLiteral("高");
    if (level == QStringLiteral("xhigh")) return QStringLiteral("超高");
    if (level == QStringLiteral("max")) return QStringLiteral("最高");
    return level;
}

QString ThinkingLevelMenuLabel(const QString& level) {
    if (level.isEmpty() || level == QStringLiteral("default")) return QStringLiteral("默认（接口预设）");
    if (level == QStringLiteral("off")) return QStringLiteral("关闭 (Off)");
    if (level == QStringLiteral("minimal")) return QStringLiteral("极低 (Minimal)");
    if (level == QStringLiteral("low")) return QStringLiteral("低 (Low)");
    if (level == QStringLiteral("medium")) return QStringLiteral("中 (Medium)");
    if (level == QStringLiteral("high")) return QStringLiteral("高 (High)");
    if (level == QStringLiteral("xhigh")) return QStringLiteral("超高 (XHigh)");
    if (level == QStringLiteral("max")) return QStringLiteral("最高 (Max)");
    return level;
}

ResolvedThinkingCapability ResolveThinkingCapability(
    const ModelSettings& model, const QString& protocol, const QString& baseUrl) {
    ResolvedThinkingCapability cap;
    if (model.thinkingLevelsCustomized) {
        cap.levels = NormalizeThinkingLevels(model.customThinkingLevels);
        cap.source = QCoreApplication::translate("AgentSettings", "手动指定");
        cap.discovered = true;
        return cap;
    }
    const QStringList fromProvider = NormalizeThinkingLevels(model.providerThinkingLevels);
    if (!fromProvider.isEmpty()) {
        cap.levels = fromProvider;
        cap.source = QCoreApplication::translate("AgentSettings", "接口获取");
        cap.discovered = true;
        return cap;
    }
    QStringList catalogLevels;
    QString catalogDefault;
    if (MatchCatalogLevels(CanonicalSlug(model.id), protocol, baseUrl, catalogLevels, catalogDefault)
        || MatchCatalogLevels(CanonicalSlug(model.name), protocol, baseUrl, catalogLevels, catalogDefault)) {
        cap.levels = NormalizeThinkingLevels(catalogLevels);
        cap.defaultLevel = catalogDefault;
        cap.source = QCoreApplication::translate("AgentSettings", "内置目录");
        cap.discovered = true;
        return cap;
    }
    return cap;
}

QString EffectiveThinkingLevel(
    const ModelSettings& model, const QString& protocol, const QString& baseUrl) {
    const auto cap = ResolveThinkingCapability(model, protocol, baseUrl);
    if (cap.levels.isEmpty()) return {};
    const QString chosen = NormalizeThinkingLevelToken(model.selectedThinkingLevel);
    if (!chosen.isEmpty() && cap.levels.contains(chosen)) return chosen;
    return {};
}

bool ExternalMcpPermissionSatisfied(const QString& minimumPermission, const AgentSettings& settings) {
    if (minimumPermission == QStringLiteral("read")) return true;
    if (minimumPermission == QStringLiteral("edit")) return settings.allowEdits;
    if (minimumPermission == QStringLiteral("save")) return settings.allowEdits && settings.allowSave;
    return settings.allowEdits && settings.allowSave && settings.allowCommands;
}

bool ExternalMcpConnectionAllowed(const ExternalMcpServerSettings& server, const AgentSettings& settings) {
    return settings.aiEnabled && server.enabled && ExternalMcpPermissionSatisfied(server.minimumPermission, settings);
}

namespace {

// 按非字母数字字符与驼峰边界拆词并转小写：GITHUB_TOKEN → github/token，apiKey → api/key，APIKey → apikey。
QStringList SecretNameWords(const QString& name) {
    QStringList words;
    QString current;
    for (const QChar ch : name) {
        if (!ch.isLetterOrNumber()) {
            if (!current.isEmpty()) { words.push_back(current.toLower()); current.clear(); }
            continue;
        }
        if (ch.isUpper() && !current.isEmpty() && current.back().isLower()) {
            words.push_back(current.toLower());
            current.clear();
        }
        current.append(ch);
    }
    if (!current.isEmpty()) words.push_back(current.toLower());
    return words;
}

// ${VAR}、$VAR、%VAR%、<占位> 只是占位符，不是明文密钥。
bool IsPlaceholderValue(const QString& value) {
    return value.startsWith(QLatin1Char('$')) || value.contains(QStringLiteral("${"))
        || (value.size() > 2 && value.startsWith(QLatin1Char('%')) && value.endsWith(QLatin1Char('%')))
        || (value.size() > 2 && value.startsWith(QLatin1Char('<')) && value.endsWith(QLatin1Char('>')));
}

// 文件路径形式的取值（证书、密钥文件路径）不按明文密钥处理。
bool IsPathLikeValue(const QString& value) {
    static const QRegularExpression kDrivePath(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return value.contains(QLatin1Char('\\')) || value.startsWith(QStringLiteral("./"))
        || value.startsWith(QStringLiteral("../")) || value.startsWith(QStringLiteral("~/"))
        || value.startsWith(QLatin1Char('/')) || kDrivePath.match(value).hasMatch();
}

bool SecretOptionValue(const QString& value) {
    const QString trimmed = value.trimmed();
    return !trimmed.isEmpty() && !IsPlaceholderValue(trimmed) && !IsPathLikeValue(trimmed);
}

bool UrlCarriesSecret(const QString& text) {
    const QUrl url(text);
    if (!url.isValid()) return false;
    if (!url.password().isEmpty()) return true;
    const QUrlQuery query(url);
    for (const auto& item : query.queryItems(QUrl::FullyDecoded)) {
        if (ExternalMcpSecretName(item.first) && SecretOptionValue(item.second)) return true;
    }
    return false;
}

} // namespace

bool ExternalMcpSecretName(const QString& name) {
    static const QSet<QString> kLastWords{
        QStringLiteral("password"), QStringLiteral("passwd"), QStringLiteral("passphrase"),
        QStringLiteral("secret"), QStringLiteral("secrets"), QStringLiteral("token"), QStringLiteral("tokens"),
        QStringLiteral("key"), QStringLiteral("apikey"), QStringLiteral("pat"),
        QStringLiteral("credential"), QStringLiteral("credentials"), QStringLiteral("auth"),
        QStringLiteral("authorization"), QStringLiteral("cookie")};
    const QStringList words = SecretNameWords(name);
    if (words.isEmpty()) return false;
    if (kLastWords.contains(words.back())) return true;
    // 无分隔的复合名（ACCESSTOKEN、DBPASSWORD）按长关键词后缀匹配；短词 key/pat/auth 只按整词，避免 MONKEY、COMPAT 误判。
    const QString lower = words.back();
    for (const auto* suffix : {"password", "passwd", "passphrase", "secret", "token", "apikey", "credential", "credentials"}) {
        if (lower.endsWith(QLatin1String(suffix))) return true;
    }
    return false;
}

bool ExternalMcpValueLooksSecret(const QString& value) {
    static const QRegularExpression kTokenFormats(QStringLiteral(
        "^(?:sk-[A-Za-z0-9_-]{16,}"
        "|(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,}"
        "|github_pat_[A-Za-z0-9_]{20,}"
        "|glpat-[A-Za-z0-9_-]{16,}"
        "|xox[abprs]-[A-Za-z0-9-]{10,}"
        "|(?:AKIA|ASIA)[A-Z0-9]{16}"
        "|AIza[0-9A-Za-z_-]{30,}"
        "|hf_[A-Za-z0-9]{20,}"
        "|npm_[A-Za-z0-9]{20,}"
        "|(?i:bearer|basic)\\s+\\S{8,})$"));
    const QString trimmed = value.trimmed();
    return !IsPlaceholderValue(trimmed) && kTokenFormats.match(trimmed).hasMatch();
}

bool ExternalMcpArgsCarrySecret(const QStringList& args) {
    for (qsizetype index = 0; index < args.size(); ++index) {
        const QString arg = args.at(index).trimmed();
        if (arg.isEmpty()) continue;
        if (ExternalMcpValueLooksSecret(arg)) return true;
        // mcp-remote 等桥接常见写法：--header "Authorization: Bearer xxx"。
        static const QRegularExpression kAuthorizationHeader(QStringLiteral("authorization\\s*[:=]\\s*(.*)$"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto match = kAuthorizationHeader.match(arg); match.hasMatch() && SecretOptionValue(match.captured(1))) return true;
        if (arg.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || arg.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
            if (UrlCarriesSecret(arg)) return true;
            continue;
        }
        if (!arg.startsWith(QLatin1Char('-'))) continue;
        QString body = arg;
        while (body.startsWith(QLatin1Char('-'))) body.remove(0, 1);
        const qsizetype equals = body.indexOf(QLatin1Char('='));
        if (!ExternalMcpSecretName(equals >= 0 ? body.left(equals) : body)) continue;
        if (equals >= 0) {
            if (SecretOptionValue(body.mid(equals + 1))) return true;
        } else if (index + 1 < args.size()) {
            const QString next = args.at(index + 1).trimmed();
            if (!next.startsWith(QLatin1Char('-')) && SecretOptionValue(next)) return true;
        }
    }
    return false;
}

} // namespace War3Map::Agent
