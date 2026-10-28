#include "War3MapCore/Script/ScriptFrameworkInputs.h"

#include "ScriptSourceBinding.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/SourceEncoding.h"
#include "../Jass/JassLexer.h"
#include "../Jass/PpLexer.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>
#include <Windows.h>

namespace War3Map::Script {
namespace {
using Kind = ScriptToolchainBindingKind;
constexpr std::size_t kUiFiles = 2048;
constexpr std::uint64_t kUiBytes = 32ull * 1024 * 1024;
const std::vector<std::string> kTemplateApis = {"StringHash", "abilityOrder", "tostring", "tonumber",
    "math.modf", "math.floor", "math.ceil", "math.abs", "math.max", "math.min"};
const std::vector<ScriptProfileDirective> kFeatureDirectives = {ScriptProfileDirective::Def,
    ScriptProfileDirective::Ifdef, ScriptProfileDirective::Else, ScriptProfileDirective::Endif, ScriptProfileDirective::Include};

template <typename T> Result<T> Failed(const Status& status) {
    return {{}, status.diagnostics};
}
std::uint32_t SourceLine(std::string_view text, std::size_t offset) {
    std::uint32_t line = 1;
    for (std::size_t i = 0; i < (std::min)(offset, text.size()); ++i)
        if (text[i] == '\r' || (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))) ++line;
    return line;
}
void Locate(Diagnostic& diagnostic, const std::filesystem::path& source, std::uint32_t line = 0) {
    if (source.empty()) return;
    if (!diagnostic.origin) diagnostic.origin = OriginSpan{source}; else diagnostic.origin->source = source;
    const auto name = source.generic_u8string();
    diagnostic.message += " [来源 " + std::string(name.begin(), name.end());
    if (line) diagnostic.message += ":" + std::to_string(line);
    diagnostic.message += "，原字节偏移 " + std::to_string(diagnostic.origin->offset) +
        "，长度 " + std::to_string(diagnostic.origin->length) + "]";
}
Status Fail(std::string code, std::string message, const std::filesystem::path& source = {},
    std::size_t offset = 0, std::size_t bytes = 0, std::uint32_t line = 0) {
    auto result = Status::Failure("Script.Framework", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{source, offset, bytes, {}};
    Locate(result.diagnostics.back(), source, line);
    return result;
}
bool SameBinding(const ScriptToolchainFileBinding& a, const ScriptToolchainFileBinding& b) {
    return a.path == b.path && a.kind == b.kind && a.sha256 == b.sha256 && a.bytes == b.bytes && a.maxBytes == b.maxBytes;
}
bool SupportedCapabilities(const ScriptToolchainProfile& profile) {
    const bool directives = profile.directives.empty() || (profile.directiveDialectVersion == "ydwe-dollar-features-v1" &&
        profile.directives == kFeatureDirectives);
    return profile.rulesetVersion == "ydwe-legacy-framework-v1" && directives &&
        profile.templateApiVersion == "forge-restricted-template-v1" && profile.templateApis == kTemplateApis &&
        profile.returnbugPolicy == ScriptProfileReturnbugPolicy::Strict;
}
Result<std::vector<ScriptToolchainFileBinding>> CaptureUiBindings(const ScriptFrameworkInputs& inputs) {
    using Capture = Result<std::vector<ScriptToolchainFileBinding>>;
    if (inputs.catalog.Manifest().size() > kUiFiles)
        return Capture::Failure("Script.Framework", "SCRIPT_FRAMEWORK_UI_BUDGET", "UI来源清单超过2048项。");
    std::vector<ScriptToolchainFileBinding> bindings;
    std::uint64_t total = 0;
    for (const auto& source : inputs.catalog.Manifest()) {
        // config可由LoadLayers合成；物理config由entryPlan独立绑定。
        if (source.relativePath == "ui/config") continue;
        const auto relative = source.relativePath.lexically_relative("ui");
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            return Capture::Failure("Script.Framework", "SCRIPT_FRAMEWORK_UI_PATH", "UI来源路径越出显式目录。");
        auto file = Detail::CaptureSourceBinding(inputs.profile.uiRoot / relative, Kind::RegularFile,
            source.present, (std::min)(4ull * 1024 * 1024, kUiBytes - total));
        if (!file.Succeeded()) return {{}, std::move(file.diagnostics)};
        if ((source.present && (file.value->binding.sha256 != source.sha256 || file.value->binding.bytes != source.size)) ||
            (!source.present && file.value->binding.kind != Kind::Missing))
            return Failed<std::vector<ScriptToolchainFileBinding>>(Fail("SCRIPT_FRAMEWORK_UI_STALE",
                "实际UI正负来源与冻结catalog不一致。", file.value->binding.path));
        total += file.value->binding.bytes;
        bindings.push_back(std::move(file.value->binding));
    }
    return Capture::Success(std::move(bindings));
}
ScriptProfileEncoding EncodingForRoots(const std::vector<ScriptProfileSourceRoot>& roots,
    ScriptProfileEncoding fallback, const std::filesystem::path& path) {
    // 所有参与根来自显式角色；嵌套根由最具体声明决定。
    const ScriptProfileSourceRoot* selected = nullptr;
    for (const auto& source : roots) {
        if (source.role == ScriptProfileSourceRole::ForceInclude && source.path == path) return source.encoding;
        if (source.role == ScriptProfileSourceRole::Native || source.role == ScriptProfileSourceRole::ForceInclude) continue;
        const auto relative = path.lexically_relative(source.path);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") continue;
        if (!selected || source.path.native().size() > selected->path.native().size()) selected = &source;
    }
    return selected ? selected->encoding : fallback;
}
// 两种编码下分词完全相同，且每个非 ASCII 字节都落在注释（不在任何记号内）、字符串或 rawcode 记号中。
bool NonAsciiOnlyOpaque(std::string_view original) {
    const auto utf = Jass::detail::LexPp(original, Text::SourceEncoding::Utf8);
    const auto gbk = Jass::detail::LexPp(original, Text::SourceEncoding::Gbk);
    if (!utf.Succeeded() || !gbk.Succeeded() || utf.value->size() != gbk.value->size()) return false;
    const auto& tokens = *utf.value;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& a = tokens[index]; const auto& b = (*gbk.value)[index];
        if (a.offset != b.offset || a.End() != b.End() || a.kind != b.kind) return false;
    }
    std::size_t index = 0;
    for (std::uint32_t at = 0; at < original.size(); ++at) {
        if (static_cast<unsigned char>(original[at]) < 0x80) continue;
        while (index < tokens.size() && tokens[index].End() <= at) ++index;
        if (index < tokens.size() && tokens[index].offset <= at && tokens[index].kind != Jass::detail::PpKind::String
            && tokens[index].kind != Jass::detail::PpKind::Char) return false;
    }
    return true;
}
ScriptProfileEncoding EncodingFor(const ScriptFrameworkInputs& inputs, const std::filesystem::path& path) {
    return EncodingForRoots(inputs.profile.sourceRoots, inputs.profile.mainEncoding, path);
}
} // namespace

ScriptProfileEncoding FrameworkEncodingFor(const ScriptFrameworkInputs& inputs, const std::filesystem::path& source) {
    for (const auto& declaration : inputs.profile.sourceEncodings)
        if (CompareStringOrdinal(declaration.path.c_str(), -1, source.c_str(), -1, TRUE) == CSTR_EQUAL) return declaration.encoding;
    if (const auto* access = MatchScriptIncludeAccess(inputs.profile.ordinaryIncludeAccess, source)) return access->encoding;
    return EncodingFor(inputs, source);
}
ScriptProfileEncoding FrameworkEncodingFor(const ScriptUserSourcePlan& plan, const std::filesystem::path& source) {
    for (const auto& declaration : plan.profileSourceEncodings)
        if (CompareStringOrdinal(declaration.path.c_str(), -1, source.c_str(), -1, TRUE) == CSTR_EQUAL) return declaration.encoding;
    if (const auto* access = MatchScriptIncludeAccess(plan.profileOrdinaryIncludeAccess, source)) return access->encoding;
    return EncodingForRoots(plan.profileEncodingRoots, plan.profileMainEncoding, source);
}

Result<Jass::SourceLexicalView> MakeFrameworkLexicalView(std::string_view original,
    const std::filesystem::path& source, ScriptProfileEncoding declaration) {
    using R = Result<Jass::SourceLexicalView>;
    const auto validate = [&](Text::SourceEncoding encoding) {
        Status status = Status::Ok();
        for (std::uint32_t at = 0; at < original.size();) {
            if (static_cast<unsigned char>(original[at]) < 0x80) { ++at; continue; }
            const auto scalar = Text::ReadSourceScalar(original, at, encoding);
            if (!scalar.Succeeded()) { status.diagnostics = scalar.diagnostics; break; }
            at += scalar.value->bytes;
        }
        for (auto& diagnostic : status.diagnostics) {
            Locate(diagnostic, source, SourceLine(original, diagnostic.origin ? diagnostic.origin->offset : 0));
        }
        return status;
    };
    if (original.size() > 16u * 1024u * 1024u) return Failed<Jass::SourceLexicalView>(Fail(
        "SCRIPT_FRAMEWORK_SOURCE_BUDGET", "参与来源超过16MiB。", source));
    auto encoding = Text::SourceEncoding::Utf8;
    if (declaration == ScriptProfileEncoding::UniqueUtf8OrGbk) {
        const auto utf = validate(Text::SourceEncoding::Utf8), gbk = validate(Text::SourceEncoding::Gbk);
        const auto first = std::find_if(original.begin(), original.end(), [](unsigned char c) { return c >= 0x80; });
        if (utf.Succeeded() && gbk.Succeeded() && first != original.end() && NonAsciiOnlyOpaque(original)) {
            // 两种读法都合法，但分词一致且中文只在注释、字符串、rawcode 里：编码选择不改变代码含义。
            // 按原字节保留（UTF-8 视图不改写文本），只提示；中文出现在标识符等代码里时仍须显式声明编码。
            Jass::SourceLexicalView view; view.text = original;
            auto result = R::Success(std::move(view));
            result.diagnostics.push_back({DiagnosticSeverity::Info, "Script.Framework", "SCRIPT_FRAMEWORK_SOURCE_ENCODING_OPAQUE",
                "该来源按 UTF-8 和 GBK 都能读通，中文只出现在注释、字符串或 rawcode 中，不影响代码含义；已按原字节保留。",
                OriginSpan{source}});
            return result;
        }
        if (utf.Succeeded() && gbk.Succeeded() && first != original.end()) {
            const auto offset = static_cast<std::size_t>(first - original.begin());
            const auto scalar = Text::ReadSourceScalar(original, static_cast<std::uint32_t>(offset), Text::SourceEncoding::Utf8);
            return Failed<Jass::SourceLexicalView>(Fail("SCRIPT_FRAMEWORK_SOURCE_ENCODING_AMBIGUOUS",
                "非ASCII来源同时符合UTF-8与精确CP936；须显式声明编码。", source, offset,
                scalar.value->bytes, SourceLine(original, offset)));
        }
        if (!utf.Succeeded() && !gbk.Succeeded()) return Failed<Jass::SourceLexicalView>(utf);
        if (!utf.Succeeded()) encoding = Text::SourceEncoding::Gbk;
    } else if (declaration == ScriptProfileEncoding::Gbk) encoding = Text::SourceEncoding::Gbk;
    else if (declaration != ScriptProfileEncoding::Utf8) return Failed<Jass::SourceLexicalView>(Fail(
        "SCRIPT_FRAMEWORK_SOURCE_ENCODING_UNSUPPORTED", "未知来源编码资格。", source));
    auto valid = validate(encoding);
    if (!valid.Succeeded()) return Failed<Jass::SourceLexicalView>(valid);
    auto tokens = Jass::detail::LexPp(original, encoding);
    if (!tokens.Succeeded()) {
        for (auto& diagnostic : tokens.diagnostics) {
            Locate(diagnostic, source, SourceLine(original, diagnostic.origin ? diagnostic.origin->offset : 0));
        }
        return {{}, std::move(tokens.diagnostics)};
    }
    Jass::SourceLexicalView view; view.text = original;
    if (encoding == Text::SourceEncoding::Utf8) return R::Success(std::move(view));
    auto hash = Sha256Text(original);
    if (!hash.Succeeded()) return {{}, std::move(hash.diagnostics)};
    std::size_t tokenIndex = 0;
    std::uint32_t sourceLine = 1;
    std::size_t opaqueStorage = 0;
    for (std::uint32_t at = 0; at < original.size();) {
        const auto scalar = Text::ReadSourceScalar(original, at, encoding);
        const auto width = scalar.value->bytes;
        while (tokenIndex < tokens.value->size() && (*tokens.value)[tokenIndex].End() <= at) ++tokenIndex;
        if (static_cast<unsigned char>(original[at]) >= 0x80) {
            auto kind = Jass::SourceOpaqueKind::Comment;
            if (tokenIndex < tokens.value->size() && (*tokens.value)[tokenIndex].offset <= at) {
                const auto& token = (*tokens.value)[tokenIndex];
                if (token.kind == Jass::detail::PpKind::String) kind = Jass::SourceOpaqueKind::String;
                else if (token.kind == Jass::detail::PpKind::Char) kind = Jass::SourceOpaqueKind::Rawcode;
                else return Failed<Jass::SourceLexicalView>(Fail("SCRIPT_FRAMEWORK_GBK_IDENTIFIER_UNSUPPORTED",
                    "GBK非ASCII代码token尚未支持跨来源绑定；不转码或猜测标识符。", source, at, width, sourceLine));
            }
            const auto ownedBytes = sizeof(Jass::SourceOpaqueSpan) + 128 + source.native().size() * sizeof(std::filesystem::path::value_type);
            if (view.opaque.size() >= 262144 || ownedBytes > 32u * 1024u * 1024u - opaqueStorage) return Failed<Jass::SourceLexicalView>(Fail(
                "SCRIPT_FRAMEWORK_SOURCE_BUDGET", "不透明标量位置超过262144项。", source, at, width));
            opaqueStorage += ownedBytes;
            std::fill_n(view.text.begin() + at, width, 'x'); // 尾0x5c属于已验证标量，不能当成转义/续行。
            view.opaque.push_back({at, width, kind, OriginSpan{source, at, width, *hash.value}, *hash.value, sourceLine});
        }
        if (original[at] == '\r' || (original[at] == '\n' && (at == 0 || original[at - 1] != '\r'))) ++sourceLine;
        at += width;
    }
    auto checked = Jass::detail::LexPp(view.text, Text::SourceEncoding::Utf8);
    if (!checked.Succeeded()) return {{}, std::move(checked.diagnostics)};
    auto result = R::Success(std::move(view));
    result.diagnostics.push_back({DiagnosticSeverity::Info, "Script.Framework", "SCRIPT_FRAMEWORK_GBK_OPAQUE_PRESERVED",
        "该来源由严格唯一/显式CP936资格读取；字符串、注释、rawcode原字节保留，等长词法视图不写入地图。", OriginSpan{source}});
    return result;
}

ScriptUserSourceRequest FrameworkSourceRequest(const ScriptFrameworkInputs& inputs) {
    ScriptUserSourceRequest request;
    request.layout = &inputs.layout; request.catalog = &inputs.catalog;
    request.uiRoot = inputs.profile.uiRoot; request.mapDirectory = inputs.profile.mainSourceDirectory;
    request.profile = &inputs.profile;
    return request;
}
Result<Jass::SourceLexicalView> RebindFrameworkLexicalView(std::string_view output,
    std::string_view ownedOriginal, const Jass::SourceLexicalView& ownedView) {
    using R = Result<Jass::SourceLexicalView>;
    auto valid = Jass::ValidateSourceLexicalView(ownedOriginal, ownedView);
    if (!valid.Succeeded()) return {{}, std::move(valid.diagnostics)};
    if (output.size() > 16u * 1024u * 1024u) return R::Failure("Script.Framework",
        "SCRIPT_FRAMEWORK_OUTPUT_ENCODING_BUDGET", "最终文本超过16MiB来源重绑定预算。");
    struct OpaqueRegion { std::uint32_t begin, end; std::vector<Jass::SourceOpaqueSpan> spans; };
    std::vector<OpaqueRegion> regions;
    std::size_t opaque = 0;
    const auto& view = ownedView.text;
    for (std::uint32_t at = 0; at < view.size();) {
        const auto begin = at;
        if (view.compare(at, 2, "//") == 0) {
            const auto end = view.find_first_of("\r\n", at); at = static_cast<std::uint32_t>(end == std::string::npos ? view.size() : end);
        } else if (view.compare(at, 2, "/*") == 0) {
            const auto end = view.find("*/", at + 2); at = static_cast<std::uint32_t>(end == std::string::npos ? view.size() : end + 2);
        } else if (view[at] == '"' || view[at] == '\'') {
            const char quote = view[at++];
            while (at < view.size()) {
                if (view[at] == '\\') { at += (std::min)(std::uint32_t{2}, static_cast<std::uint32_t>(view.size()) - at); continue; }
                if (view[at++] == quote) break;
            }
        } else { ++at; continue; }
        while (opaque < ownedView.opaque.size() && ownedView.opaque[opaque].offset < begin) ++opaque;
        OpaqueRegion region{begin, at, {}};
        while (opaque < ownedView.opaque.size() && ownedView.opaque[opaque].offset + ownedView.opaque[opaque].length <= at)
            region.spans.push_back(ownedView.opaque[opaque++]);
        if (!region.spans.empty()) regions.push_back(std::move(region));
    }
    if (regions.size() > 2048 || output.size() * regions.size() > 512ull * 1024 * 1024)
        return R::Failure("Script.Framework", "SCRIPT_FRAMEWORK_OUTPUT_ENCODING_BUDGET", "最终来源精确片段查询超过有界预算。");
    Jass::SourceLexicalView rebound; rebound.text = output;
    std::map<std::uint32_t, Jass::SourceOpaqueSpan> selected;
    std::size_t shared = 0;
    for (const auto& region : regions) {
        const auto bytes = ownedOriginal.substr(region.begin, region.end - region.begin);
        for (std::size_t found = output.find(bytes); found != std::string_view::npos; found = output.find(bytes, found + bytes.size())) {
            for (auto span : region.spans) {
                span.offset = static_cast<std::uint32_t>(found) + span.offset - region.begin;
                const auto [prior, inserted] = selected.emplace(span.offset, span);
                if (inserted || prior->second == span) continue;
                // 多处原文字节完全相同（如重复的中文注释）：遮挡位置、长度与类别一致，输出字节确定，
                // 只是诊断位置有多个候选，保留首次出现处。遮挡范围不同才是真歧义。
                if (prior->second.length != span.length || prior->second.kind != span.kind) return R::Failure("Script.Framework",
                    "SCRIPT_FRAMEWORK_OUTPUT_ENCODING_AMBIGUOUS", "相同最终不透明片段对应多个原来源且遮挡范围不同；未猜测归属。");
                ++shared;
            }
        }
    }
    for (const auto& [offset, span] : selected) {
        std::fill_n(rebound.text.begin() + offset, span.length, 'x'); rebound.opaque.push_back(span);
    }
    valid = Jass::ValidateSourceLexicalView(output, rebound);
    if (!valid.Succeeded()) return {{}, std::move(valid.diagnostics)};
    auto result = R::Success(std::move(rebound));
    if (shared > 0) result.diagnostics.push_back({DiagnosticSeverity::Info, "Script.Framework",
        "SCRIPT_FRAMEWORK_OUTPUT_OPAQUE_SHARED_BYTES", std::to_string(shared) +
        " 处不透明片段的原字节在多个来源中完全相同，输出字节不受影响；诊断位置取首次出现处。", std::nullopt});
    return result;
}
ScriptFrameworkDirectivePolicy FrameworkDirectivePolicy(const ScriptFrameworkInputs& inputs) {
    return inputs.profile.directiveDialectVersion == "ydwe-dollar-features-v1" && inputs.profile.directives == kFeatureDirectives
        ? ScriptFrameworkDirectivePolicy::FeatureFlagsV1 : ScriptFrameworkDirectivePolicy::None;
}

// 部分旧框架分支（如雪月）在保存流程里内置编译插件。按安装目录里实际存在的插件文件登记，
// 不按分支名判断；同样带这些文件的其他旧框架共用同一规则。
static void AddFrameworkPlugins(ScriptToolchainProfile& profile, const ScriptToolchainLayout& layout) {
    std::error_code error;
    const auto exists = [&](const std::filesystem::path& path) { return std::filesystem::is_regular_file(path, error); };
    const auto header = [&](std::string_view name) {
        return std::any_of(layout.systemIncludes.begin(), layout.systemIncludes.end(),
            [&](const std::filesystem::path& root) { return exists(root / name); });
    };
    const auto compile = layout.root / "share" / "script" / "compile";
    // $def 特性 → 强制包含，顺序与框架追加 Wave --forceinclude 的顺序一致。
    if (header("XGJAPI.h")) profile.featureExtensions.push_back({"XG_JAPI", "XGJAPI.h", {}});
    if (header("XGuserdataGC.h") && exists(compile / "userdataGC.lua"))
        profile.featureExtensions.push_back({"XGuserdataGC", "XGuserdataGC.h", std::string(kUserdataGcTemplateLibrary)});
    if (header("WenHaoJapi_xjass.h")) profile.featureExtensions.push_back({"WenHao__Japi", "WenHaoJapi_xjass.h", {}});
    // 按正文标识符触发、尚未实现的插件：出现即拒绝。
    if (exists(compile / "XGMacro.lua")) profile.unsupportedPlugins.push_back({"XGMacro", {"XG_Macro_Set", "XG_Macro_Get"}});
    if (exists(compile / "XGStrFormat.lua"))
        profile.unsupportedPlugins.push_back({"XGStrFormat", {"XG_StrFormat_Reg", "XG_StrFormat_Do", "XG_StrFormat_AIO"}});
    if (exists(layout.root / "jass" / "XueYue" / "SystemLibs" / "AutoGetAttr.j"))
        profile.unsupportedPlugins.push_back({"AutoGetAttr", {"XG_AutoAttr_SetClassA", "XG_AutoAttr_AddAttrA", "XG_AutoAttr_StartA",
            "XG_AutoAttr_GetAttrNum", "XG_AutoAttr_GetAttrKey", "XG_AutoAttr_GetAttrVal"}});
    if (exists(layout.root / "jass" / "XueYue" / "Calls" / "GetHeroMainAttr.j"))
        profile.unsupportedPlugins.push_back({"GetHeroMainAttr", {"XG_GetHeroMainAttr"}});
}

Result<ScriptToolchainProfile> MakeLegacyFrameworkProfile(const ScriptToolchainLayout& layout,
    const Trigger::UiCatalog& catalog, const std::filesystem::path& uiRoot,
    const std::filesystem::path& mapDirectory) {
    if (layout.executionPolicy != ScriptToolchainExecutionPolicy::FrameworkInputsRequired ||
        layout.kind != ScriptToolchainLayoutKind::LegacyShareMpq || catalog.Layers().empty()) {
        return Result<ScriptToolchainProfile>::Failure("Script.Framework", "SCRIPT_FRAMEWORK_LAYOUT_UNSUPPORTED",
            "正常旧框架档案需要已绑定share/mpq布局和实际有效目录。");
    }
    ScriptToolchainProfile profile;
    profile.profileId = "ydwe-legacy-framework-subset-v2";
    profile.layoutKind = ScriptProfileLayoutKind::LegacyShareMpq;
    profile.toolRoot = layout.root;
    // 框架保存时把地图脚本解出到 <工具根>/logs 再交给 Wave，主文的“当前目录”就是这里：
    // 地图里写的 "../memory_hack/x.j" 指工具根下的库。地图目录是随后的第一个用户包含根。
    std::filesystem::path map;
    if (!Detail::NormalizeSourcePath(uiRoot, profile.uiRoot) ||
        !Detail::NormalizeSourcePath(layout.root / "logs", profile.mainSourceDirectory) ||
        !Detail::NormalizeSourcePath(mapDirectory, map)) {
        return Result<ScriptToolchainProfile>::Failure("Script.Framework", "SCRIPT_FRAMEWORK_ROOT_INVALID",
            "工具/UI根、框架编译目录与地图目录必须显式绑定，不使用构建临时目录或猜测Component根。");
    }
    profile.targetVersionMacro = layout.versionMacro;
    profile.baseLibraryCategory = layout.allowReturnBug ? ScriptProfileBaseLibraryCategory::Pre124
        : ScriptProfileBaseLibraryCategory::Post124;
    // 此方言只授予独立阶段的无值特性声明；不授予有值宏替换或宿主预定义。
    profile.directiveDialectVersion = "ydwe-dollar-features-v1";
    profile.directives = kFeatureDirectives;
    profile.templateApiVersion = "forge-restricted-template-v1";
    profile.templateApis = kTemplateApis;
    profile.sourceRoots.push_back({ScriptProfileSourceRole::Injection, layout.root / "jass"});
    // 与框架 Wave 的 --include 顺序一致：地图目录、工具根 jass。
    profile.sourceRoots.push_back({ScriptProfileSourceRole::UserInclude, map});
    profile.sourceRoots.push_back({ScriptProfileSourceRole::UserInclude, layout.root / "jass"});
    // 相对 .. 只在工具根内取得读取资格，越出工具根仍拒绝。
    profile.ordinaryIncludeAccess.push_back({layout.root, ScriptIncludeAccessKind::Directory,
        ScriptProfileEncoding::UniqueUtf8OrGbk, true});
    for (const auto& path : layout.systemIncludes)
        profile.sourceRoots.push_back({ScriptProfileSourceRole::SystemInclude, path});
    for (const auto& path : layout.forceIncludes)
        profile.sourceRoots.push_back({ScriptProfileSourceRole::ForceInclude, path});
    profile.sourceRoots.push_back({ScriptProfileSourceRole::Native, layout.root / "jass" / "native", false});
    for (auto& source : profile.sourceRoots) source.encoding = ScriptProfileEncoding::UniqueUtf8OrGbk;
    AddFrameworkPlugins(profile, layout);
    profile.moduleLoaders = DefaultScriptModuleLoaders();
    auto valid = ValidateScriptToolchainProfile(profile);
    if (!valid.Succeeded()) return {{}, std::move(valid.diagnostics)};
    return Result<ScriptToolchainProfile>::Success(std::move(profile));
}

void MergeDeclaredIncludeAccess(ScriptToolchainProfile& profile, std::vector<ScriptProfileIncludeAccess> declared) {
    for (auto& access : declared) {
        const auto same = std::find_if(profile.ordinaryIncludeAccess.begin(), profile.ordinaryIncludeAccess.end(),
            [&](const ScriptProfileIncludeAccess& prior) {
                return CompareStringOrdinal(prior.path.c_str(), -1, access.path.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        if (same != profile.ordinaryIncludeAccess.end()) *same = std::move(access);
        else profile.ordinaryIncludeAccess.push_back(std::move(access));
    }
}

bool LegacyFrameworkProfileBindsMap(const ScriptToolchainProfile& profile, const std::filesystem::path& mapDirectory) {
    const auto first = std::find_if(profile.sourceRoots.begin(), profile.sourceRoots.end(),
        [](const ScriptProfileSourceRoot& root) { return root.role == ScriptProfileSourceRole::UserInclude; });
    std::filesystem::path map;
    return first != profile.sourceRoots.end() && Detail::NormalizeSourcePath(mapDirectory, map) &&
        CompareStringOrdinal(first->path.c_str(), -1, map.c_str(), -1, TRUE) == CSTR_EQUAL;
}

Result<ScriptFrameworkInputs> CaptureScriptFrameworkInputs(ScriptToolchainProfile profile,
    const ScriptToolchainLayout& layout, const Trigger::UiCatalog& catalog) {
    using Capture = Result<ScriptFrameworkInputs>;
    auto verified = VerifyScriptToolchainLayout(layout);
    if (!verified.Succeeded()) return Failed<ScriptFrameworkInputs>(verified);
    if (layout.executionPolicy != ScriptToolchainExecutionPolicy::FrameworkInputsRequired ||
        !SupportedCapabilities(profile)) {
        return Capture::Failure("Script.Framework", "SCRIPT_FRAMEWORK_CAPABILITY_UNSUPPORTED",
            "此执行单元只支持正常cfg/include、无值$特性、根udg与受限模板；不授予其他方言/API或returnbug资格。");
    }
    ScriptFrameworkInputs inputs;
    inputs.profile = std::move(profile); inputs.layout = layout; inputs.catalog = catalog;
    const auto request = FrameworkSourceRequest(inputs);
    auto entry = CaptureScriptUserSourcePlan(request);
    if (!entry.Succeeded()) return {{}, std::move(entry.diagnostics)};
    inputs.entryPlan = std::move(*entry.value);
    if (inputs.entryPlan.nativeState == ScriptNativeSourceState::PresentUnsupported) {
        return Failed<ScriptFrameworkInputs>(Fail("SCRIPT_FRAMEWORK_NATIVE_UNSUPPORTED",
            "档案含native后置实现根，尚未支持其声明/实现绑定与最终检查；不静默省略。", inputs.entryPlan.nativeRoot));
    }
    auto injection = CaptureScriptInjectionSources(inputs.entryPlan, request, !layout.allowReturnBug,
        ScriptInjectionLinePolicy::FrameworkText);
    if (!injection.Succeeded()) return {{}, std::move(injection.diagnostics)};
    inputs.injection = std::move(*injection.value);
    auto ui = CaptureUiBindings(inputs);
    if (!ui.Succeeded()) return {{}, std::move(ui.diagnostics)};
    inputs.uiBindings = std::move(*ui.value);
    auto result = Capture::Success(std::move(inputs));
    result.diagnostics = std::move(injection.diagnostics);
    return result;
}

Status VerifyScriptFrameworkInputs(const ScriptFrameworkInputs& inputs, const ScriptToolchainLayout& layout) {
    auto current = VerifyScriptToolchainLayout(layout);
    if (!current.Succeeded()) return current;
    auto owned = VerifyScriptToolchainLayout(inputs.layout);
    if (!owned.Succeeded()) return owned;
    if (inputs.layout.root != layout.root || inputs.layout.executionPolicy != layout.executionPolicy ||
        inputs.layout.versionMacro != layout.versionMacro || inputs.layout.allowReturnBug != layout.allowReturnBug)
        return Fail("SCRIPT_FRAMEWORK_LAYOUT_STALE", "流水线布局与拥有的档案来源不同。");
    auto profile = ValidateScriptToolchainProfile(inputs.profile);
    if (!profile.Succeeded()) return {std::move(profile.diagnostics)};
    if (*profile.value != inputs.entryPlan.profileIdentitySha256 || !SupportedCapabilities(inputs.profile))
        return Fail("SCRIPT_FRAMEWORK_PROFILE_STALE", "档案身份或已支持的实际能力已经变化。");
    const auto request = FrameworkSourceRequest(inputs);
    auto injection = VerifyScriptInjectionSources(inputs.injection, inputs.entryPlan, request,
        !layout.allowReturnBug, ScriptInjectionLinePolicy::FrameworkText);
    if (!injection.Succeeded()) return injection;
    auto ui = CaptureUiBindings(inputs);
    if (!ui.Succeeded()) return {std::move(ui.diagnostics)};
    if (inputs.uiBindings.size() != ui.value->size())
        return Fail("SCRIPT_FRAMEWORK_UI_BINDING_INVALID", "UI绑定须覆盖完整catalog正负清单，不能删去未读取的候选。");
    for (std::size_t i = 0; i < inputs.uiBindings.size(); ++i)
        if (!SameBinding(inputs.uiBindings[i], (*ui.value)[i])) return Fail("SCRIPT_FRAMEWORK_UI_STALE",
            "UI来源内容、原缺失候选或绑定顺序改变。", inputs.uiBindings[i].path);
    return Status::Ok();
}

Status ValidateFrameworkSourceEncoding(std::string_view text, const std::filesystem::path& source,
    ScriptProfileEncoding encoding, ScriptFrameworkDirectivePolicy directives) {
    if (text.size() > 16u * 1024u * 1024u)
        return Fail("SCRIPT_FRAMEWORK_SOURCE_BUDGET", "参与阶段文本超过16MiB。", source);
    auto view = MakeFrameworkLexicalView(text, source, encoding);
    if (!view.Succeeded()) return {std::move(view.diagnostics)};
    auto tokens = Jass::detail::LexPp(view.value->text, Text::SourceEncoding::Utf8);
    if (!tokens.Succeeded()) {
        for (auto& diagnostic : tokens.diagnostics) {
            if (!diagnostic.origin) diagnostic.origin = OriginSpan{source};
            else diagnostic.origin->source = source;
        }
        return {std::move(tokens.diagnostics)};
    }
    for (const auto& token : *tokens.value) {
        if (token.lineStart && token.kind == Jass::detail::PpKind::Identifier && token.text.starts_with('$') &&
            directives == ScriptFrameworkDirectivePolicy::None)
            return Fail("SCRIPT_FRAMEWORK_DOLLAR_UNSUPPORTED", "该来源的$方言尚未实现；未作为C宏或普通标识符静默通过。",
                source, token.offset, token.text.size());
    }
    return Status::Ok();
}

Status ValidateFrameworkIncludes(const ScriptFrameworkInputs& inputs, const ScriptPreprocessSources& sources) {
    for (const auto& file : sources.includes) {
        if (file.binding.kind != Kind::RegularFile) continue;
        const std::string_view text(file.bytes.empty() ? "" : reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
        auto valid = ValidateFrameworkSourceEncoding(text, file.binding.path, FrameworkEncodingFor(inputs, file.binding.path), FrameworkDirectivePolicy(inputs));
        if (!valid.Succeeded()) return valid;
    }
    return Status::Ok();
}

Result<std::string> InjectScriptFramework(std::string_view script, const ScriptFrameworkInputs& inputs,
    std::vector<std::filesystem::path>& injected, Jass::SourceLexicalView* lexicalView,
    std::vector<std::string>* sourceSha256) {
    auto valid = ValidateFrameworkSourceEncoding(script, inputs.profile.mainSourceDirectory / "war3map.j", inputs.profile.mainEncoding,
        FrameworkDirectivePolicy(inputs));
    if (!valid.Succeeded()) return Failed<std::string>(valid);
    auto mainView = MakeFrameworkLexicalView(script, inputs.profile.mainSourceDirectory / "war3map.j", inputs.profile.mainEncoding);
    if (!mainView.Succeeded()) return {{}, std::move(mainView.diagnostics)};
    auto tokens = Jass::detail::LexPp(mainView.value->text, Text::SourceEncoding::Utf8);
    if (!tokens.Succeeded()) return {{}, std::move(tokens.diagnostics)};
    std::unordered_set<std::string_view> identifiers;
    for (const auto& token : *tokens.value)
        if (token.kind == Jass::detail::PpKind::Identifier) identifiers.insert(token.text);
    if (auto missing = CheckMissingInjectionLibraries(inputs.injection, identifiers); !missing.Succeeded())
        return {{}, std::move(missing.diagnostics)};
    std::set<std::size_t> files;
    // 旧框架按子串查找 cfg 名，作者常只写函数族前缀（如 XG_CreateJumpText 覆盖 XG_CreateJumpTextFormLoc）。
    // 这里只在代码标识符以 cfg 名开头、且该标识符确实出现在所选库文件的代码里时选中；
    // 不匹配注释、字符串或名字中段。
    const std::set<std::string_view> ordered(identifiers.begin(), identifiers.end());
    const auto libraryDeclares = [&](const ScriptInjectionSourceFile& file, std::string_view key) -> Result<bool> {
        std::vector<std::string_view> longer;
        for (auto at = ordered.upper_bound(key); at != ordered.end() && at->starts_with(key); ++at) longer.push_back(*at);
        if (longer.empty()) return Result<bool>::Success(false);
        const std::string_view text(file.bytes.empty() ? "" : reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
        auto view = MakeFrameworkLexicalView(text, file.binding.path, FrameworkEncodingFor(inputs, file.binding.path));
        if (!view.Succeeded()) return {{}, std::move(view.diagnostics)};
        auto lexed = Jass::detail::LexPp(view.value->text, Text::SourceEncoding::Utf8);
        if (!lexed.Succeeded()) return {{}, std::move(lexed.diagnostics)};
        for (const auto& token : *lexed.value)
            if (token.kind == Jass::detail::PpKind::Identifier && std::find(longer.begin(), longer.end(), token.text) != longer.end())
                return Result<bool>::Success(true);
        return Result<bool>::Success(false);
    };
    for (const auto& choice : inputs.injection.choices) {
        if (choice.selectedFile >= inputs.injection.files.size())
            return Result<std::string>::Failure("Script.Framework", "SCRIPT_FRAMEWORK_INJECTION_INVALID", "注入来源索引无效。");
        if (files.contains(choice.selectedFile)) continue;
        if (identifiers.contains(choice.key)) { files.insert(choice.selectedFile); continue; }
        auto declared = libraryDeclares(inputs.injection.files[choice.selectedFile], choice.key);
        if (!declared.Succeeded()) return {{}, std::move(declared.diagnostics)};
        if (*declared.value) files.insert(choice.selectedFile);
    }
    std::vector<const ScriptInjectionSourceFile*> selected;
    for (const auto index : files) selected.push_back(&inputs.injection.files[index]);
    std::sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) { return a->binding.path < b->binding.path; });
    std::string output(script);
    auto view = std::move(*mainView.value);
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> hashes;
    std::vector<Diagnostic> diagnostics;
    if (output.size() > 8u * 1024u * 1024u - 2)
        return Result<std::string>::Failure("Script.Framework", "SCRIPT_FRAMEWORK_INJECTION_BUDGET", "主文超过8MiB捕获预算。");
    for (const auto* file : selected) {
        const std::string_view text(file->bytes.empty() ? "" : reinterpret_cast<const char*>(file->bytes.data()), file->bytes.size());
        valid = ValidateFrameworkSourceEncoding(text, file->binding.path, FrameworkEncodingFor(inputs, file->binding.path), FrameworkDirectivePolicy(inputs));
        if (!valid.Succeeded()) return Failed<std::string>(valid);
        if (output.size() > 8u * 1024u * 1024u - 2 || text.size() > 8u * 1024u * 1024u - output.size() - 2)
            return Result<std::string>::Failure("Script.Framework", "SCRIPT_FRAMEWORK_INJECTION_BUDGET", "注入后主文超过8MiB捕获预算。");
        auto selectedView = MakeFrameworkLexicalView(text, file->binding.path, FrameworkEncodingFor(inputs, file->binding.path));
        if (!selectedView.Succeeded()) return {{}, std::move(selectedView.diagnostics)};
        diagnostics.insert(diagnostics.end(), selectedView.diagnostics.begin(), selectedView.diagnostics.end());
        const auto base = static_cast<std::uint32_t>(output.size() + 2);
        if (sourceSha256) {
            const auto hash = Sha256(file->bytes);
            if (!hash.Succeeded()) return {{}, hash.diagnostics};
            hashes.push_back(*hash.value);
        }
        output += "\r\n"; output.append(text); paths.push_back(file->binding.path);
        view.text += "\r\n"; view.text += selectedView.value->text;
        for (auto span : selectedView.value->opaque) { span.offset += base; view.opaque.push_back(std::move(span)); }
    }
    injected.insert(injected.end(), paths.begin(), paths.end());
    if (sourceSha256) sourceSha256->insert(sourceSha256->end(), hashes.begin(), hashes.end());
    if (lexicalView) *lexicalView = std::move(view);
    auto result = Result<std::string>::Success(std::move(output));
    result.diagnostics = std::move(diagnostics); return result;
}

} // namespace War3Map::Script
