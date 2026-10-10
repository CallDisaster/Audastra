#pragma once

#include <QStringList>
#include <QDialog>
#include "War3MapCore/MapData/PlacementEditing.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QTableWidget;
class QLabel;

namespace War3Map::Render { struct AnimatedModelScene; struct AnimationFrame; struct AnimationInstanceSpec; struct AnimationInstanceState; }
namespace War3Map::Services { class WorldInstanceRegistry; }

namespace War3Map::Modules {

// registry成功路径使用实际谱系的Find及CapturedSpecs，不把Qt另一hostKey编码当canonical。
[[nodiscard]] bool WorldDiagnosticPlacementMatches(const MapData::PlacementId& owner, std::size_t request,
    const Render::AnimationInstanceSpec& spec, const Render::AnimationInstanceState& state,
    const Services::WorldInstanceRegistry* registry);

struct WorldAnimationDiagnosticGroup {
    QString modelPath, stage, code, reason, sourceLayer, sourceSha256;
    bool sourceKnown = false;
    std::size_t records = 0;
    std::vector<std::uint32_t> requests; // distinct请求；不因摘要24行上限丢弃。
};

struct WorldModelDiagnosticRow {
    QString modelPath, ownerText, reasonCode, reason, sourceText;
    std::vector<MapData::PlacementId> owners;
    std::size_t records = 0;
    bool canMerge = false; // 仅实际来源身份相同的行允许合并；未知来源仍逐实例。
    QString placeholderState; // 请求/已准备状态，不是GPU显示或地图故障签收。
};

// 纯文本导出不读取地图/资源；用户指定目标经QSaveFile发布，不直接写fallback。
[[nodiscard]] QString WorldModelDiagnosticText(const std::vector<WorldModelDiagnosticRow>& rows);
[[nodiscard]] Status ExportWorldModelDiagnostics(const std::vector<WorldModelDiagnosticRow>& rows,
    const QString& path);

// 列表与导出使用同一冻结rows。定位回调由宿主核对来源/世代/PlacementId，拒绝陈旧结果。
class WorldModelDiagnosticsDialog final : public QDialog {
public:
    explicit WorldModelDiagnosticsDialog(QWidget* parent = nullptr);
    void SetRows(std::vector<WorldModelDiagnosticRow> rows,
        std::function<bool(const MapData::PlacementId&)> locate);
    [[nodiscard]] const std::vector<WorldModelDiagnosticRow>& Rows() const noexcept { return rows_; }
    [[nodiscard]] bool LocateRow(std::size_t row);
    // chooser 可运行嵌套事件循环；返回后必须仍为同一对象、同一冻结列表才允许发布。
    [[nodiscard]] bool ExportWithChooser(const std::function<QString(QWidget*)>& chooser);
private:
    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<WorldModelDiagnosticRow> rows_;
    std::function<bool(const MapData::PlacementId&)> locate_;
    std::uint64_t rowsEpoch_ = 0;
};

struct WorldAnimationDiagnosticSummary {
    QStringList rows;
    std::size_t groups = 0, records = 0, representedRecords = 0;
    std::vector<WorldAnimationDiagnosticGroup> modelGroups;
};

// 只压缩界面行，不删除场景/帧的原始诊断。资产问题按不可变场景缓存；
// 相同路径必须具有同一实际来源和摘要才跨实例合并，未知来源单列。
class WorldAnimationDiagnostics final {
public:
    WorldAnimationDiagnostics();
    ~WorldAnimationDiagnostics();
    void Clear();
    [[nodiscard]] WorldAnimationDiagnosticSummary Summarize(
        std::shared_ptr<const Render::AnimatedModelScene> scene,
        const Render::AnimationFrame& frame, std::size_t rowLimit = 24);
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace War3Map::Modules
