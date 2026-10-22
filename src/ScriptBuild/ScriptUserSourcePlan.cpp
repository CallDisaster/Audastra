#include "War3MapCore/Script/ScriptUserSourcePlan.h"
#include "ScriptSourceBinding.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <Windows.h>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace War3Map::Script {
namespace {
using Kind = ScriptToolchainBindingKind;
using PlanResult = Result<ScriptUserSourcePlan>;
constexpr std::size_t kMaxItems = 128;
constexpr std::uint64_t kFileBytes = 4ull * 1024 * 1024;
constexpr std::uint64_t kTotalBytes = 32ull * 1024 * 1024;

void Fail(std::vector<Diagnostic>& output, std::string code, std::string message,
    const std::filesystem::path& path = {}) {
    output.push_back({DiagnosticSeverity::Error, "Script.UserSources", std::move(code),
        std::move(message), OriginSpan{path}});
}
bool Digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
void Part(std::string& text, std::string_view value) {
    text += std::to_string(value.size()) + ":";
    text.append(value);
}
void PathPart(std::string& text, const std::filesystem::path& value) {
    const auto utf8 = value.generic_u8string();
    Part(text, std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
}
Result<std::string> LayoutIdentity(const ScriptToolchainLayout& layout) {
    if (layout.systemIncludes.size() > kMaxItems || layout.forceIncludes.size() > kMaxItems ||
        layout.bindingFiles.size() > kMaxItems) return Result<std::string>::Failure(
            "Script.UserSources", "SCRIPT_USER_SOURCE_BINDING_BUDGET", "布局身份超过入口预算。");
    std::string text;
    Part(text, std::to_string(static_cast<int>(layout.kind)));
    Part(text, std::to_string(layout.versionMacro));
    Part(text, layout.allowReturnBug ? "1" : "0");
    Part(text, layout.semanticsSupported ? "1" : "0");
    Part(text, std::to_string(static_cast<int>(layout.executionPolicy)));
    const auto path = [&text](const std::filesystem::path& value) {
        if (value.native().size() > 4096) throw std::length_error("layout path");
        PathPart(text, value);
    };
    path(layout.root); path(layout.commonScript); path(layout.blizzardScript);
    path(layout.waveExecutable); path(layout.jassHelperExecutable); path(layout.pjassExecutable);
    Part(text, std::to_string(layout.systemIncludes.size()));
    for (const auto& value : layout.systemIncludes) path(value);
    Part(text, std::to_string(layout.forceIncludes.size()));
    for (const auto& value : layout.forceIncludes) path(value);
    Part(text, std::to_string(layout.bindingFiles.size()));
    for (const auto& binding : layout.bindingFiles) {
        if (binding.sha256.size() > 64) throw std::length_error("layout digest");
        path(binding.path); Part(text, std::to_string(static_cast<int>(binding.kind)));
        Part(text, binding.sha256); Part(text, std::to_string(binding.bytes));
        Part(text, std::to_string(binding.maxBytes));
    }
    return Sha256Text(text);
}
bool Bind(ScriptUserSourcePlan& plan, const std::filesystem::path& input, Kind expected,
    bool required, std::vector<Diagnostic>& diagnostics) {
    std::filesystem::path path;
    if (!Detail::NormalizeSourcePath(input, path)) {
        Fail(diagnostics, "SCRIPT_USER_SOURCE_PATH_INVALID", "来源必须是有界绝对普通路径。", input);
        return false;
    }
    const auto prior = std::find_if(plan.bindings.begin(), plan.bindings.end(), [&path](const auto& value) {
        return CompareStringOrdinal(value.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (prior != plan.bindings.end()) {
        if (prior->kind == expected || (!required && prior->kind == Kind::Missing)) return true;
        Fail(diagnostics, "SCRIPT_USER_SOURCE_BINDING_FAILED", "同一入口的来源类型不一致。", path);
        return false;
    }
    if (plan.bindings.size() >= kMaxItems || plan.totalFileBytes > kTotalBytes) {
        Fail(diagnostics, "SCRIPT_USER_SOURCE_BINDING_BUDGET", "关键来源超过入口或内容预算。", path);
        return false;
    }
    const auto remaining = (std::min)(kFileBytes, kTotalBytes - plan.totalFileBytes);
    auto snapshot = Detail::CaptureSourceBinding(path, expected, required, remaining);
    if (!snapshot.Succeeded()) {
        diagnostics.insert(diagnostics.end(), snapshot.diagnostics.begin(), snapshot.diagnostics.end());
        return false;
    }
    plan.totalFileBytes += snapshot.value->binding.bytes;
    auto binding = std::move(snapshot.value->binding);
    binding.maxBytes = kFileBytes;
    plan.bindings.push_back(std::move(binding));
    return true;
}
bool Equal(const ScriptUserSourcePlan& a, const ScriptUserSourcePlan& b) {
    if (a.bindingCoverage != b.bindingCoverage || a.executionSupported != b.executionSupported ||
        a.layoutKind != b.layoutKind || a.toolRoot != b.toolRoot || a.uiRoot != b.uiRoot ||
        a.legacyUserLibraryRoot != b.legacyUserLibraryRoot || a.mapDirectory != b.mapDirectory ||
        a.layoutIdentitySha256 != b.layoutIdentitySha256 || a.catalogSha256 != b.catalogSha256 ||
        a.effectiveLayers != b.effectiveLayers || a.orderedInjectionRoots != b.orderedInjectionRoots ||
        a.orderedUserIncludes != b.orderedUserIncludes || a.nativeRoot != b.nativeRoot ||
        a.nativeState != b.nativeState || a.totalFileBytes != b.totalFileBytes ||
        a.bindings.size() != b.bindings.size() || a.profileIdentitySha256 != b.profileIdentitySha256 ||
        a.profileTargetVersionMacro != b.profileTargetVersionMacro ||
        a.profileDirectiveDialectVersion != b.profileDirectiveDialectVersion || a.profileDirectives != b.profileDirectives ||
        a.profileFeatureExtensions != b.profileFeatureExtensions || a.profileUnsupportedPlugins != b.profileUnsupportedPlugins ||
        a.profileMainEncoding != b.profileMainEncoding || a.profileEncodingRoots != b.profileEncodingRoots ||
        a.profileSourceEncodings != b.profileSourceEncodings ||
        a.profileOrdinaryIncludeAccess != b.profileOrdinaryIncludeAccess ||
        a.orderedSystemIncludes != b.orderedSystemIncludes || a.orderedForceIncludes != b.orderedForceIncludes) return false;
    for (std::size_t i = 0; i < a.bindings.size(); ++i) {
        const auto& x = a.bindings[i]; const auto& y = b.bindings[i];
        if (x.path != y.path || x.kind != y.kind || x.sha256 != y.sha256 ||
            x.bytes != y.bytes || x.maxBytes != y.maxBytes) return false;
    }
    return true;
}
bool SameRoot(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::filesystem::path x, y;
    return Detail::NormalizeSourcePath(a, x) && Detail::NormalizeSourcePath(b, y) &&
        CompareStringOrdinal(x.c_str(), -1, y.c_str(), -1, TRUE) == CSTR_EQUAL;
}
} // namespace

Result<ScriptUserSourcePlan> CaptureScriptUserSourcePlan(const ScriptUserSourceRequest& request) {
    PlanResult result;
    try {
        if (!request.layout || !request.catalog || !Digest(request.catalog->Sha256()) ||
            request.catalog->Layers().empty() || request.catalog->Layers().size() > kMaxItems ||
            request.explicitInjectionRoots.size() > kMaxItems || request.explicitUserIncludes.size() > kMaxItems ||
            std::any_of(request.catalog->Layers().begin(), request.catalog->Layers().end(), [](const auto& layer) {
                return layer.empty() || layer.size() > 512;
            })) return PlanResult::Failure("Script.UserSources", "SCRIPT_USER_SOURCE_REQUEST_INVALID",
                "必须提供当前有效布局、实际 catalog 身份和有界层顺序。");
        const bool legacy = request.layout->kind == ScriptToolchainLayoutKind::LegacyXueyue;
        if ((!legacy && request.layout->kind != ScriptToolchainLayoutKind::Modern) ||
            (!request.profile && ((legacy && (request.legacyUserLibraryRoot.empty() || !request.explicitInjectionRoots.empty() ||
                !request.explicitUserIncludes.empty())) || (!legacy && !request.legacyUserLibraryRoot.empty())))) {
            return PlanResult::Failure("Script.UserSources", "SCRIPT_USER_SOURCE_REQUEST_INVALID",
                "旧布局须明确选择用户库根，且不能混用现代显式来源。");
        }
        ScriptUserSourcePlan plan;
        plan.layoutKind = request.layout->kind;
        if (request.profile) {
            auto identity = ValidateScriptToolchainProfile(*request.profile);
            if (!identity.Succeeded()) return {{}, std::move(identity.diagnostics)};
            const auto& profile = *request.profile;
            if (!request.legacyUserLibraryRoot.empty() || !request.explicitInjectionRoots.empty() ||
                !request.explicitUserIncludes.empty() || !SameRoot(profile.toolRoot, request.layout->root) ||
                !SameRoot(profile.uiRoot, request.uiRoot) || !SameRoot(profile.mainSourceDirectory, request.mapDirectory) ||
                (profile.layoutKind == ScriptProfileLayoutKind::LegacyShareMpq) != legacy ||
                profile.targetVersionMacro != request.layout->versionMacro ||
                (profile.baseLibraryCategory == ScriptProfileBaseLibraryCategory::Pre124) != request.layout->allowReturnBug) {
                return PlanResult::Failure("Script.UserSources", "SCRIPT_PROFILE_SOURCE_MISMATCH",
                    "档案与当前布局、实际UI/主文根、目标宏或基础库类别不一致，或混入旧入口参数。");
            }
            plan.profileIdentitySha256 = std::move(*identity.value);
            plan.profileTargetVersionMacro = profile.targetVersionMacro;
            plan.profileDirectiveDialectVersion = profile.directiveDialectVersion;
            plan.profileDirectives = profile.directives;
            plan.profileFeatureExtensions = profile.featureExtensions;
            plan.profileUnsupportedPlugins = profile.unsupportedPlugins;
            plan.profileMainEncoding = profile.mainEncoding;
            plan.profileEncodingRoots = profile.sourceRoots;
            plan.profileSourceEncodings = profile.sourceEncodings;
            plan.profileOrdinaryIncludeAccess = profile.ordinaryIncludeAccess;
        }
        const auto directory = [&](const std::filesystem::path& input, std::filesystem::path& output,
            bool required) {
            if (!Detail::NormalizeSourcePath(input, output)) {
                Fail(result.diagnostics, "SCRIPT_USER_SOURCE_PATH_INVALID", "根路径必须是有界绝对普通路径。", input);
                return false;
            }
            return Bind(plan, output, Kind::Directory, required, result.diagnostics);
        };
        // 档案的主文目录可以是框架编译目录（如尚未生成的 logs）；缺失时记为负绑定，只是不能从中找到 include。
        if (!directory(request.layout->root, plan.toolRoot, true) ||
            !directory(request.uiRoot, plan.uiRoot, true) || (!request.mapDirectory.empty() &&
                !directory(request.mapDirectory, plan.mapDirectory, request.profile == nullptr))) {
            if (result.diagnostics.empty()) Fail(result.diagnostics, "SCRIPT_USER_SOURCE_PATH_INVALID", "根路径无效。");
            return result;
        }
        auto layoutHash = LayoutIdentity(*request.layout);
        if (!layoutHash.Succeeded()) { result.diagnostics = std::move(layoutHash.diagnostics); return result; }
        plan.layoutIdentitySha256 = std::move(*layoutHash.value);
        plan.catalogSha256 = request.catalog->Sha256(); plan.effectiveLayers = request.catalog->Layers();
        if (!Bind(plan, plan.uiRoot / "config", Kind::RegularFile, false, result.diagnostics)) return result;
        if (request.profile) {
            for (const auto& source : request.profile->sourceRoots) {
                std::filesystem::path path;
                if (source.role == ScriptProfileSourceRole::ForceInclude) {
                    if (!Detail::NormalizeSourcePath(source.path, path)) {
                        Fail(result.diagnostics, "SCRIPT_USER_SOURCE_PATH_INVALID", "强制包含必须是有界绝对普通路径。", source.path);
                        return result;
                    }
                    if (!Bind(plan, path, Kind::RegularFile, source.required, result.diagnostics)) return result;
                    plan.orderedForceIncludes.push_back(std::move(path));
                    continue;
                }
                if (!directory(source.path, path, source.required)) return result;
                switch (source.role) {
                case ScriptProfileSourceRole::Injection: plan.orderedInjectionRoots.push_back(std::move(path)); break;
                case ScriptProfileSourceRole::UserInclude: plan.orderedUserIncludes.push_back(std::move(path)); break;
                case ScriptProfileSourceRole::SystemInclude: plan.orderedSystemIncludes.push_back(std::move(path)); break;
                case ScriptProfileSourceRole::Native: {
                    plan.nativeRoot = std::move(path);
                    const auto native = std::find_if(plan.bindings.begin(), plan.bindings.end(), [&plan](const auto& entry) {
                        return CompareStringOrdinal(entry.path.c_str(), -1, plan.nativeRoot.c_str(), -1, TRUE) == CSTR_EQUAL;
                    });
                    plan.nativeState = native->kind == Kind::Missing ?
                        ScriptNativeSourceState::Missing : ScriptNativeSourceState::PresentUnsupported;
                    break;
                }
                default: return PlanResult::Failure("Script.UserSources", "SCRIPT_PROFILE_SOURCE_MISMATCH", "未知档案来源角色。");
                }
            }
            for (auto& access : plan.profileOrdinaryIncludeAccess) {
                std::filesystem::path path;
                if (!Detail::NormalizeSourcePath(access.path, path)) {
                    Fail(result.diagnostics, "SCRIPT_USER_SOURCE_PATH_INVALID", "普通包含声明必须是有界绝对普通路径。", access.path);
                    return result;
                }
                const auto expected = access.kind == ScriptIncludeAccessKind::Directory ?
                    Kind::Directory : Kind::RegularFile;
                if (!Bind(plan, path, expected, access.required, result.diagnostics)) return result;
                access.path = std::move(path);
            }
        } else if (legacy) {
            if (!directory(request.legacyUserLibraryRoot, plan.legacyUserLibraryRoot, true)) {
                if (result.diagnostics.empty()) Fail(result.diagnostics, "SCRIPT_USER_SOURCE_PATH_INVALID", "旧用户库根无效。");
                return result;
            }
            plan.orderedInjectionRoots.push_back(plan.legacyUserLibraryRoot / "jass");
            if (!plan.mapDirectory.empty()) plan.orderedUserIncludes.push_back(plan.mapDirectory);
            plan.orderedUserIncludes.push_back(plan.orderedInjectionRoots.front());
            plan.nativeRoot = plan.toolRoot / "jass" / "native";
            if (!Bind(plan, plan.nativeRoot, Kind::Directory, false, result.diagnostics)) return result;
            const auto native = std::find_if(plan.bindings.begin(), plan.bindings.end(), [&plan](const auto& entry) {
                return CompareStringOrdinal(entry.path.c_str(), -1, plan.nativeRoot.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            plan.nativeState = native->kind == Kind::Missing ?
                ScriptNativeSourceState::Missing : ScriptNativeSourceState::PresentUnsupported;
            for (const auto* key : {"bin/EverConfig.cfg", "plugin/warcraft3/config.cfg",
                "share/script/uiloader.lua", "share/script/compile/inject_code.lua",
                "share/script/compile/wave.lua", "share/script/compile/native.lua"}) {
                if (!Bind(plan, plan.toolRoot / key, Kind::RegularFile, false, result.diagnostics)) return result;
            }
        } else {
            for (const auto& input : request.explicitInjectionRoots) {
                std::filesystem::path path;
                if (!directory(input, path, false)) return result;
                plan.orderedInjectionRoots.push_back(std::move(path));
            }
            for (const auto& input : request.explicitUserIncludes) {
                std::filesystem::path path;
                if (!directory(input, path, false)) return result;
                plan.orderedUserIncludes.push_back(std::move(path));
            }
        }
        for (const auto& path : plan.orderedInjectionRoots) {
            if (!Bind(plan, path, Kind::Directory, false, result.diagnostics)) return result;
        }
        result.value = std::move(plan);
    } catch (const std::exception&) {
        Fail(result.diagnostics, "SCRIPT_USER_SOURCE_BINDING_FAILED", "来源身份捕获失败，未返回未绑定计划。");
    }
    return result;
}

Status VerifyScriptUserSourcePlan(const ScriptUserSourcePlan& plan, const ScriptUserSourceRequest& currentRequest) {
    auto current = CaptureScriptUserSourcePlan(currentRequest);
    if (!current.Succeeded() || !Equal(plan, *current.value)) {
        Fail(current.diagnostics, "SCRIPT_USER_SOURCE_STALE", "脚本来源身份已改变或无法重新确认；须重新检查。");
        return Status{std::move(current.diagnostics)};
    }
    return Status::Ok();
}
} // namespace War3Map::Script
