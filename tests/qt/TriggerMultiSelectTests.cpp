#include "EditorProject.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "TriggerModels.h"
#include "TriggerModule.h"

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerSelection.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QApplication>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>

// 触发器编辑器的 Windows 式多选：目录与传统视图的批量删除 / 复制粘贴 / 拖放 / 允许，
// 一步撤销、祖先代表子项、分组头不算、刷新后保留选区。全部用原创合成 WTG/WCT。
using namespace War3Map;

namespace {

class Bytes {
public:
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 0xffU));
    }
    void Text(std::string_view value) {
        for (const char c : value) data.push_back(static_cast<std::byte>(c));
        data.push_back(std::byte{0});
    }
    void Magic() {
        for (const char c : std::string_view("WTG!")) data.push_back(static_cast<std::byte>(c));
        I32(7);
    }
    void TriggerHeader(std::string_view name, std::int32_t category) {
        Text(name); Text(""); I32(0); I32(1); I32(0); I32(0); I32(0); I32(category);
    }
    // 没有参数的语句；nested 时写分支号。
    void Statement(std::int32_t type, std::string_view name, std::optional<std::int32_t> branch = std::nullopt, std::int32_t children = 0) {
        I32(type);
        if (branch) I32(*branch);
        Text(name); I32(1); I32(children);
    }
    std::vector<std::byte> data;
};

std::vector<std::byte> EmptyWct(int triggers) {
    Bytes w;
    w.I32(1); w.Text(""); w.I32(0); w.I32(triggers);
    for (int index = 0; index < triggers; ++index) w.I32(0);
    return std::move(w.data);
}

// 目录：类别 1 System（A、B、C 各含 Plain，D 为空），类别 2 Combat（E 运行 A）。
std::vector<std::byte> DirectoryWtg() {
    Bytes w;
    w.Magic();
    w.I32(2);
    w.I32(1); w.Text("System"); w.I32(0);
    w.I32(2); w.Text("Combat"); w.I32(0);
    w.I32(2); w.I32(0);
    w.I32(5);
    for (const auto name : {"A", "B", "C"}) {
        w.TriggerHeader(name, 1); w.I32(1); w.Statement(2, "PlainAction");
    }
    w.TriggerHeader("D", 1); w.I32(0);
    w.TriggerHeader("E", 2); w.I32(1);
    w.I32(2); w.Text("RunTrigger"); w.I32(1);
    w.I32(1); w.Text("gg_trg_A"); w.I32(0); w.I32(0);
    w.I32(0);
    return std::move(w.data);
}

// 语句：触发器 Logic = 事件 RawEvent；动作 Alpha、Beta、If（条件 RawCond，则 Inner1、Inner2）、Gamma、Delta。
std::vector<std::byte> LogicWtg() {
    Bytes w;
    w.Magic();
    w.I32(1);
    w.I32(1); w.Text("Main"); w.I32(0);
    w.I32(2); w.I32(0);
    w.I32(1);
    w.TriggerHeader("Logic", 1);
    w.I32(6);
    w.Statement(0, "RawEvent");
    w.Statement(2, "Alpha");
    w.Statement(2, "Beta");
    w.Statement(2, "IfThenElseMultiple", std::nullopt, 3);
    w.Statement(1, "RawCond", 0);
    w.Statement(2, "Inner1", 1);
    w.Statement(2, "Inner2", 1);
    w.Statement(2, "Gamma");
    w.Statement(2, "Delta");
    return std::move(w.data);
}

void WriteUtf8(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

bool Install(QtProject::EditorProject& project, const std::filesystem::path& root, bool logic) {
    WriteUtf8(root / "ui" / "config", "base\n");
    WriteUtf8(root / "ui" / "base" / "event.txt", "[RawEvent]\ntitle=\"Event\"\ndescription=\"Map starts\"\n");
    WriteUtf8(root / "ui" / "base" / "condition.txt", "[RawCond]\ntitle=\"Cond\"\ndescription=\"Cond holds\"\n");
    std::string actions = "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n"
        "[RunTrigger]\ntitle=\"Run\"\ndescription=\"Run ${trigger}\"\n[[.args]]\ntype=trigger\n"
        "[IfThenElseMultiple]\ntitle=\"If\"\ndescription=\"If / Then / Else\"\n";
    for (const auto name : {"Alpha", "Beta", "Gamma", "Delta", "Inner1", "Inner2"}) {
        actions += std::string("[") + name + "]\ntitle=\"" + name + "\"\ndescription=\"" + name + "\"\n";
    }
    WriteUtf8(root / "ui" / "base" / "action.txt", actions);
    auto catalog = Trigger::UiCatalog::Load(root);
    if (!catalog.Succeeded() || !catalog.value) return false;
    auto parsed = Trigger::WtgWctCodec{}.Parse(logic ? LogicWtg() : DirectoryWtg(), EmptyWct(logic ? 1 : 5), *catalog.value);
    if (!parsed.Succeeded() || !parsed.value) return false;
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.triggerUndoStack_ = new QUndoStack(&project);
    return true;
}

const Trigger::TriggerRecord* Named(const QtProject::EditorProject& project, std::string_view name) {
    for (const auto& trigger : project.triggerDocument_->triggers) {
        if (trigger.name.text == name) return &trigger;
    }
    return nullptr;
}

StableId IdOf(const QtProject::EditorProject& project, std::string_view name) {
    const auto* trigger = Named(project, name);
    return trigger ? trigger->id : 0;
}

// 每个类别的触发器名称（文档顺序），类别按目录顺序：“System: A B | Combat: E”。
QString Listing(const QtProject::EditorProject& project) {
    QStringList parts;
    for (const auto& category : project.triggerDocument_->categories) {
        QStringList names;
        for (const auto& trigger : project.triggerDocument_->triggers) {
            if (trigger.categoryId == category.categoryId) names << QString::fromStdString(trigger.name.text);
        }
        parts << QString::fromStdString(category.name.text) + QStringLiteral(": ") + names.join(QLatin1Char(' '));
    }
    return parts.join(QStringLiteral(" | "));
}

// 语句树文字：根按存储顺序，块语句在括号里列出子语句（含分支号）。
QString Tree(const QtProject::EditorProject& project) {
    const auto& document = *project.triggerDocument_;
    const auto describe = [&document](const auto& self, std::uint32_t index) -> QString {
        const auto& node = document.ecaNodes[index];
        QString text = QString::fromStdString(node.name.text);
        if (node.enabled == 0) text += QLatin1Char('-');
        if (!node.children.empty()) {
            QStringList children;
            for (const std::uint32_t child : node.children) {
                children << QString::number(document.ecaNodes[child].childId.value_or(-1)) + QLatin1Char(':') + self(self, child);
            }
            text += QLatin1Char('(') + children.join(QLatin1Char(' ')) + QLatin1Char(')');
        }
        return text;
    };
    QStringList roots;
    for (const std::uint32_t root : document.triggers.front().ecas) roots << describe(describe, root);
    return roots.join(QLatin1Char(' '));
}

// 语句的稳定 ID：按名称（Logic 中名称唯一）。
StableId Eca(const QtProject::EditorProject& project, std::string_view name) {
    for (const auto& node : project.triggerDocument_->ecaNodes) {
        if (node.name.text == name && node.type != 3) return node.id;
    }
    return 0;
}

// 像用户一样选中若干行：第一行为当前项，其余 Ctrl 追加。
void SelectRows(QTreeView* view, const QModelIndexList& rows) {
    view->setCurrentIndex(rows.front());
    for (qsizetype index = 1; index < rows.size(); ++index) {
        view->selectionModel()->select(rows[index], QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
}

struct DirectoryFixture {
    QTemporaryDir root;
    QtProject::EditorProject project;
    std::unique_ptr<Modules::TriggerModule> module;
    QWidget* page = nullptr;
    QTreeView* view = nullptr;
    QSortFilterProxyModel* proxy = nullptr;
    TriggerExplorerModel* model = nullptr;
    int confirmations = 0;
    QString lastBody;

    bool Open(bool logic = false) {
        if (!root.isValid() || !Install(project, root.path().toStdWString(), logic)) return false;
        module = std::make_unique<Modules::TriggerModule>(project);
        module->SetConfirmHandler([this](const QString&, const QString& body, const QStringList&) {
            ++confirmations;
            lastBody = body;
            return true;
        });
        page = module->Widget();
        page->resize(1200, 700);
        page->show();
        view = page->findChild<QTreeView*>(QStringLiteral("triggerExplorer"));
        proxy = view ? qobject_cast<QSortFilterProxyModel*>(view->model()) : nullptr;
        model = proxy ? dynamic_cast<TriggerExplorerModel*>(proxy->sourceModel()) : nullptr;
        return model != nullptr;
    }
    QModelIndex Row(std::string_view name) const {
        return proxy->mapFromSource(model->IndexForTriggerId(IdOf(project, name)));
    }
    QModelIndex CategoryRow(std::int32_t categoryId) const {
        return proxy->mapFromSource(model->IndexForCategoryId(categoryId));
    }
    void Select(std::initializer_list<std::string_view> names) {
        QModelIndexList rows;
        for (const auto name : names) rows << Row(name);
        SelectRows(view, rows);
    }
    QStringList SelectedNames() const {
        QStringList names;
        for (const auto& item : module->SelectedDirectoryItems()) {
            if (item.category) {
                names << QStringLiteral("#%1").arg(item.categoryId);
            } else {
                for (const auto& trigger : project.triggerDocument_->triggers) {
                    if (trigger.id == item.triggerId) names << QString::fromStdString(trigger.name.text);
                }
            }
        }
        return names;
    }
};

struct LogicFixture : DirectoryFixture {
    QTreeView* logic = nullptr;
    TriggerEcaModel* ecaModel = nullptr;

    bool OpenLogic() {
        if (!Open(true) || !module->Reveal({"triggers", "trigger", "Logic", {}})) return false;
        logic = page->findChild<QTreeView*>(QStringLiteral("triggerLogicTree"));
        ecaModel = logic ? dynamic_cast<TriggerEcaModel*>(logic->model()) : nullptr;
        return ecaModel != nullptr;
    }
    QModelIndex EcaRow(std::string_view name) const {
        return ecaModel->IndexForStableId(Eca(project, name));
    }
    void SelectEca(std::initializer_list<std::string_view> names) {
        QModelIndexList rows;
        for (const auto name : names) rows << EcaRow(name);
        SelectRows(logic, rows);
    }
    QStringList SelectedEcaNames() const {
        QStringList names;
        for (const StableId id : module->SelectedEcaIds()) {
            const auto* node = Trigger::TriggerDocumentEditor::FindEca(*project.triggerDocument_, id);
            names << (node ? QString::fromStdString(node->name.text) : QStringLiteral("?"));
        }
        return names;
    }
    int SelectedRowCount() const { return static_cast<int>(logic->selectionModel()->selectedRows(0).size()); }
};

} // namespace

class TriggerMultiSelectTests final : public QObject {
    Q_OBJECT
private slots:
    void DirectoryMouseFollowsWindowsSemantics();
    void DirectoryDeleteIsOneConfirmationAndOneUndo();
    void DirectoryCopyPasteKeepsOrderAndCutMoves();
    void DirectoryCategoryCoversItsTriggers();
    void DirectoryDragMovesSeveralInOrder();
    void DirectoryFlagsBatchAndSelectionSurvivesRefresh();
    void StatementDeleteAcrossParentsIsOneUndo();
    void StatementCutPasteKeepsDocumentOrderAndRefusesWrongKinds();
    void StatementAncestorCoversChildAndHeadersIgnored();
    void StatementDuplicateEnableAndSelectionSurvivesRefresh();
    void StatementBlockMoveAndDrag();
};

void TriggerMultiSelectTests::DirectoryMouseFollowsWindowsSemantics() {
    DirectoryFixture f;
    QVERIFY(f.Open());
    QVERIFY(QTest::qWaitForWindowExposed(f.page));
    auto* viewport = f.view->viewport();
    const auto at = [&](std::string_view name) { return f.view->visualRect(f.Row(name)).center(); };
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, at("A"));
    QCOMPARE(f.SelectedNames(), (QStringList{"A"}));
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::ControlModifier, at("C"));
    QCOMPARE(f.SelectedNames(), (QStringList{"A", "C"}));
    // Shift 单击：从锚点 C 选到 D，之前 Ctrl 选的 A 不保留。
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::ShiftModifier, at("D"));
    QCOMPARE(f.SelectedNames(), (QStringList{"C", "D"}));
    // Ctrl+Shift 单击：在现有选区上追加锚点 C 到 A 的一段。
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier, at("A"));
    QCOMPARE(f.SelectedNames(), (QStringList{"A", "B", "C", "D"}));
    // Ctrl 单击已选中的项：取消它；当前项仍驱动属性区。
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::ControlModifier, at("B"));
    QCOMPARE(f.SelectedNames(), (QStringList{"A", "C", "D"}));
    // 普通单击：只选一项。
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, at("E"));
    QCOMPARE(f.SelectedNames(), (QStringList{"E"}));
    // Ctrl+A 全选：两个类别（类别代表其中全部触发器）。
    QTest::keyClick(f.view, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(f.SelectedNames(), (QStringList{"#1", "#2"}));
}

void TriggerMultiSelectTests::DirectoryDeleteIsOneConfirmationAndOneUndo() {
    DirectoryFixture f;
    QVERIFY(f.Open());
    const QString original = Listing(f.project);
    QCOMPARE(original, QStringLiteral("System: A B C D | Combat: E"));
    // A 被 E 引用，但 E 也一起删除：被删除的触发器之间的引用不算。
    f.Select({"A", "C", "E"});
    QCOMPARE(f.SelectedNames(), (QStringList{"A", "C", "E"}));
    f.module->DeleteCurrentItem();
    QCOMPARE(f.confirmations, 1);
    QVERIFY2(f.lastBody.contains(QStringLiteral("3 个触发器")), qPrintable(f.lastBody));
    QVERIFY2(!f.lastBody.contains(QStringLiteral("引用")), qPrintable(f.lastBody));
    QCOMPARE(Listing(f.project), QStringLiteral("System: B D | Combat: "));
    QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    f.project.triggerUndoStack_->undo();
    QCOMPARE(Listing(f.project), original);
    f.project.triggerUndoStack_->redo();
    QCOMPARE(Listing(f.project), QStringLiteral("System: B D | Combat: "));
    f.project.triggerUndoStack_->undo();
    QCOMPARE(Listing(f.project), original);

    // 只删 A：E 的引用列入确认。
    f.Select({"A", "D"});
    f.module->DeleteCurrentItem();
    QCOMPARE(f.confirmations, 2);
    QVERIFY2(f.lastBody.contains(QStringLiteral("1 处引用")), qPrintable(f.lastBody));
    QCOMPARE(Listing(f.project), QStringLiteral("System: B C | Combat: E"));
    f.project.triggerUndoStack_->undo();
    QCOMPARE(Listing(f.project), original);
}

void TriggerMultiSelectTests::DirectoryCopyPasteKeepsOrderAndCutMoves() {
    DirectoryFixture f;
    QVERIFY(f.Open());
    auto* stack = f.project.triggerUndoStack_;
    // 选择顺序与目录顺序无关：剪贴板按目录顺序。
    f.Select({"C", "A"});
    f.module->CopyCurrentTrigger();
    QVERIFY(f.module->Reveal({"triggers", "trigger", "E", {}}));
    f.module->PasteTrigger();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E A 副本 C 副本"));
    QCOMPARE(stack->count(), 1);
    QCOMPARE(f.SelectedNames(), (QStringList{"A 副本", "C 副本"}));
    QVERIFY(Named(f.project, "A 副本")->ecas.size() == 1);
    stack->undo();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));
    // 剪贴板在撤销后仍可再粘贴。
    f.module->PasteTrigger();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E A 副本 C 副本"));
    stack->undo();

    // 创建副本：每个副本放在所在类别最后一个选中项之后。
    f.Select({"A", "B", "E"});
    f.module->DuplicateCurrentTrigger();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B A 副本 B 副本 C D | Combat: E E 副本"));
    QCOMPARE(stack->index(), 1);
    stack->undo();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));

    // 剪切在粘贴时移动：粘贴到当前项之后，一步撤销。
    f.Select({"B", "C"});
    f.module->CutSelectedTriggers();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));
    QVERIFY(f.module->Reveal({"triggers", "trigger", "E", {}}));
    const int before = stack->index();
    f.module->PasteTrigger();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A D | Combat: E B C"));
    QCOMPARE(stack->index(), before + 1);
    QCOMPARE(f.SelectedNames(), (QStringList{"B", "C"}));
    stack->undo();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));
}

void TriggerMultiSelectTests::DirectoryCategoryCoversItsTriggers() {
    DirectoryFixture f;
    QVERIFY(f.Open());
    // 类别与其中的触发器同时选中：类别代表其中全部触发器，不再单列。
    SelectRows(f.view, {f.CategoryRow(1), f.Row("B")});
    QCOMPARE(f.SelectedNames(), (QStringList{"#1"}));
    QCOMPARE(f.module->SelectedDirectoryItems().size(), std::size_t{1});
    f.module->CopyCurrentTrigger();
    QVERIFY(f.module->Reveal({"triggers", "trigger", "E", {}}));
    f.module->PasteTrigger();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E A 副本 B 副本 C 副本 D 副本"));
    f.project.triggerUndoStack_->undo();
    // 类别和触发器混合时：不能一起拖动（拒绝并说明），文档不变。
    SelectRows(f.view, {f.CategoryRow(1), f.Row("E")});
    const auto items = f.module->SelectedDirectoryItems();
    QCOMPARE(items.size(), std::size_t{2});
    QVERIFY(!f.model->ResolveMultiDrop(items, -1, f.model->IndexForCategoryId(2)).has_value());
    TriggerDirectoryMove mixed{items.front(), 2, 0, std::nullopt, items};
    f.module->ApplyDirectoryMove(mixed);
    QCOMPARE(f.project.triggerUndoStack_->index(), 0);
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));
    // 删除类别与另一类别里的触发器：一次确认、一步撤销。
    f.module->DeleteCurrentItem();
    QCOMPARE(f.confirmations, 1);
    QVERIFY2(f.lastBody.contains(QStringLiteral("1 个类别，共 5 个触发器")), qPrintable(f.lastBody));
    QCOMPARE(Listing(f.project), QStringLiteral("Combat: "));
    f.project.triggerUndoStack_->undo();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));
}

void TriggerMultiSelectTests::DirectoryDragMovesSeveralInOrder() {
    DirectoryFixture f;
    QVERIFY(f.Open());
    f.Select({"C", "A"});
    // 拖动数据带全部选中项；放到 Combat 类别上 = 按目录顺序移到末尾。
    const std::unique_ptr<QMimeData> data(f.model->mimeData({f.model->IndexForTriggerId(IdOf(f.project, "C")),
        f.model->IndexForTriggerId(IdOf(f.project, "A"))}));
    QVERIFY(data != nullptr);
    QVERIFY(f.model->canDropMimeData(data.get(), Qt::MoveAction, -1, 0, f.model->IndexForCategoryId(2)));
    const auto items = f.module->SelectedDirectoryItems();
    auto move = f.model->ResolveMultiDrop(items, -1, f.model->IndexForCategoryId(2));
    QVERIFY(move && move->sources.size() == 2 && move->targetCategoryId == 2);
    f.module->ApplyDirectoryMove(*move);
    QCOMPARE(Listing(f.project), QStringLiteral("System: B D | Combat: E A C"));
    QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    QCOMPARE(f.SelectedNames(), (QStringList{"A", "C"}));
    f.project.triggerUndoStack_->undo();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));
    // 放在被拖动的项上：顺延到其后第一个不移动的项（A、C 放到 C 上 = 放在 D 之前）。
    f.Select({"A", "C"});
    move = f.model->ResolveMultiDrop(f.module->SelectedDirectoryItems(), -1, f.model->IndexForTriggerId(IdOf(f.project, "C")));
    QVERIFY(move);
    f.module->ApplyDirectoryMove(*move);
    QCOMPARE(Listing(f.project), QStringLiteral("System: B A C D | Combat: E"));
    f.project.triggerUndoStack_->undo();
    QCOMPARE(Listing(f.project), QStringLiteral("System: A B C D | Combat: E"));
}

void TriggerMultiSelectTests::DirectoryFlagsBatchAndSelectionSurvivesRefresh() {
    DirectoryFixture f;
    QVERIFY(f.Open());
    auto* stack = f.project.triggerUndoStack_;
    f.Select({"A", "C"});
    f.module->SetSelectedTriggerFlags(false, std::nullopt);
    QCOMPARE(Named(f.project, "A")->enabled, 0);
    QCOMPARE(Named(f.project, "C")->enabled, 0);
    QCOMPARE(Named(f.project, "B")->enabled, 1);
    QCOMPARE(stack->count(), 1);
    // 目录刷新后仍是同样的两项。
    QCOMPARE(f.SelectedNames(), (QStringList{"A", "C"}));
    QCOMPARE(f.view->selectionModel()->selectedRows(0).size(), qsizetype{2});
    f.module->SetSelectedTriggerFlags(std::nullopt, false);
    QCOMPARE(Named(f.project, "A")->initiallyClosed, 1);
    QCOMPARE(stack->count(), 2);
    stack->undo();
    stack->undo();
    QCOMPARE(Named(f.project, "A")->enabled, 1);
    QCOMPARE(Named(f.project, "C")->enabled, 1);
    QCOMPARE(Named(f.project, "A")->initiallyClosed, 0);
    QCOMPARE(f.SelectedNames(), (QStringList{"A", "C"}));
    QCOMPARE(f.view->selectionModel()->selectedRows(0).size(), qsizetype{2});
    // 单项操作照旧只选结果。
    QVERIFY(f.module->Reveal({"triggers", "trigger", "B", {}}));
    QCOMPARE(f.SelectedNames(), (QStringList{"B"}));
}

void TriggerMultiSelectTests::StatementDeleteAcrossParentsIsOneUndo() {
    LogicFixture f;
    QVERIFY(f.OpenLogic());
    auto* stack = f.project.triggerUndoStack_;
    const QString original = Tree(f.project);
    QCOMPARE(original, QStringLiteral("RawEvent Alpha Beta IfThenElseMultiple(0:RawCond 1:Inner1 1:Inner2) Gamma Delta"));
    f.SelectEca({"Beta", "Inner1"});
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"Beta", "Inner1"}));
    f.module->DeleteSelectedEca();
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Alpha IfThenElseMultiple(0:RawCond 1:Inner2) Gamma Delta"));
    QCOMPARE(stack->count(), 1);
    // 删除后停在下一条留下的兄弟上。
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"IfThenElseMultiple"}));
    stack->undo();
    QCOMPARE(Tree(f.project), original);
    stack->redo();
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Alpha IfThenElseMultiple(0:RawCond 1:Inner2) Gamma Delta"));
    stack->undo();
    QCOMPARE(Tree(f.project), original);
}

void TriggerMultiSelectTests::StatementCutPasteKeepsDocumentOrderAndRefusesWrongKinds() {
    LogicFixture f;
    QVERIFY(f.OpenLogic());
    auto* stack = f.project.triggerUndoStack_;
    const QString original = Tree(f.project);
    // 跨父级剪切：选择顺序与显示顺序无关，剪贴板按显示顺序。
    f.SelectEca({"Inner2", "Alpha"});
    f.module->CopySelectedEca(true);
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Beta IfThenElseMultiple(0:RawCond 1:Inner1) Gamma Delta"));
    QCOMPARE(stack->count(), 1);
    f.SelectEca({"Gamma"});
    f.module->PasteEca();
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Beta IfThenElseMultiple(0:RawCond 1:Inner1) Gamma Alpha Inner2 Delta"));
    QCOMPARE(stack->count(), 2);
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"Alpha", "Inner2"}));
    stack->undo();
    stack->undo();
    QCOMPARE(Tree(f.project), original);
    // 粘贴到块语句的分支里：同一分支，放在当前语句之后。
    f.SelectEca({"Gamma", "Delta"});
    f.module->CopySelectedEca(false);
    f.SelectEca({"Inner1"});
    f.module->PasteEca();
    QCOMPARE(Tree(f.project), QStringLiteral(
        "RawEvent Alpha Beta IfThenElseMultiple(0:RawCond 1:Inner1 1:Gamma 1:Delta 1:Inner2) Gamma Delta"));
    stack->undo();
    QCOMPARE(Tree(f.project), original);
    // 事件不能粘贴到“则”分支：整体拒绝，文档与撤销栈不变。
    const int count = stack->count();
    f.SelectEca({"RawEvent", "Alpha"});
    f.module->CopySelectedEca(false);
    f.SelectEca({"Inner1"});
    f.module->PasteEca();
    QCOMPARE(Tree(f.project), original);
    QCOMPARE(stack->count(), count);
}

void TriggerMultiSelectTests::StatementAncestorCoversChildAndHeadersIgnored() {
    LogicFixture f;
    QVERIFY(f.OpenLogic());
    auto* stack = f.project.triggerUndoStack_;
    const QString original = Tree(f.project);
    // 块语句与它的子语句同时选中：只算块语句。
    f.SelectEca({"Inner1", "IfThenElseMultiple"});
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"IfThenElseMultiple"}));
    // 分组头 / 分支头不算语句。
    const QModelIndex actionsHeader = f.ecaModel->index(2, 0);
    const QModelIndex thenHeader = f.ecaModel->index(1, 0, f.EcaRow("IfThenElseMultiple"));
    SelectRows(f.logic, {actionsHeader, f.EcaRow("Alpha"), thenHeader});
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"Alpha"}));
    SelectRows(f.logic, {f.ecaModel->index(0, 0), actionsHeader});
    QVERIFY(f.module->SelectedEcaIds().empty());
    f.module->DeleteSelectedEca();
    QCOMPARE(Tree(f.project), original);
    QCOMPARE(stack->count(), 0);
    // 删除块语句（含选中的子语句）：一步撤销。
    f.SelectEca({"IfThenElseMultiple", "Inner2", "Delta"});
    f.module->DeleteSelectedEca();
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Alpha Beta Gamma"));
    QCOMPARE(stack->count(), 1);
    stack->undo();
    QCOMPARE(Tree(f.project), original);
}

void TriggerMultiSelectTests::StatementDuplicateEnableAndSelectionSurvivesRefresh() {
    LogicFixture f;
    QVERIFY(f.OpenLogic());
    auto* stack = f.project.triggerUndoStack_;
    const QString original = Tree(f.project);
    // 禁用两条：一步撤销；刷新与撤销后选区不变。
    f.SelectEca({"Alpha", "Gamma"});
    f.module->SetSelectedEcaEnabled(false);
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Alpha- Beta IfThenElseMultiple(0:RawCond 1:Inner1 1:Inner2) Gamma- Delta"));
    QCOMPARE(stack->count(), 1);
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"Alpha", "Gamma"}));
    QCOMPARE(f.SelectedRowCount(), 2);
    stack->undo();
    QCOMPARE(Tree(f.project), original);
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"Alpha", "Gamma"}));
    QCOMPARE(f.SelectedRowCount(), 2);
    stack->redo();
    QCOMPARE(f.SelectedRowCount(), 2);
    stack->undo();
    // 创建副本：每段之后各一份，一步撤销，选中副本。
    f.SelectEca({"Alpha", "Beta", "Inner2"});
    f.module->DuplicateSelectedEca();
    QCOMPARE(Tree(f.project), QStringLiteral(
        "RawEvent Alpha Beta Alpha Beta IfThenElseMultiple(0:RawCond 1:Inner1 1:Inner2 1:Inner2) Gamma Delta"));
    QCOMPARE(stack->index(), 1);
    QCOMPARE(f.module->SelectedEcaIds().size(), std::size_t{3});
    stack->undo();
    QCOMPARE(Tree(f.project), original);
    stack->redo();
    QCOMPARE(Tree(f.project), QStringLiteral(
        "RawEvent Alpha Beta Alpha Beta IfThenElseMultiple(0:RawCond 1:Inner1 1:Inner2 1:Inner2) Gamma Delta"));
    stack->undo();
    QCOMPARE(Tree(f.project), original);
}

void TriggerMultiSelectTests::StatementBlockMoveAndDrag() {
    LogicFixture f;
    QVERIFY(f.OpenLogic());
    auto* stack = f.project.triggerUndoStack_;
    const QString original = Tree(f.project);
    // Alt+↓：相邻的一段整体下移（下一条移到这段之前），选区不变。
    f.SelectEca({"Alpha", "Beta"});
    f.module->MoveSelectedEca(1);
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent IfThenElseMultiple(0:RawCond 1:Inner1 1:Inner2) Alpha Beta Gamma Delta"));
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"Alpha", "Beta"}));
    f.module->MoveSelectedEca(-1);
    QCOMPARE(Tree(f.project), original);
    QCOMPARE(stack->count(), 2);
    // 不相邻：拒绝，不入栈。
    f.SelectEca({"Alpha", "Gamma"});
    f.module->MoveSelectedEca(1);
    QCOMPARE(Tree(f.project), original);
    QCOMPARE(stack->count(), 2);

    // 拖放多条：放在 Delta 上 = 插在它之前，保持显示顺序。
    const auto request = f.ecaModel->ResolveMultiDrop({Eca(f.project, "Gamma"), Eca(f.project, "Alpha")}, -1, f.EcaRow("Delta"));
    QVERIFY(request && request->ecaIds.size() == 2);
    f.module->ApplyEcaMove(*request);
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Beta IfThenElseMultiple(0:RawCond 1:Inner1 1:Inner2) Alpha Gamma Delta"));
    QCOMPARE(stack->index(), 3);
    QCOMPARE(f.SelectedEcaNames(), (QStringList{"Alpha", "Gamma"}));
    stack->undo();
    QCOMPARE(Tree(f.project), original);
    // 拖进“则”分支末尾；块语句不能和它自己的子语句一起拖进自己的分支。
    const QModelIndex thenHeader = f.ecaModel->index(1, 0, f.EcaRow("IfThenElseMultiple"));
    const auto into = f.ecaModel->ResolveMultiDrop({Eca(f.project, "Alpha"), Eca(f.project, "Delta")}, -1, thenHeader);
    QVERIFY(into && into->slot.childId == std::optional<std::int32_t>(1));
    f.module->ApplyEcaMove(*into);
    QCOMPARE(Tree(f.project), QStringLiteral("RawEvent Beta IfThenElseMultiple(0:RawCond 1:Inner1 1:Inner2 1:Alpha 1:Delta) Gamma"));
    stack->undo();
    QCOMPARE(Tree(f.project), original);
    QVERIFY(!f.ecaModel->ResolveMultiDrop({Eca(f.project, "IfThenElseMultiple"), Eca(f.project, "Alpha")}, -1,
        f.ecaModel->index(1, 0, f.EcaRow("IfThenElseMultiple"))).has_value());
    // 事件与动作一起不能放进动作位置。
    QVERIFY(!f.ecaModel->ResolveMultiDrop({Eca(f.project, "RawEvent"), Eca(f.project, "Alpha")}, -1, f.EcaRow("Delta")).has_value());
    // 拖动数据带全部选中语句。
    const std::unique_ptr<QMimeData> data(f.ecaModel->mimeData({f.EcaRow("Alpha"), f.EcaRow("Gamma"), f.ecaModel->index(2, 0)}));
    QVERIFY(data != nullptr);
    QVERIFY(f.ecaModel->canDropMimeData(data.get(), Qt::MoveAction, -1, 0, f.EcaRow("Delta")));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    // 设置写进本次运行的临时 INI：低完整性进程写不了注册表 HKCU\Software。
    QTemporaryDir testSettings;
    if (!testSettings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testSettings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, testSettings.path());
    QApplication application(argc, argv);
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    TriggerMultiSelectTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "TriggerMultiSelectTests.moc"
