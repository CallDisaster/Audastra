#include "War3MapCore/Script/LuaTestReload.h"
#include "War3MapCore/Script/LuaAuthorCode.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "../src/ScriptBuild/LuaCustomReloadAnalysis.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>

namespace {
using namespace War3Map;
using namespace War3Map::Script;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "missing result" : result.diagnostics.front().message);
    return std::move(*result.value);
}
std::string Replace(std::string text, std::string_view before, std::string_view after) {
    const auto at = text.find(before); Require(at != std::string::npos, "fixture replacement missing");
    text.replace(at, before.size(), after); return text;
}
std::string Code(int action = 1, bool condition = true, int timer = 1) {
    return "-- 原创 Lua 热重载样例\nfunction CustomAction()\n    Record(" + std::to_string(action)
        + ")\nend\nfunction CustomCondition()\n    return " + (condition ? "true" : "false")
        + "\nend\nfunction CustomTimer()\n    timerValue = " + std::to_string(timer)
        + "\nend\nfunction InitTrig_Custom()\n    initCalls = initCalls + 1\n"
        "    local t = CreateTrigger()\n    gg_trg_Custom = t\n    TriggerAddAction(t, CustomAction)\n"
        "    TriggerAddCondition(t, Condition(CustomCondition))\n    TimerStart(CreateTimer(), 1, true, CustomTimer)\nend\n";
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-custom-reload-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument document;
    std::vector<std::byte> settings;
    StableId id = 0;
    LuaTargetProfile target;
    Fixture() {
        const auto write = [&](std::string path, std::string text) {
            std::filesystem::create_directories((root / path).parent_path());
            std::ofstream stream(root / path, std::ios::binary); stream << text; Require(bool(stream), "write own fixture");
        };
        write("ui/config", "base\n"); write("ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer\n");
        for (const auto* file : {"event", "condition", "action", "call"}) write("ui/base/" + std::string(file) + ".txt", "");
        write("ui/base/action.txt", "[Record]\n[[.args]]\ntype=integer\ndefault=0\n");
        catalog = Take(Trigger::UiCatalog::Load(root)); document = Take(Trigger::WtgWctCodec{}.CreateEmpty(catalog));
        MapData::MapSettings map; map.formatVersion = 31; map.scriptLanguage = 1; map.custom.emplace(); map.tileset = 'L';
        map.gameVersion = {1, 32, 0, 0}; map.graphicsModes = 3; map.gameDataVersion = 1; settings = Take(map.Serialize());
        Header("initCalls = 0\ntimerValue = 0\n");
        const auto plan = Take(LuaAuthorCodeEditor::PrepareNewCustom(settings, document, catalog, -1, "Custom", Code(),
            Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes, "Original"));
        (void)Take(Trigger::TriggerCodeEditor::Apply(document, catalog, plan)); id = document.triggers.front().id;
        target.identity = "original-custom-lua53-test-host";
        for (const auto* name : {"trigger", "boolexpr", "triggercondition", "triggeraction"}) target.handleTypes.emplace_back(name, "handle");
        target.apis = {{"DestroyTrigger", "nothing", {"trigger"}}, {"DestroyBoolExpr", "nothing", {"boolexpr"}},
            {"CreateTrigger", "trigger", {}}, {"Condition", "boolexpr", {"code"}},
            {"TriggerAddAction", "triggeraction", {"trigger", "code"}},
            {"TriggerAddCondition", "triggercondition", {"trigger", "boolexpr"}}, {"Record", "nothing", {"integer"}}};
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Header(std::string_view source) {
        const auto plan = Take(LuaAuthorCodeEditor::PrepareText(settings, document, catalog, {Trigger::CodeBlockKind::GlobalHeader, 0}, source));
        (void)Take(Trigger::TriggerCodeEditor::Apply(document, catalog, plan));
    }
    void Set(std::string_view source) {
        const auto plan = Take(LuaAuthorCodeEditor::PrepareText(settings, document, catalog, {Trigger::CodeBlockKind::CustomTrigger, id}, source));
        (void)Take(Trigger::TriggerCodeEditor::Apply(document, catalog, plan));
    }
    struct Gui { StableId id, argument; };
    Gui AddGui(std::string name, int amount) {
        Require(!document.categories.empty(), "custom fixture category missing");
        const auto trigger = Take(Trigger::TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, std::move(name)));
        const auto receipt = Take(Trigger::TriggerDocumentEditor::AppendRootEca(document, catalog, trigger,
            Trigger::UiDefinitionKind::Action, "Record"));
        const auto* eca = Trigger::TriggerDocumentEditor::FindEca(document, receipt.rootEcaId);
        Require(eca && eca->arguments.size() == 1, "GUI action fixture missing argument");
        const auto argument = document.argumentNodes[eca->arguments.front()].id;
        SetGui({trigger, argument}, amount); return {trigger, argument};
    }
    void SetGui(Gui gui, int amount) {
        Require(Trigger::TriggerDocumentEditor::SetScalarArgument(document, gui.argument, 3, std::to_string(amount)).Succeeded(),
            "GUI action fixture edit failed");
    }
    LuaTriggerUnit Generate(bool enabled = true) {
        target.nativeAuthorCode = std::make_shared<const PreparedLuaAuthorCode>(Take(CaptureNativeLuaAuthorCode(settings, document, catalog)));
        auto result = GenerateLuaTriggerUnit(document, catalog, target, {}, {}, {enabled});
        if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.back().diagnostic.message);
        return result;
    }
};
class Host {
public:
    Host() : state_(luaL_newstate(), lua_close) {
        Require(state_ != nullptr, "allocate Lua host"); luaL_requiref(state_.get(), "_G", luaopen_base, 1); lua_pop(state_.get(), 1);
        luaL_requiref(state_.get(), "math", luaopen_math, 1); lua_pop(state_.get(), 1);
        Run(R"lua(
created, destroyed, timers, recorded = 0, 0, 0, 0
function CreateTrigger() created=created+1; return {} end
function DestroyTrigger(t) destroyed=destroyed+1 end
function Condition(f) return {callback=f} end
function DestroyBoolExpr(c) end
function TriggerAddAction(t,f) t.action=f end
function TriggerAddCondition(t,c) t.condition=c.callback end
function CreateTimer() timers=timers+1; return {} end
function TimerStart(t,p,r,f) savedTimerCallback=f end
function Record(value) if value==999 then error('original custom runtime failure',2) end; recorded=value end
)lua", "@original-host.lua");
    }
    std::string Try(std::string_view text, const char* source = "@Audastra/test-reload.lua") {
        auto* state = state_.get();
        if (luaL_loadbufferx(state, text.data(), text.size(), source, "t") != LUA_OK || lua_pcall(state, 0, 0, 0) != LUA_OK) {
            const auto* error = lua_tostring(state, -1); std::string result = error ? error : "Lua host error"; lua_pop(state, 1); return result;
        }
        return {};
    }
    void Run(std::string_view text, const char* source = "@Audastra/test-reload.lua") {
        const auto error = Try(text, source); if (!error.empty()) throw std::runtime_error(error);
    }
private:
    std::unique_ptr<lua_State, decltype(&lua_close)> state_;
};
LuaTestReloadPatch Patch(const LuaTriggerUnit& before, const LuaTriggerUnit& after) {
    return Take(PrepareLuaTestReload(before, after, "same-owned-map", "same-owned-map"));
}
void ActionsConditionsTimersAndFormalBytes() {
    Fixture fixture;
    const auto formal = fixture.Generate(false); auto baseline = fixture.Generate();
    Require(fixture.Generate(false).text == formal.text && formal.text.find(Code()) != std::string::npos,
        "formal author text changed after test generation");
    Require(formal.text.find("function(...) return CustomAction") == std::string::npos, "formal text contains test forwarding");
    Require(baseline.text.find("function(...) return CustomAction(...) end") != std::string::npos, "named action not forwarded");
    Require(baseline.text.find("TimerStart(CreateTimer(), 1, true, CustomTimer)") != std::string::npos, "timer callback rewritten");
    Host host; host.Run(baseline.text, "@original-base.lua"); host.Run("InitGlobals(); InitCustomTriggers()");
    host.Run("gg_trg_Custom.action(); savedTimerCallback(); assert(recorded==1 and timerValue==1 and initCalls==1)");
    fixture.Set(Code(5, false, 7)); auto current = fixture.Generate(); auto patch = Patch(baseline, current);
    Require(patch.changes.size() == 1 && patch.changes.front().kind == LuaTestReloadChangeKind::Functions, "custom update recreates trigger");
    host.Run(patch.source); host.Run("assert(not gg_trg_Custom.condition()); gg_trg_Custom.action(); assert(recorded==5); "
        "savedTimerCallback(); assert(timerValue==1); CustomTimer(); assert(timerValue==7); "
        "assert(created==1 and destroyed==0 and timers==1 and initCalls==1)");
    baseline = current;
    for (int iteration = 0; iteration < 20; ++iteration) {
        fixture.Set(Code(10 + iteration, true, 9)); current = fixture.Generate(); patch = Patch(baseline, current); host.Run(patch.source);
        host.Run("gg_trg_Custom.action(); assert(gg_trg_Custom.condition() and recorded==" + std::to_string(10 + iteration)
            + " and created==1 and destroyed==0 and timers==1 and initCalls==1); savedTimerCallback(); assert(timerValue==1)");
        baseline = current;
    }
    fixture.Set(Code(999)); current = fixture.Generate(); patch = Patch(baseline, current); host.Run(patch.source);
    const auto failure = host.Try("gg_trg_Custom.action()", "@original-caller.lua");
    const std::string prefix = "Audastra/test-reload.lua:"; const auto at = failure.find(prefix);
    Require(at != std::string::npos, "runtime error lost changed author source");
    const auto line = static_cast<std::size_t>(std::stoul(failure.substr(at + prefix.size())));
    const auto location = LocateLuaTestReload(patch, line);
    Require(location && location->triggerId == fixture.id && location->nodeId == fixture.id && location->sourceLine == 3,
        "runtime error not mapped to original author line");
}
void RejectedShapesAndUnchangedUnsupportedBlocks() {
    Fixture fixture; const auto baseline = fixture.Generate();
    const std::vector<std::string> rejected{
        "topLevelEffect = 1\n" + Code(2), "local captured = 1\n" + Code(2),
        Replace(Code(2), "initCalls = initCalls + 1", "initCalls = initCalls + 2"),
        Replace(Code(2), "function CustomAction()", "function CustomAction(unprovenParameter)"),
        Code(2) + "function NewFunction() end\n",
        Replace(Code(2), "TriggerAddAction(t, CustomAction)", "TriggerAddAction(t, function() CustomAction() end)"),
        Replace(Code(2), "TriggerAddAction(t, CustomAction)", "jass.common.TriggerAddAction(t, CustomAction)"),
        Replace(Code(2), "TriggerAddAction(t, CustomAction)", "jass.common['TriggerAddAction'](t, function() Record(1) end)"),
        Replace(Code(2), "TriggerAddAction(t, CustomAction)", "local add = TriggerAddAction; add(t, CustomAction)"),
        Replace(Code(2), "TriggerAddAction(t, CustomAction)", "local callback = CustomAction; TriggerAddAction(t, callback)"),
        Replace(Code(2), "Record(2)", "local CustomAction = function() end; CustomAction()"),
        Replace(Code(2), "Record(2)", "for CustomAction=1,2 do Record(CustomAction) end"),
        Replace(Code(2), "Record(2)", "for _, CustomAction in pairs({}) do CustomAction() end"),
        Replace(Code(2), "Record(2)", "function CustomAction() Record(3) end"),
        Replace(Code(2), "Record(2)", "local loaded = load('return CustomAction'); loaded()()"),
        Replace(Code(2), "Record(2)", "local module = require('original'); module.run()"),
        Replace(Code(2), "TimerStart(CreateTimer(), 1, true, CustomTimer)", "TimerStart(CreateTimer(), 1, true, function() CustomTimer() end)")};
    for (const auto& source : rejected) {
        fixture.Set(source); const auto current = fixture.Generate();
        Require(!PrepareLuaTestReload(baseline, current, "same", "same").Succeeded(), "unproven custom structure accepted");
        const auto noChange = Take(PrepareLuaTestReload(current, current, "same", "same"));
        Require(noChange.source.empty(), "unchanged unsupported source was executed again");
    }
    fixture.Set(Code()); fixture.Header("initCalls = 0\ntimerValue = 0\nlocal captured = 5\n");
    const auto localBaseline = fixture.Generate(); fixture.Set(Code(2)); const auto localCurrent = fixture.Generate();
    Require(!PrepareLuaTestReload(localBaseline, localCurrent, "same", "same").Succeeded(), "header capture accepted");
    for (const auto* header : {
        "function RetainCallback() return CustomAction end\n",
        "function HeaderCall(CustomAction) CustomAction() end\n",
        "function HeaderCall() for CustomAction=1,2 do end end\n",
        "function HeaderCall() function CustomAction() end end\n",
        "function HeaderCall() return load('return CustomAction')() end\n"}) {
        fixture.Header(std::string("initCalls = 0\ntimerValue = 0\n") + header); fixture.Set(Code());
        const auto before = fixture.Generate(); fixture.Set(Code(2));
        Require(!PrepareLuaTestReload(before, fixture.Generate(), "same", "same").Succeeded(), "header callback escape accepted");
    }
    fixture.Header("initCalls = 0\ntimerValue = 0\n"); fixture.Set(Code());
    auto original = fixture.Generate(); fixture.Header("initCalls = 0\ntimerValue = 9\n");
    Require(!PrepareLuaTestReload(original, fixture.Generate(), "same", "same").Succeeded(), "changed header accepted");
    fixture.Header("initCalls = 0\ntimerValue = 0\n");
    fixture.document.triggers.front().enabled = 0;
    Require(!PrepareLuaTestReload(original, fixture.Generate(), "same", "same").Succeeded(), "custom removal accepted");
}
void CrossBlockForwardingAndManifest() {
    Fixture fixture;
    fixture.Header("initCalls = 0\ntimerValue = 0\nfunction HeaderCall() CustomAction() end\n");
    const auto plan = Take(LuaAuthorCodeEditor::PrepareNewCustom(fixture.settings, fixture.document, fixture.catalog, -1, "Sibling",
        "function InitTrig_Sibling()\n    local t = CreateTrigger()\n    gg_trg_Sibling = t\n"
        "    TriggerAddAction(t, CustomAction)\n    TriggerAddCondition(t, Condition(CustomCondition))\nend\n",
        Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes, "Original sibling"));
    (void)Take(Trigger::TriggerCodeEditor::Apply(fixture.document, fixture.catalog, plan));
    const auto baseline = fixture.Generate(); Host host; host.Run(baseline.text); host.Run("InitGlobals(); InitCustomTriggers()");
    fixture.Set(Code(8, false, 8)); const auto current = fixture.Generate(); const auto patch = Patch(baseline, current);
    Require(patch.changes.size() == 1, "unchanged sibling unnecessarily reloaded"); host.Run(patch.source);
    host.Run("gg_trg_Sibling.action(); assert(recorded==8 and not gg_trg_Sibling.condition()); "
        "recorded=0; HeaderCall(); assert(recorded==8 and created==2 and destroyed==0 and initCalls==1); "
        "savedTimerCallback(); assert(timerValue==1)");
    auto forgedBaseline = baseline; forgedBaseline.testTriggers.front().authorSource = Code(4);
    Require(!PrepareLuaTestReload(forgedBaseline, current, "same", "same").Succeeded(), "forged baseline author source accepted");
    auto forgedCurrent = current; forgedCurrent.testTriggers.front().authorSource = Code(4);
    Require(!PrepareLuaTestReload(baseline, forgedCurrent, "same", "same").Succeeded(), "forged current author source accepted");
}
void ParsingBoundariesAndSiblingScope() {
    Fixture fixture;
    const auto source = Replace(Code(), "Record(1)", "local text = [=[end function TriggerAddAction(t, Fake)]=]\n"
        "    --[==[ function Fake() end ]==]\n    for i=1,1 do while false do break end end\n"
        "    repeat local n=1 until true\n    if true then Record(1) elseif false then Record(0) end");
    fixture.Set(source); const auto original = fixture.Generate(); fixture.Set(Replace(source, "Record(1)", "Record(2)"));
    const auto current = fixture.Generate(); Host host; host.Run(original.text); host.Run("InitGlobals(); InitCustomTriggers()");
    host.Run(Patch(original, current).source); host.Run("gg_trg_Custom.action(); assert(recorded==2)");
    // 兄弟块的顶层局部能被后续作者函数捕获，不能只审被修改的一个块。
    const auto plan = Take(LuaAuthorCodeEditor::PrepareNewCustom(fixture.settings, fixture.document, fixture.catalog, -1, "Sibling",
        "local siblingState = 1\nfunction InitTrig_Sibling() gg_trg_Sibling = CreateTrigger() end\n",
        Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes, "Sibling"));
    (void)Take(Trigger::TriggerCodeEditor::Apply(fixture.document, fixture.catalog, plan));
    const auto unsafe = fixture.Generate(); fixture.Set(Code(3));
    Require(!PrepareLuaTestReload(unsafe, fixture.Generate(), "same", "same").Succeeded(), "unsafe sibling context accepted");
    Lua::SyntaxOptions cancelled; std::stop_source stop; cancelled.cancel = stop.get_token(); stop.request_stop();
    const std::array<Script::lua::CustomReloadSource, 1> sources{{{source, "InitTrig_Custom"}}};
    Require(!Script::lua::CaptureCustomReloadContext({}, sources, cancelled).Succeeded(), "cancelled custom scan succeeded");
}
void CapturedGuiFunctionsRequireRetest() {
    Fixture fixture; const auto first = fixture.AddGui("FirstGui", 5); const auto second = fixture.AddGui("SecondGui", 6);
    const auto generated = fixture.Generate();
    const auto symbol = std::find_if(generated.symbols.begin(), generated.symbols.end(), [&](const auto& value) { return value.triggerId == first.id; });
    Require(symbol != generated.symbols.end(), "GUI fixture symbol missing");
    const auto action = symbol->actions; const auto condition = symbol->conditions;
    const auto requireReject = [&](const LuaTriggerUnit& baseline) {
        fixture.SetGui(first, 7); const auto result = PrepareLuaTestReload(baseline, fixture.Generate(), "same", "same");
        Require(!result.Succeeded() && !result.diagnostics.empty()
            && result.diagnostics.front().code == "LUA_RELOAD_AUTHOR_GUI_REFERENCE", "saved GUI function accepted");
        fixture.SetGui(first, 5);
    };
    for (const auto& expression : {"savedGui = " + action, "local callback = " + action + "; SaveCallback(callback)",
        "SaveCallback(" + condition + ")"}) {
        fixture.Set(Replace(Code(), "Record(1)", expression)); const auto baseline = fixture.Generate(); requireReject(baseline);
        // 作者保存的是 FirstGui；修改另一个 GUI 仍应通过，不能禁用整张图的 GUI 热重载。
        fixture.SetGui(second, 9); Require(PrepareLuaTestReload(baseline, fixture.Generate(), "same", "same").Succeeded(),
            "unrelated GUI incorrectly blocked"); fixture.SetGui(second, 6);
        auto forged = baseline;
        for (auto& chunk : forged.testTriggers) if (chunk.customCode) chunk.authorSource = Code();
        requireReject(forged);
    }
    fixture.Set(Code()); fixture.Header("initCalls = 0\ntimerValue = 0\nfunction RetainGui() return " + action + " end\n");
    requireReject(fixture.Generate()); fixture.Header("initCalls = 0\ntimerValue = 0\n");
    // 直接按稳定的全局名称调用，不保留旧对象；改动后调用会取得新动作。
    fixture.Set(Replace(Code(), "Record(1)", action + "()")); const auto direct = fixture.Generate();
    Host host; host.Run(direct.text); host.Run("InitGlobals(); InitCustomTriggers(); gg_trg_Custom.action(); assert(recorded==5)");
    fixture.SetGui(first, 7); const auto patch = Patch(direct, fixture.Generate()); host.Run(patch.source);
    host.Run("gg_trg_Custom.action(); assert(recorded==7)"); fixture.SetGui(first, 5);
    // 词法扫描跳过字符串和注释中的同名文本。
    fixture.Set(Replace(Code(), "Record(1)", "local label = '" + action + "' -- " + condition + "\n    Record(1)"));
    const auto literal = fixture.Generate(); fixture.SetGui(first, 7);
    Require(PrepareLuaTestReload(literal, fixture.Generate(), "same", "same").Succeeded(), "GUI name in literal/comment incorrectly blocked");
    fixture.SetGui(first, 5); fixture.Set(Replace(Code(), "Record(1)", action + "()")); const auto beforeRename = fixture.Generate();
    Require(Trigger::TriggerDocumentEditor::RenameTrigger(fixture.document, first.id, "RenamedGui").Succeeded(), "GUI rename fixture failed");
    // GUI 脚本名由稳定 ID 生成，显示名称变化不会让直接调用失效。
    const auto renamed = fixture.Generate();
    const auto renamedSymbol = std::find_if(renamed.symbols.begin(), renamed.symbols.end(), [&](const auto& value) { return value.triggerId == first.id; });
    Require(renamedSymbol != renamed.symbols.end() && renamedSymbol->actions == action
        && PrepareLuaTestReload(beforeRename, renamed, "same", "same").Succeeded(), "display rename changed stable GUI call semantics");
}
}
int main() {
    try { ActionsConditionsTimersAndFormalBytes(); RejectedShapesAndUnchangedUnsupportedBlocks();
        CrossBlockForwardingAndManifest(); ParsingBoundariesAndSiblingScope(); CapturedGuiFunctionsRequireRetest();
        std::cout << "Custom Lua reload tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
