#include "War3MapCore/Script/TriggerStorageCapabilities.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include <algorithm>
#include <map>
#include <set>

namespace War3Map::Script {
void AppendMapTriggerValueDiagnostics(MapScriptResult& generated, const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const std::filesystem::path& toolRoot,
    const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory) {
    auto analyzed = Trigger::AnalyzeTriggerValues(document, catalog);
    if (!analyzed.Succeeded()) {
        generated.diagnostics.insert(generated.diagnostics.end(), analyzed.diagnostics.begin(), analyzed.diagnostics.end()); return;
    }
    std::set<std::string> required;
    for (const auto& issue : analyzed.value->issues) if (issue.enabled && !issue.storageType.empty()) required.insert(issue.storageType);
    std::shared_ptr<const Trigger::TriggerStorageCapabilities> capabilities;
    if (!required.empty()) {
        const std::vector<std::string> names(required.begin(), required.end());
        auto captured = CaptureMapTriggerStorageCapabilities(catalog, toolRoot, uiRoot, mapDirectory, names);
        if (captured.Succeeded()) capabilities = *captured.value;
        analyzed = Trigger::AnalyzeTriggerValues(document, catalog, {}, capabilities.get());
        if (!analyzed.Succeeded()) {
            generated.diagnostics.insert(generated.diagnostics.end(), analyzed.diagnostics.begin(), analyzed.diagnostics.end()); return;
        }
    }
    std::set<StableId> replacedStorageEcas;
    for (const auto& issue : analyzed.value->issues) if (!issue.storageType.empty()) {
        const auto argument = std::find_if(document.argumentNodes.begin(), document.argumentNodes.end(), [&](const auto& row) { return row.id == issue.argumentId; });
        if (argument != document.argumentNodes.end() && argument->callEca && *argument->callEca < document.ecaNodes.size())
            replacedStorageEcas.insert(document.ecaNodes[*argument->callEca].id);
    }
    std::erase_if(generated.diagnostics, [&](const auto& diagnostic) {
        if (diagnostic.code == "TRIGGER_REAL_LITERAL_PRECISION_RISK") return true;
        if (diagnostic.code != "storage-type-unsupported" || !diagnostic.origin) return false;
        return std::any_of(generated.statements.begin(), generated.statements.end(), [&](const auto& span) {
            return replacedStorageEcas.contains(span.ecaId) && diagnostic.origin->offset >= span.byteOffset
                && diagnostic.origin->offset < span.byteOffset + span.byteLength;
        });
    });
    std::map<StableId, std::vector<Trigger::TriggerTextArgumentAnchor>> positions;
    for (auto& issue : analyzed.value->issues) {
        auto found = positions.find(issue.triggerId);
        if (found == positions.end()) {
            std::vector<Trigger::TriggerTextArgumentAnchor> anchors;
            const auto rendered = Trigger::RenderTriggerText(document, catalog, issue.triggerId, 4 * 1024 * 1024, nullptr, nullptr, &anchors);
            if (!rendered.Succeeded()) anchors.clear();
            found = positions.emplace(issue.triggerId, std::move(anchors)).first;
        }
        const auto anchor = std::find_if(found->second.begin(), found->second.end(), [&](const auto& row) { return row.argumentId == issue.argumentId; });
        if (anchor != found->second.end()) issue.diagnostic.message += "（T第" + std::to_string(anchor->line) + "行" + std::to_string(anchor->column) + "列）";
        const auto span = std::find_if(generated.statements.begin(), generated.statements.end(), [&](const auto& row) { return row.ecaId == issue.ecaId; });
        if (span != generated.statements.end()) issue.diagnostic.origin = OriginSpan{"Map/generated.j", span->byteOffset, span->byteLength, {}};
        generated.diagnostics.push_back(std::move(issue.diagnostic));
    }
}
}
