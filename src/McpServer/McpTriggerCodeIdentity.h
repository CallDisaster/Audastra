#pragma once
#include "War3MapCore/Services/TriggerCodeIdentityJson.h"
#include <QJsonObject>
namespace War3Map::Mcp {
[[nodiscard]] bool HasCodeIdentityOperation(const QJsonObject& request);
[[nodiscard]] QJsonObject CodeIdentityGraphSchema(QJsonObject schema);
[[nodiscard]] QJsonObject CodeIdentitySummaryJson(const Services::TriggerCodeIdentitySummary& summary);
[[nodiscard]] Result<Trigger::TriggerGraphEditBatch> DecodeCodeIdentityGraphBatch(
    const QJsonObject& request, const Services::TriggerCodeIdentityContext& context, bool dryRun,
    Services::TriggerCodeIdentitySummary* summary = nullptr,
    std::optional<Services::PreparedTriggerCodeIdentityEdit>* prepared = nullptr);
}
