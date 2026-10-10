#include "MapPlayerEnumDelegate.h"

#include "War3MapCore/MapData/MapSettings.h"

#include <QComboBox>

#include <array>

namespace War3Map::Modules {
namespace {

// 名称沿用原版编辑器的玩家属性用语。
constexpr std::array<const char*, 4> kControllers{"用户", "电脑", "中立", "可营救的"};
constexpr std::array<const char*, 5> kRaces{"可选择的", "人类", "兽族", "不死族", "暗夜精灵族"};

bool Known(MapPlayerEnumDelegate::Kind kind, int value) {
    return kind == MapPlayerEnumDelegate::Kind::Controller
        ? MapData::KnownPlayerController(value) : MapData::KnownPlayerRace(value);
}

int First(MapPlayerEnumDelegate::Kind kind) { return kind == MapPlayerEnumDelegate::Kind::Controller ? 1 : 0; }

} // namespace

MapPlayerEnumDelegate::MapPlayerEnumDelegate(Kind kind, std::function<bool()> rawFields, QObject* parent)
    : QStyledItemDelegate(parent), kind_(kind), rawFields_(std::move(rawFields)) {}

QString MapPlayerEnumDelegate::Label(Kind kind, int value) {
    if (!Known(kind, value)) return QObject::tr("未知（%1）").arg(value);
    const auto index = static_cast<std::size_t>(value - First(kind));
    return QObject::tr(kind == Kind::Controller ? kControllers[index] : kRaces[index]);
}

QString MapPlayerEnumDelegate::displayText(const QVariant& value, const QLocale& locale) const {
    bool okay = false;
    const int number = value.toString().trimmed().toInt(&okay, 0);
    if (!okay || (rawFields_ && rawFields_())) return QStyledItemDelegate::displayText(value, locale);
    return Label(kind_, number);
}

QWidget* MapPlayerEnumDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const {
    auto* combo = new QComboBox(parent);
    combo->setObjectName(kind_ == Kind::Controller ? "mapPlayerControllerEditor" : "mapPlayerRaceEditor");
    // 选中即提交，不要求再点别处。
    connect(combo, qOverload<int>(&QComboBox::activated), this, [this, combo] {
        emit const_cast<MapPlayerEnumDelegate*>(this)->commitData(combo);
        emit const_cast<MapPlayerEnumDelegate*>(this)->closeEditor(combo);
    });
    return combo;
}

void MapPlayerEnumDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    auto* combo = static_cast<QComboBox*>(editor);
    bool okay = false;
    const int current = index.data(Qt::EditRole).toString().trimmed().toInt(&okay, 0);
    combo->clear();
    // 未知来源值只能原样保留，不能从列表新选到其他未知值。
    if (okay && !Known(kind_, current)) combo->addItem(tr("未知（%1）· 保留原值").arg(current), current);
    const int count = kind_ == Kind::Controller ? static_cast<int>(kControllers.size()) : static_cast<int>(kRaces.size());
    for (int value = First(kind_); value < First(kind_) + count; ++value)
        combo->addItem(QStringLiteral("%1  %2").arg(Label(kind_, value)).arg(value), value);
    const int selected = okay ? combo->findData(current) : -1;
    combo->setCurrentIndex(selected >= 0 ? selected : 0);
}

void MapPlayerEnumDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const {
    const auto* combo = static_cast<QComboBox*>(editor);
    const QString value = QString::number(combo->currentData().toInt());
    if (index.data(Qt::EditRole).toString().trimmed() != value) model->setData(index, value, Qt::EditRole);
}

} // namespace War3Map::Modules
