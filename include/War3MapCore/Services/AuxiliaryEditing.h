#pragma once

#include "War3MapCore/MapData/MapDataFieldEditing.h"

#include <memory>
#include <variant>

namespace War3Map::Trigger { class TriggerDocument; }

namespace War3Map::Services {

enum class AuxiliaryEditAction { Create, Update, Delete };

// 完整记录更新，身份来自 entities_query 的行键和成员 SHA；新增使用 missing 表示成员缺席。
// 删除不携带记录；区域创建编号在更新中不可改变，新增编号由调用方明确提供。
struct AuxiliaryEditRequest {
    MapDataKind kind = MapDataKind::Regions;
    AuxiliaryEditAction action = AuxiliaryEditAction::Update;
    std::string expectedMemberSha256;
    std::uint64_t sourceIndex = 0;
    std::string expectedRowKey;
    std::variant<std::monostate, MapData::RegionRecord, MapData::CameraRecord> record;
};

struct AuxiliaryReferenceContext {
    const Trigger::TriggerDocument* triggers = nullptr;
    // false 表示未读取/未完整解析 WTG/WCT，不能将空索引解释成没有引用。
    bool completeTriggerSource = false;
    // 外部脚本尚无完整静态引用证明；AI成员仅按名字记录，始终不读取或执行内容。
    std::vector<std::string> opaqueScriptSources;
};

class AuxiliaryEditing;
class PreparedAuxiliaryEdit {
public:
    [[nodiscard]] MapDataKind Kind() const noexcept { return request_.kind; }
    [[nodiscard]] const AuxiliaryEditRequest& Request() const noexcept { return request_; }
    [[nodiscard]] const MapDocument& Before() const noexcept { return *before_; }
    [[nodiscard]] const MapDocument& After() const noexcept { return *after_; }
    [[nodiscard]] std::shared_ptr<const MapDocument> OwnedAfter() const noexcept { return after_; }
    [[nodiscard]] bool HasChanges() const noexcept { return changed_; }
private:
    friend class AuxiliaryEditing;
    PreparedAuxiliaryEdit(AuxiliaryEditRequest request, std::shared_ptr<const MapDocument> before,
        std::shared_ptr<const MapDocument> after, bool changed)
        : request_(std::move(request)), before_(std::move(before)), after_(std::move(after)), changed_(changed) {}
    AuxiliaryEditRequest request_;
    std::shared_ptr<const MapDocument> before_, after_;
    bool changed_ = false;
};

// UI/CLI/MCP 共用；纯准备检查完整结构、符号碰撞、传送门和真实引用，提交沿原事务/撤销链。
// 已有 T 或脚本引用不会被静默改写；引用存在或覆盖不全时只拒绝受影响的删除/更名。
class AuxiliaryEditing {
public:
    // 共用七类地图来源比较：只规范大小写、分隔符和 root/map 别名；逐成员状态、
    // 名称、来源数量/顺序及字节仍须相同，不合并重复来源，也不替代完整解析与封印。
    [[nodiscard]] static bool AreEquivalentMapSources(const MapDocument& left, const MapDocument& right);
    [[nodiscard]] static Result<PreparedAuxiliaryEdit> Prepare(std::shared_ptr<const MapDocument> current,
        AuxiliaryEditRequest request, const AuxiliaryReferenceContext& references);
    // 提交时必须重新捕获当前引用来源；准备结果不能授权覆盖随后新增的 T/脚本引用。
    [[nodiscard]] static Status Apply(MapData::MapDataEditSession& session, const PreparedAuxiliaryEdit& plan,
        const AuxiliaryReferenceContext& references, const MapData::MapDataPublishValidator& validator = {});
    [[nodiscard]] static Result<std::string> MemberIdentity(const MapDocument& document, MapDataKind kind);
};

} // namespace War3Map::Services
