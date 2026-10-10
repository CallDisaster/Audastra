#pragma once

#include "War3MapCore/Jass/SourceMap.h"
#include "War3MapCore/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Script::Detail {

// 调用方必须从本次实际 includeProvider 的稳定读取提供内容；本服务不读取磁盘。
struct JapiPlaceholderInclude {
    std::filesystem::path path;
    std::string text;
    std::string sha256;
};

struct JapiPlaceholderRequest {
    std::string_view generated;          // 注入之前的生成脚本；作者 WTG/WCT 不在写入范围。
    std::string_view preprocessorInput;  // SourceMap 主文件实际使用的字节。
    std::string_view preprocessed;
    const Jass::SourceMap* sourceMap = nullptr;
    std::string_view mainSourceName = "war3map.j";
    std::span<const JapiPlaceholderInclude> includes;
};

struct JapiPlaceholderRetained {
    std::string name;
    std::vector<std::string> parameterTypes;
    std::string returnType; // 空字符串表示 nothing，沿用现有 Parser。
    std::string definition; // PP 时核定的完整实现；模板后只允许换行形式变化。
    std::filesystem::path includePath;
    std::string includeSha256;
    std::uint32_t includeFirstLine = 0;
    std::uint32_t generatedFirstLine = 0;
    std::uint32_t removedFirstLine = 0;
    bool parameterNamesDiffer = false;
};

struct JapiPlaceholderResult {
    std::string text; // 仅将经证明的生成 stub 改为空白，保留 CR/LF 与其它字节。
    std::vector<JapiPlaceholderRetained> retained;
};

// 原现代PP完成之后的只读探针；不读取includes、不授予删除权限。
// 仅确切的原generated普通全局裸marker在PP中活跃且同名重复/native冲突时返回true。
// 单定义runtime hook stub不触发owned读取策略，也不需要替代实现。
[[nodiscard]] bool HasActiveJapiPlaceholders(const JapiPlaceholderRequest& request);

// 只由现代内置 PP 接缝调用。无碰撞/候选不活跃时原字节返回，绝不从普通空函数推测 marker。
[[nodiscard]] Result<JapiPlaceholderResult> ReconcileJapiPlaceholders(const JapiPlaceholderRequest& request);

// 模板求值之后、写 4_template.j/交给编译器之前调用；禁止模板使实现身份或全局唯一性漂移。
[[nodiscard]] Status VerifyJapiPlaceholderResult(std::string_view text,
    std::span<const JapiPlaceholderRetained> retained);

} // namespace War3Map::Script::Detail
