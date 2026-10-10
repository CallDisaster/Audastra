#include "ScanOracle.h"
#include "War3MapCore/Render/MdxPose.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
namespace War3Map::Render::Scan {
namespace {
using namespace War3RenderCore;
std::string FirstError(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& d : diagnostics) if (d.severity == DiagnosticSeverity::Error) return d.code;
    return "SCAN_REFERENCE_FAILURE_WITHOUT_CODE";
}
bool Bits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
template<std::size_t N> bool Bits(const std::array<float,N>& a, const std::array<float,N>& b) {
    for (std::size_t i = 0; i < N; ++i) if (!Bits(a[i],b[i])) return false;
    return true;
}
std::string Label(const Sample& s) {
    return "sequence=" + std::to_string(s.sequence) + " elapsed=" + std::to_string(s.elapsed)
        + " global=" + std::to_string(s.global) + " camera=" + (s.camera ? "1" : "0");
}
using Warning = std::pair<std::string,std::string>;
struct Oracle {
    std::vector<MdxPoseNode> nodes;
    std::vector<GroupMatrix> groups;
    std::vector<float> alphas;
    std::vector<Vec3> colors;
    std::set<Warning> warnings;
    std::string error;
    std::uint32_t frame = 0;
};
Oracle Reference(const MdxModel& model, const CompiledModel& asset, const Sample& sample,
    const std::optional<MdxPoseCamera>& camera, const Checkpoint& checkpoint) {
    Oracle out; out.nodes.reserve(asset.NodeCount());
    std::map<std::uint32_t,std::uint32_t> indices;
    for (std::uint32_t i = 0; i < asset.NodeCount(); ++i) {
        checkpoint();
        const auto old = SampleMdxNodePose(model,{sample.sequence,sample.elapsed,sample.global,asset.Node(i).objectId,camera});
        if (!old.Succeeded()) { out.error = FirstError(old.diagnostics); return out; }
        out.frame = old.value->sequenceFrame;
        for (const auto& d : old.diagnostics) if (d.severity == DiagnosticSeverity::Warning) out.warnings.emplace(d.code,d.message);
        indices.emplace(old.value->chain.back().objectId,i); out.nodes.push_back(old.value->chain.back());
    }
    // 参考入口不直接公开组矩阵：仅使用其产出的节点 worldMatrix，按冻结参考 MTGC/MATS
    // 的原始顺序 double 累加并转 float；不调用新核心的私有数学/编译组成员实现。
    for (const auto& geoset : model.geosets) {
        std::size_t offset = 0;
        for (const auto count : geoset.matrixGroupSizes) {
            checkpoint(); std::array<double,16> sum{};
            for (std::uint32_t j = 0; j < count; ++j) {
                const auto& m = out.nodes.at(indices.at(geoset.matrixIndices.at(offset+j))).worldMatrix;
                if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) { out.error = "MDX_SKIN_MATRIX_UNSUPPORTED"; return out; }
                for (std::size_t col = 0; col < 4; ++col) for (std::size_t row = 0; row < 3; ++row) sum[col*4+row] += m[col*4+row];
            }
            GroupMatrix averaged;
            for (std::size_t col = 0; col < 4; ++col) for (std::size_t row = 0; row < 3; ++row) {
                const float value = static_cast<float>(sum[col*4+row]/count);
                if (!std::isfinite(value)) { out.error = "MDX_POSE_NUMBER_INVALID"; return out; }
                averaged.values[col*3+row] = value;
            }
            out.groups.push_back(averaged); offset += count;
        }
    }
    for (std::uint32_t g = 0; g < asset.GeosetCount(); ++g) {
        checkpoint();
        const auto alpha = SampleMdxGeosetAlpha(model,{g,sample.sequence,sample.elapsed,sample.global});
        if (!alpha.Succeeded()) { out.error = FirstError(alpha.diagnostics); return out; }
        out.alphas.push_back(*alpha.value);
        const auto color = SampleMdxGeosetColor(model,{g,sample.sequence,sample.elapsed,sample.global});
        if (!color.Succeeded()) { out.error = FirstError(color.diagnostics); return out; }
        out.colors.push_back(*color.value);
    }
    return out;
}
}
void CompareSample(const MdxModel& model, const War3RenderCore::CompiledModel& asset,
    War3RenderCore::PoseBatchEvaluator& evaluator, const Sample& sample, Record& record, const Checkpoint& checkpoint) {
    using namespace War3RenderCore;
    PoseRequest request; request.instance = {1,1}; request.asset = asset.Identity(); request.sequenceIndex = sample.sequence;
    request.sequenceElapsedMs = sample.elapsed; request.globalElapsedMs = sample.global;
    request.hasCamera = sample.camera ? 1u : 0u;
    request.camera = {{1234,2345,3456},{0,0,1}}; // 明示的模型局部测试相机，不冒充原版游戏镜头。
    const auto camera = sample.camera ? std::optional<MdxPoseCamera>{{request.camera.eye,request.camera.up}} : std::nullopt;
    checkpoint();
    const auto outcome = evaluator.Evaluate(std::span<const PoseRequest>(&request,1));
    if (outcome.error != Code::Ok || evaluator.Receipts().size() != 1) throw Interrupted{"SCAN_EVALUATOR_CAPACITY"};
    const auto& receipt = evaluator.Receipts()[0];
    const auto old = Reference(model,asset,sample,camera,checkpoint);
    const auto first = [&](std::string detail) { if (record.firstSample.empty()) record.firstSample = Label(sample) + " " + detail; };
    const auto before = record.nodeMismatch + record.groupMismatch + record.alphaMismatch + record.colorMismatch + record.diagnosticMismatch;
    const std::string error = receipt.error == Code::Ok ? "" : CodeName(receipt.error);
    if (!error.empty()) ++record.rejectionCodes[error];
    if (error != old.error) {
        ++record.diagnosticMismatch; first("error reference=" + old.error + " core=" + error);
    } else if (!error.empty()) { ++record.matchedRejections; }
    else {
        ++record.numericSamples; // 仅新旧均成功才进入数值分母；同码拒绝不能算数值通过。
        const auto colors = evaluator.Colors(0);
        const auto nodes = evaluator.Nodes(0); const auto groups = evaluator.Groups(0); const auto alphas = evaluator.Alphas(0);
        if (receipt.sequenceFrame != old.frame) { ++record.diagnosticMismatch; first("sequence_frame"); }
        if (nodes.size() != old.nodes.size() || groups.size() != old.groups.size() || alphas.size() != old.alphas.size() || colors.size() != old.colors.size()) {
            ++record.diagnosticMismatch; first("output_count");
        } else {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const auto& a = old.nodes[i]; const auto& b = nodes[i]; ++record.nodes;
                if (a.objectId != b.objectId || a.parentId != b.parentId || !Bits(a.pivot,b.pivot) || !Bits(a.translation,b.translation)
                    || !Bits(a.rotation,b.rotation) || !Bits(a.scaling,b.scaling) || !Bits(a.localMatrix,b.localMatrix) || !Bits(a.worldMatrix,b.worldMatrix)) {
                    ++record.nodeMismatch; first("node=" + std::to_string(a.objectId));
                }
            }
            for (std::size_t i = 0; i < groups.size(); ++i) {
                ++record.groups; if (!Bits(old.groups[i].values,groups[i].values)) { ++record.groupMismatch; first("group="+std::to_string(i)); }
            }
            for (std::size_t i = 0; i < alphas.size(); ++i) {
                ++record.alphas; if (!Bits(old.alphas[i],alphas[i])) { ++record.alphaMismatch; first("alpha="+std::to_string(i)); }
            }
        }
        for (std::size_t i = 0; i < std::min(colors.size(),old.colors.size()); ++i) {
            ++record.colors;
            if (!Bits(old.colors[i],colors[i])) {
                ++record.colorMismatch; first("rgb_geoset="+std::to_string(i));
            }
        }
        std::set<Warning> warnings;
        for (const auto& d : evaluator.Diagnostics(0)) warnings.emplace(CodeName(d.code),DescribeDiagnostic(d));
        if (warnings != old.warnings) { ++record.diagnosticMismatch; first("warning_set"); }
    }
    if (before != record.nodeMismatch + record.groupMismatch + record.alphaMismatch + record.colorMismatch + record.diagnosticMismatch) ++record.mismatchSamples;
    ++record.samples;
}
}
