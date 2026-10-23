#include "GameUiModule.h"

#include "EditorProject.h"
#include "GameUiAuthoringJson.h"
#include "GameUiCanvas.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <cstring>

// 界面工作区的编写：新建界面描述、增删框架、拖动、改属性。每次修改都先经 UiDescriptionEdit 按文件规则检查，
// 再作为导入文件放进地图的待保存修改（导入撤销栈、保存流程与备份都沿用编辑器现有的一套）。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr int kFieldRole = Qt::UserRole + 40;

const char* IdPrefix(UiFrameType type) {
    switch (type) {
    case UiFrameType::Backdrop: return "back";
    case UiFrameType::Text: return "text";
    case UiFrameType::Bar: return "bar";
    case UiFrameType::Button: return "button";
    default: return "panel";
    }
}

// 新框架居中于父框架，带上能看见的默认外观，作者再改。
QJsonObject DefaultFrame(UiFrameType type, const QString& id) {
    const QString background = QStringLiteral("UI\\Widgets\\ToolTips\\Human\\human-tooltip-background");
    QJsonObject frame{{"id", id}, {"type", Text(std::string(UiFrameTypeName(type)))},
        {"anchors", QJsonArray{QJsonObject{{"point", "CENTER"}}}}};
    switch (type) {
    case UiFrameType::Backdrop:
        frame.insert("width", 0.12);
        frame.insert("height", 0.06);
        frame.insert("texture", background);
        break;
    case UiFrameType::Text:
        frame.insert("text", QObject::tr("文字"));
        break;
    case UiFrameType::Bar:
        frame.insert("width", 0.15);
        frame.insert("height", 0.015);
        frame.insert("texture", QStringLiteral("ReplaceableTextures\\TeamColor\\TeamColor06"));
        frame.insert("background", QStringLiteral("ReplaceableTextures\\TeamColor\\TeamColor13"));
        break;
    case UiFrameType::Button:
        frame.insert("width", 0.08);
        frame.insert("height", 0.03);
        frame.insert("texture", background);
        frame.insert("text", QObject::tr("按钮"));
        break;
    default:
        frame.insert("width", 0.12);
        frame.insert("height", 0.06);
        break;
    }
    return frame;
}

QString FirstMessage(const std::vector<Diagnostic>& diagnostics) {
    QStringList messages;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Error) continue;
        messages.push_back(Text(diagnostic.message));
        if (messages.size() == 3) break;
    }
    return messages.isEmpty() ? QObject::tr("修改不合法") : messages.join(QStringLiteral("；"));
}

QJsonArray AnchorsOf(const UiFrame& frame) { return FrameObject(frame).value(QStringLiteral("anchors")).toArray(); }

double Round4(double value) { return std::round(value * 10000.0) / 10000.0; }

} // namespace

bool GameUiModule::StageDescription(UiDescription description, bool create, const QString& undoLabel, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!definitions_) return fail(tr("界面定义仍在读取"));
    auto& files = definitions_->descriptions;
    const auto existing = std::find_if(files.begin(), files.end(),
        [&](const GameUiDescriptionFile& file) { return file.description.id == description.id; });
    if (create && existing != files.end()) return fail(tr("界面 %1 已存在").arg(Text(description.id)));
    const QString path = existing != files.end() ? Text(existing->path) : Text(UiDescriptionMemberPath(description.id));
    const std::string text = SerializeUiDescription(description);
    auto bytes = std::make_shared<std::vector<std::byte>>(text.size());
    std::memcpy(bytes->data(), text.data(), text.size());
    std::vector<QtProject::EditorProject::ImportPayload> payloads{{path, std::move(bytes), false}};
    // 自己放入的文件已在下面就地更新，不触发整体重读（重读会清空贴图缓存，画面闪烁）。
    stagingOwnImport_ = true;
    const QString problem = project_.AddImportPayloads(payloads, !create, undoLabel);
    stagingOwnImport_ = false;
    if (!problem.isEmpty()) return fail(problem);
    if (existing != files.end()) {
        existing->description = std::move(description);
        PruneDescriptionPreview(existing->description);
    } else {
        files.push_back({path.toStdString(), std::move(description)});
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.description.id < b.description.id; });
    }
    authoringDiagnostics_.clear();
    descriptionPanelFor_.clear();  // 数值与状态可能变了，面板重建
    RebuildScene();
    return true;
}

bool GameUiModule::CreateDescription(const QString& id, const QString& label, QString* error) {
    auto created = NewUiDescription(id.toStdString(), label.toStdString());
    if (!created.value) {
        if (error) *error = FirstMessage(created.diagnostics);
        return false;
    }
    if (!StageDescription(std::move(*created.value), true, tr("新建界面 %1").arg(id), error)) return false;
    (void)SelectFrame(id);
    return true;
}

bool GameUiModule::EditDescription(const QString& id, const QString& edits, const QString& undoLabel, QString* error) {
    const UiDescription* base = Description(id);
    if (!base) {
        if (error) *error = tr("地图里没有界面描述 %1").arg(id);
        return false;
    }
    auto edited = ApplyUiEdits(*base, edits.toStdString());
    if (!edited.value) {
        if (error) *error = FirstMessage(edited.diagnostics);
        return false;
    }
    return StageDescription(std::move(*edited.value), false, undoLabel.isEmpty() ? tr("修改界面 %1").arg(id) : undoLabel, error);
}

QString GameUiModule::AddDescriptionFrame(UiFrameType type, QString* error) {
    if (!tree_ || selected_ >= tree_->Nodes().size() || !tree_->Nodes()[selected_].uiFrame) {
        if (error) *error = tr("先选中一个界面描述里的框架");
        return {};
    }
    const UiFrameLink link = *tree_->Nodes()[selected_].uiFrame;
    const QString id = Text(link.description);
    const UiDescription* description = Description(id);
    if (!description) return {};
    const QString frameId = Text(NextUiFrameId(*description, IdPrefix(type)));
    QJsonObject edit{{"op", "add_frame"}, {"frame", DefaultFrame(type, frameId)}};
    if (!link.frame.empty()) edit.insert("parent", Text(link.frame));
    if (!EditDescription(id, CompactEdits({edit}), tr("界面 %1：添加 %2").arg(id, frameId), error)) return {};
    if (!SelectFrame(frameId)) (void)SelectFrame(frameId + QStringLiteral("#0"));
    return frameId;
}

bool GameUiModule::RemoveSelectedDescriptionFrame(QString* error) {
    if (!tree_ || selected_ >= tree_->Nodes().size() || !tree_->Nodes()[selected_].uiFrame ||
        tree_->Nodes()[selected_].uiFrame->frame.empty()) {
        if (error) error->clear();
        return false;
    }
    const UiFrameLink link = *tree_->Nodes()[selected_].uiFrame;
    const QString id = Text(link.description);
    const QJsonObject edit{{"op", "remove_frame"}, {"frame", Text(link.frame)}};
    if (!EditDescription(id, CompactEdits({edit}), tr("界面 %1：删除 %2").arg(id, Text(link.frame)), error)) return false;
    (void)SelectFrame(id);
    return true;
}

bool GameUiModule::MoveDescriptionFrame(std::uint32_t node, double dx, double dy, QString* error) {
    if (!tree_ || node >= tree_->Nodes().size() || !tree_->Nodes()[node].uiFrame || tree_->Nodes()[node].uiFrame->frame.empty()) {
        if (error) *error = tr("只能移动界面描述里的框架");
        return false;
    }
    const FrameNode& target = tree_->Nodes()[node];
    const UiFrameLink link = *target.uiFrame;
    const QString id = Text(link.description);
    const UiDescription* description = Description(id);
    const UiFrame* frame = description ? FindUiFrame(*description, link.frame) : nullptr;
    if (!frame) return false;
    QJsonArray anchors = AnchorsOf(*frame);
    if (anchors.isEmpty()) {
        // 没有锚点（游戏中不显示）：按当前位置相对屏幕放下。
        anchors.append(QJsonObject{{"point", "BOTTOMLEFT"}, {"to", "screen"},
            {"x", Round4(target.rect.left + dx)}, {"y", Round4(target.rect.bottom + dy)}});
    } else {
        for (qsizetype i = 0; i < anchors.size(); ++i) {
            QJsonObject anchor = anchors[i].toObject();
            anchor.insert("x", Round4(anchor.value("x").toDouble() + dx));
            anchor.insert("y", Round4(anchor.value("y").toDouble() + dy));
            anchors[i] = anchor;
        }
    }
    const QJsonObject edit{{"op", "set_frame"}, {"frame", Text(link.frame)}, {"fields", QJsonObject{{"anchors", anchors}}}};
    return EditDescription(id, CompactEdits({edit}), tr("界面 %1：移动 %2").arg(id, Text(link.frame)), error);
}

void GameUiModule::ReportAuthoring(const QString& error) {
    authoringDiagnostics_.clear();
    if (!error.isEmpty()) {
        authoringDiagnostics_.push_back(Diagnostic{DiagnosticSeverity::Error, "GameUi", "AUI_AUTHORING", error.toStdString(), std::nullopt});
    }
    UpdateProblems();
}

QList<QWidget*> GameUiModule::BuildAuthoringControls(QWidget* toolbar) {
    newDescriptionButton_ = new QToolButton(toolbar);
    newDescriptionButton_->setObjectName(QStringLiteral("gameUiNewDescription"));
    newDescriptionButton_->setText(tr("新建界面"));
    newDescriptionButton_->setPopupMode(QToolButton::InstantPopup);
    auto* createMenu = new QMenu(newDescriptionButton_);
    auto* form = new QWidget(createMenu);
    auto* formLayout = new QHBoxLayout(form);
    formLayout->setContentsMargins(6, 4, 6, 4);
    auto* idEdit = new QLineEdit(form);
    idEdit->setObjectName(QStringLiteral("gameUiNewDescriptionId"));
    idEdit->setPlaceholderText(tr("编号"));
    auto* labelEdit = new QLineEdit(form);
    labelEdit->setObjectName(QStringLiteral("gameUiNewDescriptionLabel"));
    labelEdit->setPlaceholderText(tr("显示名"));
    auto* create = new QPushButton(tr("创建"), form);
    formLayout->addWidget(idEdit);
    formLayout->addWidget(labelEdit);
    formLayout->addWidget(create);
    auto* formAction = new QWidgetAction(createMenu);
    formAction->setDefaultWidget(form);
    createMenu->addAction(formAction);
    newDescriptionButton_->setMenu(createMenu);
    const auto submit = [this, idEdit, labelEdit, createMenu] {
        QString error;
        if (CreateDescription(idEdit->text().trimmed(), labelEdit->text().trimmed(), &error)) {
            idEdit->clear();
            labelEdit->clear();
            createMenu->close();
        } else {
            ReportAuthoring(error);
        }
    };
    connect(create, &QPushButton::clicked, this, submit);
    connect(idEdit, &QLineEdit::returnPressed, this, submit);
    connect(labelEdit, &QLineEdit::returnPressed, this, submit);
    connect(createMenu, &QMenu::aboutToShow, idEdit, [idEdit] { idEdit->setFocus(); });

    addFrameButton_ = new QToolButton(toolbar);
    addFrameButton_->setObjectName(QStringLiteral("gameUiAddFrame"));
    addFrameButton_->setText(tr("添加"));
    addFrameButton_->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(addFrameButton_);
    const std::pair<UiFrameType, QString> types[] = {{UiFrameType::Panel, tr("容器")}, {UiFrameType::Backdrop, tr("背景")},
        {UiFrameType::Text, tr("文字")}, {UiFrameType::Bar, tr("进度条")}, {UiFrameType::Button, tr("按钮")}};
    for (const auto& [type, name] : types) {
        connect(addMenu->addAction(name), &QAction::triggered, this, [this, type] {
            QString error;
            if (AddDescriptionFrame(type, &error).isEmpty()) ReportAuthoring(error);
        });
    }
    addFrameButton_->setMenu(addMenu);

    removeFrameButton_ = new QPushButton(tr("删除"), toolbar);
    removeFrameButton_->setObjectName(QStringLiteral("gameUiRemoveFrame"));
    connect(removeFrameButton_, &QPushButton::clicked, this, [this] {
        QString error;
        if (!RemoveSelectedDescriptionFrame(&error) && !error.isEmpty()) ReportAuthoring(error);
    });
    UpdateAuthoringControls();
    return {newDescriptionButton_, addFrameButton_, removeFrameButton_};
}

void GameUiModule::UpdateAuthoringControls() {
    const FrameNode* node = tree_ && selected_ < tree_->Nodes().size() ? &tree_->Nodes()[selected_] : nullptr;
    const bool inDescription = node && node->uiFrame;
    const bool isFrame = inDescription && !node->uiFrame->frame.empty();
    if (addFrameButton_) addFrameButton_->setEnabled(inDescription);
    if (removeFrameButton_) removeFrameButton_->setEnabled(isFrame);
    if (canvas_) canvas_->SetDraggable(isFrame);
}

// 描述框架可直接改的字段：中文名默认，勾选“显示原始字段”时显示文件里的字段名。
void GameUiModule::AddEditableRows(QTreeWidgetItem* group, const UiFrame& frame) {
    const bool raw = rawFields_ && rawFields_->isChecked();
    const auto editable = [&](QTreeWidgetItem* parent, const QString& label, const QString& key, const QString& value) {
        auto* item = new QTreeWidgetItem(parent, {raw ? key : label, value});
        item->setData(1, kFieldRole, key);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setToolTip(1, value);
        return item;
    };
    const auto number = [](const std::optional<double>& value) { return value ? QString::number(*value) : QString(); };
    const auto binding = [](const UiBinding& bound) {
        QString text = Text(bound.value);
        if (bound.perInstance) text += QStringLiteral("[]");
        else if (bound.index) text += QStringLiteral("[%1]").arg(*bound.index);
        return text;
    };
    editable(group, tr("编号"), QStringLiteral("id"), Text(frame.id));
    editable(group, tr("显示名"), QStringLiteral("label"), Text(frame.label));
    editable(group, tr("宽"), QStringLiteral("width"), number(frame.width));
    editable(group, tr("高"), QStringLiteral("height"), number(frame.height));
    const bool image = frame.type == UiFrameType::Backdrop || frame.type == UiFrameType::Bar || frame.type == UiFrameType::Button;
    const bool text = frame.type == UiFrameType::Text || frame.type == UiFrameType::Button;
    if (image) editable(group, tr("贴图"), QStringLiteral("texture"), Text(frame.texture));
    if (frame.type == UiFrameType::Bar) editable(group, tr("底图"), QStringLiteral("background"), Text(frame.background));
    if (frame.type == UiFrameType::Button) editable(group, tr("按下贴图"), QStringLiteral("pushed"), Text(frame.pushed));
    if (text) {
        editable(group, tr("文字"), QStringLiteral("text"), Text(frame.text));
        editable(group, tr("字号"), QStringLiteral("fontHeight"), QString::number(frame.fontHeight));
    }
    if (frame.type == UiFrameType::Bar) {
        editable(group, tr("进度"), QStringLiteral("value"),
            frame.value.binding ? binding(*frame.value.binding) : QString::number(frame.value.literal));
    }
    editable(group, tr("显示"), QStringLiteral("visible"),
        frame.visible.binding ? binding(*frame.visible.binding) : (frame.visible.literal ? QStringLiteral("true") : QStringLiteral("false")));
    for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
        const UiAnchor& anchor = frame.anchors[i];
        QString target;
        switch (anchor.target) {
        case UiAnchorTarget::Parent: target = tr("父框架"); break;
        case UiAnchorTarget::Screen: target = tr("屏幕"); break;
        case UiAnchorTarget::Frame: target = Text(anchor.targetId); break;
        case UiAnchorTarget::GameArea: target = tr("游戏界面 %1").arg(Text(anchor.targetId)); break;
        }
        auto* item = new QTreeWidgetItem(group, {tr("锚点 %1").arg(Text(std::string(FramePointName(anchor.point)))),
            tr("%1 的 %2").arg(target, Text(std::string(FramePointName(anchor.targetPoint))))});
        item->setExpanded(true);
        editable(item, QStringLiteral("x"), QStringLiteral("anchor:%1:x").arg(i), QString::number(anchor.x));
        editable(item, QStringLiteral("y"), QStringLiteral("anchor:%1:y").arg(i), QString::number(anchor.y));
    }
}

void GameUiModule::OnPropertyEdited(QTreeWidgetItem* item, int column) {
    if (column != 1 || !item || !tree_ || selected_ >= tree_->Nodes().size()) return;
    const QString key = item->data(1, kFieldRole).toString();
    const auto& link = tree_->Nodes()[selected_].uiFrame;
    if (key.isEmpty() || !link) return;
    const QString id = Text(link->description);
    const UiDescription* description = Description(id);
    const UiFrame* frame = description ? FindUiFrame(*description, link->frame) : nullptr;
    if (!frame) return;
    const QString text = item->text(1).trimmed();
    QString error;
    QJsonObject fields;
    if (key == QLatin1String("width") || key == QLatin1String("height") || key == QLatin1String("fontHeight")) {
        bool ok = true;
        const double number = text.toDouble(&ok);
        if (text.isEmpty()) fields.insert(key, QJsonValue::Null);
        else if (ok) fields.insert(key, number);
        else error = tr("%1 应为数值").arg(item->text(0));
    } else if (key == QLatin1String("value") || key == QLatin1String("visible")) {
        bool ok = false;
        const double number = text.toDouble(&ok);
        if (text.isEmpty()) fields.insert(key, QJsonValue::Null);
        else if (key == QLatin1String("visible") && (text == QLatin1String("true") || text == tr("显示"))) fields.insert(key, true);
        else if (key == QLatin1String("visible") && (text == QLatin1String("false") || text == tr("隐藏"))) fields.insert(key, false);
        else if (ok && key == QLatin1String("value")) fields.insert(key, number);
        else fields.insert(key, text);  // 取值：编号、编号[] 或 编号[下标]
    } else if (key.startsWith(QLatin1String("anchor:"))) {
        const QStringList parts = key.split(QLatin1Char(':'));
        bool ok = false;
        const double number = text.toDouble(&ok);
        QJsonArray anchors = AnchorsOf(*frame);
        const qsizetype index = parts.value(1).toInt();
        if (!ok || index < 0 || index >= anchors.size()) {
            error = tr("偏移应为数值");
        } else {
            QJsonObject anchor = anchors[index].toObject();
            anchor.insert(parts.value(2), number);
            anchors[index] = anchor;
            fields.insert(QStringLiteral("anchors"), anchors);
        }
    } else {
        fields.insert(key, text.isEmpty() && key != QLatin1String("id") ? QJsonValue(QJsonValue::Null) : QJsonValue(text));
    }
    if (!error.isEmpty()) {
        ReportAuthoring(error);
        QTimer::singleShot(0, this, &GameUiModule::UpdateProperties);  // 恢复成描述里的值
        return;
    }
    const QString frameId = Text(frame->id);
    const QJsonObject edit{{"op", "set_frame"}, {"frame", frameId}, {"fields", fields}};
    std::function<void()> after;
    if (key == QLatin1String("id")) {
        after = [this, text] {
            if (!SelectFrame(text)) (void)SelectFrame(text + QStringLiteral("#0"));
        };
    }
    CommitDescriptionEdits(id, {edit}, tr("界面 %1：改 %2 的 %3").arg(id, frameId, item->text(0)), std::move(after));
}

void GameUiModule::CommitDescriptionEdits(const QString& id, const QJsonArray& edits, const QString& undoLabel,
    std::function<void()> after) {
    QTimer::singleShot(0, this, [this, id, edits, undoLabel, after = std::move(after)] {
        QString error;
        if (EditDescription(id, CompactEdits(edits), undoLabel, &error)) {
            if (after) after();
            return;
        }
        ReportAuthoring(error);
        descriptionPanelFor_.clear();
        UpdateProperties();  // 属性树与表格恢复成描述里的值
    });
}

} // namespace War3Map::Modules
