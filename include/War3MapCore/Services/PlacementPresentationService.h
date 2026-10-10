#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Services/WorldSelection.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Services {

enum class PlacementFieldState : std::uint8_t { Known, Absent, Unavailable, Opaque };
struct PlacementFieldOrigin {
    std::string field, objectRawcode, sourceId, sourcePath;
    std::optional<std::uint64_t> byteOffset;
};
struct PlacementTextFact {
    PlacementFieldState state = PlacementFieldState::Unavailable;
    std::string value, reason;
};
struct PlacementBoolFact {
    PlacementFieldState state = PlacementFieldState::Unavailable;
    std::optional<bool> value;
    std::string reason;
};
// 宿主从同一冻结目录/地图有效字段reader取得owned事实；本服务不查父链、不读物编。
// Known字段须在origins中有规范ID bcat/buch/bfil/bptx及真实来源；字段值不因模型缺失而改写。
struct PlacementPresentationFields {
    PlacementTextFact category, modelPath, pathingTexture;
    PlacementBoolFact useClickHelper;
    std::vector<PlacementFieldOrigin> origins;
};
struct PlacementPresentationInput {
    ObjectData::ObjectKey object;
    std::optional<MapData::PlacementId> owner; // 空为类型预览；正式owner由宿主当前注册表核验。
    WorldSelectionVersion version;
    PlacementPresentationFields fields;
};
// 读取前必须限制maxBytes；使用同一冻结来源，不能在高层失败后降级读取。
// resourceIdentity覆盖来源层序、地图/会话资源及工具来源修订，不是外部文件长期租约。
using PlacementPresentationResourceReader = std::function<Result<Resources::ResolvedResource>(
    std::string_view canonicalPath, std::size_t maxBytes)>;
struct PlacementPresentationContext {
    WorldSelectionVersion version;
    std::string resourceIdentity;
    PlacementPresentationResourceReader resourceReader;
};
struct PlacementPresentationLimits {
    std::size_t maxFieldBytes = 4096, maxOrigins = 32, maxLogicalBytes = 256 * 1024;
    std::size_t maxTextureBytes = 1024 * 1024, maxTexturePixels = 1024 * 1024;
};
enum class PlacementPresentationKind : std::uint8_t { Default, PathingBlockerHelper, Unknown };
enum class PlacementFootprintState : std::uint8_t { Available, Absent, Unavailable, Unsupported, Unknown, NotApplicable };
struct PlacementPathingFootprint {
    std::uint32_t pixelWidth = 0, pixelHeight = 0;
    float worldWidth = 0, worldHeight = 0;
    static constexpr float worldUnitsPerPixel = 32;
    // 仅来源尺寸事实；不执行WPM合成，也不推断原版锚点/旋转/对象缩放。
    bool nativeAnchorVerified = false, nativeRotationVerified = false, nativeObjectScaleVerified = false;
};
struct PlacementPresentationRow {
    ObjectData::ObjectKey object;
    std::optional<MapData::PlacementId> owner;
    WorldSelectionVersion version;
    // inputIdentity仅绑定输入事实；资产缓存还须绑定实际textureSource/textureSha256。
    std::string resourceIdentity, inputIdentity;
    PlacementPresentationFields fields;
    PlacementPresentationKind kind = PlacementPresentationKind::Unknown;
    std::string classificationReason, footprintReason;
    PlacementFootprintState footprintState = PlacementFootprintState::Unknown;
    std::optional<Resources::ResourceLocation> textureSource;
    std::optional<std::string> textureSha256;
    std::optional<PlacementPathingFootprint> footprint;
    // 读取失败/支持格式损坏保留明确诊断；与typed行返回，不改称Missing或换来源。
    std::vector<Diagnostic> resourceDiagnostics;
};
// 单对象按需查询：UI预览、正式实例或Agent使用同一入口，无整张地图实例数上限。
// version/resourceIdentity是宿主声明；inputIdentity绑定传入全部事实，不证明外部实时性。
// 宿主仍负责真正冻结父链、资源和owner注册表，并在提交/消费时核验当前性。
// 同样验证全部输入身份/字段/预算，仅分类；不读取占地、不构造随后丢弃的行或输入SHA。
[[nodiscard]] Result<PlacementPresentationKind> ClassifyPlacementPresentation(
    const PlacementPresentationContext& context, const WorldSelectionVersion& expectedVersion,
    std::string_view expectedResourceIdentity, const PlacementPresentationInput& input,
    const PlacementPresentationLimits& limits = {});
[[nodiscard]] Result<PlacementPresentationRow> QueryPlacementPresentation(
    const PlacementPresentationContext& context, const WorldSelectionVersion& expectedVersion,
    std::string_view expectedResourceIdentity, const PlacementPresentationInput& input,
    const PlacementPresentationLimits& limits = {});

// 一次冻结来源查询批次；有界缓存保存资源/占地事实与精确相同输入的规范片段，
// 不借用旧owner行；每行仍完整校验、组装当前身份并重新计算SHA。
// 各阶段为本次查询的CPU区间；未执行的阶段为零，不代表完成帧或GPU耗时。
struct PlacementPresentationQueryTimings {
    std::uint64_t validateNs = 0, rowCopyNs = 0;
    std::uint64_t identityCanonicalNs = 0, identityHashNs = 0;
    std::uint64_t resourceNs = 0, footprintNs = 0, totalNs = 0;
};

// context/limits在批次内固定，每次Query仍完整验证输入、当前行指纹与输出预算。
// 宿主在消费前核验来源租约；本对象本身不证明外部文件长期不变，不能跨编辑事务复用。
class PlacementPresentationBatch {
public:
    explicit PlacementPresentationBatch(PlacementPresentationContext context,
        PlacementPresentationLimits limits = {});
    ~PlacementPresentationBatch();
    PlacementPresentationBatch(PlacementPresentationBatch&&) noexcept;
    PlacementPresentationBatch& operator=(PlacementPresentationBatch&&) noexcept;
    PlacementPresentationBatch(const PlacementPresentationBatch&) = delete;
    PlacementPresentationBatch& operator=(const PlacementPresentationBatch&) = delete;
    [[nodiscard]] Result<PlacementPresentationRow> Query(const WorldSelectionVersion& expectedVersion,
        std::string_view expectedResourceIdentity, const PlacementPresentationInput& input,
        PlacementPresentationQueryTimings* timings = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Services
