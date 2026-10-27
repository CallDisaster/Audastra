// 世界视图右侧属性栏：选中对象的可编辑属性（摆放坐标/朝向、区域边界、镜头目标）与地图概况。
// 修改经 MapData::PlacementEditing 提交到共用地图数据撤销栈，与视口拖动走同一条路径。
#include "WorldModule.h"
#include "WorldModelPreview.h"
#include "WorldPanelWidgets.h"
#include "WorldSelectionProperties.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include "ObjectNames.h"
#include "../Objects/ObjectViewEntries.h"
#include "Commands/MapDataCommands.h"
#include "Icons/IconProvider.h"
#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Modules/ModuleHost.h"
#include "Widgets/ElidedLabel.h"
#include "Widgets/Motion.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Render/EditorOverlay.h"
#include "War3MapCore/Render/ModelPicking.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QDoubleSpinBox>
#include <QComboBox>
#include <QByteArray>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

namespace War3Map::Modules {
namespace {

using WorldUi::SectionLabel;

constexpr int kIconSize = 36;
constexpr double kCoordinateLimit = 1000000.0; // 与 PlacementEditing 的坐标上限一致

Status ValidateWorldField(MapDataKind kind, std::string_view fieldId, float number,
    MapData::MapFieldType type = MapData::MapFieldType::Float32) {
    MapData::MapFieldValue value;
    value.type = type;
    value.floatBits = std::bit_cast<std::uint32_t>(number);
    return MapData::ValidateMapFieldValue(kind, fieldId, value);
}

QLabel* KeyLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("worldFieldLabel"));
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setMinimumWidth(0);
    return label;
}

QLabel* ValueLabel(QWidget* parent, const char* name = nullptr) {
    auto* label = new QLabel(QStringLiteral("—"), parent);
    label->setObjectName(name ? QLatin1String(name) : QLatin1String("worldFactValue"));
    label->setProperty("worldFact", true);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setMinimumWidth(0);
    auto policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    policy.setVerticalPolicy(QSizePolicy::Preferred);
    label->setSizePolicy(policy); // 保留 wordWrap 的 height-for-width，长原值不撑宽窄属性栏。
    return label;
}

QDoubleSpinBox* Field(QWidget* parent, const char* name, double step, int decimals, const QString& suffix = {}) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(QLatin1String(name));
    spin->setProperty("worldField", true);
    spin->setRange(-kCoordinateLimit, kCoordinateLimit);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    spin->setMinimumWidth(0);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spin->setFont(UiKit::MonoFont(12));
    return spin;
}

// QDoubleSpinBox 会按显示精度舍入来源值；失焦本身不能把舍入值当作用户修改。
bool ChangedField(const QDoubleSpinBox* field, double original) {
    if (!std::isfinite(original)) return false;
    const double scale = std::pow(10.0, field->decimals());
    const double visible = std::round(std::clamp(original, field->minimum(), field->maximum()) * scale) / scale;
    return std::abs(field->value() - visible) > 1e-9;
}

void SetPlacementField(QDoubleSpinBox* spin, double value, bool mixed, bool changed,
    std::uint64_t selectionRevision, std::uint64_t mapRevision, const QString& mapKey, std::uint64_t generation) {
    if (spin->hasFocus() && !changed) return; // 保留输入及其旧修订，提交时拒绝过期草稿。
    const QSignalBlocker blocker(spin);
    if (changed) spin->clearFocus();
    spin->setValue(value);
    spin->setProperty("worldMixed", mixed);
    spin->setProperty("worldEdited", false);
    spin->setProperty("r08SelectionRevision", QVariant::fromValue<qulonglong>(selectionRevision));
    spin->setProperty("r08MapRevision", QVariant::fromValue<qulonglong>(mapRevision));
    spin->setProperty("r08MapKey", mapKey);
    spin->setProperty("r08MapGeneration", QVariant::fromValue<qulonglong>(generation));
    auto* editor = spin->findChild<QLineEdit*>();
    editor->setPlaceholderText(mixed ? QObject::tr("多个值") : QString());
    if (mixed) spin->clear();
}

bool MatchesInspectorRevision(const QWidget* field, std::uint64_t selectionRevision, std::uint64_t mapRevision,
    const QString& mapKey, std::uint64_t generation) {
    return field->property("r08SelectionRevision").isValid() && field->property("r08MapRevision").isValid()
        && field->property("r08SelectionRevision").toULongLong() == selectionRevision
        && field->property("r08MapRevision").toULongLong() == mapRevision
        && field->property("r08MapKey").toString() == mapKey
        && field->property("r08MapGeneration").isValid() && field->property("r08MapGeneration").toULongLong() == generation;
}

QPixmap RegionSwatch(const Render::OverlayColor& color, qreal ratio) {
    QPixmap pixmap(QSize(kIconSize, kIconSize) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor base = QColor::fromRgbF(color[0], color[1], color[2]);
    QColor fill = base; fill.setAlphaF(0.22f);
    painter.setBrush(fill);
    painter.setPen(QPen(base, 1.5, Qt::DashLine));
    painter.drawRoundedRect(QRectF(5, 5, kIconSize - 10, kIconSize - 10), 5, 5);
    return pixmap;
}

double Degrees(double radians) {
    double degrees = qRadiansToDegrees(radians);
    degrees = std::fmod(degrees, 360.0);
    return degrees < 0 ? degrees + 360.0 : degrees;
}

QString TypeName(std::optional<ObjectData::ObjectType> type, bool unitFile) {
    if (!type) return unitFile ? QObject::tr("单位文件") : QObject::tr("装饰物文件");
    switch (*type) {
    case ObjectData::ObjectType::Unit: return QObject::tr("单位");
    case ObjectData::ObjectType::Item: return QObject::tr("物品");
    case ObjectData::ObjectType::Destructable: return QObject::tr("可破坏物");
    case ObjectData::ObjectType::Doodad: return QObject::tr("装饰物");
    default: return {};
    }
}

QString PlayerName(std::int32_t owner) {
    switch (owner) {
    case 12: return QObject::tr("中立敌对");
    case 13: return QObject::tr("中立受害");
    case 14: return QObject::tr("中立特殊");
    case 15: return QObject::tr("中立被动");
    default: return QObject::tr("玩家 %1").arg(owner + 1);
    }
}

std::optional<double> ObjectMaximum(const QtProject::EditorProject& project, const ObjectData::ObjectKey& key,
    std::initializer_list<std::string_view> fields) {
    // 英雄运行上限还包含属性和等级；不能把基础字段冒充已知最大值。
    if (QtApp::ResolveObjectViewClassification(project, key).hero != false) return std::nullopt;
    const auto chain = ResolveProjectObjectInheritance(project, key);
    if (!chain.IsComplete()) return std::nullopt;
    const auto value = ObjectFieldValue(chain, fields);
    bool ok = false;
    const double number = value && value->size() <= 64 ? QString::fromStdString(*value).toDouble(&ok) : 0;
    return ok && std::isfinite(number) && number >= 0 ? std::optional<double>{number} : std::nullopt;
}

void SetUnitField(QWidget* field, const QString& text, bool mixed, bool changed,
    std::uint64_t selectionRevision, std::uint64_t mapRevision, const QString& mapKey,
    std::uint64_t generation, std::uint64_t projectRevision) {
    if (field->hasFocus() && !changed) return;
    const QSignalBlocker blocker(field);
    if (changed) field->clearFocus();
    field->setProperty("worldEdited", false);
    field->setProperty("r08SelectionRevision", QVariant::fromValue<qulonglong>(selectionRevision));
    field->setProperty("r08MapRevision", QVariant::fromValue<qulonglong>(mapRevision));
    field->setProperty("r08MapKey", mapKey);
    field->setProperty("r08MapGeneration", QVariant::fromValue<qulonglong>(generation));
    field->setProperty("r08ProjectRevision", QVariant::fromValue<qulonglong>(projectRevision));
    if (auto* edit = qobject_cast<QLineEdit*>(field)) {
        edit->setText(mixed ? QString() : text);
        edit->setPlaceholderText(mixed ? QObject::tr("多个值") : QString());
    } else if (auto* combo = qobject_cast<QComboBox*>(field)) {
        combo->setCurrentIndex(mixed ? -1 : combo->findText(text));
    }
}

} // namespace

QWidget* WorldModule::BuildInspector() {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("worldInspector"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(240);
    inspectorBody_ = new QWidget;
    inspectorBody_->setProperty("worldPanel", true);
    auto* column = new QVBoxLayout(inspectorBody_);
    column->setContentsMargins(14, 12, 14, 16); column->setSpacing(10);

    // ── 选中对象 ──
    column->addWidget(SectionLabel(tr("选中"), inspectorBody_));
    selectionEmpty_ = new QLabel(tr("未选择"), inspectorBody_);
    selectionEmpty_->setObjectName(QStringLiteral("worldInspectorEmpty"));
    selectionEmpty_->setWordWrap(true);
    column->addWidget(selectionEmpty_);
    auto* card = new QFrame(inspectorBody_);
    card->setObjectName(QStringLiteral("worldSelectionCard"));
    selectionCard_ = card;
    auto* cardColumn = new QVBoxLayout(card);
    cardColumn->setContentsMargins(12, 12, 12, 12); cardColumn->setSpacing(10);
    auto* identity = new QHBoxLayout;
    identity->setSpacing(10);
    selectionIcon_ = new QLabel(card);
    selectionIcon_->setObjectName(QStringLiteral("worldSelectionIcon"));
    selectionIcon_->setFixedSize(kIconSize, kIconSize);
    selectionIcon_->setAlignment(Qt::AlignCenter);
    identity->addWidget(selectionIcon_, 0, Qt::AlignTop);
    auto* names = new QVBoxLayout;
    names->setSpacing(2);
    selectionTitle_ = new QLabel(card);
    selectionTitle_->setObjectName(QStringLiteral("worldSelectionTitle"));
    selectionTitle_->setWordWrap(true);
    selectionMeta_ = new QLabel(card);
    selectionMeta_->setObjectName(QStringLiteral("worldSelectionMeta"));
    selectionMeta_->setWordWrap(true);
    names->addWidget(selectionTitle_); names->addWidget(selectionMeta_);
    identity->addLayout(names, 1);
    cardColumn->addLayout(identity);
    // 与整图视口完全独立的单实例场景；只在选中单位时读取模型与显示。
    auto* unitPreview = new WorldModelPreview(card, backend_);
    cardColumn->addWidget(unitPreview);

    const auto grid = [card](QWidget*& holder) {
        holder = new QWidget(card);
        holder->setProperty("worldCard", true);
        auto* layout = new QGridLayout(holder);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setHorizontalSpacing(8); layout->setVerticalSpacing(6);
        layout->setColumnStretch(1, 1); layout->setColumnStretch(3, 1);
        return layout;
    };
    auto* placement = grid(placementFields_);
    fieldX_ = Field(placementFields_, "worldFieldX", 16, 1);
    fieldY_ = Field(placementFields_, "worldFieldY", 16, 1);
    fieldFacing_ = Field(placementFields_, "worldFieldFacing", 15, 1, QStringLiteral("°"));
    fieldFacing_->setRange(0, 360);
    fieldFacing_->setWrapping(true);
    fieldZ_ = new QLabel(placementFields_);
    fieldZ_->setObjectName(QStringLiteral("worldFieldZ"));
    fieldZ_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    fieldZ_->setWordWrap(true);
    fieldZ_->setFont(UiKit::MonoFont(12));
    fieldZ_->setToolTip(tr("随地形 · 只读；移动时保持与地表的相对高度"));
    placement->addWidget(KeyLabel(QStringLiteral("X"), placementFields_), 0, 0); placement->addWidget(fieldX_, 0, 1);
    placement->addWidget(KeyLabel(QStringLiteral("Y"), placementFields_), 0, 2); placement->addWidget(fieldY_, 0, 3);
    placement->addWidget(KeyLabel(tr("朝向"), placementFields_), 1, 0); placement->addWidget(fieldFacing_, 1, 1, 1, 3);
    placement->addWidget(KeyLabel(QStringLiteral("Z"), placementFields_), 2, 0); placement->addWidget(fieldZ_, 2, 1, 1, 3);
    auto* scale = ValueLabel(placementFields_, "worldSelectionScale");
    const auto quantityField = [this](const char* name, const char* fieldId) {
        auto* edit = new QLineEdit(placementFields_);
        edit->setObjectName(QLatin1String(name)); edit->setProperty("mapField", QLatin1String(fieldId));
        edit->setMinimumWidth(0); edit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        edit->setToolTip(tr("输入绝对值、0–100% 或默认；分别按各单位物编上限截断。\n生命按地图格式量化到整数百分比，魔法按整数绝对值保存。"));
        connect(edit, &QLineEdit::textEdited, this, [edit] { edit->setProperty("worldEdited", true); });
        connect(edit, &QLineEdit::editingFinished, this, [this, edit] { CommitUnitAttributeField(edit); });
        return edit;
    };
    auto* life = quantityField("worldSelectionLife", "hit_points");
    auto* mana = quantityField("worldSelectionMana", "mana");
    auto* level = new QLineEdit(placementFields_);
    level->setObjectName(QStringLiteral("worldSelectionLevel")); level->setProperty("mapField", "hero_level");
    level->setMinimumWidth(0); level->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    level->setToolTip(tr("英雄的初始等级（不小于 1 的整数）；只能在全部选中的单位都是英雄时修改"));
    connect(level, &QLineEdit::textEdited, this, [level] { level->setProperty("worldEdited", true); });
    connect(level, &QLineEdit::editingFinished, this, [this, level] { CommitUnitAttributeField(level); });
    auto* owner = new QComboBox(placementFields_);
    owner->setObjectName(QStringLiteral("worldSelectionOwner")); owner->setProperty("mapField", "owner");
    owner->setMinimumWidth(0); owner->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    owner->setPlaceholderText(tr("多个值"));
    for (int player = 0; player < 16; ++player) owner->addItem(PlayerName(player), player);
    connect(owner, &QComboBox::activated, this, [this, owner] {
        owner->setProperty("worldEdited", true); CommitUnitAttributeField(owner);
    });
    placement->addWidget(KeyLabel(tr("摆放缩放"), placementFields_), 3, 0, 1, 4);
    placement->addWidget(scale, 4, 0, 1, 4);
    auto* lifeKey = KeyLabel(tr("生命"), placementFields_); lifeKey->setObjectName(QStringLiteral("worldSelectionLifeLabel"));
    auto* manaKey = KeyLabel(tr("魔法"), placementFields_); manaKey->setObjectName(QStringLiteral("worldSelectionManaLabel"));
    auto* levelKey = KeyLabel(tr("英雄等级"), placementFields_); levelKey->setObjectName(QStringLiteral("worldSelectionLevelLabel"));
    auto* ownerKey = KeyLabel(tr("所属玩家"), placementFields_); ownerKey->setObjectName(QStringLiteral("worldSelectionOwnerLabel"));
    const auto quantityRow = [this, placement](QLabel* label, QLineEdit* edit, int row) {
        auto* reset = new QPushButton(tr("默认"), placementFields_);
        reset->setObjectName(edit->objectName() + QStringLiteral("Default"));
        reset->setToolTip(tr("继承物编默认值"));
        connect(reset, &QPushButton::clicked, this, [this, edit] {
            edit->setText(tr("默认")); edit->setProperty("worldEdited", true); CommitUnitAttributeField(edit);
        });
        placement->addWidget(label, row, 0); placement->addWidget(edit, row, 1, 1, 2); placement->addWidget(reset, row, 3);
    };
    quantityRow(lifeKey, life, 5); quantityRow(manaKey, mana, 6);
    placement->addWidget(levelKey, 7, 0); placement->addWidget(level, 7, 1, 1, 3);
    placement->addWidget(ownerKey, 8, 0); placement->addWidget(owner, 8, 1, 1, 3);
    scale->setTextInteractionFlags(Qt::TextSelectableByMouse);
    scale->setToolTip(tr("当前地图/物编的只读值，不是游戏运行状态"));
    cardColumn->addWidget(placementFields_);

    auto* region = grid(regionFields_);
    const std::array regionLabels{tr("左"), tr("下"), tr("右"), tr("上")};
    const std::array regionNames{"worldRegionLeft", "worldRegionBottom", "worldRegionRight", "worldRegionTop"};
    for (std::size_t index = 0; index < regionBounds_.size(); ++index) {
        regionBounds_[index] = Field(regionFields_, regionNames[index], 32, 1);
        regionBounds_[index]->setToolTip(tr("方向键步进 32；手动输入按原值保存"));
        const int row = static_cast<int>(index % 2), col = static_cast<int>(index / 2) * 2;
        region->addWidget(KeyLabel(regionLabels[index], regionFields_), row, col);
        region->addWidget(regionBounds_[index], row, col + 1);
    }
    cardColumn->addWidget(regionFields_);

    auto* camera = grid(cameraFields_);
    cameraX_ = Field(cameraFields_, "worldCameraX", 16, 1);
    cameraY_ = Field(cameraFields_, "worldCameraY", 16, 1);
    cameraDetails_ = new QLabel(cameraFields_);
    cameraDetails_->setObjectName(QStringLiteral("worldCameraDetails"));
    cameraDetails_->setWordWrap(true);
    camera->addWidget(KeyLabel(QStringLiteral("X"), cameraFields_), 0, 0); camera->addWidget(cameraX_, 0, 1);
    camera->addWidget(KeyLabel(QStringLiteral("Y"), cameraFields_), 0, 2); camera->addWidget(cameraY_, 0, 3);
    camera->addWidget(cameraDetails_, 1, 0, 1, 4);
    cardColumn->addWidget(cameraFields_);

    selectionNote_ = new QLabel(card);
    selectionNote_->setObjectName(QStringLiteral("worldSelectionNote"));
    selectionNote_->setWordWrap(true);
    cardColumn->addWidget(selectionNote_);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(6);
    openDefinition_ = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/arrow-right")), tr("物编定义"), card);
    openDefinition_->setObjectName(QStringLiteral("worldOpenDefinition"));
    openDefinition_->setProperty("worldAction", true);
    openDefinition_->setIconSize(QSize(14, 14));
    openDefinition_->setCursor(Qt::PointingHandCursor);
    openDefinition_->setToolTip(tr("在物体编辑器中打开此类型"));
    deleteSelected_ = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/trash-2")), tr("删除"), card);
    deleteSelected_->setObjectName(QStringLiteral("worldDeleteSelected"));
    deleteSelected_->setProperty("worldAction", true);
    deleteSelected_->setProperty("danger", true);
    deleteSelected_->setIconSize(QSize(14, 14));
    deleteSelected_->setCursor(Qt::PointingHandCursor);
    deleteSelected_->setToolTip(tr("删除选中的摆放（Delete），可撤销"));
    actions->addWidget(openDefinition_, 1); actions->addWidget(deleteSelected_, 1);
    cardColumn->addLayout(actions);
    auto* properties = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/settings-2")), tr("完整属性"), card);
    properties->setObjectName(QStringLiteral("worldAuxiliaryInspectorProperties"));
    cardColumn->addWidget(properties);
    connect(properties, &QPushButton::clicked, this, &WorldModule::EditSelectedAuxiliary);
    card->hide();
    column->addWidget(card);
    column->addSpacing(6);

    // ── 地图概况 ──
    column->addWidget(SectionLabel(tr("地图"), inspectorBody_));
    auto* facts = new QGridLayout;
    facts->setHorizontalSpacing(12); facts->setVerticalSpacing(7);
    facts->setColumnStretch(1, 1);
    int row = 0;
    const auto fact = [&](const QString& key, QLabel*& value, const char* name = nullptr) {
        value = ValueLabel(inspectorBody_, name);
        auto* label = KeyLabel(key, inspectorBody_);
        facts->addWidget(label, row++, 0, 1, 2, Qt::AlignTop);
        facts->addWidget(value, row++, 0, 1, 2);
    };
    fact(tr("地形"), sceneTerrain_, "worldSceneTerrain");
    fact(tr("地表"), sceneGround_, "worldSceneGround");
    {
        auto* water = new QWidget(inspectorBody_);
        water->setProperty("worldPanel", true);
        auto* waterRow = new QVBoxLayout(water);
        waterRow->setContentsMargins(0, 0, 0, 0); waterRow->setSpacing(6);
        sceneWater_ = ValueLabel(water, "worldSceneWater");
        waterFrame_ = new QSpinBox(water);
        waterFrame_->setObjectName(QStringLiteral("worldWaterFrame"));
        waterFrame_->setPrefix(tr("帧 "));
        waterFrame_->setRange(0, 0);
        waterFrame_->setEnabled(false);
        waterFrame_->hide();
        waterFrame_->setToolTip(tr("按 Water.slk 已声明的帧序号预览。只改变当前画面，不修改地图或保存内容。"));
        waterRow->addWidget(sceneWater_); waterRow->addWidget(waterFrame_);
        facts->addWidget(KeyLabel(tr("水面"), inspectorBody_), row++, 0, 1, 2, Qt::AlignTop);
        facts->addWidget(water, row++, 0, 1, 2);
    }
    fact(tr("悬崖"), sceneCliffs_, "worldSceneCliffs");
    fact(tr("光照"), sceneLighting_, "worldSceneLighting");
    fact(tr("模型"), modelStatus_, "worldModelStatus");
    column->addLayout(facts);
    sceneNotes_ = new QLabel(inspectorBody_);
    sceneNotes_->setObjectName(QStringLiteral("worldSceneNotes"));
    sceneNotes_->setWordWrap(true);
    sceneNotes_->hide();
    column->addWidget(sceneNotes_);
    column->addSpacing(6);

    // ── 地面点（只读拾取） ──
    column->addWidget(SectionLabel(tr("地面点"), inspectorBody_));
    groundPoint_ = new QLabel(viewport_->SelectionText(), inspectorBody_);
    groundPoint_->setObjectName(QStringLiteral("worldSelectedCoordinates"));
    groundPoint_->setWordWrap(true);
    groundPoint_->setFont(UiKit::MonoFont());
    groundPoint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    groundPoint_->setToolTip(tr("单击地面取得的坐标与格信息，只读，不修改地图。"));
    column->addWidget(groundPoint_);
    column->addStretch(1);
    scroll->setWidget(inspectorBody_);

    for (auto* spin : {fieldX_, fieldY_, fieldFacing_}) {
        connect(spin->findChild<QLineEdit*>(), &QLineEdit::textEdited, spin, [spin] { spin->setProperty("worldEdited", true); });
        connect(spin, &QDoubleSpinBox::valueChanged, spin, [spin] { spin->setProperty("worldEdited", true); });
        connect(spin, &QDoubleSpinBox::editingFinished, this, &WorldModule::CommitPlacementFields);
    }
    for (auto* spin : regionBounds_) connect(spin, &QDoubleSpinBox::editingFinished, this, &WorldModule::CommitRegionFields);
    for (auto* spin : {cameraX_, cameraY_}) connect(spin, &QDoubleSpinBox::editingFinished, this, &WorldModule::CommitCameraFields);
    connect(openDefinition_, &QPushButton::clicked, this, &WorldModule::OpenSelectedDefinition);
    connect(deleteSelected_, &QPushButton::clicked, this, &WorldModule::DeleteSelection);
    connect(waterFrame_, &QSpinBox::valueChanged, this, [this](int frame) {
        if (frame >= 0) QueueWaterFrame(static_cast<std::uint32_t>(frame));
    });
    return scroll;
}

void WorldModule::UpdateInspector(bool animate) {
    if (!selectionCard_) return;
    UpdateAuxiliaryActions();
    if (auto* properties = page_->findChild<QPushButton*>(QStringLiteral("worldAuxiliaryInspectorProperties")))
        properties->setVisible(selectedRegion_.has_value() || selectedCamera_.has_value());
    auto* unitPreview = selectionCard_->findChild<WorldModelPreview*>(QStringLiteral("worldUnitModelPreview"));
    const auto* region = SelectedRegion();
    const auto* camera = SelectedCamera();
    const auto request = SelectedRequest();
    const auto document = CurrentDocument();
    const auto editStatus = worldSelection_.Version()
        ? worldSelection_.EditStatus(SelectionContext(), *worldSelection_.Version()) : Status{};
    const auto readOnlyNote = editStatus.Succeeded() ? tr("不能一起编辑")
        : tr("只读：%1").arg(WorldDiagnosticText(editStatus.diagnostics));
    const auto snapshot = worldSelection_.Version()
        ? worldSelection_.Snapshot(SelectionContext(), *worldSelection_.Version()) : Result<Services::WorldSelectionSnapshot>{};
    if (worldSelection_.Version() && !snapshot.Succeeded()) {
        selectionCard_->hide(); selectionEmpty_->show();
        selectionEmpty_->setText(tr("选择属性不可用：%1").arg(WorldDiagnosticText(snapshot.diagnostics)));
        inspectorKey_.clear();
        if (unitPreview) unitPreview->ClearSelection();
        return;
    }
    const bool mixedSelection = snapshot.Succeeded() && snapshot.value->selected.size() > 1
        && std::any_of(snapshot.value->selected.begin(), snapshot.value->selected.end(),
            [](const auto& id) { return !std::holds_alternative<MapData::PlacementId>(id); });
    if (mixedSelection) {
        const auto hex = [](const std::string& bytes) {
            return QString::fromLatin1(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())).toHex());
        };
        const auto& selected = snapshot.value->selected;
        const auto& version = snapshot.value->version;
        QString key = QStringLiteral("mixed:%1:%2:%3:%4:%5:%6;")
            .arg(hex(version.document.documentId)).arg(hex(version.document.sourceSha256))
            .arg(version.document.generation).arg(version.document.revision)
            .arg(version.catalogRevision).arg(version.editorRevision);
        std::size_t placements = 0, regions = 0, cameras = 0;
        const auto firstFlags = worldSelection_.Flags(selected.front());
        bool sameVisible = true, sameLocked = true;
        for (const auto& id : selected) {
            if (const auto* placement = std::get_if<MapData::PlacementId>(&id)) {
                ++placements;
                key += QStringLiteral("p:%1:%2:%3:%4;").arg(static_cast<int>(placement->file)).arg(placement->index)
                    .arg(hex(placement->rawcode)).arg(placement->creationId);
            } else if (const auto* regionId = std::get_if<MapData::RegionId>(&id)) {
                ++regions;
                key += QStringLiteral("r:%1:%2:%3;").arg(regionId->index).arg(hex(regionId->name)).arg(regionId->creationNumber);
            } else if (const auto* cameraId = std::get_if<MapData::CameraId>(&id)) {
                ++cameras;
                key += QStringLiteral("c:%1:%2;").arg(cameraId->index).arg(hex(cameraId->name));
            }
            const auto flags = worldSelection_.Flags(id);
            sameVisible = sameVisible && flags.visible == firstFlags.visible;
            sameLocked = sameLocked && flags.locked == firstFlags.locked;
        }
        const bool changed = key != inspectorKey_; inspectorKey_ = key;
        selectionEmpty_->hide(); selectionCard_->show();
        placementFields_->hide(); regionFields_->hide(); cameraFields_->hide();
        openDefinition_->hide(); deleteSelected_->hide();
        if (unitPreview) unitPreview->ClearSelection();
        selectionTitle_->setText(tr("已选 %1 个对象").arg(selected.size()));
        QStringList counts;
        if (placements) counts << tr("摆放 %1").arg(placements);
        if (regions) counts << tr("区域 %1").arg(regions);
        if (cameras) counts << tr("镜头 %1").arg(cameras);
        const auto visible = !sameVisible ? tr("多个值") : firstFlags.visible ? tr("可见") : tr("隐藏");
        const auto locked = !sameLocked ? tr("多个值") : firstFlags.locked ? tr("已锁定") : tr("未锁定");
        selectionMeta_->setText(counts.join(QStringLiteral(" · ")) + QStringLiteral("\n")
            + tr("显隐：%1 · 锁定：%2 · 只读").arg(visible, locked));
        selectionIcon_->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/layers"))
            .pixmap(QSize(kIconSize, kIconSize), selectionIcon_->devicePixelRatioF()));
        selectionIcon_->setProperty("framed", false);
        selectionNote_->setText(readOnlyNote); selectionNote_->show();
        if (changed) {
            selectionIcon_->style()->unpolish(selectionIcon_); selectionIcon_->style()->polish(selectionIcon_);
        }
        if (changed || animate) UiKit::FadeIn(selectionCard_, 170, 0.1);
        return;
    }
    selectionMeta_->setToolTip(QString());
    auto properties = document && !selectedPlacements_.empty()
        ? CaptureWorldPlacementProperties(*document, selectedPlacements_) : Result<WorldPlacementProperties>{};
    const bool placements = properties.Succeeded();
    QString key;
    if (placements) {
        for (const auto& id : selectedPlacements_)
            key += QStringLiteral("p:%1:%2:%3:%4;").arg(static_cast<int>(id.file)).arg(id.index)
                .arg(QString::fromLatin1(QByteArray(id.rawcode.data(), static_cast<qsizetype>(id.rawcode.size())).toHex()))
                .arg(id.creationId);
    }
    else if (region) key = QStringLiteral("r:%1").arg(selectedRegion_->index);
    else if (camera) key = QStringLiteral("c:%1").arg(selectedCamera_->index);
    const bool changed = key != inspectorKey_;
    inspectorKey_ = key;
    selectionEmpty_->setVisible(key.isEmpty());
    selectionCard_->setVisible(!key.isEmpty());
    placementFields_->setVisible(placements);
    regionFields_->setVisible(!placements && region != nullptr);
    cameraFields_->setVisible(!placements && camera != nullptr);
    if (key.isEmpty()) {
        selectionEmpty_->setText(!selectedPlacements_.empty() ? tr("摆放属性不可用：%1").arg(WorldDiagnosticText(properties.diagnostics))
            : tr("未选择"));
        if (unitPreview) unitPreview->ClearSelection();
        return;
    }
    selectionTitle_->setText(selectionName_);
    const qreal ratio = selectionIcon_->devicePixelRatioF();
    if (placements) {
        const auto primary = selection_ ? std::find(selectedPlacements_.begin(), selectedPlacements_.end(), *selection_)
            : selectedPlacements_.begin();
        const auto index = primary == selectedPlacements_.end() ? std::size_t{0}
            : static_cast<std::size_t>(primary - selectedPlacements_.begin());
        const auto& id = selectedPlacements_[index];
        const bool unit = id.file == MapData::PlacementFile::Units;
        const bool single = selectedPlacements_.size() == 1;
        const bool hasStart = std::any_of(selectedPlacements_.begin(), selectedPlacements_.end(),
            [](const auto& value) { return value.file == MapData::PlacementFile::Units && value.rawcode == "sloc"; });
        const auto& original = properties.value->transforms[index];
        auto* scale = placementFields_->findChild<QLabel*>(QStringLiteral("worldSelectionScale"));
        auto* life = placementFields_->findChild<QLineEdit*>(QStringLiteral("worldSelectionLife"));
        auto* mana = placementFields_->findChild<QLineEdit*>(QStringLiteral("worldSelectionMana"));
        auto* level = placementFields_->findChild<QLineEdit*>(QStringLiteral("worldSelectionLevel"));
        auto* ownerValue = placementFields_->findChild<QComboBox*>(QStringLiteral("worldSelectionOwner"));
        const bool finiteScale = std::all_of(properties.value->transforms.begin(), properties.value->transforms.end(), [](const auto& value) {
            return std::all_of(value.scale.begin(), value.scale.end(), [](float v) { return std::isfinite(v); });
        });
        const bool sameScale = std::all_of(properties.value->transforms.begin(), properties.value->transforms.end(),
            [&](const auto& value) { return value.scale == original.scale; });
        scale->setText(!finiteScale ? tr("不可用：非有限缩放") : !sameScale ? tr("多个值 · 只读")
            : tr("%1 × %2 × %3").arg(original.scale[0],0,'g',6).arg(original.scale[1],0,'g',6).arg(original.scale[2],0,'g',6));
        std::array<QStringList, 4> storedValues; // 生命、魔法、英雄等级、所属玩家。
        bool unknownType = false;
        bool allUnits = true;
        bool allHeroes = true;
        for (std::size_t selectedIndex = 0; selectedIndex < selectedPlacements_.size(); ++selectedIndex) {
            const auto& selected = selectedPlacements_[selectedIndex];
            const bool unitFile = selected.file == MapData::PlacementFile::Units;
            if (unitFile && selected.rawcode == "sloc") { allUnits = false; allHeroes = false; continue; }
            const auto type = PlacementType(project_, selected.rawcode,
                unitFile ? ObjectData::ObjectType::Unit : ObjectData::ObjectType::Doodad,
                unitFile ? ObjectData::ObjectType::Item : ObjectData::ObjectType::Destructable);
            if (!type) { unknownType = true; allUnits = false; allHeroes = false; continue; }
            const auto& facts = properties.value->facts[selectedIndex];
            if (*type == ObjectData::ObjectType::Unit) {
                const ObjectData::ObjectKey objectKey{*type, selected.rawcode};
                if (facts.hitPoints) {
                    const auto maximum = ObjectMaximum(project_, objectKey, {"uhpm", "hp"});
                    storedValues[0].push_back(*facts.hitPoints == -1 ? tr("默认")
                        : maximum && *facts.hitPoints >= 0 && *facts.hitPoints <= 100
                            ? QString::number(*maximum * *facts.hitPoints / 100.0, 'g', 12)
                            : tr("%1%").arg(*facts.hitPoints));
                }
                if (facts.mana) storedValues[1].push_back(*facts.mana == -1 ? tr("默认") : QString::number(*facts.mana));
                if (facts.owner) storedValues[3].push_back(*facts.owner >= 0 && *facts.owner <= 15
                    ? PlayerName(*facts.owner) : tr("不可用：玩家原值 %1").arg(*facts.owner));
                const auto classification = QtApp::ResolveObjectViewClassification(project_, objectKey);
                if (classification.hero == true && facts.heroLevel) storedValues[2].push_back(QString::number(*facts.heroLevel));
                else allHeroes = false;
                if (!classification.hero.has_value()) unknownType = true;
            } else {
                allUnits = false;
                allHeroes = false;
                if (*type == ObjectData::ObjectType::Destructable && facts.lifePercent)
                    storedValues[0].push_back(*facts.lifePercent <= 100 ? tr("%1%（摆放原值）").arg(*facts.lifePercent)
                        : tr("不可用：摆放原值 %1").arg(*facts.lifePercent));
            }
        }
        const std::array<QWidget*, 4> factLabels{life, mana, level, ownerValue};
        const std::array labelNames{"worldSelectionLifeLabel", "worldSelectionManaLabel", "worldSelectionLevelLabel", "worldSelectionOwnerLabel"};
        for (std::size_t factIndex = 0; factIndex < storedValues.size(); ++factIndex) {
            auto* field = factLabels[factIndex]; const auto& values = storedValues[factIndex];
            field->setVisible(!values.empty());
            placementFields_->findChild<QLabel*>(QLatin1String(labelNames[factIndex]))->setVisible(!values.empty());
            const bool enabled = (factIndex == 2 ? allHeroes : allUnits) && SelectionEditable() && project_.mapEdits_;
            if (factIndex < 2) {
                auto* reset = placementFields_->findChild<QPushButton*>(field->objectName() + QStringLiteral("Default"));
                reset->setVisible(!values.empty()); reset->setEnabled(enabled);
            }
            if (values.empty()) continue;
            const bool mixed = std::any_of(values.begin(), values.end(), [&](const auto& value) { return value != values.front(); });
            QString text = mixed ? tr("多个值") : values.front();
            if (static_cast<std::size_t>(values.size()) != selectedPlacements_.size())
                text += tr(" · %1/%2 个适用").arg(values.size()).arg(selectedPlacements_.size());
            field->setEnabled(enabled);
            SetUnitField(field, text, mixed, changed, SelectionRevision(),
                project_.mapEdits_ ? project_.mapEdits_->Revision() : 0, MapKey(), generation_, project_.editRevision_.Revision());
        }
        const auto& position = single && dragPosition_ ? *dragPosition_ : original.position;
        const auto selectionRevision = SelectionRevision();
        const auto mapRevision = project_.mapEdits_ ? project_.mapEdits_->Revision() : 0;
        SetPlacementField(fieldX_, position[0], !properties.value->Same(WorldPlacementProperty::X), changed, selectionRevision, mapRevision, MapKey(), generation_);
        SetPlacementField(fieldY_, position[1], !properties.value->Same(WorldPlacementProperty::Y), changed, selectionRevision, mapRevision, MapKey(), generation_);
        SetPlacementField(fieldFacing_, Degrees(single && dragRotation_ ? *dragRotation_ : original.rotation),
            !properties.value->Same(WorldPlacementProperty::Facing), changed, selectionRevision, mapRevision, MapKey(), generation_);
        const bool editable = SelectionEditable() && project_.mapEdits_ && terrain_;
        for (auto* field : {fieldX_, fieldY_, fieldFacing_}) field->setEnabled(editable);
        if (hasStart) fieldFacing_->setEnabled(false);
        fieldZ_->setText(tr("随地形 · 只读"));
        fieldZ_->setToolTip(single ? tr("Z：%1\n移动时保持与地表的相对高度").arg(position[2], 0, 'f', 1)
            : tr("每个摆放分别保持与地表的相对高度；手工高度只读"));
        selectionTitle_->setText(single ? selectionName_ : tr("已选 %1 个摆放").arg(selectedPlacements_.size()));
        QStringList meta = single ? QStringList{QString::fromUtf8(id.rawcode), TypeName(selectionType_, unit)} : QStringList{};
        selectionMeta_->setText(meta.join(QStringLiteral(" · ")));
        if (single && hasStart) selectionMeta_->setText(tr("起始位置 · %1").arg(selectionOwner_ ? PlayerName(*selectionOwner_) : tr("玩家未知")));
        if (changed) {
            QIcon icon;
            if (!selectionIconPath_.isEmpty() && project_.iconProvider_) icon = project_.iconProvider_->IconForTexture(selectionIconPath_);
            if (icon.isNull()) icon = UiKit::SvgIcon(unit ? QStringLiteral("lucide/box") : QStringLiteral("lucide/trees"));
            selectionIcon_->setPixmap(icon.pixmap(QSize(kIconSize, kIconSize), ratio));
            selectionIcon_->setProperty("framed", !selectionIconPath_.isEmpty());
        }
        if (unitPreview) {
            if (!unit || !single || !request) unitPreview->ClearSelection();
            else {
                const auto& shown = requests_[*request];
                const auto mapKey = MapKey();
                const auto editRevision = project_.mapEdits_ ? project_.mapEdits_->Revision() : 0;
                if (!unitPreview->Matches(mapKey, generation_, editRevision, id, shown)) {
                    const auto previewDocument = CurrentDocument();
                    const auto valid = previewDocument && previewDocument->Units() && id.index < previewDocument->Units()->units.size() &&
                        MapData::MakePlacementId(MapData::PlacementFile::Units, id.index,
                            previewDocument->Units()->units[id.index]) == id;
                    if (!valid || shown.modelPath.empty()) {
                        unitPreview->ShowUnavailable(mapKey, generation_, editRevision, id, shown,
                            !valid ? tr("选中单位的修订已过期，预览未读取") : tr("此单位没有可用模型路径"));
                    } else {
                        auto rowKey = MapData::MapFieldRowKey(*previewDocument, MapDataKind::Units, id.index);
                        if (!rowKey.Succeeded())
                            unitPreview->ShowUnavailable(mapKey, generation_, editRevision, id, shown,
                                tr("无法确认单位行标识，预览未读取"));
                        else {
                            const auto expectedRowKey = *rowKey.value;
                            const auto expectedModelPath = shown.modelPath;
                            const auto expectedGeneration = generation_;
                            unitPreview->ShowSelection(mapKey, generation_, editRevision, id,
                                std::move(*rowKey.value), shown, CaptureWorldTerrainSources(project_),
                                [this, mapKey, id, expectedRowKey, expectedModelPath, expectedGeneration, editRevision] {
                                    if (MapKey() != mapKey || generation_ != expectedGeneration ||
                                        (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0) != editRevision ||
                                        !selection_ || *selection_ != id) return false;
                                    const auto request = SelectedRequest();
                                    if (!request || requests_[*request].modelPath != expectedModelPath) return false;
                                    const auto current = CurrentDocument();
                                    if (!current || !current->Units() || id.index >= current->Units()->units.size() ||
                                        MapData::MakePlacementId(MapData::PlacementFile::Units, id.index,
                                            current->Units()->units[id.index]) != id) return false;
                                    const auto key = MapData::MapFieldRowKey(*current, MapDataKind::Units, id.index);
                                    return key.Succeeded() && *key.value == expectedRowKey;
                                });
                        }
                    }
                }
            }
        }
        openDefinition_->setVisible(single && selectionType_.has_value() && host_ != nullptr);
        deleteSelected_->setVisible(true);
        deleteSelected_->setEnabled(editable && !hasStart);
        deleteSelected_->setToolTip(hasStart ? tr("起始位置绑定玩家设置，不能直接删除") : tr("删除选中的摆放（Delete），可撤销"));
        selectionNote_->setText(!SelectionEditable() ? readOnlyNote
            : !terrain_ ? tr("地形未读取") : !single ? QString()
            : !request || !InstanceOfRequest(*request) ? tr("模型未读取") : QString());
        if (unknownType) selectionNote_->setText(selectionNote_->text()
            + (selectionNote_->text().isEmpty() ? QString() : QStringLiteral("\n"))
            + tr("部分类型未确认"));
        if (single && hasStart && SelectionEditable())
            selectionNote_->setText(viewport_ && viewport_->HasOverlaySupport()
                ? tr("拖动标记或修改 X、Y 会同步玩家起点。") : tr("当前渲染插件不支持起始位置标记；可在此修改 X、Y。"));
        selectionNote_->setVisible(!selectionNote_->text().isEmpty());
    } else if (region) {
        if (unitPreview) unitPreview->ClearSelection();
        const auto b = regionDraft_ ? *regionDraft_ : std::array<float, 4>{region->left, region->bottom, region->right, region->top};
        for (std::size_t index = 0; index < regionBounds_.size(); ++index) {
            SetPlacementField(regionBounds_[index], b[index], false, changed, SelectionRevision(),
                project_.mapEdits_ ? project_.mapEdits_->Revision() : 0, MapKey(), generation_);
            regionBounds_[index]->setEnabled(SelectionEditable());
        }
        selectionMeta_->setText(tr("区域 · %1 × %2").arg(b[2] - b[0], 0, 'f', 0).arg(b[3] - b[1], 0, 'f', 0));
        if (changed) selectionIcon_->setPixmap(RegionSwatch(Render::RegionOverlayColor(*region), ratio));
        selectionIcon_->setProperty("framed", false);
        selectionNote_->setText(SelectionEditable() ? QString() : readOnlyNote);
        selectionNote_->setVisible(!selectionNote_->text().isEmpty());
    } else {
        if (unitPreview) unitPreview->ClearSelection();
        const float x = cameraDraft_ ? (*cameraDraft_)[0] : camera->targetX;
        const float y = cameraDraft_ ? (*cameraDraft_)[1] : camera->targetY;
        for (const auto& [spin, value] : {std::pair{cameraX_, x}, std::pair{cameraY_, y}}) {
            SetPlacementField(spin, value, false, changed, SelectionRevision(),
                project_.mapEdits_ ? project_.mapEdits_->Revision() : 0, MapKey(), generation_);
            spin->setEnabled(SelectionEditable());
        }
        selectionMeta_->setText(tr("镜头 · 距离 %1").arg(camera->targetDistance, 0, 'f', 0));
        cameraDetails_->setText(tr("俯角 %1° · 旋转 %2° · 视野 %3° · 远裁剪 %4")
            .arg(camera->angleOfAttack, 0, 'f', 0).arg(camera->rotation, 0, 'f', 0)
            .arg(camera->fieldOfView, 0, 'f', 0).arg(camera->farClippingPlane, 0, 'f', 0));
        if (changed) selectionIcon_->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/target"), QColor(0xf2, 0xc7, 0x52))
            .pixmap(QSize(22, 22), ratio));
        selectionIcon_->setProperty("framed", false);
        selectionNote_->setText(SelectionEditable() ? QString() : readOnlyNote);
        selectionNote_->setVisible(!selectionNote_->text().isEmpty());
    }
    if (!placements) {
        openDefinition_->hide();
        deleteSelected_->setVisible(region || camera);
        deleteSelected_->setEnabled(SelectionEditable());
        deleteSelected_->setToolTip(tr("删除区域或镜头；仍被引用或引用范围不完整时拒绝，可撤销"));
        selectionNote_->setVisible(!selectionNote_->text().isEmpty());
    }
    if (changed) {
        selectionIcon_->style()->unpolish(selectionIcon_);
        selectionIcon_->style()->polish(selectionIcon_);
    }
    if (changed || animate) UiKit::FadeIn(selectionCard_, 170, 0.1);
}

void WorldModule::CommitUnitAttributeField(QWidget* field) {
    if (!field || !field->isEnabled() || !field->property("worldEdited").toBool() || !project_.mapEdits_ || dragStart_) return;
    const auto reset = [this, field] {
        const QSignalBlocker blocker(field); field->setProperty("worldEdited", false);
        field->clearFocus(); inspectorKey_.clear(); UpdateInspector();
    };
    if (!MatchesInspectorRevision(field, SelectionRevision(), project_.mapEdits_->Revision(), MapKey(), generation_)
        || field->property("r08ProjectRevision").toULongLong() != project_.editRevision_.Revision() || !SelectionEditable()) {
        selectionStatus_->setText(tr("选择、地图或物编已变化，请重新编辑属性")); reset(); return;
    }
    const auto document = CurrentDocument();
    if (!document || selectedPlacements_.empty()) return;
    const auto captured = CaptureWorldPlacementProperties(*document, selectedPlacements_);
    if (!captured.Succeeded()) { selectionStatus_->setText(WorldDiagnosticText(captured.diagnostics)); reset(); return; }
    const auto fieldId = field->property("mapField").toString().toStdString();
    const auto* descriptor = MapData::FindMapField(MapDataKind::Units, fieldId);
    if (!descriptor) return;
    QString input;
    if (const auto* edit = qobject_cast<QLineEdit*>(field)) input = edit->text().trimmed();
    else if (const auto* combo = qobject_cast<QComboBox*>(field); combo && combo->currentIndex() >= 0)
        input = QString::number(combo->currentData().toInt());
    else return;
    if (input == tr("默认")) input = QStringLiteral("default");
    std::vector<MapData::MapFieldEdit> edits; edits.reserve(selectedPlacements_.size());
    std::size_t runtimeClamped = 0;
    for (const auto& selected : selectedPlacements_) {
        const auto type = selected.file == MapData::PlacementFile::Units
            ? PlacementType(project_, selected.rawcode, ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item) : std::nullopt;
        if (type != ObjectData::ObjectType::Unit) {
            selectionStatus_->setText(tr("此属性只适用于单位，请仅选择单位")); reset(); return;
        }
        if (fieldId == "hero_level"
            && QtApp::ResolveObjectViewClassification(project_, {*type, selected.rawcode}).hero != true) {
            selectionStatus_->setText(tr("英雄等级只适用于英雄，请仅选择英雄")); reset(); return;
        }
        const auto rowKey = MapData::MapFieldRowKey(*document, MapDataKind::Units, selected.index);
        const auto before = MapData::ReadMapField(*document, MapDataKind::Units, selected.index, *descriptor);
        MapData::MapFieldQuantityInput quantity;
        if (descriptor->quantity != MapData::MapFieldQuantity::None)
            quantity.maximum = ObjectMaximum(project_, {*type, selected.rawcode}, fieldId == "hit_points"
                ? std::initializer_list<std::string_view>{"uhpm", "hp"}
                : std::initializer_list<std::string_view>{"umpm", "manan", "mana"});
        const auto after = MapData::ParseMapFieldInput(*descriptor, input.toStdString(), quantity);
        if (!rowKey.Succeeded() || !before.Succeeded() || !after.Succeeded()) {
            selectionStatus_->setText(tr("修改未应用：%1").arg(WorldDiagnosticText(!rowKey.Succeeded() ? rowKey.diagnostics
                : !before.Succeeded() ? before.diagnostics : after.diagnostics)));
            reset(); return;
        }
        if (*before.value != *after.value) {
            edits.push_back({MapDataKind::Units, selected.index, *rowKey.value, fieldId, *before.value, *after.value});
            if (std::any_of(after.diagnostics.begin(), after.diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == "MAP_FIELD_RUNTIME_CLAMP"; })) ++runtimeClamped;
        }
    }
    if (edits.empty()) { reset(); return; }
    // 一次选区操作以实际选区规模为预算；不是实例总数上限，也不截断选区。
    const auto planned = MapData::ApplyMapFieldEdits(*document, edits, selectedPlacements_.size());
    if (!planned.Succeeded()) { selectionStatus_->setText(WorldDiagnosticText(planned.diagnostics)); reset(); return; }
    field->setProperty("worldEdited", false); // 同步刷新/失焦不能重入同一次提交。
    const auto status = project_.mapEdits_->EditUnits(*planned.value->units);
    if (!status.Succeeded()) { project_.AddDiagnostics(status.diagnostics);
        selectionStatus_->setText(tr("修改未应用：%1").arg(WorldDiagnosticText(status.diagnostics))); reset(); return; }
    const auto description = std::string("修改摆放组属性 ") + fieldId;
    (void)project_.ObserveMapDataEdit(description);
    QtProject::PushMapDataEdit(project_, tr("修改 %1 个单位的%2").arg(edits.size()).arg(QString::fromUtf8(descriptor->label)), description);
    reset();
    selectionStatus_->setText(fieldId == "hit_points" ? tr("已修改 %1 个单位；生命按整数百分比存储，可一起撤销").arg(edits.size())
        : tr("已修改 %1 个单位，可一起撤销").arg(edits.size()));
    if (runtimeClamped)
        selectionStatus_->setText(selectionStatus_->text()
            + tr("；其中 %1 个保存绝对魔法原值，上限未知，将在游戏初始化时按当时上限截断").arg(runtimeClamped));
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

void WorldModule::CommitPlacementFields() {
    auto* field = qobject_cast<QDoubleSpinBox*>(sender());
    if (!field || (field != fieldX_ && field != fieldY_ && field != fieldFacing_)) return;
    if (!field->property("worldEdited").toBool()) {
        const QSignalBlocker blocker(field); field->clearFocus(); inspectorKey_.clear(); UpdateInspector();
        return; // 仅失焦不修改混合值；恢复原显示，避免spin把空文本修正成主选值。
    }
    if (!project_.mapEdits_ || !terrain_ || dragStart_) return;
    if (!MatchesInspectorRevision(field, SelectionRevision(), project_.mapEdits_->Revision(), MapKey(), generation_) || !SelectionEditable()) {
        selectionStatus_->setText(tr("选择或地图已变化，请重新编辑属性"));
        const QSignalBlocker blocker(field); field->clearFocus(); inspectorKey_.clear(); UpdateInspector();
        return;
    }
    const auto document = CurrentDocument();
    if (!document) return;
    auto captured = CaptureWorldPlacementProperties(*document, selectedPlacements_);
    if (!captured.Succeeded()) {
        selectionStatus_->setText(WorldDiagnosticText(captured.diagnostics));
        return;
    }
    const auto property = field == fieldX_ ? WorldPlacementProperty::X
        : field == fieldY_ ? WorldPlacementProperty::Y : WorldPlacementProperty::Facing;
    const auto& first = captured.value->transforms.front();
    const double sourceValue = property == WorldPlacementProperty::X ? first.position[0]
        : property == WorldPlacementProperty::Y ? first.position[1] : Degrees(first.rotation);
    if (captured.value->Same(property) && !ChangedField(field, sourceValue)) return;
    const float value = static_cast<float>(property == WorldPlacementProperty::Facing
        ? qDegreesToRadians(field->value()) : field->value());
    if (property == WorldPlacementProperty::Facing && std::any_of(selectedPlacements_.begin(), selectedPlacements_.end(),
        [](const auto& id) { return id.file == MapData::PlacementFile::Units; })) {
        const auto checked = ValidateWorldField(MapDataKind::Units, "facing", value,
            MapData::MapFieldType::AngleRadians);
        if (!checked.Succeeded()) {
            selectionStatus_->setText(tr("修改未保存：%1").arg(WorldDiagnosticText(checked.diagnostics)));
            const QSignalBlocker blocker(field); field->clearFocus(); inspectorKey_.clear(); UpdateInspector();
            return;
        }
    }
    const auto planned = PlanWorldPlacementProperty(*document, *terrain_, selectedPlacements_, property, value);
    if (!planned.Succeeded()) {
        selectionStatus_->setText(WorldDiagnosticText(planned.diagnostics));
        const QSignalBlocker blocker(field); field->clearFocus(); inspectorKey_.clear(); UpdateInspector();
        return;
    }
    const auto beforeRevision = project_.mapEdits_->Revision();
    const auto status = MapData::MovePlacements(*project_.mapEdits_, *planned.value);
    if (!status.Succeeded()) {
        project_.AddDiagnostics(status.diagnostics);
        selectionStatus_->setText(tr("修改未保存：%1").arg(WorldDiagnosticText(status.diagnostics)));
        inspectorKey_.clear(); UpdateInspector();
        return;
    }
    if (project_.mapEdits_->Revision() == beforeRevision) return;
    const bool moved = property != WorldPlacementProperty::Facing;
    const auto description = moved ? std::string("移动摆放组") : std::string("旋转摆放组");
    (void)project_.ObserveMapDataEdit(description);
    QtProject::PushMapDataEdit(project_, (moved ? tr("移动 %1 个摆放") : tr("旋转 %1 个摆放")).arg(selectedPlacements_.size()), description);
    const QSignalBlocker blocker(field); field->clearFocus(); inspectorKey_.clear(); UpdateInspector();
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

void WorldModule::CommitRegionFields() {
    const auto* region = SelectedRegion();
    if (!region || !project_.mapEdits_ || regionDraft_) return;
    auto* field = qobject_cast<QDoubleSpinBox*>(sender());
    const auto found = std::find(regionBounds_.begin(), regionBounds_.end(), field);
    if (found == regionBounds_.end()) return;
    if (!MatchesInspectorRevision(field, SelectionRevision(), project_.mapEdits_->Revision(), MapKey(), generation_) || !SelectionEditable()) {
        selectionStatus_->setText(tr("选择或地图已变化，请重新编辑属性"));
        const QSignalBlocker blocker(field); field->clearFocus(); inspectorKey_.clear(); UpdateInspector(); return;
    }
    std::array<float, 4> bounds{region->left, region->bottom, region->right, region->top};
    const auto index = static_cast<std::size_t>(std::distance(regionBounds_.begin(), found));
    if (!ChangedField(field, bounds[index])) return;
    bounds[index] = static_cast<float>(field->value());
    if (bounds == std::array<float, 4>{region->left, region->bottom, region->right, region->top}) {
        UpdateInspector(); // 输入被吸附回原值时同步显示
        return;
    }
    constexpr std::array<std::string_view, 4> fieldIds{"left", "bottom", "right", "top"};
    const auto fieldChecked = ValidateWorldField(MapDataKind::Regions, fieldIds[index], bounds[index]);
    const auto rowChecked = fieldChecked.Succeeded()
        ? MapData::ValidateMapFieldRegionBounds(bounds[0], bounds[1], bounds[2], bounds[3]) : fieldChecked;
    if (!rowChecked.Succeeded()) {
        selectionStatus_->setText(tr("修改未保存：%1").arg(WorldDiagnosticText(rowChecked.diagnostics)));
        for (auto* spin : regionBounds_) spin->clearFocus();
        UpdateInspector();
        return;
    }
    const auto status = MapData::SetRegionBounds(*project_.mapEdits_, *selectedRegion_, bounds[0], bounds[1], bounds[2], bounds[3]);
    if (!status.Succeeded()) {
        project_.AddDiagnostics(status.diagnostics);
        selectionStatus_->setText(tr("修改未保存：%1").arg(WorldDiagnosticText(status.diagnostics)));
        UpdateInspector();
        return;
    }
    (void)project_.ObserveMapDataEdit("修改区域");
    QtProject::PushMapDataEdit(project_, tr("调整区域 %1").arg(selectionName_), "修改区域");
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

void WorldModule::CommitCameraFields() {
    const auto* camera = SelectedCamera();
    if (!camera || !project_.mapEdits_ || cameraDraft_) return;
    auto* field = qobject_cast<QDoubleSpinBox*>(sender());
    if (field != cameraX_ && field != cameraY_) return;
    if (!MatchesInspectorRevision(field, SelectionRevision(), project_.mapEdits_->Revision(), MapKey(), generation_) || !SelectionEditable()) {
        selectionStatus_->setText(tr("选择或地图已变化，请重新编辑属性"));
        const QSignalBlocker blocker(field); field->clearFocus(); inspectorKey_.clear(); UpdateInspector(); return;
    }
    const double sourceValue = field == cameraX_ ? camera->targetX : camera->targetY;
    if (!ChangedField(field, sourceValue)) return;
    const auto x = field == cameraX_ ? static_cast<float>(field->value()) : camera->targetX;
    const auto y = field == cameraY_ ? static_cast<float>(field->value()) : camera->targetY;
    if (std::abs(x - camera->targetX) < 1e-4f && std::abs(y - camera->targetY) < 1e-4f) return;
    const auto checked = ValidateWorldField(MapDataKind::Cameras,
        field == cameraX_ ? "target_x" : "target_y", field == cameraX_ ? x : y);
    if (!checked.Succeeded()) {
        selectionStatus_->setText(tr("修改未保存：%1").arg(WorldDiagnosticText(checked.diagnostics)));
        field->clearFocus(); UpdateInspector();
        return;
    }
    const auto status = MapData::SetCameraTarget(*project_.mapEdits_, *selectedCamera_, x, y);
    if (!status.Succeeded()) {
        project_.AddDiagnostics(status.diagnostics);
        selectionStatus_->setText(tr("修改未保存：%1").arg(WorldDiagnosticText(status.diagnostics)));
        UpdateInspector();
        return;
    }
    (void)project_.ObserveMapDataEdit("修改镜头");
    QtProject::PushMapDataEdit(project_, tr("移动镜头 %1").arg(selectionName_), "修改镜头");
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

void WorldModule::OpenSelectedDefinition() {
    if (!host_ || !selection_ || !selectionType_) return;
    host_->Navigate({"objects", "object", QString::fromUtf8(ObjectData::ToString(*selectionType_)) + QLatin1Char(':')
        + QString::fromUtf8(selection_->rawcode), {}});
}

} // namespace War3Map::Modules
