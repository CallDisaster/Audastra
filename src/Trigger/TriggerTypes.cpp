#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/TriggerDataText.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <functional>
#include <set>
#include <tuple>

namespace War3Map::Trigger {
namespace {

std::optional<std::string_view> Field(const UiDefinitionRevision& revision, std::string_view name) {
    const auto found = std::find_if(revision.fields.begin(), revision.fields.end(),
        [name](const auto& field) { return field.first == name; });
    return found == revision.fields.end() ? std::nullopt : std::optional<std::string_view>(found->second);
}

bool IsLocalGetter(std::string_view name) {
    return name == kLocalGet || name == kLocalGetArray;
}

bool IsAnyType(std::string_view type) {
    return type == "AnyType" || type == "anytype";
}

std::string_view ClassicNativeParent(std::string_view type) {
    // 经典 1.27a common.j 的 type ... extends 声明事实；UI TriggerTypes 第 5 列
    // 没有完整记录此父链，不能由“都是句柄”或 UI 同基础类型猜测可赋值关系。
    constexpr std::pair<std::string_view, std::string_view> parents[] = {
        {"agent", "handle"}, {"event", "agent"}, {"player", "agent"},
        {"widget", "agent"}, {"unit", "widget"}, {"destructable", "widget"},
        {"item", "widget"}, {"ability", "agent"}, {"buff", "ability"},
        {"force", "agent"}, {"group", "agent"}, {"trigger", "agent"},
        {"triggercondition", "agent"}, {"triggeraction", "handle"}, {"timer", "agent"},
        {"location", "agent"}, {"region", "agent"}, {"rect", "agent"},
        {"boolexpr", "agent"}, {"sound", "agent"}, {"conditionfunc", "boolexpr"},
        {"filterfunc", "boolexpr"}, {"unitpool", "handle"}, {"itempool", "handle"},
        {"race", "handle"}, {"alliancetype", "handle"}, {"racepreference", "handle"},
        {"gamestate", "handle"}, {"igamestate", "gamestate"}, {"fgamestate", "gamestate"},
        {"playerstate", "handle"}, {"playerscore", "handle"}, {"playergameresult", "handle"},
        {"unitstate", "handle"}, {"aidifficulty", "handle"}, {"eventid", "handle"},
        {"gameevent", "eventid"}, {"playerevent", "eventid"}, {"playerunitevent", "eventid"},
        {"unitevent", "eventid"}, {"limitop", "eventid"}, {"widgetevent", "eventid"},
        {"dialogevent", "eventid"}, {"unittype", "handle"}, {"gamespeed", "handle"},
        {"gamedifficulty", "handle"}, {"gametype", "handle"}, {"mapflag", "handle"},
        {"mapvisibility", "handle"}, {"mapsetting", "handle"}, {"mapdensity", "handle"},
        {"mapcontrol", "handle"}, {"playerslotstate", "handle"}, {"volumegroup", "handle"},
        {"camerafield", "handle"}, {"camerasetup", "handle"}, {"playercolor", "handle"},
        {"placement", "handle"}, {"startlocprio", "handle"}, {"raritycontrol", "handle"},
        {"blendmode", "handle"}, {"texmapflags", "handle"}, {"effect", "agent"},
        {"effecttype", "handle"}, {"weathereffect", "handle"}, {"terraindeformation", "handle"},
        {"fogstate", "handle"}, {"fogmodifier", "agent"}, {"dialog", "agent"},
        {"button", "agent"}, {"quest", "agent"}, {"questitem", "agent"},
        {"defeatcondition", "agent"}, {"timerdialog", "agent"}, {"leaderboard", "agent"},
        {"multiboard", "agent"}, {"multiboarditem", "agent"}, {"trackable", "agent"},
        {"gamecache", "agent"}, {"version", "handle"}, {"itemtype", "handle"},
        {"texttag", "handle"}, {"attacktype", "handle"}, {"damagetype", "handle"},
        {"weapontype", "handle"}, {"soundtype", "handle"}, {"lightning", "handle"},
        {"pathingtype", "handle"}, {"image", "handle"}, {"ubersplat", "handle"},
        {"hashtable", "agent"},
    };
    for (const auto& [child, parent] : parents) if (child == type) return parent;
    return {};
}

bool ClassicHandleType(std::string_view type) {
    return type == "handle" || !ClassicNativeParent(type).empty();
}

bool ClassicPrimitiveType(std::string_view type) {
    return type == "nothing" || type == "integer" || type == "real" || type == "boolean"
        || type == "string" || type == "code" || ClassicHandleType(type);
}

bool ObjectCodeType(std::string_view type) {
    return type == "unitcode" || type == "itemcode" || type == "abilcode" || type == "buffcode"
        || type == "techcode" || type == "doodadcode" || type == "destructablecode" || type == "heroskillcode";
}

struct LiteralResolution {
    TriggerTypeSystem::Literal kind = TriggerTypeSystem::Literal::Other;
    bool valid = true;
};

LiteralResolution ResolveLiteral(const TriggerTypeSystem& types, std::string_view type) {
    using Literal = TriggerTypeSystem::Literal;
    std::array<std::string_view, 32> visited{};
    bool objectCode = false;
    for (std::size_t depth = 0; depth < visited.size(); ++depth) {
        if (type == "integer") return {objectCode ? Literal::ObjectCode : Literal::Integer};
        if (ClassicPrimitiveType(type) || type == "scriptcode") {
            // 对象编号必须有完整的 integer 父链，不能由损坏的目录变成字符串或句柄。
            if (objectCode) return {Literal::Other, false};
            if (type == "real") return {Literal::Real};
            if (type == "boolean") return {Literal::Boolean};
            if (type == "string") return {Literal::String};
            if (type == "scriptcode") return {Literal::Script};
            return {};
        }
        if (types.IsTypeSelector(type)) return {Literal::Other, !objectCode};
        for (std::size_t previous = 0; previous < depth; ++previous)
            if (visited[previous] == type) return {Literal::Other, !objectCode};
        visited[depth] = type;
        objectCode = objectCode || ObjectCodeType(type);
        const auto* info = types.Find(type);
        if (info == nullptr) return {Literal::Other, !objectCode};
        if (info->baseType.empty()) return {Literal::Other, !objectCode};
        type = info->baseType;
    }
    return {Literal::Other, !objectCode};
}

const ArgumentNode* ArgumentAt(const TriggerDocument& document, const EcaNode& eca, std::size_t position) {
    if (position >= eca.arguments.size() || eca.arguments[position] >= document.argumentNodes.size()) return nullptr;
    return &document.argumentNodes[eca.arguments[position]];
}

} // namespace

TriggerTypeSystem::TriggerTypeSystem(const UiCatalog& catalog, const MapObjectReferenceSnapshot* mapObjects)
    : catalog_(catalog), mapObjects_(mapObjects) {
    for (const auto* definition : catalog_.Definitions(UiDefinitionKind::Define)) {
        const auto value = Field(definition->Active(), "value");
        if (!value.has_value()) continue;
        if (definition->group == "TriggerTypes") {
            const auto columns = SplitTriggerDataCsv(*value);
            TriggerTypeInfo info;
            info.name = definition->name;
            info.canBeGlobal = columns.size() > 1 && columns[1] == "1";
            info.comparable = columns.size() > 2 && columns[2] == "1";
            info.display = columns.size() > 3 ? columns[3] : std::string{};
            info.baseType = columns.size() > 4 ? columns[4] : std::string{};
            // YDWE 基础框架的 StringExt（长文本）只写到第 5 列，但在 YDWE 中与文本参数一样可选文本变量和函数。
            info.editorUsesBaseType = !info.baseType.empty()
                && ((columns.size() > 6 && columns[6] == "1") || (info.name == "StringExt" && info.baseType == "string"));
            types_[info.name] = std::move(info);
        } else if (definition->group == "TriggerParams") {
            const auto columns = SplitTriggerDataCsv(*value);
            if (columns.size() > 1) presetTypes_[definition->name] = columns[1];
        }
    }
    // 类型选择器：全部预设都形如“类型名 + 编号_目标类型”的类型（YDWE 的 typename、objecttype）。
    std::unordered_map<std::string, bool> selector;
    for (const auto& [preset, type] : presetTypes_) {
        const bool shaped = preset.starts_with(type) && preset.find('_', type.size()) != std::string::npos;
        const auto [it, inserted] = selector.try_emplace(type, shaped);
        if (!inserted) it->second = it->second && shaped;
    }
    for (const auto& [type, shaped] : selector) {
        if (shaped) selectors_.insert(type);
    }
}

bool TriggerTypeSystem::IsTypeSelector(std::string_view type) const {
    return selectors_.contains(std::string(type));
}

TriggerTypeSystem::Literal TriggerTypeSystem::LiteralKind(std::string_view type) const {
    return ResolveLiteral(*this, type).kind;
}

bool TriggerTypeSystem::IsObjectCodeLiteral(std::string_view value) {
    return value.size() == 4 && value != "null" && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 32 && c <= 126 && c != '\'' && c != '\\';
    });
}

std::optional<UiDefinitionKind> TriggerTypeSystem::FunctionSlotKind(std::string_view type) {
    if (type == "eventcall") return UiDefinitionKind::Event;
    if (type == "boolexpr" || type == "boolcall") return UiDefinitionKind::Condition;
    if (type == "code") return UiDefinitionKind::Action;
    return std::nullopt;
}

bool TriggerTypeSystem::IsIntegerLiteral(std::string_view value) {
    return ParseIntegerLiteral(value).has_value();
}

std::optional<std::int32_t> TriggerTypeSystem::ParseIntegerLiteral(std::string_view value) {
    if (value.empty()) return {};
    if (value.starts_with("0x") || value.starts_with("0X") || value.starts_with('$')) {
        value.remove_prefix(value.front() == '$' ? 1 : 2);
        if (value.empty()) return {};
        std::uint32_t bits = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), bits, 16);
        if (result.ec == std::errc{} && result.ptr == value.data() + value.size()) return std::bit_cast<std::int32_t>(bits);
        return {};
    }
    std::int32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == value.data() + value.size()) return parsed;
    return {};
}

bool TriggerTypeSystem::IsRealLiteral(std::string_view value) {
    if (value.empty()) return false;
    bool dot = false, digit = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && !dot) dot = true;
        else if (c != '-' || i != 0) return false;
    }
    if (!digit) return false;
    float number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && std::isfinite(number);
}

namespace {
// 只去掉空格与制表符；换行写进脚本会断开语句，仍按非法处理。
std::string_view TrimSpaces(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}
} // namespace

Status TriggerTypeSystem::ValidateLiteral(std::string_view type, std::string_view value) const {
    const auto resolved = ResolveLiteral(*this, type);
    const Literal kind = resolved.kind;
    std::string message;
    if (value.find('\0') != std::string_view::npos) message = "参数文本不能包含 NUL 字符。";
    else if (!resolved.valid)
        message = "对象编号的类型未定义、别名成环，或没有有效的整数父链：" + std::string(type) + "。";
    else if (kind == Literal::ObjectCode && !IsObjectCodeLiteral(value))
        message = "对象编号固定值必须是四个可打印 ASCII 字节；不加引号、不使用反斜杠、转义或 null。";
    // 数值前后的空格（WE 原样存下、原样写进脚本，JASS 也接受）不是表达式，核对时忽略。
    else if (kind == Literal::Integer && !IsIntegerLiteral(TrimSpaces(value)))
        message = "请输入 int32 十进制或32位十六进制整数（0x 或 $ 前缀）；固定值不接受表达式。";
    else if (kind == Literal::Real && !IsRealLiteral(TrimSpaces(value)))
        message = "请输入有限的十进制实数；固定值不接受指数、NaN 或表达式。";
    else if (kind == Literal::Boolean && value != "true" && value != "false")
        message = "布尔固定值只能是真（true）或假（false）。";
    else if (kind == Literal::String && std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 32 && c != '\n' && c != '\r';
        })) message = "字符串含尚未支持的控制字符。";
    // 该类型有预设时列出几个，免得只看到底层整数/实数格式要求而猜不出该写什么。
    if (!message.empty() && kind != Literal::String) {
        const auto presets = PresetsOfType(catalog_, type);
        if (!presets.empty()) {
            message += " 类型 " + std::string(type) + " 的预设有：";
            for (std::size_t i = 0; i < presets.size() && i < 8; ++i) message += (i ? "、" : "") + presets[i].name;
            message += presets.size() > 8 ? " 等 " + std::to_string(presets.size()) + " 个（查询 presets 表可列全）。" : "。";
        }
    }
    return message.empty() ? Status{} : Status::Failure("Trigger.Type", "TRIGGER_LITERAL_INVALID", std::move(message));
}

const TriggerTypeInfo* TriggerTypeSystem::Find(std::string_view type) const {
    const auto found = types_.find(std::string(type));
    return found == types_.end() ? nullptr : &found->second;
}

std::string TriggerTypeSystem::DisplayName(std::string_view type) const {
    const auto* info = Find(type);
    if (info == nullptr || info->display.empty()) return std::string(type);
    std::string display = catalog_.Localize(info->display);
    return Game::WorldEditStrings::IsKey(display) ? std::string(type) : display;
}

std::string_view TriggerTypeSystem::CanonicalUiType(std::string_view type) const {
    std::array<std::string_view, 32> visited{};
    for (std::size_t depth = 0; depth < visited.size(); ++depth) {
        // 已知 native 类型不能被 UI 别名列覆盖为标量或另一个句柄类型。
        if (ClassicPrimitiveType(type)) return type;
        for (std::size_t previous = 0; previous < depth; ++previous)
            if (visited[previous] == type) return {};
        visited[depth] = type;
        const auto* info = Find(type);
        if (info == nullptr || info->baseType.empty()) return type;
        type = info->baseType;
    }
    return {}; // UI 别名成环或超过已有限制时，不把它猜成 handle。
}

bool TriggerTypeSystem::Accepts(std::string_view expected, std::string_view actual) const {
    if (expected.empty() || actual.empty() || expected == actual) return true;
    if (actual == "AnyReturnType" || IsAnyType(expected) || expected == "Null" || expected == "AnyGlobal") return true;
    expected = CanonicalUiType(expected); actual = CanonicalUiType(actual);
    if (expected.empty() || actual.empty()) return false;
    if (expected == actual) return true; // degree/radian 等 UI 别名仍与实际标量互通。
    for (std::size_t depth = 0; depth < 32; ++depth) {
        actual = ClassicNativeParent(actual);
        if (actual.empty()) return false;
        if (actual == expected) return true;
    }
    return false;
}

TriggerTypeSystem::ValueKinds TriggerTypeSystem::AllowedValueKinds(std::string_view expected) const {
    ValueKinds kinds;
    const auto* info = Find(expected);
    const bool nativeHandle = ClassicHandleType(CanonicalUiType(expected));
    if (expected.empty() || expected == "Null" || expected == "AnyGlobal" || IsAnyType(expected)
        || (info == nullptr && !nativeHandle)) return kinds;
    if (info != nullptr && info->editorUsesBaseType && info->baseType != expected) {
        // 按基础类型提供变量/函数；预设仍只取本类型自己的。
        kinds = AllowedValueKinds(info->baseType);
        kinds.preset = !PresetsOfType(catalog_, expected).empty();
        return kinds;
    }
    kinds.variable = info != nullptr && info->canBeGlobal;
    if (nativeHandle && !kinds.variable) {
        for (const auto& [name, candidate] : types_) {
            if (candidate.canBeGlobal && Accepts(expected, name)) { kinds.variable = true; break; }
        }
    }
    kinds.preset = !PresetsOfType(catalog_, expected).empty();
    kinds.function = info != nullptr && info->canBeGlobal;
    if (!kinds.function) {
        for (const auto* definition : catalog_.Definitions(UiDefinitionKind::Call)) {
            const auto returns = Field(definition->Active(), "returns").value_or(std::string_view{});
            // 不可作变量的类型（如 scriptcode 代码名）只接受声明返回该类型本身的函数：
            // 按基础类型放宽或 AnyReturnType 读取会把运行时值填进代码名之类的槽位。
            if (returns == expected || (nativeHandle && !returns.empty() && returns != "AnyReturnType"
                    && Accepts(expected, returns))) {
                kinds.function = true;
                break;
            }
        }
    }
    return kinds;
}

std::optional<std::string> TriggerTypeSystem::PresetType(std::string_view preset) const {
    auto found = presetTypes_.find(std::string(preset));
    // 与 UiCatalog::Find 相同：WE 保存的预设名可能带着定义文件键里的空格。
    if (found == presetTypes_.end()) found = presetTypes_.find(War3Map::Text::TrimCWhitespaceCopy(preset));
    return found == presetTypes_.end() ? std::nullopt : std::optional<std::string>(found->second);
}

std::string TriggerTypeSystem::DeclaredType(const UiDefinition& definition, std::size_t position) {
    std::size_t shown = 0;
    for (const auto& argument : definition.Active().arguments) {
        if (argument.type == "nothing") continue;
        if (shown++ == position) return argument.type;
    }
    return {};
}

std::string TriggerTypeSystem::TypeFromTypename(std::string_view value) {
    // typename07_unit → unit：取第一个下划线之后的部分（与 YDWE 相同）。
    const std::size_t underscore = value.find('_');
    return underscore == std::string_view::npos ? std::string{} : std::string(value.substr(underscore + 1));
}

std::string TriggerTypeSystem::ExpectedType(const TriggerDocument& document, const EcaNode& eca,
    std::size_t position, std::span<const LocalVariable> locals) const {
    const UiDefinition* definition = catalog_.Find(eca.type, eca.name.text);
    if (definition == nullptr) return {};
    const std::string declared = DeclaredType(*definition, position);
    if (declared != "Null") return declared;
    // YDWE：前面最近的类型选择器参数（typename / objecttype）决定类型。
    for (std::size_t index = position; index-- > 0;) {
        if (!IsTypeSelector(DeclaredType(*definition, index))) continue;
        const ArgumentNode* source = ArgumentAt(document, eca, index);
        if (source == nullptr || source->type != 0) return "Null";
        const std::string type = TypeFromTypename(source->value.text);
        return type.empty() ? "Null" : type;
    }
    // 原版（SetVariable 等）：跟随前一个参数的值类型。
    if (position == 0) return "Null";
    const ArgumentNode* previous = ArgumentAt(document, eca, position - 1);
    if (previous == nullptr) return "Null";
    std::string type = ValueType(document, *previous, locals);
    if (type.empty()) type = ExpectedType(document, eca, position - 1, locals);
    return type.empty() || type == "AnyGlobal" || IsAnyType(type) ? "Null" : type;
}

std::string TriggerTypeSystem::ValueType(const TriggerDocument& document, const ArgumentNode& argument,
    std::span<const LocalVariable> locals) const {
    switch (argument.type) {
    case 0: return PresetType(argument.value.text).value_or(std::string{});
    case 1: {
        const auto reference = ResolveVariableReference(document, argument.value.text, mapObjects_);
        return reference.Succeeded() ? reference.value->type : std::string{};
    }
    case 2:
        if (argument.callEca.has_value() && *argument.callEca < document.ecaNodes.size()) {
            return ReturnType(document, document.ecaNodes[*argument.callEca], locals);
        }
        return {};
    default: return {};
    }
}

Status TriggerTypeSystem::ValidateWritableVariable(const TriggerDocument& document, const ArgumentNode& argument,
    std::string_view expectedType, std::string* declaredName) const {
    const auto fail = [&](std::string code, std::string message) {
        auto status = Status::Failure("Trigger.Type", std::move(code), std::move(message));
        status.diagnostics.front().origin = argument.origin;
        return status;
    };
    if (argument.type != 1 || argument.callEca || argument.insertCall != 0)
        return fail("TRIGGER_WRITABLE_VARIABLE_REQUIRED", "此参数必须是可写变量，不能使用调用结果、预设或字面值。");
    const auto reference = ResolveVariableReference(document, argument.value.text, mapObjects_);
    if (!reference.Succeeded()) {
        const bool ambiguous = std::any_of(reference.diagnostics.begin(), reference.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "TRIGGER_GRAPH_VARIABLE_AMBIGUOUS"; });
        return fail(ambiguous ? "TRIGGER_WRITABLE_VARIABLE_AMBIGUOUS" : "TRIGGER_WRITABLE_VARIABLE_UNKNOWN",
            "可写变量没有当前文档中的唯一真实声明：" + argument.value.text);
    }
    if (reference.value->kind != VariableReferenceKind::Declared || !reference.value->writable)
        return fail("TRIGGER_WRITABLE_VARIABLE_UNKNOWN", "生成的触发器或预放对象只允许读取，不能作为赋值目标：" + argument.value.text);
    const VariableRecord* variable = nullptr;
    for (const auto& candidate : document.variables) {
        if (candidate.name.text != argument.value.text && (!argument.value.text.starts_with("udg_")
            || candidate.name.text != std::string_view(argument.value.text).substr(4))) continue;
        if (variable != nullptr)
            return fail("TRIGGER_WRITABLE_VARIABLE_AMBIGUOUS", "可写变量原名或 udg_ 引用命中多个声明：" + argument.value.text);
        variable = &candidate;
    }
    if (variable == nullptr)
        return fail("TRIGGER_WRITABLE_VARIABLE_UNKNOWN", "可写变量没有当前文档中的真实声明：" + argument.value.text);
    const auto expected = CanonicalUiType(expectedType), actual = CanonicalUiType(variable->typeName.text);
    // 左值同时参与读取与写入，不能把未知/AnyReturnType 或单向父类型兼容当作可写资格。
    if (expected.empty() || actual.empty() || expected != actual || IsAnyType(expected)
        || expected == "Null" || expected == "AnyGlobal" || expected == "AnyReturnType")
        return fail("TRIGGER_WRITABLE_VARIABLE_TYPE", "可写变量“" + argument.value.text + "”的类型“"
            + variable->typeName.text + "”与所需类型“" + std::string(expectedType) + "”不符。");
    if ((variable->isArray != 0 && variable->isArray != 1)
        || (argument.insertIndex != 0 && argument.insertIndex != 1)
        || (argument.insertIndex != 0) != argument.indexArgument.has_value()
        || (variable->isArray != 0) != argument.indexArgument.has_value())
        return fail("TRIGGER_WRITABLE_VARIABLE_SHAPE", "可写变量的数组声明、索引标志与实际下标必须一致。");
    if (argument.indexArgument && *argument.indexArgument >= document.argumentNodes.size())
        return fail("TRIGGER_WRITABLE_VARIABLE_SHAPE", "可写变量的数组下标引用越界。");
    if (declaredName != nullptr) *declaredName = variable->name.text;
    return Status::Ok();
}

std::string TriggerTypeSystem::ReturnType(const TriggerDocument& document, const EcaNode& call,
    std::span<const LocalVariable> locals) const {
    // code 参数的值是一个动作（ForGroup 等）；条件节点作为参数时不约束。
    if (call.type == 2) return "code";
    if (call.type != 3) return {};
    const UiDefinition* definition = catalog_.Find(UiDefinitionKind::Call, call.name.text);
    if (definition == nullptr) return {};
    std::string returns(Field(definition->Active(), "returns").value_or(std::string_view{}));
    if (returns == "AnyReturnType" && (call.name.text == kLocalGet || call.name.text == kLocalGetArray)) {
        // YDWE 按“名称 + 类型”区分局部变量：同名只有一种类型时才能确定，否则由所在参数决定。
        const ArgumentNode* name = ArgumentAt(document, call, 0);
        const bool array = call.name.text == kLocalGetArray;
        std::string found;
        if (name != nullptr && name->type != 2) {
            for (const LocalVariable& local : locals) {
                if (local.loop || local.isArray != array || local.name != name->value.text) continue;
                if (!found.empty() && found != local.type) return returns;
                found = local.type;
            }
        }
        if (!found.empty()) return found;
    }
    return returns;
}

std::vector<LocalVariable> TriggerTypeSystem::CollectLocals(const TriggerDocument& document,
    const TriggerRecord& trigger) const {
    std::vector<LocalVariable> locals;
    std::set<std::tuple<std::string, std::string, bool, bool>> seen;
    std::vector<char> visited(document.ecaNodes.size(), 0);
    const auto add = [&](LocalVariable local) {
        if (local.name.empty()) return;
        if (seen.emplace(local.name, local.type, local.isArray, local.loop).second) locals.push_back(std::move(local));
    };
    const std::function<void(std::uint32_t)> walk = [&](std::uint32_t index) {
        if (index >= document.ecaNodes.size() || visited[index]) return;
        visited[index] = 1;
        const EcaNode& eca = document.ecaNodes[index];
        const auto text = [&](std::size_t position) -> std::string {
            const ArgumentNode* argument = ArgumentAt(document, eca, position);
            return argument == nullptr || argument->type == 2 ? std::string{} : argument->value.text;
        };
        if (eca.name.text == kLocalSet || eca.name.text == kLocalSetArray) {
            add({text(1), TypeFromTypename(text(0)), eca.name.text == kLocalSetArray, false});
        } else if (eca.name.text == kLoopLocal) {
            add({text(0), "integer", false, true});
        }
        for (const std::uint32_t argumentIndex : eca.arguments) {
            if (argumentIndex >= document.argumentNodes.size()) continue;
            const ArgumentNode& argument = document.argumentNodes[argumentIndex];
            if (argument.callEca.has_value()) walk(*argument.callEca);
        }
        for (const std::uint32_t child : eca.children) walk(child);
    };
    for (const std::uint32_t root : trigger.ecas) walk(root);
    return locals;
}

ArgumentTypeAudit AuditArgumentTypes(const TriggerDocument& document, const UiCatalog& catalog) {
    ArgumentTypeAudit audit;
    const TriggerTypeSystem types(catalog);
    const auto special = [](std::string_view type) {
        return type == "Null" || type == "AnyGlobal" || type == "AnyType" || type == "anytype" || type.empty();
    };
    for (const TriggerRecord& trigger : document.triggers) {
        if (trigger.type != 0 || trigger.customTextFlag != 0) continue;
        const auto locals = types.CollectLocals(document, trigger);
        std::vector<char> visited(document.ecaNodes.size(), 0);
        const auto report = [&](const EcaNode& eca, std::size_t position, std::string declared, std::string expected,
                                std::string actual, std::string problem) {
            ++audit.problemsByFunction[eca.name.text + " #" + std::to_string(position) + " " + problem];
            if (audit.samples.size() < 200) {
                audit.samples.push_back({trigger.name.text, eca.name.text, position, std::move(declared),
                    std::move(expected), std::move(actual), std::move(problem)});
            }
        };
        const std::function<void(std::uint32_t)> walk = [&](std::uint32_t index) {
            if (index >= document.ecaNodes.size() || visited[index]) return;
            visited[index] = 1;
            const EcaNode& eca = document.ecaNodes[index];
            const UiDefinition* definition = catalog.Find(eca.type, eca.name.text);
            if (definition == nullptr) ++audit.missingDefinitions;
            for (std::size_t position = 0; position < eca.arguments.size(); ++position) {
                const ArgumentNode* argument = ArgumentAt(document, eca, position);
                if (argument == nullptr) continue;
                if (argument->callEca.has_value()) walk(*argument->callEca);
                if (definition == nullptr) continue;
                ++audit.arguments;
                const std::string declared = TriggerTypeSystem::DeclaredType(*definition, position);
                const std::string expected = types.ExpectedType(document, eca, position, locals);
                const std::string actual = types.ValueType(document, *argument, locals);
                if (actual == "AnyReturnType") ++audit.anyReturnValues;
                if (declared == "Null") {
                    ++audit.nullArguments;
                    if (expected != "Null") ++audit.resolvedNull;
                }
                if (expected == "Null") {
                    ++audit.unresolved;
                    report(eca, position, declared, expected, actual, "unresolved");
                } else if (!special(expected) && types.Find(expected) == nullptr) {
                    ++audit.unknownTypes;
                    report(eca, position, declared, expected, actual, "unknown-type");
                } else if (!types.Accepts(expected, actual)) {
                    // 局部变量读取的类型只按本触发器推断；YDWE 允许跨触发器传递局部变量，单独计为提示。
                    const bool local = argument->callEca.has_value() && *argument->callEca < document.ecaNodes.size() &&
                        IsLocalGetter(document.ecaNodes[*argument->callEca].name.text);
                    ++(local ? audit.localTypeHints : audit.mismatched);
                    report(eca, position, declared, expected, actual, local ? "local-type-hint" : "mismatch");
                }
            }
            for (const std::uint32_t child : eca.children) {
                if (child < document.ecaNodes.size()) {
                    const EcaNode& node = document.ecaNodes[child];
                    ++audit.blockBranches[eca.name.text + " #" + std::to_string(node.childId.value_or(-1)) + " " +
                        std::to_string(node.type)];
                }
                walk(child);
            }
        };
        for (const std::uint32_t root : trigger.ecas) walk(root);
    }
    return audit;
}

} // namespace War3Map::Trigger
