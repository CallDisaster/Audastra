#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace War3Map::Agent {

struct ModelSettings {
    QString id;
    QString name;
    bool enabled = true;
    qint64 contextOverride = 0;
    qint64 providerContext = 0;
    qint64 catalogContext = 0;
    qint64 maxOutputTokens = 0;
    bool toolsSupported = true;
    bool imagesSupported = false;
};

struct ProviderSettings {
    QString id;
    QString name;
    QString website;
    QString baseUrl;
    QString protocol = QStringLiteral("openai-chat");
    QString credentialRef;
    bool enabled = true;
    QList<ModelSettings> models;
};

struct AgentSettings {
    bool aiEnabled = false;
    bool reducedMotion = false;
    // 助手或外部 MCP 改了触发器、变量时，编辑器跳到改动处（不抢焦点）。
    bool followChanges = false;
    QList<ProviderSettings> providers;
    bool allowEdits = false;
    bool allowSave = false;
    bool allowCommands = false;
    bool mcpEnabled = false;
    quint16 mcpPort = 8767;
    QStringList disabledTools;
};

struct ContextWindow {
    qint64 tokens = 262144;
    QString source;
    bool estimated = true;
};

[[nodiscard]] ContextWindow ResolveContextWindow(const ModelSettings& model);

} // namespace War3Map::Agent
