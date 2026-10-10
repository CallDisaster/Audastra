#include "EditorProject.h"
#include "ManagerModule.h"
#include "ManagerRows.h"
#include "Modules/ModuleHost.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QHeaderView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <vector>

using namespace War3Map;

namespace {

std::unique_ptr<ObjectData::LniObjectDocument> Units(std::string_view text) {
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(text.data(), text.size())));
    if (!parsed.Succeeded()) return nullptr;
    return std::unique_ptr<ObjectData::LniObjectDocument>(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
}

// 原创合成工程：触发器 Setup 创建 hfoo；变量 Wave 被引用、Unused 未被引用；h000 是未使用的自定义单位。
void Fill(QtProject::EditorProject& project) {
    // 这是来源完整的原创工程；其他六类明确为空，不能用未加载来源来证明四字码唯一。
    for (std::size_t type = 0; type < project.objectDocuments_.size(); ++type) {
        auto empty = ObjectData::LniObjectCodec(static_cast<ObjectData::ObjectType>(type)).Parse({});
        project.objectDocuments_[type].reset(dynamic_cast<ObjectData::LniObjectDocument*>(empty.document.release()));
    }
    project.objectDocuments_[0] = Units("[hfoo]\nuhpm=420\n\n[h000]\n_parent=\"hfoo\"\n");
    auto document = std::make_unique<Trigger::TriggerDocument>();
    Trigger::VariableRecord wave;
    wave.id = 10;
    wave.name.text = "Wave";
    wave.typeName.text = "integer";
    Trigger::VariableRecord unused = wave;
    unused.id = 11;
    unused.name.text = "Unused";
    document->variables = {wave, unused};
    for (const char* value : {"hfoo", "Wave"}) {
        Trigger::ArgumentNode argument;
        argument.id = 100 + document->argumentNodes.size();
        argument.type = std::string_view(value) == "Wave" ? 1 : 3;
        argument.value.text = value;
        document->argumentNodes.push_back(argument);
    }
    Trigger::EcaNode create;
    create.id = 200;
    create.type = 2;
    create.name.text = "CreateUnit";
    create.arguments = {0, 1};
    document->ecaNodes.push_back(create);
    Trigger::TriggerRecord setup;
    setup.id = 1;
    setup.name.text = "Setup";
    setup.ecas = {0};
    Trigger::TriggerRecord off = setup;
    off.id = 2;
    off.name.text = "Off";
    off.enabled = 0;
    off.ecas = {};
    document->triggers = {setup, off};
    project.triggerDocument_ = std::move(document);
}

bool InstallNavigationCatalog(QtProject::EditorProject& project, const QTemporaryDir& root, const QByteArray& title) {
    const auto write = [&root](const QString& relative, const QByteArray& bytes) {
        const auto path = root.filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    if (!write("ui/config", "base\n")
        || !write("ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,integer\n")
        || !write("ui/base/action.txt", QByteArray("[CreateUnit]\ntitle=\"") + title
            + "\"\ncategory=\"Owned\"\ndescription=\"Owned action\"\n")) return false;
    auto loaded = Trigger::UiCatalog::Load(root.path().toStdWString());
    if (!loaded.Succeeded() || !loaded.value || !loaded.value->Find(Trigger::UiDefinitionKind::Action, "CreateUnit")) return false;
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*loaded.value));
    return true;
}

QStringList Column(QTreeWidget* table, int column) {
    QStringList values;
    for (int row = 0; row < table->topLevelItemCount(); ++row) values << table->topLevelItem(row)->text(column);
    return values;
}

QString DetailText(QTreeWidget* detail) {
    QStringList lines;
    for (int row = 0; row < detail->topLevelItemCount(); ++row) lines << detail->topLevelItem(row)->text(0);
    return lines.join('\n');
}

class NavigationRecordingHost final : public Shell::ModuleHost {
public:
    std::vector<Shell::NavigationTarget> targets;
    void Navigate(const Shell::NavigationTarget& target) override { targets.push_back(target); }
    void ActivateModule(const Shell::ModuleId&) override {}
    void ShowStatusMessage(const QString&, int) override {}
    void UndoStackChanged(const Shell::ModuleId&) override {}
};

} // namespace

class ManagerModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void PagesFiltersAndDetail();
    void ReferenceNavigationRequiresUniqueDefinitionNotResolvedName();
    void NativeObjectsShowResolvedNamesAndCurrentStrings();
    void NavigationBookmarkRestoresExactKeyFilterAndScroll();
    void NavigationBookmarkRejectsSameNameVariableReplacement();
    void NavigationBookmarkBindsActualFunctionSource();
    void NavigationBookmarkBoundsVariableIdentityMetadata();
    void RevealRejectsMissingDuplicateAndBudgetWithoutViewMutation();
    void DetailFunctionDoubleClickUsesHostAndKeepsStandaloneFallback();
};

void ManagerModuleTests::DetailFunctionDoubleClickUsesHostAndKeepsStandaloneFallback() {
    QtProject::EditorProject project; Fill(project);
    QTemporaryDir catalog; QVERIFY(catalog.isValid());
    QVERIFY(InstallNavigationCatalog(project, catalog, "Owned CreateUnit"));
    NavigationRecordingHost host;
    Modules::ManagerModule module(project); module.AttachHost(&host);
    auto* page = module.Widget(); page->resize(1200, 700); page->show();
    auto* detail = page->findChild<QTreeWidget*>("managerDetail");
    auto* pages = page->findChild<QListWidget*>("managerPages");
    QVERIFY(detail && pages);
    QVERIFY(module.Reveal({"manager", "trigger", "1", {}}));
    const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
    QTreeWidgetItem* function = nullptr;
    for (QTreeWidgetItemIterator row(detail); *row; ++row)
        if ((*row)->text(0).startsWith(QStringLiteral("CreateUnit ×"))) { function = *row; break; }
    QVERIFY(function);
    const QVariant originalTarget = function->data(0, Qt::UserRole + 3); QVERIFY(originalTarget.isValid());
    detail->expandAll(); detail->scrollToItem(function);
    QTRY_VERIFY(detail->visualItemRect(function).isValid() && !detail->visualItemRect(function).isEmpty());
    QSignalSpy activated(detail, &QTreeWidget::itemActivated); QVERIFY(activated.isValid());
    const auto click = [&] {
        const auto point = detail->visualItemRect(function).center();
        QTest::mouseClick(detail->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseDClick(detail->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    };
    click();
    QCOMPARE(activated.count(), 1); QCOMPARE(host.targets.size(), std::size_t{1});
    QCOMPARE(host.targets.front().module, QStringLiteral("manager"));
    QCOMPARE(host.targets.front().kind, QStringLiteral("function"));
    QCOMPARE(host.targets.front().id, QStringLiteral("CreateUnit"));
    // Fake host 只记录真实 UI 发出的请求；本模块不能先直接 Reveal、丢掉旧选中/滚动。
    QVERIFY(module.CaptureNavigationBookmark() == before);
    QCOMPARE(pages->currentRow(), static_cast<int>(Modules::Manager::Page::Trigger));
    function->setData(0, Qt::UserRole + 3, -1);
    click();
    QCOMPARE(activated.count(), 2); QCOMPARE(host.targets.size(), std::size_t{1});
    QVERIFY(module.CaptureNavigationBookmark() == before);
    function->setData(0, Qt::UserRole + 3, originalTarget);
    module.AttachHost(nullptr);
    click(); // 无 Shell 的独立模块仍沿原入口显示同模块函数。
    QCOMPARE(activated.count(), 3); QCOMPARE(host.targets.size(), std::size_t{1});
    QCOMPARE(pages->currentRow(), static_cast<int>(Modules::Manager::Page::Function));
    const auto arrived = module.CaptureNavigationBookmark(); QVERIFY(arrived);
    QCOMPARE(arrived->value("key").toString(), QStringLiteral("CreateUnit"));
    QCOMPARE(project.triggerDocument_->triggers.front().id, StableId{1});
    QCOMPARE(project.mapDataUndoStack_->count(), 0); QCOMPARE(project.AssetUndoStack()->count(), 0);
}

void ManagerModuleTests::RevealRejectsMissingDuplicateAndBudgetWithoutViewMutation() {
    QtProject::EditorProject project; Fill(project);
    auto* undo = project.AssetUndoStack(); QVERIFY(undo);
    undo->push(new QUndoCommand(QStringLiteral("owned synthetic history")));
    Modules::ManagerModule module(project); auto* page = module.Widget(); page->show();
    auto* table = page->findChild<QTreeWidget*>("managerTable");
    auto* detail = page->findChild<QTreeWidget*>("managerDetail");
    auto* search = page->findChild<QLineEdit*>("managerSearch");
    auto* filter = page->findChild<QComboBox*>("managerFilter");
    auto* raw = page->findChild<QCheckBox*>("managerRawValues");
    auto* pages = page->findChild<QListWidget*>("managerPages");
    QVERIFY(table && detail && search && filter && raw && pages);
    QVERIFY(module.Reveal({"manager", "variable", "Unused", {}}));
    filter->setCurrentIndex(filter->findData("unused")); search->setText("Unused"); raw->setChecked(true);
    const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    const auto state = module.SaveViewState();
    const auto rows = Column(table, 0);
    const auto details = DetailText(detail);
    auto expectedBookmark = saved;
    const int undoCount = undo->count(), undoIndex = undo->index();
    QSignalSpy mapChanged(&project, &QtProject::EditorProject::MapDocumentChanged);
    QSignalSpy triggerChanged(&project, &QtProject::EditorProject::TriggerDocumentChanged);
    const auto unchanged = [&]() {
        return module.SaveViewState() == state && module.CaptureNavigationBookmark() == expectedBookmark
            && search->text() == QStringLiteral("Unused") && filter->currentData().toString() == QStringLiteral("unused")
            && raw->isChecked() && pages->currentRow() == static_cast<int>(Modules::Manager::Page::Variable)
            && Column(table, 0) == rows && DetailText(detail) == details
            && undo->count() == undoCount && undo->index() == undoIndex
            && mapChanged.isEmpty() && triggerChanged.isEmpty();
    };
    const std::array<Shell::NavigationTarget, 5> missing{{{"manager", "variable", "Missing", {}},
        {"manager", "function", "Missing", {}}, {"manager", "object", "unit:missing", {}},
        {"manager", "trigger", "Missing", {}}, {"manager", "object", "unit:", {}}}};
    for (const auto& target : missing) {
        QVERIFY(!module.Reveal(target)); QVERIFY(unchanged());
    }
    // 两个实际触发器的相同稳定行键也不能接受首项或末项。
    const auto originalTriggers = project.triggerDocument_->triggers;
    auto duplicate = originalTriggers.front(); duplicate.name.text = "Duplicate owner";
    project.triggerDocument_->triggers.push_back(std::move(duplicate));
    expectedBookmark = module.CaptureNavigationBookmark(); // 来源变化可先使旧书签失效，拒绝不能再改变该状态。
    QVERIFY(!module.Reveal({"manager", "trigger", "1", {}})); QVERIFY(unchanged());
    project.triggerDocument_->triggers = originalTriggers;
    // 在另一页制造预算拒绝，以便当前可信书签仍可完整比较。
    for (std::size_t index = 0; index < 4095; ++index) {
        auto trigger = originalTriggers.front(); trigger.id = 1000 + static_cast<StableId>(index);
        project.triggerDocument_->triggers.push_back(std::move(trigger));
    }
    expectedBookmark = module.CaptureNavigationBookmark();
    QVERIFY(!module.Reveal({"manager", "trigger", "1", {}})); QVERIFY(unchanged());
    project.triggerDocument_->triggers = originalTriggers;
    const auto originalVariables = project.triggerDocument_->variables;
    for (std::size_t index = 0; index < 22; ++index) {
        auto variable = originalVariables.front(); variable.id = 1000 + static_cast<StableId>(index);
        variable.name.text = std::to_string(index) + std::string(50000, 'x');
        project.triggerDocument_->variables.push_back(std::move(variable));
    }
    expectedBookmark = module.CaptureNavigationBookmark();
    QVERIFY(!module.Reveal({"manager", "variable", "Unused", {}})); QVERIFY(unchanged());
    project.triggerDocument_->variables = originalVariables;
    // 失败后无需恢复控件字符串，原目标仍可由同一业务查询正常定位。
    QVERIFY(module.Reveal({"manager", "trigger", "1", {}}));
    QCOMPARE(pages->currentRow(), static_cast<int>(Modules::Manager::Page::Trigger));
    QVERIFY(table->currentItem()); QCOMPARE(table->currentItem()->text(0), QStringLiteral("Setup"));
    QCOMPARE(undo->count(), undoCount); QCOMPARE(undo->index(), undoIndex);
}

void ManagerModuleTests::NavigationBookmarkRestoresExactKeyFilterAndScroll() {
    QtProject::EditorProject project; Fill(project);
    for (int index = 0; index < 80; ++index) {
        auto variable = project.triggerDocument_->variables.front();
        variable.id = 1000 + static_cast<StableId>(index);
        variable.name.text = QStringLiteral("Entry%1").arg(index, 3, 10, QLatin1Char('0')).toStdString();
        project.triggerDocument_->variables.push_back(std::move(variable));
    }
    Modules::ManagerModule module(project); auto* page = module.Widget(); page->resize(780, 360); page->show();
    auto* table = page->findChild<QTreeWidget*>("managerTable");
    auto* search = page->findChild<QLineEdit*>("managerSearch");
    auto* filter = page->findChild<QComboBox*>("managerFilter");
    auto* raw = page->findChild<QCheckBox*>("managerRawValues");
    auto* pages = page->findChild<QListWidget*>("managerPages");
    QVERIFY(table && search && filter && raw && pages);
    QVERIFY(module.Reveal({"manager", "variable", "Entry040", {}}));
    search->setText("Entry"); filter->setCurrentIndex(filter->findData("unused")); raw->setChecked(true);
    table->sortItems(0, Qt::DescendingOrder);
    QTRY_VERIFY(table->verticalScrollBar()->maximum() > 0);
    table->verticalScrollBar()->setValue(table->verticalScrollBar()->maximum() / 2);
    const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    QCOMPARE(saved->value("key").toString(), QStringLiteral("Entry040"));
    QCOMPARE(saved->value("filter").toString(), QStringLiteral("unused"));
    const auto count = project.triggerDocument_->variables.size();
    QVERIFY(module.Reveal({"manager", "object", "unit:hfoo", {}})); raw->setChecked(false);
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Restored);
    QCOMPARE(search->text(), QStringLiteral("Entry")); QCOMPARE(filter->currentData().toString(), QStringLiteral("unused"));
    QVERIFY(raw->isChecked()); QVERIFY(table->currentItem()); QCOMPARE(table->currentItem()->text(0), QStringLiteral("Entry040"));
    QCOMPARE(table->header()->sortIndicatorOrder(), Qt::DescendingOrder);
    QCOMPARE(table->verticalScrollBar()->value(), saved->value("vertical").toInt());
    QCOMPARE(project.triggerDocument_->variables.size(), count);
    for (const auto* field : {"key", "filter", "search"}) {
        auto invalid = *saved; invalid.insert(QLatin1String(field), QStringLiteral("MissingTarget"));
        const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
        QCOMPARE(module.RestoreNavigationBookmark(invalid), Shell::NavigationRestoreResult::Invalid);
        QVERIFY(module.CaptureNavigationBookmark() == before);
    }
    // 实体被删除后，不接受 PopulateTable 的首行回退。
    std::erase_if(project.triggerDocument_->variables, [](const auto& item) { return item.name.text == "Entry040"; });
    const auto beforeRemoved = module.CaptureNavigationBookmark();
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Invalid);
    QVERIFY(module.CaptureNavigationBookmark() == beforeRemoved);
    // 概况页不能将隐藏 table 的旧选中键带入历史。
    pages->setCurrentRow(static_cast<int>(Modules::Manager::Page::Overview));
    const auto overview = module.CaptureNavigationBookmark(); QVERIFY(overview);
    QVERIFY(overview->value("key").toString().isEmpty());
    QVERIFY(module.Reveal({"manager", "variable", "Wave", {}}));
    QCOMPARE(module.RestoreNavigationBookmark(*overview), Shell::NavigationRestoreResult::Restored);
    QCOMPARE(pages->currentRow(), static_cast<int>(Modules::Manager::Page::Overview));
}

void ManagerModuleTests::NavigationBookmarkRejectsSameNameVariableReplacement() {
    QtProject::EditorProject project; Fill(project);
    Modules::ManagerModule module(project); auto* page = module.Widget(); page->show();
    QVERIFY(module.Reveal({"manager", "variable", "Wave", {}}));
    const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    QCOMPARE(saved->value("identity").toObject().value("stableId").toString(), QStringLiteral("10"));
    auto replacement = project.triggerDocument_->variables.front();
    replacement.id = 900;
    project.triggerDocument_->variables.erase(project.triggerDocument_->variables.begin());
    project.triggerDocument_->variables.push_back(replacement); // 新定义复用旧名字，不能复用旧导航身份。
    QVERIFY(!module.CaptureNavigationBookmark()); // 正式刷新前的旧行也不能由新定义补身份。
    const auto before = module.SaveViewState();
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), before);
    module.Refresh(true);
    const auto current = module.CaptureNavigationBookmark(); QVERIFY(current);
    QCOMPARE(current->value("identity").toObject().value("stableId").toString(), QStringLiteral("900"));
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Invalid);
    QVERIFY(module.CaptureNavigationBookmark() == current);
    auto forged = *current; auto identity = forged.value("identity").toObject();
    identity.insert("stableId", QStringLiteral("0900")); forged.insert("identity", identity);
    QCOMPARE(module.RestoreNavigationBookmark(forged), Shell::NavigationRestoreResult::Invalid);
    QVERIFY(module.CaptureNavigationBookmark() == current);
    project.triggerDocument_->variables.front().id = 900; // 不同名字的 ID 冲突同样没有可信身份。
    QVERIFY(!module.CaptureNavigationBookmark());
    QCOMPARE(module.RestoreNavigationBookmark(*current), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), before);
}

void ManagerModuleTests::NavigationBookmarkBindsActualFunctionSource() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QtProject::EditorProject project; Fill(project);
    QVERIFY(InstallNavigationCatalog(project, root, "Owned function"));
    Modules::ManagerModule module(project); auto* page = module.Widget(); page->show();
    QVERIFY(module.Reveal({"manager", "function", "CreateUnit", {}}));
    const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    const auto identity = saved->value("identity").toObject();
    const auto* definition = project.uiCatalog_->Find(Trigger::UiDefinitionKind::Action, "CreateUnit"); QVERIFY(definition);
    QCOMPARE(identity.value("sourceSha256").toString(), QString::fromStdString(definition->Active().sourceSha256));
    QCOMPARE(identity.value("catalogSha256").toString(), QString::fromStdString(project.uiCatalog_->Sha256()));
    QCOMPARE(identity.value("source").toString(), QString::fromStdWString(definition->Active().source.generic_wstring()));
    QCOMPARE(identity.value("referenceGeneration").toString(), QString::number(project.ReferenceGeneration()));
    QVERIFY(module.Reveal({"manager", "object", "unit:hfoo", {}}));
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Restored);
    const auto sameSourceSha = identity.value("sourceSha256");
    QVERIFY(InstallNavigationCatalog(project, root, "Owned function")); // 字节不变，真实引用代数仍换代。
    QCOMPARE(QString::fromStdString(project.uiCatalog_->Find(Trigger::UiDefinitionKind::Action, "CreateUnit")->Active().sourceSha256),
        sameSourceSha.toString());
    const auto beforeGeneration = module.SaveViewState();
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), beforeGeneration);
    module.Refresh(true);
    const auto regenerated = module.CaptureNavigationBookmark(); QVERIFY(regenerated);
    QVERIFY(regenerated->value("identity").toObject().value("referenceGeneration") != identity.value("referenceGeneration"));
    QVERIFY(InstallNavigationCatalog(project, root, "Replacement function"));
    QVERIFY(QString::fromStdString(project.uiCatalog_->Find(Trigger::UiDefinitionKind::Action, "CreateUnit")->Active().sourceSha256)
        != sameSourceSha.toString());
    const auto beforeSource = module.SaveViewState();
    QCOMPARE(module.RestoreNavigationBookmark(*regenerated), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), beforeSource);
    project.uiCatalog_.reset(); // 被引用但无可信目录定义的函数只能只读展示，不捕获导航身份。
    QVERIFY(!module.CaptureNavigationBookmark());
    QCOMPARE(module.RestoreNavigationBookmark(*regenerated), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), beforeSource);
}

void ManagerModuleTests::NavigationBookmarkBoundsVariableIdentityMetadata() {
    QtProject::EditorProject project; Fill(project);
    Modules::ManagerModule module(project); auto* page = module.Widget(); page->show();
    QVERIFY(module.Reveal({"manager", "variable", "Wave", {}}));
    const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    const auto before = module.SaveViewState();
    const auto base = project.triggerDocument_->variables;
    for (int index = 0; index < 22; ++index) {
        auto variable = base.front(); variable.id = 1000 + static_cast<StableId>(index);
        variable.name.text = std::to_string(index) + std::string(50000, 'x');
        project.triggerDocument_->variables.push_back(std::move(variable));
    }
    QVERIFY(!module.CaptureNavigationBookmark()); // 每条低于64 KiB，但全部定义已超过1 MiB。
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), before);
    project.triggerDocument_->variables = base;
    for (int index = 0; index < 4095; ++index) {
        auto variable = base.front(); variable.id = 1000 + static_cast<StableId>(index);
        variable.name.text = "Owned" + std::to_string(index);
        project.triggerDocument_->variables.push_back(std::move(variable));
    }
    QVERIFY(!module.CaptureNavigationBookmark()); // 小文本仍不能越过4096定义上限。
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Invalid);
    QCOMPARE(module.SaveViewState(), before);
    project.triggerDocument_->variables = base;
    QVERIFY(module.CaptureNavigationBookmark() == saved);
}

void ManagerModuleTests::ReferenceNavigationRequiresUniqueDefinitionNotResolvedName() {
    QtProject::EditorProject project;
    Fill(project);
    const auto withoutName = Modules::Manager::ResolveToken(project, "hfoo");
    QVERIFY(withoutName);
    QCOMPARE(withoutName->target.id, QStringLiteral("unit:hfoo"));
    QVERIFY(withoutName->label.contains(QStringLiteral("hfoo")));
    QVERIFY(withoutName->label.contains(QStringLiteral("原版物体目录未加载")));
    QVERIFY(project.objectDocuments_[0]->SetModification({ObjectData::ObjectType::Unit, "hfoo"},
        {"unam"}, std::string("TRIGSTR_123")).Succeeded());
    emit project.MapDocumentChanged();
    const auto withoutString = Modules::Manager::ResolveToken(project, "hfoo");
    QVERIFY(withoutString);
    QCOMPARE(withoutString->target.id, QStringLiteral("unit:hfoo"));
    QVERIFY(withoutString->label.contains(QStringLiteral("hfoo")));
    QVERIFY(withoutString->label.contains(QStringLiteral("地图字符串表未加载")));
    QVERIFY(!withoutString->label.contains(QStringLiteral("TRIGSTR_123 (hfoo)")));
    // 未载入的竞争类别不等于空；恢复明确空来源后才重新允许跳转。
    project.objectDocuments_[1].reset();
    QVERIFY(!Modules::Manager::ResolveToken(project, "hfoo"));
    auto empty = ObjectData::LniObjectCodec(ObjectData::ObjectType::Ability).Parse({});
    QVERIFY(empty.Succeeded());
    project.objectDocuments_[1].reset(dynamic_cast<ObjectData::LniObjectDocument*>(empty.document.release()));
    QVERIFY(Modules::Manager::ResolveToken(project, "hfoo"));
    const std::string broken = "not-an-lni-structure\n";
    auto partial = ObjectData::LniObjectCodec(ObjectData::ObjectType::Ability).Parse(std::as_bytes(std::span(broken.data(), broken.size())));
    project.objectDocuments_[1].reset(dynamic_cast<ObjectData::LniObjectDocument*>(partial.document.release()));
    QVERIFY(project.objectDocuments_[1] && !project.objectDocuments_[1]->IsStructurallySafe());
    QVERIFY(!Modules::Manager::ResolveToken(project, "hfoo"));
    const std::string duplicate = "[hfoo]\nunam=\"同码技能\"\n";
    auto ability = ObjectData::LniObjectCodec(ObjectData::ObjectType::Ability).Parse(std::as_bytes(std::span(duplicate.data(), duplicate.size())));
    QVERIFY(ability.Succeeded());
    project.objectDocuments_[1].reset(dynamic_cast<ObjectData::LniObjectDocument*>(ability.document.release()));
    QVERIFY(!Modules::Manager::ResolveToken(project, "hfoo"));
}

void ManagerModuleTests::NativeObjectsShowResolvedNamesAndCurrentStrings() {
    std::vector<std::byte> native;
    const auto integer = [&](std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8) native.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    };
    const auto raw = [&](std::string_view value) { for (const char c : value) native.push_back(static_cast<std::byte>(c)); };
    integer(2); integer(0); integer(1); raw("hfoo"); raw("H001"); integer(1);
    raw("unam"); integer(3); raw("TRIGSTR_8"); native.push_back(std::byte{0}); integer(0);
    auto parsed = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Unit).Parse(native);
    QVERIFY(parsed.Succeeded() && parsed.document);
    QtProject::EditorProject project;
    project.nativeObjectDocuments_[0].reset(dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.release()));
    QVERIFY(project.nativeObjectDocuments_[0]);
    const std::string wts = "STRING 8\n{\n原生自定义单位\n}\n";
    const auto bytes = std::as_bytes(std::span(wts.data(), wts.size()));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.wts", {bytes.begin(), bytes.end()}}, {"war3map.w3u", native}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    Trigger::VariableRecord initial;
    initial.name.text = "UnitType";
    initial.typeName.text = "unitcode";
    initial.hasDefault = 1;
    initial.value.text = "H001";
    project.triggerDocument_->variables.push_back(initial);
    Modules::ManagerModule module(project);
    auto* page = module.Widget(); page->show();
    QVERIFY(module.Reveal({"manager", "object", "unit:H001", {}}));
    auto* table = page->findChild<QTreeWidget*>("managerTable");
    auto* rawValues = page->findChild<QCheckBox*>("managerRawValues");
    QVERIFY(table && rawValues);
    const auto allDetails = [&] {
        QStringList text;
        for (QTreeWidgetItemIterator item(page->findChild<QTreeWidget*>("managerDetail")); *item; ++item)
            text << (*item)->text(0);
        return text.join(QLatin1Char('\n'));
    };
    QCOMPARE(table->topLevelItemCount(), 1);
    QCOMPARE(Column(table, 0), QStringList{QStringLiteral("原生自定义单位")});
    QCOMPARE(Column(table, 1), QStringList{"H001"});
    QVERIFY(allDetails().contains(QStringLiteral("初始值 = 原生自定义单位")));
    const auto owner = Services::ObjectOwnerId({ObjectData::ObjectType::Unit, "H001"});
    QVERIFY(project.References().Outgoing(owner).contains("hfoo"));
    const auto generation = project.ReferenceGeneration();
    rawValues->setChecked(true);
    QCOMPARE(Column(table, 0), QStringList{"H001"});
    QVERIFY(allDetails().contains(QStringLiteral("初始值 = H001")));
    rawValues->setChecked(false);
    auto strings = *project.mapEdits_->Snapshot().Strings();
    strings.entries[0].value = "当前字符串名字";
    QVERIFY(project.mapEdits_->EditStrings(strings).Succeeded());
    emit project.MapDocumentChanged();
    QCOMPARE(Column(table, 0), QStringList{QStringLiteral("当前字符串名字")});
    QVERIFY(project.ReferenceGeneration() > generation);
    QVERIFY(project.References().Outgoing(owner).contains("hfoo"));
    QCOMPARE(ObjectData::ValueToDisplayString(project.nativeObjectDocuments_[0]->Objects()[0].modifications[0].value), std::string("TRIGSTR_8"));
    QCOMPARE(project.mapDocument_->ResolveString("TRIGSTR_8").value, std::optional<std::string>("原生自定义单位"));
}

void ManagerModuleTests::PagesFiltersAndDetail() {
    QtProject::EditorProject project;
    Fill(project);
    Modules::ManagerModule module(project);
    QWidget* page = module.Widget();
    page->resize(1200, 700);
    page->show();
    auto* pages = page->findChild<QListWidget*>("managerPages");
    auto* table = page->findChild<QTreeWidget*>("managerTable");
    auto* filter = page->findChild<QComboBox*>("managerFilter");
    auto* detail = page->findChild<QTreeWidget*>("managerDetail");
    auto* overview = page->findChild<QListWidget*>("managerOverview");
    QVERIFY(pages && table && filter && detail && overview);

    // 概况：清理建议列出未使用的变量与自定义物体、已禁用的触发器。
    QStringList overviewText;
    for (int row = 0; row < overview->count(); ++row) overviewText << overview->item(row)->text();
    QVERIFY(overviewText.join('\n').contains(QStringLiteral("未发现静态引用的变量　1")));
    QVERIFY(overviewText.join('\n').contains(QStringLiteral("未发现静态引用的自定义物体　1")));
    QVERIFY(overviewText.join('\n').contains(QStringLiteral("已禁用的触发器　1")));

    // 单位页：已修改的 hfoo 被触发器与 h000 的父对象各引用一次；自定义 h000 未使用。
    QVERIFY(module.Reveal({"manager", "object", "unit:hfoo", {}}));
    QCOMPARE(pages->currentRow(), static_cast<int>(Modules::Manager::Page::Unit));
    QCOMPARE(table->topLevelItemCount(), 2);
    QVERIFY(Column(table, 1).contains("hfoo") && Column(table, 1).contains("h000"));
    QVERIFY(DetailText(detail).contains(QStringLiteral("被引用（2）")));
    QVERIFY(DetailText(detail).contains(QStringLiteral("放置在地图上（0）")));

    filter->setCurrentIndex(filter->findData("unused"));
    QCOMPARE(Column(table, 1), QStringList{"h000"});
    QVERIFY(module.Reveal({"manager", "object", "unit:h000", {}}));
    auto* deletionWarning = page->findChild<QLabel*>("managerWarning");
    QVERIFY(deletionWarning != nullptr);
    QVERIFY(deletionWarning->text().contains(QStringLiteral("删除暂不可用")));
    QVERIFY(deletionWarning->toolTip().contains(QStringLiteral("JASS")));
    filter->setCurrentIndex(filter->findData("modified"));
    QCOMPARE(Column(table, 1), QStringList{"hfoo"});

    // 触发器页：使用了 hfoo 与变量 Wave，调用了 CreateUnit。
    QVERIFY(module.Reveal({"manager", "trigger", "1", {}}));
    const QString triggerDetail = DetailText(detail);
    QVERIFY2(triggerDetail.contains(QStringLiteral("使用了（2）")), qPrintable(triggerDetail));
    QVERIFY(triggerDetail.contains(QStringLiteral("调用的函数（1）")));
    filter->setCurrentIndex(filter->findData("off"));
    QCOMPARE(Column(table, 0), QStringList{"Off"});

    // 变量页与函数页。
    QVERIFY(module.Reveal({"manager", "variable", "Unused", {}}));
    filter->setCurrentIndex(filter->findData("unused"));
    QCOMPARE(Column(table, 0), QStringList{"Unused"});
    QVERIFY(module.Reveal({"manager", "function", "CreateUnit", {}}));
    QVERIFY(DetailText(detail).contains(QStringLiteral("调用位置（1）")));

    // 视图状态往返。
    const auto state = module.SaveViewState();
    Modules::ManagerModule restored(project);
    restored.RestoreViewState(state);
    auto* restoredPage = restored.Widget();
    QCOMPARE(restoredPage->findChild<QListWidget*>("managerPages")->currentRow(), static_cast<int>(Modules::Manager::Page::Function));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    ManagerModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ManagerModuleTests.moc"
