#include "AgentTypes.h"

#include <QCoreApplication>

namespace War3Map::Agent {

ContextWindow ResolveContextWindow(const ModelSettings& model) {
    if (model.contextOverride > 0) return {model.contextOverride,
        QCoreApplication::translate("AgentSettings", "用户指定"), false};
    if (model.providerContext > 0) return {model.providerContext,
        QCoreApplication::translate("AgentSettings", "提供商声明"), false};
    if (model.catalogContext > 0) return {model.catalogContext,
        QCoreApplication::translate("AgentSettings", "内置默认"), false};
    return {262144, QCoreApplication::translate("AgentSettings", "256K 估算"), true};
}

bool ExternalMcpPermissionSatisfied(const QString& minimumPermission, const AgentSettings& settings) {
    if (minimumPermission == QStringLiteral("read")) return true;
    if (minimumPermission == QStringLiteral("edit")) return settings.allowEdits;
    if (minimumPermission == QStringLiteral("save")) return settings.allowEdits && settings.allowSave;
    return settings.allowEdits && settings.allowSave && settings.allowCommands;
}

} // namespace War3Map::Agent
