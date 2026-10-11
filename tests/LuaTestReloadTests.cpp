#include "War3MapCore/Script/LuaTestReload.h"
#include "War3MapCore/Services/LuaTestReloadController.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "../src/Core/Services/LuaTestMailboxSessionInternal.h"
#include "../src/WarcraftRuntime32/OwnLua/LuaMailboxRuntime.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Script;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << text;
    Require(static_cast<bool>(stream), "fixture write failed");
}
struct Fixture {
    Trigger::TriggerDocument document;
    Trigger::UiCatalog catalog;
    LuaTargetProfile target;
    std::uint32_t amount = 0, condition = 0, period = 0;
    explicit Fixture(const std::filesystem::path& root) {
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer\nreal=0,1,1,Real\nboolean=0,1,1,Boolean\n");
        Write(root / "ui/base/event.txt", "[TimerEvent]\nscript_name=TriggerRegisterTimerEvent\n"
            "[[.args]]\ntype=real\n[[.args]]\ntype=boolean\n");
        Write(root / "ui/base/condition.txt", "[CheckFlag]\nreturns=boolean\n[[.args]]\ntype=boolean\n");
        Write(root / "ui/base/action.txt", "[Record]\n[[.args]]\ntype=integer\n");
        Write(root / "ui/base/call.txt", "");
        auto loaded = Trigger::UiCatalog::Load(root);
        if (!loaded.Succeeded() || !loaded.value) {
            std::string errors; for (const auto& diagnostic : loaded.diagnostics) errors += diagnostic.code + ": " + diagnostic.message + "\n";
            throw std::runtime_error("fixture catalog failed: " + errors);
        }
        catalog = std::move(*loaded.value);
        target.identity = "original-lua53-test-host";
        for (const auto* name : {"trigger", "event", "boolexpr", "triggercondition", "triggeraction"})
            target.handleTypes.emplace_back(name, "handle");
        target.apis = {{"CreateTrigger", "trigger", {}}, {"DestroyTrigger", "nothing", {"trigger"}},
            {"Condition", "boolexpr", {"code"}}, {"DestroyBoolExpr", "nothing", {"boolexpr"}},
            {"TriggerAddCondition", "triggercondition", {"trigger", "boolexpr"}},
            {"TriggerAddAction", "triggeraction", {"trigger", "code"}}, {"DisableTrigger", "nothing", {"trigger"}},
            {"ConditionalTriggerExecute", "nothing", {"trigger"}},
            {"TriggerRegisterTimerEvent", "event", {"trigger", "real", "boolean"}},
            {"Record", "nothing", {"integer"}}, {"CheckFlag", "boolean", {"boolean"}}};
        const auto argument = [&](std::string text) {
            Trigger::ArgumentNode node; node.id = 100 + document.argumentNodes.size(); node.type = 3; node.value.text = std::move(text);
            document.argumentNodes.push_back(std::move(node)); return static_cast<std::uint32_t>(document.argumentNodes.size() - 1);
        };
        period = argument("1.0"); const auto repeating = argument("true"); condition = argument("true"); amount = argument("1");
        const auto eca = [&](int kind, const char* name, std::vector<std::uint32_t> arguments) {
            Trigger::EcaNode node; node.id = 200 + document.ecaNodes.size(); node.type = kind; node.name.text = name;
            node.arguments = std::move(arguments); document.ecaNodes.push_back(std::move(node));
        };
        eca(0, "TimerEvent", {period, repeating}); eca(1, "CheckFlag", {condition}); eca(2, "Record", {amount});
        Trigger::TriggerRecord trigger; trigger.id = 42; trigger.name.text = "Original"; trigger.ecas = {0, 1, 2};
        document.triggers.push_back(std::move(trigger));
        Trigger::VariableRecord variable; variable.id = 501; variable.name.text = "Existing";
        variable.typeName.text = "integer"; variable.value.text = "4"; document.variables.push_back(std::move(variable));
    }
    LuaTriggerUnit Generate(bool reload = true) const {
        auto result = GenerateLuaTriggerUnit(document, catalog, target, {}, {}, {reload});
        if (!result.Succeeded()) {
            std::string errors; for (const auto& diagnostic : result.diagnostics) errors += diagnostic.diagnostic.message + "\n";
            throw std::runtime_error(errors);
        }
        return result;
    }
};
class Host {
public:
    Host() : state_(luaL_newstate(), lua_close) {
        Require(state_ != nullptr, "Lua host allocation failed");
        luaL_requiref(state_.get(), "_G", luaopen_base, 1); lua_pop(state_.get(), 1);
        luaL_requiref(state_.get(), "math", luaopen_math, 1); lua_pop(state_.get(), 1);
        Run(R"lua(
activeTriggers, activeConditions, recorded = 0, 0, 0
function CreateTrigger() activeTriggers = activeTriggers + 1; return {} end
function DestroyTrigger(t) assert(not t.destroyed); t.destroyed = true; activeTriggers = activeTriggers - 1 end
function Condition(f) activeConditions = activeConditions + 1; return {callback=f} end
function DestroyBoolExpr(c) assert(not c.destroyed); c.destroyed=true; activeConditions=activeConditions-1 end
function TriggerAddCondition(t,c) t.condition=c; return {} end
function TriggerAddAction(t,f) t.action=f; return {} end
function TriggerRegisterTimerEvent(t,period,repeating) t.period=period; t.repeating=repeating; return {} end
function DisableTrigger(t) t.disabled=true end
function ConditionalTriggerExecute(t) if not t.disabled and t.condition.callback() then t.action() end end
function CheckFlag(flag) return flag end
function Record(amount) assert(amount ~= 999, 'original-runtime-error'); recorded=amount end
)lua");
    }
    void Run(std::string_view text) {
        auto* state = state_.get();
        if (luaL_loadbufferx(state, text.data(), text.size(), "@original-reload-fixture.lua", "t") != LUA_OK
            || lua_pcall(state, 0, 0, 0) != LUA_OK) {
            const auto* message = lua_tostring(state, -1); const auto error = message ? message : "Lua error";
            const std::string owned(error); lua_pop(state, 1); throw std::runtime_error(owned);
        }
    }
private:
    std::unique_ptr<lua_State, decltype(&lua_close)> state_;
};
LuaTestReloadPatch Prepare(const LuaTriggerUnit& before, const LuaTriggerUnit& after) {
    const auto result = PrepareLuaTestReload(before, after, "owned-map-facts", "owned-map-facts");
    if (!result.Succeeded() || !result.value) {
        std::string errors; for (const auto& diagnostic : result.diagnostics) errors += diagnostic.message + "\n";
        throw std::runtime_error(errors);
    }
    return *result.value;
}
struct MailboxFixture {
    std::shared_ptr<Services::LuaTestMailboxSession> session;
    std::unique_ptr<Services::detail::LuaTestMailboxLaunchHandle> inherited;
    Audastra::Runtime32::LuaTestMailbox* view = nullptr;
    std::unique_ptr<Audastra::Runtime32::OwnLua::LuaMailboxRuntime> runtime;
    bool running = true;
    MailboxFixture() {
        session = *Services::CreateLuaTestMailboxSession(std::string(64, 'a')).value;
        auto prepared = Services::detail::LuaTestMailboxLaunchAccess::Prepare(session, [this](std::uint32_t pid) {
            using Ownership = Services::detail::LuaTestProcessOwnership;
            return pid != GetCurrentProcessId() ? Ownership::Unconfirmed : running ? Ownership::OwnedRunning : Ownership::OwnedExited;
        });
        Require(prepared.Succeeded(), "mailbox launch fixture failed"); inherited = std::move(*prepared.value);
        view = static_cast<Audastra::Runtime32::LuaTestMailbox*>(MapViewOfFile(
            reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(inherited->Value())), FILE_MAP_READ | FILE_MAP_WRITE,
            0, 0, sizeof(*view)));
        Require(view != nullptr, "mailbox view failed");
        runtime = std::make_unique<Audastra::Runtime32::OwnLua::LuaMailboxRuntime>(*view);
        Require(runtime->Attach(inherited->Nonce(), session->MapSha256()), "mailbox runtime fixture failed");
    }
    ~MailboxFixture() { runtime.reset(); if (view) UnmapViewOfFile(view); }
};
void Controller(const std::filesystem::path& root) {
    Fixture fixture(root); MailboxFixture mailbox;
    auto baseline = fixture.Generate(); const auto firstSha = baseline.scriptSha256;
    const auto starting = Services::CreateLuaTestMailboxSession(std::string(64, 'b'));
    Require(starting.Succeeded(), "starting fixture failed");
    Services::LuaTestReloadController startingController(*starting.value, baseline, "map-facts");
    Require(startingController.Poll().value->outcome == Services::LuaReloadOutcome::Ready, "starting runtime falsely stopped");
    Services::LuaTestReloadController controller(mailbox.session, baseline, "map-facts");
    fixture.document.argumentNodes[fixture.amount].value.text = "17";
    auto next = fixture.Generate(); const auto nextSha = next.scriptSha256;
    auto sent = controller.Submit(next, "map-facts");
    Require(sent.Succeeded() && sent.value->outcome == Services::LuaReloadOutcome::Pending
        && sent.value->baselineScriptSha256 == firstSha, "queued request advanced baseline");
    Require(!controller.Submit(next, "map-facts").Succeeded(), "pending request was overwritten");
    auto request = mailbox.runtime->Take(); Require(request.has_value(), "no request received");
    Host host; host.Run(baseline.text); host.Run("InitGlobals(); InitCustomTriggers()"); host.Run(request->source);
    host.Run("ConditionalTriggerExecute(gg_trg_F42); assert(recorded==17)");
    Require(controller.Poll().value->baselineScriptSha256 == firstSha, "running request advanced baseline");
    Require(mailbox.runtime->Complete(request->sequence, Audastra::Runtime32::LuaReloadResult::Success, "updated"), "ack failed");
    mailbox.view->resultSequence += 1;
    Require(!controller.Poll().Succeeded(), "wrong sequence advanced baseline");
    mailbox.view->resultSequence -= 1;
    auto done = controller.Poll();
    Require(done.Succeeded() && done.value->outcome == Services::LuaReloadOutcome::Succeeded
        && done.value->baselineScriptSha256 == nextSha, "success ACK did not advance baseline");
    Require(controller.Submit(next, "map-facts").value->outcome == Services::LuaReloadOutcome::NoChanges, "noop queued source");
    Require(!controller.Submit(next, "changed-map-facts").Succeeded(), "map changes passed controller");
    fixture.document.argumentNodes[fixture.amount].value.text = "18"; next = fixture.Generate();
    sent = controller.Submit(next, "map-facts"); Require(sent.Succeeded(), "second reload failed");
    request = mailbox.runtime->Take(); Require(request.has_value(), "second request missing");
    const auto generatedLine = 1 + std::count(request->source.begin(), request->source.begin()
        + request->source.find("Record"), '\n');
    Require(mailbox.runtime->Complete(request->sequence, Audastra::Runtime32::LuaReloadResult::RuntimeError,
        "Audastra/test-reload.lua:" + std::to_string(generatedLine) + ": error"), "error ack failed");
    done = controller.Poll();
    Require(done.Succeeded() && done.value->outcome == Services::LuaReloadOutcome::RestartRequired
        && done.value->baselineScriptSha256 == nextSha && done.value->location
        && done.value->location->triggerId == 42 && done.value->location->nodeId == 202, "failure location/baseline lost");
    Require(!controller.Submit(next, "map-facts").Succeeded(), "partial failure allowed retry");
    mailbox.running = false;
    Require(controller.Poll().value->outcome == Services::LuaReloadOutcome::Stopped, "owned exit not observed");
}
void MapFacts(const std::filesystem::path& root) {
    const std::vector<MapArchiveMember> original{{"resource.bin", {std::byte{1}}}, {"war3map.wtg", {std::byte{2}}},
        {"war3map.lua", {std::byte{3}}}, {"main.lua", {std::byte{4}}}};
    auto written = WriteVerifiedArchive(root / "facts.w3x", original); Require(written.Succeeded(), "map fixture write");
    auto archive = MapArchiveSession::Open(root / "facts.w3x"); Require(archive.Succeeded(), "map fixture open");
    const std::vector<std::string> scripts{"war3map.lua", "war3map.j"};
    auto binding = Services::CaptureLuaTestMapBinding(*archive.value, {}, {}, scripts); Require(binding.Succeeded(), "facts capture");
    const std::vector<MemberReplacement> scriptChange{{"war3map.lua", {std::byte{7}}}, {"war3map.wtg", {std::byte{8}}}};
    Require(Services::CaptureLuaTestMapBinding(*archive.value, scriptChange, {}, scripts).value == binding.value, "script change became map change");
    const std::vector<MemberReplacement> moduleChange{{"main.lua", {std::byte{5}}}};
    Require(Services::CaptureLuaTestMapBinding(*archive.value, moduleChange, {}, scripts).value != binding.value, "author module incorrectly excluded");
    const std::vector<std::string> removed{"resource.bin"};
    Require(Services::CaptureLuaTestMapBinding(*archive.value, {}, removed, scripts).value != binding.value, "resource deletion ignored");
    Services::MapSaveTask task; Require(task.FreezeArchive(*archive.value).Succeeded(), "save task freeze");
    task.target = root / "test.w3x"; task.script.emplace(); task.script->luaTestOptions.testHotReload = true;
    Services::MapSaveService::Run(task);
    Require(!task.succeeded && !std::filesystem::exists(task.target)
        && std::any_of(task.diagnostics.begin(), task.diagnostics.end(), [](const auto& d) { return d.code == "LUA_TEST_SAVE_REQUIRED"; }),
        "unmarked formal save allowed test layer");
}
void Check(const std::filesystem::path& root) {
    Fixture fixture(root);
    const auto saved = fixture.Generate(false);
    const auto defaultSaved = GenerateLuaTriggerUnit(fixture.document, fixture.catalog, fixture.target);
    Require(defaultSaved.Succeeded() && saved.text == defaultSaved.text && saved.scriptSha256 == defaultSaved.scriptSha256
        && saved.text.find("__forge_test_") == std::string::npos && saved.testTriggers.empty(), "Default/formal save changed");
    auto baseline = fixture.Generate(); Host host;
    host.Run(baseline.text); host.Run("InitGlobals(); InitCustomTriggers(); udg_Existing=777; ConditionalTriggerExecute(gg_trg_F42); assert(recorded==1)");
    Require(Prepare(baseline, baseline).source.empty(), "No changes generated an executable patch");
    for (unsigned i = 1; i <= 20; ++i) {
        fixture.document.argumentNodes[fixture.amount].value.text = std::to_string(i + 1);
        auto next = fixture.Generate(); const auto patch = Prepare(baseline, next);
        Require(patch.changes.size() == 1 && patch.changes[0].kind == LuaTestReloadChangeKind::Functions,
            "Action update recreated a trigger");
        host.Run("held=gg_trg_F42"); host.Run(patch.source);
        host.Run("ConditionalTriggerExecute(gg_trg_F42); assert(held==gg_trg_F42 and activeTriggers==1 and activeConditions==1 and recorded=="
            + std::to_string(i + 1) + " and udg_Existing==777)");
        baseline = std::move(next); // 模拟已经收到对应成功 ACK；不把此 fixture 当成游戏证据。
    }
    fixture.document.argumentNodes[fixture.condition].value.text = "false";
    auto next = fixture.Generate(); auto patch = Prepare(baseline, next);
    host.Run(patch.source); host.Run("recorded=0; ConditionalTriggerExecute(gg_trg_F42); assert(recorded==0)"); baseline = std::move(next);
    fixture.document.argumentNodes[fixture.period].value.text = "2.0";
    next = fixture.Generate(); patch = Prepare(baseline, next);
    Require(patch.changes[0].kind == LuaTestReloadChangeKind::Recreate, "Changed event was not recreated");
    host.Run("held=gg_trg_F42"); host.Run(patch.source);
    host.Run("assert(held.destroyed and held~=gg_trg_F42 and gg_trg_F42.period==2 and activeTriggers==1 and activeConditions==1)"); baseline = std::move(next);
    auto extra = fixture.document.triggers[0]; extra.id = 43; extra.name.text = "Added"; fixture.document.triggers.push_back(extra);
    next = fixture.Generate(); patch = Prepare(baseline, next); host.Run(patch.source);
    host.Run("assert(activeTriggers==2 and activeConditions==2 and gg_trg_F43~=nil)"); baseline = std::move(next);
    fixture.document.triggers.erase(fixture.document.triggers.begin());
    next = fixture.Generate(); patch = Prepare(baseline, next); host.Run(patch.source);
    host.Run("assert(activeTriggers==1 and activeConditions==1 and gg_trg_F42==nil)"); baseline = std::move(next);
    auto variable = fixture.document.variables[0]; variable.id = 502; variable.name.text = "Added"; variable.value.text = "99";
    fixture.document.variables.push_back(variable); next = fixture.Generate(); patch = Prepare(baseline, next); host.Run(patch.source);
    host.Run("assert(udg_Added==99 and udg_Existing==777)"); baseline = std::move(next);
    next = fixture.Generate();
    Require(!PrepareLuaTestReload(baseline, next, "before", "after").Succeeded(), "Non-script map changes passed");
    Require(!PrepareLuaTestReload(saved, next, "same", "same").Succeeded(), "Formal map allowed hot reload");
    auto corrupt = next; corrupt.testTriggers[0].functionsOffset = next.text.size();
    Require(!PrepareLuaTestReload(baseline, corrupt, "same", "same").Succeeded(), "Unbound generated fragment passed");
    fixture.document.variables[0].typeName.text = "real"; next = fixture.Generate();
    Require(!PrepareLuaTestReload(baseline, next, "same", "same").Succeeded(), "Existing variable type change passed");
    fixture.document.variables[0].typeName.text = "integer"; fixture.document.triggers[0].runOnInit = 1; next = fixture.Generate();
    Require(!PrepareLuaTestReload(baseline, next, "same", "same").Succeeded(), "Initialization replay passed");
    fixture.document.triggers[0].runOnInit = 0; fixture.document.argumentNodes[fixture.condition].value.text = "true";
    fixture.document.argumentNodes[fixture.amount].value.text = "999";
    next = fixture.Generate(); patch = Prepare(baseline, next); host.Run(patch.source);
    bool runtimeFailed = false;
    try { host.Run("ConditionalTriggerExecute(gg_trg_F43)"); } catch (const std::exception& error) {
        runtimeFailed = std::string_view(error.what()).find("original-runtime-error") != std::string_view::npos;
    }
    Require(runtimeFailed, "Actual callback runtime error disappeared");
    Lua::SyntaxOptions tiny; tiny.maxSourceBytes = 1;
    Require(!PrepareLuaTestReload(baseline, next, "same", "same", tiny).Succeeded(), "Syntax compile budget ignored");
}
}
int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / ("audastra-reload-fixture-" + std::to_string(GetCurrentProcessId()));
        Check(root); Controller(root / "controller"); MapFacts(root); std::filesystem::remove_all(root);
        std::cout << "Lua test generation/diff and 20 original offline host updates passed; game evidence=0\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
