#include "Icons/IconProvider.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "ObjectModels.h"
#include "ObjectNames.h"
#include "TriggerCommands.h"
#include "TriggerModels.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QColor>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QStringList>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>

namespace {

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result(text.size());
    std::transform(text.begin(), text.end(), result.begin(), [](unsigned char character) {
        return static_cast<std::byte>(character);
    });
    return result;
}

void WriteFixture(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

class QtModelTests final : public QObject {
    Q_OBJECT

private slots:
    void ObjectAndFieldModelsExposeEditableLni();
    void TriggerModelsExposeNaturalLanguageTree();
    void EcaTypesReuseOnlyAcceptedCatalogIdentity();
    void EcaTypesRevalidateAfterResetCallback();
    void EcaTypesNeverCacheDocumentQualification();
    void IconProviderMapsGameFunctionsToCategories();
    void IconProviderMergesCatalogCategories();
    void IconProviderMergesExternalCatalog();
    void IconProbeChainOnExternalData();
};

void QtModelTests::ObjectAndFieldModelsExposeEditableLni() {
    using namespace War3Map::ObjectData;

    const std::string fixture =
        "[hfoo]\n"
        "_parent = \"hfoo\"\n"
        "Name = \"步兵\"\n"
        "HP = 420\n"
        "Ubertip = {\n"
        "\"只读多行值\",\n"
        "}\n";
    LniObjectCodec codec(ObjectType::Unit);
    auto parsed = codec.Parse(Bytes(fixture));
    QVERIFY(parsed.Succeeded());
    auto* raw = dynamic_cast<LniObjectDocument*>(parsed.document.release());
    QVERIFY(raw != nullptr);
    std::unique_ptr<LniObjectDocument> document(raw);

    ObjectListModel listModel;
    listModel.SetDocument(document.get());
    // 独立模型不自行猜名称；与生产 ObjectModule 一样显式注入共用 N1 工程适配器。
    listModel.SetPresentationResolver([&document](const ObjectRecord& record) {
        const auto shown = ResolveObjectPresentation(record.key, document.get(), nullptr);
        return std::make_pair(shown.name, QIcon{});
    });
    QCOMPARE(listModel.rowCount(), 1);
    QCOMPARE(listModel.columnCount(), 4);
    QCOMPARE(listModel.index(0, 0).data().toString(), QStringLiteral("hfoo"));
    QCOMPARE(listModel.index(0, 1).data().toString(), QStringLiteral("步兵"));

    ObjectFieldModel fieldModel;
    const ObjectKey object{ObjectType::Unit, "hfoo"};
    fieldModel.SetDocument(document.get(), &object);
    QCOMPARE(fieldModel.rowCount(), 3);

    bool edited = false;
    fieldModel.SetEditCallback([&edited]() { edited = true; });
    const QModelIndex nameValue = fieldModel.index(0, 3);
    QVERIFY(nameValue.flags().testFlag(Qt::ItemIsEditable));
    QVERIFY(fieldModel.setData(nameValue, QStringLiteral("精锐步兵"), Qt::EditRole));
    QVERIFY(edited);
    QCOMPARE(nameValue.data().toString(), QStringLiteral("精锐步兵"));
    listModel.RefreshPresentation();
    QCOMPARE(listModel.index(0, 1).data().toString(), QStringLiteral("精锐步兵"));

    const QModelIndex opaqueValue = fieldModel.index(2, 3);
    QVERIFY(!opaqueValue.flags().testFlag(Qt::ItemIsEditable));
    QVERIFY(!fieldModel.setData(opaqueValue, QStringLiteral("禁止改写"), Qt::EditRole));

    const auto serialized = codec.Serialize(*document);
    QVERIFY(serialized.Succeeded());
    const std::string output(
        reinterpret_cast<const char*>(serialized.value->data()),
        serialized.value->size());
    QVERIFY(output.find("Name = \"精锐步兵\"") != std::string::npos);
    QVERIFY(output.find("\"只读多行值\"") != std::string::npos);
}

void QtModelTests::TriggerModelsExposeNaturalLanguageTree() {
    using namespace War3Map::Trigger;

    const std::filesystem::path uiRoot =
        std::filesystem::current_path() / "qt-trigger-ui-fixture";
    std::filesystem::remove_all(uiRoot);
    WriteFixture(uiRoot / "ui" / "config", "base\n");
    WriteFixture(
        uiRoot / "ui" / "base" / "event.txt",
        "[RawThirdPartyEvent]\n"
        "title=\"测试事件\"\n"
        "description=\"当 ${值} 发生\"\n"
        "[[.args]]\n"
        "type=string\n");
    WriteFixture(
        uiRoot / "ui" / "base" / "action.txt",
        "[ProtectedAction]\n"
        "title=\"受保护动作\"\n"
        "description=\"受保护动作\"\n"
        "[CommentsString]\n"
        "title=\"注释\"\n"
        "description=\"注释：${文本}\"\n"
        "[[.args]]\n"
        "type=string\n"
        "[PlainAction]\n"
        "title=\"普通动作\"\n"
        "description=\"普通动作\"\n"
        "[IfThenElseMultiple]\n"
        "title=\"如果/则/否则\"\n"
        "description=\"如果 (所有条件成立) 则 (动作) 否则 (动作)\"\n"
        "[CreateUnitAtLoc]\n"
        "title=\"创建单位\"\n"
        "description=\"在 ${点} 创建 ${单位}\"\n"
        "[[.args]]\n"
        "type=location\n"
        "[[.args]]\n"
        "type=unitcode\n");
    WriteFixture(
        uiRoot / "ui" / "base" / "condition.txt",
        "[RawCond]\n"
        "title=\"测试条件\"\n"
        "description=\"测试条件成立\"\n");
    auto loadedCatalog = UiCatalog::Load(uiRoot);
    QVERIFY(loadedCatalog.Succeeded());

    TriggerDocument document;
    CategoryRecord category;
    category.id = 101;
    category.categoryId = 7;
    category.name.text = "战斗系统";
    document.categories.push_back(std::move(category));

    TriggerRecord trigger;
    trigger.id = 201;
    trigger.categoryId = 7;
    trigger.name.text = "第三方函数检查";
    trigger.ecas.push_back(0);
    trigger.ecas.push_back(2);
    document.triggers.push_back(std::move(trigger));

    EcaNode root;
    root.id = 301;
    root.sourceTriggerId = 201;
    root.type = 0;
    root.name.text = "RawThirdPartyEvent";
    root.arguments.push_back(0);
    root.children.push_back(1);
    root.uiLayer = "optional/library";
    root.uiSource = "ui/optional/library/event.txt";
    document.ecaNodes.push_back(std::move(root));

    EcaNode child;
    child.id = 302;
    child.sourceTriggerId = 201;
    child.type = 2;
    child.childId = 0;
    child.name.text = "PlainAction";
    document.ecaNodes.push_back(std::move(child));

    // If/Then/Else 多分支函数体：分支 0=两个条件、分支 1=Then 动作、分支 2=Else 动作。
    EcaNode ifAction;
    ifAction.id = 303;
    ifAction.sourceTriggerId = 201;
    ifAction.type = 2;
    ifAction.name.text = "IfThenElseMultiple";
    ifAction.children = {3, 4, 5, 6};
    document.ecaNodes.push_back(std::move(ifAction));

    EcaNode condA;
    condA.id = 304;
    condA.sourceTriggerId = 201;
    condA.type = 1;
    condA.childId = 0;
    condA.name.text = "RawCond";
    document.ecaNodes.push_back(condA);

    EcaNode condB = condA;
    condB.id = 305;
    document.ecaNodes.push_back(std::move(condB));

    EcaNode thenAction;
    thenAction.id = 306;
    thenAction.sourceTriggerId = 201;
    thenAction.type = 2;
    thenAction.childId = 1;
    thenAction.name.text = "PlainAction";
    document.ecaNodes.push_back(thenAction);

    EcaNode elseAction = thenAction;
    elseAction.id = 307;
    elseAction.childId = 2;
    document.ecaNodes.push_back(std::move(elseAction));

    ArgumentNode argument;
    argument.id = 401;
    argument.value.text = "原始参数";
    document.argumentNodes.push_back(std::move(argument));

    TriggerExplorerModel explorer;
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(), 1);
    const QModelIndex categoryIndex = explorer.index(0, 0);
    QCOMPARE(categoryIndex.data().toString(), QStringLiteral("战斗系统"));
    QCOMPARE(explorer.rowCount(categoryIndex), 1);
    const QModelIndex triggerIndex = explorer.index(0, 0, categoryIndex);
    QCOMPARE(triggerIndex.data().toString(), QStringLiteral("第三方函数检查"));
    QVERIFY(explorer.TriggerAt(triggerIndex) != nullptr);
    QCOMPARE(explorer.TriggerAt(triggerIndex)->id, War3Map::StableId{201});

    TriggerEcaModel ecas;
    ecas.SetDocument(&document, &*loadedCatalog.value);
    ecas.SetTrigger(201);
    // 顶层为事件/条件/动作 三个分组头。
    QCOMPARE(ecas.rowCount(), 3);
    const QModelIndex eventsGroup = ecas.index(0, 0);
    QCOMPARE(eventsGroup.data().toString(), QStringLiteral("事件 (1)"));
    QCOMPARE(ecas.rowCount(eventsGroup), 1);        // 事件分组含 1 个顶层事件
    QCOMPARE(ecas.rowCount(ecas.index(1, 0)), 0);   // 条件分组为空

    const QModelIndex rootIndex = ecas.index(0, 0, eventsGroup);
    QCOMPARE(rootIndex.data().toString(), QStringLiteral("当 原始参数 发生"));
    const QModelIndex eventNode = ecas.index(0, 0, eventsGroup);
    QCOMPARE(ecas.rowCount(eventNode), 1);
    const QModelIndex bodyHeader = ecas.index(0, 0, eventNode);
    QCOMPARE(bodyHeader.data().toString(), QStringLiteral("动作 (1)"));
    const QModelIndex childIndex = ecas.index(0, 0, bodyHeader);
    QCOMPARE(childIndex.data().toString(), QStringLiteral("普通动作"));
    QCOMPARE(ecas.parent(childIndex), bodyHeader);  // 子节点父级为角色分组头
    QCOMPARE(ecas.parent(bodyHeader), eventNode);   // 角色分组头父级为函数体节点
    QCOMPARE(ecas.parent(eventNode), eventsGroup);  // 顶层节点父级为分组头
    QVERIFY(ecas.ArgumentAt(rootIndex, 0) != nullptr);
    QCOMPARE(
        QString::fromUtf8(ecas.ArgumentAt(rootIndex, 0)->value.text),
        QStringLiteral("原始参数"));
    QVERIFY(rootIndex.data(Qt::ToolTipRole).toString().contains(
        QStringLiteral("RawThirdPartyEvent")));

    // 被 Core 拒绝的编辑（目标节点不存在）不得在 undo stack 留下脏命令。
    QUndoStack rejectedStack;
    rejectedStack.setClean();
    rejectedStack.push(new SetEcaEnabledUndoCommand(
        &document, 999, true, false, QStringLiteral("编辑不存在的节点")));
    QCOMPARE(rejectedStack.count(), 0);
    QVERIFY(rejectedStack.isClean());
    QCOMPARE(document.ecaNodes[1].enabled, std::int32_t{1});

    // U7-P1：If/Then/Else 函数体按 childId 分为三个分支头，标签与原版一致（如果 - 条件 / 则 - 动作 / 否则 - 动作）。
    const QModelIndex actionsGroup = ecas.index(2, 0);
    QCOMPARE(ecas.rowCount(actionsGroup), 1); // 追加 CommentsString 前只有 IfThenElseMultiple
    const QModelIndex ifNode = ecas.index(0, 0, actionsGroup);
    QCOMPARE(ecas.rowCount(ifNode), 3);
    const QModelIndex condHeader = ecas.index(0, 0, ifNode);
    const QModelIndex thenHeader = ecas.index(1, 0, ifNode);
    const QModelIndex elseHeader = ecas.index(2, 0, ifNode);
    QCOMPARE(condHeader.data().toString(), QStringLiteral("如果 - 条件 (2)"));
    QCOMPARE(thenHeader.data().toString(), QStringLiteral("则 - 动作 (1)"));
    QCOMPARE(elseHeader.data().toString(), QStringLiteral("否则 - 动作 (1)"));
    QCOMPARE(ecas.rowCount(condHeader), 2);
    QCOMPARE(ecas.rowCount(thenHeader), 1);
    QCOMPARE(ecas.rowCount(elseHeader), 1);
    const QModelIndex condIndex = ecas.index(0, 0, condHeader);
    QCOMPARE(condIndex.data().toString(), QStringLiteral("测试条件成立"));
    QCOMPARE(ecas.parent(condIndex), condHeader);
    QCOMPARE(ecas.parent(condHeader), ifNode);
    // 嵌套节点稳定 ID 反查：行号为角色分组内位置，父链经角色分组头。
    const QModelIndex thenByStableId = ecas.IndexForStableId(306, 0);
    QVERIFY(thenByStableId.isValid());
    QCOMPARE(thenByStableId.data().toString(), QStringLiteral("普通动作"));
    QCOMPARE(ecas.parent(thenByStableId), thenHeader);

    auto appended = TriggerDocumentEditor::AppendRootEca(
        document,
        *loadedCatalog.value,
        201,
        UiDefinitionKind::Action,
        "CommentsString");
    QVERIFY(appended.Succeeded());
    const War3Map::StableId appendedEcaId = appended.value->rootEcaId;
    const War3Map::StableId appendedArgumentId =
        appended.value->appendedArguments.front().id;

    QUndoStack undoStack;
    undoStack.push(new AppendEcaUndoCommand(
        &document,
        std::move(*appended.value),
        QStringLiteral("新增注释动作")));
    ecas.SetTrigger(201);
    // 新增的是动作，归入“动作”分组（index(2,0)），与 IfThenElseMultiple 并列。
    QCOMPARE(ecas.rowCount(), 3);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 2);
    QVERIFY(ecas.IndexForStableId(appendedEcaId, 0).isValid());

    undoStack.undo();
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 1); // 撤销后只剩 IfThenElseMultiple
    undoStack.redo();
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 2); // 重做后恢复

    // 删除节点：Core RemoveEcaSubtree（压实无孤儿）→ 撤销/重做经 RemoveEcaUndoCommand。
    auto removal = War3Map::Trigger::TriggerDocumentEditor::RemoveEcaSubtree(
        document, appendedEcaId);
    QVERIFY(removal.Succeeded());
    QCOMPARE(removal.value->parentTriggerId, War3Map::StableId{201});
    undoStack.push(new RemoveEcaUndoCommand(
        &document, std::move(*removal.value), QStringLiteral("删除注释动作")));
    // 压实后文档必须无孤儿：删除后模型只从挂载点取行号，孤儿即不可见。
    // 可序列化证明由 trigger-codec 合成用例与真实图 edit-roundtrip 覆盖。
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 1); // 删除后只剩 IfThenElseMultiple

    undoStack.undo();
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 2); // 撤销删除后恢复
    QVERIFY(ecas.IndexForStableId(appendedEcaId, 0).isValid());
    undoStack.redo();
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 1); // 重做删除后再次移除
    undoStack.undo(); // 还原现场，供后续参数编辑断言使用
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 2);

    auto* editableArgument = TriggerDocumentEditor::FindArgument(
        document,
        appendedArgumentId);
    QVERIFY(editableArgument != nullptr);
    const ArgumentNode before = *editableArgument;
    QVERIFY(TriggerDocumentEditor::SetScalarArgument(
        document,
        appendedArgumentId,
        3,
        "新的自然语言注释").Succeeded());
    const ArgumentNode after = *editableArgument;
    undoStack.push(new EditTriggerArgumentsUndoCommand(
        &document,
        {{appendedArgumentId, before, after}},
        QStringLiteral("编辑注释参数")));
    QCOMPARE(QString::fromUtf8(editableArgument->value.text),
        QStringLiteral("新的自然语言注释"));

    undoStack.undo();
    QCOMPARE(QString::fromUtf8(editableArgument->value.text),
        QString::fromUtf8(before.value.text));
    undoStack.redo();
    QCOMPARE(QString::fromUtf8(editableArgument->value.text),
        QStringLiteral("新的自然语言注释"));

    // 触发器管理（N2a）：新建/重命名/删除经撤销栈，Explorer 行数同步。
    auto addedTrigger = War3Map::Trigger::TriggerDocumentEditor::AddTrigger(
        document, 7, "管理测试触发器 N2a");
    QVERIFY(addedTrigger.Succeeded());
    undoStack.push(new AddTriggerUndoCommand(
        &document, *addedTrigger.value, QStringLiteral("新建触发器")));
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 2);
    undoStack.undo();
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 1);
    undoStack.redo();
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 2);
    QVERIFY(explorer.IndexForTriggerId(*addedTrigger.value).isValid());

    QVERIFY(War3Map::Trigger::TriggerDocumentEditor::RenameTrigger(
        document, *addedTrigger.value, "改名后 N2a").Succeeded());
    undoStack.push(new RenameTriggerUndoCommand(
        &document, *addedTrigger.value, "管理测试触发器 N2a", "改名后 N2a",
        QStringLiteral("重命名触发器")));
    const auto triggerName = [&]() {
        return QString::fromUtf8(
            War3Map::Trigger::TriggerDocumentEditor::FindTrigger(
                document, *addedTrigger.value)->name.text);
    };
    QCOMPARE(triggerName(), QStringLiteral("改名后 N2a"));
    undoStack.undo();
    QCOMPARE(triggerName(), QStringLiteral("管理测试触发器 N2a"));
    undoStack.redo();
    QCOMPARE(triggerName(), QStringLiteral("改名后 N2a"));

    auto removedTrigger = War3Map::Trigger::TriggerDocumentEditor::RemoveTrigger(
        document, *addedTrigger.value);
    QVERIFY(removedTrigger.Succeeded());
    undoStack.push(new RemoveTriggerUndoCommand(
        &document, std::move(*removedTrigger.value), QStringLiteral("删除触发器")));
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 1);
    undoStack.undo();
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 2);
    undoStack.redo();
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 1);
    undoStack.undo(); // 还原现场
    explorer.SetDocument(&document);
    QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 2);

    // 编辑后的增量刷新：结构不变时不重置，只通知真正变化的行；结构变化时退回重置。
    {
        QSignalSpy resets(&explorer, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&explorer, &QAbstractItemModel::dataChanged);
        QVERIFY(!explorer.RefreshDocument(&document));
        QCOMPARE(resets.count(), 0); QCOMPARE(changes.count(), 0);
        auto* renamed = War3Map::Trigger::TriggerDocumentEditor::FindTrigger(document, *addedTrigger.value);
        renamed->name.text = "增量刷新改名";
        QVERIFY(!explorer.RefreshDocument(&document));
        QCOMPARE(resets.count(), 0); QCOMPARE(changes.count(), 1);
        const auto changedIndex = changes.front().at(0).value<QModelIndex>();
        QCOMPARE(changedIndex, explorer.IndexForTriggerId(*addedTrigger.value));
        QCOMPARE(changedIndex.data(Qt::DisplayRole).toString(), QStringLiteral("增量刷新改名"));
        renamed->name.text = "改名后 N2a";
        QVERIFY(!explorer.RefreshDocument(&document)); QCOMPARE(changes.count(), 2);
        undoStack.redo(); // 删除：结构变化，必须重置
        QVERIFY(explorer.RefreshDocument(&document));
        QCOMPARE(resets.count(), 1); QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 1);
        undoStack.undo();
        QVERIFY(explorer.RefreshDocument(&document)); QCOMPARE(explorer.rowCount(explorer.index(0, 0)), 2);
        explorer.SetDocument(&document); QCOMPARE(resets.count(), 3); // 完整重绑总是重置
    }

    // 物体引用参数名称解析（N6c）：unitcode 四字码显示为物体名称。
    auto createdUnitEca = War3Map::Trigger::TriggerDocumentEditor::AppendRootEca(
        document, *loadedCatalog.value, 201, UiDefinitionKind::Action, "CreateUnitAtLoc");
    QVERIFY(createdUnitEca.Succeeded());
    const War3Map::StableId unitArgumentId =
        createdUnitEca.value->appendedArguments.back().id;
    QVERIFY(War3Map::Trigger::TriggerDocumentEditor::SetScalarArgument(
        document, unitArgumentId, 3, "hfoo").Succeeded());
    ecas.SetObjectNameResolver([](std::string_view rawcode) -> std::string {
        return rawcode == "hfoo" ? "步兵" : std::string{};
    });
    ecas.SetTrigger(201);
    const QModelIndex unitEcaIndex =
        ecas.IndexForStableId(createdUnitEca.value->rootEcaId, 0);
    QVERIFY(unitEcaIndex.isValid());
    QVERIFY(unitEcaIndex.data().toString().contains(QStringLiteral("步兵")));
    QVERIFY(!unitEcaIndex.data().toString().contains(QStringLiteral("hfoo")));
    ecas.SetObjectNameResolver(nullptr);

    // 复制粘贴（N6b）：克隆 IfThenElseMultiple 子树到触发器根，撤销/重做。
    auto pasteSnapshot = War3Map::Trigger::TriggerDocumentEditor::CopyEcaSubtree(
        document, 303);
    QVERIFY(pasteSnapshot.Succeeded());
    const std::size_t pasteRow = document.triggers.front().ecas.size();
    auto pasted = War3Map::Trigger::TriggerDocumentEditor::PasteEcaSubtree(
        document, *loadedCatalog.value, *pasteSnapshot.value, 201, 0, pasteRow);
    const QString pasteError = pasted.diagnostics.empty()
        ? QStringLiteral("未知粘贴错误")
        : QString::fromUtf8(pasted.diagnostics.front().message);
    QVERIFY2(pasted.Succeeded(), qPrintable(pasteError));
    undoStack.push(new PasteEcaUndoCommand(
        &document, &*loadedCatalog.value, *pasteSnapshot.value,
        201, 0, pasteRow, std::nullopt,
        pasted.value->rootEcaId, QStringLiteral("粘贴节点")));
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 4); // 原 3 个动作 + 粘贴体
    undoStack.undo();
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 3);
    undoStack.redo();
    ecas.SetTrigger(201);
    QCOMPARE(ecas.rowCount(ecas.index(2, 0)), 4);

    // 变量管理（N2b）：新建/重命名/删除经撤销栈。
    QVERIFY(War3Map::Trigger::TriggerDocumentEditor::AddVariable(
        document, "测试变量", "integer", false).Succeeded());
    undoStack.push(new AddVariableUndoCommand(
        &document, "测试变量", "integer", false, QStringLiteral("新建变量")));
    QCOMPARE(document.variables.size(), std::size_t{1});
    undoStack.undo();
    QCOMPARE(document.variables.size(), std::size_t{0});
    undoStack.redo();
    QCOMPARE(document.variables.size(), std::size_t{1});

    QVERIFY(War3Map::Trigger::TriggerDocumentEditor::RenameVariable(
        document, "测试变量", "改名变量").Succeeded());
    undoStack.push(new RenameVariableUndoCommand(
        &document, "测试变量", "改名变量", QStringLiteral("重命名变量")));
    QCOMPARE(document.variables.front().name.text, std::string("改名变量"));
    undoStack.undo();
    QCOMPARE(document.variables.front().name.text, std::string("测试变量"));
    undoStack.redo();
    QCOMPARE(document.variables.front().name.text, std::string("改名变量"));

    const War3Map::Trigger::VariableRecord variableRecord = document.variables.front();
    QVERIFY(War3Map::Trigger::TriggerDocumentEditor::RemoveVariable(
        document, "改名变量").Succeeded());
    undoStack.push(new RemoveVariableUndoCommand(
        &document, variableRecord, 0, QStringLiteral("删除变量")));
    QCOMPARE(document.variables.size(), std::size_t{0});
    undoStack.undo();
    QCOMPARE(document.variables.size(), std::size_t{1});
    QCOMPARE(document.variables.front().name.text, std::string("改名变量"));
    undoStack.redo();
    QCOMPARE(document.variables.size(), std::size_t{0});
    std::filesystem::remove_all(uiRoot);
}

// 图标服务：从真实游戏 MPQ 的 UI\TriggerData.txt 解析分类与函数分类映射。
// 仅在 WAR3_GAME_ROOT 指向有效安装目录时运行；不渲染位图（guiless 安全）。
void QtModelTests::IconProviderMapsGameFunctionsToCategories() {
    if (!qEnvironmentVariableIsSet("WAR3_GAME_ROOT")) {
        QSKIP("WAR3_GAME_ROOT 未设置：跳过真实游戏图标映射验证。");
    }
    const std::filesystem::path root(
        qEnvironmentVariable("WAR3_GAME_ROOT").toStdWString());
    War3Map::QtApp::IconProvider provider;
    QVERIFY(provider.SetGameRoot(root));
    QVERIFY(provider.IsReady());
    // TriggerData.txt 实测：54 个分类、>1000 条函数分类映射。
    QVERIFY(provider.CategoryCount() > 10);
    QVERIFY(provider.FunctionCategoryCount() > 100);
}

// 可选 UI 库的函数分类可以只存在定义的 category 字段中：
// IconProvider 应从当前 UiCatalog 动态合并，并在切库时清理旧映射。
// 纯哈希断言，不渲染位图（guiless 安全）。
void QtModelTests::IconProviderMergesCatalogCategories() {
    using namespace War3Map::Trigger;

    const std::filesystem::path uiRoot =
        std::filesystem::current_path() / "qt-icon-catalog-fixture";
    std::filesystem::remove_all(uiRoot);
    WriteFixture(uiRoot / "ui" / "config", "base\n");
    WriteFixture(
        uiRoot / "ui" / "base" / "action.txt",
        "[SyntheticCategorizedAction]\n"
        "title=\"合成分类动作\"\n"
        "description=\"合成分类动作\"\n"
        "category = TC_SYNTHETIC\n"
        "[SyntheticUncategorizedAction]\n"
        "title=\"无分类动作\"\n"
        "description=\"无分类动作\"\n");
    WriteFixture(
        uiRoot / "ui" / "base" / "define.txt",
        "[TriggerCategories]\n"
        "TC_SYNTHETIC=测试分类,ReplaceableTextures\\CommandButtons\\BTNTest.blp\n");
    auto loadedCatalog = UiCatalog::Load(uiRoot);
    QVERIFY(loadedCatalog.Succeeded());

    War3Map::QtApp::IconProvider provider;
    provider.MergeCatalogCategories(*loadedCatalog.value);
    QCOMPARE(
        provider.CategoryForFunction(QStringLiteral("SyntheticCategorizedAction")),
        QStringLiteral("TC_SYNTHETIC"));
    QVERIFY(provider.CategoryForFunction(QStringLiteral("SyntheticUncategorizedAction")).isEmpty());
    QVERIFY(provider.CategoryForFunction(QStringLiteral("MissingSyntheticAction")).isEmpty());
    QCOMPARE(provider.FunctionCategoryCount(), 1);
    // 分类纹理同样取自 Catalog 中的 define.txt，不另读文件。
    QCOMPARE(provider.TextureForCategory(QStringLiteral("TC_SYNTHETIC")),
        QStringLiteral("ReplaceableTextures\\CommandButtons\\BTNTest.blp"));
    provider.ResetUiMappings();
    QVERIFY(provider.CategoryForFunction(QStringLiteral("SyntheticCategorizedAction")).isEmpty());
    QVERIFY(provider.TextureForCategory(QStringLiteral("TC_SYNTHETIC")).isEmpty());
    QCOMPARE(provider.FunctionCategoryCount(), 0);
    QCOMPARE(provider.CategoryCount(), 0);
    std::filesystem::remove_all(uiRoot);
}

// 真实 UI 数据只能由本地环境显式提供；公开 CI 不依赖任何
// Warcraft/YDWE/地图资源。断言从 Catalog 动态选取定义，不绑定库名或函数名。
void QtModelTests::IconProviderMergesExternalCatalog() {
    if (!qEnvironmentVariableIsSet("WAR3_UI_DATA_ROOT")) {
        QSKIP("WAR3_UI_DATA_ROOT 未设置：跳过真实 UI Catalog 验证。");
    }
    const std::filesystem::path dataRoot(
        qEnvironmentVariable("WAR3_UI_DATA_ROOT").toStdWString());
    auto loadedCatalog = War3Map::Trigger::UiCatalog::Load(dataRoot);
    QVERIFY(loadedCatalog.Succeeded());

    War3Map::QtApp::IconProvider provider;
    provider.MergeCatalogCategories(*loadedCatalog.value);

    bool foundFunctionCategory = false;
    constexpr War3Map::Trigger::UiDefinitionKind kinds[] = {
        War3Map::Trigger::UiDefinitionKind::Event,
        War3Map::Trigger::UiDefinitionKind::Condition,
        War3Map::Trigger::UiDefinitionKind::Action,
        War3Map::Trigger::UiDefinitionKind::Call,
    };
    for (const auto kind : kinds) {
        for (const auto* definition : loadedCatalog.value->Definitions(kind)) {
            const auto category = std::find_if(
                definition->Active().fields.begin(),
                definition->Active().fields.end(),
                [](const auto& field) {
                    return field.first == "category" && !field.second.empty();
                });
            if (category == definition->Active().fields.end()) {
                continue;
            }
            QCOMPARE(
                provider.CategoryForFunction(QString::fromStdString(definition->name)),
                QString::fromStdString(War3Map::Trigger::TriggerCategoryTable::CategoryOf(*definition)));
            foundFunctionCategory = true;
            break;
        }
        if (foundFunctionCategory) {
            break;
        }
    }
    QVERIFY2(foundFunctionCategory, "外部 Catalog 中没有可验证的 category 字段。");


    bool foundCategoryTexture = false;
    const War3Map::Trigger::TriggerCategoryTable categories(*loadedCatalog.value);
    for (const auto& category : categories.Categories()) {
        if (category.icon.empty()) {
            continue;
        }
        QCOMPARE(
            provider.TextureForCategory(QString::fromStdString(category.id)),
            QString::fromStdString(category.icon));
        foundCategoryTexture = true;
        break;
    }
    QVERIFY2(foundCategoryTexture, "外部 Catalog 中没有可验证的分类纹理。");
}

// 真实链路同时需要本地游戏根和外部 UI 根，两者都是 opt-in。
// 从 Catalog 中动态寻找可由当前游戏资源解码的链路。
void QtModelTests::IconProbeChainOnExternalData() {
    if (!qEnvironmentVariableIsSet("WAR3_GAME_ROOT") ||
        !qEnvironmentVariableIsSet("WAR3_UI_DATA_ROOT")) {
        QSKIP("WAR3_GAME_ROOT/WAR3_UI_DATA_ROOT 未同时设置：跳过真实链路验证。");
    }
    const std::filesystem::path gameRoot(
        qEnvironmentVariable("WAR3_GAME_ROOT").toStdWString());
    const std::filesystem::path dataRoot(
        qEnvironmentVariable("WAR3_UI_DATA_ROOT").toStdWString());

    auto loadedCatalog = War3Map::Trigger::UiCatalog::Load(dataRoot);
    QVERIFY(loadedCatalog.Succeeded());

    War3Map::QtApp::IconProvider provider;
    QVERIFY(provider.SetGameRoot(gameRoot));
    provider.MergeCatalogCategories(*loadedCatalog.value);

    auto mounted = War3Map::Game::GameDataSource::Mount(gameRoot);
    QVERIFY(mounted.Succeeded() && mounted.value.has_value());

    bool foundDecodableChain = false;
    constexpr War3Map::Trigger::UiDefinitionKind kinds[] = {
        War3Map::Trigger::UiDefinitionKind::Event,
        War3Map::Trigger::UiDefinitionKind::Condition,
        War3Map::Trigger::UiDefinitionKind::Action,
        War3Map::Trigger::UiDefinitionKind::Call,
    };
    for (const auto kind : kinds) {
        for (const auto* definition : loadedCatalog.value->Definitions(kind)) {
            const QString functionName = QString::fromStdString(definition->name);
            const QString category = provider.CategoryForFunction(functionName);
            QString texture = provider.TextureForCategory(category);
            if (category.isEmpty() || texture.isEmpty()) {
                continue;
            }
            texture.replace('/', '\\');
            if (!texture.endsWith(QStringLiteral(".blp"), Qt::CaseInsensitive)) {
                texture += QStringLiteral(".blp");
            }
            auto blp = mounted.value->ReadFile(texture.toStdString());
            if (!blp.Succeeded() || !blp.value.has_value() || blp.value->empty()) {
                continue;
            }
            auto decoded = War3Map::Image::DecodeBlp(*blp.value);
            if (decoded.Succeeded() && decoded.value.has_value() && decoded.value->Valid()) {
                foundDecodableChain = true;
                break;
            }
        }
        if (foundDecodableChain) {
            break;
        }
    }
    QVERIFY2(foundDecodableChain, "外部 UI Catalog 中未找到可由当前游戏资源解码的图标链路。");
}

void QtModelTests::EcaTypesReuseOnlyAcceptedCatalogIdentity() {
    using namespace War3Map::Trigger;
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    WriteFixture(root / "ui/config", "base\n");
    const auto define = root / "ui/base/define.txt";
    WriteFixture(define, "[TriggerTypes]\ninteger=0,1,1,整数\nreal=0,1,1,实数\n"
        "alias=0,1,1,WESTRING_CACHE_TYPE,integer\n[TriggerParams]\nPreset=0,integer,1,一\n");
    auto loaded = UiCatalog::Load(root); QVERIFY(loaded.Succeeded() && loaded.value);
    auto catalog = std::move(*loaded.value); TriggerDocument document; TriggerEcaModel model;
    model.SetDocument(&document, &catalog); const auto* original = model.TypeSystem(); QVERIFY(original);
    QCOMPARE(original->LiteralKind("alias"), TriggerTypeSystem::Literal::Integer);
    QCOMPARE(original->PresetType("Preset"), std::optional<std::string>{"integer"});
    model.SetDocument(&document, &catalog); QCOMPARE(model.TypeSystem(), original);
    QCOMPARE(&model.TypeSystem()->Catalog(), &catalog);
    // 真实 Localize 读取当前表；目录型缓存不保存旧本地化文字或 WTS。
    auto firstStrings = std::make_shared<War3Map::Game::WorldEditStrings>();
    QVERIFY(firstStrings->AddEntry("WESTRING_CACHE_TYPE", "整数别名")); catalog.SetStrings(firstStrings);
    QCOMPARE(model.TypeSystem()->DisplayName("alias"), std::string("整数别名"));
    auto nextStrings = std::make_shared<War3Map::Game::WorldEditStrings>();
    QVERIFY(nextStrings->AddEntry("WESTRING_CACHE_TYPE", "当前类型标题")); catalog.SetStrings(nextStrings);
    model.SetDocument(&document, &catalog); QCOMPARE(model.TypeSystem(), original);
    QCOMPARE(model.TypeSystem()->DisplayName("alias"), std::string("当前类型标题"));
    auto sameBytes = catalog; QCOMPARE(sameBytes.Sha256(), catalog.Sha256());
    model.SetDocument(&document, &sameBytes);
    QCOMPARE(&model.TypeSystem()->Catalog(), &sameBytes); // 同 SHA 不同 owner 必须重新绑定。
    const auto priorSha = sameBytes.Sha256();
    WriteFixture(define, "[TriggerTypes]\ninteger=0,1,1,整数\nreal=0,1,1,实数\n"
        "alias=0,1,1,WESTRING_CACHE_TYPE,real\n[TriggerParams]\nPreset=0,real,1.5,一点五\n");
    loaded = UiCatalog::Load(root); QVERIFY(loaded.Succeeded() && loaded.value);
    sameBytes = std::move(*loaded.value); QVERIFY(sameBytes.Sha256() != priorSha);
    model.SetDocument(&document, &sameBytes); // 同址不同实际 SHA，不沿用旧类型/预设。
    QCOMPARE(model.TypeSystem()->LiteralKind("alias"), TriggerTypeSystem::Literal::Real);
    QCOMPARE(model.TypeSystem()->PresetType("Preset"), std::optional<std::string>{"real"});
    model.SetDocument(nullptr, &sameBytes); QVERIFY(!model.TypeSystem());
    model.SetDocument(&document, nullptr); QVERIFY(!model.TypeSystem());
    model.SetDocument(&document, &catalog); QVERIFY(model.TypeSystem());
    QCOMPARE(&model.TypeSystem()->Catalog(), &catalog);
    QCOMPARE(model.TypeSystem()->LiteralKind("alias"), TriggerTypeSystem::Literal::Integer);
    UiCatalog empty; model.SetDocument(&document, &empty); QVERIFY(model.TypeSystem());
    QVERIFY(model.TypeSystem()->Find("alias") == nullptr); // 空来源不能继承已接受目录。
    model.SetDocument(nullptr, nullptr); QVERIFY(!model.TypeSystem());
}

void QtModelTests::EcaTypesRevalidateAfterResetCallback() {
    using namespace War3Map::Trigger;
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    WriteFixture(root / "ui/config", "base\n");
    const auto define = root / "ui/base/define.txt";
    WriteFixture(define, "[TriggerTypes]\ninteger=0,1,1,整数\nreal=0,1,1,实数\n"
        "alias=0,1,1,别名,integer\n[TriggerParams]\nPreset=0,integer,1,一\n");
    auto loaded = UiCatalog::Load(root); QVERIFY(loaded.Succeeded() && loaded.value);
    auto catalog = std::move(*loaded.value); TriggerDocument document; TriggerEcaModel model;
    model.SetDocument(&document, &catalog);
    QVERIFY(model.TypeSystem());
    QCOMPARE(model.TypeSystem()->LiteralKind("alias"), TriggerTypeSystem::Literal::Integer);
    QCOMPARE(model.TypeSystem()->PresetType("Preset"), std::optional<std::string>{"integer"});
    const auto priorSha = catalog.Sha256();
    WriteFixture(define, "[TriggerTypes]\ninteger=0,1,1,整数\nreal=0,1,1,实数\n"
        "alias=0,1,1,别名,real\n[TriggerParams]\nPreset=0,real,1.5,一点五\n");
    auto replacement = UiCatalog::Load(root); QVERIFY(replacement.Succeeded() && replacement.value);
    QVERIFY(replacement.value->Sha256() != priorSha);
    bool replaced = false;
    const auto connection = QObject::connect(&model, &QAbstractItemModel::modelAboutToBeReset, &model, [&] {
        if (!replaced) {
            catalog = std::move(*replacement.value); // 真实同步通知中同 owner 地址换已加载来源。
            replaced = true;
        }
    });
    model.SetDocument(&document, &catalog);
    QObject::disconnect(connection);
    QVERIFY(replaced); QVERIFY(catalog.Sha256() != priorSha); QVERIFY(model.TypeSystem());
    QCOMPARE(&model.TypeSystem()->Catalog(), &catalog);
    QCOMPARE(model.TypeSystem()->LiteralKind("alias"), TriggerTypeSystem::Literal::Real);
    QCOMPARE(model.TypeSystem()->PresetType("Preset"), std::optional<std::string>{"real"});
}

void QtModelTests::EcaTypesNeverCacheDocumentQualification() {
    using namespace War3Map::Trigger;
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    WriteFixture(root / "ui/config", "base\n");
    WriteFixture(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数\nreal=0,1,1,实数\n");
    WriteFixture(root / "ui/base/action.txt", "[SetVariable]\ndescription=\"设置 ${变量} = ${值}\"\n"
        "[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n");
    auto loaded = UiCatalog::Load(root); QVERIFY(loaded.Succeeded() && loaded.value);
    TriggerDocument document; VariableRecord variable; variable.id = 11;
    variable.name.text = "Scalar"; variable.typeName.text = "integer"; document.variables.push_back(variable);
    TriggerRecord trigger; trigger.id = 21; trigger.ecas = {0}; document.triggers.push_back(trigger);
    EcaNode eca; eca.id = 31; eca.sourceTriggerId = 21; eca.type = 2; eca.name.text = "SetVariable"; eca.arguments = {0, 1};
    document.ecaNodes.push_back(eca);
    ArgumentNode destination; destination.id = 41; destination.type = 1; destination.value.text = "Scalar";
    ArgumentNode value; value.id = 42; value.type = 3; value.value.text = "1";
    document.argumentNodes = {destination, value}; TriggerEcaModel model;
    model.SetDocument(&document, &*loaded.value); model.SetTrigger(21); const auto* cached = model.TypeSystem();
    QVERIFY(cached); QCOMPARE(cached->ExpectedType(document, document.ecaNodes[0], 1), std::string("integer"));
    QVERIFY(cached->ValidateWritableVariable(document, document.argumentNodes[0], "integer").Succeeded());
    QVERIFY(model.IndexForStableId(31).data().toString().contains(QStringLiteral("1")));
    document.variables[0].typeName.text = "real"; document.argumentNodes[1].value.text = "2.5";
    document.ecaNodes[0].id = 32;
    model.SetDocument(&document, &*loaded.value); QCOMPARE(model.TypeSystem(), cached); model.SetTrigger(21);
    QCOMPARE(cached->ExpectedType(document, document.ecaNodes[0], 1), std::string("real"));
    QVERIFY(!cached->ValidateWritableVariable(document, document.argumentNodes[0], "integer").Succeeded());
    QVERIFY(cached->ValidateWritableVariable(document, document.argumentNodes[0], "real").Succeeded());
    QVERIFY(!model.IndexForStableId(31).isValid()); QVERIFY(model.IndexForStableId(32).isValid());
    QVERIFY(model.IndexForStableId(32).data().toString().contains(QStringLiteral("2.5")));
    document.variables.clear(); model.SetDocument(&document, &*loaded.value); model.SetTrigger(21);
    QCOMPARE(model.TypeSystem(), cached);
    QVERIFY(!cached->ValidateWritableVariable(document, document.argumentNodes[0], "real").Succeeded());
    QCOMPARE(cached->ExpectedType(document, document.ecaNodes[0], 1), std::string("Null"));
}

QTEST_GUILESS_MAIN(QtModelTests)
#include "QtModelTests.moc"
