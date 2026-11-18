#include "TokenMeter.h"

#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace War3Map::Agent {

qint64 TokenMeter::EstimateText(QStringView text) noexcept {
    if (text.isEmpty()) return 0;
    qint64 cjk = 0;
    qint64 other = 0;
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        if ((code >= 0x2E80 && code <= 0x9FFF)
            || (code >= 0xAC00 && code <= 0xD7AF)
            || (code >= 0xF900 && code <= 0xFAFF)
            || (code >= 0xFF00 && code <= 0xFFEF)) {
            ++cjk;
        } else {
            ++other;
        }
    }
    return static_cast<qint64>(std::ceil(static_cast<double>(cjk) * 0.68 + static_cast<double>(other) * 0.28));
}

qint64 TokenMeter::EstimateJson(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull()) return 1;
    if (value.isString()) return EstimateText(value.toString());
    if (value.isBool() || value.isDouble()) return 2;
    const QByteArray compact = value.isObject()
        ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
        : QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    return EstimateText(QString::fromUtf8(compact));
}

qint64 TokenMeter::EstimateTool(const QJsonObject& tool) {
    if (tool.isEmpty()) return 0;
    const QJsonValue schema = tool.contains(QStringLiteral("inputSchema"))
        ? tool.value(QStringLiteral("inputSchema"))
        : tool.value(QStringLiteral("parameters"));
    const QByteArray schemaBytes = schema.isObject()
        ? QJsonDocument(schema.toObject()).toJson(QJsonDocument::Compact)
        : QByteArrayLiteral("{}");
    return 16
        + EstimateText(tool.value(QStringLiteral("name")).toString())
        + EstimateText(tool.value(QStringLiteral("description")).toString())
        + static_cast<qint64>(std::ceil(static_cast<double>(schemaBytes.size()) * 0.28));
}

qint64 TokenMeter::EstimateMessage(const QJsonObject& message) {
    if (message.isEmpty()) return 0;
    // system 首帧保存了初始提示词与工具快照，由外部分项（系统提示词/工具/技能/记忆）单独统计，避免在消息历史里重复计费。
    if (message.value(QStringLiteral("role")).toString() == QStringLiteral("system")) return 0;
    qint64 tokens = 4;
    const QJsonValue content = message.value(QStringLiteral("content"));
    if (content.isString()) {
        tokens += EstimateText(content.toString());
    } else if (content.isArray()) {
        for (const auto& item : content.toArray()) {
            const auto block = item.toObject();
            const QString type = block.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("text")) {
                tokens += EstimateText(block.value(QStringLiteral("text")).toString());
            } else if (type == QStringLiteral("thinking")) {
                tokens += EstimateText(block.value(QStringLiteral("thinking")).toString());
            } else if (type == QStringLiteral("toolCall")) {
                tokens += 12
                    + EstimateText(block.value(QStringLiteral("name")).toString())
                    + EstimateJson(block.value(QStringLiteral("arguments")));
            } else if (type == QStringLiteral("image")) {
                tokens += 768;
            } else {
                tokens += EstimateJson(block);
            }
        }
    }
    return tokens;
}

ContextCompositionSnapshot TokenMeter::ProjectToActualInput(
    const ContextCompositionSnapshot& raw, qint64 actualPromptTokens) {
    if (actualPromptTokens <= 0) return raw;
    const qint64 estimatedTotal = raw.TotalEstimatedUsed();
    if (estimatedTotal <= 0) {
        auto copy = raw;
        copy.messagesTokens = actualPromptTokens;
        return copy;
    }
    const double ratio = static_cast<double>(actualPromptTokens) / static_cast<double>(estimatedTotal);
    // 合理分词器偏差范围内按比例缩放各构成项，误差余数归入消息历史，保证各项之和精确等于服务端输入用量。
    auto scaled = raw;
    const auto scale = [ratio](qint64 v) {
        return v > 0 ? std::max<qint64>(1, static_cast<qint64>(std::llround(static_cast<double>(v) * ratio))) : 0;
    };
    scaled.editorToolsTokens = scale(raw.editorToolsTokens);
    scaled.mcpToolsTokens = scale(raw.mcpToolsTokens);
    scaled.systemPromptTokens = scale(raw.systemPromptTokens);
    scaled.skillsTokens = scale(raw.skillsTokens);
    scaled.mcpInstructionsTokens = scale(raw.mcpInstructionsTokens);
    scaled.memoryFilesTokens = scale(raw.memoryFilesTokens);
    const qint64 fixedSum = scaled.editorToolsTokens + scaled.mcpToolsTokens + scaled.systemPromptTokens
        + scaled.skillsTokens + scaled.mcpInstructionsTokens + scaled.memoryFilesTokens;
    if (fixedSum <= actualPromptTokens) {
        scaled.messagesTokens = actualPromptTokens - fixedSum;
    } else {
        scaled.messagesTokens = scale(raw.messagesTokens);
    }
    for (auto& item : scaled.editorToolItems) item.tokens = scale(item.tokens);
    for (auto& item : scaled.mcpToolItems) item.tokens = scale(item.tokens);
    for (auto& item : scaled.skillItems) item.tokens = scale(item.tokens);
    for (auto& item : scaled.memoryItems) item.tokens = scale(item.tokens);
    return scaled;
}

} // namespace War3Map::Agent
