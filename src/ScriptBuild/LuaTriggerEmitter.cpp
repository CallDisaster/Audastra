#include "LuaTriggerEmitter.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/LuaAuthorCode.h"

#include <algorithm>
#include <new>

namespace War3Map::Script::lua {
extern const std::string_view kLuaTriggerRuntimeSource;
namespace {
bool RuntimeGlobal(std::string_view name) {
    return name == "math" || name == "type" || name == "assert" || name == "setmetatable";
}
bool MapFunction(std::string_view name) {
    constexpr std::string_view names[]{"config", "main", "InitGlobals", "InitCustomTriggers", "RunInitializationTriggers",
        "InitSounds", "CreateRegions", "CreateCameras", "InitUpgrades", "InitTechTree", "CreateAllDestructables",
        "CreateAllItems", "CreateAllUnits", "InitCustomPlayerSlots", "InitCustomTeams", "InitAllyPriorities"};
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
}

Emitter::Emitter(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    const LuaTargetProfile& target, std::span<const LuaCallbackBinding> callbacks, std::span<const LuaGlobalBinding> globals)
    : document_(document), catalog_(catalog), types_(catalog), target_(target), callbacks_(callbacks), globals_(globals) {}

void Emitter::Prepare() {
    if (target_.identity.empty() || target_.identity.size() > 256 || target_.apis.size() > 4096 || target_.handleTypes.size() > 512
        || target_.constants.size() > 4096
        || callbacks_.size() > 1024 || document_.variables.size() > 8192 || document_.triggers.size() > 8192
        || document_.categories.size() > 8192
        || document_.argumentNodes.size() > 200000 || document_.ecaNodes.size() > 200000)
        Fail("LUA_INPUT_BUDGET", "输入缺少目标身份或超过编译预算。");
    if (document_.HasTrustedSourceSeal() && document_.UiCatalogSha256() != catalog_.Sha256())
        Fail("LUA_CATALOG_IDENTITY", "触发器来源绑定的 UI Catalog 已变化。");
    if (target_.nativeAuthorCode) {
        const auto verified = target_.nativeAuthorCode->Verify(document_, catalog_);
        if (!verified.Succeeded()) {
            const auto& diagnostic = verified.diagnostics.front();
            Fail(diagnostic.code, diagnostic.message);
        }
    } else if (!document_.wct.customCode.text.empty())
        Fail("LUA_SOURCE_UNTRANSLATED", "地图自定义 JASS 尚未转换，原文仍保留。");
    for (const auto& [type, parent] : target_.handleTypes) {
        if (!Identifier(type) || !Identifier(parent) || KnownType(type) || !parents_.emplace(type, parent).second)
            Fail("LUA_TARGET_TYPE", "目标句柄类型无效或重复。");
    }
    for (const auto& [type, parent] : parents_) {
        (void)parent;
        if (!Assignable(type, "handle")) Fail("LUA_TARGET_TYPE", "句柄父链未终止于 handle：" + type);
    }
    for (const auto& api : target_.apis) {
        if (!Identifier(api.name) || RuntimeGlobal(api.name) || api.name.starts_with("__forge_") || api.name.starts_with("udg_")
            || api.name.starts_with("gg_trg_") || api.name.starts_with("Trig_F") || api.name.starts_with("InitTrig_F")
            || MapFunction(api.name)
            || api.parameters.size() > 32 || !KnownType(api.returns) || !apis_.emplace(api.name, &api).second)
            Fail("LUA_TARGET_API", "目标 API 身份、返回类型或数量无效。");
        for (const auto& type : api.parameters)
            if (!KnownType(type) || type == "nothing") Fail("LUA_TARGET_API", "目标 API 参数类型无效：" + api.name);
    }
    for (const auto& constant : target_.constants) {
        if (!Identifier(constant.name) || RuntimeGlobal(constant.name) || MapFunction(constant.name)
            || constant.name.starts_with("__forge_") || constant.name.starts_with("udg_") || constant.name.starts_with("gg_")
            || constant.name.starts_with("Trig_F") || constant.name.starts_with("InitTrig_F")
            || !KnownType(constant.type) || constant.type == "nothing" || constant.type == "code"
            || apis_.contains(constant.name) || !constants_.emplace(constant.name, constant.type).second)
            Fail("LUA_TARGET_CONSTANT", "目标常量名称、类型或绑定冲突无效：" + constant.name);
    }
    std::size_t globalBytes = 0;
    for (const auto& global : globals_) {
        const auto bytes = sizeof(LuaGlobalBinding) + global.name.size() + global.type.size();
        if (bytes > 16 * 1024 * 1024 - globalBytes) Fail("LUA_INPUT_BUDGET", "生成对象符号目录超过 16 MiB 预算。");
        globalBytes += bytes;
        if (!Identifier(global.name) || RuntimeGlobal(global.name) || MapFunction(global.name)
            || global.name.starts_with("__forge_") || global.name.starts_with("udg_") || global.name.starts_with("gg_trg_")
            || global.name.starts_with("Trig_F") || global.name.starts_with("InitTrig_F")
            || !KnownType(global.type) || global.type == "nothing" || global.type == "code"
            || apis_.contains(global.name) || constants_.contains(global.name)
            || !boundGlobals_.emplace(global.name, global.type).second)
            Fail("LUA_GLOBAL_BINDING", "生成对象符号名称、类型无效或重名：" + global.name);
    }
    std::string targetText;
    const auto append = [&](std::string_view value) { targetText += std::to_string(value.size()) + ":" + std::string(value); };
    append(target_.identity);
    append(std::to_string(parents_.size()));
    for (const auto& [type, parent] : parents_) { append(type); append(parent); }
    append(std::to_string(apis_.size()));
    for (const auto& [name, api] : apis_) {
        append(name); append(api->returns); append(std::to_string(api->parameters.size()));
        for (const auto& parameter : api->parameters) append(parameter);
    }
    append(std::to_string(constants_.size()));
    for (const auto& [name, type] : constants_) { append(name); append(type); }
    const auto fingerprint = Sha256Text(targetText);
    if (!fingerprint.Succeeded()) Fail("LUA_HASH_FAILED", "目标 API 档案指纹生成失败。");
    result_.targetSha256 = *fingerprint.value;
    for (const auto& variable : document_.variables) {
        currentNode_ = variable.id; currentOrigin_ = variable.origin;
        const auto type = Base(variable.typeName.text);
        if (!Identifier(variable.name.text) || !KnownType(type) || type == "nothing"
            || !variables_.emplace(variable.name.text, &variable).second)
            Fail("LUA_VARIABLE_INVALID", "变量名称/类型无效或重名。");
        if ((variable.isArray != 0 && variable.isArray != 1) || (variable.hasDefault != 0 && variable.hasDefault != 1)
            || (variable.isArray && (variable.size < 0 || variable.size > 8191)))
            Fail("LUA_VARIABLE_SHAPE", "变量标志或数组初始化范围无效。");
    }
    for (const auto& trigger : document_.triggers) {
        currentTrigger_ = trigger.id; currentNode_ = trigger.id; currentOrigin_ = trigger.origin;
        if ((trigger.customTextFlag || !trigger.customText.text.empty()) && !target_.nativeAuthorCode)
            Fail("LUA_SOURCE_UNTRANSLATED", "触发器自定义 JASS 尚未转换（包括禁用源），原文仍保留。");
        if (!trigger.customTextFlag && !trigger.customText.text.empty())
            Fail("LUA_AUTHOR_TRIGGER_SHAPE", "GUI 记录包含没有自定义代码标志的作者正文，不能略过。");
        if (trigger.type != 0 && trigger.type != 1) Fail("LUA_TRIGGER_KIND", "未知的触发器记录种类，不能略过。");
        if (trigger.type == 1 && !trigger.ecas.empty()) Fail("LUA_TRIGGER_KIND", "注释记录含有 ECA，不能丢弃。");
        if (trigger.type == 1) continue;
        if (!trigger.enabled) {
            result_.diagnostics.push_back({{DiagnosticSeverity::Info, "Script.Lua", "LUA_DISABLED_SOURCE_RETAINED",
                "禁用触发器源保留在文档中，不计入本次生成脚本。", trigger.origin}, trigger.id, trigger.id});
            continue;
        }
        LuaTriggerSymbol symbol;
        if (trigger.customTextFlag) {
            const auto blocks = target_.nativeAuthorCode->Triggers();
            const auto block = std::find_if(blocks.begin(), blocks.end(),
                [&](const auto& value) { return value.triggerId == trigger.id; });
            if (block == blocks.end()) Fail("LUA_AUTHOR_TRIGGER_MISSING", "活动 Lua 作者块不在冻结凭据中。");
            symbol = {trigger.id, block->variable, {}, {}, block->initializer};
        } else {
            const auto suffix = "F" + std::to_string(trigger.id);
            symbol = {trigger.id, "gg_trg_" + suffix, "Trig_" + suffix + "_Conditions",
                "Trig_" + suffix + "_Actions", "InitTrig_" + suffix};
        }
        if (!trigger.id || !symbols_.emplace(trigger.id, symbol).second)
            Fail("LUA_TRIGGER_ID", "活动触发器缺少唯一稳定身份。");
        if (!symbol.actions.empty()) callableSymbols_.insert(symbol.actions);
    }
    for (const auto& binding : callbacks_) {
        if (!Identifier(binding.symbol) || RuntimeGlobal(binding.symbol) || binding.symbol.starts_with("__forge_") || binding.symbol.starts_with("udg_")
            || binding.symbol.starts_with("gg_trg_") || binding.symbol.starts_with("Trig_F") || binding.symbol.starts_with("InitTrig_F")
            || MapFunction(binding.symbol) || !symbols_.contains(binding.triggerId)
            || symbols_.at(binding.triggerId).actions.empty() || apis_.contains(binding.symbol)
            || constants_.contains(binding.symbol) || boundGlobals_.contains(binding.symbol)
            || !callableSymbols_.insert(binding.symbol).second)
            Fail("LUA_CALLBACK_BINDING", "全局回调名称冲突或没有活动 GUI 动作来源。");
    }
    currentTrigger_ = 0; currentNode_ = 0; currentOrigin_ = {};
}

void Emitter::AuthorHeader() {
    if (!target_.nativeAuthorCode || target_.nativeAuthorCode->Header().empty()) return;
    const auto start = output_.Size();
    output_.Raw(target_.nativeAuthorCode->Header());
    output_.RecordAuthor(start, target_.nativeAuthorCode->Header().size(), 0, LuaSourceSpanKind::AuthorHeader);
    if (output_.Size() > 8 * 1024 * 1024) Fail("LUA_OUTPUT_BUDGET", "生成文本超过 8 MiB 预算。");
}
void Emitter::AuthorTrigger(const Trigger::TriggerRecord& trigger, const LuaTriggerSymbol& symbol) {
    currentTrigger_ = trigger.id; currentNode_ = trigger.id; currentOrigin_ = trigger.origin;
    const auto blocks = target_.nativeAuthorCode->Triggers();
    const auto block = std::find_if(blocks.begin(), blocks.end(), [&](const auto& value) { return value.triggerId == trigger.id; });
    if (block == blocks.end()) Fail("LUA_AUTHOR_TRIGGER_MISSING", "活动 Lua 作者块不在冻结凭据中。");
    const auto start = output_.Size();
    output_.Raw(block->text);
    output_.RecordAuthor(start, block->text.size(), trigger.id, LuaSourceSpanKind::AuthorCustom);
    if (trigger.runOnInit) initializers_.emplace_back(trigger.id, symbol.variable);
}

std::optional<Expression> Emitter::BoundGlobal(std::string_view name) {
    const auto found = boundGlobals_.find(std::string(name));
    if (found == boundGlobals_.end()) return std::nullopt;
    requiredGlobals_.insert(found->first);
    return Expression{found->second, found->first, {}};
}
std::optional<Expression> Emitter::BoundConstant(std::string_view name) {
    const auto found = constants_.find(std::string(name));
    if (found == constants_.end()) return std::nullopt;
    requiredConstants_.insert(found->first);
    return Expression{found->second, found->first, {}};
}

void Emitter::Globals() {
    MappedText initialize;
    initialize.Line("function InitGlobals()"); initialize.Begin();
    for (const auto& variable : document_.variables) {
        currentNode_ = variable.id; currentOrigin_ = variable.origin;
        const auto type = Base(variable.typeName.text);
        const auto name = "udg_" + variable.name.text;
        const auto declarationStart = output_.Size();
        output_.Line(name + " = " + (variable.isArray ? "__forge_array(" + Default(type) + ")" : Default(type)));
        output_.Record(declarationStart, 0, variable.id, LuaSourceSpanKind::Variable);
        std::string initial = variable.value.text;
        Expression value{type, Default(type), {}};
        if (!initial.empty()) value = Literal(initial, variable.typeName.text, false);
        else if (!variable.hasDefault) {
            initial = Define("TriggerTypeDefaults", variable.typeName.text, 0);
            if (initial.ends_with("()") && Identifier(initial.substr(0, initial.size() - 2)))
                value = Native(initial.substr(0, initial.size() - 2), {}, type);
            else if (!initial.empty()) value = Literal(initial, variable.typeName.text, false);
        }
        const auto initializeStart = initialize.Size();
        if (variable.isArray) {
            // GUI 声明的初始化范围是包含两端的 0..size；工厂不能折叠为共享对象。
            initialize.Line("for i = 0, " + std::to_string(variable.size) + " do"); initialize.Begin();
            initialize.Line(name + "[i] = " + value.code);
            initialize.End(); initialize.Line("end");
        } else initialize.Line(name + " = " + value.code);
        initialize.Record(initializeStart, 0, variable.id, LuaSourceSpanKind::Variable);
        if (output_.Size() + initialize.Size() > 8 * 1024 * 1024)
            Fail("LUA_OUTPUT_BUDGET", "变量初始化文本超过 8 MiB 预算。");
    }
    initialize.End(); initialize.Line("end");
    output_.Append(initialize);
}

void Emitter::Children(const Trigger::EcaNode& node, std::int32_t block, MappedText& output) {
    for (const auto index : node.children) {
        if (index >= document_.ecaNodes.size()) Fail("LUA_ECA_INDEX", "子节点索引越界。");
        const auto& child = document_.ecaNodes[index];
        if (child.childId == block) Statement(index, output);
    }
}

void Emitter::Statement(std::uint32_t index, MappedText& output, bool gateEnabled) {
    Visit visit(*this, false, index);
    if (output.Size() > 8 * 1024 * 1024) Fail("LUA_OUTPUT_BUDGET", "动作文本超过 8 MiB 预算。");
    const auto& node = document_.ecaNodes[index];
    if (gateEnabled && !node.enabled) return;
    const auto start = output.Size();
    StatementBody(node, output);
    output.Record(start, currentTrigger_, node.id, LuaSourceSpanKind::Statement);
}

void Emitter::StatementBody(const Trigger::EcaNode& node, MappedText& output) {
    if (node.type != 2) Fail("LUA_STATEMENT_KIND", "动作位置存在非动作节点。");
    const auto& definition = Definition(node);
    if (node.name.text == "SetVariable" && node.arguments.size() == 2 && node.children.empty()) {
        if (node.arguments[0] >= document_.argumentNodes.size()) Fail("LUA_ARGUMENT_INDEX", "赋值目标索引越界。");
        const auto& target = document_.argumentNodes[node.arguments[0]];
        if (!variables_.contains(target.value.text)) Fail("LUA_ASSIGNMENT_TARGET", "赋值目标必须是用户变量；生成对象绑定只读。");
        const auto variable = Variable(target);
        const auto value = Argument(node.arguments[1], variables_.at(target.value.text)->typeName.text);
        output.Line("do"); output.Begin();
        if (target.indexArgument) {
            // 把索引和右侧副作用显式排好序，避免右侧改变下标变量后写入另一格。
            output.Line("local key = " + Argument(*target.indexArgument, "integer").code);
            output.Line("local value = " + value.code);
            output.Line("udg_" + target.value.text + "[key] = value");
        } else output.Line(variable.code + " = " + value.code);
        output.End(); output.Line("end");
        return;
    }
    if (node.name.text == "ForLoopVarMultiple" && node.arguments.size() == 3) {
        if (node.arguments[0] >= document_.argumentNodes.size()) Fail("LUA_ARGUMENT_INDEX", "循环变量索引越界。");
        const auto& target = document_.argumentNodes[node.arguments[0]];
        if (!variables_.contains(target.value.text)) Fail("LUA_LOOP_VARIABLE", "循环目标必须是用户整数变量，不能覆盖生成对象绑定。");
        const auto variable = Variable(target);
        if (variable.type != "integer" || target.indexArgument) Fail("LUA_LOOP_VARIABLE", "循环变量必须是整数标量。");
        for (const auto child : node.children)
            if (child >= document_.ecaNodes.size() || document_.ecaNodes[child].childId != 0)
                Fail("LUA_LOOP_BLOCK", "循环存在未知子块。");
        output.Line(variable.code + " = " + Argument(node.arguments[1], "integer").code);
        output.Line("while true do"); output.Begin();
        output.Line("if " + variable.code + " > " + Argument(node.arguments[2], "integer").code + " then break end");
        Children(node, 0, output);
        output.Line(variable.code + " = __forge_i32(" + variable.code + " + 1)");
        output.End(); output.Line("end");
        return;
    }
    if (node.name.text == "IfThenElseMultiple" && node.arguments.empty()) {
        std::string condition = "true";
        for (const auto childIndex : node.children) {
            if (childIndex >= document_.ecaNodes.size()) Fail("LUA_ECA_INDEX", "条件子节点索引越界。");
            const auto& child = document_.ecaNodes[childIndex];
            if (!child.childId || *child.childId < 0 || *child.childId > 2)
                Fail("LUA_IF_BLOCK", "条件语句含未知子块。");
            if (child.childId == 0 && child.enabled)
                condition = CheckExpressionSize(condition + " and (" + Checked(Node(childIndex), "boolean").code + ")");
        }
        output.Line("if " + condition + " then"); output.Begin(); Children(node, 1, output);
        output.End(); output.Line("else"); output.Begin(); Children(node, 2, output);
        output.End(); output.Line("end");
        return;
    }
    if (node.name.text == "ReturnAction" && node.arguments.empty() && node.children.empty()) {
        output.Line("do return end");
        return;
    }
    if (!node.children.empty()) Fail("LUA_CHILDREN_UNTRANSLATED", "此动作的子块尚未支持，不能丢弃或顺序展开。");
    const auto call = Native(ScriptName(node, definition), Arguments(node, definition), "nothing");
    output.Line(call.code + ";");
}

void Emitter::TriggerBody(const Trigger::TriggerRecord& trigger, const LuaTriggerSymbol& symbol) {
    currentTrigger_ = trigger.id; currentNode_ = trigger.id; currentOrigin_ = trigger.origin;
    const auto triggerStart = output_.Size();
    MappedText actions, events;
    std::string conditions = "true";
    bool initialization = trigger.runOnInit != 0;
    for (const auto index : trigger.ecas) {
        if (events.Size() + actions.Size() > 8 * 1024 * 1024)
            Fail("LUA_OUTPUT_BUDGET", "触发器注册或动作文本超过 8 MiB 预算。");
        if (index >= document_.ecaNodes.size()) Fail("LUA_ECA_INDEX", "根 ECA 索引越界。");
        const auto& node = document_.ecaNodes[index];
        if (!node.enabled) continue;
        if (node.childId) Fail("LUA_ROOT_BLOCK", "根节点不应含子块编号。");
        if (node.type == 1) conditions = CheckExpressionSize(conditions + " and (" + Checked(Node(index), "boolean").code + ")");
        else if (node.type == 2) Statement(index, actions);
        else if (node.type == 0) {
            Visit visit(*this, false, index);
            const auto& definition = Definition(node);
            if (!node.children.empty()) Fail("LUA_EVENT_CHILDREN", "事件子块尚未支持。");
            if (node.name.text == "MapInitializationEvent" && node.arguments.empty()) initialization = true;
            else {
                auto arguments = Arguments(node, definition);
                arguments.insert(arguments.begin(), {"trigger", symbol.variable, {}});
                const auto eventStart = events.Size();
                events.Line(Native(ScriptName(node, definition), std::move(arguments), "event").code + ";");
                events.Record(eventStart, trigger.id, node.id, LuaSourceSpanKind::Event);
            }
        } else Fail("LUA_ROOT_KIND", "根节点不是事件/条件/动作。");
    }
    output_.Line(symbol.variable + " = nil");
    output_.Line("function " + symbol.conditions + "() return " + conditions + " end");
    output_.Line("function " + symbol.actions + "()"); output_.Append(actions); output_.Line("end");
    output_.Line("function " + symbol.initializer + "()"); output_.Begin();
    output_.Line(symbol.variable + " = " + Native("CreateTrigger", {}, "trigger").code + ";");
    output_.Append(events);
    if (trigger.initiallyClosed)
        output_.Line(Native("DisableTrigger", {{"trigger", symbol.variable, {}}}, "nothing").code + ";");
    auto condition = Native("Condition", {{"code", symbol.conditions, {}}}, "boolexpr");
    output_.Line(Native("TriggerAddCondition", {{"trigger", symbol.variable, {}}, std::move(condition)}, "triggercondition").code + ";");
    output_.Line(Native("TriggerAddAction", {{"trigger", symbol.variable, {}}, {"code", symbol.actions, {}}}, "triggeraction").code + ";");
    output_.End(); output_.Line("end");
    output_.Record(triggerStart, trigger.id, trigger.id, LuaSourceSpanKind::Trigger);
    // 只按明确初始化事件/标志登记，仍经条件执行。初始关闭的运行语义留给目标 native。
    if (initialization) initializers_.emplace_back(trigger.id, symbol.variable);
}

LuaTriggerUnit Emitter::Run() {
    result_.targetIdentity = target_.identity;
    result_.uiCatalogSha256 = catalog_.Sha256();
    result_.sourceWtgSha256 = document_.SourceWtgSha256();
    result_.sourceWctSha256 = document_.SourceWctSha256();
    try {
        Prepare();
        result_.diagnostics.push_back({{DiagnosticSeverity::Warning, "Script.Lua", "LUA_RUNTIME_UNVERIFIED",
            "候选仅含触发器单元；整图初始化和原版 Lua/联机未验证，实数计算不承诺经典 float32 逐位等价。", {}}, 0, 0});
        output_.Raw("-- FORGE Lua trigger compilation unit; requires host map initialization.");
        output_.Raw(kLuaTriggerRuntimeSource);
        Globals();
        AuthorHeader();
        // 分类和原始触发器顺序稳定；初始化 API 和随机调用的先后不能依赖哈希容器。
        std::vector<const Trigger::TriggerRecord*> ordered;
        std::set<StableId> placed;
        for (const auto& category : document_.categories)
            for (const auto& trigger : document_.triggers)
                if (trigger.categoryId == category.categoryId && symbols_.contains(trigger.id) && placed.insert(trigger.id).second)
                    ordered.push_back(&trigger);
        for (const auto& trigger : document_.triggers)
            if (symbols_.contains(trigger.id) && placed.insert(trigger.id).second) ordered.push_back(&trigger);
        for (const auto* trigger : ordered) {
            if (trigger->customTextFlag) AuthorTrigger(*trigger, symbols_.at(trigger->id));
            else TriggerBody(*trigger, symbols_.at(trigger->id));
            result_.symbols.push_back(symbols_.at(trigger->id));
            if (output_.Size() > 8 * 1024 * 1024) Fail("LUA_OUTPUT_BUDGET", "生成文本超过 8 MiB 预算。");
        }
        for (const auto& binding : callbacks_) {
            const auto start = output_.Size();
            output_.Line("function " + binding.symbol + "() return " + symbols_.at(binding.triggerId).actions + "() end");
            output_.Record(start, binding.triggerId, binding.triggerId, LuaSourceSpanKind::Callback);
        }
        output_.Line("function InitCustomTriggers()"); output_.Begin();
        for (const auto& symbol : result_.symbols) {
            const auto start = output_.Size();
            output_.Line(symbol.initializer + "()");
            output_.Record(start, symbol.triggerId, symbol.triggerId, LuaSourceSpanKind::Trigger);
        }
        output_.End(); output_.Line("end");
        output_.Line("function RunInitializationTriggers()"); output_.Begin();
        for (const auto& [id, name] : initializers_) {
            const auto start = output_.Size();
            output_.Line(Native("ConditionalTriggerExecute", {{"trigger", name, {}}}, "nothing").code + ";");
            output_.Record(start, id, id, LuaSourceSpanKind::Trigger);
        }
        output_.End(); output_.Line("end");
        const auto scriptHash = Sha256Text(output_.Text());
        if (!scriptHash.Succeeded()) Fail("LUA_HASH_FAILED", "候选脚本指纹生成失败。");
        auto spans = output_.TakeSpans();
        std::string mapIdentity = "forge-lua-source-map-v1:" + *scriptHash.value;
        for (const auto& span : spans) {
            for (const auto value : {static_cast<std::uint64_t>(span.byteOffset), static_cast<std::uint64_t>(span.byteLength),
                span.triggerId, span.nodeId, static_cast<std::uint64_t>(span.kind)}) {
                for (unsigned shift = 0; shift < 64; shift += 8) mapIdentity.push_back(static_cast<char>((value >> shift) & 0xff));
            }
        }
        const auto mapHash = Sha256Text(mapIdentity);
        if (!mapHash.Succeeded()) Fail("LUA_HASH_FAILED", "代码来源映射指纹生成失败。");
        result_.scriptSha256 = *scriptHash.value;
        result_.sourceMapSha256 = *mapHash.value;
        result_.sourceSpans = std::move(spans);
        result_.text = output_.Take();
    } catch (const LuaSourceDiagnostic& failure) {
        result_.diagnostics.push_back(failure);
    } catch (const SourceMapLimit&) {
        result_.diagnostics.push_back({{DiagnosticSeverity::Error, "Script.Lua", "LUA_SOURCE_MAP_BUDGET",
            "生成代码来源超过 262144 个范围，没有返回部分候选。", {}}, currentTrigger_, currentNode_});
    } catch (const std::bad_alloc&) {
        result_.diagnostics.push_back({{DiagnosticSeverity::Error, "Script.Lua", "LUA_ALLOCATION_FAILED", "编译内存不足。", {}}, currentTrigger_, currentNode_});
    }
    result_.requiredApis.assign(requiredApis_.begin(), requiredApis_.end());
    result_.requiredGlobals.assign(requiredGlobals_.begin(), requiredGlobals_.end());
    result_.requiredConstants.assign(requiredConstants_.begin(), requiredConstants_.end());
    return std::move(result_);
}
} // namespace War3Map::Script::lua

namespace War3Map::Script {
bool LuaTriggerUnit::Succeeded() const noexcept {
    return !text.empty() && std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) {
        return item.diagnostic.severity == DiagnosticSeverity::Error;
    });
}
LuaTriggerUnit GenerateLuaTriggerUnit(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    const LuaTargetProfile& target, std::span<const LuaCallbackBinding> callbacks, std::span<const LuaGlobalBinding> globals) {
    try { return lua::Emitter(document, catalog, target, callbacks, globals).Run(); }
    catch (const std::bad_alloc&) {
        LuaTriggerUnit result;
        result.diagnostics.push_back({{DiagnosticSeverity::Error, "Script.Lua", "LUA_ALLOCATION_FAILED", "类型目录建立失败：内存不足。", {}}, 0, 0});
        return result;
    }
}
} // namespace War3Map::Script
