#pragma once

#include "War3MapCore/Text/SourceEncoding.h"
#include "War3MapCore/Jass/Preprocessor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Jass {

inline constexpr std::string_view kUdgIdentifierRule = "forge-udg-codepoints-v1";

struct IdentifierRewriteLimits {
    std::size_t maxInputBytes = 16 * 1024 * 1024;
    std::size_t maxOutputBytes = 32 * 1024 * 1024;
    std::size_t maxTokens = 1024 * 1024;
    std::size_t maxMappings = 16384;
    std::size_t maxReferences = 262144;
    std::size_t maxSyntaxDepth = 128;
};

struct IdentifierRewriteRequest {
    std::string_view text;
    std::optional<Text::SourceEncoding> encoding; // 必须显式声明，不猜测/默认UTF-8。
    std::string sourceName; // 仅报告位置；不得参与稳定名称摘要。
    std::string expectedInputSha256; // 必须绑定本次完整阶段文本。
    std::string globalScopeIdentity; // 调用方给出的稳定语义域，不得用路径/行号/输入SHA。
    IdentifierRewriteLimits limits;
    const SourceLexicalView* lexicalView = nullptr; // 可选的逐来源等长视图，服务重新严格验证。
};

struct IdentifierOccurrence {
    OriginSpan origin;
    std::uint32_t line = 0;
};

struct IdentifierRename {
    std::string originalBytes;
    std::u32string codepoints; // 不做 Unicode 归一化；UTF-8 与 CP936 等码点产生同名。
    std::string generatedName;
    std::string scopeIdentity;
    IdentifierOccurrence declaration;
    std::vector<IdentifierOccurrence> references; // 不含声明；size() 即真实引用次数。
};

enum class IdentifierRewriteCoverage {
    NoCandidate, // 仅词法/输入身份有效；未验证无关中文名称、旧别名或其他方言的语义。
    RootGlobalJass,
};

struct IdentifierRewriteResult {
    std::string text;
    std::string inputSha256, outputSha256;
    std::string ruleVersion;
    IdentifierRewriteCoverage coverage = IdentifierRewriteCoverage::NoCandidate;
    std::vector<IdentifierRename> mappings;
    SourceLexicalView lexicalView;
};

// 原创纯内存服务：只改根 globals 中含 CJK 基本区/扩展A的 udg_ 变量及已绑定 Name 引用。
// UTF-8 BOM只在开头作为标记解释且原字节保留；非目标片段不转码或重排。
// private/public、scope/library/struct、局部遮蔽、未展开指令和未知绑定拒绝，无部分结果。
// 当前只接受单一显式编码阶段文本，不能冒称混合 include 来源映射；不授予保存/类型例外。
[[nodiscard]] Result<IdentifierRewriteResult> RewriteUdgIdentifiers(const IdentifierRewriteRequest& request);

// 根级JASS或完整语句片段的精确标识符改写。字符串、注释和原始排版保持原字节。
// 不解释宏/vJass作用域；局部遮蔽、动态分发及不完整语法明确拒绝。
struct ExactIdentifierMapping { std::string before, after; };
struct ExactIdentifierDeclaration {
    std::string name;
    std::uint32_t offset = 0, line = 0;
    bool function = false, local = false, initializer = false;
};
struct ExactIdentifierResult {
    std::string text;
    std::vector<ExactIdentifierDeclaration> declarations;
    std::size_t replacements = 0;
};
[[nodiscard]] Result<ExactIdentifierResult> RewriteExactIdentifiers(
    const IdentifierRewriteRequest& request, std::span<const ExactIdentifierMapping> mappings,
    bool statements = false);

} // namespace War3Map::Jass
