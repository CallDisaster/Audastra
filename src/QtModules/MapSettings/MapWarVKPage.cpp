#include "MapWarVKPage.h"
#include "EditorProject.h"
#include "Icons/IconProvider.h"
#include "ObjectNames.h"
#include "ProjectObjectCatalog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace War3Map::Modules {
namespace {
using MapData::WarVKInitTiming;
constexpr double kDefaultDistance = 300000, kDefaultNearClip = 4;
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
    return next;
}

void MapWarVKPage::Fill() {
    const auto read = project_.QueryWarVKMapSettings();
    const bool ok = read.Succeeded() && read.value;
    for (QWidget* widget : std::initializer_list<QWidget*>{limits_, distance_, near_, timing_, far_, removeFar_}) widget->setEnabled(ok);
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
