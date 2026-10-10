#include "ProjectSearchCapture.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "../../src/Core/Mpq/MpqCrypt.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace War3Map;
using QtApp::ProjectSearchCapture;
using QtApp::ProjectSearchCaptureOptions;
namespace {
std::vector<std::byte> Bytes(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {bytes.begin(), bytes.end()};
}
bool Initialize(QtProject::EditorProject& project) {
    if (!project.editRevision_.Reset().Succeeded()) return false;
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.wts", Bytes("STRING 1\n{\nOwned\n}\n")}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    Trigger::TriggerRecord trigger; trigger.id = 1; trigger.name.text = "Owned trigger";
    project.triggerDocument_->triggers.push_back(std::move(trigger));
    return true;
}
Status Prepare(ProjectSearchCapture& capture, const QtProject::EditorProject& project,
    std::string revision, const ProjectSearchCaptureOptions& options = {}) {
    return capture.Prepare(project, "owned-capture", project.editRevision_.Generation(), std::move(revision), options);
}
bool Catalog(QtProject::EditorProject& project, const QTemporaryDir& root, QByteArray title) {
    const auto write = [&root](const QString& name, const QByteArray& contents) {
        const auto path = root.filePath(name);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    };
    if (!write("ui/config", "base\n") || !write("ui/base/define.txt",
        QByteArray("[TriggerTypes]\ninteger=0,1,1,integer\nunitcode=0,1,1,") + title + ",integer\n")) return false;
    auto loaded = Trigger::UiCatalog::Load(std::filesystem::path(root.path().toStdWString()));
    if (!loaded.Succeeded() || !loaded.value) return false;
    // 原创目录已真实Load；使用实际生命周期信号，不调会读取作者WorldEditStrings的加载外围。
    emit project.TriggerDocumentAboutToChange();
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*loaded.value));
    emit project.TriggerDocumentChanged();
    return true;
}
bool Archive(QtProject::EditorProject& project, const QTemporaryDir& root, std::size_t count) {
    std::vector<MapArchiveMember> members;
    for (std::size_t index = 0; index < count; ++index) members.push_back({
        QStringLiteral("Textures/Owned%1.blp").arg(static_cast<qulonglong>(index), 5, 10, QLatin1Char('0')).toStdString(),
        {std::byte{'x'}}});
    members.push_back({"war3map.wts", Bytes("STRING 1\n{\nOwned\n}\n")});
    const auto path = std::filesystem::path(root.filePath("owned-input.w3x").toStdWString());
    const auto saved = WriteVerifiedArchive(path, members);
    if (!saved.Succeeded()) return false;
    auto opened = MapArchiveSession::Open(path);
    if (!opened.Succeeded() || !opened.value) return false;
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath();
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    emit project.MapDocumentChanged();
    project.NotifyImportsChanged();
    return true;
}
bool Code(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& item) { return item.code == code; });
}

bool OpaqueArchive(QtProject::EditorProject& project, const QTemporaryDir& root) {
    // 原创三块MPQ：listfile仅公开已知资源，另一个加密块故意没有可枚举名称。
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const auto known = Bytes("known-original-payload"), opaque = Bytes("opaque-original-payload");
    const auto list = Bytes("Textures/KnownOpaque.blp\r\n");
    constexpr std::uint32_t hashCount = 8, blockCount = 3, knownAt = 32;
    const auto opaqueAt = knownAt + static_cast<std::uint32_t>(known.size());
    const auto listAt = opaqueAt + static_cast<std::uint32_t>(opaque.size());
    const auto hashAt = listAt + static_cast<std::uint32_t>(list.size());
    const auto blockAt = hashAt + hashCount * 16;
    std::vector<std::byte> bytes(blockAt + blockCount * 16);
    const auto put = [&bytes](std::size_t at, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes[at + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
    };
    put(0, 0x1a51504d); put(4, 32); put(8, static_cast<std::uint32_t>(bytes.size()));
    put(12, 0x00030000); put(16, hashAt); put(20, blockAt); put(24, hashCount); put(28, blockCount);
    std::copy(known.begin(), known.end(), bytes.begin() + knownAt);
    std::copy(opaque.begin(), opaque.end(), bytes.begin() + opaqueAt);
    crypt.EncryptBlock(bytes.data() + opaqueAt, opaque.size(), crypt.HashString("unlisted-preview.bin", Mpq::MpqCrypt::HashType::FileKey));
    std::copy(list.begin(), list.end(), bytes.begin() + listAt);
    std::fill(bytes.begin() + hashAt, bytes.begin() + blockAt, std::byte{0xff});
    std::array<bool, hashCount> used{};
    const auto hash = [&](std::string_view name, std::uint32_t block) {
        auto bucket = crypt.HashString(name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (used[bucket]) bucket = (bucket + 1) & (hashCount - 1);
        used[bucket] = true;
        const auto at = hashAt + bucket * 16;
        put(at, crypt.HashString(name, Mpq::MpqCrypt::HashType::NameA));
        put(at + 4, crypt.HashString(name, Mpq::MpqCrypt::HashType::NameB)); put(at + 8, 0); put(at + 12, block);
    };
    hash("Textures/KnownOpaque.blp", 0); hash("unlisted-preview.bin", 1); hash("(listfile)", 2);
    crypt.EncryptBlock(bytes.data() + hashAt, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto block = [&](std::uint32_t index, std::uint32_t at, std::size_t length, bool encrypted) {
        const auto offset = blockAt + index * 16;
        put(offset, at); put(offset + 4, static_cast<std::uint32_t>(length)); put(offset + 8, static_cast<std::uint32_t>(length));
        put(offset + 12, Mpq::kFileExists | Mpq::kFileSingleUnit | (encrypted ? Mpq::kFileEncrypted : 0U));
    };
    block(0, knownAt, known.size(), false); block(1, opaqueAt, opaque.size(), true); block(2, listAt, list.size(), false);
    crypt.EncryptBlock(bytes.data() + blockAt, blockCount * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto path = root.filePath("owned-opaque-input.w3x");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())) return false;
    file.close();
    auto opened = MapArchiveSession::Open(std::filesystem::path(path.toStdWString()));
    if (!opened.Succeeded() || !opened.value) return false;
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath();
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    emit project.MapDocumentChanged(); project.NotifyImportsChanged();
    return true;
}
} // namespace

class ProjectSearchCaptureTests final : public QObject {
    Q_OBJECT
private slots:
    void TypesAndMapPreparedOnceAcrossPagesAndUnrelatedRevision();
    void FullAssetAdmissionRetainsLateCollisionAndReplacement();
    void OpaqueArchiveRetainsKnownRowsAndPartialCoverage();
    void OverBudgetAssetDomainDoesNotMergeOrBlockOtherDomains();
    void EditedMapBytesEnforceBudgetAndRealSessionInvalidation();
    void ObjectSourcesBorrowLoadedArraysWithoutDirectoryFallback();
    void PreparedBorrowRejectsSignalsOwnerAndGenerationChanges();
};

void ProjectSearchCaptureTests::TypesAndMapPreparedOnceAcrossPagesAndUnrelatedRevision() {
    QtProject::EditorProject project; QVERIFY(Initialize(project));
    QTemporaryDir catalog; QVERIFY(catalog.isValid()); QVERIFY(Catalog(project, catalog, "unitcode"));
    ProjectSearchCapture capture; QVERIFY(Prepare(capture, project, "owned/1").Succeeded());
    const auto first = capture.Borrow(project, "owned/1"); QVERIFY(first.Succeeded() && first.value);
    QVERIFY(first.value->types && first.value->mapData && first.value->strings);
    QCOMPARE(first.value->types->LiteralKind("unitcode"), Trigger::TriggerTypeSystem::Literal::ObjectCode);
    QCOMPARE(capture.Stats().typePreparations, std::uint64_t{1}); QCOMPARE(capture.Stats().mapPreparations, std::uint64_t{1});
    QVERIFY(capture.Stats().catalogSourceRows >= 6); QCOMPARE(capture.Stats().catalogManifestSha256.size(), std::size_t{64});
    QVERIFY(capture.Stats().mapSnapshotBytesCharged > 0);
    const auto map = first.value->mapData; const auto types = first.value->types;
    for (int page = 0; page < 3; ++page) {
        QVERIFY(Prepare(capture, project, "owned/1").Succeeded());
        const auto borrowed = capture.Borrow(project, "owned/1"); QVERIFY(borrowed.Succeeded());
        QCOMPARE(borrowed.value->mapData, map); QCOMPARE(borrowed.value->types, types);
    }
    // 工程搜索修订变更不等于当前mapDoc/source/edit-session变更。
    QVERIFY(Prepare(capture, project, "owned/2").Succeeded());
    QCOMPARE(capture.Stats().mapPreparations, std::uint64_t{1}); QCOMPARE(capture.Stats().typePreparations, std::uint64_t{1});
    QVERIFY(capture.Stats().mapCacheHits >= 4 && capture.Stats().typeCacheHits >= 4);
    QVERIFY(Code(capture.Borrow(project, "owned/1"), "SEARCH_CAPTURE_STALE"));
    QVERIFY(Catalog(project, catalog, "Changed display"));
    QVERIFY(Code(capture.Borrow(project, "owned/2"), "SEARCH_CAPTURE_STALE"));
    QVERIFY(Prepare(capture, project, "owned/3").Succeeded());
    QCOMPARE(capture.Stats().typePreparations, std::uint64_t{2}); QCOMPARE(capture.Stats().mapPreparations, std::uint64_t{1});
    const auto changed = capture.Borrow(project, "owned/3"); QVERIFY(changed.Succeeded()); QCOMPARE(changed.value->mapData, map);
}

void ProjectSearchCaptureTests::FullAssetAdmissionRetainsLateCollisionAndReplacement() {
    QtProject::EditorProject project; QVERIFY(Initialize(project)); QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(Archive(project, root, 4200));
    ProjectSearchCapture capture; QVERIFY(Prepare(capture, project, "assets/1").Succeeded());
    QCOMPARE(capture.Assets().size(), std::size_t{4097});
    const auto target = capture.Assets().front().path;
    const auto original = capture.Assets().front().originalPath;
    const auto tail = QStringLiteral("Textures/Owned04199.blp");
    QVERIFY(tail != original && project.archiveSession_->HasMember(tail.toStdString()));
    project.assetEdits_.push_back({tail, target}); project.NotifyImportsChanged();
    QVERIFY(capture.Assets().empty());
    QVERIFY(Prepare(capture, project, "assets/2").Succeeded());
    const auto collided = std::find_if(capture.Assets().begin(), capture.Assets().end(), [&](const auto& entry) { return entry.path == target; });
    QVERIFY(collided != capture.Assets().end() && collided->ambiguous);
    auto borrowed = capture.Borrow(project, "assets/2"); QVERIFY(borrowed.Succeeded());
    QCOMPARE(borrowed.value->importState, Services::ProjectSearchState::Partial);
    QVERIFY(std::none_of(borrowed.value->currentFiles.begin(), borrowed.value->currentFiles.end(), [&](const auto& row) {
        return row.path == target.toUtf8().toStdString();
    })); // 原始前缀之后的冲突不能泄漏可导航行。
    project.assetEdits_.clear();
    auto owned = std::make_shared<const std::vector<std::byte>>(Bytes("owned replacement"));
    project.pendingImports_.push_back({target, {}, static_cast<qint64>(owned->size()), owned}); project.NotifyImportsChanged();
    QVERIFY(Prepare(capture, project, "assets/3").Succeeded());
    const auto replacement = std::find_if(capture.Assets().begin(), capture.Assets().end(), [&](const auto& entry) { return entry.path == target; });
    QVERIFY(replacement != capture.Assets().end() && replacement->Replacement() && !replacement->ambiguous);
    QCOMPARE(replacement->originalPath, original); QVERIFY(replacement->ownedPayload == owned);
    borrowed = capture.Borrow(project, "assets/3"); QVERIFY(borrowed.Succeeded());
    const auto id = ("pending/" + target).toUtf8().toStdString();
    QVERIFY(std::any_of(borrowed.value->currentFiles.begin(), borrowed.value->currentFiles.end(), [&](const auto& row) { return row.stableId == id; }));
    QCOMPARE(capture.Stats().assetMergeCalls, std::uint64_t{3});
    QCOMPARE(capture.Stats().mapPreparations, std::uint64_t{1});
}

void ProjectSearchCaptureTests::OverBudgetAssetDomainDoesNotMergeOrBlockOtherDomains() {
    QtProject::EditorProject project; QVERIFY(Initialize(project)); QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(Archive(project, root, 4));
    ProjectSearchCapture capture; ProjectSearchCaptureOptions options; options.maxAssetInputRows = 1;
    QVERIFY(Prepare(capture, project, "budget/1", options).Succeeded());
    QCOMPARE(capture.Stats().assetMergeCalls, std::uint64_t{0}); QCOMPARE(capture.Stats().assetReason, std::string("SEARCH_ASSETS_BUDGET"));
    auto borrowed = capture.Borrow(project, "budget/1"); QVERIFY(borrowed.Succeeded());
    QCOMPARE(borrowed.value->importState, Services::ProjectSearchState::Unavailable);
    QVERIFY(borrowed.value->currentFiles.empty() && capture.Assets().empty() && borrowed.value->mapData);
    borrowed.value->triggers = project.triggerDocument_.get(); borrowed.value->triggerState = Services::ProjectSearchState::Ready;
    const auto page = Services::SearchProjectPage(*borrowed.value, "budget/1", "Owned"); QVERIFY(page.Succeeded());
    QVERIFY(std::any_of(page.value->rows.begin(), page.value->rows.end(), [](const auto& row) { return row.category == Services::ProjectSearchCategory::Trigger; }));
    options = {}; options.maxAssetPathBytes = 8;
    QVERIFY(Prepare(capture, project, "budget/2", options).Succeeded()); QCOMPARE(capture.Stats().assetMergeCalls, std::uint64_t{0});
    borrowed = capture.Borrow(project, "budget/2"); QVERIFY(borrowed.Succeeded() && borrowed.value && borrowed.value->mapData);
    options = {}; project.assetEdits_.push_back({QString::fromUtf8(project.archiveSession_->Members().front().name), QString(2049, QLatin1Char('x'))});
    project.NotifyImportsChanged(); QVERIFY(Prepare(capture, project, "budget/3", options).Succeeded());
    QCOMPARE(capture.Stats().assetMergeCalls, std::uint64_t{0}); QVERIFY(capture.Assets().empty());
}

void ProjectSearchCaptureTests::OpaqueArchiveRetainsKnownRowsAndPartialCoverage() {
    QtProject::EditorProject project; QVERIFY(Initialize(project)); QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(Archive(project, root, 1)); QVERIFY(!project.archiveSession_->HasOpaqueMembers());
    ProjectSearchCapture capture; QVERIFY(Prepare(capture, project, "opaque/complete").Succeeded());
    QCOMPARE(capture.Stats().assetState, Services::ProjectSearchState::Ready);
    QVERIFY(OpaqueArchive(project, root)); QVERIFY(project.archiveSession_->HasOpaqueMembers());
    QCOMPARE(project.archiveSession_->OpaqueMemberCount(), std::size_t{1});
    QVERIFY(project.archiveSession_->HasMember("Textures/KnownOpaque.blp"));
    QVERIFY(!project.archiveSession_->HasMember("unlisted-preview.bin"));
    const auto sourceSha = project.archiveSession_->SourceSha256();
    const auto mapRevision = project.mapEdits_->Revision(); const int undo = project.AssetUndoStack()->count();
    QVERIFY(Prepare(capture, project, "opaque/partial").Succeeded());
    QCOMPARE(capture.Stats().assetState, Services::ProjectSearchState::Partial);
    QCOMPARE(capture.Stats().assetReason, std::string("SEARCH_ASSETS_PARTIAL"));
    auto borrowed = capture.Borrow(project, "opaque/partial"); QVERIFY(borrowed.Succeeded() && borrowed.value);
    QCOMPARE(borrowed.value->importState, Services::ProjectSearchState::Partial);
    QVERIFY(std::any_of(borrowed.value->currentFiles.begin(), borrowed.value->currentFiles.end(), [](const auto& row) {
        return row.stableId == "Textures/KnownOpaque.blp" && row.path == "Textures/KnownOpaque.blp";
    }));
    QVERIFY(std::none_of(borrowed.value->currentFiles.begin(), borrowed.value->currentFiles.end(), [](const auto& row) {
        return row.path == "unlisted-preview.bin";
    }));
    const auto page = Services::SearchProjectPage(*borrowed.value, "opaque/partial", "KnownOpaque");
    QVERIFY(page.Succeeded() && page.value);
    QCOMPARE(page.value->coverage[static_cast<std::size_t>(Services::ProjectSearchCategory::ImportedFile)].state, Services::ProjectSearchState::Partial);
    QVERIFY(std::any_of(page.value->rows.begin(), page.value->rows.end(), [](const auto& row) {
        return row.category == Services::ProjectSearchCategory::ImportedFile && row.rowKey == "Textures/KnownOpaque.blp";
    }));
    QCOMPARE(project.archiveSession_->SourceSha256(), sourceSha); QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
    QCOMPARE(project.mapEdits_->Revision(), mapRevision); QCOMPARE(project.AssetUndoStack()->count(), undo);
}

void ProjectSearchCaptureTests::EditedMapBytesEnforceBudgetAndRealSessionInvalidation() {
    QtProject::EditorProject project; QVERIFY(Initialize(project));
    ProjectSearchCapture capture; ProjectSearchCaptureOptions options;
    options.maxMapBytes = project.mapDocument_->Member(MapDataKind::Strings).sources.front().data.size() + 1;
    const auto baseSize = project.mapDocument_->Member(MapDataKind::Strings).sources.front().data.size();
    QVERIFY(Prepare(capture, project, "map/1", options).Succeeded());
    auto borrowed = capture.Borrow(project, "map/1"); QVERIFY(borrowed.Succeeded() && borrowed.value->mapData);
    const auto oldRevision = project.mapEdits_->Revision();
    const auto edited = Bytes("STRING 1\n{\n" + std::string(2000, 'x') + "\n}\n");
    QVERIFY(project.mapEdits_->EditSerialized(MapDataKind::Strings, edited).Succeeded());
    QVERIFY(project.mapEdits_->Revision() > oldRevision);
    QVERIFY(Code(capture.Borrow(project, "map/1"), "SEARCH_CAPTURE_STALE"));
    QVERIFY(Prepare(capture, project, "map/2", options).Succeeded());
    borrowed = capture.Borrow(project, "map/2"); QVERIFY(borrowed.Succeeded() && !borrowed.value->mapData);
    QCOMPARE(capture.Stats().mapReason, std::string("MAP_SNAPSHOT_BUDGET"));
    QCOMPARE(project.mapDocument_->Member(MapDataKind::Strings).sources.front().data.size(), baseSize);
    options = {}; QVERIFY(Prepare(capture, project, "map/3", options).Succeeded());
    borrowed = capture.Borrow(project, "map/3"); QVERIFY(borrowed.Succeeded() && borrowed.value->mapData);
    QCOMPARE(borrowed.value->mapDataBinding.revision, project.mapEdits_->Revision());
    QVERIFY(capture.Stats().mapSnapshotBytesCharged >= edited.size());
    const auto count = capture.Stats().mapPreparations;
    const auto oldSession = project.mapEdits_->SessionIdentity();
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    QVERIFY(project.mapEdits_->SessionIdentity() != oldSession); QVERIFY(Code(capture.Borrow(project, "map/3"), "SEARCH_CAPTURE_STALE"));
    QVERIFY(Prepare(capture, project, "map/4", options).Succeeded()); QCOMPARE(capture.Stats().mapPreparations, count + 1);
    borrowed = capture.Borrow(project, "map/4"); QVERIFY(borrowed.Succeeded() && borrowed.value && borrowed.value->mapData);
}

void ProjectSearchCaptureTests::ObjectSourcesBorrowLoadedArraysWithoutDirectoryFallback() {
    QtProject::EditorProject project; QVERIFY(Initialize(project));
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(Bytes("[hfoo]\nuhpm=100\n"));
    QVERIFY(parsed.Succeeded()); project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    QVERIFY(project.objectDocuments_[0]); QTemporaryDir root; QVERIFY(root.isValid());
    project.projectRoot_ = std::filesystem::path(root.filePath("never-created").toStdWString());
    ProjectSearchCapture capture; QVERIFY(Prepare(capture, project, "objects/1").Succeeded());
    auto borrowed = capture.Borrow(project, "objects/1"); QVERIFY(borrowed.Succeeded());
    QCOMPARE(borrowed.value->objects[0].state, Services::ProjectSearchState::Ready);
    QVERIFY(borrowed.value->objects[0].records.data() == project.objectDocuments_[0]->Objects().data());
    for (std::size_t i = 1; i < 7; ++i) QCOMPARE(borrowed.value->objects[i].state, Services::ProjectSearchState::Unavailable);
    QVERIFY(!QFileInfo::exists(QString::fromStdWString(project.projectRoot_.wstring())));
    QVERIFY(Archive(project, root, 2)); QVERIFY(Prepare(capture, project, "objects/2").Succeeded());
    borrowed = capture.Borrow(project, "objects/2"); QVERIFY(borrowed.Succeeded());
    for (std::size_t i = 1; i < 7; ++i) QCOMPARE(borrowed.value->objects[i].state, Services::ProjectSearchState::Missing);
    project.sourceChangeSuspected_ = true; emit project.SourceStateChanged();
    QVERIFY(Prepare(capture, project, "objects/3").Succeeded()); borrowed = capture.Borrow(project, "objects/3"); QVERIFY(borrowed.Succeeded());
    for (std::size_t i = 1; i < 7; ++i) QCOMPARE(borrowed.value->objects[i].state, Services::ProjectSearchState::Unavailable);
    QCOMPARE(borrowed.value->objects[0].state, Services::ProjectSearchState::Ready);
}

void ProjectSearchCaptureTests::PreparedBorrowRejectsSignalsOwnerAndGenerationChanges() {
    QtProject::EditorProject project; QVERIFY(Initialize(project));
    ProjectSearchCapture capture; QVERIFY(Code(capture.Borrow(project, "lease/1"), "SEARCH_CAPTURE_STALE"));
    const auto mapRevision = project.mapEdits_->Revision();
    const int mapUndo = project.mapDataUndoStack_->count(), assetUndo = project.AssetUndoStack()->count();
    QSignalSpy imports(&project, &QtProject::EditorProject::ImportsChanged);
    QSignalSpy changed(&project, &QtProject::EditorProject::MapDocumentChanged);
    QVERIFY(Prepare(capture, project, "lease/1").Succeeded()); QVERIFY(capture.Borrow(project, "lease/1").Succeeded());
    QVERIFY(imports.isEmpty() && changed.isEmpty()); QCOMPARE(project.mapEdits_->Revision(), mapRevision);
    QCOMPARE(project.mapDataUndoStack_->count(), mapUndo); QCOMPARE(project.AssetUndoStack()->count(), assetUndo);
    emit project.ImportsChanged(); QVERIFY(Code(capture.Borrow(project, "lease/1"), "SEARCH_CAPTURE_STALE"));
    QVERIFY(Prepare(capture, project, "lease/2").Succeeded());
    QtProject::EditorProject other; QVERIFY(Initialize(other)); QVERIFY(Code(capture.Borrow(other, "lease/2"), "SEARCH_CAPTURE_STALE"));
    QVERIFY(project.editRevision_.Reset().Succeeded()); QVERIFY(Code(capture.Borrow(project, "lease/2"), "SEARCH_CAPTURE_STALE"));
    QVERIFY(Prepare(capture, project, "lease/3").Succeeded()); QVERIFY(capture.Borrow(project, "lease/3").Succeeded());
    capture.Reset(); QVERIFY(Code(capture.Borrow(project, "lease/3"), "SEARCH_CAPTURE_STALE"));
}

QTEST_GUILESS_MAIN(ProjectSearchCaptureTests)
#include "ProjectSearchCaptureTests.moc"
