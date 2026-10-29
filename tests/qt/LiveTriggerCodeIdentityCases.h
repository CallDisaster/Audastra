#pragma once
#include "ProjectTriggerCodeIdentity.h"
#include "TriggerModels.h"
#include "TriggerModule.h"
#include "../support/TriggerCodeIdentitySaveFixture.h"

void LiveEditorToolsTests::CustomCodeIdentitySharesProjectUndoAndForge() {
    namespace I = AudastraTest::CodeIdentity;
    I::Fixture native; Fixture f; QVERIFY2(f.Initialize(false, false), qPrintable(f.error));
    for (const auto& member : native.members) if (member.name != "war3map.wtg" && member.name != "war3map.wct")
        QVERIFY(WriteFile(f.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
    f.project.war3GameRoot_ = native.base.inputs.game;
    auto& document = *f.project.triggerDocument_;
    const auto custom = TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "CodeSource"); QVERIFY(custom.Succeeded());
    auto* row = TriggerDocumentEditor::FindTrigger(document, *custom.value); row->customTextFlag = 1;
    row->customText = native.triggers.triggers.back().customText;
    const auto encoded = WtgWctCodec{}.Serialize(document, *f.project.uiCatalog_); QVERIFY(encoded.Succeeded());
    QVERIFY(WriteFile(f.source.filePath("map/war3map.wtg"), Bytes(encoded.value->wtg)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.wct"), Bytes(encoded.value->wct)));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, *f.project.uiCatalog_); QVERIFY(parsed.Succeeded());
    f.project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(*parsed.value));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.LoadMapDocument(); f.project.ResetEditTracking();
    const auto make = [&](const char* name, bool copy = false) {
        auto context = I::Take(QtProject::CaptureProjectTriggerCodeIdentityContext(f.project));
        auto request = native.Edit(name, copy); const auto& current = *f.project.triggerDocument_;
        const auto found = std::find_if(current.triggers.begin(), current.triggers.end(), [](const auto& t) { return t.customTextFlag != 0; });
        request.triggerId = found->id; request.expectedName = found->name;
        request.expectedCodeSha256 = I::Take(Sha256(I::Take(found->customText.ToBytes())));
        request.expected = I::Take(CaptureTriggerGraphBinding(current, *f.project.uiCatalog_, context.mapObjects));
        request.categoryId = found->categoryId; request.afterTriggerId = found->id; request.expectedCategoryOrder.clear();
        request.categoryRecordId = std::find_if(current.categories.begin(), current.categories.end(), [&](const auto& c) { return c.categoryId == found->categoryId; })->id;
        for (const auto& t : current.triggers) if (t.categoryId == found->categoryId) request.expectedCategoryOrder.push_back(t.id);
        auto result = QJsonDocument::fromJson(QByteArray::fromStdString(I::Envelope(request, true))).object();
        const auto identity = f.Context(); result["project_identity"] = identity.value("project_identity"); result["expected_revision"] = identity.value("expected_revision");
        return result;
    };
    auto request = make("CodeNew"); const auto undoCount = f.project.triggerUndoStack_->count();
    auto missing = request; missing.remove("dry_run");
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", missing)), QString("TRIGGER_CODE_REQUEST_INVALID"));
    auto preview = f.tools->Invoke("editor_edit_trigger_graph", request); QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QCOMPARE(f.project.triggerUndoStack_->count(), undoCount);
    QCOMPARE(QString::fromStdString(f.project.triggerDocument_->triggers.back().name.text), QString("CodeSource"));
    request["dry_run"] = false; auto changed = f.tools->Invoke("editor_edit_trigger_graph", request);
    QVERIFY2(changed.value("ok").toBool(), qPrintable(Explain(changed)));
    QCOMPARE(f.project.triggerUndoStack_->count(), undoCount + 1);
    QCOMPARE(QString::fromStdString(f.project.triggerDocument_->triggers.back().name.text), QString("CodeNew"));
    QVERIFY(!f.tools->Invoke("editor_edit_trigger_graph", request).value("ok").toBool());
    f.project.triggerUndoStack_->undo(); QCOMPARE(QString::fromStdString(f.project.triggerDocument_->triggers.back().name.text), QString("CodeSource"));
    f.project.triggerUndoStack_->redo(); QCOMPARE(QString::fromStdString(f.project.triggerDocument_->triggers.back().name.text), QString("CodeNew"));
    auto same = make("CodeNew"); same["dry_run"] = false;
    const auto noOp = f.tools->Invoke("editor_edit_trigger_graph", same); QVERIFY2(noOp.value("ok").toBool(), qPrintable(Explain(noOp)));
    QVERIFY(!noOp.value("published").toBool()); QCOMPARE(f.project.triggerUndoStack_->count(), undoCount + 1);
    request = make("CodeClone", true); request["dry_run"] = false;
    changed = f.tools->Invoke("editor_edit_trigger_graph", request); QVERIFY2(changed.value("ok").toBool(), qPrintable(Explain(changed)));
    QCOMPARE(f.project.triggerDocument_->triggers.size(), std::size_t(3));
    const auto target = std::filesystem::path(f.outputs.filePath("code-identity.forge").toStdWString());
    const auto saved = f.project.SaveProjectToPath(target); QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "save code project" : saved.diagnostics.front().message.c_str());
    const auto restored = QtProject::PrepareProjectOpen(target); QVERIFY2(restored.Succeeded(), restored.diagnostics.empty() ? "reopen code project" : restored.diagnostics.front().message.c_str());
    QVERIFY((*restored.value)->triggers);
    const auto& reopened = (*restored.value)->triggers->triggers;
    QCOMPARE(reopened.size(), std::size_t(3)); QCOMPARE(QString::fromStdString(reopened.back().name.text), QString("CodeClone"));
    QVERIFY(reopened.back().customText.text.find("function InitTrig_CodeClone") != std::string::npos);
    auto context = I::Take(QtProject::CaptureProjectTriggerCodeIdentityContext(f.project));
    auto sourceRequest = native.Edit("CodeNext"); const auto& current = f.project.triggerDocument_->triggers[1];
    sourceRequest.triggerId = current.id; sourceRequest.expectedName = current.name;
    sourceRequest.expectedCodeSha256 = I::Take(Sha256(I::Take(current.customText.ToBytes())));
    sourceRequest.expected = I::Take(CaptureTriggerGraphBinding(*f.project.triggerDocument_, *f.project.uiCatalog_, context.mapObjects));
    auto plan = Services::TriggerCodeIdentityEditing::Prepare(context, sourceRequest); QVERIFY(plan.Succeeded());
    const auto count = f.project.triggerUndoStack_->count(); f.project.luaMapTarget_.emplace(); ++f.project.luaMapTargetGeneration_;
    auto rejected = QtProject::ApplyProjectTriggerCodeIdentity(f.project, *plan.value, QStringLiteral("test"));
    QVERIFY(!rejected.Succeeded()); QCOMPARE(f.project.triggerUndoStack_->count(), count);
}

// 触发器目录里给自定义代码触发器改名、复制：走同一代码身份服务，一步撤销，生成符号随名字同步。
void LiveEditorToolsTests::CustomCodeIdentityFromTriggerDirectory() {
    namespace I = AudastraTest::CodeIdentity;
    I::Fixture native; Fixture f; QVERIFY2(f.Initialize(false, false), qPrintable(f.error));
    for (const auto& member : native.members) if (member.name != "war3map.wtg" && member.name != "war3map.wct")
        QVERIFY(WriteFile(f.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
    f.project.war3GameRoot_ = native.base.inputs.game;
    auto& document = *f.project.triggerDocument_;
    const auto custom = TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "CodeSource"); QVERIFY(custom.Succeeded());
    auto* row = TriggerDocumentEditor::FindTrigger(document, *custom.value); row->customTextFlag = 1;
    row->customText = native.triggers.triggers.back().customText;
    const auto encoded = WtgWctCodec{}.Serialize(document, *f.project.uiCatalog_); QVERIFY(encoded.Succeeded());
    QVERIFY(WriteFile(f.source.filePath("map/war3map.wtg"), Bytes(encoded.value->wtg)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.wct"), Bytes(encoded.value->wct)));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, *f.project.uiCatalog_); QVERIFY(parsed.Succeeded());
    f.project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(*parsed.value));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.LoadMapDocument(); f.project.ResetEditTracking();
    const auto codeTrigger = [&]() -> const TriggerRecord* {
        const auto& rows = f.project.triggerDocument_->triggers;
        const auto found = std::find_if(rows.begin(), rows.end(), [](const auto& t) { return t.customTextFlag != 0; });
        return found == rows.end() ? nullptr : &*found;
    };
    const auto* source = codeTrigger(); QVERIFY(source);
    const auto id = source->id; const auto category = source->categoryId; const auto before = source->customText.text;
    const auto undoCount = f.project.triggerUndoStack_->count();
    Modules::TriggerModule module(f.project); (void)module.Widget();
    QVERIFY(module.RenameDirectoryItem({false, id, category}, QStringLiteral("CodeGui")));
    QTRY_COMPARE(QString::fromStdString(codeTrigger()->name.text), QString("CodeGui"));
    QCOMPARE(f.project.triggerUndoStack_->count(), undoCount + 1);
    QVERIFY(codeTrigger()->customText.text.find("function InitTrig_CodeGui") != std::string::npos);
    QVERIFY(codeTrigger()->customText.text != before);
    module.DuplicateCurrentTrigger();
    QCOMPARE(f.project.triggerDocument_->triggers.size(), std::size_t(3));
    QCOMPARE(f.project.triggerUndoStack_->count(), undoCount + 2);
    const auto& copy = f.project.triggerDocument_->triggers.back();
    QVERIFY(copy.customTextFlag != 0 && copy.id != id);
    QVERIFY(copy.name.text.starts_with("CodeGui"));
    QVERIFY(copy.customText.text.find("function InitTrig_CodeGui takes") == std::string::npos);
    f.project.triggerUndoStack_->undo(); f.project.triggerUndoStack_->undo();
    QCOMPARE(f.project.triggerDocument_->triggers.size(), std::size_t(2));
    QCOMPARE(QString::fromStdString(codeTrigger()->name.text), QString("CodeSource"));
    QCOMPARE(codeTrigger()->customText.text, before);
}
