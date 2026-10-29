#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include "War3MapCore/Script/TriggerStorageCapabilities.h"
#include "../MapDataFixtures.h"
#include "../AuxiliarySaveFixtures.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Services;

void Require(bool condition, std::string_view reason) {
    if (!condition) throw std::runtime_error(std::string(reason));
}

std::vector<std::byte> OriginalBytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void WriteOriginalText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(output.good(), "create original temporary text source");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    Require(output.good(), "write complete original temporary text source");
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<unsigned> sequence{0};
        parent_ = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        path_ = parent_ / (std::string(kPrefix) + std::to_string(GetCurrentProcessId()) + "-"
            + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "create sole original modern-project fixture directory");
    }
    ~TemporaryDirectory() {
        // 只删除本测试创建的、已核对绝对父目录与唯一前缀的临时目录。
        if (path_.is_absolute() && path_.parent_path() == parent_
            && path_.filename().string().starts_with(kPrefix)) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    const std::filesystem::path& Path() const noexcept { return path_; }
private:
    static constexpr std::string_view kPrefix = "Forge-modern-project-check-original-";
    std::filesystem::path parent_, path_;
};

std::vector<std::byte> OriginalEmptyWtg() {
    MapDataFixtures::Fixture fixture;
    fixture.Raw("WTG!"); fixture.Int(7); fixture.Int(0); fixture.Int(2); fixture.Int(0); fixture.Int(0);
    return std::move(fixture.bytes);
}

std::vector<std::byte> OriginalHeaderWct(std::string_view header) {
    MapDataFixtures::Fixture fixture;
    fixture.Int(1); fixture.String(""); fixture.Int(static_cast<std::int32_t>(header.size() + 1));
    fixture.String(header); fixture.Int(0);
    return std::move(fixture.bytes);
}

std::vector<std::byte> OriginalClassicSettings() {
    auto parsed = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(parsed.Succeeded() && parsed.value && parsed.value->FullyParsed(), "parse original complete W3I v25 fixture");
    auto& settings = *parsed.value;
    settings.mapName = "Original modern checker fixture";
    settings.author = "Original service regression";
    settings.description = "No authored map data or Warcraft runtime is used";
    settings.recommendedPlayers = "0";
    settings.flags = 0;
    settings.environment = {};
    settings.players.clear(); settings.forces.clear(); settings.custom.emplace();
    auto encoded = settings.Serialize();
    Require(encoded.Succeeded() && encoded.value, "serialize minimal original W3I v25 through production codec");
    const auto verified = MapData::MapSettings::Parse(*encoded.value);
    Require(verified.Succeeded() && verified.value && verified.value->FullyParsed()
        && verified.value->SourceFormatVersion() == 25 && verified.value->scriptLanguage == 0,
        "original settings must retain the fully parsed classic JASS layout");
    return std::move(*encoded.value);
}

// 原创最小声明仅覆盖本夹具生产整图生成器实际发射的空地图初始化调用；不是运行库替代品。
constexpr std::string_view kOriginalCommon =
    "type player extends handle\n"
    "type unit extends handle\n"
    "type trigger extends handle\n"
    "type sound extends handle\n"
    "type rect extends handle\n"
    "type weathereffect extends handle\n"
    "globals\n"
    "constant integer CAMERA_MARGIN_LEFT = 0\n"
    "constant integer CAMERA_MARGIN_RIGHT = 1\n"
    "constant integer CAMERA_MARGIN_TOP = 2\n"
    "constant integer CAMERA_MARGIN_BOTTOM = 3\n"
    "constant integer MAP_PLACEMENT_USE_MAP_SETTINGS = 0\n"
    "endglobals\n"
    "native GetCameraMargin takes integer side returns real\n"
    "native SetCameraBounds takes real a, real b, real c, real d, real e, real f, real g, real h returns nothing\n"
    "native SetDayNightModels takes string terrainModel, string unitModel returns nothing\n"
    "native NewSoundEnvironment takes string name returns nothing\n"
    "native SetAmbientDaySound takes string name returns nothing\n"
    "native SetAmbientNightSound takes string name returns nothing\n"
    "native SetMapMusic takes string name, boolean looping, integer index returns nothing\n"
    "native SetMapName takes string name returns nothing\n"
    "native SetMapDescription takes string description returns nothing\n"
    "native SetPlayers takes integer count returns nothing\n"
    "native SetTeams takes integer count returns nothing\n"
    "native SetGamePlacement takes integer placement returns nothing\n"
    "type triggeraction extends handle\n"
    "native CreateTrigger takes nothing returns trigger\n"
    "native TriggerAddAction takes trigger whichTrigger, code actionFunc returns triggeraction\n";

constexpr std::string_view kOriginalBlizzard =
    "function InitBlizzard takes nothing returns nothing\nendfunction\n"
    "function InitGenericPlayerSlots takes nothing returns nothing\nendfunction\n";

// cfg 注入实际追加在整图正文之后。原创库显式声明 vJass 依赖，由生产展开器排序；
// 不能让作者正文前向调用稍后才定义的普通 JASS 函数，也不能通过补 native 绕过顺序守卫。
constexpr std::string_view kBaseInjection =
    "library BaseProbe\n"
    "function BaseInjectedProbe takes nothing returns integer\nreturn 3\nendfunction\n"
    "endlibrary\n";
constexpr std::string_view kAddonInjection =
    "library AddonProbe\n"
    "function AddonInjectedProbe takes nothing returns integer\nreturn 7\nendfunction\n"
    "endlibrary\n";
constexpr std::string_view kChangedAddonInjection =
    "library AddonProbe\n"
    "function AddonInjectedProbe takes nothing returns integer\nreturn 1907\nendfunction\n"
    "endlibrary\n";
constexpr std::string_view kQuotedInclude =
    "function IncludedProbe takes nothing returns integer\nreturn 11\nendfunction\n";
constexpr std::string_view kAuthorHeader =
    "library AuthorFixture requires BaseProbe, AddonProbe\n"
    "#include \"Nested/IncludedProbe.h\"\n"
    "function AuthorProbe takes nothing returns integer\n"
    "return BaseInjectedProbe() + AddonInjectedProbe() + IncludedProbe()\n"
    "endfunction\n"
    "endlibrary\n";

// 原创强制包含：与 YDWE 框架公开的写法相同，YDNL 先宏展开为模板，再由模板求值输出换行。
constexpr std::string_view kTemplateForcedInclude =
    "#define YDNL <?='\\n'?>\n"
    "#define UNSUPPORTED_TEMPLATE <? local slk = require 'slk' ?>\n";

class ModernProjectFixture final {
public:
    explicit ModernProjectFixture(std::string_view forcedInclude = "// original forced include\n", bool storageProbe = false) {
        tool = temporary.Path() / "tool";
        source = temporary.Path() / "maps" / "original.w3x";
        baseLibrary = tool / "ui/base/jass/BaseProbe.j";
        addonLibrary = tool / "ui/addon/jass/AddonProbe.j";
        quotedInclude = tool / "ui/addon/jass/Nested/IncludedProbe.h";
        WriteOriginalText(tool / "ui/config", "base\naddon\n");
        WriteOriginalText(tool / "ui/base/define.txt", "[TriggerTypes]\ntrigger=0,1,1,触发器\n");
        WriteOriginalText(tool / "ui/addon/define.txt", "[TriggerTypes]\nProbeInteger=0,1,1,原创类型,integer\n");
        // T 检查用：一个有实现的动作（原创 blizzard 夹具中的函数）与一个脚本里不存在的动作。
        WriteOriginalText(tool / "ui/addon/action.txt", "[InitBlizzard]\n[MissingProbeAction]\n");
        if (storageProbe) {
            WriteOriginalText(tool / "ui/addon/define.txt", "[TriggerTypes]\nProbeInteger=0,1,1,原创类型,integer\n"
                "integer=0,1,1,Integer,integer\nhandle=0,1,1,Handle,handle\nstring=0,1,1,String,string\n"
                "typename=1,0,0,Type,string\nNull=0,0,0,Value\nAnyReturnType=0,0,0,Any\n"
                "[TriggerParams]\ntypename01_integer=0,typename,1,Integer\ntypename39_handle=0,typename,39,Handle\n");
            WriteOriginalText(tool / "ui/addon/action.txt", "[InitBlizzard]\n[MissingProbeAction]\n[RecordInteger]\n[[.args]]\ntype=integer\n");
            WriteOriginalText(tool / "ui/addon/call.txt", "[GetHandleId]\nreturns=integer\n[[.args]]\ntype=handle\n"
                "[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=string\n");
        }
        WriteOriginalText(tool / "compiler/jass/24/common.j", kOriginalCommon);
        if (storageProbe) WriteOriginalText(tool / "compiler/jass/24/common.j", std::string(kOriginalCommon)
            + "native GetHandleId takes handle value returns integer\nnative RecordInteger takes integer value returns nothing\n");
        WriteOriginalText(tool / "compiler/jass/24/blizzard.j", kOriginalBlizzard);
        WriteOriginalText(tool / "compiler/include/WaveForce.i", forcedInclude);
        for (const auto* name : {"YDTrigger.h", "Globals.h", "Function.h"}) {
            WriteOriginalText(tool / "compiler/include/YDTrigger" / name, "// original empty generator include\n");
        }
        WriteOriginalText(tool / "ui/base/jass/BaseProbe.cfg", "[general]\nBaseInjectedProbe\n");
        WriteOriginalText(baseLibrary, kBaseInjection);
        WriteOriginalText(tool / "ui/addon/jass/AddonProbe.cfg", "[general]\nAddonInjectedProbe\n");
        WriteOriginalText(addonLibrary, kAddonInjection);
        WriteOriginalText(quotedInclude, kQuotedInclude);
        auto catalog = Trigger::UiCatalog::Load(tool);
        Require(catalog.Succeeded() && catalog.value && catalog.value->Layers().size() == 2,
            "load the actual two enabled original modern UI layers");
        originalWtg = OriginalEmptyWtg(); originalWct = OriginalHeaderWct(kAuthorHeader);
        auto document = Trigger::WtgWctCodec{}.Parse(originalWtg, originalWct, *catalog.value);
        Require(document.Succeeded() && document.value, "parse the original sealed WTG and WCT together");
        const std::vector<MapArchiveMember> members{{"war3map.w3i", OriginalClassicSettings()},
            {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
            {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
            {"war3map.wtg", originalWtg}, {"war3map.wct", originalWct},
            {"war3map.j", OriginalBytes("// original source script; shared checker must generate its own private candidate\n")}};
        std::filesystem::create_directories(source.parent_path());
        const auto written = WriteVerifiedArchive(source, members);
        Require(written.Succeeded() && written.value, "write and read back the sole original small map archive");
        sourceSha = written.value->sha256;
        const auto opened = MapArchiveSession::Open(source);
        Require(opened.Succeeded() && opened.value && opened.value->SourceSha256() == sourceSha,
            "bind the actual original archive SHA");
        auto map = MapDocument::FromArchive(*opened.value);
        const auto initialization = map.InitializationSourceStatus();
        if (!initialization.Succeeded()) {
            std::string reason = "source archive supplies complete classic initialization data";
            for (const auto& diagnostic : initialization.diagnostics) reason += "\n" + diagnostic.code + " " + diagnostic.message;
            throw std::runtime_error(reason);
        }
        Require(map.Settings() && map.Settings()->formatVersion == 25
            && map.Units() && map.Units()->units.empty()
            && map.Doodads() && map.Doodads()->doodads.empty() && map.Doodads()->specialDoodads.empty(),
            "source archive supplies parsed W3I25 and actual empty native placement members");
        auto snapshot = std::make_shared<CodeCheckProjectSnapshot>();
        snapshot->binding = "original-modern-project/" + sourceSha;
        snapshot->revision = 1; snapshot->sourcePath = source; snapshot->sourceSha256 = sourceSha;
        snapshot->toolRoot = tool; snapshot->uiRoot = tool / "ui";
        snapshot->triggers = std::make_shared<const Trigger::TriggerDocument>(std::move(*document.value));
        snapshot->catalog = std::make_shared<const Trigger::UiCatalog>(std::move(*catalog.value));
        snapshot->map = std::move(map);
        frozen = std::move(snapshot);
    }

    void RequireSourceUnchanged() const {
        const auto hash = Sha256File(source);
        Require(hash.Succeeded() && hash.value && *hash.value == sourceSha, "shared checks must not modify the original map bytes");
        const auto encoded = Trigger::WtgWctCodec{}.Serialize(*frozen->triggers, *frozen->catalog);
        Require(encoded.Succeeded() && encoded.value && encoded.value->wtg == originalWtg && encoded.value->wct == originalWct,
            "private draft checking must not publish changes to the sealed live trigger document");
    }

    TemporaryDirectory temporary;
    std::filesystem::path tool, source, baseLibrary, addonLibrary, quotedInclude;
    std::string sourceSha;
    std::vector<std::byte> originalWtg, originalWct;
    std::shared_ptr<const CodeCheckProjectSnapshot> frozen;
};

void RequireFull(const CodeCheckResult& result, std::string_view label) {
    if (result.Succeeded() && result.completed && result.syntaxChecked && result.semanticChecked
        && result.contextComplete && !result.cancelled && result.declarations.completed) return;
    std::string message(label);
    message += ": reason=" + result.reason;
    for (const auto& diagnostic : result.diagnostics) {
        message += "\n" + diagnostic.code + " " + diagnostic.source + ":" + std::to_string(diagnostic.line)
            + " " + diagnostic.message;
    }
    throw std::runtime_error(message);
}

bool HasOwnedSource(const CodeCheckResult& result, const std::filesystem::path& path, std::string_view text) {
    return std::any_of(result.externalSources.begin(), result.externalSources.end(), [&](const auto& source) {
        return source.name == PathText(path) && source.text == text;
    });
}

bool HasActualDeclaration(const CodeCheckResult& result, std::string_view name, const std::filesystem::path& path,
    std::uint32_t sourceLine = 1) {
    return std::any_of(result.declarations.symbols.begin(), result.declarations.symbols.end(), [&](const auto& symbol) {
        return symbol.name == name && symbol.source == PathText(path) && symbol.line == sourceLine
            && symbol.kind == Jass::DeclarationKind::Function && symbol.type == "integer";
    });
}

void ActualModernLayerSourcesAndRepeatedCache() {
    ModernProjectFixture fixture;
    CodeCheckProjectCache cache;
    for (const auto generation : {101u, 102u}) {
        const auto checked = CheckProjectCode(fixture.frozen, cache, {}, std::string(kAuthorHeader), CodeLanguage::VJass, generation);
        RequireFull(checked, "quoted enabled-layer include and both actual cfg libraries must have complete project semantics");
        Require(checked.generation == generation && checked.contextBinding == fixture.frozen->binding,
            "same cache recheck must retain the requested generation and actual snapshot binding");
        Require(checked.externalSourcesComplete && HasOwnedSource(checked, fixture.baseLibrary, kBaseInjection)
            && HasOwnedSource(checked, fixture.addonLibrary, kAddonInjection)
            && HasOwnedSource(checked, fixture.quotedInclude, kQuotedInclude),
            "actual checked injection and quoted include bytes must be owned and navigable");
        Require(HasActualDeclaration(checked, "BaseInjectedProbe", fixture.baseLibrary, 2)
            && HasActualDeclaration(checked, "AddonInjectedProbe", fixture.addonLibrary, 2)
            && HasActualDeclaration(checked, "IncludedProbe", fixture.quotedInclude),
            "shared production parser must index real library/include declarations at their own source lines");
        fixture.RequireSourceUnchanged();
    }
}

void ActualLayerLibraryChangeRejectsOldContextThenRecaptures() {
    ModernProjectFixture fixture;
    CodeCheckProjectCache cache;
    RequireFull(CheckProjectCode(fixture.frozen, cache, {}, std::string(kAuthorHeader), CodeLanguage::VJass, 201),
        "freeze original modern library identity");
    WriteOriginalText(fixture.addonLibrary, kChangedAddonInjection);
    const auto stale = CheckProjectCode(fixture.frozen, cache, {}, std::string(kAuthorHeader), CodeLanguage::VJass, 202);
    Require(!stale.semanticChecked && !stale.contextComplete && !stale.reason.empty()
        && !HasOwnedSource(stale, fixture.addonLibrary, kAddonInjection),
        "changed actual UI J source must not reuse old full-semantic context or stale navigation bytes");
    fixture.RequireSourceUnchanged();
    const auto recaptured = CheckProjectCode(fixture.frozen, cache, {}, std::string(kAuthorHeader), CodeLanguage::VJass, 203);
    RequireFull(recaptured, "next actual request must explicitly recapture changed enabled-layer source");
    Require(HasOwnedSource(recaptured, fixture.addonLibrary, kChangedAddonInjection)
        && !HasOwnedSource(recaptured, fixture.addonLibrary, kAddonInjection)
        && HasActualDeclaration(recaptured, "AddonInjectedProbe", fixture.addonLibrary, 2),
        "recaptured full context must carry only the actual new library bytes and original source identity");
    fixture.RequireSourceUnchanged();
}

void UnboundRootsCannotSupplyQuotedIncludesOrInjection() {
    ModernProjectFixture fixture;
    CodeCheckProjectCache cache;
    const auto outside = fixture.temporary.Path() / "unbound/Leak.h";
    constexpr std::string_view leak = "function UnboundIncludeProbe takes nothing returns integer\nreturn 71\nendfunction\n";
    WriteOriginalText(outside, leak);
    const auto unenabled = fixture.tool / "ui/unenabled/jass/UnboundProbe.j";
    constexpr std::string_view unboundInjection = "function UnboundInjectedProbe takes nothing returns integer\nreturn 73\nendfunction\n";
    WriteOriginalText(unenabled.parent_path() / "UnboundProbe.cfg", "[general]\nUnboundInjectedProbe\n");
    WriteOriginalText(unenabled, unboundInjection);
    // 文件确实存在；拒绝依赖来源资格，而不是碰巧找不到测试文件。
    Require(std::filesystem::is_regular_file(outside) && std::filesystem::is_regular_file(unenabled),
        "original unbound negative sources really exist on disk");
    const std::string outsideDraft = "#include \"../unbound/Leak.h\"\n"
        "function AuthorProbe takes nothing returns integer\nreturn UnboundIncludeProbe()\nendfunction\n";
    const auto include = CheckProjectCode(fixture.frozen, cache, {}, outsideDraft, CodeLanguage::VJass, 301);
    Require(!include.contextComplete && !include.semanticChecked && !include.reason.empty()
        && !HasOwnedSource(include, outside, leak)
        && !HasActualDeclaration(include, "UnboundIncludeProbe", outside),
        "quoted parent traversal cannot read an existing external root without a binding");
    const std::string injectionDraft = "function AuthorProbe takes nothing returns integer\n"
        "return UnboundInjectedProbe()\nendfunction\n";
    const auto injection = CheckProjectCode(fixture.frozen, cache, {}, injectionDraft, CodeLanguage::VJass, 302);
    Require(injection.completed && injection.syntaxChecked && injection.semanticChecked && injection.contextComplete
        && !injection.Succeeded() && !HasOwnedSource(injection, unenabled, unboundInjection)
        && std::any_of(injection.diagnostics.begin(), injection.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.message.find("UnboundInjectedProbe") != std::string::npos;
        }), "a disabled UI layer must not inject a real but unauthorized function to satisfy semantics");
    fixture.RequireSourceUnchanged();
}

// 作者代码里的 YDNL 必须像保存链一样展开后再检查，不能把未展开的宏名当成多余的词；
// 展开出的行仍报在作者原行，编辑器不执行的模板如实退回。
void TemplateMacrosExpandBeforeCheckingAndKeepAuthorLines() {
    ModernProjectFixture fixture(kTemplateForcedInclude);
    CodeCheckProjectCache cache;
    const std::string expanded =
        "library AuthorFixture requires BaseProbe, AddonProbe\n"
        "#include \"Nested/IncludedProbe.h\"\n"
        "function AuthorProbe takes nothing returns integer\n"
        "local integer base = BaseInjectedProbe() YDNL return base + AddonInjectedProbe() + IncludedProbe()\n"
        "endfunction\n"
        "endlibrary\n";
    RequireFull(CheckProjectCode(fixture.frozen, cache, {}, expanded, CodeLanguage::VJass, 401),
        "YDNL must expand through the production template stage and keep complete project semantics");

    std::string broken = expanded;
    broken.replace(broken.find("IncludedProbe()\n"), std::string("IncludedProbe()").size(), "MissingTemplateProbe()");
    const auto failed = CheckProjectCode(fixture.frozen, cache, {}, broken, CodeLanguage::VJass, 402);
    Require(failed.completed && failed.contextComplete && failed.semanticChecked && !failed.Succeeded(),
        "an error after YDNL must come from the complete expanded check");
    Require(std::any_of(failed.diagnostics.begin(), failed.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.message.find("MissingTemplateProbe") != std::string::npos
            && diagnostic.source == "WCT/header.j" && diagnostic.line == 4;
    }), "the error on the line produced by YDNL must map back to the author's original line 4");
    Require(std::none_of(failed.diagnostics.begin(), failed.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.message.find("YDNL") != std::string::npos;
    }), "the expanded macro name must never be reported as a stray token");

    std::string unsupported = expanded;
    unsupported.replace(unsupported.find("endfunction"), 0, "UNSUPPORTED_TEMPLATE\n");
    const auto skipped = CheckProjectCode(fixture.frozen, cache, {}, unsupported, CodeLanguage::VJass, 403);
    Require(!skipped.contextComplete && !skipped.semanticChecked
        && skipped.reason.find("TEMPLATE_SLK_SKIPPED") != std::string::npos,
        "a template block the editor does not execute must fall back with its actual reason");
    fixture.RequireSourceUnchanged();
}

// 整图别处有编辑器不执行的模板（真实大图的物编生成块）时，本段仍按保存链展开 YDNL 后检查语法，
// 不把宏名当成多余的词，错误报在作者原行。
void UnexecutedTemplateElsewhereStillExpandsAuthorSection() {
    ModernProjectFixture fixture(kTemplateForcedInclude);
    WriteOriginalText(fixture.addonLibrary, std::string(kAddonInjection) + "UNSUPPORTED_TEMPLATE\n");
    CodeCheckProjectCache cache;
    // 与作者截图一致的普通触发器函数写法（不是 vJass 库）。
    const std::string valid =
        "function DoNothingProbe takes nothing returns nothing\n"
        "endfunction\n"
        "function AuthorProbe takes nothing returns integer\n"
        "local integer base = AddonInjectedProbe()\n"
        "set base = base + 1 YDNL set base = base + 2\n"
        "call DoNothingProbe() YDNL return base + 3\n"
        "endfunction\n";
    const auto checked = CheckProjectCode(fixture.frozen, cache, {}, valid, CodeLanguage::VJass, 501);
    std::string details = checked.reason;
    for (const auto& diagnostic : checked.diagnostics) details += "\n" + diagnostic.source + ":" + std::to_string(diagnostic.line) + " " + diagnostic.message;
    Require(checked.completed && checked.syntaxChecked && !checked.contextComplete && checked.Succeeded()
        && checked.reason.find("仅检查本段语法") != std::string::npos
        && checked.reason.find("TEMPLATE_SLK_SKIPPED") != std::string::npos,
        "the author section must expand YDNL and pass its own syntax check: " + details);

    std::string broken = valid;
    broken.replace(broken.find("return base + 3"), std::string("return base + 3").size(), "return base 3");
    const auto failed = CheckProjectCode(fixture.frozen, cache, {}, broken, CodeLanguage::VJass, 502);
    details = failed.reason;
    for (const auto& diagnostic : failed.diagnostics) details += "\n" + diagnostic.source + ":" + std::to_string(diagnostic.line) + " " + diagnostic.message;
    Require(failed.completed && !failed.Succeeded() && std::any_of(failed.diagnostics.begin(), failed.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error
            && diagnostic.source == "WCT/header.j" && diagnostic.line == 6; }),
        "a real syntax error after YDNL must be reported on the author's original line 6: " + details);
    Require(details.find("YDNL") == std::string::npos, "the macro name must never be reported: " + details);
    fixture.RequireSourceUnchanged();
}

// T 检查：未发布的触发器候选按保存链生成整图并做同一检查；GUI 语句生成代码上的错误
// 附上紧凑 T 行列与函数名，冻结的实时文档和源地图都不改变。
void TriggerCheckLocatesGuiStatements() {
    ModernProjectFixture fixture;
    CodeCheckProjectCache cache;
    auto document = std::make_shared<Trigger::TriggerDocument>(*fixture.frozen->triggers);
    document->categories.push_back({900001, 0, Trigger::RawText{"原创分类"}, 0, {}});
    const auto trigger = Trigger::TriggerDocumentEditor::AddTrigger(*document, 0, "Probe");
    Require(trigger.Succeeded() && trigger.value, "add the original probe trigger");
    const auto replace = [&](std::string text) {
        const auto binding = Trigger::CaptureTriggerGraphBinding(*document, *fixture.frozen->catalog);
        Require(binding.Succeeded() && binding.value, "bind the candidate trigger document");
        Trigger::TriggerGraphEditBatch batch{*binding.value, {}};
        batch.operations.emplace_back(Trigger::GraphReplaceTriggerText{{Trigger::GraphRecordKind::Trigger, *trigger.value, {}}, std::move(text)});
        const auto applied = Trigger::ApplyTriggerGraphEditBatch(*document, *fixture.frozen->catalog, batch);
        Require(applied.Succeeded(), "write the compact T candidate");
    };
    replace("actions:\n    InitBlizzard()\n");
    const auto passed = CheckProjectTriggers(fixture.frozen, cache, document, 601);
    RequireFull(passed, "a GUI trigger calling a defined function must pass the complete check");

    replace("actions:\n    InitBlizzard()\n    MissingProbeAction()\n");
    const auto failed = CheckProjectTriggers(fixture.frozen, cache, document, 602);
    std::string details = failed.reason;
    for (const auto& diagnostic : failed.diagnostics) details += "\n" + diagnostic.source + ":" + std::to_string(diagnostic.line) + " " + diagnostic.message;
    Require(failed.completed && failed.contextComplete && !failed.Succeeded(), "a missing script function must fail the complete check: " + details);
    const auto located = std::find_if(failed.diagnostics.begin(), failed.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error && diagnostic.trigger;
    });
    Require(located != failed.diagnostics.end(), "the error must carry its T location: " + details);
    const auto& at = *located->trigger;
    Require(at.triggerId == *trigger.value && at.triggerName == "Probe" && at.function == "MissingProbeAction"
        && at.line == 3 && at.column == 5 && at.generated.find("MissingProbeAction") != std::string::npos,
        "the error must point at line 3 column 5 of the compact T text: " + at.function + " " + std::to_string(at.line)
            + ":" + std::to_string(at.column) + " " + at.generated);

    // 空闲释放整图上下文后，下一次检查按同一冻结来源重新捕获，结论不变。
    Require(cache.TryRelease(), "an idle cache must release its captured context");
    replace("actions:\n    InitBlizzard()\n");
    RequireFull(CheckProjectTriggers(fixture.frozen, cache, document, 603), "a released cache must recapture and pass again");
    fixture.RequireSourceUnchanged();
}

// 删除审计的模板上下文必须冻结全部七类物编；不能仅绑定生成器使用的前三类。
// 本夹具不依赖大型SLK默认库，只验证真实宿主提供器覆盖和旧确认的来源身份。
void PendingHigherObjectEditsInvalidateRemovalCoverage() {
    ModernProjectFixture fixture;
    auto document = *fixture.frozen->triggers;
    const auto category = Trigger::TriggerDirectoryEditor::AddCategory(document, "Original removal objects");
    Require(category.Succeeded() && category.value, "add original removal fixture category");
    const auto trigger = Trigger::TriggerDocumentEditor::AddTrigger(document, *category.value, "PendingObjectTarget");
    Require(trigger.Succeeded() && trigger.value, "add original ordinary removal target");
    Trigger::GraphParentExpectation parent{Trigger::GraphParentKind::CategoryTriggers,
        {Trigger::GraphRecordKind::Category, document.categories.back().id, {}}, {}, {}};
    parent.orderedChildren.push_back({Trigger::GraphRecordKind::Trigger, *trigger.value, {}});
    Trigger::GraphRemove remove{{Trigger::GraphRecordKind::Trigger, *trigger.value, {}}, "PendingObjectTarget", parent};

    constexpr auto type = ObjectData::ObjectType::Buff;
    const ObjectData::ObjectKey object{type, "B900"};
    const ObjectData::ModificationKey field{"fnam", {}, {}, {}};
    auto native = ObjectData::NativeObjectDocument::CreateEmpty(type);
    Require(native.Succeeded() && native.value, "create original high-index native object baseline");
    Require(native.value->AddCustomObject(object.rawcode, "B000").Succeeded(), "add original custom buff");
    Require(native.value->AddModification(object, field, ObjectData::NativeValueType::String, std::string("original-value")).Succeeded(),
        "set original high-index native field");
    auto baseline = std::make_shared<CodeCheckProjectSnapshot>(*fixture.frozen);
    baseline->binding += "/pending-high-object";
    baseline->triggers = std::make_shared<const Trigger::TriggerDocument>(document);
    baseline->nativeObjects[static_cast<std::size_t>(type)] = std::make_shared<const ObjectData::NativeObjectDocument>(*native.value);
    auto baseProvider = MakeProjectRemovalPreprocessProvider(baseline);
    const auto original = Trigger::QueryDeclarationRemovalAudit(document, *baseline->catalog, remove, nullptr, &baseProvider);
    std::string details;
    for (const auto& diagnostic : original.diagnostics) details += diagnostic.code + ":" + diagnostic.message + "\n";
    if (original.value) for (const auto& block : original.value->individualBlocks) details += block.incompleteReason + "\n";
    Require(original.Succeeded() && original.value && original.value->complete && !original.value->preprocessIdentitySha256.empty(),
        "actual host must provide complete baseline removal coverage: " + details);
    if (original.value->groupableCount) {
        Trigger::RemovalCodeConfirmation confirmation{original.value->reviewSha256, "no_reference", "reviewed original zero-hit group", {}};
        confirmation.group = Trigger::RemovalCodeConfirmation::Group{original.value->groupableCount, original.value->groupableSha256};
        remove.codeAudits.push_back(std::move(confirmation));
    }
    for (const auto& block : original.value->individualBlocks)
        remove.codeAudits.push_back({original.value->reviewSha256, "no_reference", "reviewed original individual evidence", {block.identity}});
    Require(Trigger::CheckDeclarationRemovalAudit(document, *baseline->catalog, remove, nullptr, &baseProvider).Succeeded(),
        "original confirmation is valid before object-only changes");

    auto changedNative = std::make_shared<CodeCheckProjectSnapshot>(*baseline);
    changedNative->binding += "/native-edit"; ++changedNative->revision;
    auto pending = *native.value;
    Require(pending.SetModification(object, field, std::string("pending-native-value")).Succeeded(), "change high-index native field only in memory");
    changedNative->nativeObjects[static_cast<std::size_t>(type)] = std::make_shared<const ObjectData::NativeObjectDocument>(std::move(pending));

    const auto lniBytes = ObjectData::RenderObjectRecordsToLni(type, native.value->Objects());
    Require(lniBytes.Succeeded() && lniBytes.value, "render original high-index native baseline as LNI");
    auto parsed = ObjectData::LniObjectCodec(type).Parse(*lniBytes.value);
    Require(parsed.Succeeded() && parsed.document, "parse original high-index LNI view");
    auto* lni = dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.get());
    Require(lni && lni->SetModification(object, field, std::string("pending-lni-value")).Succeeded(), "change high-index LNI field only in memory");
    auto changedLni = std::make_shared<CodeCheckProjectSnapshot>(*baseline);
    changedLni->binding += "/lni-edit"; ++changedLni->revision;
    changedLni->editableObjects[static_cast<std::size_t>(type)] = std::make_shared<const ObjectData::LniObjectDocument>(*lni);

    auto changedStrings = std::make_shared<CodeCheckProjectSnapshot>(*baseline);
    changedStrings->binding += "/wts-edit"; ++changedStrings->revision;
    Require(baseline->map.has_value(), "baseline carries current parsed map members");
    std::vector<MapArchiveMember> pendingMembers;
    for (const auto& member : baseline->map->Members()) {
        if (member.kind == MapDataKind::Strings) continue;
        pendingMembers.insert(pendingMembers.end(), member.sources.begin(), member.sources.end());
    }
    pendingMembers.push_back({"war3map.wts", OriginalBytes("STRING 9\n{\npending original WTS value\n}\n")});
    changedStrings->map = MapDocument::FromMembers(pendingMembers, fixture.source);
    const auto resolved = changedStrings->map->ResolveString("TRIGSTR_009");
    Require(changedStrings->map->Member(MapDataKind::Strings).state == MapDataState::Ready
        && resolved.Succeeded() && resolved.value && *resolved.value == "pending original WTS value",
        "pending WTS is a valid native member in the private map document");
    Require(changedStrings->triggers == baseline->triggers && changedStrings->nativeObjects == baseline->nativeObjects
        && changedStrings->editableObjects == baseline->editableObjects,
        "WTS-only snapshot leaves trigger and all native/LNI object snapshots unchanged");

    for (const auto& changed : {changedNative, changedLni, changedStrings}) {
        auto provider = MakeProjectRemovalPreprocessProvider(changed);
        const auto review = Trigger::QueryDeclarationRemovalAudit(document, *changed->catalog, remove, nullptr, &provider);
        details.clear();
        for (const auto& diagnostic : review.diagnostics) details += diagnostic.code + ":" + diagnostic.message + "\n";
        if (review.value) for (const auto& block : review.value->individualBlocks) details += block.incompleteReason + "\n";
        Require(review.Succeeded() && review.value && review.value->complete, "pending object or WTS edit remains auditable: " + details);
        Require(review.value->preprocessIdentitySha256 != original.value->preprocessIdentitySha256
            && review.value->reviewSha256 != original.value->reviewSha256,
            "unsaved high-index native/LNI or WTS edit changes coverage and review identities");
        const auto checked = Trigger::CheckDeclarationRemovalAudit(document, *changed->catalog, remove, nullptr, &provider);
        Require(!checked.Succeeded() && std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TRIGGER_REMOVE_CODE_AUDIT_STALE";
        }), "old confirmation must reject unsaved high-index native/LNI or WTS changes");
    }
    fixture.RequireSourceUnchanged();
}

#include "CodeCheckStorageDiagnosticCases.h"

} // namespace

int main() {
    try {
        ActualModernLayerSourcesAndRepeatedCache();
        ActualLayerLibraryChangeRejectsOldContextThenRecaptures();
        UnboundRootsCannotSupplyQuotedIncludesOrInjection();
        TemplateMacrosExpandBeforeCheckingAndKeepAuthorLines();
        UnexecutedTemplateElsewhereStillExpandsAuthorSection();
        TriggerCheckLocatesGuiStatements();
        PendingHigherObjectEditsInvalidateRemovalCoverage();
        UnsupportedLocalStorageWarningReachesCheckAndSave();
        std::cout << "Original modern project: enabled UI injection, quoted include, repeated cache, changed identity, unbound roots and template macros passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
