#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <span>
#include <variant>

namespace War3Map::Trigger {

inline constexpr std::size_t kMaxTriggerEditOperations = 256;

// 挂载身份不用会在前次删除后移动的行号。parentEcaId=0 表示 trigger 根；
// 非零表示父 ECA 的 children。argument.callEca 不是同一种槽，明确拒绝。
struct TriggerEditEcaTarget {
    StableId triggerId = 0;
    StableId ecaId = 0;
    StableId parentEcaId = 0;
    std::int32_t expectedKind = 0;
    std::string expectedFunction;
    std::int32_t expectedEnabled = 1;
    std::optional<std::int32_t> expectedChildId;
    std::size_t expectedArgumentCount = 0;
    std::size_t expectedChildCount = 0;
    std::string expectedSourceSpanSha256;
};

struct TriggerRemoveEcaEdit { TriggerEditEcaTarget target; };
struct TriggerSetEcaEnabledEdit { TriggerEditEcaTarget target; bool enabled = true; };
struct TriggerSetArgumentEdit {
    StableId triggerId = 0, ownerEcaId = 0, argumentId = 0;
    std::int32_t expectedValueKind = 3;
    std::string expectedValue, expectedType;
    std::int32_t valueKind = 3;
    std::string value;
};
struct TriggerEditTriggerTarget {
    StableId triggerId = 0;
    std::vector<StableId> expectedRootIds;
    std::string expectedSourceSpanSha256;
};
struct TriggerAppendRootEdit {
    TriggerEditTriggerTarget target;
    UiDefinitionKind kind = UiDefinitionKind::Action;
    std::string function;
};
struct TriggerTextExpectation {
    TextEncoding encoding = TextEncoding::Utf8;
    std::string expectedUtf8Sha256, expectedEncodedSha256;
};
struct TriggerSetGlobalCodeEdit { TriggerTextExpectation expected; std::string text; };
struct TriggerSetCustomTextEdit { StableId triggerId = 0; TriggerTextExpectation expected; std::string text; };
struct TriggerConvertCustomTextEdit { TriggerEditTriggerTarget target; TriggerTextExpectation expected; std::string text; };
using TriggerEditOperation = std::variant<TriggerRemoveEcaEdit, TriggerSetEcaEnabledEdit, TriggerSetArgumentEdit,
    TriggerAppendRootEdit, TriggerSetGlobalCodeEdit, TriggerSetCustomTextEdit, TriggerConvertCustomTextEdit>;

struct TriggerEditBatch {
    std::string expectedWtgSha256;
    std::string expectedWctSha256;
    std::string expectedCatalogSha256;
    std::vector<TriggerEditOperation> operations;
};

struct TriggerEditBatchReceipt {
    std::size_t operationCount = 0;
    std::size_t removedEcaCount = 0;
    std::size_t removedArgumentCount = 0;
    std::size_t enabledChangeCount = 0;
    std::size_t argumentChangeCount = 0, appendedRootCount = 0, textChangeCount = 0, convertedGuiCount = 0;
};

// 仅校验请求形状/预算，不授予来源或写出许可。
[[nodiscard]] Status ValidateTriggerEditBatch(const TriggerEditBatch& batch);

// 一次复制候选，全批成功才替换 document。原始来源 span 和 seal 同时核对，
// 每次执行前还比较当前子树，防止前一操作改变/删除旧请求的目标。
// 原文件、来源 seal 和 WCT 未改槽位不被重建或改编码；保存仍由共用服务完成。
[[nodiscard]] Result<TriggerEditBatchReceipt> ApplyTriggerEditBatch(
    TriggerDocument& document, const UiCatalog& catalog,
    std::span<const std::byte> originalWtg, const TriggerEditBatch& batch);

[[nodiscard]] std::string_view TriggerEditEncodingName(TextEncoding encoding) noexcept;
[[nodiscard]] std::optional<TextEncoding> ParseTriggerEditEncoding(std::string_view name) noexcept;

enum class TriggerTextSlot { GlobalCode, TriggerCode };
struct TriggerTextQuery {
    TriggerTextSlot slot = TriggerTextSlot::GlobalCode;
    StableId triggerId = 0; // GlobalCode 要求 0；TriggerCode 要求实际稳定 ID。
    std::size_t offset = 0, limit = 65536; // UTF-8 字节分页，起点必须位于码点边界。
};
struct TriggerTextPage {
    TriggerTextSlot slot = TriggerTextSlot::GlobalCode;
    StableId triggerId = 0;
    std::string wtgSha256, wctSha256, catalogSha256;
    TextEncoding encoding = TextEncoding::Utf8;
    std::string utf8Sha256, encodedSha256, text;
    std::size_t totalBytes = 0, offset = 0, nextOffset = 0;
    bool customText = false;
};
// 已配对且有可信 seal 的文档，保留原编码字节 SHA 与 UTF-8 文本 SHA 的区别。
[[nodiscard]] Result<TriggerTextPage> QueryTriggerText(const TriggerDocument& document,
    const UiCatalog& catalog, const TriggerTextQuery& query);

} // namespace War3Map::Trigger
