#include "EditorProject.h"
#include "ProjectOpen.h"
#include "AssetOwnedPayload.h"
#include "ProjectQuerySources.h"
#include "MapSaveJob.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "../../src/Core/Mpq/MpqCrypt.h"
#include "../MapDataFixtures.h"
#include "SyntheticObjectCatalogFixture.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardItemModel>
#include <QDir>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <algorithm>
#include <array>

using namespace War3Map;
using namespace War3Map::QtProject;

namespace {
bool ModelHasCode(const QStandardItemModel& model, const QString& code) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.item(row, 2)->text() == code) return true;
    }
    return false;
}

// 仅提供本测试已有父对象的原创表；来源资格由真实临时游戏 MPQ 与共用目录捕获建立。
bool InstallOwnedGameBase(EditorProject& project, ObjectData::ObjectType type, const std::string& rawcode) {
    const auto table = Tests::ObjectTable({{{"ID", rawcode}, {"Name", "Original parent fixture"}}});
    ObjectData::CatalogLoadOptions options;
    options.sources = {{type, {"owned-base.slk"}, {}}};
    options.stringFiles.clear();
    const auto catalog = ObjectData::StandardObjectCatalog::Load([&](const std::string& name) {
        return name == "owned-base.slk" ? Result<std::vector<std::byte>>::Success(Tests::ObjectBytes(table))
            : Result<std::vector<std::byte>>::Failure("Test", "MISSING_OWNED_BASE", name);
    }, options);
    return catalog.Succeeded() && Tests::InstallSyntheticGameCatalog(project, *catalog.value);
}

bool LoadFixture(EditorProject& project, const QString& root) {
    QFile marker(root + "/map.txt");
    if (!marker.open(QIODevice::WriteOnly) || marker.write("source") != 6) return false;
    marker.close();
    auto opened = DirectoryWorkspace::Open(std::filesystem::path(root.toStdWString()));
    if (!opened.Succeeded()) return false;
    project.projectRoot_ = std::filesystem::path(root.toStdWString());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    const std::string text = "[hfoo]\nuhpm=420\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(
        std::as_bytes(std::span(text.data(), text.size())));
    if (!parsed.Succeeded()) return false;
    project.objectDocuments_[0].reset(
        dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    return project.objectDocuments_[0] != nullptr;
}

std::vector<std::byte> UnlistedTerrainBytes() {
    MapDataFixtures::Fixture fixture;
    fixture.Raw("W3E!"); fixture.UInt(11); fixture.Byte('L'); fixture.UInt(0);
    fixture.UInt(1); fixture.Raw("TxAa"); fixture.UInt(1); fixture.Raw("CLdi");
    fixture.UInt(2); fixture.UInt(2); fixture.Float(-128); fixture.Float(-128);
    for (int index = 0; index < 4; ++index) {
        fixture.Byte(0); fixture.Byte(0x20); // ground raw 8192
        fixture.Byte(0); fixture.Byte(0x1f); // water raw 7936
        fixture.Byte(0x80); fixture.Byte(0); fixture.Byte(2);
    }
    return std::move(fixture.bytes);
}

// 原创 MPQ：只有 known.bin 写入 listfile，W3E 块保留准确 hash，但没有可枚举名称。
bool WriteUnlistedTerrain(const QString& path, const std::vector<std::byte>& terrain,
    std::uint32_t extraFlags = 0, std::uint32_t declaredBytes = 0) {
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const std::string list = "known.bin\r\n";
    const auto listSpan = std::as_bytes(std::span(list.data(), list.size()));
    constexpr std::uint32_t hashCount = 8, blockCount = 3, knownAt = 32, terrainAt = 33;
    const auto listAt = terrainAt + static_cast<std::uint32_t>(terrain.size());
    const auto hashAt = listAt + static_cast<std::uint32_t>(list.size());
    const auto blockAt = hashAt + hashCount * 16;
    std::vector<std::byte> bytes(blockAt + blockCount * 16);
    const auto put = [&bytes](std::size_t at, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes[at + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
    };
    put(0, 0x1a51504d); put(4, 32); put(8, static_cast<std::uint32_t>(bytes.size()));
    put(12, 0x00030000); put(16, hashAt); put(20, blockAt); put(24, hashCount); put(28, blockCount);
    bytes[knownAt] = std::byte{7};
    std::copy(terrain.begin(), terrain.end(), bytes.begin() + terrainAt);
    std::copy(listSpan.begin(), listSpan.end(), bytes.begin() + listAt);
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
    hash("known.bin", 0); hash("war3map.w3e", 1); hash("(listfile)", 2);
    crypt.EncryptBlock(bytes.data() + hashAt, hashCount * 16,
        crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto block = [&](std::uint32_t index, std::uint32_t at, std::size_t length,
                           std::uint32_t outputBytes, std::uint32_t flags) {
        const auto offset = blockAt + index * 16;
        put(offset, at); put(offset + 4, static_cast<std::uint32_t>(length));
        put(offset + 8, outputBytes); put(offset + 12, Mpq::kFileExists | Mpq::kFileSingleUnit | flags);
    };
    block(0, knownAt, 1, 1, 0);
    block(1, terrainAt, terrain.size(), declaredBytes ? declaredBytes : static_cast<std::uint32_t>(terrain.size()), extraFlags);
    block(2, listAt, list.size(), static_cast<std::uint32_t>(list.size()), 0);
    crypt.EncryptBlock(bytes.data() + blockAt, blockCount * 16,
        crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size());
}

bool HasReadDiagnostic(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}
}

class EditorProjectTests final : public QObject {
    Q_OBJECT
private slots:
    void IndependentProjectsDoNotShareEdits();
    void MapOnlyLibraryKeepsGlobalSettings();
    void MissingImportCannotProduceSavedFingerprint();
    void RevisionLifecycleTracksSeparateSaveTargets();
    void MapDataObservationPreservesOtherScopesAndRejectsFailedTracking();
    void FailedObservationPreservesBaselineAndRecovers();
    void NewlyCreatedEmptyCategoryReturnsToClean();
    void SourceWatchAndChecksWorkWithoutWindow();
    void SourceIdentityMismatchReturnsDiagnostics();
    void SaveAndRecoveryWorkWithoutUndoWidgets();
    void PreparedOpenPreservesSessionAndCleansCancelledFiles();
    void MapDataOverlaySurvivesProjectSaveAndPreflight();
    void GeneratedObjectsSurviveCommitAndSecondSave();
    void NativeCloneOriginsSurviveProjectRestore();
    void PendingNativeCloneOriginsSurviveMissingOrEmptyMember();
    void SaveCommitRejectsRevisionDrift();
    void BoundedAssetsPreserveCurrentPrefix();
    void ObjectQueriesDistinguishMissingFromUnparsedSources();
    void TriggerChangesCannotSkipUnpreparedFramework();
    void OwnedLuaProjectRecoveryPreservesPayloadRoleAndMapGate();
    void UnlistedTerrainReadPreservesNamedInventory();
    void UnlistedTerrainReadRejectsChangedSource();
    void UnlistedTerrainReadPreservesDecodeAndBudgetErrors();
};

void EditorProjectTests::UnlistedTerrainReadPreservesNamedInventory() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto terrain = UnlistedTerrainBytes();
    const auto path = root.filePath("unlisted-terrain.w3x"); QVERIFY(WriteUnlistedTerrain(path, terrain));
    auto opened = MapArchiveSession::Open(std::filesystem::path(path.toStdWString())); QVERIFY(opened.Succeeded());
    EditorProject project; project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    const auto namedCount = project.archiveSession_->Members().size();
    const auto opaqueCount = project.archiveSession_->OpaqueMemberCount();
    const auto list = project.archiveSession_->ReadMember("(listfile)"); QVERIFY(list.Succeeded());
    const auto sourceSha = Sha256File(std::filesystem::path(path.toStdWString())); QVERIFY(sourceSha.Succeeded());
    QVERIFY(!project.archiveSession_->HasMember("war3map.w3e"));
    QVERIFY(HasReadDiagnostic(project.archiveSession_->ReadMember("war3map.w3e"), "MEMBER_MISSING"));
    const auto read = project.ReadMapMember("war3map.w3e"); QVERIFY(read.Succeeded() && read.value);
    QVERIFY(*read.value == terrain);
    const auto parsed = MapData::ParseMapTerrain(*read.value); QVERIFY(parsed.Succeeded() && parsed.value);
    QCOMPARE(parsed.value->Snapshot()->header.width, std::uint32_t{2});
    QCOMPARE(parsed.value->Snapshot()->vertices.size(), std::size_t{4});
    const auto upper = project.ReadMapMember("WAR3MAP.W3E"); QVERIFY(upper.Succeeded() && *upper.value == terrain);
    QVERIFY(HasReadDiagnostic(project.ReadMapMember("war3map.shd"), "MAP_MEMBER_UNAVAILABLE"));
    QVERIFY(!project.archiveSession_->HasMember("war3map.w3e"));
    QVERIFY(HasReadDiagnostic(project.archiveSession_->ReadMember("war3map.w3e"), "MEMBER_MISSING"));
    QCOMPARE(project.archiveSession_->Members().size(), namedCount);
    QCOMPARE(project.archiveSession_->OpaqueMemberCount(), opaqueCount);
    QVERIFY(project.archiveSession_->ReadMember("(listfile)").value == list.value);
    QVERIFY(Sha256File(std::filesystem::path(path.toStdWString())).value == sourceSha.value);
}

void EditorProjectTests::UnlistedTerrainReadRejectsChangedSource() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto path = root.filePath("changed-unlisted-terrain.w3x"); QVERIFY(WriteUnlistedTerrain(path, UnlistedTerrainBytes()));
    auto opened = MapArchiveSession::Open(std::filesystem::path(path.toStdWString())); QVERIFY(opened.Succeeded());
    EditorProject project; project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    const auto namedCount = project.archiveSession_->Members().size();
    QFile changed(path); QVERIFY(changed.open(QIODevice::ReadWrite)); QVERIFY(changed.seek(32));
    QCOMPARE(changed.write("x", 1), qint64{1}); changed.close();
    const auto read = project.ReadMapMember("war3map.w3e");
    QVERIFY(!read.Succeeded() && !read.value); QVERIFY(HasReadDiagnostic(read, "SOURCE_CHANGED"));
    QVERIFY(!HasReadDiagnostic(read, "MAP_MEMBER_UNAVAILABLE"));
    const auto missing = project.ReadMapMember("war3map.shd");
    QVERIFY(!missing.value && HasReadDiagnostic(missing, "SOURCE_CHANGED"));
    QCOMPARE(project.archiveSession_->Members().size(), namedCount);
    QVERIFY(!project.archiveSession_->HasMember("war3map.w3e"));
}

void EditorProjectTests::UnlistedTerrainReadPreservesDecodeAndBudgetErrors() {
    for (const bool oversized : {false, true}) {
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto path = root.filePath("unreadable-unlisted-terrain.w3x");
        QVERIFY(WriteUnlistedTerrain(path, UnlistedTerrainBytes(), oversized ? Mpq::kFileCompress : Mpq::kFilePatch,
            oversized ? 64u * 1024u * 1024u + 1u : 0u));
        auto opened = MapArchiveSession::Open(std::filesystem::path(path.toStdWString())); QVERIFY(opened.Succeeded());
        EditorProject project; project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
        const auto read = project.ReadMapMember("war3map.w3e");
        QVERIFY(!read.Succeeded() && !read.value);
        QVERIFY(HasReadDiagnostic(read, oversized ? "FILE_READ_BUDGET_EXCEEDED" : "EXACT_MEMBER_ENCODING_UNSUPPORTED"));
        QVERIFY(!HasReadDiagnostic(read, "MAP_MEMBER_UNAVAILABLE"));
        QVERIFY(!project.archiveSession_->HasMember("war3map.w3e"));
    }
}

void EditorProjectTests::PendingNativeCloneOriginsSurviveMissingOrEmptyMember() {
    using namespace ObjectData;
    for (const bool sourcePresent : {false, true}) {
        QTemporaryDir root; QVERIFY(root.isValid());
        auto native = NativeObjectDocument::CreateEmpty(ObjectType::Item); QVERIFY(native.Succeeded());
        const NativeObjectCodec codec(ObjectType::Item);
        const auto nativeBytes = codec.Serialize(*native.value); QVERIFY(nativeBytes.Succeeded());
        std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
        if (sourcePresent) members.push_back({"war3map.w3t", *nativeBytes.value});
        const auto source = std::filesystem::path(root.filePath("source.mpq").toStdWString());
        QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
        const auto sourceHash = Sha256File(source); QVERIFY(sourceHash.Succeeded());
        auto archive = MapArchiveSession::Open(source); QVERIFY(archive.Succeeded());
        QUndoStack undo;
        EditorProject project;
        project.sourceArchivePath_ = source;
        const auto workspace = root.filePath("workspace"); QVERIFY(QDir().mkpath(workspace));
        project.projectRoot_ = std::filesystem::path(workspace.toStdWString());
        project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
        project.LoadMapDocument();
        auto parsed = LniObjectCodec(ObjectType::Item).Parse(std::span<const std::byte>{}); QVERIFY(parsed.Succeeded());
        project.objectDocuments_[2].reset(static_cast<LniObjectDocument*>(parsed.document.release()));
        project.nativeObjectDocuments_[2] = std::make_unique<NativeObjectDocument>(*native.value);
        project.objectUndoStacks_[2] = &undo;
        QVERIFY(InstallOwnedGameBase(project, ObjectType::Item, "itst"));
        project.ResetEditTracking();
        auto& edited = *project.objectDocuments_[2];
        QVERIFY(edited.AddObject("I001", "itst").Succeeded());
        QVERIFY(edited.SetModification({ObjectType::Item, "I001"}, {"unam"}, std::string("Pending source")).Succeeded());
        QVERIFY(edited.CloneCustomObject({ObjectType::Item, "I001"}, "I002").Succeeded());
        QVERIFY(edited.CloneCustomObject({ObjectType::Item, "I002"}, "I003").Succeeded());
        project.ObserveObjectEdit(2);
        const auto fingerprint = edited.DocumentFingerprint();
        const auto target = std::filesystem::path(root.filePath("pending.forge").toStdWString());
        const auto saved = project.SaveProjectToPath(target);
        QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "pending project save" : saved.diagnostics.front().message.c_str());
        const auto prepared = PrepareProjectOpen(target);
        QVERIFY2(prepared.Succeeded(), prepared.diagnostics.empty() ? "pending project open" : prepared.diagnostics.front().message.c_str());
        const auto* restored = (*prepared.value)->objects[2].get(); QVERIFY(restored);
        QCOMPARE(restored->DocumentFingerprint(), fingerprint);
        QCOMPARE(restored->CloneSources().size(), std::size_t{2});
        auto candidate = *native.value;
        QVERIFY(ApplyLniChangesToNative(candidate, *restored).Succeeded());
        QCOMPARE(candidate.CustomObjectCount(), std::size_t{3});
        QCOMPARE(candidate.ModificationValueType({ObjectType::Item, "I002"}, {"unam"}), std::optional{NativeValueType::String});
        auto savedArchive = MapArchiveSession::Open(target); QVERIFY(savedArchive.Succeeded());
        const auto manifestBytes = savedArchive.value->ReadMember("project.json"); QVERIFY(manifestBytes.Succeeded());
        auto manifest = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(manifestBytes.value->data()),
            static_cast<qsizetype>(manifestBytes.value->size()))).object();
        const auto origins = manifest["objectCloneSources"].toArray(); QCOMPARE(origins.size(), 1);
        auto category = origins[0].toObject();
        QCOMPARE(category["source_present"].toBool(), sourcePresent);
        category["source_present"] = !sourcePresent;
        manifest["objectCloneSources"] = QJsonArray{category};
        const auto bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        auto tamperedMembers = std::vector<MapArchiveMember>(savedArchive.value->Members().begin(), savedArchive.value->Members().end());
        auto manifestMember = std::find_if(tamperedMembers.begin(), tamperedMembers.end(), [](const auto& member) { return member.name == "project.json"; });
        QVERIFY(manifestMember != tamperedMembers.end());
        const auto* begin = reinterpret_cast<const std::byte*>(bytes.constData());
        manifestMember->data.assign(begin, begin + bytes.size());
        const auto tampered = std::filesystem::path(root.filePath("wrong-presence.forge").toStdWString());
        QVERIFY(WriteVerifiedArchive(tampered, tamperedMembers).Succeeded());
        QVERIFY(!PrepareProjectOpen(tampered).Succeeded());
        QCOMPARE(edited.DocumentFingerprint(), fingerprint);
        QCOMPARE(codec.Serialize(*native.value).value, nativeBytes.value);
        undo.push(new QUndoCommand("pending clone undo history"));
        const auto mapCandidate = std::filesystem::path(root.filePath("pending.mpq").toStdWString());
        QVERIFY(project.SaveMapCopy(mapCandidate).has_value());
        QVERIFY(project.objectDocuments_[2]->CloneSources().empty());
        QCOMPARE(undo.count(), 0);
        QVERIFY(!project.HasUnsavedMapChanges());
        const auto readback = MapArchiveSession::Open(mapCandidate); QVERIFY(readback.Succeeded());
        const auto itemBytes = readback.value->ReadMember("war3map.w3t"); QVERIFY(itemBytes.Succeeded());
        const auto itemNative = codec.Parse(*itemBytes.value); QVERIFY(itemNative.Succeeded());
        const auto* readbackDocument = dynamic_cast<const NativeObjectDocument*>(itemNative.document.get()); QVERIFY(readbackDocument);
        QCOMPARE(readbackDocument->CustomObjectCount(), std::size_t{3});
        const auto second = std::filesystem::path(root.filePath("pending-second.mpq").toStdWString());
        QVERIFY(project.SaveMapCopy(second).has_value());
        QVERIFY(!project.HasUnsavedMapChanges());
        QCOMPARE(Sha256File(source).value, sourceHash.value);
    }
}

void EditorProjectTests::NativeCloneOriginsSurviveProjectRestore() {
    using namespace ObjectData;
    QTemporaryDir root; QVERIFY(root.isValid());
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit); QVERIFY(native.Succeeded());
    QVERIFY(native.value->AddCustomObject("E000", "Eevi").Succeeded());
    QVERIFY(native.value->AddModification({ObjectType::Unit, "E000"}, {"udef"}, NativeValueType::Unreal, 5.0f).Succeeded());
    QVERIFY(native.value->AddModification({ObjectType::Unit, "E000"}, {"ucam"}, NativeValueType::Int32, std::int32_t{0}).Succeeded());
    const NativeObjectCodec codec(ObjectType::Unit);
    const auto nativeBytes = codec.Serialize(*native.value); QVERIFY(nativeBytes.Succeeded());
    const auto source = std::filesystem::path(root.filePath("source.mpq").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.w3u", *nativeBytes.value},
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}}).Succeeded());
    const auto sourceHash = Sha256File(source); QVERIFY(sourceHash.Succeeded());
    auto archive = MapArchiveSession::Open(source); QVERIFY(archive.Succeeded());
    QUndoStack undo;
    EditorProject project;
    project.sourceArchivePath_ = source;
    const auto workspace = root.filePath("workspace"); QVERIFY(QDir().mkpath(workspace));
    project.projectRoot_ = std::filesystem::path(workspace.toStdWString());
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.LoadMapDocument();
    const auto lni = ConvertNativeObjectDataToLni(ObjectType::Unit, *nativeBytes.value); QVERIFY(lni.Succeeded());
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(*lni.value); QVERIFY(parsed.Succeeded());
    project.objectDocuments_[0].reset(static_cast<LniObjectDocument*>(parsed.document.release()));
    project.nativeObjectDocuments_[0] = std::make_unique<NativeObjectDocument>(*native.value);
    project.objectUndoStacks_[0] = &undo;
    QVERIFY(InstallOwnedGameBase(project, ObjectType::Unit, "Eevi"));
    project.ResetEditTracking();
    auto& edited = *project.objectDocuments_[0];
    QVERIFY(edited.CloneCustomObject({ObjectType::Unit, "E000"}, "E001").Succeeded());
    QVERIFY(edited.CloneCustomObject({ObjectType::Unit, "E001"}, "E002").Succeeded());
    QVERIFY(edited.SetModification({ObjectType::Unit, "E002"}, {"udef"}, 7.0f).Succeeded());
    project.ObserveObjectEdit(0);
    const auto fingerprint = edited.DocumentFingerprint();
    const auto target = std::filesystem::path(root.filePath("clone.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "clone project save" : saved.diagnostics.front().message.c_str());
    auto prepared = PrepareProjectOpen(target);
    QVERIFY2(prepared.Succeeded(), prepared.diagnostics.empty() ? "clone project restore" : prepared.diagnostics.front().message.c_str());
    const auto* restored = (*prepared.value)->objects[0].get(); QVERIFY(restored);
    QCOMPARE(restored->CloneSources().size(), std::size_t{2});
    QCOMPARE(restored->DocumentFingerprint(), fingerprint);
    auto candidate = *native.value;
    QVERIFY(ApplyLniChangesToNative(candidate, *restored).Succeeded());
    QCOMPARE(candidate.ModificationValueType({ObjectType::Unit, "E002"}, {"udef"}), std::optional{NativeValueType::Unreal});
    QCOMPARE(Sha256File(source).value, sourceHash.value);
    QCOMPARE(codec.Serialize(*native.value).value, nativeBytes.value);

    auto savedArchive = MapArchiveSession::Open(target); QVERIFY(savedArchive.Succeeded());
    const auto manifestBytes = savedArchive.value->ReadMember("project.json"); QVERIFY(manifestBytes.Succeeded());
    const auto manifest = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(manifestBytes.value->data()),
        static_cast<qsizetype>(manifestBytes.value->size()))).object();
    QCOMPARE(manifest["objectCloneSources"].toArray().size(), 1);
    int sequence = 0;
    const auto writeManifest = [&](const QJsonObject& changed) {
        std::vector<MapArchiveMember> members(savedArchive.value->Members().begin(), savedArchive.value->Members().end());
        const auto bytes = QJsonDocument(changed).toJson(QJsonDocument::Compact);
        auto member = std::find_if(members.begin(), members.end(), [](const auto& entry) { return entry.name == "project.json"; });
        if (member == members.end()) return std::filesystem::path{};
        const auto* start = reinterpret_cast<const std::byte*>(bytes.constData());
        member->data.assign(start, start + bytes.size());
        const auto path = std::filesystem::path(root.filePath(QString("tampered-%1.forge").arg(++sequence)).toStdWString());
        if (!WriteVerifiedArchive(path, members).Succeeded()) return std::filesystem::path{};
        return path;
    };
    const auto assertRejected = [&](const QJsonValue& origins) {
        auto changed = manifest; changed["objectCloneSources"] = origins;
        const auto path = writeManifest(changed);
        QVERIFY(!path.empty());
        QVERIFY(!PrepareProjectOpen(path).Succeeded());
        QCOMPARE(edited.DocumentFingerprint(), fingerprint);
        QCOMPARE(Sha256File(source).value, sourceHash.value);
    };
    const auto categories = manifest["objectCloneSources"].toArray();
    auto category = categories[0].toObject();
    auto rows = category["sources"].toArray();
    assertRejected(QString("not an array"));
    assertRejected(QJsonArray{category, category});
    auto unknown = category; unknown["kind"] = "unknown"; assertRejected(QJsonArray{unknown});
    auto wrongHash = category; wrongHash["native_sha256"] = QString(64, '0'); assertRejected(QJsonArray{wrongHash});
    auto duplicate = category; duplicate["sources"] = QJsonArray{rows[0], rows[0]}; assertRejected(QJsonArray{duplicate});
    auto cycle = category; cycle["sources"] = QJsonArray{QJsonObject{{"new_id", "E001"}, {"source_id", "E002"}},
        QJsonObject{{"new_id", "E002"}, {"source_id", "E001"}}}; assertRejected(QJsonArray{cycle});
    auto missing = category; missing["sources"] = QJsonArray{QJsonObject{{"new_id", "E001"}, {"source_id", "E999"}}}; assertRejected(QJsonArray{missing});
    auto existing = category; existing["sources"] = QJsonArray{QJsonObject{{"new_id", "E000"}, {"source_id", "E001"}}}; assertRejected(QJsonArray{existing});
    auto extra = category; extra["unexpected"] = true; assertRejected(QJsonArray{extra});
    auto oldCategory = category; oldCategory.remove("source_present");
    auto oldManifest = manifest; oldManifest["objectCloneSources"] = QJsonArray{oldCategory};
    const auto oldManifestPath = writeManifest(oldManifest); QVERIFY(!oldManifestPath.empty());
    QVERIFY(PrepareProjectOpen(oldManifestPath).Succeeded()); // 旧三字段记录仍要求真实存在的 native 来源。
    auto legacy = manifest; legacy.remove("objectCloneSources");
    const auto legacyPath = writeManifest(legacy); QVERIFY(!legacyPath.empty());
    const auto legacyOpened = PrepareProjectOpen(legacyPath); QVERIFY(legacyOpened.Succeeded());
    QVERIFY((*legacyOpened.value)->objects[0]->CloneSources().empty());
    undo.push(new QUndoCommand("pre-save clone snapshot"));
    const auto mapCandidate = std::filesystem::path(root.filePath("cloned.mpq").toStdWString());
    QVERIFY(project.SaveMapCopy(mapCandidate).has_value());
    QVERIFY(project.objectDocuments_[0]->CloneSources().empty());
    QCOMPARE(undo.count(), 0);
    QVERIFY(!project.HasUnsavedMapChanges());
    const auto second = std::filesystem::path(root.filePath("cloned-second.mpq").toStdWString());
    QVERIFY(project.SaveMapCopy(second).has_value());
    QVERIFY(!project.HasUnsavedMapChanges());
    const auto rebasedProject = std::filesystem::path(root.filePath("rebased.forge").toStdWString());
    QVERIFY(project.SaveProjectToPath(rebasedProject).Succeeded());
    QVERIFY(PrepareProjectOpen(rebasedProject).Succeeded());
    QCOMPARE(Sha256File(source).value, sourceHash.value);
}

void EditorProjectTests::TriggerChangesCannotSkipUnpreparedFramework() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto tool = std::filesystem::path(root.filePath("legacy").toStdWString());
    for (const auto* name : {"plugin/wave/include/fixture.j", "jass/system/ht/common.j", "jass/system/ht/blizzard.j"}) {
        const auto filePath = tool / name; std::filesystem::create_directories(filePath.parent_path());
        QFile file(QString::fromStdWString(filePath.wstring())); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("// fixture\n") > 0);
    }
    const auto source = std::filesystem::path(root.filePath("source.mpq").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.j", {std::byte{'x'}}}}).Succeeded());
    auto opened = MapArchiveSession::Open(source); QVERIFY(opened.Succeeded());
    EditorProject project; project.compilerDataRoot_ = tool; project.activeUiDataRoot_ = tool;
    for (bool triggerDirty : {false, true}) {
        project.triggerDirty_ = triggerDirty;
        MapSaveJob job; QVERIFY(job.FreezeArchive(*opened.value).Succeeded()); job.target = source.parent_path() / "candidate.mpq";
        QCOMPARE(project.PrepareScriptRebuild(job), EditorProject::ScriptRebuild::Failed);
        QVERIFY(!job.scriptSkipped); QVERIFY(!job.script); QVERIFY(!std::filesystem::exists(job.target));
    }
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "SCRIPT_INPUT_MISSING"));
}

void EditorProjectTests::ObjectQueriesDistinguishMissingFromUnparsedSources() {
    using State = Services::ProjectSearchState;
    EditorProject project;
    QCOMPARE(CaptureProjectObjectSources(project)[0].state, State::Unavailable);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("source.mpq").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.w3u", {std::byte{1}}}}).Succeeded());
    auto opened = MapArchiveSession::Open(source); QVERIFY(opened.Succeeded());
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    auto captured = CaptureProjectObjectSources(project);
    QCOMPARE(captured[0].state, State::Unavailable); // 成员存在但没有可解析文档。
    QVERIFY(std::all_of(captured.begin() + 1, captured.end(), [](const auto& row) { return row.state == State::Missing; }));
    project.archiveSession_.reset(); project.sourceArchivePath_ = source;
    QCOMPARE(CaptureProjectObjectSources(project)[1].state, State::Unavailable);
    project.sourceArchivePath_.clear(); project.projectRoot_ = std::filesystem::path(root.path().toStdWString());
    QVERIFY(QDir(root.path()).mkpath("table") && QDir(root.path()).mkpath("map"));
    for (const auto& relative : {"table/unit.ini", "map/war3map.w3t"}) {
        QFile file(root.filePath(relative)); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("invalid"), qint64{7});
    }
    auto workspace = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(workspace.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
    captured = CaptureProjectObjectSources(project);
    QCOMPARE(captured[0].state, State::Unavailable); QCOMPARE(captured[2].state, State::Unavailable);
    QCOMPARE(captured[1].state, State::Missing);
    project.projectRoot_ /= "different-root";
    QCOMPARE(CaptureProjectObjectSources(project)[1].state, State::Unavailable);
}

void EditorProjectTests::BoundedAssetsPreserveCurrentPrefix() {
    QTemporaryDir root;
    const auto source = std::filesystem::path(root.filePath("assets.mpq").toStdWString());
    const auto written = WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.j", {std::byte{'x'}}},
        {"Textures\\A.blp", {std::byte{1}}}, {"Textures\\B.blp", {std::byte{2}}}, {"Textures\\C.blp", {std::byte{3}}}});
    auto opened = MapArchiveSession::Open(source);
    QVERIFY(root.isValid() && written.Succeeded() && opened.Succeeded());
    EditorProject project; project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    project.assetEdits_ = {{"Textures\\B.blp", {}}, {"Textures\\C.blp", "Textures\\Moved.blp"}};
    project.pendingImports_ = {{"Pending\\First.blp", "first.blp", 10}, {"Pending\\Second.blp", "second.blp", 20}};
    const auto all = project.AssetEntries(), one = project.AssetEntries(1), three = project.AssetEntries(3), oversized = project.AssetEntries(9);
    const auto same = [](const AssetEntry& a, const AssetEntry& b) {
        return a.path == b.path && a.originalPath == b.originalPath && a.size == b.size && a.diskPath == b.diskPath;
    };
    QCOMPARE(all.size(), std::size_t{4});
    QVERIFY(project.AssetEntries(0).empty());
    QVERIFY(one.size() == 1 && std::equal(one.begin(), one.end(), all.begin(), same));
    QVERIFY(three.size() == 3 && std::equal(three.begin(), three.end(), all.begin(), same) && three.back().path == "Pending\\First.blp");
    QVERIFY(oversized.size() == all.size() && std::equal(oversized.begin(), oversized.end(), all.begin(), same)
        && all.back().path == "Pending\\Second.blp" && std::any_of(all.begin(), all.end(), [](const auto& entry) { return entry.path == "Textures\\Moved.blp"; }));
}

void EditorProjectTests::GeneratedObjectsSurviveCommitAndSecondSave() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    using namespace ObjectData;
    const auto source = std::filesystem::path(root.filePath("source.mpq").toStdWString());
    auto initial = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    QVERIFY(initial.Succeeded());
    const auto initialBytes = NativeObjectCodec(ObjectType::Unit).Serialize(*initial.value);
    QVERIFY(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.w3u", *initialBytes.value},
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}}).Succeeded());
    auto opened = MapArchiveSession::Open(source); QVERIFY(opened.Succeeded());
    EditorProject project;
    project.sourceArchivePath_ = source;
    project.projectRoot_ = std::filesystem::path(root.path().toStdWString());
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    project.LoadMapDocument();
    auto emptyLni = LniObjectCodec(ObjectType::Unit).Parse({});
    project.objectDocuments_[0].reset(static_cast<LniObjectDocument*>(emptyLni.document.release()));
    project.nativeObjectDocuments_[0] = std::make_unique<NativeObjectDocument>(std::move(*initial.value));
    QVERIFY(InstallOwnedGameBase(project, ObjectType::Unit, "hfoo"));
    project.ResetEditTracking();
    QUndoStack undo; project.objectUndoStacks_[0] = &undo;
    undo.push(new QUndoCommand("old document command"));
    QSignalSpy before(&project, &EditorProject::ObjectDocumentsAboutToReplace);
    QSignalSpy after(&project, &EditorProject::ObjectDocumentsReplaced);

    auto generated = NativeObjectDocument::CreateEmpty(ObjectType::Unit);
    const ObjectKey key{ObjectType::Unit, "H001"};
    QVERIFY(generated.value->AddCustomObject(key.rawcode, "hfoo").Succeeded());
    QVERIFY(generated.value->AddModification(key, {"unam"}, NativeValueType::String, std::string("generated")).Succeeded());
    QVERIFY(generated.value->AddModification(key, {"uhpm"}, NativeValueType::Int32, std::int32_t{987}).Succeeded());
    Script::PipelineResult pipeline;
    pipeline.script = "function main takes nothing returns nothing\nendfunction\n";
    pipeline.syntaxChecked = true; pipeline.checkedBy = "synthetic transaction fixture";
    pipeline.objectMembers = {{"war3map.w3u", *NativeObjectCodec(ObjectType::Unit).Serialize(*generated.value).value}};
    auto bundle = Script::PrepareScriptSaveBundle(pipeline, "war3map.j"); QVERIFY(bundle.Succeeded());
    std::array<const LniObjectDocument*, kObjectTypeCount> editableInputs{};
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) editableInputs[i] = project.objectDocuments_[i].get();
    QVERIFY(Script::PreflightScriptSaveBaselineReplacement(*bundle.value, editableInputs).Succeeded());
    bool mapObserverSawFinalObjects = false;
    QObject::connect(&project, &EditorProject::MapDocumentChanged, &project, [&]() {
        mapObserverSawFinalObjects = project.objectDocuments_[0] && project.objectDocuments_[0]->FindObject(key) &&
            project.nativeObjectDocuments_[0] && project.nativeObjectDocuments_[0]->FindObject(key);
    });
    MapSaveJob job;
    QVERIFY(job.FreezeArchive(*project.archiveSession_).Succeeded());
    job.target = std::filesystem::path(root.filePath("first.mpq").toStdWString());
    job.preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(),
        project.editRevision_.Revision(), project.editRevision_.Current()};
    job.nextBaselines = std::move(bundle.value->nativeBaselines);
    job.generatedObjectBaselines = std::move(bundle.value->editableBaselines);
    QVERIFY(Script::MergeScriptSaveMembers(job.replacements, {}, bundle.value->members).Succeeded());
    // 这里只注入原创已检查结果，真实运行保存/回读/唯一UI提交；不是Lua或编译器验收。
    EditorProject::RunMapSave(job);
    QVERIFY(job.succeeded);
    QVERIFY(project.CommitMapSave(job).has_value());
    QVERIFY(mapObserverSawFinalObjects);
    QCOMPARE(before.count(), 1); QCOMPARE(after.count(), 1); QCOMPARE(undo.count(), 0);
    QVERIFY(project.objectDocuments_[0]->FindObject(key));
    QVERIFY(project.nativeObjectDocuments_[0]->FindObject(key));
    QVERIFY(!project.HasUnsavedMapChanges());
    QVERIFY(project.objectDocuments_[0]->SetModification(key, {"unam"}, std::string("second edit")).Succeeded());
    project.ObserveObjectEdit(0);
    QVERIFY(project.HasUnsavedMapChanges());
    const auto secondPath = std::filesystem::path(root.filePath("second.mpq").toStdWString());
    QVERIFY(project.SaveMapCopy(secondPath).has_value());
    auto second = MapArchiveSession::Open(secondPath); QVERIFY(second.Succeeded());
    auto parsed = NativeObjectCodec(ObjectType::Unit).Parse(*second.value->ReadMember("war3map.w3u").value);
    QVERIFY(parsed.Succeeded());
    const auto* doc = static_cast<NativeObjectDocument*>(parsed.document.get());
    const auto* unit = doc->FindObject(key); QVERIFY(unit);
    bool health = false, name = false;
    for (const auto& modification : unit->modifications) {
        if (modification.key.fieldId == "uhpm") health = std::get<std::int32_t>(modification.value) == 987;
        if (modification.key.fieldId == "unam") name = std::get<std::string>(modification.value) == "second edit";
    }
    QVERIFY(health && name);
    project.objectUndoStacks_[0] = nullptr;
}

void EditorProjectTests::SaveCommitRejectsRevisionDrift() {
    for (int mode = 0; mode < 2; ++mode) {
        const bool inPlace = mode == 1;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto source = std::filesystem::path(root.filePath("source.mpq").toStdWString());
        const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
        QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
        auto opened = MapArchiveSession::Open(source);
        QVERIFY(opened.Succeeded());
        EditorProject project;
        project.sourceArchivePath_ = source;
        project.projectRoot_ = std::filesystem::path(root.path().toStdWString());
        project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
        project.LoadMapDocument();
        QVERIFY(project.mapDocument_ && project.mapDocument_->Settings() && project.mapEdits_);
        project.ResetEditTracking();
        const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map);
        QVERIFY(baseline.has_value());
        const auto target = std::filesystem::path(root.filePath("candidate.mpq").toStdWString());
        auto job = project.PrepareMapSave(inPlace ? std::filesystem::path{} : target, inPlace);
        QVERIFY(job && job->preparedRevision);

        // 模拟后台保存的嵌套事件循环中到达的编辑：任务只持有准备时的成员视图。
        auto settings = *project.mapDocument_->Settings();
        settings.mapName = "Late edit";
        QVERIFY(project.mapEdits_->EditSettings(settings).Succeeded());
        QVERIFY(project.ObserveEditState("保存期间修改地图名称"));
        const auto lateScopes = project.editRevision_.Current();
        QVERIFY(project.HasUnsavedMapChanges());

        EditorProject::RunMapSave(*job);
        QVERIFY(job->succeeded && job->nextSession && std::filesystem::exists(job->written));
        QVERIFY(!project.CommitMapSave(*job).has_value());
        QVERIFY(project.sourceArchivePath_ == source && project.mapEdits_ && project.mapEdits_->IsDirty());
        QCOMPARE(project.editRevision_.Current(), lateScopes);
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        const auto current = project.mapEdits_->Snapshot();
        QVERIFY(current.Settings());
        QCOMPARE(current.Settings()->mapName, std::string("Late edit"));
        QVERIFY(ModelHasCode(*project.diagnosticsModel_, QStringLiteral("SAVE_REVISION_CHANGED")));
        if (inPlace) {
            QVERIFY(!job->backup.empty() && std::filesystem::exists(job->backup));
            QVERIFY(project.sourceChangeSuspected_ && project.sourceChangeConfirmed_);
        } else {
            QVERIFY(!project.sourceChangeConfirmed_);
        }
    }
}

void EditorProjectTests::IndependentProjectsDoNotShareEdits() {
    QTemporaryDir firstRoot, secondRoot;
    QVERIFY(firstRoot.isValid() && secondRoot.isValid());
    EditorProject first, second;
    QVERIFY(LoadFixture(first, firstRoot.path()));
    QVERIFY(LoadFixture(second, secondRoot.path()));
    const auto original = first.CaptureEditScopes();
    QVERIFY(original.has_value());
    QCOMPARE(*original, *second.CaptureEditScopes());
    const auto secondFingerprint = second.ProjectFingerprint();
    QVERIFY(first.editRevision_.Reset(*original, *original, *original).Succeeded());
    first.editTrackingReady_ = true;
    ObjectData::ModificationKey health;
    health.fieldId = "uhpm";
    QVERIFY(first.objectDocuments_[0]->SetModification(
        {ObjectData::ObjectType::Unit, "hfoo"}, health, std::int32_t{750}).Succeeded());
    const auto edited = first.CaptureEditScopes();
    QVERIFY(edited.has_value() && *edited != *original);
    QVERIFY(first.ProjectFingerprint() != secondFingerprint);
    QCOMPARE(second.ProjectFingerprint(), secondFingerprint);
    QVERIFY(first.editRevision_.Observe(*edited, "修改生命值").Succeeded());
    QVERIFY(first.editRevision_.IsDirty(EditSaveTarget::Project));
    QVERIFY(first.objectDocuments_[0]->SetModification(
        {ObjectData::ObjectType::Unit, "hfoo"}, health, std::int32_t{420}).Succeeded());
    QVERIFY(first.editRevision_.Observe(*first.CaptureEditScopes(), "恢复生命值", EditOperation::Undo).Succeeded());
    QVERIFY(!first.editRevision_.IsDirty(EditSaveTarget::Project));
}

void EditorProjectTests::MissingImportCannotProduceSavedFingerprint() {
    QTemporaryDir root;
    EditorProject project;
    QVERIFY(LoadFixture(project, root.path()));
    const auto baseline = project.CaptureEditScopes();
    QVERIFY(baseline.has_value());
    QVERIFY(project.editRevision_.Reset(*baseline, *baseline, *baseline).Succeeded());
    project.editTrackingReady_ = true;
    project.pendingImports_.push_back({"Textures/missing.blp", root.filePath("absent.blp"), 10});
    QVERIFY(!project.CaptureEditScopes().has_value());
    QVERIFY(project.ProjectFingerprint().empty());
    QCOMPARE(project.editRevision_.Current(), *baseline);
    QCOMPARE(project.editRevision_.Revision(), std::uint64_t{0});
}

void EditorProjectTests::RevisionLifecycleTracksSeparateSaveTargets() {
    QTemporaryDir root;
    EditorProject project;
    QVERIFY(LoadFixture(project, root.path()));
    project.ResetEditTracking();
    QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    QVERIFY(!project.objectDirty_[0]);
    ObjectData::ModificationKey health;
    health.fieldId = "uhpm";
    QVERIFY(project.objectDocuments_[0]->SetModification(
        {ObjectData::ObjectType::Unit, "hfoo"}, health, std::int32_t{850}).Succeeded());
    project.ObserveObjectEdit(0);
    QVERIFY(project.objectDirty_[0]);
    const auto revision = project.editRevision_.Revision();
    project.ObserveObjectEdit(0); // 重复 UI 通知不增加修订。
    project.ObserveObjectEdit(-1);
    project.ObserveObjectEdit(7);
    QCOMPARE(project.editRevision_.Revision(), revision);
    const auto ticket = project.editRevision_.BeginSave(EditSaveTarget::Project);
    QVERIFY(ticket.Succeeded());
    QVERIFY(project.editRevision_.CompleteSave(*ticket.value, true).Succeeded());
    project.UpdateEditDirtyFlags();
    QVERIFY(project.objectDirty_[0]); // 工程保存不能清掉地图副本修改。
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Project));
    // 模拟调用方已经完成地图输出/回读后的通知，不冒充实际归档测试。
    project.CommitMapEditBaseline();
    QVERIFY(!project.objectDirty_[0]);
    QVERIFY(project.objectDocuments_[0]->SetModification(
        {ObjectData::ObjectType::Unit, "hfoo"}, health, std::int32_t{420}).Succeeded());
    project.ObserveObjectEdit(0);
    QVERIFY(project.objectDirty_[0]); // 回到最初内容仍区别于刚保存的 850。
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project));
}

void EditorProjectTests::MapDataObservationPreservesOtherScopesAndRejectsFailedTracking() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto bytes = MapDataFixtures::FullMapSettingsV25().bytes;
    QFile file(root.filePath("war3map.w3i")); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())), qsizetype(bytes.size()));
    file.close();
    EditorProject project; QVERIFY(LoadFixture(project, root.path())); project.LoadMapDocument();
    QVERIFY(project.mapEdits_ && project.mapDocument_->Settings());
    project.ResetEditTracking();
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    QVERIFY(project.objectDocuments_[0]->SetModification(
        {ObjectData::ObjectType::Unit, "hfoo"}, {"uhpm"}, std::int32_t{850}).Succeeded());
    project.ObserveObjectEdit(0);
    const auto objectScope = project.editRevision_.Current().at("objects/unit.ini");
    auto settings = *project.mapDocument_->Settings(); settings.mapName = "Scoped placement-style edit";
    QVERIFY(project.mapEdits_->EditSettings(settings).Succeeded());
    const auto edit = project.mapEdits_->LastEdit();
    QVERIFY(project.ObserveMapDataEdit("只观察公共地图成员"));
    const auto full = project.CaptureEditScopes(); QVERIFY(full);
    QCOMPARE(project.editRevision_.Current(), *full);
    QCOMPARE(project.editRevision_.Current().at("objects/unit.ini"), objectScope);
    const auto revision = project.editRevision_.Revision();
    QVERIFY(project.ObserveMapDataEdit("重复通知")); QCOMPARE(project.editRevision_.Revision(), revision);
    QVERIFY(project.mapEdits_->Replay(edit, false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("撤销地图成员"));
    QVERIFY(!project.editRevision_.Current().contains("map/war3map.w3i"));
    QCOMPARE(project.editRevision_.Current().at("objects/unit.ini"), objectScope);
    QVERIFY(project.objectDirty_[0]);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
    project.pendingImports_.push_back({"Textures/missing.blp", root.filePath("absent.blp"), 10});
    QVERIFY(!project.ObserveEditState("完整保存检查仍拒绝丢失来源"));
    QVERIFY(!project.ObserveMapDataEdit("局部观察不能清除无关失败")); QVERIFY(project.editTrackingFailed_);
    project.pendingImports_.clear(); QVERIFY(project.ObserveEditState("完整复核恢复"));
    project.directorySnapshot_.reset();
    QVERIFY(!project.ObserveMapDataEdit("来源失效不能借用旧scopes")); QVERIFY(project.editTrackingFailed_);
}

void EditorProjectTests::FailedObservationPreservesBaselineAndRecovers() {
    QTemporaryDir root;
    EditorProject project;
    QVERIFY(LoadFixture(project, root.path()));
    project.ResetEditTracking();
    const auto saved = project.editRevision_.Baseline(EditSaveTarget::Map);
    const auto revision = project.editRevision_.Revision();
    project.pendingImports_.push_back({"Textures/missing.blp", root.filePath("absent.blp"), 10});
    QVERIFY(!project.ObserveEditState("检查缺失导入"));
    QVERIFY(project.editTrackingFailed_);
    project.CommitMapEditBaseline();
    QCOMPARE(project.editRevision_.Revision(), revision);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, saved->scopes);
    project.pendingImports_.clear();
    QVERIFY(project.ObserveEditState("完整复核恢复"));
    QVERIFY(!project.editTrackingFailed_);
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    project.ObserveTriggerEdit(); // 缺少函数定义，不能把触发器当作成功序列化。
    QVERIFY(project.editTrackingFailed_);
    QCOMPARE(project.editRevision_.Revision(), revision);
    project.triggerDocument_.reset();
    QVERIFY(project.ObserveEditState("完整复核恢复"));
    QVERIFY(!project.editTrackingFailed_);
}

void EditorProjectTests::NewlyCreatedEmptyCategoryReturnsToClean() {
    QTemporaryDir root;
    EditorProject project;
    QVERIFY(LoadFixture(project, root.path()));
    project.ResetEditTracking();
    auto empty = ObjectData::LniObjectCodec(ObjectData::ObjectType::Ability).Parse({});
    QVERIFY(empty.Succeeded());
    project.objectDocuments_[1].reset(dynamic_cast<ObjectData::LniObjectDocument*>(empty.document.release()));
    QVERIFY(project.objectDocuments_[1] != nullptr);
    const auto before = *project.objectDocuments_[1];
    QVERIFY(project.objectDocuments_[1]->AddObject("A000", "AHbz").Succeeded());
    project.ObserveObjectEdit(1);
    QVERIFY(project.objectDirty_[1]);
    *project.objectDocuments_[1] = before; // 等价于创建命令撤销后的文档。
    project.ObserveObjectEdit(1);
    QVERIFY(!project.objectDirty_[1]);
    QVERIFY(!project.editRevision_.Current().contains("objects/ability.ini"));
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Map));
}

void EditorProjectTests::SourceWatchAndChecksWorkWithoutWindow() {
    QTemporaryDir root;
    QPointer<QFileSystemWatcher> watcher;
    {
        EditorProject project;
        QVERIFY(LoadFixture(project, root.path()));
        watcher = project.projectWatch_;
        QCOMPARE(watcher->parent(), &project);
        QSignalSpy notifications(&project, &EditorProject::SourceStateChanged);
        project.RefreshProjectWatch();
        QVERIFY(project.CheckProjectSources().Succeeded());
        const auto baseline = project.directorySnapshot_->Snapshot().aggregateSha256;
        const int beforeEvent = notifications.count();
        QFile changed(root.filePath("map.txt"));
        QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changed.write("changed"), qint64{7});
        changed.close();
        QTRY_VERIFY_WITH_TIMEOUT(project.sourceChangeSuspected_, 4000);
        QVERIFY(notifications.count() > beforeEvent);
        QVERIFY(!project.sourceChangeConfirmed_); // 事件不是已验证变化。
        const auto result = project.CheckProjectSources();
        QVERIFY(!result.Succeeded() && !result.diagnostics.empty());
        QVERIFY(project.sourceChangeConfirmed_);
        QCOMPARE(project.directorySnapshot_->Snapshot().aggregateSha256, baseline);
        project.ResetProjectWatch();
        QVERIFY(watcher->files().isEmpty() && watcher->directories().isEmpty());
        QVERIFY(!project.sourceChangeSuspected_ && !project.sourceChangeConfirmed_);
    }
    QVERIFY(watcher.isNull()); // 无主窗口也能正确销毁监视器。
}

void EditorProjectTests::SourceIdentityMismatchReturnsDiagnostics() {
    QTemporaryDir first, second;
    EditorProject project;
    QVERIFY(LoadFixture(project, first.path()));
    project.projectRoot_ = std::filesystem::path(second.path().toStdWString());
    const auto mismatched = project.CheckProjectSources();
    QVERIFY(!mismatched.Succeeded());
    QVERIFY(std::any_of(mismatched.diagnostics.begin(), mismatched.diagnostics.end(), [](const auto& item) {
        return item.code == "SOURCE_IDENTITY_CHANGED";
    }));
    project.projectRoot_ = std::filesystem::path(first.path().toStdWString());
    QVERIFY(project.CheckProjectSources().Succeeded());
    project.sourceArchivePath_ = std::filesystem::path(first.filePath("missing.w3x").toStdWString());
    const auto incomplete = project.CheckProjectSources();
    QVERIFY(!incomplete.Succeeded());
    QCOMPARE(incomplete.diagnostics.front().code, std::string("SOURCE_SNAPSHOT_UNAVAILABLE"));
}

void EditorProjectTests::SaveAndRecoveryWorkWithoutUndoWidgets() {
    QTemporaryDir root;
    const auto source = root.filePath("source");
    QVERIFY(QDir().mkpath(source));
    EditorProject project;
    QVERIFY(LoadFixture(project, source));
    project.ResetEditTracking();
    const auto sourceSha = project.directorySnapshot_->Snapshot().aggregateSha256;
    ObjectData::ModificationKey health;
    health.fieldId = "uhpm";
    const auto edit = [&](std::int32_t value) {
        const auto status = project.objectDocuments_[0]->SetModification(
            {ObjectData::ObjectType::Unit, "hfoo"}, health, value);
        if (status.Succeeded()) project.ObserveObjectEdit(0);
        return status.Succeeded();
    };
    QVERIFY(edit(800));
    const auto target = std::filesystem::path(root.filePath("project.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "save failed" : saved.diagnostics.front().message.c_str());
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Project));
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Map));
    const auto reopened = MapArchiveSession::Open(target);
    QVERIFY(reopened.Succeeded());
    QVERIFY(reopened.value->HasMember("overlay/table/unit.ini"));
    QCOMPARE(reopened.value->ReadMember("baseline/workspace/map.txt").value->size(), std::size_t{6});
    QVERIFY(project.directorySnapshot_->VerifyUnchanged().Succeeded());
    QCOMPARE(project.directorySnapshot_->Snapshot().aggregateSha256, sourceSha);
    QVERIFY(edit(900));
    const auto recoveryPath = std::filesystem::path(root.filePath("recovery.forge").toStdWString());
    const auto recovery = project.SaveProjectToPath(recoveryPath, true);
    QVERIFY(recovery.Succeeded());
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project));
    QCOMPARE(project.projectFilePath_, saved.value->path);
    QCOMPARE(project.projectFileSha256_, saved.value->sha256);
    const auto updated = project.SaveProjectToPath(target);
    QVERIFY(updated.Succeeded() && updated.value->backupPath.has_value());
    QCOMPARE(*Sha256File(*updated.value->backupPath).value, saved.value->sha256);
    QVERIFY(edit(1000));
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Project)->scopes;
    QFile external(root.filePath("project.forge"));
    QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(external.write("external"), qint64{8});
    external.close();
    QVERIFY(!project.SaveProjectToPath(target).Succeeded());
    QCOMPARE(project.projectFileSha256_, updated.value->sha256);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Project)->scopes, baseline);
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project));
    QFile check(root.filePath("project.forge"));
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), QByteArray("external"));
    // 失败票据必须被结束，随后独立另存仍能成功。
    const auto another = project.SaveProjectToPath(std::filesystem::path(root.filePath("another.forge").toStdWString()));
    QVERIFY(another.Succeeded());
}

void EditorProjectTests::PreparedOpenPreservesSessionAndCleansCancelledFiles() {
    QTemporaryDir root;
    const auto source = root.filePath("source");
    QVERIFY(QDir().mkpath(source));
    EditorProject current;
    QVERIFY(LoadFixture(current, source));
    current.ResetEditTracking();
    const auto target = std::filesystem::path(root.filePath("project.forge").toStdWString());
    QVERIFY(current.SaveProjectToPath(target).Succeeded());
    const auto* document = current.objectDocuments_[0].get();
    const auto fingerprint = current.ProjectFingerprint();
    std::filesystem::path extracted;
    {
        auto prepared = PrepareProjectOpen(target);
        QVERIFY2(prepared.Succeeded(), prepared.diagnostics.empty() ? "preflight" : prepared.diagnostics.front().message.c_str());
        const auto& candidate = **prepared.value;
        extracted = candidate.root;
        QVERIFY(std::filesystem::exists(extracted));
        QVERIFY(candidate.objects[0] != nullptr);
        QCOMPARE(candidate.objects[0]->DocumentFingerprint(), document->DocumentFingerprint());
        QVERIFY(!candidate.map && !candidate.recovery);
        QCOMPARE(current.objectDocuments_[0].get(), document);
        QCOMPARE(current.ProjectFingerprint(), fingerprint);
        // 不应用候选等价于取消，析构只清理候选文件。
    }
    QVERIFY(!std::filesystem::exists(extracted));
    QVERIFY(current.directorySnapshot_->VerifyUnchanged().Succeeded());
    QFile invalid(root.filePath("invalid.forge"));
    QVERIFY(invalid.open(QIODevice::WriteOnly));
    QCOMPARE(invalid.write("invalid"), qint64{7});
    invalid.close();
    QVERIFY(!PrepareProjectOpen(std::filesystem::path(invalid.fileName().toStdWString())).Succeeded());
    QCOMPARE(current.objectDocuments_[0].get(), document);
    QCOMPARE(current.ProjectFingerprint(), fingerprint);
}

void EditorProjectTests::MapDataOverlaySurvivesProjectSaveAndPreflight() {
    QTemporaryDir root;
    const auto source = root.filePath("source");
    QVERIFY(QDir().mkpath(source));
    const auto original = MapDataFixtures::FullMapSettingsV25().bytes;
    QFile sourceMember(source + "/war3map.w3i");
    QVERIFY(sourceMember.open(QIODevice::WriteOnly));
    QCOMPARE(sourceMember.write(reinterpret_cast<const char*>(original.data()),
        static_cast<qint64>(original.size())), static_cast<qint64>(original.size()));
    sourceMember.close();
    auto workspace = DirectoryWorkspace::Open(std::filesystem::path(source.toStdWString()));
    QVERIFY(workspace.Succeeded());
    EditorProject project;
    project.projectRoot_ = std::filesystem::path(source.toStdWString());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
    project.LoadMapDocument();
    QVERIFY(project.mapDocument_ && project.mapDocument_->Settings());
    project.ResetEditTracking();
    auto settings = *project.mapDocument_->Settings();
    settings.mapName = "EditFromProject";
    QVERIFY(project.mapEdits_->EditSettings(settings).Succeeded());
    QVERIFY(project.ObserveEditState("修改地图设置"));
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Map));

    const auto target = std::filesystem::path(root.filePath("map-data.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "save project" : saved.diagnostics.front().message.c_str());
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Project));
    auto archive = MapArchiveSession::Open(target);
    QVERIFY(archive.Succeeded() && archive.value->HasMember("overlay/map/war3map.w3i"));
    const auto overlay = archive.value->ReadMember("overlay/map/war3map.w3i");
    QVERIFY(overlay.Succeeded());
    const auto parsed = MapData::MapSettings::Parse(*overlay.value);
    QVERIFY(parsed.Succeeded());
    QCOMPARE(parsed.value->mapName, std::string("EditFromProject"));

    auto prepared = PrepareProjectOpen(target);
    QVERIFY2(prepared.Succeeded(), prepared.diagnostics.empty() ? "open project" : prepared.diagnostics.front().message.c_str());
    QVERIFY((*prepared.value)->mapDataEdits && (*prepared.value)->mapDataEdits->IsDirty());
    const auto replacements = (*prepared.value)->mapDataEdits->Replacements();
    QCOMPARE(replacements.size(), std::size_t{1});
    QCOMPARE(replacements.front().data, *overlay.value);
    QVERIFY(project.directorySnapshot_->VerifyUnchanged().Succeeded());
}

void EditorProjectTests::MapOnlyLibraryKeepsGlobalSettings() {
    // 隔离本机设置：测试只写入专用组织名下的键，结束后删除。
    const QString organization = QCoreApplication::organizationName();
    QCoreApplication::setOrganizationName(QStringLiteral("War3MapEditor64-Tests"));
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto write = [&root](const QString& name, const QByteArray& bytes) {
        QVERIFY(QDir(root.path()).mkpath(QFileInfo(root.filePath(name)).path()));
        QFile file(root.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    };
    write("ui/config", "base\r\nlib\r\n");
    write("ui/base/action.txt", "[DoNothing]\r\ntitle=\"Do nothing\"\r\n");
    write("ui/lib/action.txt", "[LibraryAction]\r\ntitle=\"Library\"\r\n");
    // 一个分类、一个触发器，根节点是库动作 LibraryAction（无参数）。
    QByteArray wtg("WTG!");
    const auto i32 = [](QByteArray& bytes, std::int32_t value) { bytes.append(reinterpret_cast<const char*>(&value), 4); };
    const auto text = [](QByteArray& bytes, const char* value) { bytes.append(value); bytes.append('\0'); };
    i32(wtg, 7); i32(wtg, 1); i32(wtg, 1); text(wtg, "Category"); i32(wtg, 0);
    i32(wtg, 2); i32(wtg, 0); i32(wtg, 1);
    text(wtg, "Uses library"); text(wtg, "");
    for (const std::int32_t flag : {0, 1, 0, 0, 0, 1, 1, 2}) i32(wtg, flag);
    text(wtg, "LibraryAction"); i32(wtg, 1); i32(wtg, 0);
    QByteArray wct;
    i32(wct, 1); text(wct, ""); i32(wct, 0); i32(wct, 1); i32(wct, 0);
    write("map/war3map.wtg", wtg);
    write("map/war3map.wct", wct);

    const std::filesystem::path uiRoot(root.path().toStdWString());
    UiLibrary::GlobalSettings global;
    global.packages = {{"base", true, {}}, {"lib", false, {}}};
    EditorProject::WriteUiLibrarySettings(uiRoot, global);

    EditorProject project;
    project.projectRoot_ = uiRoot;
    project.activeUiDataRoot_ = uiRoot;
    QVERIFY(project.LoadTriggers());
    QVERIFY(project.triggerDocument_ != nullptr);
    QCOMPARE(project.uiLibraryMapOnly_, std::vector<std::string>{"lib"});
    QVERIFY(project.uiLibraryUsage_.has_value());
    QCOMPARE(project.uiLibraryUsage_->packages.size(), std::size_t{1});
    QCOMPARE(project.uiLibraryUsage_->packages[0].id, std::string("lib"));
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "UI_LIBRARY_MAP_ONLY"));
    // 全局设置不变，数据根 config 不变。
    const auto after = EditorProject::ReadUiLibrarySettings(uiRoot);
    QCOMPARE(after.packages.size(), std::size_t{2});
    QVERIFY(!after.packages[1].enabled);
    QFile config(root.filePath("ui/config"));
    QVERIFY(config.open(QIODevice::ReadOnly));
    QCOMPARE(config.readAll(), QByteArray("base\r\nlib\r\n"));
    QCOMPARE(project.UiLibraryUsageJson().size(), 1);

    // 全局启用后无需仅本图启用。
    global.packages[1].enabled = true;
    QVERIFY(project.ApplyUiLibrarySettings(global, {}));
    QVERIFY(project.uiLibraryMapOnly_.empty());
    QVERIFY(project.triggerDocument_ != nullptr);

    // 热重载（§4.6）：包内文本变化后按新定义重新载入；有未导出修改时只提示、不丢修改。
    QVERIFY(project.DetectUiLibraryChanges().empty());
    QSignalSpy changed(&project, &EditorProject::UiLibrariesChanged);
    write("ui/lib/action.txt", "[LibraryAction]\r\ntitle=\"Library v2\"\r\n");
    QVERIFY(changed.wait(5000));
    QCOMPARE(changed.last().at(0).toStringList(), QStringList{QStringLiteral("lib")});
    QVERIFY(changed.last().at(1).toBool());
    QVERIFY(project.triggerDocument_ != nullptr);
    QVERIFY(project.DetectUiLibraryChanges().empty());
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "UI_LIBRARY_RELOADED"));
    const auto* before = project.triggerDocument_.get();
    project.triggerDirty_ = true;
    write("ui/lib/action.txt", "[LibraryAction]\r\ntitle=\"Library v3\"\r\n");
    QVERIFY(changed.wait(5000));
    QVERIFY(!changed.last().at(1).toBool());
    QCOMPARE(project.triggerDocument_.get(), before); // 未重新载入
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "UI_LIBRARY_CHANGED"));

    QSettings{}.remove(QStringLiteral("uiLibraries"));
    QCoreApplication::setOrganizationName(organization);
}

void EditorProjectTests::OwnedLuaProjectRecoveryPreservesPayloadRoleAndMapGate() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(QDir{}.mkpath(root.filePath("source-workspace")));
    const auto source = std::filesystem::path(root.filePath("source-workspace/source.mpq").toStdWString());
    const std::vector<MapArchiveMember> sourceMembers{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}
    };
    QVERIFY(WriteVerifiedArchive(source, sourceMembers).Succeeded());
    const auto sourceHash = Sha256File(source); QVERIFY(sourceHash.Succeeded());
    auto opened = MapArchiveSession::Open(source); QVERIFY(opened.Succeeded());
    EditorProject project; project.sourceArchivePath_ = source;
    project.projectRoot_ = source.parent_path();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    project.LoadMapDocument(); project.ResetEditTracking();
    const std::string text = "-- 工程应能恢复尚未写完的 Lua 草稿\nfunction unfinished(\n";
    const auto payload = std::make_shared<const std::vector<std::byte>>(
        std::as_bytes(std::span(text.data(), text.size())).begin(), std::as_bytes(std::span(text.data(), text.size())).end());
    auto state = project.CaptureAssets();
    QVERIFY(PutOwnedAsset(project, state, "scripts/demo.lua", payload).isEmpty());
    project.RestoreAssets(state);
    const auto target = std::filesystem::path(root.filePath("owned.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(target, true);
    QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "owned project save" : saved.diagnostics.front().message.c_str());
    auto prepared = PrepareProjectOpen(target);
    QVERIFY2(prepared.Succeeded(), prepared.diagnostics.empty() ? "owned project reopen" : prepared.diagnostics.front().message.c_str());
    const auto& imports = (*prepared.value)->imports;
    QCOMPARE(imports.size(), std::size_t{1}); QVERIFY(imports[0].ownedPayload);
    QCOMPARE(*imports[0].ownedPayload, *payload); QVERIFY(imports[0].diskPath.isEmpty()); QVERIFY(imports[0].runtimeLuaDraft);
    QVERIFY(!std::filesystem::exists((*prepared.value)->root / "imports/0")); // 内存正文不提取成临时磁盘文件。
    QVERIFY(!project.PrepareMapSave(std::filesystem::path(root.filePath("forbidden.mpq").toStdWString()), false));
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MAP_LUA_SYNTAX"));
    auto package = MapArchiveSession::Open(target); QVERIFY(package.Succeeded());
    auto members = std::vector<MapArchiveMember>(package.value->Members().begin(), package.value->Members().end());
    auto manifest = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "project.json"; });
    QVERIFY(manifest != members.end());
    auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(manifest->data.data()),
        static_cast<qsizetype>(manifest->data.size()))).object();
    auto records = json["imports"].toArray(); auto record = records[0].toObject(); record["runtimeLuaDraft"] = false;
    records[0] = record; json["imports"] = records; const auto serialized = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const auto bytes = std::as_bytes(std::span(serialized.data(), static_cast<std::size_t>(serialized.size())));
    manifest->data.assign(bytes.begin(), bytes.end());
    const auto tampered = std::filesystem::path(root.filePath("role-cleared.forge").toStdWString());
    QVERIFY(WriteVerifiedArchive(tampered, members).Succeeded());
    auto roleCleared = PrepareProjectOpen(tampered); QVERIFY(roleCleared.Succeeded());
    EditorProject recovered; recovered.RestoreAssets({(*roleCleared.value)->imports, {}});
    QVERIFY(!recovered.PrepareMapSave(std::filesystem::path(root.filePath("bypass.mpq").toStdWString()), false));
    QVERIFY(ModelHasCode(*recovered.diagnosticsModel_, "MAP_LUA_SYNTAX"));
    const auto diskPath = root.filePath("demo.lua"); QFile disk(diskPath); QVERIFY(disk.open(QIODevice::WriteOnly));
    QCOMPARE(disk.write("local answer = 43\n"), qint64{18}); disk.close();
    QCOMPARE(project.AddImportFiles({diskPath}, "scripts"), 1);
    QVERIFY(!project.pendingImports_[0].ownedPayload); QVERIFY(project.pendingImports_[0].runtimeLuaDraft);
    const auto diskProject = std::filesystem::path(root.filePath("disk-replacement.forge").toStdWString());
    QVERIFY(project.SaveProjectToPath(diskProject, true).Succeeded());
    auto diskPrepared = PrepareProjectOpen(diskProject); QVERIFY(diskPrepared.Succeeded());
    QCOMPARE((*diskPrepared.value)->imports.size(), std::size_t{1});
    QVERIFY(!(*diskPrepared.value)->imports[0].ownedPayload && (*diskPrepared.value)->imports[0].runtimeLuaDraft);
    QVERIFY(QFile::exists((*diskPrepared.value)->imports[0].diskPath));
    QCOMPARE(Sha256File(source).value, sourceHash.value); QVERIFY(!std::filesystem::exists(root.filePath("forbidden.mpq").toStdWString()));
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) return 2;
    // 保存链读取真正的设置；本夹具的设置和备份均归临时根所有，不能碰作者配置。
    QCoreApplication::setOrganizationName(QStringLiteral("Audastra-EditorProject-Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("EditorProject"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot.path());
    {
        QSettings settings;
        settings.setValue(QStringLiteral("mapSave/backupDirectory"), settingsRoot.filePath("backups"));
        settings.sync();
        if (settings.status() != QSettings::NoError) return 2;
    }
    EditorProjectTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "EditorProjectTests.moc"
