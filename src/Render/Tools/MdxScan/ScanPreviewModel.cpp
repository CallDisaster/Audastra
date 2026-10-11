#include "ScanPreview.h"
#include "War3MapCore/Render/AnimationPremFrame.h"
#include "War3RenderCore/PoseTypes.h"
#include <algorithm>
#include <array>
#include <set>

namespace War3Map::Render::Scan {
std::string_view PreviewDiagnosticCategory(std::string_view code) noexcept {
    if (code.find("BUDGET") != std::string_view::npos || code.find("CAPACITY") != std::string_view::npos ||
        code.find("LIMIT") != std::string_view::npos || code.find("TIMEOUT") != std::string_view::npos ||
        code.find("ALLOCATION") != std::string_view::npos || code.find("MEMORY") != std::string_view::npos)
        return "budget_uncovered";
    if (code == "SCAN_EXPLICIT_CAMERA_NO_GEOMETRY" || code == "MDX_GEOA_DUPLICATE" || code.find("GAME_ORDER") != std::string_view::npos ||
        code.find("SOURCE_ORDER") != std::string_view::npos || code.find("IDENTICAL_KEYS") != std::string_view::npos)
        return "compatibility_notice";
    if (code == "file-not-found" || code == "WORLD_ASSET_MISSING" ||
        code.find("SOURCE") != std::string_view::npos || code.find("RESOURCE") != std::string_view::npos ||
        code.starts_with("MPQ_") || code.starts_with("SCAN_READ") || code.starts_with("GAME_"))
        return "resource_source";
    if (code.find("UNSUPPORTED") != std::string_view::npos || code.find("UNIMPLEMENTED") != std::string_view::npos ||
        code.find("UNINTERPRETED") != std::string_view::npos || code == "MATERIAL_UV_ANIMATION_STATIC")
        return "unsupported_or_unverified";
    if (code.find("INVALID") != std::string_view::npos || code.find("TRUNCATED") != std::string_view::npos ||
        code == "MDX_MAGIC" || code == "PARTICLE_MAGIC")
        return "rejected_data";
    return "needs_investigation";
}
namespace {
void Add(PreviewPolicyReport& report, std::string code, std::string message, std::string severity,
    std::uint32_t sequence = UINT32_MAX, std::uint64_t elapsed = 0) {
    auto [entry, inserted] = report.issues.try_emplace(code);
    auto& issue = entry->second;
    if (inserted) {
        issue.category = PreviewDiagnosticCategory(code); issue.severity = std::move(severity);
        issue.message = std::move(message); issue.firstSequence = sequence; issue.firstElapsedMs = elapsed;
    } else if (severity == "error") issue.severity = "error";
    ++issue.occurrences;
}
void Add(PreviewPolicyReport& report, const std::vector<Diagnostic>& diagnostics,
    std::uint32_t sequence = UINT32_MAX, std::uint64_t elapsed = 0) {
    for (const auto& diagnostic : diagnostics) Add(report, diagnostic.code, diagnostic.message,
        diagnostic.severity == DiagnosticSeverity::Error ? "error" : "notice", sequence, elapsed);
}
void AddPose(PreviewPolicyReport& report, const War3RenderCore::PoseDiagnostic& diagnostic,
    std::uint32_t sequence, std::uint64_t elapsed) {
    if (diagnostic.code == War3RenderCore::Code::Ok) return;
    Add(report, War3RenderCore::CodeName(diagnostic.code), War3RenderCore::DescribeDiagnostic(diagnostic),
        "unspecified", sequence, elapsed);
}
bool Limited(const PreviewPolicyReport& report) {
    return std::any_of(report.issues.begin(), report.issues.end(), [](const auto& issue) {
        return issue.second.category == "budget_uncovered";
    });
}
}
PreviewModelReport EvaluatePreviewModel(std::string path, const AnimationResource& model,
    const AnimationResourceReader& reader, const Limits& limits, const Checkpoint& checkpoint) {
    PreviewModelReport output{std::move(path), model.sourceLayer, model.digest, {}};
    const auto parsed = ParseMdx(model.bytes); checkpoint();
    for (const bool interactive : {false, true}) {
        auto& report = output.policies.emplace_back();
        report.policy = interactive ? "interactive" : "strict";
        Add(report, parsed.diagnostics);
        if (!parsed.Succeeded()) { report.state = Limited(report) ? "uncovered" : "parse_rejected"; continue; }
        std::uint64_t vertices = 0;
        for (const auto& geoset : parsed.value->geosets) vertices += geoset.positions.size();
        if (MdxNodeCount(*parsed.value) > limits.maxNodes || vertices > limits.maxVertices) {
            report.state = "uncovered"; Add(report, "SCAN_MODEL_COMPLEXITY_LIMIT", "模型超过声明扫描预算。", "error"); continue;
        }
        auto animationLimits = interactive ? InteractiveAnimationPreviewLimits() : AnimationLimits{};
        animationLimits.retainUnsupportedStatic = true;
        animationLimits.enableParticles = true;
        animationLimits.enablePremPreview = true;
        animationLimits.geometry.maxModels = 1;
        animationLimits.geometry.maxInstances = 1;
        animationLimits.geometry.maxVertices = limits.maxVertices;
        animationLimits.geometry.maxTextureDimension = 256;
        animationLimits.geometry.maxTotalTextureBytes = 32ull * 1024 * 1024;
        AnimationInstanceSpec instance; instance.identity = {1, 1}; instance.hostKey = "preview-scan";
        instance.model.modelPath = output.path; instance.elapsedMs = 0; instance.globalMs = 0;
        auto created = ModelAnimationService::Create({1, 0, model.digest}, std::span(&instance, 1), reader, animationLimits);
        checkpoint(); Add(report, created.diagnostics);
        if (!created.Succeeded()) {
            report.state = Limited(report) ? "uncovered" : "prepare_rejected";
            continue;
        }
        const auto service = *created.value;
        Add(report, service->Scene()->geometry.warnings);
        for (const auto& unavailable : service->Scene()->unavailableInstances)
            Add(report, unavailable.code, "模型资产未准入；原始诊断代码保留。", "error");
        if (service->Scene()->geometry.untexturedLayers)
            Add(report, "SCAN_RESOURCE_UNTEXTURED", "有材质未取得贴图，数值姿势不证明完整画面。", "notice");
        for (const auto& capability : service->Scene()->particleCapabilities) {
            if (!capability.rejectionCode.empty())
                Add(report, capability.rejectionCode, "粒子资产能力诊断。", "notice");
            if (capability.skippedEmitters) for (const auto& issue : *capability.skippedEmitters)
                Add(report, issue.code, issue.message, "notice");
        }
        // 纯粒子/节点模型没有网格取景框。扫描器使用声明的观察相机继续数值采样，
        // 不能让 Fit 的异常丢掉已取得的模型身份与此前诊断，也不声称此视点覆盖画面。
        const bool withoutGeometry = service->Scene()->geometry.instances.empty();
        const War3RenderCameraV2 camera = withoutGeometry
            ? War3RenderCameraV2{sizeof(War3RenderCameraV2), 0, 0, 0, 500, -1.1f, 0.5f}
            : FitAnimationSceneCamera(*service->Scene());
        if (withoutGeometry)
            Add(report, "SCAN_EXPLICIT_CAMERA_NO_GEOMETRY", "没有可取景网格；使用目标(0,0,0)、距离500、偏航-1.1、俯仰0.5的显式相机进行CPU采样，不证明可见画面。", "notice");
        const auto& sequences = parsed.value->sequences;
        report.planned = sequences.size() * 3;
        bool premCapabilitiesCaptured = false;
        for (std::uint32_t sequence = 0; sequence < sequences.size(); ++sequence) {
            const auto& definition = sequences[sequence];
            const auto duration = definition.end >= definition.start ? static_cast<std::uint64_t>(definition.end) - definition.start : 0;
            std::optional<std::uint64_t> firstMatrix;
            bool moved = false;
            // 循环动作末帧会回绕，仍保留端点样本；moving 只表示这些样本之间实际数值变化。
            for (const auto elapsed : std::array<std::uint64_t, 3>{0, duration / 2, duration}) {
                checkpoint();
                if (report.sampled >= limits.maxSamples) break;
                AnimationAdvance request;
                AnimationEdit edit; edit.identity = instance.identity; edit.sequenceIndex = sequence;
                edit.elapsedMs = elapsed; edit.globalMs = elapsed;
                request.edits.push_back(edit);
                request.worldCamera = AnimationWorldCamera(camera);
                const auto sampled = service->Prepare(nullptr, request); ++report.sampled;
                Add(report, sampled.diagnostics, sequence, elapsed);
                if (!sampled.Succeeded()) continue;
                const auto& frame = **sampled.value;
                report.visible += std::any_of(frame.poses.begin(), frame.poses.end(), [](const auto& pose) {
                    return !pose.hidden && !pose.noVisibleGeometry;
                }) || !frame.particleVertices.empty();
                report.animated += frame.animatedInstances != 0;
                if (frame.budgetStaticInstances || frame.budgetParticleInstances)
                    Add(report, "SCAN_FRAME_BUDGET", "帧预算保留静态或省略粒子，未覆盖完整动画。", "notice", sequence, elapsed);
                if (frame.omittedDiagnostics)
                    Add(report, "SCAN_DIAGNOSTIC_LIMIT", "帧详细诊断被有界列表截断。", "notice", sequence, elapsed);
                for (const auto& diagnostic : frame.diagnostics) AddPose(report, diagnostic, sequence, elapsed);
                for (const auto& pose : frame.poses) {
                    if (pose.hidden) AddPose(report, pose.rejection, sequence, elapsed);
                    if (!pose.rejectionCode.empty()) Add(report, pose.rejectionCode, pose.rejectionMessage, "error", sequence, elapsed);
                    if (pose.animated && !pose.hidden && !pose.staticForBudget) {
                        if (firstMatrix) moved |= *firstMatrix != pose.matrixHash;
                        else firstMatrix = pose.matrixHash;
                    }
                    for (std::uint32_t i = 0; i < pose.warnings.count; ++i)
                        AddPose(report, pose.warnings.records[i], sequence, elapsed);
                    if (pose.warnings.omitted)
                        Add(report, "SCAN_DIAGNOSTIC_LIMIT", "姿势详细诊断被有界列表截断。", "notice", sequence, elapsed);
                }
                for (const auto& issue : frame.geosetIssues) AddPose(report, issue.diagnostic, sequence, elapsed);
                for (const auto& issue : frame.materialIssues)
                    Add(report, War3RenderCore::CodeName(issue.source.error), War3RenderCore::CodeMessage(issue.source.error),
                        "unspecified", sequence, elapsed);
                for (const auto& particle : frame.particleLinks) if (!particle.rejectionCode.empty())
                    Add(report, particle.rejectionCode, "粒子预览拒绝或部分显示。", "notice", sequence, elapsed);
                if (frame.prem) {
                    if (!premCapabilitiesCaptured && frame.prem->capabilities) {
                        for (const auto& capability : *frame.prem->capabilities) if (!capability.rejectionCode.empty())
                            Add(report, capability.rejectionCode, capability.rejectionMessage, "notice", sequence, elapsed);
                        premCapabilitiesCaptured = true;
                    }
                    for (const auto& error : frame.prem->sampleErrors) {
                        Add(report, error.code, "PREM 出生节点采样失败。", "notice", sequence, elapsed);
                        AddPose(report, error.diagnostic, sequence, elapsed);
                    }
                    if (frame.prem->requestBudgetOmissions || frame.prem->slotSuppressedChildren)
                        Add(report, "SCAN_PREM_BUDGET", "PREM 请求或子模型槽预算有省略。", "notice", sequence, elapsed);
                }
            }
            report.moving += moved;
        }
        report.state = report.sampled == report.planned && !Limited(report) ? "complete" : "uncovered";
        if (report.sampled != report.planned) Add(report, "SCAN_SAMPLE_LIMIT", "声明的动作样本未全部完成。", "error");
        if (sequences.empty()) Add(report, "ANIMATION_SEQUENCE_UNAVAILABLE", "模型没有可播放动作。", "notice");
        checkpoint();
    }
    return output;
}
}
