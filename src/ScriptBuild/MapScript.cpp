#include "MapSections.h"

#include "ScriptNames.h"
#include "TriggerEmitter.h"
#include "War3MapCore/MapData/ClassicMapLayouts.h"
#include "War3MapCore/MapData/GeneratedObjectSymbol.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Script {
namespace detail {
namespace {

bool IsCode(const Trigger::TriggerRecord& trigger) { return trigger.type == 0; }
bool IsEmittedTrigger(const Trigger::TriggerRecord& trigger) { return IsCode(trigger) && trigger.enabled != 0; }

// GUI 输出只收集 JASS 标识符；手写代码保守扫描原文，避免漏掉 vJass 的 //! 宏参数。
void CollectObjectReferences(std::string_view text, std::set<std::string>& output, bool handwritten = false) {
    if (handwritten) {
        for (const std::string_view prefix : {std::string_view("gg_unit_"), std::string_view("gg_item_"), std::string_view("gg_dest_")}) {
            std::size_t position = 0;
            while ((position = text.find(prefix, position)) != std::string_view::npos) {
                std::size_t end = position + prefix.size();
                while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
                output.emplace(text.substr(position, end - position));
                position = end;
            }
        }
        return;
    }
    enum class State { Code, LineComment, BlockComment, String, Rawcode } state = State::Code;
    const auto identifier = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (std::size_t index = 0; index < text.size();) {
        const char current = text[index];
        const char next = index + 1 < text.size() ? text[index + 1] : '\0';
        if (state == State::LineComment) {
            if (current == '\n') state = State::Code;
        } else if (state == State::BlockComment) {
            if (current == '*' && next == '/') { state = State::Code; ++index; }
        } else if (state == State::String || state == State::Rawcode) {
            if (current == '\\' && index + 1 < text.size()) ++index;
            else if (current == (state == State::String ? '"' : '\'')) state = State::Code;
        } else if (current == '/' && next == '/') {
            state = State::LineComment;
            ++index;
        } else if (current == '/' && next == '*') {
            state = State::BlockComment;
            ++index;
        } else if (current == '"' || current == '\'') {
            state = current == '"' ? State::String : State::Rawcode;
        } else if ((index == 0 || !identifier(text[index - 1])) &&
            (text.substr(index).starts_with("gg_unit_") || text.substr(index).starts_with("gg_item_") ||
                text.substr(index).starts_with("gg_dest_"))) {
            std::size_t end = index;
            while (end < text.size() && identifier(text[end])) ++end;
            output.emplace(text.substr(index, end - index));
            index = end;
            continue;
        }
        ++index;
    }
}

// 原版 WTG 会保存停用的 GUI 动作。先验证将被发射的图，再用同一个 TriggerEmitter
// 输出到临时文本收集引用，避免把停用分支或 CommentString 当成必需的全局变量。
class ActiveGraphValidator {
public:
    ActiveGraphValidator(const Trigger::TriggerDocument& document, std::vector<Diagnostic>& diagnostics)
        : document_(document), diagnostics_(diagnostics), ecaState_(document.ecaNodes.size()),
          argumentState_(document.argumentNodes.size()) {}

    bool Trigger(const Trigger::TriggerRecord& trigger) {
        for (const auto index : trigger.ecas) {
            if (index >= document_.ecaNodes.size()) { Fail("ECA", index); break; }
            const auto type = document_.ecaNodes[index].type;
            if (type >= 0 && type <= 2) Eca(index, true, 0);
        }
        return valid_;
    }

private:
    void Fail(std::string_view kind, std::uint32_t index) {
        if (!valid_) return;
        diagnostics_.push_back({DiagnosticSeverity::Error, "Script.Map", "TRIGGER_REFERENCE_GRAPH_INVALID",
            "活动触发器的 " + std::string(kind) + " 节点 " + std::to_string(index) + " 越界、成环或超过 256 层，拒绝脚本重建", {}});
        valid_ = false;
    }

    void Eca(std::uint32_t index, bool gateEnabled, std::size_t depth) {
        if (!valid_) return;
        if (index >= ecaState_.size()) { Fail("ECA", index); return; }
        const auto& eca = document_.ecaNodes[index];
        if (gateEnabled && !TriggerEmitter::Enabled(eca)) return;
        if (depth > 256) { Fail("ECA", index); return; }
        if (ecaState_[index] == 1) { Fail("ECA", index); return; }
        if (ecaState_[index] == 2) return;
        ecaState_[index] = 1;
        for (const auto argument : eca.arguments) Argument(argument, depth + 1);
        for (const auto child : eca.children) Eca(child, true, depth + 1);
        ecaState_[index] = 2;
    }

    void Argument(std::uint32_t index, std::size_t depth) {
        if (!valid_) return;
        if (index >= argumentState_.size() || depth > 256) { Fail("argument", index); return; }
        if (argumentState_[index] == 1) { Fail("argument", index); return; }
        if (argumentState_[index] == 2) return;
        argumentState_[index] = 1;
        const auto& argument = document_.argumentNodes[index];
        // 函数调用参数的 enabled 位不被 TriggerEmitter 用作门禁。
        if (argument.type == 2 && argument.callEca) Eca(*argument.callEca, false, depth + 1);
        if (argument.indexArgument) Argument(*argument.indexArgument, depth + 1);
        argumentState_[index] = 2;
    }

    const Trigger::TriggerDocument& document_;
    std::vector<Diagnostic>& diagnostics_;
    std::vector<std::uint8_t> ecaState_;
    std::vector<std::uint8_t> argumentState_;
    bool valid_ = true;
};

bool CollectEmittedObjectReferences(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    MapContext& context) {
    ActiveGraphValidator validator(document, context.diagnostics);
    ScriptText emitted;
    std::vector<Diagnostic> probeDiagnostics;
    TriggerEmitter emitter(document, catalog, emitted, probeDiagnostics, context.in.debug, context.in.nullSafeHandleSet);
    CollectObjectReferences(document.wct.customCode.text, context.referencedObjects, true);
    for (const auto& trigger : document.triggers) {
        if (!IsEmittedTrigger(trigger)) continue;
        if (trigger.customTextFlag != 0) {
            CollectObjectReferences(trigger.customText.text, context.referencedObjects, true);
        } else {
            if (!validator.Trigger(trigger)) return false;
            emitter.EmitGuiTrigger(trigger);
        }
    }
    CollectObjectReferences(emitted.Text(), context.referencedObjects);
    return true;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') continue;
        if (text[i] == '\n') {
            lines.push_back(std::move(current));
            current.clear();
            continue;
        }
        current += text[i];
    }
    if (!current.empty()) lines.push_back(std::move(current));
    return lines;
}

// WE 按分类顺序输出触发器，分类内保持触发器列表顺序；未归类的排在最后。
std::vector<const Trigger::TriggerRecord*> OrderedTriggers(const Trigger::TriggerDocument& document) {
    std::vector<const Trigger::TriggerRecord*> ordered;
    std::unordered_set<const Trigger::TriggerRecord*> placed;
    for (const auto& category : document.categories) {
        for (const auto& trigger : document.triggers) {
            if (trigger.categoryId == category.categoryId && placed.insert(&trigger).second) ordered.push_back(&trigger);
        }
    }
    for (const auto& trigger : document.triggers) {
        if (placed.insert(&trigger).second) ordered.push_back(&trigger);
    }
    return ordered;
}

void WriteHeader(MapContext& context) {
    auto& out = context.out;
    const auto* settings = context.in.settings;
    out.Line("//===========================================================================");
    out.Line("// ");
    out.Line("// " + (settings != nullptr ? context.Resolve(settings->mapName) : std::string("Untitled")));
    out.Line("// ");
    out.Line("//   Warcraft III map script");
    out.Line("//   Generated by the Warcraft III World Editor");
    if (!context.in.generatedDate.empty()) out.Line("//   Date: " + context.in.generatedDate);
    if (settings != nullptr) out.Line("//   Map Author: " + context.Resolve(settings->author));
    out.Line("// ");
    out.Line("//===========================================================================");
    out.Line("");
}

void WriteTriggerBanner(ScriptText& out, const Trigger::TriggerRecord& trigger) {
    out.Line("//===========================================================================");
    out.Line("// Trigger: " + trigger.name.text);
    const auto lines = SplitLines(trigger.description.text);
    if (!lines.empty()) {
        out.Line("//");
        for (const auto& line : lines) out.Line("// " + line);
    }
    out.Line("//===========================================================================");
}

} // namespace

std::string MapContext::Resolve(std::string_view text) const {
    if (in.strings != nullptr && text.rfind("TRIGSTR_", 0) == 0) {
        if (auto resolved = in.strings->Resolve(text); resolved.Succeeded()) return *resolved.value;
    }
    return std::string(text);
}

std::string MapContext::RegionVariable(std::string_view name) const { return "gg_rct_" + ConvertObjectName(name); }

std::string MapContext::RegionByCreation(std::int32_t creationNumber) const {
    if (in.regions == nullptr) return {};
    for (const auto& region : in.regions->entries) {
        if (region.creationNumber == creationNumber) return RegionVariable(region.name);
    }
    return {};
}

bool MapContext::IsItem(std::string_view rawcode) const {
    return in.queries.isItem ? in.queries.isItem(rawcode) : false;
}

bool MapContext::IsBuilding(std::string_view rawcode) const {
    return in.queries.isBuilding ? in.queries.isBuilding(rawcode) : false;
}

bool MapContext::IsHero(std::string_view rawcode) const {
    if (in.queries.isHero) return in.queries.isHero(rawcode);
    return !rawcode.empty() && rawcode.front() >= 'A' && rawcode.front() <= 'Z';
}

std::string MapContext::PlacedVariable(std::string_view prefix, const MapData::UnitPlacement& unit) const {
    const auto kind = prefix == "gg_unit_" ? "unit" : prefix == "gg_item_" ? "item" : "";
    auto symbol = MapData::GeneratedObjectSymbol(kind, unit.rawcode, unit.creationId);
    if (symbol.Succeeded() && symbol.value) return std::move(*symbol.value);
    // 未被T引用的普通对象仍可用四字节rawcode创建；无合法全局名不等于不能创建对象。
    // 真实参数资格由捕获服务检查，错误脚本引用仍由最终JASS检查器拒绝。
    return {};
}

std::string MapContext::DoodadVariable(const MapData::DoodadPlacement& doodad) const {
    auto symbol = MapData::GeneratedObjectSymbol("dest", doodad.rawcode, doodad.creationId);
    if (symbol.Succeeded() && symbol.value) return std::move(*symbol.value);
    diagnostics.insert(diagnostics.end(), symbol.diagnostics.begin(), symbol.diagnostics.end());
    return {};
}

static bool ValidateDestructables(MapContext& context) {
    std::set<std::string> bound;
    bool valid = true;
    const auto reject = [&](std::string code, std::string message) {
        context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", std::move(code), std::move(message),
            OriginSpan{"war3map.doo", 0, 0, {}}});
        valid = false;
    };
    if (context.in.doodads != nullptr) {
        const auto& entries = context.in.doodads->doodads;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            // 暴雪 HumanX06 原图：flags 0/4 的 51 条恰与 CreateAllDestructables 对应；
            // flags 2/3/6/7 的 2255 条未在原脚本显式创建，保留原 DOO 成员。其他位暂无原版脚本证据。
            if (entry.flags != 0 && entry.flags != 2 && entry.flags != 3 && entry.flags != 4
                && entry.flags != 6 && entry.flags != 7) {
                reject("DESTRUCTABLE_FLAGS_UNKNOWN", "war3map.doo 第 " + std::to_string(index) + " 条的创建标志未验证，拒绝重建脚本");
                continue;
            }
            if ((entry.flags & 2u) != 0) {
                if (entry.droppedItemTable != -1 || HasDoodadDropChoices(entry)) {
                    reject("DOODAD_DROP_UNSUPPORTED", "war3map.doo 第 " + std::to_string(index) + " 条不在脚本创建组却含掉落表，未验证其初始化规则");
                }
                continue;
            }
            const bool identifier = entry.rawcode.size() == 4 && std::all_of(entry.rawcode.begin(), entry.rawcode.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            });
            if (!identifier || entry.creationId < 0) {
                reject("DESTRUCTABLE_ID_INVALID", "war3map.doo 第 " + std::to_string(index) + " 条不能形成可靠的脚本对象名");
                continue;
            }
            if (entry.lifePercent != 100 || entry.droppedItemTable != -1) {
                reject("DESTRUCTABLE_INIT_UNSUPPORTED", "war3map.doo 第 " + std::to_string(index) + " 条含未经原版对照的生命值或全局掉落表");
                continue;
            }
            const std::string name = context.DoodadVariable(entry);
            if (!bound.insert(name).second) {
                reject("DESTRUCTABLE_ID_DUPLICATE", "war3map.doo 第 " + std::to_string(index) + " 条与其他脚本创建对象共用 " + name);
                continue;
            }
            context.scriptedDoodadIndexes.push_back(index);
        }
    }
    for (const auto& reference : context.referencedObjects) {
        if (reference.starts_with("gg_dest_") && !bound.contains(reference)) {
            reject("DESTRUCTABLE_REFERENCE_UNBOUND", "脚本引用 " + reference + "，但最终 war3map.doo 中没有对应的脚本创建对象");
        }
    }
    return valid;
}

void MapContext::Warn(std::string code, std::string message) const {
    diagnostics.push_back({DiagnosticSeverity::Warning, "Script.Map", std::move(code), std::move(message), {}});
}

void WriteSectionBanner(ScriptText& out, std::string_view title) {
    out.Line("//***************************************************************************");
    out.Line("//*");
    out.Line("//*  " + std::string(title));
    out.Line("//*");
    out.Line("//***************************************************************************");
    out.Line("");
}

std::string Real1(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

std::string Real3(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

std::string Rawcode(std::string_view rawcode) { return "'" + std::string(rawcode) + "'"; }

} // namespace detail

MapScriptResult GenerateMapScript(const MapScriptInputs& inputs) {
    MapScriptResult result;
    result.diagnostics = MapData::ValidateClassicMapLayouts({
        inputs.units, inputs.doodads, inputs.cameras, inputs.sounds, inputs.settings}).diagnostics;
    if (!result.Succeeded()) return result;
    if (inputs.triggers == nullptr || inputs.catalog == nullptr) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "inputs-missing",
            "生成整图脚本需要触发器文档和函数库目录。", {}});
        return result;
    }
    const auto& document = *inputs.triggers;
    ScriptText out;
    detail::MapContext context{inputs, out, result.diagnostics, {}, {}};
    if (!detail::CollectEmittedObjectReferences(document, *inputs.catalog, context)) return result;
    if (!detail::ValidateDestructables(context)) return result;

    detail::TriggerEmitter emitter(document, *inputs.catalog, out, result.diagnostics, inputs.debug, inputs.nullSafeHandleSet);
    const auto ordered = detail::OrderedTriggers(document);
    std::vector<std::string> triggerVariables;
    std::unordered_map<std::string, const Trigger::TriggerRecord*> byVariable;
    for (const auto* trigger : ordered) {
        if (!detail::IsCode(*trigger)) continue;
        triggerVariables.push_back(emitter.TriggerVariable(*trigger));
        // 中文名按字节数转换，不同名称可能得到同一标识符；WE 不允许这种重名，这里直接报错而不是交给语法检查。
        const auto [found, inserted] = byVariable.emplace(triggerVariables.back(), trigger);
        if (!inserted) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "TRIGGER_SCRIPT_NAME_DUPLICATE",
                "触发器“" + trigger->name.text + "”与“" + found->second->name.text + "”在脚本中的名称相同（" +
                    triggerVariables.back() + "），请改名", {}});
        }
    }

    detail::WriteHeader(context);
    const std::size_t storageSwitch = detail::WriteGlobals(context, triggerVariables);
    detail::WriteInitGlobals(context);
    detail::WriteItemTables(context);
    detail::WriteDoodadTables(context);
    detail::WriteSounds(context);
    detail::WriteDestructables(context);
    detail::WriteItems(context);
    detail::WriteUnits(context);
    detail::WriteRegions(context);
    detail::WriteCameras(context);

    detail::WriteSectionBanner(out, "Custom Script Code");
    result.authorCode.push_back({0, out.Size(), document.wct.customCode.text.size()});
    out.Put(document.wct.customCode.text);
    if (!document.wct.customCode.text.empty() && document.wct.customCode.text.back() != '\n') out.Line("");
    detail::WriteSectionBanner(out, "Triggers");
    std::vector<const Trigger::TriggerRecord*> initialized;
    std::vector<const Trigger::TriggerRecord*> runOnInit;
    for (const auto* trigger : ordered) {
        if (!detail::IsEmittedTrigger(*trigger)) continue;
        detail::WriteTriggerBanner(out, *trigger);
        if (trigger->customTextFlag != 0) {
            result.authorCode.push_back({trigger->id, out.Size(), trigger->customText.text.size()});
            out.Put(trigger->customText.text);
            if (!trigger->customText.text.empty() && trigger->customText.text.back() != '\n') out.Line("");
            out.Line("");
        } else {
            emitter.EmitGuiTrigger(*trigger);
        }
        ++result.triggerCount;
        initialized.push_back(trigger);
        if (trigger->runOnInit != 0 || (trigger->customTextFlag == 0 && emitter.HasMapInitEvent(*trigger))) {
            runOnInit.push_back(trigger);
        }
    }
    if (!emitter.UsesStorage() && !detail::MentionsStorage(document)) {
        constexpr std::string_view switchText = "#define DISABLE_SAVE_LOAD_SYSTEM\n";
        out.Insert(storageSwitch, switchText);
        for (auto& span : result.authorCode) {
            if (span.byteOffset >= storageSwitch) span.byteOffset += switchText.size();
        }
    }
    out.Line("//===========================================================================");
    out.Line("function InitCustomTriggers takes nothing returns nothing");
    out.Begin();
    for (const auto* trigger : initialized) out.Line("call " + emitter.InitTriggerName(*trigger) + "(  )");
    out.End();
    out.Line("endfunction");
    out.Line("");
    out.Line("//===========================================================================");
    out.Line("function RunInitializationTriggers takes nothing returns nothing");
    out.Begin();
    for (const auto* trigger : runOnInit) out.Line("call ConditionalTriggerExecute( " + emitter.TriggerVariable(*trigger) + " )");
    out.End();
    out.Line("endfunction");
    out.Line("");

    const bool upgrades = detail::WriteUpgrades(context);
    const bool techTree = detail::WriteTechRestrictions(context);
    detail::WritePlayers(context);
    detail::WriteMain(context, upgrades, techTree);
    detail::WriteConfig(context);

    const auto& marks = out.OwnerMarks();
    for (std::size_t i = 0; i < marks.size(); ++i) {
        if (marks[i].owner.trigger == 0) continue;
        const std::size_t end = i + 1 < marks.size() ? marks[i + 1].offset : out.Size();
        if (end > marks[i].offset) result.statements.push_back({marks[i].owner.trigger, marks[i].owner.eca, marks[i].offset, end - marks[i].offset});
    }
    result.text = out.Take();
    for (std::size_t position = 0; (position = result.text.find("\nfunction ", position)) != std::string::npos; ++position) {
        ++result.functionCount;
    }
    return result;
}

bool ValidTerrainExtent(const TerrainExtent& extent) noexcept {
    if (extent.width < 2 || extent.width > 2049 || extent.height < 2 || extent.height > 2049) return false;
    const double values[]{extent.offsetX, extent.offsetY,
        extent.offsetX + (static_cast<double>(extent.width) - 1) * 128,
        extent.offsetY + (static_cast<double>(extent.height) - 1) * 128};
    for (const auto value : values) if (!std::isfinite(value) || std::abs(value) > 16777216.0) return false;
    return true;
}

std::optional<TerrainExtent> ReadTerrainExtent(std::span<const std::byte> w3e) {
    std::size_t offset = 0;
    const auto take = [&](std::size_t size) -> const std::byte* {
        if (size > w3e.size() - offset) return nullptr;
        const auto* at = w3e.data() + offset;
        offset += size;
        return at;
    };
    const auto integer = [&]() -> std::optional<std::int32_t> {
        const auto* at = take(4);
        if (at == nullptr) return std::nullopt;
        std::int32_t value = 0;
        std::memcpy(&value, at, 4);
        return value;
    };
    const auto* magic = take(4);
    if (magic == nullptr || std::memcmp(magic, "W3E!", 4) != 0) return std::nullopt;
    // 版本、主地形集、自定义地形标记。
    const auto version = integer();
    if (!version || *version != 11 || take(1) == nullptr || !integer()) return std::nullopt;
    for (int list = 0; list < 2; ++list) {
        const auto count = integer();
        if (!count || *count < 0 || *count > 256 || take(static_cast<std::size_t>(*count) * 4) == nullptr) return std::nullopt;
    }
    const auto width = integer();
    const auto height = integer();
    const auto* offsets = take(8);
    if (!width || !height || offsets == nullptr) return std::nullopt;
    TerrainExtent extent{*width, *height, 0.0f, 0.0f};
    std::memcpy(&extent.offsetX, offsets, 4);
    std::memcpy(&extent.offsetY, offsets + 4, 4);
    return ValidTerrainExtent(extent) ? std::optional{extent} : std::nullopt;
}

} // namespace War3Map::Script
