#include "War3MapCore/Script/RemovalPreprocess.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Jass/ByteLexical.h"
#include "War3MapCore/Jass/IdentifierRewrite.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

namespace War3Map::Script {
namespace {
std::string PathText(const std::filesystem::path& path) { const auto s = path.generic_u8string(); return {s.begin(), s.end()}; }
std::vector<std::size_t> Lines(std::string_view text) {
    std::vector<std::size_t> result{0};
    for (std::size_t i = 0; i < text.size(); ++i) if (text[i] == '\r' || text[i] == '\n') {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        result.push_back(i + 1);
    }
    return result;
}
Result<std::string> AuditText(const Jass::PreprocessResult& result, std::size_t& failureLine) {
    std::string text = result.text;
    // 与保存/检查词法共享字节边界；不透明内容保持原字节，不猜代码页。
    std::size_t line = 1;
    for (std::size_t at = 0; at < text.size();) {
        if (const auto opaque = Jass::ScanByteOpaque(text, at)) {
            if (!opaque->closed) { failureLine = line; return Result<std::string>::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_LEXICAL", "不透明文本未闭合。"); }
            while (at < opaque->end) { if (text[at] == '\r' || (text[at] == '\n' && (!at || text[at - 1] != '\r'))) ++line; ++at; }
            continue;
        }
        const auto c = static_cast<unsigned char>(text[at]);
        if (c >= 128) { failureLine = line; return Result<std::string>::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_ENCODING", "执行代码含未映射的非ASCII标识符，不能声明完整引用覆盖。"); }
        if (c == '\r' || (c == '\n' && (!at || text[at - 1] != '\r'))) ++line;
        ++at;
    }
    return Result<std::string>::Success(std::move(text));
}
}
static Result<Trigger::RemovalPreprocessCoverage> CaptureCoverage(
    const MapScriptInputs& inputs, const ScriptFrameworkInputs* framework, const ScriptUserSourcePlan& entry,
    const ScriptUserSourceRequest& current, const ScriptInjectionSources& injection, const Trigger::GraphRemove& removedTarget,
    std::string_view versionString, bool scriptInjection, const RemovalTemplateInputs* templates) {
    using Out = Result<Trigger::RemovalPreprocessCoverage>;
    using Kind = Trigger::AtomicRenameCodeKind;
    if (!inputs.triggers || !inputs.catalog || !current.layout || inputs.catalog->Sha256() != entry.catalogSha256)
        return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_CONTEXT", "预处理覆盖需要当前候选与同一工具链目录。");
    const auto verified = framework ? VerifyScriptFrameworkInputs(*framework, *current.layout)
        : VerifyScriptInjectionSources(injection, entry, current, !current.layout->allowReturnBug);
    if (!verified.Succeeded()) return {{}, verified.diagnostics};
    const auto profile = framework ? ValidateScriptToolchainProfile(framework->profile)
        : Sha256Text("remove-modern-profile-v1:" + injection.entryPlanIdentitySha256);
    if (!profile.Succeeded()) return {{}, profile.diagnostics};
    Trigger::RemovalPreprocessCoverage result; result.profileSha256 = *profile.value;
    // 私有删除投影仍由生产生成器组成整图。去掉目标自身的声明/初始化，再扫描全部生成正文，
    // 避免把自动生成的自引用误当存活引用，也覆盖宏经GUI参数生成的新引用；不发布这个投影。
    auto projected = *inputs.triggers;
    if (removedTarget.target.kind == Trigger::GraphRecordKind::Trigger) {
        const auto found = std::find_if(projected.triggers.begin(), projected.triggers.end(), [&](const auto& t) { return t.id == removedTarget.target.id && t.name.text == removedTarget.expectedName; });
        if (found == projected.triggers.end() || found->customTextFlag)
            return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_CONTEXT", "删除投影需要同一普通触发器身份。");
        const auto removed = Trigger::TriggerDocumentEditor::RemoveTrigger(projected, found->id);
        if (!removed.Succeeded()) return {{}, removed.diagnostics};
    } else if (removedTarget.target.kind == Trigger::GraphRecordKind::Variable) {
        const auto found = std::find_if(projected.variables.begin(), projected.variables.end(), [&](const auto& v) { return v.id == removedTarget.target.id && v.name.text == removedTarget.expectedName; });
        if (found == projected.variables.end()) return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_CONTEXT", "删除投影的变量身份已改变。");
        const auto removed = Trigger::TriggerDocumentEditor::RemoveVariable(projected, found->name.text);
        if (!removed.Succeeded()) return {{}, removed.diagnostics};
    } else return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_CONTEXT", "不支持此类删除投影。");
    auto projectedInputs = inputs; projectedInputs.triggers = &projected;
    const auto generated = GenerateMapScript(projectedInputs);
    if (!generated.Succeeded()) return {{}, generated.diagnostics};
    std::vector<std::filesystem::path> injected;
    std::vector<std::string> injectedHashes;
    Jass::SourceLexicalView lexical;
    auto mainText = Result<std::string>::Success(generated.text);
    if (scriptInjection) {
        if (framework) mainText = InjectScriptFramework(generated.text, *framework, injected, &lexical, &injectedHashes);
        else {
            mainText = InjectOwnedCode(generated.text, injection, Jass::PreprocessCheckOptions(), &injected);
            for (const auto& path : injected) {
                const auto file = std::find_if(injection.files.begin(), injection.files.end(), [&](const auto& f) { return f.binding.path == path; });
                if (file == injection.files.end()) return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_MAP", "现代注入来源丢失。");
                injectedHashes.push_back(file->binding.sha256);
            }
        }
    }
    if (!mainText.Succeeded()) return {{}, mainText.diagnostics};
    ScriptPreprocessSourceRequest request;
    request.mainText = *mainText.value; request.mainName = "war3map.j";
    request.mainDirectory = framework ? framework->profile.mainSourceDirectory : entry.mapDirectory;
    request.captureMacroAudit = true; request.forbidAiIncludes = true;
    if (framework) request.frameworkDirectivePolicy = FrameworkDirectivePolicy(*framework);
    request.includeCandidatePolicy = framework ? ScriptIncludeCandidatePolicy::BoundRootsSearch : ScriptIncludeCandidatePolicy::Strict;
    request.defines = {{"WARCRAFT_VERSION", std::to_string(current.layout->versionMacro)}, {"YDWE_VERSION_STRING", "\"" + std::string(versionString) + "\""}};
    if (inputs.debug) request.defines.emplace_back("DEBUG", "1");
    if (scriptInjection) { request.defines.emplace_back("SCRIPT_INJECTION", "1"); if (framework) request.mainLexicalView = lexical; }
    // 档案中的前置特性阶段保持与保存一致；模板在预处理后求值。
    const auto captured = CaptureScriptPreprocessSources(entry, current, request);
    if (!captured.value) return {{}, captured.diagnostics};
    const auto& sources = *captured.value;
    std::string identity = "remove-preprocess-v3\njass-bytes-v1\nnames=utf8,gbk\n" + result.profileSha256 + "\n" + sources.mainSha256 + "\n";
    identity += entry.layoutIdentitySha256 + "\n" + entry.catalogSha256 + "\n";
    for (const auto& [name, value] : request.defines) identity += std::to_string(name.size()) + ":" + name + std::to_string(value.size()) + ":" + value + "\n";
    result.inputBytes = request.mainText.size();
    for (const auto& binding : entry.bindings) identity += PathText(binding.path) + "\n" + binding.sha256 + "\n";
    for (const auto& file : injection.files) {
        identity += PathText(file.binding.path) + "\n" + file.binding.sha256 + "\n";
        result.inputBytes += file.bytes.size();
    }
    for (std::size_t i = 0; i < injected.size(); ++i) identity += PathText(injected[i]) + "\n" + injectedHashes[i] + "\n";
    std::set<std::filesystem::path> included;
    for (const auto& trace : sources.includeCandidates) {
        identity += PathText(trace.candidate) + "\n";
        if (trace.sourceIndex < sources.includes.size()) {
            const auto& file = sources.includes[trace.sourceIndex];
            identity += file.binding.sha256 + "\n";
            if (file.binding.kind == ScriptToolchainBindingKind::RegularFile) included.insert(file.binding.path);
        } else identity += "not-read\n";
    }
    for (const auto& file : sources.includes) result.inputBytes += file.binding.bytes;
    result.includedFileCount = included.size();
    std::vector<std::string> libraries = templates ? templates->libraries : std::vector<std::string>{};
    if (framework) for (const auto& extension : sources.entryPlan.profileFeatureExtensions) {
        if (!extension.templateLibrary.empty() && std::find(sources.frameworkFeatureFlags.begin(), sources.frameworkFeatureFlags.end(), extension.feature) != sources.frameworkFeatureFlags.end()
            && std::find(libraries.begin(), libraries.end(), extension.templateLibrary) == libraries.end()) libraries.push_back(extension.templateLibrary);
    }
    identity += "template-host-v1\n" + (templates ? templates->identitySha256 : "builtin-context") + "\n";
    for (const auto& library : libraries) identity += "library:" + library + "\n";
    const auto hash = Sha256Text(identity); if (!hash.Succeeded()) return {{}, hash.diagnostics};
    result.identitySha256 = *hash.value;
    auto expanded = sources.preprocessed;
    result.complete = expanded.Succeeded();
    if (!framework) for (const auto& trace : sources.includeCandidates) {
        const bool main = trace.requester == std::filesystem::path(request.mainName)
            || trace.requester == request.mainDirectory / request.mainName;
        if (!main || trace.searchRole != Jass::IncludeSearchRole::CurrentFile) continue;
        auto name = trace.candidate.filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return std::towlower(c); });
        const bool stage = name == L"1_war3map.j" || name == L"2_inject.j";
        const bool local = trace.sourceIndex < sources.includes.size()
            && sources.includes[trace.sourceIndex].binding.kind == ScriptToolchainBindingKind::RegularFile;
        if (stage || local) {
            result.complete = false;
            result.incompleteReason = "现代保存使用独立编译目录；源图目录的影子包含或构建中间文件未证明等价：" + PathText(trace.candidate);
            break;
        }
    }
    if (!result.complete) {
        if (result.incompleteReason.empty()) result.incompleteReason = "预处理失败";
        for (const auto& d : expanded.diagnostics) if (d.severity == DiagnosticSeverity::Error) result.incompleteReason += "；" + d.code + ":" + d.message;
        return Out::Success(std::move(result));
    }
    TemplateContext context = templates ? templates->context : TemplateContext{};
    context.importRoot.reset();
    const auto evaluate = [&](std::string_view input) -> Result<TemplateResult> {
        auto evaluated = EvaluateTemplates(input, context);
        const bool whole = (evaluated.skippedBlocks || !evaluated.Succeeded()) && context.wholeChunk
            && std::none_of(evaluated.diagnostics.begin(), evaluated.diagnostics.end(), [](const auto& d) { return d.code == "TEMPLATE_IMPORT_FAILED"; });
        if (whole) {
            const auto output = context.wholeChunk(input, libraries);
            if (!output.Succeeded() || !output.value) return {{}, output.diagnostics};
            if (!output.value->imports.empty()) return Result<TemplateResult>::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_TEMPLATE_IMPORT", "只读模板宿主不得产生成员。");
            auto aligned = AlignTemplateOutput(input, evaluated, output.value->script);
            if (!aligned) return Result<TemplateResult>::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_TEMPLATE_MAP", "模板输出不能按TemplateSpan精确对回来源。");
            evaluated = std::move(*aligned);
            evaluated.diagnostics.insert(evaluated.diagnostics.end(), output.value->diagnostics.begin(), output.value->diagnostics.end());
        }
        if (!evaluated.Succeeded()) return {{}, evaluated.diagnostics};
        if (!evaluated.imports.empty()) return Result<TemplateResult>::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_TEMPLATE_IMPORT", "只读模板求值不得产生成员。");
        if (framework && std::any_of(evaluated.diagnostics.begin(), evaluated.diagnostics.end(), [](const auto& d) { return d.code == "ABILITY_ORDER_UNKNOWN"; }))
            return Result<TemplateResult>::Failure("Script.RemoveAudit", "SCRIPT_FRAMEWORK_ABILITY_ORDER_UNRESOLVED", "模板命令ID未绑定到实际物编。");
        return Result<TemplateResult>::Success(std::move(evaluated));
    };
    if (expanded.text.find("<?") != std::string::npos) {
        auto evaluated = evaluate(expanded.text);
        if (!evaluated.Succeeded() || !evaluated.value) {
            result.complete = false; result.incompleteReason = "模板求值失败";
            for (const auto& d : evaluated.diagnostics) result.incompleteReason += "；" + d.code + ":" + d.message;
            return Out::Success(std::move(result));
        }
        auto map = MapTemplateSourceLines(expanded.text, *evaluated.value);
        expanded.sourceMap = map.ComposeWith(expanded.sourceMap, static_cast<std::uint32_t>(Lines(evaluated.value->text).size()));
        expanded.text = std::move(evaluated.value->text);
    }
    if (framework) {
        const auto rewriteHash = Sha256Text(expanded.text); if (!rewriteHash.Succeeded()) return {{}, rewriteHash.diagnostics};
        Jass::IdentifierRewriteRequest rewrite;
        rewrite.text = expanded.text; rewrite.encoding = Text::SourceEncoding::Utf8;
        rewrite.sourceName = "remove-template.j"; rewrite.expectedInputSha256 = *rewriteHash.value;
        rewrite.globalScopeIdentity = "map:root";
        const auto mapped = Jass::RewriteUdgIdentifiers(rewrite);
        if (!mapped.Succeeded() || !mapped.value) {
            result.complete = false; result.incompleteReason = "旧版标识符映射未完成";
            for (const auto& d : mapped.diagnostics) result.incompleteReason += "；" + d.code + ":" + d.message;
            return Out::Success(std::move(result));
        }
        expanded.text = mapped.value->text;
        identity += "identifier-rule:" + mapped.value->ruleVersion + "\n" + mapped.value->outputSha256 + "\n";
    }
    const auto mainLines = Lines(request.mainText);
    struct Appended { std::size_t begin, end; std::string file; std::vector<std::size_t> lines; };
    std::vector<Appended> appended;
    std::size_t appendedAt = generated.text.size();
    for (const auto& path : injected) {
        const auto file = std::find_if(injection.files.begin(), injection.files.end(), [&](const auto& f) { return f.binding.path == path; });
        if (file == injection.files.end()) return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_MAP", "注入来源丢失。");
        if (framework) appendedAt += 2;
        const std::string_view text(reinterpret_cast<const char*>(file->bytes.data()), file->bytes.size());
        appended.push_back({appendedAt, appendedAt + file->bytes.size(), PathText(path), Lines(text)});
        appendedAt += file->bytes.size();
        if (!framework) appendedAt += 2;
    }
    std::map<StableId, std::size_t> index;
    const auto add = [&](StableId id, const Trigger::RawText& source) -> Status {
        const auto bytes = source.ToBytes(); if (!bytes.Succeeded()) return {bytes.diagnostics};
        const auto digest = Sha256(*bytes.value); if (!digest.Succeeded()) return {digest.diagnostics};
        index.emplace(id, result.blocks.size());
        Trigger::RemovalPreprocessBlock block; block.kind = id ? Kind::TriggerCustomCode : Kind::GlobalCustomCode;
        block.id = id; block.sourceSha256 = *digest.value; result.blocks.push_back(std::move(block)); return Status::Ok();
    };
    if (const auto s = add(0, inputs.triggers->wct.customCode); !s.Succeeded()) return {{}, s.diagnostics};
    for (const auto& span : generated.authorCode) if (span.triggerId && !index.contains(span.triggerId)) {
        const auto found = std::find_if(inputs.triggers->triggers.begin(), inputs.triggers->triggers.end(), [&](const auto& t) { return t.id == span.triggerId; });
        if (found == inputs.triggers->triggers.end()) return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_MAP", "生成来源缺少触发器。");
        if (const auto s = add(found->id, found->customText); !s.Succeeded()) return {{}, s.diagnostics};
    }
    const auto isMain = [&](std::string_view file) {
        return file == request.mainName || PathText(std::filesystem::u8path(file).lexically_normal())
            == PathText((request.mainDirectory / request.mainName).lexically_normal());
    };
    const auto owner = [&](std::string_view file, std::size_t line) -> std::optional<StableId> {
        if (!isMain(file)) return StableId{0};
        if (!line || line > mainLines.size()) return {};
        const auto offset = mainLines[line - 1];
        if (offset >= generated.text.size()) return StableId{0};
        const auto found = std::upper_bound(generated.authorCode.begin(), generated.authorCode.end(), offset,
            [](auto n, const auto& span) { return n < span.byteOffset; });
        if (found == generated.authorCode.begin()) return StableId{0};
        const auto& span = *std::prev(found);
        if (offset < span.byteOffset || offset >= span.byteOffset + span.byteLength) return StableId{0};
        return span.triggerId;
    };
    const auto origin = [&](std::string file, std::size_t line) -> Trigger::RemovalPreprocessOrigin {
        if (isMain(file) && line && line <= mainLines.size()) {
            const auto offset = mainLines[line - 1];
            for (const auto& part : appended) if (offset >= part.begin && offset < part.end) {
                const auto n = std::upper_bound(part.lines.begin(), part.lines.end(), offset - part.begin) - part.lines.begin();
                return {part.file, static_cast<std::size_t>(n)};
            }
            const auto found = std::upper_bound(generated.authorCode.begin(), generated.authorCode.end(), offset,
                [](auto n, const auto& span) { return n < span.byteOffset; });
            if (found != generated.authorCode.begin()) {
                const auto& span = *std::prev(found);
                if (offset >= span.byteOffset && offset < span.byteOffset + span.byteLength) {
                    const auto first = std::upper_bound(mainLines.begin(), mainLines.end(), span.byteOffset) - mainLines.begin();
                    return {span.triggerId ? "WCT/trigger-" + std::to_string(span.triggerId) + ".j" : "WCT/header.j", line - static_cast<std::size_t>(first) + 1};
                }
            }
            return {"Map/generated.j", line};
        }
        return {std::move(file), line};
    };
    std::size_t failureLine = 0;
    const auto normalized = AuditText(expanded, failureLine);
    if (!normalized.Succeeded()) {
        result.complete = false;
        result.incompleteReason = normalized.diagnostics.front().code + ":" + normalized.diagnostics.front().message;
        const auto position = expanded.sourceMap.Find(static_cast<std::uint32_t>(failureLine));
        if (position && position->file < expanded.sourceMap.Files().size()) {
            const auto located = origin(expanded.sourceMap.Files()[position->file], position->line);
            result.incompleteReason += "；来源=" + located.file + ":" + std::to_string(located.line);
        }
        return Out::Success(std::move(result));
    }
    const auto& expandedText = *normalized.value;
    const auto outputLines = Lines(expandedText);
    for (std::size_t i = 0; i < outputLines.size(); ++i) {
        const auto position = expanded.sourceMap.Find(static_cast<std::uint32_t>(i + 1));
        if (!position || position->file >= expanded.sourceMap.Files().size()) {
            const auto end = i + 1 < outputLines.size() ? outputLines[i + 1] : expandedText.size();
            if (expandedText.substr(outputLines[i], end - outputLines[i]).find_first_not_of(" \t\r\n") != std::string::npos) {
                result.complete = false; result.incompleteReason = "展开文本缺少来源映射";
            }
            continue;
        }
        const auto& file = expanded.sourceMap.Files()[position->file];
        const auto target = owner(file, position->line); if (!target) continue;
        auto& block = result.blocks[index.at(*target)];
        auto end = i + 1 < outputLines.size() ? outputLines[i + 1] : expandedText.size();
        while (end > outputLines[i] && (expandedText[end - 1] == '\r' || expandedText[end - 1] == '\n')) --end;
        block.text.append(expandedText, outputLines[i], end - outputLines[i]); block.text += '\n';
        block.origins.push_back(origin(file, position->line));
    }
    for (const auto& macro : expanded.macroAudit) {
        const auto target = owner(macro.source, macro.line).value_or(0);
        const auto located = origin(macro.source, macro.line);
        result.blocks[index.at(target)].macros.push_back({macro.name, macro.body, located.file, located.line, macro.expanded, macro.tokenConstruction});
    }
    // 参数/初值的原块仍独立绑定：生产正文已扫描；未发射的块也不因禁用而丢失模板证据。
    // 上下文相关而不能独立求值的原块保持不完整，不能借整图成功覆盖未知的禁用源码。
    const auto localTemplate = [&](Kind kind, StableId id, const Trigger::RawText& raw) -> Status {
        if (raw.text.find("<?") == std::string::npos) return Status::Ok();
        const auto bytes = raw.ToBytes(); if (!bytes.Succeeded()) return {bytes.diagnostics};
        const auto hash = Sha256(*bytes.value); if (!hash.Succeeded()) return {hash.diagnostics};
        auto evaluated = evaluate(raw.text);
        if (!evaluated.Succeeded() || !evaluated.value) {
            result.complete = false; result.incompleteReason = "原代码块模板求值不完整：" + std::to_string(id);
            for (const auto& d : evaluated.diagnostics) result.incompleteReason += "；" + d.code + ":" + d.message;
            return Status::Ok();
        }
        Trigger::RemovalPreprocessBlock block; block.kind = kind; block.id = id; block.sourceSha256 = *hash.value;
        const auto mapping = MapTemplateSourceLines(raw.text, *evaluated.value);
        block.text = std::move(evaluated.value->text);
        Jass::PreprocessResult local; local.text = block.text;
        std::size_t failedLine = 0;
        const auto validText = AuditText(local, failedLine);
        if (!validText.Succeeded()) {
            result.complete = false; result.incompleteReason = "模板原块执行字节不能覆盖：WTG/" + std::to_string(id) + ":" + std::to_string(failedLine);
            return Status::Ok();
        }
        const auto lines = Lines(block.text);
        for (std::size_t line = 1; line <= lines.size(); ++line) {
            const auto mapped = mapping.Find(static_cast<std::uint32_t>(line));
            if (!mapped) { result.complete = false; result.incompleteReason = "原块模板缺少来源映射"; return Status::Ok(); }
            block.origins.push_back({"WTG/" + std::string(kind == Kind::ArgumentCode ? "argument-" : "variable-") + std::to_string(id), mapped->line});
        }
        result.blocks.push_back(std::move(block));
        return Status::Ok();
    };
    for (const auto& argument : projected.argumentNodes) if (argument.type == 3)
        if (const auto s = localTemplate(Kind::ArgumentCode, argument.id, argument.value); !s.Succeeded()) return {{}, s.diagnostics};
    for (const auto& variable : projected.variables)
        if (const auto s = localTemplate(Kind::VariableInitialValue, variable.id, variable.value); !s.Succeeded()) return {{}, s.diagnostics};
    if (templates && templates->identityAfterEvaluation) {
        const auto dependency = templates->identityAfterEvaluation();
        if (!dependency.Succeeded() || !dependency.value) return {{}, dependency.diagnostics};
        identity += "template-dependencies:" + *dependency.value + "\n";
    }
    const auto finalHash = Sha256Text(identity); if (!finalHash.Succeeded()) return {{}, finalHash.diagnostics};
    result.identitySha256 = *finalHash.value;
    // 未激活的分支仍绑定身份；vJass指令仍在本轮范围之外。
    const auto unsupported = [](std::string_view text) {
        for (auto offset : Lines(text)) {
            while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t')) ++offset;
            if (text.substr(offset).starts_with("//!")) return true;
        }
        return false;
    };
    if (unsupported(request.mainText)) { result.complete = false; result.incompleteReason = "本轮未覆盖vJass指令"; }
    for (const auto& file : sources.includes) {
        const std::string_view text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
        if (unsupported(text)) { result.complete = false; result.incompleteReason = "包含文件含本轮未覆盖的vJass指令：" + PathText(file.binding.path); break; }
    }
    return Out::Success(std::move(result));
}
Result<Trigger::RemovalPreprocessCoverage> CaptureRemovalPreprocessCoverage(const MapScriptInputs& inputs,
    const ScriptFrameworkInputs& framework, const Trigger::GraphRemove& target, std::string_view version, bool inject, const RemovalTemplateInputs* templates) {
    return CaptureCoverage(inputs, &framework, framework.entryPlan, FrameworkSourceRequest(framework), framework.injection, target, version, inject, templates);
}
Result<Trigger::RemovalPreprocessCoverage> CaptureRemovalPreprocessCoverage(const MapScriptInputs& inputs,
    const ScriptUserSourcePlan& entry, const ScriptUserSourceRequest& current, const ScriptInjectionSources& injection,
    const Trigger::GraphRemove& target, std::string_view version, bool inject, const RemovalTemplateInputs* templates) {
    return CaptureCoverage(inputs, nullptr, entry, current, injection, target, version, inject, templates);
}
}
