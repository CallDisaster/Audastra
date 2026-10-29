#include "War3MapCore/Script/TriggerStorageCapabilities.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/ReadOnlyScriptSource.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/ScriptUserSourceRequest.h"
#include <algorithm>
#include <cctype>

namespace War3Map::Script {
namespace {
ScriptUserSourceRequest CurrentSources(const ScriptFrameworkInputs& inputs) {
    if (inputs.layout.kind != ScriptToolchainLayoutKind::Modern) return FrameworkSourceRequest(inputs);
    return MakeModernUserSourceRequest(inputs.layout, inputs.catalog, inputs.entryPlan.uiRoot, inputs.entryPlan.mapDirectory);
}
Status VerifyInputs(const ScriptFrameworkInputs& inputs) {
    if (inputs.layout.kind != ScriptToolchainLayoutKind::Modern) return VerifyScriptFrameworkInputs(inputs, inputs.layout);
    if (const auto layout = VerifyScriptToolchainLayout(inputs.layout); !layout.Succeeded()) return layout;
    const auto request = CurrentSources(inputs);
    if (const auto entry = VerifyScriptUserSourcePlan(inputs.entryPlan, request); !entry.Succeeded()) return entry;
    return VerifyScriptInjectionSources(inputs.injection, inputs.entryPlan, request, !inputs.layout.allowReturnBug);
}
}
Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>> CaptureMapTriggerStorageCapabilities(
    const Trigger::UiCatalog& catalog, const std::filesystem::path& toolRoot, const std::filesystem::path& uiRoot,
    const std::filesystem::path& mapDirectory, std::span<const std::string> storageTypes) {
    using Output = Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>>;
    const auto incomplete = [](const std::vector<Diagnostic>& diagnostics) {
        return Output::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", "无法冻结实际存取能力上下文："
            + (diagnostics.empty() ? std::string("来源不完整。") : diagnostics.front().code + "：" + diagnostics.front().message));
    };
    const auto layout = ResolveScriptToolchainLayout({toolRoot});
    if (!layout.Succeeded()) return incomplete(layout.diagnostics);
    ScriptFrameworkInputs capturedInputs;
    if (layout.value->kind == ScriptToolchainLayoutKind::Modern) {
        const auto request = MakeModernUserSourceRequest(*layout.value, catalog, uiRoot, mapDirectory);
        auto entry = CaptureScriptUserSourcePlan(request);
        if (!entry.Succeeded()) return incomplete(entry.diagnostics);
        auto injection = CaptureScriptInjectionSources(*entry.value, request, !layout.value->allowReturnBug);
        if (!injection.Succeeded()) return incomplete(injection.diagnostics);
        capturedInputs.layout = *layout.value; capturedInputs.catalog = catalog;
        capturedInputs.entryPlan = std::move(*entry.value); capturedInputs.injection = std::move(*injection.value);
    } else {
        auto profile = MakeLegacyFrameworkProfile(*layout.value, catalog, uiRoot, mapDirectory);
        if (!profile.Succeeded()) return incomplete(profile.diagnostics);
        auto inputs = CaptureScriptFrameworkInputs(std::move(*profile.value), *layout.value, catalog);
        if (!inputs.Succeeded()) return incomplete(inputs.diagnostics);
        capturedInputs = std::move(*inputs.value);
    }
    auto result = CaptureTriggerStorageCapabilities(capturedInputs, storageTypes);
    if (!result.Succeeded()) return incomplete(result.diagnostics);
    if (const auto verified = VerifyScriptToolchainLayout(*layout.value); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    return result;
}
Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>> CaptureTriggerStorageCapabilities(
    const ScriptFrameworkInputs& inputs, std::span<const std::string> storageTypes) {
    using Output = Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>>;
    const auto failure = [](std::string text) { return Output::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", std::move(text)); };
    if (storageTypes.empty() || storageTypes.size() > 32) return failure("存取能力探测需要1至32种实际槽类型。");
    if (const auto verified = VerifyInputs(inputs); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    auto snapshot = std::shared_ptr<Trigger::TriggerStorageCapabilities>(new Trigger::TriggerStorageCapabilities);
    snapshot->catalogSha256_ = inputs.catalog.Sha256();
    std::string binding = inputs.entryPlan.layoutIdentitySha256 + inputs.catalog.Sha256();
    binding += inputs.injection.entryPlanIdentitySha256;
    for (const auto& source : inputs.injection.files) binding += source.binding.sha256;
    std::map<std::filesystem::path, ScriptToolchainFileBinding> includeBindings;
    for (const auto& type : storageTypes) {
        if (type.empty() || type.size() > 128 || !std::all_of(type.begin(), type.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
            return failure("存取能力探测类型不是有界的标识符。");
        const std::string marker = "AudastraStorageProbe_" + type;
        const std::string macro = "YDTRIGGER_" + type;
        std::string probe = "#include <YDTrigger/YDTrigger.h>\n" + marker + " " + macro + "(GET)\n";
        std::vector<std::filesystem::path> paths;
        std::vector<std::string> hashes;
        const std::string_view required[] = {Trigger::kLocalGet};
        auto injected = inputs.layout.kind == ScriptToolchainLayoutKind::Modern
            ? InjectOwnedCode(probe + "\n" + std::string(Trigger::kLocalGet) + "\n", inputs.injection, {}, &paths)
            : InjectScriptFramework(probe, inputs, paths, nullptr, &hashes, required);
        if (!injected.Succeeded()) return {std::nullopt, injected.diagnostics};
        ScriptPreprocessSourceRequest request;
        request.mainText = std::move(*injected.value); request.mainName = "audastra-storage-probe.j";
        request.mainDirectory = inputs.entryPlan.mapDirectory;
        request.defines = {{"WARCRAFT_VERSION", std::to_string(inputs.layout.versionMacro)}, {"SCRIPT_INJECTION", "1"}, {"YDWE_VERSION_STRING", "\"1.32.13\""}};
        request.frameworkDirectivePolicy = inputs.layout.kind == ScriptToolchainLayoutKind::Modern
            ? ScriptFrameworkDirectivePolicy::None : FrameworkDirectivePolicy(inputs);
        auto captured = CaptureScriptPreprocessSources(inputs.entryPlan, CurrentSources(inputs), request);
        if (!captured.Succeeded() || !captured.value) return failure("实际存取能力预处理未能完整捕获，不能猜测扩展支持："
            + (captured.diagnostics.empty() ? std::string("没有完整来源。") : captured.diagnostics.front().code + "：" + captured.diagnostics.front().message));
        auto replay = ReplayScriptPreprocessSources(*captured.value);
        // 此API的executionSupported不授予整链执行；这里只使用同源的纯宏捕获和回放。
        if (!replay.Succeeded() || replay.text != captured.value->preprocessed.text) return failure("实际存取能力预处理回放失败或不同源。");
        const auto at = replay.text.find(marker);
        if (at == replay.text.npos || replay.text.find(marker, at + marker.size()) != replay.text.npos) return failure("存取能力探测标记丢失或歧义。");
        const auto end = replay.text.find_first_of("\r\n", at);
        const auto expression = replay.text.substr(at + marker.size(), end == replay.text.npos ? end : end - at - marker.size());
        snapshot->supported_[type] = expression.find(macro) == expression.npos && expression.find("YDTRIGGER_") == expression.npos
            && std::any_of(expression.begin(), expression.end(), [](unsigned char c) { return std::isalpha(c); });
        binding += captured.value->mainSha256 + captured.value->includeCandidatePolicySha256;
        for (const auto& source : captured.value->includes) {
            binding += source.binding.sha256;
            includeBindings.emplace(source.binding.path, source.binding);
        }
        for (const auto& hash : hashes) binding += hash;
        if (const auto verified = VerifyScriptPreprocessSources(*captured.value, CurrentSources(inputs), request); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    }
    if (const auto verified = VerifyInputs(inputs); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    const auto hash = Sha256(std::as_bytes(std::span(binding)));
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    snapshot->binding_ = *hash.value;
    const auto frozen = std::make_shared<const ScriptFrameworkInputs>(inputs);
    snapshot->verifySources_ = [frozen, sources = std::move(includeBindings)] {
        if (const auto verified = VerifyInputs(*frozen); !verified.Succeeded())
            return Status::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", "存取能力快照的框架来源已变化，请重新检查。");
        for (const auto& [path, binding] : sources) {
            const auto current = CaptureReadOnlyScriptSource(path, false, binding.maxBytes ? binding.maxBytes : 4 * 1024 * 1024);
            if (!current.Succeeded() || current.value->binding.kind != binding.kind || current.value->binding.sha256 != binding.sha256)
                return Status::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", "存取能力快照的包含候选已变化，请重新检查。");
        }
        return Status::Ok();
    };
    return Output::Success(std::move(snapshot));
}
}
