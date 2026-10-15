#pragma once

#include "War3MapCore/Services/AuxiliaryEditing.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include <QString>
#include <filesystem>

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
} // namespace War3Map::QtProject
