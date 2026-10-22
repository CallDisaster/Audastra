#pragma once

#include "War3MapCore/CompatibilityProfile.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace War3Map {

namespace Trigger {
class WtgWctCodec;
}

namespace ObjectData {
enum class ObjectType : std::uint8_t;
class LniObjectCodec;
class NativeObjectCodec;
}

namespace MapData { class MapDataFieldPatchCodec; class MapDataPlacementPatchCodec; class TerrainPatchCodec; }
namespace Services { class AuxiliaryTransactionSnapshot; }

struct OpenRequest {
    std::filesystem::path workspacePath;
    CompatibilityProfile profile;
    std::string uiCatalogSha256;
};

struct BinaryFilePatch {
    std::filesystem::path relativePath;
    std::vector<std::byte> replacement;
    std::string summary;
    std::string expectedSourceSha256;
};

class TriggerPatch {
public:
    TriggerPatch(TriggerPatch&&) noexcept = default;
    TriggerPatch& operator=(TriggerPatch&&) noexcept = default;

    TriggerPatch(const TriggerPatch&) = delete;
    TriggerPatch& operator=(const TriggerPatch&) = delete;

private:
    friend class Trigger::WtgWctCodec;
    friend class EditorSession;

    explicit TriggerPatch(
        std::vector<BinaryFilePatch> files,
        std::string uiCatalogSha256);

    std::vector<BinaryFilePatch> files_;
    std::string uiCatalogSha256_;
};

class ObjectPatch {
public:
    ObjectPatch(ObjectPatch&&) noexcept = default;
    ObjectPatch& operator=(ObjectPatch&&) noexcept = default;

    ObjectPatch(const ObjectPatch&) = delete;
    ObjectPatch& operator=(const ObjectPatch&) = delete;

private:
    friend class ObjectData::LniObjectCodec;
    friend class ObjectData::NativeObjectCodec;
    friend class EditorSession;

    explicit ObjectPatch(std::vector<BinaryFilePatch> files,
        ObjectData::ObjectType type, bool native);

    std::vector<BinaryFilePatch> files_;
    ObjectData::ObjectType type_;
    bool native_ = false;
};

// 只由共用地图字段 codec 密封的既有原生成员替换；没有任意 bytes/删除/新增入口。
class MapDataPatch {
public:
    MapDataPatch(MapDataPatch&&) noexcept = default;
    MapDataPatch& operator=(MapDataPatch&&) noexcept = default;
    MapDataPatch(const MapDataPatch&) = delete;
    MapDataPatch& operator=(const MapDataPatch&) = delete;
private:
    friend class MapData::MapDataFieldPatchCodec;
    friend class MapData::MapDataPlacementPatchCodec;
    friend class EditorSession;
    explicit MapDataPatch(std::vector<BinaryFilePatch> files);
    std::vector<BinaryFilePatch> files_;
};

// 共用区域/镜头命令序列的完整重放证明，只授权 W3R/W3C；缺席用明确的 missing 身份。
class AuxiliaryPatch {
public:
    AuxiliaryPatch(AuxiliaryPatch&&) noexcept = default;
    AuxiliaryPatch& operator=(AuxiliaryPatch&&) noexcept = default;
    AuxiliaryPatch(const AuxiliaryPatch&) = delete;
    AuxiliaryPatch& operator=(const AuxiliaryPatch&) = delete;
private:
    friend class Services::AuxiliaryTransactionSnapshot;
    friend class EditorSession;
    explicit AuxiliaryPatch(std::vector<BinaryFilePatch> files) : files_(std::move(files)) {}
    std::vector<BinaryFilePatch> files_;
};

// 只由共用地形 codec 重放证明；不扩大 MapDataPatch 的成员范围。
class TerrainPatch {
public:
    TerrainPatch(TerrainPatch&&) noexcept = default;
    TerrainPatch& operator=(TerrainPatch&&) noexcept = default;
    TerrainPatch(const TerrainPatch&) = delete;
    TerrainPatch& operator=(const TerrainPatch&) = delete;
    // 只读查看已由codec核对的完整成员集，宿主复核使用同一份来源和输出。
    [[nodiscard]] std::span<const BinaryFilePatch> Files() const noexcept { return files_; }
private:
    friend class MapData::TerrainPatchCodec;
    friend class EditorSession;
    explicit TerrainPatch(std::vector<BinaryFilePatch> files) : files_(std::move(files)) {}
    std::vector<BinaryFilePatch> files_;
};

class EditorSession {
public:
    EditorSession(EditorSession&&) noexcept;
    EditorSession& operator=(EditorSession&&) noexcept;
    ~EditorSession();

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    [[nodiscard]] static Result<EditorSession> Open(OpenRequest request);

    [[nodiscard]] ProjectRevision Revision() const;
    [[nodiscard]] const CompatibilityProfile& Profile() const noexcept;
    // 事务入口可在每次操作前复核打开时的只读来源快照；该检查不修改来源或 overlay。
    [[nodiscard]] Status VerifySourceUnchanged() const;

    [[nodiscard]] Result<TransactionId> BeginEdit(
        const ProjectRevision& expectedRevision);
    [[nodiscard]] Status ApplyTriggerPatch(
        TransactionId transactionId,
        TriggerPatch patch);
    [[nodiscard]] Status ApplyObjectPatch(
        TransactionId transactionId,
        ObjectPatch patch);
    [[nodiscard]] Status ApplyMapDataPatch(
        TransactionId transactionId,
        MapDataPatch patch);
    [[nodiscard]] Status ApplyTerrainPatch(TransactionId transactionId, TerrainPatch patch);
    [[nodiscard]] Status ApplyAuxiliaryPatch(TransactionId transactionId, AuxiliaryPatch patch);
    // 仅 codec 密封的空基线物编可创建成员。既有替换入口不放宽；类别、格式、
    // 来源缺失及空基线身份均须成立。再次编辑同一新增成员仍使用这个显式入口。
    [[nodiscard]] Status CreateObjectMember(
        TransactionId transactionId,
        ObjectPatch patch);
    [[nodiscard]] Result<SemanticDiff> PreviewEdit(
        TransactionId transactionId) const;
    [[nodiscard]] ValidationReport Validate(
        TransactionId transactionId,
        const CompatibilityProfile& profile,
        CompatibilityLevel requestedLevel) const;
    // finalGuard 在临时目录与 manifest 完成后、首次发布 rename 前调用。
    // 回调运行时持有 session mutex，必须是无副作用且不可重入 EditorSession 的只读检查。
    [[nodiscard]] Status CommitCandidate(
        TransactionId transactionId,
        const ProjectRevision& expectedRevision,
        const std::filesystem::path& outputPath,
        std::function<Status()> finalGuard = {});
    [[nodiscard]] Status Rollback(TransactionId transactionId);

private:
    class Impl;
    explicit EditorSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map
