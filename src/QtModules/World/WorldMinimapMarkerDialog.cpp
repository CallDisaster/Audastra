#include "WorldMinimapMarkerDialog.h"
#include "EditorProject.h"
#include "Icons/IconProvider.h"
#include "Icons/SvgIcons.h"
#include "ProjectMinimapPreview.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QFormLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRubberBand>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace War3Map::Modules {
namespace {
using Services::MinimapMarkerEdit;
using Services::MinimapMarkerOperation;
using Services::MinimapMarkerOrigin;
using Services::MinimapMarkerView;
using MapData::MinimapMarkerRecord;

QString Tr(const char* text) { return QCoreApplication::translate("MinimapMarkerDialog", text); }

struct MarkerStyle {
    QString title;
    QIcon icon;
};
using MarkerStyles = std::array<MarkerStyle, 4>;

MarkerStyles LoadStyles(QtProject::EditorProject& project) {
    // 经典版 war3.mpq 的大厅图标；资源仍由已挂载的游戏目录提供。
    constexpr std::array names{"金矿", "中立建筑", "开始位置", "未知图标"};
    constexpr std::array files{"MinimapIconGold.blp", "MinimapIconNeutralBuilding.blp", "MinimapIconStartLoc.blp"};
    MarkerStyles styles;
    for (std::size_t i = 0; i < styles.size(); ++i) {
        auto& style = styles[i];
        style.title = Tr(names[i]);
        if (i < files.size() && project.iconProvider_) {
            const auto path = QStringLiteral("UI/MiniMap/MiniMapIcon/") + QString::fromLatin1(files[i]);
            if (project.iconProvider_->HasGameFile(path)) style.icon = project.iconProvider_->IconForTexture(path);
        }
        if (style.icon.isNull()) style.icon = UiKit::SvgIcon(
            i == 2 ? QStringLiteral("lucide/flag") : QStringLiteral("lucide/map-pin"));
    }
    return styles;
}

const MarkerStyle& Style(const MarkerStyles& styles, std::int32_t type) {
    return styles[type >= 0 && type < 3 ? static_cast<std::size_t>(type) : 3];
}

QColor ColorOf(const MinimapMarkerRecord& marker) {
    const auto& c = marker.colorBgra;
    return {c[2], c[1], c[0], c[3]};
}

class MarkerList final : public QAbstractListModel {
public:
    MarkerList(const MarkerStyles& styles, QObject* parent) : QAbstractListModel(parent), styles_(styles) {}
    const std::optional<MinimapMarkerView>& View() const { return view_; }
    void SetView(std::optional<MinimapMarkerView> view) {
        beginResetModel();
        view_ = std::move(view);
        endResetModel();
    }
    int rowCount(const QModelIndex& parent) const override {
        return view_ && !parent.isValid() ? static_cast<int>(view_->records.size()) : 0;
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount({})) return {};
        const auto row = static_cast<std::size_t>(index.row());
        const auto& style = Style(styles_, view_->records[row].typeRaw);
        if (role == Qt::DecorationRole) return style.icon;
        if (role == Qt::DisplayRole) return Tr("%1 · %2").arg(style.title, view_->origins[row] == MinimapMarkerOrigin::Generated
            ? Tr("自动") : view_->origins[row] == MinimapMarkerOrigin::Author ? Tr("手动") : Tr("旧图标"));
        return {};
    }
private:
    const MarkerStyles& styles_;
    std::optional<MinimapMarkerView> view_;
};

class MarkerCanvas final : public QWidget {
public:
    MarkerCanvas(const MarkerList& model, const MarkerStyles& styles, QWidget* parent)
        : QWidget(parent), model_(model), styles_(styles), band_(QRubberBand::Rectangle, this) {
        setObjectName(QStringLiteral("minimapMarkerCanvas"));
        setAccessibleName(Tr("图标位置"));
        setMinimumSize(300, 300);
    }
    void SetBackground(QImage image) {
        background_ = std::move(image);
        update();
    }
    [[nodiscard]] bool HasBackground() const { return !background_.isNull(); }
    void SetPlacement(bool active) {
        placing_ = active;
        setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    std::vector<int> selected;
    std::function<void(const std::vector<int>&, bool toggle)> select;
    std::function<void(QPoint)> place;
    std::function<void(int, QPoint)> move;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::Base));
        const auto imageRect = ImageRect();
        if (!HasBackground()) return;
        painter.drawImage(imageRect, background_);
        painter.setClipRect(imageRect);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!model_.View()) return;
        const auto& rows = model_.View()->records;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& marker = rows[i];
            if (!Valid(marker)) continue;
            const QPointF point = dragRow_ == static_cast<int>(i) && dragging_ ? PointOf(Pixel(last_))
                : PointOf({marker.positionRaw[0], marker.positionRaw[1]});
            const bool active = std::ranges::binary_search(selected, static_cast<int>(i));
            painter.setPen(QPen(palette().color(active ? QPalette::Highlight : QPalette::Base), active ? 3 : 1));
            painter.setBrush(palette().brush(QPalette::Window));
            painter.drawEllipse(point, 10, 10);
            Style(styles_, marker.typeRaw).icon.paint(&painter, QRectF(point - QPointF(7, 7), QSizeF(14, 14)).toRect());
            painter.setPen(QPen(palette().color(QPalette::WindowText), 1));
            painter.setBrush(ColorOf(marker));
            painter.drawEllipse(point + QPointF(7, 7), 3.5, 3.5);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !HasBackground() || !ImageRect().contains(event->position())) return;
        press_ = last_ = event->position();
        pressed_ = true;
        toggling_ = bool(event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));
        dragRow_ = placing_ ? -1 : Hit(press_);
        if (dragRow_ >= 0 && select) select({dragRow_}, toggling_);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!pressed_ || !(event->buttons() & Qt::LeftButton)) return;
        last_ = event->position();
        dragging_ |= (last_ - press_).manhattanLength() >= QApplication::startDragDistance();
        if (dragging_ && dragRow_ < 0 && !placing_) {
            band_.setGeometry(QRectF(press_, last_).normalized().toAlignedRect());
            band_.show();
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !pressed_) return;
        const auto end = event->position();
        const bool dragged = dragging_ || (end - press_).manhattanLength() >= QApplication::startDragDistance();
        const int row = std::exchange(dragRow_, -1);
        pressed_ = dragging_ = false;
        band_.hide();
        if (placing_ && !dragged && ImageRect().contains(end) && place) place(Pixel(end));
        else if (!placing_ && row >= 0 && dragged && !toggling_ && move) move(row, Pixel(end));
        else if (!placing_ && row < 0 && select) {
            std::vector<int> ids;
            if (dragged && model_.View()) {
                const auto box = QRectF(press_, end).normalized();
                const auto& rows = model_.View()->records;
                for (std::size_t i = 0; i < rows.size(); ++i)
                    if (Valid(rows[i]) && box.contains(PointOf({rows[i].positionRaw[0], rows[i].positionRaw[1]})))
                        ids.push_back(static_cast<int>(i));
            }
            select(ids, toggling_);
        }
        update();
    }

private:
    static bool Valid(const MinimapMarkerRecord& marker) {
        return std::ranges::all_of(marker.positionRaw, [](auto v) { return v >= 0 && v < 256; });
    }
    QRectF ImageRect() const {
        const auto side = std::min(width(), height());
        return {(width() - side) / 2.0, (height() - side) / 2.0, qreal(side), qreal(side)};
    }
    QPointF PointOf(QPoint pixel) const {
        const auto box = ImageRect();
        return box.topLeft() + QPointF((pixel.x() + 0.5) * box.width() / 256, (pixel.y() + 0.5) * box.height() / 256);
    }
    QPoint Pixel(QPointF point) const {
        const auto box = ImageRect();
        return {std::clamp(static_cast<int>(std::floor((point.x() - box.left()) * 256 / box.width())), 0, 255),
            std::clamp(static_cast<int>(std::floor((point.y() - box.top()) * 256 / box.height())), 0, 255)};
    }
    int Hit(QPointF point) const {
        if (!model_.View()) return -1;
        int result = -1;
        qreal nearest = 12 * 12;
        const auto& rows = model_.View()->records;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (!Valid(row)) continue;
            const auto delta = point - PointOf({row.positionRaw[0], row.positionRaw[1]});
            if (const auto distance = QPointF::dotProduct(delta, delta); distance <= nearest) {
                nearest = distance;
                result = static_cast<int>(i);
            }
        }
        return result;
    }
    const MarkerList& model_;
    const MarkerStyles& styles_;
    QRubberBand band_;
    QImage background_;
    QPointF press_, last_;
    int dragRow_ = -1;
    bool pressed_ = false, dragging_ = false, placing_ = false, toggling_ = false;
};
} // namespace

void OpenMinimapMarkerDialog(QtProject::EditorProject& project, QWidget* parent) {
    const auto styles = LoadStyles(project);
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("minimapMarkerDialog"));
    dialog.setWindowTitle(Tr("小地图图标"));
    dialog.resize(760, 500);
    auto* outer = new QVBoxLayout(&dialog);
    auto* body = new QHBoxLayout;
    outer->addLayout(body, 1);
    auto* model = new MarkerList(styles, &dialog);
    auto* canvas = new MarkerCanvas(*model, styles, &dialog);
    body->addWidget(canvas, 3);
    auto* tools = new QVBoxLayout;
    body->addLayout(tools, 2);
    auto* addRow = new QHBoxLayout;
    tools->addLayout(addRow);
    auto* type = new QComboBox(&dialog);
    type->setObjectName(QStringLiteral("minimapMarkerType"));
    type->setAccessibleName(Tr("图标类型"));
    for (std::size_t i = 0; i < 3; ++i) type->addItem(styles[i].icon, styles[i].title, static_cast<int>(i));
    addRow->addWidget(type, 1);
    auto* color = new QPushButton(Tr("颜色"), &dialog);
    color->setObjectName(QStringLiteral("minimapMarkerColor"));
    addRow->addWidget(color);
    auto* add = new QToolButton(&dialog);
    add->setObjectName(QStringLiteral("minimapMarkerPlace"));
    add->setText(Tr("放置"));
    add->setCheckable(true);
    addRow->addWidget(add);
    auto* list = new QListView(&dialog);
    list->setObjectName(QStringLiteral("minimapMarkerList"));
    list->setAccessibleName(Tr("图标"));
    list->setModel(model);
    list->setUniformItemSizes(true);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tools->addWidget(list, 1);
    auto* advanced = new QToolButton(&dialog);
    advanced->setText(Tr("高级坐标"));
    advanced->setCheckable(true);
    tools->addWidget(advanced);
    auto* coordinates = new QWidget(&dialog);
    auto* form = new QFormLayout(coordinates);
    form->setContentsMargins(0, 0, 0, 0);
    auto* x = new QSpinBox(coordinates); x->setRange(0, 255);
    auto* y = new QSpinBox(coordinates); y->setRange(0, 255);
    auto* applyPosition = new QPushButton(Tr("应用坐标"), coordinates);
    form->addRow(Tr("X"), x);
    form->addRow(Tr("Y"), y);
    form->addRow(applyPosition);
    coordinates->hide();
    tools->addWidget(coordinates);
    auto* footer = new QHBoxLayout;
    outer->addLayout(footer);
    auto* message = new QLabel(&dialog);
    message->setWordWrap(true);
    message->hide();
    footer->addWidget(message, 1);
    auto* remove = new QPushButton(Tr("删除所选"), &dialog);
    remove->setObjectName(QStringLiteral("minimapMarkerRemove"));
    footer->addWidget(remove);
    auto* close = new QPushButton(Tr("关闭"), &dialog);
    footer->addWidget(close);
    QColor chosen(Qt::white);
    const auto updateColor = [&] {
        QPixmap swatch(16, 16);
        swatch.fill(chosen);
        color->setIcon(QIcon(swatch));
    };
    const auto selected = [&] {
        std::vector<int> rows;
        for (const auto& index : list->selectionModel()->selectedRows()) rows.push_back(index.row());
        std::ranges::sort(rows);
        return rows;
    };
    const auto updateSelection = [&] {
        canvas->selected = selected();
        canvas->update();
        remove->setEnabled(!canvas->selected.empty());
        const bool single = model->View() && canvas->selected.size() == 1;
        coordinates->setEnabled(single);
        if (single) {
            const auto& p = model->View()->records[static_cast<std::size_t>(canvas->selected.front())].positionRaw;
            x->setValue(p[0]); y->setValue(p[1]);
        }
    };
    const auto report = [&](const auto& diagnostics) {
        project.AddDiagnostics(diagnostics);
        message->setText(Tr("无法编辑图标。"));
        message->show();
    };
    const auto refresh = [&] {
        auto query = project.QueryMinimapMarkers();
        if (!query.Succeeded() || !query.value) {
            model->SetView({});
            report(query.diagnostics);
        } else {
            model->SetView(std::move(*query.value));
            message->hide();
        }
        add->setEnabled(model->View().has_value() && canvas->HasBackground());
        updateSelection();
    };
    const auto apply = [&](std::span<const MinimapMarkerEdit> edits) {
        if (!model->View() || edits.empty()) return false;
        const auto status = project.EditMinimapMarkers(model->View()->revision, edits);
        if (!status.Succeeded()) { report(status.diagnostics); return false; }
        refresh();
        return true;
    };
    canvas->select = [&](const std::vector<int>& rows, bool toggle) {
        auto* selection = list->selectionModel();
        if (!toggle) selection->clearSelection();
        QItemSelection items;
        for (const int row : rows) {
            const auto index = model->index(row, 0);
            if (index.isValid()) items.select(index, index);
        }
        selection->select(items, toggle ? QItemSelectionModel::Toggle : QItemSelectionModel::Select);
    };
    canvas->place = [&](QPoint position) {
        const MinimapMarkerEdit edit{MinimapMarkerOperation::Add, 0, {type->currentData().toInt(),
            {position.x(), position.y()}, {std::uint8_t(chosen.blue()), std::uint8_t(chosen.green()),
                std::uint8_t(chosen.red()), std::uint8_t(chosen.alpha())}}};
        if (apply(std::span(&edit, 1))) add->setChecked(false);
    };
    canvas->move = [&](int row, QPoint position) {
        if (!model->View() || row < 0 || row >= model->rowCount({})) return;
        const auto& original = model->View()->records[static_cast<std::size_t>(row)];
        if (original.positionRaw[0] == position.x() && original.positionRaw[1] == position.y()) return;
        const MinimapMarkerEdit edit{MinimapMarkerOperation::Move, static_cast<std::size_t>(row),
            {original.typeRaw, {position.x(), position.y()}, original.colorBgra}};
        (void)apply(std::span(&edit, 1));
    };
    QObject::connect(list->selectionModel(), &QItemSelectionModel::selectionChanged, &dialog, updateSelection);
    QObject::connect(add, &QToolButton::toggled, canvas, [canvas](bool on) { canvas->SetPlacement(on); });
    QObject::connect(advanced, &QToolButton::toggled, coordinates, &QWidget::setVisible);
    QObject::connect(applyPosition, &QPushButton::clicked, &dialog, [&] {
        const auto rows = selected();
        if (rows.size() == 1) canvas->move(rows.front(), {x->value(), y->value()});
    });
    QObject::connect(color, &QPushButton::clicked, &dialog, [&] {
        const auto result = QColorDialog::getColor(chosen, &dialog, Tr("图标颜色"),
            QColorDialog::DontUseNativeDialog | QColorDialog::ShowAlphaChannel);
        if (result.isValid()) { chosen = result; updateColor(); }
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        auto rows = selected();
        std::vector<MinimapMarkerEdit> edits;
        edits.reserve(rows.size());
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            edits.push_back({MinimapMarkerOperation::Remove, static_cast<std::size_t>(*it), {}});
        (void)apply(edits);
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    QtProject::ProjectMinimapPreview preview(project, &dialog);
    QObject::connect(&preview, &QtProject::ProjectMinimapPreview::Changed, &dialog, [&] {
        if (preview.Pending()) return;
        const auto& result = preview.ResultValue();
        if (result && result->Succeeded() && result->value && result->value->present) {
            const auto& image = result->value->image;
            canvas->SetBackground(QImage(reinterpret_cast<const uchar*>(image.rgba.data()), int(image.width),
                int(image.height), QImage::Format_RGBA8888).copy());
        } else if (preview.Options().current) {
            auto saved = preview.Options(); saved.current = false;
            preview.Request(saved);
            return;
        } else {
            canvas->SetBackground({});
            message->setText(Tr("小地图底图不可用。"));
            message->show();
        }
        const bool placeable = model->View().has_value() && canvas->HasBackground();
        add->setEnabled(placeable);
        if (!placeable) add->setChecked(false);
    });
    updateColor();
    refresh();
    preview.Request({});
    dialog.exec();
    // 回调引用本函数的局部对象；对话框拆除子控件前先断开，避免选区信号落到已析构的局部。
    QObject::disconnect(list->selectionModel(), nullptr, &dialog, nullptr);
}
} // namespace War3Map::Modules
