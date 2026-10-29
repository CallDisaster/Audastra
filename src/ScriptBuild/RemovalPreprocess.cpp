#include "War3MapCore/Script/RemovalPreprocess.h"
#include "War3MapCore/Hash.h"
#include <algorithm>
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
}
Result<Trigger::RemovalPreprocessCoverage> CaptureRemovalPreprocessCoverage(
    const MapScriptInputs& inputs, const ScriptFrameworkInputs& framework, std::string_view versionString, bool scriptInjection) {
    using Out = Result<Trigger::RemovalPreprocessCoverage>;
    using Kind = Trigger::AtomicRenameCodeKind;
    if (!inputs.triggers || !inputs.catalog || inputs.catalog->Sha256() != framework.catalog.Sha256())
        return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_CONTEXT", "预处理覆盖需要当前候选与同一工具链目录。");
    if (const auto s = VerifyScriptFrameworkInputs(framework, framework.layout); !s.Succeeded()) return {{}, s.diagnostics};
    const auto profile = ValidateScriptToolchainProfile(framework.profile);
    if (!profile.Succeeded()) return {{}, profile.diagnostics};
    Trigger::RemovalPreprocessCoverage result; result.profileSha256 = *profile.value;
    const auto generated = GenerateMapScript(inputs);
    if (!generated.Succeeded()) return {{}, generated.diagnostics};
    std::vector<std::filesystem::path> injected;
    std::vector<std::string> injectedHashes;
    Jass::SourceLexicalView lexical;
    auto mainText = Result<std::string>::Success(generated.text);
    if (scriptInjection) mainText = InjectScriptFramework(generated.text, framework, injected, &lexical, &injectedHashes);
    if (!mainText.Succeeded()) return {{}, mainText.diagnostics};
    ScriptPreprocessSourceRequest request;
    request.mainText = *mainText.value; request.mainName = "war3map.j";
    request.mainDirectory = framework.profile.mainSourceDirectory;
    request.captureMacroAudit = true; request.forbidAiIncludes = true;
    request.frameworkDirectivePolicy = FrameworkDirectivePolicy(framework);
    request.includeCandidatePolicy = ScriptIncludeCandidatePolicy::BoundRootsSearch;
    request.defines = {{"WARCRAFT_VERSION", std::to_string(framework.layout.versionMacro)}, {"YDWE_VERSION_STRING", "\"" + std::string(versionString) + "\""}};
    if (inputs.debug) request.defines.emplace_back("DEBUG", "1");
    if (scriptInjection) { request.defines.emplace_back("SCRIPT_INJECTION", "1"); request.mainLexicalView = lexical; }
    // 档案中的前置特性阶段保持与保存一致；不执行模板或vJass指令。
    const auto captured = CaptureScriptPreprocessSources(framework.entryPlan, FrameworkSourceRequest(framework), request);
    if (!captured.value) return {{}, captured.diagnostics};
    const auto& sources = *captured.value;
    std::string identity = "remove-preprocess-v1\n" + result.profileSha256 + "\n" + sources.mainSha256 + "\n";
    for (const auto& [name, value] : request.defines) identity += std::to_string(name.size()) + ":" + name + std::to_string(value.size()) + ":" + value + "\n";
    result.inputBytes = request.mainText.size();
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
    const auto hash = Sha256Text(identity); if (!hash.Succeeded()) return {{}, hash.diagnostics};
    result.identitySha256 = *hash.value;
    const auto& expanded = sources.preprocessed;
    result.complete = expanded.Succeeded();
    if (!result.complete) {
        result.incompleteReason = "预处理失败";
        for (const auto& d : expanded.diagnostics) if (d.severity == DiagnosticSeverity::Error) result.incompleteReason += "；" + d.code + ":" + d.message;
        return Out::Success(std::move(result));
    }
    const auto mainLines = Lines(request.mainText), outputLines = Lines(expanded.text);
    struct Appended { std::size_t begin, end; std::string file; std::vector<std::size_t> lines; };
    std::vector<Appended> appended;
    std::size_t appendedAt = generated.text.size();
    for (const auto& path : injected) {
        const auto file = std::find_if(framework.injection.files.begin(), framework.injection.files.end(), [&](const auto& f) { return f.binding.path == path; });
        if (file == framework.injection.files.end()) return Out::Failure("Script.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_MAP", "注入来源丢失。");
        appendedAt += 2;
        const std::string_view text(reinterpret_cast<const char*>(file->bytes.data()), file->bytes.size());
        appended.push_back({appendedAt, appendedAt + file->bytes.size(), PathText(path), Lines(text)});
        appendedAt += file->bytes.size();
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
    const auto owner = [&](std::string_view file, std::size_t line) -> std::optional<StableId> {
        if (file != request.mainName && file != PathText(request.mainDirectory / request.mainName)) return StableId{0};
        if (!line || line > mainLines.size()) return {};
        const auto offset = mainLines[line - 1];
        if (offset >= generated.text.size()) return StableId{0};
        const auto found = std::upper_bound(generated.authorCode.begin(), generated.authorCode.end(), offset,
            [](auto n, const auto& span) { return n < span.byteOffset; });
        if (found == generated.authorCode.begin()) return {};
        const auto& span = *std::prev(found);
        if (offset < span.byteOffset || offset >= span.byteOffset + span.byteLength) return {};
        return span.triggerId;
    };
    const auto origin = [&](std::string file, std::size_t line) -> Trigger::RemovalPreprocessOrigin {
        if ((file == request.mainName || file == PathText(request.mainDirectory / request.mainName)) && line && line <= mainLines.size()) {
            const auto offset = mainLines[line - 1];
            for (const auto& part : appended) if (offset >= part.begin && offset < part.end) {
                const auto n = std::upper_bound(part.lines.begin(), part.lines.end(), offset - part.begin) - part.lines.begin();
                return {part.file, static_cast<std::size_t>(n)};
            }
        }
        return {std::move(file), line};
    };
    for (std::size_t i = 0; i < outputLines.size(); ++i) {
        const auto position = expanded.sourceMap.Find(static_cast<std::uint32_t>(i + 1));
        if (!position || position->file >= expanded.sourceMap.Files().size()) {
            if (i + 1 < outputLines.size() && outputLines[i + 1] > outputLines[i] + 1) {
                result.complete = false; result.incompleteReason = "展开文本缺少来源映射";
            }
            continue;
        }
        const auto& file = expanded.sourceMap.Files()[position->file];
        const auto target = owner(file, position->line); if (!target) continue;
        auto& block = result.blocks[index.at(*target)];
        auto end = i + 1 < outputLines.size() ? outputLines[i + 1] : expanded.text.size();
        while (end > outputLines[i] && (expanded.text[end - 1] == '\r' || expanded.text[end - 1] == '\n')) --end;
        block.text.append(expanded.text, outputLines[i], end - outputLines[i]); block.text += '\n';
        block.origins.push_back(origin(file, position->line));
    }
    for (const auto& macro : expanded.macroAudit) {
        const auto target = owner(macro.source, macro.line); if (!target) continue;
        const auto located = origin(macro.source, macro.line);
        result.blocks[index.at(*target)].macros.push_back({macro.name, macro.body, located.file, located.line, macro.expanded, macro.tokenConstruction});
    }
    // 读取过但未激活的分支仍绑定身份；不把未实现模板或vJass预处理当成已覆盖。
    const auto unsupported = [](std::string_view text) {
        if (text.find("<?") != std::string_view::npos) return true;
        for (auto offset : Lines(text)) {
            while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t')) ++offset;
            if (text.substr(offset).starts_with("//!")) return true;
        }
        return false;
    };
    if (unsupported(request.mainText)) { result.complete = false; result.incompleteReason = "本轮未覆盖vJass指令或Lua模板"; }
    for (const auto& file : sources.includes) {
        const std::string_view text(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
        if (unsupported(text)) { result.complete = false; result.incompleteReason = "包含文件含本轮未覆盖的vJass指令或Lua模板：" + PathText(file.binding.path); break; }
    }
    return Out::Success(std::move(result));
}
}
