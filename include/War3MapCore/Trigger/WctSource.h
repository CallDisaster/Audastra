#pragma once

#include "War3MapCore/Types.h"
#include <memory>
#include <span>

namespace War3Map::Trigger {

struct WctSourceLimits {
    std::size_t maxBytes = 64 * 1024 * 1024;
    std::size_t maxBlocks = 100000;
    std::size_t maxBlockBytes = 16 * 1024 * 1024;
    std::size_t maxCommentBytes = 1024 * 1024;
};

struct WctSourceRange {
    std::size_t offset = 0;
    std::size_t length = 0;
};

struct WctSourceBlock {
    std::size_t lengthFieldOffset = 0;
    WctSourceRange payload; // 声明的全部字节；包括内部及尾部 NUL，不按字符计长。
    bool containsNul = false;
    bool endsWithNul = false;
};

struct WctSourceHeader {
    std::uint32_t version = 0;
    std::optional<std::uint32_t> subversionMarker; // 原始 0x80000001 / 0x80000004。
    std::optional<WctSourceRange> comment; // 原始 Z 字段，含末尾 NUL。
    std::optional<WctSourceBlock> globalCode;
    std::optional<std::uint32_t> declaredBlockCount; // 次版本 04 不存在此字段。
};

// 独立、只读的长度块来源目录。不推断语言，不把槽号当成 WTG 触发器身份。
// 与经典可编辑 WtgWctCodec 分离；不会让扩展 WCT 自动获得编辑/保存能力。
class WctSourceDocument {
public:
    [[nodiscard]] const WctSourceHeader& Header() const noexcept;
    [[nodiscard]] const std::string& SourceSha256() const noexcept;
    [[nodiscard]] std::span<const WctSourceBlock> Blocks() const noexcept;
    [[nodiscard]] std::span<const std::byte> OriginalBytes() const noexcept;
private:
    struct Storage;
    explicit WctSourceDocument(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const Storage> storage_;
    friend Result<WctSourceDocument> ReadWctSource(std::span<const std::byte>, const WctSourceLimits&);
};

[[nodiscard]] Result<WctSourceDocument> ReadWctSource(std::span<const std::byte> bytes,
    const WctSourceLimits& limits = {});

struct WctSourcePage {
    std::string sourceSha256;
    std::size_t startIndex = 0; // 只在同一 sourceSha256 内稳定，不是触发器 ID。
    std::vector<WctSourceBlock> blocks;
    std::optional<std::size_t> nextIndex;
};
[[nodiscard]] Result<WctSourcePage> QueryWctSourceBlocks(const WctSourceDocument& document,
    std::string_view expectedSourceSha256, std::size_t startIndex, std::size_t limit = 128);

struct WctSourceChunk {
    std::string sourceSha256;
    std::optional<std::size_t> blockIndex; // nullopt 表示 globalCode，v0 不存在。
    std::size_t payloadOffset = 0;
    std::vector<std::byte> bytes;
    std::optional<std::size_t> nextOffset;
};
[[nodiscard]] Result<WctSourceChunk> ReadWctSourceChunk(const WctSourceDocument& document,
    std::string_view expectedSourceSha256, std::optional<std::size_t> blockIndex,
    std::size_t payloadOffset, std::size_t limit = 65536);

} // namespace War3Map::Trigger
