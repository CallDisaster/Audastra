#pragma once

#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Render/ModelSurfaceIndex.h"
#include "War3MapCore/Render/TerrainPicking.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/Services/DoodadScatter.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTimer;
class QWidget;

namespace War3Map::MapData { struct TerrainGridSnapshot; }
namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::Modules {

// 世界页“散布”工具：调色板、笔刷设置、落点表面、一笔的预览与提交（一笔一步撤销）。
// 取点规则在 Services::ScatterDab，模型表面在 Render::ModelSurfaceIndex；本类只把它们接到视口和工程撤销栈。
class WorldScatterBrush final : public QObject {
    Q_OBJECT
public:
    // 当前世界场景的摆放请求（与视口同序）；visible 为编辑器显示状态。
    struct Scene {
        std::vector<Render::ModelInstanceRequest> requests;
        std::vector<MapData::PlacementId> ids;
        std::function<bool(std::uint32_t request)> visible;
    };

    explicit WorldScatterBrush(QtProject::EditorProject& project, QObject* parent = nullptr);
    ~WorldScatterBrush() override;

    [[nodiscard]] QWidget* BuildPage(QWidget* parent);
    void SetMapKey(const QString& key);
    void SetActive(bool active);
    void SceneChanged(Scene scene);
    void AddType(ObjectData::ObjectType type, const QString& rawcode, const QString& name);
    void AdjustRadius(int step);

    // 视口的表面拾取：沿视线取最近的落点表面（符合过滤的模型、勾选时含地形）。
    [[nodiscard]] std::optional<std::array<double, 3>> Pick(const Render::TerrainRay& ray, double maxDistance) const;
    void Hover(std::optional<std::array<double, 3>> point);
    void Begin(double x, double y);
    void Extend(double x, double y);
    void Finish();
    void Cancel();
    [[nodiscard]] bool StrokeActive() const noexcept { return stroke_ != nullptr; }
    void AppendOverlay(std::vector<War3RenderOverlayLineV2>& lines) const;
    // MCP world_scatter_stroke：设置界面状态后按同一规则画一笔（一笔一步撤销）；模型表面未就绪时返回可重试的失败。
    [[nodiscard]] QJsonObject RunTool(const QJsonObject& arguments);
    void SetActivator(std::function<void()> activate) { activator_ = std::move(activate); }

signals:
    void OverlayChanged();
    void StatusText(const QString& text);

private:
    struct PaletteRow {
        ObjectData::ObjectType type = ObjectData::ObjectType::Doodad;
        std::string rawcode;
        QString name;
        double weight = 1;
        std::uint32_t variations = 1;
        std::optional<std::uint64_t> vertices; // 变体 0 模型的顶点数；读取前为空
    };
    struct Stroke;
    struct BuildResult;

    [[nodiscard]] std::optional<Services::ScatterBrushSettings> Settings(QString* error = nullptr) const;
    [[nodiscard]] std::vector<Services::ScatterPaletteType> PaletteTypes() const;
    [[nodiscard]] std::optional<Services::ScatterSurfaceSample> Probe(double x, double y) const;
    void Dab(double x, double y);
    void RebuildPalette();
    void RequestRebuild();
    void StartRebuild();
    void UpdateBudget();
    void UpdateDensityUnits();
    [[nodiscard]] std::string SurfaceKey() const;
    void SaveUnitsPerMetre();

    QtProject::EditorProject& project_;
    QString mapKey_;
    bool active_ = false;
    Scene scene_;
    std::vector<PaletteRow> palette_;
    std::map<std::string, std::shared_ptr<const Render::MdxModel>> models_; // 本次启用期间读过的模型（按路径）
    std::shared_ptr<const Render::ModelSurfaceIndex> surface_;
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain_;
    std::uint64_t buildSerial_ = 0;
    bool building_ = false, rebuildPending_ = false;
    std::optional<std::array<double, 3>> hover_;
    std::unique_ptr<Stroke> stroke_;
    std::map<std::string, std::uint64_t> mapTypeCounts_;
    std::size_t mapDoodads_ = 0;
    // 最近一次预算：地图装饰物数（含进行中的一笔）、调色板类型顶点合计、顶点是否已全部读到。
    std::int64_t budgetMapCount_ = 0;
    std::uint64_t budgetVertexCount_ = 0;
    bool budgetVerticesComplete_ = true;
    std::function<void()> activator_;
    std::optional<std::uint64_t> nextSeed_;
    std::string builtKey_, buildingKey_;
    QString surfaceError_; // 最近一次表面读取失败的原因；成功读取后清空

    QWidget* page_ = nullptr;
    QButtonGroup* mode_ = nullptr;
    QTableWidget* paletteTable_ = nullptr;
    QDoubleSpinBox* radius_ = nullptr;
    QComboBox* densityMode_ = nullptr;
    QDoubleSpinBox* density_ = nullptr;
    QSpinBox* perDab_ = nullptr;
    QDoubleSpinBox* unitsPerMetre_ = nullptr;
    QDoubleSpinBox* yawMin_ = nullptr;
    QDoubleSpinBox* yawMax_ = nullptr;
    QDoubleSpinBox* scaleMin_ = nullptr;
    QDoubleSpinBox* scaleMax_ = nullptr;
    QCheckBox* randomVariation_ = nullptr;
    QDoubleSpinBox* spacing_ = nullptr;
    QDoubleSpinBox* slopeMin_ = nullptr;
    QDoubleSpinBox* slopeMax_ = nullptr;
    QCheckBox* onTerrain_ = nullptr;
    QLineEdit* surfaceFilter_ = nullptr;
    QLabel* surfaceStatus_ = nullptr;
    QLabel* budgetMap_ = nullptr;
    QLabel* budgetStroke_ = nullptr;
    QLabel* budgetVertices_ = nullptr;
    QTimer* rebuildTimer_ = nullptr;
};

} // namespace War3Map::Modules
