#include "War3MapCore/Services/LuaTriggerCandidate.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::Services {
struct PreparedLuaTriggerInput::State {
    Trigger::TriggerDocument document;
    Trigger::UiCatalog catalog;
    LuaCandidateProfile profile;
    LuaTriggerInputIdentity identity;
};
namespace {
constexpr std::size_t kDocumentBytes = 32 * 1024 * 1024;
constexpr std::size_t kIdentityBytes = 8 * 1024 * 1024;
template<class T> Result<T> Fail(const char* code, const char* text) {
    return Result<T>::Failure("Script.LuaCandidate", code, text);
}
struct Budget {
    std::size_t left;
    void Take(std::size_t count) {
        if (count > left) throw std::length_error("Lua input budget");
        left -= count;
    }
    void Text(const Trigger::RawText& value) { Take(value.text.size()); Take(value.originalBytes.size()); }
};
void CheckDocument(const Trigger::TriggerDocument& document) {
    if (document.categories.size() > 8192 || document.variables.size() > 8192 || document.triggers.size() > 8192
        || document.ecaNodes.size() > 200000 || document.argumentNodes.size() > 200000)
        throw std::length_error("Lua node budget");
    Budget budget{kDocumentBytes};
    budget.Text(document.wct.customCode); budget.Text(document.wct.customComment);
    for (const auto& v : document.categories) budget.Text(v.name);
    for (const auto& v : document.variables) { budget.Text(v.name); budget.Text(v.typeName); budget.Text(v.value); }
    for (const auto& v : document.triggers) {
        budget.Text(v.name); budget.Text(v.description); budget.Text(v.customText);
        if (v.ecas.size() > 200000) throw std::length_error("Lua edge budget");
        budget.Take(v.ecas.size() * sizeof(std::uint32_t));
    }
    for (const auto& v : document.ecaNodes) {
        budget.Text(v.name); budget.Take(v.uiSignature.size()); budget.Take(v.uiLayer.size());
        budget.Take(v.uiSource.native().size() * sizeof(std::filesystem::path::value_type));
        if (v.arguments.size() > 200000 || v.children.size() > 200000) throw std::length_error("Lua edge budget");
        budget.Take((v.arguments.size() + v.children.size()) * sizeof(std::uint32_t));
    }
    for (const auto& v : document.argumentNodes) budget.Text(v.value);
}
class Fingerprint {
public:
    void Add(std::string_view value) {
        budget_.Take(24); budget_.Take(value.size());
        text_ += std::to_string(value.size()); text_ += ':'; text_ += value;
    }
    void Id(std::uint64_t value) { Add(std::to_string(value)); }
    std::string Hash() const {
        const auto hash = Sha256Text(text_);
        if (!hash.Succeeded()) throw std::runtime_error("Lua input hash");
        return *hash.value;
    }
private:
    Budget budget_{kIdentityBytes};
    std::string text_;
};
std::string ProfileHash(const LuaCandidateProfile& profile) {
    const auto& target = profile.target;
    if (target.apis.size() > 4096 || target.handleTypes.size() > 512 || target.constants.size() > 4096
        || profile.callbacks.size() > 1024)
        throw std::length_error("Lua target budget");
    Fingerprint out; out.Add("forge-lua-target-request-v1"); out.Add(target.identity);
    out.Id(target.handleTypes.size());
    for (const auto& [name, parent] : target.handleTypes) { out.Add(name); out.Add(parent); }
    out.Id(target.apis.size());
    for (const auto& api : target.apis) {
        if (api.parameters.size() > 32) throw std::length_error("Lua parameter budget");
        out.Add(api.name); out.Add(api.returns); out.Id(api.parameters.size());
        for (const auto& parameter : api.parameters) out.Add(parameter);
    }
    out.Id(target.constants.size());
    for (const auto& constant : target.constants) { out.Add(constant.name); out.Add(constant.type); }
    out.Id(profile.callbacks.size());
    for (const auto& binding : profile.callbacks) { out.Add(binding.symbol); out.Id(binding.triggerId); }
    return out.Hash();
}
std::string Revision(const Trigger::TriggerDocument& document, const LuaTriggerInputIdentity& identity) {
    Fingerprint out; out.Add("forge-lua-trigger-input-v1");
    for (const auto* value : {&identity.wtgSha256, &identity.wctSha256, &identity.catalogSha256,
        &identity.sourceWtgSha256, &identity.sourceWctSha256, &identity.targetRequestSha256}) out.Add(*value);
    // WTG 线格式不含编辑期稳定 ID；Lua 符号却使用它，必须单独绑定。
    const auto ids = [&](const auto& records) {
        out.Id(records.size()); for (const auto& record : records) out.Id(record.id);
    };
    ids(document.categories); ids(document.variables); ids(document.triggers);
    ids(document.ecaNodes); ids(document.argumentNodes);
    return out.Hash();
}
}

const LuaTriggerInputIdentity* PreparedLuaTriggerInput::Identity() const noexcept {
    return state_ ? &state_->identity : nullptr;
}

Result<PreparedLuaTriggerInput> PrepareLuaTriggerInput(const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const LuaCandidateProfile& profile) {
    try {
        CheckDocument(document);
        const auto profileHash = ProfileHash(profile);
        // 复用保存入口的 seal/图/签名校验；此处只在内存编码，不写文件。
        const auto encoded = Trigger::WtgWctCodec{}.Serialize(document, catalog);
        if (!encoded.Succeeded()) return {std::nullopt, encoded.diagnostics};
        if (encoded.value->wtg.size() > kDocumentBytes || encoded.value->wct.size() > kDocumentBytes - encoded.value->wtg.size())
            throw std::length_error("Lua encoded source budget");
        const auto wtgHash = Sha256(encoded.value->wtg), wctHash = Sha256(encoded.value->wct);
        if (!wtgHash.Succeeded() || !wctHash.Succeeded())
            return Fail<PreparedLuaTriggerInput>("LUA_INPUT_HASH", "无法计算编译输入指纹。");
        LuaTriggerInputIdentity identity;
        identity.wtgSha256 = *wtgHash.value; identity.wctSha256 = *wctHash.value;
        identity.catalogSha256 = catalog.Sha256(); identity.sourceWtgSha256 = document.SourceWtgSha256();
        identity.sourceWctSha256 = document.SourceWctSha256(); identity.targetRequestSha256 = profileHash;
        identity.revision = Revision(document, identity);
        PreparedLuaTriggerInput result;
        result.state_ = std::make_shared<const PreparedLuaTriggerInput::State>(
            PreparedLuaTriggerInput::State{document, catalog, profile, std::move(identity)});
        return Result<PreparedLuaTriggerInput>::Success(std::move(result));
    } catch (const std::length_error&) {
        return Fail<PreparedLuaTriggerInput>("LUA_INPUT_BUDGET", "编译来源、节点或目标声明超过冻结预算。");
    } catch (const std::bad_alloc&) {
        return Fail<PreparedLuaTriggerInput>("LUA_INPUT_ALLOCATION", "无法冻结独立编译输入。");
    } catch (const std::exception&) {
        return Fail<PreparedLuaTriggerInput>("LUA_INPUT_FAILED", "编译输入冻结失败，没有可用快照。");
    }
}

LuaCandidateCompileResult CompileLuaTriggerCandidate(const PreparedLuaTriggerInput& input,
    std::string_view expectedRevision) {
    if (!input.state_ || input.state_->identity.revision != expectedRevision)
        return Fail<LuaTriggerCandidate>("LUA_INPUT_STALE", "请求未绑定该冻结编译输入的完整修订。");
    try {
        const auto& state = *input.state_;
        auto unit = Script::GenerateLuaTriggerUnit(state.document, state.catalog, state.profile.target, state.profile.callbacks);
        if (!unit.Succeeded()) {
            LuaCandidateCompileResult failure;
            failure.sourceDiagnostics = std::move(unit.diagnostics);
            for (const auto& item : failure.sourceDiagnostics) failure.diagnostics.push_back(item.diagnostic);
            return failure;
        }
        if (unit.text.size() > 8 * 1024 * 1024)
            return Fail<LuaTriggerCandidate>("LUA_CANDIDATE_BUDGET", "完整候选超过 8 MiB，没有返回部分脚本。");
        LuaTriggerCandidate result;
        result.identity_ = state.identity;
        result.unit_ = std::make_shared<const Script::LuaTriggerUnit>(std::move(unit));
        return Result<LuaTriggerCandidate>::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Fail<LuaTriggerCandidate>("LUA_CANDIDATE_ALLOCATION", "候选编译内存不足。");
    } catch (const std::exception&) {
        return Fail<LuaTriggerCandidate>("LUA_CANDIDATE_FAILED", "候选编译失败，没有返回脚本。");
    }
}

Result<LuaTriggerCandidateChunk> QueryLuaTriggerCandidate(const LuaTriggerCandidate& candidate,
    std::string_view currentInputRevision, std::size_t offset, std::size_t limit) {
    const auto unit = candidate.Unit();
    if (!unit || candidate.Identity().revision != currentInputRevision)
        return Fail<LuaTriggerCandidateChunk>("LUA_CANDIDATE_STALE", "候选不属于当前文档/函数库/目标/回调修订。");
    if (limit == 0 || limit > 65536 || offset > unit->text.size())
        return Fail<LuaTriggerCandidateChunk>("LUA_CANDIDATE_RANGE", "候选字节范围无效，单页必须为 1..65536 字节。");
    try {
        const auto count = std::min(limit, unit->text.size() - offset);
        const auto begin = reinterpret_cast<const std::byte*>(unit->text.data()) + offset;
        return Result<LuaTriggerCandidateChunk>::Success({candidate.Identity().revision, unit->scriptSha256,
            offset, unit->text.size(), std::vector<std::byte>(begin, begin + count)});
    } catch (const std::bad_alloc&) {
        return Fail<LuaTriggerCandidateChunk>("LUA_CANDIDATE_ALLOCATION", "无法分配候选字节页。");
    }
}
} // namespace War3Map::Services
