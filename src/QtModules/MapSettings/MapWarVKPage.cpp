#include "MapWarVKPage.h"
#include "EditorProject.h"
#include "Icons/IconProvider.h"
#include "ObjectNames.h"
#include "ProjectObjectCatalog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QIcon>
#include <QPixmap>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace War3Map::Modules {
namespace {
using MapData::WarVKInitTiming;
constexpr double kDefaultDistance = 300000, kDefaultNearClip = 4;
constexpr double kDefaultAzimuth = 0, kDefaultElevation = 45;

// 数值框只显示有限位小数：显示值没被改动时沿用设置里的原值，避免改别的项时顺带把 Agent 写的精确值截短。
double Kept(double current, const QDoubleSpinBox* box) {
    const double scale = std::pow(10.0, box->decimals());
    return std::abs(box->value() - std::round(current * scale) / scale) < 0.5 / scale ? current : box->value();
}

void AddSwitchItems(QComboBox* box) {
    box->addItem(QObject::tr("不设置"), -1);
    box->addItem(QObject::tr("开"), 1);
    box->addItem(QObject::tr("关"), 0);
}

std::optional<bool> SwitchValue(const QComboBox* box) {
    const int value = box->currentData().toInt();
    return value < 0 ? std::nullopt : std::optional<bool>(value == 1);
}

void SetSwitch(QComboBox* box, const std::optional<bool>& value) { box->setCurrentIndex(!value ? 0 : *value ? 1 : 2); }

QColor SunColor(const MapData::WarVKSunColor& color) {
    return QColor::fromRgbF(static_cast<float>(color.red), static_cast<float>(color.green), static_cast<float>(color.blue));
}

// 颜色对话框取的是 8 位分量，写成 4 位小数（204 → 0.8）。
double Channel(int value) { return std::round(value / 255.0 * 10000.0) / 10000.0; }
} // namespace

MapWarVKPage::MapWarVKPage(QtProject::EditorProject& project, QObject* context, std::function<QString()> blocked,
    std::function<void(const QString&)> report, bool raw, QWidget* parent)
    : QWidget(parent), project_(project), context_(context), blocked_(std::move(blocked)), report_(std::move(report)), raw_(raw) {
    setObjectName(QStringLiteral("mapWarVKPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(9);
    layout->addLayout(form);
    const auto caption = [&](const QString& display, const QString& rawKey, QWidget* field) {
        auto* label = new QLabel(this);
        label->setBuddy(field);
        captions_.push_back({label, display, rawKey});
        form->addRow(label, field);
    };

    limits_ = new QCheckBox(tr("启用"), this);
    limits_->setObjectName(QStringLiteral("mapWarVKCameraLimits"));
    caption(tr("镜头限制"), QStringLiteral("camera.limits"), limits_);
    distance_ = new QDoubleSpinBox(this);
    distance_->setObjectName(QStringLiteral("mapWarVKCameraDistance"));
    distance_->setRange(MapData::kWarVKCameraMaxDistanceMin, MapData::kWarVKCameraMaxDistanceMax);
    distance_->setDecimals(0);
    distance_->setSingleStep(1000);
    distance_->setValue(kDefaultDistance);
    caption(tr("镜头远距上限"), QStringLiteral("camera.limits[0]"), distance_);
    near_ = new QDoubleSpinBox(this);
    near_->setObjectName(QStringLiteral("mapWarVKCameraNear"));
    near_->setRange(MapData::kWarVKCameraNearClipMin, MapData::kWarVKCameraNearClipMax);
    near_->setDecimals(1);
    near_->setValue(kDefaultNearClip);
    caption(tr("近裁剪"), QStringLiteral("camera.limits[1]"), near_);
    timing_ = new QComboBox(this);
    timing_->setObjectName(QStringLiteral("mapWarVKTiming"));
    timing_->addItem(tr("初始化后 0 秒"), static_cast<int>(WarVKInitTiming::AfterInitialization));
    timing_->addItem(tr("main 开头"), static_cast<int>(WarVKInitTiming::MainStart));
    caption(tr("执行时机"), QStringLiteral("timing"), timing_);
    // 天空模型：列出地图里（含待保存导入）的模型文件，也可直接输入游戏路径。
    sky_ = new QComboBox(this);
    sky_->setObjectName(QStringLiteral("mapWarVKSkyModel"));
    sky_->setEditable(true);
    sky_->setInsertPolicy(QComboBox::NoInsert);
    sky_->setMaxVisibleItems(20);
    caption(tr("天空模型"), QStringLiteral("sky.model"), sky_);

    sunDirection_ = new QCheckBox(tr("启用"), this);
    sunDirection_->setObjectName(QStringLiteral("mapWarVKSunDirection"));
    caption(tr("太阳方向"), QStringLiteral("sun.direction"), sunDirection_);
    azimuth_ = new QDoubleSpinBox(this);
    azimuth_->setObjectName(QStringLiteral("mapWarVKSunAzimuth"));
    azimuth_->setRange(0, 359.99);
    azimuth_->setDecimals(2);
    azimuth_->setWrapping(true);
    azimuth_->setSuffix(QStringLiteral("°"));
    azimuth_->setToolTip(tr("0° 东，90° 北（同单位朝向）"));
    caption(tr("太阳方位角"), QStringLiteral("sun.direction[0]"), azimuth_);
    elevation_ = new QDoubleSpinBox(this);
    elevation_->setObjectName(QStringLiteral("mapWarVKSunElevation"));
    elevation_->setRange(MapData::kWarVKSunElevationMin, MapData::kWarVKSunElevationMax);
    elevation_->setDecimals(2);
    elevation_->setSuffix(QStringLiteral("°"));
    caption(tr("太阳仰角"), QStringLiteral("sun.direction[1]"), elevation_);
    auto* colorBox = new QWidget(this);
    auto* colorLayout = new QHBoxLayout(colorBox);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    sunColor_ = new QCheckBox(tr("启用"), colorBox);
    sunColor_->setObjectName(QStringLiteral("mapWarVKSunColor"));
    colorLayout->addWidget(sunColor_);
    sunColorButton_ = new QPushButton(colorBox);
    sunColorButton_->setObjectName(QStringLiteral("mapWarVKSunColorButton"));
    sunColorButton_->setIconSize(QSize(28, 14));
    colorLayout->addWidget(sunColorButton_);
    colorLayout->addStretch();
    caption(tr("太阳颜色"), QStringLiteral("sun.color"), colorBox);
    intensity_ = new QDoubleSpinBox(this);
    intensity_->setObjectName(QStringLiteral("mapWarVKSunIntensity"));
    intensity_->setRange(0, MapData::kWarVKSunIntensityMax);
    intensity_->setDecimals(3);
    intensity_->setSingleStep(0.1);
    caption(tr("太阳强度"), QStringLiteral("sun.color[3]"), intensity_);
    celestial_ = new QComboBox(this);
    celestial_->setObjectName(QStringLiteral("mapWarVKCelestialMotion"));
    AddSwitchItems(celestial_);
    caption(tr("天体运行"), QStringLiteral("lighting.celestialMotion"), celestial_);
    grading_ = new QComboBox(this);
    grading_->setObjectName(QStringLiteral("mapWarVKTimeColorGrading"));
    AddSwitchItems(grading_);
    caption(tr("按时间调色"), QStringLiteral("lighting.timeColorGrading"), grading_);

    auto* farBox = new QWidget(this);
    auto* farLayout = new QVBoxLayout(farBox);
    farLayout->setContentsMargins(0, 0, 0, 0);
    far_ = new QListWidget(farBox);
    far_->setObjectName(QStringLiteral("mapWarVKFarTypes"));
    far_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    far_->setIconSize(QSize(24, 24));
    far_->setMinimumHeight(160);
    farLayout->addWidget(far_);
    auto* farButtons = new QHBoxLayout;
    farCount_ = new QLabel(farBox);
    farCount_->setObjectName(QStringLiteral("mapWarVKFarCount"));
    farButtons->addWidget(farCount_);
    farButtons->addStretch();
    removeFar_ = new QPushButton(tr("取消远景"), farBox);
    removeFar_->setObjectName(QStringLiteral("mapWarVKRemoveFar"));
    farButtons->addWidget(removeFar_);
    farLayout->addLayout(farButtons);
    caption(tr("远景装饰物"), QStringLiteral("doodad.far"), farBox);

    problem_ = new QLabel(this);
    problem_->setObjectName(QStringLiteral("mapWarVKProblem"));
    problem_->setWordWrap(true);
    problem_->setVisible(false);
    layout->addWidget(problem_);

    connect(limits_, &QCheckBox::toggled, this, [this](bool) { Commit(Edited(), tr("修改 WarVK 镜头限制")); });
    connect(distance_, &QDoubleSpinBox::editingFinished, this, [this] { Commit(Edited(), tr("修改镜头远距上限")); });
    connect(near_, &QDoubleSpinBox::editingFinished, this, [this] { Commit(Edited(), tr("修改近裁剪")); });
    connect(timing_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        Commit(Edited(), tr("修改 WarVK 执行时机"));
    });
    connect(sky_, qOverload<int>(&QComboBox::activated), this, [this](int) { Commit(Edited(), tr("修改天空模型")); });
    connect(sky_->lineEdit(), &QLineEdit::editingFinished, this, [this] { Commit(Edited(), tr("修改天空模型")); });
    connect(sunDirection_, &QCheckBox::toggled, this, [this](bool) { Commit(Edited(), tr("修改太阳方向")); });
    connect(azimuth_, &QDoubleSpinBox::editingFinished, this, [this] { Commit(Edited(), tr("修改太阳方位角")); });
    connect(elevation_, &QDoubleSpinBox::editingFinished, this, [this] { Commit(Edited(), tr("修改太阳仰角")); });
    connect(sunColor_, &QCheckBox::toggled, this, [this](bool) { Commit(Edited(), tr("修改太阳颜色")); });
    connect(intensity_, &QDoubleSpinBox::editingFinished, this, [this] { Commit(Edited(), tr("修改太阳强度")); });
    connect(sunColorButton_, &QPushButton::clicked, this, [this] {
        auto next = Edited();
        if (!next.sunColor) return;
        const auto picked = QColorDialog::getColor(SunColor(*next.sunColor), this, tr("太阳颜色"));
        if (!picked.isValid()) return;
        next.sunColor->red = Channel(picked.red());
        next.sunColor->green = Channel(picked.green());
        next.sunColor->blue = Channel(picked.blue());
        Commit(std::move(next), tr("修改太阳颜色"));
    });
    connect(celestial_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { Commit(Edited(), tr("修改天体运行")); });
    connect(grading_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { Commit(Edited(), tr("修改按时间调色")); });
    connect(far_, &QListWidget::itemSelectionChanged, this, [this] { removeFar_->setEnabled(!far_->selectedItems().isEmpty()); });
    connect(removeFar_, &QPushButton::clicked, this, [this] {
        auto next = current_;
        int removed = 0;
        for (const auto* item : far_->selectedItems()) {
            const auto code = item->data(Qt::UserRole).toString().toStdString();
            if (std::erase(next.farDoodadTypes, code)) ++removed;
        }
        if (removed) Commit(std::move(next), tr("取消 %1 个远景装饰物").arg(removed));
    });
    // 待保存导入被其他入口改动（资源页替换、物编勾选在草稿期间提交）时重新读取。
    connect(&project_, &QtProject::EditorProject::ImportsChanged, this, &MapWarVKPage::Fill);
    SetRaw(raw);
}

void MapWarVKPage::SetRaw(bool raw) {
    raw_ = raw;
    for (const auto& caption : captions_) caption.widget->setText(raw ? caption.raw : caption.display);
    Fill();
}

MapData::WarVKMapSettings MapWarVKPage::Edited() const {
    auto next = current_;
    next.timing = static_cast<WarVKInitTiming>(timing_->currentData().toInt());
    if (limits_->isChecked()) next.cameraLimits = MapData::WarVKCameraLimits{distance_->value(), near_->value()};
    else next.cameraLimits.reset();
    next.skyModel = sky_->currentText().trimmed().replace(QLatin1Char('/'), QLatin1Char('\\')).toStdString();
    if (sunDirection_->isChecked()) {
        const auto old = current_.sunDirection.value_or(MapData::WarVKSunDirection{kDefaultAzimuth, kDefaultElevation});
        next.sunDirection = MapData::WarVKSunDirection{Kept(old.azimuth, azimuth_), Kept(old.elevation, elevation_)};
    } else {
        next.sunDirection.reset();
    }
    if (sunColor_->isChecked()) {
        auto color = current_.sunColor.value_or(MapData::WarVKSunColor{});
        color.intensity = Kept(color.intensity, intensity_);
        next.sunColor = color;
    } else {
        next.sunColor.reset();
    }
    next.celestialMotion = SwitchValue(celestial_);
    next.timeColorGrading = SwitchValue(grading_);
    return next;
}

void MapWarVKPage::Fill() {
    const auto read = project_.QueryWarVKMapSettings();
    const bool ok = read.Succeeded() && read.value;
    for (QWidget* widget : std::initializer_list<QWidget*>{limits_, distance_, near_, timing_, sky_, sunDirection_, azimuth_,
             elevation_, sunColor_, sunColorButton_, intensity_, celestial_, grading_, far_, removeFar_})
        widget->setEnabled(ok);
    problem_->setVisible(!ok);
    if (!ok) {
        problem_->setText(read.diagnostics.empty() ? tr("无法读取 WarVK 设置。")
            : QString::fromStdString(read.diagnostics.front().code + " · " + read.diagnostics.front().message));
        return;
    }
    current_ = *read.value;
    const QSignalBlocker limitsBlocker(limits_), distanceBlocker(distance_), nearBlocker(near_), timingBlocker(timing_);
    limits_->setChecked(current_.cameraLimits.has_value());
    distance_->setValue(current_.cameraLimits ? current_.cameraLimits->maxDistance : kDefaultDistance);
    near_->setValue(current_.cameraLimits ? current_.cameraLimits->nearClip : kDefaultNearClip);
    distance_->setEnabled(current_.cameraLimits.has_value());
    near_->setEnabled(current_.cameraLimits.has_value());
    timing_->setCurrentIndex(current_.timing == WarVKInitTiming::MainStart ? 1 : 0);
    {
        const QSignalBlocker directionBlocker(sunDirection_), azimuthBlocker(azimuth_), elevationBlocker(elevation_),
            colorBlocker(sunColor_), intensityBlocker(intensity_), celestialBlocker(celestial_), gradingBlocker(grading_);
        const auto direction = current_.sunDirection.value_or(MapData::WarVKSunDirection{kDefaultAzimuth, kDefaultElevation});
        sunDirection_->setChecked(current_.sunDirection.has_value());
        azimuth_->setValue(direction.azimuth);
        elevation_->setValue(direction.elevation);
        azimuth_->setEnabled(current_.sunDirection.has_value());
        elevation_->setEnabled(current_.sunDirection.has_value());
        const auto color = current_.sunColor.value_or(MapData::WarVKSunColor{});
        sunColor_->setChecked(current_.sunColor.has_value());
        intensity_->setValue(color.intensity);
        intensity_->setEnabled(current_.sunColor.has_value());
        sunColorButton_->setEnabled(current_.sunColor.has_value());
        QPixmap swatch(28, 14);
        swatch.fill(SunColor(color));
        sunColorButton_->setIcon(QIcon(swatch));
        sunColorButton_->setText(SunColor(color).name(QColor::HexRgb).toUpper());
        SetSwitch(celestial_, current_.celestialMotion);
        SetSwitch(grading_, current_.timeColorGrading);
    }
    {
        const QSignalBlocker skyBlocker(sky_), skyEditBlocker(sky_->lineEdit());
        QStringList models;
        for (const auto& entry : project_.AssetEntries()) {
            const auto lower = entry.path.toLower();
            if (lower.endsWith(QStringLiteral(".mdx")) || lower.endsWith(QStringLiteral(".mdl"))) models << entry.path;
        }
        models.sort(Qt::CaseInsensitive);
        sky_->clear();
        sky_->addItem(QString());
        sky_->addItems(models);
        sky_->setEditText(QString::fromStdString(current_.skyModel));
    }

    if (!project_.standardCatalog_) (void)QtProject::EnsureProjectObjectCatalog(project_);
    far_->clear();
    for (const auto& code : current_.farDoodadTypes) {
        const auto raw = QString::fromStdString(code);
        auto* item = new QListWidgetItem(raw, far_);
        item->setData(Qt::UserRole, raw);
        item->setToolTip(raw);
        if (raw_) continue;
        const auto shown = ResolveProjectObjectPresentation(project_, {ObjectData::ObjectType::Doodad, code});
        if (!shown.name.isEmpty()) item->setText(shown.name + QStringLiteral(" (") + raw + QChar(')'));
        if (project_.iconProvider_ && !shown.iconPath.isEmpty()) item->setIcon(project_.iconProvider_->IconForTexture(shown.iconPath));
    }
    farCount_->setText(QStringLiteral("%1 / %2").arg(current_.farDoodadTypes.size()).arg(MapData::kWarVKMaxFarDoodadTypes));
    removeFar_->setEnabled(false);
}

void MapWarVKPage::Commit(MapData::WarVKMapSettings next, const QString& label) {
    if (next == current_) { Fill(); return; }
    if (const auto reason = blocked_ ? blocked_() : QString{}; !reason.isEmpty()) {
        if (report_) report_(reason);
        Fill();
        return;
    }
    // 发布会让地图设置页重建本页：推迟到事件循环下一轮，不在本页控件的信号处理里删掉自己。
    QTimer::singleShot(0, context_, [project = &project_, page = QPointer<MapWarVKPage>(this), next = std::move(next), label,
        report = report_] {
        const auto status = project->EditWarVKMapSettings(next, label);
        if (status.Succeeded()) return;
        if (report && !status.diagnostics.empty())
            report(QString::fromStdString(status.diagnostics.front().code + " · " + status.diagnostics.front().message));
        if (page) page->Fill();
    });
}

} // namespace War3Map::Modules
