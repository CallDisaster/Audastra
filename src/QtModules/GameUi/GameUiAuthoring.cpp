#include "GameUiModule.h"

#include "EditorProject.h"
#include "GameUiAuthoringJson.h"
#include "GameUiCanvas.h"
#include "Modules/ModuleHost.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "War3MapCore/GameUi/UiPresets.h"
#include "War3MapCore/GameUi/UiTriggers.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
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

bool GameUiModule::CreateDescription(const QString& id, const QString& label, QString* error, const QString& preset) {
    auto created = preset.isEmpty() ? NewUiDescription(id.toStdString(), label.toStdString())
                                    : NewUiDescriptionFromPreset(preset.toStdString(), id.toStdString(), label.toStdString());
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
    if (*edited.value == *base) return true;  // 改完与原来相同（例如选回同一项）：不产生修改
    return StageDescription(std::move(*edited.value), false, undoLabel.isEmpty() ? tr("修改界面 %1").arg(id) : undoLabel, error);
}

QString GameUiModule::AddDescriptionFrame(UiFrameType type, QString* error) {
    return AddFrameObject(DefaultFrame(type, {}), IdPrefix(type), error);
}

QString GameUiModule::AddDescriptionTemplate(const QString& templateName, QString* error) {
    if (templateName.trimmed().isEmpty()) {
        if (error) *error = tr("模板名为空");
        return {};
    }
    QJsonObject frame{{"type", "template"}, {"template", templateName.trimmed()},
        {"anchors", QJsonArray{QJsonObject{{"point", "CENTER"}}}}};
    return AddFrameObject(std::move(frame), "template", error);
}

// 编号按类型取下一个空号，放在所选框架下（选中描述根节点时为顶层）。
QString GameUiModule::AddFrameObject(QJsonObject frame, const char* idPrefix, QString* error) {
    if (!tree_ || selected_ >= tree_->Nodes().size() || !tree_->Nodes()[selected_].uiFrame) {
        if (error) *error = tr("先选中一个界面描述里的框架");
        return {};
    }
    const UiFrameLink link = *tree_->Nodes()[selected_].uiFrame;
    const QString id = Text(link.description);
    const UiDescription* description = Description(id);
    if (!description) return {};
    const QString frameId = Text(NextUiFrameId(*description, idPrefix));
    frame.insert(QStringLiteral("id"), frameId);
    QJsonObject edit{{"op", "add_frame"}, {"frame", frame}};
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
    auto* presetBox = new QComboBox(form);
    presetBox->setObjectName(QStringLiteral("gameUiNewDescriptionPreset"));
    presetBox->addItem(tr("空白"), QString());
    for (const auto& preset : UiPresets()) presetBox->addItem(Text(std::string(preset.label)), Text(std::string(preset.id)));
    auto* create = new QPushButton(tr("创建"), form);
    formLayout->addWidget(presetBox);
    formLayout->addWidget(idEdit);
    formLayout->addWidget(labelEdit);
    formLayout->addWidget(create);
    auto* formAction = new QWidgetAction(createMenu);
    formAction->setDefaultWidget(form);
    createMenu->addAction(formAction);
    newDescriptionButton_->setMenu(createMenu);
    const auto submit = [this, idEdit, labelEdit, presetBox, createMenu] {
        QString error;
        if (CreateDescription(idEdit->text().trimmed(), labelEdit->text().trimmed(), &error, presetBox->currentData().toString())) {
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
    // 模板：游戏与地图界面文件里的框架模板，按名字搜索后回车或双击添加。
    auto* templateMenu = addMenu->addMenu(tr("模板"));
    auto* picker = new QWidget(templateMenu);
    auto* pickerLayout = new QVBoxLayout(picker);
    pickerLayout->setContentsMargins(6, 4, 6, 4);
    auto* templateSearch = new QLineEdit(picker);
    templateSearch->setObjectName(QStringLiteral("gameUiTemplateSearch"));
    templateSearch->setPlaceholderText(tr("搜索模板"));
    templateSearch->setClearButtonEnabled(true);
    auto* templateList = new QListWidget(picker);
    templateList->setObjectName(QStringLiteral("gameUiTemplateList"));
    templateList->setMinimumSize(280, 320);
    pickerLayout->addWidget(templateSearch);
    pickerLayout->addWidget(templateList);
    auto* pickerAction = new QWidgetAction(templateMenu);
    pickerAction->setDefaultWidget(picker);
    templateMenu->addAction(pickerAction);
    const auto filter = [templateList](const QString& text) {
        QListWidgetItem* first = nullptr;
        for (int i = 0; i < templateList->count(); ++i) {
            QListWidgetItem* item = templateList->item(i);
            item->setHidden(!item->text().contains(text.trimmed(), Qt::CaseInsensitive));
            if (!first && !item->isHidden()) first = item;
        }
        templateList->setCurrentItem(first);
    };
    connect(templateMenu, &QMenu::aboutToShow, this, [this, templateList, templateSearch, filter] {
        templateList->clear();
        templateList->addItems(FrameTemplates());
        filter(templateSearch->text());
        templateSearch->setFocus();
    });
    connect(templateSearch, &QLineEdit::textChanged, templateList, filter);
    const auto addTemplate = [this, addMenu](const QString& name) {
        addMenu->close();
        QString error;
        if (AddDescriptionTemplate(name, &error).isEmpty()) ReportAuthoring(error);
    };
    connect(templateList, &QListWidget::itemActivated, this, [addTemplate](QListWidgetItem* item) { addTemplate(item->text()); });
    connect(templateSearch, &QLineEdit::returnPressed, this, [templateList, addTemplate] {
        if (QListWidgetItem* item = templateList->currentItem(); item && !item->isHidden()) addTemplate(item->text());
    });
    addFrameButton_->setMenu(addMenu);

    removeFrameButton_ = new QPushButton(tr("删除"), toolbar);
    removeFrameButton_->setObjectName(QStringLiteral("gameUiRemoveFrame"));
    connect(removeFrameButton_, &QPushButton::clicked, this, [this] {
        QString error;
        if (!RemoveSelectedDescriptionFrame(&error) && !error.isEmpty()) ReportAuthoring(error);
    });
    generateTriggersButton_ = new QPushButton(tr("生成触发器"), toolbar);
    generateTriggersButton_->setObjectName(QStringLiteral("gameUiGenerateTriggers"));
    connect(generateTriggersButton_, &QPushButton::clicked, this, [this] {
        const FrameNode* node = tree_ && selected_ < tree_->Nodes().size() ? &tree_->Nodes()[selected_] : nullptr;
        if (!node || !node->uiFrame) return;
        QString error;
        const auto generated = GenerateDescriptionTriggers(Text(node->uiFrame->description), false, &error);
        if (!generated) {
            ReportAuthoring(error);
            return;
        }
        authoringDiagnostics_ = generated->warnings;
        UpdateProblems();
        if (host_) {
            host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(std::string(kUiTriggerCategory))), 8000);
        }
        OfferMapRuntime();
    });
    castSystemButton_ = new QPushButton(tr("读条系统"), toolbar);
    castSystemButton_->setObjectName(QStringLiteral("gameUiCastSystem"));
    connect(castSystemButton_, &QPushButton::clicked, this, &GameUiModule::ShowCastSystemDialog);
    partySystemButton_ = new QPushButton(tr("团队框架"), toolbar);
    partySystemButton_->setObjectName(QStringLiteral("gameUiPartySystem"));
    connect(partySystemButton_, &QPushButton::clicked, this, &GameUiModule::ShowPartySystemDialog);
    targetSystemButton_ = new QPushButton(tr("目标框架"), toolbar);
    targetSystemButton_->setObjectName(QStringLiteral("gameUiTargetSystem"));
    connect(targetSystemButton_, &QPushButton::clicked, this, &GameUiModule::ShowTargetSystemDialog);
    talentSystemButton_ = new QPushButton(tr("天赋树"), toolbar);
    talentSystemButton_->setObjectName(QStringLiteral("gameUiTalentSystem"));
    connect(talentSystemButton_, &QPushButton::clicked, this, &GameUiModule::ShowTalentSystemDialog);
    UpdateAuthoringControls();
    return {newDescriptionButton_, addFrameButton_, removeFrameButton_, generateTriggersButton_, castSystemButton_, partySystemButton_,
        targetSystemButton_, talentSystemButton_};
}

void GameUiModule::UpdateAuthoringControls() {
    const FrameNode* node = tree_ && selected_ < tree_->Nodes().size() ? &tree_->Nodes()[selected_] : nullptr;
    const bool inDescription = node && node->uiFrame;
    const bool isFrame = inDescription && !node->uiFrame->frame.empty();
    if (addFrameButton_) addFrameButton_->setEnabled(inDescription);
    if (removeFrameButton_) removeFrameButton_->setEnabled(isFrame);
    if (generateTriggersButton_) generateTriggersButton_->setEnabled(inDescription);
    if (canvas_) canvas_->SetDraggable(isFrame);
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
