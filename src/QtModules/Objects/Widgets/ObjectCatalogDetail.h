#pragma once

#include "Widgets/ObjectCatalogWidgets.h"

#include "Icons/IconProvider.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>


namespace War3Map::QtApp {

// 对象目录与选择弹层共用的显示工具、委托与条目模型（仅模块内部使用）。
namespace CatalogDetail {

inline QString KindName(const QString& kind) {
    static const QHash<QString, QString> names = {
        {QStringLiteral("unit"), QStringLiteral("单位")},
        {QStringLiteral("item"), QStringLiteral("物品")},
        {QStringLiteral("destructable"), QStringLiteral("可破坏物")},
        {QStringLiteral("doodad"), QStringLiteral("装饰物")},
        {QStringLiteral("ability"), QStringLiteral("技能")},
        {QStringLiteral("buff"), QStringLiteral("魔法效果")},
        {QStringLiteral("upgrade"), QStringLiteral("科技")},
        {QStringLiteral("model"), QStringLiteral("模型")},
        {QStringLiteral("texture"), QStringLiteral("纹理")},
        {QStringLiteral("sound"), QStringLiteral("声音")}
    };
    return names.value(kind, kind);
}

inline QString EntryName(const ObjectViewEntry& entry) {
    return entry.name.isEmpty() ? entry.id : entry.name;
}

inline QString ClassificationName(const QString& token) {
    static const QHash<QString, QString> names{
        {"human", "人族"}, {"orc", "兽族"}, {"undead", "不死族"},
        {"nightelf", "暗夜精灵"}, {"naga", "娜迦"}, {"creeps", "中立敌对"},
        {"critters", "中立生物"}, {"other", "其他"}, {"commoner", "中立"},
        {"demon", "恶魔"}, {"unknown", "未分类"}, {"building", "建筑"},
        {"hero", "英雄"}, {"unit", "单位"}, {"item", "物品"},
        {"buff", "魔法效果"}, {"effect", "效果"}
    };
    return names.value(token, token);
}

inline QIcon EntryIcon(IconProvider* icons, const ObjectViewEntry& entry) {
    return icons && !entry.iconPath.isEmpty() ? icons->IconForTexture(entry.iconPath) : QIcon{};
}

inline QString IconStatus(IconProvider* icons, const ObjectViewEntry& entry) {
    if (!entry.available) {
        return QStringLiteral("资源不可用");
    }
    if (entry.iconPath.isEmpty()) {
        return QStringLiteral("未指定图标");
    }
    if (EntryIcon(icons, entry).isNull()) {
        return icons ? QStringLiteral("图标未找到或无法解码") : QStringLiteral("图标服务未配置");
    }
    return {};
}

inline QString EntryTooltip(IconProvider* icons, const ObjectViewEntry& entry) {
    QStringList parts;
    parts << QStringLiteral("<b>%1</b>").arg(EntryName(entry).toHtmlEscaped());
    parts << QStringLiteral("%1 · %2").arg(KindName(entry.kind).toHtmlEscaped(), entry.id.toHtmlEscaped());
    const QStringList classification{entry.race, entry.category};
    for (const auto& text : classification) {
        if (!text.isEmpty()) {
            parts << ClassificationName(text).toHtmlEscaped();
        }
    }
    if (!entry.description.isEmpty()) {
        parts << entry.description.toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
    }
    for (const auto& detail : entry.details) {
        parts << detail.toHtmlEscaped();
    }
    if (!entry.iconPath.isEmpty()) {
        parts << QStringLiteral("图标：%1").arg(entry.iconPath.toHtmlEscaped());
    }
    const auto status = IconStatus(icons, entry);
    if (!status.isEmpty()) {
        parts << status.toHtmlEscaped();
    }
    return QStringLiteral("<qt>%1</qt>").arg(parts.join(QStringLiteral("<br>")));
}

inline void FillCombo(QComboBox* combo, const QString& allLabel, QStringList values, bool kindLabels = false) {
    const QString previous = combo->currentData().toString();
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(allLabel, QString{});
    values.removeAll(QString{});
    values.removeDuplicates();
    std::sort(values.begin(), values.end(), [](const QString& a, const QString& b) {
        return QString::localeAwareCompare(a, b) < 0;
    });
    for (const auto& value : values) {
        combo->addItem(kindLabels ? KindName(value) : ClassificationName(value), value);
    }
    const int index = combo->findData(previous);
    combo->setCurrentIndex(index < 0 ? 0 : index);
}

// 目录项只在交互时显示底色；名称与 ID 固定两行，避免原生 IconMode 为长提示撑高网格。
class ObjectCatalogDelegate final : public QStyledItemDelegate {
public:
    explicit ObjectCatalogDelegate(QListView* view) : QStyledItemDelegate(view), view_(view) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return view_->viewMode() == QListView::IconMode ? QSize(112, 96) : QSize(160, 44);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const bool grid = view_->viewMode() == QListView::IconMode;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool enabled = option.state.testFlag(QStyle::State_Enabled);
        const bool focused = option.state.testFlag(QStyle::State_HasFocus);
        const auto group = enabled ? QPalette::Active : QPalette::Disabled;
        const auto text = option.palette.color(group, QPalette::Text);
        const auto muted = option.palette.color(group, QPalette::PlaceholderText);
        const auto accent = option.palette.color(group, QPalette::Link);
        const QRect tile = option.rect.adjusted(2, 2, -2, -2);
        painter->save();
        painter->setClipRect(option.rect);
        painter->setRenderHint(QPainter::Antialiasing);
        if (selected || hovered || focused) {
            painter->setBrush(option.palette.color(selected ? QPalette::Highlight : QPalette::Button));
            QColor outline = selected || focused ? accent : Qt::transparent;
            if (selected && !focused) outline.setAlpha(105);
            painter->setPen(QPen(outline, 1));
            painter->drawRoundedRect(QRectF(tile).adjusted(.5, .5, -.5, -.5), 4, 4);
        }

        const int edge = grid ? 48 : 32;
        const QRect iconRect(grid ? tile.center().x() - edge / 2 : tile.left() + 6,
            grid ? tile.top() + 5 : tile.top() + (tile.height() - edge) / 2, edge, edge);
        const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            const auto pixmap = icon.pixmap(QSize(edge * 2, edge * 2), enabled ? QIcon::Normal : QIcon::Disabled);
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            // 部分游戏图标原图仅32px，明确绘制到头像区域而非在48px空框内保持小图。
            painter->drawPixmap(iconRect, pixmap);
        } else {
            PaintMissingIcon(painter, iconRect, index.data(Qt::UserRole + 2).toString(), muted, option.palette);
        }

        QFont nameFont = option.font;
        nameFont.setPixelSize(12);
        nameFont.setWeight(selected ? QFont::Medium : QFont::Normal);
        painter->setFont(nameFont);
        painter->setPen(selected && enabled ? accent : text);
        const QRect nameRect = grid ? QRect(tile.left() + 5, iconRect.bottom() + 4, tile.width() - 10, 18)
            : QRect(iconRect.right() + 9, tile.top() + 3, tile.right() - iconRect.right() - 17, 18);
        const int alignment = grid ? Qt::AlignHCenter | Qt::AlignVCenter : Qt::AlignLeft | Qt::AlignVCenter;
        painter->drawText(nameRect, alignment,
            QFontMetrics(nameFont).elidedText(index.data(Qt::UserRole + 1).toString(), Qt::ElideRight, nameRect.width()));

        QFont metaFont = option.font;
        metaFont.setPixelSize(10);
        painter->setFont(metaFont);
        painter->setPen(muted);
        QString metadata = index.data(Qt::UserRole).toString();
        if (!grid) {
            const auto classification = index.data(Qt::UserRole + 3).toString();
            if (!classification.isEmpty()) metadata += QStringLiteral("  ·  ") + classification;
        }
        const QRect metaRect(nameRect.left(), nameRect.bottom(), nameRect.width(), 15);
        painter->drawText(metaRect, alignment,
            QFontMetrics(metaFont).elidedText(metadata, Qt::ElideRight, metaRect.width()));
        painter->restore();
    }

private:
    static void PaintMissingIcon(QPainter* painter, const QRect& rect, const QString& kind,
        const QColor& foreground, const QPalette& palette) {
        painter->save();
        painter->translate(rect.topLeft());
        painter->scale(rect.width() / 48.0, rect.height() / 48.0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(palette.color(QPalette::AlternateBase));
        painter->drawRoundedRect(QRectF(0, 0, 48, 48), 4, 4);
        QColor ink = foreground;
        ink.setAlpha(165);
        painter->setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath shape;
        if (kind == QStringLiteral("unit")) {
            painter->drawEllipse(QRectF(18, 10, 12, 13));
            shape.moveTo(11, 38); shape.lineTo(13, 30); shape.quadTo(24, 21, 35, 30); shape.lineTo(37, 38);
            shape.moveTo(18, 30); shape.lineTo(20, 38); shape.lineTo(28, 38); shape.lineTo(30, 30);
        } else if (kind == QStringLiteral("ability")) {
            shape.moveTo(25, 8); shape.lineTo(13, 27); shape.lineTo(23, 25);
            shape.lineTo(20, 40); shape.lineTo(36, 18); shape.lineTo(25, 20); shape.closeSubpath();
        } else if (kind == QStringLiteral("buff")) {
            shape.moveTo(24, 9); shape.lineTo(36, 14); shape.lineTo(34, 28);
            shape.quadTo(31, 35, 24, 39); shape.quadTo(17, 35, 14, 28);
            shape.lineTo(12, 14); shape.closeSubpath();
            shape.moveTo(18, 24); shape.lineTo(22, 28); shape.lineTo(30, 19);
        } else if (kind == QStringLiteral("doodad")) {
            shape.moveTo(24, 9); shape.lineTo(12, 28); shape.lineTo(36, 28); shape.closeSubpath();
            shape.moveTo(24, 29); shape.lineTo(24, 39); shape.moveTo(19, 39); shape.lineTo(29, 39);
        } else if (kind == QStringLiteral("upgrade")) {
            shape.moveTo(13, 22); shape.lineTo(24, 10); shape.lineTo(35, 22);
            shape.moveTo(13, 33); shape.lineTo(24, 21); shape.lineTo(35, 33);
            shape.moveTo(13, 39); shape.lineTo(35, 39);
        } else {
            shape.moveTo(12, 18); shape.lineTo(24, 11); shape.lineTo(36, 18); shape.lineTo(36, 33);
            shape.lineTo(24, 40); shape.lineTo(12, 33); shape.closeSubpath();
            shape.moveTo(12, 18); shape.lineTo(24, 25); shape.lineTo(36, 18);
            shape.moveTo(24, 25); shape.lineTo(24, 40);
        }
        painter->drawPath(shape);
        painter->restore();
    }

    QListView* view_;
};

} // namespace CatalogDetail

using namespace CatalogDetail;


// 图标仅在可见行或悬浮项请求 data 时解码，不在目录装载时遍历解码整个游戏。
class ObjectEntryModel final : public QAbstractListModel {
public:
    explicit ObjectEntryModel(IconProvider* icons, QObject* parent)
        : QAbstractListModel(parent), icons_(icons) {}

    void SetEntries(const QVector<ObjectViewEntry>& entries) {
        beginResetModel();
        entries_ = entries;
        endResetModel();
    }

    void RefreshIcons() {
        if (!entries_.isEmpty()) {
            emit dataChanged(index(0), index(static_cast<int>(entries_.size()) - 1),
                {Qt::DecorationRole, Qt::DisplayRole, Qt::ToolTipRole});
        }
    }

    const QVector<ObjectViewEntry>& Entries() const noexcept { return entries_; }
    const ObjectViewEntry* EntryAt(int row) const {
        return row >= 0 && row < entries_.size() ? &entries_[row] : nullptr;
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(entries_.size());
    }
    QVariant data(const QModelIndex& index, int role) const override {
        const auto* entry = EntryAt(index.row());
        if (!index.isValid() || !entry) {
            return {};
        }
        if (role == Qt::DecorationRole) {
            return EntryIcon(icons_, *entry);
        }
        if (role == Qt::DisplayRole) {
            QString text = EntryName(*entry) + '\n' + entry->id;
            const auto status = IconStatus(icons_, *entry);
            if (!status.isEmpty()) {
                text += '\n' + status;
            }
            return text;
        }
        if (role == Qt::ToolTipRole) {
            return EntryTooltip(icons_, *entry);
        }
        if (role == Qt::AccessibleTextRole) {
            return KindName(entry->kind) + ' ' + EntryName(*entry) + ' ' + entry->id;
        }
        if (role == Qt::UserRole) {
            return entry->id;
        }
        if (role == Qt::UserRole + 1) return EntryName(*entry);
        if (role == Qt::UserRole + 2) return entry->kind;
        if (role == Qt::UserRole + 3) {
            QStringList labels;
            if (!entry->race.isEmpty()) labels << ClassificationName(entry->race);
            if (!entry->category.isEmpty()) labels << ClassificationName(entry->category);
            labels.removeDuplicates();
            return labels.join(QStringLiteral(" · "));
        }
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        const auto* entry = EntryAt(index.row());
        if (!index.isValid() || !entry) {
            return Qt::NoItemFlags;
        }
        return entry->available ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }

private:
    IconProvider* icons_ = nullptr;
    QVector<ObjectViewEntry> entries_;
};

class ObjectEntryFilter final : public QSortFilterProxyModel {
public:
    explicit ObjectEntryFilter(QObject* parent) : QSortFilterProxyModel(parent) {}
    void SetFilters(const QString& kind, const QString& race, const QString& category, const QString& search) {
        kind_ = kind;
        race_ = race;
        category_ = category;
        words_ = search.simplified().split(' ', Qt::SkipEmptyParts);
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex&) const override {
        const auto* model = static_cast<const ObjectEntryModel*>(sourceModel());
        const auto* entry = model->EntryAt(row);
        if (!entry || (!kind_.isEmpty() && entry->kind != kind_) ||
            (!race_.isEmpty() && entry->race != race_) ||
            (!category_.isEmpty() && entry->category != category_)) {
            return false;
        }
        const auto searchable = entry->name + ' ' + entry->id + ' ' + entry->kind + ' ' +
            KindName(entry->kind) + ' ' + entry->race + ' ' + entry->category + ' ' + entry->iconPath + ' ' +
            ClassificationName(entry->race) + ' ' + ClassificationName(entry->category);
        return std::all_of(words_.begin(), words_.end(), [&searchable](const QString& word) {
            return searchable.contains(word, Qt::CaseInsensitive);
        });
    }
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        const auto* model = static_cast<const ObjectEntryModel*>(sourceModel());
        const auto* a = model->EntryAt(left.row());
        const auto* b = model->EntryAt(right.row());
        if (!a || !b) {
            return left.row() < right.row();
        }
        const int compare = QString::localeAwareCompare(EntryName(*a), EntryName(*b));
        return compare == 0 ? a->id < b->id : compare < 0;
    }

private:
    QString kind_;
    QString race_;
    QString category_;
    QStringList words_;
};


} // namespace War3Map::QtApp
