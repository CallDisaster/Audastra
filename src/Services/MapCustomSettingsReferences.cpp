#include "MapCustomSettingsReferences.h"
#include "War3MapCore/Services/ProjectReferenceIndex.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Text/TextUtil.h"
#include <algorithm>
#include <cstdio>
#include <cctype>

namespace War3Map::Services {
namespace {
Status Bad(const char* code, std::string text) { return Status::Failure("Map.CustomSettings", code, std::move(text)); }
std::string Numbered(const char* pattern, std::int32_t number) {
    char buffer[80]; std::snprintf(buffer, sizeof(buffer), pattern, number); return buffer;
}
bool IdentifierByte(unsigned char c) { return c >= 128 || std::isalnum(c) || c == '_'; }
bool MentionsSymbol(std::string_view text, std::string_view symbol) {
    for (std::size_t position = 0; (position = text.find(symbol, position)) != text.npos; ++position) {
        const auto end = position + symbol.size();
        if ((position == 0 || !IdentifierByte(text[position - 1])) && (end == text.size() || !IdentifierByte(text[end]))) return true;
    }
    return false;
}
bool PlainIdentifier(std::string_view value) {
    if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front()))) return false;
    return std::all_of(value.begin(), value.end(), IdentifierByte);
}
bool OpaqueEca(const Trigger::TriggerDocument& document, const Trigger::EcaNode& eca) {
    if (eca.name.text == "CustomScriptCode" || eca.name.text == "ExecuteFunc") return true;
    // WTG解码/同链编辑冻结的实际参数签名；缺失签名不能假定普通字符串。
    const auto open = eca.uiSignature.rfind('('), close = eca.uiSignature.rfind(')');
    if (open == std::string::npos || close <= open) return true;
    auto types = std::string_view(eca.uiSignature).substr(open + 1, close - open - 1);
    std::size_t position = 0;
    while (!types.empty()) {
        const auto comma = types.find(','); const auto type = types.substr(0, comma);
        if (type != "nothing") {
            if (position >= eca.arguments.size() || eca.arguments[position] >= document.argumentNodes.size()) return true;
            const auto& argument = document.argumentNodes[eca.arguments[position++]];
            if (type == "scriptcode" && !PlainIdentifier(argument.value.text)) return true;
        }
        if (comma == types.npos) break;
        types.remove_prefix(comma + 1);
    }
    return position != eca.arguments.size();
}
}
Status CheckRandomSettingsReferences(const MapDocument& document, MapCustomSettingsSection section,
    const MapCustomSettingsValue* before, const MapCustomSettingsValue* after,
    const MapCustomSettingsValidation& validation) {
    const bool units = section == MapCustomSettingsSection::RandomUnits;
    if (!units && section != MapCustomSettingsSection::RandomItems) return Status::Ok();
    const auto number = [&](const auto* value) { return units ? std::get<MapData::MapRandomUnitTable>(*value).number
        : std::get<MapData::MapRandomItemTable>(*value).number; };
    bool structureChanged = false;
    if (units && before && after) structureChanged = std::get<MapData::MapRandomUnitTable>(*before).columnTypes
        != std::get<MapData::MapRandomUnitTable>(*after).columnTypes;
    if (before && after && !structureChanged) return Status::Ok();
    const auto id = number(before ? before : after);
    const auto symbol = units ? Numbered("gg_rg_%03d", id) : Numbered("ItemTable%06d_DropItems", id);
    if (before) {
        for (const auto kind : {MapDataKind::Units, MapDataKind::Doodads}) {
            const auto state = document.Member(kind).state;
            if (state != MapDataState::Missing && state != MapDataState::Ready)
                return Bad("SETTINGS_RANDOM_REFERENCE_COVERAGE_REQUIRED", "摆放成员未完整解析，无法核对随机表引用。");
        }
        if (document.Units()) for (const auto& unit : document.Units()->units) {
            if (!units && unit.droppedItemTable == id)
                return Bad("SETTINGS_RANDOM_REFERENCED", "单位创建编号 " + std::to_string(unit.creationId) + " 仍引用全局掉落表。");
            if (units) if (const auto* reference = std::get_if<MapData::RandomGlobalTable>(&unit.random);
                reference && reference->tableId == id) {
                if (!after) return Bad("SETTINGS_RANDOM_REFERENCED", "随机摆放仍引用待删除单位表。");
                const auto& oldColumns = std::get<MapData::MapRandomUnitTable>(*before).columnTypes;
                const auto& columns = std::get<MapData::MapRandomUnitTable>(*after).columnTypes;
                if (reference->column < 0 || static_cast<std::size_t>(reference->column) >= oldColumns.size()
                    || static_cast<std::size_t>(reference->column) >= columns.size()
                    || oldColumns[reference->column] != columns[reference->column])
                    return Bad("SETTINGS_RANDOM_REFERENCED", "列结构改变会使现有随机摆放越界或改变类别。");
            }
        }
        if (!units && document.Doodads()) for (const auto& doodad : document.Doodads()->doodads)
            if (doodad.droppedItemTable == id) return Bad("SETTINGS_RANDOM_REFERENCED", "可破坏物创建编号 "
                + std::to_string(doodad.creationId) + " 仍引用全局掉落表。");
    }
    const auto context = validation.references ? validation.references() : AuxiliaryReferenceContext{};
    if (!context.completeTriggerSource || !context.triggers)
        return Bad("SETTINGS_RANDOM_REFERENCE_COVERAGE_REQUIRED", "需要完整 T/WCT 来源，才能核对随机表脚本符号 " + symbol + "。");
    const auto& triggers = *context.triggers;
    ProjectReferenceInputs inputs; inputs.triggers = &triggers; inputs.map = &document;
    const auto index = ProjectReferenceIndex::Build(inputs);
    const std::vector<std::string> tokens{symbol};
    if (!index.UsagesOf(tokens).empty() || !index.CallsOf(symbol).empty()
        || std::any_of(triggers.argumentNodes.begin(), triggers.argumentNodes.end(),
            [&](const auto& argument) { return MentionsSymbol(argument.value.text, symbol); })
        || std::any_of(triggers.variables.begin(), triggers.variables.end(),
            [&](const auto& variable) { return variable.hasDefault && MentionsSymbol(variable.value.text, symbol); })
        || std::any_of(triggers.ecaNodes.begin(), triggers.ecaNodes.end(),
            [&](const auto& eca) { return eca.name.text == symbol; }))
        return Bad("SETTINGS_RANDOM_REFERENCED", "触发器参数、变量或调用中仍包含随机表符号 " + symbol + "；当前不能证明其不再被引用，请先核对相关位置。");
    if (!context.opaqueScriptSources.empty() || !Text::TrimLineWhitespace(triggers.wct.customCode.text).empty()
        || std::any_of(triggers.triggers.begin(), triggers.triggers.end(), [](const auto& trigger) {
            return !Text::TrimLineWhitespace(trigger.customText.text).empty(); })
        || std::any_of(triggers.ecaNodes.begin(), triggers.ecaNodes.end(), [&](const auto& eca) { return OpaqueEca(triggers, eca); }))
        return Bad("SETTINGS_RANDOM_SCRIPT_REFERENCES_UNKNOWN", "存在自定义或不透明脚本，无法证明随机表符号 " + symbol + " 的引用及定义完整。");
    return Status::Ok();
}
}
