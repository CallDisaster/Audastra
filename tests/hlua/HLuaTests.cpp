#include "War3MapCore/HLua/Hslk.h"
#include "War3MapCore/HLua/IdAllocator.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include "War3MapCore/HLua/Test.h"
#include "War3MapCore/HLua/ProjectConfiguration.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "../../src/HLua/BuildInternal.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

void TestHLuaObjectMapping();
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
    Need(!ids.Allocate("item", "ratf", "I[00").Succeeded(), "section syntax is not a rawcode");
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
    const auto external = Lua::ScanExternalCapabilities("-- io.open('ignored')\nlocal s='os.execute()'; os['execute']('x'); package.loadlib('x'); io.open('x')");
    Need(external.size() == 3 && external[0].line == 2, "external capability evidence ignores comments and strings");
    Need(Lua::ScanLiteralCallArguments("-- helper.exec('ignored')\nhelper.exec('FixtureNative')","helper","exec").size()==1,"literal call evidence");
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
        "assert(math.type(1)=='integer' and math.type(1.0)=='float' and tostring(1.0)=='1' and tostring(1/2)=='0.5')\n"
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
    project.source.files.clear(); File(project.source,"hslk/spec.lua","hslk_unit{HP=9007199254740993}");
    Need(!EvaluateHslk(framework,project,{}).Succeeded(),"integer is not silently rounded to double");
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
void TestSourceAudit() {
    const auto root=std::filesystem::temp_directory_path()/
        ("audastra-hlua-audit-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    Need(std::filesystem::create_directory(root),"exclusive original fixture directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);} } cleanup{root};
    const auto bytes=[](std::string_view text){return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data()+text.size()));};
    for(const auto unsafe:{false,true}) {
        const auto map=root/(unsafe ? "unsafe.w3x" : "safe.w3x");
        const std::vector<Mpq::MpqBuildFile> files{{"main.lua",bytes("return require 'lib.helper'")},
            {"lib/helper.lua",bytes(unsafe ? "local x=os['execute']; return x" : "-- io.open('comment')\nreturn 'os.execute()'")}};
        Good(Mpq::WriteMpqArchive(map,files)); auto sha=Sha256File(map); Good(sha);
        for(const auto engine:{Services::MapTestLuaEngine::Owned,Services::MapTestLuaEngine::Ydwe}) {
            const auto result=ValidateTestEngineSources(map,*sha.value,engine);
            Need(result.Succeeded()!=unsafe,"all requested engines pass the same source gate before a possible engine fallback");
            Need(!result.diagnostics.empty() && result.diagnostics.front().code==
                (unsafe ? "HLUA_TEST_EXTERNAL_CAPABILITY" : "HLUA_TEST_SOURCE_AUDIT"),"source audit is reviewable in receipt");
        }
        Need(!ValidateTestEngineSources(map,std::string(64,'0'),Services::MapTestLuaEngine::Owned).Succeeded(),"stale source fails before engine selection");
    }
}
void ExtraFrameworkModules() {
    auto framework=FixtureFramework();
    File(framework.runtime,"builtIn/console/test.lua","return {}\n");
    File(framework.runtime,"user-helper/extra.lua","return {original=true}\n");
    ProjectSnapshot project;
    File(project.source,"main.lua","require('user-helper.extra')\nfunction main() end\n");
    project.runtimeFiles={0};
    HslkOutput output; output.injectedSlk="local ids={}\n";
    auto members=Detail::RuntimeMembers(framework,project,output,BuildTarget::Test); Good(members);
    const auto found=std::find_if(members.value->begin(),members.value->end(),[](const auto& member){return member.name=="user-helper/extra.lua";});
    Need(found!=members.value->end() && std::string(reinterpret_cast<const char*>(found->data.data()),found->data.size())=="return {original=true}\n",
        "framework extra modules are packed verbatim instead of silently dropping unfamiliar directories");
    File(project.source,"user-helper/extra.lua","return 'collision'\n");project.runtimeFiles.push_back(1);
    Need(!Detail::RuntimeMembers(framework,project,output,BuildTarget::Test).Succeeded(),"extra framework/project collision is still refused");
}
}
void ConfigurationAndReload() {
    const auto root = std::filesystem::temp_directory_path() /
        ("audastra-hlua-config-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Need(std::filesystem::create_directory(root), "unique configuration fixture");
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);} } cleanup{root};
    auto config = ReadProjectConfiguration(root); Good(config); Need(!config.value->recognized, "ordinary Lua folder");
    std::ofstream(root / "main.lua") << "function main() end";
    std::filesystem::create_directory(root / "hslk");
    config = ReadProjectConfiguration(root); Good(config); Need(config.value->recognized && config.value->sha256.empty(), "h-lua recognized without config");
    Need(!ResolveProjectFramework(root).Succeeded(), "missing framework reported");
    FrameworkLocation application{root / "app",{},{}}, explicitLocation{root / "explicit",{}, {}};
    auto resolved = ResolveProjectFramework(root,{},application); Good(resolved); Need(resolved.value->repository==application.repository,"application default");
    resolved=ResolveProjectFramework(root,explicitLocation,application);Good(resolved);Need(resolved.value->repository==explicitLocation.repository,"request precedence");
    Good(ConfigureProjectFramework(root,{},""));
    Need(!VerifyProjectConfiguration(root,*config.value).Succeeded(),"new sidecar invalidates in-flight operation");
    Need(!ConfigureProjectFramework(root,{},"").Succeeded(),"configuration CAS prevents overwrite");
    config=ReadProjectConfiguration(root);Good(config);Good(ConfigureProjectFramework(root,{},config.value->sha256));
    std::ofstream(root/".audastra-hlua.json",std::ios::trunc)<<"{\"schema\":1,\"schema\":1,\"framework\":\"\"}";
    Need(!ReadProjectConfiguration(root).Succeeded(),"duplicate config key refused");
    Need(!ResolveProjectFramework(root,explicitLocation).Succeeded(),"bad sidecar not bypassed by explicit path");
    std::ofstream(root/".audastra-hlua.json",std::ios::trunc)<<"{\"schema\":true,\"framework\":\"\"}";
    Need(!ReadProjectConfiguration(root).Succeeded(),"boolean schema refused");
    ProjectSnapshot project;
    File(project.source,"main.lua","function main() end"); File(project.source,"scripts/hello.lua","print('one')");
    File(project.source,"hslk/unit.lua","hslk_unit{Name='one'}");project.runtimeFiles={0,1};project.hslkFiles={2};
    auto initial=PrepareReload(project,"");Good(initial);Need(initial.value->rebuildRequired && initial.value->authorModules.empty(),"no build baseline refuses reload");
    auto reload=PrepareReload(project,initial.value->hslkSha256);Good(reload);Need(!reload.value->rebuildRequired && reload.value->authorModules.size()==2,"author modules only");
    project.source.files.pop_back();File(project.source,"hslk/unit.lua","hslk_unit{Name='two'}");
    reload=PrepareReload(project,initial.value->hslkSha256);Good(reload);Need(reload.value->rebuildRequired && reload.value->authorModules.empty(),"hslk change sends nothing");
    project.runtimeFiles={0,2};Need(!PrepareReload(project,reload.value->hslkSha256).Succeeded(),"overlap refused");
    const auto bytes=[](std::string_view text){return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),reinterpret_cast<const std::byte*>(text.data()+text.size()));};
    const auto oldMap=root/"existing.w3x",candidate=root/"candidate.w3x";
    const std::vector<Mpq::MpqBuildFile> oldFiles{{"war3map.j",bytes("old")},{"obsolete.txt",bytes("old member")}};
    const std::vector<Mpq::MpqBuildFile> newFiles{{"war3map.j",bytes("new")},{"fresh.txt",bytes("new member")}};
    Good(Mpq::WriteMpqArchive(oldMap,oldFiles));
    Good(Mpq::WriteMpqArchive(candidate,newFiles));
    const auto oldSha=Sha256File(oldMap);Good(oldSha);
    Need(!PublishBuildOutput(candidate,oldMap,std::string(64,'0')).Succeeded(),"stale target not replaced");
    auto unchanged=Sha256File(oldMap);Good(unchanged);Need(*unchanged.value==*oldSha.value,"stale refusal is atomic");
    Good(PublishBuildOutput(candidate,oldMap,*oldSha.value));
    const auto saved=MapArchiveSession::Open(oldMap);Good(saved);
    Need(saved.value->HasMember("fresh.txt") && !saved.value->HasMember("obsolete.txt"),"whole verified candidate published with removals");
    const auto text=saved.value->ReadMember("war3map.j");Good(text);Need(*text.value==bytes("new"),"replacement content readback");
    Need(std::filesystem::is_directory(root/"War3MapEditor Backups"),"replacement uses normal backup service");
    const auto marker=EncodeProjectBuildState({std::string(64,'a'),std::string(64,'b'),""});Good(marker);
    const auto marked=root/"marked.w3x";
    const std::vector<Mpq::MpqBuildFile> metadata{{std::string(kBuildStateMember),*marker.value}};
    Good(Mpq::WriteMpqArchive(marked,metadata));auto map=MapArchiveSession::Open(marked);Good(map);
    auto state=ReadProjectBuildState(*map.value);Good(state);Need(state.value->has_value() && (**state.value).projectSha256==std::string(64,'a'),"build state strict roundtrip");
    Need(!EncodeProjectBuildState({"bad",std::string(64,'b'),""}).Succeeded(),"bad build identity refused");
    const auto corrupt=root/"marker-corrupt.w3x";
    const std::vector<Mpq::MpqBuildFile> badMetadata{{std::string(kBuildStateMember),bytes("{\"schema\":true}")}};
    Good(Mpq::WriteMpqArchive(corrupt,badMetadata));auto badMap=MapArchiveSession::Open(corrupt);Good(badMap);
    Need(!ReadProjectBuildState(*badMap.value).Succeeded(),"corrupt marker never means synchronized");
}
int main() {
    try { Ids(); Lexical(); Sandbox(); TestHLuaObjectMapping(); TestSourceAudit(); ExtraFrameworkModules(); ConfigurationAndReload(); std::cout << "HLua core fixtures passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
