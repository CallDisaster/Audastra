#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <memory>
#include <span>

namespace War3Map::Trigger {

// 展开工作区修订与整图 SHA 属于不同编号空间，不按相等关系猜测它们的来源。
struct TriggerTransactionBinding {
    std::string sourceSha256;
    TransactionId transactionId = 0;
    ProjectRevision baseRevision;
    std::uint64_t editGeneration = 0;
};

struct TriggerTransactionLimits {
    std::size_t maxMemberBytes = 64U * 1024U * 1024U;
    std::size_t maxRecords = 250000;
    std::size_t maxTextBytes = 128U * 1024U * 1024U;
};

struct TriggerTransactionReceipt {
    TriggerTransactionBinding binding;
    std::string originalWtgSha256, originalWctSha256, catalogSha256;
    std::string savedWtgSha256, savedWctSha256;
    std::vector<std::string> effectiveLayers;
    std::size_t variableCount = 0, triggerCount = 0, ecaCount = 0, argumentCount = 0;
};

// 只能通过可信来源 seal、实际原配对字节和同一 Catalog 构造。客户端不能提供
// 任意候选成员字节；冻结过程自行验证并序列化完整 GUI 图，保留已有自定义代码。
// 本对象不授予事务 lease；宿主仍须在发布前复核活动 revision/generation。
class TriggerTransactionSnapshot final {
public:
    [[nodiscard]] static Result<std::shared_ptr<const TriggerTransactionSnapshot>> Freeze(
        const TriggerDocument& document, const UiCatalog& catalog,
        std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
        TriggerTransactionBinding binding, const TriggerTransactionLimits& limits = {});

    // 仅为同一保存事务中的物编改动提供原样 WTG/WCT 配对。来源、Catalog、
    // 原代码、预算和回读门禁与 Freeze 相同，但候选必须逐字节等于原配对。
    // 此封印本身不是触发器改动；保存入口须拒绝没有物编候选的 companion。
    [[nodiscard]] static Result<std::shared_ptr<const TriggerTransactionSnapshot>> FreezeSourceCompanion(
        const TriggerDocument& document, const UiCatalog& catalog,
        std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
        TriggerTransactionBinding binding, const TriggerTransactionLimits& limits = {});

    [[nodiscard]] const TriggerTransactionBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] const TriggerDocument& Document() const noexcept { return document_; }
    [[nodiscard]] const UiCatalog& Catalog() const noexcept { return catalog_; }
    [[nodiscard]] const EncodedTriggerDocument& Encoded() const noexcept { return encoded_; }
    [[nodiscard]] std::span<const std::byte> OriginalWtg() const noexcept { return originalWtg_; }
    [[nodiscard]] std::span<const std::byte> OriginalWct() const noexcept { return originalWct_; }
    [[nodiscard]] const std::vector<std::string>& EffectiveLayers() const noexcept { return catalog_.Layers(); }
    [[nodiscard]] const TriggerTransactionReceipt& Receipt() const noexcept { return receipt_; }
    [[nodiscard]] bool IsSourceCompanion() const noexcept { return sourceCompanion_; }

    // 保存入口必须用刚从冻结源归档/有效 UI 层序读取的事实再次核对，不能只信请求标记。
    [[nodiscard]] Status VerifyAgainst(std::string_view sourceSha256,
        std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
        const UiCatalog& catalog) const;

    TriggerTransactionSnapshot(const TriggerTransactionSnapshot&) = delete;
    TriggerTransactionSnapshot& operator=(const TriggerTransactionSnapshot&) = delete;

private:
    TriggerTransactionSnapshot(TriggerDocument document, UiCatalog catalog,
        std::vector<std::byte> originalWtg, std::vector<std::byte> originalWct,
        EncodedTriggerDocument encoded, TriggerTransactionReceipt receipt,
        TriggerTransactionLimits limits, bool sourceCompanion);

    [[nodiscard]] static Result<std::shared_ptr<const TriggerTransactionSnapshot>> FreezeImpl(
        const TriggerDocument& document, const UiCatalog& catalog,
        std::span<const std::byte> originalWtg, std::span<const std::byte> originalWct,
        TriggerTransactionBinding binding, const TriggerTransactionLimits& limits, bool sourceCompanion);

    TriggerTransactionBinding binding_;
    TriggerDocument document_;
    UiCatalog catalog_;
    std::vector<std::byte> originalWtg_, originalWct_;
    EncodedTriggerDocument encoded_;
    TriggerTransactionReceipt receipt_;
    TriggerTransactionLimits limits_;
    bool sourceCompanion_ = false;
};

} // namespace War3Map::Trigger
