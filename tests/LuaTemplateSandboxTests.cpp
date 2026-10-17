// 进程内 Lua 模板沙盒回归。语法/沙盒/import 部分无需外部数据；
// slk 部分只读 FORGE_YDWE_ROOT 下的 w3x2lni 物编数据（未设置时只跳过这一部分）。不运行任何外部程序。
#include "War3MapCore/Script/LuaTemplateSandbox.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
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
bool Mentions(const auto& result, std::string_view text) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [text](const Diagnostic& diagnostic) { return diagnostic.message.find(text) != std::string::npos; });
}
std::vector<std::byte> Bytes(std::string_view value) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    return {begin, begin + value.size()};
}
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("audastra-lua-template-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Scratch() { std::filesystem::create_directory(path); path = std::filesystem::canonical(path); }
    ~Scratch() {
        std::error_code error;
        if (path.filename().wstring().starts_with(L"audastra-lua-template-")) std::filesystem::remove_all(path, error);
    }
};

Result<TemplateTransactionOutput> Run(std::string_view script, LuaTemplateRequest request = {}) {
    request.preprocessed = script;
    return RunLuaTemplate(request);
}

void TestExpressionsAndControl() {
    auto plain = Run("a <?='x'..1?> b <?=StringHash('abc/Def')?> c<?= ?>\n");
    Require(plain.Succeeded(), "plain template should run:\n" + Codes(plain));
    char hash[16];
    std::snprintf(hash, sizeof(hash), "0x%08X", StormStringHash("abc/Def"));
    Require(plain.value->script == std::string("a x1 b ") + hash + " c\n", "expanded script: " + plain.value->script);
    Require(plain.value->objectMembers.empty(), "no slk use means no object output");

    // YDWE 模板运行在 Lua 5.1 的赋值上下文：多返回值只保留第一个，
    // 整数值的浮点按 "%.14g" 写成整数文本（例如 math.modf 的 32.0 写成 32）。
    auto multi = Run("a<?= ('x'):gsub('x', 'y') ?>b");
    Require(multi.Succeeded() && multi.value->script == "ayb",
        "multi-value expression keeps first: " + (multi.value ? multi.value->script : Codes(multi)));
    auto integral = Run("a<?= math.modf(8192 / 256) ?>b");
    Require(integral.Succeeded() && integral.value->script == "a32b",
        "integral float formats as integer: " + (integral.value ? integral.value->script : Codes(integral)));

    // 控制语句、跨块局部变量、浮点与字符串里的模板；块外文本含引号/反斜杠/CRLF/中文逐字节保留。
    const std::string control = "x\r\n<? local n = 0 for i = 1, 3 do n = n + i ?>v<?= i ?> <? end ?>\r\n"
        "set s = \"<?= 1+2 ?>\\\\\"中文\" r=<?= 1.5 ?> <? if n > 5 then ?>big<? else ?>small<? end ?>";
    auto controlled = Run(control);
    Require(controlled.Succeeded(), "control flow:\n" + Codes(controlled));
    Require(controlled.value->script == "x\r\nv1 v2 v3 \r\nset s = \"3\\\\\"中文\" r=1.5 big",
        "control flow output: " + controlled.value->script);

    auto located = Run("line1\nline2\n<? local a = nil\n\n local b = a.x ?>");
    Require(!located.Succeeded() && Mentions(located, "第 5 行"), "runtime error reports the script line:\n" + Codes(located));
    auto syntax = Run("a\n<? if then ?>");
    Require(!syntax.Succeeded() && HasCode(syntax, "TEMPLATE_SYNTAX") && Mentions(syntax, "第 2 行"),
        "syntax error reports the script line:\n" + Codes(syntax));
    auto unterminated = Run("a <? local x = 1");
    Require(!unterminated.Succeeded() && HasCode(unterminated, "TEMPLATE_UNTERMINATED"), "unterminated tag:\n" + Codes(unterminated));
}

void TestSandbox() {
    for (const auto* code : {"<? os.execute('calc') ?>", "<? io.open('x') ?>", "<? load('return 1')() ?>",
             "<? require('lfs') ?>", "<? dofile('a.lua') ?>", "<? string.dump(print) ?>", "<?= package.path ?>"}) {
        auto denied = Run(code);
        Require(!denied.Succeeded() && !denied.value, std::string("sandbox must deny: ") + code + "\n" + Codes(denied));
    }
    LuaTemplateRequest small;
    small.limits.instructions = 2'000'000;
    auto endless = Run("<? while true do end ?>", small);
    Require(!endless.Succeeded() && HasCode(endless, "TEMPLATE_LIMIT"), "endless loop hits the budget:\n" + Codes(endless));
    LuaTemplateRequest tight;
    tight.limits.memoryBytes = 8 * 1024 * 1024;
    auto memory = Run("<? local t = {} for i = 1, 1e8 do t[i] = ('x'):rep(100) .. i end ?>", tight);
    Require(!memory.Succeeded() && HasCode(memory, "TEMPLATE_LIMIT"), "memory budget:\n" + Codes(memory));
    for (const auto* code : {"<?= math.log(-1) ?>", "<?= 1/0 ?>", "<?= true ?>", "<?= {} ?>", "<?= 1, 2 ?>"}) {
        auto refused = Run(code);
        Require(!refused.Succeeded(), std::string("unprintable value refused: ") + code);
    }
    auto printed = Run("<? print('hello', 1) ?>ok");
    Require(printed.Succeeded() && printed.value->script == "ok" && !printed.value->toolMessages.empty()
        && printed.value->toolMessages.front() == "print: hello\t1", "print goes to tool messages");
    auto order = Run("<?= abilityOrder(\"'AHtb'\", 'Order') ?>");
    Require(order.Succeeded() && order.value->script == "0" && HasCode(*order.value, "ABILITY_ORDER_UNKNOWN"),
        "unknown order is 0 with a warning:\n" + Codes(order));
    auto noSlk = Run("<? require 'slk' ?>");
    Require(!noSlk.Succeeded(), "slk without data is refused");
}

void TestImport() {
    Scratch scratch;
    { std::ofstream(scratch.path / "data.txt", std::ios::binary) << "file-bytes"; }
    LuaTemplateRequest request;
    request.importRoot = scratch.path;
    auto imported = Run("<? import('a\\\\b.txt')('text') import('c.txt')('data.txt', true) ?>done", request);
    Require(imported.Succeeded() && imported.value->imports.size() == 2, "imports:\n" + Codes(imported));
    Require(imported.value->imports[0].name == "a\\b.txt" && imported.value->imports[0].data == Bytes("text")
        && imported.value->imports[1].data == Bytes("file-bytes"), "import contents");
    auto escape = Run("<? import('x.txt')('../secret', true) ?>", request);
    Require(!escape.Succeeded(), "import path cannot leave the map directory");
    auto reserved = Run("<? import('war3map.j')('x') ?>", request);
    Require(!reserved.Succeeded(), "import cannot overwrite the map script");
}

struct SlkFixture {
    std::filesystem::path root;
    Scratch scratch;
    int runs = 0;

    Result<TemplateTransactionOutput> Run(const std::vector<MapArchiveMember>& members, std::string_view script) {
        const auto file = scratch.path / ("map" + std::to_string(++runs) + ".w3x");
        Require(WriteVerifiedArchive(file, members).Succeeded(), "write synthetic map");
        auto archive = MapArchiveSession::Open(file);
        Require(archive.Succeeded(), "open synthetic map");
        auto sources = SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 0);
        Require(sources.Succeeded(), "capture sources");
        LuaTemplateRequest request;
        request.preprocessed = script;
        request.slk = [&]() { return Result<LuaTemplateSlkSources>::Success({root, &*sources.value}); };
        return RunLuaTemplate(request);
    }
};

// 原创经典 v25 W3I 夹具：只用于让 slk 选择 prebuilt 数据集。
std::vector<MapArchiveMember> BaseMap() {
    return {{"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
}

const ObjectData::ObjectRecord* FindGenerated(const MemberReplacement& member, std::string_view id,
    std::unique_ptr<CodecDocument>& holder) {
    auto parsed = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Ability).Parse(member.data);
    Require(parsed.Succeeded(), "generated w3a parses");
    holder = std::move(parsed.document);
    const auto* native = dynamic_cast<const ObjectData::NativeObjectDocument*>(holder.get());
    Require(native && native->Version() == 2, "generated w3a is classic v2");
    return native->FindObject({ObjectData::ObjectType::Ability, std::string(id)});
}

void TestSlk(SlkFixture& fixture) {
    // 读取标准值：等级字段、列表字段、代码、misc。
    auto read = fixture.Run(BaseMap(),
        "<? local slk = require 'slk' ?><?= slk.ability.AHtb.DataA1 ?>|<?= slk.ability.AHtb.Dur3 ?>|"
        "<?= slk.ability.AHtb.code ?>|<?= slk.ability.AHtb.Buttonpos ?>|<?= slk.item.afac.goldcost ?>|"
        "<?= tonumber(slk.misc.Misc.DefenseArmor) ?>|<?= slk.ability.XXXX.Name ?>|");
    Require(read.Succeeded(), "slk reads:\n" + Codes(read));
    Require(read.value->script.starts_with("100|5|AHtb|0,2|") && read.value->script.ends_with("|0.06||"),
        "slk read values: " + read.value->script);
    Require(read.value->objectMembers.empty(), "reading produces no writes");

    // 静态 ID 与动态标签生成；第二次在生成结果上执行同一模板不再写出。
    const std::string script =
        "<?\nlocal slk = require 'slk'\nlocal o = slk.ability.AHtb:new('A9Z1')\no.Name = 'forge test'\no.levels = 1\no.Dur = 10\n"
        "local d = slk.ability.AHtb:new('标签')\nd.Name = 'dyn'\n?>id=<?= d:get_id() ?>\n";
    auto first = fixture.Run(BaseMap(), script);
    Require(first.Succeeded(), "creating template should run:\n" + Codes(first));
    Require(first.value->objectMembers.size() == 1 && first.value->objectMembers[0].name == "war3map.w3a",
        "only the ability table is written");
    Require(first.value->script == "id=A000\n", "dynamic id: " + first.value->script);
    std::unique_ptr<CodecDocument> holder;
    const auto* created = FindGenerated(first.value->objectMembers[0], "A9Z1", holder);
    Require(created && created->parentRawcode == "AHtb", "static object created from AHtb");
    const auto has = [&](std::string_view field, const ObjectData::Value& value) {
        return std::any_of(created->modifications.begin(), created->modifications.end(), [&](const auto& mod) {
            return mod.key.fieldId == field && mod.value == value;
        });
    };
    Require(has("anam", std::string("forge test")) && has("alev", std::int32_t{1}) && has("W2lo", std::string("static"))
        && has("adur", 10.0f), "generated fields and W2lo marker");

    // 专有字段按技能代码取类型：AId0（代码 AIde）的 Idef 是整数，即使模板给的是 2^0 这样的浮点数。
    auto bonus = fixture.Run(BaseMap(), "<? local slk = require 'slk'\nlocal o = slk.ability.AId0:new('YDc0')\no.DataA1 = 2^0\n?>");
    Require(bonus.Succeeded() && bonus.value->objectMembers.size() == 1, "bonus template:\n" + Codes(bonus));
    std::unique_ptr<CodecDocument> bonusHolder;
    const auto* bonusObject = FindGenerated(bonus.value->objectMembers[0], "YDc0", bonusHolder);
    std::string bonusFields;
    if (bonusObject) {
        for (const auto& mod : bonusObject->modifications)
            bonusFields += mod.key.fieldId + "#" + std::to_string(mod.value.index()) + " ";
    }
    Require(bonusObject && std::any_of(bonusObject->modifications.begin(), bonusObject->modifications.end(),
        [](const auto& mod) { return mod.key.fieldId == "Idef" && mod.value == ObjectData::Value{std::int32_t{1}}; }),
        "AIde Idef is written as an integer: " + bonusFields);

    auto next = BaseMap();
    next.push_back(first.value->objectMembers[0]);
    auto second = fixture.Run(next, script);
    Require(second.Succeeded() && second.value->objectMembers.empty() && second.value->script == first.value->script,
        "unchanged generated objects produce no writes and keep dynamic ids:\n" + Codes(second));

    // 删掉模板代码：上次生成的对象被移除，用户对象保留。
    auto removed = fixture.Run(next, "<? local slk = require 'slk' ?>x");
    Require(removed.Succeeded() && removed.value->objectMembers.size() == 1, "removal rewrites the table:\n" + Codes(removed));
    Require(!FindGenerated(removed.value->objectMembers[0], "A9Z1", holder)
        && !FindGenerated(removed.value->objectMembers[0], "A000", holder), "generated objects removed");

    // 写入现有对象与不存在对象：不生效，但作为警告报告；misc 读取地图覆盖。
    auto map = BaseMap();
    map.push_back({"war3mapMisc.txt", Bytes("[Misc]\r\nDefenseArmor=0.09\r\n")});
    auto ignored = fixture.Run(map, "<? local slk = require 'slk'\nslk.ability.AHtb.Name = 'x'\nslk.item[{}].goldcost = 1\n?>"
        "<?= tonumber(slk.misc.Misc.DefenseArmor) ?>");
    Require(ignored.Succeeded() && ignored.value->script == "0.09" && ignored.value->objectMembers.empty()
        && HasCode(*ignored.value, "TEMPLATE_SLK_REPORT"), "ignored writes are reported:\n" + Codes(*ignored.value));

    // 数值类型错误不能静默丢弃。
    auto invalid = fixture.Run(BaseMap(), "<? local o = require('slk').ability.AHtb:new('A9Z3') o.Cool = 'abc' ?>");
    Require(!invalid.Succeeded() && !invalid.value, "invalid numeric value fails:\n" + Codes(invalid));
}
} // namespace

int main() {
    try {
        TestExpressionsAndControl();
        TestSandbox();
        TestImport();
        wchar_t root[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", root, 1024);
        const auto data = length == 0 || length >= 1024 ? std::nullopt : FindTemplateW3x2lniRoot(std::filesystem::path(root));
        if (!data) {
            std::cout << "lua template sandbox tests passed (slk part skipped: FORGE_YDWE_ROOT not set)\n";
            return 0;
        }
        SlkFixture fixture{*data};
        TestSlk(fixture);
        std::cout << "lua template sandbox tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
