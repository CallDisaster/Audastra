#include "MapSections.h"

#include "ScriptNames.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <cstdio>

namespace War3Map::Script::detail {
namespace {

std::string DefineField(const Trigger::UiCatalog& catalog, std::string_view group, std::string_view name, std::size_t field) {
    const auto* definition = catalog.Find(Trigger::UiDefinitionKind::Define, name, group);
    if (definition == nullptr || definition->revisions.empty()) return {};
    for (const auto& [key, value] : definition->Active().fields) {
        if (key != "value") continue;
        const auto parts = SplitDefineValue(value);
        return field < parts.size() ? parts[field] : std::string{};
    }
    return {};
}

// 变量的 JASS 类型：TriggerTypes 第五列为基础类型（unitcode → integer）。
std::string JassType(const Trigger::UiCatalog& catalog, const Trigger::TriggerTypeSystem& types, const std::string& type) {
    if (types.LiteralKind(type) == Trigger::TriggerTypeSystem::Literal::ObjectCode) return "integer";
    const std::string base = DefineField(catalog, "TriggerTypes", type, 4);
    return base.empty() ? type : base;
}

std::string DeclaredDefault(const std::string& jassType) {
    if (jassType == "integer" || jassType == "real") return "0";
    if (jassType == "boolean") return "false";
    if (jassType == "string") return "\"\"";
    return "null";
}

std::string Padded(std::string text, std::size_t width) {
    if (text.size() < width) text.append(width - text.size(), ' ');
    return text;
}

void Declare(ScriptText& out, const std::string& type, const std::string& name, const std::string& value) {
    out.Line(Padded(type, 23) + " " + Padded(name, 26) + " = " + value);
}

// 变量初始值：有初始值文本时按类型写字面量；只勾选了初始值时写类型零值；否则用类型默认构造。
std::string InitialValue(MapContext& context, const Trigger::TriggerTypeSystem& types, const Trigger::VariableRecord& variable) {
    const auto& catalog = *context.in.catalog;
    const std::string& type = variable.typeName.text;
    const std::string jass = JassType(catalog, types, type);
    const std::string& value = variable.value.text;
    if (!value.empty() || variable.hasDefault != 0) {
        if (value.empty()) return DeclaredDefault(jass);
        const auto role = types.LiteralKind(type);
        if (role == Trigger::TriggerTypeSystem::Literal::ObjectCode
            || role == Trigger::TriggerTypeSystem::Literal::Other) {
            const auto checked = types.ValidateLiteral(type, value);
            if (!checked.Succeeded()) {
                for (auto diagnostic : checked.diagnostics) {
                    diagnostic.origin = variable.origin;
                    context.diagnostics.push_back(std::move(diagnostic));
                }
                return {};
            }
        }
        if (role == Trigger::TriggerTypeSystem::Literal::ObjectCode) return Rawcode(value);
        if (jass == "string") return QuoteJassString(value);
        if (value.rfind("gg_", 0) == 0) return value;
        // 预设值（TriggerParams 中同类型的条目）写其脚本文本；基础类型为 integer 的枚举（如属性类型）同样适用。
        if (DefineField(catalog, "TriggerParams", value, 1) == type) {
            std::string preset = DefineField(catalog, "TriggerParams", value, 2);
            for (auto& ch : preset) {
                if (ch == '`') ch = '"';
            }
            if (!preset.empty()) return preset;
        }
        return value;
    }
    return DefineField(catalog, "TriggerTypeDefaults", type, 0);
}

} // namespace

bool MentionsStorage(const Trigger::TriggerDocument& document) {
    const auto mentions = [](std::string_view text) {
        for (const std::string_view word : {"YDLocal", "YDUserData", "YDHash", "YDLOC", "YDHT", "YDTRIGGER_"}) {
            if (text.find(word) != std::string_view::npos) return true;
        }
        return false;
    };
    if (mentions(document.wct.customCode.text)) return true;
    for (const auto& trigger : document.triggers) {
        if (mentions(trigger.customText.text)) return true;
    }
    for (const auto& argument : document.argumentNodes) {
        if (mentions(argument.value.text)) return true;
    }
    return false;
}

std::size_t WriteGlobals(MapContext& context, const std::vector<std::string>& triggerVariables) {
    auto& out = context.out;
    const auto& catalog = *context.in.catalog;
    const Trigger::TriggerTypeSystem types(catalog);
    WriteSectionBanner(out, "Global Variables");
    out.Line("#define USE_BJ_ANTI_LEAK");
    // YDWE 每次保存改写 YDTrigger/Import.h：没有用到存储时定义 DISABLE_SAVE_LOAD_SYSTEM。这里由生成器直接决定。
    const std::size_t storageSwitch = out.Size();
    out.Line("#include <YDTrigger/YDTrigger.h>");
    out.Line("globals");
    out.Begin();
    out.Line("// User-defined");
    for (const auto& variable : context.in.triggers->variables) {
        const std::string type = JassType(catalog, types, variable.typeName.text);
        const std::string name = "udg_" + variable.name.text;
        if (variable.isArray != 0) out.Line(Padded(type + " array", 23) + " " + name);
        else Declare(out, type, name, DeclaredDefault(type));
    }
    out.Line("");
    out.Line("// Generated");
    if (context.in.regions != nullptr) {
        for (const auto& region : context.in.regions->entries) Declare(out, "rect", context.RegionVariable(region.name), "null");
    }
    if (context.in.cameras != nullptr) {
        for (const auto& camera : context.in.cameras->entries) Declare(out, "camerasetup", "gg_cam_" + ConvertObjectName(camera.name), "null");
    }
    if (context.in.sounds != nullptr) {
        for (const auto& sound : context.in.sounds->entries) {
            const std::string name = sound.name.rfind("gg_snd_", 0) == 0 ? sound.name : "gg_snd_" + ConvertObjectName(sound.name);
            // WE 把音乐声明为不带初始值的 string（值在 InitSounds 里设置）。
            if (sound.IsMusic()) out.Line(Padded("string", 23) + " " + name);
            else Declare(out, "sound", name, "null");
        }
    }
    for (const auto& variable : triggerVariables) Declare(out, "trigger", variable, "null");
    if (context.in.units != nullptr) {
        for (const auto& unit : context.in.units->units) {
            const bool item = context.IsItem(unit.rawcode);
            const std::string name = context.PlacedVariable(item ? "gg_item_" : "gg_unit_", unit);
            if (context.referencedObjects.contains(name)) Declare(out, item ? "item" : "unit", name, "null");
        }
    }
    if (context.in.doodads != nullptr) {
        for (const auto index : context.scriptedDoodadIndexes) {
            const std::string name = context.DoodadVariable(context.in.doodads->doodads[index]);
            if (context.referencedObjects.contains(name)) Declare(out, "destructable", name, "null");
        }
    }
    out.End();
    out.Line("#include <YDTrigger/Globals.h>");
    out.Line("endglobals");
    out.Line("#include <YDTrigger/Function.h>");
    out.Line("");
    return storageSwitch;
}

void WriteInitGlobals(MapContext& context) {
    auto& out = context.out;
    const auto& catalog = *context.in.catalog;
    const Trigger::TriggerTypeSystem types(catalog);
    out.Line("function InitGlobals takes nothing returns nothing");
    out.Begin();
    out.Line("local integer i = 0");
    for (const auto& variable : context.in.triggers->variables) {
        const std::string value = InitialValue(context, types, variable);
        if (value.empty()) continue;
        const std::string name = "udg_" + variable.name.text;
        if (variable.isArray == 0) {
            out.Line("set " + name + " = " + value);
            continue;
        }
        out.Line("set i = 0");
        out.Line("loop");
        out.Begin();
        out.Line("exitwhen (i > " + std::to_string(variable.size) + ")");
        out.Line("set " + name + "[i] = " + value);
        out.Line("set i = i + 1");
        out.End();
        out.Line("endloop");
        out.Line("");
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
}

} // namespace War3Map::Script::detail
