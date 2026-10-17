#include "WorldTerrainAssets.h"
#include "WorldPathingHelperPresenter.h"
#include "WorldThumbnailCache.h"
#include "EditorProject.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUndoStack>
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Modules;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::shared_ptr<const std::vector<std::byte>> Payload(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char value : text) bytes.push_back(static_cast<std::byte>(value));
    return std::make_shared<const std::vector<std::byte>>(std::move(bytes));
}
void BindProject(QtProject::EditorProject& project, const WorldTerrainSources& sources) {
    auto opened = MapArchiveSession::Open(sources.archivePath); Require(opened.Succeeded(), "open project archive fixture");
    project.sourceArchivePath_ = sources.archivePath;
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
}
WorldTerrainSources Capture(const QtProject::EditorProject& project) {
    auto captured = CaptureWorldTerrainSources(project); Require(captured.Succeeded(), "capture complete world sources");
    return std::move(*captured.value);
}
template<class Sources>
auto& Entry(Sources& sources, std::string_view path) {
    const auto found = std::find_if(sources.entries.begin(), sources.entries.end(),
        [&](const auto& entry) { return entry.path == path; });
    Require(found != sources.entries.end(), "captured resource entry exists"); return *found;
}
void Import(QtProject::EditorProject& project, QString path, std::shared_ptr<const std::vector<std::byte>> bytes) {
    std::vector<QtProject::EditorProject::ImportPayload> payloads{{std::move(path), std::move(bytes)}};
    Require(project.AddImportPayloads(payloads, true, QStringLiteral("owned fixture import")).isEmpty(), "publish actual import transaction");
}
std::string HelperIdentity(const WorldTerrainSources& sources) {
    auto identity = WorldPathingHelperResourceIdentity(sources, 7); Require(identity.Succeeded(), "helper source identity");
    return std::move(*identity.value);
}
QString ThumbnailIdentity(WorldThumbnailCache& cache, const WorldTerrainSources& sources) {
    cache.SetContext({QStringLiteral("fixture"), 1, 0}, Result<WorldTerrainSources>::Success(sources), [] { return true; });
    Render::ModelInstanceRequest request; request.modelPath = "Fixture.bin";
    return cache.Request(std::move(request)); // 空backend只比较生产请求键，不生成图像或请求GPU。
}
WorldTerrainSources Fixture(const std::filesystem::path& path) {
    const std::vector<Mpq::MpqBuildFile> files{{"Fixture.bin", {std::byte{1}, std::byte{2}, std::byte{3}}}};
    Require(Mpq::WriteMpqArchive(path, files).Succeeded(), "write original fixture archive");
    const auto digest = Sha256File(path); Require(digest.Succeeded(), "hash fixture");
    WorldTerrainSources sources; sources.archivePath = path; sources.archiveSha = *digest.value;
    sources.entries.push_back({"Fixture.bin", "Fixture.bin", {}});
    return sources;
}
void VerifiedLeaseSkipsHashWithoutSharingTheReader() {
    QTemporaryDir directory; Require(directory.isValid(), "temporary directory");
    const auto sources = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    WorldResourceReader::OpenTimings cold, warm;
    auto first = WorldResourceReader::Open(sources, nullptr, &cold);
    Require(first.Succeeded() && !cold.verifiedArchiveReused && cold.hashedBytes == std::filesystem::file_size(sources.archivePath),
        "first lease performs complete SHA");
    auto second = WorldResourceReader::Open(sources, first.value->get(), &warm);
    Require(second.Succeeded() && warm.verifiedArchiveReused && warm.hashedBytes == 0 && warm.hashNs == 0,
        "same locked file reuses only verified SHA capability");
    const auto writer = CreateFileW(sources.archivePath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto error = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Require(writer == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION, "live source lease denies in-place writes");
    first.value->reset();
    const auto read = (*second.value)->ReadAnimationResource("Fixture.bin", 1024);
    Require(read.Succeeded() && read.value->bytes == std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}}
        && (*second.value)->VerifyCurrentSource().Succeeded(), "new reader owns independent lease and MPQ");
    auto wrongHash = sources; wrongHash.archiveSha.assign(64, '0');
    const auto refused = WorldResourceReader::Open(wrongHash, second.value->get());
    Require(!refused.Succeeded() && refused.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "caller supplied expected SHA remains mandatory");
}
void ReplacementWithIdenticalBytesDoesNotReuseOldIdentity() {
    QTemporaryDir directory; Require(directory.isValid(), "temporary directory");
    const auto sources = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    auto original = WorldResourceReader::Open(sources); Require(original.Succeeded(), "open first source");
    auto replacement = sources; replacement.archivePath = sources.archivePath.parent_path() / L"replacement.w3x";
    std::filesystem::copy_file(sources.archivePath, replacement.archivePath);
    const auto identicalHash = Sha256File(replacement.archivePath);
    Require(identicalHash.Succeeded() && *identicalHash.value == sources.archiveSha, "replacement is byte identical");
    const auto foreign = WorldResourceReader::Open(replacement, original.value->get());
    Require(!foreign.Succeeded() && foreign.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "same digest and bytes on another file are not the held lease");
    if (MoveFileExW(replacement.archivePath.c_str(), sources.archivePath.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
        const auto replaceError = GetLastError();
        std::cerr << "MoveFileEx(REPLACE_EXISTING) under deny-write lease failed: " << replaceError
            << "; retaining original in a sibling test path before replacing source path\n";
        // 某些Windows文件系统不允许覆盖一个仍被拒写租约占用的目标，但允许共享DELETE
        // 的原文件改名。保留原文件与其句柄，再安装相同字节的新文件，仍必须证明确实拒旧身份。
        const auto retained = sources.archivePath.parent_path() / L"retained-original.w3x";
        const auto movedOriginal = MoveFileExW(sources.archivePath.c_str(), retained.c_str(), 0);
        if (!movedOriginal) std::cerr << "MoveFileEx(retain original) failed: " << GetLastError() << '\n';
        Require(movedOriginal != FALSE, "test-only original rename under delete-sharing lease");
        const auto movedReplacement = MoveFileExW(replacement.archivePath.c_str(), sources.archivePath.c_str(), 0);
        if (!movedReplacement) std::cerr << "MoveFileEx(install replacement) failed: " << GetLastError() << '\n';
        Require(movedReplacement != FALSE, "test-only new source installation after retaining original");
    }
    const auto current = (*original.value)->VerifyCurrentSource();
    Require(!current.Succeeded() && current.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "current path replacement is detected");
    const auto stale = WorldResourceReader::Open(sources, original.value->get());
    Require(!stale.Succeeded() && stale.diagnostics.front().code == "WORLD_ASSET_SOURCE_CHANGED",
        "replaced path cannot inherit old verification");
    WorldResourceReader::OpenTimings freshTiming;
    const auto fresh = WorldResourceReader::Open(sources, nullptr, &freshTiming);
    Require(fresh.Succeeded() && freshTiming.hashedBytes != 0 && !freshTiming.verifiedArchiveReused,
        "explicit fresh open validates replacement independently");
}

void OwnedImportsWinAndKeepAnimationProvenance() {
    QTemporaryDir directory; Require(directory.isValid(), "owned import temporary directory");
    const auto fixture = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    QtProject::EditorProject project; BindProject(project, fixture);
    project.war3GameRoot_ = fixture.archivePath.parent_path() / L"game";
    std::filesystem::create_directory(project.war3GameRoot_);
    const std::vector<Mpq::MpqBuildFile> gameFiles{{"Fixture.bin", {std::byte{9}}},
        {"New.bin", {std::byte{8}}}, {"GameOnly.bin", {std::byte{7}}}};
    Require(Mpq::WriteMpqArchive(project.war3GameRoot_ / L"War3.mpq", gameFiles).Succeeded(), "write competing game fixture");
    const auto replacement = Payload("owned map replacement"), added = Payload("owned new import");
    Import(project, QStringLiteral("Fixture.bin"), replacement);
    Import(project, QStringLiteral("New.bin"), added);
    const auto sources = Capture(project), repeated = Capture(project);
    const auto& replaced = Entry(sources, "Fixture.bin"); const auto& created = Entry(sources, "New.bin");
    Require(replaced.ownedPayload == replacement && replaced.archivePath == "Fixture.bin" && replaced.diskPath.empty()
        && replaced.size == static_cast<std::int64_t>(replacement->size()) && !replaced.ambiguous && replaced.ownedPayloadVersion,
        "capture preserves owned replacement, size, ambiguity and controlled version");
    Require(created.ownedPayload == added && created.archivePath.empty() && created.diskPath.empty(), "new import has no old archive or disk path");
    Require(SameWorldTerrainSources(sources, repeated), "same live immutable owners reuse captured versions");
    auto opened = WorldResourceReader::Open(sources); Require(opened.Succeeded(), "open owned resources");
    for (const auto& [path, expected] : std::vector<std::pair<std::string, std::shared_ptr<const std::vector<std::byte>>>>{
        {"Fixture.bin", replacement}, {"New.bin", added}}) {
        const auto read = (*opened.value)->Resolver().Read(path);
        Require(read.Succeeded() && read.value->bytes == *expected && read.value->location.layer == Resources::ResourceLayer::MapImport,
            "owned import wins over old map and same-name game resource");
    }
    const auto fallback = (*opened.value)->Resolver().Read("GameOnly.bin");
    Require(fallback.Succeeded() && fallback.value->bytes == gameFiles[2].data, "uncovered game resource remains available");
    const auto animation = (*opened.value)->ReadAnimationResource("New.bin", added->size());
    const auto digest = Sha256(*added); Require(digest.Succeeded(), "owned animation expected digest");
    Require(animation.Succeeded() && animation.value->bytes == *added && animation.value->digest == *digest.value
        && animation.value->sourceLayer.ends_with("|session:owned"), "animation records actual bytes SHA and session-owned layer");
    const auto tooSmall = (*opened.value)->ReadAnimationResource("New.bin", added->size() - 1);
    Require(!tooSmall.Succeeded() && tooSmall.diagnostics.front().code == "WORLD_ANIMATION_RESOURCE_BUDGET",
        "owned animation obeys caller byte limit before copying");
    const auto metadataLimit = CaptureWorldTerrainSources(project, 1);
    Require(!metadataLimit.Succeeded() && metadataLimit.diagnostics.front().code == "WORLD_ASSET_METADATA_BUDGET", "metadata capture remains fail-closed");
    // 同名归档+会话覆盖的顺序不影响结果；两份会话正文仍属同层冲突。
    auto layered = sources; layered.entries.push_back({"FIXTURE.BIN", "Fixture.bin", {}, 3});
    auto layerReader = WorldResourceReader::Open(layered); Require(layerReader.Succeeded(), "owned import wins when archive entry follows");
    std::reverse(layered.entries.begin(), layered.entries.end());
    layerReader = WorldResourceReader::Open(layered); Require(layerReader.Succeeded(), "owned import wins when archive entry precedes");
    const auto layerRead = (*layerReader.value)->Resolver().Read("Fixture.bin");
    Require(layerRead.Succeeded() && layerRead.value->bytes == *replacement, "duplicate lower layer cannot replace owned bytes");
    layered.entries.push_back(replaced);
    const auto conflict = WorldResourceReader::Open(layered);
    Require(!conflict.Succeeded() && conflict.diagnostics.front().code == "WORLD_ASSET_CONFLICT", "same-layer owned duplicates are rejected");
}

void EmptyOwnedOverridesAndPerResourceBudgetsRemainLocal() {
    QTemporaryDir directory; Require(directory.isValid(), "resource budget temporary directory");
    const auto fixture = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    QtProject::EditorProject project; BindProject(project, fixture);
    Import(project, QStringLiteral("Fixture.bin"), Payload(""));
    auto empty = Capture(project);
    // 若错误地优先磁盘或给owned正文装disk guard，此不存在路径会使实际读取失败。
    Entry(empty, "Fixture.bin").diskPath = fixture.archivePath.parent_path() / L"missing-import.bin";
    auto opened = WorldResourceReader::Open(empty); Require(opened.Succeeded(), "open explicit empty owned override");
    const auto read = (*opened.value)->Resolver().Read("Fixture.bin");
    const auto animation = (*opened.value)->ReadAnimationResource("Fixture.bin", 0);
    const auto emptyDigest = Sha256({}); Require(emptyDigest.Succeeded(), "hash empty fixture");
    Require(read.Succeeded() && read.value->bytes.empty() && animation.Succeeded() && animation.value->bytes.empty()
        && animation.value->digest == *emptyDigest.value && animation.value->sourceLayer.ends_with("|session:owned"),
        "empty owned bytes override archive and invalid disk without fallback");
    project.pendingImports_.clear(); project.AssetUndoStack()->clear();
    const auto large = std::make_shared<const std::vector<std::byte>>(50 * 1024 * 1024, std::byte{4});
    project.pendingImports_.push_back({QStringLiteral("UnusedSound.wav"), {}, static_cast<qint64>(large->size()), large});
    auto sources = Capture(project);
    opened = WorldResourceReader::Open(sources); Require(opened.Succeeded(), "unused large owned sound does not block world reader");
    const auto ground = (*opened.value)->Resolver().Read("Fixture.bin");
    Require(ground.Succeeded() && ground.value->bytes.size() == 3, "unrelated original world member remains readable");
    const auto oversized = (*opened.value)->Resolver().Read("UnusedSound.wav");
    Require(!oversized.Succeeded() && oversized.diagnostics.front().code == "RESOURCE_READ_FAILED"
        && std::any_of(oversized.diagnostics.begin(), oversized.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "WORLD_ASSET_READ"; }), "actual owned read retains existing 16 MiB cap and resolver diagnostic wrapper");
    const auto animationOversized = (*opened.value)->ReadAnimationResource("UnusedSound.wav", large->size());
    Require(!animationOversized.Succeeded() && animationOversized.diagnostics.front().code == "WORLD_ANIMATION_RESOURCE_BUDGET", "animation cannot expand existing member cap");
    const auto boundary = std::make_shared<const std::vector<std::byte>>(16 * 1024 * 1024, std::byte{5});
    project.pendingImports_ = {{QStringLiteral("Boundary.bin"), {}, static_cast<qint64>(boundary->size()), boundary}};
    sources = Capture(project); opened = WorldResourceReader::Open(sources); Require(opened.Succeeded(), "open exact limit owned member");
    const auto boundaryRead = (*opened.value)->Resolver().Read("Boundary.bin");
    Require(boundaryRead.Succeeded() && boundaryRead.value->bytes == *boundary, "existing exact 16 MiB ability is preserved");
    auto ambiguous = sources; ambiguous.entries.front().ambiguous = true;
    Require(!SameWorldTerrainSources(sources, ambiguous), "ambiguity affects live source identity");
    const auto refused = WorldResourceReader::Open(ambiguous);
    Require(!refused.Succeeded() && refused.diagnostics.front().code == "WORLD_ASSET_CONFLICT", "explicit ambiguous source fails before fallback");
    auto wrongSize = sources; ++Entry(wrongSize, "Boundary.bin").size;
    const auto sizeRefused = WorldResourceReader::Open(wrongSize);
    Require(!sizeRefused.Succeeded() && sizeRefused.diagnostics.front().code == "WORLD_ASSET_SIZE", "owned metadata must match actual bytes");
    project.pendingImports_.push_back(project.pendingImports_.front());
    const auto actualAmbiguous = Capture(project);
    Require(Entry(actualAmbiguous, "Boundary.bin").ambiguous, "capture preserves ambiguity from real AssetEntries");
    const auto ambiguityRefused = WorldResourceReader::Open(actualAmbiguous);
    Require(!ambiguityRefused.Succeeded() && ambiguityRefused.diagnostics.front().code == "WORLD_ASSET_CONFLICT",
        "actual duplicate imports cannot silently pick one payload");
}

void ReplacementUndoRedoAndBranchesInvalidateLongLivedKeys() {
    QTemporaryDir directory; Require(directory.isValid(), "cache identity temporary directory");
    const auto fixture = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    QtProject::EditorProject project; BindProject(project, fixture);
    WorldThumbnailCache thumbnails(nullptr);
    Import(project, QStringLiteral("Fixture.bin"), Payload("AAA"));
    const auto first = Capture(project); auto oldReader = WorldResourceReader::Open(first);
    Require(oldReader.Succeeded(), "open first owned snapshot");
    const auto helperFirst = HelperIdentity(first); const auto thumbFirst = ThumbnailIdentity(thumbnails, first);
    Import(project, QStringLiteral("Fixture.bin"), Payload("BBB"));
    const auto replacement = Capture(project);
    Require(!project.editTrackingReady_,
        "fixture deliberately covers imports before edit tracking is ready");
    Require(!SameWorldTerrainSources(first, replacement) && !(*oldReader.value)->MatchesSources(replacement), "equal-length owned replacement changes complete source identity");
    Require(HelperIdentity(replacement) != helperFirst && ThumbnailIdentity(thumbnails, replacement) != thumbFirst, "both long-lived keys observe owned replacement");
    const auto oldRead = (*oldReader.value)->Resolver().Read("Fixture.bin");
    auto newReader = WorldResourceReader::Open(replacement); Require(newReader.Succeeded(), "open replacement owned snapshot");
    const auto newRead = (*newReader.value)->Resolver().Read("Fixture.bin");
    Require(oldRead.Succeeded() && oldRead.value->bytes == *Payload("AAA") && newRead.Succeeded() && newRead.value->bytes == *Payload("BBB"), "old snapshot keeps original immutable bytes alive while new reader sees replacement");
    project.AssetUndoStack()->undo(); const auto undone = Capture(project);
    Require(SameWorldTerrainSources(first, undone) && !SameWorldTerrainSources(replacement, undone), "undo restores exact previous live owner");
    Require(HelperIdentity(undone) == helperFirst && ThumbnailIdentity(thumbnails, undone) == thumbFirst, "undo may safely reuse the previous content owner key");
    project.AssetUndoStack()->redo(); const auto redone = Capture(project);
    Require(SameWorldTerrainSources(replacement, redone), "redo restores replacement identity");
    project.AssetUndoStack()->undo(); Import(project, QStringLiteral("Fixture.bin"), Payload("CCC"));
    const auto branch = Capture(project);
    Require(!project.AssetUndoStack()->canRedo() && !SameWorldTerrainSources(branch, replacement)
        && HelperIdentity(branch) != HelperIdentity(replacement) && ThumbnailIdentity(thumbnails, branch) != ThumbnailIdentity(thumbnails, replacement), "new undo branch with same path and size cannot inherit old keys");
    const auto beforeRevision = project.editRevision_.Revision();
    Require(project.editRevision_.Observe({{"fixture", "changed"}}, "fixture edit revision").Succeeded(), "advance real edit revision");
    const auto revised = Capture(project);
    Require(project.editRevision_.Revision() != beforeRevision && SameWorldTerrainSources(branch, revised)
        && HelperIdentity(revised) == HelperIdentity(branch) && ThumbnailIdentity(thumbnails, revised) == ThumbnailIdentity(thumbnails, branch), "unrelated map revision cannot invalidate immutable resources");
    const auto beforeGeneration = project.editRevision_.Generation();
    Require(project.editRevision_.Reset().Succeeded(), "reset real edit generation");
    const auto regenerated = Capture(project);
    Require(project.editRevision_.Generation() != beforeGeneration && SameWorldTerrainSources(revised, regenerated)
        && HelperIdentity(regenerated) == HelperIdentity(revised) && ThumbnailIdentity(thumbnails, regenerated) == ThumbnailIdentity(thumbnails, revised), "task generation is independent of immutable resource keys");
    auto ambiguous = regenerated; Entry(ambiguous, "Fixture.bin").ambiguous = true;
    Require(HelperIdentity(ambiguous) != HelperIdentity(regenerated)
        && ThumbnailIdentity(thumbnails, ambiguous) != ThumbnailIdentity(thumbnails, regenerated), "ambiguity participates in both long-lived keys");
    auto invalid = regenerated; Entry(invalid, "Fixture.bin").ownedPayloadVersion = 0;
    Require(!WorldPathingHelperResourceIdentity(invalid, 7).Succeeded() && !WorldResourceReader::Open(invalid).Succeeded(),
        "hand-built owned snapshot without controlled version is explicitly unavailable");
    const auto invalidKey = ThumbnailIdentity(thumbnails, invalid);
    const auto invalidResult = thumbnails.Lookup(invalidKey);
    Require(invalidResult && !invalidResult->issue.isEmpty() && invalidResult->image.isNull(), "thumbnail reports missing owned version instead of reusing a successful image");
}

void WeakVersionsRejectAliasesAndKeepCompleteBatchesStable() {
    QTemporaryDir directory; Require(directory.isValid(), "weak version temporary directory");
    const auto fixture = Fixture(std::filesystem::path(directory.path().toStdWString()) / L"source.w3x");
    std::weak_ptr<const std::vector<std::byte>> released;
    {
        QtProject::EditorProject project; BindProject(project, fixture);
        auto payload = Payload("memo is weak"); released = payload;
        project.pendingImports_.push_back({QStringLiteral("Owned.bin"), {}, static_cast<qint64>(payload->size()), payload});
        const auto snapshot = Capture(project); payload.reset();
        project.pendingImports_.clear();
        Require(!released.expired() && Entry(snapshot, "Owned.bin").ownedPayload, "snapshot strongly owns payload after project removes it");
    }
    Require(released.expired(), "version memo cannot retain bytes after project and snapshot release them");
    QtProject::EditorProject project; BindProject(project, fixture);
    const std::vector<std::byte> backing{std::byte{6}};
    const auto alias = [&] { return std::shared_ptr<const std::vector<std::byte>>(&backing, [](const auto*) {}); };
    auto ownerOne = alias();
    project.pendingImports_ = {{QStringLiteral("Alias.bin"), {}, 1, ownerOne}};
    auto first = Capture(project); const auto firstVersion = Entry(first, "Alias.bin").ownedPayloadVersion;
    auto ownerTwo = alias();
    project.pendingImports_ = {{QStringLiteral("Alias.bin"), {}, 1, ownerTwo}};
    const auto second = Capture(project);
    Require(ownerOne.get() == ownerTwo.get() && Entry(second, "Alias.bin").ownedPayloadVersion != firstVersion,
        "same raw object address with distinct live ownership never shares version");
    ownerOne.reset(); first = {}; // 使旧control block过期；同地址新owner仍须取得新版本。
    project.pendingImports_ = {{QStringLiteral("Alias.bin"), {}, 1, alias()}};
    const auto afterExpiry = Capture(project);
    Require(Entry(afterExpiry, "Alias.bin").ownedPayloadVersion != firstVersion
        && Entry(afterExpiry, "Alias.bin").ownedPayloadVersion != Entry(second, "Alias.bin").ownedPayloadVersion,
        "expired ownership cannot create address-ABA cache reuse");
    project.pendingImports_.clear();
    std::vector<QtProject::EditorProject::ImportPayload> batch;
    batch.reserve(1025);
    for (std::size_t index = 0; index < 1025; ++index) {
        batch.push_back({QStringLiteral("Batch/%1.bin").arg(static_cast<qulonglong>(index)), Payload("x")});
    }
    Require(project.AddImportPayloads(batch, true, QStringLiteral("complete owned batch")).isEmpty(), "publish one real 1025-owner transaction");
    const auto complete = Capture(project), repeated = Capture(project), third = Capture(project);
    WorldThumbnailCache thumbnails(nullptr);
    Require(SameWorldTerrainSources(complete, repeated) && SameWorldTerrainSources(complete, third)
        && HelperIdentity(complete) == HelperIdentity(third)
        && ThumbnailIdentity(thumbnails, complete) == ThumbnailIdentity(thumbnails, third),
        "1025 live owners retain every identity across complete fixed-order captures");
    Require(complete.ownedIdentityMetadataCapacityBytes > 0
        && complete.ownedIdentityMetadataCapacityBytes <= WorldOwnedIdentityLimits{}.maxMetadataBytes, "actual identity vector capacity is accounted");
    const auto refused = CaptureWorldTerrainSources(project, std::numeric_limits<std::size_t>::max(), {1});
    Require(!refused.Succeeded() && !refused.value && refused.diagnostics.front().code == "WORLD_ASSET_IDENTITY_BUDGET", "tight metadata budget returns no partial source");
    const auto afterBudget = Capture(project);
    Require(SameWorldTerrainSources(complete, afterBudget), "budget refusal does not evict live identities or publish partial versions");
    const auto enlarged = CaptureWorldTerrainSources(project, std::numeric_limits<std::size_t>::max(),
        {WorldOwnedIdentityLimits{}.maxMetadataBytes + 1});
    Require(!enlarged.Succeeded(), "identity metadata hard budget cannot be enlarged");
    const auto batchImports = project.pendingImports_;
    project.pendingImports_.clear();
    const auto idle = Capture(project);
    Require(std::none_of(idle.entries.begin(), idle.entries.end(), [](const auto& entry) { return static_cast<bool>(entry.ownedPayload); })
        && idle.ownedIdentityMetadataCapacityBytes > 0
        && idle.ownedIdentityMetadataCapacityBytes <= WorldOwnedIdentityLimits{}.maxMetadataBytes,
        "zero-owned capture still accounts the resident weak registry capacity");
    const auto idleRefused = CaptureWorldTerrainSources(project, std::numeric_limits<std::size_t>::max(), {1});
    Require(!idleRefused.Succeeded() && !idleRefused.value
        && idleRefused.diagnostics.front().code == "WORLD_ASSET_IDENTITY_BUDGET", "zero-owned capture cannot bypass the resident metadata budget");
    project.pendingImports_ = batchImports;
    Require(SameWorldTerrainSources(complete, Capture(project)), "idle capture and its budget refusal do not evict live identities");

    struct Views { std::vector<std::byte> a{std::byte{1}}, b{std::byte{2}}; };
    auto storage = std::make_shared<Views>();
    const std::shared_ptr<const std::vector<std::byte>> viewA(storage, &storage->a), viewB(storage, &storage->b);
    project.pendingImports_ = {{QStringLiteral("ViewA.bin"), {}, 1, viewA}, {QStringLiteral("ViewB.bin"), {}, 1, viewB}};
    const auto views = Capture(project), viewsAgain = Capture(project);
    Require(Entry(views, "ViewA.bin").ownedPayloadVersion != Entry(views, "ViewB.bin").ownedPayloadVersion
        && SameWorldTerrainSources(views, viewsAgain), "one live owner with different views has distinct stable identities");
    const auto validImports = project.pendingImports_;
    const std::shared_ptr<const std::vector<std::byte>> ownerless(std::shared_ptr<const std::vector<std::byte>>{}, &backing);
    project.pendingImports_ = {{QStringLiteral("Ownerless.bin"), {}, 1, ownerless}};
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto invalid = CaptureWorldTerrainSources(project);
        Require(!invalid.Succeeded() && !invalid.value && invalid.diagnostics.front().code == "WORLD_ASSET_IDENTITY", "ownerless nonempty alias is refused rather than reissued each capture");
    }
    project.pendingImports_ = validImports;
    Require(SameWorldTerrainSources(views, Capture(project)), "invalid ownership does not invalidate accepted owner-view identities");
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        VerifiedLeaseSkipsHashWithoutSharingTheReader();
        ReplacementWithIdenticalBytesDoesNotReuseOldIdentity();
        OwnedImportsWinAndKeepAnimationProvenance();
        EmptyOwnedOverridesAndPerResourceBudgetsRemainLocal();
        ReplacementUndoRedoAndBranchesInvalidateLongLivedKeys();
        WeakVersionsRejectAliasesAndKeepCompleteBatchesStable();
        std::cout << "world-resource-reader: source lease, owned payload and cache identity regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "world-resource-reader: " << error.what() << '\n';
        return 1;
    }
}
