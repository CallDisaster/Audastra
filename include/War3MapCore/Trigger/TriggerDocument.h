#pragma once

#include "War3MapCore/Codec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Trigger {

constexpr std::int32_t kWtgVersion = 7;
constexpr std::int32_t kWctVersion = 1;
constexpr std::uint32_t kInvalidNodeIndex = 0xffffffffU;

struct RawText {
    std::string text;
    TextEncoding encoding = TextEncoding::Utf8;
    std::vector<std::byte> originalBytes;

    [[nodiscard]] Result<std::vector<std::byte>> ToBytes() const;
};

// 复用 WTG/WCT 的逐段解码：严格 UTF-8、GB18030，最后为保留字节的 Latin-1。
// text 是展示用 UTF-8；originalBytes 和 encoding 用于未修改往返及原编码写回。
[[nodiscard]] RawText DecodeSourceText(std::span<const std::byte> raw);

struct CategoryRecord {
    StableId id = 0;
    std::int32_t categoryId = 0;
    RawText name;
    std::int32_t comment = 0;
    OriginSpan origin;
};

struct VariableRecord {
    StableId id = 0;
    RawText name;
    RawText typeName;
    std::int32_t unknown = 1;
    std::int32_t isArray = 0;
    std::int32_t size = 1;
    std::int32_t hasDefault = 0;
    RawText value;
    OriginSpan origin;
};

struct ArgumentNode {
    StableId id = 0;
    std::int32_t type = 0;
    RawText value;
    std::int32_t insertCall = 0;
    std::optional<std::uint32_t> callEca;
    std::int32_t insertIndex = 0;
    std::optional<std::uint32_t> indexArgument;
    OriginSpan origin;
};

struct EcaNode {
    StableId id = 0;
    StableId sourceTriggerId = 0;
    std::int32_t type = 0;
    std::optional<std::int32_t> childId;
    RawText name;
    std::int32_t enabled = 1;
    std::vector<std::uint32_t> arguments;
    std::vector<std::uint32_t> children;
    std::filesystem::path uiSource;
    std::string uiLayer;
    std::string uiSignature;
    OriginSpan origin;
};

struct TriggerRecord {
    StableId id = 0;
    RawText name;
    RawText description;
    std::int32_t type = 0;
    std::int32_t enabled = 1;
    std::int32_t customTextFlag = 0;
    std::int32_t initiallyClosed = 0;
    std::int32_t runOnInit = 0;
    std::int32_t categoryId = 0;
    std::vector<std::uint32_t> ecas;
    RawText customText;
    OriginSpan origin;
};

struct WctHeader {
    std::int32_t version = kWctVersion;
    RawText customComment;
    RawText customCode;
};

class TriggerDocument final : public CodecDocument {
public:
    [[nodiscard]] std::string FormatName() const override;

    // 只读暴露解析器建立的来源 seal（原始 WTG/WCT 与 UI Catalog 身份）。
    // 普通复制共享同一份 seal；编辑器只能修改文档 IR，不能替换来源记录。
    [[nodiscard]] bool HasTrustedSourceSeal() const noexcept;
    [[nodiscard]] std::string_view SourceWtgSha256() const noexcept;
    [[nodiscard]] std::string_view SourceWctSha256() const noexcept;
    [[nodiscard]] std::string_view UiCatalogSha256() const noexcept;

    std::vector<CategoryRecord> categories;
    std::int32_t variablesHeader = 2;
    std::vector<VariableRecord> variables;
    std::vector<TriggerRecord> triggers;
    std::vector<EcaNode> ecaNodes;
    std::vector<ArgumentNode> argumentNodes;
    WctHeader wct;

private:
    struct SourceSeal;
    std::shared_ptr<const SourceSeal> sourceSeal_;

    friend class WtgWctCodec;
    friend class TriggerCodeEditor; // 只为转回 T 的探测文档复用同一 seal，不能新建或替换来源记录。
};

struct EncodedTriggerDocument {
    std::vector<std::byte> wtg;
    std::vector<std::byte> wct;
};

struct TriggerInspection {
    std::size_t categoryCount = 0;
    std::size_t variableCount = 0;
    std::size_t triggerCount = 0;
    std::size_t ecaCount = 0;
    std::size_t argumentCount = 0;
    std::size_t customTextSlotCount = 0;
    std::size_t eventCount = 0;
    std::size_t conditionCount = 0;
    std::size_t actionCount = 0;
    std::size_t callCount = 0;
};

} // namespace War3Map::Trigger
