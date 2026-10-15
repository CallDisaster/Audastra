#include "MapEntitiesModule.h"
#include "../World/WorldNavigationBookmark.h"

#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "ProjectAuxiliaryEdits.h"
#include "Icons/IconProvider.h"
#include "ObjectNames.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QHeaderView>
#include <QCheckBox>
#include <QInputDialog>
#include <QPushButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QJsonArray>
#include <QScopedValueRollback>
#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <numbers>
#include <limits>
#include <optional>
#include <string>

namespace War3Map::Modules {
namespace {

using MapData::MapDataEditSession;

constexpr std::array kKinds{MapDataKind::Units, MapDataKind::Doodads,
    MapDataKind::Regions, MapDataKind::Cameras};

const std::array<QStringList, 4> kFields{
    QStringList{"rawcode", "owner", "x", "y", "z", "rotation", "health", "mana", "gold"},
    QStringList{"rawcode", "variation", "x", "y", "z", "rotation", "life"},
    QStringList{"name", "left", "bottom", "right", "top", "ambientSound", "weather", "color"},
    QStringList{"name", "targetX", "targetY", "zOffset", "rotation", "angle", "distance", "roll", "fov", "far", "near"}};

const std::array<QStringList, 4> kLabels{
    QStringList{"类型", "所属玩家", "X", "Y", "高度", "朝向（度）", "生命", "魔法", "金币"},
    QStringList{"类型", "变体", "X", "Y", "高度", "朝向（度）", "生命 %"},
    QStringList{"名称", "左边界", "下边界", "右边界", "上边界", "环境音变量", "天气 ID", "颜色 BGR（十六进制）"},
    QStringList{"名称", "目标 X", "目标 Y", "高度偏移", "旋转", "攻角", "距离", "滚转", "视野", "远裁剪", "第十槽（只读）"}};

Result<float> ParseFloat(std::string_view text) {
    float result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(result))
        return Result<float>::Failure("MapEntities", "MAP_NUMBER_INVALID", "请输入有效的有限数值");
    return Result<float>::Success(result);
}

Result<std::int32_t> ParseInt(std::string_view text) {
    std::int32_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return Result<std::int32_t>::Failure("MapEntities", "MAP_NUMBER_INVALID", "请输入有效整数");
    return Result<std::int32_t>::Success(result);
}

Status Failure(std::string code, std::string message) {
    return Status::Failure("MapEntities", std::move(code), std::move(message));
}

template <typename T>
Status SetFloat(T& output, std::string_view value) {
    auto parsed = ParseFloat(value);
    if (!parsed.Succeeded()) { Status status; status.diagnostics = parsed.diagnostics; return status; }
    output = *parsed.value;
    return Status::Ok();
}

template <typename T>
Status SetInt(T& output, std::string_view value) {
    auto parsed = ParseInt(value);
    if (!parsed.Succeeded()) { Status status; status.diagnostics = parsed.diagnostics; return status; }
    output = *parsed.value;
    return Status::Ok();
}

Result<std::vector<std::byte>> CurrentBytes(const MapDocument& document, MapDataKind kind) {
    switch (kind) {
    case MapDataKind::Units: return document.Units() ? document.Units()->Serialize() :
        Result<std::vector<std::byte>>::Failure("MapEntities", "MAP_SOURCE_MISSING", "没有单位放置成员");
    case MapDataKind::Doodads: return document.Doodads() ? document.Doodads()->Serialize() :
        Result<std::vector<std::byte>>::Failure("MapEntities", "MAP_SOURCE_MISSING", "没有装饰物成员");
    case MapDataKind::Regions: return document.Regions() ? document.Regions()->Serialize() :
        Result<std::vector<std::byte>>::Failure("MapEntities", "MAP_SOURCE_MISSING", "没有区域成员");
    case MapDataKind::Cameras: return document.Cameras() ? document.Cameras()->Serialize() :
        Result<std::vector<std::byte>>::Failure("MapEntities", "MAP_SOURCE_MISSING", "没有镜头成员");
    default: return Result<std::vector<std::byte>>::Failure("MapEntities", "MAP_KIND_INVALID", "该类别不能在此编辑");
    }
}

QString Value(float number) { return QString::number(number, 'g', 8); }

QString DefaultOrValue(std::int32_t number) {
    return number == -1 ? QObject::tr("默认") : QString::number(number);
}

QString Degrees(float radians) {
    // 表格只格式化显示；未提交编辑时保留来源中的原始 float 位。
    const double degrees = static_cast<double>(radians) * (180.0 / std::numbers::pi);
    QString text = QString::number(std::abs(degrees) < 0.0005 ? 0.0 : degrees, 'f', 3);
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    return text;
}

QString PlayerName(const MapDocument& snapshot, std::int32_t owner) {
    QString label;
    if (snapshot.Settings()) {
        const auto& players = snapshot.Settings()->players;
        const auto player = std::find_if(players.begin(), players.end(),
            [owner](const auto& value) { return value.number == owner; });
        if (player != players.end() && !player->name.empty()) {
            const auto resolved = snapshot.ResolveString(player->name);
            label = QString::fromUtf8(resolved.Succeeded() && resolved.value ? *resolved.value : player->name);
        }
    }
    if (!label.isEmpty()) return QObject::tr("%1（玩家 %2）").arg(label).arg(static_cast<qint64>(owner) + 1);
    if (owner >= 0 && owner < 12) return QObject::tr("玩家 %1").arg(owner + 1);
    // 当前放置格式对应经典 12 玩家；中立编号与脚本生成器保持一致。
    if (owner == 12) return QObject::tr("中立敌对");
    if (owner == 15) return QObject::tr("中立被动");
    return QObject::tr("玩家 ID %1").arg(owner);
}

Status SetDefaultOrInt(std::int32_t& output, std::string_view value) {
    if (value == "默认" || value == "default") { output = -1; return Status::Ok(); }
    return SetInt(output, value);
}

Status SetDegrees(float& output, std::string_view value) {
    const auto degrees = ParseFloat(value);
    if (!degrees.Succeeded()) { Status status; status.diagnostics = degrees.diagnostics; return status; }
    const float radians = *degrees.value * (std::numbers::pi_v<float> / 180.0f);
    if (!std::isfinite(radians)) return Failure("MAP_ANGLE_INVALID", "角度换算后超出可保存范围");
    output = radians;
    return Status::Ok();
}

Status SetCatalogFloat(float& output, MapDataKind kind, std::string_view fieldId, std::string_view input) {
    const auto* field = MapData::FindMapField(kind, fieldId);
    if (!field) return Failure("MAP_FIELD_UNKNOWN", "字段目录中没有对应的可编辑字段");
    auto parsed = MapData::ParseMapFieldInput(*field, input);
    if (!parsed.Succeeded() || !parsed.value) return Status{std::move(parsed.diagnostics)};
    const auto checked = MapData::ValidateMapFieldValue(kind, fieldId, *parsed.value);
    if (!checked.Succeeded()) return checked;
    output = std::bit_cast<float>(parsed.value->floatBits);
    return Status::Ok();
}

} // namespace

MapEntitiesModule::MapEntitiesModule(QtProject::EditorProject& project) : project_(project) {
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, &MapEntitiesModule::Refresh);
    if (project_.mapDataUndoStack_)
        connect(project_.mapDataUndoStack_, &QUndoStack::indexChanged, this, &MapEntitiesModule::Refresh);
}

MapEntitiesModule::~MapEntitiesModule() { delete page_.data(); }

Shell::ModuleInfo MapEntitiesModule::Info() const {
    Shell::ModuleInfo info{"entities", tr("地图实例"), "lucide/map-pin", QKeySequence(Qt::ALT | Qt::Key_0)};
    info.showInSwitcher = false;
    return info;
}

QWidget* MapEntitiesModule::Widget() {
    if (page_) return page_;
    page_ = new QWidget;
    page_->setObjectName("mapEntitiesWorkspace");
    auto* layout = new QVBoxLayout(page_);
    layout->setContentsMargins(12, 10, 12, 10);
    auto* actions = new QHBoxLayout;
    auto* add = new QPushButton(tr("新建"), page_);
    auto* remove = new QPushButton(tr("删除选中"), page_);
    add->setObjectName("mapEntitiesAddAuxiliary");
    remove->setObjectName("mapEntitiesDeleteAuxiliary");
    actions->addWidget(add); actions->addWidget(remove); actions->addStretch();
    layout->addLayout(actions);
    rawFields_ = new QCheckBox(tr("显示原始字段"), page_);
    rawFields_->setObjectName("mapEntitiesRawFields");
    layout->addWidget(rawFields_, 0, Qt::AlignRight);
    tabs_ = new QTabWidget(page_);
    const QStringList titles{tr("单位与物品"), tr("装饰物"), tr("区域"), tr("镜头")};
    for (int kind = 0; kind < 4; ++kind) {
        auto* table = new QTableWidget(tabs_);
        table->setObjectName(QStringLiteral("mapEntitiesTable%1").arg(kind));
        table->setColumnCount(kFields[kind].size());
        table->setHorizontalHeaderLabels(kLabels[kind]);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->horizontalHeader()->setStretchLastSection(false);
        table->horizontalHeader()->setMinimumSectionSize(70);
        for (int column = 1; column < table->columnCount(); ++column)
            table->setColumnWidth(column, 98);
        if (kind == 0) table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->setIconSize({28, 28});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->verticalHeader()->hide();
        tables_[kind] = table;
        tabs_->addTab(table, titles[kind]);
        connect(table, &QTableWidget::cellDoubleClicked, this,
            [this, kind](int row, int column) { EditSelected(kind, row, column); });
        connect(table->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this, kind] { RefreshVisibleIcons(kind); });
    }
    connect(rawFields_, &QCheckBox::toggled, this, [this](bool raw) {
        for (int kind = 0; kind < 4; ++kind)
            tables_[kind]->setHorizontalHeaderLabels(raw ? kFields[kind] : kLabels[kind]);
        Refresh();
    });
    connect(tabs_, &QTabWidget::currentChanged, this,
        [this, add, remove](int kind) { RefreshVisibleIcons(kind); add->setVisible(kind >= 2); remove->setVisible(kind >= 2); });
    add->setVisible(false); remove->setVisible(false);
    connect(add, &QPushButton::clicked, this, &MapEntitiesModule::AddAuxiliary);
    connect(remove, &QPushButton::clicked, this, [this] {
        const int category = tabs_->currentIndex();
        if (category < 2 || category > 3 || tables_[category]->currentRow() < 0) return;
        ShowError(DeleteAuxiliary(kKinds[category], static_cast<std::size_t>(tables_[category]->currentRow())));
    });
    layout->addWidget(tabs_, 1);
    Refresh();
    return page_;
}

QUndoStack* MapEntitiesModule::UndoStack() { return project_.mapDataUndoStack_; }

void MapEntitiesModule::Refresh() {
    if (!page_ || !project_.mapEdits_) return;
    const auto snapshot = project_.mapEdits_->Snapshot();
    for (int kind = 0; kind < 4; ++kind) {
        auto* table = tables_[kind];
        const int selected = table->currentRow();
        table->setRowCount(0);
        const auto append = [table](const QStringList& cells, const QString& rawcode = {},
            const QString& iconPath = {}, const QString& issue = {}, const QString& nameReference = {}) {
            const int row = table->rowCount();
            table->insertRow(row);
            for (int column = 0; column < cells.size(); ++column) {
                auto* item = new QTableWidgetItem(cells[column]);
                if (column == 0 && !rawcode.isEmpty()) {
                    item->setData(Qt::UserRole, rawcode);
                    item->setData(Qt::UserRole + 1, iconPath);
                    item->setToolTip(issue.isEmpty() ? QObject::tr("对象 ID：%1").arg(rawcode)
                        : QObject::tr("对象 ID：%1\n%2").arg(rawcode, issue));
                    if (!nameReference.isEmpty())
                        item->setToolTip(item->toolTip() + QObject::tr("\n名称引用：%1").arg(nameReference));
                }
                table->setItem(row, column, item);
            }
        };
        if (kind == 0 && snapshot.Units()) for (const auto& unit : snapshot.Units()->units) {
            const auto presentation = PlacementPresentation(project_, snapshot, unit.rawcode,
                ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item);
            const QString id = QString::fromUtf8(unit.rawcode);
            const QString title = rawFields_->isChecked() || presentation.name == id
                ? id : tr("%1  ·  %2").arg(presentation.name, id);
            append({title, rawFields_->isChecked() ? QString::number(unit.owner) : PlayerName(snapshot, unit.owner), Value(unit.position[0]),
                Value(unit.position[1]), Value(unit.position[2]), Degrees(unit.rotation),
                DefaultOrValue(unit.hitPoints), DefaultOrValue(unit.mana), QString::number(unit.gold)},
                id, presentation.iconPath, presentation.issue, presentation.nameReference);
            auto* owner = table->item(table->rowCount() - 1, 1);
            owner->setData(Qt::UserRole, QString::number(unit.owner));
            owner->setToolTip(tr("玩家 ID（从 0 开始）：%1").arg(unit.owner));
        }
        if (kind == 1 && snapshot.Doodads()) for (const auto& doodad : snapshot.Doodads()->doodads) {
            const auto presentation = PlacementPresentation(project_, snapshot, doodad.rawcode,
                ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Destructable);
            const QString id = QString::fromUtf8(doodad.rawcode);
            const QString title = rawFields_->isChecked() || presentation.name == id
                ? id : tr("%1  ·  %2").arg(presentation.name, id);
            append({title, QString::number(doodad.variation),
                Value(doodad.position[0]), Value(doodad.position[1]), Value(doodad.position[2]),
                Degrees(doodad.rotation), QString::number(doodad.lifePercent)},
                id, presentation.iconPath, presentation.issue, presentation.nameReference);
        }
        if (kind == 2 && snapshot.Regions()) for (const auto& region : snapshot.Regions()->entries)
            append({QString::fromUtf8(region.name), Value(region.left), Value(region.bottom),
                Value(region.right), Value(region.top), QString::fromUtf8(region.ambientSound),
                std::all_of(region.weatherId.begin(), region.weatherId.end(), [](auto b) { return b == std::byte{}; })
                    ? QString{} : QString::fromLatin1(reinterpret_cast<const char*>(region.weatherId.data()), 4),
                QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(region.colorBgra.data()), 3).toHex())});
        if (kind == 3 && snapshot.Cameras()) for (const auto& camera : snapshot.Cameras()->entries)
            append({QString::fromUtf8(camera.name), Value(camera.targetX), Value(camera.targetY), Value(camera.zOffset),
                Value(camera.rotation), Value(camera.angleOfAttack), Value(camera.targetDistance), Value(camera.roll),
                Value(camera.fieldOfView), Value(camera.farClippingPlane), Value(camera.nearClippingPlane)});
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto id = WorldNavigation::At(snapshot, kind, static_cast<std::uint32_t>(row));
            if (id) table->item(row, 0)->setData(Qt::UserRole + 20, WorldNavigation::Encode(*id));
        }
        if (selected >= 0 && selected < table->rowCount()) table->selectRow(selected);
    }
    RefreshVisibleIcons(tabs_->currentIndex());
}

void MapEntitiesModule::RefreshVisibleIcons(int category) {
    if (category < 0 || category > 1 || !project_.iconProvider_ || !tables_[category]) return;
    auto* table = tables_[category];
    if (table->rowCount() == 0) return;
    int first = table->rowAt(0);
    if (first < 0) first = 0;
    int last = table->rowAt(table->viewport()->height() - 1);
    if (last < first) last = std::min(first + 35, table->rowCount() - 1);
    for (int row = first; row <= std::min(last + 3, table->rowCount() - 1); ++row) {
        auto* item = table->item(row, 0);
        if (!item || item->data(Qt::UserRole + 2).toBool()) continue;
        const QString path = item->data(Qt::UserRole + 1).toString();
        if (!path.isEmpty()) item->setIcon(project_.iconProvider_->IconForTexture(path));
        item->setData(Qt::UserRole + 2, true);
    }
}

Status MapEntitiesModule::EditCell(MapDataKind kind, std::size_t row,
    std::string_view field, std::string_view value, const QtProject::AuxiliaryEditBinding* auxiliaryBinding) {
    if (auxiliaryBinding && !QtProject::AuxiliaryEditBindingCurrent(project_, *auxiliaryBinding))
        return Failure("AUX_PROJECT_STALE", "弹窗期间地图来源、编辑会话或修订已变化，请重新打开属性");
    if (!project_.mapEdits_) return Failure("MAP_SOURCE_MISSING", "尚未打开地图数据");
    const auto snapshot = project_.mapEdits_->Snapshot();
    const auto before = CurrentBytes(snapshot, kind);
    if (!before.Succeeded()) { Status status; status.diagnostics = before.diagnostics; return status; }
    auto next = snapshot;
    Status parsed = Status::Ok();
    Result<std::vector<std::byte>> after;
    if (kind == MapDataKind::Units && next.Units()) {
        auto doc = *next.Units();
        if (row >= doc.units.size()) return Failure("MAP_ROW_INVALID", "单位行不存在");
        auto& unit = doc.units[row];
        if (field == "owner") parsed = SetInt(unit.owner, value);
        else if (field == "x") parsed = SetFloat(unit.position[0], value);
        else if (field == "y") parsed = SetFloat(unit.position[1], value);
        else if (field == "z") parsed = SetFloat(unit.position[2], value);
        else if (field == "rotation") parsed = SetCatalogFloat(unit.rotation, kind, "facing", value);
        else if (field == "health") parsed = SetDefaultOrInt(unit.hitPoints, value);
        else if (field == "mana") parsed = SetDefaultOrInt(unit.mana, value);
        else if (field == "gold") parsed = SetInt(unit.gold, value);
        else return Failure("MAP_FIELD_READONLY", "单位类型在物编或类型选择器中修改");
        if (parsed.Succeeded()) after = doc.Serialize();
    } else if (kind == MapDataKind::Doodads && next.Doodads()) {
        auto doc = *next.Doodads();
        if (row >= doc.doodads.size()) return Failure("MAP_ROW_INVALID", "装饰物行不存在");
        auto& doodad = doc.doodads[row];
        if (field == "variation") parsed = SetInt(doodad.variation, value);
        else if (field == "x") parsed = SetFloat(doodad.position[0], value);
        else if (field == "y") parsed = SetFloat(doodad.position[1], value);
        else if (field == "z") parsed = SetFloat(doodad.position[2], value);
        else if (field == "rotation") parsed = SetDegrees(doodad.rotation, value);
        else if (field == "life") {
            auto percent = ParseInt(value);
            if (!percent.Succeeded() || *percent.value < 0 || *percent.value > 100)
                return Failure("MAP_RANGE_INVALID", "装饰物生命百分比必须在 0 到 100 之间");
            doodad.lifePercent = static_cast<std::uint8_t>(*percent.value);
        } else return Failure("MAP_FIELD_READONLY", "装饰物类型在物编或类型选择器中修改");
        if (parsed.Succeeded()) after = doc.Serialize();
    } else if (kind == MapDataKind::Regions && next.Regions()) {
        auto doc = *next.Regions();
        if (row >= doc.entries.size()) return Failure("MAP_ROW_INVALID", "区域行不存在");
        auto& region = doc.entries[row];
        if (field == "name") region.name.assign(value);
        else if (field == "left") parsed = SetCatalogFloat(region.left, kind, "left", value);
        else if (field == "bottom") parsed = SetCatalogFloat(region.bottom, kind, "bottom", value);
        else if (field == "right") parsed = SetCatalogFloat(region.right, kind, "right", value);
        else if (field == "top") parsed = SetCatalogFloat(region.top, kind, "top", value);
        else if (field == "ambientSound") region.ambientSound.assign(value);
        else if (field == "weather") {
            if (!MapData::IsEditableRegionWeatherId(value)) return Failure("REGION_WEATHER_INVALID", "天气 ID 须为空或四字节可打印 rawcode，不能含引号或反斜杠");
            region.weatherId.fill(std::byte{});
            for (std::size_t i = 0; i < value.size(); ++i) region.weatherId[i] = static_cast<std::byte>(value[i]);
        } else if (field == "color") {
            if (value.size() != 6 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }))
                return Failure("REGION_COLOR_INVALID", "颜色须为六位十六进制，按 B、G、R 顺序填写；保留字节不变");
            const auto bytes = QByteArray::fromHex(QByteArray(value.data(), static_cast<qsizetype>(value.size())));
            for (std::size_t i = 0; i < 3; ++i) region.colorBgra[i] = static_cast<std::uint8_t>(bytes[static_cast<qsizetype>(i)]);
        }
        else return Failure("MAP_FIELD_READONLY", "区域字段尚未在此编辑");
        if (parsed.Succeeded() && (field == "left" || field == "bottom" || field == "right" || field == "top"))
            parsed = MapData::ValidateMapFieldRegionBounds(region.left, region.bottom, region.right, region.top);
        if (parsed.Succeeded()) after = doc.Serialize();
    } else if (kind == MapDataKind::Cameras && next.Cameras()) {
        auto doc = *next.Cameras();
        if (row >= doc.entries.size()) return Failure("MAP_ROW_INVALID", "镜头行不存在");
        auto& camera = doc.entries[row];
        if (field == "name") camera.name.assign(value);
        else if (field == "targetX") parsed = SetCatalogFloat(camera.targetX, kind, "target_x", value);
        else if (field == "targetY") parsed = SetCatalogFloat(camera.targetY, kind, "target_y", value);
        else if (field == "zOffset") parsed = SetFloat(camera.zOffset, value);
        else if (field == "rotation") parsed = SetFloat(camera.rotation, value);
        else if (field == "angle") parsed = SetFloat(camera.angleOfAttack, value);
        else if (field == "distance") parsed = SetFloat(camera.targetDistance, value);
        else if (field == "roll") parsed = SetFloat(camera.roll, value);
        else if (field == "fov") parsed = SetFloat(camera.fieldOfView, value);
        else if (field == "far") parsed = SetFloat(camera.farClippingPlane, value);
        else if (field == "near") return Failure("CAMERA_CLASSIC_SLOT_READ_ONLY", "经典镜头第十个槽位用途未证实，逐位保留原值");
        else return Failure("MAP_FIELD_READONLY", "镜头字段尚未在此编辑");
        if (parsed.Succeeded()) after = doc.Serialize();
    } else return Failure("MAP_SOURCE_MISSING", "此类地图成员缺失或无法解析");
    if (!parsed.Succeeded()) return parsed;
    if (!after.Succeeded()) { Status status; status.diagnostics = after.diagnostics; return status; }
    if (*after.value == *before.value) return Status::Ok();
    if (kind == MapDataKind::Regions || kind == MapDataKind::Cameras) {
        Services::AuxiliaryEditRequest request;
        request.kind = kind; request.sourceIndex = row;
        const auto identity = Services::AuxiliaryEditing::MemberIdentity(snapshot, kind);
        const auto key = MapData::MapFieldRowKey(snapshot, kind, row);
        if (!identity.Succeeded()) return {identity.diagnostics};
        if (!key.Succeeded()) return {key.diagnostics};
        request.expectedMemberSha256 = *identity.value; request.expectedRowKey = *key.value;
        if (kind == MapDataKind::Regions) {
            const auto parsedRecord = MapData::ParseRegions(*after.value);
            if (!parsedRecord.Succeeded()) return {parsedRecord.diagnostics};
            request.record = parsedRecord.value->entries[row];
        } else {
            const auto parsedRecord = MapData::ParseCameras(*after.value);
            if (!parsedRecord.Succeeded()) return {parsedRecord.diagnostics};
            request.record = parsedRecord.value->entries[row];
        }
        if (auxiliaryBinding) {
            const auto applied = QtProject::ApplyAuxiliaryEditWithResult(project_, *auxiliaryBinding,
                std::move(request), tr("修改区域或镜头"));
            return {applied.diagnostics};
        }
        return QtProject::ApplyAuxiliaryEdit(project_, std::move(request), tr("修改区域或镜头"));
    }
    auto status = ApplySerialized(kind, *after.value);
    if (!status.Succeeded()) return status;
    QtProject::PushMapDataEdit(project_, tr("修改地图属性"), "修改地图实例、区域或镜头");
    Refresh();
    return Status::Ok();
}

Status MapEntitiesModule::ApplySerialized(MapDataKind kind, std::span<const std::byte> bytes) {
    if (!project_.mapEdits_) return Failure("MAP_SOURCE_MISSING", "尚未打开地图数据");
    if (kind == MapDataKind::Regions || kind == MapDataKind::Cameras)
        return Failure("AUX_TYPED_COMMAND_REQUIRED", "区域和镜头须使用带行身份与引用检查的共用命令，不能直接替换整表");
    const auto status = project_.mapEdits_->EditSerialized(kind, bytes);
    if (!status.Succeeded()) return status;
    (void)project_.ObserveEditState("修改地图实例、区域或镜头");
    Refresh();
    return Status::Ok();
}

Status MapEntitiesModule::CreateAuxiliary(MapData::RegionRecord record) {
    if (!project_.mapEdits_) return Failure("MAP_SOURCE_MISSING", "尚未打开地图数据");
    Services::AuxiliaryEditRequest request;
    request.kind = MapDataKind::Regions; request.action = Services::AuxiliaryEditAction::Create;
    const auto identity = Services::AuxiliaryEditing::MemberIdentity(project_.mapEdits_->Snapshot(), request.kind);
    if (!identity.Succeeded()) return {identity.diagnostics};
    request.expectedMemberSha256 = *identity.value; request.record = std::move(record);
    return QtProject::ApplyAuxiliaryEdit(project_, std::move(request), tr("新建区域"));
}

Status MapEntitiesModule::CreateAuxiliary(MapData::CameraRecord record) {
    if (!project_.mapEdits_) return Failure("MAP_SOURCE_MISSING", "尚未打开地图数据");
    Services::AuxiliaryEditRequest request;
    request.kind = MapDataKind::Cameras; request.action = Services::AuxiliaryEditAction::Create;
    const auto identity = Services::AuxiliaryEditing::MemberIdentity(project_.mapEdits_->Snapshot(), request.kind);
    if (!identity.Succeeded()) return {identity.diagnostics};
    request.expectedMemberSha256 = *identity.value; request.record = std::move(record);
    return QtProject::ApplyAuxiliaryEdit(project_, std::move(request), tr("新建镜头"));
}

Status MapEntitiesModule::DeleteAuxiliary(MapDataKind kind, std::size_t row) {
    if (!project_.mapEdits_) return Failure("MAP_SOURCE_MISSING", "尚未打开地图数据");
    const auto current = project_.mapEdits_->Snapshot();
    const auto identity = Services::AuxiliaryEditing::MemberIdentity(current, kind);
    const auto key = MapData::MapFieldRowKey(current, kind, row);
    if (!identity.Succeeded()) return {identity.diagnostics};
    if (!key.Succeeded()) return {key.diagnostics};
    Services::AuxiliaryEditRequest request;
    request.kind = kind; request.action = Services::AuxiliaryEditAction::Delete;
    request.sourceIndex = row; request.expectedRowKey = *key.value; request.expectedMemberSha256 = *identity.value;
    return QtProject::ApplyAuxiliaryEdit(project_, std::move(request), tr("删除区域或镜头"));
}

void MapEntitiesModule::ShowError(const Status& status) {
    if (!page_) return;
    auto* warning = page_->findChild<QLabel*>("mapEntitiesError");
    if (status.Succeeded()) { if (warning) warning->hide(); return; }
    if (!warning) {
        warning = new QLabel(page_); warning->setWordWrap(true);
        warning->setObjectName("mapEntitiesError"); page_->layout()->addWidget(warning);
    }
    warning->setText(status.diagnostics.empty() ? tr("操作未完成") : QString::fromUtf8(status.diagnostics.front().message));
    warning->show();
}

void MapEntitiesModule::AddAuxiliary() {
    if (!project_.mapEdits_ || !tabs_ || tabs_->currentIndex() < 2) return;
    const auto binding = QtProject::CaptureAuxiliaryEditBinding(project_);
    if (!binding.Succeeded()) { ShowError({binding.diagnostics}); return; }
    const auto source = project_.mapEdits_->Snapshot();
    Services::AuxiliaryEditRequest request;
    request.kind = kKinds[tabs_->currentIndex()]; request.action = Services::AuxiliaryEditAction::Create;
    const auto identity = Services::AuxiliaryEditing::MemberIdentity(source, request.kind);
    if (!identity.Succeeded()) { ShowError({identity.diagnostics}); return; }
    request.expectedMemberSha256 = *identity.value;
    const QScopedValueRollback<bool> draft(editingCell_, true);
    bool accepted = false;
    const auto name = QInputDialog::getText(page_, tr("新建区域或镜头"), tr("名称"), QLineEdit::Normal, {}, &accepted);
    if (!accepted) return;
    if (request.kind == MapDataKind::Regions) {
        MapData::RegionRecord region{};
        region.name = name.toUtf8().toStdString(); region.left = region.bottom = -256;
        region.right = region.top = 256; region.colorBgra = {255, 128, 0, 255};
        if (source.Regions()) for (const auto& old : source.Regions()->entries) {
            if (old.creationNumber == std::numeric_limits<std::int32_t>::max()) {
                ShowError(Failure("REGION_ID_EXHAUSTED", "区域创建编号已用尽")); return;
            }
            region.creationNumber = std::max(region.creationNumber, old.creationNumber + 1);
        }
        request.record = std::move(region);
    } else {
        MapData::CameraRecord camera{};
        // 明确的可编辑起始值；第十槽只作为原生记录模板保留，不发射未证实的游戏接口。
        camera.name = name.toUtf8().toStdString(); camera.rotation = 90; camera.angleOfAttack = 304;
        camera.targetDistance = 1650; camera.fieldOfView = 70; camera.farClippingPlane = 5000;
        camera.nearClippingPlane = source.Cameras() && !source.Cameras()->entries.empty()
            ? source.Cameras()->entries.front().nearClippingPlane : 100;
        request.record = std::move(camera);
    }
    const auto applied = QtProject::ApplyAuxiliaryEditWithResult(project_, *binding.value,
        std::move(request), tr("新建区域或镜头"));
    ShowError({applied.diagnostics});
}

void MapEntitiesModule::EditSelected(int category, int row, int column) {
    if (category < 0 || category >= 4 || column < 0 || column >= kFields[category].size()) return;
    if ((category == 0 || category == 1) && column == 0) return;
    if (category == 3 && kFields[category][column] == "near") return;
    auto* item = tables_[category]->item(row, column);
    if (!item) return;
    std::optional<QtProject::AuxiliaryEditBinding> auxiliaryBinding;
    if (category >= 2) {
        const auto binding = QtProject::CaptureAuxiliaryEditBinding(project_);
        if (!binding.Succeeded()) { ShowError({binding.diagnostics}); return; }
        auxiliaryBinding = *binding.value;
    }
    const QScopedValueRollback<bool> draft(editingCell_, true);
    std::optional<std::string> originalRowKey;
    std::optional<std::string> originalMember;
    if (project_.mapEdits_) {
        const auto snapshot = project_.mapEdits_->Snapshot();
        const auto key = MapData::MapFieldRowKey(snapshot, kKinds[category],
            static_cast<std::size_t>(row));
        if (key.Succeeded() && key.value) originalRowKey = *key.value;
        if (category >= 2) {
            const auto member = Services::AuxiliaryEditing::MemberIdentity(snapshot, kKinds[category]);
            if (member.Succeeded()) originalMember = *member.value;
        }
    }
    const QString initial = category == 0 && column == 1 ? item->data(Qt::UserRole).toString() : item->text();
    bool accepted = false;
    const QString text = QInputDialog::getText(page_, tr("修改地图属性"),
        category == 0 && column == 1 ? tr("所属玩家 ID（从 0 开始）") : kLabels[category][column],
        QLineEdit::Normal, initial, &accepted);
    if (!accepted || text == initial) return;
    Status result;
    if (project_.mapEdits_) {
        const auto snapshot = project_.mapEdits_->Snapshot();
        const auto currentKey = MapData::MapFieldRowKey(snapshot, kKinds[category],
            static_cast<std::size_t>(row));
        const auto currentMember = category >= 2
            ? Services::AuxiliaryEditing::MemberIdentity(snapshot, kKinds[category]) : Result<std::string>::Success({});
        if (!originalRowKey || !currentKey.Succeeded() || !currentKey.value || *currentKey.value != *originalRowKey)
            result = Failure("MAP_ROW_STALE", "弹窗期间实例行已变化，请重新选择后编辑");
        else if (category >= 2 && (!originalMember || !currentMember.Succeeded() || *currentMember.value != *originalMember))
            result = Failure("AUX_MEMBER_STALE", "弹窗期间区域或镜头成员已变化，请重新选择后编辑");
        else result = EditCell(kKinds[category], static_cast<std::size_t>(row),
            kFields[category][column].toStdString(), text.toStdString(), auxiliaryBinding ? &*auxiliaryBinding : nullptr);
    } else result = EditCell(kKinds[category], static_cast<std::size_t>(row),
        kFields[category][column].toStdString(), text.toStdString());
    if (!result.Succeeded()) {
        auto* warning = page_->findChild<QLabel*>("mapEntitiesError");
        if (!warning) {
            warning = new QLabel(page_);
            warning->setObjectName("mapEntitiesError");
            page_->layout()->addWidget(warning);
        }
        warning->setText(QString::fromUtf8(result.diagnostics.front().message));
        warning->show();
    } else if (auto* warning = page_->findChild<QLabel*>("mapEntitiesError")) {
        warning->hide();
    }
}

bool MapEntitiesModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "entities") return false;
    (void)Widget();
    for (int category = 0; category < 4; ++category) {
        const QStringList categories{"unit", "doodad", "region", "camera"};
        if (target.kind != categories[category]) continue;
        auto* table = tables_[category];
        if (target.extra.contains("sourceIndex")) {
            const auto value = target.extra.value("sourceIndex");
            const auto text = value.toString().toStdString();
            if (value.typeId() != QMetaType::QString || text.empty() || text.size() > 20) return false;
            std::uint64_t index = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                index >= static_cast<std::uint64_t>(table->rowCount())) return false;
            tabs_->setCurrentIndex(category);
            table->selectRow(static_cast<int>(index));
            table->scrollToItem(table->item(static_cast<int>(index), 0));
            return true;
        }
        tabs_->setCurrentIndex(category);
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto* item = table->item(row, 0);
            if (item->data(Qt::UserRole).toString() != target.id && item->text() != target.id) continue;
            table->selectRow(row);
            table->scrollToItem(table->item(row, 0));
            return true;
        }
    }
    return false;
}

QJsonObject MapEntitiesModule::SaveViewState() const {
    return {{"tab", tabs_ ? tabs_->currentIndex() : 0},
        {"rawFields", rawFields_ && rawFields_->isChecked()}};
}

void MapEntitiesModule::RestoreViewState(const QJsonObject& state) {
    (void)Widget();
    tabs_->setCurrentIndex(std::clamp(state.value("tab").toInt(), 0, 3));
    rawFields_->setChecked(state.value("rawFields").toBool());
}

std::optional<QJsonObject> MapEntitiesModule::CaptureNavigationBookmark() const {
    if (!page_ || !tabs_ || !rawFields_ || !project_.mapEdits_) return std::nullopt;
    const auto document = project_.mapEdits_->Snapshot();
    QJsonArray tables;
    for (const auto* table : tables_) {
        QJsonValue selection(QJsonValue::Null);
        if (const auto* item = table->item(table->currentRow(), 0)) {
            const auto encoded = item->data(Qt::UserRole + 20).toJsonObject();
            const auto id = WorldNavigation::Decode(encoded);
            if (!id || !WorldNavigation::Contains(document, *id)) return std::nullopt;
            selection = encoded;
        }
        tables.push_back(QJsonObject{{"selection", selection}, {"column", std::max(0, table->currentColumn())},
            {"horizontal", table->horizontalScrollBar()->value()}, {"vertical", table->verticalScrollBar()->value()}});
    }
    QJsonObject state{{"schema", 1}, {"tab", tabs_->currentIndex()}, {"rawFields", rawFields_->isChecked()}, {"tables", tables}};
    return WorldNavigation::Bounded(state) ? std::optional(state) : std::nullopt;
}

Shell::NavigationRestoreResult MapEntitiesModule::RestoreNavigationBookmark(const QJsonObject& state) {
    using Restore = Shell::NavigationRestoreResult;
    if (!PrepareNavigationLeave()) return Restore::Blocked;
    const auto schema = WorldNavigation::Integer(state.value("schema"), 1, 1);
    const auto tab = WorldNavigation::Integer(state.value("tab"), 0, 3);
    if (!schema || !tab || !state.value("rawFields").isBool() || !state.value("tables").isArray()
        || !WorldNavigation::Bounded(state) || !project_.mapEdits_) return Restore::Invalid;
    const auto entries = state.value("tables").toArray();
    if (entries.size() != 4) return Restore::Invalid;
    const auto document = project_.mapEdits_->Snapshot();
    std::array<std::optional<Services::WorldObjectId>, 4> identities;
    std::array<int, 4> columns{}, horizontal{}, vertical{};
    for (int kind = 0; kind < 4; ++kind) {
        if (!entries[kind].isObject()) return Restore::Invalid;
        const auto entry = entries[kind].toObject();
        const auto column = WorldNavigation::Integer(entry.value("column"), 0, kFields[kind].size() - 1);
        const auto h = WorldNavigation::Integer(entry.value("horizontal")), v = WorldNavigation::Integer(entry.value("vertical"));
        if (!column || !h || !v) return Restore::Invalid;
        columns[kind] = static_cast<int>(*column); horizontal[kind] = static_cast<int>(*h); vertical[kind] = static_cast<int>(*v);
        if (!entry.value("selection").isNull()) {
            identities[kind] = WorldNavigation::Decode(entry.value("selection"));
            if (!identities[kind] || WorldNavigation::Tab(*identities[kind]) != kind
                || !WorldNavigation::Contains(document, *identities[kind])) return Restore::Invalid;
        }
    }
    // 完整检查四个表的所有目标后才允许刷新、切页或改变选择；失效目标不退回同名行。
    (void)Widget();
    {
        const QSignalBlocker raw(rawFields_);
        rawFields_->setChecked(state.value("rawFields").toBool());
        for (int kind = 0; kind < 4; ++kind) tables_[kind]->setHorizontalHeaderLabels(rawFields_->isChecked() ? kFields[kind] : kLabels[kind]);
    }
    Refresh();
    tabs_->setCurrentIndex(static_cast<int>(*tab));
    for (int kind = 0; kind < 4; ++kind) {
        auto* table = tables_[kind];
        table->clearSelection();
        table->setCurrentItem(nullptr);
        if (identities[kind]) table->setCurrentCell(static_cast<int>(WorldNavigation::Index(*identities[kind])), columns[kind],
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        table->doItemsLayout();
        table->horizontalScrollBar()->setValue(horizontal[kind]); table->verticalScrollBar()->setValue(vertical[kind]);
    }
    return Restore::Restored;
}

} // namespace War3Map::Modules
