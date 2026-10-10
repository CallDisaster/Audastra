#include "ScanOracle.h"
#include "ScanDetail.h"
#include "War3RenderCore/ModelContent.h"
#include "War3MapCore/Render/MdxPose.h"
#include <algorithm>
#include <array>
namespace War3Map::Render::Scan {
void Compare(const MdxModel& model, Record& record, const Limits& limits, const Checkpoint& checkpoint) {
    using namespace War3RenderCore;
    // 不调整 CompileMdxModel 的严格程度；资源限制单独标记，不能算作语义不兼容。
    const auto compiled = CompileMdxModel(model,{1,1},{record.path,record.source,record.digest});
    record.compile = compiled.Succeeded() ? "OK" : compiled.diagnostics.front().code;
    record.particleOnly = compiled.Succeeded() && ClassifyMdxContent(model) == ModelContentKind::ParticleOnlyUnimplemented ? 1u : 0u;
    for (const auto& diagnostic : compiled.diagnostics) AppendDetail(record, diagnostic.code + ":" + diagnostic.message);
    checkpoint();
    const auto samples = Samples(model,record,limits,checkpoint); record.planned = samples.size();
    const bool limited = record.state == "SCAN_SAMPLE_LIMIT";
    if (samples.empty()) {
        if (record.particleOnly != 0) {
            record.state = "particle_only_unimplemented";
            AppendDetail(record, "no_sequence=1; pose_samples=0");
        } else record.state = "SCAN_NO_SEQUENCE_SAMPLES";
        return;
    }
    if (!compiled.Succeeded()) {
        if (record.compile == CodeName(Code::AllocationFailed) || record.compile == CodeName(Code::CapacityExceeded)) {
            record.state = "SCAN_COMPILE_BUDGET"; return;
        }
        for (const auto& sample : samples) {
            for (std::size_t g = 0; g < model.geosets.size(); ++g) {
                checkpoint();
                const auto camera = sample.camera ? std::optional<MdxPoseCamera>{{{1234,2345,3456},{0,0,1}}} : std::nullopt;
                const auto old = SampleMdxRigidSkinnedGeoset(model,{g,sample.sequence,sample.elapsed,sample.global,camera});
                // 这个指标只说明旧入口有非空几何姿势，不声称可见度/材质/原版画面也已通过。
                if (old.Succeeded() && !old.value->positions.empty()) {
                    record.legacyUsable = 1; record.samples += 1;
                    record.firstSample = "legacy_geoset=" + std::to_string(g) + " sequence=" + std::to_string(sample.sequence)
                        + " elapsed=" + std::to_string(sample.elapsed) + " camera=" + (sample.camera ? "1" : "0");
                    record.state = "compile_rejected_legacy_positive"; return;
                }
            }
            ++record.samples;
        }
        record.state = limited ? "SCAN_SAMPLE_LIMIT" : "compile_rejected"; return;
    }
    const auto& asset = *compiled.value;
    if (asset.NodeCount() != MdxNodeCount(model) || asset.GeosetCount() != model.geosets.size()) {
        record.diagnosticMismatch = 1; record.mismatchSamples = 1; record.firstSample = "compiled_layout_count";
        record.state = "complete"; return;
    }
    BatchLimits batch; batch.maxInstances = 1;
    batch.maxNodes = std::max(1u,asset.NodeCount()); batch.maxGroups = std::max(1u,asset.GroupCount());
    batch.maxGeosets = std::max(1u,asset.GeosetCount());
    const std::array assets{asset}; auto created = PoseBatchEvaluator::Create(assets,1,batch);
    if (!created.Succeeded()) { record.state = "SCAN_EVALUATOR_BUDGET"; return; }
    auto evaluator = std::move(*created.value);
    for (const auto& sample : samples) { CompareSample(model,asset,evaluator,sample,record,checkpoint); checkpoint(); }
    record.state = limited ? "SCAN_SAMPLE_LIMIT" : record.particleOnly != 0 ? "particle_only_unimplemented" : "complete";
}
}
