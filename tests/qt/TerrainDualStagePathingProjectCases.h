#pragma once

#include "PendingImportPayload.h"
#include "War3MapCore/Resources/ImportTable.h"

// 由EditorProjectTests.cpp包含；本头只覆盖Qt冻结、工程恢复、worker及保存提交。
// 沿既有原创worker夹具替代脚本工具链准备，不把这些测试当作脚本编译或原版进图验收。
namespace {
namespace DualStageQt {
using Bytes = std::vector<std::byte>;
constexpr auto kDoodad = ObjectData::ObjectType::Doodad;
constexpr auto kDoodadIndex = static_cast<std::size_t>(kDoodad);
constexpr std::string_view kOldTexture = "PathTextures/dual-stage-old.tga";
constexpr std::string_view kNewTexture = "PathTextures/dual-stage-new.tga";

Bytes Texture(bool final) {
    const unsigned width = final ? 8u : 4u, height = 4;
    Bytes bytes(18 + width * height * 3, std::byte{0});
    bytes[2] = std::byte{2}; bytes[12] = static_cast<std::byte>(width);
    bytes[14] = static_cast<std::byte>(height); bytes[16] = std::byte{24};
    for (std::size_t at = 18; at < bytes.size(); at += 3) {
        if (final) bytes[at + 1] = std::byte{0xff};
        else bytes[at] = bytes[at + 2] = std::byte{0xff};
    }
    return bytes;
}

const Bytes& Member(const std::vector<MapArchiveMember>& members, std::string_view name) {
    const auto found = std::find_if(members.begin(), members.end(),
        [&](const auto& member) { return member.name == name; });
    if (found == members.end()) throw std::runtime_error("dual-stage fixture member absent");
    return found->data;
}

std::vector<MapArchiveMember> SourceMembers() {
    auto members = NativePathingQtMembers();
    MapData::DoodadPlacements placements;
    MapData::DoodadPlacement placed;
    placed.rawcode = "Doo0"; placed.position = {512, 512, 0};
    placed.scale = {1, 1, 1}; placed.flags = 2; placed.creationId = 1;
    placements.doodads.push_back(placed);
    const auto doo = Tests::WaterTake(placements.Serialize());
    for (auto& member : members) {
        if (member.name == "war3map.doo") member.data = doo;
        if (member.name == "war3map.wpm") {
            // G0独立矩阵：浅水08、作者11，4x4红蓝ca覆盖[14,18)^2，合成为db。
            for (unsigned y = 14; y < 18; ++y) for (unsigned x = 14; x < 18; ++x)
                member.data[16 + y * 32 + x] = std::byte{0xdb};
        }
    }
    members.push_back({"war3mapUnits.doo", Tests::WaterTake(MapData::UnitPlacements{}.Serialize())});
    members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable({"destructableID"}, {{"Dst0"}})});
    members.push_back({"Units/DestructableMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
    members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable(
        {"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kOldTexture), "E", "270"}})});
    members.push_back({"Doodads/DoodadMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"dptx", "pathTex", "DoodadData", "pathingTexture", "0", "0", "0"}})});
    auto native = Tests::WaterTake(ObjectData::NativeObjectDocument::CreateEmpty(kDoodad));
    const auto added = native.AddOriginalObject("Doo0");
    if (!added.Succeeded()) throw std::runtime_error(ReadDiagnosticText(added));
    const auto modified = native.AddModification({kDoodad, "Doo0"}, {"dptx"},
        ObjectData::NativeValueType::String, std::string(kOldTexture));
    if (!modified.Succeeded()) throw std::runtime_error(ReadDiagnosticText(modified));
    members.push_back({"war3map.w3d", Tests::WaterTake(ObjectData::NativeObjectCodec(kDoodad).Serialize(native))});
    members.push_back({std::string(kOldTexture), Texture(false)});
    auto imports = Resources::ImportTable::CreateEmpty();
    if (!imports.Upsert(kOldTexture).Succeeded()) throw std::runtime_error("dual-stage import fixture");
    members.push_back({"war3map.imp", Tests::WaterTake(imports.Serialize())});
    members.push_back({"war3mapMap.tga", Tests::WaterText("authored dual-stage minimap stays unchanged")});
    return members;
}

bool Bind(EditorProject& project, const std::filesystem::path& source, const std::filesystem::path& game) {
    if (!BindTerrainSaveProject(project, source)) return false;
    project.war3GameRoot_ = game;
    const auto bytes = project.archiveSession_->ReadMember("war3map.w3d");
    if (!bytes.Succeeded() || !bytes.value) return false;
    auto parsed = ObjectData::NativeObjectCodec(kDoodad).Parse(*bytes.value);
    const auto* native = parsed.Succeeded()
        ? dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
    const auto text = ObjectData::ConvertNativeObjectDataToLni(kDoodad, *bytes.value);
    if (!native || !text.Succeeded() || !text.value) return false;
    auto editable = ObjectData::LniObjectCodec(kDoodad).Parse(*text.value);
    auto* lni = editable.Succeeded() ? dynamic_cast<ObjectData::LniObjectDocument*>(editable.document.get()) : nullptr;
    if (!lni) return false;
    project.nativeObjectDocuments_[kDoodadIndex] = std::make_unique<ObjectData::NativeObjectDocument>(*native);
    project.objectDocuments_[kDoodadIndex].reset(static_cast<ObjectData::LniObjectDocument*>(editable.document.release()));
    project.ResetEditTracking();
    return project.editTrackingReady_ && !project.editTrackingFailed_;
}

bool Edit(EditorProject& project, bool objectOverride) {
    const auto path = objectOverride ? kNewTexture : kOldTexture;
    std::vector<EditorProject::ImportPayload> payloads{{QString::fromUtf8(path.data(), qsizetype(path.size())),
        std::make_shared<const Bytes>(Texture(true))}};
    if (!project.AddImportPayloads(payloads, true, "替换原创静态占地图").isEmpty()) return false;
    if (payloads.front().replaced == objectOverride) return false;
    if (objectOverride) {
        const auto* object = project.objectDocuments_[kDoodadIndex]->FindObject({kDoodad, "Doo0"});
        if (!object) return false;
        const auto field = std::find_if(object->modifications.begin(), object->modifications.end(),
            [](const auto& modification) { return modification.key.fieldId == "dptx"; });
        if (field == object->modifications.end()) return false;
        // Doodad的完整键含variation=0/dataIndex=0，不能新增一个无索引同名字段。
        const auto changed = project.objectDocuments_[kDoodadIndex]->SetModification({kDoodad, "Doo0"},
            field->key, std::string(kNewTexture));
        if (!changed.Succeeded()) return false;
        project.ObserveObjectEdit(int(kDoodadIndex));
    }
    if (!DeepenNativePathingQtProject(project)) return false;
    PushMapDataEdit(project, "Dual-stage water", "与占地资源同存的深水修改");
    return true;
}

Bytes ExpectedPathing(const std::vector<MapArchiveMember>& members, bool deep) {
    auto bytes = Member(members, "war3map.wpm");
    std::fill(bytes.begin() + 16, bytes.end(), std::byte{0x19});
    // G1完整独立矩阵：8x4绿色只加04，[12,20)x[14,18)为1d。
    // 原红色c2必须消失；单顶点深水额外02只作用于[14,18)^2。
    for (unsigned y = 14; y < 18; ++y) for (unsigned x = 12; x < 20; ++x)
        bytes[16 + y * 32 + x] = std::byte{0x1d};
    if (deep) for (unsigned y = 14; y < 18; ++y) for (unsigned x = 14; x < 18; ++x)
        bytes[16 + y * 32 + x] = std::byte{0x1f};
    return bytes;
}

Bytes ExpectedTerrain(const std::vector<MapArchiveMember>& members, bool deep) {
    auto terrain = Tests::WaterTake(MapData::ParseMapTerrain(Member(members, "war3map.w3e")));
    auto grid = *terrain.Snapshot();
    if (deep) grid.vertices[4 * 9 + 4].waterHeightAndFlagsRaw = 8192 + 80 * 4;
    MapData::RefreshTerrainDerived(grid, 0, 0, 9, 9);
    return Tests::WaterTake(MapData::SerializeMapTerrain(grid));
}

std::unique_ptr<MapSaveJob> Freeze(EditorProject& project, const std::filesystem::path& target) {
    if (!project.ObserveEditState("冻结原创组合地形保存")) return {};
    auto job = FreezeNativePathingQtJob(project, target);
    if (!job) return {};
    // 与生产准备相同：只对任务副本回灌已有类型字段，不用手写W3D替代待保存的LNI。
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) {
        if (!project.nativeObjectDocuments_[i]) continue;
        job->nextBaselines[i] = std::make_unique<ObjectData::NativeObjectDocument>(*project.nativeObjectDocuments_[i]);
        if (!project.objectDirty_[i]) continue;
        if (!project.objectDocuments_[i] || !ObjectData::ApplyLniChangesToNative(*job->nextBaselines[i],
            *project.objectDocuments_[i], project.objectFieldSchema_.get(), project.standardCatalog_.get()).Succeeded()) return {};
        auto encoded = ObjectData::NativeObjectCodec(static_cast<ObjectData::ObjectType>(i)).Serialize(*job->nextBaselines[i]);
        if (!encoded.Succeeded() || !encoded.value) return {};
        job->replacements.push_back({std::string(ObjectData::NativeFileName(static_cast<ObjectData::ObjectType>(i))), std::move(*encoded.value)});
    }
    if (!project.assetEdits_.empty()) return {}; // 本夹具只使用真正导入，不默默丢弃路径编辑。
    if (!project.pendingImports_.empty()) {
        const auto source = job->archive->ReadMember("war3map.imp");
        if (!source.Succeeded() || !source.value) return {};
        auto imports = Resources::ImportTable::Parse(*source.value);
        if (!imports.Succeeded() || !imports.value) return {};
        for (std::size_t i = 0; i < project.pendingImports_.size(); ++i) {
            const auto& entry = project.pendingImports_[i];
            const auto scope = job->preparedRevision->scopes.find("imports/" + std::to_string(i));
            if (scope == job->preparedRevision->scopes.end() || scope->second.size() < 64) return {};
            auto payload = ReadFrozenPendingImport(entry, std::string_view(scope->second).substr(scope->second.size() - 64));
            if (!payload.Succeeded() || !payload.value) return {};
            const auto name = entry.archivePath.toStdString();
            if (!imports.value->Upsert(name, [&](std::string_view member) { return job->archive->HasMember(member); }).Succeeded()) return {};
            job->replacements.push_back({name, std::move(*payload.value)});
        }
        auto encoded = imports.value->Serialize();
        if (!encoded.Succeeded() || !encoded.value) return {};
        job->replacements.push_back({"war3map.imp", std::move(*encoded.value)});
    }
    job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
    InstallMapSaveDerivedPreparation(*job);
    return job;
}

bool Restore(EditorProject& project, PreparedProjectOpen& candidate, const std::filesystem::path& game) {
    if (!candidate.map || candidate.recovery || !candidate.mapDataEdits || candidate.triggers) return false;
    const auto source = candidate.sourceMapMatchesBaseline ? candidate.sourceMapPath : candidate.BaselinePath();
    if (!Bind(project, source, game)) return false;
    const auto originalScopes = project.editRevision_.Current();
    project.recordedSourceMapPath_ = candidate.sourceMapPath;
    project.restoredFiles_ = std::move(candidate.extraction);
    project.mapEdits_ = std::move(candidate.mapDataEdits);
    project.terrainJournalWarning_ = candidate.terrainJournalWarning;
    if (project.mapDataUndoStack_) for (auto& receipt : candidate.terrainReceipts)
        project.mapDataUndoStack_->push(new MapDataEditCommand(project, std::move(receipt), "Restored dual-stage water", "恢复组合地形笔划"));
    for (std::size_t i = 0; i < candidate.objects.size(); ++i) {
        if (!candidate.objects[i]) continue;
        if (!project.nativeObjectDocuments_[i]) return false;
        project.objectDocuments_[i] = std::move(candidate.objects[i]); project.objectDirty_[i] = true;
    }
    project.pendingImports_ = std::move(candidate.imports);
    project.assetEdits_ = std::move(candidate.assetEdits);
    project.NotifyImportsChanged();
    project.lastProjectFingerprint_ = project.ProjectFingerprint();
    project.restoredProjectJournalJson_ = candidate.journalJson;
    project.projectFilePath_ = candidate.sourcePath; project.projectFileSha256_ = candidate.sourceSha256;
    const auto scopes = project.CaptureEditScopes();
    if (!scopes) return false;
    project.editTrackingReady_ = project.editRevision_.Reset(*scopes, *scopes, originalScopes).Succeeded();
    project.editTrackingFailed_ = !project.editTrackingReady_; project.UpdateEditDirtyFlags();
    return project.editTrackingReady_;
}

void VerifyOutput(const MapArchiveSession& archive, const std::vector<MapArchiveMember>& originals,
    bool objectOverride, bool deep) {
    QCOMPARE(archive.ReadMember("war3map.w3e").value, std::optional(ExpectedTerrain(originals, deep)));
    QCOMPARE(archive.ReadMember("war3map.wpm").value, std::optional(ExpectedPathing(originals, deep)));
    const auto path = objectOverride ? kNewTexture : kOldTexture;
    QCOMPARE(archive.ReadMember(std::string(path)).value, std::optional(Texture(true)));
    if (objectOverride) QCOMPARE(archive.ReadMember(std::string(kOldTexture)).value, std::optional(Texture(false)));
    const auto native = archive.ReadMember("war3map.w3d"); QVERIFY(native.Succeeded() && native.value);
    if (!objectOverride) QCOMPARE(native.value, std::optional(Member(originals, "war3map.w3d")));
    auto parsed = ObjectData::NativeObjectCodec(kDoodad).Parse(*native.value); QVERIFY(parsed.Succeeded());
    const auto* document = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get()); QVERIFY(document);
    const auto* object = document->FindObject({kDoodad, "Doo0"}); QVERIFY(object);
    const auto field = std::find_if(object->modifications.begin(), object->modifications.end(),
        [](const auto& modification) { return modification.key.fieldId == "dptx"; });
    QVERIFY(field != object->modifications.end()); QCOMPARE(std::get<std::string>(field->value), std::string(path));
    const auto imp = archive.ReadMember("war3map.imp"); QVERIFY(imp.Succeeded() && imp.value);
    if (!objectOverride) QCOMPARE(imp.value, std::optional(Member(originals, "war3map.imp")));
    const auto imports = Resources::ImportTable::Parse(*imp.value); QVERIFY(imports.Succeeded() && imports.value);
    const auto originalImports = Resources::ImportTable::Parse(Member(originals, "war3map.imp"));
    QVERIFY(originalImports.Succeeded() && originalImports.value);
    QCOMPARE(imports.value->Entries().size(), originalImports.value->Entries().size() + (objectOverride ? 1u : 0u));
    QCOMPARE(archive.ReadMember("war3map.mmp").value, std::optional(Bytes(8, std::byte{0})));
    for (const auto& member : originals) {
        if (member.name == "war3map.w3e" || member.name == "war3map.wpm" || member.name == "war3map.w3d"
            || member.name == "war3map.imp" || member.name == "war3map.mmp" || member.name == kOldTexture) continue;
        QCOMPARE(archive.ReadMember(member.name).value, std::optional(member.data));
    }
}
} // namespace DualStageQt
} // namespace

void EditorProjectTests::DualStageStaticPathingSaveRebase_data() {
    QTest::addColumn<bool>("objectOverride");
    QTest::newRow("same-path-footprint-import-and-water") << false;
    QTest::newRow("native-pathTex-and-new-import-and-water") << true;
}

void EditorProjectTests::DualStageStaticPathingSaveRebase() {
    QFETCH(bool, objectOverride);
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto members = DualStageQt::SourceMembers(); QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    // 工程析构会显式释放资源撤销栈；沿用它持有的栈，断言早退也不能留下栈地址。
    EditorProject project;
    auto& terrainUndo = *project.mapDataUndoStack_;
    auto& assetsUndo = *project.assetUndoStack_;
    QVERIFY(DualStageQt::Bind(project, source, game)); QVERIFY(DualStageQt::Edit(project, objectOverride));
    QCOMPARE(terrainUndo.count(), 1); QCOMPARE(assetsUndo.count(), 1);
    const auto session = project.mapEdits_->SessionIdentity();
    const auto target = std::filesystem::path(root.filePath("saved.w3x").toStdWString());
    for (int phase = 0; phase < 3; ++phase) {
        if (phase == 1) terrainUndo.undo();
        if (phase == 2) terrainUndo.redo();
        const bool deep = phase != 1;
        const auto previousSha = Sha256File(project.sourceArchivePath_); QVERIFY(previousSha.Succeeded());
        const auto previousSourceRevision = project.mapEdits_->TerrainSourceRevision();
        auto job = DualStageQt::Freeze(project, target); QVERIFY(job && job->terrainDerivationRequired);
        // 首存必须使用旧归档G0，后续Undo/Redo从刚保存的新占地图和新WPM建立G0。
        QCOMPARE(job->archive->ReadMember(std::string(DualStageQt::kOldTexture)).value,
            std::optional(DualStageQt::Texture(phase != 0 && !objectOverride)));
        if (phase != 0) {
            job->inPlace = true; job->inPlaceOptions.backupDirectory =
                std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        EditorProject::RunMapSave(*job); QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded() && actual.value);
        DualStageQt::VerifyOutput(*actual.value, members, objectOverride, deep);
        QVERIFY(project.CommitMapSave(*job));
        QCOMPARE(project.sourceArchivePath_, target); QCOMPARE(project.mapEdits_->SessionIdentity(), session);
        QVERIFY(project.mapEdits_->TerrainSourceRevision() > previousSourceRevision);
        QVERIFY(!project.HasUnsavedMapChanges()); QVERIFY(project.pendingImports_.empty());
        QCOMPARE(assetsUndo.count(), 0); // 资源receipt依赖旧归档，保存后按产品设计清除。
        QCOMPARE(terrainUndo.count(), 1); QCOMPARE(terrainUndo.index(), deep ? 1 : 0);
        if (phase != 0) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, previousSha.value); }
        QCOMPARE(Sha256File(source).value, sourceSha.value);
    }
    terrainUndo.clear();
}

void EditorProjectTests::DualStageStaticPathingProjectColdRestore_data() {
    DualStageStaticPathingSaveRebase_data();
}

void EditorProjectTests::DualStageStaticPathingWithoutTerrainNet_data() {
    QTest::addColumn<bool>("objectOverride");
    QTest::addColumn<int>("minimapPolicy"); // 0作者TGA，1登记的作者BLP，2显式保留。
    QTest::addColumn<QString>("mode");
    QTest::newRow("same-path-author-tga") << false << 0 << QStringLiteral("success");
    QTest::newRow("native-path-author-tga") << true << 0 << QStringLiteral("success");
    QTest::newRow("same-path-imported-author-blp") << false << 1 << QStringLiteral("success");
    QTest::newRow("native-path-explicit-preserve") << true << 2 << QStringLiteral("success");
    QTest::newRow("native-path-publish-failure") << true << 0 << QStringLiteral("publish-failure");
    QTest::newRow("same-path-late-import") << false << 0 << QStringLiteral("late-import");
    QTest::newRow("native-path-late-object") << true << 0 << QStringLiteral("late-object");
}

void EditorProjectTests::DualStageStaticPathingWithoutTerrainNet() {
    QFETCH(bool, objectOverride); QFETCH(int, minimapPolicy); QFETCH(QString, mode);
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    auto members = DualStageQt::SourceMembers();
    const std::string minimap = minimapPolicy == 1 ? "war3mapMap.blp" : "war3mapMap.tga";
    if (minimapPolicy == 1) {
        for (auto& member : members) {
            if (member.name == "war3mapMap.tga") {
                member.name = minimap; member.data = Tests::WaterText("authored dual-stage BLP stays unchanged");
            }
            if (member.name == "war3map.imp") {
                auto imports = Resources::ImportTable::Parse(member.data); QVERIFY(imports.Succeeded() && imports.value);
                QVERIFY(imports.value->Upsert(minimap).Succeeded());
                auto encoded = imports.value->Serialize(); QVERIFY(encoded.Succeeded() && encoded.value);
                member.data = std::move(*encoded.value);
            }
        }
    }
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project;
    auto& terrainUndo = *project.mapDataUndoStack_;
    auto& assetsUndo = *project.assetUndoStack_;
    QVERIFY(DualStageQt::Bind(project, source, game)); QVERIFY(DualStageQt::Edit(project, objectOverride));
    QCOMPARE(terrainUndo.count(), 1); QCOMPARE(assetsUndo.count(), 1);
    terrainUndo.undo();
    // 用户可撤销最后一笔地形，但尚未保存的真实占地图/物编修改仍必须参与G0/G1。
    QVERIFY(!MapData::TerrainSavePlan::HasTerrainChanges(*project.mapEdits_));
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value,
        std::optional(DualStageQt::Member(members, "war3map.w3e")));
    QVERIFY(project.HasUnsavedMapChanges() && project.HasPendingAssetEdits());
    QCOMPARE(project.objectDirty_[DualStageQt::kDoodadIndex], objectOverride);
    const auto fingerprint = project.ProjectFingerprint(); QVERIFY(!fingerprint.empty());
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto sourceRevision = project.mapEdits_->TerrainSourceRevision();
    const auto* archiveBefore = project.archiveSession_.get();
    const auto target = std::filesystem::path(root.filePath("saved-without-terrain-net.w3x").toStdWString());
    auto job = DualStageQt::Freeze(project, target); QVERIFY(job);
    job->minimapChoice = minimapPolicy == 2 ? Services::TerrainMinimapSaveChoice::Preserve
        : Services::TerrainMinimapSaveChoice::Automatic;
    if (mode == "publish-failure") job->beforePublish = [] {
        return Status::Failure("Test", "DUAL_STAGE_PUBLISH_REFUSED", "原创发布失败夹具");
    };
    EditorProject::RunMapSave(*job);
    if (mode == "publish-failure") {
        QVERIFY(!job->succeeded && !project.CommitMapSave(*job));
        QVERIFY2(HasReadDiagnostic(*job, "DUAL_STAGE_PUBLISH_REFUSED"), ReadDiagnosticText(*job).c_str());
        QVERIFY(!std::filesystem::exists(target));
        QCOMPARE(project.ProjectFingerprint(), fingerprint);
        QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision);
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        QCOMPARE(project.archiveSession_.get(), archiveBefore);
        QVERIFY(project.HasUnsavedMapChanges() && project.HasPendingAssetEdits());
        QCOMPARE(assetsUndo.count(), 1); QCOMPARE(terrainUndo.index(), 0);
        QCOMPARE(Sha256File(source).value, sourceSha.value); terrainUndo.clear(); return;
    }
    QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
    // 作者TGA须保留，不能让“不重建小地图”同时跳过静态通行派生。
    const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded() && actual.value);
    QCOMPARE(actual.value->ReadMember(minimap).value, std::optional(DualStageQt::Member(members, minimap)));
    QCOMPARE(actual.value->ReadMember("war3map.w3e").value,
        std::optional(DualStageQt::Member(members, "war3map.w3e")));
    QCOMPARE(actual.value->ReadMember("war3map.wpm").value,
        std::optional(DualStageQt::ExpectedPathing(members, false)));
    DualStageQt::VerifyOutput(*actual.value, members, objectOverride, false);
    QVERIFY(project.HasUnsavedMapChanges() && project.HasPendingAssetEdits());
    if (mode == "late-import") {
        std::vector<EditorProject::ImportPayload> late{{QStringLiteral("Docs/late-import.txt"),
            std::make_shared<const DualStageQt::Bytes>(Tests::WaterText("late owned import"))}};
        QVERIFY(project.AddImportPayloads(late, false, "保存后晚到的资源修改").isEmpty());
    } else if (mode == "late-object") {
        const auto* object = project.objectDocuments_[DualStageQt::kDoodadIndex]->FindObject({DualStageQt::kDoodad, "Doo0"});
        QVERIFY(object);
        const auto field = std::find_if(object->modifications.begin(), object->modifications.end(),
            [](const auto& modification) { return modification.key.fieldId == "dptx"; });
        QVERIFY(field != object->modifications.end());
        QVERIFY(project.objectDocuments_[DualStageQt::kDoodadIndex]->SetModification({DualStageQt::kDoodad, "Doo0"},
            field->key, std::string(DualStageQt::kOldTexture)).Succeeded());
        project.ObserveObjectEdit(int(DualStageQt::kDoodadIndex));
    }
    if (mode == "late-import" || mode == "late-object") {
        const auto lateFingerprint = project.ProjectFingerprint(); QVERIFY(!lateFingerprint.empty() && lateFingerprint != fingerprint);
        QVERIFY(!project.CommitMapSave(*job));
        QVERIFY(ModelHasCode(*project.diagnosticsModel_, QStringLiteral("SAVE_REVISION_CHANGED")));
        QCOMPARE(project.ProjectFingerprint(), lateFingerprint);
        QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision);
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        QCOMPARE(project.archiveSession_.get(), archiveBefore);
        QVERIFY(project.HasUnsavedMapChanges() && project.HasPendingAssetEdits());
        QCOMPARE(Sha256File(source).value, sourceSha.value); terrainUndo.clear(); return;
    }
    QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
    QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    QVERIFY(project.CommitMapSave(*job)); QVERIFY(!project.HasUnsavedMapChanges());
    QVERIFY(project.pendingImports_.empty()); QCOMPARE(assetsUndo.count(), 0);
    QCOMPARE(terrainUndo.count(), 1); QCOMPARE(terrainUndo.index(), 0);
    QCOMPARE(Sha256File(source).value, sourceSha.value); terrainUndo.clear();
}

void EditorProjectTests::DualStageStaticPathingProjectColdRestore() {
    QFETCH(bool, objectOverride);
    QTemporaryDir root, projectOutput; QVERIFY(root.isValid() && projectOutput.isValid());
    QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto members = DualStageQt::SourceMembers(); QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto forge = std::filesystem::path(projectOutput.filePath("dual-stage.forge").toStdWString());
    {
        EditorProject hot;
        auto& terrainUndo = *hot.mapDataUndoStack_;
        auto& assetsUndo = *hot.assetUndoStack_;
        QVERIFY(DualStageQt::Bind(hot, source, game)); QVERIFY(DualStageQt::Edit(hot, objectOverride));
        const auto saved = hot.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
        const auto packed = MapArchiveSession::Open(forge); QVERIFY(packed.Succeeded());
        QCOMPARE(packed.value->ReadMember("imports/0").value, std::optional(DualStageQt::Texture(true)));
        QVERIFY(packed.value->HasMember("overlay/table/" + std::string(ObjectData::LniFileName(DualStageQt::kDoodad))));
        QVERIFY(packed.value->HasMember("overlay/editor/terrain-strokes.bin")); terrainUndo.clear(); assetsUndo.clear();
    }
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{1});
    QCOMPARE((*opened.value)->imports.size(), std::size_t{1});
    QVERIFY((*opened.value)->objects[DualStageQt::kDoodadIndex]);
    const auto again = std::filesystem::path(projectOutput.filePath("dual-stage-again.forge").toStdWString());
    {
        EditorProject cold;
        auto& terrainUndo = *cold.mapDataUndoStack_;
        QVERIFY(DualStageQt::Restore(cold, **opened.value, game));
        QCOMPARE(terrainUndo.count(), 1); QVERIFY(cold.terrainJournalWarning_.empty());
        QCOMPARE(cold.pendingImports_.size(), std::size_t{1});
        QVERIFY(cold.pendingImports_.front().ownedPayload);
        QCOMPARE(*cold.pendingImports_.front().ownedPayload, DualStageQt::Texture(true));
        terrainUndo.undo(); QCOMPARE(MapData::SerializeMapTerrain(*cold.mapEdits_->Terrain()).value,
            std::optional(DualStageQt::ExpectedTerrain(members, false)));
        QCOMPARE(cold.pendingImports_.size(), std::size_t{1}); terrainUndo.redo();
        QCOMPARE(MapData::SerializeMapTerrain(*cold.mapEdits_->Terrain()).value,
            std::optional(DualStageQt::ExpectedTerrain(members, true)));
        const auto saved = cold.SaveProjectToPath(again); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
        QVERIFY(!HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY")); terrainUndo.clear();
    }
    auto reopened = PrepareProjectOpen(again, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QVERIFY(HasReadDiagnostic(reopened, "TERRAIN_JOURNAL_RESTORED"));
    EditorProject restored;
    auto& terrainUndo = *restored.mapDataUndoStack_;
    QVERIFY(DualStageQt::Restore(restored, **reopened.value, game));
    const auto target = std::filesystem::path(root.filePath("cold-saved.w3x").toStdWString());
    auto job = DualStageQt::Freeze(restored, target); QVERIFY(job && job->terrainDerivationRequired);
    EditorProject::RunMapSave(*job); QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
    QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded() && actual.value);
    DualStageQt::VerifyOutput(*actual.value, members, objectOverride, true);
    QVERIFY(restored.CommitMapSave(*job)); QVERIFY(!restored.HasUnsavedMapChanges());
    QCOMPARE(terrainUndo.count(), 1); QCOMPARE(terrainUndo.index(), 1);
    QCOMPARE(Sha256File(source).value, sourceSha.value); terrainUndo.clear();
}

void EditorProjectTests::DualStageUnrelatedEditsPreserveUnresolvedPathing_data() {
    QTest::addColumn<bool>("unitHealth");
    QTest::addColumn<bool>("lateStaticInput");
    QTest::newRow("unrelated-icon-keeps-unresolved-source-wpm") << false << false;
    QTest::newRow("unit-health-keeps-unresolved-source-wpm") << true << false;
    QTest::newRow("unchanged-proof-rechecks-late-static-input") << false << true;
}

void EditorProjectTests::DualStageUnrelatedEditsPreserveUnresolvedPathing() {
    QFETCH(bool, unitHealth); QFETCH(bool, lateStaticInput);
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    auto members = DualStageQt::SourceMembers();
    for (auto& member : members) if (member.name == DualStageQt::kOldTexture)
        member.data = Tests::WaterText("legacy footprint outside current decoder support");
    constexpr auto unitType = ObjectData::ObjectType::Unit;
    constexpr auto unitIndex = static_cast<std::size_t>(unitType);
    if (unitHealth) {
        auto units = ObjectData::NativeObjectDocument::CreateEmpty(unitType); QVERIFY(units.Succeeded() && units.value);
        QVERIFY(units.value->AddOriginalObject("hfoo").Succeeded());
        QVERIFY(units.value->AddModification({unitType, "hfoo"}, {"uhpm"},
            ObjectData::NativeValueType::Int32, std::int32_t{0}).Succeeded());
        auto encoded = ObjectData::NativeObjectCodec(unitType).Serialize(*units.value); QVERIFY(encoded.Succeeded() && encoded.value);
        members.push_back({"war3map.w3u", std::move(*encoded.value)});
    }
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project;
    auto& assetsUndo = *project.assetUndoStack_;
    QVERIFY(DualStageQt::Bind(project, source, game));
    const std::string iconName = "Textures/unrelated-icon.blp";
    const auto icon = Tests::WaterText("unrelated owned icon bytes");
    if (unitHealth) {
        const auto nativeBytes = project.archiveSession_->ReadMember("war3map.w3u"); QVERIFY(nativeBytes.Succeeded() && nativeBytes.value);
        auto parsed = ObjectData::NativeObjectCodec(unitType).Parse(*nativeBytes.value); QVERIFY(parsed.Succeeded());
        const auto* native = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
        auto text = ObjectData::ConvertNativeObjectDataToLni(unitType, *nativeBytes.value); QVERIFY(text.Succeeded() && text.value);
        auto editable = ObjectData::LniObjectCodec(unitType).Parse(*text.value); QVERIFY(editable.Succeeded());
        auto* lni = dynamic_cast<ObjectData::LniObjectDocument*>(editable.document.get()); QVERIFY(lni);
        project.nativeObjectDocuments_[unitIndex] = std::make_unique<ObjectData::NativeObjectDocument>(*native);
        project.objectDocuments_[unitIndex].reset(static_cast<ObjectData::LniObjectDocument*>(editable.document.release()));
        project.ResetEditTracking(); QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
        const auto* object = project.objectDocuments_[unitIndex]->FindObject({unitType, "hfoo"}); QVERIFY(object);
        const auto field = std::find_if(object->modifications.begin(), object->modifications.end(),
            [](const auto& modification) { return modification.key.fieldId == "uhpm"; });
        QVERIFY(field != object->modifications.end());
        QVERIFY(project.objectDocuments_[unitIndex]->SetModification({unitType, "hfoo"}, field->key, std::int32_t{1}).Succeeded());
        project.ObserveObjectEdit(int(unitIndex));
    } else {
        std::vector<EditorProject::ImportPayload> payloads{{QString::fromStdString(iconName),
            std::make_shared<const DualStageQt::Bytes>(icon)}};
        QVERIFY(project.AddImportPayloads(payloads, false, "导入无关图标").isEmpty());
    }
    QVERIFY(!MapData::TerrainSavePlan::HasTerrainChanges(*project.mapEdits_));
    QVERIFY(project.HasUnsavedMapChanges());
    const auto fingerprint = project.ProjectFingerprint(); QVERIFY(!fingerprint.empty());
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto sourceRevision = project.mapEdits_->TerrainSourceRevision();
    const auto* archiveBefore = project.archiveSession_.get();
    const auto target = std::filesystem::path(root.filePath("unrelated-saved.w3x").toStdWString());
    auto job = DualStageQt::Freeze(project, target); QVERIFY(job);
    job->minimapChoice = Services::TerrainMinimapSaveChoice::Automatic;
    if (lateStaticInput) {
        auto* task = job.get();
        job->beforePublish = [task] {
            // 工厂先见到真正无关的图标；发布回调才注入静态变化，不能借nullopt免核。
            task->replacements.push_back({std::string(DualStageQt::kOldTexture), DualStageQt::Texture(true)});
            return Status::Ok();
        };
    }
    EditorProject::RunMapSave(*job);
    if (lateStaticInput) {
        QVERIFY(!job->succeeded && !project.CommitMapSave(*job));
        QVERIFY2(HasReadDiagnostic(*job, "TERRAIN_FINAL_INPUTS_CHANGED"), ReadDiagnosticText(*job).c_str());
        QVERIFY(!std::filesystem::exists(target));
        QCOMPARE(project.ProjectFingerprint(), fingerprint); QVERIFY(project.HasUnsavedMapChanges());
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision);
        QCOMPARE(project.archiveSession_.get(), archiveBefore);
        QCOMPARE(project.pendingImports_.size(), std::size_t{1}); QCOMPARE(assetsUndo.count(), 1);
        QCOMPARE(Sha256File(source).value, sourceSha.value); return;
    }
    QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
    QVERIFY(!job->terrainSavePlan || !job->terrainSavePlan->RebuiltPathing());
    const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded() && actual.value);
    for (const auto name : {"war3map.wpm", "war3map.w3e", "war3mapMap.tga"})
        QCOMPARE(actual.value->ReadMember(name).value, std::optional(DualStageQt::Member(members, name)));
    if (unitHealth) {
        const auto changed = actual.value->ReadMember("war3map.w3u"); QVERIFY(changed.Succeeded() && changed.value);
        auto parsed = ObjectData::NativeObjectCodec(unitType).Parse(*changed.value); QVERIFY(parsed.Succeeded());
        const auto* native = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
        const auto* object = native->FindObject({unitType, "hfoo"}); QVERIFY(object);
        const auto field = std::find_if(object->modifications.begin(), object->modifications.end(),
            [](const auto& modification) { return modification.key.fieldId == "uhpm"; });
        QVERIFY(field != object->modifications.end()); QCOMPARE(std::get<std::int32_t>(field->value), std::int32_t{1});
    } else QCOMPARE(actual.value->ReadMember(iconName).value, std::optional(icon));
    QVERIFY(project.CommitMapSave(*job)); QVERIFY(!project.HasUnsavedMapChanges());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}
