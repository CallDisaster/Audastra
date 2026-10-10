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

} // namespace War3Map::Agent
