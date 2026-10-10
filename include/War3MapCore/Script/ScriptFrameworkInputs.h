#pragma once

#include "War3MapCore/Script/ScriptInjectionSources.h"
#include "War3MapCore/Script/ScriptPreprocessSources.h"
#include "War3MapCore/Trigger/UiCatalog.h"

namespace War3Map::Script {

// 只用于当前已实现的正常cfg/owned注入/include/根udg/受限模板子集。
// 原始库、目录正负候选和遮蔽选择都由已有捕获服务拥有；不运行安装内Lua。
struct ScriptFrameworkInputs {
    ScriptToolchainProfile profile;
    ScriptToolchainLayout layout;
    Trigger::UiCatalog catalog;
    ScriptUserSourcePlan entryPlan;
    ScriptInjectionSources injection;
    std::vector<ScriptToolchainFileBinding> uiBindings;
};

[[nodiscard]] Result<ScriptToolchainProfile> MakeLegacyFrameworkProfile(
    const ScriptToolchainLayout& layout, const Trigger::UiCatalog& catalog,
    const std::filesystem::path& uiRoot, const std::filesystem::path& mainDirectory);
[[nodiscard]] Result<ScriptFrameworkInputs> CaptureScriptFrameworkInputs(
    ScriptToolchainProfile profile, const ScriptToolchainLayout& layout,
    const Trigger::UiCatalog& catalog);
[[nodiscard]] Status VerifyScriptFrameworkInputs(
    const ScriptFrameworkInputs& inputs, const ScriptToolchainLayout& layout);
[[nodiscard]] ScriptUserSourceRequest FrameworkSourceRequest(const ScriptFrameworkInputs& inputs);
[[nodiscard]] ScriptFrameworkDirectivePolicy FrameworkDirectivePolicy(const ScriptFrameworkInputs& inputs);
[[nodiscard]] ScriptProfileEncoding FrameworkEncodingFor(const ScriptFrameworkInputs& inputs,
    const std::filesystem::path& source);
[[nodiscard]] ScriptProfileEncoding FrameworkEncodingFor(const ScriptUserSourcePlan& plan,
    const std::filesystem::path& source);
[[nodiscard]] Result<Jass::SourceLexicalView> MakeFrameworkLexicalView(std::string_view original,
    const std::filesystem::path& source, ScriptProfileEncoding encoding);
// 只重绑定上个已验证阶段中完全相同的不透明片段；新GBK输出、改写片段及歧义来源拒绝。
[[nodiscard]] Result<Jass::SourceLexicalView> RebindFrameworkLexicalView(std::string_view output,
    std::string_view ownedOriginal, const Jass::SourceLexicalView& ownedView);

// 只追加owned的所选J；完整标识符匹配，字符串、注释、rawcode不触发。
[[nodiscard]] Result<std::string> InjectScriptFramework(
    std::string_view script, const ScriptFrameworkInputs& inputs,
    std::vector<std::filesystem::path>& injected, Jass::SourceLexicalView* lexicalView = nullptr);

// 参与的单来源编码由档案声明；GBK非ASCII混合阶段当前明确拒绝，不猜测或转码。
[[nodiscard]] Status ValidateFrameworkSourceEncoding(std::string_view text,
    const std::filesystem::path& source, ScriptProfileEncoding encoding,
    ScriptFrameworkDirectivePolicy directives = ScriptFrameworkDirectivePolicy::None);
[[nodiscard]] Status ValidateFrameworkIncludes(const ScriptFrameworkInputs& inputs,
    const ScriptPreprocessSources& sources);

} // namespace War3Map::Script
