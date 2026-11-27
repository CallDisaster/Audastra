#include "WorldScatterBrush.h"

#include "WorldPanelWidgets.h"
#include "WorldPlacementModels.h"
#include "WorldTerrainAssets.h"
#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/Render/MdxModel.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace War3Map::Modules {
namespace {
constexpr std::array<float, 3> kPaint{0.36f, 0.92f, 0.68f};  // 薄荷绿
constexpr std::array<float, 3> kErase{0.95f, 0.35f, 0.32f};
constexpr std::size_t kMaxMarkers = 4096;
constexpr double kTerrainNormalStep = 16;

void Line(std::vector<War3RenderOverlayLineV2>& lines, const std::array<double, 3>& a, const std::array<double, 3>& b,
    const std::array<float, 3>& color) {
    War3RenderOverlayLineV2 line{};
    for (int i = 0; i < 3; ++i) {
        line.from[i] = static_cast<float>(a[i]); line.to[i] = static_cast<float>(b[i]); line.color[i] = color[i];
    }
    lines.push_back(line);
}

void Cross(std::vector<War3RenderOverlayLineV2>& lines, const std::array<double, 3>& p, double size, const std::array<float, 3>& color) {
    const double z = p[2] + 4;
    Line(lines, {p[0] - size, p[1] - size, z}, {p[0] + size, p[1] + size, z}, color);
    Line(lines, {p[0] - size, p[1] + size, z}, {p[0] + size, p[1] - size, z}, color);
    Line(lines, {p[0], p[1], z}, {p[0], p[1], z + size * 2}, color);
}

QDoubleSpinBox* Number(QWidget* parent, const char* name, double minimum, double maximum, double value, int decimals,
    const QString& suffix = {}) {
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setRange(minimum, maximum); box->setDecimals(decimals); box->setValue(value);
    if (!suffix.isEmpty()) box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    return box;
}

QLabel* Field(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("worldFieldLabel"));
    return label;
}

std::uint64_t VertexCount(const Render::MdxModel& model) {
    std::uint64_t total = 0;
    for (const auto& geoset : model.geosets) total += geoset.positions.size();
    return total;
}
} // namespace

struct WorldScatterBrush::Stroke {
    bool erase = false;
    Services::ScatterBrushSettings settings;
    std::vector<Services::ScatterPaletteType> palette;
    std::optional<Services::ScatterSpacingIndex> spacing;
    std::optional<Services::ScatterRandom> random;
    std::uint64_t seed = 0;
    std::vector<Services::ScatterCandidate> placed;
    std::vector<MapData::DoodadPlacement> doodads; // 擦除：按下时的装饰物
    std::vector<bool> erased;
    std::vector<std::uint32_t> eraseOrder;
    std::array<double, 2> last{};
    std::uint32_t noSurface = 0, slope = 0, crowded = 0;
};

struct WorldScatterBrush::BuildResult {
    std::uint64_t serial = 0;
    std::shared_ptr<const Render::ModelSurfaceIndex> surface;
    std::map<std::string, std::shared_ptr<const Render::MdxModel>> models;
    std::vector<Diagnostic> diagnostics;
    std::size_t unreadable = 0;
};

WorldScatterBrush::WorldScatterBrush(QtProject::EditorProject& project, QObject* parent) : QObject(parent), project_(project) {
    rebuildTimer_ = new QTimer(this);
    rebuildTimer_->setSingleShot(true);
    rebuildTimer_->setInterval(250);
    connect(rebuildTimer_, &QTimer::timeout, this, &WorldScatterBrush::StartRebuild);
}

WorldScatterBrush::~WorldScatterBrush() = default;

QWidget* WorldScatterBrush::BuildPage(QWidget* parent) {
    page_ = WorldUi::Surface(parent);
    page_->setObjectName(QStringLiteral("worldScatterPage"));
    auto* column = new QVBoxLayout(page_);
    column->setContentsMargins(10, 10, 10, 10); column->setSpacing(8);

    mode_ = new QButtonGroup(page_);
    column->addWidget(WorldUi::Segment(page_, mode_, {{tr("绘制"), 0}, {tr("擦除"), 1}}, "worldScatterMode"));
    if (auto* paint = mode_->button(0)) paint->setChecked(true);

    column->addWidget(WorldUi::SectionLabel(tr("调色板"), page_));
    paletteTable_ = new QTableWidget(0, 3, page_);
    paletteTable_->setObjectName(QStringLiteral("worldScatterPalette"));
    paletteTable_->setHorizontalHeaderLabels({tr("类型"), tr("代码"), tr("权重")});
    paletteTable_->verticalHeader()->hide();
    paletteTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    paletteTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    paletteTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    paletteTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    paletteTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    paletteTable_->setMinimumHeight(96);
    paletteTable_->setToolTip(tr("在物体目录里单击装饰物加入"));
    column->addWidget(paletteTable_);
    auto* remove = new QPushButton(tr("移除所选"), page_);
    remove->setObjectName(QStringLiteral("worldScatterRemove"));
    column->addWidget(remove);
    connect(remove, &QPushButton::clicked, this, [this] {
        std::set<int, std::greater<>> rows;
        for (const auto* item : paletteTable_->selectedItems()) rows.insert(item->row());
        for (const int row : rows) if (row >= 0 && static_cast<std::size_t>(row) < palette_.size()) palette_.erase(palette_.begin() + row);
        RebuildPalette();
        RequestRebuild();
    });

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(6); grid->setVerticalSpacing(6);
    int row = 0;
    const auto pair = [&](const QString& label, QWidget* a, QWidget* b = nullptr) {
        grid->addWidget(Field(label, page_), row, 0);
        if (b) { grid->addWidget(a, row, 1); grid->addWidget(b, row, 2); }
        else grid->addWidget(a, row, 1, 1, 2);
        ++row;
    };
    radius_ = Number(page_, "worldScatterRadius", 8, 8192, 256, 0);
    pair(tr("半径"), radius_);
    densityMode_ = new QComboBox(page_);
    densityMode_->setObjectName(QStringLiteral("worldScatterDensityMode"));
    densityMode_->addItem(tr("每 100 平方米"), 0); densityMode_->addItem(tr("每笔"), 1);
    density_ = Number(page_, "worldScatterDensity", 0.001, 10000, 1, 3);
    perDab_ = new QSpinBox(page_);
    perDab_->setObjectName(QStringLiteral("worldScatterPerDab"));
    perDab_->setRange(1, 4096); perDab_->setValue(5);
    perDab_->hide();
    auto* densityValue = new QWidget(page_);
    auto* densityRow = new QHBoxLayout(densityValue);
    densityRow->setContentsMargins(0, 0, 0, 0);
    densityRow->addWidget(density_); densityRow->addWidget(perDab_);
    pair(tr("密度"), densityMode_, densityValue);
    unitsPerMetre_ = Number(page_, "worldScatterUnitsPerMetre", 1, 1000, 40, 1);
    pair(tr("游戏单位/米"), unitsPerMetre_);
    yawMin_ = Number(page_, "worldScatterYawMin", -360, 360, 0, 0, QStringLiteral("°"));
    yawMax_ = Number(page_, "worldScatterYawMax", -360, 360, 360, 0, QStringLiteral("°"));
    pair(tr("朝向"), yawMin_, yawMax_);
    scaleMin_ = Number(page_, "worldScatterScaleMin", 0.01, 100, 0.8, 2);
    scaleMax_ = Number(page_, "worldScatterScaleMax", 0.01, 100, 1.2, 2);
    pair(tr("缩放"), scaleMin_, scaleMax_);
    spacing_ = Number(page_, "worldScatterSpacing", 0, 8192, 64, 0);
    pair(tr("最小间距"), spacing_);
    slopeMin_ = Number(page_, "worldScatterSlopeMin", 0, 90, 0, 0, QStringLiteral("°"));
    slopeMax_ = Number(page_, "worldScatterSlopeMax", 0, 90, 35, 0, QStringLiteral("°"));
    pair(tr("坡度"), slopeMin_, slopeMax_);
    randomVariation_ = new QCheckBox(tr("随机变体"), page_);
    randomVariation_->setObjectName(QStringLiteral("worldScatterRandomVariation"));
    randomVariation_->setChecked(true);
    grid->addWidget(randomVariation_, row++, 0, 1, 3);
    column->addLayout(grid);

    column->addWidget(WorldUi::SectionLabel(tr("落点表面"), page_));
    onTerrain_ = new QCheckBox(tr("地形"), page_);
    onTerrain_->setObjectName(QStringLiteral("worldScatterOnTerrain"));
    onTerrain_->setChecked(true);
    column->addWidget(onTerrain_);
    surfaceFilter_ = new QLineEdit(page_);
    surfaceFilter_->setObjectName(QStringLiteral("worldScatterSurfaceFilter"));
    surfaceFilter_->setPlaceholderText(tr("模型路径或代码，如 Sonne\\Models\\near_*"));
    surfaceFilter_->setToolTip(tr("落在这些装饰物的模型表面上；多条用分号分隔，可用 * 和 ?"));
    column->addWidget(surfaceFilter_);
    surfaceStatus_ = new QLabel(page_);
    surfaceStatus_->setObjectName(QStringLiteral("worldScatterSurfaceStatus"));
    surfaceStatus_->setWordWrap(true);
    column->addWidget(surfaceStatus_);

    column->addWidget(WorldUi::SectionLabel(tr("预算"), page_));
    budgetMap_ = new QLabel(page_); budgetMap_->setObjectName(QStringLiteral("worldScatterBudgetMap"));
    budgetStroke_ = new QLabel(page_); budgetStroke_->setObjectName(QStringLiteral("worldScatterBudgetStroke"));
    budgetVertices_ = new QLabel(page_); budgetVertices_->setObjectName(QStringLiteral("worldScatterBudgetVertices"));
    for (auto* label : {budgetMap_, budgetStroke_, budgetVertices_}) column->addWidget(label);
    column->addStretch(1);

    connect(densityMode_, &QComboBox::currentIndexChanged, this, [this] {
        const bool perDab = densityMode_->currentData().toInt() == 1;
        density_->setVisible(!perDab); perDab_->setVisible(perDab);
    });
    connect(unitsPerMetre_, &QDoubleSpinBox::valueChanged, this, [this] { SaveUnitsPerMetre(); });
    connect(onTerrain_, &QCheckBox::toggled, this, [this] { RequestRebuild(); emit OverlayChanged(); });
    connect(surfaceFilter_, &QLineEdit::editingFinished, this, &WorldScatterBrush::RequestRebuild);
    connect(radius_, &QDoubleSpinBox::valueChanged, this, [this] { emit OverlayChanged(); });
    connect(mode_, &QButtonGroup::idToggled, this, [this](int, bool on) { if (on) emit OverlayChanged(); });
    UpdateBudget();
    return page_;
}

void WorldScatterBrush::SetMapKey(const QString& key) {
    mapKey_ = key;
    if (!unitsPerMetre_) return;
    const QSignalBlocker blocker(unitsPerMetre_);
    const auto stored = QSettings{}.value(QStringLiteral("world/scatter/unitsPerMetre/") + key, 40.0).toDouble();
    unitsPerMetre_->setValue(stored > 0 ? stored : 40.0);
}

void WorldScatterBrush::SaveUnitsPerMetre() {
    if (!mapKey_.isEmpty()) QSettings{}.setValue(QStringLiteral("world/scatter/unitsPerMetre/") + mapKey_, unitsPerMetre_->value());
}

void WorldScatterBrush::SetActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (!active) { Cancel(); hover_.reset(); models_.clear(); surface_.reset(); return; }
    RequestRebuild();
    UpdateBudget();
}

void WorldScatterBrush::SceneChanged(Scene scene) {
    scene_ = std::move(scene);
    if (!active_) return;
    RequestRebuild();
    UpdateBudget();
}

void WorldScatterBrush::AddType(ObjectData::ObjectType type, const QString& rawcode, const QString& name) {
    if (type != ObjectData::ObjectType::Doodad && type != ObjectData::ObjectType::Destructable) {
        emit StatusText(tr("散布只接受装饰物和可破坏物"));
        return;
    }
    const auto code = rawcode.toStdString();
    if (std::any_of(palette_.begin(), palette_.end(), [&](const auto& row) { return row.rawcode == code; })) return;
    PaletteRow row;
    row.type = type; row.rawcode = code; row.name = name.isEmpty() ? rawcode : name;
    row.variations = PlacementVariationCount(project_, type, code);
    palette_.push_back(std::move(row));
    RebuildPalette();
    RequestRebuild(); // 新类型的顶点数在后台读；表面也要排除调色板类型
}

void WorldScatterBrush::AdjustRadius(int step) {
    if (radius_) radius_->setValue(radius_->value() * (step > 0 ? 1.25 : 0.8));
}

void WorldScatterBrush::RebuildPalette() {
    if (!paletteTable_) return;
    paletteTable_->setRowCount(static_cast<int>(palette_.size()));
    for (int row = 0; row < static_cast<int>(palette_.size()); ++row) {
        auto& entry = palette_[static_cast<std::size_t>(row)];
        auto* name = new QTableWidgetItem(entry.name);
        name->setToolTip(entry.vertices ? tr("变体 %1 · 每个 %2 个顶点").arg(entry.variations).arg(*entry.vertices)
                                        : tr("变体 %1").arg(entry.variations));
        paletteTable_->setItem(row, 0, name);
        paletteTable_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(entry.rawcode)));
        auto* weight = Number(paletteTable_, "worldScatterWeight", 0.01, 1000, entry.weight, 2);
        paletteTable_->setCellWidget(row, 2, weight);
        connect(weight, &QDoubleSpinBox::valueChanged, this, [this, code = entry.rawcode](double value) {
            for (auto& item : palette_) if (item.rawcode == code) item.weight = value;
        });
    }
    UpdateBudget();
}

std::optional<Services::ScatterBrushSettings> WorldScatterBrush::Settings(QString* error) const {
    if (!radius_) return std::nullopt;
    Services::ScatterBrushSettings s;
    s.radius = radius_->value();
    s.densityMode = densityMode_->currentData().toInt() == 1 ? Services::ScatterDensityMode::PerDab : Services::ScatterDensityMode::PerArea;
    s.perTenThousandSquareUnits = Services::ScatterDensityFromSquareMetres(density_->value(), unitsPerMetre_->value());
    s.countPerDab = static_cast<std::uint32_t>(perDab_->value());
    s.yawMinDegrees = yawMin_->value(); s.yawMaxDegrees = yawMax_->value();
    s.scaleMin = scaleMin_->value(); s.scaleMax = scaleMax_->value();
    s.randomVariation = randomVariation_->isChecked();
    s.minSpacing = spacing_->value();
    s.slopeMinDegrees = slopeMin_->value(); s.slopeMaxDegrees = slopeMax_->value();
    const auto palette = PaletteTypes();
    const auto valid = Services::ValidateScatter(s, palette);
    if (!valid.Succeeded()) {
        if (error) *error = valid.diagnostics.empty() ? tr("散布设置无效") : QString::fromStdString(valid.diagnostics.front().message);
        return std::nullopt;
    }
    return s;
}

std::vector<Services::ScatterPaletteType> WorldScatterBrush::PaletteTypes() const {
    std::vector<Services::ScatterPaletteType> types;
    for (const auto& row : palette_) types.push_back({row.rawcode, row.weight, row.variations});
    return types;
}

std::optional<Services::ScatterSurfaceSample> WorldScatterBrush::Probe(double x, double y) const {
    std::optional<Services::ScatterSurfaceSample> best;
    if (surface_) if (const auto hit = surface_->Drop(x, y)) best = Services::ScatterSurfaceSample{hit->position, hit->normal};
    if (terrain_ && onTerrain_ && onTerrain_->isChecked()) {
        if (const auto z = Render::SampleTerrainHeight(*terrain_, x, y); z && (!best || *z > best->position[2])) {
            const auto h = [&](double dx, double dy) { return Render::SampleTerrainHeight(*terrain_, x + dx, y + dy).value_or(*z); };
            const double d = kTerrainNormalStep;
            const double gx = (h(d, 0) - h(-d, 0)) / (2 * d), gy = (h(0, d) - h(0, -d)) / (2 * d);
            const double length = std::sqrt(gx * gx + gy * gy + 1);
            best = Services::ScatterSurfaceSample{{x, y, *z}, {-gx / length, -gy / length, 1 / length}};
        }
    }
    return best;
}

std::optional<std::array<double, 3>> WorldScatterBrush::Pick(const Render::TerrainRay& ray, double maxDistance) const {
    std::optional<std::array<double, 3>> best;
    double nearest = maxDistance;
    if (surface_) if (const auto hit = surface_->Raycast(ray.origin, ray.direction, maxDistance)) { best = hit->position; nearest = hit->distance; }
    if (terrain_ && onTerrain_ && onTerrain_->isChecked()) {
        const auto picked = Render::PickTerrainHeightPreview(*terrain_, ray, {nearest, 8192});
        if (picked.Succeeded() && picked.value->hit && picked.value->hit->distance < nearest) best = picked.value->hit->position;
    }
    return best;
}

void WorldScatterBrush::Hover(std::optional<std::array<double, 3>> point) {
    hover_ = point;
    emit OverlayChanged();
}

void WorldScatterBrush::Begin(double x, double y) {
    Cancel();
    QString error;
    const auto settings = Settings(&error);
    if (!settings) { emit StatusText(error); return; }
    if (!project_.mapEdits_) return;
    auto stroke = std::make_unique<Stroke>();
    stroke->erase = mode_ && mode_->checkedId() == 1;
    stroke->settings = *settings;
    stroke->palette = PaletteTypes();
    const auto document = project_.mapEdits_->Snapshot();
    const auto* doodads = document.Doodads() ? &document.Doodads()->doodads : nullptr;
    if (stroke->erase) {
        if (doodads) stroke->doodads = *doodads;
        stroke->erased.assign(stroke->doodads.size(), false);
    } else {
        if (!surface_ && !(onTerrain_->isChecked() && terrain_)) { emit StatusText(tr("没有可落点的表面")); return; }
        stroke->spacing.emplace(settings->minSpacing);
        if (doodads) for (const auto& doodad : *doodads)
            if (std::any_of(stroke->palette.begin(), stroke->palette.end(), [&](const auto& type) { return type.rawcode == doodad.rawcode; }))
                stroke->spacing->Add(doodad.position[0], doodad.position[1]);
        stroke->seed = nextSeed_ ? *nextSeed_ : QRandomGenerator::global()->generate64();
        nextSeed_.reset();
        stroke->random.emplace(stroke->seed);
    }
    stroke_ = std::move(stroke);
    Dab(x, y);
}

void WorldScatterBrush::Extend(double x, double y) {
    if (!stroke_) return;
    // 沿拖动路径按半径的一半补笔，快速拖动也不会漏段。
    const double step = std::max(stroke_->settings.radius * 0.5, 1.0);
    const double dx = x - stroke_->last[0], dy = y - stroke_->last[1];
    const double distance = std::hypot(dx, dy);
    if (distance < step) return;
    const int count = static_cast<int>(std::min(distance / step, 256.0));
    for (int i = 1; i <= count; ++i) Dab(stroke_->last[0] + dx * i / count, stroke_->last[1] + dy * i / count);
}

void WorldScatterBrush::Dab(double x, double y) {
    auto& stroke = *stroke_;
    stroke.last = {x, y};
    if (stroke.erase) {
        for (const auto index : Services::SelectScatterErase(x, y, stroke.settings.radius, stroke.palette, stroke.doodads))
            if (!stroke.erased[index]) { stroke.erased[index] = true; stroke.eraseOrder.push_back(index); }
    } else {
        auto report = Services::ScatterDab(x, y, stroke.settings, stroke.palette,
            [this](double px, double py) { return Probe(px, py); }, *stroke.spacing, *stroke.random);
        if (!report.Succeeded() || !report.value) {
            emit StatusText(report.diagnostics.empty() ? tr("散布失败") : QString::fromStdString(report.diagnostics.front().message));
            return;
        }
        stroke.noSurface += report.value->noSurface; stroke.slope += report.value->slope; stroke.crowded += report.value->spacing;
        for (auto& item : report.value->placed) stroke.placed.push_back(std::move(item));
    }
    UpdateBudget();
    emit OverlayChanged();
}

void WorldScatterBrush::Cancel() {
    if (!stroke_) return;
    stroke_.reset();
    UpdateBudget();
    emit OverlayChanged();
}

void WorldScatterBrush::Finish() {
    if (!stroke_) return;
    const auto stroke = std::move(stroke_);
    if (!project_.mapEdits_) return;
    if (stroke->erase) {
        std::vector<MapData::PlacementId> ids;
        for (const auto index : stroke->eraseOrder)
            ids.push_back(MapData::MakePlacementId(MapData::PlacementFile::Doodads, index, stroke->doodads[index]));
        if (ids.empty()) { emit OverlayChanged(); return; }
        const auto removed = MapData::DeletePlacements(*project_.mapEdits_, ids);
        if (!removed.Succeeded()) {
            project_.AddDiagnostics(removed.diagnostics);
            emit StatusText(tr("擦除失败：%1").arg(QString::fromStdString(removed.diagnostics.empty() ? std::string{} : removed.diagnostics.front().message)));
            emit OverlayChanged(); return;
        }
        (void)project_.ObserveMapDataEdit("擦除散布装饰物");
        QtProject::PushMapDataEdit(project_, tr("擦除 %1 个装饰物").arg(ids.size()), "擦除散布装饰物");
        emit StatusText(tr("已擦除 %1 个装饰物").arg(ids.size()));
    } else {
        std::vector<MapData::DoodadPlacement> records;
        records.reserve(stroke->placed.size());
        for (const auto& candidate : stroke->placed) records.push_back(Services::MakeScatterDoodad(candidate));
        const auto skipped = stroke->noSurface + stroke->slope + stroke->crowded;
        if (records.empty()) {
            emit StatusText(tr("这一笔没有落点（无表面 %1，坡度 %2，间距 %3）").arg(stroke->noSurface).arg(stroke->slope).arg(stroke->crowded));
            emit OverlayChanged(); return;
        }
        const auto added = MapData::AddPlacementBatch(*project_.mapEdits_, {}, records);
        if (!added.Succeeded()) {
            project_.AddDiagnostics(added.diagnostics);
            emit StatusText(tr("散布失败：%1").arg(QString::fromStdString(added.diagnostics.empty() ? std::string{} : added.diagnostics.front().message)));
            emit OverlayChanged(); return;
        }
        (void)project_.ObserveMapDataEdit("散布装饰物");
        QtProject::PushMapDataEdit(project_, tr("散布 %1 个装饰物").arg(records.size()), "散布装饰物");
        emit StatusText(skipped ? tr("已散布 %1 个装饰物；未落下 %2 个（无表面 %3，坡度 %4，间距 %5）").arg(records.size()).arg(skipped)
                                     .arg(stroke->noSurface).arg(stroke->slope).arg(stroke->crowded)
                                : tr("已散布 %1 个装饰物").arg(records.size()));
    }
    UpdateBudget();
    emit OverlayChanged();
}

void WorldScatterBrush::AppendOverlay(std::vector<War3RenderOverlayLineV2>& lines) const {
    const bool erase = stroke_ ? stroke_->erase : mode_ && mode_->checkedId() == 1;
    const auto& color = erase ? kErase : kPaint;
    if (hover_ && radius_) {
        // 笔刷圈贴着落点表面：沿圆周逐点取高度，取不到的点用中心高度。
        const double radius = radius_->value();
        const int segments = std::clamp(static_cast<int>(2 * std::numbers::pi * radius / 32), 24, 96);
        std::array<double, 3> previous{};
        for (int i = 0; i <= segments; ++i) {
            const double angle = 2 * std::numbers::pi * i / segments;
            const double x = (*hover_)[0] + radius * std::cos(angle), y = (*hover_)[1] + radius * std::sin(angle);
            const auto sample = Probe(x, y);
            const std::array<double, 3> point{x, y, (sample ? sample->position[2] : (*hover_)[2]) + 6};
            if (i) Line(lines, previous, point, color);
            previous = point;
        }
        Cross(lines, *hover_, std::max(radius * 0.06, 6.0), color);
    }
    if (!stroke_) return;
    std::size_t markers = 0;
    if (stroke_->erase) {
        for (const auto index : stroke_->eraseOrder) {
            if (++markers > kMaxMarkers) break;
            const auto& p = stroke_->doodads[index].position;
            Cross(lines, {p[0], p[1], p[2]}, 12, kErase);
        }
    } else {
        for (const auto& item : stroke_->placed) {
            if (++markers > kMaxMarkers) break;
            Cross(lines, item.position, 10 * item.scale, kPaint);
        }
    }
}

std::string WorldScatterBrush::SurfaceKey() const {
    std::string key = surfaceFilter_ ? surfaceFilter_->text().trimmed().toStdString() : std::string{};
    key += onTerrain_ && onTerrain_->isChecked() ? "|terrain" : "|no-terrain";
    std::vector<std::string> codes;
    for (const auto& row : palette_) codes.push_back(row.rawcode);
    std::sort(codes.begin(), codes.end());
    for (const auto& code : codes) key += "|" + code;
    return key;
}

void WorldScatterBrush::RequestRebuild() {
    if (!active_) return;
    terrain_ = project_.mapEdits_ ? project_.mapEdits_->Terrain() : nullptr;
    rebuildTimer_->start();
}

void WorldScatterBrush::StartRebuild() {
    if (!active_) return;
    if (building_) { rebuildPending_ = true; return; }
    if (stroke_) { rebuildTimer_->start(); return; } // 一笔进行中保持同一表面
    const auto filter = surfaceFilter_ ? surfaceFilter_->text().trimmed().toStdString() : std::string{};
    std::set<std::string> paletteCodes;
    for (const auto& row : palette_) paletteCodes.insert(row.rawcode);
    // 落点表面：符合过滤、正在显示的装饰物与可破坏物，调色板里的类型除外。
    std::vector<std::pair<std::uint32_t, std::string>> wanted;
    std::vector<std::array<float, 12>> transforms;
    if (!filter.empty()) {
        for (std::uint32_t i = 0; i < scene_.requests.size() && i < scene_.ids.size(); ++i) {
            const auto& id = scene_.ids[i];
            const auto& request = scene_.requests[i];
            if (id.file != MapData::PlacementFile::Doodads || request.modelPath.empty() || request.previewGhost) continue;
            if (paletteCodes.contains(id.rawcode) || (scene_.visible && !scene_.visible(i))) continue;
            if (!Services::ScatterSurfaceFilterMatches(filter, id.rawcode, request.modelPath)) continue;
            wanted.emplace_back(i, request.modelPath);
            transforms.push_back(Render::ModelInstanceTransform(request));
        }
    }
    // 调色板类型变体 0 的模型：只为顶点预算读取。
    std::vector<std::pair<std::string, std::string>> paletteModels; // rawcode → 模型路径
    for (const auto& row : palette_) {
        if (row.vertices) continue;
        MapData::PlacementTransform placement; placement.rawcode = row.rawcode; placement.scale = {1, 1, 1};
        if (const auto request = CapturePlacementPreview(project_, row.type, placement, 0); request && !request->modelPath.empty())
            paletteModels.emplace_back(row.rawcode, request->modelPath);
    }
    buildingKey_ = SurfaceKey();
    if (wanted.empty() && paletteModels.empty()) {
        surface_.reset();
        builtKey_ = buildingKey_;
        surfaceError_ = filter.empty() ? QString{} : tr("没有符合过滤的装饰物");
        if (surfaceStatus_) surfaceStatus_->setText(filter.empty() ? (onTerrain_->isChecked() ? tr("只落在地形上") : tr("没有落点表面"))
                                                                   : tr("没有符合过滤的装饰物"));
        emit OverlayChanged();
        return;
    }
    auto sources = CaptureWorldTerrainSources(project_);
    if (!sources.Succeeded()) {
        surface_.reset();
        builtKey_ = buildingKey_;
        surfaceError_ = tr("资源来源不可用");
        if (surfaceStatus_) surfaceStatus_->setText(surfaceError_);
        project_.AddDiagnostics(sources.diagnostics);
        return;
    }
    building_ = true;
    const auto wantedCount = wanted.size();
    const auto serial = ++buildSerial_;
    if (surfaceStatus_ && !wanted.empty()) surfaceStatus_->setText(tr("正在读取 %1 个表面模型…").arg(wanted.size()));
    auto result = std::make_shared<BuildResult>();
    result->serial = serial;
    auto* worker = QThread::create([result, cache = models_, wanted = std::move(wanted), transforms = std::move(transforms),
        paletteModels, sources = std::move(*sources.value)]() mutable {
        try {
            auto reader = WorldResourceReader::Open(sources);
            if (!reader.Succeeded()) { result->diagnostics = reader.diagnostics; return; }
            auto& resolver = (*reader.value)->Resolver();
            const auto load = [&](const std::string& path) -> std::shared_ptr<const Render::MdxModel> {
                if (const auto found = cache.find(path); found != cache.end()) return found->second;
                std::shared_ptr<const Render::MdxModel> model;
                auto bytes = resolver.Read(path);
                if (bytes.Succeeded() && bytes.value) {
                    auto parsed = Render::ParseMdx(bytes.value->bytes);
                    if (!parsed.Succeeded()) parsed = Render::ParseMdxRigidStaticPreview(bytes.value->bytes, {});
                    if (parsed.Succeeded() && parsed.value) model = std::make_shared<const Render::MdxModel>(std::move(*parsed.value));
                }
                if (!model) ++result->unreadable;
                cache.emplace(path, model);
                return model;
            };
            std::vector<Render::ModelSurfaceSource> surfaceSources;
            for (std::size_t i = 0; i < wanted.size(); ++i)
                if (auto model = load(wanted[i].second)) surfaceSources.push_back({wanted[i].first, std::move(model), transforms[i]});
            for (const auto& [rawcode, path] : paletteModels) (void)load(path);
            if (!surfaceSources.empty()) {
                auto built = Render::ModelSurfaceIndex::Build(surfaceSources);
                for (auto& diagnostic : built.diagnostics) result->diagnostics.push_back(std::move(diagnostic));
                if (built.value) result->surface = std::make_shared<const Render::ModelSurfaceIndex>(std::move(*built.value));
            }
            result->models = std::move(cache);
        } catch (const std::exception&) {
            result->diagnostics.push_back({DiagnosticSeverity::Error, "World.Scatter", "WORLD_SCATTER_SURFACE_FAILED", "落点表面读取失败。", std::nullopt});
        }
    });
    connect(worker, &QThread::finished, this, [this, worker, result, paletteModels, wantedCount] {
        worker->deleteLater();
        building_ = false;
        if (!active_ || result->serial != buildSerial_) { if (rebuildPending_) { rebuildPending_ = false; StartRebuild(); } return; }
        for (auto& [path, model] : result->models) models_.insert_or_assign(path, model);
        surface_ = result->surface;
        builtKey_ = buildingKey_;
        surfaceError_ = wantedCount && !surface_ ? tr("符合过滤的 %1 个装饰物模型都读不到").arg(wantedCount) : QString{};
        for (const auto& [rawcode, path] : paletteModels) {
            const auto found = models_.find(path);
            for (auto& row : palette_) if (row.rawcode == rawcode) row.vertices = found != models_.end() && found->second ? VertexCount(*found->second) : 0;
        }
        if (!result->diagnostics.empty()) project_.AddDiagnostics(result->diagnostics);
        if (surfaceStatus_) {
            QString text = surface_ ? tr("模型表面：%1 个摆放，%2 个三角形").arg(surface_->SourceCount()).arg(surface_->TriangleCount())
                                    : (surfaceFilter_->text().trimmed().isEmpty() ? tr("只落在地形上") : tr("没有可用的模型表面"));
            if (result->unreadable) text += tr("；%1 个模型读不到").arg(result->unreadable);
            surfaceStatus_->setText(text);
        }
        RebuildPalette();
        emit OverlayChanged();
        if (rebuildPending_) { rebuildPending_ = false; StartRebuild(); }
    });
    worker->start(QThread::LowPriority);
}

QJsonObject WorldScatterBrush::RunTool(const QJsonObject& args) {
    const auto fail = [](const char* code, const QString& message) {
        return QJsonObject{{"ok", false}, {"diagnostics", QJsonArray{QJsonObject{{"severity", "error"},
            {"subsystem", "World.Scatter"}, {"code", code}, {"message", message}}}}};
    };
    if (!project_.mapEdits_) return fail("WORLD_SCATTER_NO_MAP", tr("没有打开的地图"));
    if (stroke_) return fail("WORLD_SCATTER_STROKE_ACTIVE", tr("界面里正在画一笔，稍后再试"));
    const auto finite = [](const QJsonValue& value) { return value.isDouble() && std::isfinite(value.toDouble()); };
    // 先完整校验，再改界面状态。
    std::vector<PaletteRow> palette;
    for (const auto& value : args.value("palette").toArray()) {
        const auto entry = value.toObject();
        const auto code = entry.value("rawcode").toString().toStdString();
        if (code.size() != 4) return fail("WORLD_SCATTER_ARGUMENT", tr("调色板 rawcode 须为四字符"));
        if (entry.contains("weight") && (!finite(entry.value("weight")) || entry.value("weight").toDouble() <= 0))
            return fail("WORLD_SCATTER_ARGUMENT", tr("调色板权重须为正数"));
        PaletteRow row;
        row.type = entry.value("type").toString() == QStringLiteral("destructable") ? ObjectData::ObjectType::Destructable
                                                                                     : ObjectData::ObjectType::Doodad;
        row.rawcode = code; row.name = QString::fromStdString(code);
        row.weight = entry.contains("weight") ? entry.value("weight").toDouble() : 1;
        row.variations = PlacementVariationCount(project_, row.type, code);
        for (const auto& old : palette_) if (old.rawcode == code) { row.name = old.name; row.vertices = old.vertices; }
        palette.push_back(std::move(row));
    }
    std::vector<std::array<double, 2>> path;
    for (const auto& value : args.value("path").toArray()) {
        const auto point = value.toArray();
        if (point.size() != 2 || !finite(point[0]) || !finite(point[1])) return fail("WORLD_SCATTER_ARGUMENT", tr("路径点须为两个有限数"));
        path.push_back({point[0].toDouble(), point[1].toDouble()});
    }
    if (palette.empty() || path.empty()) return fail("WORLD_SCATTER_ARGUMENT", tr("调色板和路径都不能为空"));
    const auto settings = args.value("settings").toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it)
        if (it.key() != QStringLiteral("random_variation") && !finite(it.value()))
            return fail("WORLD_SCATTER_ARGUMENT", tr("设置 %1 须为有限数").arg(it.key()));
    std::optional<std::uint64_t> seed;
    if (args.contains("seed")) {
        bool parsed = false;
        seed = args.value("seed").toString().toULongLong(&parsed);
        if (!parsed) return fail("WORLD_SCATTER_ARGUMENT", tr("seed 须为十进制整数串"));
    }
    const bool erase = args.value("mode").toString() == QStringLiteral("erase");
    const bool dryRun = args.value("dry_run").toBool();

    if (activator_) activator_();
    if (!active_ || !page_) return fail("WORLD_SCATTER_INACTIVE", tr("散布工具没有启用"));
    palette_ = std::move(palette);
    RebuildPalette();
    if (auto* button = mode_->button(erase ? 1 : 0)) button->setChecked(true);
    const auto set = [&](const char* key, QDoubleSpinBox* box) { if (settings.contains(key)) box->setValue(settings.value(key).toDouble()); };
    set("radius", radius_); set("units_per_metre", unitsPerMetre_); set("density_per_100m2", density_);
    set("yaw_min", yawMin_); set("yaw_max", yawMax_); set("scale_min", scaleMin_); set("scale_max", scaleMax_);
    set("min_spacing", spacing_); set("slope_min", slopeMin_); set("slope_max", slopeMax_);
    if (settings.contains("count_per_dab")) { perDab_->setValue(settings.value("count_per_dab").toInt()); densityMode_->setCurrentIndex(1); }
    else if (settings.contains("density_per_100m2")) densityMode_->setCurrentIndex(0);
    if (settings.contains("random_variation")) randomVariation_->setChecked(settings.value("random_variation").toBool());
    const auto surface = args.value("surface").toObject();
    if (surface.contains("terrain")) onTerrain_->setChecked(surface.value("terrain").toBool());
    if (surface.contains("filter")) surfaceFilter_->setText(surface.value("filter").toString());
    // 落点表面必须是本次参数对应的那一份：否则先开始读取并请调用方重试。
    if (building_ || rebuildTimer_->isActive() || builtKey_ != SurfaceKey()) {
        if (!building_) { rebuildTimer_->stop(); StartRebuild(); }
        return fail("WORLD_SCATTER_SURFACE_BUILDING", tr("落点表面正在读取，稍后重试"));
    }
    // 给了过滤却没有模型表面时不静默退回地形：调用方要的是落在模型上。
    if (!erase && !surfaceFilter_->text().trimmed().isEmpty() && !surface_)
        return fail("WORLD_SCATTER_NO_SURFACE", surfaceError_.isEmpty() ? tr("过滤没有得到可用的模型表面") : surfaceError_);
    if (!erase && !surface_ && !(onTerrain_->isChecked() && terrain_))
        return fail("WORLD_SCATTER_NO_SURFACE", tr("没有可落点的表面：未给模型过滤，且未勾选地形"));
    QString error;
    if (!Settings(&error)) return fail("WORLD_SCATTER_SETTINGS", error);

    nextSeed_ = seed;
    const auto revision = project_.mapEdits_->Revision();
    Begin(path.front()[0], path.front()[1]);
    nextSeed_.reset();
    if (!stroke_) return fail("WORLD_SCATTER_SETTINGS", tr("无法开始这一笔"));
    for (std::size_t i = 1; i < path.size(); ++i) Extend(path[i][0], path[i][1]);
    QJsonObject report{{"ok", true}, {"mode", erase ? QStringLiteral("erase") : QStringLiteral("paint")}, {"dry_run", dryRun}};
    if (!erase) report.insert("seed", QString::number(stroke_->seed));
    report.insert("rejected", QJsonObject{{"no_surface", static_cast<qint64>(stroke_->noSurface)},
        {"slope", static_cast<qint64>(stroke_->slope)}, {"spacing", static_cast<qint64>(stroke_->crowded)}});
    if (erase) report.insert("erased", static_cast<qint64>(stroke_->eraseOrder.size()));
    else {
        report.insert("placed", static_cast<qint64>(stroke_->placed.size()));
        QJsonArray samples;
        for (const auto& item : stroke_->placed) {
            if (samples.size() >= 64) break;
            samples.append(QJsonObject{{"rawcode", QString::fromStdString(item.rawcode)}, {"variation", static_cast<qint64>(item.variation)},
                {"x", item.position[0]}, {"y", item.position[1]}, {"z", item.position[2]},
                {"yaw_degrees", item.yawRadians * 180 / std::numbers::pi}, {"scale", item.scale}});
        }
        report.insert("candidates", samples);
    }
    if (surface_) report.insert("surface", QJsonObject{{"model_placements", static_cast<qint64>(surface_->SourceCount())},
        {"triangles", static_cast<qint64>(surface_->TriangleCount())}, {"terrain", onTerrain_->isChecked()}});
    else report.insert("surface", QJsonObject{{"model_placements", 0}, {"triangles", 0}, {"terrain", onTerrain_->isChecked()}});
    if (dryRun) { Cancel(); report.insert("applied", false); }
    else {
        Finish();
        report.insert("applied", project_.mapEdits_->Revision() != revision);
    }
    report.insert("map_doodads", static_cast<qint64>(budgetMapCount_));
    report.insert("palette_vertices", static_cast<qint64>(budgetVertexCount_));
    report.insert("palette_vertices_complete", budgetVerticesComplete_);
    return report;
}

void WorldScatterBrush::UpdateBudget() {
    if (!budgetMap_) return;
    if (!stroke_ && project_.mapEdits_) {
        // 只在没有笔画时重数地图：笔画期间地图不变。
        mapTypeCounts_.clear();
        const auto document = project_.mapEdits_->Snapshot();
        mapDoodads_ = document.Doodads() ? document.Doodads()->doodads.size() : 0;
        if (document.Doodads()) for (const auto& doodad : document.Doodads()->doodads) ++mapTypeCounts_[doodad.rawcode];
    }
    std::map<std::string, std::int64_t> strokeCounts;
    std::int64_t strokeTotal = 0;
    if (stroke_) {
        if (stroke_->erase) for (const auto index : stroke_->eraseOrder) { --strokeCounts[stroke_->doodads[index].rawcode]; --strokeTotal; }
        else for (const auto& item : stroke_->placed) { ++strokeCounts[item.rawcode]; ++strokeTotal; }
    }
    std::uint64_t vertices = 0;
    bool unknown = false;
    for (const auto& row : palette_) {
        const auto found = mapTypeCounts_.find(row.rawcode);
        const std::int64_t count = (found == mapTypeCounts_.end() ? 0 : static_cast<std::int64_t>(found->second)) + strokeCounts[row.rawcode];
        if (!row.vertices) { unknown = unknown || count > 0; continue; }
        vertices += static_cast<std::uint64_t>(std::max<std::int64_t>(count, 0)) * *row.vertices;
    }
    budgetMapCount_ = static_cast<std::int64_t>(mapDoodads_) + strokeTotal;
    budgetVertexCount_ = vertices;
    budgetVerticesComplete_ = !unknown;
    budgetMap_->setText(tr("地图装饰物 %1").arg(static_cast<qint64>(budgetMapCount_)));
    budgetStroke_->setText(strokeTotal >= 0 ? tr("本笔 +%1").arg(strokeTotal) : tr("本笔 %1").arg(strokeTotal));
    budgetVertices_->setText(unknown ? tr("调色板类型顶点 %1（部分模型未读完）").arg(vertices) : tr("调色板类型顶点 %1").arg(vertices));
}

} // namespace War3Map::Modules
