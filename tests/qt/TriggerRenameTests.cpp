// NQ020 原创 GUI 触发器引用同步；不使用作者地图，不启动游戏。
#include "EditorProject.h"
#include "TriggerCommands.h"
#include "TriggerModule.h"
#include "TriggerModels.h"
#include "MapSaveJob.h"
#include "LiveEditorTools.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Workspace.h"
#include "../MapDataFixtures.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSortFilterProxyModel>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Trigger;
using War3Map::QtProject::EditorProject;

namespace {
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data()); return {p, p + text.size()};
}
struct Wire {
    std::vector<std::byte> data;
    void Int(std::int32_t n) { for (int bit = 0; bit < 32; bit += 8) data.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(n) >> bit) & 255)); }
    void Text(std::string_view text) { auto b = Bytes(text); data.insert(data.end(), b.begin(), b.end()); data.push_back(std::byte{}); }
};
std::vector<std::byte> Wtg(bool custom, bool opaque, std::string_view functionName = "RunTrigger") {
    Wire w; w.data = Bytes("WTG!"); w.Int(7); w.Int(1); w.Int(1); w.Text("Original GUI"); w.Int(0);
    w.Int(2); w.Int(0); w.Int(3);
    const auto trigger = [&](std::string_view name, bool code, int roots) {
        w.Text(name); w.Text("original description\r\nunchanged"); w.Int(0); w.Int(1); w.Int(code); w.Int(0); w.Int(0); w.Int(1); w.Int(roots);
    };
    trigger("Init", custom, 0); trigger("Other", false, 0); trigger("Caller", false, opaque ? 3 : 2);
    for (int i = 0; i < 2; ++i) {
        w.Int(2); w.Text(functionName); w.Int(1);
        w.Int(1); w.Text("gg_trg_Init"); w.Int(0); w.Int(0); w.Int(0);
    }
    if (opaque) {
        w.Int(2); w.Text("CustomScriptCode"); w.Int(1);
        w.Int(3); w.Text("call DoNothing()"); w.Int(0); w.Int(0); w.Int(0);
    }
    return std::move(w.data);
}
std::vector<std::byte> Wct(std::string_view header, bool custom) {
    Wire w; w.Int(1); w.Text("");
    const auto code = [&](std::string_view text) { w.Int(text.empty() ? 0 : static_cast<std::int32_t>(text.size() + 1)); if (!text.empty()) w.Text(text); };
    code(header); w.Int(3);
    code(custom ? "function InitTrig_Init takes nothing returns nothing\nendfunction\n" : ""); code(""); code("");
    return std::move(w.data);
}
void Require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
std::string Explain(const auto& result) {
    std::string text;
    for (const auto& d : result.diagnostics) text += d.code + ": " + d.message + "\n";
    return text;
}
template<class T> T Take(Result<T>&& result) { Require(result.Succeeded() && result.value.has_value(), Explain(result)); return std::move(*result.value); }
bool Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    QDir().mkpath(QString::fromStdWString(path.parent_path().wstring()));
    QFile file(QString::fromStdWString(path.wstring()));
    return file.open(QIODevice::WriteOnly) && file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size());
}
bool Write(const std::filesystem::path& path, std::string_view text) { return Write(path, Bytes(text)); }
const TriggerRecord* Named(const EditorProject& project, std::string_view name) {
    const auto& rows = project.triggerDocument_->triggers;
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.name.text == name; });
    return it == rows.end() ? nullptr : &*it;
}
struct Fixture {
    QTemporaryDir temp;
    EditorProject project;
    std::unique_ptr<Modules::TriggerModule> module;
    std::unique_ptr<QtApp::LiveEditorTools> tools;
    std::filesystem::path root, ui;
    EncodedTriggerDocument original;
    StableId target = 0;
    QString error;
    bool Initialize(std::string_view header = {}, bool custom = false, bool opaque = false,
        const std::filesystem::path& readonlyUiRoot = {}) {
        try {
            Require(temp.isValid(), "owned test directory"); root = std::filesystem::path(temp.path().toStdWString());
            if (readonlyUiRoot.empty()) {
                ui = root / "catalog";
                Require(Write(ui / "ui/config", "base\n") && Write(ui / "ui/base/define.txt", "[TriggerTypes]\ntrigger=0,1,1,Trigger\nstring=0,1,1,String,string\nscriptcode=0,1,1,Code,string\n")
                    && Write(ui / "ui/base/action.txt", "[RunTrigger]\nscript_name=TriggerExecute\ndescription=Run ${target}\n[[.args]]\ntype=trigger\n[CustomScriptCode]\ndescription=Code ${code}\n[[.args]]\ntype=scriptcode\n"), "owned UI metadata");
            } else {
                Require(readonlyUiRoot.is_absolute(), "explicit readonly UI root");
                ui = UiCatalog::ResolveUiRoot(readonlyUiRoot); // 不写 config/元数据；实际层序从最初的 seal 建立。
            }
            auto catalog = Take(UiCatalog::Load(ui));
            auto document = Take(WtgWctCodec{}.Parse(Wtg(custom, opaque, readonlyUiRoot.empty() ? "RunTrigger" : "TriggerExecute"), Wct(header, custom), catalog));
            original = Take(WtgWctCodec{}.Serialize(document, catalog));
            Require(Write(root / "source/map/war3map.wtg", original.wtg) && Write(root / "source/map/war3map.wct", original.wct), "owned source bytes");
            auto source = Take(DirectoryWorkspace::Open(root / "source"));
            project.projectRoot_ = root / "source";
            project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(source));
            project.uiDataRoot_ = project.activeUiDataRoot_ = ui;
            project.uiCatalog_ = std::make_unique<UiCatalog>(std::move(catalog));
            project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(document));
            target = Named(project, "Init")->id;
            project.triggerUndoStack_ = new QUndoStack(&project);
            QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &project, [&] { project.ObserveTriggerEdit(); });
            project.ResetEditTracking(); Require(project.editTrackingReady_ && !project.editTrackingFailed_, "owned edit baseline");
            module = std::make_unique<Modules::TriggerModule>(project); (void)module->Widget();
            tools = std::make_unique<QtApp::LiveEditorTools>(project); return true;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
    }
    EncodedTriggerDocument Pair() const { return Take(WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_)); }
    bool OriginalBytes() const { const auto p = Pair(); return p.wtg == original.wtg && p.wct == original.wct; }
    bool SourceBytes() const {
        const auto read = [](const auto& path) { QFile f(QString::fromStdWString(path.wstring())); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; };
        return read(root / "source/map/war3map.wtg") == QByteArray(reinterpret_cast<const char*>(original.wtg.data()), static_cast<qsizetype>(original.wtg.size()))
            && read(root / "source/map/war3map.wct") == QByteArray(reinterpret_cast<const char*>(original.wct.data()), static_cast<qsizetype>(original.wct.size()));
    }
    bool RenameThroughModel(const QString& name) {
        auto* view = module->Widget()->findChild<QTreeView*>("triggerExplorer");
        auto* proxy = view ? qobject_cast<QSortFilterProxyModel*>(view->model()) : nullptr;
        auto* model = proxy ? dynamic_cast<TriggerExplorerModel*>(proxy->sourceModel()) : nullptr;
        return model && proxy->setData(proxy->mapFromSource(model->IndexForTriggerId(target)), name, Qt::EditRole);
    }
    bool ReferencesEqual(std::string_view name) const {
        const auto& args = project.triggerDocument_->argumentNodes;
        return std::count_if(args.begin(), args.end(), [&](const auto& a) { return a.type == 1 && a.value.text == "gg_trg_" + std::string(name); }) == 2;
    }
    QJsonObject RenameRequest(QString name) const {
        const auto state = tools->Invoke("editor_state", {});
        const auto b = Take(CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_));
        const auto& raw = TriggerDocumentEditor::FindTrigger(*project.triggerDocument_, target)->name;
        return {{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
            {"expected", QJsonObject{{"source_wtg_sha256", QString::fromStdString(b.sourceWtgSha256)},
                {"source_wct_sha256", QString::fromStdString(b.sourceWctSha256)}, {"ui_catalog_sha256", QString::fromStdString(b.uiCatalogSha256)},
                {"current_wtg_sha256", QString::fromStdString(b.currentWtgSha256)}, {"current_wct_sha256", QString::fromStdString(b.currentWctSha256)}}},
            {"operations", QJsonArray{QJsonObject{{"op", "rename"}, {"target", QJsonObject{{"kind", "trigger"}, {"id", QString::number(target)}}},
                {"expected_name", QJsonObject{{"text", QString::fromStdString(raw.text)}, {"encoding", "utf8"},
                    {"raw_bytes_hex", QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(raw.originalBytes.data()), static_cast<qsizetype>(raw.originalBytes.size())).toHex())}}},
                {"name", name}}}}};
    }
};
} // namespace

class TriggerRenameTests final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("Audastra-NQ020-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("TriggerRenameTests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }
    void GuiRenameSynchronizesReferencesAndWholeUndo();
    void GuiAndMcpShareOneUndoTransaction();
    void UnsafeCodeAndDuplicateNameRejectWithoutMutation();
    void QueuedStaleSessionAndUndoReject();
    void FrozenGraphIdentityAndCatalogSourceGuardsReject();
    void UnboundIdentityAndRejectedFirstPublicationExposeFailure();
    void ActualSmallMapSaveAndReopen();
};

void TriggerRenameTests::GuiRenameSynchronizesReferencesAndWholeUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto* identity = f.project.triggerDocument_.get();
    QVERIFY(f.RenameThroughModel(QStringLiteral("新的 波次")));
    QTRY_VERIFY(Named(f.project, "新的 波次")); QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    QVERIFY(f.ReferencesEqual("新的 波次")); QCOMPARE(f.project.triggerDocument_.get(), identity);
    const auto edited = f.Pair(); QVERIFY(edited.wct == f.original.wct); QVERIFY(f.SourceBytes());
    Script::MapScriptInputs inputs; inputs.triggers = f.project.triggerDocument_.get(); inputs.catalog = f.project.uiCatalog_.get();
    const auto script = Script::GenerateMapScript(inputs); QVERIFY2(script.Succeeded(), Explain(script).c_str());
    const auto symbol = "gg_trg_" + TriggerDirectoryEditor::ScriptName("新的 波次");
    QVERIFY(script.text.find("call TriggerExecute(" + symbol + ")") != std::string::npos);
    QVERIFY(script.text.find("gg_trg_Init") == std::string::npos);
    f.project.triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes()); QVERIFY(f.SourceBytes()); QVERIFY(f.ReferencesEqual("Init"));
    f.project.triggerUndoStack_->redo(); const auto again = f.Pair(); QVERIFY(again.wtg == edited.wtg && again.wct == edited.wct);
    QVERIFY(f.SourceBytes()); QCOMPARE(f.project.triggerDocument_.get(), identity);
}

void TriggerRenameTests::GuiAndMcpShareOneUndoTransaction() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QVERIFY(f.RenameThroughModel(QStringLiteral("GuiName"))); QTRY_VERIFY(Named(f.project, "GuiName"));
    const auto gui = f.Pair();
    const auto response = f.tools->Invoke("editor_edit_trigger_graph", f.RenameRequest(QStringLiteral("McpName")));
    QVERIFY2(response.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))));
    QCOMPARE(f.project.triggerUndoStack_->count(), 2); QVERIFY(f.ReferencesEqual("McpName"));
    f.project.triggerUndoStack_->undo(); const auto first = f.Pair(); QVERIFY(first.wtg == gui.wtg && first.wct == gui.wct);
    f.project.triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes());
    f.project.triggerUndoStack_->redo(); f.project.triggerUndoStack_->redo(); QVERIFY(f.ReferencesEqual("McpName")); QVERIFY(f.SourceBytes());
}

void TriggerRenameTests::UnsafeCodeAndDuplicateNameRejectWithoutMutation() {
    for (int kind = 0; kind < 4; ++kind) {
        Fixture f; QVERIFY2(f.Initialize(kind == 1 ? "// opaque author header\r\n" : "", kind == 2, kind == 3), qPrintable(f.error));
        const auto fingerprint = f.project.ProjectFingerprint();
        const bool accepted = f.RenameThroughModel(kind == 0 ? QStringLiteral("Other") : QStringLiteral("Changed"));
        if (kind == 0 || kind == 2) QVERIFY(!accepted);
        else { QVERIFY(accepted); QCoreApplication::processEvents(); }
        QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.ReferencesEqual("Init"));
        QCOMPARE(f.project.ProjectFingerprint(), fingerprint); QVERIFY(f.SourceBytes());
        if (kind == 1 || kind == 3) {
            const auto response = f.tools->Invoke("editor_edit_trigger_graph", f.RenameRequest(QStringLiteral("Changed")));
            QVERIFY(!response.value("ok").toBool());
            QVERIFY(QString::fromUtf8(QJsonDocument(response).toJson()).contains(QStringLiteral("TRIGGER_RENAME_CODE_AUDIT_REQUIRED")));
            QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes());
        }
    }
}

void TriggerRenameTests::QueuedStaleSessionAndUndoReject() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QVERIFY(f.RenameThroughModel(QStringLiteral("StaleName")));
    f.project.ResetEditTracking(); QCoreApplication::processEvents();
    QVERIFY(f.OriginalBytes()); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    QVERIFY(f.RenameThroughModel(QStringLiteral("GoodName"))); QTRY_VERIFY(Named(f.project, "GoodName"));
    const auto saved = f.Pair();
    f.project.ResetEditTracking(); f.project.triggerUndoStack_->undo();
    const auto untouched = f.Pair(); QVERIFY(saved.wtg == untouched.wtg && saved.wct == untouched.wct); QVERIFY(f.SourceBytes());
}

void TriggerRenameTests::FrozenGraphIdentityAndCatalogSourceGuardsReject() {
    for (int kind = 0; kind < 7; ++kind) {
        Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
        QVERIFY(f.RenameThroughModel(QStringLiteral("GoodName"))); QTRY_VERIFY(Named(f.project, "GoodName"));
        const UiCatalog originalCatalog(*f.project.uiCatalog_);
        const auto identity = Take(CaptureTriggerGraphReplayIdentity(*f.project.triggerDocument_, originalCatalog));
        const auto originalPayload = f.Pair();
        if (kind == 0) std::swap(f.project.triggerDocument_->argumentNodes[0].id, f.project.triggerDocument_->argumentNodes[1].id);
        if (kind == 1) f.project.triggerDocument_->argumentNodes[0].value.encoding = TextEncoding::Latin1;
        if (kind == 2) ++f.project.triggerDocument_->argumentNodes[0].origin.offset;
        if (kind == 3) {
            QVERIFY(Write(f.ui / "ui/base/action.txt", "[RunTrigger]\nscript_name=TriggerExecute\ntitle=New presentation\ndescription=Run ${target}\n[[.args]]\ntype=trigger\n[CustomScriptCode]\ndescription=Code ${code}\n[[.args]]\ntype=scriptcode\n"));
            *f.project.uiCatalog_ = Take(UiCatalog::Load(f.ui)); // 指针不变、真实元数据 SHA 改变。
        }
        if (kind == 4) f.project.uiCatalog_ = std::make_unique<UiCatalog>(originalCatalog); // 内容相同、会话指针不同。
        if (kind == 5) f.project.sourceChangeSuspected_ = true;
        if (kind == 6) f.project.sourceChangeConfirmed_ = true;
        const auto mutated = Take(WtgWctCodec{}.Serialize(*f.project.triggerDocument_, originalCatalog));
        QVERIFY(mutated.wtg == originalPayload.wtg && mutated.wct == originalPayload.wct);
        if (kind <= 2) QVERIFY(!VerifyTriggerGraphReplayIdentity(*f.project.triggerDocument_, originalCatalog, identity).Succeeded());
        const auto id0 = f.project.triggerDocument_->argumentNodes[0].id;
        const auto encoding0 = f.project.triggerDocument_->argumentNodes[0].value.encoding;
        const auto origin0 = f.project.triggerDocument_->argumentNodes[0].origin.offset;
        f.project.triggerUndoStack_->undo(); // 拒绝旧事务；不能覆盖同载荷的 IR 元信息。
        const auto after = Take(WtgWctCodec{}.Serialize(*f.project.triggerDocument_, originalCatalog));
        QVERIFY(after.wtg == mutated.wtg && after.wct == mutated.wct);
        QCOMPARE(f.project.triggerDocument_->argumentNodes[0].id, id0);
        QCOMPARE(f.project.triggerDocument_->argumentNodes[0].value.encoding, encoding0);
        QCOMPARE(f.project.triggerDocument_->argumentNodes[0].origin.offset, origin0);
        QVERIFY(f.SourceBytes()); QVERIFY(Named(f.project, "GoodName"));
    }
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    f.project.sourceChangeSuspected_ = true;
    QVERIFY(!f.RenameThroughModel(QStringLiteral("Denied")));
    QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.SourceBytes());

    // MCP 写入会真实核验来源：只有磁盘确实改变，不能把人工疑似标志当成永久拒绝。
    const auto request = f.RenameRequest(QStringLiteral("Denied"));
    const auto sourcePath = f.root / "source/map/war3map.wct";
    const auto originalSourceSha = Take(Sha256File(sourcePath));
    auto externalBytes = f.original.wct; externalBytes.push_back(std::byte{0x23});
    QVERIFY(Write(sourcePath, externalBytes));
    const auto changedSourceSha = Take(Sha256File(sourcePath));
    QVERIFY(changedSourceSha != originalSourceSha);
    const auto fingerprint = f.project.ProjectFingerprint();
    const auto response = f.tools->Invoke("editor_edit_trigger_graph", request);
    QVERIFY(!response.value("ok").toBool());
    const auto diagnostics = response.value("diagnostics").toArray();
    QVERIFY(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) {
        return d.toObject().value("code").toString() == QStringLiteral("SOURCE_CHANGED");
    }));
    QVERIFY(f.project.sourceChangeSuspected_ && f.project.sourceChangeConfirmed_);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.ReferencesEqual("Init"));
    QCOMPARE(f.project.ProjectFingerprint(), fingerprint);
    QCOMPARE(Take(Sha256File(sourcePath)), changedSourceSha); // 不覆盖外部改变后的 owned 源。

    Fixture clean; QVERIFY2(clean.Initialize(), qPrintable(clean.error));
    clean.project.sourceChangeSuspected_ = true;
    const auto accepted = clean.tools->Invoke("editor_edit_trigger_graph", clean.RenameRequest(QStringLiteral("VerifiedName")));
    QVERIFY2(accepted.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(accepted).toJson(QJsonDocument::Compact))));
    QVERIFY(!clean.project.sourceChangeSuspected_ && !clean.project.sourceChangeConfirmed_);
    QCOMPARE(clean.project.triggerUndoStack_->count(), 1); QVERIFY(clean.ReferencesEqual("VerifiedName")); QVERIFY(clean.SourceBytes());
    clean.project.triggerUndoStack_->undo(); QVERIFY(clean.OriginalBytes()); QVERIFY(clean.ReferencesEqual("Init")); QVERIFY(clean.SourceBytes());
}

void TriggerRenameTests::UnboundIdentityAndRejectedFirstPublicationExposeFailure() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QVERIFY(!VerifyTriggerGraphReplayIdentity(*f.project.triggerDocument_, *f.project.uiCatalog_, {}).Succeeded());
    TriggerDocument before(*f.project.triggerDocument_), invalid(before);
    invalid.ecaNodes.front().arguments.clear(); // 原函数要求一个参数；实际 codec 拒绝此候选。
    auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(f.project, std::move(before), std::move(invalid), QStringLiteral("Denied test candidate"));
    QVERIFY(!command->Ready()); const auto published = command->FirstPublication();
    QVERIFY(!*published);
    f.project.triggerUndoStack_->push(command.release()); // obsolete 命令可被删，Outcome 独立保活。
    QVERIFY(!*published); QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.SourceBytes());
}

void TriggerRenameTests::ActualSmallMapSaveAndReopen() {
    const auto outputText = qEnvironmentVariable("FORGE_TRIGGER_RENAME_REAL_ROOT");
    if (outputText.isEmpty()) QSKIP("Set FORGE_TRIGGER_RENAME_REAL_ROOT for a single owned tiny-map save; no game executed.");
    try {
        const auto output = std::filesystem::path(outputText.toStdWString());
        const auto toolRoot = std::filesystem::path(qEnvironmentVariable("FORGE_YDWE_ROOT").toStdWString());
        const auto gameRoot = std::filesystem::path(qEnvironmentVariable("FORGE_WAR3_ROOT").toStdWString());
        Require(output.is_absolute() && toolRoot.is_absolute() && gameRoot.is_absolute(), "explicit absolute output/tool/game roots");
        Require(std::filesystem::create_directory(output), "unique output must not already exist; refusing prior receipt overwrite");
        const auto readonlyUi = UiCatalog::ResolveUiRoot(toolRoot);
        const auto configSha = Take(Sha256File(readonlyUi / "config"));
        const auto macroSha = Take(Sha256File(readonlyUi / "ydwe/jass/AntiBJLeak/MainMacro.h"));
        Fixture f; const bool initialized = f.Initialize({}, false, false, toolRoot);
        Require(initialized, f.error.toStdString());
        Require(f.ui == readonlyUi && std::find(f.project.uiCatalog_->Layers().begin(), f.project.uiCatalog_->Layers().end(), "ydwe")
            != f.project.uiCatalog_->Layers().end(), "actual readonly UI catalog and enabled ydwe include layer are bound from initial parse");
        MapData::MapSettings settings; settings.formatVersion = 25; settings.tileset = 'L'; settings.mapName = "Original NQ020 tiny fixture"; settings.custom.emplace();
        const auto settingBytes = Take(settings.Serialize());
        const auto source = output / "rename-source.w3x", candidate = output / "rename-candidate.w3x";
        const std::vector<MapArchiveMember> members{{"war3map.w3i", settingBytes}, {"war3map.wtg", f.original.wtg}, {"war3map.wct", f.original.wct},
            {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()}, {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
            {"war3map.j", Bytes("function config takes nothing returns nothing\nendfunction\nfunction main takes nothing returns nothing\nendfunction\n")},
            {"unchanged.bin", Bytes("Original opaque unrelated member")}};
        (void)Take(WriteVerifiedArchive(source, members)); auto archive = Take(MapArchiveSession::Open(source));
        const auto beforeSha = Take(Sha256File(source));
        f.project.archiveSession_ = std::make_unique<MapArchiveSession>(archive);
        f.project.sourceArchivePath_ = source; f.project.compilerDataRoot_ = toolRoot; f.project.war3GameRoot_ = gameRoot;
        f.project.ResetEditTracking();
        Require(f.RenameThroughModel(QStringLiteral("RenamedInit")), "actual UI rename queued");
        QTRY_VERIFY(Named(f.project, "RenamedInit")); Require(f.ReferencesEqual("RenamedInit"), "both actual references renamed");
        const auto edited = f.Pair();
        auto job = f.project.PrepareMapSave(candidate, false);
        Require(job && job->script && !job->scriptSkipped, "production project save preparation did not bind script rebuild");
        auto& task = *job;
        struct Evidence {
            Services::MapSaveTask& task;
            ~Evidence() noexcept { try { qInfo().noquote() << "script_workspace=" << QString::fromStdWString(task.scriptWorkspace.wstring()); } catch (...) {} }
        } evidence{task};
        task.beforePublish = [&] { return archive.VerifySourceUnchanged(); };
        EditorProject::RunMapSave(task);
        Require(task.executed && task.succeeded && task.scriptRebuilt && !task.scriptSkipped && task.nextSession, Explain(task));
        auto reopened = Take(MapArchiveSession::Open(candidate));
        const auto wtg = Take(reopened.ReadMember("war3map.wtg")), wct = Take(reopened.ReadMember("war3map.wct"));
        Require(wtg == edited.wtg && wct == edited.wct && wct == f.original.wct, "exact saved edited WTG and original WCT");
        const auto doc = Take(WtgWctCodec{}.Parse(wtg, wct, *f.project.uiCatalog_));
        Require(doc.triggers[0].name.text == "RenamedInit" && std::count_if(doc.argumentNodes.begin(), doc.argumentNodes.end(),
            [](const auto& a) { return a.type == 1 && a.value.text == "gg_trg_RenamedInit"; }) == 2, "independent reopened name and both references");
        const auto script = Take(reopened.ReadMember("war3map.j")); const std::string text(reinterpret_cast<const char*>(script.data()), script.size());
        Require(text.find("gg_trg_Init") == std::string::npos && text.find("call TriggerExecute(gg_trg_RenamedInit)") != std::string::npos,
            "actual compiled script uses exactly the renamed symbol");
        Require(Take(reopened.ReadMember("unchanged.bin")) == members.back().data && Take(Sha256File(source)) == beforeSha
            && archive.VerifySourceUnchanged().Succeeded() && f.SourceBytes(), "actual original sources and unrelated member unchanged");
        const auto committed = f.project.CommitMapSave(task);
        Require(committed && committed->scriptRebuilt && f.project.triggerUndoStack_->isClean(), "production own-save commits same graph and clean point");
        const auto candidateSha = Take(Sha256File(candidate));
        f.project.triggerUndoStack_->undo(); Require(f.OriginalBytes() && f.ReferencesEqual("Init"), "normal own-save preserves graph seal for whole transaction Undo");
        f.project.triggerUndoStack_->redo(); const auto replayed = f.Pair();
        Require(replayed.wtg == edited.wtg && replayed.wct == edited.wct && Take(Sha256File(candidate)) == candidateSha,
            "normal own-save Undo/Redo changes only current overlay, not saved candidate");
        Require(Take(Sha256File(readonlyUi / "config")) == configSha
            && Take(Sha256File(readonlyUi / "ydwe/jass/AntiBJLeak/MainMacro.h")) == macroSha,
            "real UI config and compiler include dependency remained readonly");
        Services::FinalizeScriptWorkspace(task);
        qInfo().noquote() << "source_sha256=" << QString::fromStdString(beforeSha)
            << "candidate_sha256=" << QString::fromStdString(reopened.SourceSha256()) << "script_sha256=" << QString::fromStdString(Take(Sha256(script)))
            << "actual_save_reopen=true script_rebuilt=true source_unchanged=true own_save_undo_redo=true game_executed=false";
    } catch (const std::exception& e) { QFAIL(e.what()); }
}

QTEST_MAIN(TriggerRenameTests)
#include "TriggerRenameTests.moc"
