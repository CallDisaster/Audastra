#include "War3MapCore/Script/ScriptToolchainProfile.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"
#include "ScriptSourceBinding.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <string_view>
#include <Windows.h>

namespace War3Map::Script {
namespace {

constexpr std::size_t MaxSources = 128;
constexpr std::size_t MaxApis = 128;
constexpr std::size_t MaxNameBytes = 128;
constexpr std::size_t MaxPathCharacters = 4096;
constexpr std::size_t MaxIdentityBytes = 1024 * 1024;

Result<std::string> Fail(std::string code, std::string message) {
    return Result<std::string>::Failure("Script.Profile", std::move(code), std::move(message));
}

bool IsName(std::string_view text, bool api = false) {
    if (text.empty() || text.size() > MaxNameBytes || Text::TrimHorizontal(text) != text) return false;
    auto letter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (api) {
        bool first = true;
        for (const unsigned char c : text) {
            if (c == '.') {
                if (first) return false;
                first = true;
                continue;
            }
            if (first && !letter(c) && c != '_') return false;
            if (!letter(c) && !(c >= '0' && c <= '9') && c != '_') return false;
            first = false;
        }
        return !first;
    }
    return std::all_of(text.begin(), text.end(), [&](unsigned char c) {
        return letter(c) || (c >= '0' && c <= '9') || c == '_' || (!api && (c == '-' || c == '.'));
    });
}

bool IsEncoding(ScriptProfileEncoding encoding) {
    return encoding == ScriptProfileEncoding::Utf8 || encoding == ScriptProfileEncoding::Gbk ||
        encoding == ScriptProfileEncoding::UniqueUtf8OrGbk;
}

bool IsSourceRole(ScriptProfileSourceRole role) {
    switch (role) {
    case ScriptProfileSourceRole::Injection:
    case ScriptProfileSourceRole::UserInclude:
    case ScriptProfileSourceRole::SystemInclude:
    case ScriptProfileSourceRole::ForceInclude:
    case ScriptProfileSourceRole::Native: return true;
    }
    return false;
}

bool IsDirective(ScriptProfileDirective directive) {
    switch (directive) {
    case ScriptProfileDirective::Def:
    case ScriptProfileDirective::Ifdef:
    case ScriptProfileDirective::Else:
    case ScriptProfileDirective::Endif:
    case ScriptProfileDirective::Include: return true;
    }
    return false;
}

// 只做词法校验；不canonical、不解析符号链接，也不触碰声明目录。
bool IsRootPath(const std::filesystem::path& path) {
    if (path.empty() || path.native().size() > MaxPathCharacters || !path.is_absolute()) return false;
    for (const auto character : path.native()) {
        if (static_cast<std::uint32_t>(character) < 0x20) return false;
    }
    for (const auto& component : path) if (component == "..") return false;
    return true;
}

bool IsHostApi(std::string_view api) {
    // API清单不是宿主脚本入口；未在这里实现的公开API仍需后续能力注册。
    constexpr std::string_view names[]{"io", "os", "load", "loadfile", "dofile", "require", "package", "debug"};
    const auto root = api.substr(0, api.find('.'));
    return std::find(std::begin(names), std::end(names), root) != std::end(names);
}

class IdentityWriter {
public:
    bool Add(std::string_view value) {
        const auto length = std::to_string(value.size());
        const auto overhead = length.size() + 1;
        if (overhead > MaxIdentityBytes - text_.size() || value.size() > MaxIdentityBytes - text_.size() - overhead) return false;
        text_.append(length);
        text_.push_back(':');
        text_.append(value);
        return true;
    }
    bool Number(std::uint64_t value) { return Add(std::to_string(value)); }
    bool Path(const std::filesystem::path& path) {
        const auto bytes = path.lexically_normal().generic_u8string();
        return Add(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    const std::string& Text() const { return text_; }
private:
    std::string text_;
};

} // namespace

const ScriptProfileIncludeAccess* MatchScriptIncludeAccess(
    std::span<const ScriptProfileIncludeAccess> declarations, const std::filesystem::path& source) {
    std::filesystem::path path;
    if (!Detail::NormalizeSourcePath(source, path)) return nullptr;
    const ScriptProfileIncludeAccess* selected = nullptr;
    std::size_t selectedLength = 0;
    for (const auto& declaration : declarations) {
        std::filesystem::path root;
        if (!Detail::NormalizeSourcePath(declaration.path, root)) continue;
        if (declaration.kind == ScriptIncludeAccessKind::ExactFile) {
            if (CompareStringOrdinal(root.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) return &declaration;
            continue;
        }
        if (declaration.kind != ScriptIncludeAccessKind::Directory) continue;
        auto part = path.begin(); bool within = true;
        for (const auto& expected : root) {
            if (part == path.end() || CompareStringOrdinal(part->c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
                within = false; break;
            }
            ++part;
        }
        if (within && (!selected || root.native().size() > selectedLength)) {
            selected = &declaration; selectedLength = root.native().size();
        }
    }
    return selected;
}

Result<std::string> ValidateScriptToolchainProfile(const ScriptToolchainProfile& profile) {
    if (profile.schemaVersion != 1 || profile.profileRevision == 0 || !IsName(profile.profileId) || !IsName(profile.rulesetVersion)) {
        return Fail("SCRIPT_PROFILE_INVALID", "工具链档案的schema、修订或逻辑名称无效。");
    }
    if ((profile.layoutKind != ScriptProfileLayoutKind::LegacyShareMpq && profile.layoutKind != ScriptProfileLayoutKind::ModernUi)
        || !IsEncoding(profile.mainEncoding) || profile.targetVersionMacro < 100 || profile.targetVersionMacro > 199
        || (profile.baseLibraryCategory != ScriptProfileBaseLibraryCategory::Pre124 && profile.baseLibraryCategory != ScriptProfileBaseLibraryCategory::Post124)) {
        return Fail("SCRIPT_PROFILE_INVALID", "工具链档案的布局、编码、目标宏或基础库类别无效。");
    }
    if (profile.textPolicy != ScriptProfileTextPolicy::NormalTextOnly || profile.namingPolicy != ScriptProfileNamingPolicy::IdentifierOnly
        || (profile.returnbugPolicy != ScriptProfileReturnbugPolicy::Strict &&
            profile.returnbugPolicy != ScriptProfileReturnbugPolicy::InventoriedOnly)) {
        return Fail("SCRIPT_PROFILE_POLICY_UNSUPPORTED", "档案只允许正常文本、标识符改名和明确的严格/旧档位盘点检查声明，不允许历史文本怪癖或全文替换资格。");
    }
    if ((profile.returnbugPolicy == ScriptProfileReturnbugPolicy::Strict && !profile.returnbugPatternSetVersion.empty()) ||
        (profile.returnbugPolicy == ScriptProfileReturnbugPolicy::InventoriedOnly &&
            (profile.targetVersionMacro >= 124 || profile.baseLibraryCategory != ScriptProfileBaseLibraryCategory::Pre124 ||
                profile.returnbugPatternSetVersion != kPre124ReturnbugPatternSet))) {
        return Fail("SCRIPT_PROFILE_RETURNBUG_SCOPE", "盘点例外只限1.24以前目标及Pre124基础库档位的YDWEBase_returnbug；不透明导入不授予主脚本类型例外。");
    }
    if (profile.sourceRoots.size() > MaxSources || profile.ordinaryIncludeAccess.size() > MaxSources ||
        profile.templateApis.size() > MaxApis || profile.directives.size() > 5) {
        return Fail("SCRIPT_PROFILE_LIMIT", "工具链档案的来源、模板API或指令清单超过有界声明预算。");
    }
    if (!IsName(profile.directiveDialectVersion) || (!profile.templateApiVersion.empty() && !IsName(profile.templateApiVersion))
        || (!profile.templateApis.empty() && profile.templateApiVersion.empty())
        || (!profile.returnbugPatternSetVersion.empty() && !IsName(profile.returnbugPatternSetVersion))) {
        return Fail("SCRIPT_PROFILE_INVALID", "工具链档案的方言、模板API或returnbug模式集版本声明无效。");
    }
    for (std::size_t index = 0; index < profile.templateApis.size(); ++index) {
        const auto& api = profile.templateApis[index];
        if (!IsName(api, true) || IsHostApi(api) || std::find(profile.templateApis.begin(), profile.templateApis.begin() + index, api) != profile.templateApis.begin() + index) {
            return Fail("SCRIPT_PROFILE_API_INVALID", "模板API必须为有界、唯一的公开标识符，不能声明宿主文件、系统或动态加载入口。");
        }
    }
    if (profile.moduleLoaders.size() > 8) return Fail("SCRIPT_PROFILE_LIMIT", "模块加载声明超过8项。");
    for (std::size_t i = 0; i < profile.moduleLoaders.size(); ++i) {
        const auto& loader = profile.moduleLoaders[i];
        if (!ScriptModuleLoaderIdentity(loader).Succeeded() || std::any_of(profile.moduleLoaders.begin(),
            profile.moduleLoaders.begin() + i, [&](const auto& prior) {
                return prior.id == loader.id || prior.functionName == loader.functionName;
            })) return Fail("SCRIPT_PROFILE_MODULE_LOADER_INVALID", "模块加载声明无效或有重复身份、函数名。");
    }
    if (profile.featureExtensions.size() > 64 || profile.unsupportedPlugins.size() > 64)
        return Fail("SCRIPT_PROFILE_LIMIT", "框架编译插件声明超过64项。");
    const auto identifier = [](std::string_view text) {
        return !text.empty() && text.size() <= 128 && !(text.front() >= '0' && text.front() <= '9') &&
            std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            });
    };
    for (std::size_t index = 0; index < profile.featureExtensions.size(); ++index) {
        const auto& extension = profile.featureExtensions[index];
        // 头文件只是单个文件名，由已绑定的包含根查找；不接受路径、驱动器或 ..。
        const bool header = !extension.forceInclude.empty() && extension.forceInclude.size() <= 128 &&
            extension.forceInclude.front() != '.' && std::all_of(extension.forceInclude.begin(), extension.forceInclude.end(),
                [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'; });
        if (!identifier(extension.feature) || !header ||
            (!extension.templateLibrary.empty() && extension.templateLibrary != kUserdataGcTemplateLibrary) ||
            std::any_of(profile.featureExtensions.begin(), profile.featureExtensions.begin() + index,
                [&](const auto& prior) { return prior.feature == extension.feature; }))
            return Fail("SCRIPT_PROFILE_PLUGIN_INVALID", "框架特性插件需要唯一特性名、单个头文件名和已实现的模板函数集。");
    }
    for (const auto& plugin : profile.unsupportedPlugins) {
        if (!identifier(plugin.name) || plugin.identifiers.empty() || plugin.identifiers.size() > 64 ||
            !std::all_of(plugin.identifiers.begin(), plugin.identifiers.end(), identifier))
            return Fail("SCRIPT_PROFILE_PLUGIN_INVALID", "未支持插件声明需要插件名和有界的触发标识符。");
    }
    for (std::size_t index = 0; index < profile.directives.size(); ++index) {
        const auto directive = profile.directives[index];
        if (!IsDirective(directive) || std::find(profile.directives.begin(), profile.directives.begin() + index, directive) != profile.directives.begin() + index) {
            return Fail("SCRIPT_PROFILE_INVALID", "指令能力枚举无效或重复；声明清单不授予指令执行资格。");
        }
    }
    try {
        std::size_t pathCharacters = 0;
        auto checkPath = [&](const std::filesystem::path& path) {
            if (!IsRootPath(path) || path.native().size() > MaxIdentityBytes / 4 - pathCharacters) return false;
            pathCharacters += path.native().size();
            return true;
        };
        if (!checkPath(profile.toolRoot) || !checkPath(profile.uiRoot) || !checkPath(profile.mainSourceDirectory)) {
            return Fail("SCRIPT_PROFILE_PATH_INVALID", "档案根路径必须为有界绝对路径，不能含父目录段或控制字符。");
        }
        std::size_t nativeRoots = 0;
        for (const auto& source : profile.sourceRoots) {
            if (!IsSourceRole(source.role) || !IsEncoding(source.encoding)) return Fail("SCRIPT_PROFILE_INVALID", "来源角色或编码枚举无效。");
            if (source.role == ScriptProfileSourceRole::Native && ++nativeRoots > 1) return Fail("SCRIPT_PROFILE_LIMIT", "档案最多声明一个native实现根；声明不证明其实现已受支持。");
            if (!checkPath(source.path)) return Fail("SCRIPT_PROFILE_PATH_INVALID", "来源必须为有界绝对路径，不能含父目录段或控制字符。");
        }
        if (profile.sourceEncodings.size() > 1024) return Fail("SCRIPT_PROFILE_LIMIT", "逐来源编码声明超过1024项。");
        std::vector<std::filesystem::path> encodingPaths;
        for (const auto& source : profile.sourceEncodings) {
            if ((source.encoding != ScriptProfileEncoding::Utf8 && source.encoding != ScriptProfileEncoding::Gbk) ||
                !checkPath(source.path) || std::any_of(encodingPaths.begin(), encodingPaths.end(), [&](const auto& prior) {
                    return CompareStringOrdinal(prior.c_str(), -1, source.path.c_str(), -1, TRUE) == CSTR_EQUAL;
                }))
                return Fail("SCRIPT_PROFILE_ENCODING_INVALID", "逐来源编码必须为唯一绝对路径上的显式UTF8/GBK选择。");
            encodingPaths.push_back(source.path);
        }
        std::vector<std::filesystem::path> accessPaths;
        for (const auto& access : profile.ordinaryIncludeAccess) {
            std::filesystem::path normalized;
            if ((access.kind != ScriptIncludeAccessKind::Directory && access.kind != ScriptIncludeAccessKind::ExactFile) ||
                !IsEncoding(access.encoding)) return Fail("SCRIPT_PROFILE_INCLUDE_ACCESS_INVALID", "普通include资格的类型或编码无效。");
            if (!checkPath(access.path) || !Detail::NormalizeSourcePath(access.path, normalized))
                return Fail("SCRIPT_PROFILE_INCLUDE_ACCESS_INVALID", "普通include资格需要有界绝对普通路径，不能包含设备路径、ADS或父目录段。");
            if (std::any_of(accessPaths.begin(), accessPaths.end(), [&](const auto& prior) {
                return CompareStringOrdinal(prior.c_str(), -1, normalized.c_str(), -1, TRUE) == CSTR_EQUAL;
            })) return Fail("SCRIPT_PROFILE_INCLUDE_ACCESS_CONFLICT", "同一路径不能重复或冲突声明普通include资格。");
            accessPaths.push_back(std::move(normalized));
        }
        IdentityWriter identity;
        bool valid = identity.Add("War3Map.ScriptToolchainProfile.v1") && identity.Number(profile.schemaVersion)
            && identity.Number(profile.profileRevision) && identity.Add(profile.profileId) && identity.Add(profile.rulesetVersion)
            && identity.Number(static_cast<std::uint64_t>(profile.layoutKind)) && identity.Path(profile.toolRoot)
            && identity.Path(profile.uiRoot) && identity.Path(profile.mainSourceDirectory)
            && identity.Number(static_cast<std::uint64_t>(profile.mainEncoding)) && identity.Number(profile.sourceRoots.size());
        for (const auto& source : profile.sourceRoots) {
            valid = valid && identity.Number(static_cast<std::uint64_t>(source.role)) && identity.Path(source.path)
                && identity.Number(source.required ? 1 : 0) && identity.Number(static_cast<std::uint64_t>(source.encoding));
        }
        valid = valid && identity.Number(profile.sourceEncodings.size());
        for (const auto& source : profile.sourceEncodings)
            valid = valid && identity.Path(source.path) && identity.Number(static_cast<std::uint64_t>(source.encoding));
        valid = valid && identity.Number(static_cast<std::uint64_t>(profile.targetVersionMacro))
            && identity.Number(static_cast<std::uint64_t>(profile.baseLibraryCategory))
            && identity.Number(static_cast<std::uint64_t>(profile.textPolicy)) && identity.Number(static_cast<std::uint64_t>(profile.namingPolicy))
            && identity.Add(profile.templateApiVersion) && identity.Number(profile.templateApis.size());
        for (const auto& api : profile.templateApis) valid = valid && identity.Add(api);
        valid = valid && identity.Number(static_cast<std::uint64_t>(profile.returnbugPolicy)) && identity.Add(profile.returnbugPatternSetVersion)
            && identity.Add(profile.directiveDialectVersion) && identity.Number(profile.directives.size());
        for (const auto directive : profile.directives) valid = valid && identity.Number(static_cast<std::uint64_t>(directive));
        // 空列表不追加任何字节，保留已有档案的逻辑SHA和默认行为。
        if (!profile.ordinaryIncludeAccess.empty()) {
            valid = valid && identity.Add("War3Map.OrdinaryIncludeAccess.v1") && identity.Number(profile.ordinaryIncludeAccess.size());
            for (const auto& access : profile.ordinaryIncludeAccess) valid = valid && identity.Path(access.path) &&
                identity.Number(static_cast<std::uint64_t>(access.kind)) && identity.Number(static_cast<std::uint64_t>(access.encoding)) &&
                identity.Number(access.required ? 1 : 0);
        }
        if (!profile.featureExtensions.empty() || !profile.unsupportedPlugins.empty()) {
            valid = valid && identity.Add("War3Map.FrameworkPlugins.v1") && identity.Number(profile.featureExtensions.size());
            for (const auto& extension : profile.featureExtensions) valid = valid && identity.Add(extension.feature) &&
                identity.Add(extension.forceInclude) && identity.Add(extension.templateLibrary);
            valid = valid && identity.Number(profile.unsupportedPlugins.size());
            for (const auto& plugin : profile.unsupportedPlugins) {
                valid = valid && identity.Add(plugin.name) && identity.Number(plugin.identifiers.size());
                for (const auto& name : plugin.identifiers) valid = valid && identity.Add(name);
            }
        }
        if (!profile.moduleLoaders.empty()) {
            valid = valid && identity.Add("War3Map.ModuleLoaders.v1") && identity.Number(profile.moduleLoaders.size());
            for (const auto& loader : profile.moduleLoaders) valid = valid && identity.Add(*ScriptModuleLoaderIdentity(loader).value);
        }
        if (!valid) return Fail("SCRIPT_PROFILE_LIMIT", "工具链档案的完整逻辑身份超过1MiB预算。");
        return Sha256Text(identity.Text());
    } catch (const std::exception&) {
        return Fail("SCRIPT_PROFILE_PATH_INVALID", "工具链档案路径无法按声明转换为有界逻辑身份。");
    }
}

} // namespace War3Map::Script
