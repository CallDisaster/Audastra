#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapFields.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace War3Map::MapData {
namespace detail { struct MapDataFieldEditingAccess; }

// 原来源绑定的累计字段差异；宿主转换为现有 MapEditFieldOperation 后沿用原保存链。
struct OriginalMapFieldOperation {
    std::string expectedMemberSha256;
    MapFieldEdit edit;
};

// 只能由纯冻结器创建，不接受外部原生成员字节或任意成员替换。
class MapDataFieldFreeze {
public:
    [[nodiscard]] std::span<const OriginalMapFieldOperation> Operations() const noexcept { return operations_; }
    [[nodiscard]] std::span<const MapArchiveMember> Members() const noexcept { return members_; }
private:
    friend struct detail::MapDataFieldEditingAccess;
    MapDataFieldFreeze(std::vector<OriginalMapFieldOperation> operations, std::vector<MapArchiveMember> members)
        : operations_(std::move(operations)), members_(std::move(members)) {}
    std::vector<OriginalMapFieldOperation> operations_;
    std::vector<MapArchiveMember> members_;
};

// 不可变、自有候选。私有构造保证 serialized 由共用字段规则和真实 native writer 产生。
class PreparedMapDataFields {
public:
    [[nodiscard]] MapDataKind Kind() const noexcept { return kind_; }
    [[nodiscard]] const MapDocument& Original() const noexcept { return *original_; }
    [[nodiscard]] const MapDocument& Before() const noexcept { return *before_; }
    [[nodiscard]] const MapDocument& After() const noexcept { return *after_; }
    [[nodiscard]] std::shared_ptr<const MapDocument> OwnedBefore() const noexcept { return before_; }
    [[nodiscard]] std::shared_ptr<const MapDocument> OwnedAfter() const noexcept { return after_; }
    [[nodiscard]] std::span<const MapFieldEdit> Edits() const noexcept { return edits_; }
    [[nodiscard]] std::span<const std::byte> BeforeMemberBytes() const noexcept;
    [[nodiscard]] std::span<const std::byte> AfterMemberBytes() const noexcept;
    [[nodiscard]] const std::string& OriginalMemberSha256() const noexcept { return originalSha256_; }
    [[nodiscard]] const std::string& BeforeMemberSha256() const noexcept { return beforeSha256_; }
    [[nodiscard]] const std::string& AfterMemberSha256() const noexcept { return afterSha256_; }
private:
    friend struct detail::MapDataFieldEditingAccess;
    PreparedMapDataFields(MapDataKind kind, std::shared_ptr<const MapDocument> original,
        std::shared_ptr<const MapDocument> before, std::shared_ptr<const MapDocument> after,
        std::vector<MapFieldEdit> edits, std::string originalSha256,
        std::string beforeSha256, std::string afterSha256)
        : kind_(kind), original_(std::move(original)), before_(std::move(before)), after_(std::move(after)),
          edits_(std::move(edits)), originalSha256_(std::move(originalSha256)),
          beforeSha256_(std::move(beforeSha256)), afterSha256_(std::move(afterSha256)) {}
    MapDataKind kind_;
    std::shared_ptr<const MapDocument> original_, before_, after_;
    std::vector<MapFieldEdit> edits_;
    std::string originalSha256_, beforeSha256_, afterSha256_;
};

// 只在原会话预分配完成后、命令身份/历史/字节/修订发布前调用；不得重入修改该会话。
using MapDataPublishValidator = std::function<Status(const MapDocument& before,
    const MapDocument& after, const MapDataEditSession::EditReceipt& receipt)>;

// 完整原文档加当前 typed 覆盖；未修改来源保留 Invalid/Ambiguous/Unreadable 及 W3I/WTS。
[[nodiscard]] Result<MapDocument> ProjectMapData(const MapDocument& original, const MapDataEditSession& current);
// 只读拥有的 native 替换投影，复用同一来源状态保留规则；不创建临时会话或命令回执。
[[nodiscard]] Result<MapDocument> ProjectMapData(const MapDocument& original,
    std::span<const MapArchiveMember> replacements);
// 唯一已有 classic W3S v1、完整原字节往返及 raw/gg_snd_ 名称资格；所有入口共用。
[[nodiscard]] Status ValidateSoundFieldSource(const MapDocument& document);
// 只允许完整经典 settings、已有 strings/units/regions/cameras/sounds 的目录可写字段；
// 同一批次同类且最多 32 项。未知/Partial 来源不能由字段候选重建。
[[nodiscard]] Result<PreparedMapDataFields> PrepareMapDataFields(const MapDocument& original,
    const MapDocument& current, std::string_view expectedCurrentMemberSha256, std::span<const MapFieldEdit> edits);
// 独立字段事务累计原来源差异最多 32 项；重放所得完整 native bytes 必须与 current 相等。
// GUI 原完整保存不要求调用此冻结器，不能据此拒绝作者先前的其他领域编辑。
// 恢复所有原值时返回空操作，保存适配器须显式报告无字段变化。
[[nodiscard]] Result<MapDataFieldFreeze> FreezeOriginalMapDataFields(const MapDocument& original,
    const MapDocument& current);

} // namespace War3Map::MapData
