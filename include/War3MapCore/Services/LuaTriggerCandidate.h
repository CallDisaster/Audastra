#pragma once

#include "War3MapCore/Script/LuaTriggerUnit.h"

namespace War3Map::Services {

struct LuaCandidateProfile {
    Script::LuaTargetProfile target;
    std::vector<Script::LuaCallbackBinding> callbacks;
};

// 显式声明，不执行脚本，也不凭签名认定目标客户端已支持 Lua/native。
// TSV schema forge-lua-target-v1；最多 1 MiB，未知记录/列数/重复 identity 拒绝。
[[nodiscard]] Result<LuaCandidateProfile> ParseLuaCandidateProfile(std::string_view text);

struct LuaTriggerInputIdentity {
    std::string revision;
    std::string wtgSha256, wctSha256, catalogSha256;
    std::string sourceWtgSha256, sourceWctSha256;
    std::string targetRequestSha256; // 包括显式回调，保留请求中的顺序
};

class LuaTriggerCandidate;
struct LuaCandidateCompileResult;
class PreparedLuaTriggerInput {
public:
    [[nodiscard]] const LuaTriggerInputIdentity* Identity() const noexcept;
private:
    struct State;
    std::shared_ptr<const State> state_;
    friend Result<PreparedLuaTriggerInput> PrepareLuaTriggerInput(const Trigger::TriggerDocument&,
        const Trigger::UiCatalog&, const LuaCandidateProfile&);
    friend LuaCandidateCompileResult CompileLuaTriggerCandidate(const PreparedLuaTriggerInput&, std::string_view);
};

// 在文档所属线程调用；输入在调用期间不得并发修改。返回独立副本，可交后台编译。
// 必须有经典解析器的 seal；当前 WTG/WCT 序列化及节点 ID 纳入内容修订。
// 只有保存可达图的内容与身份属于修订；废弃不可达节点的载荷不是编辑源。
[[nodiscard]] Result<PreparedLuaTriggerInput> PrepareLuaTriggerInput(const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const LuaCandidateProfile& profile);

class LuaTriggerCandidate {
public:
    [[nodiscard]] const LuaTriggerInputIdentity& Identity() const noexcept { return identity_; }
    [[nodiscard]] std::shared_ptr<const Script::LuaTriggerUnit> Unit() const noexcept { return unit_; }
private:
    LuaTriggerInputIdentity identity_;
    std::shared_ptr<const Script::LuaTriggerUnit> unit_;
    friend LuaCandidateCompileResult CompileLuaTriggerCandidate(const PreparedLuaTriggerInput&, std::string_view);
};

struct LuaCandidateCompileResult : Result<LuaTriggerCandidate> {
    // 保留机器可读节点/触发器身份；UI/Agent 不应从 message 反向提取 ID。
    std::vector<Script::LuaSourceDiagnostic> sourceDiagnostics;
    LuaCandidateCompileResult() = default;
    LuaCandidateCompileResult(Result<LuaTriggerCandidate> result) : Result<LuaTriggerCandidate>(std::move(result)) {}
};

// 结果永远只是 trigger-unit，不能作为完整 war3map.lua 保存许可。
[[nodiscard]] LuaCandidateCompileResult CompileLuaTriggerCandidate(const PreparedLuaTriggerInput& input,
    std::string_view expectedRevision);

struct LuaTriggerCandidateChunk {
    std::string inputRevision, scriptSha256;
    std::size_t offset = 0, totalBytes = 0;
    std::vector<std::byte> bytes; // 字节页可能切开 UTF-8 字符，适配器须用二进制编码传输
};
// 调用方须传当前文档完整输入修订；过期结果拒绝。单页最多 64 KiB。
[[nodiscard]] Result<LuaTriggerCandidateChunk> QueryLuaTriggerCandidate(const LuaTriggerCandidate& candidate,
    std::string_view currentInputRevision, std::size_t offset, std::size_t limit);

struct LuaTriggerSourcePage {
    std::string inputRevision, scriptSha256, sourceMapSha256;
    std::size_t line = 0, lineByteOffset = 0, lineByteLength = 0;
    std::size_t offset = 0, totalSpans = 0, totalMatches = 0;
    // 指定行时返回最小的覆盖区间，与当前页是否包含它无关；空值明确表示未映射。
    std::optional<Script::LuaSourceSpan> innermost;
    std::vector<Script::LuaSourceSpan> spans;
};

// line=0 按区间顺序分页；line>=1 定位实际生成文件的行并分页返回匹配来源。
// 换行按 LF 计数、兼容 CRLF；最后一个 LF 后的空行有效且无来源。
// 单页 1..128，绑定当前输入及报错脚本 SHA；行定位不代表表达式逐字符映射。
[[nodiscard]] Result<LuaTriggerSourcePage> QueryLuaTriggerSources(const LuaTriggerCandidate& candidate,
    std::string_view currentInputRevision, std::string_view expectedScriptSha256,
    std::size_t line, std::size_t offset, std::size_t limit);

} // namespace War3Map::Services
