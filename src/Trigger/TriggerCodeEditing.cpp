#include "War3MapCore/Trigger/TriggerCodeEditing.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "TriggerIdAllocation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace War3Map::Trigger {
namespace detail {
enum class CodeChangeKind { Text, Create, Convert, Recover };
struct CodeEditState {
    CodeChangeKind kind = CodeChangeKind::Text;
    CodeBlockTarget target;
    bool changed = false;
    std::string sourceWtg, sourceWct, catalog;
    std::string beforeBinding;
    mutable std::string afterBinding; // 结构转换首次应用后才确定。
    RawText beforeText, afterText;
    TriggerRecord afterTrigger;
    std::optional<CategoryRecord> newCategory;
    std::size_t categoryRow = 0;
    mutable TriggerDocumentEditor::TriggerRemoveReceipt removed; // 最近一次正向删除的回执
    // 转回 T：被替换的代码记录，以及把片段追加到节点表末尾的恢复回执。
    TriggerRecord beforeTrigger;
    TriggerDocumentEditor::TriggerRemoveReceipt restore;
    std::size_t row = 0;
    // 完整核验模式下首次应用时记录的整图身份；之后每次重放前后都与之比对。
    mutable std::string fullBefore, fullAfter;
};
}
namespace {
using State = detail::CodeEditState;
constexpr std::size_t kBindingBudget = 64 * 1024 * 1024;
static_assert(std::is_nothrow_swappable_v<RawText>);
static_assert(std::is_nothrow_swappable_v<TriggerDocument>);

class BindingWriter {
public:
    void Number(std::uint64_t value) {
        std::byte bytes[8];
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<std::byte>(value >> (i * 8));
        Append(bytes);
    }
    void Text(std::string_view value) {
        Number(value.size()); Append(std::as_bytes(std::span(value.data(), value.size())));
    }
    void Bytes(std::span<const std::byte> value) {
        Text({reinterpret_cast<const char*>(value.data()), value.size()});
    }
    void Raw(const RawText& value) {
        Number(static_cast<unsigned>(value.encoding)); Text(value.text); Bytes(value.originalBytes);
    }
    void Origin(const OriginSpan& value) {
        Path(value.source);
        Number(value.offset); Number(value.length); Text(value.fingerprint);
    }
    void Record(const TriggerRecord& value) {
        Number(value.id); Raw(value.name); Raw(value.description);
        Number(value.type); Number(value.enabled); Number(value.customTextFlag);
        Number(value.initiallyClosed); Number(value.runOnInit); Number(value.categoryId);
        Number(value.ecas.size()); for (const auto index : value.ecas) Number(index);
        Raw(value.customText); Origin(value.origin);
    }
    std::string Finish() {
        if (closed_) throw std::runtime_error("代码图身份 writer 已封闭。");
        closed_ = true;
        Flush();
        auto hash = stream_.Finish();
        if (!hash.Succeeded()) throw std::runtime_error("代码身份摘要生成失败。");
        return std::move(*hash.value);
    }
    std::size_t BytesCharged() const noexcept { return accepted_; }
    std::size_t ScratchBytes() const noexcept {
        return stream_.ScratchBytes() + pathScratch_ + pathNativeScratch_ + pathMemoOwned_
            + sizeof(BindingWriter) - sizeof(Sha256Stream);
    }
    void Path(const std::filesystem::path& path) {
        // 仅复用本次捕获中的路径转换；每个字段仍按原长度和原字节参与完整身份。
        // 按实际 native 值比较，不跨文档、代际或 before/wanted 捕获保留缓存。
        for (std::size_t index = 0; index < pathMemoCount_; ++index) {
            const auto& entry = pathMemo_[index];
            if (entry.native == path.native()) {
                Text({reinterpret_cast<const char*>(entry.encoded.data()), entry.encoded.size()});
                return;
            }
        }
        auto encoded = path.generic_u8string();
        pathScratch_ = (std::max)(pathScratch_, encoded.capacity() + 1);
        constexpr std::size_t memoBudget = 64 * 1024;
        const bool shortNative = path.native().size() <= 2048;
        const auto nativeBytes = shortNative
            ? (path.native().size() + 1) * sizeof(std::filesystem::path::value_type) : memoBudget + 1;
        if (pathMemoCount_ < pathMemo_.size() && shortNative
            && encoded.capacity() < memoBudget && nativeBytes <= memoBudget - encoded.capacity() - 1
            && nativeBytes + encoded.capacity() + 1 <= memoBudget - pathMemoOwned_) {
            std::filesystem::path::string_type native(path.native());
            const auto capacityBytes = (native.capacity() + 1) * sizeof(std::filesystem::path::value_type);
            pathNativeScratch_ = (std::max)(pathNativeScratch_, capacityBytes);
            if (capacityBytes <= memoBudget - encoded.capacity() - 1
                && capacityBytes + encoded.capacity() + 1 <= memoBudget - pathMemoOwned_) {
                auto& entry = pathMemo_[pathMemoCount_++];
                pathMemoOwned_ += capacityBytes + encoded.capacity() + 1;
                entry.native = std::move(native); entry.encoded = std::move(encoded);
                Text({reinterpret_cast<const char*>(entry.encoded.data()), entry.encoded.size()});
                return;
            }
        }
        Text({reinterpret_cast<const char*>(encoded.data()), encoded.size()});
    }
private:
    // 元数据含数百万短 Number/Text。局部合批只减少 Update/Status 调用；
    // 每个原 Append 仍准入同 64MiB，完整字节顺序、配对摘要与两次 capture 不变。
    void Feed(std::span<const std::byte> bytes) {
        const auto status = stream_.Update(bytes);
        if (!status.Succeeded()) {
            closed_ = true;
            throw std::runtime_error("代码图身份摘要或 64 MiB 预算检查失败。");
        }
    }
    void Flush() {
        if (!used_) return;
        Feed(std::span(pending_.data(), used_));
        used_ = 0;
    }
    void Append(std::span<const std::byte> bytes) {
        if (closed_ || bytes.size() > kBindingBudget - accepted_) {
            closed_ = true;
            throw std::runtime_error("代码图身份摘要或 64 MiB 预算检查失败。");
        }
        accepted_ += bytes.size();
        while (!bytes.empty()) {
            if (!used_ && bytes.size() >= pending_.size()) {
                Feed(bytes); // 已编码成员等大 span 原样直通，不再拥有另一整段。
                return;
            }
            const auto count = (std::min)(bytes.size(), pending_.size() - used_);
            std::copy_n(bytes.data(), count, pending_.data() + used_);
            used_ += count;
            bytes = bytes.subspan(count);
            if (used_ == pending_.size()) Flush();
        }
    }
    Sha256Stream stream_{kBindingBudget};
    std::array<std::byte, 8 * 1024> pending_{};
    std::size_t accepted_ = 0, used_ = 0;
    bool closed_ = false;
    std::size_t pathScratch_ = 0;
    struct PathMemo {
        std::filesystem::path::string_type native;
        std::u8string encoded;
    };
    std::array<PathMemo, 8> pathMemo_{};
    std::size_t pathMemoCount_ = 0, pathMemoOwned_ = 0, pathNativeScratch_ = 0;
};

const TriggerRecord* Find(const TriggerDocument& document, StableId id) {
    const TriggerRecord* result = nullptr;
    for (const auto& record : document.triggers) {
        if (record.id != id) continue;
        if (result) return nullptr; // 重复身份也不能任意选一个。
        result = &record;
    }
    return result;
}
RawText* TargetText(TriggerDocument& document, CodeBlockTarget target) {
    if (target.kind == CodeBlockKind::GlobalHeader && target.triggerId == 0) return &document.wct.customCode;
    if (target.kind != CodeBlockKind::CustomTrigger || target.triggerId == 0) return nullptr;
    auto* record = TriggerDocumentEditor::FindTrigger(document, target.triggerId);
    return record && record->type == 0 && record->customTextFlag == 1 && record->ecas.empty()
        ? &record->customText : nullptr;
}
const RawText* TargetText(const TriggerDocument& document, CodeBlockTarget target) {
    if (target.kind == CodeBlockKind::GlobalHeader && target.triggerId == 0) return &document.wct.customCode;
    if (target.kind != CodeBlockKind::CustomTrigger || target.triggerId == 0) return nullptr;
    const auto* record = Find(document, target.triggerId);
    return record && record->type == 0 && record->customTextFlag == 1 && record->ecas.empty()
        ? &record->customText : nullptr;
}
void SourceBinding(BindingWriter& writer, const TriggerDocument& document, const UiCatalog& catalog) {
    writer.Number(document.HasTrustedSourceSeal()); writer.Text(document.SourceWtgSha256());
    writer.Text(document.SourceWctSha256()); writer.Text(document.UiCatalogSha256()); writer.Text(catalog.Sha256());
}
std::string TextBinding(const TriggerDocument& document, const UiCatalog& catalog, CodeBlockTarget target) {
    BindingWriter writer;
    SourceBinding(writer, document, catalog);
    writer.Number(static_cast<unsigned>(target.kind)); writer.Number(target.triggerId);
    if (!TargetText(document, target)) throw std::runtime_error("代码块已不存在或类型已改变。");
    if (target.kind == CodeBlockKind::GlobalHeader) {
        writer.Number(document.wct.version); writer.Raw(document.wct.customComment); writer.Raw(document.wct.customCode);
    } else writer.Record(*Find(document, target.triggerId));
    return writer.Finish();
}
struct GraphBindingDetails {
    std::string wtgSha256, wctSha256;
    std::size_t bytes = 0, encodedBytes = 0, encodedCapacity = 0, scratchBytes = 0;
};
std::string GraphBinding(const TriggerDocument& document, const UiCatalog& catalog, GraphBindingDetails* details = nullptr) {
    auto encoded = WtgWctCodec{}.Serialize(document, catalog);
    if (!encoded.Succeeded()) throw std::runtime_error("触发器图不能通过共用序列化门禁。");
    // 两个字节成员各有 8B 长度头；此必要门在任何新增摘要工作前拒绝超帽成员。
    if (encoded.value->wtg.size() > kBindingBudget - 16
        || encoded.value->wct.size() > kBindingBudget - 16 - encoded.value->wtg.size())
        throw std::runtime_error("配对格式字节已超过完整身份预算。");
    const auto digest = [&] {
        BindingWriter writer;
        SourceBinding(writer, document, catalog);
        writer.Bytes(encoded.value->wtg); writer.Bytes(encoded.value->wct);
        writer.Number(document.wct.version); writer.Raw(document.wct.customComment); writer.Raw(document.wct.customCode);
        // 文件字节不存 stable ID / origin / 编码提示，这些也属于重放身份。
        writer.Number(document.categories.size());
        for (const auto& category : document.categories) {
            writer.Number(category.id); writer.Raw(category.name); writer.Origin(category.origin);
        }
        writer.Number(document.variables.size());
        for (const auto& variable : document.variables) {
            writer.Number(variable.id); writer.Raw(variable.name); writer.Raw(variable.typeName);
            writer.Raw(variable.value); writer.Origin(variable.origin);
        }
        writer.Number(document.triggers.size()); for (const auto& record : document.triggers) writer.Record(record);
        writer.Number(document.ecaNodes.size());
        for (const auto& eca : document.ecaNodes) {
            writer.Number(eca.id); writer.Number(eca.sourceTriggerId); writer.Raw(eca.name); writer.Origin(eca.origin);
            writer.Number(eca.arguments.size()); for (const auto index : eca.arguments) writer.Number(index);
            writer.Number(eca.children.size()); for (const auto index : eca.children) writer.Number(index);
            writer.Path(eca.uiSource);
            writer.Text(eca.uiLayer); writer.Text(eca.uiSignature);
        }
        writer.Number(document.argumentNodes.size());
        for (const auto& argument : document.argumentNodes) {
            writer.Number(argument.id); writer.Raw(argument.value); writer.Origin(argument.origin);
            writer.Number(argument.callEca.has_value()); if (argument.callEca) writer.Number(*argument.callEca);
            writer.Number(argument.indexArgument.has_value()); if (argument.indexArgument) writer.Number(*argument.indexArgument);
        }
        auto value = writer.Finish();
        if (details) {
            details->bytes = writer.BytesCharged();
            details->scratchBytes = writer.ScratchBytes();
        }
        return value;
    }(); // 完整身份准入/摘要成功后释放 writer，配对摘要不叠加第二个流式状态。
    if (details) {
        const auto pairDigest = [&](std::span<const std::byte> bytes) {
            Sha256Stream stream(bytes.size());
            const auto updated = stream.Update(bytes);
            if (!updated.Succeeded()) throw std::runtime_error("配对格式摘要更新失败。");
            auto hash = stream.Finish();
            if (!hash.Succeeded()) throw std::runtime_error("配对格式摘要终结失败。");
            details->scratchBytes = (std::max)(details->scratchBytes, stream.ScratchBytes());
            return std::move(*hash.value);
        };
        details->wtgSha256 = pairDigest(encoded.value->wtg);
        details->wctSha256 = pairDigest(encoded.value->wct);
        details->encodedBytes = encoded.value->wtg.size() + encoded.value->wct.size();
        if (encoded.value->wct.capacity() > (std::numeric_limits<std::size_t>::max)() - encoded.value->wtg.capacity())
            throw std::runtime_error("配对格式容量账溢出。");
        details->encodedCapacity = encoded.value->wtg.capacity() + encoded.value->wct.capacity();
    }
    return digest;
}
// 完整核验：测试与 Debug 构建默认开启，或由环境变量 AUDASTRA_TRIGGER_FULL_REPLAY_VERIFY=1 开启。
// 产品默认只做局部身份核对；整图字节身份由保存门禁负责。
std::atomic<int>& FullVerificationOverride() {
    static std::atomic<int> value{-1};
    return value;
}
bool FullVerification() {
    const auto overridden = FullVerificationOverride().load(std::memory_order_relaxed);
    if (overridden >= 0) return overridden != 0;
    static const bool fromEnvironment = [] {
#ifndef NDEBUG
        return true;
#else
        char* value = nullptr;
        std::size_t length = 0;
        const bool enabled = _dupenv_s(&value, &length, "AUDASTRA_TRIGGER_FULL_REPLAY_VERIFY") == 0
            && value && std::string_view(value) == "1";
        std::free(value);
        return enabled;
#endif
    }();
    return fromEnvironment;
}

void WriteEca(BindingWriter& writer, std::size_t index, const EcaNode& eca) {
    writer.Number(index); writer.Number(eca.id); writer.Number(eca.sourceTriggerId);
    writer.Number(static_cast<std::uint32_t>(eca.type)); writer.Number(eca.childId.has_value());
    if (eca.childId) writer.Number(static_cast<std::uint32_t>(*eca.childId));
    writer.Raw(eca.name); writer.Number(static_cast<std::uint32_t>(eca.enabled)); writer.Origin(eca.origin);
    writer.Number(eca.arguments.size()); for (const auto value : eca.arguments) writer.Number(value);
    writer.Number(eca.children.size()); for (const auto value : eca.children) writer.Number(value);
    writer.Path(eca.uiSource); writer.Text(eca.uiLayer); writer.Text(eca.uiSignature);
}
void WriteArgument(BindingWriter& writer, std::size_t index, const ArgumentNode& argument) {
    writer.Number(index); writer.Number(argument.id); writer.Number(static_cast<std::uint32_t>(argument.type));
    writer.Raw(argument.value); writer.Origin(argument.origin);
    writer.Number(static_cast<std::uint32_t>(argument.insertCall));
    writer.Number(argument.callEca.has_value()); if (argument.callEca) writer.Number(*argument.callEca);
    writer.Number(static_cast<std::uint32_t>(argument.insertIndex));
    writer.Number(argument.indexArgument.has_value()); if (argument.indexArgument) writer.Number(*argument.indexArgument);
}
// 目标触发器的完整子树（ECA 与参数，含嵌套调用和数组下标），按下标升序。
void CollectSubtree(const TriggerDocument& document, const std::vector<std::uint32_t>& roots,
    std::vector<std::uint32_t>& ecas, std::vector<std::uint32_t>& arguments) {
    std::vector<char> seenEca(document.ecaNodes.size(), 0), seenArgument(document.argumentNodes.size(), 0);
    std::vector<std::uint32_t> pendingEca(roots.begin(), roots.end()), pendingArgument;
    while (!pendingEca.empty() || !pendingArgument.empty()) {
        if (!pendingEca.empty()) {
            const auto index = pendingEca.back(); pendingEca.pop_back();
            if (index >= document.ecaNodes.size()) throw std::runtime_error("触发器子树引用了不存在的 ECA。");
            if (seenEca[index]) continue;
            seenEca[index] = 1; ecas.push_back(index);
            const auto& eca = document.ecaNodes[index];
            pendingEca.insert(pendingEca.end(), eca.children.begin(), eca.children.end());
            pendingArgument.insert(pendingArgument.end(), eca.arguments.begin(), eca.arguments.end());
        } else {
            const auto index = pendingArgument.back(); pendingArgument.pop_back();
            if (index >= document.argumentNodes.size()) throw std::runtime_error("触发器子树引用了不存在的参数。");
            if (seenArgument[index]) continue;
            seenArgument[index] = 1; arguments.push_back(index);
            const auto& argument = document.argumentNodes[index];
            if (argument.callEca) pendingEca.push_back(*argument.callEca);
            if (argument.indexArgument) pendingArgument.push_back(*argument.indexArgument);
        }
    }
    std::sort(ecas.begin(), ecas.end()); std::sort(arguments.begin(), arguments.end());
}
std::size_t RowOf(const TriggerDocument& document, StableId id) {
    std::size_t found = document.triggers.size();
    for (std::size_t row = 0; row < document.triggers.size(); ++row) {
        if (document.triggers[row].id != id) continue;
        if (found != document.triggers.size()) throw std::runtime_error("触发器身份重复。");
        found = row;
    }
    return found;
}
// 结构转换（新建/转换）的局部重放身份：来源 seal、各集合规模、全部变量声明（生成代码按变量名引用）、
// 目标行与相邻行身份、目标触发器记录及其完整子树。撤销栈按后进先出重放，规模或目标区域的任何变化都会被拒绝；
// 整图字节身份只在保存时及完整核验模式下检查。
std::string LocalGraphBinding(const TriggerDocument& document, const UiCatalog& catalog, const State& state, bool after) {
    BindingWriter writer;
    SourceBinding(writer, document, catalog);
    writer.Number(static_cast<unsigned>(state.kind)); writer.Number(after);
    writer.Number(document.categories.size()); writer.Number(document.variables.size());
    writer.Number(document.triggers.size()); writer.Number(document.ecaNodes.size());
    writer.Number(document.argumentNodes.size());
    for (const auto& variable : document.variables) {
        writer.Number(variable.id); writer.Raw(variable.name); writer.Raw(variable.typeName);
        writer.Number(static_cast<std::uint32_t>(variable.isArray)); writer.Number(static_cast<std::uint32_t>(variable.size));
        writer.Number(static_cast<std::uint32_t>(variable.hasDefault)); writer.Raw(variable.value);
    }
    const auto id = state.target.triggerId;
    const auto row = RowOf(document, id);
    const bool present = row != document.triggers.size();
    writer.Number(present);
    if (state.kind == detail::CodeChangeKind::Create) {
        if (state.newCategory) {
            const auto category = std::find_if(document.categories.begin(), document.categories.end(),
                [&](const auto& item) { return item.categoryId == state.newCategory->categoryId; });
            writer.Number(category != document.categories.end());
            if (category != document.categories.end()) {
                writer.Number(static_cast<std::size_t>(category - document.categories.begin()));
                writer.Number(category->id); writer.Raw(category->name); writer.Origin(category->origin);
            }
        }
        if (!present) { writer.Number(document.triggers.empty() ? 0 : document.triggers.back().id); return writer.Finish(); }
    }
    if (!present) return writer.Finish();
    writer.Number(row);
    writer.Number(row > 0 ? document.triggers[row - 1].id : 0);
    writer.Number(row + 1 < document.triggers.size() ? document.triggers[row + 1].id : 0);
    const auto& record = document.triggers[row];
    writer.Record(record);
    std::vector<std::uint32_t> ecas, arguments;
    CollectSubtree(document, record.ecas, ecas, arguments);
    writer.Number(ecas.size()); for (const auto index : ecas) WriteEca(writer, index, document.ecaNodes[index]);
    writer.Number(arguments.size());
    for (const auto index : arguments) WriteArgument(writer, index, document.argumentNodes[index]);
    return writer.Finish();
}

std::string Normalize(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            result.push_back('\n');
        } else result.push_back(text[i]);
    }
    return result;
}
Result<RawText> Edited(const RawText& before, std::string_view input, std::size_t maximum) {
    // 未改的大块只做展示基线比较，不把 2 MiB 编辑预算误用到只读脚本。
    if (input == Normalize(before.text)) return Result<RawText>::Success(before);
    maximum = std::min(maximum, TriggerCodeEditor::kDefaultMaxEditedBytes);
    if (input.size() > maximum || input.size() > TriggerCodeEditor::kDefaultMaxEditedBytes)
        return Result<RawText>::Failure("Trigger.Code", "CODE_EDIT_BUDGET", "编辑代码块超过 2 MiB 字节预算。");
    if (input.find('\0') != std::string_view::npos)
        return Result<RawText>::Failure("Trigger.Code", "CODE_EDIT_NUL", "代码不能包含 NUL。");
    RawText utf8{std::string(input), TextEncoding::Utf8, {}};
    auto checked = utf8.ToBytes();
    if (!checked.Succeeded()) { Result<RawText> failure; failure.diagnostics = checked.diagnostics; return failure; }
    if (input.find('\r') != std::string_view::npos)
        return Result<RawText>::Failure("Trigger.Code", "CODE_EDIT_EOL", "编辑输入须使用 LF 换行。");
    // 保留既有各行的换行顺序；新增行延用首个既有换行规则。
    std::vector<std::string_view> separators;
    for (std::size_t i = 0; i < before.text.size(); ++i) {
        if (before.text[i] == '\r') {
            const auto count = i + 1 < before.text.size() && before.text[i + 1] == '\n' ? 2 : 1;
            separators.emplace_back(before.text.data() + i, count); i += count - 1;
        } else if (before.text[i] == '\n') separators.emplace_back(before.text.data() + i, 1);
    }
    RawText after{{}, before.encoding, {}};
    after.text.reserve(input.size());
    std::size_t line = 0;
    for (const char value : input) {
        if (value != '\n') after.text.push_back(value);
        else {
            after.text.append(line < separators.size() ? separators[line]
                : separators.empty() ? std::string_view("\n") : separators.front());
            ++line;
        }
    }
    auto encoded = after.ToBytes();
    if (!encoded.Succeeded()) { Result<RawText> failure; failure.diagnostics = encoded.diagnostics; return failure; }
    if (after.text.size() > maximum || encoded.value->size() > maximum)
        return Result<RawText>::Failure("Trigger.Code", "CODE_EDIT_BUDGET", "代码原编码或换行展开超过编辑预算。");
    if (after.encoding == TextEncoding::Utf8Bom) {
        if (maximum < 3 || encoded.value->size() > maximum - 3)
            return Result<RawText>::Failure("Trigger.Code", "CODE_EDIT_BUDGET", "代码 BOM 与正文超过编辑预算。");
        after.originalBytes = {std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
        after.originalBytes.insert(after.originalBytes.end(), encoded.value->begin(), encoded.value->end());
    }
    return Result<RawText>::Success(std::move(after));
}
Status SourceCheck(const TriggerDocument& document, const UiCatalog& catalog, const State* state = nullptr) {
    if (!document.HasTrustedSourceSeal() || document.UiCatalogSha256() != catalog.Sha256())
        return Status::Failure("Trigger.Code", "CODE_EDIT_SOURCE", "代码编辑要求当前已验证的触发器来源及同一函数库。");
    if (state && (document.SourceWtgSha256() != state->sourceWtg
        || document.SourceWctSha256() != state->sourceWct || catalog.Sha256() != state->catalog))
        return Status::Failure("Trigger.Code", "CODE_EDIT_STALE_SOURCE", "代码编辑来源或函数库已改变。");
    return Status::Ok();
}
void BindSource(State& state, const TriggerDocument& document, const UiCatalog& catalog) {
    state.sourceWtg = document.SourceWtgSha256(); state.sourceWct = document.SourceWctSha256();
    state.catalog = catalog.Sha256();
}
// 结构转换（新建/转换）的原地正反操作。调用前已核对局部身份，常规失败只剩分配失败。
Status ApplyStructural(TriggerDocument& document, const State& state, bool forward) {
    const auto id = state.target.triggerId;
    const auto stale = [] { return Status::Failure("Trigger.Code", "CODE_EDIT_STALE_GRAPH", "触发器图已改变，未应用代码转换收据。"); };
    if (state.kind == detail::CodeChangeKind::Create) {
        if (forward) {
            if (state.row != document.triggers.size() || (state.newCategory && state.categoryRow > document.categories.size()))
                return stale();
            auto trigger = state.afterTrigger; // 先完成全部复制和容量预留，插入只做 noexcept 移动。
            auto category = state.newCategory;
            document.categories.reserve(document.categories.size() + 1);
            document.triggers.reserve(document.triggers.size() + 1);
            if (category)
                document.categories.insert(document.categories.begin() + static_cast<std::ptrdiff_t>(state.categoryRow), std::move(*category));
            document.triggers.push_back(std::move(trigger));
            return Status::Ok();
        }
        const auto row = RowOf(document, id);
        if (row == document.triggers.size() || !document.triggers[row].ecas.empty()) return stale();
        document.triggers.erase(document.triggers.begin() + static_cast<std::ptrdiff_t>(row));
        return state.newCategory ? TriggerDirectoryEditor::RemoveEmptyCategory(document, state.newCategory->categoryId) : Status::Ok();
    }
    if (state.kind == detail::CodeChangeKind::Recover) {
        // 转回 T 与“转换为代码”方向相反：正向删代码记录、按回执把片段放回节点表末尾；反向删 GUI 子树、放回代码记录。
        if (forward) {
            const auto row = RowOf(document, id);
            if (row != state.row || !document.triggers[row].ecas.empty() || document.triggers[row].customTextFlag != 1) return stale();
            auto code = std::move(document.triggers[row]);
            document.triggers.erase(document.triggers.begin() + static_cast<std::ptrdiff_t>(row));
            auto restored = TriggerDocumentEditor::UndoRemoveTrigger(document, state.restore);
            if (!restored.Succeeded())
                document.triggers.insert(document.triggers.begin() + static_cast<std::ptrdiff_t>(row), std::move(code));
            return restored;
        }
        auto code = state.beforeTrigger;
        auto removed = TriggerDocumentEditor::RemoveTrigger(document, id);
        if (!removed.Succeeded()) return {std::move(removed.diagnostics)};
        document.triggers.insert(document.triggers.begin() + static_cast<std::ptrdiff_t>(removed.value->row), std::move(code));
        return Status::Ok();
    }
    if (forward) {
        auto trigger = state.afterTrigger;
        auto removed = TriggerDocumentEditor::RemoveTrigger(document, id);
        if (!removed.Succeeded()) return {std::move(removed.diagnostics)};
        // 删除后容量不变，同位置插入不重新分配。
        document.triggers.insert(document.triggers.begin() + static_cast<std::ptrdiff_t>(removed.value->row), std::move(trigger));
        state.removed = std::move(*removed.value);
        return Status::Ok();
    }
    const auto row = RowOf(document, id);
    if (row == document.triggers.size() || !document.triggers[row].ecas.empty() || state.removed.trigger.id != id) return stale();
    auto code = std::move(document.triggers[row]);
    document.triggers.erase(document.triggers.begin() + static_cast<std::ptrdiff_t>(row));
    auto restored = TriggerDocumentEditor::UndoRemoveTrigger(document, state.removed);
    if (!restored.Succeeded())
        document.triggers.insert(document.triggers.begin() + static_cast<std::ptrdiff_t>(row), std::move(code));
    return restored;
}
Status ReplayState(TriggerDocument& document, const UiCatalog& catalog, const State& state, bool forward) {
    if (auto status = SourceCheck(document, catalog, &state); !status.Succeeded()) return status;
    const auto& expected = forward ? state.beforeBinding : state.afterBinding;
    if (state.kind == detail::CodeChangeKind::Text) {
        if (TextBinding(document, catalog, state.target) != expected)
            return Status::Failure("Trigger.Code", "CODE_EDIT_STALE_BLOCK", "代码块或所属元数据已改变，未应用收据。");
        if (!state.changed) return Status::Ok();
        auto replacement = forward ? state.afterText : state.beforeText;
        auto* target = TargetText(document, state.target);
        if (!target) return Status::Failure("Trigger.Code", "CODE_EDIT_TARGET", "找不到自定义代码块。");
        std::swap(*target, replacement); // 所有分配及身份核对已完成。
        return Status::Ok();
    }
    // 结构转换：核对局部身份后原地修改，不复制整张触发器图。撤销栈按后进先出重放，
    // 局部身份覆盖集合规模、目标行与相邻行、目标记录及完整子树；整图字节身份由保存门禁负责。
    if (expected.empty()) return Status::Failure("Trigger.Code", "CODE_EDIT_UNSEALED", "代码转换尚未应用，不能撤销。");
    if (LocalGraphBinding(document, catalog, state, !forward) != expected)
        return Status::Failure("Trigger.Code", "CODE_EDIT_STALE_GRAPH", "触发器图已改变，未应用代码转换收据。");
    const bool verify = FullVerification();
    std::string fullCurrent;
    if (verify) {
        fullCurrent = GraphBinding(document, catalog);
        const auto& known = forward ? state.fullBefore : state.fullAfter;
        if (!known.empty() && known != fullCurrent)
            return Status::Failure("Trigger.Code", "CODE_EDIT_STALE_GRAPH", "完整核验：触发器图在撤销栈之外被修改。");
    }
    Status changed = Status::Ok();
    try {
        changed = ApplyStructural(document, state, forward);
    } catch (const std::exception& error) {
        return Status::Failure("Trigger.Code", "CODE_EDIT_INCONSISTENT",
            std::string("代码转换在修改中途分配失败，触发器图可能不完整；请勿保存，重新打开地图。") + error.what());
    }
    if (!changed.Succeeded()) return changed;
    const auto rollback = [&] { try { (void)ApplyStructural(document, state, !forward); } catch (...) {} };
    auto reached = LocalGraphBinding(document, catalog, state, forward);
    if (forward && state.afterBinding.empty()) state.afterBinding = std::move(reached);
    else if (reached != (forward ? state.afterBinding : state.beforeBinding)) {
        rollback();
        return Status::Failure("Trigger.Code", "CODE_EDIT_REPLAY_MISMATCH", "代码转换回读与收据身份不一致。");
    }
    if (verify) {
        auto fullReached = GraphBinding(document, catalog);
        auto& knownBefore = forward ? state.fullBefore : state.fullAfter;
        auto& knownAfter = forward ? state.fullAfter : state.fullBefore;
        if (knownBefore.empty()) knownBefore = std::move(fullCurrent);
        if (knownAfter.empty()) knownAfter = std::move(fullReached);
        else if (knownAfter != fullReached) {
            rollback();
            return Status::Failure("Trigger.Code", "CODE_EDIT_REPLAY_MISMATCH", "完整核验：代码转换回读与首次应用的整图身份不一致。");
        }
    }
    return Status::Ok();
}
}

Result<TriggerGraphReplayIdentity> CaptureTriggerGraphReplayIdentity(
    const TriggerDocument& document, const UiCatalog& catalog) {
    try {
        const auto source = SourceCheck(document, catalog);
        if (!source.Succeeded()) return {{}, source.diagnostics};
        TriggerGraphReplayIdentity identity;
        GraphBindingDetails details;
        identity.binding_ = GraphBinding(document, catalog, &details);
        identity.currentWtg_ = std::move(details.wtgSha256);
        identity.currentWct_ = std::move(details.wctSha256);
        identity.bytesCharged_ = details.bytes;
        identity.encodedBytes_ = details.encodedBytes;
        identity.encodedCapacityBytes_ = details.encodedCapacity;
        identity.scratchBytes_ = details.scratchBytes;
        return Result<TriggerGraphReplayIdentity>::Success(std::move(identity));
    } catch (...) {
        return Result<TriggerGraphReplayIdentity>::Failure("Trigger.Code", "TRIGGER_GRAPH_REPLAY_CAPTURE_FAILED",
            "无法在整图身份预算内捕获完整重放身份，未返回部分身份。");
    }
}

Status VerifyTriggerGraphReplayIdentity(const TriggerDocument& document, const UiCatalog& catalog,
    const TriggerGraphReplayIdentity& identity) {
    if (identity.binding_.empty()) return Status::Failure("Trigger.Code", "TRIGGER_GRAPH_REPLAY_UNBOUND", "没有已捕获的整图重放身份。");
    try {
        if (const auto source = SourceCheck(document, catalog); !source.Succeeded()) return source;
        // Verify 仍实际 Validate、Serialize 并捕获完整 wire/元信息摘要；只省略未消费的两成员独立 SHA。
        // 公共 Capture 和不透明收据仍生成实际配对 SHA，before/wanted 两次核验与回滚原样。
        if (GraphBinding(document, catalog) != identity.binding_)
            return Status::Failure("Trigger.Code", "TRIGGER_GRAPH_REPLAY_STALE", "来源、函数目录、整图字节或稳定身份/编码元信息已变化。");
        return Status::Ok();
    } catch (...) {
        return Status::Failure("Trigger.Code", "TRIGGER_GRAPH_REPLAY_CAPTURE_FAILED",
            "无法在整图身份预算内捕获完整重放身份，未返回部分身份。");
    }
}

bool TriggerCodeEditPlan::Changed() const noexcept { return state_ && state_->changed; }
CodeBlockTarget TriggerCodeEditPlan::Target() const noexcept { return state_ ? state_->target : CodeBlockTarget{}; }
bool TriggerCodeEditReceipt::Changed() const noexcept { return state_ && state_->changed; }
CodeBlockTarget TriggerCodeEditReceipt::Target() const noexcept { return state_ ? state_->target : CodeBlockTarget{}; }
std::string TriggerCodeEditor::NormalizedText(const RawText& text) { return Normalize(text.text); }

Result<TriggerCodeEditPlan> TriggerCodeEditor::PrepareText(const TriggerDocument& document,
    const UiCatalog& catalog, CodeBlockTarget target, std::string_view normalizedUtf8, std::size_t maximum) {
    try {
        if (auto status = SourceCheck(document, catalog); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        const auto* before = TargetText(document, target);
        if (!before) return Result<TriggerCodeEditPlan>::Failure("Trigger.Code", "CODE_EDIT_TARGET", "目标须为全局头部或既有自定义代码触发器。");
        auto edited = Edited(*before, normalizedUtf8, maximum);
        if (!edited.Succeeded()) return {std::nullopt, std::move(edited.diagnostics)};
        auto state = std::make_shared<State>();
        state->target = target; state->changed = normalizedUtf8 != Normalize(before->text);
        BindSource(*state, document, catalog); state->beforeText = *before; state->afterText = std::move(*edited.value);
        state->beforeBinding = TextBinding(document, catalog, target);
        // 正文收据只绑定目标，不复制全图。借助同一完整元数据写入器计算后状态。
        BindingWriter writer; SourceBinding(writer, document, catalog);
        writer.Number(static_cast<unsigned>(target.kind)); writer.Number(target.triggerId);
        if (target.kind == CodeBlockKind::GlobalHeader) {
            writer.Number(document.wct.version); writer.Raw(document.wct.customComment); writer.Raw(state->afterText);
        } else { auto record = *Find(document, target.triggerId); record.customText = state->afterText; writer.Record(record); }
        state->afterBinding = writer.Finish();
        TriggerCodeEditPlan plan; plan.state_ = std::move(state); return Result<TriggerCodeEditPlan>::Success(std::move(plan));
    } catch (const std::exception& error) {
        return Result<TriggerCodeEditPlan>::Failure("Trigger.Code", "CODE_EDIT_PREPARE_FAILED", error.what());
    }
}
Result<TriggerCodeEditPlan> TriggerCodeEditor::PrepareNewCustom(const TriggerDocument& document,
    const UiCatalog& catalog, std::int32_t category, std::string name, std::string_view input, std::size_t maximum,
    std::string newCategoryName) {
    try {
        if (auto status = SourceCheck(document, catalog); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        auto state = std::make_shared<State>(); state->kind = detail::CodeChangeKind::Create; state->changed = true;
        BindSource(*state, document, catalog);
        // 只复制目录、变量与触发器记录来分配名称和身份，不复制十万级 ECA/参数节点；
        // 节点里已用的生成身份用一个占位记录带入，保证新身份与全图不冲突。
        TriggerDocument probe;
        probe.categories = document.categories; probe.variables = document.variables; probe.triggers = document.triggers;
        if (const auto counter = detail::NextGeneratedCounter(document); counter > 1) {
            EcaNode marker; marker.id = detail::kGeneratedDomain | (counter - 1);
            probe.ecaNodes.push_back(std::move(marker));
        }
        if (!newCategoryName.empty() && std::none_of(probe.categories.begin(), probe.categories.end(),
            [category](const auto& record) { return record.categoryId == category; })) {
            auto createdCategory = TriggerDirectoryEditor::AddCategory(probe, std::move(newCategoryName));
            if (!createdCategory.Succeeded()) return {std::nullopt, std::move(createdCategory.diagnostics)};
            category = *createdCategory.value;
            state->categoryRow = probe.categories.size() - 1;
            state->newCategory = probe.categories.back();
        }
        auto created = TriggerDocumentEditor::AddTrigger(probe, category, std::move(name));
        if (!created.Succeeded()) return {std::nullopt, std::move(created.diagnostics)};
        state->target = {CodeBlockKind::CustomTrigger, *created.value}; state->row = probe.triggers.size() - 1;
        auto& record = probe.triggers.back(); record.customTextFlag = 1;
        std::string skeleton;
        if (input.empty()) {
            const auto suffix = TriggerDirectoryEditor::ScriptName(record.name.text);
            skeleton = "function InitTrig_" + suffix + " takes nothing returns nothing\n"
                "    set gg_trg_" + suffix + " = CreateTrigger()\nendfunction\n";
            input = skeleton;
        }
        auto edited = Edited(record.customText, input, maximum);
        if (!edited.Succeeded()) return {std::nullopt, std::move(edited.diagnostics)};
        record.customText = std::move(*edited.value); state->afterTrigger = record;
        state->beforeBinding = LocalGraphBinding(document, catalog, *state, false);
        if (FullVerification()) state->fullBefore = GraphBinding(document, catalog); // 完整核验：计划绑定整张图。
        TriggerCodeEditPlan plan; plan.state_ = std::move(state); return Result<TriggerCodeEditPlan>::Success(std::move(plan));
    } catch (const std::exception& error) {
        return Result<TriggerCodeEditPlan>::Failure("Trigger.Code", "CODE_EDIT_PREPARE_FAILED", error.what());
    }
}
Result<TriggerCodeEditPlan> TriggerCodeEditor::PrepareConversion(const TriggerDocument& document,
    const UiCatalog& catalog, StableId id, std::string_view input, bool runOnInit, std::size_t maximum) {
    try {
        if (auto status = SourceCheck(document, catalog); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        const auto* record = Find(document, id);
        if (!record || record->type != 0 || record->customTextFlag != 0)
            return Result<TriggerCodeEditPlan>::Failure("Trigger.Code", "CODE_EDIT_GUI_REQUIRED", "转换要求一个普通 GUI 触发器。");
        if (input.empty()) return Result<TriggerCodeEditPlan>::Failure("Trigger.Code", "CODE_EDIT_EMPTY_CONVERSION", "转换输出不能为空。");
        auto edited = Edited(record->customText, input, maximum);
        if (!edited.Succeeded()) return {std::nullopt, std::move(edited.diagnostics)};
        auto state = std::make_shared<State>(); state->kind = detail::CodeChangeKind::Convert; state->changed = true;
        state->target = {CodeBlockKind::CustomTrigger, id}; BindSource(*state, document, catalog);
        state->row = RowOf(document, id);
        state->afterTrigger = *record;
        state->afterTrigger.ecas.clear(); state->afterTrigger.customTextFlag = 1;
        state->afterTrigger.customText = std::move(*edited.value);
        state->afterTrigger.runOnInit = runOnInit ? 1 : 0;
        // 删除回执与转换后身份在首次应用时由实际修改生成，不复制整张图预演。
        state->beforeBinding = LocalGraphBinding(document, catalog, *state, false);
        if (FullVerification()) state->fullBefore = GraphBinding(document, catalog); // 完整核验：计划绑定整张图。
        TriggerCodeEditPlan plan; plan.state_ = std::move(state); return Result<TriggerCodeEditPlan>::Success(std::move(plan));
    } catch (const std::exception& error) {
        return Result<TriggerCodeEditPlan>::Failure("Trigger.Code", "CODE_EDIT_PREPARE_FAILED", error.what());
    }
}
Result<GuiTriggerFragment> TriggerCodeEditor::CaptureGuiFragment(const TriggerDocument& document, StableId id) {
    try {
        const auto* record = Find(document, id);
        if (!record || record->type != 0 || record->customTextFlag != 0)
            return Result<GuiTriggerFragment>::Failure("Trigger.Code", "CODE_EDIT_GUI_REQUIRED", "只能读取普通 GUI 触发器的子树。");
        std::vector<std::uint32_t> ecas, arguments;
        CollectSubtree(document, record->ecas, ecas, arguments);
        std::unordered_map<std::uint32_t, std::uint32_t> ecaLocal, argumentLocal;
        for (std::uint32_t i = 0; i < ecas.size(); ++i) ecaLocal.emplace(ecas[i], i);
        for (std::uint32_t i = 0; i < arguments.size(); ++i) argumentLocal.emplace(arguments[i], i);
        GuiTriggerFragment fragment; fragment.runOnInit = record->runOnInit != 0;
        for (const auto root : record->ecas) fragment.roots.push_back(ecaLocal.at(root));
        fragment.ecas.reserve(ecas.size()); fragment.arguments.reserve(arguments.size());
        for (const auto index : ecas) {
            auto node = document.ecaNodes[index];
            for (auto& child : node.children) child = ecaLocal.at(child);
            for (auto& argument : node.arguments) argument = argumentLocal.at(argument);
            fragment.ecas.push_back(std::move(node));
        }
        for (const auto index : arguments) {
            auto argument = document.argumentNodes[index];
            if (argument.callEca) argument.callEca = ecaLocal.at(*argument.callEca);
            if (argument.indexArgument) argument.indexArgument = argumentLocal.at(*argument.indexArgument);
            fragment.arguments.push_back(std::move(argument));
        }
        return Result<GuiTriggerFragment>::Success(std::move(fragment));
    } catch (const std::exception& error) {
        return Result<GuiTriggerFragment>::Failure("Trigger.Code", "CODE_EDIT_CAPTURE_FAILED", error.what());
    }
}
Result<TriggerCodeEditPlan> TriggerCodeEditor::PrepareRecovery(const TriggerDocument& document,
    const UiCatalog& catalog, StableId id, const GuiTriggerFragment& fragment) {
    const auto fail = [](const char* code, std::string message) {
        return Result<TriggerCodeEditPlan>::Failure("Trigger.Code", code, std::move(message));
    };
    try {
        if (auto status = SourceCheck(document, catalog); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        const auto* record = Find(document, id);
        if (!record || record->type != 0 || record->customTextFlag != 1 || !record->ecas.empty())
            return fail("CODE_EDIT_CUSTOM_REQUIRED", "转回 T 要求一个自定义代码触发器。");
        const auto ecaCount = fragment.ecas.size(), argumentCount = fragment.arguments.size();
        if (ecaCount >= kInvalidNodeIndex - document.ecaNodes.size() || argumentCount >= kInvalidNodeIndex - document.argumentNodes.size())
            return fail("CODE_EDIT_NODE_LIMIT", "转回 T 后节点数量超过格式上限。");
        // 片段必须是一棵树：每个节点恰好被引用一次，没有共享、环或孤立节点。
        // code / 条件 / eventcall 参数使用动作 / 条件 / 事件 ECA；普通值才使用 type 3。
        std::vector<char> seenEca(ecaCount, 0), seenArgument(argumentCount, 0);
        std::vector<std::pair<std::uint32_t, int>> pending; // (ECA 局部下标, 参数所需 ECA 类型；-1 为根/子动作)
        for (const auto root : fragment.roots) pending.emplace_back(root, -1);
        while (!pending.empty()) {
            const auto [index, expectedType] = pending.back(); pending.pop_back();
            const bool call = expectedType >= 0;
            if (index >= ecaCount || seenEca[index]) return fail("CODE_EDIT_FRAGMENT", "转回 T 的候选子树引用了不存在或重复的节点。");
            seenEca[index] = 1;
            const auto& eca = fragment.ecas[index];
            const bool root = !call && std::find(fragment.roots.begin(), fragment.roots.end(), index) != fragment.roots.end();
            // 保留原来允许的普通调用（例如 Null / 返回条件的函数），再开放声明匹配的函数槽 ECA。
            if ((call ? eca.type != 3 && eca.type != expectedType : (eca.type < 0 || eca.type > 2)) || ((root || call) && eca.childId))
                return fail("CODE_EDIT_FRAGMENT", "转回 T 的候选节点类型与位置不符：" + eca.name.text);
            const auto* definition = catalog.Find(eca.type, eca.name.text);
            if (!definition) return fail("CODE_EDIT_FRAGMENT", "函数库中没有“" + eca.name.text + "”的定义。");
            for (const auto child : eca.children) pending.emplace_back(child, -1);
            for (std::size_t slot = 0; slot < eca.arguments.size(); ++slot) {
                auto argumentIndex = eca.arguments[slot];
                const auto& slots = definition->Active().arguments;
                if (slot >= slots.size()) return fail("CODE_EDIT_FRAGMENT", "转回 T 的候选参数超出函数库声明。");
                const auto& type = slots[slot].type;
                const auto slotKind = TriggerTypeSystem::FunctionSlotKind(type);
                int required = slotKind ? EcaTypeFromUiDefinitionKind(*slotKind).value_or(3) : 3;
                for (;;) {
                    if (argumentIndex >= argumentCount || seenArgument[argumentIndex]) return fail("CODE_EDIT_FRAGMENT", "转回 T 的候选参数引用无效。");
                    seenArgument[argumentIndex] = 1;
                    const auto& argument = fragment.arguments[argumentIndex];
                    if (argument.callEca) pending.emplace_back(*argument.callEca, required);
                    if (!argument.indexArgument) break;
                    argumentIndex = *argument.indexArgument;
                    required = 3; // 数组下标只能是整数值调用，不能借用所属参数的函数槽。
                }
            }
        }
        if (std::find(seenEca.begin(), seenEca.end(), 0) != seenEca.end() || std::find(seenArgument.begin(), seenArgument.end(), 0) != seenArgument.end())
            return fail("CODE_EDIT_FRAGMENT", "转回 T 的候选含有不属于触发器的孤立节点。");
        const auto baseEca = static_cast<std::uint32_t>(document.ecaNodes.size());
        const auto baseArgument = static_cast<std::uint32_t>(document.argumentNodes.size());
        const auto encoding = detail::NewTextEncoding(document);
        const auto localText = [encoding](RawText& text) { if (text.originalBytes.empty()) text.encoding = encoding; };
        auto counter = detail::NextGeneratedCounter(document);
        auto state = std::make_shared<State>(); state->kind = detail::CodeChangeKind::Recover; state->changed = true;
        state->target = {CodeBlockKind::CustomTrigger, id}; BindSource(*state, document, catalog);
        state->row = RowOf(document, id);
        state->beforeTrigger = *record;
        auto& restore = state->restore;
        restore.row = state->row;
        restore.trigger = *record;
        restore.trigger.customTextFlag = 0; restore.trigger.customText = RawText{{}, record->customText.encoding, {}};
        restore.trigger.runOnInit = fragment.runOnInit ? 1 : 0;
        restore.trigger.ecas.clear();
        for (const auto root : fragment.roots) restore.trigger.ecas.push_back(baseEca + root);
        restore.removedEcas.reserve(ecaCount); restore.removedEcaOldIndices.reserve(ecaCount);
        for (std::uint32_t i = 0; i < ecaCount; ++i) {
            auto node = fragment.ecas[i];
            node.id = detail::AllocateId(counter);
            if (node.id == 0) return fail("TRIGGER_ID_EXHAUSTED", "生成节点 ID 空间已耗尽。");
            node.sourceTriggerId = id; localText(node.name);
            const auto* definition = catalog.Find(node.type, node.name.text);
            node.uiSource = definition->Active().source; node.uiLayer = definition->Active().layer; node.uiSignature = definition->Signature();
            for (auto& child : node.children) child += baseEca;
            for (auto& argument : node.arguments) argument += baseArgument;
            restore.removedEcas.push_back(std::move(node)); restore.removedEcaOldIndices.push_back(baseEca + i);
        }
        restore.removedArguments.reserve(argumentCount); restore.removedArgumentOldIndices.reserve(argumentCount);
        for (std::uint32_t i = 0; i < argumentCount; ++i) {
            auto argument = fragment.arguments[i];
            argument.id = detail::AllocateId(counter);
            if (argument.id == 0) return fail("TRIGGER_ID_EXHAUSTED", "生成节点 ID 空间已耗尽。");
            localText(argument.value);
            if (argument.callEca) argument.callEca = *argument.callEca + baseEca;
            if (argument.indexArgument) argument.indexArgument = *argument.indexArgument + baseArgument;
            restore.removedArguments.push_back(std::move(argument)); restore.removedArgumentOldIndices.push_back(baseArgument + i);
        }
        state->beforeBinding = LocalGraphBinding(document, catalog, *state, false);
        if (FullVerification()) state->fullBefore = GraphBinding(document, catalog);
        TriggerCodeEditPlan plan; plan.state_ = std::move(state); return Result<TriggerCodeEditPlan>::Success(std::move(plan));
    } catch (const std::exception& error) {
        return fail("CODE_EDIT_PREPARE_FAILED", error.what());
    }
}
Result<TriggerDocument> TriggerCodeEditor::RecoveryProbe(const TriggerDocument& document, const UiCatalog& catalog,
    StableId id, const GuiTriggerFragment& fragment) {
    try {
        if (auto status = SourceCheck(document, catalog); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        if (!Find(document, id)) return Result<TriggerDocument>::Failure("Trigger.Code", "CODE_EDIT_TARGET", "找不到要转回 T 的触发器。");
        TriggerDocument probe;
        probe.sourceSeal_ = document.sourceSeal_;
        probe.variablesHeader = document.variablesHeader; probe.wct.version = document.wct.version;
        probe.categories = document.categories; probe.variables = document.variables;
        probe.triggers.reserve(document.triggers.size());
        for (const auto& record : document.triggers) {
            TriggerRecord copy;
            copy.id = record.id; copy.name = record.name; copy.description = record.description; copy.type = record.type;
            copy.enabled = record.enabled; copy.customTextFlag = record.customTextFlag; copy.initiallyClosed = record.initiallyClosed;
            copy.runOnInit = record.runOnInit; copy.categoryId = record.categoryId; copy.origin = record.origin;
            if (record.id == id) {
                copy.customTextFlag = 0; copy.runOnInit = fragment.runOnInit ? 1 : 0; copy.ecas = fragment.roots;
            }
            probe.triggers.push_back(std::move(copy));
        }
        probe.ecaNodes = fragment.ecas; probe.argumentNodes = fragment.arguments;
        // 探测用的临时身份：与原图生成身份不冲突，只为通过共用校验，不进入真实文档。
        auto counter = detail::NextGeneratedCounter(document);
        for (auto& argument : probe.argumentNodes) argument.id = detail::AllocateId(counter);
        for (auto& node : probe.ecaNodes) {
            node.id = detail::AllocateId(counter);
            node.sourceTriggerId = id;
            if (const auto* definition = catalog.Find(node.type, node.name.text)) {
                node.uiSource = definition->Active().source; node.uiLayer = definition->Active().layer; node.uiSignature = definition->Signature();
            }
        }
        return Result<TriggerDocument>::Success(std::move(probe));
    } catch (const std::exception& error) {
        return Result<TriggerDocument>::Failure("Trigger.Code", "CODE_EDIT_PROBE_FAILED", error.what());
    }
}
bool TriggerCodeEditor::FullReplayVerificationEnabled() noexcept { return FullVerification(); }
void TriggerCodeEditor::SetFullReplayVerification(std::optional<bool> enabled) noexcept {
    FullVerificationOverride().store(enabled ? (*enabled ? 1 : 0) : -1, std::memory_order_relaxed);
}
Result<TriggerCodeEditReceipt> TriggerCodeEditor::Apply(TriggerDocument& document,
    const UiCatalog& catalog, const TriggerCodeEditPlan& plan) {
    try {
        if (!plan.state_) return Result<TriggerCodeEditReceipt>::Failure("Trigger.Code", "CODE_EDIT_UNSEALED", "代码计划没有共用服务身份。");
        TriggerCodeEditReceipt receipt; receipt.state_ = plan.state_;
        auto result = Result<TriggerCodeEditReceipt>::Success(std::move(receipt));
        if (auto status = ReplayState(document, catalog, *plan.state_, true); !status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
        return result;
    } catch (const std::exception& error) {
        return Result<TriggerCodeEditReceipt>::Failure("Trigger.Code", "CODE_EDIT_APPLY_FAILED", error.what());
    }
}
Status TriggerCodeEditor::Replay(TriggerDocument& document, const UiCatalog& catalog,
    const TriggerCodeEditReceipt& receipt, bool forward) {
    try {
        if (!receipt.state_) return Status::Failure("Trigger.Code", "CODE_EDIT_UNSEALED", "代码收据没有共用服务身份。");
        return ReplayState(document, catalog, *receipt.state_, forward);
    } catch (const std::exception& error) {
        return Status::Failure("Trigger.Code", "CODE_EDIT_REPLAY_FAILED", error.what());
    }
}
} // namespace War3Map::Trigger
