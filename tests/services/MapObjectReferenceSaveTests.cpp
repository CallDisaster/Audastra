#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "../MapDataFixtures.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using Snapshot = Trigger::MapObjectReferenceSnapshot;
using SnapshotResult = Result<std::shared_ptr<const Snapshot>>;

// 只验证真实来源资格和最终成员保存；不以此代替脚本重建、原版或游戏验收。
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
void Explain(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
std::shared_ptr<const Snapshot> Captured(SnapshotResult result) {
    if (!result.Succeeded()) Explain(result.diagnostics);
    Require(result.Succeeded() && result.value && *result.value, "capture original source through the public service");
    return std::move(*result.value);
}
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char character : text) bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(output.good(), "write owned test input");
}
void WriteText(const std::filesystem::path& path, std::string_view text) { Write(path, Bytes(text)); }

class TempDirectory final {
public:
    TempDirectory() : parent_(std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal()) {
        static std::atomic<unsigned> sequence{0};
        path_ = parent_ / ("Forge-MapObjectReferenceSave-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "create exclusive private fixture directory");
    }
    ~TempDirectory() noexcept {
        try {
            if (std::uncaught_exceptions() != 0) {
                std::cerr << "retained-fixture=" << path_.string() << '\n';
                return;
            }
            const auto normalized = std::filesystem::absolute(path_).lexically_normal();
            if (normalized.parent_path() != parent_ || !normalized.filename().string().starts_with("Forge-MapObjectReferenceSave-")) return;
            std::error_code error;
            std::filesystem::remove_all(normalized, error);
        } catch (...) {
            // 清理失败保留本次独占夹具，不让析构异常掩盖原始验证结果。
        }
    }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path parent_, path_;
};

std::vector<std::byte> NativeUnit(std::string_view parent = "P001") {
    MapDataFixtures::Fixture writer;
    writer.Int(2); writer.Int(0); writer.Int(1);
    writer.Raw(parent); writer.Raw("n00Q"); writer.Int(0);
    return std::move(writer.bytes);
}
std::vector<std::byte> EmptyNative() {
    MapDataFixtures::Fixture writer;
    writer.Int(2); writer.Int(0); writer.Int(0);
    return std::move(writer.bytes);
}
std::vector<std::byte> Units(float z = 12.f) {
    MapData::UnitPlacements records;
    for (const auto& [rawcode, creation] : std::array<std::pair<const char*, std::int32_t>, 2>{{{"n00Q", 17}, {"hfoo", 18}}}) {
        MapData::UnitPlacement unit;
        unit.rawcode = rawcode; unit.creationId = creation; unit.flags = 2;
        unit.scale = {1.f, 1.f, 1.f}; unit.position = {4.f, 8.f, z};
        records.units.push_back(std::move(unit));
    }
    auto bytes = records.Serialize();
    Require(bytes.Succeeded(), "serialize original placement fixture");
    return std::move(*bytes.value);
}
std::vector<std::byte> Slk(std::string_view rawcode, std::string_view name = "Synthetic") {
    return Bytes("ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"name\"\nC;X1;Y2;K\""
        + std::string(rawcode) + "\"\nC;X2;K\"" + std::string(name) + "\"\nE\n");
}
void WriteGame(const std::filesystem::path& gameRoot, std::string_view archive = "war3.mpq",
    std::string_view unitName = "Synthetic", bool classificationTables = true) {
    std::filesystem::create_directories(gameRoot);
    std::vector<Mpq::MpqBuildFile> files;
    if (classificationTables) {
        files.push_back({"Units/UnitData.slk", Slk("hfoo", unitName), false});
        files.push_back({"Units/ItemData.slk", Slk("ratf"), false});
    } else files.push_back({"unrelated.dat", Bytes("original unrelated data"), false});
    Require(Mpq::WriteMpqArchive(gameRoot / std::filesystem::path(archive), files).Succeeded(), "write original private game MPQ");
}

struct Fixture {
    TempDirectory directory;
    std::filesystem::path expanded = directory.Path() / "expanded";
    std::filesystem::path source = directory.Path() / "source.w3x";
    std::filesystem::path game = directory.Path() / "game";
    Trigger::UiCatalog catalog;
    Trigger::EncodedTriggerDocument triggers;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> archive;

    explicit Fixture(bool withPlacements = true) {
        const auto ui = directory.Path() / "ui";
        WriteText(ui / "config", "base\n");
        WriteText(ui / "base" / "define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
        auto loaded = Trigger::UiCatalog::Load(ui);
        Require(loaded.Succeeded(), "load original GUI-T catalog");
        catalog = std::move(*loaded.value);
        Trigger::WtgWctCodec codec;
        auto empty = codec.CreateEmpty(catalog);
        Require(empty.Succeeded(), "create a sealed original trigger document");
        empty.value->categories.push_back({1, 1, Trigger::RawText{"Natural T"}, 0, {}});
        Require(Trigger::TriggerDocumentEditor::AddTrigger(*empty.value, 1, "Existing GUI trigger").Succeeded(), "create ordinary GUI T");
        auto encoded = codec.Serialize(*empty.value, catalog);
        Require(encoded.Succeeded(), "serialize original WTG/WCT pair");
        triggers = std::move(*encoded.value);
        members = {{"war3map.wtg", triggers.wtg}, {"war3map.wct", triggers.wct},
            {"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
            {"unmanaged.bin", Bytes("retained original payload")}};
        if (withPlacements) {
            members.push_back({"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes});
            members.push_back({"war3mapUnits.doo", Units()});
            members.push_back({"war3map.w3u", NativeUnit()});
            members.push_back({"war3map.w3t", EmptyNative()});
            WriteGame(game);
        }
        for (const auto& member : members) Write(expanded / std::filesystem::path(member.name), member.data);
        Require(WriteVerifiedArchive(source, members).Succeeded(), "write original source map MPQ");
        auto opened = MapArchiveSession::Open(source);
        Require(opened.Succeeded(), "open original source MPQ");
        archive = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    }
    void Prepare(MapSaveTask& task, std::shared_ptr<const Snapshot> references, std::string_view target,
        const std::filesystem::path& gameRoot) const {
        Require(task.FreezeArchive(*archive).Succeeded(), "freeze original source for shared save service");
        task.target = directory.Path() / std::filesystem::path(target);
        task.mapObjectReferences = std::move(references);
        task.mapObjectReferenceGameRoot = gameRoot;
        task.replacements = {{"unmanaged.bin", Bytes("independent payload edit")}};
    }
    void RequireSourceUnchanged() const {
        const auto hash = Sha256File(source);
        Require(hash.Succeeded() && *hash.value == archive->SourceSha256(), "source map SHA remains unchanged");
        const auto wtg = Sha256File(expanded / "war3map.wtg"), wct = Sha256File(expanded / "war3map.wct");
        const auto originalWtg = Sha256(triggers.wtg), originalWct = Sha256(triggers.wct);
        Require(wtg.Succeeded() && wct.Succeeded() && originalWtg.Succeeded() && originalWct.Succeeded()
            && *wtg.value == *originalWtg.value && *wct.value == *originalWct.value,
            "expanded source trigger pair remains unchanged");
    }
};

void ExpectRejected(MapSaveTask& task, const Fixture& fixture, std::string_view code) {
    MapSaveService::Run(task);
    if (task.succeeded || !HasCode(task.diagnostics, code)) Explain(task.diagnostics);
    Require(task.executed && !task.succeeded && HasCode(task.diagnostics, code), "source mismatch/read failure must reject the frozen save");
    Require(!std::filesystem::exists(task.target) && task.written.empty() && !task.nextSession,
        "rejected save must not publish target or establish a new session");
    fixture.RequireSourceUnchanged();
}

void DirectoryAndArchiveCanMoveEqualMemberBytes() {
    Fixture fixture;
    const auto directory = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded, fixture.game));
    const auto archive = Captured(CaptureArchiveMapObjectReferences(*fixture.archive, fixture.game));
    Require(directory->References().size() == 2 && directory->Find("gg_unit_n00Q_0017")
        && directory->Find("gg_unit_hfoo_0018"), "real native and real game SLK independently establish positive unit facts");
    Require(directory->Identity() != archive->Identity(), "strict snapshots retain distinct physical source locations");
    Require(VerifyMapObjectReferenceContents(*directory, *archive).Succeeded(), "equal map bytes allow directory-to-archive migration");
    Require(VerifyMapObjectReferenceStandardSources(*directory, fixture.game).Succeeded(), "same standard origin verifies");

    MapSaveTask task;
    fixture.Prepare(task, directory, "equal-final-members.w3x", fixture.game);
    task.replacements.push_back({"war3mapUnits.doo", Units()});
    task.replacements.push_back({"war3map.w3u", NativeUnit()});
    MapSaveService::Run(task);
    if (!task.succeeded) Explain(task.diagnostics);
    // 临时目录可由 Windows 返回 ADMINI~1；归档服务返回展开后的规范路径。
    // 校验实际文件身份，不能把同一文件的长短路径差异误当成发布到别处。
    Require(task.succeeded && task.nextSession && std::filesystem::equivalent(task.written, task.target),
        "matching final replacements publish and reopen through shared save service");
    const auto outputHash = Sha256File(task.target);
    Require(outputHash.Succeeded() && *outputHash.value == task.nextSession->SourceSha256(), "new session binds actual candidate SHA");
    const auto output = Captured(CaptureArchiveMapObjectReferences(*task.nextSession, fixture.game));
    Require(VerifyMapObjectReferenceContents(*directory, *output).Succeeded(), "reopened candidate keeps the same generated-object qualifications");
    const auto wtg = task.nextSession->ReadMember("war3map.wtg"), wct = task.nextSession->ReadMember("war3map.wct");
    Require(wtg.Succeeded() && wct.Succeeded() && *wtg.value == fixture.triggers.wtg && *wct.value == fixture.triggers.wct,
        "candidate preserves real editable WTG/WCT bytes");
    const auto reopenedT = Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, fixture.catalog);
    Require(reopenedT.Succeeded() && reopenedT.value->triggers.size() == 1 && reopenedT.value->triggers.front().customTextFlag == 0,
        "reopened trigger remains ordinary GUI T");
    const auto payload = task.nextSession->ReadMember("unmanaged.bin");
    Require(payload.Succeeded() && *payload.value == Bytes("independent payload edit"), "candidate contains the intended independent edit");
    fixture.RequireSourceUnchanged();
}

void FinalPlacementAndNativeReplacementAreBound() {
    Fixture fixture;
    const auto references = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded, fixture.game));
    {
        MapSaveTask task;
        fixture.Prepare(task, references, "changed-final-doo.w3x", fixture.game);
        task.replacements.push_back({"war3mapUnits.doo", Units(99.f)});
        ExpectRejected(task, fixture, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED");
    }
    {
        MapSaveTask task;
        fixture.Prepare(task, references, "changed-final-native.w3x", fixture.game);
        task.replacements.push_back({"war3map.w3u", NativeUnit("P002")});
        ExpectRejected(task, fixture, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED");
    }
    {
        MapSaveTask task;
        fixture.Prepare(task, references, "removed-final-native.w3x", fixture.game);
        task.removals = {"war3map.w3u"};
        ExpectRejected(task, fixture, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED");
    }
    {
        MapSaveTask task;
        fixture.Prepare(task, references, "damaged-final-native.w3x", fixture.game);
        task.replacements.push_back({"war3map.w3u", {std::byte{0}}});
        ExpectRejected(task, fixture, "MAP_OBJECT_REFERENCE_NATIVE_UNQUALIFIED");
    }
}

void StandardRootAndShadowOriginCannotChange() {
    Fixture fixture;
    const auto references = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded, fixture.game));
    const auto otherGame = fixture.directory.Path() / "same-bytes-other-game";
    std::filesystem::create_directory(otherGame);
    Require(std::filesystem::copy_file(fixture.game / "war3.mpq", otherGame / "war3.mpq"), "copy only the owned tiny synthetic MPQ");
    const auto other = Captured(CaptureArchiveMapObjectReferences(*fixture.archive, otherGame));
    Require(!VerifyMapObjectReferenceContents(*references, *other).Succeeded(), "identical standard bytes from another root are not the same source");
    Require(!VerifyMapObjectReferenceStandardSources(*references, otherGame).Succeeded(), "standard-only verifier rejects a changed root");
    MapSaveTask rootTask;
    fixture.Prepare(rootTask, references, "wrong-standard-root.w3x", otherGame);
    ExpectRejected(rootTask, fixture, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED");

    WriteGame(fixture.game, "war3patch.mpq");
    MapSaveTask shadowTask;
    fixture.Prepare(shadowTask, references, "shadowed-standard.w3x", fixture.game);
    ExpectRejected(shadowTask, fixture, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED");
}

void StandardBytesAndReadabilityAreBound() {
    {
        Fixture fixture;
        const auto references = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded, fixture.game));
        // 保留旧合成来源；只替换本夹具路径，同名、同类别但真实SLK字节已不同。
        std::filesystem::rename(fixture.game / "war3.mpq", fixture.game / "original-war3.mpq");
        WriteGame(fixture.game, "war3.mpq", "Changed synthetic display");
        Require(!VerifyMapObjectReferenceStandardSources(*references, fixture.game).Succeeded(), "standard-only verifier binds actual bytes");
        MapSaveTask task;
        fixture.Prepare(task, references, "changed-standard-bytes.w3x", fixture.game);
        ExpectRejected(task, fixture, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED");
    }
    {
        Fixture fixture;
        const auto references = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded, fixture.game));
        struct ExclusiveHandle {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~ExclusiveHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        };
        {
            ExclusiveHandle lock;
            lock.value = CreateFileW((fixture.game / "war3.mpq").c_str(), GENERIC_READ, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            Require(lock.value != INVALID_HANDLE_VALUE, "exclusively lease the owned synthetic game archive");
            MapSaveTask task;
            fixture.Prepare(task, references, "unreadable-standard.w3x", fixture.game);
            // 当前来源捕获先取只读租约；独占锁必须在进入游戏包读取前拒绝，不能降级为缺表。
            ExpectRejected(task, fixture, "WORLD_ASSET_SOURCE_CHANGED");
            Require(HasCode(task.diagnostics, "RESOURCE_PROBE_FAILED"),
                "failed game-source lease propagates through the resource probe without publishing a save");
        }
        Require(VerifyMapObjectReferenceStandardSources(*references, fixture.game).Succeeded(),
            "the unchanged owned source verifies again after the exclusive test lock is released");
    }
    {
        Fixture fixture;
        const auto references = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded, fixture.game));
        WriteText(fixture.game / "war3patch.mpq", "present corrupt high-priority archive");
        MapSaveTask task;
        fixture.Prepare(task, references, "corrupt-shadow.w3x", fixture.game);
        ExpectRejected(task, fixture, "archive-open-failed");
    }
}

void MissingStandardSourceStillHasAnIdentity() {
    Fixture fixture;
    const auto game = fixture.directory.Path() / "game-without-classification-tables";
    WriteGame(game, "war3.mpq", "Synthetic", false);
    const auto references = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded, game));
    Require(references->Inputs().size() == 9 && !references->Inputs()[3].present && !references->Inputs()[4].present,
        "real absent game members are captured as missing");
    const auto check = VerifyMapObjectReferenceStandardSources(*references, {});
    Require(!check.Succeeded() && HasCode(check.diagnostics, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED"),
        "a configured missing source cannot silently become an unconfigured source");
    MapSaveTask task;
    fixture.Prepare(task, references, "removed-missing-standard-origin.w3x", {});
    ExpectRejected(task, fixture, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED");
}

void OrdinaryWtgOnlyWorkspaceNeedsNoDooQualification() {
    Fixture fixture(false);
    const auto references = Captured(CaptureDirectoryMapObjectReferences(fixture.expanded));
    Require(references->References().empty() && references->Inputs().size() == 9 && !references->Inputs().front().present
        && HasCode(references->Diagnostics(), "MAP_OBJECT_REFERENCE_MEMBER_MISSING"), "WTG-only workspace captures empty external-object qualification");
    const auto archive = Captured(CaptureArchiveMapObjectReferences(*fixture.archive));
    Require(VerifyMapObjectReferenceContents(*references, *archive).Succeeded(), "matching missing DOO survives source migration");
    MapSaveTask task;
    fixture.Prepare(task, references, "wtg-only-candidate.w3x", {});
    MapSaveService::Run(task);
    if (!task.succeeded) Explain(task.diagnostics);
    Require(task.succeeded && task.nextSession && std::filesystem::exists(task.target), "empty external context does not block independent valid archive saves");
    fixture.RequireSourceUnchanged();
}
} // namespace

int main() {
    try {
        DirectoryAndArchiveCanMoveEqualMemberBytes();
        FinalPlacementAndNativeReplacementAreBound();
        StandardRootAndShadowOriginCannotChange();
        StandardBytesAndReadabilityAreBound();
        MissingStandardSourceStillHasAnIdentity();
        OrdinaryWtgOnlyWorkspaceNeedsNoDooQualification();
        std::cout << "map-object-reference-save: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "map-object-reference-save: " << error.what() << '\n';
        return 1;
    }
}
