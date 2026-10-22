#include "War3MapCore/Script/MapScriptBuild.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Script/LuaTemplateSandbox.h"
#include "War3MapCore/Script/ScriptQueries.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Script/ClassicLuaEntryPipeline.h"

#include <algorithm>
#include <cctype>

namespace War3Map::Script {
namespace {

// 可选成员：缺失按“地图没有这类对象”处理；存在但解析失败时报错（生成的脚本会缺内容）。
bool SameMember(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        return std::tolower(a) == std::tolower(b);
    });
}

template <typename T, typename Parser>
std::optional<T> ParseMember(const MapArchiveSession& archive, std::span<const MemberReplacement> edits,
    std::span<const std::string> removals,
    std::string_view name, Parser parser, std::vector<Diagnostic>& diagnostics) {
    const MemberReplacement* override = nullptr;
    for (const auto& member : edits) {
        if (!SameMember(member.name, name)) continue;
        if (override) {
            diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "MAP_EDIT_CONFLICT",
                std::string(name) + " 有多个待保存版本，拒绝任取一个生成脚本", {}});
            return std::nullopt;
        }
        override = &member;
    }
    const bool removed = std::any_of(removals.begin(), removals.end(),
        [name](const std::string& member) { return SameMember(member, name); });
    if (removed && override) {
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "MAP_EDIT_CONFLICT",
            std::string(name) + " 同时被修改和删除，拒绝生成脚本", {}});
        return std::nullopt;
    }
    if (removed) return std::nullopt;
    if (!override && !archive.HasMember(name)) return std::nullopt;
    const auto bytes = override ? Result<std::vector<std::byte>>::Success(override->data) : archive.ReadMember(name);
    if (!bytes.Succeeded() || !bytes.value) {
        diagnostics.insert(diagnostics.end(), bytes.diagnostics.begin(), bytes.diagnostics.end());
        return std::nullopt;
    }
    auto parsed = parser(*bytes.value);
    if (!parsed.Succeeded() || !parsed.value) {
        diagnostics.insert(diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "MAP_MEMBER_UNPARSED",
            std::string(name) + " 无法解析，不能生成完整的地图脚本", {}});
        return std::nullopt;
    }
    return std::move(*parsed.value);
}

bool HasError(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) return true;
    }
    return false;
}

} // namespace

bool MapScriptBuildResult::Succeeded() const noexcept {
    if (classicLuaBundle) return !HasError(diagnostics) && luaGenerated && luaGenerated->Succeeded()
        && classicLuaBundle->Succeeded() && (!pipeline || pipeline->Succeeded());
    if (luaGenerated) return !HasError(diagnostics) && luaGenerated->Succeeded()
        && (!luaPipeline || luaPipeline->Succeeded());
    if (HasError(diagnostics) || !generated.Succeeded()) return false;
    return !pipeline || pipeline->Succeeded();
}

const std::string* MapScriptBuildResult::FinalScript() const noexcept {
    if (classicLuaBundle) return Succeeded() && pipeline && pipeline->Succeeded() ? &pipeline->script : nullptr;
    if (luaGenerated) return Succeeded() && luaPipeline && luaPipeline->Succeeded() ? &luaPipeline->script : nullptr;
    return Succeeded() && pipeline && pipeline->Succeeded() ? &pipeline->script : nullptr;
}

std::string MapScriptMemberName(const MapArchiveSession& archive) {
    if (archive.HasMember("war3map.w3i")) {
        const auto bytes = archive.ReadMember("war3map.w3i");
        if (bytes.Succeeded()) {
            const auto settings = MapData::MapSettings::Parse(*bytes.value);
            if (settings.Succeeded() && settings.value->scriptLanguage == 1) return "war3map.lua";
        }
    }
    if (archive.HasMember("war3map.j")) return "war3map.j";
    if (archive.HasMember("scripts\\war3map.j")) return "scripts\\war3map.j";
    return "war3map.j";
}

MapScriptBuildResult BuildMapScript(const MapScriptBuildRequest& request) {
    MapScriptBuildResult result;
    auto& diagnostics = result.diagnostics;
    if (request.archive == nullptr || request.triggers == nullptr || request.catalog == nullptr) {
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "BUILD_INPUT_MISSING", "缺少地图归档、触发器或函数库目录", {}});
        return result;
    }
    auto options = request.toolchain;
    const bool luaMode = request.luaTarget != nullptr;
    const bool classicLua = luaMode && request.luaTarget->dialect == LuaMapSaveDialect::Classic127YDWE;
    if (request.compile && !luaMode && !options.layout) {
        auto layout = ResolveScriptToolchainLayout({options.toolRoot, options.warcraftVersion, options.actualVersionMacro});
        diagnostics.insert(diagnostics.end(), layout.diagnostics.begin(), layout.diagnostics.end());
        if (!layout.Succeeded() || !layout.value) return result;
        options.layout = std::make_shared<const ScriptToolchainLayout>(std::move(*layout.value));
        options.toolRoot = options.layout->root;
    }
    if (request.compile && !luaMode && !options.layout->semanticsSupported &&
        options.layout->executionPolicy != ScriptToolchainExecutionPolicy::FrameworkInputsRequired) {
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "SCRIPT_TOOLCHAIN_UNSUPPORTED",
            "旧工具链重写和模板语义尚未适配，不能据路径识别成功重建脚本", {}});
        return result;
    }
    if (request.compile && !luaMode && options.layout->executionPolicy == ScriptToolchainExecutionPolicy::FrameworkInputsRequired) {
        if (!options.frameworkInputs) {
            const auto uiRoot = Trigger::UiCatalog::ResolveUiRoot(options.uiRoot.empty() ? options.toolRoot : options.uiRoot);
            auto profile = MakeLegacyFrameworkProfile(*options.layout, *request.catalog, uiRoot, request.archive->SourcePath().parent_path());
            if (!profile.Succeeded()) { diagnostics.insert(diagnostics.end(), profile.diagnostics.begin(), profile.diagnostics.end()); return result; }
            auto captured = CaptureScriptFrameworkInputs(std::move(*profile.value), *options.layout, *request.catalog);
            diagnostics.insert(diagnostics.end(), captured.diagnostics.begin(), captured.diagnostics.end());
            if (!captured.Succeeded()) return result;
            options.frameworkInputs = std::make_shared<const ScriptFrameworkInputs>(std::move(*captured.value));
        } else if (options.frameworkInputs->catalog.Sha256() != request.catalog->Sha256() ||
            options.frameworkInputs->catalog.Layers() != request.layers ||
            !LegacyFrameworkProfileBindsMap(options.frameworkInputs->profile, request.archive->SourcePath().parent_path())) {
            diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "SCRIPT_FRAMEWORK_CATALOG_STALE",
                "框架输入目录/层序与本次生成器不同；拒绝借用另一张地图的来源。", {}});
            return result;
        }
    }
    const auto& archive = *request.archive;
    if (request.finalSaveMembers && (!request.editedMapMembers.empty() || !request.removedMapMembers.empty())) {
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "MAP_EDIT_CONFLICT",
            "完整保存快照不能与第二组成员覆盖同时提供", {}});
        return result;
    }
    const auto edits = request.finalSaveMembers ? request.finalSaveMembers->replacements
        : std::span<const MemberReplacement>(request.editedMapMembers);
    const auto removals = request.finalSaveMembers ? request.finalSaveMembers->removals
        : std::span<const std::string>(request.removedMapMembers);
    if (request.compile && !luaMode) {
        // 只能从本次最终保存视图捕获，不能复用另一张地图或上一修订的基础库。
        auto bases = CaptureScriptBaseInputs({options.layout.get(), options.basePolicy, &archive,
            edits, removals, request.game, options.gameLibraryVersion, request.documentRevision});
        diagnostics.insert(diagnostics.end(), bases.diagnostics.begin(), bases.diagnostics.end());
        if (!bases.Succeeded()) return result;
        options.baseInputs = std::make_shared<const ScriptBaseInputs>(std::move(*bases.value));
    }
    auto settings = ParseMember<MapData::MapSettings>(archive, edits, removals, "war3map.w3i", [](auto bytes) { return MapData::MapSettings::Parse(bytes); }, diagnostics);
    if (!request.selectedLuaSettings.empty()) {
        if (!luaMode) {
            diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "LUA_MAP_TARGET_MISSING",
                "选中Lua设置必须和同次目标档案一起提供。", {}});
        } else {
            const auto selected = MapData::MapSettings::Parse(request.selectedLuaSettings);
            diagnostics.insert(diagnostics.end(), selected.diagnostics.begin(), selected.diagnostics.end());
            if (!selected.Succeeded() || !selected.value) return result;
            settings = *selected.value;
        }
    }
    auto units = ParseMember<MapData::UnitPlacements>(archive, edits, removals, "war3mapUnits.doo", [](auto bytes) { return MapData::ParseUnitPlacements(bytes); }, diagnostics);
    auto doodads = ParseMember<MapData::DoodadPlacements>(archive, edits, removals, "war3map.doo", [](auto bytes) { return MapData::ParseDoodadPlacements(bytes); }, diagnostics);
    auto regions = ParseMember<MapData::RegionsDocument>(archive, edits, removals, "war3map.w3r", [](auto bytes) { return MapData::ParseRegions(bytes); }, diagnostics);
    auto cameras = ParseMember<MapData::CamerasDocument>(archive, edits, removals, "war3map.w3c", [](auto bytes) { return MapData::ParseCameras(bytes); }, diagnostics);
    auto sounds = ParseMember<MapData::SoundsDocument>(archive, edits, removals, "war3map.w3s", [](auto bytes) { return MapData::ParseSounds(bytes); }, diagnostics);
    auto strings = ParseMember<MapData::StringTableDocument>(archive, edits, removals, "war3map.wts", [](auto bytes) { return MapData::ParseStringTable(bytes); }, diagnostics);
    const auto terrain = ParseMember<TerrainExtent>(archive, edits, removals, "war3map.w3e", [](auto bytes) {
        const auto extent = ReadTerrainExtent(bytes);
        return extent ? Result<TerrainExtent>::Success(*extent) : Result<TerrainExtent>::Failure(
            "Script.Build", "MAP_TERRAIN_INVALID", "待保存地形没有有效的尺寸与坐标范围");
    }, diagnostics);
    if (!settings) {
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "MAP_SETTINGS_MISSING", "地图缺少可解析的 war3map.w3i", {}});
    }
    if (HasError(diagnostics)) return result;
    if (luaMode && (settings->formatVersion != (classicLua ? 25 : 31)
        || settings->SourceFormatVersion() != (classicLua ? 25 : 31) || settings->scriptLanguage != (classicLua ? 0 : 1))) {
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "LUA_MAP_LANGUAGE_MISMATCH",
            "最终W3I的版本和语言必须与明确选择的Lua档案一致；不会自动升级地图格式。", {}});
        return result;
    }
    // 经典 JASS：TFT v25 与混乱之治 v18（两个 RoC 专有字段由游戏直接读地图设置，不进入脚本）。
    if (!luaMode && settings->formatVersion != 25 && settings->formatVersion != 18) {
        diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "MAP_SCRIPT_FORMAT_UNSUPPORTED",
            "W3I v31 已能独立读取和往返，但整图脚本与新版成员尚未验证；保存继续拒绝。", {}});
        return result;
    }
    if (luaMode) {
        const std::array<std::string, 2> payloads{"tiara.ai", "tiara.blp"};
        const auto identities = archive.DescribeExactMemberIdentities(archive.SourceSha256(), payloads);
        diagnostics.insert(diagnostics.end(), identities.diagnostics.begin(), identities.diagnostics.end());
        if (!identities.Succeeded()) return result;
        const bool dependency = std::any_of(identities.value->members.begin(), identities.value->members.end(),
            [](const auto& member) { return member.present; }) || std::any_of(edits.begin(), edits.end(),
            [&](const auto& member) { return SameMember(member.name, payloads[0]) || SameMember(member.name, payloads[1]); });
        if (dependency) {
            diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "LUA_MEMHACK_UNSUPPORTED",
                "地图包含已确认的tiara/MemHack导入依赖，只支持JASS保存；Lua不能保留其运行语义。导入正文未解析。", {}});
            return result;
        }
        if (request.standard == nullptr && units && !units->units.empty()) {
            diagnostics.push_back({DiagnosticSeverity::Error, "Script.Build", "LUA_OBJECT_CATALOG_REQUIRED",
                "Lua预放对象初始化需要明确的有效物编目录；不能用默认建筑/英雄判定替代。", {}});
            return result;
        }
    }
    if (request.standard == nullptr && !luaMode) {
        diagnostics.push_back({DiagnosticSeverity::Warning, "Script.Build", "STANDARD_CATALOG_MISSING",
            "未载入原版物体目录：建筑 / 物品 / 技能判定退化为默认规则，生成的初始化代码可能与 WE 不同", {}});
    }

    ObjectQuerySources sources;
    sources.standard = request.standard;
    sources.units = request.units;
    sources.items = request.items;
    sources.abilities = request.abilities;

    MapScriptInputs inputs;
    inputs.queries = BuildObjectQueries(std::move(sources));
    AttachSoundQueries(inputs.queries, request.game, request.archive);
    inputs.triggers = request.triggers;
    inputs.debug = request.toolchain.debug;
    if (!luaMode) {
        std::error_code error;
        const auto includes = options.layout ? options.layout->systemIncludes
            : std::vector<std::filesystem::path>{options.toolRoot / "compiler" / "include"};
        inputs.nullSafeHandleSet = std::any_of(includes.begin(), includes.end(), [&](const auto& include) {
            return std::filesystem::exists(include / "YDTrigger" / "SaveLoadSystem" / "HTSystem.j", error);
        });
    }
    inputs.catalog = request.catalog;
    inputs.settings = &*settings;
    inputs.units = units ? &*units : nullptr;
    inputs.doodads = doodads ? &*doodads : nullptr;
    inputs.regions = regions ? &*regions : nullptr;
    inputs.cameras = cameras ? &*cameras : nullptr;
    inputs.sounds = sounds ? &*sounds : nullptr;
    inputs.strings = strings ? &*strings : nullptr;
    inputs.terrain = terrain;
    if (request.toolchain.onStage) request.toolchain.onStage("generate");
    if (luaMode) {
        auto plan = BuildMapInitializationPlan(inputs, request.luaTarget->initialization);
        diagnostics.insert(diagnostics.end(), plan.diagnostics.begin(), plan.diagnostics.end());
        if (!plan.Succeeded()) return result;
        result.luaGenerated = GenerateLuaMapScript(*plan.value, *request.triggers, *request.catalog,
            request.luaTarget->target, request.luaTarget->callbacks, request.luaTestOptions, {classicLua});
        if (classicLua && result.luaGenerated->Succeeded()) {
            result.classicLuaBundle = BuildClassicLuaSaveBundle(*plan.value, *result.luaGenerated, request.luaTarget->target);
            if (request.compile && result.classicLuaBundle->Succeeded()) {
                const auto bases = CaptureScriptBaseInputs({nullptr, ScriptBasePolicy::MapGameOnly, &archive,
                    edits, removals, request.game, 24, request.documentRevision});
                diagnostics.insert(diagnostics.end(), bases.diagnostics.begin(), bases.diagnostics.end());
                if (!bases.Succeeded() || !bases.value) return result;
                const auto ownedBases = std::make_shared<const ScriptBaseInputs>(*bases.value);
                const auto dependencies = ValidateClassicLuaDependencies(*plan.value, *result.luaGenerated,
                    request.luaTarget->target, *ownedBases);
                diagnostics.insert(diagnostics.end(), dependencies.diagnostics.begin(), dependencies.diagnostics.end());
                if (!dependencies.Succeeded()) return result;
                result.pipeline = CheckClassicLuaEntryScript(result.classicLuaBundle->jassEntry,
                    ownedBases, {}, options.onStage);
            }
            return result;
        }
        if (request.compile && result.luaGenerated->Succeeded()) {
            result.luaPipeline = CheckLuaMapScript(*result.luaGenerated, request.luaTarget->checker,
                options.workDir / "lua-check", options.timeout, options.onStage);
        }
        return result;
    }
    auto templates = BuildTemplateContext(options.toolRoot, request.standard, request.abilities);
    // YDWE import(名)(路径, true) 相对地图文件所在目录读取。
    if (request.archive && !request.archive->SourcePath().empty())
        templates.importRoot = request.archive->SourcePath().parent_path();
    result.generated = GenerateMapScript(inputs);
    if (!request.compile || !result.generated.Succeeded()) return result;

    if (options.jassDirs.empty() && !options.frameworkInputs) {
        const auto uiRoot = Trigger::UiCatalog::ResolveUiRoot(options.uiRoot.empty() ? options.toolRoot : options.uiRoot);
        for (const auto& layer : request.layers) {
            const auto directory = uiRoot / std::filesystem::path(std::u8string(layer.begin(), layer.end())) / "jass";
            std::error_code error;
            if (std::filesystem::is_directory(directory, error)) options.jassDirs.push_back(directory);
        }
    }
    // 内置求值覆盖不了的模板（slk 物编生成、控制语句等）由进程内 Lua 沙盒整份执行一次，
    // slk 库首次 require 时才捕获本次保存的最终成员，同一快照供保存时的三方合并使用。
    templates.wholeChunk = MakeLuaWholeChunk(templates, {options.toolRoot, &archive, edits, removals, request.documentRevision,
        options.onStage ? std::function<void()>([&] { options.onStage("lua-template"); }) : std::function<void()>{}}, result.slkSources);
    result.pipeline = CompileMapScript(result.generated.text, options, templates);
    if (std::any_of(result.pipeline->diagnostics.begin(), result.pipeline->diagnostics.end(), [](const Diagnostic& diagnostic) {
            return diagnostic.code == "TEMPLATE_SLK_SKIPPED";
        })) {
        // 失败路径仍检查实际待保存来源，给出可复查的缺口；绝不把捕获成功当作模板执行成功。
        if (!result.slkSources) {
            auto snapshot = SlkMapSourceSnapshot::Capture(archive, edits, removals, request.documentRevision);
            diagnostics.insert(diagnostics.end(), snapshot.diagnostics.begin(), snapshot.diagnostics.end());
            if (snapshot.Succeeded()) result.slkSources = std::move(*snapshot.value);
        }
        if (result.slkSources) {
            diagnostics.push_back({DiagnosticSeverity::Info, "Script.Build", "SLK_SOURCES_CAPTURED",
                "已捕获本次最终成员来源；摘要=" + result.slkSources->ManifestSha256() +
                    "。此摘要不是地图文件 SHA，SLK 模板仍未通过，保存继续中止。", {}});
        }
    }
    return result;
}

} // namespace War3Map::Script
