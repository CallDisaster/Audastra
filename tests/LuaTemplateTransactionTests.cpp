// YDWE Lua 模板生产适配器回归。需要本机 YDWE：设置 FORGE_YDWE_ROOT；未设置时跳过（返回 77）。
// 只使用原创合成地图，在私有临时目录运行。
#include "War3MapCore/Script/LuaTemplateTransaction.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Script;
namespace {
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string Codes(const auto& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ": " + diagnostic.message + "\n";
    return text;
}
bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}
std::vector<std::byte> Bytes(std::string_view value) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    return {begin, begin + value.size()};
}
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("forge-lua-template-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Scratch() { std::filesystem::create_directory(path); path = std::filesystem::canonical(path); }
    ~Scratch() {
        std::error_code error;
        if (path.filename().wstring().starts_with(L"forge-lua-template-")) std::filesystem::remove_all(path, error);
    }
};

struct Fixture {
    LuaTemplateToolchain toolchain;
    Scratch scratch;
    int runs = 0;

    Result<LuaTemplateOutput> Run(const std::vector<MapArchiveMember>& members, std::string_view script) {
        const auto file = scratch.path / ("map" + std::to_string(++runs) + ".w3x");
        Require(WriteVerifiedArchive(file, members).Succeeded(), "write synthetic map");
        auto archive = MapArchiveSession::Open(file);
        Require(archive.Succeeded(), "open synthetic map");
        auto sources = SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 0);
        Require(sources.Succeeded(), "capture sources");
        const auto work = scratch.path / ("work" + std::to_string(runs));
        std::filesystem::create_directory(work);
        return RunLuaTemplateTransaction(script, *sources.value, toolchain, work);
    }
};

std::vector<MapArchiveMember> BaseMap() {
    return {{"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")}};
}

void TestExpressionsAndHash(Fixture& fixture) {
    auto result = fixture.Run(BaseMap(), "a <?='x'..1?> b <?=StringHash('abc/Def')?> c\n");
    Require(result.Succeeded(), "plain template should run:\n" + Codes(result));
    char expected[16];
    std::snprintf(expected, sizeof(expected), "0x%08X", StormStringHash("abc/Def"));
    Require(result.value->script == std::string("a x1 b ") + expected + " c\n", "expanded script: " + result.value->script);
    Require(result.value->objectMembers.empty(), "no slk use means no object output");
}

void TestCreationAndRoundTrip(Fixture& fixture) {
    const std::string script =
        "<?\nlocal slk = require 'slk'\nlocal o = slk.ability.AHhb:new('A9Z1')\no.Name = 'forge test'\n?>"
        "id=<?=abilityOrder(\"'A9Z1'\", 'Order')?>\n";
    auto first = fixture.Run(BaseMap(), script);
    Require(first.Succeeded(), "creating template should run:\n" + Codes(first));
    Require(first.value->objectMembers.size() == 1 && first.value->objectMembers[0].name == "war3map.w3a",
        "only the ability table is written");
    const ObjectData::NativeObjectCodec codec(ObjectData::ObjectType::Ability);
    auto parsed = codec.Parse(first.value->objectMembers[0].data);
    Require(parsed.Succeeded(), "generated w3a parses with the native codec");
    const auto* native = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get());
    Require(native && native->Version() == 2, "generated w3a is classic v2");
    Require(first.value->script.find("id=") != std::string::npos && first.value->script.find("<?") == std::string::npos,
        "template tags consumed");

    // 把生成结果写回地图再执行同一模板：YDWE 语义下不应再有任何物编写出，脚本相同。
    auto next = BaseMap();
    next.push_back(first.value->objectMembers[0]);
    auto second = fixture.Run(next, script);
    Require(second.Succeeded(), "second pass should run:\n" + Codes(second));
    Require(second.value->objectMembers.empty(), "unchanged generated objects produce no writes");
    Require(second.value->script == first.value->script, "second pass script identical");
}

void TestFailuresPublishNothing(Fixture& fixture) {
    auto failed = fixture.Run(BaseMap(), "<? error('boom') ?>");
    Require(!failed.Succeeded() && !failed.value, "template error fails without output");
    auto denied = fixture.Run(BaseMap(), "<? os.execute('calc') ?>");
    Require(!denied.Succeeded() && HasCode(denied, "TEMPLATE_CAPABILITY_DENIED"), "os access denied:\n" + Codes(denied));
    auto unknownField = fixture.Run(BaseMap(),
        "<? local o = require('slk').ability.AHhb:new('A9Z2'); o.NoSuchField = 1 ?>");
    Require(!unknownField.Succeeded() && !unknownField.value, "unknown field fails:\n" + Codes(unknownField));
    auto endless = fixture.Run(BaseMap(), "<? while true do end ?>");
    Require(!endless.Succeeded() && HasCode(endless, "TEMPLATE_LIMIT"), "endless loop hits the budget:\n" + Codes(endless));
}
void TestReadOnlyMiscAndLog(Fixture& fixture) {
    auto map = BaseMap();
    map.push_back({"war3mapMisc.txt", Bytes("[Misc]\r\nDefenseArmor=0.09\r\n")});
    const auto result = fixture.Run(map,
        "<? local x=tonumber(require('slk').misc.Misc.DefenseArmor); assert(x==0.09) ?>"
        "<?=math.log(1+tonumber(require('slk').misc.Misc.DefenseArmor))/math.log(2)?>");
    Require(result.Succeeded(), "misc reads the captured map override:\n" + Codes(result));
    const double value = std::stod(result.value->script);
    Require(value > 0.12 && value < 0.13 && result.value->objectMembers.empty(),
        "finite positive log expression produces no native writes");
    const auto denied = fixture.Run(map, "<? require('slk').misc.Misc.DefenseArmor=0.5 ?>");
    Require(!denied.value && HasCode(denied, "TEMPLATE_CAPABILITY_DENIED"), "misc write is denied:\n" + Codes(denied));
    for (const auto expression : {"<?=math.log(-1)?>", "<?=math.log(2,1)?>"}) {
        const auto invalid = fixture.Run(BaseMap(), expression);
        Require(!invalid.value && HasCode(invalid, "TEMPLATE_VALUE_UNSUPPORTED"), "invalid logarithm is refused:\n" + Codes(invalid));
    }
}
} // namespace

int main() {
    try {
        wchar_t root[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", root, 1024);
        if (length == 0 || length >= 1024) {
            std::cout << "SKIP: FORGE_YDWE_ROOT not set\n";
            return 77;
        }
        auto toolchain = ResolveLuaTemplateToolchain(std::filesystem::path(root));
        if (!toolchain.Succeeded()) {
            std::cout << "SKIP: YDWE template toolchain unavailable\n" << Codes(toolchain);
            return 77;
        }
        Fixture fixture{*toolchain.value};
        TestExpressionsAndHash(fixture);
        TestCreationAndRoundTrip(fixture);
        TestFailuresPublishNothing(fixture);
        TestReadOnlyMiscAndLog(fixture);
        std::cout << "lua template transaction tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
