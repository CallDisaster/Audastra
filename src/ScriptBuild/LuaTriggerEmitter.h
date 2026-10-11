#pragma once

#include "War3MapCore/Script/LuaTriggerUnit.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "LuaMappedText.h"

#include <map>
#include <set>
#include <unordered_set>

namespace War3Map::Script::lua {

struct Expression {
    std::string type;
    std::string code;
    std::optional<std::string> stringLiteral;
};

[[nodiscard]] std::string Quote(std::string_view text);
[[nodiscard]] bool Identifier(std::string_view text);

class Emitter {
public:
    Emitter(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
        const LuaTargetProfile& target, std::span<const LuaCallbackBinding> callbacks,
        std::span<const LuaGlobalBinding> globals = {}, LuaTriggerBuildOptions options = {});
    LuaTriggerUnit Run();

private:
    // 每次展开都计费（不按去重节点计费）；共享子树不能绕过代码大小与递归预算。
    class Visit {
    public:
        Visit(Emitter& emitter, bool argument, std::uint32_t index);
        ~Visit();
    private:
        Emitter& emitter_;
        std::uint64_t key_;
        StableId previousId_;
        OriginSpan previousOrigin_;
    };
    [[noreturn]] void Fail(std::string code, std::string message) const;
    void Prepare();
    void Globals();
    void AuthorHeader();
    void AuthorTrigger(const Trigger::TriggerRecord& trigger, const LuaTriggerSymbol& symbol);
    void TriggerBody(const Trigger::TriggerRecord& trigger, const LuaTriggerSymbol& symbol);
    void Statement(std::uint32_t index, MappedText& output, bool gateEnabled = true);
    void StatementBody(const Trigger::EcaNode& node, MappedText& output);
    void Children(const Trigger::EcaNode& node, std::int32_t block, MappedText& output);
    Expression Argument(std::uint32_t index, const std::string& expected);
    Expression Node(std::uint32_t index);
    Expression Literal(std::string value, const std::string& expected, bool preset);
    Expression Variable(const Trigger::ArgumentNode& argument, bool allowArray = false);
    std::optional<Expression> BoundGlobal(std::string_view name);
    std::optional<Expression> BoundConstant(std::string_view name);
    Expression Native(std::string_view name, std::vector<Expression> arguments, std::string_view expected = {});
    Expression Checked(Expression expression, const std::string& expected) const;
    Expression Binary(std::string op, Expression left, Expression right, bool arithmetic);
    const Trigger::UiDefinitionRevision& Definition(const Trigger::EcaNode& node) const;
    std::string Field(const Trigger::UiDefinitionRevision& definition, std::string_view field) const;
    std::string Define(std::string_view group, std::string_view name, std::size_t column) const;
    std::string Base(std::string type) const;
    bool Assignable(std::string actual, const std::string& expected) const;
    bool KnownType(const std::string& type) const;
    std::string Default(const std::string& type) const;
    std::string Operator(std::uint32_t index) const;
    std::string ScriptName(const Trigger::EcaNode& node, const Trigger::UiDefinitionRevision& definition) const;
    std::vector<Expression> Arguments(const Trigger::EcaNode& node,
        const Trigger::UiDefinitionRevision& definition);
    std::string CheckExpressionSize(std::string text) const;

    const Trigger::TriggerDocument& document_;
    const Trigger::UiCatalog& catalog_;
    Trigger::TriggerTypeSystem types_;
    const LuaTargetProfile& target_;
    std::span<const LuaCallbackBinding> callbacks_;
    std::span<const LuaGlobalBinding> globals_;
    LuaTriggerBuildOptions options_;
    LuaTriggerUnit result_;
    MappedText output_;
    std::map<std::string, const Trigger::VariableRecord*> variables_;
    std::map<std::string, const LuaApiSignature*> apis_;
    std::map<std::string, std::string> boundGlobals_, constants_;
    std::map<std::string, std::string> parents_;
    std::map<StableId, LuaTriggerSymbol> symbols_;
    std::set<std::string> callableSymbols_;
    std::set<std::string> requiredApis_;
    std::set<std::string> requiredGlobals_, requiredConstants_;
    std::vector<std::pair<StableId, std::string>> initializers_;
    std::unordered_set<std::uint64_t> visiting_;
    std::size_t visited_ = 0;
    StableId currentTrigger_ = 0;
    StableId currentNode_ = 0;
    OriginSpan currentOrigin_;
};

} // namespace War3Map::Script::lua
