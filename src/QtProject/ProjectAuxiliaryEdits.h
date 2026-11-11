#pragma once

#include "War3MapCore/Services/AuxiliaryEditing.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include <QString>
#include <filesystem>
#include <span>
#include <vector>

namespace War3Map::QtProject {
class EditorProject;
// 对话框/Agent 请求绑定实际编辑会话；相同内容的另一地图不能复用旧请求。
struct AuxiliaryEditBinding {
    std::filesystem::path projectRoot, sourceArchive;
    std::uint64_t sessionIdentity = 0, generation = 0, revision = 0;
};
struct AuxiliaryEditResult {
    AuxiliaryEditBinding binding;
    bool changed = false;
    std::string memberSha256, rowKey;
    std::variant<std::monostate, MapData::RegionId, MapData::CameraId> object;
    MapData::MapDataEditSession::EditReceipt receipt;
};
[[nodiscard]] Result<AuxiliaryEditBinding> CaptureAuxiliaryEditBinding(const EditorProject& project);
[[nodiscard]] bool AuxiliaryEditBindingCurrent(const EditorProject& project, const AuxiliaryEditBinding& expected);
[[nodiscard]] Services::AuxiliaryReferenceContext CaptureAuxiliaryReferences(EditorProject& project);
[[nodiscard]] Result<AuxiliaryEditResult> ApplyAuxiliaryEditWithResult(EditorProject& project,
    const AuxiliaryEditBinding& expected, Services::AuxiliaryEditRequest request, const QString& label,
    const MapData::MapDataPublishValidator& validator = {});
[[nodiscard]] Status ApplyAuxiliaryEdit(EditorProject& project, Services::AuxiliaryEditRequest request,
    const QString& label);
// 一次删除多条区域/镜头：同一会话事务、一次撤销；任一行身份或引用不符整批不改。
[[nodiscard]] Status ApplyAuxiliaryDeletion(EditorProject& project, const AuxiliaryEditBinding& expected,
    std::vector<Services::AuxiliaryEditRequest> requests, const QString& label);
// 世界对象一次删除：摆放与区域/镜头可以混选，合成一个撤销步骤。全部身份与引用先检查，
// 任一拒绝则都不改；第二个事务意外失败时撤回第一个，不留下半个删除。
[[nodiscard]] Status DeleteWorldObjects(EditorProject& project, const AuxiliaryEditBinding& expected,
    std::span<const MapData::PlacementId> placements, std::vector<Services::AuxiliaryEditRequest> auxiliary,
    const QString& label);
} // namespace War3Map::QtProject
