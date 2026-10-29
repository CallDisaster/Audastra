#include "War3MapCore/Script/TriggerStorageCapabilities.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/ReadOnlyScriptSource.h"
#include <algorithm>
#include <cctype>

namespace War3Map::Script {
Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>> CaptureMapTriggerStorageCapabilities(
    const Trigger::UiCatalog& catalog, const std::filesystem::path& toolRoot, const std::filesystem::path& uiRoot,
    const std::filesystem::path& mapDirectory, std::span<const std::string> storageTypes) {
    using Output = Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>>;
    const auto layout = ResolveScriptToolchainLayout({toolRoot});
    if (!layout.Succeeded()) return {std::nullopt, layout.diagnostics};
    auto profile = MakeLegacyFrameworkProfile(*layout.value, catalog, uiRoot, mapDirectory);
    if (!profile.Succeeded()) return {std::nullopt, profile.diagnostics};
    auto inputs = CaptureScriptFrameworkInputs(std::move(*profile.value), *layout.value, catalog);
    if (!inputs.Succeeded()) return {std::nullopt, inputs.diagnostics};
    auto result = CaptureTriggerStorageCapabilities(*inputs.value, storageTypes);
    if (const auto verified = VerifyScriptToolchainLayout(*layout.value); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    return result;
}
Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>> CaptureTriggerStorageCapabilities(
    const ScriptFrameworkInputs& inputs, std::span<const std::string> storageTypes) {
    using Output = Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>>;
    const auto failure = [](std::string text) { return Output::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", std::move(text)); };
    if (storageTypes.empty() || storageTypes.size() > 32) return failure("存取能力探测需要1至32种实际槽类型。");
    if (const auto verified = VerifyScriptFrameworkInputs(inputs, inputs.layout); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    auto snapshot = std::shared_ptr<Trigger::TriggerStorageCapabilities>(new Trigger::TriggerStorageCapabilities);
    snapshot->catalogSha256_ = inputs.catalog.Sha256();
    std::string binding = inputs.entryPlan.layoutIdentitySha256 + inputs.catalog.Sha256();
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
        auto injected = InjectScriptFramework(probe, inputs, paths, nullptr, &hashes, required);
        if (!injected.Succeeded()) return {std::nullopt, injected.diagnostics};
        ScriptPreprocessSourceRequest request;
        request.mainText = std::move(*injected.value); request.mainName = "audastra-storage-probe.j";
        request.mainDirectory = inputs.entryPlan.mapDirectory;
        request.defines = {{"WARCRAFT_VERSION", std::to_string(inputs.layout.versionMacro)}, {"SCRIPT_INJECTION", "1"}, {"YDWE_VERSION_STRING", "\"1.32.13\""}};
        request.frameworkDirectivePolicy = FrameworkDirectivePolicy(inputs);
        auto captured = CaptureScriptPreprocessSources(inputs.entryPlan, FrameworkSourceRequest(inputs), request);
        if (!captured.Succeeded() || !captured.value || !captured.value->executionSupported) return failure("实际存取能力预处理未能完整捕获，不能猜测扩展支持。");
        auto replay = ReplayScriptPreprocessSources(*captured.value);
        if (!replay.Succeeded()) return failure("实际存取能力预处理回放失败。");
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
        if (const auto verified = VerifyScriptPreprocessSources(*captured.value, FrameworkSourceRequest(inputs), request); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    }
    if (const auto verified = VerifyScriptFrameworkInputs(inputs, inputs.layout); !verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    const auto hash = Sha256(std::as_bytes(std::span(binding)));
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    snapshot->binding_ = *hash.value;
    const auto frozen = std::make_shared<const ScriptFrameworkInputs>(inputs);
    snapshot->verifySources_ = [frozen, sources = std::move(includeBindings)] {
        if (const auto verified = VerifyScriptFrameworkInputs(*frozen, frozen->layout); !verified.Succeeded())
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
