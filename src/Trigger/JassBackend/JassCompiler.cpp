#include "War3MapCore/Trigger/JassCompiler.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Trigger {
namespace {

bool Identifier(std::string_view value) {
    const auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    if (value.empty() || !alpha(value.front())) return false;
    if (!std::all_of(value.begin(), value.end(), [&](char c) {
        return alpha(c) || (c >= '0' && c <= '9');
    })) return false;
    static const std::unordered_set<std::string_view> keywords = {
        "globals", "endglobals", "function", "endfunction", "takes", "returns", "nothing",
        "native", "type", "extends", "constant", "local", "set", "call", "return", "if",
        "then", "else", "elseif", "endif", "loop", "endloop", "exitwhen", "debug", "array",
        "and", "or", "not", "true", "false", "null"};
    return !keywords.contains(value);
}

std::optional<std::string> Field(const UiDefinitionRevision& definition, std::string_view name) {
    for (const auto& [key, value] : definition.fields) if (key == name) return value;
    return std::nullopt;
}

// 保留第三列的 JASS 引号；逗号只在引号外分隔。
std::vector<std::string> Csv(std::string_view value) {
    std::vector<std::string> parts(1);
    char quote = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (quote != 0) {
            parts.back() += c;
            if (c == '\\' && i + 1 < value.size()) parts.back() += value[++i];
            else if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
            parts.back() += c;
        } else if (c == ',') parts.emplace_back();
        else parts.back() += c;
    }
    if (quote != 0) return {};
    for (auto& part : parts) {
        const auto first = part.find_first_not_of(" \t\r\n");
        part = first == std::string::npos ? "" : part.substr(first, part.find_last_not_of(" \t\r\n") - first + 1);
    }
    return parts;
}

std::string Quoted(std::string_view value) {
    std::string output = "\"";
    for (const unsigned char c : value) {
        switch (c) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        default: output += static_cast<char>(c); break;
        }
    }
    return output + '"';
}

bool BooleanFlag(std::int32_t flag) { return flag == 0 || flag == 1; }

// 这些目录项描述运算表达式，并不是同名 JASS 函数。
// 在独立表达式 lowering 接入前必须拒绝普通调用回退。
bool RequiresOperatorLowering(std::string_view name) {
    static const std::unordered_set<std::string_view> operators = {
        "OperatorString", "OperatorInt", "OperatorReal", "YDWEOperatorString3", "YDWEOperatorInt3", "YDWEOperatorReal3",
        "OperatorIntegerAdd", "OperatorIntegerSubtract", "OperatorIntegerMultiply", "OperatorIntegerDivide",
        "OperatorRealAdd", "OperatorRealSubtract", "OperatorRealMultiply", "OperatorRealDivide",
        "OperatorDegreeAdd", "OperatorDegreeSubtract", "OperatorDegreeMultiply", "OperatorDegreeDivide",
        "OperatorRadianAdd", "OperatorRadianSubtract", "OperatorRadianMultiply", "OperatorRadianDivide"};
    return operators.contains(name);
}

class Compiler {
public:
    Compiler(const TriggerDocument& document, const UiCatalog& catalog)
        : doc_(document), catalog_(catalog), types_(catalog) {}

    JassCompileResult Compile() {
        std::string globals = "globals\n";
        for (const auto& variable : doc_.variables) {
            location_ = "变量 " + variable.name.text;
            if (!Identifier(variable.name.text) || !variables_.emplace(variable.name.text, &variable).second) {
                Error("jass.variable-name-invalid", "变量名称无效或重复。", variable.origin);
                continue;
            }
            const std::string type = Type(variable.typeName.text);
            if (type.empty() || type == "nothing" || type == "code") {
                Error("jass.variable-type-invalid", "缺少可确认的变量类型。", variable.origin);
                continue;
            }
            if (!BooleanFlag(variable.isArray) || !BooleanFlag(variable.hasDefault) || variable.size < 0 || variable.size > 8192) {
                Error("jass.variable-shape-invalid", "变量数组或默认值标记无效。", variable.origin);
                continue;
            }
            globals += "    " + type + (variable.isArray ? " array udg_" : " udg_") + variable.name.text;
            if (variable.hasDefault) {
                if (variable.isArray) Error("jass.array-initialization-unsupported", "数组初始值循环尚未实现。", variable.origin);
                else globals += " = " + DefaultValue(variable.value.text, variable.typeName.text, variable.origin);
            }
            globals += "\n";
        }
        std::unordered_set<std::string> symbols;
        for (std::size_t i = 0; i < doc_.triggers.size(); ++i) {
            const auto& trigger = doc_.triggers[i];
            location_ = "触发器 " + trigger.name.text;
            if (trigger.type != 0 && trigger.type != 1) Error("jass.trigger-type-invalid", "触发器类型无法识别。", trigger.origin);
            if (!BooleanFlag(trigger.enabled) || !BooleanFlag(trigger.customTextFlag) ||
                !BooleanFlag(trigger.initiallyClosed) || !BooleanFlag(trigger.runOnInit)) {
                Error("jass.trigger-flags-invalid", "触发器状态标记无效。", trigger.origin);
            }
            if (trigger.type != 0 || trigger.enabled == 0) continue;
            // 合法英文名保持可读，其余用位置身份避免中文名称净化后互相碰撞。
            std::string symbol = Identifier(trigger.name.text) ? trigger.name.text : "EditorTrigger_" + std::to_string(i);
            if (!symbols.insert(symbol).second) {
                Error("jass.trigger-symbol-collision", "触发器生成符号冲突。", trigger.origin);
                continue;
            }
            triggerSymbols_.emplace(i, symbol);
            globals += "    trigger gg_trg_" + symbol + " = null\n";
        }
        globals += "endglobals\n\n";
        if (!doc_.wct.customCode.text.empty()) Error("jass.custom-code-unsupported", "地图自定义代码需要脚本解析和外部编译验证。");
        std::string body;
        // 按原文档顺序生成，避免 unordered_map 遍历改变初始化顺序。
        for (std::size_t i = 0; i < doc_.triggers.size(); ++i) {
            const auto it = triggerSymbols_.find(i);
            if (it != triggerSymbols_.end()) body += Trigger(i, it->second);
        }
        body += "function InitCustomTriggers takes nothing returns nothing\n";
        for (std::size_t i = 0; i < doc_.triggers.size(); ++i) {
            const auto it = triggerSymbols_.find(i);
            if (it != triggerSymbols_.end()) body += "    call InitTrig_" + it->second + "()\n";
        }
        body += "endfunction\n\nfunction RunInitializationTriggers takes nothing returns nothing\n";
        for (std::size_t i = 0; i < doc_.triggers.size(); ++i) {
            const auto it = triggerSymbols_.find(i);
            if (it != triggerSymbols_.end() && doc_.triggers[i].runOnInit)
                body += "    call ConditionalTriggerExecute(gg_trg_" + it->second + ")\n";
        }
        body += "endfunction\n";
        if (result_.Succeeded()) result_.jassCode = "// War3MapEditor64: trigger fragment, not a complete map script.\n" + globals + body;
        return std::move(result_);
    }

private:
    void Error(std::string code, std::string text, std::optional<OriginSpan> origin = std::nullopt) {
        if (result_.diagnostics.size() >= 256) return;
        result_.diagnostics.push_back({DiagnosticSeverity::Error, "Jass", std::move(code),
            location_ + (nodeLocations_.empty() ? "" : " / " + nodeLocations_.back()) +
                ": " + std::move(text), std::move(origin)});
    }

    std::string Type(std::string type, std::unordered_set<std::string> seen = {}) const {
        static const std::unordered_set<std::string> builtin = {
            "nothing", "integer", "real", "boolean", "string", "code", "handle", "agent", "event", "player",
            "widget", "unit", "destructable", "item", "ability", "buff", "force", "group", "trigger", "triggercondition",
            "triggeraction", "timer", "location", "region", "rect", "boolexpr", "sound", "conditionfunc", "filterfunc",
            "unitpool", "itempool", "race", "alliancetype", "racepreference", "gamestate", "igamestate", "fgamestate",
            "playerstate", "playerscore", "playergameresult", "unitstate", "aidifficulty", "eventid", "gameevent",
            "playerevent", "playerunitevent", "unitevent", "limitop", "widgetevent", "dialogevent", "unittype",
            "gamespeed", "gamedifficulty", "gametype", "mapflag", "mapvisibility", "mapsetting", "mapdensity",
            "mapcontrol", "playerslotstate", "volumegroup", "camerafield", "camerasetup", "playercolor", "placement",
            "startlocprio", "raritycontrol", "blendmode", "texmapflags", "effect", "effecttype", "weathereffect",
            "terraindeformation", "fogstate", "fogmodifier", "dialog", "button", "quest", "questitem", "defeatcondition",
            "timerdialog", "leaderboard", "multiboard", "multiboarditem", "trackable", "gamecache", "version",
            "itemtype", "texttag", "attacktype", "damagetype", "weapontype", "soundtype", "lightning",
            "pathingtype", "image", "ubersplat", "hashtable"};
        if (builtin.contains(type)) return type;
        if (!seen.insert(type).second || seen.size() > 32) return {};
        const auto* definition = catalog_.Find(UiDefinitionKind::Define, type, "TriggerTypes");
        if (definition == nullptr) return {};
        const auto raw = Field(definition->Active(), "value");
        if (!raw) return {};
        const auto columns = Csv(*raw);
        return columns.size() > 4 && !columns[4].empty() ? Type(columns[4], std::move(seen)) : std::string{};
    }

    bool Compatible(std::string_view actual, std::string_view expected) const {
        const std::string a = Type(std::string(actual)), e = Type(std::string(expected));
        return !a.empty() && !e.empty() && (a == e || (a == "integer" && e == "real"));
    }

    std::string Literal(const std::string& value, const std::string& expected, const OriginSpan& origin) {
        const std::string type = Type(expected);
        if (type.empty() || type == "nothing" || type == "code") {
            Error("jass.literal-type-unsupported", "无法按类型生成固定值: " + expected, origin);
            return {};
        }
        const auto role = types_.LiteralKind(expected);
        if (role == TriggerTypeSystem::Literal::ObjectCode) {
            if (TriggerTypeSystem::IsObjectCodeLiteral(value)) return "'" + value + "'";
            Error("jass.rawcode-invalid", "对象编号固定值必须是四个安全字节，不加引号或转义。", origin);
            return {};
        }
        if (role == TriggerTypeSystem::Literal::Other && !types_.ValidateLiteral(expected, value).Succeeded()) {
            Error("jass.literal-type-unsupported", "对象编号类型的整数父链无效：" + expected, origin);
            return {};
        }
        if (type == "string") {
            if (value.find('\0') != std::string::npos || value.find('\t') != std::string::npos) {
                Error("jass.string-control-character", "字符串含尚未支持的控制字符。", origin);
                return {};
            }
            if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 && c != '\n' && c != '\r'; })) {
                Error("jass.string-control-character", "字符串含尚未支持的控制字符。", origin);
                return {};
            }
            return Quoted(value);
        }
        if (type == "integer" && TriggerTypeSystem::IsIntegerLiteral(value)) {
            std::int32_t number = 0;
            (void)std::from_chars(value.data(), value.data() + value.size(), number);
            return std::to_string(number);
        }
        if (type == "real" && TriggerTypeSystem::IsRealLiteral(value)) return value.find('.') == std::string::npos ? value + ".0" : value;
        if (type == "boolean" && (value == "true" || value == "false")) return value;
        if (type != "integer" && type != "real" && type != "boolean" && value == "null") return value;
        Error("jass.literal-invalid", "固定值不符合参数类型 " + expected + "。", origin);
        return {};
    }

    std::string Preset(const UiDefinition& definition, const std::string& expected, const OriginSpan& origin) {
        const auto value = Field(definition.Active(), "value");
        const auto columns = value ? Csv(*value) : std::vector<std::string>{};
        if (columns.size() < 3 || !Compatible(columns[1], expected)) {
            Error("jass.preset-type-invalid", "预设类型或定义不完整: " + definition.name, origin);
            return {};
        }
        // 目录定义的常量符号可按声明类型使用；任意函数/运算表达式需要专门解析。
        const auto& code = columns[2];
        if (Identifier(code)) return code;
        if (Type(columns[1]) == "string") {
            if (code.size() < 2 || code.front() != '"' || code.back() != '"') {
                Error("jass.preset-expression-unsupported", "字符串预设不是完整字符串字面量。", origin);
                return {};
            }
            std::string decoded;
            for (std::size_t i = 1; i + 1 < code.size(); ++i) {
                char c = code[i];
                if (c == '"' || c == '\n' || c == '\r') {
                    Error("jass.preset-expression-unsupported", "字符串预设包含无效引号或换行。", origin); return {};
                }
                if (c == '\\') {
                    if (++i + 1 >= code.size()) { Error("jass.preset-expression-unsupported", "预设转义不完整。", origin); return {}; }
                    c = code[i];
                    if (c == 'n') c = '\n';
                    else if (c == 'r') c = '\r';
                    else if (c != '\\' && c != '"') { Error("jass.preset-expression-unsupported", "预设转义尚未支持。", origin); return {}; }
                }
                decoded += c;
            }
            return Literal(decoded, columns[1], origin);
        }
        if (types_.LiteralKind(columns[1]) == TriggerTypeSystem::Literal::ObjectCode) {
            // 预设保存的是目录中的 JASS 表达式；它与作者输入的裸四字节固定值不是同一来源。
            if (code.size() == 6 && code.front() == '\'' && code.back() == '\'')
                return Literal(code.substr(1, 4), columns[1], origin);
            if (TriggerTypeSystem::IsIntegerLiteral(code)) return Literal(code, "integer", origin);
        }
        return Literal(code, columns[1], origin);
    }

    std::string DefaultValue(const std::string& value, const std::string& type, const OriginSpan& origin) {
        if (const auto* preset = catalog_.Find(UiDefinitionKind::Define, value, "TriggerParams")) return Preset(*preset, type, origin);
        return Literal(value, type, origin);
    }

    const UiDefinition* Definition(const EcaNode& node) {
        const auto* definition = catalog_.Find(node.type, node.name.text);
        if (definition == nullptr) Error("jass.unknown-function", "函数未注册: " + node.name.text, node.origin);
        else if (definition->revisions.empty()) { Error("jass.definition-invalid", "函数定义为空。", node.origin); return nullptr; }
        return definition;
    }

    std::string ScriptName(const UiDefinition& definition, const EcaNode& node) {
        const auto script = Field(definition.Active(), "script_name").value_or(node.name.text);
        if (!Identifier(script)) { Error("jass.script-name-invalid", "脚本函数名不是合法标识符。", node.origin); return {}; }
        return script;
    }

    bool EnterEca(std::uint32_t index) {
        if (index >= doc_.ecaNodes.size()) { Error("jass.eca-index-invalid", "ECA 索引越界: " + std::to_string(index)); return false; }
        if (++visitedNodes_ > 100000) { Error("jass.graph-budget", "触发器图展开超过 100000 次访问。", doc_.ecaNodes[index].origin); return false; }
        if (ecaStack_.size() + argStack_.size() >= 256 || !ecaStack_.insert(index).second) {
            Error("jass.graph-cycle", "ECA/参数循环引用或嵌套超过 256 层。", doc_.ecaNodes[index].origin); return false;
        }
        const auto& node = doc_.ecaNodes[index];
        nodeLocations_.push_back("ECA " + std::to_string(node.id) + " (" + node.name.text + ")");
        if (!BooleanFlag(node.enabled)) Error("jass.eca-flags-invalid", "ECA 启用标记无效。", node.origin);
        return true;
    }

    void LeaveEca(std::uint32_t index) {
        ecaStack_.erase(index);
        nodeLocations_.pop_back();
    }

    std::string Arguments(const EcaNode& node, const UiDefinition& definition) {
        std::vector<std::string> types;
        for (const auto& arg : definition.Active().arguments) if (arg.type != "nothing") types.push_back(arg.type);
        if (types.size() != node.arguments.size()) {
            Error("jass.argument-count-invalid", "参数数量与目录定义不一致: " + node.name.text, node.origin);
            return {};
        }
        std::string output;
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (i) output += ", ";
            output += Argument(node.arguments[i], types[i]);
        }
        return output;
    }

    std::string Argument(std::uint32_t index, const std::string& expected) {
        if (index >= doc_.argumentNodes.size()) { Error("jass.arg-index-invalid", "参数索引越界: " + std::to_string(index)); return {}; }
        if (++visitedNodes_ > 100000) { Error("jass.graph-budget", "触发器图展开超过 100000 次访问。", doc_.argumentNodes[index].origin); return {}; }
        if (ecaStack_.size() + argStack_.size() >= 256 || !argStack_.insert(index).second) {
            Error("jass.graph-cycle", "ECA/参数循环引用或嵌套超过 256 层。", doc_.argumentNodes[index].origin); return {};
        }
        const auto& arg = doc_.argumentNodes[index];
        nodeLocations_.push_back("参数 " + std::to_string(arg.id) + " [" + std::to_string(index) + "]");
        const auto finish = [&](std::string result) { argStack_.erase(index); nodeLocations_.pop_back(); return result; };
        if (!BooleanFlag(arg.insertCall) || !BooleanFlag(arg.insertIndex) ||
            (arg.insertCall != 0) != arg.callEca.has_value() || (arg.insertIndex != 0) != arg.indexArgument.has_value()) {
            Error("jass.argument-shape-invalid", "参数调用/下标标記与结构不一致。", arg.origin); return finish({});
        }
        if (Type(expected).empty()) { Error("jass.argument-type-unknown", "无法确认参数类型: " + expected, arg.origin); return finish({}); }
        if (arg.type == 2 && arg.callEca && !arg.indexArgument) {
            return finish(Call(*arg.callEca, expected));
        }
        if (arg.callEca || (arg.type != 1 && arg.indexArgument)) {
            Error("jass.argument-shape-invalid", "参数类型与调用/数组结构冲突。", arg.origin); return finish({});
        }
        if (arg.type == 1) {
            std::string name = arg.value.text;
            auto variable = variables_.find(name);
            if (name.starts_with("udg_")) {
                const auto prefixed = variables_.find(name.substr(4));
                if (variable != variables_.end() && prefixed != variables_.end()) {
                    Error("jass.variable-ambiguous", "变量原名与 udg_ 脚本符号存在歧义。", arg.origin); return finish({});
                }
                if (variable == variables_.end()) variable = prefixed;
            }
            if (variable == variables_.end()) {
                Error("jass.variable-unresolved", "未解析的变量/预放置符号: " + name, arg.origin); return finish({});
            }
            if (!Compatible(variable->second->typeName.text, expected) ||
                (variable->second->isArray != 0) != arg.indexArgument.has_value()) {
                Error("jass.variable-type-invalid", "变量类型或数组下标与参数不匹配。", arg.origin); return finish({});
            }
            std::string expression = "udg_" + variable->second->name.text;
            if (arg.indexArgument) expression += "[" + Argument(*arg.indexArgument, "integer") + "]";
            return finish(std::move(expression));
        }
        if (arg.type == 0) {
            const auto* preset = catalog_.Find(UiDefinitionKind::Define, arg.value.text, "TriggerParams");
            if (!preset) { Error("jass.preset-unresolved", "预设未注册: " + arg.value.text, arg.origin); return finish({}); }
            return finish(Preset(*preset, expected, arg.origin));
        }
        if (arg.type == 3) return finish(Literal(arg.value.text, expected, arg.origin));
        Error("jass.argument-kind-unsupported", "尚未支持该参数形式。", arg.origin);
        return finish({});
    }

    std::string Call(std::uint32_t index, const std::string& expected) {
        if (!EnterEca(index)) return {};
        const auto& node = doc_.ecaNodes[index];
        std::string output;
        const auto* definition = Definition(node);
        if (node.type != 3 || node.enabled != 1 || !node.children.empty())
            Error("jass.call-shape-invalid", "表达式必须是启用的无函数体返回值调用。", node.origin);
        else if (RequiresOperatorLowering(node.name.text))
            Error("jass.operator-unsupported", "目录运算表达式需要运算符语义，不能作为普通函数调用生成。", node.origin);
        else if (definition) {
            const auto returns = Field(definition->Active(), "returns");
            if (!returns || !Compatible(*returns, expected)) Error("jass.return-type-invalid", "函数返回类型不匹配或未声明。", node.origin);
            else output = ScriptName(*definition, node) + "(" + Arguments(node, *definition) + ")";
        }
        LeaveEca(index);
        return output;
    }

    std::string Conditions(const std::vector<std::uint32_t>& children, bool useOr = false) {
        std::string output;
        for (const auto child : children) {
            if (child < doc_.ecaNodes.size() && doc_.ecaNodes[child].enabled == 0) continue;
            const auto expression = Condition(child);
            if (expression.empty()) continue;
            if (!output.empty()) output += useOr ? " or " : " and ";
            output += "(" + expression + ")";
        }
        // 空集合的布尔单位元是明确语义；无效子节点始终已有 Error，绝不据此成功。
        return output.empty() ? (useOr ? "false" : "true") : output;
    }

    std::string Condition(std::uint32_t index) {
        if (!EnterEca(index)) return {};
        const auto& node = doc_.ecaNodes[index];
        const auto* definition = Definition(node);
        std::string output;
        if (node.type != 1 || node.enabled != 1) Error("jass.condition-shape-invalid", "条件分支包含非条件节点。", node.origin);
        else if (definition) {
            if (node.name.text == "AndMultiple" || node.name.text == "OrMultiple") {
                if (!node.arguments.empty()) Error("jass.structure-unsupported", "多条件组合器参数结构尚未支持。", node.origin);
                for (auto child : node.children) {
                    if (child >= doc_.ecaNodes.size() || doc_.ecaNodes[child].childId != 0)
                        Error("jass.branch-invalid", "组合条件缺少明确的条件分支标记。", node.origin);
                }
                output = Conditions(node.children, node.name.text == "OrMultiple");
            } else if (!node.children.empty()) Error("jass.structure-unsupported", "该条件函数体尚未实现: " + node.name.text, node.origin);
            else {
                const auto returns = Field(definition->Active(), "returns");
                if (returns && *returns != "boolean") Error("jass.return-type-invalid", "条件声明必须返回 boolean。", node.origin);
                output = ScriptName(*definition, node) + "(" + Arguments(node, *definition) + ")";
            }
        }
        LeaveEca(index);
        return output;
    }

    std::string Action(std::uint32_t index, const std::string& indent) {
        if (!EnterEca(index)) return {};
        const auto& node = doc_.ecaNodes[index];
        std::string output;
        if (node.enabled == 0) { LeaveEca(index); return {}; }
        const auto* definition = Definition(node);
        if (node.type != 2) Error("jass.action-shape-invalid", "动作分支包含非动作节点。", node.origin);
        else if (definition) {
            if (node.name.text == "IfThenElseMultiple") {
                if (!node.arguments.empty()) Error("jass.structure-unsupported", "IfThenElseMultiple 参数结构尚未支持。", node.origin);
                std::vector<std::uint32_t> condition, yes, no;
                for (auto child : node.children) {
                    if (child >= doc_.ecaNodes.size()) { Error("jass.eca-index-invalid", "分支子节点索引越界。", node.origin); continue; }
                    const auto branch = doc_.ecaNodes[child].childId;
                    if (branch == 0) condition.push_back(child);
                    else if (branch == 1) yes.push_back(child);
                    else if (branch == 2) no.push_back(child);
                    else Error("jass.branch-invalid", "If 分支必须明确标记为条件、Then 或 Else。", node.origin);
                }
                output += indent + "if " + Conditions(condition) + " then\n";
                for (auto child : yes) output += Action(child, indent + "    ");
                if (!no.empty()) {
                    output += indent + "else\n";
                    for (auto child : no) output += Action(child, indent + "    ");
                }
                output += indent + "endif\n";
            } else if (!node.children.empty() || node.name.text.starts_with("ForLoop") ||
                node.name.text.starts_with("ForGroup") || node.name.text.starts_with("ForForce") ||
                node.name.text.starts_with("IfThenElse")) {
                Error("jass.structure-unsupported", "尚未实现该控制流/回调，不能按顺序展开: " + node.name.text, node.origin);
            } else if (node.name.text == "SetVariable") {
                Error("jass.assignment-unsupported", "变量赋值需要可写左值语义，不能作为普通函数调用生成。", node.origin);
            } else if (node.name.text == "ReturnAction") {
                Error("jass.return-unsupported", "提前返回需要控制流语义，不能作为普通函数调用生成。", node.origin);
            } else if (node.name.text == "WaitForCondition") {
                Error("jass.structure-unsupported", "等待条件需要循环与重复求值语义，不能作为普通函数调用生成。", node.origin);
            } else if (node.name.text == "CustomScriptCode") {
                Error("jass.custom-code-unsupported", "自定义语句需要脚本解析和外部编译验证。", node.origin);
            } else if (node.name.text == "CommentString") {
                if (node.arguments.size() != 1 || node.arguments[0] >= doc_.argumentNodes.size()) {
                    Error("jass.argument-count-invalid", "注释参数无效。", node.origin);
                } else {
                    const auto& arg = doc_.argumentNodes[node.arguments[0]];
                    if (arg.type != 3 || arg.callEca || arg.indexArgument || arg.insertCall || arg.insertIndex)
                        Error("jass.argument-shape-invalid", "注释必须是固定文本。", arg.origin);
                    else {
                        output = indent + "// ";
                        for (char c : arg.value.text) output += c == '\n' || c == '\r' ? "\n" + indent + "// " : std::string(1, c);
                        output += "\n";
                    }
                }
            } else {
                const auto arguments = Arguments(node, *definition);
                output = indent + "call " + ScriptName(*definition, node) + "(" + arguments + ")\n";
            }
        }
        LeaveEca(index);
        return output;
    }

    std::string Event(std::uint32_t index, const std::string& symbol) {
        if (!EnterEca(index)) return {};
        const auto& node = doc_.ecaNodes[index];
        std::string output;
        if (node.enabled != 0) {
            const auto* definition = Definition(node);
            if (!node.children.empty()) Error("jass.structure-unsupported", "事件函数体尚未实现。", node.origin);
            else if (node.name.text == "MapInitializationEvent") {
                Error("jass.map-init-event-unsupported", "地图初始化事件需要完整地图初始化调度。", node.origin);
            } else if (definition) {
                const auto args = Arguments(node, *definition);
                output = "    call " + ScriptName(*definition, node) + "(gg_trg_" + symbol + (args.empty() ? "" : ", " + args) + ")\n";
            }
        }
        LeaveEca(index);
        return output;
    }

    std::string Trigger(std::size_t index, const std::string& symbol) {
        const auto& trigger = doc_.triggers[index];
        location_ = "触发器 " + trigger.name.text + " (ID " + std::to_string(trigger.id) + ")";
        if (trigger.customTextFlag || !trigger.customText.text.empty()) {
            Error("jass.custom-code-unsupported", "自定义触发器文本需要脚本解析和外部编译验证。", trigger.origin);
            return {};
        }
        std::vector<std::uint32_t> conditions, actions, events;
        for (auto nodeIndex : trigger.ecas) {
            if (nodeIndex >= doc_.ecaNodes.size()) { Error("jass.eca-index-invalid", "顶层 ECA 索引越界。", trigger.origin); continue; }
            const auto& node = doc_.ecaNodes[nodeIndex];
            if (node.childId) Error("jass.branch-invalid", "顶层节点不能携带子分支标记。", node.origin);
            if (!BooleanFlag(node.enabled)) Error("jass.eca-flags-invalid", "节点启用标记无效。", node.origin);
            if (node.enabled == 0) continue;
            if (node.type == 0) events.push_back(nodeIndex);
            else if (node.type == 1) conditions.push_back(nodeIndex);
            else if (node.type == 2) actions.push_back(nodeIndex);
            else Error("jass.root-kind-invalid", "顶层只允许事件、条件和动作。", node.origin);
        }
        std::string output;
        if (!conditions.empty()) output = "function Trig_" + symbol + "_Conditions takes nothing returns boolean\n    return " + Conditions(conditions) + "\nendfunction\n\n";
        output += "function Trig_" + symbol + "_Actions takes nothing returns nothing\n";
        for (auto action : actions) output += Action(action, "    ");
        output += "endfunction\n\nfunction InitTrig_" + symbol + " takes nothing returns nothing\n";
        output += "    set gg_trg_" + symbol + " = CreateTrigger()\n";
        if (trigger.initiallyClosed) output += "    call DisableTrigger(gg_trg_" + symbol + ")\n";
        for (auto event : events) output += Event(event, symbol);
        if (!conditions.empty()) output += "    call TriggerAddCondition(gg_trg_" + symbol + ", Condition(function Trig_" + symbol + "_Conditions))\n";
        output += "    call TriggerAddAction(gg_trg_" + symbol + ", function Trig_" + symbol + "_Actions)\nendfunction\n\n";
        return output;
    }

    const TriggerDocument& doc_;
    const UiCatalog& catalog_;
    const TriggerTypeSystem types_;
    JassCompileResult result_;
    std::string location_ = "地图触发器";
    std::unordered_map<std::string, const VariableRecord*> variables_;
    std::unordered_map<std::size_t, std::string> triggerSymbols_;
    std::unordered_set<std::uint32_t> ecaStack_, argStack_;
    std::vector<std::string> nodeLocations_;
    std::size_t visitedNodes_ = 0;
};

} // namespace

JassCompileResult CompileTriggersToJass(const TriggerDocument& document, const UiCatalog& catalog) {
    return Compiler(document, catalog).Compile();
}

} // namespace War3Map::Trigger
