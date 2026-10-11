#pragma once
#include "ScanTypes.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include <map>
#include <string_view>

namespace War3Map::Render::Scan {
struct PreviewIssue {
    std::string category, severity, message;
    std::uint64_t occurrences = 0;
    std::uint32_t firstSequence = UINT32_MAX;
    std::uint64_t firstElapsedMs = 0;
};
struct PreviewPolicyReport {
    std::string policy, state = "pending";
    std::uint64_t planned = 0, sampled = 0, visible = 0, animated = 0, moving = 0;
    std::map<std::string, PreviewIssue, std::less<>> issues;
};
struct PreviewModelReport {
    std::string path, source, sha256;
    std::vector<PreviewPolicyReport> policies;
};
// 分类只描述已知诊断含义；unknown 必须留待人工取证，不把“未实现”当成资产损坏。
[[nodiscard]] std::string_view PreviewDiagnosticCategory(std::string_view code) noexcept;
[[nodiscard]] PreviewModelReport EvaluatePreviewModel(std::string path, const AnimationResource& model,
    const AnimationResourceReader& reader, const Limits& limits, const Checkpoint& checkpoint);
// Windows 宿主模式；严格 Core 差分扫描的 Options/Record/入口保持原合同。
int RunPreview(const Options& options, const std::filesystem::path& mapPath);
}
