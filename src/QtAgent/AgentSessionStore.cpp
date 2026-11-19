#include "AgentSessionStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace War3Map::Agent {
namespace {

constexpr qint64 kSessionBudget = 8 * 1024 * 1024;
constexpr qsizetype kSessionCountBudget = 4096;
constexpr qint64 kListBudget = 64 * 1024 * 1024;

void Error(QString* error, const QString& message) { if (error) *error = message; }
QString Now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
bool ValidId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern.match(id).hasMatch();
}

bool SameLegacyMap(const QString& target, const QString& source, const QString& runtimeIdentity) {
    if (source == target || (!runtimeIdentity.isEmpty() && source == runtimeIdentity)) return true;
    // 旧宿主把文档代次拼在绝对路径后。仅去掉确证的数字代次，不把一般地图身份模糊匹配。
    const auto separator = source.lastIndexOf(QLatin1Char('|'));
    if (separator < 0 || !QFileInfo(target).isAbsolute()) return false;
    bool number = false;
    (void)source.mid(separator + 1).toULongLong(&number);
    return number && QDir::cleanPath(source.left(separator)).toCaseFolded() == QDir::cleanPath(target).toCaseFolded();
}

bool CompleteSystemMessage(const QJsonObject& message) {
    const auto content = message.value(QStringLiteral("content"));
    if (!content.isString() && !content.isArray()) return false;
    if (content.isArray()) for (const auto& value : content.toArray()) {
        const auto block = value.toObject();
        if (!value.isObject() || block.value(QStringLiteral("type")).toString() != QStringLiteral("text")
            || !block.value(QStringLiteral("text")).isString()) return false;
    }
    const auto timestamp = message.value(QStringLiteral("timestamp"));
    if (!timestamp.isDouble() || !std::isfinite(timestamp.toDouble()) || timestamp.toDouble() < 0) return false;
    if (message.contains(QStringLiteral("sections"))) {
        if (!message.value(QStringLiteral("sections")).isObject()) return false;
        const auto sections = message.value(QStringLiteral("sections")).toObject();
        for (auto iterator = sections.constBegin(); iterator != sections.constEnd(); ++iterator)
            if (!iterator.value().isString() && !iterator.value().isNull()) return false;
    }
    for (const auto& field : {QStringLiteral("toolsAdded"), QStringLiteral("toolsRemoved")}) {
        if (!message.contains(field)) continue;
        if (!message.value(field).isArray()) return false;
        QSet<QString> names;
        for (const auto& value : message.value(field).toArray()) {
            const auto tool = value.toObject();
            const auto name = tool.value(QStringLiteral("name")).toString();
            if (!value.isObject() || name.isEmpty() || names.contains(name)) return false;
            names.insert(name);
            if (field == QStringLiteral("toolsAdded") && (!tool.value(QStringLiteral("description")).isString()
                || !tool.value(QStringLiteral("parameters")).isObject())) return false;
        }
    }
    return true;
}

// 当会话因历史工具返回过大接近 8 MiB 预算时，仅压缩 transcript_log 中超大工具结果详情，绝不删减用户原始提问与助手正文。
QJsonArray CompactTranscriptLogForBudget(const QJsonArray& log, int maxToolDetailChars) {
    QJsonArray out;
    for (const auto& entry : log) {
        auto obj = entry.toObject();
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("tool_end") || type == QStringLiteral("tool_start")) {
            auto details = obj.value(QStringLiteral("details")).toObject();
            const QByteArray encoded = QJsonDocument(details).toJson(QJsonDocument::Compact);
            if (encoded.size() > maxToolDetailChars) {
                obj.insert(QStringLiteral("details"), QJsonObject{
                    {QStringLiteral("truncated_for_storage"), true},
                    {QStringLiteral("preview"), QString::fromUtf8(encoded.left(maxToolDetailChars))},
                });
            }
        }
        out.append(obj);
    }
    return out;
}

// 裁剪工具详情后仍超预算时，按“思考 → 较早的非用户条目 → 较早的任意条目”的顺序整条移除，
// 并在开头留一条 transcript_trimmed 标记写明移除数量。日志超限不能连累 messages 无法保存。
QJsonArray TrimTranscriptLogToBudget(const QJsonArray& log, qint64 budget) {
    qint64 removed = 0;
    qsizetype begin = 0;
    if (!log.isEmpty() && log.first().toObject().value(QStringLiteral("type")).toString() == QStringLiteral("transcript_trimmed")) {
        removed = log.first().toObject().value(QStringLiteral("removed")).toInteger();
        begin = 1;
    }
    std::vector<qint64> sizes;
    std::vector<bool> keep;
    qint64 total = 0;
    for (qsizetype i = begin; i < log.size(); ++i) {
        const qint64 size = QJsonDocument(log.at(i).toObject()).toJson(QJsonDocument::Compact).size() + 1;
        sizes.push_back(size);
        keep.push_back(true);
        total += size;
    }
    const auto typeAt = [&](std::size_t i) {
        return log.at(begin + static_cast<qsizetype>(i)).toObject().value(QStringLiteral("type")).toString();
    };
    const auto dropWhere = [&](const auto& predicate) {
        for (std::size_t i = 0; i < sizes.size() && total > budget; ++i) {
            if (!keep[i] || !predicate(i)) continue;
            keep[i] = false; total -= sizes[i]; ++removed;
        }
    };
    dropWhere([&](std::size_t i) { return typeAt(i) == QStringLiteral("thinking_block"); });
    dropWhere([&](std::size_t i) { return typeAt(i) != QStringLiteral("user_message"); });
    dropWhere([](std::size_t) { return true; });
    QJsonArray out;
    if (removed > 0) out.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("transcript_trimmed")},
        {QStringLiteral("removed"), removed}, {QStringLiteral("historical"), true}});
    for (std::size_t i = 0; i < sizes.size(); ++i) if (keep[i]) out.append(log.at(begin + static_cast<qsizetype>(i)));
    return out;
}

QJsonObject Json(const AgentSession& session, const QJsonArray& logOverride) {
    QJsonObject obj{{QStringLiteral("version"), 2}, {QStringLiteral("runtime"), QStringLiteral("pi")},
        {QStringLiteral("session_id"), session.id}, {QStringLiteral("map_identity"), session.mapIdentity},
        {QStringLiteral("title"), session.title}, {QStringLiteral("created_at"), session.createdAt},
        {QStringLiteral("captured_at"), session.updatedAt}, {QStringLiteral("provider_id"), session.providerId},
        {QStringLiteral("model_id"), session.modelId}, {QStringLiteral("complete"), true},
        {QStringLiteral("messages"), session.messages}, {QStringLiteral("migrated_from"), session.migratedFrom}};
    if (!logOverride.isEmpty()) obj.insert(QStringLiteral("transcript_log"), logOverride);
    if (!session.lastUsage.isEmpty()) obj.insert(QStringLiteral("last_usage"), session.lastUsage);
    return obj;
}

std::optional<QJsonObject> ReadJson(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kSessionBudget) {
        Error(error, QStringLiteral("无法读取完整 AI 会话，或文件超过 8 MiB；原文件保留。")); return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        Error(error, QStringLiteral("AI 会话不是有效 JSON；原文件保留。")); return std::nullopt;
    }
    return document.object();
}

QString Title(const QJsonArray& messages) {
    for (const auto& value : messages) {
        const auto message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user") || IsHiddenAgentMessage(message)) continue;
        const auto text = AgentMessageText(message).simplified();
        if (!text.isEmpty()) return text.left(30);
    }
    return QStringLiteral("新会话");
}

} // namespace

QString AgentMessageText(const QJsonObject& message) {
    const auto content = message.value(QStringLiteral("content"));
    if (content.isString()) return content.toString();
    QStringList text;
    for (const auto& value : content.toArray()) {
        const auto block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")) text.append(block.value(QStringLiteral("text")).toString());
    }
    return text.join(QLatin1Char('\n'));
}

bool IsHiddenAgentMessage(const QJsonObject& message) {
    if (message.value(QStringLiteral("forge_hidden")).toBool()) return true;
    const auto injection = message.value(QStringLiteral("forge_injection")).toString();
    if (!injection.isEmpty()) return true;
    const auto role = message.value(QStringLiteral("role")).toString();
    if (role != QStringLiteral("user") && role != QStringLiteral("assistant") && role != QStringLiteral("toolResult")) return true;
    if (role == QStringLiteral("user")) {
        const QString text = AgentMessageText(message);
        if (text.startsWith(QStringLiteral("<compacted-summary>"))) return true;
        if (text.startsWith(QStringLiteral("较早完整回合已从模型上下文压缩；完整记录仍由宿主保存。下列只是历史工具结果摘要，身份可能已过期，继续编辑前重新查询当前地图。\n")))
            return true;
    }
    return false;
}

bool IsCompleteAgentTranscript(const QJsonArray& messages) {
    if (messages.size() > 10000) return false;
    QSet<QString> pending;
    for (const auto& entry : messages) {
        if (!entry.isObject()) return false;
        const auto message = entry.toObject();
        const auto role = message.value(QStringLiteral("role")).toString();
        if (role == QStringLiteral("system")) {
            if (!pending.isEmpty() || !CompleteSystemMessage(message)) return false;
        } else if (role == QStringLiteral("user")) {
            if (!pending.isEmpty()) return false;
        } else if (role == QStringLiteral("assistant")) {
            const auto reason = message.value(QStringLiteral("stopReason")).toString();
            if (reason == QStringLiteral("aborted") || reason == QStringLiteral("error")) return false;
            for (const auto& block : message.value(QStringLiteral("content")).toArray()) {
                const auto object = block.toObject();
                if (object.value(QStringLiteral("type")).toString() != QStringLiteral("toolCall")) continue;
                const auto id = object.value(QStringLiteral("id")).toString();
                if (id.isEmpty() || pending.contains(id)) return false;
                pending.insert(id);
            }
        } else if (role == QStringLiteral("toolResult")) {
            if (!pending.remove(message.value(QStringLiteral("toolCallId")).toString())) return false;
        } else return false;
    }
    return pending.isEmpty();
}

AgentSessionStore::AgentSessionStore(QString directory) : directory_(std::move(directory)) {}
QString AgentSessionStore::MapHash(const QString& mapIdentity) {
    return QString::fromLatin1(QCryptographicHash::hash(mapIdentity.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString AgentSessionStore::LegacyBinding(const QString& map, const QString& provider, const QString& model) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{map, provider, model})
        .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QString AgentSessionStore::SessionPath(const QString& mapIdentity, const QString& id) const {
    if (!ValidId(id)) return {};
    return QDir(directory_).filePath(QStringLiteral("sessions/%1/%2.json").arg(MapHash(mapIdentity), id));
}

std::optional<AgentSession> AgentSessionStore::Read(const QString& mapIdentity, const QString& id, QString* error) const {
    const auto path = SessionPath(mapIdentity, id);
    if (path.isEmpty()) { Error(error, QStringLiteral("无效会话 ID。")); return std::nullopt; }
    if (QFileInfo(path).isSymLink()) { Error(error, QStringLiteral("会话文件不能是符号链接。")); return std::nullopt; }
    const auto json = ReadJson(path, error);
    if (!json) return std::nullopt;
    const auto& object = *json;
    if (object.value(QStringLiteral("version")).toInt() != 2
        || object.value(QStringLiteral("session_id")).toString() != id
        || object.value(QStringLiteral("map_identity")).toString() != mapIdentity
        || !object.value(QStringLiteral("complete")).toBool() || !object.value(QStringLiteral("messages")).isArray()
        || !IsCompleteAgentTranscript(object.value(QStringLiteral("messages")).toArray())) {
        Error(error, QStringLiteral("AI 会话身份或工具配对无效；不会恢复不完整上下文，原文件保留。")); return std::nullopt;
    }
    if (object.contains(QStringLiteral("transcript_log")) && !object.value(QStringLiteral("transcript_log")).isArray()) {
        Error(error, QStringLiteral("AI 会话历史日志格式无效；原文件保留。")); return std::nullopt;
    }
    AgentSession session{id, mapIdentity, object.value(QStringLiteral("title")).toString(),
        object.value(QStringLiteral("created_at")).toString(), object.value(QStringLiteral("captured_at")).toString(),
        object.value(QStringLiteral("provider_id")).toString(), object.value(QStringLiteral("model_id")).toString(),
        object.value(QStringLiteral("messages")).toArray(), object.value(QStringLiteral("migrated_from")).toString(),
        object.value(QStringLiteral("transcript_log")).toArray(),
        object.value(QStringLiteral("last_usage")).toObject()};
    return session;
}

QList<AgentSession> AgentSessionStore::List(const QString& mapIdentity, QString* error, QString* warning) const {
    if (error != nullptr) error->clear();
    if (warning != nullptr) warning->clear();
    QList<AgentSession> result;
    const QDir directory(QDir(directory_).filePath(QStringLiteral("sessions/") + MapHash(mapIdentity)));
    const auto files = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    if (files.size() > kSessionCountBudget) { Error(error, QStringLiteral("本地图会话超过 4096 个，请先归档后再恢复。")); return {}; }
    qint64 totalBytes = 0;
    QString firstItemError;
    int skippedInvalid = 0;
    for (const auto& file : files) {
        if (file.size() > kSessionBudget) {
            ++skippedInvalid;
            if (firstItemError.isEmpty()) firstItemError = QStringLiteral("无法读取完整 AI 会话，或文件超过 8 MiB；原文件保留。");
            continue;
        }
        if (file.size() > kListBudget - totalBytes) { Error(error, QStringLiteral("会话列表超过 64 MiB 读取预算，请先归档较早会话。")); return {}; }
        totalBytes += file.size();
        QString itemError;
        auto session = Read(mapIdentity, file.completeBaseName(), &itemError);
        if (!session) {
            ++skippedInvalid;
            if (firstItemError.isEmpty()) firstItemError = itemError;
            continue;
        }
        result.append(std::move(*session));
    }
    // 单个会话文件超限或损坏时不连坐清空同地图其他正常会话；若全部文件均损坏则报告原错误拒绝覆盖。
    if (result.isEmpty() && skippedInvalid > 0) {
        Error(error, firstItemError);
        return {};
    }
    if (skippedInvalid > 0 && warning != nullptr) {
        *warning = QStringLiteral("已跳过 %1 个损坏或超限的会话文件（原文件已保留）。").arg(skippedInvalid);
    }
    std::sort(result.begin(), result.end(), [](const AgentSession& left, const AgentSession& right) {
        return left.updatedAt == right.updatedAt ? left.id < right.id : left.updatedAt > right.updatedAt;
    });
    return result;
}

std::optional<AgentSession> AgentSessionStore::Create(const QString& mapIdentity, QString* error) const {
    AgentSession session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.mapIdentity = mapIdentity;
    session.title = QStringLiteral("新会话");
    session.createdAt = session.updatedAt = Now();
    if (!Save(session, error)) return std::nullopt;
    return session;
}

bool AgentSessionStore::Save(const AgentSession& session, QString* error, QJsonArray* writtenLog) const {
    const auto path = SessionPath(session.mapIdentity, session.id);
    if (path.isEmpty() || session.title.size() > 256 || !IsCompleteAgentTranscript(session.messages)) {
        Error(error, QStringLiteral("会话身份、标题或工具配对无效，未写入。")); return false;
    }
    QJsonArray logToSave = session.transcriptLog;
    QByteArray bytes = QJsonDocument(Json(session, logToSave)).toJson(QJsonDocument::Compact);
    if (bytes.size() > kSessionBudget && !logToSave.isEmpty()) {
        logToSave = CompactTranscriptLogForBudget(logToSave, 2048);
        bytes = QJsonDocument(Json(session, logToSave)).toJson(QJsonDocument::Compact);
    }
    if (bytes.size() > kSessionBudget && !logToSave.isEmpty()) {
        logToSave = CompactTranscriptLogForBudget(logToSave, 256);
        bytes = QJsonDocument(Json(session, logToSave)).toJson(QJsonDocument::Compact);
    }
    if (bytes.size() > kSessionBudget && !logToSave.isEmpty()) {
        // 预留标记与数组分隔符的余量；messages 本身超限时仍按原规则拒绝写入。
        const qint64 base = QJsonDocument(Json(session, {})).toJson(QJsonDocument::Compact).size();
        const qint64 logBudget = kSessionBudget - base - 4096;
        logToSave = logBudget > 0 ? TrimTranscriptLogToBudget(logToSave, logBudget) : QJsonArray{};
        bytes = QJsonDocument(Json(session, logToSave)).toJson(QJsonDocument::Compact);
    }
    if (bytes.size() > kSessionBudget || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        Error(error, QStringLiteral("会话超过 8 MiB 或目录不可写，原历史保留。")); return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        Error(error, QStringLiteral("会话无法原子写入，原历史保留。")); return false;
    }
    if (writtenLog != nullptr) *writtenLog = std::move(logToSave);
    return true;
}

bool AgentSessionStore::Rename(const QString& mapIdentity, const QString& id, const QString& title, QString* error) const {
    const auto trimmed = title.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 256) { Error(error, QStringLiteral("会话标题应为 1–256 个字符。")); return false; }
    auto session = Read(mapIdentity, id, error);
    if (!session) return false;
    session->title = trimmed;
    session->updatedAt = Now();
    return Save(*session, error);
}

bool AgentSessionStore::Archive(const QString& mapIdentity, const QString& id, QString* error) const {
    if (!Read(mapIdentity, id, error)) return false;
    const auto directory = QDir(directory_).filePath(QStringLiteral("archives/") + MapHash(mapIdentity));
    const auto target = QDir(directory).filePath(id + QLatin1Char('-')
        + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".json"));
    if (!QDir().mkpath(directory) || !QFile::rename(SessionPath(mapIdentity, id), target)) {
        Error(error, QStringLiteral("无法归档会话，原文件保留。")); return false;
    }
    return true;
}

bool AgentSessionStore::MigrateLegacy(const QString& mapIdentity, QString* error, const QString& legacyMapIdentity) const {
    const QDir directory(QDir(directory_).filePath(QStringLiteral("sessions")));
    const auto files = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    if (files.size() > kSessionCountBudget) { Error(error, QStringLiteral("旧会话超过迁移读取预算，原文件保留。")); return false; }
    static const QRegularExpression legacyName(QStringLiteral("^[0-9a-f]{64}$"));
    qint64 totalBytes = 0;
    for (const auto& file : files) {
        if (!legacyName.match(file.completeBaseName()).hasMatch()) continue;
        if (file.size() > kListBudget - totalBytes) { Error(error, QStringLiteral("旧会话超过 64 MiB 迁移预算，原文件保留。")); return false; }
        totalBytes += file.size();
        const auto json = ReadJson(file.absoluteFilePath(), error);
        if (!json) return false;
        const auto& object = *json;
        const auto sourceMap = object.value(QStringLiteral("map_identity")).toString();
        if (!SameLegacyMap(mapIdentity, sourceMap, legacyMapIdentity)) continue;
        const auto provider = object.value(QStringLiteral("provider_id")).toString();
        const auto model = object.value(QStringLiteral("model_id")).toString();
        const auto binding = LegacyBinding(sourceMap, provider, model);
        if (object.value(QStringLiteral("version")).toInt() != 1 || binding != file.completeBaseName()
            || !object.value(QStringLiteral("complete")).toBool() || !object.value(QStringLiteral("messages")).isArray()
            || !IsCompleteAgentTranscript(object.value(QStringLiteral("messages")).toArray())) {
            Error(error, QStringLiteral("旧会话身份或工具配对无效，未迁移；原文件保留。")); return false;
        }
        const auto id = binding.left(8) + QLatin1Char('-') + binding.mid(8, 4) + QLatin1Char('-')
            + binding.mid(12, 4) + QLatin1Char('-') + binding.mid(16, 4) + QLatin1Char('-') + binding.mid(20, 12);
        if (QFileInfo::exists(SessionPath(mapIdentity, id))) continue;
        const auto archived = QDir(QDir(directory_).filePath(QStringLiteral("archives/") + MapHash(mapIdentity)))
            .entryList({id + QStringLiteral("-*.json")}, QDir::Files | QDir::NoSymLinks);
        if (!archived.isEmpty()) continue;
        AgentSession session{id, mapIdentity, Title(object.value(QStringLiteral("messages")).toArray()),
            object.value(QStringLiteral("captured_at")).toString(Now()), object.value(QStringLiteral("captured_at")).toString(Now()),
            provider, model, {}, file.fileName(), {}, {}};
        for (const auto& value : object.value(QStringLiteral("messages")).toArray()) {
            auto message = value.toObject();
            message.insert(QStringLiteral("forge_provider_id"), provider);
            message.insert(QStringLiteral("forge_model_id"), model);
            if (IsHiddenAgentMessage(message)) message.insert(QStringLiteral("forge_hidden"), true);
            session.messages.append(message);
        }
        if (!Save(session, error)) return false;
    }
    return true;
}

} // namespace War3Map::Agent
