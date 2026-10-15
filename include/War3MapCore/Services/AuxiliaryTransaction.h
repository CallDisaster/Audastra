#pragma once

#include "War3MapCore/EditorSession.h"
#include "War3MapCore/Services/AuxiliaryEditing.h"

#include <span>

namespace War3Map::Services {

// 仅由共用编辑命令重放密封；EditorSession 只允许该封印新增缺席的 W3R/W3C。
class AuxiliaryTransactionSnapshot {
public:
    [[nodiscard]] static Result<std::shared_ptr<const AuxiliaryTransactionSnapshot>> Freeze(
        const MapDocument& original, const MapDocument& current,
        std::span<const AuxiliaryEditRequest> commands, const AuxiliaryReferenceContext& references);
    [[nodiscard]] Status VerifyAgainst(const MapDocument& original,
        const AuxiliaryReferenceContext& references) const;
    [[nodiscard]] Result<AuxiliaryPatch> Seal(const WorkspaceSnapshot& source) const;
    [[nodiscard]] const MapDocument& Original() const noexcept { return *original_; }
    [[nodiscard]] const MapDocument& Document() const noexcept { return *document_; }
    [[nodiscard]] std::span<const AuxiliaryEditRequest> Commands() const noexcept { return commands_; }
    // 包括显式恢复原值的已触及成员，用于精确核对原 overlay。
    [[nodiscard]] std::span<const MapArchiveMember> Members() const noexcept { return members_; }
    [[nodiscard]] bool HasChanges() const noexcept { return changed_; }
private:
    AuxiliaryTransactionSnapshot() = default;
    std::shared_ptr<const MapDocument> original_, document_;
    std::vector<AuxiliaryEditRequest> commands_;
    std::vector<MapArchiveMember> members_;
    std::string sourceWtgSha256_, sourceWctSha256_, catalogSha256_;
    bool changed_ = false;
};

// UI/独立宿主与保存链共用脚本覆盖分类，不把动态脚本/匿名来源解释为无引用。
void ObserveAuxiliaryScriptSource(AuxiliaryReferenceContext& context, std::string name);

// expectedMembers 只冻结本次实际编辑的 W3R/W3C；模板合并后仍须唯一、未删除且字节相同。
// 未冻结的成员不受此门限制，不能把正常模板资源导入一并禁止。
[[nodiscard]] Status VerifyAuxiliarySaveOutput(std::span<const MapArchiveMember> expectedMembers,
    std::span<const MapArchiveMember> finalMembers, std::span<const std::string> removals);

} // namespace War3Map::Services
