#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <array>

namespace War3Map::Trigger {

struct WtgSourceLimits {
    // 计入来源、记录、字符串和节点载荷，不含分配器管理开销。
    // 默认节点预算可显式配置到hardMaxNodes；其他来源/解码/深度预算仍只能收紧。
    static constexpr std::size_t hardMaxNodes = 1000000;
    std::size_t maxBytes = 64 * 1024 * 1024;
    std::size_t maxDecodedBytes = 256 * 1024 * 1024;
    std::size_t maxItems = 100000;
    std::size_t maxNodes = 250000; // ECA与参数合计；不是地图实例数量限制。
    std::size_t maxStringBytes = 1024 * 1024;
    std::size_t maxDepth = 128;
};

struct WtgSourceCountGroup {
    std::uint32_t type = 0;
    std::int32_t declaredCount = 0; // 不猜它与存活/删除/内存占位之间的关系
    std::vector<std::int32_t> deletedIds;
    OriginSpan origin;
};

struct WtgSourceItem {
    std::uint32_t type = 0; // 原始类型 1/4/8/16/32/64；不是 recordIndex 的命名空间
    StableId sourceId = 0; // 来源偏移身份，不能把可重复的存盘 id 当作对象唯一身份
    std::optional<std::int32_t> storedId;
    std::optional<std::int32_t> parentId;
    std::optional<std::int32_t> expandedRaw;
    std::uint32_t recordIndex = 0; // 对应 categories/triggers/variableItems 的下标
    OriginSpan origin;
};

struct WtgSourceVariableIdentity {
    std::optional<std::int32_t> storedId;
    std::optional<std::int32_t> parentId;
};

struct WtgSourceSnapshot {
    std::int32_t baseVersion = 0;
    std::optional<std::uint32_t> subVersionMarker;
    std::string sourceSha256;
    std::string catalogSha256;
    std::array<WtgSourceCountGroup, 8> counts;
    std::vector<WtgSourceItem> items;
    std::vector<WtgSourceVariableIdentity> variableIdentities;
    std::vector<RawText> variableItems; // 树节点的原名，不用名称猜它对应哪个变量定义
    // 共用 ECA/参数表示与读取器；无 WCT 配对、无可信来源 seal，经典编辑/序列化会拒绝。
    // v4 不存在的 comment/arraySize 字段须按 baseVersion 判断，不能把结构默认值当存盘值。
    TriggerDocument document;
};

class WtgSource {
public:
    [[nodiscard]] std::shared_ptr<const WtgSourceSnapshot> Snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] std::span<const std::byte> OriginalBytes() const noexcept {
        return bytes_ ? std::span<const std::byte>(*bytes_) : std::span<const std::byte>{};
    }
private:
    std::shared_ptr<const std::vector<std::byte>> bytes_;
    std::shared_ptr<const WtgSourceSnapshot> snapshot_;
    friend Result<WtgSource> ParseWtgSource(std::span<const std::byte>, const UiCatalog&, const WtgSourceLimits&);
};

// 按基础版本 4/7 与扩展标记 80000001/80000004 读取完整成员。
// 未知类型没有可跳过长度，未知函数没有参数数量，两者均拒绝；不返回部分树。
[[nodiscard]] Result<WtgSource> ParseWtgSource(std::span<const std::byte> bytes,
    const UiCatalog& catalog, const WtgSourceLimits& limits = {});

enum class WtgSourceTable { Items, Variables, DeletedIds, Ecas, Arguments };
struct WtgSourceField {
    std::string name;
    std::string encoding; // i32/u64/bytes-hex/u32-list；不在适用版本的字段不输出
    std::string value;
};
struct WtgSourceRow {
    std::size_t index = 0;
    StableId sourceId = 0;
    OriginSpan origin;
    std::vector<WtgSourceField> fields;
};
struct WtgSourcePage {
    std::string sourceSha256, catalogSha256;
    std::size_t totalRows = 0;
    std::vector<WtgSourceRow> rows;
};

// UI/CLI/Agent 共用：双 SHA 绑定，最多 128 行，结果字符载荷最多 1 MiB。
[[nodiscard]] Result<WtgSourcePage> QueryWtgSource(const WtgSource& source,
    std::string_view expectedSourceSha256, std::string_view expectedCatalogSha256,
    WtgSourceTable table, std::size_t start, std::size_t limit);

} // namespace War3Map::Trigger
