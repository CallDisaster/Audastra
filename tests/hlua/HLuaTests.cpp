#include "War3MapCore/HLua/Hslk.h"
#include "War3MapCore/HLua/IdAllocator.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::HLua;
void Need(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
template<class T> void Good(const T& result) {
    if (result.Succeeded()) return;
    std::string message;
    for (const auto& d : result.diagnostics) message += d.code + ": " + d.message + "\n";
    throw std::runtime_error(message);
}
void File(Services::LuaFolderSnapshot& snapshot, std::string path, std::string_view text) {
    auto bytes = std::make_shared<const std::vector<std::byte>>(
        reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size()));
    auto hash = Sha256(*bytes); Good(hash);
    snapshot.files.push_back({std::move(path), *hash.value, std::move(bytes)});
}
void Ids() {
    const std::vector<std::string> used{"v000", "K000", "I000"};
    IdAllocator ids(used);
    auto unit = ids.Allocate("unit", "hfoo"); Good(unit); Need(*unit.value == "v001", "lowercase collision");
    auto hero = ids.Allocate("unit", "Hpal"); Good(hero); Need(*hero.value == "V002", "shared unit counter");
    auto item = ids.Allocate("item", "ratf"); Good(item); Need(*item.value == "I001", "item collision");
    Need(!ids.Allocate("item", "ratf", "1234").Succeeded(), "numeric first forced id");
    Need(!ids.Allocate("item", "ratf", "I001").Succeeded(), "forced conflict");
    Need(!ids.Allocate("item", "", "A001").Succeeded(), "parent required");
    IdAllocator full({});
    for (unsigned i = 0; i < 46656; ++i) Good(full.Allocate("ability", "AHbz"));
    Need(!full.Allocate("ability", "AHbz").Succeeded(), "overflow is error");
}
void Lexical() {
    const std::string source = "// call InitGlobals()\nfunction main takes nothing returns nothing\n"
        "call BJDebugMsg(\"call InitGlobals()\")\ncall /* gap */ InitGlobals ( ) // comment\nendfunction\n";
    auto first = InsertLuaEntry(source); Good(first);
    Need(first.value->find("exec-lua: \\\"h-lua-cli\\\"") != std::string::npos, "entry string escaping");
    auto second = InsertLuaEntry(*first.value); Good(second); Need(*first.value == *second.value, "idempotent insertion");
    Need(!InsertLuaEntry("// call InitGlobals()\n").Succeeded(), "comment not entry");
    Need(!InsertLuaEntry("/* unclosed").Succeeded(), "malformed input refused");
    auto multiple = InsertLuaEntry("call InitGlobals()\ncall InitGlobals()\n"); Good(multiple);
    auto again = InsertLuaEntry(*multiple.value); Good(again); Need(*again.value == *multiple.value, "all actual calls");
    const auto modules = Lua::ScanModuleRequires("-- require 'bad'\nlocal x=require('jass.slk'); require(dynamic); t.require('skip')");
    Need(modules.size() == 2 && modules[0].name == "jass.slk" && modules[1].dynamic, "require lexical evidence");
    auto sites = Lua::FindEmptyTableInitializers("-- ids={}\nlocal ids = { }\nprint('ids={}')", "ids");
    Need(sites.size() == 1, "only initializer code");
}
FrameworkSnapshot FixtureFramework() {
    FrameworkSnapshot framework;
    for (auto name : {"builtIn/console/go.lua", "slk/pilot.lua", "slk/system.lua", "slk/setter.lua",
        "const/abilityTarget.lua", "const/attribute.lua", "const/cache.lua", "const/damageSource.lua",
        "const/event.lua", "const/hero.lua", "const/hotKey.lua", "const/item.lua", "const/monitor.lua",
        "const/target.lua", "const/unit.lua", "const/orderStr.lua", "foundation/color.lua", "foundation/array.lua",
        "foundation/math.lua", "foundation/string.lua", "foundation/mbstring.lua", "foundation/table.lua",
        "foundation/description.lua"}) File(framework.runtime, name, "");
    // 原创协议夹具：只实现测试所需的宿主接口，没有框架默认值和系统物体。
    File(framework.runtime, "slk/slk.lua", "local hslk_cli_ids = {}\nHSLK_CLI_H_IDS = {}\nHSLK_CLI_H_IDI=1\n"
        "function hslk_unit(t) assert(HSLK_CLI_H_IDS[HSLK_CLI_H_IDI]); HSLK_CLI_H_IDI=HSLK_CLI_H_IDI+1 end\n"
        "hslk_item=hslk_unit\n");
    for (auto name : {"json.lua", "string.lua", "table.lua", "setter.lua", "system.lua"}) File(framework.build, name, "");
    File(framework.build, "go.lua", "SLK_GO={}\nfunction SLK_GO_INI(text) assert(type(text)=='string') end\n");
    File(framework.build, "slk.lua", "function hslk_unit(t) t._class='unit'; t._parent=t._parent or 'hfoo'; "
        "t._id=SLK_ID(t); SLK_GO[#SLK_GO+1]=t; return t end\n"
        "function hslk_item(t) t._class='item'; t._parent=t._parent or 'ratf'; "
        "t._id=SLK_ID(t); SLK_GO[#SLK_GO+1]=t; return t end\n");
    return framework;
}
void Sandbox() {
    auto framework = FixtureFramework();
    ProjectSnapshot project;
    File(project.source, "hslk/spec.lua", "assert(io==nil and os==nil and package==nil and load==nil)\n"
        "hslk_unit{Name='Test unit',HP=0.13799999999999998,Art='path\\\\file',Buttonpos_1=0}\n"
        "hslk_item{Name='Test item',Cool={0.125,0.001}}\n");
    project.hslkFiles = {0};
    auto result = EvaluateHslk(framework, project, {}); Good(result);
    Need(result.value->objects.size() == 2 && result.value->replayPassed, "build and replay");
    Need(result.value->objects[0].id == "v000" && result.value->objects[1].id == "I000", "id order");
    const auto& fields = result.value->objects[0].fields;
    const auto hp = std::find_if(fields.begin(), fields.end(), [](const auto& f) { return f.name == "HP"; });
    Need(hp != fields.end() && std::get<double>(hp->values.front()) == 0.13799999999999998, "no decimal truncation");
    project.source.files.clear(); File(project.source, "hslk/spec.lua", "hslk_unit{Name=true}");
    Need(!EvaluateHslk(framework, project, {}).Succeeded(), "boolean refused");
    project.source.files.clear(); File(project.source, "hslk/spec.lua", "hslk_unit{Name={{'nested'}}}");
    Need(!EvaluateHslk(framework, project, {}).Succeeded(), "nested array refused");
    project.source.files.clear(); File(project.source, "hslk/spec.lua", "while true do end");
    HslkLimits limits; limits.instructions = 5000;
    Need(!EvaluateHslk(framework, project, {}, limits).Succeeded(), "instruction budget");
    project.source.files.clear(); File(project.source, "hslk/spec.lua", "hslk_unit{Name='one'}");
    framework.runtime.files.back().bytes.reset(); // 被替换前不借用失效文件。
    framework.runtime.files.pop_back();
    File(framework.runtime, "slk/slk.lua", "local hslk_cli_ids={}\nHSLK_CLI_H_IDS={}\nHSLK_CLI_H_IDI=1\nfunction hslk_unit(t) end");
    Need(!EvaluateHslk(framework, project, {}).Succeeded(), "replay count mismatch");
}
}
int main() {
    try { Ids(); Lexical(); Sandbox(); std::cout << "HLua core fixtures passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
