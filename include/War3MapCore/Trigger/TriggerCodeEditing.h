#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Trigger {
namespace detail { struct CodeEditState; }

// 只读、不透明的整图重放身份，复用代码编辑现有 GraphBinding。
// 覆盖源 seal、Catalog、WTG/WCT 字节、稳定 ID、origin 与 RawText 编码元信息。
// 此身份不是代码审计、修改授权或保存证明。
class TriggerGraphReplayIdentity {
public:
    // 只读审查/字节布局回归用；不提供 setter，摘要文本不能构造重放凭据。
    [[nodiscard]] std::string_view Digest() const noexcept { return binding_; }
    [[nodiscard]] std::string_view CurrentWtgSha256() const noexcept { return currentWtg_; }
    [[nodiscard]] std::string_view CurrentWctSha256() const noexcept { return currentWct_; }
    [[nodiscard]] std::size_t BytesCharged() const noexcept { return bytesCharged_; }
    [[nodiscard]] std::size_t EncodedBytes() const noexcept { return encodedBytes_; }
    [[nodiscard]] std::size_t EncodedCapacityBytes() const noexcept { return encodedCapacityBytes_; }
    // 身份生成器拥有的流式状态及最大路径临时量；不含另列的 codec 字节或 OS 内部量。
    [[nodiscard]] std::size_t ScratchBytes() const noexcept { return scratchBytes_; }
private:
    std::string binding_;
    std::string currentWtg_, currentWct_;
    std::size_t bytesCharged_ = 0, encodedBytes_ = 0, encodedCapacityBytes_ = 0, scratchBytes_ = 0;
    friend Result<TriggerGraphReplayIdentity> CaptureTriggerGraphReplayIdentity(
        const TriggerDocument&, const UiCatalog&);
    friend Status VerifyTriggerGraphReplayIdentity(
        const TriggerDocument&, const UiCatalog&, const TriggerGraphReplayIdentity&);
};
[[nodiscard]] Result<TriggerGraphReplayIdentity> CaptureTriggerGraphReplayIdentity(
    const TriggerDocument& document, const UiCatalog& catalog);
[[nodiscard]] Status VerifyTriggerGraphReplayIdentity(
    const TriggerDocument& document, const UiCatalog& catalog, const TriggerGraphReplayIdentity& identity);

enum class CodeBlockKind : std::uint8_t { GlobalHeader, CustomTrigger };
struct CodeBlockTarget {
    CodeBlockKind kind = CodeBlockKind::GlobalHeader;
    StableId triggerId = 0;
};

// 一个 GUI 触发器的完整 ECA 子树：roots 与节点内的 children / arguments / callEca / indexArgument
// 都是片段内的局部下标。用于“代码转回 T”：转换前快照或按生成写法解析出的候选。
struct GuiTriggerFragment {
    std::vector<std::uint32_t> roots;
    std::vector<EcaNode> ecas;
    std::vector<ArgumentNode> arguments;
    bool runOnInit = false;
};

// 计划与收据不暴露可改写的正文或图身份；空对象不能用作放行凭据。
class TriggerCodeEditPlan {
public:
    [[nodiscard]] bool Changed() const noexcept;
    [[nodiscard]] CodeBlockTarget Target() const noexcept;
private:
    std::shared_ptr<const detail::CodeEditState> state_;
    friend class TriggerCodeEditor;
};
class TriggerCodeEditReceipt {
public:
    [[nodiscard]] bool Changed() const noexcept;
    [[nodiscard]] CodeBlockTarget Target() const noexcept;
private:
    std::shared_ptr<const detail::CodeEditState> state_;
    friend class TriggerCodeEditor;
};

class TriggerCodeEditor {
public:
    static constexpr std::size_t kDefaultMaxEditedBytes = 2 * 1024 * 1024;
    // 展示基线为 UTF-8 / LF；未编辑时保留原 encoding、originalBytes 和全部 EOL。
    [[nodiscard]] static std::string NormalizedText(const RawText& text);
    [[nodiscard]] static Result<TriggerCodeEditPlan> PrepareText(
        const TriggerDocument& document, const UiCatalog& catalog, CodeBlockTarget target,
        std::string_view normalizedUtf8, std::size_t maxEditedBytes = kDefaultMaxEditedBytes);
    // 空正文创建合法的 InitTrig_ 骨架；GUI 转换正文由生产 ScriptBuild 生成器提供。
    [[nodiscard]] static Result<TriggerCodeEditPlan> PrepareNewCustom(
        const TriggerDocument& document, const UiCatalog& catalog, std::int32_t categoryId,
        std::string name, std::string_view normalizedUtf8 = {},
        std::size_t maxEditedBytes = kDefaultMaxEditedBytes, std::string newCategoryName = {});
    [[nodiscard]] static Result<TriggerCodeEditPlan> PrepareConversion(
        const TriggerDocument& document, const UiCatalog& catalog, StableId triggerId,
        std::string_view normalizedUtf8, bool effectiveRunOnInit,
        std::size_t maxEditedBytes = kDefaultMaxEditedBytes);
    // 读取 GUI 触发器的完整子树（只读）；转换前保存，供未改动的代码精确转回。
    [[nodiscard]] static Result<GuiTriggerFragment> CaptureGuiFragment(const TriggerDocument& document, StableId triggerId);
    // 代码转回 T：把自定义代码触发器替换为片段（名称、说明、开关与分类保持）。这里只做结构检查、
    // 分配新稳定身份并封装可撤销计划；片段与当前正文是否一致由调用方用生产生成器回生成核对。
    [[nodiscard]] static Result<TriggerCodeEditPlan> PrepareRecovery(
        const TriggerDocument& document, const UiCatalog& catalog, StableId triggerId,
        const GuiTriggerFragment& fragment);
    // 转回 T 的探测文档：同一来源 seal、目录、变量和全部触发器记录（不带节点与正文），目标触发器换成片段。
    // 用来在不复制整张节点表的情况下，对候选运行共用校验与生产生成器；不能用于保存或发布。
    [[nodiscard]] static Result<TriggerDocument> RecoveryProbe(const TriggerDocument& document, const UiCatalog& catalog,
        StableId triggerId, const GuiTriggerFragment& fragment);
    [[nodiscard]] static Result<TriggerCodeEditReceipt> Apply(
        TriggerDocument& document, const UiCatalog& catalog, const TriggerCodeEditPlan& plan);
    [[nodiscard]] static Status Replay(
        TriggerDocument& document, const UiCatalog& catalog,
        const TriggerCodeEditReceipt& receipt, bool forward);
    // 新建/转换的撤销重做默认只核对目标区域的局部身份。完整核验模式另外在每次重放前后
    // 比对整图身份，用来在测试里发现绕过撤销栈的修改：Debug 构建或环境变量
    // AUDASTRA_TRIGGER_FULL_REPLAY_VERIFY=1 时开启；传 nullopt 恢复默认。
    static void SetFullReplayVerification(std::optional<bool> enabled) noexcept;
    [[nodiscard]] static bool FullReplayVerificationEnabled() noexcept;
};

} // namespace War3Map::Trigger
