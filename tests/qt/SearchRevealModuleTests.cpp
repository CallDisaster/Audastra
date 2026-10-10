#include "TriggerModule.h"
#include "TriggerModels.h"
#include "ObjectModule.h"
#include "AssetsModule.h"
#include "TriggerLuaScripts.h"
#include "EditorProject.h"
#include "MapSizeNavigation.h"
#include "Modules/ModuleHost.h"
#include "Modules/ModuleRegistry.h"
#include "Shell/ShellWindow.h"
#include "Widgets/CodeView.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QApplication>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>

#include <memory>
#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

using namespace War3Map;
namespace {
class Host final : public Shell::ModuleHost {
public:
    QString status;
    void Navigate(const Shell::NavigationTarget&) override {}
    void ActivateModule(const Shell::ModuleId&) override {}
    void ShowStatusMessage(const QString& text, int) override { status = text; }
    void UndoStackChanged(const Shell::ModuleId&) override {}
};
struct Bytes {
    std::vector<std::byte> data;
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            data.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 0xffu));
    }
    void Text(std::string_view text) { for (const char c : text) data.push_back(static_cast<std::byte>(c)); data.push_back(std::byte{}); }
};
bool Write(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool InstallTriggers(QtProject::EditorProject& project, const QTemporaryDir& root) {
    if (!Write(root.filePath("ui/config"), "base\n")
        || !Write(root.filePath("ui/base/define.txt"), "[TriggerTypes]\ninteger=0,1,1,整数\n")
        || !Write(root.filePath("ui/base/action.txt"), "[PlainAction]\ndescription=原创动作\n[ValueAction]\ndescription=值 ${值}\n[[.args]]\ntype=integer\n")
        || !Write(root.filePath("ui/base/call.txt"), "[NestedValue]\ndescription=函数 ${值}\nreturns=integer\n[[.args]]\ntype=integer\n")) return false;
    auto catalog = Trigger::UiCatalog::Load(root.path().toStdWString()); if (!catalog.Succeeded()) return false;
    Bytes w; for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7); w.I32(1); w.I32(37); w.Text("Owned category"); w.I32(0); w.I32(2); w.I32(0); w.I32(2);
    for (int row = 0; row < 2; ++row) {
        w.Text(row == 0 ? "First" : "Second"); w.Text("");
        w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(37); w.I32(1);
        w.I32(2); w.Text("PlainAction"); w.I32(1); w.I32(0);
    }
    Bytes c; c.I32(1); c.Text(""); c.I32(0); c.I32(2); c.I32(0); c.I32(0);
    auto parsed = Trigger::WtgWctCodec{}.Parse(w.data, c.data, *catalog.value); if (!parsed.Succeeded()) return false;
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.triggerUndoStack_ = new QUndoStack(&project); return true;
}
bool InstallObjects(QtProject::EditorProject& project) {
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\nC;X1;Y2;K\"hfoo\"\nC;X2;K\"Owned footman\"\nE\n";
    const auto reader = [slk](const std::string& path) {
        if (path != "owned-unit.slk") return Result<std::vector<std::byte>>::Failure("test", "missing", path);
        const auto bytes = std::as_bytes(std::span(slk.data(), slk.size()));
        return Result<std::vector<std::byte>>::Success({bytes.begin(), bytes.end()});
    };
    ObjectData::CatalogLoadOptions options; options.sources = {{ObjectData::ObjectType::Unit, {"owned-unit.slk"}, {}}}; options.stringFiles.clear();
    auto catalog = ObjectData::StandardObjectCatalog::Load(reader, options); if (!catalog.Succeeded()) return false;
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    const std::string lni = "[hfoo]\nhp=250\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    if (!parsed.Succeeded() || !parsed.document) return false;
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    return project.objectDocuments_[0] != nullptr;
}
Shell::NavigationTarget Parameter(StableId owner, StableId eca, StableId argument) {
    return {"triggers", "eca", QString::number(eca), {{"triggerId", QString::number(owner)},
        {"argumentId", QString::number(argument)}, {"stableIdentity", true}}};
}
bool InstallAssets(QtProject::EditorProject& project, const QTemporaryDir& root) {
    const auto bytes = [](std::string_view text) {
        const auto view = std::as_bytes(std::span(text.data(), text.size()));
        return std::vector<std::byte>(view.begin(), view.end());
    };
    const auto path = std::filesystem::path(root.filePath("owned-reveal.w3x").toStdWString());
    const std::vector<MapArchiveMember> members{{"Lua\\authored.lua", bytes("-- original\nreturn 1\n")},
        {"Textures\\Other.blp", bytes("original-other")}, {"Textures\\Conflict.blp", bytes("original-conflict")}};
    if (!WriteVerifiedArchive(path, members).Succeeded()) return false;
    auto opened = MapArchiveSession::Open(path); if (!opened.Succeeded() || !opened.value) return false;
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    project.sourceArchivePath_ = path; project.projectRoot_ = root.path().toStdWString();
    project.ResetEditTracking(); return true;
}
Shell::NavigationTarget Asset(QString path, QString identity) {
    return {"assets", "file", std::move(path), {{"stableIdentity", true}, {"sourceIdentity", std::move(identity)}}};
}
// 实际服务+私有生产adapter+Assets模块的桥接夹具，不冒MainWindow查询或菜单执行证明。
Result<Services::MapSizeReport> AssetSizeReport(const QtProject::EditorProject& project) {
    Services::MapSizeContext context; context.contextRevision = "owned-map-size-bridge/1";
    if (!project.archiveSession_) return Result<Services::MapSizeReport>::Failure("test", "source", "Missing owned archive");
    context.membersComplete = !project.archiveSession_->HasOpaqueMembers();
    for (const auto& member : project.archiveSession_->Members())
        context.members.push_back({member.name, static_cast<std::uint64_t>(member.data.size())});
    for (const auto& edit : project.assetEdits_)
        context.assetEdits.push_back({edit.from.toUtf8().toStdString(), edit.to.toUtf8().toStdString()});
    for (const auto& pending : project.pendingImports_) {
        const auto size = pending.ownedPayload
            ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(pending.ownedPayload->size())}
            : pending.size >= 0 ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(pending.size)} : std::nullopt;
        context.pending.push_back({pending.archivePath.toUtf8().toStdString(), size,
            pending.ownedPayload ? Services::MapSizeByteTrust::OwnedMetadata : Services::MapSizeByteTrust::ReportedUnverified});
    }
    return Services::AnalyzeMapSize(context, context.contextRevision);
}
std::optional<Services::MapSizeRow> SizeRow(const Services::MapSizeReport& report, const QString& path) {
    const auto encoded = path.toUtf8().toStdString();
    const auto found = std::find_if(report.top20.begin(), report.top20.end(), [&](const auto& row) { return row.currentPath == encoded; });
    return found == report.top20.end() ? std::nullopt : std::optional<Services::MapSizeRow>{*found};
}
StableId SelectedEca(QWidget* page) {
    auto* tree = page->findChild<QTreeView*>("triggerLogicTree"); if (!tree) return 0;
    const auto* model = static_cast<TriggerEcaModel*>(tree->model());
    const auto* eca = model->EcaAt(tree->currentIndex()); return eca ? eca->id : 0;
}
} // namespace

class SearchRevealModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void ActualArgumentOwnerAndArrayIndex();
    void NestedFunctionShowsContainingStatementWithoutModal();
    void WrongOwnerPreservesPendingDraft();
    void ExplicitObjectSourceCannotFallback();
    void ExactAssetIdentityRejectsConflictsBeforeViewChanges();
    void MapSizeBridgeUsesCurrentOriginalRenamedAndPendingSources();
    void ActualLuaDraftLeaveInvalidatesOldAssetIdentityWithoutHistoryChange();
private:
    QTemporaryDir settings_;
    QString oldOrganization_, oldApplication_;
    QSettings::Format oldFormat_ = QSettings::NativeFormat;
};
void SearchRevealModuleTests::initTestCase() {
    QVERIFY(settings_.isValid()); oldOrganization_ = QCoreApplication::organizationName(); oldApplication_ = QCoreApplication::applicationName();
    oldFormat_ = QSettings::defaultFormat(); QSettings::setDefaultFormat(QSettings::IniFormat);
    QCoreApplication::setOrganizationName("AudastraSearchRevealTest"); QCoreApplication::setApplicationName("OwnedFixture");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path()); QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.path());
}
void SearchRevealModuleTests::cleanupTestCase() {
    QSettings::setDefaultFormat(oldFormat_); QCoreApplication::setOrganizationName(oldOrganization_); QCoreApplication::setApplicationName(oldApplication_);
}
void SearchRevealModuleTests::ActualArgumentOwnerAndArrayIndex() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
    const auto owner = project.triggerDocument_->triggers[0].id;
    const auto added = Trigger::TriggerDocumentEditor::AppendRootEca(*project.triggerDocument_, *project.uiCatalog_, owner,
        Trigger::UiDefinitionKind::Action, "ValueAction"); QVERIFY(added.Succeeded());
    auto* eca = Trigger::TriggerDocumentEditor::FindEca(*project.triggerDocument_, added.value->rootEcaId); QVERIFY(eca && eca->arguments.size() == 1);
    const auto ecaId = eca->id; const auto argumentIndex = eca->arguments[0];
    const auto argumentId = project.triggerDocument_->argumentNodes[argumentIndex].id;
    Host host; Modules::TriggerModule module(project); module.AttachHost(&host); auto* page = module.Widget();
    QVERIFY(module.ShowGlobalCustomCode()); // 已选中同一个 trigger 时没有 currentChanged，仍须明确离开地图头。
    QVERIFY(module.Reveal(Parameter(owner, ecaId, argumentId))); QCOMPARE(SelectedEca(page), ecaId);
    QCOMPARE(module.CaptureNavigationBookmark()->value("mode").toString(), QString("traditional"));
    QVERIFY(host.status.contains(QString::number(argumentId))); QVERIFY(!QApplication::activeModalWidget());
    Trigger::ArgumentNode index; index.id = 9007199254740993ULL; index.type = 3; index.value.text = "7";
    QVERIFY(Trigger::TriggerDocumentEditor::AddVariable(*project.triggerDocument_, "OwnedArray", "integer", true).Succeeded());
    const auto indexSlot = static_cast<std::uint32_t>(project.triggerDocument_->argumentNodes.size());
    project.triggerDocument_->argumentNodes.push_back(index);
    project.triggerDocument_->argumentNodes[argumentIndex].type = 1;
    project.triggerDocument_->argumentNodes[argumentIndex].value.text = "OwnedArray";
    project.triggerDocument_->argumentNodes[argumentIndex].insertIndex = 1;
    project.triggerDocument_->argumentNodes[argumentIndex].indexArgument = indexSlot;
    QVERIFY(module.Reveal(Parameter(owner, ecaId, index.id))); QCOMPARE(SelectedEca(page), ecaId);
    QVERIFY(host.status.contains(QString::number(index.id))); QCOMPARE(project.triggerUndoStack_->count(), 0);
    const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
    project.triggerDocument_->argumentNodes[indexSlot].insertIndex = 1;
    project.triggerDocument_->argumentNodes[indexSlot].indexArgument = indexSlot;
    QVERIFY(!module.Reveal(Parameter(owner, ecaId, index.id)));
    const auto after = module.CaptureNavigationBookmark(); QVERIFY(after && *after == *before);
    QVERIFY(!QApplication::activeModalWidget()); QCOMPARE(project.triggerUndoStack_->count(), 0);
}
void SearchRevealModuleTests::NestedFunctionShowsContainingStatementWithoutModal() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
    const auto owner = project.triggerDocument_->triggers[0].id;
    const auto added = Trigger::TriggerDocumentEditor::AppendRootEca(*project.triggerDocument_, *project.uiCatalog_, owner,
        Trigger::UiDefinitionKind::Action, "ValueAction"); QVERIFY(added.Succeeded());
    const auto* rootEca = Trigger::TriggerDocumentEditor::FindEca(*project.triggerDocument_, added.value->rootEcaId); QVERIFY(rootEca);
    const auto rootId = rootEca->id, outerArgument = project.triggerDocument_->argumentNodes[rootEca->arguments[0]].id;
    const auto call = Trigger::TriggerDocumentEditor::AppendArgumentCall(*project.triggerDocument_, *project.uiCatalog_, outerArgument, "integer", "NestedValue");
    QVERIFY(call.Succeeded()); const auto* nested = Trigger::TriggerDocumentEditor::FindEca(*project.triggerDocument_, call.value->rootEcaId);
    QVERIFY(nested && nested->arguments.size() == 1);
    const auto nestedId = nested->id, argument = project.triggerDocument_->argumentNodes[nested->arguments[0]].id;
    Host host; Modules::TriggerModule module(project); module.AttachHost(&host); auto* page = module.Widget();
    QVERIFY(module.Reveal(Parameter(owner, nestedId, argument))); QCOMPARE(SelectedEca(page), rootId);
    QVERIFY(host.status.contains(QString::number(nestedId)) && host.status.contains(QString::number(argument))
        && host.status.contains(QStringLiteral("没有独立逻辑树行")));
    QVERIFY(!QApplication::activeModalWidget()); QCOMPARE(project.triggerUndoStack_->count(), 0);
}
void SearchRevealModuleTests::WrongOwnerPreservesPendingDraft() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
    const auto owner = project.triggerDocument_->triggers[0].id, other = project.triggerDocument_->triggers[1].id;
    const auto added = Trigger::TriggerDocumentEditor::AppendRootEca(*project.triggerDocument_, *project.uiCatalog_, owner,
        Trigger::UiDefinitionKind::Action, "ValueAction"); QVERIFY(added.Succeeded());
    const auto* eca = Trigger::TriggerDocumentEditor::FindEca(*project.triggerDocument_, added.value->rootEcaId); QVERIFY(eca);
    const auto ecaId = eca->id, argument = project.triggerDocument_->argumentNodes[eca->arguments[0]].id;
    Modules::TriggerModule module(project); (void)module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    auto* view = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(view); view->SetText("// author pending draft\n");
    QVERIFY(module.HasPendingCodeDraft()); const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
    const auto body = project.triggerDocument_->wct.customCode.text;
    QVERIFY(!module.Reveal(Parameter(other, ecaId, argument))); QVERIFY(module.HasPendingCodeDraft());
    auto bad = Parameter(owner, ecaId, argument); bad.extra["argumentId"] = 1; QVERIFY(!module.Reveal(bad));
    bad = Parameter(owner, ecaId, argument); bad.extra["argumentId"] = QStringLiteral("01"); QVERIFY(!module.Reveal(bad));
    bad = Parameter(owner, ecaId, 999999); QVERIFY(!module.Reveal(bad));
    const auto after = module.CaptureNavigationBookmark(); QVERIFY(after && *after == *before);
    QCOMPARE(view->toPlainText(), QString("// author pending draft\n")); QCOMPARE(project.triggerDocument_->wct.customCode.text, body);
    QCOMPARE(project.triggerUndoStack_->count(), 0); QVERIFY(!QApplication::activeModalWidget());
}
void SearchRevealModuleTests::ExplicitObjectSourceCannotFallback() {
    QtProject::EditorProject project; QVERIFY(InstallObjects(project)); Modules::ObjectModule module(project);
    (void)module.Widget(); module.UpdateObjectReferenceEntries(); module.SelectFirstType();
    QVERIFY(module.Reveal({"objects", "object", "unit:hfoo", {{"source", "standard"}}}));
    auto state = module.CaptureNavigationBookmark(); QVERIFY(state); QCOMPARE(state->value("source").toString(), QString("standard"));
    QVERIFY(module.Reveal({"objects", "object", "unit:hfoo", {{"source", "map"}}}));
    state = module.CaptureNavigationBookmark(); QVERIFY(state); QCOMPARE(state->value("source").toString(), QString("map"));
    const auto fingerprint = project.objectDocuments_[0]->DocumentFingerprint();
    QVERIFY(!module.Reveal({"objects", "object", "badtype:hfoo", {{"source", "standard"}}}));
    QVERIFY(!module.Reveal({"objects", "object", "unit:hfoo", {{"source", 1}}}));
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), fingerprint);
    module.DetachDocuments(); project.objectDocuments_[0].reset();
    QVERIFY(module.Reveal({"objects", "object", "unit:hfoo", {{"source", "standard"}}}));
    const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
    QVERIFY(!module.Reveal({"objects", "object", "unit:hfoo", {{"source", "map"}}}));
    const auto after = module.CaptureNavigationBookmark(); QVERIFY(after && *after == *before);
    QVERIFY(module.Reveal({"objects", "object", "unit:hfoo", {}})); // 未指定来源仍保留旧目录兼容入口。
}

void SearchRevealModuleTests::ExactAssetIdentityRejectsConflictsBeforeViewChanges() {
    QtProject::EditorProject project; QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(InstallAssets(project, root));
    Modules::AssetsModule module(project); (void)module.Widget();
    const auto script = QStringLiteral("Lua\\authored.lua"), other = QStringLiteral("Textures\\Other.blp");
    QVERIFY(module.Reveal(Asset(script, script)));
    const auto selected = module.CaptureNavigationBookmark(); QVERIFY(selected);
    QCOMPARE(selected->value("current_path").toString(), script);
    QVERIFY(module.Reveal({"assets", "file", other, {}})); // 旧无extra入口保留。
    const auto baseline = module.CaptureNavigationBookmark(); QVERIFY(baseline);
    const int undo = project.AssetUndoStack()->count(); const auto sourceSha = project.archiveSession_->SourceSha256();
    auto bad = Asset(script, script); bad.extra.remove("sourceIdentity");
    std::vector<Shell::NavigationTarget> invalid{bad, Asset(script, "wrong-origin"), Asset("Lua\\AUTHORED.lua", script)};
    bad = Asset(script, script); bad.extra["sourceIdentity"] = 7; invalid.push_back(bad);
    bad = Asset(script, script); bad.extra["stableIdentity"] = 1; invalid.push_back(bad);
    bad = Asset(script, script); bad.extra["stableIdentity"] = false; invalid.push_back(bad);
    bad = Asset(script, script); bad.kind = "folder"; invalid.push_back(bad);
    for (const auto& target : invalid) {
        QVERIFY(!module.Reveal(target)); const auto after = module.CaptureNavigationBookmark();
        QVERIFY(after && *after == *baseline); QCOMPARE(project.AssetUndoStack()->count(), undo);
    }
    // 接受producer持有的真实当前路径拼法；只在显示层规范斜杠，不把旧别名当当前身份。
    const auto slash = QStringLiteral("Lua/authored.lua");
    project.assetEdits_.push_back({script, slash}); project.NotifyImportsChanged();
    QVERIFY(module.Reveal(Asset(slash, script))); const auto displayed = module.CaptureNavigationBookmark(); QVERIFY(displayed);
    QCOMPARE(displayed->value("current_path").toString(), script);
    QVERIFY(!module.Reveal(Asset(script, script)));
    const auto afterAlias = module.CaptureNavigationBookmark(); QVERIFY(afterAlias && *afterAlias == *displayed);
    project.assetEdits_.clear(); project.NotifyImportsChanged(); QVERIFY(module.Reveal({"assets", "file", other, {}}));
    project.assetEdits_.push_back({QStringLiteral("Textures\\Conflict.blp"), script}); project.NotifyImportsChanged();
    const auto collided = module.CaptureNavigationBookmark(); QVERIFY(collided);
    const auto entries = project.AssetEntries();
    QVERIFY(std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.path == script && entry.ambiguous; }));
    QVERIFY(!module.Reveal(Asset(script, script)));
    const auto afterConflict = module.CaptureNavigationBookmark(); QVERIFY(afterConflict && *afterConflict == *collided);
    QCOMPARE(project.AssetUndoStack()->count(), undo); QCOMPARE(project.archiveSession_->SourceSha256(), sourceSha);
    QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
}

void SearchRevealModuleTests::MapSizeBridgeUsesCurrentOriginalRenamedAndPendingSources() {
    using Origin = Services::MapSizeRowOrigin;
    QtProject::EditorProject project; QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(InstallAssets(project, root));
    Modules::AssetsModule module(project); (void)module.Widget();
    const auto script = QStringLiteral("Lua\\authored.lua"), other = QStringLiteral("Textures\\Other.blp");
    const auto renamedPath = QStringLiteral("Lua/Renamed.lua"), pendingPath = QStringLiteral("New/Second.lua");
    const auto sourceSha = project.archiveSession_->SourceSha256(); const auto undo = project.AssetUndoStack()->count();
    QVERIFY(module.Reveal({"assets", "file", other, {}}));
    auto report = AssetSizeReport(project); QVERIFY(report.Succeeded() && report.value);
    auto original = SizeRow(*report.value, script); QVERIFY(original && original->navigationAllowed);
    QCOMPARE(original->origin, Origin::Original); QVERIFY(original->identity.starts_with("member/"));
    const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
    QVERIFY(!module.Reveal(Asset(script, QString::fromStdString(original->identity)))); // 统计身份不能当资源来源身份。
    const auto afterOldIdentity = module.CaptureNavigationBookmark(); QVERIFY(afterOldIdentity && *afterOldIdentity == *before);
    auto target = QtApp::MakeMapSizeAssetNavigationTarget(*original, project.AssetEntries()); QVERIFY(target);
    QCOMPARE(target->extra.value("sourceIdentity").toString(), script);
    QVERIFY(module.Reveal(*target)); QCOMPARE(module.CaptureNavigationBookmark()->value("current_path").toString(), script);

    project.assetEdits_.push_back({script, renamedPath}); project.NotifyImportsChanged();
    QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(*original, project.AssetEntries()));
    report = AssetSizeReport(project); QVERIFY(report.Succeeded() && report.value);
    auto renamed = SizeRow(*report.value, renamedPath); QVERIFY(renamed && renamed->navigationAllowed);
    QCOMPARE(renamed->origin, Origin::Renamed); QCOMPARE(renamed->originalPath, original->originalPath);
    QCOMPARE(renamed->identity, original->identity); // 改名只改变当前路径，保统计来源身份。
    target = QtApp::MakeMapSizeAssetNavigationTarget(*renamed, project.AssetEntries()); QVERIFY(target);
    QCOMPARE(target->id, renamedPath); QCOMPARE(target->extra.value("sourceIdentity").toString(), script);
    QVERIFY(module.Reveal(*target));
    QCOMPARE(module.CaptureNavigationBookmark()->value("current_path").toString(), QStringLiteral("Lua\\Renamed.lua"));

    const auto payload = [](std::string_view text) {
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        return std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
    };
    const auto replacementBytes = payload("-- owned replacement\nreturn 3\n");
    project.pendingImports_.push_back({renamedPath, {}, static_cast<qint64>(replacementBytes->size()), replacementBytes});
    project.NotifyImportsChanged(); QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(*renamed, project.AssetEntries()));
    report = AssetSizeReport(project); QVERIFY(report.Succeeded() && report.value);
    auto replacement = SizeRow(*report.value, renamedPath); QVERIFY(replacement && replacement->navigationAllowed);
    QCOMPARE(replacement->origin, Origin::PendingReplacement); QCOMPARE(replacement->identity, original->identity);
    QCOMPARE(replacement->originalLogicalBytes, original->originalLogicalBytes);
    QCOMPARE(replacement->currentLogicalBytes, std::optional<std::uint64_t>{static_cast<std::uint64_t>(replacementBytes->size())});
    target = QtApp::MakeMapSizeAssetNavigationTarget(*replacement, project.AssetEntries()); QVERIFY(target);
    QCOMPARE(target->extra.value("sourceIdentity").toString(), QStringLiteral("pending/") + renamedPath);
    QVERIFY(module.Reveal(*target));

    const auto importBytes = payload("-- owned import\nreturn 4\n");
    project.pendingImports_.push_back({pendingPath, {}, static_cast<qint64>(importBytes->size()), importBytes});
    project.NotifyImportsChanged(); report = AssetSizeReport(project); QVERIFY(report.Succeeded() && report.value);
    auto imported = SizeRow(*report.value, pendingPath); QVERIFY(imported && imported->navigationAllowed);
    QCOMPARE(imported->origin, Origin::PendingImport); QVERIFY(imported->originalPath.empty());
    target = QtApp::MakeMapSizeAssetNavigationTarget(*imported, project.AssetEntries()); QVERIFY(target);
    QCOMPARE(target->extra.value("sourceIdentity").toString(), QStringLiteral("pending/") + pendingPath);
    QVERIFY(target->extra.value("sourceIdentity").toString() != QString::fromStdString(imported->identity));
    QVERIFY(module.Reveal(*target));
    const auto selected = module.CaptureNavigationBookmark(); QVERIFY(selected);
    QCOMPARE(selected->value("current_path").toString(), QStringLiteral("New\\Second.lua"));

    auto invalid = *imported; invalid.originalPath = "different-origin.lua";
    QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(invalid, project.AssetEntries()));
    invalid = *imported; invalid.currentPath = "New/SECOND.lua";
    QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(invalid, project.AssetEntries()));
    invalid = *imported; invalid.currentLogicalBytes = *invalid.currentLogicalBytes + 1;
    QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(invalid, project.AssetEntries()));
    invalid = *imported; invalid.trust = Services::MapSizeByteTrust::Unknown;
    QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(invalid, project.AssetEntries()));
    auto duplicateEntries = project.AssetEntries();
    const auto matching = std::find_if(duplicateEntries.begin(), duplicateEntries.end(), [&](const auto& entry) { return entry.path == pendingPath; });
    QVERIFY(matching != duplicateEntries.end()); const auto duplicate = *matching; duplicateEntries.push_back(duplicate);
    QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(*imported, duplicateEntries));
    project.assetEdits_.push_back({other, renamedPath}); project.NotifyImportsChanged();
    const auto conflictView = module.CaptureNavigationBookmark(); QVERIFY(conflictView);
    QVERIFY(!QtApp::MakeMapSizeAssetNavigationTarget(*replacement, project.AssetEntries()));
    const auto afterConflict = module.CaptureNavigationBookmark(); QVERIFY(afterConflict && *afterConflict == *conflictView);
    QCOMPARE(project.AssetUndoStack()->count(), undo); QCOMPARE(project.archiveSession_->SourceSha256(), sourceSha);
    QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
}

void SearchRevealModuleTests::ActualLuaDraftLeaveInvalidatesOldAssetIdentityWithoutHistoryChange() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(root.isValid());
    QVERIFY(InstallTriggers(project, root)); QVERIFY(InstallAssets(project, root));
    const auto script = QStringLiteral("Lua\\authored.lua"), other = QStringLiteral("Textures\\Other.blp");
    Shell::ModuleRegistry registry;
    auto triggerModule = std::make_unique<Modules::TriggerModule>(project); auto* triggers = triggerModule.get();
    auto assetModule = std::make_unique<Modules::AssetsModule>(project); auto* assets = assetModule.get();
    QVERIFY(registry.Register(std::move(triggerModule))); QVERIFY(registry.Register(std::move(assetModule)));
    Shell::ShellWindow shell(registry, nullptr); shell.ActivateModule("triggers");
    QVERIFY(shell.TryNavigate({"assets", "file", other, {}}));
    QVERIFY(shell.TryNavigate({"triggers", "trigger", QString::number(project.triggerDocument_->triggers.front().id), {{"stableIdentity", true}}}));
    QVERIFY(shell.GoBack()); QVERIFY(shell.ForwardAction()->isEnabled());
    shell.ActivateModule("triggers");
    QVERIFY(triggers->Reveal({"triggers", "lua-file", script, {}}));
    const auto sizeBeforeLeave = AssetSizeReport(project); QVERIFY(sizeBeforeLeave.Succeeded() && sizeBeforeLeave.value);
    const auto oldSizeRow = SizeRow(*sizeBeforeLeave.value, script); QVERIFY(oldSizeRow);
    const auto bridgeTargetBeforeLeave = QtApp::MakeMapSizeAssetNavigationTarget(*oldSizeRow, project.AssetEntries());
    QVERIFY(bridgeTargetBeforeLeave && bridgeTargetBeforeLeave->extra.value("sourceIdentity").toString() == script);
    auto* lua = triggers->LuaScripts(); QVERIFY(lua && lua->Editor());
    const QString draft = QStringLiteral("-- authored pending draft\nreturn 2\n"); lua->Editor()->SetText(draft);
    QVERIFY(lua->HasPendingDraft()); QVERIFY(project.pendingImports_.empty());
    const auto sourceSha = project.archiveSession_->SourceSha256(); const int undo = project.AssetUndoStack()->count();
    const bool back = shell.BackAction()->isEnabled(), forward = shell.ForwardAction()->isEnabled();
    QVERIFY(forward); std::optional<QJsonObject> afterLegitimateRefresh; int sourceChanges = 0;
    // Assets的正常ImportsChanged刷新已先接线；记录其更新后、Reveal尚未调用的视图。
    const auto connection = connect(&project, &QtProject::EditorProject::ImportsChanged, &shell, [&] {
        ++sourceChanges; afterLegitimateRefresh = assets->CaptureNavigationBookmark();
    });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
    QVERIFY(!shell.TryNavigate(*bridgeTargetBeforeLeave)); // 生产adapter之后的真实Leave提交仍必须由目标端拒绝旧来源。
    QVERIFY(sourceChanges > 0 && afterLegitimateRefresh); QVERIFY(!lua->HasPendingDraft());
    QVERIFY(shell.CurrentModule() == std::optional<Shell::ModuleId>{"triggers"});
    QCOMPARE(shell.BackAction()->isEnabled(), back); QCOMPARE(shell.ForwardAction()->isEnabled(), forward);
    const auto afterRefusal = assets->CaptureNavigationBookmark(); QVERIFY(afterRefusal && *afterRefusal == *afterLegitimateRefresh);
    const auto entries = project.AssetEntries();
    const auto replacement = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.path == script; });
    QVERIFY(replacement != entries.end() && replacement->Pending() && replacement->ownedPayload && !replacement->ambiguous);
    const auto utf8 = draft.toUtf8();
    const auto expected = std::as_bytes(std::span(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    QVERIFY(std::equal(expected.begin(), expected.end(), replacement->ownedPayload->begin(), replacement->ownedPayload->end()));
    QCOMPARE(lua->Editor()->toPlainText(), draft); QCOMPARE(project.AssetUndoStack()->count(), undo + 1);
    const auto sizeAfterLeave = AssetSizeReport(project); QVERIFY(sizeAfterLeave.Succeeded() && sizeAfterLeave.value);
    const auto pendingSizeRow = SizeRow(*sizeAfterLeave.value, script); QVERIFY(pendingSizeRow);
    const auto bridgeTargetAfterLeave = QtApp::MakeMapSizeAssetNavigationTarget(*pendingSizeRow, project.AssetEntries());
    QVERIFY(bridgeTargetAfterLeave && bridgeTargetAfterLeave->extra.value("sourceIdentity").toString() == QStringLiteral("pending/") + script);
    QVERIFY(assets->Reveal(*bridgeTargetAfterLeave));
    const auto selected = assets->CaptureNavigationBookmark(); QVERIFY(selected);
    QCOMPARE(selected->value("current_path").toString(), script);
    QCOMPARE(project.archiveSession_->SourceSha256(), sourceSha); QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
}
QTEST_MAIN(SearchRevealModuleTests)
#include "SearchRevealModuleTests.moc"
