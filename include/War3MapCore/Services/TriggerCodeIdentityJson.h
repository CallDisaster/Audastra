#pragma once
#include "War3MapCore/Services/TriggerCodeIdentityEditing.h"
namespace War3Map::Services {
struct ParsedTriggerCodeIdentityEdit {
    TriggerCodeIdentityRequest request;
    bool dryRun = true;
};
// CLI与MCP共用的有界JSON入口；没有代码正文、路径或来源批准字段。
[[nodiscard]] Result<ParsedTriggerCodeIdentityEdit> ParseTriggerCodeIdentityEdit(std::string_view json);
[[nodiscard]] std::string TriggerCodeIdentitySummaryJson(const TriggerCodeIdentitySummary& summary,
    const Trigger::TriggerGraphEditReceipt* graph = nullptr);
}
