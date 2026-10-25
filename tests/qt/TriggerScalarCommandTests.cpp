// 原创小型 CPU/Qt 撤销链夹具；不加载原图、游戏或原版编辑器。
#include "EditorProject.h"
#include "ProjectOpen.h"
#include "ProjectSaveJob.h"
#include "War3MapCore/MapArchiveSession.h"
#include "TriggerCommands.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace War3Map;
using namespace War3Map::Trigger;
using War3Map::QtProject::EditorProject;
using War3Map::QtProject::TriggerGraphSnapshotUndoCommand;
using War3Map::QtProject::AssetState;
using War3Map::QtProject::PublishTriggerGraphOperationsWithAssets;
using War3Map::QtProject::PrepareProjectOpen;

namespace {
struct Wire {
    std::vector<std::byte> bytes;
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 255U));
    }
    void CString(std::string_view value) {
        for (const char byte : value) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
        bytes.push_back(std::byte{});
    }
};
constexpr std::string_view kGbText = "\xB2\xE2\xCA\xD4"; // GB18030/GBK 的“测试”，原创源字节。
std::vector<std::byte> Wtg() {
    Wire w;
    for (const char byte : std::string_view("WTG!")) w.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    w.I32(7); w.I32(1); w.I32(1); w.CString(kGbText); w.I32(0); w.I32(2); w.I32(2);
    for (const auto name : {"OldText", "NextText"}) {
        w.CString(name); w.CString("string"); w.I32(1); w.I32(0); w.I32(1); w.I32(0); w.CString("");
    }
    w.I32(1); w.CString("Owned GUI"); w.CString(kGbText);
    w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1); w.I32(300);
    for (int index = 0; index < 300; ++index) {
        w.I32(2); w.CString("UseText"); w.I32(1);
        w.I32(3); w.CString(kGbText); w.I32(0); w.I32(0); w.I32(0);
    }
    return std::move(w.bytes);
}
std::vector<std::byte> Wct() {
    Wire w; w.I32(1); w.CString(kGbText);
    w.I32(1); w.CString(""); // 全局空槽 length=1+NUL，触发器槽 length=0。
    w.I32(1); w.I32(0); return std::move(w.bytes);
}
template<class T> T Take(Result<T>&& result) {
    if (!result.Succeeded() || !result.value) {
        std::string message;
        for (const auto& diagnostic : result.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    Require(QDir().mkpath(QString::fromStdWString(path.parent_path().wstring())), "owned fixture parent");
    QFile file(QString::fromStdWString(path.wstring()));
    Require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "owned fixture new file");
    Require(file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size()))
        == static_cast<qint64>(bytes.size()), "owned fixture write");
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, std::as_bytes(std::span(text.data(), text.size()))); }
bool SameArgument(const ArgumentNode& left, const ArgumentNode& right) {
    return left.id == right.id && left.type == right.type && left.insertCall == right.insertCall && left.callEca == right.callEca
        && left.insertIndex == right.insertIndex && left.indexArgument == right.indexArgument
        && left.value.text == right.value.text && left.value.encoding == right.value.encoding && left.value.originalBytes == right.value.originalBytes
        && left.origin.source == right.origin.source && left.origin.offset == right.origin.offset && left.origin.length == right.origin.length
        && left.origin.fingerprint == right.origin.fingerprint;
}
struct Fixture {
    QTemporaryDir temp;
    std::unique_ptr<EditorProject> project;
    std::unique_ptr<UiCatalog> originalCatalog;
    TriggerDocument originalDocument;
    EncodedTriggerDocument original;
    std::filesystem::path root;
    std::string sourceWtgSha, sourceWctSha;
    QString error;
    bool Initialize() {
        try {
            Require(temp.isValid(), "owned temp"); root = std::filesystem::path(temp.path().toStdWString());
            const auto ui = root / "catalog";
            Write(ui / "ui/config", "base\n");
            Write(ui / "ui/base/define.txt", "[TriggerTypes]\nstring=0,1,1,String,string\n");
            Write(ui / "ui/base/action.txt", "[UseText]\ndescription=Use ${value}\n[[.args]]\ntype=string\n");
            auto catalog = Take(UiCatalog::Load(ui));
            const auto sourceWtg = Wtg(), sourceWct = Wct();
            auto document = Take(WtgWctCodec{}.Parse(sourceWtg, sourceWct, catalog));
            original = Take(WtgWctCodec{}.Serialize(document, catalog));
            Require(original.wtg == sourceWtg && original.wct == sourceWct, "fixture byte-exact codec roundtrip");
            originalDocument = document; originalCatalog = std::make_unique<UiCatalog>(catalog);
            Write(root / "source/map/war3map.wtg", original.wtg); Write(root / "source/map/war3map.wct", original.wct);
            sourceWtgSha = Take(Sha256File(root / "source/map/war3map.wtg"));
            sourceWctSha = Take(Sha256File(root / "source/map/war3map.wct"));
            project = std::make_unique<EditorProject>();
            project->projectRoot_ = root / "source";
            project->directorySnapshot_ = std::make_unique<DirectoryWorkspace>(Take(DirectoryWorkspace::Open(project->projectRoot_)));
            project->uiDataRoot_ = project->activeUiDataRoot_ = ui;
            project->uiCatalog_ = std::make_unique<UiCatalog>(std::move(catalog));
            project->triggerDocument_ = std::make_unique<TriggerDocument>(std::move(document));
            project->triggerUndoStack_ = new QUndoStack(project.get());
            project->ResetEditTracking();
            Require(project->editTrackingReady_ && !project->editTrackingFailed_, "actual project edit baseline");
            return true;
        } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); return false; }
    }
    TriggerGraphEditBatch Batch(std::size_t count = 256) const {
        Require(count > 0 && count <= 256, "test batch hard256");
        TriggerGraphEditBatch batch;
        batch.expected = Take(CaptureTriggerGraphBinding(*project->triggerDocument_, *project->uiCatalog_));
        for (std::size_t index = 0; index < count; ++index) {
            const auto& node = project->triggerDocument_->argumentNodes[index];
            batch.operations.emplace_back(GraphSet{{GraphRecordKind::Argument, node.id, {}}, GraphField::ArgumentScalar,
                GraphScalarValue{node.type, node.value.text}, GraphScalarValue{1, "NextText"}});
        }
        return batch;
    }
    TriggerGraphScalarEditReceipt Prepare() const {
        return Take(PrepareTriggerGraphScalarEditBatch(*project->triggerDocument_, *project->uiCatalog_, Batch()));
    }
    EncodedTriggerDocument Pair() const { return Take(WtgWctCodec{}.Serialize(*project->triggerDocument_, *originalCatalog)); }
    bool OriginalBytes() const { const auto encoded = Pair(); return encoded.wtg == original.wtg && encoded.wct == original.wct; }
    bool SourcesUnchanged() const {
        return Take(Sha256File(root / "source/map/war3map.wtg")) == sourceWtgSha
            && Take(Sha256File(root / "source/map/war3map.wct")) == sourceWctSha;
    }
    Services::MapRuntimePackageSource RuntimePackage() {
        using namespace Services;
        const auto packageRoot = root / "runtime-package";
        QJsonArray files;
        const auto add = [&](const char* path, const char* member, const char* role, const char* text) {
            Write(packageRoot / path, text);
            const auto sha = Take(Sha256File(packageRoot / path));
            files.append(QJsonObject{{"path",path},{"member",member},{"role",role},
                {"size",static_cast<qint64>(std::string_view(text).size())},{"sha256",QString::fromStdString(sha)}});
        };
        add("boot.blob","warvk_loader_raw.ai","bootstrap","original opaque boot fixture");
        add("payload.blob","warvk_runtime.blp","payload","original opaque payload fixture");
        add("notice.txt","audastra/runtime/licenses/fixture.txt","notice","original source notice");
        add("client.j","","client","original synthetic client");
        const QJsonObject manifest{{"schema",1},{"id","original-test"},{"version","1.0.0"},{"abi",0x20000},
            {"scope","internal"},{"gameSha256","e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a"},{"files",files}};
        Write(packageRoot / "manifest.json",QJsonDocument(manifest).toJson(QJsonDocument::Compact).toStdString());
        return {packageRoot,Take(Sha256File(packageRoot / "manifest.json")),false};
    }
};
} // namespace

class TriggerScalarCommandTests final : public QObject {
    Q_OBJECT
private slots:
    void Actual256OneUndoPreservesGbBytesAndUnselectedFields();
    void StaleContextOrMetadataRefusesWholePublication_data();
    void StaleContextOrMetadataRefusesWholePublication();
    void StaleMetadataAfterApplyRefusesUndo();
    void DefaultReceiptAndDeletedProjectDoNotPublish();
    void GeneralSnapshotModeStillRoundTrips();
    void ScalarReplayPreservesFailedTrackingAndUndo();
    void ScalarUiNoticeIsScopedSingleConsumerAndActualIndexBound();
    void ScalarUiNoticeRejectsMacroAndNestedGenericNotification();
    void ScalarUiScopeSurvivesOwnerDeletionAfterPublication();
    void GraphAndAssetsPublishInOneRevisionAndUndo();
    void GraphAndAssetsPreparationFailureChangesNeither();
    void GraphAndAssetsRejectStaleUndo();
    void GraphAndAssetsRejectFailedObservation();
    void AssetOnlyGraphTransactionAndImmutablePayload();
    void RuntimeConsentSharesGraphRevisionAndUndo();
    void RuntimeConsentRejectsMissingPackageAndStaleRevision();
    void RuntimeConsentSurvivesProjectSaveAndBindsSource();
};

void TriggerScalarCommandTests::Actual256OneUndoPreservesGbBytesAndUnselectedFields() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto beforeIdentity = Take(CaptureTriggerGraphReplayIdentity(*f.project->triggerDocument_, *f.project->uiCatalog_));
    const auto receipt = f.Prepare(); QVERIFY(!receipt.Empty()); QCOMPARE(receipt.Receipt().appliedOperations, std::size_t{256});
    QVERIFY(f.OriginalBytes()); QVERIFY(f.SourcesUnchanged());
    QVERIFY(VerifyTriggerGraphScalarReplay(*f.project->triggerDocument_, *f.project->uiCatalog_, receipt, false).Succeeded());
    QSignalSpy changes(f.project.get(), &EditorProject::TriggerDocumentChanged);
    const auto documentAddress = f.project->triggerDocument_.get();
    const auto generation = f.project->editRevision_.Generation();
    const auto initialRevision = f.project->editRevision_.Revision();
    const auto scopesMatchActualEncoding = [&] {
        const auto captured = f.project->CaptureEditScopes();
        return captured && *captured == f.project->editRevision_.Current();
    };
    QVERIFY(scopesMatchActualEncoding()); QVERIFY(!f.project->triggerDirty_);
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, receipt, QStringLiteral("Owned256"));
    QVERIFY(command->Ready()); const auto published = command->FirstPublication(); QVERIFY(!*published);
    f.project->triggerUndoStack_->push(command.release());
    QVERIFY(*published); QCOMPARE(f.project->triggerUndoStack_->count(), 1); QCOMPARE(changes.count(), 1);
    QCOMPARE(f.project->triggerDocument_.get(), documentAddress); QCOMPARE(f.project->editRevision_.Generation(), generation);
    QVERIFY(f.project->editRevision_.Revision() > initialRevision);
    QVERIFY(scopesMatchActualEncoding()); QVERIFY(f.project->triggerDirty_);
    const auto& after = *f.project->triggerDocument_;
    QVERIFY(after.HasTrustedSourceSeal()); QVERIFY(after.SourceWtgSha256() == f.sourceWtgSha);
    QVERIFY(after.SourceWctSha256() == f.sourceWctSha);
    for (std::size_t index = 0; index < after.argumentNodes.size(); ++index) {
        const auto& actual = after.argumentNodes[index]; const auto& old = f.originalDocument.argumentNodes[index];
        if (index < 256) {
            QCOMPARE(old.value.encoding, TextEncoding::Gb18030); QVERIFY(!old.value.originalBytes.empty());
            auto expected = old; expected.type = 1; expected.value.text = "NextText"; expected.value.originalBytes.clear();
            QVERIFY(SameArgument(actual, expected));
        } else QVERIFY(SameArgument(actual, old));
    }
    const auto edited = f.Pair(); QVERIFY(edited.wtg != f.original.wtg && edited.wct == f.original.wct);
    f.project->triggerUndoStack_->undo(); QCOMPARE(changes.count(), 2); QVERIFY(f.OriginalBytes());
    QVERIFY(scopesMatchActualEncoding()); QVERIFY(!f.project->triggerDirty_);
    QVERIFY(VerifyTriggerGraphReplayIdentity(*f.project->triggerDocument_, *f.project->uiCatalog_, beforeIdentity).Succeeded());
    for (std::size_t index = 0; index < f.originalDocument.argumentNodes.size(); ++index)
        QVERIFY(SameArgument(f.project->triggerDocument_->argumentNodes[index], f.originalDocument.argumentNodes[index]));
    f.project->triggerUndoStack_->redo(); QCOMPARE(changes.count(), 3);
    QVERIFY(scopesMatchActualEncoding()); QVERIFY(f.project->triggerDirty_);
    const auto redone = f.Pair(); QVERIFY(redone.wtg == edited.wtg && redone.wct == edited.wct);
    QVERIFY(f.SourcesUnchanged()); QCOMPARE(f.project->triggerDocument_.get(), documentAddress);
}

void TriggerScalarCommandTests::StaleContextOrMetadataRefusesWholePublication_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("stable-id-same-payload") << 0;
    QTest::newRow("encoding-same-payload") << 1;
    QTest::newRow("origin-same-payload") << 2;
    QTest::newRow("original-bytes-same-payload") << 3;
    QTest::newRow("catalog-pointer") << 4;
    QTest::newRow("document-pointer") << 5;
    QTest::newRow("source-suspected") << 6;
    QTest::newRow("source-confirmed") << 7;
    QTest::newRow("edit-generation") << 8;
    QTest::newRow("catalog-sha-same-pointer") << 9;
}
void TriggerScalarCommandTests::StaleContextOrMetadataRefusesWholePublication() {
    QFETCH(int, kind);
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); const auto receipt = f.Prepare();
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, receipt, QStringLiteral("Reject owned stale"));
    QVERIFY(command->Ready()); const auto published = command->FirstPublication();
    auto& document = *f.project->triggerDocument_;
    if (kind == 0) std::swap(document.argumentNodes[0].id, document.argumentNodes[1].id);
    if (kind == 1) document.argumentNodes[0].value.encoding = TextEncoding::Latin1;
    if (kind == 2) ++document.argumentNodes[0].origin.offset;
    if (kind == 3) document.argumentNodes[0].value.originalBytes.clear();
    if (kind == 4) f.project->uiCatalog_ = std::make_unique<UiCatalog>(*f.project->uiCatalog_);
    if (kind == 5) f.project->triggerDocument_ = std::make_unique<TriggerDocument>(document);
    if (kind == 6) f.project->sourceChangeSuspected_ = true;
    if (kind == 7) f.project->sourceChangeConfirmed_ = true;
    if (kind == 8) f.project->ResetEditTracking();
    if (kind == 9) {
        const auto changedUi = f.root / "changed-catalog";
        Write(changedUi / "ui/config", "base\n");
        Write(changedUi / "ui/base/define.txt", "[TriggerTypes]\nstring=0,1,1,String,string\n");
        Write(changedUi / "ui/base/action.txt", "[UseText]\ntitle=Changed catalog\ndescription=Use ${value}\n[[.args]]\ntype=string\n");
        *f.project->uiCatalog_ = Take(UiCatalog::Load(changedUi));
    }
    const auto mutated = f.Pair(); QVERIFY(mutated.wtg == f.original.wtg && mutated.wct == f.original.wct);
    const auto identity = Take(CaptureTriggerGraphReplayIdentity(*f.project->triggerDocument_, *f.originalCatalog));
    QSignalSpy changes(f.project.get(), &EditorProject::TriggerDocumentChanged);
    // Ready不把标量元数据误当已验资格；真正push必须经过Core整图门。
    if (kind <= 3) QVERIFY(command->Ready()); else QVERIFY(!command->Ready());
    f.project->triggerUndoStack_->push(command.release());
    QVERIFY(!*published); QCOMPARE(f.project->triggerUndoStack_->count(), 0); QCOMPARE(changes.count(), 0);
    QVERIFY(VerifyTriggerGraphReplayIdentity(*f.project->triggerDocument_, *f.originalCatalog, identity).Succeeded());
    const auto retained = f.Pair(); QVERIFY(retained.wtg == mutated.wtg && retained.wct == mutated.wct); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::StaleMetadataAfterApplyRefusesUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, f.Prepare(), QStringLiteral("Owned undo stale"));
    const auto published = command->FirstPublication(); f.project->triggerUndoStack_->push(command.release()); QVERIFY(*published);
    ++f.project->triggerDocument_->argumentNodes[299].origin.offset; // 未选参数也属于完整方向身份。
    const auto identity = Take(CaptureTriggerGraphReplayIdentity(*f.project->triggerDocument_, *f.project->uiCatalog_));
    const auto edited = f.Pair(); QSignalSpy changes(f.project.get(), &EditorProject::TriggerDocumentChanged);
    f.project->triggerUndoStack_->undo(); QCOMPARE(changes.count(), 0);
    QVERIFY(VerifyTriggerGraphReplayIdentity(*f.project->triggerDocument_, *f.project->uiCatalog_, identity).Succeeded());
    const auto retained = f.Pair(); QVERIFY(retained.wtg == edited.wtg && retained.wct == edited.wct); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::DefaultReceiptAndDeletedProjectDoNotPublish() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto empty = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, TriggerGraphScalarEditReceipt{}, QStringLiteral("Empty"));
    QVERIFY(!empty->Ready()); const auto emptyPublished = empty->FirstPublication(); f.project->triggerUndoStack_->push(empty.release());
    QVERIFY(!*emptyPublished); QCOMPARE(f.project->triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes());
    auto deleted = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, f.Prepare(), QStringLiteral("Deleted project"));
    QVERIFY(deleted->Ready()); const auto deletedPublished = deleted->FirstPublication();
    f.project.reset(); QVERIFY(!deleted->Ready());
    QUndoStack independentStack; independentStack.push(deleted.release());
    QVERIFY(!*deletedPublished); QCOMPARE(independentStack.count(), 0); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::GeneralSnapshotModeStillRoundTrips() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QObject consumer; bool unexpectedScalarNotice = false;
    connect(f.project.get(), &EditorProject::TriggerDocumentChanged, &consumer, [&] {
        unexpectedScalarNotice = unexpectedScalarNotice || static_cast<bool>(f.project->ActiveScalarUiNotice(
            f.project->triggerDocument_.get(), f.project->uiCatalog_.get(), f.project->editRevision_.Generation(),
            f.project->editRevision_.Revision(), &consumer));
    });
    auto before = *f.project->triggerDocument_, after = before;
    const auto applied = ApplyTriggerGraphEditBatch(after, *f.project->uiCatalog_, f.Batch(2));
    QVERIFY(applied.Succeeded());
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, std::move(before), std::move(after), QStringLiteral("Existing general mode"));
    QVERIFY(command->Ready()); const auto published = command->FirstPublication();
    f.project->triggerUndoStack_->push(command.release()); QVERIFY(*published); QCOMPARE(f.project->triggerUndoStack_->count(), 1);
    const auto edited = f.Pair(); f.project->triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes());
    f.project->triggerUndoStack_->redo(); const auto redone = f.Pair();
    QVERIFY(redone.wtg == edited.wtg && redone.wct == edited.wct); QVERIFY(f.SourcesUnchanged());
    QVERIFY(!unexpectedScalarNotice);
}

void TriggerScalarCommandTests::ScalarReplayPreservesFailedTrackingAndUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto originalScopes = f.project->editRevision_.Current();
    const auto originalRevision = f.project->editRevision_.Revision();
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, f.Prepare(), QStringLiteral("Unknown tracking"));
    QVERIFY(command->Ready()); const auto published = command->FirstPublication();
    f.project->editTrackingFailed_ = true;
    f.project->triggerUndoStack_->push(command.release());
    QVERIFY(*published); QCOMPARE(f.project->triggerUndoStack_->count(), 1);
    QVERIFY(f.project->editTrackingFailed_); QVERIFY(!f.OriginalBytes());
    QVERIFY(f.project->editRevision_.Current() == originalScopes);
    QCOMPARE(f.project->editRevision_.Revision(), originalRevision);
    // 未恢复未知的全工程状态，但实际 Core 变更仍保留整批 Undo。
    f.project->triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes());
    QVERIFY(f.project->editTrackingFailed_); QCOMPARE(f.project->triggerUndoStack_->count(), 1);
    QVERIFY(f.project->editRevision_.Current() == originalScopes); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::ScalarUiNoticeIsScopedSingleConsumerAndActualIndexBound() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QObject consumer, foreign;
    auto generation = f.project->editRevision_.Generation();
    auto revision = f.project->editRevision_.Revision();
    EditorProject::ScalarUiNotice pending, previous;
    int accepted = 0, indexAccepted = 0; bool rejected = true, publishedBeforeNotice = true;
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, f.Prepare(), QStringLiteral("Scoped UI"));
    const auto published = command->FirstPublication();
    connect(f.project.get(), &EditorProject::TriggerDocumentChanged, &consumer, [&] {
        publishedBeforeNotice = publishedBeforeNotice && *published;
        pending = f.project->ActiveScalarUiNotice(f.project->triggerDocument_.get(), f.project->uiCatalog_.get(),
            generation, revision, &consumer);
        if (pending) ++accepted;
        rejected = rejected && !f.project->ActiveScalarUiNotice(f.project->triggerDocument_.get(), f.project->uiCatalog_.get(),
            generation, revision, &foreign);
    });
    connect(f.project->triggerUndoStack_, &QUndoStack::indexChanged, &consumer, [&](int index) {
        if (f.project->ScalarUiNoticeMatchesIndex(pending, f.project->triggerUndoStack_, index, &consumer)) ++indexAccepted;
        rejected = rejected && !f.project->ScalarUiNoticeMatchesIndex(pending, f.project->triggerUndoStack_, index, &foreign)
            && !f.project->ScalarUiNoticeMatchesIndex(previous, f.project->triggerUndoStack_, index, &consumer);
        previous = pending; generation = f.project->editRevision_.Generation(); revision = f.project->editRevision_.Revision();
    });
    QVERIFY(!f.project->ActiveScalarUiNotice(f.project->triggerDocument_.get(), f.project->uiCatalog_.get(), generation, revision, &consumer));
    f.project->triggerUndoStack_->push(command.release()); f.project->triggerUndoStack_->undo(); f.project->triggerUndoStack_->redo();
    QCOMPARE(accepted, 3); QCOMPARE(indexAccepted, 3); QVERIFY(rejected); QVERIFY(publishedBeforeNotice);
    QVERIFY(!f.project->ActiveScalarUiNotice(f.project->triggerDocument_.get(), f.project->uiCatalog_.get(), generation, revision, &consumer));
    f.project->triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes()); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::ScalarUiNoticeRejectsMacroAndNestedGenericNotification() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QObject consumer;
    EditorProject::ScalarUiNotice pending; bool nested = false, nestedRejected = false, macroRejected = false;
    const auto generation = f.project->editRevision_.Generation(), revision = f.project->editRevision_.Revision();
    connect(f.project.get(), &EditorProject::TriggerDocumentChanged, &consumer, [&] {
        const auto notice = f.project->ActiveScalarUiNotice(f.project->triggerDocument_.get(), f.project->uiCatalog_.get(),
            generation, revision, &consumer);
        if (nested) { nestedRejected = !notice; return; }
        pending = notice; nested = true; emit f.project->TriggerDocumentChanged(); nested = false;
    });
    connect(f.project->triggerUndoStack_, &QUndoStack::indexChanged, &consumer, [&](int index) {
        macroRejected = !f.project->ScalarUiNoticeMatchesIndex(pending, f.project->triggerUndoStack_, index, &consumer);
    });
    f.project->triggerUndoStack_->beginMacro(QStringLiteral("Actual outer macro"));
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, f.Prepare(), QStringLiteral("Scalar child"));
    const auto published = command->FirstPublication(); f.project->triggerUndoStack_->push(command.release());
    f.project->triggerUndoStack_->endMacro();
    QVERIFY(*published); QVERIFY(pending); QVERIFY(nestedRejected); QVERIFY(macroRejected);
    QCOMPARE(f.project->triggerUndoStack_->count(), 1); f.project->triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes());
}

void TriggerScalarCommandTests::ScalarUiScopeSurvivesOwnerDeletionAfterPublication() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QObject consumer;
    auto command = std::make_unique<TriggerGraphSnapshotUndoCommand>(*f.project, f.Prepare(), QStringLiteral("Delete receiver owner"));
    const auto published = command->FirstPublication(); bool sawPublication = false;
    connect(f.project.get(), &EditorProject::TriggerDocumentChanged, &consumer, [&] {
        sawPublication = *published; f.project.reset();
    });
    // 独立持有 command，避免在 QUndoStack::push 内删除正在执行的所属 stack。
    command->redo(); QVERIFY(sawPublication); QVERIFY(*published); QVERIFY(!f.project); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::GraphAndAssetsPublishInOneRevisionAndUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto revision = f.project->editRevision_.Revision();
    const auto bytes = std::make_shared<const std::vector<std::byte>>(4, std::byte{42});
    bool signalsConsistent = true;
    connect(f.project.get(), &EditorProject::TriggerDocumentChanged, f.project.get(), [&] {
        const auto actual = f.project->CaptureEditScopes();
        signalsConsistent = signalsConsistent && actual && *actual == f.project->editRevision_.Current();
    });
    const auto edited = War3Map::QtProject::PublishTriggerGraphOperationsWithAssets(*f.project,
        f.Batch(1).operations, QStringLiteral("Graph and resources"), [&](auto& state) {
            std::vector<EditorProject::ImportPayload> payloads{{"audastra/runtime/settings.json", bytes}};
            const auto problem = f.project->PrepareImportPayloads(state, payloads, false);
            return problem.isEmpty() ? Status::Ok() : Status::Failure("test", "PREPARE_FAILED", problem.toStdString());
        });
    QVERIFY(edited.Succeeded()); QVERIFY(!f.OriginalBytes());
    QCOMPARE(f.project->CaptureAssets().imports.size(), std::size_t{1});
    QCOMPARE(f.project->triggerUndoStack_->count(), 1); QCOMPARE(f.project->AssetUndoStack()->count(), 0);
    QCOMPARE(f.project->editRevision_.Revision(), revision + 1); QVERIFY(signalsConsistent);
    f.project->triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes()); QVERIFY(f.project->CaptureAssets().imports.empty());
    QCOMPARE(f.project->editRevision_.Revision(), revision + 2); QVERIFY(signalsConsistent);
    f.project->triggerUndoStack_->redo(); QVERIFY(!f.OriginalBytes());
    QCOMPARE(f.project->CaptureAssets().imports.size(), std::size_t{1});
    QCOMPARE(f.project->editRevision_.Revision(), revision + 3); QVERIFY(signalsConsistent); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::GraphAndAssetsPreparationFailureChangesNeither() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto revision = f.project->editRevision_.Revision();
    const auto edited = War3Map::QtProject::PublishTriggerGraphOperationsWithAssets(*f.project,
        f.Batch(1).operations, QStringLiteral("Cancelled consent"), [&](auto& state) {
            state.imports.push_back({"audastra/runtime/settings.json", {}, 1,
                std::make_shared<const std::vector<std::byte>>(1, std::byte{1})});
            return Status::Failure("test", "CANCELLED", "author cancelled");
        });
    QVERIFY(!edited.Succeeded()); QVERIFY(f.OriginalBytes()); QVERIFY(f.project->CaptureAssets().imports.empty());
    QCOMPARE(f.project->triggerUndoStack_->count(), 0); QCOMPARE(f.project->AssetUndoStack()->count(), 0);
    QCOMPARE(f.project->editRevision_.Revision(), revision); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::GraphAndAssetsRejectStaleUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto edited = War3Map::QtProject::PublishTriggerGraphOperationsWithAssets(*f.project,
        f.Batch(1).operations, QStringLiteral("Bound resources"), [](auto& state) {
            state.imports.push_back({"owned.txt", {}, 1, std::make_shared<const std::vector<std::byte>>(1, std::byte{1})});
            return Status::Ok();
        });
    QVERIFY(edited.Succeeded()); const auto after = f.Pair();
    auto assets = f.project->CaptureAssets(); assets.imports.front().archivePath = "changed.txt";
    f.project->PublishAssetState(assets);
    const auto revision = f.project->editRevision_.Revision();
    f.project->triggerUndoStack_->undo();
    QVERIFY(f.Pair().wtg == after.wtg); QVERIFY(f.project->CaptureAssets() == assets);
    QCOMPARE(f.project->editRevision_.Revision(), revision); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::GraphAndAssetsRejectFailedObservation() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto revision = f.project->editRevision_.Revision();
    const auto edited = War3Map::QtProject::PublishTriggerGraphOperationsWithAssets(*f.project,
        f.Batch(1).operations, QStringLiteral("Invalid resource fingerprint"), [](auto& state) {
            // 长度与正文不符，实际来源捕获应拒绝；不降低保存校验来接受候选。
            state.imports.push_back({"broken.txt", {}, 100, std::make_shared<const std::vector<std::byte>>(1, std::byte{1})});
            return Status::Ok();
        });
    QVERIFY(!edited.Succeeded()); QVERIFY(f.OriginalBytes()); QVERIFY(f.project->CaptureAssets().imports.empty());
    QCOMPARE(f.project->editRevision_.Revision(), revision); QCOMPARE(f.project->triggerUndoStack_->count(), 0);
    QVERIFY(!f.project->editTrackingFailed_); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::AssetOnlyGraphTransactionAndImmutablePayload() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto bytes = std::make_shared<std::vector<std::byte>>(1, std::byte{12});
    const auto revision = f.project->editRevision_.Revision();
    const auto edited = War3Map::QtProject::PublishTriggerGraphOperationsWithAssets(*f.project,
        {}, QStringLiteral("Settings without graph delta"), [&](auto& state) {
            std::vector<EditorProject::ImportPayload> payloads{{"settings.txt", bytes}};
            const auto problem = f.project->PrepareImportPayloads(state, payloads, false);
            return problem.isEmpty() ? Status::Ok() : Status::Failure("test", "PREPARE_FAILED", problem.toStdString());
        });
    QVERIFY(edited.Succeeded()); bytes->front() = std::byte{55};
    QVERIFY(f.OriginalBytes()); QCOMPARE(f.project->triggerUndoStack_->count(), 1);
    QVERIFY(f.project->CaptureAssets().imports.front().ownedPayload->front() == std::byte{12});
    QCOMPARE(f.project->editRevision_.Revision(), revision + 1);
    f.project->triggerUndoStack_->undo(); QVERIFY(f.project->CaptureAssets().imports.empty());
    f.project->triggerUndoStack_->redo();
    QVERIFY(f.project->CaptureAssets().imports.front().ownedPayload->front() == std::byte{12});
    QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::RuntimeConsentSharesGraphRevisionAndUndo() {
    Fixture f; QVERIFY2(f.Initialize(),qPrintable(f.error));
    const auto package = f.RuntimePackage(); f.project->runtimePackageSources_ = {package};
    const auto generation = f.project->editRevision_.Generation(), revision = f.project->editRevision_.Revision();
    const auto initial = f.project->QueryMapRuntimeState();
    QVERIFY(initial.Succeeded()); QVERIFY(!initial.value->approved);
    QCOMPARE(initial.value->packages.size(),std::size_t(1));
    QVERIFY(initial.value->packages.front().available); QVERIFY(initial.value->packages.front().internalOnly);
    QCOMPARE(initial.value->generation,generation); QCOMPARE(initial.value->revision,revision);
    QCOMPARE(f.project->editRevision_.Revision(),revision); QVERIFY(f.project->CaptureAssets().imports.empty());
    const Services::MapRuntimeSettings settings{Services::MapRuntimeMode::Automatic,package.manifestSha256};
    const auto edited = PublishTriggerGraphOperationsWithAssets(*f.project,f.Batch(1).operations,QStringLiteral("Enable runtime"),
        [&](AssetState& state) { return f.project->PrepareMapRuntimeSettings(state,settings,generation,revision); });
    QVERIFY(edited.Succeeded()); QCOMPARE(f.project->runtimeApprovedPackageSha256_,package.manifestSha256);
    QCOMPARE(f.project->QueryMapRuntimeSettings().value,std::optional(settings));
    QVERIFY(f.project->QueryMapRuntimeState().value->approved);
    QCOMPARE(f.project->editRevision_.Revision(),revision + 1);
    QCOMPARE(f.project->editRevision_.Current().at("runtime/approval"),package.manifestSha256);
    QCOMPARE(f.project->triggerUndoStack_->count(),1); QCOMPARE(f.project->AssetUndoStack()->count(),0);
    f.project->triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes()); QVERIFY(f.project->runtimeApprovedPackageSha256_.empty());
    QVERIFY(f.project->QueryMapRuntimeSettings().value->packageSha256.empty());
    f.project->triggerUndoStack_->redo(); QVERIFY(!f.OriginalBytes());
    QCOMPARE(f.project->runtimeApprovedPackageSha256_,package.manifestSha256);
    QVERIFY(f.project->ObserveEditState("verify consent transaction"));
    f.project->ResetEditTracking(); QVERIFY(f.project->runtimeApprovedPackageSha256_.empty());
    QCOMPARE(f.project->QueryMapRuntimeSettings().value,std::optional(settings));
    QVERIFY(!f.project->QueryMapRuntimeState().value->approved);
    QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::RuntimeConsentRejectsMissingPackageAndStaleRevision() {
    Fixture f; QVERIFY2(f.Initialize(),qPrintable(f.error));
    const auto package = f.RuntimePackage();
    const auto generation = f.project->editRevision_.Generation(), revision = f.project->editRevision_.Revision();
    auto state = f.project->CaptureAssets();
    const Services::MapRuntimeSettings settings{Services::MapRuntimeMode::Enabled,package.manifestSha256};
    const auto missing = f.project->PrepareMapRuntimeSettings(state,settings,generation,revision);
    QVERIFY(!missing.Succeeded()); QCOMPARE(missing.diagnostics.front().code,std::string("RUNTIME_PACKAGE_MISSING"));
    QVERIFY(state == f.project->CaptureAssets()); f.project->runtimePackageSources_ = {package};
    f.project->runtimePackageSources_.push_back(package);
    const auto duplicate = f.project->QueryMapRuntimeState();
    QVERIFY(!duplicate.Succeeded()); QCOMPARE(duplicate.diagnostics.front().code,std::string("RUNTIME_PACKAGE_CONFLICT"));
    f.project->runtimePackageSources_.pop_back();
    f.project->runtimeCatalogDiagnostics_ = Status::Failure("test","RUNTIME_CATALOG_INVALID","invalid").diagnostics;
    const auto invalidCatalog = f.project->QueryMapRuntimeState();
    QVERIFY(!invalidCatalog.Succeeded()); QCOMPARE(invalidCatalog.diagnostics.front().code,std::string("RUNTIME_CATALOG_INVALID"));
    QVERIFY(!f.project->PrepareMapRuntimeSettings(state,settings,generation,revision).Succeeded());
    f.project->runtimeCatalogDiagnostics_.clear();
    const auto stale = f.project->PrepareMapRuntimeSettings(state,settings,generation,revision + 1);
    QVERIFY(!stale.Succeeded()); QCOMPARE(stale.diagnostics.front().code,std::string("RUNTIME_REVISION_STALE"));
    QVERIFY(state == f.project->CaptureAssets());
    const auto edited = PublishTriggerGraphOperationsWithAssets(*f.project,{},QStringLiteral("Enable with no graph delta"),
        [&](AssetState& candidate) { return f.project->PrepareMapRuntimeSettings(candidate,settings,generation,revision); });
    QVERIFY(edited.Succeeded()); QVERIFY(f.OriginalBytes());
    const auto current = f.project->editRevision_.Revision();
    const auto disabled = PublishTriggerGraphOperationsWithAssets(*f.project,{},QStringLiteral("Disable"),
        [&](AssetState& candidate) { return f.project->PrepareMapRuntimeSettings(candidate,{Services::MapRuntimeMode::Disabled,{}},generation,current); });
    QVERIFY(disabled.Succeeded()); QVERIFY(f.project->runtimeApprovedPackageSha256_.empty());
    f.project->triggerUndoStack_->undo(); QCOMPARE(f.project->runtimeApprovedPackageSha256_,package.manifestSha256);
    f.project->triggerUndoStack_->undo(); QVERIFY(f.project->runtimeApprovedPackageSha256_.empty());
    QVERIFY(f.project->CaptureAssets().imports.empty()); QVERIFY(f.SourcesUnchanged());
}

void TriggerScalarCommandTests::RuntimeConsentSurvivesProjectSaveAndBindsSource() {
    Fixture f; QVERIFY2(f.Initialize(),qPrintable(f.error));
    const auto package = f.RuntimePackage(); f.project->runtimePackageSources_ = {package};
    const auto edited = PublishTriggerGraphOperationsWithAssets(*f.project,{},QStringLiteral("Enable"),[&](AssetState& state) {
        return f.project->PrepareMapRuntimeSettings(state,{Services::MapRuntimeMode::Automatic,package.manifestSha256},
            f.project->editRevision_.Generation(),f.project->editRevision_.Revision());
    });
    QVERIFY(edited.Succeeded());
    const auto prepared = f.project->PrepareProjectSave(f.root / "runtime.forge");
    QVERIFY(prepared.Succeeded()); auto& job = **prepared.value;
    EditorProject::RunProjectSave(job); QVERIFY(job.Outcome().Succeeded());
    QVERIFY(f.project->CommitProjectSave(job).Succeeded());
    const auto reopened = PrepareProjectOpen(job.Target());
    QVERIFY(reopened.Succeeded()); QCOMPARE((*reopened.value)->runtimeApprovedPackageSha256,package.manifestSha256);
    const auto archive = MapArchiveSession::Open(job.Target()); QVERIFY(archive.Succeeded());
    std::vector<MapArchiveMember> members(archive.value->Members().begin(),archive.value->Members().end());
    for (auto& member : members) if (member.name == "project.json") {
        auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(member.data.data()),static_cast<qsizetype>(member.data.size()))).object();
        auto approval = json["runtimeApproval"].toObject(); approval["sourceSha256"] = QString(64,'f'); json["runtimeApproval"] = approval;
        const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
        member.data.assign(reinterpret_cast<const std::byte*>(bytes.data()),reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
    }
    const auto wrong = f.root / "wrong-source.forge";
    QVERIFY(WriteVerifiedArchive(wrong,members).Succeeded()); QVERIFY(!PrepareProjectOpen(wrong).Succeeded());
    QVERIFY(f.SourcesUnchanged());
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraOwnedTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TriggerScalarCommandTests"));
    TriggerScalarCommandTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "TriggerScalarCommandTests.moc"
