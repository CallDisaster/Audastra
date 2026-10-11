#include "ObjectModels.h"
#include "ObjectModelsDetail.h"
#include "Widgets/RoundedPopups.h"
#include "Widgets/WarcraftTextPreview.h"
#include "Widgets/TablePopupAnchor.h"

#include <QPlainTextEdit>
#include <QAbstractItemView>
#include <QApplication>
#include <QItemSelectionModel>
#include <QScrollBar>
#include <QTimer>

using namespace War3Map::ObjectData;
using namespace ObjectModelsDetail;

// 字段编辑委托：按字段类型提供编辑器与引用选择。

ObjectFieldDelegate::ObjectFieldDelegate(QObject* parent) : QStyledItemDelegate(parent) {
    if (auto* view = qobject_cast<QAbstractItemView*>(parent)) {
        view->installEventFilter(this);
        view->setEditTriggers(view->editTriggers() | QAbstractItemView::AnyKeyPressed | QAbstractItemView::EditKeyPressed);
    }
}
bool ObjectFieldDelegate::eventFilter(QObject* object, QEvent* event) {
    if (object == parent()) {
        auto* view = qobject_cast<QAbstractItemView*>(object);
        if (view && event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (resourcePickerFactory_ && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
                const auto current = view->currentIndex().siblingAtColumn(3);
                const auto* fields = qobject_cast<const ObjectFieldModel*>(current.model());
                const auto* definition = fields ? fields->DefinitionAt(current.row()) : nullptr;
                if (definition && War3Map::Services::DescribeObjectResourceField(*definition)) {
                    view->setCurrentIndex(current);
                    QStyleOptionViewItem option; option.widget = view->viewport(); option.rect = view->visualRect(current);
                    return editorEvent(event, view->model(), option, current);
                }
            }
            const auto text = key->text();
            const bool numeric = !text.isEmpty() && (text.front().isDigit() || text.front() == '-' || text.front() == '+' || text.front() == '.');
            if (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
                (key->key() == Qt::Key_F2 || numeric) && view->currentIndex().isValid()) {
                const auto value = view->currentIndex().siblingAtColumn(3);
                if (value.flags() & Qt::ItemIsEditable) view->setCurrentIndex(value);
            }
        }
        return QObject::eventFilter(object, event);
    }
    return QStyledItemDelegate::eventFilter(object, event);
}
void ObjectFieldDelegate::SetReferenceContext(War3Map::QtApp::IconProvider* icons,
    const QVector<War3Map::QtApp::ObjectViewEntry>& entries) {
    if (referenceEditor_) referenceEditor_->close();
    icons_ = icons;
    entries_ = entries;
}
QWidget* ObjectFieldDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const auto* model = qobject_cast<const ObjectFieldModel*>(index.model());
    if (index.column() != 3 || !model) return QStyledItemDelegate::createEditor(parent, option, index);
    const auto* definition = model->DefinitionAt(index.row());
    const auto value = model->EditorValueAt(index.row());
    if (!value) return nullptr;
    if (definition && resourcePickerFactory_ && War3Map::Services::DescribeObjectResourceField(*definition)) return nullptr;
    if (definition && (!definition->referenceTypes.empty() || IsMultilineTextField(definition, value))) return nullptr;
    if (definition && !definition->choices.empty()) {
        auto* choices = new QComboBox(parent);
        for (const auto& choice : definition->choices) choices->addItem(ToQString(choice.displayName), ToQString(choice.value));
        return choices;
    }
    if (std::holds_alternative<std::int32_t>(*value)) {
        auto* spin = new QSpinBox(parent);
        spin->setRange((std::numeric_limits<std::int32_t>::min)(), (std::numeric_limits<std::int32_t>::max)());
        spin->setFrame(false);
        return spin;
    }
    if (std::holds_alternative<bool>(*value)) return new QCheckBox(parent);
    // 实数用完整精度文本输入，避免QDoubleSpinBox打开即四舍五入/截断。
    auto* edit = new QLineEdit(parent);
    edit->setFrame(false);
    return edit;
}
void ObjectFieldDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    const auto* model = qobject_cast<const ObjectFieldModel*>(index.model());
    const auto value = model ? model->EditorValueAt(index.row()) : std::nullopt;
    if (!value) return;
    if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
        spin->setValue(std::get<std::int32_t>(*value)); spin->setFocus(Qt::OtherFocusReason); spin->selectAll(); return;
    }
    if (auto* check = qobject_cast<QCheckBox*>(editor)) { check->setChecked(std::get<bool>(*value)); return; }
    const QString text = ToQString(ValueToDisplayString(*value));
    if (auto* choices = qobject_cast<QComboBox*>(editor)) {
        int item = choices->findData(text);
        if (item < 0) { choices->addItem(tr("未知值 %1").arg(text), text); item = choices->count() - 1; }
        choices->setCurrentIndex(item);
        return;
    }
    if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
        edit->setText(text); edit->setFocus(Qt::OtherFocusReason); edit->selectAll();
    }
}
void ObjectFieldDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const {
    if (auto* spin = qobject_cast<QSpinBox*>(editor)) { model->setData(index, spin->value(), Qt::EditRole); return; }
    if (auto* check = qobject_cast<QCheckBox*>(editor)) { model->setData(index, check->isChecked() ? "true" : "false", Qt::EditRole); return; }
    if (auto* choices = qobject_cast<QComboBox*>(editor)) { model->setData(index, choices->currentData(), Qt::EditRole); return; }
    if (auto* edit = qobject_cast<QLineEdit*>(editor)) model->setData(index, edit->text(), Qt::EditRole);
}

bool ObjectFieldDelegate::editorEvent(QEvent* event, QAbstractItemModel* abstractModel,
    const QStyleOptionViewItem& option, const QModelIndex& index) {
    auto* model = qobject_cast<ObjectFieldModel*>(abstractModel);
    const auto* definition = model ? model->DefinitionAt(index.row()) : nullptr;
    if (!model || index.column() != 3 || !definition ||
        !(model->flags(index) & Qt::ItemIsEditable)) return QStyledItemDelegate::editorEvent(event, abstractModel, option, index);
    const auto value = model->EditorValueAt(index.row());
    const auto resource = resourcePickerFactory_ ? War3Map::Services::DescribeObjectResourceField(*definition) : std::nullopt;
    const bool multiline = IsMultilineTextField(definition, value);
    if (!resource && !multiline && definition->referenceTypes.empty())
        return QStyledItemDelegate::editorEvent(event, abstractModel, option, index);
    const auto* mouse = dynamic_cast<QMouseEvent*>(event);
    const auto* key = dynamic_cast<QKeyEvent*>(event);
    if (!((event->type() == (resource ? QEvent::MouseButtonDblClick : QEvent::MouseButtonRelease) && mouse && mouse->button() == Qt::LeftButton) ||
        (event->type() == QEvent::KeyPress && key &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_F2 || key->key() == Qt::Key_Space)))) return false;
    if (referenceEditor_) { referenceEditor_->close(); referenceEditor_->deleteLater(); }
    auto* anchor = const_cast<QWidget*>(option.widget);
    if (!anchor || !anchor->screen()) return false;
    const QPersistentModelIndex persistent(index);
    const QPointer<ObjectFieldModel> guard(model);
    if (resource && value) {
        const auto original = ToQString(ValueToDisplayString(*value));
        const QPointer<ObjectFieldDelegate> self(this);
        const QPointer<QWidget> anchorGuard(anchor);
        const auto factory = resourcePickerFactory_;
        auto services = factory();
        if (!self || !guard || !persistent.isValid() || !anchorGuard) return false;
        auto* picker = new War3Map::Modules::ObjectResourcePicker(*resource,
            original, std::move(services), anchor);
        picker->SetCommit([guard, persistent](const QString& submitted) {
            return guard && persistent.isValid() && guard->setData(persistent, submitted, Qt::EditRole);
        });
        referenceEditor_ = picker;
        picker->show(); War3Map::UiKit::AnchorTablePopup(picker, anchor, index); picker->raise();
        return true;
    }
    if (multiline) {
        if (!value) return false;
        const QPointer<ObjectFieldDelegate> self(this);
        const auto label = ObjectFieldLabel(definition);
        const QPointer<QWidget> anchorGuard(anchor);
        auto previewSource = model->PrepareColourPreview();
        if (!self || !guard || !persistent.isValid() || !anchorGuard) return false;
        auto* panel = new QFrame(anchor, Qt::Tool | Qt::FramelessWindowHint);
        panel->setObjectName(QStringLiteral("objectMultilineEditor"));
        panel->setWindowModality(Qt::NonModal);
        panel->setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(panel);
        layout->addWidget(new QLabel(label, panel));
        auto* text = new QPlainTextEdit(panel);
        text->setObjectName(QStringLiteral("objectMultilineText"));
        const std::string rawOriginal = ValueToDisplayString(*value);
        const QString original = ToQString(rawOriginal);
        text->setPlainText(original);
        const QString editorBaseline = text->toPlainText(); // Qt 仅规范化控件显示；未编辑时仍保留原字节。
        layout->addWidget(text, 1);
        auto* preview = new War3Map::UiKit::WarcraftTextPreview(panel);
        const QPointer<QFrame> panelGuard(panel);
        const QPointer<War3Map::UiKit::WarcraftTextPreview> previewGuard(preview);
        preview->Bind(text, tr("当前字段草稿；WTS 引用未提供可信正文时不解析编号。"),
            original.toUtf8().toStdString() == rawOriginal, std::move(previewSource));
        if (!self || !guard || !persistent.isValid() || !anchorGuard || !panelGuard || !previewGuard) {
            if (panelGuard) panelGuard->deleteLater();
            return false;
        }
        connect(model, &QAbstractItemModel::dataChanged, preview, &War3Map::UiKit::WarcraftTextPreview::Refresh);
        layout->addWidget(preview);
        auto* status = new QLabel(panel);
        status->setObjectName(QStringLiteral("objectMultilineStatus"));
        status->setTextFormat(Qt::PlainText);
        layout->addWidget(status);
        auto* actions = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, panel);
        layout->addWidget(actions);
        connect(actions->button(QDialogButtonBox::Apply), &QPushButton::clicked, panel,
            [guard, persistent, panelGuard, text, status, editorBaseline] {
                const QString edited = text->toPlainText();
                if (edited == editorBaseline) {
                    if (panelGuard) panelGuard->close();
                    return;
                }
                if (guard && persistent.isValid() && guard->setData(persistent, edited, Qt::EditRole)) {
                    if (panelGuard) panelGuard->close();
                } else if (panelGuard) status->setText(QObject::tr("字段未写入；当前值或字段结构无法安全编辑。"));
            });
        connect(actions, &QDialogButtonBox::rejected, panel, &QWidget::close);
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), panel);
        connect(escape, &QShortcut::activated, panel, &QWidget::close);
        connect(model, &QAbstractItemModel::modelAboutToBeReset, panel, &QWidget::close);
        self->referenceEditor_ = panel;
        const QRect available = anchor->screen()->availableGeometry();
        const QSize size = QSize(620, 420).boundedTo(available.size());
        const QPoint origin = anchor->mapToGlobal(option.rect.topRight());
        panel->setGeometry(QRect(QPoint(std::clamp(origin.x(), available.left(), available.right() - size.width() + 1),
            std::clamp(origin.y(), available.top(), available.bottom() - size.height() + 1)), size));
        War3Map::UiKit::ConfigureRoundedPopup(*panel);
        panel->show();
        War3Map::UiKit::AnchorTablePopup(panel, anchor, index);
        panel->raise();
        text->setFocus();
        return true;
    }
    const auto* modification = model->ModificationAt(index.row());
    if (!modification) return false;
    QVector<War3Map::QtApp::ObjectViewEntry> candidates;
    for (const auto& entry : entries_) {
        for (const auto type : definition->referenceTypes) {
            if (entry.kind == ReferenceKind(type)) { candidates.push_back(entry); break; }
        }
    }
    if (!definition->isList) {
        auto* picker = new War3Map::QtApp::ObjectPickerPopup(icons_, anchor);
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->SetEntries(candidates);
        picker->SetTitle(ObjectFieldLabel(definition));
        picker->SetCurrentId(ToQString(ValueToDisplayString(modification->value)));
        referenceEditor_ = picker;
        connect(model, &QAbstractItemModel::modelAboutToBeReset, picker, &QWidget::close);
        connect(picker, &War3Map::QtApp::ObjectPickerPopup::EntryChosen, picker,
            [guard, persistent, picker](const War3Map::QtApp::ObjectViewEntry& entry) {
                if (guard && persistent.isValid()) guard->setData(persistent, entry.id, Qt::EditRole);
                picker->close();
            });
        // 用当前单元格作锚点，避免浮窗始终贴在整张属性表的顶端。
        auto* cellAnchor = new QWidget(anchor);
        cellAnchor->setGeometry(option.rect);
        cellAnchor->setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(picker, &QObject::destroyed, cellAnchor, &QObject::deleteLater);
        picker->OpenAt(cellAnchor);
        War3Map::UiKit::AnchorTablePopup(picker, anchor, index, cellAnchor);
    } else {
        auto* panel = new QFrame(anchor, Qt::Tool | Qt::FramelessWindowHint);
        panel->setObjectName(QStringLiteral("objectReferenceEditor"));
        panel->setWindowModality(Qt::NonModal);
        panel->setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(panel);
        layout->addWidget(new QLabel(ObjectFieldLabel(definition), panel));
        auto* list = new War3Map::QtApp::OrderedObjectListWidget(icons_, panel);
        list->SetEntries(candidates);
        list->SetKnownEntries(entries_);
        if (definition->referenceTypes.size() == 1) list->SetKindFilter(ReferenceKind(definition->referenceTypes.front()));
        list->SetIds(ReferenceIds(modification->value));
        list->SetInlineCatalogEnabled(true);
        layout->addWidget(list);
        auto* actions = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, panel);
        layout->addWidget(actions);
        connect(actions->button(QDialogButtonBox::Apply), &QPushButton::clicked, panel, [guard, persistent, panel, list] {
            if (guard && persistent.isValid() && guard->setData(persistent, list->Ids(), Qt::EditRole)) panel->close();
        });
        connect(actions, &QDialogButtonBox::rejected, panel, &QWidget::close);
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), panel);
        connect(escape, &QShortcut::activated, panel, &QWidget::close);
        connect(model, &QAbstractItemModel::modelAboutToBeReset, panel, &QWidget::close);
        referenceEditor_ = panel;
        const QRect available = anchor->screen()->availableGeometry();
        const QSize size = QSize(860, 500).boundedTo(available.size());
        const QPoint origin = anchor->mapToGlobal(option.rect.topRight());
        panel->setGeometry(QRect(QPoint(std::clamp(origin.x(), available.left(), available.right() - size.width() + 1),
            std::clamp(origin.y(), available.top(), available.bottom() - size.height() + 1)), size));
        War3Map::UiKit::ConfigureRoundedPopup(*panel);
        panel->show();
        War3Map::UiKit::AnchorTablePopup(panel, anchor, index);
        panel->raise();
    }
    return true;
}
