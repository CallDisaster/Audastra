#include "LuaTriggerEmitter.h"

#include <algorithm>

namespace War3Map::Script::lua {

const Trigger::UiDefinitionRevision& Emitter::Definition(const Trigger::EcaNode& node) const {
    if (node.name.text.size() > 128) Fail("LUA_UI_NAME_BUDGET", "UI 函数名称超过预算。");
    const auto* found = catalog_.Find(node.type, node.name.text);
    if (!found || found->revisions.empty()) Fail("LUA_UI_DEFINITION_MISSING", "GUI 节点没有生效的 UI 定义：" + node.name.text);
    if (found->ArgumentCount() != node.arguments.size()) Fail("LUA_ARGUMENT_COUNT", "GUI 参数数量与当前 UI 定义不符。");
    return found->Active();
}
std::string Emitter::ScriptName(const Trigger::EcaNode& node, const Trigger::UiDefinitionRevision& definition) const {
    auto name = Field(definition, "script_name");
    if (name.empty()) name = Field(definition, "script");
    return name.empty() ? node.name.text : name;
}
std::vector<Expression> Emitter::Arguments(const Trigger::EcaNode& node,
    const Trigger::UiDefinitionRevision& definition) {
    std::vector<Expression> arguments;
    for (std::size_t i = 0; i < node.arguments.size(); ++i)
        arguments.push_back(Argument(node.arguments[i], definition.arguments[i].type));
    return arguments;
}

Expression Emitter::Native(std::string_view name, std::vector<Expression> arguments, std::string_view expected) {
    const auto found = apis_.find(std::string(name));
    if (found == apis_.end()) Fail("LUA_NATIVE_UNAVAILABLE", "目标档案未声明 API：" + std::string(name));
    const auto& api = *found->second;
    if (api.parameters.size() != arguments.size()) Fail("LUA_NATIVE_SIGNATURE", "目标 API 参数数量不符：" + api.name);
    // 原生参数各求值一次且严格从左到右；不能依赖表达式内未指定的求值次序。
    std::string prefix = "(function() ";
    std::string call = api.name + "(";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        arguments[i] = Checked(std::move(arguments[i]), api.parameters[i]);
        const auto temporary = "v" + std::to_string(i);
        prefix += "local " + temporary + " = " + arguments[i].code + "; ";
        if (i) call += ", ";
        call += temporary;
    }
    if (api.name == "ExecuteFunc" && (arguments.size() != 1 || !arguments[0].stringLiteral
        || !callableSymbols_.contains(*arguments[0].stringLiteral)))
        Fail("LUA_DYNAMIC_ENTRY_UNRESOLVED", "ExecuteFunc 只接受绑定到已生成全局函数的常量名称。");
    requiredApis_.insert(api.name);
    Expression result{api.returns, CheckExpressionSize(prefix + "return " + call + ") end)()"), {}};
    return expected.empty() ? result : Checked(std::move(result), std::string(expected));
}

Expression Emitter::Variable(const Trigger::ArgumentNode& argument, bool allowArray) {
    if (argument.type != 1 || argument.callEca || argument.insertCall != 0)
        Fail("LUA_VARIABLE_EXPECTED", "此处需要明确的变量引用。");
    const auto found = variables_.find(argument.value.text);
    if (found == variables_.end()) {
        if (auto global = BoundGlobal(argument.value.text)) {
            if (argument.indexArgument || argument.insertIndex || allowArray)
                Fail("LUA_ARRAY_SHAPE", "已绑定的地图初始化符号不是数组：" + argument.value.text);
            return std::move(*global); // 完整声明身份；不按gg_前缀猜测，也不改成udg_名字。
        }
        Fail("LUA_VARIABLE_UNBOUND", "未绑定的变量或预放对象：" + argument.value.text);
    }
    const auto& variable = *found->second;
    if ((argument.indexArgument.has_value() != (argument.insertIndex != 0))
        || (!allowArray && (variable.isArray != 0) != argument.indexArgument.has_value()))
        Fail("LUA_ARRAY_SHAPE", "变量数组形状与索引标记不一致。");
    std::string text = "udg_" + argument.value.text;
    if (argument.indexArgument) text += "[" + Argument(*argument.indexArgument, "integer").code + "]";
    return {Base(variable.typeName.text), CheckExpressionSize(std::move(text)), {}};
}

Expression Emitter::Argument(std::uint32_t index, const std::string& expectedSlot) {
    Visit visit(*this, true, index);
    const auto& argument = document_.argumentNodes[index];
    const auto expected = Base(expectedSlot == "boolcall" ? "boolean" : expectedSlot);
    Expression result;
    if (argument.type == 2) {
        if (!argument.callEca || argument.insertCall != 1 || argument.indexArgument || argument.insertIndex != 0)
            Fail("LUA_CALL_SHAPE", "函数参数的调用/索引标记不一致。");
        if (expected == "code") {
            MappedText body;
            // 和共用源模型的 JASS 后端一样，调用参数节点的 enabled 位不是禁用门禁。
            Statement(*argument.callEca, body, false);
            // 此处变成表达式字符串；来源由外部调用语句覆盖，不冒称已映射匿名回调的内部动作。
            result = {"code", "function()\n" + body.Take() + "end", {}};
        } else {
            result = Node(*argument.callEca);
            if (expected == "boolexpr") {
                result = Checked(std::move(result), "boolean");
                return Native("Condition", {{"code", "function() return " + result.code + " end", {}}}, "boolexpr");
            }
        }
    } else if (argument.type == 1) {
        result = Variable(argument);
    } else if (argument.type == 0 || argument.type == 3) {
        if (argument.callEca || argument.insertCall || argument.indexArgument || argument.insertIndex)
            Fail("LUA_LITERAL_SHAPE", "字面量或预设不允许隐藏子调用。");
        result = Literal(argument.value.text, expectedSlot, argument.type == 0);
    } else Fail("LUA_ARGUMENT_KIND", "未支持的参数种类。");
    return Checked(std::move(result), expected);
}

std::string Emitter::Operator(std::uint32_t index) const {
    if (index >= document_.argumentNodes.size()) Fail("LUA_ARGUMENT_INDEX", "运算符索引越界。");
    const auto& argument = document_.argumentNodes[index];
    if (argument.callEca || argument.indexArgument || argument.insertCall || argument.insertIndex
        || (argument.type != 0 && argument.type != 3)) Fail("LUA_OPERATOR_INVALID", "运算符必须是常量预设。");
    return argument.type == 0 ? Define("TriggerParams", argument.value.text, 2) : argument.value.text;
}

Expression Emitter::Binary(std::string op, Expression left, Expression right, bool arithmetic) {
    std::string type;
    std::string body;
    if (arithmetic) {
        if (op == "..") {
            left = Checked(std::move(left), "string"); right = Checked(std::move(right), "string");
            type = "string"; body = "__forge_concat(a, b)";
        } else {
            if ((left.type != "integer" && left.type != "real") || (right.type != "integer" && right.type != "real")
                || (op != "+" && op != "-" && op != "*" && op != "/"))
                Fail("LUA_ARITHMETIC_TYPE", "运算符或数值类型不受支持。");
            type = left.type == "integer" && right.type == "integer" ? "integer" : "real";
            if (type == "integer" && op == "/") body = "__forge_idiv(a, b)";
            else body = (type == "integer" ? "__forge_i32(" : "__forge_real(") + std::string("a ") + op + " b)";
        }
    } else {
        if (op == "!=") op = "~=";
        if (op != "==" && op != "~=" && op != "<" && op != "<=" && op != ">" && op != ">=")
            Fail("LUA_OPERATOR_INVALID", "不支持的比较运算符。");
        if (left.type == "real" || right.type == "real")
            Fail("LUA_VM_SEMANTICS_UNVERIFIED", "实数比较尚未证明与经典 VM 一致。");
        if (!Assignable(left.type, right.type) && !Assignable(right.type, left.type))
            Fail("LUA_COMPARISON_TYPE", "比较两侧的类型不兼容。");
        if (op != "==" && op != "~=" && (left.type != "integer" || right.type != "integer"))
            Fail("LUA_COMPARISON_TYPE", "有序比较只支持经典整数。");
        type = "boolean"; body = "a " + op + " b";
    }
    return {type, CheckExpressionSize("(function() local a = " + left.code + "; local b = "
        + right.code + "; return " + body + " end)()"), {}};
}

Expression Emitter::Node(std::uint32_t index) {
    Visit visit(*this, false, index);
    const auto& node = document_.ecaNodes[index];
    const auto& definition = Definition(node);
    if (node.type != 1 && node.type != 3) Fail("LUA_EXPRESSION_KIND", "表达式位置不是条件或调用。");
    if ((node.name.text == "AndMultiple" || node.name.text == "OrMultiple") && node.arguments.empty()) {
        const bool conjunction = node.name.text == "AndMultiple";
        std::string text = conjunction ? "true" : "false";
        for (const auto childIndex : node.children) {
            if (childIndex >= document_.ecaNodes.size() || document_.ecaNodes[childIndex].childId != 0)
                Fail("LUA_BOOLEAN_BLOCK", "复合条件含未知子块。");
            if (document_.ecaNodes[childIndex].enabled)
                text = CheckExpressionSize(text + (conjunction ? " and (" : " or (") + Checked(Node(childIndex), "boolean").code + ")");
        }
        return {"boolean", CheckExpressionSize("(" + text + ")"), {}};
    }
    if (!node.children.empty()) Fail("LUA_CHILDREN_UNTRANSLATED", "此表达式尚未支持子块，不能顺序展开。");
    if (node.name.text == "Null" && node.arguments.empty()) return {"null", "nil", {}};
    if (node.name.text == "GetBooleanVariableValue" && node.arguments.size() == 1)
        return Argument(node.arguments[0], "boolean");
    if (node.arguments.size() == 3 && (definition.arguments[1].type == "ArithmeticOperator"
        || definition.arguments[1].type == "ComparisonOperator" || definition.arguments[1].type == "EqualNotEqualOperator")) {
        auto left = Argument(node.arguments[0], definition.arguments[0].type);
        auto right = Argument(node.arguments[2], definition.arguments[2].type);
        const bool arithmetic = definition.arguments[1].type == "ArithmeticOperator";
        auto result = Binary(Operator(node.arguments[1]), std::move(left), std::move(right), arithmetic);
        return Checked(std::move(result), arithmetic ? Base(Field(definition, "returns")) : "boolean");
    }
    if (node.name.text == "OperatorString" && node.arguments.size() == 2) {
        auto left = Argument(node.arguments[0], "string");
        auto right = Argument(node.arguments[1], "string");
        return Binary("..", std::move(left), std::move(right), true);
    }
    if ((node.name.text == "GetBooleanAnd" || node.name.text == "GetBooleanOr") && node.arguments.size() == 2) {
        auto left = Argument(node.arguments[0], "boolean");
        auto right = Argument(node.arguments[1], "boolean");
        return {"boolean", CheckExpressionSize("(" + left.code + (node.name.text == "GetBooleanAnd" ? " and " : " or ")
            + right.code + ")"), {}};
    }
    return Native(ScriptName(node, definition), Arguments(node, definition),
        node.type == 1 ? "boolean" : Base(Field(definition, "returns")));
}

} // namespace War3Map::Script::lua
