#include "GameUiModule.h"

#include "EditorProject.h"
#include "GameUiAuthoringJson.h"
#include "GameUiCanvas.h"
#include "GameUiKindPicker.h"
#include "Modules/ModuleHost.h"
#include "TriggerCommands.h"
#include "War3MapCore/ApplicationPaths.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "War3MapCore/GameUi/UiFrameKinds.h"
#include "War3MapCore/GameUi/UiStyles.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/GameUi/UiOrigins.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
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

// 自动编号的界面显示名：ui3 → 界面3；其他编号原样。
QString NewDescriptionLabel(const QString& id) {
    if (id.startsWith(QLatin1String("ui"))) {
        bool number = false;
        (void)id.mid(2).toUInt(&number);
        if (number) return QObject::tr("界面%1").arg(id.mid(2));
    }
    return id;
}

// 输入框标红并在旁边写出原因；problem 为空时恢复。
void MarkInvalid(QLineEdit* edit, QLabel* label, const QString& problem) {
    if (edit->property("invalid").toBool() != !problem.isEmpty()) {
        edit->setProperty("invalid", !problem.isEmpty());
        edit->style()->unpolish(edit);
        edit->style()->polish(edit);
    }
    label->setText(problem);
    label->setVisible(!problem.isEmpty());
}

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
    for (const auto& other : files) {
        if (other.description.id == description.id) continue;
        for (const auto& change : description.origins) {
            if (std::any_of(other.description.origins.begin(), other.description.origins.end(), [&](const auto& previous) { return previous.name == change.name; }))
                return fail(tr("原版节点 %1 已由界面 %2 调整").arg(Text(change.name), Text(other.description.id)));
        }
    }
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

QString GameUiModule::SuggestDescriptionId() const {
    std::vector<std::string> used;
    if (definitions_) {
        for (const auto& file : definitions_->descriptions) used.push_back(file.description.id);
    }
    return Text(NextUiDescriptionId(used));
}

QString GameUiModule::CheckNewDescriptionId(const QString& id) const {
    if (!definitions_) return tr("界面定义仍在读取");
    if (id.isEmpty()) return tr("填写编号");
    if (!IsValidUiId(id.toStdString())) return tr("编号要以英文字母或下划线开头，只能用英文字母、数字和下划线，最长 64 个字符");
    for (const auto& file : definitions_->descriptions) {
        if (Text(file.description.id).compare(id, Qt::CaseInsensitive) == 0) return tr("编号 %1 已被使用").arg(Text(file.description.id));
    }
    return {};
}

bool GameUiModule::CreateDescription(const QString& id, const QString& label, QString* error, bool starter) {
    auto created = NewUiDescription(id.toStdString(), label.toStdString());
    if (!created.value) {
        if (error) *error = FirstMessage(created.diagnostics);
        return false;
    }
    QString select = id;
    if (starter) {
        UiFrame panel = StarterUiFrame(NextUiFrameId(*created.value, "panel"));
        select = Text(panel.id);
        created.value->frames.push_back(std::move(panel));
    }
    if (!StageDescription(std::move(*created.value), true, tr("新建界面 %1").arg(id), error)) return false;
    SetCurrentDescription(id);
    SelectFrameWhenReady(select);
    return true;
}

bool GameUiModule::ImportDescriptionFile(const QString& path, bool withTriggers, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    static constexpr qint64 kFileBytes = 8 * 1024 * 1024;
    const auto read = [this](const QString& file, std::string* text) -> QString {
        QFile input(file);
        if (!input.open(QIODevice::ReadOnly)) return tr("读不了 %1：%2").arg(QDir::toNativeSeparators(file), input.errorString());
        if (input.size() > kFileBytes) return tr("%1 超过 8 MB").arg(QDir::toNativeSeparators(file));
        const QByteArray bytes = input.readAll();
        text->assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        if (text->starts_with("\xEF\xBB\xBF")) text->erase(0, 3);
        return {};
    };
    std::string json;
    if (const QString problem = read(path, &json); !problem.isEmpty()) return fail(problem);
    auto parsed = ParseUiDescription(json, path.toStdString());
    if (!parsed.value) return fail(FirstMessage(parsed.diagnostics));
    const QString id = Text(parsed.value->id);
    const QString undoLabel = tr("导入界面 %1").arg(id);
    if (!withTriggers) {
        if (!StageDescription(std::move(*parsed.value), true, undoLabel, error)) return false;
        SelectFrameWhenReady(id);
        return true;
    }
    std::string triggers;
    if (const QString problem = read(QFileInfo(path).dir().filePath(QStringLiteral("triggers.txt")), &triggers); !problem.isEmpty()) {
        return fail(problem);
    }
    // 与命令行导入一致：界面自己的变量和触发器先生成，示例触发器随后写入，同一步撤销。
    const std::string category = parsed.value->label.empty() ? parsed.value->id : parsed.value->label;
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{category, std::move(triggers), "import", std::nullopt});
    QString staged;
    QString generationError;
    const auto generated = PublishDescriptionTriggers(*parsed.value, false, std::move(operations), undoLabel, [&]() -> Status {
        if (StageDescription(*parsed.value, true, undoLabel, &staged)) return Status::Ok();
        return Status::Failure("GameUi", "AUI_IMPORT_DESCRIPTION", staged.toStdString());
    }, &generationError);
    if (!generated) return fail(staged.isEmpty() ? generationError : staged);
    SelectFrameWhenReady(id);
    return true;
}

void GameUiModule::ImportExample(const QString& path) {
    bool withTriggers = false;
    if (QFileInfo::exists(QFileInfo(path).dir().filePath(QStringLiteral("triggers.txt")))) {
        const auto answer = QMessageBox::question(page_, tr("导入界面"), tr("同一目录里有 triggers.txt，一并写进触发器？"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer == QMessageBox::Cancel) return;
        withTriggers = answer == QMessageBox::Yes;
    }
    QString error;
    if (!ImportDescriptionFile(path, withTriggers, &error)) ReportAuthoring(error);
}

bool GameUiModule::EditDescription(const QString& id, const QString& edits, const QString& undoLabel, QString* error) {
    const UiDescription* base = Description(id);
    if (!base) {
        if (error) *error = tr("地图里没有界面 %1").arg(id);
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

QString GameUiModule::AddDescriptionFrame(const QString& kind, QString* error, const QString& templateName) {
    if (!tree_ || selected_ >= tree_->Nodes().size() || !tree_->Nodes()[selected_].uiFrame) {
        // 没选中自己的界面时放进当前界面的顶层；还没有界面时新建一份。
        return CreateDescriptionFrame(kind, CurrentDescription(), {}, std::nullopt, templateName, error);
    }
    const UiFrameLink link = *tree_->Nodes()[selected_].uiFrame;
    return CreateDescriptionFrame(kind, Text(link.description), Text(link.frame), std::nullopt, templateName, error);
}

QString GameUiModule::AddDescriptionTemplate(const QString& templateName, QString* error) {
    return AddDescriptionFrame(QStringLiteral("template"), error, templateName);
}

// 编号按类型取下一个空号；给了位置时把中心锚点的偏移换算到父框架中心。
QString GameUiModule::CreateDescriptionFrame(const QString& kindId, const QString& id, const QString& parent,
    std::optional<QPointF> at, const QString& templateName, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QString();
    };
    // 原版样式写作 style:<编号>，其余为控件类型编号。
    const UiStyle* style = kindId.startsWith(QLatin1String("style:")) ? FindUiStyle(kindId.mid(6).toStdString()) : nullptr;
    const UiFrameKind* kind = style ? nullptr : FindUiFrameKind(kindId.toStdString());
    if (!kind && !style) return fail(tr("没有控件类型 %1").arg(kindId));
    if (kind && kind->type == UiFrameType::Template && templateName.trimmed().isEmpty()) return fail(tr("模板名为空"));
    if (!definitions_) return fail(tr("界面定义仍在读取"));
    // 没有指定界面：新建一份，与控件同一步撤销。
    std::optional<UiDescription> fresh;
    if (id.isEmpty()) {
        const QString newId = SuggestDescriptionId();
        auto created = NewUiDescription(newId.toStdString(), NewDescriptionLabel(newId).toStdString());
        if (!created.value) return fail(FirstMessage(created.diagnostics));
        fresh = std::move(*created.value);
    }
    const UiDescription* description = fresh ? &*fresh : Description(id);
    if (!description) return fail(tr("地图里没有界面 %1").arg(id));
    const QString descriptionId = Text(description->id);
    if (!parent.isEmpty() && !FindUiFrame(*description, parent.toStdString()))
        return fail(tr("界面 %1 里没有控件 %2").arg(descriptionId, parent));
    UiFrame frame = style ? StyleUiFrame(*style, NextUiFrameId(*description, style->prefix))
                          : DefaultUiFrame(*kind, NextUiFrameId(*description, UiFrameIdPrefix(*kind)), templateName.trimmed().toStdString());
    // 父控件中心：顶层控件的父节点是铺满画面的界面根。
    std::optional<QPointF> parentCenter;
    if (fresh) {
        const FrameTreeOptions options = tree_ ? tree_->Options() : FrameTreeOptions{};
        parentCenter = QPointF(options.screenWidth / 2.0, options.screenHeight / 2.0);
    } else if (tree_) {
        for (const auto& node : tree_->Nodes()) {
            if (node.uiFrame && Text(node.uiFrame->description) == descriptionId && Text(node.uiFrame->frame) == parent) {
                if (node.layout == LayoutState::Placed)
                    parentCenter = QPointF((node.rect.left + node.rect.right) / 2.0, (node.rect.top + node.rect.bottom) / 2.0);
                break;
            }
        }
    }
    if (at && parentCenter && !frame.anchors.empty()) {
        frame.anchors.front().x = Round4(at->x() - parentCenter->x());
        frame.anchors.front().y = Round4(at->y() - parentCenter->y());
    }
    const QString frameId = Text(frame.id);
    if (fresh) {
        fresh->frames.push_back(std::move(frame));
        if (const auto problems = ValidateUiDescription(*fresh); !problems.empty()) return fail(FirstMessage(problems));
        if (!StageDescription(std::move(*fresh), true, tr("新建界面 %1").arg(descriptionId), error)) return {};
        SetCurrentDescription(descriptionId);
        SelectFrameWhenReady(frameId);
        return frameId;
    }
    QJsonObject edit{{"op", "add_frame"}, {"frame", FrameObject(frame)}};
    if (!parent.isEmpty()) edit.insert("parent", parent);
    if (!EditDescription(id, CompactEdits({edit}), tr("界面 %1：添加 %2").arg(id, frameId), error)) return {};
    SelectFrameWhenReady(frameId);
    return frameId;
}

GameUiModule::FramePreview GameUiModule::PreviewFrame(const UiFrame& frame) {
    UiDescription description;
    description.id = "AudastraKindPreview";
    description.frames.push_back(frame);
    return PreviewDescription(description);
}

GameUiModule::FramePreview GameUiModule::PreviewDescription(const UiDescription& description) {
    FramePreview preview;
    if (!definitions_ || !definitions_->library) return preview;
    const UiSkin skin = MakeSkin();
    FrameTreeOptions options;
    options.race = race_.toStdString();
    options.expansion = expansion_;
    preview.tree = std::make_unique<FrameTree>(options);
    std::vector<Diagnostic> diagnostics;
    (void)AddUiDescription(*preview.tree, *definitions_->library, &skin, description, {}, diagnostics);
    (void)preview.tree->Layout();
    preview.list = BuildDrawList(*preview.tree, {});
    // 放大到顶层控件合起来的范围。
    for (const auto& node : preview.tree->Nodes()) {
        if (!node.uiFrame || node.uiFrame->frame.empty() || node.layout != LayoutState::Placed) continue;
        const bool top = std::any_of(description.frames.begin(), description.frames.end(),
            [&](const UiFrame& frame) { return frame.id == node.uiFrame->frame; });
        if (!top) continue;
        if (!preview.focus) {
            preview.focus = node.rect;
            continue;
        }
        preview.focus->left = std::min(preview.focus->left, node.rect.left);
        preview.focus->bottom = std::min(preview.focus->bottom, node.rect.bottom);
        preview.focus->right = std::max(preview.focus->right, node.rect.right);
        preview.focus->top = std::max(preview.focus->top, node.rect.top);
    }
    previewDrawList_ = preview.list;
    StartAssetsJob();
    return preview;
}

QPointer<QWidget> GameUiModule::OpenKindPicker(std::uint32_t node, std::optional<QPointF> at, const QPoint& global) {
    if (!definitions_) return nullptr;
    // 放进：所点节点所在的控件，其次所选节点的，再其次当前界面的顶层；还没有界面时新建一份。
    const auto linkOf = [this](std::uint32_t id) -> std::optional<UiFrameLink> {
        if (!tree_ || id >= tree_->Nodes().size() || !tree_->Nodes()[id].uiFrame) return std::nullopt;
        return tree_->Nodes()[id].uiFrame;
    };
    std::optional<UiFrameLink> preferred = linkOf(node) ? linkOf(node) : linkOf(selected_);
    if (!preferred && !CurrentDescription().isEmpty()) preferred = UiFrameLink{CurrentDescription().toStdString(), {}, std::nullopt};
    QList<GameUiKindPicker::Target> targets;
    int current = 0;
    const auto label = [](const std::string& name, const std::string& id) {
        return name.empty() ? Text(id) : QStringLiteral("%1  %2").arg(Text(name), Text(id));
    };
    const std::function<void(const UiDescription&, const std::vector<UiFrame>&, int)> addFrames =
        [&](const UiDescription& description, const std::vector<UiFrame>& frames, int depth) {
            for (const auto& frame : frames) {
                if (preferred && preferred->description == description.id && preferred->frame == frame.id) {
                    current = static_cast<int>(targets.size());
                }
                targets.push_back({QString(depth * 2, QChar::Space) + label(frame.label, frame.id), Text(description.id), Text(frame.id)});
                addFrames(description, frame.children, depth + 1);
            }
        };
    for (const auto& file : definitions_->descriptions) {
        const UiDescription& description = file.description;
        if (preferred && preferred->description == description.id && preferred->frame.empty()) current = static_cast<int>(targets.size());
        targets.push_back({label(description.label, description.id), Text(description.id), {}});
        addFrames(description, description.frames, 1);
    }
    // 最后一项：新建一份界面放进去（还没有界面时就是它）。
    targets.push_back({tr("新界面 %1").arg(SuggestDescriptionId()), {}, {}});
    if (!preferred) current = static_cast<int>(targets.size()) - 1;
    auto* picker = new GameUiKindPicker(*this, std::move(targets), current, page_);
    connect(picker, &GameUiKindPicker::Chosen, this,
        [this, at](const QString& kind, const QString& templateName, const QString& description, const QString& frame) {
            if (kind.startsWith(QLatin1String("example:"))) {
                ImportExample(kind.mid(8));
                return;
            }
            QString error;
            if (CreateDescriptionFrame(kind, description, frame, at, templateName, &error).isEmpty()) ReportAuthoring(error);
        });
    picker->adjustSize();
    QRect geometry(global, picker->sizeHint());
    if (const QScreen* screen = QGuiApplication::screenAt(global)) {
        const QRect available = screen->availableGeometry();
        geometry.moveLeft(std::clamp(geometry.left(), available.left(), std::max(available.left(), available.right() - geometry.width())));
        geometry.moveTop(std::clamp(geometry.top(), available.top(), std::max(available.top(), available.bottom() - geometry.height())));
    }
    picker->move(geometry.topLeft());
    picker->show();
    return picker;
}

void GameUiModule::ShowContextMenu(std::uint32_t node, std::optional<QPointF> at, const QPoint& global, bool fromTree) {
    if (!tree_ || !page_) return;
    // 画布上只认能点中的节点（不可选的原版界面上右键等于在空白处右键）；节点树里任何节点都可以。
    if (!fromTree && !NodePickable(node)) node = kNoNode;
    const FrameNode* item = node < tree_->Nodes().size() && node != kScreenNode ? &tree_->Nodes()[node] : nullptr;
    SelectNode(item ? node : kNoNode);
    const bool isFrame = item && item->uiFrame && !item->uiFrame->frame.empty();
    const bool isRoot = item && item->uiFrame && item->uiFrame->frame.empty();
    const bool hasClip = HasClipboardFrame();
    const auto paste = [this] { QString error; if (!PasteDescriptionFrame(&error)) ReportAuthoring(error); };
    QMenu menu(page_);
    menu.setToolTipsVisible(true);
    if (isFrame) {
        // 自己的控件。
        menu.addAction(tr("设置…"), this, [this, node, global] { (void)OpenFrameSettings(node, global); });
        menu.addAction(tr("添加子控件…"), this, [this, node, at, global] { (void)OpenKindPicker(node, at, global); });
        menu.addSeparator();
        menu.addAction(tr("复制"), this, [this] { QString error; if (!CopySelectedDescriptionFrame(&error)) ReportAuthoring(error); });
        menu.addAction(tr("粘贴"), this, paste)->setEnabled(hasClip);
        menu.addAction(tr("删除"), this, [this] {
            QString error;
            if (!RemoveSelectedDescriptionFrame(&error) && !error.isEmpty()) ReportAuthoring(error);
        });
        menu.addSeparator();
        menu.addAction(tr("置于顶层"), this, [this, node] { QString error; if (!RaiseDescriptionFrame(node, true, &error) && !error.isEmpty()) ReportAuthoring(error); });
        menu.addAction(tr("置于底层"), this, [this, node] { QString error; if (!RaiseDescriptionFrame(node, false, &error) && !error.isEmpty()) ReportAuthoring(error); });
        menu.addSeparator();
        AddNewDescriptionForm(menu.addMenu(tr("新建界面")));
    } else if (isRoot) {
        // 自己的界面（根）。
        menu.addAction(tr("添加控件…"), this, [this, node, at, global] { (void)OpenKindPicker(node, at, global); });
        menu.addAction(tr("粘贴"), this, paste)->setEnabled(hasClip);
        menu.addSeparator();
        AddNewDescriptionForm(menu.addMenu(tr("新建界面")));
    } else {
        // 空白处或原版界面：先给新建，原版节点的调整排在后面。
        AddNewDescriptionForm(menu.addMenu(tr("新建界面")));
        menu.addAction(tr("添加控件…"), this, [this, at, global] { (void)OpenKindPicker(kNoNode, at, global); })->setEnabled(definitions_.has_value());
        menu.addAction(tr("粘贴"), this, paste)->setEnabled(hasClip && !CurrentDescription().isEmpty());
    }
    const QString originName = OriginNodeName(node);
    if (!originName.isEmpty()) {
        menu.addSeparator();
        auto* hide = menu.addAction(tr("隐藏"), this, [this, originName] { QString error; if (!EditOriginNode(originName, {{"visible", false}}, &error)) ReportAuthoring(error); });
        const auto* part = FindUiOriginPart(originName.toStdString());
        hide->setEnabled(part && part->visibility);
        if (part && !part->visibility) hide->setToolTip(Text(part->reason));
        menu.addAction(tr("恢复原样"), this, [this, originName] { QString error; if (!RestoreOriginNode(originName, &error)) ReportAuthoring(error); });
    }
    if (item && !item->uiFrame && !item->scriptFrame) {
        if (originName.isEmpty()) menu.addSeparator();
        auto* copy = menu.addAction(tr("复制为自己的界面"), this, [this, node] { QString error; if (!CopyOriginTemplate(node, &error)) ReportAuthoring(error); });
        const bool canCopy = definitions_ && definitions_->library && item->codeLabel.empty() && definitions_->library->FindTemplate(item->name).has_value();
        copy->setEnabled(canCopy);
        if (!canCopy) copy->setToolTip(tr("此节点由游戏代码创建，或没有可复制的模板"));
    }
    if (fromTree && item) {
        menu.addSeparator();
        menu.addAction(tr("只显示这一项"), this, [this, node] { ShowOnly(node); });
        menu.addAction(tr("全部显示"), this, [this] {
            hiddenKeys_.clear();
            StoreMapView();
            RebuildDrawList();
            PopulateTree();
            StartAssetsJob();
        });
        if (item->parent == kScreenNode && !item->uiFrame) {
            const QString name = Text(item->name);
            menu.addAction(tr("移除"), this, [this, name] { RemoveRoot(name); });
        }
    }
    menu.exec(global);
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
    SelectFrameWhenReady(id);
    return true;
}

bool GameUiModule::RaiseDescriptionFrame(std::uint32_t node, bool top, QString* error) {
    if (!tree_ || node >= tree_->Nodes().size() || !tree_->Nodes()[node].uiFrame || tree_->Nodes()[node].uiFrame->frame.empty()) {
        if (error) *error = tr("只能调整自己界面里的控件");
        return false;
    }
    const UiFrameLink link = *tree_->Nodes()[node].uiFrame;
    const QString id = Text(link.description);
    const UiDescription* description = Description(id);
    const auto position = description ? UiFrameSiblingIndex(*description, link.frame) : std::nullopt;
    if (!position) return false;
    // 同级里后面的画在上面。
    const std::size_t index = top ? position->second - 1 : 0;
    if (index == position->first) {
        if (error) error->clear();
        return false;
    }
    const QString parent = Text(UiFrameParentId(*description, link.frame));
    QJsonObject edit{{"op", "move_frame"}, {"frame", Text(link.frame)}, {"index", static_cast<qint64>(index)}};
    if (!parent.isEmpty()) edit.insert("parent", parent);
    return EditDescription(id, CompactEdits({edit}),
        (top ? tr("界面 %1：%2 置于顶层") : tr("界面 %1：%2 置于底层")).arg(id, Text(link.frame)), error);
}

bool GameUiModule::MoveDescriptionFrame(std::uint32_t node, double dx, double dy, QString* error) {
    if (!tree_ || node >= tree_->Nodes().size() || !tree_->Nodes()[node].uiFrame || tree_->Nodes()[node].uiFrame->frame.empty()) {
        if (error) *error = tr("只能移动自己界面里的控件");
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
            {"x", RoundUiUnits(target.rect.left + dx)}, {"y", RoundUiUnits(target.rect.bottom + dy)}});
    } else {
        for (qsizetype i = 0; i < anchors.size(); ++i) {
            QJsonObject anchor = anchors[i].toObject();
            anchor.insert("x", RoundUiUnits(anchor.value("x").toDouble() + dx));
            anchor.insert("y", RoundUiUnits(anchor.value("y").toDouble() + dy));
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
        // 问题列表收起时只有计数在变；操作没生效要在状态栏看得见。
        if (host_) host_->ShowStatusMessage(error.section(QLatin1Char('\n'), 0, 0), 10000);
    }
    UpdateProblems();
}

// 新建界面：只要填名称，编号自动给出（可改）。编号不合法时输入框标红、原因写在下面，“创建”不可点。
// 新界面自带一个面板并选中它。
void GameUiModule::AddNewDescriptionForm(QMenu* menu) {
    auto* form = new QWidget(menu);
    auto* formLayout = new QGridLayout(form);
    formLayout->setContentsMargins(6, 4, 6, 4);
    formLayout->setHorizontalSpacing(6);
    formLayout->setVerticalSpacing(4);
    auto* labelEdit = new QLineEdit(form);
    labelEdit->setObjectName(QStringLiteral("gameUiNewDescriptionLabel"));
    labelEdit->setMinimumWidth(150);
    auto* idEdit = new QLineEdit(form);
    idEdit->setObjectName(QStringLiteral("gameUiNewDescriptionId"));
    idEdit->setMinimumWidth(90);
    auto* create = new QPushButton(tr("创建"), form);
    create->setObjectName(QStringLiteral("gameUiNewDescriptionCreate"));
    auto* problem = new QLabel(form);
    problem->setObjectName(QStringLiteral("formProblem"));
    problem->setWordWrap(true);
    problem->setVisible(false);
    formLayout->addWidget(new QLabel(tr("名称"), form), 0, 0);
    formLayout->addWidget(labelEdit, 0, 1);
    formLayout->addWidget(new QLabel(tr("编号"), form), 0, 2);
    formLayout->addWidget(idEdit, 0, 3);
    formLayout->addWidget(create, 0, 4);
    formLayout->addWidget(problem, 1, 1, 1, 4);
    auto* formAction = new QWidgetAction(menu);
    formAction->setDefaultWidget(form);
    menu->addAction(formAction);
    const auto validate = [this, idEdit, problem, create] {
        const QString reason = CheckNewDescriptionId(idEdit->text().trimmed());
        MarkInvalid(idEdit, problem, reason);
        create->setEnabled(reason.isEmpty());
    };
    connect(idEdit, &QLineEdit::textEdited, form, [idEdit, validate] {
        idEdit->setProperty("autoId", false);  // 作者改过编号：之后不再自动换
        validate();
    });
    const auto submit = [this, idEdit, labelEdit, create, menu] {
        if (!create->isEnabled()) return;
        const QString id = idEdit->text().trimmed();
        const QString label = labelEdit->text().trimmed();
        QString error;
        if (!CreateDescription(id, label.isEmpty() ? labelEdit->placeholderText() : label, &error, true)) {
            ReportAuthoring(error);
            return;
        }
        labelEdit->clear();
        idEdit->setProperty("autoId", true);
        // 放在右键菜单的子菜单里时连同上级菜单一起关掉。
        for (QWidget* open = menu; auto* current = qobject_cast<QMenu*>(open); open = current->parentWidget()) current->close();
    };
    connect(create, &QPushButton::clicked, this, submit);
    connect(idEdit, &QLineEdit::returnPressed, this, submit);
    connect(labelEdit, &QLineEdit::returnPressed, this, submit);
    idEdit->setProperty("autoId", true);
    connect(menu, &QMenu::aboutToShow, form, [this, idEdit, labelEdit, validate] {
        const QString suggested = SuggestDescriptionId();
        if (idEdit->property("autoId").toBool()) idEdit->setText(suggested);
        labelEdit->setPlaceholderText(NewDescriptionLabel(suggested));
        validate();
        labelEdit->setFocus();
    });
}

QList<QWidget*> GameUiModule::BuildAuthoringControls(QWidget* toolbar) {
    // 与同排的删除、生成触发器同为按钮外观；带菜单的按钮给下拉箭头单独留位。
    newDescriptionButton_ = new QPushButton(tr("新建界面"), toolbar);
    newDescriptionButton_->setObjectName(QStringLiteral("gameUiNewDescription"));
    newDescriptionButton_->setProperty("menuButton", true);
    auto* createMenu = new QMenu(newDescriptionButton_);
    AddNewDescriptionForm(createMenu);
    createMenu->addSeparator();
    // 从文件导入：默认打开随编辑器带的示例目录；同目录有示例触发器时问一次是否一并写入。
    QAction* importAction = createMenu->addAction(tr("从文件导入…"));
    importAction->setObjectName(QStringLiteral("gameUiImportDescription"));
    connect(importAction, &QAction::triggered, this, [this] {
        const QString examples = QString::fromStdWString((CurrentInstallationRoot() / "examples" / "game-ui").wstring());
        const QString path = QFileDialog::getOpenFileName(page_, tr("导入界面"), QDir(examples).exists() ? examples : QString(),
            tr("界面文件 (*.json)"));
        if (!path.isEmpty()) ImportExample(path);
    });
    newDescriptionButton_->setMenu(createMenu);

    addFrameButton_ = new QPushButton(tr("添加控件"), toolbar);
    addFrameButton_->setObjectName(QStringLiteral("gameUiAddFrame"));
    addFrameButton_->setProperty("menuButton", true);
    auto* addMenu = new QMenu(addFrameButton_);
    for (const auto& kind : UiFrameKinds()) {
        if (kind.type == UiFrameType::Template) continue;
        const QString id = Text(std::string(kind.id));
        QAction* action = addMenu->addAction(Text(std::string(kind.label)));
        action->setToolTip(Text(std::string(kind.purpose)));
        connect(action, &QAction::triggered, this, [this, id] {
            QString error;
            if (AddDescriptionFrame(id, &error).isEmpty()) ReportAuthoring(error);
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
        if (!node) return;
        const auto descriptionId = node->uiFrame ? Text(node->uiFrame->description) : OriginDescriptionId(OriginNodeName(selected_));
        if (descriptionId.isEmpty()) return;
        QString error;
        const auto generated = GenerateDescriptionTriggers(descriptionId, false, &error);
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
    runtimeSettingsButton_ = new QPushButton(tr("运行库…"), toolbar);
    runtimeSettingsButton_->setObjectName(QStringLiteral("gameUiRuntimeSettings"));
    connect(runtimeSettingsButton_, &QPushButton::clicked, this, [this] { OfferMapRuntime(true); });
    UpdateAuthoringControls();
    return {newDescriptionButton_, addFrameButton_, removeFrameButton_, generateTriggersButton_, runtimeSettingsButton_};
}

void GameUiModule::UpdateAuthoringControls() {
    const FrameNode* node = tree_ && selected_ < tree_->Nodes().size() ? &tree_->Nodes()[selected_] : nullptr;
    const bool inDescription = node && node->uiFrame;
    const bool isFrame = inDescription && !node->uiFrame->frame.empty();
    if (addFrameButton_) addFrameButton_->setEnabled(definitions_.has_value());
    if (removeFrameButton_) removeFrameButton_->setEnabled(isFrame);
    const bool isOrigin = !OriginNodeName(selected_).isEmpty();
    if (generateTriggersButton_) generateTriggersButton_->setEnabled(inDescription || (isOrigin && !OriginDescriptionId(OriginNodeName(selected_)).isEmpty()));
    if (runtimeSettingsButton_) runtimeSettingsButton_->setEnabled(project_.editTrackingReady_ && !project_.savingMap_);
    const auto* origin = isOrigin ? FindUiOriginPart(OriginNodeName(selected_).toStdString()) : nullptr;
    if (canvas_) {
        canvas_->SetDraggable(isFrame || (origin && origin->position));
        canvas_->SetResizable(isFrame);
    }
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
