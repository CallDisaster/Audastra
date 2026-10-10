#pragma once

#include "War3MapCore/Jass/SourceMap.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Types.h"
#include "War3MapCore/Text/SourceEncoding.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 自研脚本预处理器：完整的 C99 预处理（#define 含函数宏 / 可变参数 / # / ##、#include、#if 表达式、
// #ifdef / #ifndef / #elif / #else / #endif、#undef、#error、#pragma once），用于替代 YDWE 流程中的
// Boost.Wave。行为按 Wave 的可观察输出对齐（独立实现）：
//   - <文件> 只在系统目录中查找；"文件" 依次查找当前文件目录、用户目录、系统目录；
//   - '$' 视为标识符字符（JASS 十六进制 $FF、vJass textmacro 的 $NAME$ 不会被宏替换）；
//   - 预定义 __WAVE__ / __WAVE_HAS_VARIADICS__ / __STDC_VERSION__=199901L，使 Boost.Preprocessor 走同一配置。
// 与 Wave 不同之处：JASS 字符串可以跨行（WE 会写出含换行的字符串）；未展开的文本（含注释）原样输出，
// 便于 jasshelper 读取 //! 指令和对照阅读。
namespace War3Map::Jass {

enum class SourceOpaqueKind { Comment, String, Rawcode };
struct SourceOpaqueSpan {
    std::uint32_t offset = 0, length = 0;
    SourceOpaqueKind kind = SourceOpaqueKind::Comment;
    OriginSpan origin;
    std::string sourceSha256;
    std::uint32_t sourceLine = 0;
    [[nodiscard]] bool operator==(const SourceOpaqueSpan& other) const {
        return offset == other.offset && length == other.length && kind == other.kind &&
            origin.source == other.origin.source && origin.offset == other.origin.offset &&
            origin.length == other.origin.length && origin.fingerprint == other.origin.fingerprint &&
            sourceSha256 == other.sourceSha256 && sourceLine == other.sourceLine;
    }
};
// 与原文等长的词法视图；只有已严格验证的GBK不透明片段可以屏蔽。
// 编译输出仍使用原字节；视图不是可写出的转码文本。
struct SourceLexicalView {
    std::string text;
    std::vector<SourceOpaqueSpan> opaque;
};
[[nodiscard]] Status ValidateSourceLexicalView(std::string_view original, const SourceLexicalView& view);

struct PreprocessIncludeContent {
    bool exists = false;
    std::string text;
};

enum class IncludeCandidateEligibility { Allowed, SkippedOutOfScope };
enum class IncludeSearchRole { CurrentFile, User, System, Absolute };
enum class IncludePhase { FrameworkDollar, CPreprocessor };
struct IncludeCandidateRequest {
    IncludePhase phase = IncludePhase::CPreprocessor;
    std::filesystem::path requester, searchDirectory, candidate;
    std::string spelling;
    // 借用当前阶段实际输入；offset/length 属于这些字节，不冒称作者原文件位置。
    std::string_view requesterStageText;
    std::uint32_t requesterLine = 0, requesterOffset = 0, requesterLength = 0, searchOrdinal = 0;
    IncludeSearchRole searchRole = IncludeSearchRole::CurrentFile;
    bool angled = false, forced = false;
};

[[nodiscard]] inline JassCheckOptions PreprocessCheckOptions() {
    JassCheckOptions limits;
    limits.maxSources = 1024; // 实际 include/预定义也登记为来源，含重复读取。
    return limits;
}

struct PreprocessOptions {
    std::vector<std::filesystem::path> systemIncludes; // <文件> 与 "文件" 的最后一级
    std::vector<std::filesystem::path> userIncludes;   // "文件" 的第二级（第一级为当前文件所在目录）
    // 预定义宏：{名称, 值}；值为空时定义为 1（与命令行 --define=NAME 相同）。
    std::vector<std::pair<std::string, std::string>> defines;
    // 在主文件之前包含的文件（按 "文件" 规则、相对主文件目录查找）。
    std::vector<std::string> forceIncludes;
    std::uint32_t maxIncludeDepth = 200;
    // 提供器接管全部包含读取；不存在时继续下一候选，失败时不回退到磁盘或低优先级目录。
    std::function<Result<PreprocessIncludeContent>(const std::filesystem::path&)> includeProvider;
    // 纯路径资格预检：不得访问文件系统；Skipped 不等于 Missing，允许继续下一候选。
    std::function<Result<IncludeCandidateEligibility>(const IncludeCandidateRequest&)> includeCandidatePolicy;
    std::function<Result<SourceLexicalView>(std::string_view, const std::filesystem::path&)> lexicalViewProvider;
    std::optional<SourceLexicalView> mainLexicalView;
    // 0 使用 check.maxTotalBytes 硬上限；非零时取两者中较小的输出/单次展开预算。
    std::size_t maxOutputBytes = 0;
    JassCheckOptions check = PreprocessCheckOptions();
};

// C include 和框架 $include 共用；未设置 policy 时保留原查找行为。
[[nodiscard]] Result<IncludeCandidateEligibility> CheckIncludeCandidate(
    const PreprocessOptions& options, const IncludeCandidateRequest& request);

struct PreprocessResult {
    std::string text;
    SourceLexicalView lexicalView; // 仅显式框架来源模式拥有，原现代调用为空。
    std::vector<Diagnostic> diagnostics;
    SourceMap sourceMap;            // 输出行 → 源文件行
    std::size_t filesIncluded = 0;  // 实际读取（含缓存命中）的包含文件次数
    std::size_t cacheHits = 0;      // 按内容哈希复用词法结果的次数
    std::size_t macroExpansions = 0;
    bool completed = true;
    std::size_t tokens = 0;
    std::size_t expansionNodes = 0;

    [[nodiscard]] bool Succeeded() const noexcept;
};

// 包含文件的词法缓存：按文件内容哈希复用词法结果。可在多次构建间共享（线程不安全，同一时间只供一次 Run 使用）。
class PreprocessCache {
public:
    PreprocessCache();
    ~PreprocessCache();
    PreprocessCache(const PreprocessCache&) = delete;
    PreprocessCache& operator=(const PreprocessCache&) = delete;

    [[nodiscard]] std::size_t Entries() const noexcept;
    void Clear();

    struct Impl;
    [[nodiscard]] Impl& Get() noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// 预处理主文件文本（mainName 用于诊断与 __FILE__，mainDirectory 用于查找 "文件"）。
[[nodiscard]] PreprocessResult Preprocess(std::string_view mainText, const std::string& mainName,
    const std::filesystem::path& mainDirectory, const PreprocessOptions& options, PreprocessCache* cache = nullptr);

} // namespace War3Map::Jass
