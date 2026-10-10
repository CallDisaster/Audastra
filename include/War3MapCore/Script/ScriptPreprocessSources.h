#pragma once

#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Script/ScriptUserSourcePlan.h"

namespace War3Map::Script {

enum class ScriptPreprocessMainPolicy { None, LegacyGlobalBytesSubset };
// 独立于 C/Wave 宏空间：先 $include，再无值特性声明，再条件选择。
// 有值替换、外部宿主预定义和 C 包含中晚到的方言不属于此子集。
enum class ScriptFrameworkDirectivePolicy { None, FeatureFlagsV1 };
// Strict 保留历史范围外首候选立即拒绝；BoundRootsSearch 只改变候选资格查找。
// 新来源资格只来自已绑定档案的 ordinaryIncludeAccess，不改变任何搜索目录或顺序。
enum class ScriptIncludeCandidatePolicy { Strict, BoundRootsSearch };
struct MainTransformReport {
    struct Name {
        std::string original, replacement;
        std::uint32_t declarationLine = 0;
        std::uint64_t occurrences = 0;
        [[nodiscard]] bool operator==(const Name&) const = default;
    };
    ScriptPreprocessMainPolicy policy = ScriptPreprocessMainPolicy::None;
    std::string inputSha256, outputSha256;
    std::uint64_t inputBytes = 0, outputBytes = 0;
    bool wholeTextReplacement = false; // 字节替换也作用于字符串和注释；不是词法/作用域重命名。
    std::vector<Name> names;
    [[nodiscard]] bool operator==(const MainTransformReport&) const = default;
};

struct ScriptPreprocessSourceRequest {
    std::string mainText;
    std::string mainName = "war3map.j";
    std::filesystem::path mainDirectory;
    std::vector<std::pair<std::string, std::string>> defines;
    ScriptPreprocessMainPolicy mainPolicy = ScriptPreprocessMainPolicy::None;
    ScriptFrameworkDirectivePolicy frameworkDirectivePolicy = ScriptFrameworkDirectivePolicy::None;
    ScriptIncludeCandidatePolicy includeCandidatePolicy = ScriptIncludeCandidatePolicy::Strict;
    std::optional<Jass::SourceLexicalView> mainLexicalView;
    // 运行控制不参与来源身份；捕获和owned回放共用同一取消/绝对截止时间。
    Jass::JassCheckOptions check = [] { Jass::JassCheckOptions value; value.maxSources = 1024; return value; }();
};

struct ScriptIncludeSource {
    ScriptToolchainFileBinding binding;
    std::vector<std::byte> bytes;
    std::uint64_t lastWriteTicks = 0;
};

struct ScriptIncludeCandidateTrace {
    Jass::IncludePhase phase = Jass::IncludePhase::CPreprocessor;
    std::filesystem::path requester, searchDirectory, candidate;
    std::string requesterStageSha256, spelling;
    std::uint32_t requesterLine = 0, requesterOffset = 0, requesterLength = 0, searchOrdinal = 0;
    Jass::IncludeSearchRole searchRole = Jass::IncludeSearchRole::CurrentFile;
    Jass::IncludeCandidateEligibility eligibility = Jass::IncludeCandidateEligibility::Allowed;
    bool angled = false, forced = false;
    // Skipped 不持有正/负来源索引，不检查范围外文件是否存在。
    std::uint32_t sourceIndex = UINT32_MAX;
    [[nodiscard]] bool operator==(const ScriptIncludeCandidateTrace&) const = default;
};

// 仅绑定这次内置预处理实际访问的候选：高优先级缺失路径也保存，防止后来新增遮蔽。
// 未激活的条件包含、整棵 include 树、外部 Wave 行为及旧模板执行均不在覆盖内。
struct ScriptPreprocessSources {
    ScriptUserSourcePlan entryPlan;
    ScriptPreprocessSourceRequest request;
    std::vector<std::filesystem::path> systemIncludes;
    std::vector<std::string> forceIncludes;
    std::vector<ScriptIncludeSource> includes;
    std::vector<ScriptIncludeCandidateTrace> includeCandidates;
    std::string includeCandidatePolicySha256;
    std::string mainSha256;
    Jass::PreprocessResult preprocessed;
    std::uint64_t totalIncludeBytes = 0;
    bool executionSupported = false;
    std::string preparedMainText; // None或无变换名时为空，表示原request.mainText，不重复拥有主文。
    MainTransformReport mainTransform;
    std::string frameworkText;
    Jass::SourceMap frameworkSourceMap;
    std::vector<std::string> frameworkFeatureFlags;
};

// 原字节和缺失探测通过共用绑定服务捕获；每文件4MiB、合计32MiB、4096候选/512文件。
// 输入8MiB、输出16MiB，最多16384次候选访问/累计64MiB包含读取；单次词法缓存复用重复头。
// 包含范围省略亦占访问/4096唯一路径预算；完整 trace 元数据<=8MiB，阶段 offset 绑定实际阶段 SHA。
// 普通include声明及入口正/负类型绑定参与策略身份；Replay重新核对资格，不访问声明之外的磁盘。
// 只运行内置宏预处理，不执行 Lua/模板或改变保存门禁。
// 显式主文转换只支持声明子集；不覆盖include/宏生成名或完整旧宿主执行。
// 列0唯一globals块、简单内置类型/字面量；4096个udg、256个变换名，单名4096B/合计64KiB。
// 整段搜索累计512MiB、命中65536；比旧错误提取更严格拒绝，不宣称全部旧语义等价。
[[nodiscard]] Result<ScriptPreprocessSources> CaptureScriptPreprocessSources(
    const ScriptUserSourcePlan& entryPlan, const ScriptUserSourceRequest& currentSources,
    const ScriptPreprocessSourceRequest& request);
[[nodiscard]] Status VerifyScriptPreprocessSources(const ScriptPreprocessSources& sources,
    const ScriptUserSourceRequest& currentSources, const ScriptPreprocessSourceRequest& currentRequest);
// 只消费 owned 字节，不查磁盘；遇到未绑定的候选路径明确失败。
[[nodiscard]] Jass::PreprocessResult ReplayScriptPreprocessSources(const ScriptPreprocessSources& sources);

} // namespace War3Map::Script
