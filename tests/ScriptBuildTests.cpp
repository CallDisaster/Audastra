#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/ScriptToolchainLayout.h"
#include "War3MapCore/Script/ScriptBaseInputs.h"
#include "War3MapCore/Script/ScriptUserSourcePlan.h"
#include "War3MapCore/Script/ScriptInjectionSources.h"
#include "War3MapCore/Script/ScriptPreprocessSources.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Jass/IdentifierRewrite.h"
#include "War3MapCore/Text/SourceEncoding.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/JassCompiler.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include "../src/ScriptBuild/YDTriggerStorage.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

// 整图脚本构建回归：全部使用原创合成数据，不依赖 YDWE 目录与外部编译器。
namespace {

using namespace War3Map;
using namespace War3Map::Script;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

void TestDeclaredSourceEncoding() {
    using namespace War3Map::Text;
    const auto utf = ReadSourceScalar("\xe4\xb8\xad", 0, SourceEncoding::Utf8), gbk = ReadSourceScalar("\xd6\xd0", 0, SourceEncoding::Gbk);
    Require(utf.Succeeded() && gbk.Succeeded() && utf.value->codepoint == 0x4e2du && gbk.value->codepoint == utf.value->codepoint
        && utf.value->bytes == 3 && gbk.value->bytes == 2, "declared UTF-8 and GBK decode the same Chinese scalar");
    const std::string escaped = "\x81\x5c\"";
    const auto pair = ReadSourceScalar(escaped, 0, SourceEncoding::Gbk), quote = ReadSourceScalar(escaped, 2, SourceEncoding::Gbk);
    Require(pair.Succeeded() && pair.value->bytes == 2 && pair.value->codepoint > 127u
        && quote.Succeeded() && quote.value->codepoint == '"' && quote.value->offset == 2u,
        "GBK trail backslash belongs to its scalar, leaving the following quote at its original offset");
    const auto four = ReadSourceScalar("\xf0\x9f\x98\x80", 0, SourceEncoding::Utf8);
    Require(four.Succeeded() && four.value->codepoint == 0x1f600u && four.value->bytes == 4, "UTF-8 four-byte scalar");
    for (const auto& bytes : std::array<std::string, 4>{"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8"}) {
        const auto bad = ReadSourceScalar("ab" + bytes, 2, SourceEncoding::Utf8);
        Require(!bad.Succeeded() && !bad.diagnostics.empty() && bad.diagnostics.front().origin && bad.diagnostics.front().origin->offset == 2u,
            "invalid scalar retains the original byte offset without guessing another encoding");
    }
    const auto bom = ReadSourceScalar("\xef\xbb\xbf", 0, SourceEncoding::Utf8), end = ReadSourceScalar("\x1a", 0, SourceEncoding::Gbk);
    Require(bom.Succeeded() && bom.value->codepoint == 0xfeffu && end.Succeeded() && end.value->codepoint == 0x1au, "BOM and CTRL-Z remain source scalars");
    Require(!ReadSourceScalar("x", 0, static_cast<SourceEncoding>(99)).Succeeded(), "unknown declared encoding is rejected");
}

void TestStringHash() {
    // 与游戏 StringHash 相同的值（UTF-8 字节）；大小写与斜杠归一。
    Require(StormStringHash("随机选取") == 0xD39371E0u, "StringHash of chinese text");
    Require(StormStringHash("运动器索引") == 0xFD698C78u, "StringHash longer than 12 bytes");
    Require(StormStringHash("abc/def") == StormStringHash("ABC\\DEF"), "StringHash normalizes case and slashes");
    Require(StormStringHash("a") != StormStringHash("b"), "StringHash distinguishes input");
}

// YDTrigger 存储宏的展开：与 YDWE 头文件经 Wave + 模板求值后的文本一致（值取自验收图的参考输出）。
void TestYDTriggerStorage() {
    using namespace War3Map::Script::detail;
    // 较新 YDWE（带 HTSystem.j）：句柄存储走存 null 时清除的库函数。
    const auto unit = StorageFor("unit", true);
    Require(unit.known && unit.load == "LoadUnitHandle" && unit.save == "YDTRIGGER_SET_unit" && unit.have == "HaveSavedHandle" &&
            unit.remove == "RemoveSavedHandle", "unit storage natives");
    // 1.32.13 等没有 HTSystem.j 的版本：直接 Save<Type>Handle。
    const auto plainUnit = StorageFor("unit", false);
    Require(plainUnit.known && plainUnit.save == "SaveUnitHandle" && plainUnit.load == "LoadUnitHandle", "unit storage without HTSystem.j");
    const auto text = StorageFor("StringExt", true);
    Require(text.load == "LoadStr" && text.save == "SaveStr" && text.have == "HaveSavedString" && text.remove == "RemoveSavedString",
        "string storage natives");
    const auto code = StorageFor("abilcode", false);
    Require(code.load == "LoadInteger" && code.save == "SaveInteger" && code.have == "HaveSavedInteger", "rawcode storage natives");
    const auto unknown = StorageFor("eventid", false);
    Require(!unknown.known && unknown.load == "YDTRIGGER_eventid(GET)" && unknown.save == "YDTRIGGER_eventid(SET)", "unsupported type keeps macro names");
    Require(StorageKey("注册单位") == "0x433971BC", "storage key is StringHash in hex");
    Require(StorageKey("a\\nb") == StorageKey("a\nb") && StorageKey("a\\nb") != StorageKey("a\\\\nb"), "storage key applies template escapes");
    Require(LocalPage(1) == "GetHandleId(GetTriggeringTrigger())*ydl_localvar_step" && LocalPage(5) == "ydl_triggerstep", "local pages");
    Require(LocalExecuteTrigger("ydl_trigger") ==
                "set ydl_triggerstep = GetHandleId(ydl_trigger)*(LoadInteger(YDLOC, GetHandleId(ydl_trigger), 0xCFDE6C76) + 3)",
        "execute trigger page");
    const auto handleKey = UserDataTableWrap("unit");
    const auto stringKey = UserDataTableWrap("string");
    const auto integerKey = UserDataTableWrap("integer");
    Require(handleKey.known && handleKey.prefix == "GetHandleId(" && stringKey.prefix == "StringHash(" && integerKey.prefix.empty(),
        "user data table keys");
    Require(!UserDataTableWrap("boolean").known && !UserDataTableWrap("degree").known, "boolean / degree keys are unsupported");
    Require(ObjectPropertyTable("YDWE_OBJECT_TYPE_UNIT") == "unit" && ObjectPropertyTable("UNIT").empty(), "object property tables");
    Require(ReplaceIdentifier("call F(t, Player(n), \"n\", 'n000', n2) // n", "n", "3") ==
                "call F(t, Player(3), \"n\", 'n000', n2) // n",
        "replace identifier skips strings, rawcodes, longer names and comments");
}

void TestTemplates() {
    TemplateContext context;
    context.abilityOrder = [](std::string_view ability, std::string_view type) -> std::optional<std::int64_t> {
        if (ability == "'A000'" && type == "Order") return 852214;
        return std::nullopt;
    };
    const std::string input =
        "a<?='\\n'?>b\n"
        "h=<?=StringHash( \"随机选取\")?>\n"
        "<?\nlocal N = 128 -- 注释\n?><?\n  local N = 256\n?>"
        "n=<?=N?> m=<?=math.modf(8192 / N)?> d=<?=8192 / N?> c=<?=\"x\" .. N?>\n"
        "o=<?=abilityOrder(\"'A000'\", ( \"Order\"))?>\n"
        "tail";
    const auto result = EvaluateTemplates(input, context);
    Require(result.Succeeded(), "template evaluation failed");
    const std::string expected =
        "a\nb\n"
        "h=0xD39371E0\n"
        "n=256 m=32 d=32.0 c=x256\n"
        "o=852214\n"
        "tail";
    Require(result.text == expected, "template output mismatch:\n" + result.text);
    Require(result.skippedBlocks == 0, "supported templates must not skip blocks");
    // 每个模板块登记输入/输出范围；块外文本逐字节对应，调用方据此把输出行映射回输入行。
    Require(result.spans.size() == 9, "every template block must record its source and output range");
    Require(result.spans[0].sourceBegin == 1 && result.spans[0].sourceEnd == 10
        && result.spans[0].outputBegin == 1 && result.spans[0].outputEnd == 2, "YDNL-style newline span");
    std::size_t source = 0, output = 0;
    for (const auto& span : result.spans) {
        Require(span.sourceBegin - source == span.outputBegin - output
            && input.compare(source, span.sourceBegin - source, result.text, output, span.outputBegin - output) == 0,
            "text between template blocks must be copied verbatim");
        source = span.sourceEnd; output = span.outputEnd;
    }
    Require(input.compare(source, std::string::npos, result.text, output, std::string::npos) == 0, "trailing text copied verbatim");

    // 原创合成物编模板：固定循环和字段写入也不能在未实现解释器时假报成功。
    const std::string objectTemplate =
        "header\n<?\nlocal slk = require 'slk'\n"
        "for i = 1, 2 do\n"
        "  local object = slk.ability.Afoo:new('Tst' .. i)\n"
        "  object.levels = 3\n"
        "  object.DataA1 = 2^(i-1)\n"
        "end\n?>tail";
    const auto objects = EvaluateTemplates(objectTemplate, context);
    Require(!objects.Succeeded() && objects.skippedBlocks == 1, "unsupported object writes must fail template evaluation");
    Require(objects.text == objectTemplate, "failed object template must remain visible in diagnostic output");
    bool slkError = false;
    for (const auto& diagnostic : objects.diagnostics) {
        if (diagnostic.code == "TEMPLATE_SLK_SKIPPED" && diagnostic.severity == DiagnosticSeverity::Error) {
            slkError = diagnostic.message.find("第 2 行") != std::string::npos;
        }
    }
    Require(slkError, "unsupported SLK block must report an error with its source line");

    // 不能靠查找连续 slk 字符串放行未知语句：模块名可以拼接、物编对象也可以使用别名。
    const std::string aliasedTemplate = "<? local objects = require('s' .. 'lk') objects.ability.Afoo:new('Tst0') ?>";
    const auto aliased = EvaluateTemplates(aliasedTemplate, context);
    Require(!aliased.Succeeded() && HasCode(aliased.diagnostics, "TEMPLATE_BLOCK_SKIPPED") && aliased.text == aliasedTemplate,
        "unknown block must fail even without a literal slk token");
    const auto unterminated = EvaluateTemplates("prefix<? local slk = require 'slk'", context);
    Require(!unterminated.Succeeded() && HasCode(unterminated.diagnostics, "TEMPLATE_UNTERMINATED")
        && unterminated.text == "prefix<? local slk = require 'slk'", "unterminated template must fail and preserve its boundary");
    const auto literal = EvaluateTemplates("<? local label = 'slk' ?><?=label?>", context);
    Require(literal.Succeeded() && literal.text == "slk", "supported literal assignments must remain valid");

    const auto missing = EvaluateTemplates("<?=Undefined?>", context);
    Require(!missing.Succeeded() && HasCode(missing.diagnostics, "TEMPLATE_EXPRESSION"), "undefined variable must fail");
    const auto unknownOrder = EvaluateTemplates("<?=abilityOrder(\"'B000'\", \"Order\")?>", context);
    Require(unknownOrder.Succeeded() && unknownOrder.text == "0" && HasCode(unknownOrder.diagnostics, "ABILITY_ORDER_UNKNOWN"),
        "unknown ability order falls back to 0 with a warning");
}

// 原创样例：YDWE 模板 import 的几种写法（文本、长字符串、拼接、地图目录下的文件）。
void TestTemplateImports(const std::filesystem::path& root) {
    const auto mapDir = root / "map";
    WriteText(mapDir / "lua" / "main.lua", "print('hi')\r\n");
    TemplateContext context;
    context.importRoot = mapDir;
    const std::string input =
        "a<?import(\"main.lua\")  (\"lua\\\\main.lua\", true)?>b\n"
        "<?\n    import 'inline.lua' [[\r\n        return 1\r\n    ]]\n?>"
        "<?import(\"ui.toc\")([=[\nui.fdf\n]=] .. '\\r\\n')?>"
        "<?import(\"ui.fdf\")[==[Frame \"X\" {}]]]==]?>"
        "c";
    const auto result = EvaluateTemplates(input, context);
    Require(result.Succeeded() && result.skippedBlocks == 0, "import forms evaluate without skipping blocks");
    Require(result.text == "ab\nc", "import blocks produce no script text:\n" + result.text);
    const auto content = [&](std::string_view name) {
        for (const auto& member : result.imports)
            if (member.name == name) return std::string(reinterpret_cast<const char*>(member.data.data()), member.data.size());
        return std::string("<missing>");
    };
    Require(result.imports.size() == 4 && content("main.lua") == "print('hi')\r\n", "path import copies the file next to the map");
    Require(content("inline.lua") == "        return 1\n    ", "long string skips the first newline and normalizes line ends");
    Require(content("ui.toc") == "ui.fdf\n\r\n", "concatenated content");
    Require(content("ui.fdf") == "Frame \"X\" {}]]", "leveled long bracket");
    Require(HasCode(result.diagnostics, "TEMPLATE_IMPORTED"), "imports are summarized");

    const auto checkOnly = EvaluateTemplates(input, TemplateContext{});
    Require(checkOnly.Succeeded() && checkOnly.imports.empty(), "without an import root only the syntax is checked");
    for (const auto* bad : {"<?import(\"x.lua\")(\"../secret.lua\", true)?>", "<?import(\"x.lua\")(\"lua\\\\missing.lua\", true)?>",
            "<?import(\"war3map.j\")[[x]]?>", "<?import(\"x.lua\")?>", "<?import(\"C:\\\\x.lua\")[[x]]?>"}) {
        const auto failed = EvaluateTemplates(bad, context);
        Require(!failed.Succeeded() && failed.skippedBlocks == 0 && HasCode(failed.diagnostics, "TEMPLATE_IMPORT_FAILED")
            && failed.imports.empty(), std::string("invalid import is reported clearly: ") + bad);
    }
    const auto twice = EvaluateTemplates("<?import 'a.lua' [[1]]?><?import 'A.lua' [[2]]?>", context);
    Require(twice.Succeeded() && twice.imports.size() == 1 && twice.imports[0].data.size() == 1
        && twice.imports[0].data[0] == std::byte{'2'} && HasCode(twice.diagnostics, "TEMPLATE_IMPORT_DUPLICATE"),
        "the last import of the same file wins");
}

void TestInject(const std::filesystem::path& root) {
    const auto jass = root / "pkg" / "jass";
    WriteText(jass / "lib" / "Foo.cfg", "# 注释\r\n[general]\r\nFooCall\r\n[old]\r\nOldOnly\r\n[new]\r\nNewOnly\r\n");
    WriteText(jass / "lib" / "Foo.j", "library Foo\r\nendlibrary");
    WriteText(jass / "Bar.cfg", "BarCall\r\n");
    WriteText(jass / "Bar.j", "library Bar\r\nendlibrary");
    std::vector<Diagnostic> diagnostics;
    const auto table = ScanInjectTable({jass}, true, diagnostics);
    Require(table.entries.size() == 3, "1.24 table must contain general + new names");
    std::vector<std::filesystem::path> injected;
    const auto script = InjectCode("call FooCall()\r\n", table, diagnostics, &injected);
    Require(injected.size() == 1 && script == "call FooCall()\r\nlibrary Foo\r\nendlibrary\r\n", "inject must append the used file");
    Require(InjectCode("call OldOnly()", table, diagnostics) == "call OldOnly()", "old-only names are not used for 1.24");
    const auto both = InjectCode("NewOnly BarCall", table, diagnostics, &injected);
    Require(both.find("library Foo") != std::string::npos && both.find("library Bar") != std::string::npos, "each used file is appended");

    // MemHack 的允许泄漏检测不是雪月 LeakMonitor；子串匹配会误注入其 XGJAPI 依赖。
    WriteText(jass / "LeakMonitor.cfg", "LeakMonitor\nCreateLeakMonitorReport\n");
    WriteText(jass / "LeakMonitor.j", "library LeakMonitor requires XGJAPI\ncall BarCall()\nendlibrary");
    const auto lexicalTable = ScanInjectTable({jass}, true, diagnostics);
    const std::string memHack = "call MHDebug_EnableLeakMonitor()\n";
    Require(InjectCode(memHack, lexicalTable, diagnostics) == memHack, "MemHack identifier must not inject the unrelated LeakMonitor library");
    const std::string ignored =
        "// call LeakMonitor()\n//# LeakMonitor\n/* function LeakMonitor */\n"
        "call DisplayTextToPlayer(Player(0), 0, 0, \"escaped \\\" LeakMonitor \\\"\")\n"
        "call LeakMonitorSuffix()\ncall PrefixLeakMonitor()\n";
    Require(InjectCode(ignored, lexicalTable, diagnostics) == ignored, "comments, literals and longer identifiers must not inject");
    for (const std::string source : {"call LeakMonitor()\n", "call TimerStart(t, 1.0, false, function LeakMonitor)\n"}) {
        injected.clear();
        const auto output = InjectCode(source, lexicalTable, diagnostics, &injected);
        Require(injected.size() == 1 && injected.front() == jass / "LeakMonitor.j"
            && output.find("library LeakMonitor requires XGJAPI") != std::string::npos
            && output.find("library Bar") == std::string::npos, "direct calls and function references inject once without recursively injecting appended code");
    }
    injected.clear();
    const auto multiple = InjectCode("call LeakMonitor()\ncall CreateLeakMonitorReport()\ncall BarCall()\ncall LeakMonitor()\n",
        lexicalTable, diagnostics, &injected);
    Require(injected.size() == 2 && multiple.find("library LeakMonitor requires XGJAPI") != std::string::npos
        && multiple.find("library Bar") != std::string::npos, "multiple identifiers inject each referenced file once");
    Require(diagnostics.empty(), "inject reported unexpected diagnostics");
}

// UI 根选 YDWE 根或其 ui 目录都能找到 compiler/；没有 ui/config 标记的目录不向上查找。
void TestToolRoot(const std::filesystem::path& root) {
    const auto ydwe = root / "ydwe";
    WriteText(ydwe / "ui" / "config", "[ui]");
    WriteText(ydwe / "compiler" / "include" / "WaveForce.j", "// original synthetic include\n");
    WriteText(ydwe / "compiler" / "jass" / "24" / "common.j", "// original synthetic common\n");
    WriteText(ydwe / "compiler" / "jass" / "24" / "blizzard.j", "// original synthetic blizzard\n");
    WriteText(ydwe / "other" / "readme.txt", "");
    Require(ResolveScriptToolRoot(ydwe) == ydwe, "YDWE root is already a tool root");
    Require(ResolveScriptToolRoot(ydwe / "ui") == ydwe, "ui directory resolves to its parent");
    Require(ResolveScriptToolRoot(ydwe / "other") == ydwe / "other", "unmarked directory must not walk up");
    Require(ResolveScriptToolRoot({}).empty(), "empty root stays empty");
}

void TestScriptBaseInputs(const std::filesystem::path& root, const ScriptToolchainLayout& layout) {
    const auto bytes = [](std::string_view text) {
        std::vector<std::byte> result(text.size()); std::memcpy(result.data(), text.data(), text.size()); return result;
    };
    const auto text = [](const ScriptBaseInput& input) {
        return std::string(reinterpret_cast<const char*>(input.bytes.data()), input.bytes.size());
    };
    const std::vector<MemberReplacement> members{{"common.j", bytes("// archive root common\n")},
        {"scripts\\common.j", bytes("// archive scripts common\n")}};
    Require(WriteVerifiedArchive(root / "base-input-map.w3x", members).Succeeded(), "write original base-input map fixture");
    const auto archive = MapArchiveSession::Open(root / "base-input-map.w3x");
    Require(archive.Succeeded(), "open original base-input map fixture");
    ScriptBaseInputsRequest request; request.layout = &layout; request.archive = &*archive.value;
    const auto defaults = CaptureScriptBaseInputs(request);
    Require(defaults.Succeeded() && defaults.value->common.origin == ScriptBaseOrigin::Layout
        && defaults.value->blizzard.origin == ScriptBaseOrigin::Layout,
        "default policy keeps bound layout even when the map contains common.j");
    Require(VerifyScriptBaseInputs(*defaults.value, layout, ScriptBasePolicy::BoundLayout).Succeeded(), "frozen default inputs verify");
    auto tampered = *defaults.value; tampered.common.bytes.front() ^= std::byte{1};
    Require(!VerifyScriptBaseInputs(tampered, layout, ScriptBasePolicy::BoundLayout).Succeeded(), "changed frozen bytes reject their old SHA");

    request.policy = ScriptBasePolicy::MapGameThenLayout;
    std::vector<MemberReplacement> edits{{"scripts/common.j", bytes("// replacement scripts common\n")},
        {"COMMON.J", bytes("native FrozenMapNative takes nothing returns nothing\n")}};
    request.replacements = edits;
    const auto mixed = CaptureScriptBaseInputs(request);
    Require(mixed.Succeeded() && text(mixed.value->common).find("FrozenMapNative") != std::string::npos
        && mixed.value->common.origin == ScriptBaseOrigin::MapReplacement
        && mixed.value->blizzard.origin == ScriptBaseOrigin::Layout,
        "root replacement wins over scripts alias while each library reports its own origin");
    ToolchainOptions options; options.toolRoot = layout.root; options.workDir = root / "frozen-base-pipeline";
    options.scriptInjection = false; options.layout = std::make_shared<const ScriptToolchainLayout>(layout);
    options.basePolicy = request.policy; options.baseInputs = std::make_shared<const ScriptBaseInputs>(*mixed.value);
    const auto compiled = CompileMapScript("function main takes nothing returns nothing\ncall FrozenMapNative()\nendfunction\n", options, {});
    Require(compiled.Succeeded() && compiled.syntaxChecked, "builtin pipeline checks a native declared only in the frozen map library");

    request.replacements = {};
    const auto fromArchive = CaptureScriptBaseInputs(request);
    Require(fromArchive.Succeeded() && fromArchive.value->common.origin == ScriptBaseOrigin::MapArchive
        && text(fromArchive.value->common) == "// archive root common\n", "map archive root wins over scripts alias");
    std::vector<std::string> removals{"common.j"}; request.removals = removals;
    const auto scripts = CaptureScriptBaseInputs(request);
    Require(scripts.Succeeded() && scripts.value->common.origin == ScriptBaseOrigin::MapArchive
        && text(scripts.value->common) == "// archive scripts common\n", "final removal hides archive root and selects scripts alias");
    edits.resize(1); request.replacements = edits;
    const auto scriptsEdit = CaptureScriptBaseInputs(request);
    Require(scriptsEdit.Succeeded() && text(scriptsEdit.value->common) == "// replacement scripts common\n",
        "final scripts replacement remains visible after deleting root member");
    request.replacements = {}; removals.push_back("SCRIPTS/COMMON.J"); request.removals = removals;
    const auto removed = CaptureScriptBaseInputs(request);
    Require(removed.Succeeded() && removed.value->common.origin == ScriptBaseOrigin::Layout, "removing both aliases permits bound-layout fallback");
    edits = {{"common.j", bytes("// one\n")}, {"COMMON.J", bytes("// two\n")}}; request.replacements = edits; request.removals = {};
    const auto duplicate = CaptureScriptBaseInputs(request);
    Require(!duplicate.Succeeded() && HasCode(duplicate.diagnostics, "SCRIPT_BASE_EDIT_CONFLICT"), "normalized replacement duplicates reject");
    edits.resize(1); request.replacements = edits; request.removals = removals;
    const auto conflict = CaptureScriptBaseInputs(request);
    Require(!conflict.Succeeded() && HasCode(conflict.diagnostics, "SCRIPT_BASE_EDIT_CONFLICT"), "replacement and removal cannot silently cancel");

    const auto gameRoot = root / "base-input-game"; std::filesystem::create_directories(gameRoot);
    const std::vector<MemberReplacement> gameMembers{{"common.j", bytes("// game root common\n")},
        {"scripts\\common.j", bytes("// game scripts common\n")}, {"scripts\\blizzard.j", bytes("// game scripts blizzard\n")}};
    Require(WriteVerifiedArchive(gameRoot / "war3patch.mpq", gameMembers).Succeeded(), "write original game archive fixture");
    const auto game = Game::GameDataSource::MountStrict(gameRoot); Require(game.Succeeded(), "mount original game archive fixture");
    request.game = &*game.value; request.replacements = {};
    struct Case { std::optional<int> version; bool success; ScriptBaseOrigin origin; const char* code; };
    const std::array cases{Case{{}, true, ScriptBaseOrigin::Layout, "GAME_BASE_POLICY_UNBOUND"},
        Case{20, true, ScriptBaseOrigin::Layout, "GAME_BASE_POLICY_MISMATCH"},
        Case{24, true, ScriptBaseOrigin::GameArchive, ""}, Case{27, false, ScriptBaseOrigin::Layout, "SCRIPT_BASE_GAME_POLICY_INVALID"}};
    for (const auto& item : cases) {
        request.gameLibraryVersion = item.version;
        const auto selected = CaptureScriptBaseInputs(request);
        Require(selected.Succeeded() == item.success, "game policy has the expected resolution status");
        if (*item.code) Require(HasCode(selected.diagnostics, item.code), "game policy reason remains visible");
        if (!item.success) continue;
        Require(selected.value->common.origin == item.origin && selected.value->blizzard.origin == item.origin,
            "unbound or mismatched game policy cannot supply the base libraries");
        if (item.origin == ScriptBaseOrigin::GameArchive)
            Require(text(selected.value->common) == "// game root common\n"
                && selected.value->common.archiveName == "war3patch.mpq"
                && text(selected.value->blizzard) == "// game scripts blizzard\n", "game library alias priority and actual MPQ provenance");
    }
    const auto frozenText = text(defaults.value->common);
    WriteText(layout.commonScript, "// common changed after base capture\n");
    Require(text(defaults.value->common) == frozenText && !VerifyScriptToolchainLayout(layout).Succeeded(),
        "frozen base bytes survive a source mutation, while the bound source is correctly stale");
}

void TestPreprocessSources(const std::filesystem::path& root, ScriptUserSourceRequest request) {
    const auto fixture = root / "preprocess-fixture", main = fixture / "main", user = fixture / "user";
    const auto tool = fixture / "tool", system = tool / "compiler" / "include";
    for (const auto* name : {"common.j", "blizzard.j"}) WriteText(tool / "compiler" / "jass" / "24" / name, "// base source\n");
    WriteText(system / "WaveForce.i", "#define MACRO_HEADER \"macro.h\"\n#define FORCE_VALUE 17\n");
    WriteText(main / "pick.h", "CURRENT_PICK\n"); WriteText(user / "pick.h", "IGNORED_USER_PICK\n"); WriteText(system / "pick.h", "SYSTEM_PICK\n");
    WriteText(user / "user.h", "USER_ONLY\n"); WriteText(system / "system.h", "SYSTEM_QUOTED\n"); WriteText(user / "macro.h", "MACRO_USER\n");
    const auto layout = ResolveScriptToolchainLayout({tool, 24, {}});
    Require(layout.Succeeded(), "resolve private preprocessing fixture with bound force header");
    request.layout = &*layout.value; request.explicitInjectionRoots.clear(); request.explicitUserIncludes = {user};
    const auto entry = CaptureScriptUserSourcePlan(request);
    Require(entry.Succeeded(), "capture real catalog and ordered include entrypoints");
    ScriptPreprocessSourceRequest input; input.mainDirectory = main; input.defines = {{"REQUEST_VALUE", "29"}};
    input.mainText = "#include <pick.h>\n#include <pick.h>\n#include \"pick.h\"\n#include \"user.h\"\n#include \"system.h\"\n#include MACRO_HEADER\n"
        "FORCE_VALUE REQUEST_VALUE\n#if 0\n#include \"unvisited.h\"\n#endif\n";
    const auto captured = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(captured.Succeeded(), "capture force and macro includes through the shared owned provider");
    const auto& snapshot = *captured.value; const auto original = snapshot.preprocessed.text;
    Require(snapshot.preprocessed.cacheHits > 0, "repeated include reuses a single-run lexical cache");
    for (const auto* marker : {"SYSTEM_PICK", "CURRENT_PICK", "USER_ONLY", "SYSTEM_QUOTED", "MACRO_USER", "17", "29"})
        Require(original.find(marker) != std::string::npos, "system angle and current/user/system quoted search select real sources");
    Require(original.find("IGNORED_USER_PICK") == std::string::npos && !snapshot.executionSupported
        && std::none_of(snapshot.includes.begin(), snapshot.includes.end(), [](const auto& include) {
            return include.binding.path.filename() == "unvisited.h";
        }), "coverage is only accessed candidates, excluding inactive conditional includes and legacy execution");
    Require(VerifyScriptPreprocessSources(snapshot, request, input).Succeeded(), "unchanged owned preprocessing snapshot verifies");
    WriteText(system / "system.h", "ALTERED_SYSTEM\n");
    Require(std::filesystem::remove(user / "user.h"), "delete only a private included source");
    const auto replay = ReplayScriptPreprocessSources(snapshot);
    Require(replay.Succeeded() && replay.text == original && !VerifyScriptPreprocessSources(snapshot, request, input).Succeeded(),
        "owned replay remains identical after changed/deleted include files while disk verification refuses them");
    WriteText(system / "system.h", "SYSTEM_QUOTED\n"); WriteText(user / "user.h", "USER_ONLY\n");
    const auto fresh = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(fresh.Succeeded(), "capture a fresh snapshot before higher priority shadowing");
    WriteText(main / "user.h", "NEW_CURRENT_OVERRIDE\n");
    Require(ReplayScriptPreprocessSources(*fresh.value).text == fresh.value->preprocessed.text
        && !VerifyScriptPreprocessSources(*fresh.value, request, input).Succeeded(), "a previously missing current-directory include cannot later shadow owned user bytes");
    WriteText(main / "fallback.h", "MUST_NOT_READ_DISK\n");
    Jass::PreprocessOptions options; options.userIncludes = {user}; options.systemIncludes = {system};
    int providerCalls = 0;
    options.includeProvider = [&](const std::filesystem::path&) {
        ++providerCalls;
        return Result<Jass::PreprocessIncludeContent>::Failure("Fixture", "PROVIDER_REFUSED", "original provider failure");
    };
    const auto refused = Jass::Preprocess("#include \"fallback.h\"\n", "main.j", main, options);
    Require(!refused.Succeeded() && HasCode(refused.diagnostics, "PROVIDER_REFUSED") && providerCalls == 1
        && refused.text.find("MUST_NOT_READ_DISK") == std::string::npos, "provider error stops first candidate without disk or lower-priority fallback");
    options.includeProvider = [](const std::filesystem::path&) -> Result<Jass::PreprocessIncludeContent> { throw std::runtime_error("fixture provider exception"); };
    const auto exceptional = Jass::Preprocess("#include \"fallback.h\"\n", "main.j", main, options);
    Require(!exceptional.Succeeded() && HasCode(exceptional.diagnostics, "PP_INCLUDE_PROVIDER_EXCEPTION"), "provider exception becomes a preprocess error");
    options.includeProvider = {}; options.maxOutputBytes = 8;
    const auto bounded = Jass::Preprocess("#define LONG 12345678901234567890\nLONG\n", "main.j", main, options);
    Require(!bounded.Succeeded() && HasCode(bounded.diagnostics, "PP_OUTPUT_BUDGET")
        && bounded.text.size() <= options.maxOutputBytes, "small output budget rejects expansion without emitting oversized text");
    WriteText(system / "budget.h", "#define BIG_PAYLOAD \"" + std::string(128 * 1024, 'X') + "\"\n");
    auto repeated = input; repeated.mainText.clear();
    for (int index = 0; index < 600; ++index) repeated.mainText += "#include <budget.h>\n";
    const auto accessBounded = CaptureScriptPreprocessSources(*entry.value, request, repeated);
    Require(!accessBounded.Succeeded() && HasCode(accessBounded.diagnostics, "SCRIPT_INCLUDE_ACCESS_BUDGET"),
        "repeated no-output include content cannot bypass the accumulated access budget");
}

void TestPreprocessMainTransform(const std::filesystem::path& root, const ScriptUserSourceRequest& request) {
    const auto entry = CaptureScriptUserSourcePlan(request);
    Require(entry.Succeeded(), "main transform uses the real catalog-bound owned source plan");
    ScriptPreprocessSourceRequest input; input.mainDirectory = root;
    input.mainText = "globals\n    integer udg_生命 = 1\n    integer udg_生命值 = 2\nendglobals\n"
        "function Probe takes nothing returns nothing\n    set udg_生命值 = udg_生命 + 2\n"
        "    call DisplayTextToPlayer(null, 0, 0, \"udg_生命值/udg_生命\")\n"
        "    // udg_生命值/udg_生命\nendfunction\n";
    const std::string expected = "globals\n    integer xgdg_A = 1\n    integer xgdg_z = 2\nendglobals\n"
        "function Probe takes nothing returns nothing\n    set xgdg_z = xgdg_A + 2\n"
        "    call DisplayTextToPlayer(null, 0, 0, \"xgdg_z/xgdg_A\")\n    // xgdg_z/xgdg_A\nendfunction\n";
    const auto normal = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(normal.Succeeded() && normal.value->preparedMainText.empty() && normal.value->request.mainText == input.mainText
        && normal.value->mainTransform.inputSha256 == normal.value->mainTransform.outputSha256
        && normal.value->mainTransform.names.empty(), "None leaves the owned main input and its identity unchanged");
    input.mainPolicy = ScriptPreprocessMainPolicy::LegacyGlobalBytesSubset;
    const auto transformed = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(transformed.Succeeded(), "explicit bounded whole-text transformation captures the two-name oracle fixture");
    const auto& snapshot = *transformed.value; const auto inputHash = Sha256Text(input.mainText), outputHash = Sha256Text(expected);
    Require(snapshot.preparedMainText == expected && snapshot.request.mainText == input.mainText
        && inputHash.Succeeded() && outputHash.Succeeded() && snapshot.mainSha256 == *inputHash.value
        && snapshot.mainTransform.inputSha256 == *inputHash.value && snapshot.mainTransform.outputSha256 == *outputHash.value
        && snapshot.mainTransform.inputBytes == input.mainText.size() && snapshot.mainTransform.outputBytes == expected.size()
        && snapshot.mainTransform.wholeTextReplacement && snapshot.mainTransform.names.size() == 2 && !snapshot.executionSupported,
        "long-name-first replacement matches the old oracle including strings/comments without overwriting original bytes");
    for (const auto& name : snapshot.mainTransform.names)
        Require(name.occurrences == 4 && name.declarationLine == (name.original == "udg_生命" ? 2u : 3u), "mapping retains original declaration line and actual replacement count");
    const auto replay = ReplayScriptPreprocessSources(snapshot);
    Require(replay.Succeeded() && replay.text == snapshot.preprocessed.text, "owned transformation replay is deterministic");
    auto changedPolicy = input; changedPolicy.mainPolicy = ScriptPreprocessMainPolicy::None;
    Require(!VerifyScriptPreprocessSources(snapshot, request, changedPolicy).Succeeded(), "a different main policy invalidates the snapshot");
    auto tampered = snapshot; tampered.preparedMainText += "// tampered\n";
    Require(!ReplayScriptPreprocessSources(tampered).Succeeded() && !VerifyScriptPreprocessSources(tampered, request, input).Succeeded(),
        "prepared bytes cannot be changed independently of the owned transform report");
    tampered = snapshot; tampered.mainTransform.names[0].occurrences += 1;
    const auto badReport = ReplayScriptPreprocessSources(tampered);
    Require(!badReport.Succeeded() && HasCode(badReport.diagnostics, "SCRIPT_MAIN_TRANSFORM_SNAPSHOT_MISMATCH")
        && !VerifyScriptPreprocessSources(tampered, request, input).Succeeded(), "a changed report never authorizes replay or verification");
    input.mainText.insert(input.mainText.find(" = 1") + 4, " // inline marker");
    const auto ambiguous = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(!ambiguous.Succeeded() && HasCode(ambiguous.diagnostics, "SCRIPT_MAIN_TRANSFORM_UNSUPPORTED"), "inline-comment declaration is rejected rather than guessed");
    const std::string longName = "udg_" + std::string(1024, 'a') + "生命";
    input.mainText = "globals\n    integer " + longName + " = 1\nendglobals\n// " + longName.substr(0, longName.size() - 3) + "值\n";
    const auto sharedPrefix = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(sharedPrefix.Succeeded() && sharedPrefix.value->mainTransform.names[0].occurrences == 1
        && sharedPrefix.value->preparedMainText.ends_with("// " + longName.substr(0, longName.size() - 3) + "值\n"),
        "a long shared prefix with a different final character is left intact by literal matching");
}

void TestInjectionLinePolicy(const std::filesystem::path& root, ScriptUserSourceRequest request) {
    const auto library = root / "line-policy-libraries";
    const std::string cfg = std::string(1, '\0') + "Edge" + '\0' + "\r\nVisible\x1a\r\nHidden\n";
    const std::string script = std::string("// raw ") + '\xD6' + '\xD0' + '\0' + "\r\n";
    WriteText(library / "Only.cfg", cfg); WriteText(library / "Only.j", script);
    request.explicitInjectionRoots = {library}; request.explicitUserIncludes.clear();
    const auto entry = CaptureScriptUserSourcePlan(request);
    Require(entry.Succeeded(), "line-policy fixture uses the actual catalog-bound source plan");
    const auto normal = CaptureScriptInjectionSources(*entry.value, request, true);
    const auto legacy = CaptureScriptInjectionSources(*entry.value, request, true, ScriptInjectionLinePolicy::Lua535WindowsTextC);
    Require(normal.Succeeded() && legacy.Succeeded() && legacy.value->textBoundaries.size() == 1, "capture both line policies and the text EOF boundary");
    Require(normal.value->registrations.size() == 3 && legacy.value->registrations.size() == 2
        && normal.value->registrations[0].key == "Edge" && legacy.value->registrations[0].key == cfg.substr(0, 6),
        "the default trims edge NUL; the CLI policy preserves it and excludes the key after CTRL-Z");
    const auto text = [](const auto& bytes) { return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()); };
    Require(text(legacy.value->files.at(legacy.value->textBoundaries[0].configFile).bytes) == cfg
        && text(legacy.value->files.at(legacy.value->registrations[0].libraryFile).bytes) == script
        && legacy.value->textBoundaries[0].originalOffset == cfg.find('\x1a'), "owned cfg/J bytes and EOF offset stay exact");
    const auto invalid = CaptureScriptInjectionSources(*entry.value, request, true, static_cast<ScriptInjectionLinePolicy>(99));
    Require(!invalid.Succeeded() && HasCode(invalid.diagnostics, "INJECTION_LINE_POLICY_INVALID")
        && !legacy.value->executionSupported && !legacy.value->legacyLineSemanticsProven, "invalid policy rejects and reference semantics grant no execution");
    Require(!VerifyScriptInjectionSources(*legacy.value, *entry.value, request, true).Succeeded(),
        "changing the requested line policy invalidates the owned snapshot");
}

void TestInjectionSources(const std::filesystem::path& root, ScriptUserSourceRequest request) {
    const auto library = root / "snapshot-libraries", missing = root / "snapshot-missing";
    const std::string cfg = "[general]\nShared\nShared\n//Literal\n[new]\nNewOnly\n[old]\nOldOnly\n", script = "// owned original library\n";
    WriteText(library / "A.cfg", cfg); WriteText(library / "A.j", script);
    WriteText(library / "B.cfg", "Shared\n"); WriteText(library / "B.j", "// newer shared library\n");
    WriteText(library / "Cr.cfg", "CrFirst\rCrSecond\r"); WriteText(library / "Cr.j", "// CR-only fixture\n");
    const auto older = std::filesystem::file_time_type::clock::now() - std::chrono::seconds(5), newer = older + std::chrono::seconds(2);
    std::filesystem::last_write_time(library / "A.j", older); std::filesystem::last_write_time(library / "B.j", newer);
    request.explicitInjectionRoots = {library}; request.explicitUserIncludes.clear();
    const auto entry = CaptureScriptUserSourcePlan(request);
    Require(entry.Succeeded(), "capture actual catalog-bound injection entrypoint plan");
    const auto captured = CaptureScriptInjectionSources(*entry.value, request, true);
    Require(captured.Succeeded(), "capture original cfg and library snapshot without executing it");
    const auto& snapshot = *captured.value;
    const auto choice = [](const ScriptInjectionSources& sources, std::string_view key) {
        return std::find_if(sources.choices.begin(), sources.choices.end(), [&](const auto& value) { return value.key == key; });
    };
    const auto shared = choice(snapshot, "Shared");
    Require(shared != snapshot.choices.end() && snapshot.files.at(shared->selectedFile).binding.path == library / "B.j"
        && std::any_of(shared->shadowedFiles.begin(), shared->shadowedFiles.end(), [&](std::size_t file) {
            return snapshot.files.at(file).binding.path == library / "A.j";
        }), "newer library wins while duplicate registrations in the same J preserve the older shadow");
    Require(choice(snapshot, "NewOnly") != snapshot.choices.end() && choice(snapshot, "OldOnly") == snapshot.choices.end()
        && choice(snapshot, "//Literal") != snapshot.choices.end() && choice(snapshot, "CrFirst\rCrSecond") != snapshot.choices.end(),
        "version sections apply without silently deleting // keys or splitting CR-only original keys");
    const auto old = CaptureScriptInjectionSources(*entry.value, request, false);
    Require(old.Succeeded() && choice(*old.value, "OldOnly") != old.value->choices.end()
        && choice(*old.value, "NewOnly") == old.value->choices.end(), "general and old sections are selected for the old version");
    Require(snapshot.coverage == ScriptInjectionCoverage::InjectionConfigAndLibraries && !snapshot.executionSupported
        && !snapshot.legacyLineSemanticsProven && VerifyScriptInjectionSources(snapshot, *entry.value, request, true).Succeeded(),
        "owned injection coverage neither grants execution nor claims old host line semantics");
    const auto configFile = std::find_if(snapshot.files.begin(), snapshot.files.end(), [&](const auto& file) { return file.binding.path == library / "A.cfg"; });
    const auto scriptFile = std::find_if(snapshot.files.begin(), snapshot.files.end(), [&](const auto& file) { return file.binding.path == library / "A.j"; });
    Require(configFile != snapshot.files.end() && scriptFile != snapshot.files.end(), "both cfg and matching J are owned");
    const auto configHash = Sha256(configFile->bytes), scriptHash = Sha256(scriptFile->bytes);
    const auto text = [](const auto& bytes) { return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()); };
    Require(configHash.Succeeded() && scriptHash.Succeeded() && configFile->binding.sha256 == *configHash.value
        && scriptFile->binding.sha256 == *scriptHash.value && text(configFile->bytes) == cfg && text(scriptFile->bytes) == script,
        "owned exact bytes and source SHA agree");
    WriteText(library / "A.cfg", "Changed\n"); WriteText(library / "A.j", "// changed original path\n");
    Require(text(configFile->bytes) == cfg && text(scriptFile->bytes) == script
        && !VerifyScriptInjectionSources(snapshot, *entry.value, request, true).Succeeded(), "source mutation invalidates verification without changing owned bytes");
    WriteText(library / "A.cfg", cfg); WriteText(library / "A.j", script);
    std::filesystem::last_write_time(library / "A.j", older); std::filesystem::last_write_time(library / "B.j", older);
    Require(!CaptureScriptInjectionSources(*entry.value, request, true).Succeeded(), "equal mtime across different J files cannot choose a winner");
    std::filesystem::last_write_time(library / "B.j", newer);
    Require(std::filesystem::remove(library / "B.j"), "remove B.j");
    const auto withoutB = CaptureScriptInjectionSources(*entry.value, request, true);
    const auto sharedWithoutB = withoutB.Succeeded() ? choice(*withoutB.value, "Shared") : snapshot.choices.end();
    Require(withoutB.Succeeded() && HasCode(withoutB.diagnostics, "INJECTION_SOURCE_LIBRARY_MISSING")
        && withoutB.value->missingLibraries.empty() && sharedWithoutB != withoutB.value->choices.end()
        && withoutB.value->files.at(sharedWithoutB->selectedFile).binding.path == library / "A.j",
        "a cfg without its library is a warning and keys still served by another library keep that library");
    WriteText(library / "B.j", "// newer shared library\n"); std::filesystem::last_write_time(library / "B.j", newer);
    WriteText(library / "Cr.cfg", "Dot.Call\n");
    const auto pattern = CaptureScriptInjectionSources(*entry.value, request, true);
    Require(pattern.Succeeded() && !pattern.value->literalLookupSupported && !pattern.value->executionSupported
        && std::any_of(pattern.value->registrations.begin(), pattern.value->registrations.end(), [](const auto& registration) {
            return registration.key == "Dot.Call" && registration.patternUnsupported;
        }), "Lua pattern keys remain owned but cannot claim supported literal lookup or execution");
    WriteText(library / "Extra.cfg", "ExtraUnique\n"); WriteText(library / "Extra.j", "// newly enumerated library\n");
    Require(!VerifyScriptInjectionSources(*pattern.value, *entry.value, request, true).Succeeded(),
        "new cfg/J inside an existing root invalidates the snapshot even when entrypoint directory state is unchanged");
    WriteText(library / "Cr.cfg", std::string(1024 * 1024 + 1, 'X'));
    Require(!CaptureScriptInjectionSources(*entry.value, request, true).Succeeded(), "cfg content above the read budget is rejected");
    request.explicitInjectionRoots = {missing};
    const auto missingEntry = CaptureScriptUserSourcePlan(request);
    Require(missingEntry.Succeeded(), "bind an explicit missing root");
    const auto empty = CaptureScriptInjectionSources(*missingEntry.value, request, true);
    Require(empty.Succeeded() && empty.value->files.empty(), "missing root has no fabricated library files");
    std::filesystem::create_directories(missing);
    Require(!VerifyScriptInjectionSources(*empty.value, *missingEntry.value, request, true).Succeeded(), "newly present root invalidates the original missing binding");
}

void TestFrameworkProfile(const std::filesystem::path& root, ScriptUserSourceRequest request) {
    const auto library = root / "framework-libraries", user = root / "framework-includes";
    const std::string cfg = "\xEF\xBB\xBF" "First // note\r\nSecond\nThird\r", script = "// owned profile library\r\n";
    WriteText(library / "Only.cfg", cfg); WriteText(library / "Only.j", script);
    const auto forced = root / "independent-force.j";
    WriteText(forced, "globals\ninteger profileForced = 73\nendglobals\n");
    std::filesystem::create_directories(user);
    ScriptToolchainProfile profile; profile.profileId = "synthetic-framework";
    profile.toolRoot = request.layout->root; profile.uiRoot = request.uiRoot;
    profile.mainSourceDirectory = root; profile.targetVersionMacro = request.layout->versionMacro;
    profile.sourceRoots = {{ScriptProfileSourceRole::Injection, library}, {ScriptProfileSourceRole::UserInclude, user},
        {ScriptProfileSourceRole::ForceInclude, forced}};
    request.explicitInjectionRoots.clear(); request.explicitUserIncludes.clear(); request.legacyUserLibraryRoot.clear();
    request.mapDirectory = root; request.profile = &profile;
    const auto identity = ValidateScriptToolchainProfile(profile);
    Require(identity.Succeeded(), "validate a bounded declarative framework profile");
    Require(profile.returnbugPolicy == ScriptProfileReturnbugPolicy::Strict, "framework profiles default to strict main-script typing");
    auto legacyReturns = profile; legacyReturns.returnbugPolicy = ScriptProfileReturnbugPolicy::InventoriedOnly;
    legacyReturns.returnbugPatternSetVersion = kPre124ReturnbugPatternSet;
    Require(!ValidateScriptToolchainProfile(legacyReturns).Succeeded(), "post124 does not grant returnbug for opaque MemHack imports");
    legacyReturns.baseLibraryCategory = ScriptProfileBaseLibraryCategory::Pre124; legacyReturns.targetVersionMacro = 127;
    Require(!ValidateScriptToolchainProfile(legacyReturns).Succeeded(), "a legacy base library does not weaken 1.27a main-script typing");
    legacyReturns.targetVersionMacro = 120;
    Require(ValidateScriptToolchainProfile(legacyReturns).Succeeded(), "pre124 can declare the inventoried YDWEBase scope without execution permission");
    legacyReturns.returnbugPatternSetVersion = "memhack-127";
    Require(!ValidateScriptToolchainProfile(legacyReturns).Succeeded(), "the inventoried scope cannot be extended to unrelated libraries");
    auto invalid = profile; invalid.returnbugPolicy = static_cast<ScriptProfileReturnbugPolicy>(99);
    Require(!ValidateScriptToolchainProfile(invalid).Succeeded(), "a profile cannot select an unknown returnbug policy");
    invalid = profile; invalid.mainEncoding = static_cast<ScriptProfileEncoding>(99);
    Require(!ValidateScriptToolchainProfile(invalid).Succeeded(), "an unknown profile encoding is rejected");
    const auto entry = CaptureScriptUserSourcePlan(request);
    Require(entry.Succeeded() && entry.value->profileIdentitySha256 == *identity.value
        && entry.value->orderedInjectionRoots == std::vector<std::filesystem::path>{library}
        && entry.value->orderedUserIncludes == std::vector<std::filesystem::path>{user}
        && entry.value->orderedSystemIncludes.empty() && entry.value->orderedForceIncludes == std::vector<std::filesystem::path>{forced} && !entry.value->executionSupported,
        "profile roles use declared roots without inheriting layout system/force includes or granting execution");
    ++profile.profileRevision; const auto revised = ValidateScriptToolchainProfile(profile);
    Require(revised.Succeeded() && *revised.value != *identity.value && !VerifyScriptUserSourcePlan(*entry.value, request).Succeeded(),
        "a profile revision changes logical identity and invalidates its source plan");
    --profile.profileRevision;
    const auto captured = CaptureScriptInjectionSources(*entry.value, request, true);
    Require(captured.Succeeded(), "profile selects normal framework text without requiring a separate injection path");
    const auto& snapshot = *captured.value; const auto& registrations = snapshot.registrations;
    Require(snapshot.linePolicy == ScriptInjectionLinePolicy::FrameworkText && registrations.size() == 3
        && registrations[0].key == "First" && registrations[1].key == "Second" && registrations[2].key == "Third"
        && HasCode(captured.diagnostics, "INJECTION_FRAMEWORK_TEXT_NORMALIZED") && snapshot.textBoundaries.empty() && !snapshot.executionSupported,
        "BOM and CRLF/LF/CR plus trailing comments are interpreted as normal text with explicit differences");
    const auto text = [](const auto& bytes) { return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()); };
    const auto configHash = Sha256Text(cfg); const auto& config = snapshot.files.at(registrations[0].configFile);
    Require(configHash.Succeeded() && text(config.bytes) == cfg && config.binding.sha256 == *configHash.value
        && text(snapshot.files.at(registrations[0].libraryFile).bytes) == script, "normalization preserves complete owned cfg/J bytes and source SHA");
    const auto historical = CaptureScriptInjectionSources(*entry.value, request, true, ScriptInjectionLinePolicy::Lua535WindowsTextC);
    Require(!historical.Succeeded() && HasCode(historical.diagnostics, "INJECTION_PROFILE_POLICY_REJECTED"), "profiles reject historical text-mode quirks");
    const auto oldVersion = CaptureScriptInjectionSources(*entry.value, request, false);
    Require(!oldVersion.Succeeded() && HasCode(oldVersion.diagnostics, "INJECTION_PROFILE_VERSION_REJECTED"), "Post124 profile rejects old-version injection");
    ScriptPreprocessSourceRequest input; input.mainDirectory = root; input.mainText = "globals\ninteger frameworkVersion = WARCRAFT_VERSION\nendglobals\n";
    const auto plain = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(plain.Succeeded() && plain.value->systemIncludes.empty() && plain.value->forceIncludes.size() == 1, "preprocessing also consumes only explicit profile roles");
    Require(plain.value->preprocessed.text.find("integer profileForced = 73") != std::string::npos, "an exact declared force file works outside user include roots");
    Require(plain.value->preprocessed.text.find("= " + std::to_string(profile.targetVersionMacro)) != std::string::npos
        && plain.value->preprocessed.text.find("WARCRAFT_VERSION") == std::string::npos, "profile injects its actual target version macro");
    auto conflict = input; conflict.defines = {{"WARCRAFT_VERSION", std::to_string(profile.targetVersionMacro + 1)}};
    const auto refusedMacro = CaptureScriptPreprocessSources(*entry.value, request, conflict);
    Require(!refusedMacro.Succeeded() && HasCode(refusedMacro.diagnostics, "SCRIPT_PROFILE_PREPROCESS_REJECTED"),
        "a caller cannot override the profile target version");
    auto macroReplay = *plain.value; macroReplay.request.defines = conflict.defines;
    Require(!ReplayScriptPreprocessSources(macroReplay).Succeeded(), "tampered replay cannot override the profile target version");
    input.mainPolicy = ScriptPreprocessMainPolicy::LegacyGlobalBytesSubset;
    const auto wholeText = CaptureScriptPreprocessSources(*entry.value, request, input);
    Require(!wholeText.Succeeded() && HasCode(wholeText.diagnostics, "SCRIPT_PROFILE_PREPROCESS_REJECTED"), "profile forbids whole-text variable replacement");
    auto replay = *plain.value; replay.request.mainPolicy = input.mainPolicy;
    Require(!ReplayScriptPreprocessSources(replay).Succeeded(), "a tampered profile snapshot cannot re-enable whole-text replay");
    auto modern = request; modern.profile = nullptr; modern.explicitInjectionRoots = {library};
    const auto modernEntry = CaptureScriptUserSourcePlan(modern);
    Require(modernEntry.Succeeded(), "the old modern source request remains available without a profile");
    const auto oldDefault = CaptureScriptInjectionSources(*modernEntry.value, modern, true);
    Require(oldDefault.Succeeded() && oldDefault.value->linePolicy == ScriptInjectionLinePolicy::ExistingLfControlTrim,
        "modern requests without a profile retain the existing default parser");
    auto controlled = cfg; const auto offset = cfg.find("Second") + 3; controlled.insert(offset, 1, '\x1a');
    WriteText(library / "Only.cfg", controlled);
    const auto refused = CaptureScriptInjectionSources(*entry.value, request, true);
    Require(!refused.Succeeded() && !refused.value && std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == "INJECTION_FRAMEWORK_CONTROL_BYTE" && diagnostic.origin && diagnostic.origin->offset == offset;
    }), "middle CTRL-Z is diagnosed at its original offset, never truncated into a successful snapshot");

    // 坏条目只影响自身：非函数名键跳过；缺 J 的 cfg 登记后仅在主文用到时报错；规范化提示整框架一次。
    WriteText(library / "Only.cfg", cfg);
    WriteText(library / "Pattern.cfg", "[MNXT]\r\nPatternCall\r\n"); WriteText(library / "Pattern.j", script);
    WriteText(library / "Orphan.cfg", "OrphanCall\r\nFirst\r\n");
    const auto tolerantEntry = CaptureScriptUserSourcePlan(request);
    Require(tolerantEntry.Succeeded(), "recapture the framework plan after adding cfgs");
    const auto tolerant = CaptureScriptInjectionSources(*tolerantEntry.value, request, true);
    Require(tolerant.Succeeded(), "an invalid key or a missing library does not fail the whole framework");
    const auto& tolerantRegistrations = tolerant.value->registrations;
    Require(HasCode(tolerant.diagnostics, "INJECTION_FRAMEWORK_KEY_INVALID") && HasCode(tolerant.diagnostics, "INJECTION_SOURCE_LIBRARY_MISSING")
        && std::none_of(tolerantRegistrations.begin(), tolerantRegistrations.end(), [](const auto& entry) { return entry.key == "[MNXT]"; })
        && std::any_of(tolerantRegistrations.begin(), tolerantRegistrations.end(), [](const auto& entry) { return entry.key == "PatternCall"; }),
        "a non-identifier key is skipped with a warning, never matched as a Lua pattern");
    Require(tolerant.value->missingLibraries.size() == 1 && tolerant.value->missingLibraries[0].key == "OrphanCall",
        "only keys without any real library are recorded as missing");
    const auto crlfNotes = std::count_if(tolerant.diagnostics.begin(), tolerant.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "INJECTION_FRAMEWORK_TEXT_NORMALIZED" && diagnostic.message.find("CRLF") != std::string::npos;
    });
    Require(crlfNotes == 1 && std::all_of(tolerant.diagnostics.begin(), tolerant.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code != "INJECTION_FRAMEWORK_TEXT_NORMALIZED" || diagnostic.severity == DiagnosticSeverity::Info;
    }), "CRLF normalization is one info note for the whole framework");
    const std::unordered_set<std::string_view> unused{"PatternCall", "First"}, used{"OrphanCall"};
    Require(CheckMissingInjectionLibraries(*tolerant.value, unused).Succeeded(), "an unused missing library is not an error");
    const auto usedMissing = CheckMissingInjectionLibraries(*tolerant.value, used);
    Require(!usedMissing.Succeeded() && HasCode(usedMissing.diagnostics, "INJECTION_LIBRARY_MISSING_USED"),
        "using a key whose library is missing is an error naming the cfg");
    for (const auto* name : {"Pattern.cfg", "Pattern.j", "Orphan.cfg"}) std::filesystem::remove(library / name);
}

void TestScriptUserSources(const std::filesystem::path& root, const ScriptToolchainLayout& modern) {
    const auto ui = root / "source-ui";
    const std::vector<std::string> layers{"alpha", "beta"};
    const std::string definition = "[TriggerTypes]\ninteger=0,1,1,整数,integer\n";
    WriteText(ui / "config", "alpha\nbeta\n");
    for (const auto& layer : layers) {
        WriteText(ui / layer / "define.txt", definition);
        WriteText(ui / layer / "jass" / "unselected.cfg", "UnselectedCall\n");
    }
    auto catalog = Trigger::UiCatalog::LoadLayers(ui, layers);
    Require(catalog.Succeeded() && !catalog.value->Sha256().empty(), "load original source-plan catalog rather than inventing its identity");
    const auto legacy = ResolveScriptToolchainLayout({root / "legacy", 24, {}});
    Require(legacy.Succeeded(), "resolve source-plan legacy layout");
    const auto chosen = root / "Component", map = root / "map-directory";
    WriteText(chosen / "jass" / "library.j", "// explicitly chosen user source\n");
    WriteText(chosen / "jass" / "native" / "other.lua", "-- not the installation native root\n");
    std::filesystem::create_directories(map);
    const auto switches = legacy.value->root / "bin" / "EverConfig.cfg";
    WriteText(switches, "[ScriptInjection]\nOption=0\n");
    ScriptUserSourceRequest request; request.layout = &*legacy.value; request.catalog = &*catalog.value;
    request.uiRoot = ui; request.legacyUserLibraryRoot = chosen; request.mapDirectory = map;
    const auto captured = CaptureScriptUserSourcePlan(request);
    Require(captured.Succeeded(), "capture explicit legacy source entrypoints without executing them");
    const auto& plan = *captured.value;
    Require(plan.orderedInjectionRoots == std::vector<std::filesystem::path>{chosen / "jass"}
        && plan.orderedUserIncludes == std::vector<std::filesystem::path>{map, chosen / "jass"}, "UI layer names never map to injection roots and map directory is include-only");
    Require(plan.effectiveLayers == layers && plan.catalogSha256 == catalog.value->Sha256()
        && plan.bindingCoverage == ScriptUserBindingCoverage::EntrypointsOnly && !plan.executionSupported,
        "actual catalog identity and deliberately limited coverage remain explicit");
    Require(plan.nativeRoot == legacy.value->root / "jass" / "native" && plan.nativeState == ScriptNativeSourceState::Missing
        && VerifyScriptUserSourcePlan(plan, request).Succeeded(), "native entrypoint is installation-bound, not Component-bound");
    const std::vector<std::string> reversed{"beta", "alpha"};
    auto reordered = Trigger::UiCatalog::LoadLayers(ui, reversed);
    Require(reordered.Succeeded(), "load reordered real catalog");
    request.catalog = &*reordered.value;
    Require(!VerifyScriptUserSourcePlan(plan, request).Succeeded(), "new loaded layer order invalidates the old source plan");
    WriteText(ui / "alpha" / "define.txt", "[TriggerTypes]\ninteger=0,1,1,Changed,integer\n");
    auto changed = Trigger::UiCatalog::LoadLayers(ui, layers);
    Require(changed.Succeeded() && changed.value->Sha256() != catalog.value->Sha256(), "real definition bytes change loaded catalog SHA");
    request.catalog = &*changed.value;
    Require(!VerifyScriptUserSourcePlan(plan, request).Succeeded(), "new loaded catalog SHA invalidates the old plan");
    WriteText(ui / "alpha" / "define.txt", definition); request.catalog = &*catalog.value;
    WriteText(switches, "[ScriptInjection]\nOption=1\n");
    Require(!VerifyScriptUserSourcePlan(plan, request).Succeeded(), "old switch configuration content is an entrypoint binding");
    WriteText(switches, "[ScriptInjection]\nOption=0\n");
    std::filesystem::create_directories(plan.nativeRoot);
    Require(!VerifyScriptUserSourcePlan(plan, request).Succeeded(), "missing native source cannot become present under an old plan");
    ScriptUserSourceRequest explicitModern; explicitModern.layout = &modern; explicitModern.catalog = &*catalog.value; explicitModern.uiRoot = ui;
    const auto first = root / "explicit-first", second = root / "explicit-second";
    explicitModern.explicitInjectionRoots = {first, second}; explicitModern.explicitUserIncludes = {second, first};
    const auto modernPlan = CaptureScriptUserSourcePlan(explicitModern);
    Require(modernPlan.Succeeded() && modernPlan.value->orderedInjectionRoots == explicitModern.explicitInjectionRoots
        && modernPlan.value->orderedUserIncludes == explicitModern.explicitUserIncludes
        && VerifyScriptUserSourcePlan(*modernPlan.value, explicitModern).Succeeded(), "modern explicit roots keep independent declared order without UI inference");
    TestInjectionSources(root, explicitModern);
    TestInjectionLinePolicy(root, explicitModern);
    TestPreprocessSources(root, explicitModern);
    TestPreprocessMainTransform(root, explicitModern);
    TestFrameworkProfile(root, explicitModern);
    explicitModern.catalog = nullptr;
    Require(!CaptureScriptUserSourcePlan(explicitModern).Succeeded(), "source plan requires an actual loaded catalog");
    explicitModern.catalog = &*catalog.value; explicitModern.uiRoot = "relative-ui";
    Require(!CaptureScriptUserSourcePlan(explicitModern).Succeeded(), "relative UI roots cannot bind a source plan");
    explicitModern.uiRoot = ui; explicitModern.explicitInjectionRoots.assign(129, first);
    Require(!CaptureScriptUserSourcePlan(explicitModern).Succeeded(), "entrypoint count above the declared budget rejects without deduplicating");
}

void TestToolchainLayouts(const std::filesystem::path& root) {
    const auto modern = [&](const std::filesystem::path& path) {
        WriteText(path / "ui" / "config", "base\n");
        WriteText(path / "compiler" / "include" / "WaveForce.j", "// synthetic force include\n");
        for (const auto version : {20, 24}) {
            const auto scripts = path / "compiler" / "jass" / std::to_string(version);
            WriteText(scripts / "common.j", "// common " + std::to_string(version) + " original\n");
            WriteText(scripts / "blizzard.j", "// blizzard " + std::to_string(version) + " original\n");
        }
    };
    const auto legacy = [&](const std::filesystem::path& path) {
        WriteText(path / "share" / "mpq" / "config", "base\n");
        WriteText(path / "plugin" / "wave" / "include" / "WaveForce.j", "// synthetic legacy include\n");
        for (const auto* version : {"ht", "rb"}) {
            const auto scripts = path / "jass" / "system" / version;
            WriteText(scripts / "common.j", "// synthetic legacy common\n");
            WriteText(scripts / "blizzard.j", "// synthetic legacy blizzard\n");
        }
    };
    modern(root / "modern"); legacy(root / "legacy");
    modern(root / "missing");
    Require(std::filesystem::remove(root / "missing" / "compiler" / "jass" / "24" / "common.j"), "remove synthetic common fixture");
    modern(root / "conflict"); legacy(root / "conflict");
    modern(root / "zero-tools");
    for (const auto& relative : {"compiler/wave/Wave.exe", "compiler/jasshelper/clijasshelper.exe", "compiler/pjass/pjass-latest.exe"})
        WriteText(root / "zero-tools" / relative, "");
    struct Case { const char* name; ToolchainLayoutRequest request; bool resolves, modern, returnBug; int macro; std::filesystem::path scripts; };
    const std::array cases{
        Case{"modern24", {root / "modern", 24, {}}, true, true, false, 124, "compiler/jass/24"},
        Case{"modern20", {root / "modern", 20, {}}, true, true, true, 120, "compiler/jass/20"},
        Case{"actual127", {root / "modern", 24, 127}, true, true, false, 127, "compiler/jass/24"},
        Case{"modern-ui", {root / "modern" / "ui", 24, {}}, true, true, false, 124, "compiler/jass/24"},
        Case{"legacy24", {root / "legacy", 24, {}}, true, false, false, 124, "jass/system/ht"},
        Case{"legacy20", {root / "legacy", 20, {}}, true, false, true, 120, "jass/system/rb"},
        Case{"legacy-share", {root / "legacy" / "share" / "mpq", 24, {}}, true, false, false, 124, "jass/system/ht"},
        Case{"missing-library", {root / "missing", 24, {}}, false, true, false, 124, {}},
        Case{"ambiguous-layout", {root / "conflict", 24, {}}, false, true, false, 124, {}},
        Case{"zero-executables", {root / "zero-tools", 24, {}}, true, true, false, 124, "compiler/jass/24"}
    };
    for (const auto& item : cases) {
        const auto resolved = ResolveScriptToolchainLayout(item.request);
        Require(resolved.Succeeded() == item.resolves, std::string(item.name) + ": expected resolution status");
        if (!resolved.Succeeded()) continue;
        const auto& layout = *resolved.value;
        const auto selected = item.request.toolRoot.filename();
        const auto expectedRoot = selected == "ui" ? item.request.toolRoot.parent_path()
            : selected == "mpq" ? item.request.toolRoot.parent_path().parent_path() : item.request.toolRoot;
        Require(layout.root == expectedRoot, std::string(item.name) + ": only documented UI tails select the installation root");
        Require((layout.kind == ScriptToolchainLayoutKind::Modern) == item.modern && layout.semanticsSupported == item.modern,
            std::string(item.name) + ": legacy recognition does not grant semantic compatibility");
        Require(layout.versionMacro == item.macro && layout.allowReturnBug == item.returnBug
            && layout.commonScript == layout.root / item.scripts / "common.j"
            && layout.blizzardScript == layout.root / item.scripts / "blizzard.j",
            std::string(item.name) + ": version macro, common scripts and return-bug policy are independent");
        Require(layout.waveExecutable.empty() && layout.jassHelperExecutable.empty() && layout.pjassExecutable.empty(),
            std::string(item.name) + ": absent/empty external tools cannot be selected, modern builtin remains usable");
        Require(VerifyScriptToolchainLayout(layout).Succeeded(), std::string(item.name) + ": unchanged frozen binding verifies");
    }
    const auto frozen = ResolveScriptToolchainLayout({root / "modern", 24, {}});
    Require(frozen.Succeeded(), "layout for stale binding check");
    TestScriptUserSources(root, *frozen.value);
    TestScriptBaseInputs(root, *frozen.value);
    WriteText(root / "modern" / "compiler" / "jass" / "24" / "common.j", "// common 24 modified\n");
    Require(!VerifyScriptToolchainLayout(*frozen.value).Succeeded(), "changed common SHA refuses frozen layout");

    const std::string source = "globals\ninteger version = WARCRAFT_VERSION\nendglobals\n"
        "function main takes nothing returns nothing\nendfunction\n";
    ToolchainOptions options; options.toolRoot = root / "modern"; options.workDir = root / "macro-127";
    options.warcraftVersion = 24; options.actualVersionMacro = 127; options.scriptInjection = false;
    const auto modern127 = ResolveScriptToolchainLayout({options.toolRoot, 24, 127});
    Require(modern127.Succeeded(), "resolve real builtin pipeline layout");
    options.layout = std::make_shared<const ScriptToolchainLayout>(*modern127.value);
    const auto compiled = CompileMapScript(source, options, {});
    std::string compact;
    for (const char byte : compiled.script) if (std::string_view(" \t\r\n").find(byte) == std::string_view::npos) compact += byte;
    Require(compiled.Succeeded() && compiled.syntaxChecked
        && compact.find("integerversion=127") != std::string::npos
        && compiled.script.find("WARCRAFT_VERSION") == std::string::npos,
        "actual version 127 macro reaches a successfully checked builtin script");
    options.uiRoot = root / "legacy"; options.workDir = root / "modern-tool-legacy-ui";
    const auto mixedProfile = CompileMapScript(source, options, {});
    Require(!mixedProfile.Succeeded() && HasCode(mixedProfile.diagnostics, "SCRIPT_TOOLCHAIN_UNSUPPORTED")
        && !std::filesystem::exists(options.workDir), "modern tools cannot silently omit legacy UI rewrite semantics");
    WriteText(root / "plain-ui" / "config", "base\n");
    for (const auto& ui : {root / "plain-ui", root / "modern" / "ui"}) {
        options.uiRoot = ui; options.workDir = root / ("accepted-ui-" + ui.parent_path().filename().string() + "-" + ui.filename().string());
        Require(CompileMapScript(source, options, {}).Succeeded(), "ordinary independent UI and complete modern UI remain supported");
    }
    struct BrokenUi { const char* name; const char* code; };
    const std::array brokenUis{
        BrokenUi{"missing-common", "SCRIPT_LAYOUT_BASE_SCRIPT_MISSING"},
        BrokenUi{"empty-common", "SCRIPT_LAYOUT_BASE_SCRIPT_MISSING"},
        BrokenUi{"missing-include", "SCRIPT_LAYOUT_INCLUDE_MISSING"},
        BrokenUi{"conflicting-ui", "SCRIPT_LAYOUT_AMBIGUOUS"}
    };
    for (const auto& item : brokenUis) {
        const auto ui = root / item.name;
        legacy(ui);
        const auto commonPath = ui / "jass" / "system" / "ht" / "common.j";
        if (std::string_view(item.name) == "missing-common") Require(std::filesystem::remove(commonPath), "remove legacy common fixture");
        else if (std::string_view(item.name) == "empty-common") WriteText(commonPath, "");
        else if (std::string_view(item.name) == "missing-include") {
            Require(std::filesystem::remove(ui / "plugin" / "wave" / "include" / "WaveForce.j")
                && std::filesystem::remove(ui / "plugin" / "wave" / "include"), "remove empty legacy include fixture");
        } else std::filesystem::create_directories(ui / "compiler");
        options.uiRoot = ui; options.workDir = root / (std::string("refused-ui-") + item.name);
        const auto refused = CompileMapScript(source, options, {});
        Require(!refused.Succeeded() && HasCode(refused.diagnostics, item.code)
            && !std::filesystem::exists(options.workDir), std::string(item.name) + ": preserve identified UI layout failure before any stage writes");
    }
    options.uiRoot.clear();
    options.workDir = root / "changed-during-preprocess";
    const auto common = modern127.value->commonScript;
    options.onStage = [common](std::string_view stage) {
        if (stage == "preprocess") WriteText(common, "// common changed while the pipeline ran\n");
    };
    const auto stale = CompileMapScript(source, options, {});
    Require(!stale.Succeeded() && HasCode(stale.diagnostics, "SCRIPT_LAYOUT_STALE") && stale.script.empty(),
        "a library mutation after preflight rejects final output rather than publishing checked text");
    options.toolRoot = root / "legacy"; options.workDir = root / "unsupported-legacy"; options.onStage = {};
    const auto old = ResolveScriptToolchainLayout({options.toolRoot, 24, 127});
    Require(old.Succeeded(), "identify legacy layout before pipeline semantic gate");
    options.layout = std::make_shared<const ScriptToolchainLayout>(*old.value);
    const auto unsupported = CompileMapScript(source, options, {});
    Require(!unsupported.Succeeded() && HasCode(unsupported.diagnostics, "SCRIPT_TOOLCHAIN_UNSUPPORTED")
        && !std::filesystem::exists(options.workDir), "legacy semantic gate rejects before creating stage workDir");
}

void TestFrameworkPipeline(const std::filesystem::path& root) {
    const auto tool = root / "framework", ui = tool / "share" / "mpq", map = root / "map-source";
    WriteText(ui / "config", "base\n");
    WriteText(ui / "base" / "define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
    WriteText(tool / "plugin" / "wave" / "include" / "WaveForce.i", "#define FORCE_VALUE 11\n");
    WriteText(tool / "jass" / "system" / "ht" / "common.j", "// original synthetic common\n");
    WriteText(tool / "jass" / "system" / "ht" / "blizzard.j", "// original synthetic blizzard\n");
    const std::string library = "library Fixture\nfunction InjectedCall takes nothing returns nothing\nendfunction\nendlibrary\n";
    WriteText(tool / "jass" / "Fixture.cfg", "\xef\xbb\xbf// original cfg\rInjectedCall // normal comment\r[new]\rNewOnly\r[old]\rOldOnly\r");
    WriteText(tool / "jass" / "Fixture.j", library);
    const std::string local = "#define LOCAL_VALUE 2\n";
    WriteText(map / "local.j", local);
    const auto layout = ResolveScriptToolchainLayout({tool, 24, 127});
    const std::array<std::string, 1> layers{"base"};
    const auto catalog = Trigger::UiCatalog::LoadLayers(ui, layers);
    Require(layout.Succeeded() && catalog.Succeeded(), "original legacy framework fixture has real layout and catalog");
    auto profile = MakeLegacyFrameworkProfile(*layout.value, *catalog.value, ui, map);
    Require(profile.Succeeded() && profile.value->directives.size() == 5 &&
        profile.value->directiveDialectVersion == "ydwe-dollar-features-v1", "default legacy profile declares only the explicit feature subset");
    auto host = *profile.value; host.templateApis.push_back("io.open");
    Require(!ValidateScriptToolchainProfile(host).Succeeded(), "qualified template declarations cannot name host members");
    auto malformed = *profile.value; malformed.templateApis.push_back("math..floor");
    Require(!ValidateScriptToolchainProfile(malformed).Succeeded(), "each qualified API segment must be a nonempty ASCII identifier");
    const auto captured = CaptureScriptFrameworkInputs(*profile.value, *layout.value, *catalog.value);
    Require(captured.Succeeded() && VerifyScriptFrameworkInputs(*captured.value, *layout.value).Succeeded(),
        "normal cfg and complete actual UI sources freeze without granting old Lua semantics");
    ToolchainOptions options; options.toolRoot = tool; options.uiRoot = ui; options.workDir = root / "no-owned-inputs";
    options.actualVersionMacro = 127; options.layout = std::make_shared<const ScriptToolchainLayout>(*layout.value);
    const std::string plain = "function main takes nothing returns nothing\nendfunction\n";
    Require(!CompileMapScript(plain, options, {}).Succeeded() && !std::filesystem::exists(options.workDir),
        "recognition alone cannot execute the legacy pipeline");
    options.frameworkInputs = std::make_shared<const ScriptFrameworkInputs>(*captured.value);
    options.workDir = root / "owned-global"; options.scriptInjection = false;
    const std::string source = "#include \"local.j\"\nglobals\ninteger udg_中文 = FORCE_VALUE + LOCAL_VALUE\n"
        "string literal = \"udg_中文\"\nendglobals\nfunction main takes nothing returns nothing\n"
        "set udg_中文 = <?=math.floor(4.8)?>\nendfunction\n";
    const auto compiled = CompileMapScript(source, options, {});
    Require(compiled.Succeeded() && compiled.syntaxChecked && compiled.frameworkPreprocessSources &&
        compiled.frameworkIdentifierReport && compiled.frameworkIdentifierReport->mappings.size() == 1 &&
        compiled.frameworkProfileSha256 == captured.value->entryPlan.profileIdentitySha256 &&
        compiled.script.find("\"udg_中文\"") != std::string::npos && compiled.script.find("FORCE_VALUE") == std::string::npos,
        "same pipeline replays forced/local inputs, rewrites bound root udg, and strictly checks final JASS");
    const auto replay = ReplayScriptPreprocessSources(*compiled.frameworkPreprocessSources);
    Require(replay.Succeeded() && replay.text == compiled.frameworkPreprocessSources->preprocessed.text,
        "owned preprocessing has deterministic replay");
    Require(std::filesystem::remove(map / "local.j"), "remove only the synthetic include");
    Require(ReplayScriptPreprocessSources(*compiled.frameworkPreprocessSources).Succeeded() &&
        !VerifyScriptPreprocessSources(*compiled.frameworkPreprocessSources, FrameworkSourceRequest(*captured.value),
            compiled.frameworkPreprocessSources->request).Succeeded(), "replay keeps owned bytes while live-source verification rejects disappearance");
    WriteText(map / "local.j", local);
    WriteText(map / "feature.j", "$def FEATURE\nglobals\ninteger IncludedValue=7\nendglobals\n");
    ScriptPreprocessSourceRequest feature;
    feature.mainDirectory = map; feature.frameworkDirectivePolicy = ScriptFrameworkDirectivePolicy::FeatureFlagsV1;
    feature.mainText = "#ifdef FEATURE\n#error wrong C namespace\n#endif\n#define CFLAG 1\n"
        "$ifdef CFLAG\n#error wrong dollar namespace\n$endif\n$ifdef FEATURE\n"
        "function Chosen takes nothing returns nothing\nendfunction\n$else\n#error missing feature\n$endif\n"
        "$include \"feature.j\"\n/* FEATURE\n FEATURE */\n";
    const auto features = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), feature);
    Require(features.Succeeded() && features.value->frameworkFeatureFlags == std::vector<std::string>{"FEATURE"} &&
        features.value->preprocessed.text.find("function Chosen") != std::string::npos &&
        features.value->preprocessed.text.find("IncludedValue") != std::string::npos &&
        ReplayScriptPreprocessSources(*features.value).text == features.value->preprocessed.text,
        "independent include/feature/condition phases see late declarations without merging C macro names or comment contents");
    bool includedOrigin = false;
    for (std::uint32_t line = 1; line <= 32; ++line)
        if (features.value->preprocessed.sourceMap.Describe(line).find("feature.j:3") != std::string::npos) includedOrigin = true;
    Require(includedOrigin, "dollar include source line survives the subsequent C source map");
    auto valued = feature; valued.mainText = "$def FEATURE value\n";
    const auto refusedValue = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), valued);
    Require(!refusedValue.Succeeded() && HasCode(refusedValue.diagnostics, "SCRIPT_FRAMEWORK_DOLLAR_DEF_UNSUPPORTED") &&
        refusedValue.diagnostics.front().origin && refusedValue.diagnostics.front().origin->offset == 0,
        "unproven valued substitutions reject with the original source offset");
    auto unknownDollar = feature; unknownDollar.mainText = "$unknown FEATURE\n";
    Require(!CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), unknownDollar).Succeeded(),
        "unknown framework directive never passes through as regular Wave text");
    auto incompatible = feature; incompatible.frameworkDirectivePolicy = ScriptFrameworkDirectivePolicy::None;
    Require(!CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), incompatible).Succeeded(),
        "owned profile cannot silently disable its explicit directive policy");
    WriteText(map / "late.j", "$def LATE\n");
    auto lateInclude = feature; lateInclude.mainText = "#include \"late.j\"\n";
    const auto lateFailure = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), lateInclude);
    Require(!lateFailure.Succeeded() && HasCode(lateFailure.diagnostics, "SCRIPT_FRAMEWORK_DOLLAR_LATE_INCLUDE"),
        "C-only include cannot inject a late feature into the already-completed framework phase");
    auto outside = feature; outside.mainText = "// first\n// second\n#include \"../outside.j\"\n";
    const auto outsideFailure = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), outside);
    Require(!outsideFailure.Succeeded() && HasCode(outsideFailure.diagnostics, "SCRIPT_INCLUDE_PATH_SCOPE") &&
        outsideFailure.diagnostics.front().origin && outsideFailure.diagnostics.front().origin->source == root / "outside.j" &&
        outsideFailure.diagnostics.front().message.find("include请求") != std::string::npos &&
        outsideFailure.diagnostics.front().message.find(outside.mainName + ":3") != std::string::npos &&
        outsideFailure.diagnostics.front().message.find("outside.j") != std::string::npos,
        "scope rejection names its exact attempted path and requesting source line without reading outside the bound roots");
    WriteText(tool / "plugin" / "wave" / "eligible.j", "ELIGIBLE_SOURCE\n");
    WriteText(root / "eligible.j", "OUT_OF_SCOPE_OLD\n");
    auto boundSearch = feature;
    boundSearch.includeCandidatePolicy = ScriptIncludeCandidatePolicy::BoundRootsSearch;
    boundSearch.mainText = "$include \"../eligible.j\"\n#include \"../eligible.j\"\n";
    const auto bounded = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), boundSearch);
    Require(bounded.Succeeded() && ReplayScriptPreprocessSources(*bounded.value).text == bounded.value->preprocessed.text,
        "explicit bound-root policy shares ordered eligibility across dollar and C include, with owned replay");
    std::size_t skipped = 0; bool dollarStage = false, cStage = false;
    std::string dollarHash, cHash;
    for (const auto& attempt : bounded.value->includeCandidates) {
        if (attempt.spelling != "../eligible.j") continue;
        if (attempt.eligibility == Jass::IncludeCandidateEligibility::SkippedOutOfScope) {
            ++skipped; Require(attempt.sourceIndex == UINT32_MAX, "scope skip cannot claim positive or Missing source");
        } else Require(attempt.sourceIndex < bounded.value->includes.size(), "allowed trace names exact owned source");
        if (attempt.phase == Jass::IncludePhase::FrameworkDollar) { dollarStage = true; dollarHash = attempt.requesterStageSha256; }
        else { cStage = true; cHash = attempt.requesterStageSha256; }
    }
    Require(skipped >= 2 && dollarStage && cStage && dollarHash != cHash &&
        std::none_of(bounded.value->includes.begin(), bounded.value->includes.end(), [&](const auto& item) { return item.binding.path == root / "eligible.j"; }),
        "trace binds real phase bytes and skips unauthorised candidates without fabricating negative bindings");
    WriteText(root / "eligible.j", "OUT_OF_SCOPE_CHANGED\n");
    Require(VerifyScriptPreprocessSources(*bounded.value, FrameworkSourceRequest(*captured.value), boundSearch).Succeeded(),
        "skipped outside content is not read or included in verification");
    auto changedTrace = *bounded.value;
    const auto skip = std::find_if(changedTrace.includeCandidates.begin(), changedTrace.includeCandidates.end(), [](const auto& item) {
        return item.eligibility == Jass::IncludeCandidateEligibility::SkippedOutOfScope;
    });
    skip->eligibility = Jass::IncludeCandidateEligibility::Allowed;
    Require(!ReplayScriptPreprocessSources(changedTrace).Succeeded(), "tampering skipped eligibility cannot authorize an outside read on replay");
    changedTrace = *bounded.value; changedTrace.includeCandidates.pop_back();
    Require(!ReplayScriptPreprocessSources(changedTrace).Succeeded(), "missing trace step rejects owned replay");
    changedTrace = *bounded.value; changedTrace.includeCandidates.push_back(changedTrace.includeCandidates.back());
    Require(!ReplayScriptPreprocessSources(changedTrace).Succeeded(), "unconsumed trace suffix rejects owned replay");
    auto strictSearch = boundSearch; strictSearch.includeCandidatePolicy = ScriptIncludeCandidatePolicy::Strict;
    Require(!CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), strictSearch).Succeeded() &&
        !VerifyScriptPreprocessSources(*bounded.value, FrameworkSourceRequest(*captured.value), strictSearch).Succeeded(),
        "default Strict retains original scope refusal and changing policy invalidates the complete snapshot");
    auto noEligible = boundSearch; noEligible.mainText = "$include \"../unavailable.j\"\n";
    const auto none = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), noEligible);
    Require(!none.Succeeded() && HasCode(none.diagnostics, "SCRIPT_INCLUDE_PATH_SCOPE"), "unselected search with any scope skip remains a scope error");
    auto invalidPath = boundSearch; invalidPath.mainText = "#include \"bad:dir/../eligible.j\"\n";
    const auto invalid = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), invalidPath);
    Require(!invalid.Succeeded() && HasCode(invalid.diagnostics, "SCRIPT_INCLUDE_PATH_INVALID"), "lexical parent normalization cannot erase an ADS component to gain eligibility");
    const auto ordinary = root / "ordinary", entryFile = ordinary / "nested" / "entry.j";
    WriteText(entryFile, "ORDINARY_SOURCE\n"); WriteText(ordinary / "sibling.j", "SIBLING_SOURCE\n");
    auto accessRequest = boundSearch; accessRequest.mainText = "$include \"../ordinary/nested/entry.j\"\n#include \"../ordinary/nested/entry.j\"\n";
    Require(!CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), accessRequest).Succeeded(),
        "searching a relative sibling never implicitly authorizes ordinary include access");
    auto accessProfile = *profile.value;
    accessProfile.ordinaryIncludeAccess = {{ordinary, ScriptIncludeAccessKind::Directory, ScriptProfileEncoding::Gbk},
        {entryFile, ScriptIncludeAccessKind::ExactFile, ScriptProfileEncoding::Utf8}};
    const auto accessInputs = CaptureScriptFrameworkInputs(accessProfile, *layout.value, *catalog.value);
    Require(accessInputs.Succeeded() && accessInputs.value->entryPlan.orderedUserIncludes == captured.value->entryPlan.orderedUserIncludes &&
        accessInputs.value->entryPlan.orderedSystemIncludes == captured.value->entryPlan.orderedSystemIncludes &&
        accessInputs.value->entryPlan.orderedInjectionRoots == captured.value->entryPlan.orderedInjectionRoots &&
        FrameworkEncodingFor(*accessInputs.value, entryFile) == ScriptProfileEncoding::Utf8 &&
        FrameworkEncodingFor(accessInputs.value->entryPlan, ordinary / "sibling.j") == ScriptProfileEncoding::Gbk,
        "ordinary access binds types and encoding independently without adding search or injection roots");
    const auto accessed = CaptureScriptPreprocessSources(accessInputs.value->entryPlan, FrameworkSourceRequest(*accessInputs.value), accessRequest);
    Require(accessed.Succeeded() && accessed.value->preprocessed.text.find("ORDINARY_SOURCE") != std::string::npos &&
        ReplayScriptPreprocessSources(*accessed.value).text == accessed.value->preprocessed.text &&
        VerifyScriptPreprocessSources(*accessed.value, FrameworkSourceRequest(*accessInputs.value), accessRequest).Succeeded(),
        "explicit ordinary permissions share dollar/C owned capture, verification and replay");
    auto modifiedAccess = *accessed.value; modifiedAccess.entryPlan.profileOrdinaryIncludeAccess[0].encoding = ScriptProfileEncoding::Utf8;
    Require(!ReplayScriptPreprocessSources(modifiedAccess).Succeeded(), "changing an owned permission encoding invalidates replay policy identity");
    auto exactProfile = accessProfile; exactProfile.ordinaryIncludeAccess.erase(exactProfile.ordinaryIncludeAccess.begin());
    const auto exactInputs = CaptureScriptFrameworkInputs(exactProfile, *layout.value, *catalog.value);
    auto sibling = accessRequest; sibling.mainText = "$include \"../ordinary/sibling.j\"\n";
    Require(exactInputs.Succeeded() && CaptureScriptPreprocessSources(exactInputs.value->entryPlan, FrameworkSourceRequest(*exactInputs.value), accessRequest).Succeeded() &&
        !CaptureScriptPreprocessSources(exactInputs.value->entryPlan, FrameworkSourceRequest(*exactInputs.value), sibling).Succeeded(),
        "ExactFile grants only that file, never its sibling directory");
    WriteText(root / "ordinary-extra" / "entry.j", "PREFIX_NEIGHBOR\n");
    auto prefix = accessRequest; prefix.mainText = "#include \"../ordinary-extra/entry.j\"\n";
    Require(!CaptureScriptPreprocessSources(accessInputs.value->entryPlan, FrameworkSourceRequest(*accessInputs.value), prefix).Succeeded(),
        "Directory qualification uses component boundaries, not a path string prefix");
    std::filesystem::create_directories(ordinary / "bad.j");
    WriteText(tool / "plugin" / "wave" / "ordinary" / "bad.j", "LOWER_PRIORITY_VALID\n");
    auto badType = accessRequest; badType.mainText = "$include \"../ordinary/bad.j\"\n";
    const auto badAccess = CaptureScriptPreprocessSources(accessInputs.value->entryPlan, FrameworkSourceRequest(*accessInputs.value), badType);
    Require(!badAccess.Succeeded() && HasCode(badAccess.diagnostics, "SCRIPT_USER_SOURCE_BINDING_FAILED"),
        "an authorized candidate with the wrong file type fails rather than falling through to a later valid source");
    auto precedence = accessProfile; precedence.ordinaryIncludeAccess.push_back({ordinary / "nested", ScriptIncludeAccessKind::Directory, ScriptProfileEncoding::Utf8});
    precedence.sourceEncodings.push_back({entryFile, ScriptProfileEncoding::Gbk});
    const auto precedenceInputs = CaptureScriptFrameworkInputs(precedence, *layout.value, *catalog.value);
    Require(precedenceInputs.Succeeded() && FrameworkEncodingFor(*precedenceInputs.value, entryFile) == ScriptProfileEncoding::Gbk &&
        FrameworkEncodingFor(precedenceInputs.value->entryPlan, ordinary / "nested" / "other.j") == ScriptProfileEncoding::Utf8,
        "per-file encoding overrides ExactFile, while the most specific Directory controls other children");
    auto conflictAccess = accessProfile; conflictAccess.ordinaryIncludeAccess.push_back({entryFile, ScriptIncludeAccessKind::ExactFile, ScriptProfileEncoding::Gbk});
    Require(!ValidateScriptToolchainProfile(conflictAccess).Succeeded(), "same-path permission and encoding conflicts reject before reading");
    auto optionalAccess = accessProfile; optionalAccess.ordinaryIncludeAccess.push_back({ordinary / "later.j", ScriptIncludeAccessKind::ExactFile, ScriptProfileEncoding::Utf8, false});
    const auto optionalInputs = CaptureScriptFrameworkInputs(optionalAccess, *layout.value, *catalog.value);
    Require(optionalInputs.Succeeded(), "optional absent ordinary file has a real negative entry binding");
    WriteText(ordinary / "later.j", "APPEARED_SOURCE\n");
    Require(!VerifyScriptFrameworkInputs(*optionalInputs.value, *layout.value).Succeeded(), "an optional permission entry appearing invalidates its captured negative identity");
    options.workDir = root / "owned-injected"; options.scriptInjection = true;
    const auto injected = CompileMapScript("function main takes nothing returns nothing\ncall InjectedCall()\nendfunction\n", options, {});
    Require(injected.Succeeded() && injected.syntaxChecked && injected.script.find("function InjectedCall") != std::string::npos,
        "owned exact injection reaches existing vJass ordering and strict final check");
    std::vector<std::filesystem::path> paths;
    const std::string ignored = "// InjectedCall\nglobals\nstring s=\"InjectedCall\"\nendglobals\ncall InjectedCallSuffix()\n";
    auto untouched = InjectScriptFramework(ignored, *captured.value, paths);
    Require(untouched.Succeeded() && *untouched.value == ignored && paths.empty(),
        "literal/comment/longer names do not select an owned library");
    options.workDir = root / "unknown-template";
    const auto unknown = CompileMapScript("globals\ninteger v=<?=StrFormat(1)?>\nendglobals\n" + plain, options, {});
    Require(!unknown.Succeeded() && unknown.script.empty() && HasCode(unknown.diagnostics, "TEMPLATE_EXPRESSION"),
        "unknown public template API fails its real stage without old wholeChunk fallback");
    options.workDir = root / "strict-main";
    const auto pragma = CompileMapScript("//# +rb\n" + plain, options, {});
    Require(!pragma.Succeeded() && HasCode(pragma.diagnostics, "SCRIPT_FRAMEWORK_CHECK_PRAGMA_UNSUPPORTED"),
        "strict framework cannot obtain a main-script returnbug exception from a pragma");
    auto unsupported = *profile.value; unsupported.templateApis.push_back("StrFormat");
    Require(!CaptureScriptFrameworkInputs(unsupported, *layout.value, *catalog.value).Succeeded(),
        "a declared unimplemented API cannot enlarge execution capability");
    const auto dollar = ValidateFrameworkSourceEncoding("// $def ignored\n$def X 1\n", map / "input.j", ScriptProfileEncoding::Utf8);
    Require(!dollar.Succeeded() && HasCode(dollar.diagnostics, "SCRIPT_FRAMEWORK_DOLLAR_UNSUPPORTED") &&
        dollar.diagnostics.front().origin && dollar.diagnostics.front().origin->offset == 16,
        "unsupported directive reports its original byte position, not comments");
    Require(ValidateFrameworkSourceEncoding("set x=$FF\n// $include ignored\n", map / "input.j", ScriptProfileEncoding::Utf8).Succeeded(),
        "JASS hexadecimal expressions are distinct from line-start dollar directives");
    Require(ValidateFrameworkSourceEncoding("// \xd6\xd0\n", map / "gbk.j", ScriptProfileEncoding::Gbk).Succeeded(),
        "declared GBK comments retain original bytes through a verified lexical-only view");
    // 中文只在注释/字符串里时编码选择不影响代码含义：原字节保留并提示；出现在标识符里仍须显式声明。
    const auto opaqueOnly = MakeFrameworkLexicalView("// \xe4\xb8\xad\xe6\x96\x87\nset s = \"\xe4\xb8\xad\xe6\x96\x87\"\n", map / "comment.j", ScriptProfileEncoding::UniqueUtf8OrGbk);
    Require(opaqueOnly.Succeeded() && opaqueOnly.value->text == "// \xe4\xb8\xad\xe6\x96\x87\nset s = \"\xe4\xb8\xad\xe6\x96\x87\"\n" && opaqueOnly.value->opaque.empty() &&
        HasCode(opaqueOnly.diagnostics, "SCRIPT_FRAMEWORK_SOURCE_ENCODING_OPAQUE"),
        "ambiguous bytes only in comments and strings keep original bytes with an info note");
    const auto ambiguous = MakeFrameworkLexicalView("function \xe4\xb8\xad\xe6\x96\x87 takes nothing returns nothing\n", map / "ambiguous.j", ScriptProfileEncoding::UniqueUtf8OrGbk);
    Require(!ambiguous.Succeeded() && HasCode(ambiguous.diagnostics, "SCRIPT_FRAMEWORK_SOURCE_ENCODING_AMBIGUOUS") &&
        ambiguous.diagnostics.front().origin && ambiguous.diagnostics.front().origin->source == map / "ambiguous.j" &&
        ambiguous.diagnostics.front().origin->offset == 9 && ambiguous.diagnostics.front().origin->length == 3 &&
        ambiguous.diagnostics.front().message.find("ambiguous.j:1") != std::string::npos &&
        ambiguous.diagnostics.front().message.find("原字节偏移 9") != std::string::npos,
        "an ambiguous encoding retains the actual source and first original scalar location in every host's message");
    auto explicitEncoding = *profile.value; explicitEncoding.sourceEncodings.push_back({map / "ambiguous.j", ScriptProfileEncoding::Utf8});
    const auto explicitCaptured = CaptureScriptFrameworkInputs(explicitEncoding, *layout.value, *catalog.value);
    Require(explicitCaptured.Succeeded() && explicitCaptured.value->entryPlan.profileIdentitySha256 != captured.value->entryPlan.profileIdentitySha256 &&
        FrameworkEncodingFor(explicitCaptured.value->entryPlan, map / "ambiguous.j") == ScriptProfileEncoding::Utf8 &&
        VerifyScriptFrameworkInputs(*explicitCaptured.value, *layout.value).Succeeded(),
        "an explicit per-file encoding is bound to the logical profile and frozen source plan");
    Require(!MakeFrameworkLexicalView("globals\ninteger udg_\xd6\xd0=1\nendglobals\n", map / "gbk-name.j", ScriptProfileEncoding::Gbk).Succeeded(),
        "GBK code identifiers remain unsupported instead of whole-text transcoding");
    const std::string opaque = "// \xd6\xd0\n#define OPAQUE_LITERAL \"\x81\x5c\"\nglobals\nstring value=OPAQUE_LITERAL\nendglobals\n";
    WriteText(map / "opaque.j", opaque);
    auto mixed = feature; mixed.mainText = "$include \"opaque.j\"\n" + plain;
    const auto mixedCaptured = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), mixed);
    Require(mixedCaptured.Succeeded() && mixedCaptured.value->preprocessed.text.find("\"\x81\x5c\"") != std::string::npos &&
        !mixedCaptured.value->preprocessed.lexicalView.opaque.empty() &&
        Jass::ValidateSourceLexicalView(mixedCaptured.value->preprocessed.text, mixedCaptured.value->preprocessed.lexicalView).Succeeded(),
        "mixed sources carry raw GBK literal bytes, including a trail backslash, through macro expansion");
    const auto mixedReplay = ReplayScriptPreprocessSources(*mixedCaptured.value);
    Require(mixedReplay.Succeeded() && mixedReplay.text == mixedCaptured.value->preprocessed.text &&
        mixedReplay.lexicalView.opaque == mixedCaptured.value->preprocessed.lexicalView.opaque,
        "owned replay reproduces both raw bytes and their source qualification");
    Jass::IdentifierRewriteRequest opaqueRewrite;
    opaqueRewrite.text = mixedReplay.text; opaqueRewrite.encoding = Text::SourceEncoding::Utf8;
    opaqueRewrite.sourceName = "mixed-stage.j"; opaqueRewrite.globalScopeIdentity = "map:root";
    opaqueRewrite.expectedInputSha256 = *Sha256Text(mixedReplay.text).value;
    opaqueRewrite.lexicalView = &mixedReplay.lexicalView;
    const auto opaqueRewritten = Jass::RewriteUdgIdentifiers(opaqueRewrite);
    Require(opaqueRewritten.Succeeded() && opaqueRewritten.value->text == mixedReplay.text,
        "rewrite lexes the qualified view but returns exact original literal bytes");
    const auto translatedOpaque = RebindFrameworkLexicalView("globals\nstring value=\"\x81\x5c\"\nendglobals\n", mixedReplay.text, mixedReplay.lexicalView);
    Require(translatedOpaque.Succeeded() && !translatedOpaque.value->opaque.empty(),
        "later output only rebinds a complete literal copied unchanged from an owned qualified source");
    Require(!RebindFrameworkLexicalView("globals\nstring value=\"new\x81\x5c\"\nendglobals\n", mixedReplay.text, mixedReplay.lexicalView).Succeeded(),
        "new or transformed opaque output cannot inherit unrelated source encoding qualification");
    auto forged = mixedReplay.lexicalView; forged.text[0] = '#';
    Require(!Jass::ValidateSourceLexicalView(mixedReplay.text, forged).Succeeded(),
        "the lexical provider cannot hide or replace arbitrary original ASCII code");
    WriteText(map / "stringize.j", "#define STR(x) #x\nglobals\nstring value=STR(\"\x81\x5c\")\nendglobals\n");
    auto unsupportedStringize = feature; unsupportedStringize.mainText = "$include \"stringize.j\"\n";
    const auto stringize = CaptureScriptPreprocessSources(captured.value->entryPlan, FrameworkSourceRequest(*captured.value), unsupportedStringize);
    Require(!stringize.Succeeded() && HasCode(stringize.diagnostics, "PP_OPAQUE_STRINGIZE_UNSUPPORTED"),
        "macro stringification cannot reinterpret opaque cross-encoding bytes");
    auto incomplete = *captured.value; incomplete.uiBindings.pop_back();
    Require(!VerifyScriptFrameworkInputs(incomplete, *layout.value).Succeeded(), "source binding list cannot omit a UI negative candidate");
    WriteText(ui / "base" / "action.txt", "// new source that was absent\n");
    Require(!VerifyScriptFrameworkInputs(*captured.value, *layout.value).Succeeded(), "new UI source shadows a previously missing catalog candidate");
}

void TestNamesAndTerrain() {
    Require(ConvertObjectName("Hero Init ") == "Hero_Init", "ascii object name");
    Require(ConvertObjectName("技能") == std::string(6, '_') + "u", "chinese object name");
    std::string w3e = "W3E!";
    const auto integer = [&w3e](std::int32_t value) { w3e.append(reinterpret_cast<const char*>(&value), 4); };
    const auto real = [&w3e](float value) { w3e.append(reinterpret_cast<const char*>(&value), 4); };
    integer(11);
    w3e += 'L';
    integer(0);
    integer(1);
    w3e += "Ldrt";
    integer(1);
    w3e += "CLdi";
    integer(129);
    integer(193);
    real(-8192.0f);
    real(-13312.0f);
    const auto extent = ReadTerrainExtent(std::as_bytes(std::span(w3e.data(), w3e.size())));
    Require(extent && extent->width == 129 && extent->height == 193 && extent->offsetX == -8192.0f && extent->offsetY == -13312.0f,
        "terrain extent header");
    Require(!ReadTerrainExtent(std::as_bytes(std::span(w3e.data(), 10))), "truncated terrain header must fail");
    const auto rejects = [&](std::size_t offset, const auto value) {
        auto invalid = w3e;
        std::memcpy(invalid.data() + offset, &value, sizeof(value));
        Require(!ReadTerrainExtent(std::as_bytes(std::span(invalid.data(), invalid.size()))),
            "invalid header version, table count, dimensions or coordinates must reject");
    };
    rejects(4, std::int32_t{12});
    rejects(13, std::int32_t{257});
    rejects(29, std::int32_t{0});
    rejects(29, std::numeric_limits<std::int32_t>::min());
    rejects(33, std::int32_t{2050});
    rejects(37, std::numeric_limits<float>::quiet_NaN());
    rejects(41, std::numeric_limits<float>::infinity());
    rejects(37, 16777216.0f); // 起点虽在边界，东端已越界。
}

void TestPlayerConfiguration(MapScriptInputs inputs) {
    // 原创双人样例：未启用自定义势力时仍有包含全部玩家的默认势力表。
    // 使用不连续的玩家编号，确保槽位编号与出生点数组索引不会混淆。
    MapData::MapSettings settings;
    settings.formatVersion = 25;
    settings.tileset = 'L';
    settings.players.resize(2);
    settings.players[0].number = 0;
    settings.players[0].controller = 1;
    settings.players[0].race = 1;
    settings.players[0].startX = 640.0f;
    settings.players[0].startY = 1280.0f;
    settings.players[1].number = 3;
    settings.players[1].controller = 1;
    settings.players[1].race = 2;
    settings.players[1].startX = -960.0f;
    settings.players[1].startY = -1920.0f;
    settings.forces.push_back({0u, 0xffffffffu, "Default force"});
    inputs.settings = &settings;

    // 0x1dc14 是实机失败图的选项组合：其中没有自定义势力位。
    // 优先级与自定义势力独立切换，覆盖普通图和自定义图的两种出生点布局。
    for (const std::uint32_t flags : {0u, 0x1dc14u, 0x40u, 0x1dc54u}) {
        for (const int priority : {0, 1, 2}) {
            settings.flags = flags;
            settings.players[0].allyLowPriorityMask = priority == 1 ? 8u : 0u;
            settings.players[0].allyHighPriorityMask = priority == 2 ? 8u : 0u;
            const auto generated = GenerateMapScript(inputs);
            Require(generated.Succeeded(), "player configuration should generate");
            const auto begin = generated.text.find("function config takes nothing returns nothing");
            const auto end = generated.text.find("endfunction", begin);
            Require(begin != std::string::npos && end != std::string::npos, "config function must exist");
            const auto config = generated.text.substr(begin, end - begin);
            const bool custom = (flags & 0x40u) != 0;
            Require((config.find("call InitCustomTeams(  )") != std::string::npos) == custom,
                "default forces must not place all melee players on one team");
            Require((config.find("call InitGenericPlayerSlots(  )") != std::string::npos) == !custom,
                "ordinary maps must restore lobby-controlled player slots");
            for (const auto number : {0, 3}) {
                Require((config.find("call SetPlayerSlotAvailable( Player(" + std::to_string(number)
                            + "), MAP_CONTROL_USER )") != std::string::npos) == !custom,
                    "ordinary slot availability must use the stored player number");
            }
            Require(config.find(priority == 0
                        ? "call SetGamePlacement( MAP_PLACEMENT_USE_MAP_SETTINGS )"
                        : "call SetGamePlacement( MAP_PLACEMENT_TEAMS_TOGETHER )") != std::string::npos,
                "game placement must follow either ally priority mask, independently of custom forces");
            Require(config.find("call SetTeams( 2 )") != std::string::npos
                    && config.find("call DefineStartLocation( 0, 640.0, 1280.0 )") != std::string::npos
                    && config.find("call DefineStartLocation( 1, -960.0, -1920.0 )") != std::string::npos,
                "configuration must retain team count and ordered start coordinates");
            const auto slots = config.find("call InitCustomPlayerSlots(  )");
            const auto teams = config.find(custom ? "call InitCustomTeams(  )" : "call InitGenericPlayerSlots(  )");
            const auto priorities = config.find("call InitAllyPriorities(  )");
            Require(slots < teams && teams < priorities && priorities != std::string::npos,
                "player slots, team configuration and ally priorities must retain WE initialization order");
        }
    }
    // 未知控制者/种族不能静默写成用户/随机种族。
    for (const auto& [controller, race] : {std::pair{0, 1}, {5, 1}, {1, -1}, {1, 5}}) {
        settings.flags = 0;
        settings.players[1].controller = controller;
        settings.players[1].race = race;
        const auto generated = GenerateMapScript(inputs);
        Require(!generated.Succeeded() && HasCode(generated.diagnostics, "MAP_PLAYER_ENUM_UNSUPPORTED")
                && generated.text.find("call SetPlayerController( Player(3)") == std::string::npos,
            "unknown player controller or race must fail instead of defaulting");
    }
}

void TestEnvironmentInitialization(MapScriptInputs inputs) {
    MapData::MapSettings settings;
    settings.formatVersion = 25;
    settings.tileset = 'V';
    settings.cameraMargins = {1, 2, 3, 4};
    settings.flags = 0x10000u;
    settings.environment.weather = {std::byte{0x52}, std::byte{0x41}, std::byte{0x6c}, std::byte{0x72}};
    inputs.settings = &settings;
    inputs.terrain = TerrainExtent{17, 25, -512.0f, -1024.0f};
    const auto weather = GenerateMapScript(inputs);
    Require(weather.Succeeded(), "global weather should generate with a valid terrain extent");
    const auto main = weather.text.find("function main takes nothing returns nothing\n    local weathereffect we\n");
    const auto water = weather.text.find("call SetWaterBaseColor(", main);
    const auto create = weather.text.find("set we = AddWeatherEffect( Rect(-512.0,-1024.0,1536.0,2048.0), 'RAlr' )", main);
    const auto enable = weather.text.find("call EnableWeatherEffect( we, true )", main);
    const auto sound = weather.text.find("call NewSoundEnvironment(", main);
    Require(main != std::string::npos && water < create && create < enable && enable < sound
            && sound != std::string::npos,
        "weather must cover the full terrain despite camera margins, after water tint and before sound setup");
    settings.flags = 0;
    Require(GenerateMapScript(inputs).text.find("AddWeatherEffect( Rect(-512.0,-1024.0,1536.0,2048.0), 'RAlr'")
            != std::string::npos,
        "global weather must not depend on fog, water tint or unrelated map flags");

    const auto rejectsTerrain = [&]() {
        const auto result = GenerateMapScript(inputs);
        bool located = false;
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.code == "MAP_WEATHER_TERRAIN_INVALID") {
                located = diagnostic.severity == DiagnosticSeverity::Error && diagnostic.origin
                    && diagnostic.origin->source == "war3map.w3e";
            }
        }
        Require(!result.Succeeded() && located && result.text.find("AddWeatherEffect(") == std::string::npos,
            "configured weather requires a located terrain error instead of a fabricated rectangle");
    };
    inputs.terrain.reset();
    rejectsTerrain();
    for (const auto& invalid : {TerrainExtent{0, 25, -512.0f, -1024.0f}, TerrainExtent{-1, 25, -512.0f, -1024.0f},
             TerrainExtent{17, 1, -512.0f, -1024.0f},
             TerrainExtent{17, 25, std::numeric_limits<float>::infinity(), -1024.0f},
             TerrainExtent{17, 25, -512.0f, std::numeric_limits<float>::quiet_NaN()},
             TerrainExtent{2, 25, 1.0e11f, -1024.0f},
             TerrainExtent{17, 25, std::numeric_limits<float>::max(), -1024.0f}}) {
        inputs.terrain = invalid;
        rejectsTerrain();
    }
    settings.environment.weather = {};
    inputs.terrain = TerrainExtent{std::numeric_limits<std::int32_t>::min(), 25, 0, 0};
    const auto invalidWithoutWeather = GenerateMapScript(inputs);
    Require(!invalidWithoutWeather.Succeeded() && HasCode(invalidWithoutWeather.diagnostics, "MAP_TERRAIN_INVALID"),
        "invalid extent must reject camera initialization even without global weather");
    inputs.terrain.reset();
    const auto noWeather = GenerateMapScript(inputs);
    Require(noWeather.Succeeded() && noWeather.text.find("AddWeatherEffect(") == std::string::npos,
        "maps without global weather must not require terrain or create an empty weather effect");

    const std::pair<char, const char*> lightGroups[] = {
        {'A', "Ashenvale"}, {'C', "Felwood"}, {'D', "Dungeon"}, {'G', "Underground"},
        {'J', "Dalaran"}, {'X', "Dalaran"}, {'B', "Lordaeron"}, {'F', "Lordaeron"},
        {'I', "Lordaeron"}, {'K', "Lordaeron"}, {'L', "Lordaeron"}, {'N', "Lordaeron"},
        {'O', "Lordaeron"}, {'Q', "Lordaeron"}, {'V', "Lordaeron"}, {'W', "Lordaeron"},
        {'Y', "Lordaeron"}, {'Z', "Lordaeron"}};
    for (const auto& [code, group] : lightGroups) {
        for (const bool custom : {false, true}) {
            settings.tileset = static_cast<std::uint8_t>(custom ? 'V' : code);
            settings.environment.lightEnvironment = static_cast<std::uint8_t>(custom ? code : 0);
            const auto result = GenerateMapScript(inputs);
            const std::string prefix = "Environment\\\\DNC\\\\DNC" + std::string(group) + "\\\\DNC" + group;
            Require(result.Succeeded()
                    && result.text.find(prefix + "Terrain\\\\DNC" + group + "Terrain.mdl") != std::string::npos
                    && result.text.find(prefix + "Unit\\\\DNC" + group + "Unit.mdl") != std::string::npos,
                "day/night models must use the explicit light environment or the terrain when unspecified");
            if (custom) Require(result.text.find("SetAmbientDaySound( \"VillageDay\" )") != std::string::npos,
                "overriding light models must not change the terrain's ambient sound");
        }
    }
    for (const bool custom : {false, true}) {
        settings.tileset = custom ? 'V' : '?';
        settings.environment.lightEnvironment = custom ? '?' : 0;
        const auto result = GenerateMapScript(inputs);
        Require(!result.Succeeded() && HasCode(result.diagnostics, "MAP_LIGHT_ENVIRONMENT_INVALID")
                && result.text.find("call SetDayNightModels(") == std::string::npos,
            "invalid explicit or inherited light environment must fail instead of guessing a model");
    }
}

void TestGoldMineInitialization(MapScriptInputs inputs) {
    MapData::UnitPlacements units;
    for (const auto& [rawcode, amount] : {std::pair{"ngol", 12500}, {"ugol", 0}, {"ngol", 20000}, {"hfoo", 54321}}) {
        MapData::UnitPlacement unit;
        unit.rawcode = rawcode;
        unit.owner = 15;
        unit.gold = amount;
        unit.creationId = static_cast<std::int32_t>(units.units.size());
        units.units.push_back(unit);
    }
    inputs.units = &units;
    const auto result = GenerateMapScript(inputs);
    Require(result.Succeeded(), "gold mine initialization should generate");
    for (const auto amount : {12500, 0, 20000}) {
        Require(result.text.find("call SetResourceAmount( u, " + std::to_string(amount) + " )") != std::string::npos,
            "gold mines must write their stored amount even for 12500 or an empty mine");
    }
    Require(result.text.find("call SetResourceAmount( u, 54321 )") == std::string::npos,
        "ordinary units must not acquire resource semantics from an unused placement field");
}

void TestUnreferencedPlacementWithoutIdentifier(MapScriptInputs inputs) {
    MapData::UnitPlacements units;
    MapData::UnitPlacement unit;
    unit.rawcode = "@001";
    unit.creationId = 42;
    unit.owner = 0;
    units.units.push_back(unit);
    inputs.units = &units;
    const auto result = GenerateMapScript(inputs);
    Require(result.Succeeded() && result.text.find("CreateUnit( p, '@001',") != std::string::npos,
        "unreferenced four-byte object remains creatable without a legal generated global identifier");
    Require(result.text.find("gg_unit_@001_") == std::string::npos,
        "unreferenced object must not receive an invalid generated global identifier");
}

void TestDestructableCreation(MapScriptInputs inputs) {
    MapData::DoodadPlacements doodads;
    MapData::DoodadPlacement automatic;
    automatic.rawcode = "DTg5";
    automatic.flags = 2;
    automatic.creationId = 7;
    automatic.itemSets.push_back({{}});
    doodads.doodads.push_back(automatic);
    MapData::DoodadPlacement gate;
    gate.rawcode = "DTg5";
    gate.creationId = 47;
    gate.scale = {1, 1, 1};
    gate.itemSets.push_back({{}});
    doodads.doodads.push_back(gate);
    MapData::DoodadPlacement raised = gate;
    raised.rawcode = "DTrx";
    raised.flags = 4;
    raised.creationId = 485;
    raised.position = {-4480, 10368, 256};
    raised.itemSets.push_back({{{"rman", 100}}});
    doodads.doodads.push_back(raised);
    Trigger::TriggerDocument triggers = *inputs.triggers;
    triggers.wct.customCode.text = "function Ref takes nothing returns nothing\ncall KillDestructable(gg_dest_DTg5_0047)\nendfunction\n";
    inputs.triggers = &triggers;
    inputs.doodads = &doodads;
    const auto generated = GenerateMapScript(inputs);
    Require(generated.Succeeded() && generated.text.find("destructable            gg_dest_DTg5_0047") != std::string::npos
        && generated.text.find("set gg_dest_DTg5_0047 = CreateDestructable( 'DTg5'") != std::string::npos
        && generated.text.find("CreateDestructableZ( 'DTrx', -4480.0, 10368.0, 256.0") != std::string::npos
        && generated.text.find("Doodad000002_DropItems") != std::string::npos
        && generated.text.find("call CreateAllDestructables(  )") != std::string::npos
        && generated.text.find("gg_dest_DTg5_0007") == std::string::npos
        && generated.text.find("Doodad000001_DropItems") == std::string::npos,
        "DOO script-created destructables must bind referenced globals, Z placement and drop tables without auto-loaded records");
    triggers.wct.customCode.text += "call KillDestructable(gg_dest_DTg5_0099)\n";
    const auto missing = GenerateMapScript(inputs);
    Require(!missing.Succeeded() && HasCode(missing.diagnostics, "DESTRUCTABLE_REFERENCE_UNBOUND"), "unbound destructable reference must fail");
    triggers.wct.customCode.text.clear();
    Trigger::ArgumentNode staleArgument;
    staleArgument.type = 1;
    staleArgument.value.text = "gg_dest_DTfx_0050";
    const auto argumentIndex = static_cast<std::uint32_t>(triggers.argumentNodes.size());
    triggers.argumentNodes.push_back(staleArgument);
    Trigger::EcaNode staleAction;
    staleAction.type = 2;
    staleAction.name.text = "KillDestructable";
    staleAction.enabled = 0;
    staleAction.arguments.push_back(argumentIndex);
    const auto ecaIndex = static_cast<std::uint32_t>(triggers.ecaNodes.size());
    triggers.ecaNodes.push_back(staleAction);
    triggers.triggers.front().ecas.push_back(ecaIndex);
    Require(GenerateMapScript(inputs).Succeeded(), "disabled stale GUI destructable reference must not block save");
    triggers.ecaNodes[ecaIndex].enabled = 1;
    const auto activeStale = GenerateMapScript(inputs);
    Require(!activeStale.Succeeded() && HasCode(activeStale.diagnostics, "DESTRUCTABLE_REFERENCE_UNBOUND"), "enabled stale GUI reference must fail");
    triggers.ecaNodes[ecaIndex].enabled = 0;
    triggers.ecaNodes[ecaIndex].children.push_back(ecaIndex);
    Require(GenerateMapScript(inputs).Succeeded(), "disabled ECA cycle must remain outside emitted graph");
    triggers.ecaNodes[ecaIndex].enabled = 1;
    const auto cyclic = GenerateMapScript(inputs);
    Require(!cyclic.Succeeded() && HasCode(cyclic.diagnostics, "TRIGGER_REFERENCE_GRAPH_INVALID"), "active ECA cycle must fail");
    triggers.ecaNodes[ecaIndex].children.clear();
    triggers.ecaNodes[ecaIndex].arguments.front() = static_cast<std::uint32_t>(triggers.argumentNodes.size());
    const auto outOfBounds = GenerateMapScript(inputs);
    Require(!outOfBounds.Succeeded() && HasCode(outOfBounds.diagnostics, "TRIGGER_REFERENCE_GRAPH_INVALID"), "active argument bounds must fail");
    triggers.triggers.front().ecas.pop_back();
    triggers.ecaNodes[ecaIndex].arguments.front() = argumentIndex;
    triggers.ecaNodes[ecaIndex].enabled = 0;
    triggers.ecaNodes[ecaIndex].childId = 1;
    Trigger::EcaNode block;
    block.type = 2;
    block.name.text = "IfThenElseMultiple";
    block.children.push_back(ecaIndex);
    triggers.ecaNodes.push_back(block);
    triggers.triggers.front().ecas.push_back(static_cast<std::uint32_t>(triggers.ecaNodes.size() - 1));
    Require(GenerateMapScript(inputs).Succeeded(), "disabled child action must not require its stale destructable");
    triggers.ecaNodes[ecaIndex].enabled = 1;
    const auto activeChild = GenerateMapScript(inputs);
    Require(!activeChild.Succeeded() && HasCode(activeChild.diagnostics, "DESTRUCTABLE_REFERENCE_UNBOUND"), "enabled child reference must fail");
    triggers.triggers.front().ecas.pop_back();
    doodads.doodads[0].itemSets[0].items.push_back({"rman", 100});
    const auto unsupportedDrop = GenerateMapScript(inputs);
    Require(!unsupportedDrop.Succeeded() && HasCode(unsupportedDrop.diagnostics, "DOODAD_DROP_UNSUPPORTED"), "auto-loaded drop choices must fail");
    doodads.doodads[0].itemSets[0].items.clear();
    doodads.doodads[1].lifePercent = 75;
    const auto unsupported = GenerateMapScript(inputs);
    Require(!unsupported.Succeeded() && HasCode(unsupported.diagnostics, "DESTRUCTABLE_INIT_UNSUPPORTED"), "unverified life must fail");
}

void TestSoundDefaultOverride(MapScriptInputs inputs) {
    MapData::SoundsDocument sounds;
    auto& sound = sounds.entries.emplace_back();
    sound.name = "gg_snd_Switch"; sound.filePath = "Switch.wav"; sound.flags = 2; sound.volume = 80; sound.channel = -1;
    sound.pitch = sound.minDistance = sound.maxDistance = sound.distanceCutoff = sound.coneAngleInside = sound.coneAngleOutside = 4294967296.0f;
    sound.coneOutsideVolume = -1; sound.coneOrientation.fill(4294967296.0f);
    inputs.sounds = &sounds;
    inputs.queries.soundLabel = [](std::string_view) -> std::optional<std::string> { return std::string("Switch"); };
    const auto generated = GenerateMapScript(inputs);
    Require(generated.Succeeded() && generated.text.find("SetSoundParamsFromLabel( gg_snd_Switch, \"Switch\" )") != std::string::npos
        && generated.text.find("SetSoundVolume( gg_snd_Switch, 80 )") != std::string::npos
        && generated.text.find("4294967296.0") == std::string::npos, "one explicit volume must retain label defaults without emitting W3S sentinels");
    sound.maxDistance = 10000.0f;
    const auto mixed = GenerateMapScript(inputs);
    Require(!mixed.Succeeded() && HasCode(mixed.diagnostics, "SOUND_DEFAULT_GROUP_MIXED"), "a partial distance override cannot be guessed");
}

void TestObjectCodeEditAndCompilation(const std::filesystem::path& root) {
    using namespace Trigger;
    WriteText(root / "ui/config", "base\n");
    WriteText(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nunit=0,1,1,Unit,unit\n"
        "abilcode=0,1,1,Ability,integer\nabilityalias=0,1,1,AbilityAlias,abilcode\nabilityalias2=0,1,1,AbilityAlias2,abilityalias\n"
        "customcode=0,1,1,PlainInteger,integer\nComparisonOperator=0,0,0,Operator\n[TriggerParams]\n"
        "OperatorGreater=0,ComparisonOperator,>,Greater\nAbilityPreset=0,abilcode,'A0E0',Ability\nNumericAbility=0,abilcode,1234,Numeric\n");
    WriteText(root / "ui/base/action.txt", "[RecordLevel]\n[[.args]]\ntype=integer\n"
        "[UseAbilityAlias]\nscript_name=RecordAbility\n[[.args]]\ntype=abilityalias2\n"
        "[UseCustomCode]\nscript_name=RecordLevel\n[[.args]]\ntype=customcode\n");
    WriteText(root / "ui/base/call.txt", "[GetTriggerUnit]\nreturns=unit\n"
        "[GetUnitAbilityLevel]\nreturns=integer\n[[.args]]\ntype=unit\n[[.args]]\ntype=abilcode\n");
    WriteText(root / "ui/base/condition.txt", "[OperatorCompareInteger]\nreturns=boolean\n"
        "[[.args]]\ntype=integer\n[[.args]]\ntype=ComparisonOperator\n[[.args]]\ntype=integer\n");
    auto loaded = UiCatalog::Load(root); Require(loaded.Succeeded(), "rawcode original catalog");
    auto empty = WtgWctCodec{}.CreateEmpty(*loaded.value); Require(empty.Succeeded(), "rawcode empty pair");
    auto document = std::move(*empty.value);
    const auto category = TriggerDirectoryEditor::AddCategory(document, "Original"); Require(category.Succeeded(), "rawcode category");
    const auto trigger = TriggerDocumentEditor::AddTrigger(document, *category.value, "AbilityCheck"); Require(trigger.Succeeded(), "rawcode trigger");
    const auto scalar = [](int kind, std::string value) { GraphArgumentValue result; result.scalar = {kind, std::move(value)}; return result; };
    const auto eca = [](std::string function, UiDefinitionKind kind, std::vector<GraphArgumentValue> arguments) {
        auto result = std::make_shared<GraphEcaValue>(); result->function = std::move(function); result->kind = kind;
        result->arguments = std::move(arguments); return result;
    };
    const auto call = [&](std::string function, std::vector<GraphArgumentValue> arguments = {}) {
        GraphArgumentValue result; result.call = eca(std::move(function), UiDefinitionKind::Call, std::move(arguments)); return result;
    };
    const auto append = [&](std::shared_ptr<GraphEcaValue> node) {
        auto binding = CaptureTriggerGraphBinding(document, *loaded.value); Require(binding.Succeeded(), "rawcode append binding");
        GraphParentExpectation parent{GraphParentKind::TriggerEcas, {GraphRecordKind::Trigger, *trigger.value, {}}, {}, {}};
        for (const auto index : document.triggers.front().ecas) parent.orderedChildren.push_back({GraphRecordKind::Eca, document.ecaNodes[index].id, {}});
        TriggerGraphEditBatch batch{*binding.value, {GraphInsertSubtree{parent, {}, std::move(node)}}};
        Require(ApplyTriggerGraphEditBatch(document, *loaded.value, batch).Succeeded(), "rawcode public graph edit succeeds");
    };
    append(eca("OperatorCompareInteger", UiDefinitionKind::Condition,
        {call("GetUnitAbilityLevel", {call("GetTriggerUnit"), scalar(3, "A0E0")}), scalar(0, "OperatorGreater"), scalar(3, "0")}));
    append(eca("RecordLevel", UiDefinitionKind::Action,
        {call("GetUnitAbilityLevel", {call("GetTriggerUnit"), scalar(3, "A0E0")})}));
    append(eca("UseAbilityAlias", UiDefinitionKind::Action, {scalar(3, "1234")}));
    append(eca("UseAbilityAlias", UiDefinitionKind::Action, {scalar(0, "AbilityPreset")}));
    append(eca("UseAbilityAlias", UiDefinitionKind::Action, {scalar(0, "NumericAbility")}));
    append(eca("UseCustomCode", UiDefinitionKind::Action, {scalar(3, "1234")}));
    auto binding = CaptureTriggerGraphBinding(document, *loaded.value); Require(binding.Succeeded(), "rawcode variable binding");
    TriggerGraphEditBatch variables{*binding.value, {
        GraphCreate{"object", GraphVariableValue{"ObjectNumber", "abilityalias2", false, 1, true, "1234"}, {GraphParentKind::Variables, {}, {}, {}}, {}},
        GraphCreate{"number", GraphVariableValue{"DecimalNumber", "integer", false, 1, true, "1234"},
            {GraphParentKind::Variables, {}, {{GraphRecordKind::Variable, 0, "object"}}, {}}, {}}}};
    Require(ApplyTriggerGraphEditBatch(document, *loaded.value, variables).Succeeded(), "rawcode alias default public edit");
    append(eca("UseAbilityAlias", UiDefinitionKind::Action, {scalar(1, "DecimalNumber")}));
    const auto encoded = WtgWctCodec{}.Serialize(document, *loaded.value); Require(encoded.Succeeded(), "rawcode graph pair serializes");
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, *loaded.value); Require(parsed.Succeeded(), "rawcode graph pair reopens");
    document = std::move(*parsed.value);
    auto previewDocument = document;
    // 窄预览不实现比较运算符；用同图中同一能力查询动作验证 literal lowering，不伪装整图后端。
    previewDocument.triggers.front().ecas.erase(previewDocument.triggers.front().ecas.begin());
    const auto preview = CompileTriggersToJass(previewDocument, *loaded.value);
    Require(preview.Succeeded() && preview.jassCode.find("GetUnitAbilityLevel(GetTriggerUnit(), 'A0E0')") != std::string::npos
        && preview.jassCode.find("RecordAbility('1234')") != std::string::npos
        && preview.jassCode.find("RecordAbility(1234)") != std::string::npos, "typed edit and narrow preview agree on rawcode/preset roles");
    MapScriptInputs inputs; inputs.triggers = &document; inputs.catalog = &*loaded.value;
    const auto generated = GenerateMapScript(inputs); Require(generated.Succeeded(), "rawcode complete script generation");
    std::string compact; for (const char byte : generated.text) if (std::string_view(" \t\r\n").find(byte) == std::string_view::npos) compact += byte;
    for (const auto* expected : {"GetUnitAbilityLevel(GetTriggerUnit(),'A0E0')", "callRecordAbility('1234')",
             "callRecordAbility('A0E0')", "callRecordAbility(1234)", "callRecordLevel(1234)", "callRecordAbility(udg_DecimalNumber)",
             "integerudg_ObjectNumber=0", "setudg_ObjectNumber='1234'", "setudg_DecimalNumber=1234"})
        Require(compact.find(expected) != std::string::npos, std::string("rawcode complete script lacks ") + expected);
    // 原创最小 API 只供实际生产编译器验证，不代表 Warcraft 游戏执行。
    const std::string common = "type unit extends handle\ntype trigger extends handle\ntype boolexpr extends handle\n"
        "type triggercondition extends handle\ntype triggeraction extends handle\ntype weathereffect extends handle\n"
        "native GetTriggerUnit takes nothing returns unit\nnative GetUnitAbilityLevel takes unit u, integer a returns integer\n"
        "native RecordLevel takes integer value returns nothing\nnative RecordAbility takes integer value returns nothing\n"
        "native CreateTrigger takes nothing returns trigger\nnative Condition takes code callback returns boolexpr\n"
        "native TriggerAddCondition takes trigger t, boolexpr condition returns triggercondition\n"
        "native TriggerAddAction takes trigger t, code callback returns triggeraction\n"
        "native DisableTrigger takes trigger t returns nothing\nnative ConditionalTriggerExecute takes trigger t returns nothing\n"
        "native InitBlizzard takes nothing returns nothing\nnative SetMapMusic takes string music, boolean looping, integer index returns nothing\n"
        "native SetPlayers takes integer players returns nothing\nnative SetTeams takes integer teams returns nothing\n"
        "native SetGamePlacement takes integer placement returns nothing\nnative InitGenericPlayerSlots takes nothing returns nothing\n"
        "globals\nconstant integer MAP_PLACEMENT_USE_MAP_SETTINGS = 0\nendglobals\n";
    WriteText(root / "compiler/jass/24/common.j", common);
    WriteText(root / "compiler/jass/24/blizzard.j", "// original empty map helper library\n");
    WriteText(root / "compiler/include/WaveForce.j", "// original force include\n");
    for (const auto* file : {"YDTrigger.h", "Globals.h", "Function.h"}) WriteText(root / "compiler/include/YDTrigger" / file, "// original empty header\n");
    ToolchainOptions options; options.toolRoot = root; options.workDir = root / "compiled"; options.scriptInjection = false;
    const auto compiled = CompileMapScript(generated.text, options, {});
    std::string details; for (const auto& diagnostic : compiled.diagnostics) details += diagnostic.code + ": " + diagnostic.message + "\n";
    Require(compiled.Succeeded() && compiled.syntaxChecked, "public rawcode edit reaches real full-script compilation:\n" + details);
    const auto ability = std::find_if(document.argumentNodes.begin(), document.argumentNodes.end(), [](const auto& arg) { return arg.type == 3 && arg.value.text == "A0E0"; });
    Require(ability != document.argumentNodes.end(), "rawcode active argument exists"); ability->value.text = "A'00";
    const auto invalid = GenerateMapScript(inputs);
    Require(!invalid.Succeeded() && HasCode(invalid.diagnostics, "TRIGGER_LITERAL_INVALID"), "production emitter also rejects malformed bypassed rawcode text");
}

void TestWritableLoopGeneration(const std::filesystem::path& root) {
    using namespace Trigger;
    WriteText(root / "ui/config", "base\n");
    WriteText(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n"
        "integervar=1,0,0,IntegerVariable,integer\nreal=0,1,1,Real,real\n");
    WriteText(root / "ui/base/action.txt", "[Marker]\n"
        "[ForLoopVarMultiple]\n[[.args]]\ntype=integervar\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[ForLoopVar]\n[[.args]]\ntype=integervar\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=code\n");
    WriteText(root / "ui/base/call.txt", "[LoopIndex]\nreturns=integer\n[LoopStart]\nreturns=integer\n[LoopEnd]\nreturns=integer\n");
    auto catalog = UiCatalog::Load(root); Require(catalog.Succeeded(), "loop original catalog");
    TriggerDocument document;
    auto category = TriggerDirectoryEditor::AddCategory(document, "Loops"); Require(category.Succeeded(), "loop category");
    Require(TriggerDocumentEditor::AddVariable(document, "Counter", "integer").Succeeded()
        && TriggerDocumentEditor::AddVariable(document, "Slots", "integer", true).Succeeded(), "loop declarations");
    StableId nextId = 10000;
    const auto argument = [&](std::int32_t kind, const std::string& text) {
        ArgumentNode node; node.id = nextId++; node.type = kind; node.value.text = text;
        document.argumentNodes.push_back(std::move(node)); return static_cast<std::uint32_t>(document.argumentNodes.size() - 1);
    };
    const auto call = [&](const std::string& name) {
        EcaNode node; node.id = nextId++; node.type = 3; node.name.text = name; node.enabled = 1;
        document.ecaNodes.push_back(std::move(node)); const auto nodeIndex = static_cast<std::uint32_t>(document.ecaNodes.size() - 1);
        const auto index = argument(2, name); document.argumentNodes[index].insertCall = 1; document.argumentNodes[index].callEca = nodeIndex;
        return index;
    };
    std::vector<std::uint32_t> targets;
    for (const bool single : {false, true}) for (const bool array : {false, true}) {
        const auto trigger = TriggerDocumentEditor::AddTrigger(document, *category.value, "Loop" + std::to_string(targets.size()));
        Require(trigger.Succeeded(), "loop trigger");
        const auto target = argument(1, array ? "Slots" : "Counter"); targets.push_back(target);
        if (array) { const auto index = call("LoopIndex"); document.argumentNodes[target].insertIndex = 1; document.argumentNodes[target].indexArgument = index; }
        EcaNode loop; loop.id = nextId++; loop.type = 2; loop.enabled = 1; loop.name.text = single ? "ForLoopVar" : "ForLoopVarMultiple";
        loop.arguments = {target, call("LoopStart"), call("LoopEnd")};
        EcaNode body; body.id = nextId++; body.type = 2; body.enabled = 1; body.name.text = "Marker";
        if (!single) body.childId = 0;
        document.ecaNodes.push_back(std::move(body)); const auto bodyIndex = static_cast<std::uint32_t>(document.ecaNodes.size() - 1);
        if (single) {
            const auto action = argument(2, "Marker"); document.argumentNodes[action].insertCall = 1; document.argumentNodes[action].callEca = bodyIndex;
            loop.arguments.push_back(action);
        } else loop.children.push_back(bodyIndex);
        document.ecaNodes.push_back(std::move(loop)); document.triggers.back().ecas.push_back(static_cast<std::uint32_t>(document.ecaNodes.size() - 1));
    }
    MapScriptInputs inputs; inputs.triggers = &document; inputs.catalog = &*catalog.value;
    const auto generated = GenerateMapScript(inputs); Require(generated.Succeeded(), "scalar and indexed loops generate");
    const auto occurrences = [&](std::string_view needle) {
        std::size_t count = 0;
        for (std::size_t position = 0; (position = generated.text.find(needle, position)) != std::string::npos; position += needle.size()) ++count;
        return count;
    };
    Require(occurrences("udg_Slots[LoopIndex()]") == 8 && occurrences("LoopStart()") == 4 && occurrences("LoopEnd()") == 4,
        "both loop forms retain four target occurrences and per-check upper bounds");
    for (const auto* target : {"udg_Counter", "udg_Slots[LoopIndex()]"}) {
        const auto initialize = generated.text.find("set " + std::string(target) + " = LoopStart()");
        const auto condition = generated.text.find("exitwhen " + std::string(target) + " > LoopEnd()", initialize);
        const auto body = generated.text.find("call Marker()", condition);
        const auto increment = generated.text.find("set " + std::string(target) + " = " + target + " + 1", body);
        Require(initialize < condition && condition < body && body < increment && increment != std::string::npos,
            "initialization, dynamic condition, body and two-occurrence increment keep template order");
    }
    auto aliases = document;
    for (const auto target : targets) aliases.argumentNodes[target].value.text.insert(0, "udg_");
    inputs.triggers = &aliases; const auto aliasScript = GenerateMapScript(inputs);
    Require(aliasScript.Succeeded() && aliasScript.text == generated.text, "raw declaration names and udg targets produce identical scripts");
    const auto rejects = [&](const auto& mutate, const char* code) {
        auto invalid = document; mutate(invalid); inputs.triggers = &invalid;
        const auto result = GenerateMapScript(inputs);
        Require(!result.Succeeded() && HasCode(result.diagnostics, code), "generation also enforces shared writable target qualification");
    };
    rejects([&](auto& invalid) { invalid.argumentNodes[targets[0]].type = 3; invalid.argumentNodes[targets[0]].value.text = "1"; }, "TRIGGER_WRITABLE_VARIABLE_REQUIRED");
    rejects([&](auto& invalid) { invalid.argumentNodes[targets[0]].insertCall = 1; }, "TRIGGER_WRITABLE_VARIABLE_REQUIRED");
    rejects([&](auto& invalid) { invalid.argumentNodes[targets[0]].value.text = "Missing"; }, "TRIGGER_WRITABLE_VARIABLE_UNKNOWN");
    rejects([&](auto& invalid) { invalid.variables[0].typeName.text = "real"; }, "TRIGGER_WRITABLE_VARIABLE_TYPE");
    rejects([&](auto& invalid) { invalid.variables[0].isArray = 1; }, "TRIGGER_WRITABLE_VARIABLE_SHAPE");
    rejects([&](auto& invalid) {
        auto duplicate = invalid.variables[0]; duplicate.id = nextId++; duplicate.name.text = "udg_Counter"; invalid.variables.push_back(duplicate);
        invalid.argumentNodes[targets[0]].value.text = "udg_Counter";
    }, "TRIGGER_WRITABLE_VARIABLE_AMBIGUOUS");

    // 原创最小 API 验证最终 JASS 可编译；不把它当作原版 GUI 或 VM 副作用顺序的证明。
    WriteText(root / "compiler/jass/24/common.j", "type trigger extends handle\ntype triggeraction extends handle\ntype weathereffect extends handle\n"
        "native LoopIndex takes nothing returns integer\nnative LoopStart takes nothing returns integer\nnative LoopEnd takes nothing returns integer\n"
        "native Marker takes nothing returns nothing\nnative CreateTrigger takes nothing returns trigger\n"
        "native TriggerAddAction takes trigger t, code callback returns triggeraction\nnative DisableTrigger takes trigger t returns nothing\n"
        "native ConditionalTriggerExecute takes trigger t returns nothing\nnative InitBlizzard takes nothing returns nothing\n"
        "native SetMapMusic takes string music, boolean looping, integer index returns nothing\nnative SetPlayers takes integer players returns nothing\n"
        "native SetTeams takes integer teams returns nothing\nnative SetGamePlacement takes integer placement returns nothing\n"
        "native InitGenericPlayerSlots takes nothing returns nothing\nglobals\nconstant integer MAP_PLACEMENT_USE_MAP_SETTINGS = 0\nendglobals\n");
    WriteText(root / "compiler/jass/24/blizzard.j", "// original loop helper library\n");
    WriteText(root / "compiler/include/WaveForce.j", "// original force include\n");
    for (const auto* file : {"YDTrigger.h", "Globals.h", "Function.h"}) WriteText(root / "compiler/include/YDTrigger" / file, "// original empty header\n");
    ToolchainOptions options; options.toolRoot = root; options.workDir = root / "compiled"; options.scriptInjection = false;
    const auto compiled = CompileMapScript(generated.text, options, {});
    std::string details; for (const auto& diagnostic : compiled.diagnostics) details += diagnostic.code + ": " + diagnostic.message + "\n";
    Require(compiled.Succeeded() && compiled.syntaxChecked, "indexed loops reach final full-script compiler:\n" + details);
}

void TestGenerate(const std::filesystem::path& root) {
    WriteText(root / "ui" / "config", "base\r\n");
    WriteText(root / "ui" / "base" / "define.txt", "[TriggerTypes]\r\ninteger=0,1,1,整数,integer\r\n");
    WriteText(root / "ui" / "base" / "event.txt", "[TimerEvent]\r\nscript_name=TriggerRegisterTimerEvent\r\n[[.args]]\r\ntype=real\r\n");
    WriteText(root / "ui" / "base" / "condition.txt", "[AndMultiple]\r\nreturns=boolean\r\n");
    WriteText(root / "ui" / "base" / "action.txt", "[DoNothing]\r\n");
    WriteText(root / "ui" / "base" / "call.txt", "[GetLastCreated]\r\nreturns=unit\r\nscript_name=GetLastCreatedUnit\r\n");
    auto catalog = Trigger::UiCatalog::Load(root);
    Require(catalog.Succeeded(), "synthetic catalog did not load");
    Trigger::TriggerDocument document;
    Trigger::CategoryRecord category;
    category.id = 1;
    category.categoryId = 1;
    category.name.text = "Category";
    document.categories.push_back(category);
    auto hello = Trigger::TriggerDocumentEditor::AddTrigger(document, 1, "Hello World");
    Require(hello.Succeeded(), "add trigger");
    Require(Trigger::TriggerDocumentEditor::AppendRootEca(document, *catalog.value, *hello.value, Trigger::UiDefinitionKind::Action, "DoNothing")
                .Succeeded(), "append action");

    MapScriptInputs inputs;
    inputs.triggers = &document;
    inputs.catalog = &*catalog.value;
    const auto result = GenerateMapScript(inputs);
    Require(result.Succeeded(), "generation failed");
    for (const std::string_view needle : {"trigger                 gg_trg_Hello_World         = null", "function Trig_Hello_WorldActions takes nothing returns nothing",
             "    call DoNothing()", "function InitTrig_Hello_World takes nothing returns nothing", "    set gg_trg_Hello_World = CreateTrigger()",
             "function InitCustomTriggers takes nothing returns nothing", "    call InitTrig_Hello_World(", "function main takes nothing returns nothing",
             "function config takes nothing returns nothing"}) {
        Require(result.text.find(needle) != std::string::npos, "generated script lacks: " + std::string(needle) + "\n" + result.text);
    }
    Require(result.triggerCount == 1, "trigger count");
    Require(result.authorCode.size() == 1 && result.authorCode[0].triggerId == 0
        && result.authorCode[0].byteLength == 0 && result.authorCode[0].byteOffset <= result.text.size(),
        "empty global header has an actual emission boundary");
    // 相同正文出现两次，映射仍由各次真实写出位置记录；不能用find把两段映成同一处。
    auto mappedDocument = document;
    mappedDocument.wct.customCode.text = "// identical author body\r\n";
    mappedDocument.triggers.front().customTextFlag = 1;
    mappedDocument.triggers.front().customText.text = mappedDocument.wct.customCode.text;
    MapScriptInputs mappedInputs = inputs; mappedInputs.triggers = &mappedDocument;
    const auto mapped = GenerateMapScript(mappedInputs);
    Require(mapped.Succeeded() && mapped.authorCode.size() == 2
        && mapped.authorCode[0].triggerId == 0 && mapped.authorCode[1].triggerId == mappedDocument.triggers.front().id
        && mapped.authorCode[0].byteOffset < mapped.authorCode[1].byteOffset, "author spans retain stable emission identities");
    for (const auto& span : mapped.authorCode) Require(span.byteLength == mappedDocument.wct.customCode.text.size()
        && mapped.text.substr(span.byteOffset, span.byteLength) == mappedDocument.wct.customCode.text,
        "actual spans remain exact after storage-switch insertion");
    mappedDocument.triggers.front().enabled = 0;
    Require(GenerateMapScript(mappedInputs).authorCode.size() == 1, "disabled author body is not mapped as emitted code");
    // 没有用到存储：由生成器关闭 YDTrigger 存储系统（代替 YDWE 每次保存改写的 Import.h）。
    const auto storageSwitch = result.text.find("#define DISABLE_SAVE_LOAD_SYSTEM\n#include <YDTrigger/YDTrigger.h>");
    Require(storageSwitch != std::string::npos && result.text.find("Import.h") == std::string::npos, "storage switch replaces Import.h");
    document.wct.customCode.text = "function Custom takes nothing returns nothing\n    call YDLocal1Release()\nendfunction\n";
    const auto customStorage = GenerateMapScript(inputs);
    Require(customStorage.text.find("DISABLE_SAVE_LOAD_SYSTEM") == std::string::npos, "hand-written storage macros keep the system");
    document.wct.customCode.text.clear();

    // 绕过编辑层直接写入同脚本名的触发器（模拟外部工具写出的数据）：生成器必须报错。
    Trigger::TriggerRecord clash = document.triggers.front();
    clash.id = 999;
    clash.name.text = "Hello_World";
    clash.ecas.clear();
    document.triggers.push_back(clash);
    const auto duplicated = GenerateMapScript(inputs);
    Require(!duplicated.Succeeded() && HasCode(duplicated.diagnostics, "TRIGGER_SCRIPT_NAME_DUPLICATE"), "duplicate script names must fail");
    document.triggers.pop_back();
    auto chinese = Trigger::TriggerDocumentEditor::AddTrigger(document, 1, "技能");
    Require(chinese.Succeeded(), "Chinese trigger name");
    Require(Trigger::TriggerDocumentEditor::AppendRootEca(document, *catalog.value, *chinese.value,
        Trigger::UiDefinitionKind::Action, "DoNothing").Succeeded(), "Chinese trigger action");
    const auto nonAscii = GenerateMapScript(inputs);
    Require(nonAscii.Succeeded() && nonAscii.text.find("gg_trg_______u") != std::string::npos
        && nonAscii.text.find("function InitTrig_______u takes nothing returns nothing") != std::string::npos,
        "UTF-8 Chinese trigger name must use the observed byte-wise WE/YDWE symbol rule");

    // 独立切换 TFT 地形雾、物品分类、水色位，防止不相关的地图选项改变生成脚本。
    MapData::MapSettings settings;
    settings.formatVersion = 25;
    settings.tileset = 'L';
    settings.environment.fogStart = 1200.0f;
    settings.environment.fogEnd = 5000.0f;
    settings.environment.fogDensity = 0.5f;
    inputs.settings = &settings;
    for (const std::uint32_t flags : {0u, 0x2000u, 0x4000u, 0x8000u, 0x10000u,
             0xa000u, 0x12000u, 0x1e000u, 0x80000000u}) {
        settings.flags = flags;
        const auto environment = GenerateMapScript(inputs);
        Require(environment.Succeeded(), "environment flag combination should generate");
        Require((environment.text.find("call SetTerrainFogEx(") != std::string::npos)
                == ((flags & 0x2000u) != 0),
            "terrain fog must follow the v25 fog flag independently of item classification");
        Require((environment.text.find("call SetWaterBaseColor(") != std::string::npos)
                == ((flags & 0x10000u) != 0),
            "water tint must follow its own v25 flag independently of terrain fog");
    }
    TestPlayerConfiguration(inputs);
    TestEnvironmentInitialization(inputs);
    TestGoldMineInitialization(inputs);
    TestUnreferencedPlacementWithoutIdentifier(inputs);
    TestDestructableCreation(inputs);
    TestSoundDefaultOverride(inputs);
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "war3-map-script-build-tests";
    try {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        TestDeclaredSourceEncoding();
        TestStringHash();
        TestYDTriggerStorage();
        TestTemplates();
        TestTemplateImports(root / "imports");
        TestInject(root / "inject");
        TestToolRoot(root / "tools");
        TestToolchainLayouts(root / "layouts");
        TestFrameworkPipeline(root / "framework");
        TestNamesAndTerrain();
        TestObjectCodeEditAndCompilation(root / "object-code");
        TestWritableLoopGeneration(root / "loop-targets");
        TestGenerate(root / "generate");
        std::filesystem::remove_all(root, error);
        std::cout << "ScriptBuild: 12 scenario groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ScriptBuild: " << exception.what() << '\n';
        return 1;
    }
}
