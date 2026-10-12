#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Script/LuaAuthorCode.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "LuaMapSaveTarget.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace War3Map::Services {
namespace {

using PreparationResult = Result<ScriptSavePreparationState>;

PreparationResult Fail(std::string code, std::string message,
    const std::filesystem::path& source = {}) {
    auto result = PreparationResult::Failure("ScriptSave.Prepare", std::move(code), std::move(message));
    if (!source.empty()) result.diagnostics.back().origin = OriginSpan{source, 0, 0, {}};
    return result;
}

PreparationResult Forward(std::vector<Diagnostic> diagnostics, std::string code) {
    PreparationResult result;
    result.diagnostics = std::move(diagnostics);
    if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    })) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "ScriptSave.Prepare", std::move(code),
            "脚本输入来源未返回完整结果；保存未开始。"});
    }
    return result;
}

Result<std::filesystem::path> UniqueScriptWorkspace() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    if (error) return Result<std::filesystem::path>::Failure("ScriptSave.Prepare",
        "SCRIPT_WORKSPACE_FAILED", "无法定位系统临时目录。");
    const auto root = temporary / "War3MapEditor" / "script-build";
    std::filesystem::create_directories(root, error);
    if (error) return Result<std::filesystem::path>::Failure("ScriptSave.Prepare",
        "SCRIPT_WORKSPACE_FAILED", "无法创建脚本构建根目录。");

    static std::atomic<std::uint64_t> sequence{0};
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto candidate = root / ("save-" + std::to_string(stamp) + "-"
            + std::to_string(sequence.fetch_add(1)));
        error.clear();
        if (std::filesystem::create_directory(candidate, error)) {
            return Result<std::filesystem::path>::Success(candidate);
        }
        if (error && error != std::errc::file_exists) break;
    }
    return Result<std::filesystem::path>::Failure("ScriptSave.Prepare",
        "SCRIPT_WORKSPACE_FAILED", "无法取得独占的脚本构建目录。");
}

constexpr std::string_view kSuccessMarker = "FORGE_SCRIPT_WORKSPACE_SUCCESS_V1\n";
constexpr std::size_t kRetainedSuccessCount = 20;

bool SafeDirectoryTree(const std::filesystem::path& path, std::size_t& count) {
    if (++count > 65536) return false;
    std::error_code error;
    const auto type = std::filesystem::symlink_status(path, error);
    if (error || type.type() != std::filesystem::file_type::directory) return false;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
        const auto child = it->path();
        const auto childType = std::filesystem::symlink_status(child, error);
        if (error || ++count > 65536) return false;
#ifdef _WIN32
        const auto childAttributes = GetFileAttributesW(child.c_str());
        if (childAttributes == INVALID_FILE_ATTRIBUTES || (childAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (childType.type() == std::filesystem::file_type::directory) {
            if (!SafeDirectoryTree(child, count)) return false;
        } else if (childType.type() != std::filesystem::file_type::regular) {
            return false;
        }
    }
    return !error;
}

bool SafeDirectoryNode(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::symlink_status(path, error).type() != std::filesystem::file_type::directory || error) return false;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    return true;
}

bool HasSuccessMarker(const std::filesystem::path& directory) {
    const auto marker = directory / ".forge-success";
    std::error_code error;
    if (std::filesystem::symlink_status(marker, error).type() != std::filesystem::file_type::regular || error) return false;
    if (std::filesystem::file_size(marker, error) != kSuccessMarker.size() || error) return false;
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(marker.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    std::ifstream input(marker, std::ios::binary);
    std::string content(kSuccessMarker.size(), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    return input.is_open() && input.gcount() == static_cast<std::streamsize>(content.size())
        && !input.bad() && content == kSuccessMarker;
}

void WarnCleanup(MapSaveTask& task) noexcept {
    try {
        task.diagnostics.push_back({DiagnosticSeverity::Warning, "ScriptSave.Cleanup", "SCRIPT_WORKSPACE_CLEANUP_FAILED",
            "脚本构建目录清理未完成；已保存的地图不受影响。"});
    } catch (...) {}
}

} // namespace

Result<std::vector<Script::ScriptProfileSourceEncoding>> NormalizeScriptSourceEncodingDeclarations(
    std::vector<Script::ScriptProfileSourceEncoding> declarations) {
    using Output = Result<std::vector<Script::ScriptProfileSourceEncoding>>;
    const auto fail = [](std::string code, std::string message, const std::filesystem::path& source) {
        auto result = Output::Failure("ScriptSave.Prepare", std::move(code), std::move(message));
        if (!source.empty()) result.diagnostics.back().origin = OriginSpan{source};
        return result;
    };
    if (declarations.size() > 1024)
        return fail("SCRIPT_SOURCE_ENCODINGS_LIMIT", "逐文件编码声明超过1024项。", {});
    try {
        std::size_t characters = 0;
        for (std::size_t index = 0; index < declarations.size(); ++index) {
            auto& declaration = declarations[index];
            auto& path = declaration.path;
            if ((declaration.encoding != Script::ScriptProfileEncoding::Utf8
                    && declaration.encoding != Script::ScriptProfileEncoding::Gbk)
                || path.empty() || !path.is_absolute() || path.native().size() > 4096
                || path.filename().empty() || path.filename() == "."
                || std::any_of(path.native().begin(), path.native().end(), [](auto character) {
                    return static_cast<std::uint32_t>(character) < 0x20;
                }) || std::any_of(path.begin(), path.end(), [](const auto& part) { return part == ".."; }))
                return fail("SCRIPT_SOURCE_ENCODING_INVALID", "来源声明须为有界绝对单文件路径及明确utf8/gbk编码，不能含父目录段或控制字符。", path);
            path = path.lexically_normal().make_preferred();
            if (path.native().size() > 256 * 1024 - characters)
                return fail("SCRIPT_SOURCE_ENCODINGS_LIMIT", "逐文件编码声明的总路径超过256Ki字符预算。", path);
            characters += path.native().size();
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (CompareStringOrdinal(declarations[prior].path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
                    return fail("SCRIPT_SOURCE_ENCODING_DUPLICATE", "同一来源重复声明编码；未按顺序遮蔽或猜测选择。", path);
            }
        }
        return Output::Success(std::move(declarations));
    } catch (const std::exception&) {
        return fail("SCRIPT_SOURCE_ENCODING_INVALID", "逐文件编码声明无法规范化为有界来源路径。", {});
    }
}

Result<Script::ScriptProfileSourceEncoding> ParseScriptSourceEncodingDeclaration(
    const std::filesystem::path& source, std::string_view encoding) {
    using Output = Result<Script::ScriptProfileSourceEncoding>;
    if (encoding != "utf8" && encoding != "gbk") {
        auto result = Output::Failure("ScriptSave.Prepare", "SCRIPT_SOURCE_ENCODING_INVALID",
            "单文件编码仅接受明确utf8或gbk，不允许自动猜测编码。");
        if (!source.empty()) result.diagnostics.back().origin = OriginSpan{source};
        return result;
    }
    auto normalized = NormalizeScriptSourceEncodingDeclarations({{source, encoding == "utf8"
        ? Script::ScriptProfileEncoding::Utf8 : Script::ScriptProfileEncoding::Gbk}});
    if (!normalized.Succeeded() || !normalized.value) return {{}, std::move(normalized.diagnostics)};
    return Output::Success(std::move(normalized.value->front()));
}

Result<std::vector<Script::ScriptProfileIncludeAccess>> NormalizeScriptIncludeAccessDeclarations(
    std::vector<Script::ScriptProfileIncludeAccess> declarations) {
    using Output = Result<std::vector<Script::ScriptProfileIncludeAccess>>;
    if (declarations.empty()) return Output::Success(std::move(declarations));
    try {
        // 只借完整档案的纯声明校验，不复制设备路径/ADS/重复权限算法。
        // witness根不作为实际工具或来源发布，整个过程不读取FS。
        Script::ScriptToolchainProfile witness;
        witness.profileId = "ordinary-include-declarations-v1";
        witness.toolRoot = declarations.front().path.root_path();
        witness.uiRoot = witness.mainSourceDirectory = witness.toolRoot;
        witness.ordinaryIncludeAccess = std::move(declarations);
        auto valid = Script::ValidateScriptToolchainProfile(witness);
        if (!valid.Succeeded() || !valid.value) return {{}, std::move(valid.diagnostics)};
        for (auto& declaration : witness.ordinaryIncludeAccess) {
            auto& path = declaration.path;
            path = path.lexically_normal();
            if (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
            path.make_preferred();
        }
        return Output::Success(std::move(witness.ordinaryIncludeAccess));
    } catch (const std::exception&) {
        return Output::Failure("ScriptSave.Prepare", "SCRIPT_INCLUDE_ACCESS_DECLARATION_INVALID",
            "普通include声明无法规范化为有界档案路径；未读取任何来源。");
    }
}

Result<Script::ScriptProfileIncludeAccess> ParseScriptIncludeAccessDeclaration(
    const std::filesystem::path& source, std::string_view kind, std::string_view encoding, bool required) {
    using Output = Result<Script::ScriptProfileIncludeAccess>;
    if ((kind != "directory" && kind != "file") || (encoding != "utf8" && encoding != "gbk" && encoding != "auto")) {
        auto result = Output::Failure("ScriptSave.Prepare", "SCRIPT_INCLUDE_ACCESS_DECLARATION_INVALID",
            "普通include声明仅接受directory/file种类及utf8/gbk/auto编码。");
        if (!source.empty()) result.diagnostics.back().origin = OriginSpan{source};
        return result;
    }
    const auto selected = encoding == "utf8" ? Script::ScriptProfileEncoding::Utf8
        : encoding == "gbk" ? Script::ScriptProfileEncoding::Gbk : Script::ScriptProfileEncoding::UniqueUtf8OrGbk;
    auto normalized = NormalizeScriptIncludeAccessDeclarations({{source, kind == "directory"
        ? Script::ScriptIncludeAccessKind::Directory : Script::ScriptIncludeAccessKind::ExactFile, selected, required}});
    if (!normalized.Succeeded() || !normalized.value) return {{}, std::move(normalized.diagnostics)};
    return Output::Success(std::move(normalized.value->front()));
}

ScriptRebuildCapability QueryScriptRebuildCapability(
    const std::filesystem::path& toolRoot, const std::filesystem::path& uiRoot) {
    ScriptRebuildCapability capability;
    auto resolved = Script::ResolveScriptToolchainLayout({toolRoot, 24, {}});
    const bool hasLayout = resolved.Succeeded() && resolved.value.has_value();
    bool unsupportedUi = false;
    const bool ownFrameworkUi = !uiRoot.empty() && hasLayout && resolved.value->executionPolicy ==
        Script::ScriptToolchainExecutionPolicy::FrameworkInputsRequired &&
        Trigger::UiCatalog::ResolveUiRoot(uiRoot).lexically_normal() ==
            Trigger::UiCatalog::ResolveUiRoot(resolved.value->root).lexically_normal();
    if (!uiRoot.empty() && uiRoot != toolRoot && !ownFrameworkUi) {
        auto uiStatus = Script::CheckScriptUiToolchainSemantics({uiRoot, 24, {}});
        unsupportedUi = !uiStatus.Succeeded() && !uiStatus.diagnostics.empty()
            && std::all_of(uiStatus.diagnostics.begin(), uiStatus.diagnostics.end(),
                [](const Diagnostic& diagnostic) { return diagnostic.code == "SCRIPT_TOOLCHAIN_UNSUPPORTED"; });
        if (!uiStatus.Succeeded() && !unsupportedUi) {
            capability.diagnostics = std::move(uiStatus.diagnostics);
            return capability; // 损坏或冲突的独立 UI 不能降级为缺工具链。
        }
    }
    const bool missing = toolRoot.empty() || std::any_of(resolved.diagnostics.begin(), resolved.diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.code == "SCRIPT_LAYOUT_ROOT_MISSING" || diagnostic.code == "SCRIPT_LAYOUT_UNRECOGNIZED"
                || diagnostic.code == "SCRIPT_LAYOUT_BASE_SCRIPT_MISSING" || diagnostic.code == "SCRIPT_LAYOUT_INCLUDE_MISSING";
        });
    capability.diagnostics = std::move(resolved.diagnostics);
    if (!hasLayout && !missing) return capability;
    const bool frameworkEntry = hasLayout && resolved.value->executionPolicy ==
        Script::ScriptToolchainExecutionPolicy::FrameworkInputsRequired;
    if (!hasLayout || (!resolved.value->semanticsSupported && !frameworkEntry) || unsupportedUi) {
        const bool unsupported = hasLayout || unsupportedUi;
        capability.state = unsupported ? ScriptRebuildCapabilityState::Unsupported : ScriptRebuildCapabilityState::Missing;
        capability.diagnostics.insert(capability.diagnostics.begin(), {DiagnosticSeverity::Error, "ScriptSave.Prepare",
            unsupported ? "SCRIPT_TOOLCHAIN_UNSUPPORTED" : "SCRIPT_TOOLCHAIN_MISSING",
            unsupported ? "旧版 YDWE 工具链尚未支持，无法重建脚本。" : "缺少可绑定的基础脚本或包含目录，无法重建脚本。",
            OriginSpan{unsupportedUi ? uiRoot : toolRoot, 0, 0, {}}});
        return capability;
    }
    capability.state = ScriptRebuildCapabilityState::Ready;
    capability.layout = std::move(resolved.value);
    return capability;
}

Result<ScriptSavePreparationState> PrepareScriptSaveInputs(
    MapSaveTask& task, ScriptSaveInputSources&& sources) {
    try {
        if (task.executed || task.script || task.scriptSkipped || !task.archive) {
            return Fail("SCRIPT_TASK_INVALID", "保存任务尚未冻结源归档，或脚本输入已经准备/执行。");
        }
        auto declarations = NormalizeScriptSourceEncodingDeclarations(std::move(sources.sourceEncodings));
        if (!declarations.Succeeded() || !declarations.value)
            return Forward(std::move(declarations.diagnostics), "SCRIPT_SOURCE_ENCODING_INVALID");
        auto includeAccess = NormalizeScriptIncludeAccessDeclarations(std::move(sources.includeAccess));
        if (!includeAccess.Succeeded() || !includeAccess.value)
            return Forward(std::move(includeAccess.diagnostics), "SCRIPT_INCLUDE_ACCESS_DECLARATION_INVALID");
        ScriptRebuildCapability capability;
        if (sources.luaTarget) {
            if (!declarations.value->empty() || !includeAccess.value->empty())
                return Fail("LUA_MAP_FRAMEWORK_INPUT_UNSUPPORTED",
                    "Lua完整保存尚不消费JASS框架编码/include声明；未静默忽略这些来源。");
            capability.state = ScriptRebuildCapabilityState::Ready;
        } else {
            capability = QueryScriptRebuildCapability(sources.toolRoot, sources.uiRoot);
        }
        if (!includeAccess.value->empty() && (!capability.layout || capability.state != ScriptRebuildCapabilityState::Ready
            || capability.layout->executionPolicy != Script::ScriptToolchainExecutionPolicy::FrameworkInputsRequired))
            return Fail("SCRIPT_INCLUDE_ACCESS_UNSUPPORTED",
                "当前工具链不消费普通include读取资格，未静默忽略声明或沿用源脚本。", includeAccess.value->front().path);
        if (capability.state == ScriptRebuildCapabilityState::Unknown)
            return Forward(std::move(capability.diagnostics), "SCRIPT_TOOLCHAIN_INVALID");
        if (!declarations.value->empty() && (!capability.layout || capability.state != ScriptRebuildCapabilityState::Ready
            || capability.layout->executionPolicy != Script::ScriptToolchainExecutionPolicy::FrameworkInputsRequired))
            return Fail("SCRIPT_SOURCE_ENCODINGS_UNSUPPORTED",
                "当前工具链不消费框架逐文件编码声明，未静默忽略声明或沿用源脚本。", declarations.value->front().path);
        if (capability.state != ScriptRebuildCapabilityState::Ready) {
            if (!sources.allowMissingToolchain)
                return Forward(std::move(capability.diagnostics), "SCRIPT_TOOLCHAIN_INVALID");
            // 沿用源脚本只沿用宿主已经授权的事务规则；布局识别不会授予脚本重建兼容性。
            task.scriptSkipped = true;
            task.scriptLayers.clear();
            auto skipped = PreparationResult::Success(ScriptSavePreparationState::Skipped);
            auto diagnostic = capability.diagnostics.front();
            diagnostic.severity = DiagnosticSeverity::Warning;
            diagnostic.message += "本次沿用源地图脚本，触发器源文件改动尚未作用于游戏脚本。";
            skipped.diagnostics.push_back(std::move(diagnostic));
            return skipped;
        }

        if (!sources.triggers || !sources.catalog || sources.scriptMember.empty()) {
            return Fail("SCRIPT_INPUT_MISSING", "缺少已解析的触发器、函数目录或游戏脚本成员。");
        }
        if (sources.scriptMember != Script::MapScriptMemberName(*task.archive)
            && !(sources.luaTarget && sources.scriptMember ==
                (sources.luaTarget->dialect == Script::LuaMapSaveDialect::Classic127YDWE ? "war3map.j" : "war3map.lua"))) {
            return Fail("SCRIPT_MEMBER_MISMATCH", "脚本成员名称与冻结的源归档不一致。");
        }
        if (sources.layers.empty() || sources.layers != sources.catalog->Layers()) {
            return Fail("SCRIPT_LAYER_ORDER_MISMATCH",
                "保存请求的函数库层序为空或与解析触发器的函数目录层序不一致；不能退回全部层。");
        }
        if (sources.expectedEffectiveLayers && sources.layers != *sources.expectedEffectiveLayers) {
            return Fail("SCRIPT_UI_LAYER_STALE", "当前函数库目录与启用层序不一致；请重新载入后再保存。");
        }

        MapSaveScriptSnapshot frozen;
        frozen.triggers = std::move(sources.triggers);
        frozen.catalog = std::move(sources.catalog);
        frozen.editableInputs = std::move(sources.editableInputs);
        frozen.units = std::move(sources.units);
        frozen.items = std::move(sources.items);
        frozen.abilities = std::move(sources.abilities);
        frozen.layers = std::move(sources.layers);
        frozen.scriptMember = std::move(sources.scriptMember);
        frozen.documentRevision = sources.documentRevision;
        frozen.toolchain.uiRoot = sources.uiRoot.empty() ? sources.toolRoot : sources.uiRoot;
        if (capability.layout) {
            frozen.toolchain.toolRoot = capability.layout->root;
            frozen.toolchain.layout = std::make_shared<const Script::ScriptToolchainLayout>(std::move(*capability.layout));
        }

        PreparationResult result;
        result.diagnostics = std::move(capability.diagnostics);
        if (sources.luaTarget) {
            const auto settings = LuaMapSave::ReadSettings(task);
            if (!settings.Succeeded() || !settings.value)
                return Forward(settings.diagnostics, "LUA_MAP_SETTINGS_INVALID");
            // 只从任务拥有的实际源 W3I 取得作者 Lua 资格；目标语言选择不提供该权限。
            const auto sourceSettings = MapData::MapSettings::Parse(settings.value->source);
            if (sourceSettings.Succeeded() && sourceSettings.value
                && sourceSettings.value->SourceFormatVersion() == 31 && sourceSettings.value->scriptLanguage == 1) {
                auto native = Script::CaptureNativeLuaAuthorCode(settings.value->source, *frozen.triggers, *frozen.catalog);
                if (!native.Succeeded() || !native.value)
                    return Forward(std::move(native.diagnostics), "LUA_AUTHOR_CAPTURE_FAILED");
                sources.luaTarget->profile.target.nativeAuthorCode =
                    std::make_shared<const Script::PreparedLuaAuthorCode>(std::move(*native.value));
            } else sources.luaTarget->profile.target.nativeAuthorCode.reset();
            auto selected = PrepareMapScriptTargetSelection(settings.value->source, settings.value->final,
                *frozen.triggers, *frozen.catalog, *sources.luaTarget, frozen.documentRevision);
            if (!selected.Succeeded() || !selected.value)
                return Forward(std::move(selected.diagnostics), "LUA_MAP_TARGET_INVALID");
            result.diagnostics.insert(result.diagnostics.end(), selected.diagnostics.begin(), selected.diagnostics.end());
            frozen.luaTarget = std::move(*selected.value);
            frozen.scriptMember = frozen.luaTarget->Profile()->dialect == Script::LuaMapSaveDialect::Classic127YDWE
                ? "war3map.j" : "war3map.lua";
        } else if (frozen.toolchain.layout->executionPolicy == Script::ScriptToolchainExecutionPolicy::FrameworkInputsRequired) {
            auto profile = Script::MakeLegacyFrameworkProfile(*frozen.toolchain.layout, *frozen.catalog,
                Trigger::UiCatalog::ResolveUiRoot(frozen.toolchain.uiRoot), task.archive->SourcePath().parent_path());
            if (!profile.Succeeded()) return Forward(std::move(profile.diagnostics), "SCRIPT_FRAMEWORK_PROFILE_INVALID");
            // 仅覆写既有来源的解码选择；其余档案身份仍由已核实的布局和Catalog生成。
            profile.value->sourceEncodings = std::move(*declarations.value);
            profile.value->ordinaryIncludeAccess = std::move(*includeAccess.value);
            auto framework = Script::CaptureScriptFrameworkInputs(std::move(*profile.value), *frozen.toolchain.layout, *frozen.catalog);
            if (!framework.Succeeded()) return Forward(std::move(framework.diagnostics), "SCRIPT_FRAMEWORK_CAPTURE_FAILED");
            result.diagnostics.insert(result.diagnostics.end(), framework.diagnostics.begin(), framework.diagnostics.end());
            frozen.toolchain.frameworkInputs = std::make_shared<const Script::ScriptFrameworkInputs>(std::move(*framework.value));
        }
        if (!sources.gameRoot.empty()) {
            auto game = Game::GameDataSource::MountStrict(sources.gameRoot);
            if (!game.Succeeded() || !game.value) return Forward(std::move(game.diagnostics), "SCRIPT_GAME_DATA_MISSING");
            result.diagnostics.insert(result.diagnostics.end(), game.diagnostics.begin(), game.diagnostics.end());
            auto standard = ObjectData::StandardObjectCatalog::Load(*game.value);
            if (!standard.Succeeded() || !standard.value) return Forward(std::move(standard.diagnostics), "SCRIPT_STANDARD_DATA_MISSING");
            result.diagnostics.insert(result.diagnostics.end(), standard.diagnostics.begin(), standard.diagnostics.end());
            frozen.standard = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*standard.value));
            frozen.game = std::move(*game.value);
        } else {
            result.diagnostics.push_back({DiagnosticSeverity::Warning, "ScriptSave.Prepare",
                "SCRIPT_GAME_DATA_OMITTED", "未提供游戏数据根；脚本构建将省略游戏声音标签及音频时长。"});
        }

        auto workspace = UniqueScriptWorkspace();
        if (!workspace.Succeeded() || !workspace.value) return Forward(std::move(workspace.diagnostics), "SCRIPT_WORKSPACE_FAILED");
        frozen.toolchain.workDir = std::move(*workspace.value);
        task.scriptLayers = frozen.layers;
        task.script.emplace(std::move(frozen));
        task.scriptWorkspace = task.script->toolchain.workDir;
        result.value.emplace(ScriptSavePreparationState::Ready);
        return result;
    } catch (const std::exception&) {
        return Fail("SCRIPT_PREPARATION_FAILED", "冻结脚本输入时发生异常；保存未开始。");
    }
}

void FinalizeScriptWorkspace(MapSaveTask& task) noexcept {
    if (!task.succeeded || task.written.empty() || task.scriptWorkspace.empty()) return;
    try {
        const auto directory = task.scriptWorkspace.lexically_normal();
        const auto root = directory.parent_path();
        if (directory.filename().wstring().rfind(L"save-", 0) != 0
            || root.filename() != "script-build" || root.parent_path().filename() != "War3MapEditor") {
            WarnCleanup(task);
            return;
        }
        std::error_code error;
        const auto resolvedRoot = std::filesystem::canonical(root, error);
        if (error || !SafeDirectoryNode(root.parent_path()) || !SafeDirectoryNode(root)
            || !SafeDirectoryNode(directory)) {
            WarnCleanup(task);
            return;
        }
        const auto resolvedDirectory = std::filesystem::canonical(directory, error);
        if (error || resolvedDirectory.parent_path() != resolvedRoot) {
            WarnCleanup(task);
            return;
        }
        {
            std::ofstream marker(directory / ".forge-success", std::ios::binary | std::ios::trunc);
            marker << kSuccessMarker;
            marker.close();
            if (!marker) { WarnCleanup(task); return; }
        }
        struct Completed { std::filesystem::path directory; std::filesystem::file_time_type timestamp; };
        std::vector<Completed> completed;
        std::size_t scanned = 0;
        for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            if (++scanned > 4096) { WarnCleanup(task); return; }
            const auto child = it->path();
            if (child.filename().wstring().rfind(L"save-", 0) != 0 || !HasSuccessMarker(child)) continue;
            const auto resolvedChild = std::filesystem::canonical(child, error);
            if (error || resolvedChild.parent_path() != resolvedRoot) { WarnCleanup(task); return; }
            const auto timestamp = std::filesystem::last_write_time(child / ".forge-success", error);
            if (error) { WarnCleanup(task); return; }
            completed.push_back({child, timestamp});
        }
        if (error) { WarnCleanup(task); return; }
        std::sort(completed.begin(), completed.end(), [](const Completed& left, const Completed& right) {
            if (left.timestamp != right.timestamp) return left.timestamp > right.timestamp;
            return left.directory > right.directory;
        });
        for (std::size_t i = kRetainedSuccessCount; i < completed.size(); ++i) {
            std::size_t count = 0;
            if (!SafeDirectoryTree(completed[i].directory, count)) { WarnCleanup(task); continue; }
            std::filesystem::remove_all(completed[i].directory, error);
            if (error) { WarnCleanup(task); error.clear(); }
        }
    } catch (const std::exception&) {
        WarnCleanup(task);
    } catch (...) {
        WarnCleanup(task);
    }
}

} // namespace War3Map::Services
