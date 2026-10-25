#include "EditorProject.h"
#include "ProjectOpen.h"
#include "ProjectObjectCatalog.h"
#include "AssetOwnedPayload.h"
#include "ProjectQuerySources.h"
#include "MapSaveJob.h"
#include "ProjectSaveJob.h"
#include "Commands/MapDataCommands.h"
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
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Services/TerrainMinimapSave.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainShadowBake.h"
#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "../../src/Core/Mpq/MpqCrypt.h"
#include "../MapDataFixtures.h"
#include "SyntheticObjectCatalogFixture.h"
#include "TerrainWaterEditFixture.h"
#include "../TerrainCornerRampFixtures.h"
#include "../support/TerrainBoundsFixture.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QSettings>
#include <QStandardItemModel>
#include <QDir>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUndoStack>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>

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

// 原创 MPQ：默认只有known.bin列名；恢复测试可独立列名W3E/WPM，块始终保留准确hash。
bool WriteUnlistedTerrain(const QString& path, const std::vector<std::byte>& terrain,
    std::uint32_t extraFlags = 0, std::uint32_t declaredBytes = 0,
    const std::vector<std::byte>* pathing = nullptr, bool listTerrain = false, bool listPathing = false) {
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const std::string list = std::string("known.bin\r\n") + (listTerrain ? "war3map.w3e\r\n" : "")
        + (pathing && listPathing ? "war3map.wpm\r\n" : "");
    const auto listSpan = std::as_bytes(std::span(list.data(), list.size()));
    constexpr std::uint32_t hashCount = 8, knownAt = 32, terrainAt = 33;
    const std::uint32_t blockCount = pathing ? 4 : 3;
    const auto pathingAt = terrainAt + static_cast<std::uint32_t>(terrain.size());
    const auto listAt = pathingAt + static_cast<std::uint32_t>(pathing ? pathing->size() : 0);
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
    if (pathing) std::copy(pathing->begin(), pathing->end(), bytes.begin() + pathingAt);
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
    hash("known.bin", 0); hash("war3map.w3e", 1);
    if (pathing) hash("war3map.wpm", 2);
    hash("(listfile)", blockCount - 1);
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
    if (pathing) block(2, pathingAt, pathing->size(), static_cast<std::uint32_t>(pathing->size()), 0);
    block(blockCount - 1, listAt, list.size(), static_cast<std::uint32_t>(list.size()), 0);
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

std::string ReadDiagnosticText(const auto& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) {
        if (!text.empty()) text += '\n';
        text += diagnostic.code + ": " + diagnostic.message;
    }
    return text.empty() ? "operation returned no diagnostics" : text;
}

std::vector<MapArchiveMember> TerrainSaveMembers() {
    MapData::TerrainGridSnapshot terrain;
    terrain.header.width = terrain.header.height = 9;
    terrain.header.mainTileset = 'L';
    terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"Cl01"};
    terrain.vertices.resize(81);
    for (auto& vertex : terrain.vertices) {
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2;
    }
    MapData::RefreshTerrainDerived(terrain, 0, 0, 9, 9);
    const auto terrainBytes = MapData::SerializeMapTerrain(terrain);
    if (!terrainBytes.Succeeded() || !terrainBytes.value) return {};
    MapDataFixtures::Fixture pathing;
    pathing.Raw("MP3W"); pathing.UInt(0); pathing.UInt(32); pathing.UInt(32);
    pathing.bytes.resize(16 + 32 * 32, std::byte{0x82});
    const std::string script = "function config takes nothing returns nothing\nendfunction\n"
        "function main takes nothing returns nothing\nendfunction\n";
    const auto scriptBytes = std::as_bytes(std::span(script.data(), script.size()));
    // SHD仅作为不解释的原字追踪值；MMP真实空，MapBLP缺失，不构成原版地图验收。
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.w3e", *terrainBytes.value}, {"war3map.wpm", std::move(pathing.bytes)},
        {"war3map.shd", {std::byte{7}, std::byte{11}}}, {"war3map.mmp", {}},
        {"war3map.j", {scriptBytes.begin(), scriptBytes.end()}}};
}

bool BindTerrainSaveProject(EditorProject& project, const std::filesystem::path& source,
    const std::function<bool()>& beforeLoad = {}) {
    auto opened = MapArchiveSession::Open(source);
    if (!opened.Succeeded() || !opened.value) return false;
    project.projectRoot_ = source.parent_path(); project.sourceArchivePath_ = source;
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    // 来源变化夹具在旧归档快照之后、LoadMapDocument建立物编来源租约之前注入。
    if (beforeLoad && !beforeLoad()) return false;
    project.LoadMapDocument();
    if (!project.mapEdits_ || !project.mapEdits_->Terrain()) return false;
    project.ResetEditTracking();
    return project.editTrackingReady_ && project.mapEdits_->VerifyTerrainPathingForSave().Succeeded();
}

bool RaiseTerrainSaveProject(EditorProject& project, float strength = 10) {
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Raise; brush.radius = 0; brush.strength = strength;
    const MapData::TerrainStrokePoint point{512, 512};
    return project.mapEdits_ && project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded()
        && project.ObserveMapDataEdit("原创地形保存回读夹具");
}

std::vector<MapArchiveMember> MissingMinimapQtMembers() {
    auto members = TerrainSaveMembers();
    const auto text = [](std::string_view value) {
        const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    members.push_back({"TerrainArt\\Terrain.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"tileID\"\nC;X2;Y1;K\"dir\"\nC;X3;Y1;K\"file\"\n"
        "C;X1;Y2;K\"Tg01\"\nC;X2;Y2;K\"Fixture\"\nC;X3;Y2;K\"Ground\"\nE\n")});
    members.push_back({"TerrainArt\\CliffTypes.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nE\n")});
    members.push_back({"TerrainArt\\Water.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"waterID\"\nE\n")});
    // 原创手造16→8→4→2→1调色板BLP，不调用生产encoder生成来源。
    std::vector<std::byte> atlas(1180 + 341);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned shift = 0; shift < 4; ++shift) atlas[at + shift] = static_cast<std::byte>((value >> (shift * 8)) & 255);
    };
    const auto magic = text("BLP1"); std::copy(magic.begin(), magic.end(), atlas.begin());
    put(4, 1); put(12, 16); put(16, 16); put(20, 5); put(24, 1);
    atlas[156 + 1 * 4] = std::byte{40}; atlas[156 + 1 * 4 + 1] = std::byte{120}; atlas[156 + 1 * 4 + 2] = std::byte{80};
    std::size_t offset = 1180;
    for (std::uint32_t level = 0, size = 16; level < 5; ++level, size /= 2) {
        put(28 + level * 4, static_cast<std::uint32_t>(offset)); put(92 + level * 4, size * size);
        std::fill_n(atlas.begin() + static_cast<std::ptrdiff_t>(offset), size * size, std::byte{1}); offset += size * size;
    }
    members.push_back({"Fixture\\Ground.blp", std::move(atlas)});
    return members;
}

std::vector<MapArchiveMember> DeclaredMissingMinimapQtMembers() {
    auto members = MissingMinimapQtMembers();
    // 独立classic v25原字，8×8 tiles扣L/R/B/T={1,3,2,1}后为4×5；不用writer生成期望。
    std::vector<std::byte> settings(147);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned b = 0; b < 4; ++b) settings[at + b] = static_cast<std::byte>((value >> (b * 8)) & 255);
    };
    put(0, 25); put(8, 6061); put(48, 1); put(52, 3); put(56, 2); put(60, 1);
    put(64, 4); put(68, 5); settings[76] = std::byte{'L'};
    MapDataFixtures::Fixture terrain;
    terrain.Raw("W3E!"); terrain.UInt(11); terrain.Byte('L'); terrain.UInt(0);
    terrain.UInt(1); terrain.Raw("Tg01"); terrain.UInt(1); terrain.Raw("Cl01");
    terrain.UInt(9); terrain.UInt(9); terrain.Float(0); terrain.Float(0);
    for (unsigned y = 0; y < 9; ++y) for (unsigned x = 0; x < 9; ++x) {
        const bool edge = x < 8 && y < 8 && (x < 1 || x >= 5 || y < 2 || y >= 7);
        terrain.Byte(0); terrain.Byte(32); terrain.Byte(0); terrain.Byte(edge ? 96 : 32);
        terrain.Byte(0); terrain.Byte(0); terrain.Byte(0xf2);
    }
    members[0].data = std::move(settings); members[1].data = std::move(terrain.bytes); return members;
}

bool RaiseDeclaredMinimapProject(EditorProject& project, float strength = 39) {
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Raise; brush.radius = 0; brush.strength = strength;
    // vertex3,4的完整一格邻域均在声明内域，不借新mask策略放宽原height SaveGate。
    const MapData::TerrainStrokePoint point{384, 512};
    return project.mapEdits_ && project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded()
        && project.ObserveMapDataEdit("声明边距单次小地图高度夹具");
}

std::vector<MapArchiveMember> NativePathingQtMembers() {
    auto members = MissingMinimapQtMembers();
    const auto text = [](std::string_view value) {
        const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    MapData::MapSettings settings;
    settings.formatVersion = 25; settings.custom.emplace(); settings.flags = 0x400;
    settings.tileset = 'L'; settings.playableWidth = settings.playableHeight = 8;
    const auto settingsBytes = settings.Serialize();
    const auto doo = MapData::DoodadPlacements{}.Serialize();
    if (!settingsBytes.Succeeded() || !settingsBytes.value || !doo.Succeeded() || !doo.value) return {};
    const auto terrain = MapData::ParseMapTerrain(members[1].data);
    if (!terrain.Succeeded() || !terrain.value) return {};
    auto grid = *terrain.value->Snapshot();
    for (auto& vertex : grid.vertices) {
        vertex.waterHeightAndFlagsRaw = 8384; // 基础水高48；原地高0，独立期望为浅水。
        vertex.textureAndFlagsRaw = 0x40;
    }
    MapData::RefreshTerrainDerived(grid, 0, 0, 9, 9);
    const auto wire = MapData::SerializeMapTerrain(grid);
    if (!wire.Succeeded() || !wire.value) return {};
    members[0].data = *settingsBytes.value; members[1].data = *wire.value;
    // 独立手写原版浅水G0=0x08，并保留不属于原版生成位的0x11；不调用生成器造期望。
    std::fill(members[2].data.begin() + 16, members[2].data.end(), std::byte{0x19});
    for (auto& member : members) {
        if (member.name == "TerrainArt\\Terrain.slk")
            member.data = text("ID;PWXL;N;E\nC;X1;Y1;K\"tileID\"\nC;X2;Y1;K\"dir\"\nC;X3;Y1;K\"file\"\n"
                "C;X4;Y1;K\"walkable\"\nC;X5;Y1;K\"flyable\"\nC;X6;Y1;K\"buildable\"\n"
                "C;X1;Y2;K\"Tg01\"\nC;X2;Y2;K\"Fixture\"\nC;X3;Y2;K\"Ground\"\n"
                "C;X4;Y2;K1\nC;X5;Y2;K1\nC;X6;Y2;K1\nE\n");
        if (member.name == "TerrainArt\\Water.slk") {
            const std::pair<std::string_view, std::string_view> fields[]{
                {"waterID", "LSha"}, {"texFile", "Fixture\\Water"}, {"height", "0"}, {"numTex", "1"},
                {"texRate", "1"}, {"texOffset", "0"}, {"cells", "1"}, {"alphaMode", "0"},
                {"lighting", "0"}, {"impassable", "0"}, {"mmRed", "20"}, {"mmGreen", "50"},
                {"mmBlue", "90"}, {"mmAlpha", "128"}, {"Smin_R", "0"}, {"Smin_G", "0"},
                {"Smin_B", "0"}, {"Smin_A", "0"}, {"Smax_R", "0"}, {"Smax_G", "0"},
                {"Smax_B", "0"}, {"Smax_A", "0"}, {"Dmin_R", "0"}, {"Dmin_G", "0"},
                {"Dmin_B", "0"}, {"Dmin_A", "0"}, {"Dmax_R", "0"}, {"Dmax_G", "0"},
                {"Dmax_B", "0"}, {"Dmax_A", "0"}};
            std::string table = "ID;PWXL;N;E\n";
            std::size_t column = 0;
            for (const auto& [name, value] : fields) {
                const auto x = std::to_string(++column);
                table += "C;X" + x + ";Y1;K\"" + std::string(name) + "\"\nC;X" + x + ";Y2;K\"" + std::string(value) + "\"\n";
            }
            member.data = text(table + "E\n");
        }
    }
    members.push_back({"war3map.doo", *doo.value});
    members.push_back({"UI\\MiscData.txt", text("[Water]\nDeepLevelPathing=64\n")});
    members.push_back({"UI\\WorldEditData.txt", text("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")});
    return members;
}

bool DeepenNativePathingQtProject(EditorProject& project) {
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::WaterHeight;
    brush.radius = 0; brush.waterHeight = 80;
    const MapData::TerrainStrokePoint point{512, 512};
    return project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded()
        && project.ObserveMapDataEdit("原创浅水改为深水");
}

std::unique_ptr<MapSaveJob> FreezeNativePathingQtJob(EditorProject& project, const std::filesystem::path& target,
    Services::TerrainMinimapSaveChoice minimap = Services::TerrainMinimapSaveChoice::Preserve) {
    // 仅替代脚本工具链准备；实际最终派生、归档写出、回读和前台提交走生产入口。
    auto job = std::make_unique<MapSaveJob>();
    const auto draft = project.mapEdits_->SerializePathingRestrictionDraft();
    if (!draft.Succeeded() || !draft.value || !job->FreezeArchive(*project.archiveSession_).Succeeded()) return {};
    job->target = target; job->minimapChoice = minimap;
    job->minimapGameRoot = project.war3GameRoot_;
    job->replacements = project.mapEdits_->Replacements(); job->removals = project.mapEdits_->Removals();
    job->terrainDerivationRequired = MapData::TerrainSavePlan::HasTerrainChanges(*project.mapEdits_);
    // 与生产Prepare一致：没有意图层时不传声明；显式传入的空声明仍由worker拒绝。
    if (!draft.value->empty())
        job->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>(*draft.value);
    job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(),
        project.editRevision_.Current(), project.luaMapTargetGeneration_};
    return job;
}

std::vector<std::byte> NativeDoodadQtTable(const std::vector<std::string>& columns,
    const std::vector<std::vector<std::string>>& rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const std::vector<std::string>& cells, std::size_t row) {
        for (std::size_t x = 0; x < cells.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(row) + ";K\"" + cells[x] + "\"\n";
    };
    append(columns, 1);
    for (std::size_t y = 0; y < rows.size(); ++y) append(rows[y], y + 2);
    return Tests::WaterText(text + "E\n");
}

std::vector<MapArchiveMember> TerrainShadowQtMembers(bool ridge, bool caster, bool authoredMinimap = true) {
    auto members = TerrainSaveMembers();
    if (members.empty()) return {};
    MapData::MapSettings settings;
    settings.formatVersion = 25; settings.custom.emplace(); settings.flags = 0x400;
    settings.tileset = 'L'; settings.playableWidth = settings.playableHeight = 8;
    const auto settingsBytes = settings.Serialize();
    if (!settingsBytes.Succeeded() || !settingsBytes.value) return {};
    members[0].data = *settingsBytes.value;
    auto terrain = MapData::ParseMapTerrain(members[1].data);
    if (!terrain.Succeeded() || !terrain.value) return {};
    auto grid = *terrain.value->Snapshot();
    if (ridge) for (std::size_t y = 0; y < 9; ++y) grid.vertices[y * 9 + 4].groundHeightRaw = 10240;
    MapData::RefreshTerrainDerived(grid, 0, 0, 9, 9);
    const auto wire = MapData::SerializeMapTerrain(grid);
    if (!wire.Succeeded() || !wire.value) return {};
    members[1].data = *wire.value; members[3].data.assign(32 * 32, std::byte{0x7d});
    MapData::DoodadPlacements placements;
    if (caster) {
        MapData::DoodadPlacement placed;
        placed.rawcode = "Sh01"; placed.position = {256, 256, 0}; placed.scale = {1, 1, 1};
        placements.doodads.push_back(placed);
        members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable({"destructableID"}, {{"Dst0"}})});
        members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable(
            {"doodID", "shadow", "defScale", "maxPitch", "maxRoll", "fixedRot", "floats", "numVar", "file"},
            {{"Sh01", "1", "1", "0", "0", "-1", "0", "1", "Fixture/ShadowCaster"}})});
        // 捕获owner冻结真实成员原字；本例在非空caster缺高度边界时、模型解析前拒绝，不能当模型oracle。
        members.push_back({"Fixture/ShadowCaster.mdx", Tests::WaterText("original bounded caster bytes")});
    }
    const auto doo = placements.Serialize();
    if (!doo.Succeeded() || !doo.value) return {};
    members.push_back({"war3map.doo", *doo.value});
    if (authoredMinimap) members.push_back({"war3mapMap.tga", Tests::WaterText("authored Qt minimap sentinel")});
    return members;
}

std::filesystem::path TerrainShadowQtGame(const QString& directory, bool withDirection) {
    const auto game = directory + "/shadow-game";
    if (!QDir().mkpath(game)) return {};
    const auto path = std::filesystem::path(game.toStdWString());
    const std::vector<MapArchiveMember> gameMembers{{"fixture.txt", Tests::WaterText("private Qt shadow source")}};
    if (!WriteVerifiedArchive(path / "war3.mpq", gameMembers).Succeeded()) return {};
    if (withDirection) {
        QFile profile(game + "/WorldEditData.txt");
        const QByteArray text("[Light]\nDirection=3,0,-4\n");
        if (!profile.open(QIODevice::WriteOnly | QIODevice::NewOnly) || profile.write(text) != text.size()) return {};
    }
    return path;
}

std::vector<std::byte> LiteralTerrainShadowQtExpected(bool ridge) {
    // 独立原字：512高的x=4脊线、Direction=3,0,-4；32×32 receiver的x17..26被覆盖。
    // 不调用产品采样器、投影器或SHD编码器制造expected。
    std::vector<std::byte> expected(32 * 32, std::byte{0});
    if (ridge) for (std::size_t y = 0; y < 32; ++y)
        for (std::size_t x = 17; x <= 26; ++x) expected[y * 32 + x] = std::byte{0xff};
    return expected;
}

std::vector<MapArchiveMember> CGridInvariantQtMembers(int divergentAxis = -1, bool occluded = false) {
    auto members = NativePathingQtMembers();
    if (members.empty()) return {};
    const auto texture = [](std::size_t side) {
        std::vector<std::byte> bytes(18 + side * side * 4, std::byte{0});
        bytes[2] = std::byte{2}; bytes[12] = bytes[14] = static_cast<std::byte>(side);
        bytes[16] = std::byte{32}; bytes[17] = std::byte{8};
        // 原创BGRA={0,255,0,0}：透明不清除绿色通行位，且不会被浅水的0x08掩盖。
        for (std::size_t at = 18; at < bytes.size(); at += 4) bytes[at + 1] = std::byte{255};
        return bytes;
    };
    members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable({"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "0"}})});
    members.push_back({"Units/DestructableMetaData.slk", NativeDoodadQtTable({"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
    members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Cg01", "Cg01", "PathTextures/C-grid.tga", "C", "270"},
            {"Eg01", "Eg01", "PathTextures/00-cover.tga", "E", "270"}})});
    members.push_back({"PathTextures/C-grid.tga", texture(8)});
    members.push_back({"PathTextures/00-cover.tga", texture(16)});
    MapData::DoodadPlacements placements;
    MapData::DoodadPlacement placed;
    placed.rawcode = "Cg01"; placed.position = {256, 256, 0}; placed.scale = {1, 1, 1};
    placed.flags = 2; placed.creationId = 1; placements.doodads.push_back(placed);
    placed.position = {512, 512, 0}; placed.creationId = 2;
    if (divergentAxis >= 0) placed.position[static_cast<std::size_t>(divergentAxis)] += 32;
    placements.doodads.push_back(placed);
    if (occluded) {
        auto cover = placed; cover.rawcode = "Eg01"; cover.position = {512, 512, 0}; cover.creationId = 3;
        placements.doodads.insert(placements.doodads.begin(), cover);
    }
    const auto wire = placements.Serialize();
    if (!wire.Succeeded() || !wire.value) return {};
    for (auto& member : members) if (member.name == "war3map.doo") member.data = *wire.value;
    // 独立格子预期：8x8在256和512中心起点为4、12；+32使snap2起点多2格，snap4仍为12。
    // 偏移行字面写snap2来源；实际C启动目录选snap4时由完整旧WPM解释门裁决。
    const auto footprint = [&](std::size_t x0, std::size_t y0, std::size_t side) {
        for (std::size_t y = y0; y < y0 + side; ++y) for (std::size_t x = x0; x < x0 + side; ++x)
            members[2].data[16 + y * 32 + x] |= std::byte{4};
    };
    footprint(4, 4, 8);
    footprint(divergentAxis == 0 ? 14 : 12, divergentAxis == 1 ? 14 : 12, 8);
    if (occluded) footprint(8, 8, 16); // 非C实例遮住差异，两个来源矩阵等价时允许按实际目录保存。
    return members;
}

bool WriteStartupCategoryQtGame(const std::filesystem::path& gameRoot, std::string_view profile) {
    if (!std::filesystem::create_directory(gameRoot)) return false;
    // 空文本表示固定启动profile全缺席；资格仍由实际目录捕获，不传调用方hasC。
    if (profile.empty()) return true;
    const std::vector<MapArchiveMember> members{{"UI/WorldEditData.txt", Tests::WaterText(profile)}};
    return WriteVerifiedArchive(gameRoot / "war3.mpq", members).Succeeded();
}

std::vector<MapArchiveMember> StartupCategoryCQtMembers(std::size_t footprintStart) {
    auto members = CGridInvariantQtMembers();
    if (members.empty() || (footprintStart != 12 && footprintStart != 14)) return {};
    const auto doo = std::find_if(members.begin(), members.end(),
        [](const auto& member) { return member.name == "war3map.doo"; });
    if (doo == members.end()) return {};
    auto parsed = MapData::ParseDoodadPlacements(doo->data);
    if (!parsed.Succeeded() || !parsed.value || parsed.value->doodads.empty()) return {};
    auto placed = parsed.value->doodads.front(); placed.position = {544, 544, 0};
    parsed.value->doodads = {placed};
    const auto wire = parsed.value->Serialize();
    if (!wire.Succeeded() || !wire.value) return {};
    doo->data = *wire.value;
    // 纯字面矩阵：同一544中心，实际目录含C时[12,20)^2，否则[14,22)^2。
    // 不调用任何产品吸附/占地生成器来制造来源或期望。
    std::fill(members[2].data.begin() + 16, members[2].data.end(), std::byte{0x19});
    for (std::size_t y = footprintStart; y < footprintStart + 8; ++y)
        for (std::size_t x = footprintStart; x < footprintStart + 8; ++x)
            members[2].data[16 + y * 32 + x] |= std::byte{4};
    // 地图同名profile刻意含C；不能替代真实gameRoot启动来源的缺席或小写c。
    for (auto& member : members) if (member.name == "UI\\WorldEditData.txt")
        member.data = Tests::WaterText("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n"
            "[DoodadCategories]\nC=map-must-not-select-grid\n");
    return members;
}

std::vector<MapArchiveMember> EffectiveCategoryQtMembers(bool custom, const std::string& standardCategory,
    const std::optional<std::string>& categoryOverride, const std::string& rawcode, std::size_t footprintStart) {
    using namespace ObjectData;
    auto members = StartupCategoryCQtMembers(footprintStart);
    if (members.empty()) return {};
    const std::string parent = custom ? (standardCategory == "C" ? "PC01" : "PE01") : rawcode;
    for (auto& member : members) {
        if (member.name == "Doodads/Doodads.slk")
            member.data = NativeDoodadQtTable({"doodID", "code", "pathTex", "category", "fixedRot"},
                {{parent, parent, "PathTextures/C-grid.tga", standardCategory, "270"}});
        if (member.name == "war3map.doo") {
            auto placements = MapData::ParseDoodadPlacements(member.data);
            if (!placements.Succeeded() || !placements.value || placements.value->doodads.size() != 1) return {};
            placements.value->doodads.front().rawcode = rawcode;
            const auto wire = placements.value->Serialize();
            if (!wire.Succeeded() || !wire.value) return {};
            member.data = *wire.value;
        }
    }
    // dcat真实类型为doodadCategory，wire必须为String；不以字段名替代metadata地址校验。
    members.push_back({"Doodads/DoodadMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"dptx", "pathTex", "DoodadData", "pathingTexture", "0", "0", "0"},
            {"dcat", "category", "DoodadData", "doodadCategory", "0", "0", "0"}})});
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Doodad);
    if (!native.Succeeded() || !native.value) return {};
    const auto added = custom ? native.value->AddCustomObject(rawcode, parent) : native.value->AddOriginalObject(rawcode);
    if (!added.Succeeded()) return {};
    if (categoryOverride && !native.value->AddModification({ObjectType::Doodad, rawcode}, {"dcat"},
        NativeValueType::String, *categoryOverride).Succeeded()) return {};
    const auto wire = NativeObjectCodec(ObjectType::Doodad).Serialize(*native.value);
    if (!wire.Succeeded() || !wire.value) return {};
    members.push_back({"war3map.w3d", *wire.value});
    return members;
}

std::vector<MapArchiveMember> FixedRotationQtMembers(bool custom, float degrees, bool wrongWireType = false) {
    using namespace ObjectData;
    auto members = NativePathingQtMembers();
    if (members.empty()) return {};
    members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable({"destructableID", "code", "walkable"},
        {{"Dst0", "Dst0", "0"}})});
    members.push_back({"Units/DestructableMetaData.slk", NativeDoodadQtTable({"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
    members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Fr01", "Fr01", "PathTextures/fixed-rotation.tga", "E", "270"}})});
    // 原创对象表使用已闭合的实际dfxr地址/类型契约：DoodadData、非重复零索引、Unreal F32。
    members.push_back({"Doodads/DoodadMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"dptx", "pathTex", "DoodadData", "pathingTexture", "0", "0", "0"},
            {"dfxr", "fixedRot", "DoodadData", "unreal", "0", "0", "0"}})});
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Doodad);
    if (!native.Succeeded() || !native.value) return {};
    const std::string rawcode = custom ? "Zr01" : "Fr01";
    const auto added = custom ? native.value->AddCustomObject(rawcode, "Fr01") : native.value->AddOriginalObject(rawcode);
    if (!added.Succeeded()) return {};
    const auto modified = wrongWireType
        ? native.value->AddModification({ObjectType::Doodad, rawcode}, {"dfxr"}, NativeValueType::Int32, std::int32_t{90})
        : native.value->AddModification({ObjectType::Doodad, rawcode}, {"dfxr"}, NativeValueType::Unreal, degrees);
    if (!modified.Succeeded()) return {};
    const auto encoded = NativeObjectCodec(ObjectType::Doodad).Serialize(*native.value);
    if (!encoded.Succeeded() || !encoded.value) return {};
    members.push_back({"war3map.w3d", *encoded.value});
    // 原创4×2顶原点BGRA：黑、蓝、绿、红、蓝绿、蓝红、黑、蓝；alpha=0仍保留通行。
    std::vector<std::byte> texture(18, std::byte{0});
    texture[2] = std::byte{2}; texture[12] = std::byte{4}; texture[14] = std::byte{2};
    texture[16] = std::byte{32}; texture[17] = std::byte{40};
    constexpr std::array<std::array<std::uint8_t, 3>, 8> pixels{{
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 0, 0}, {1, 0, 0}}};
    for (const auto& pixel : pixels) {
        for (const auto channel : pixel) texture.push_back(static_cast<std::byte>(channel));
        texture.push_back(std::byte{0});
    }
    members.push_back({"PathTextures/fixed-rotation.tga", std::move(texture)});
    MapData::DoodadPlacements placements;
    MapData::DoodadPlacement placed;
    placed.rawcode = rawcode; placed.position = {512, 512, 0}; placed.rotation = 0;
    placed.scale = {1, 1, 1}; placed.flags = 2; placed.creationId = 1;
    placements.doodads.push_back(placed);
    const auto doo = placements.Serialize();
    if (!doo.Succeeded() || !doo.value) return {};
    for (auto& member : members) if (member.name == "war3map.doo") member.data = *doo.value;
    // 独立文字矩阵：wire角0先吸附XY。fixedRot=0得到2×4起点(16,14)，90得到4×2起点(15,15)。
    // 标准270具有不同矩阵；忽略覆盖或按最终角度重新吸附均不能得到这些完整WPM。
    // 此处直接写文件位：内部blue04→WPM08、green02→04、red31→C2，混色35→CA、06→0C。
    constexpr std::array<std::uint8_t, 8> zero{0, 0x0c, 8, 0xca, 4, 0, 0xc2, 8};
    constexpr std::array<std::uint8_t, 8> ninety{0xc2, 4, 8, 0, 8, 0, 0xca, 0x0c};
    const bool forceZero = degrees == 0;
    const auto& matrix = forceZero ? zero : ninety;
    const std::size_t x0 = forceZero ? 16 : 15, y0 = forceZero ? 14 : 15, width = forceZero ? 2 : 4;
    for (std::size_t at = 0; at < matrix.size(); ++at)
        members[2].data[16 + (y0 + at / width) * 32 + x0 + at % width] |= static_cast<std::byte>(matrix[at]);
    return members;
}

std::vector<MapArchiveMember> WalkableDestructableQtMembers(bool patterned) {
    auto members = NativePathingQtMembers();
    if (members.empty()) return {};
    const auto parsed = MapData::ParseMapTerrain(members[1].data);
    if (!parsed.Succeeded() || !parsed.value) return {};
    auto terrain = *parsed.value->Snapshot();
    terrain.header.groundTilesets.push_back("Tg02");
    const auto wire = MapData::SerializeMapTerrain(terrain);
    if (!wire.Succeeded() || !wire.value) return {};
    members[1].data = *wire.value;
    for (auto& member : members) if (member.name == "TerrainArt\\Terrain.slk")
        member.data = NativeDoodadQtTable({"tileID", "dir", "file", "walkable", "flyable", "buildable"},
            {{"Tg01", "Fixture", "Ground", "1", "1", "1"}, {"Tg02", "Fixture", "Ground", "0", "1", "1"}});
    members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable(
        {"destructableID", "code", "walkable", "pathTex", "fixedRot"},
        {{"Br01", "Br01", "1", "PathTextures/bridge.tga", "0"}})});
    members.push_back({"Units/DestructableMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
    members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable(
        {"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Bl01", "Bl01", "PathTextures/blocker.tga", "E", "0"}})});
    const auto texture = [](bool bridge, bool pattern) {
        std::vector<std::byte> bytes(18 + 4 * 4 * 4, std::byte{0});
        bytes[2] = std::byte{2}; bytes[12] = bytes[14] = std::byte{4};
        bytes[16] = std::byte{32}; bytes[17] = std::byte{40};
        for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x) {
            const auto at = 18 + (y * 4 + x) * 4;
            const bool corner = (x == 0 || x == 3) && (y == 0 || y == 3);
            if (bridge && pattern && corner) bytes[at + 3] = std::byte{255};
            else if (bridge) {
                // 非零RGB、alpha=0应仅抑制terrain；RGB不能泄漏成普通静态阻挡位。
                bytes[at] = bytes[at + 1] = bytes[at + 2] = std::byte{255};
            } else bytes[at + 1] = std::byte{255};
        }
        return bytes;
    };
    members.push_back({"PathTextures/bridge.tga", texture(true, patterned)});
    members.push_back({"PathTextures/blocker.tga", texture(false, false)});
    MapData::DoodadPlacements placements;
    MapData::DoodadPlacement placed;
    placed.rawcode = "Br01"; placed.position = {544, 512, 0}; placed.scale = {1, 1, 1};
    placed.flags = 2; placed.lifePercent = 100; placed.creationId = 1;
    placements.doodads.push_back(placed);
    placed.rawcode = "Bl01"; placed.position = {512, 512, 0}; placed.creationId = 2;
    placements.doodads.push_back(placed);
    const auto doo = placements.Serialize();
    if (!doo.Succeeded() || !doo.value) return {};
    for (auto& member : members) if (member.name == "war3map.doo") member.data = *doo.value;
    // 独立字面来源矩阵：桥x16..19/y14..17仅去掉terrain08，普通green x14..17仍加04。
    // pattern四角黑色但alpha=255，必须保留浅水19；对称四角不依赖旋转方向。
    for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 16; x < 20; ++x) {
        const bool corner = (x == 16 || x == 19) && (y == 14 || y == 17);
        members[2].data[16 + y * 32 + x] = patterned && corner ? std::byte{0x19} : std::byte{0x11};
    }
    for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < 18; ++x)
        members[2].data[16 + y * 32 + x] |= std::byte{4};
    return members;
}

struct WalkableNativeQtFixture {
    std::vector<MapArchiveMember> members;
    std::vector<std::byte> editedPathing;
};

WalkableNativeQtFixture WalkableDestructableNativeQtMembers(std::string_view scenario,
    std::string_view customId = {}, std::string_view parentId = "Br01") {
    using namespace ObjectData;
    WalkableNativeQtFixture fixture{WalkableDestructableQtMembers(false), {}};
    auto& members = fixture.members;
    if (members.empty()) return {};
    const bool combined = scenario == "combined";
    const bool renamed = scenario == "renamed-real";
    const bool rectangular = combined || renamed || scenario.starts_with("rotation-");
    const bool overrideWalkable = combined || scenario == "walkable";
    const bool overridePath = combined || scenario == "path";
    const std::string rawcode = customId.empty() ? std::string(parentId) : std::string(customId);
    const std::string walkableId = renamed ? "Qw01" : "bwal";
    const std::string pathId = renamed ? "Qp01" : "bptx";
    const std::string rotationId = renamed ? "Qr01" : "bfxr";
    const float degrees = scenario == "rotation-negative" ? -1.0f : scenario == "rotation-zero" ? 0.0f : 90.25f;
    for (auto& member : members) {
        if (member.name == "Units/DestructableData.slk")
            member.data = NativeDoodadQtTable({"destructableID", "code", "walkable", "pathTex", "fixedRot"},
                {{std::string(parentId), std::string(parentId), overrideWalkable ? "0" : "1",
                    overridePath ? "PathTextures/unused-standard-bridge.tga" : "PathTextures/bridge.tga", rectangular ? "270" : "0"}});
        // 实际Destructable metadata没有repeat/data列；scalar地址是明确index=0。
        // 一行改用不同ID及Real，确保保存资格来自本次metadata而非硬编码bwal/bptx/bfxr。
        // 空Original复用旧夹具的单一walkable metadata，不替未使用的path/rotation补资格。
        if (member.name == "Units/DestructableMetaData.slk" && scenario != "empty")
            member.data = NativeDoodadQtTable({"ID", "field", "slk", "type", "index"},
                {{walkableId, "walkable", "DestructableData", "bool", "0"},
                    {pathId, "pathTex", "DestructableData", "pathingTexture", "0"},
                    {rotationId, "fixedRot", "DestructableData", renamed ? "real" : "unreal", "0"}});
        if (member.name == "war3map.doo" && !customId.empty()) {
            auto placements = MapData::ParseDoodadPlacements(member.data);
            if (!placements.Succeeded() || !placements.value) return {};
            placements.value->doodads.front().rawcode = rawcode;
            const auto wire = placements.value->Serialize();
            if (!wire.Succeeded() || !wire.value) return {};
            member.data = *wire.value;
        }
    }
    if (overridePath) {
        const auto texture = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "PathTextures/bridge.tga"; });
        if (texture == members.end()) return {};
        auto unused = texture->data;
        std::fill(unused.begin() + 18, unused.end(), std::byte{}); // 标准全黑，不抑制terrain；有效覆盖的非黑占地有真实差别。
        members.push_back({"PathTextures/unused-standard-bridge.tga", std::move(unused)});
    }
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Destructable);
    if (!native.Succeeded() || !native.value) return {};
    const auto added = customId.empty() ? native.value->AddOriginalObject(rawcode)
        : native.value->AddCustomObject(rawcode, std::string(parentId));
    if (!added.Succeeded()) return {};
    if (overrideWalkable && !native.value->AddModification({ObjectType::Destructable, rawcode}, {walkableId},
        NativeValueType::Int32, std::int32_t{1}).Succeeded()) return {};
    if (overridePath && !native.value->AddModification({ObjectType::Destructable, rawcode}, {pathId},
        NativeValueType::String, std::string("PathTextures/bridge.tga")).Succeeded()) return {};
    if (rectangular && !native.value->AddModification({ObjectType::Destructable, rawcode}, {rotationId},
        renamed ? NativeValueType::Real : NativeValueType::Unreal, degrees).Succeeded()) return {};
    const auto nativeWire = NativeObjectCodec(ObjectType::Destructable).Serialize(*native.value);
    if (!nativeWire.Succeeded() || !nativeWire.value) return {};
    members.push_back({"war3map.w3b", *nativeWire.value});
    std::array<bool, 32 * 32> suppressed{};
    if (rectangular) {
        // 原创顶原点4x2纹理，按文件顺序黑/白/白/白、白/白/黑/白；alpha不授予抑制。
        std::vector<std::byte> texture(18 + 8 * 4, std::byte{});
        texture[2] = std::byte{2}; texture[12] = std::byte{4}; texture[14] = std::byte{2};
        texture[16] = std::byte{32}; texture[17] = std::byte{40};
        constexpr std::array<bool, 8> colored{false, true, true, true, true, true, false, true};
        for (std::size_t i = 0; i < colored.size(); ++i) {
            const auto at = 18 + i * 4;
            if (colored[i]) texture[at] = texture[at + 1] = texture[at + 2] = std::byte{255};
            else texture[at + 3] = std::byte{255};
        }
        for (auto& member : members) {
            if (member.name == "PathTextures/bridge.tga") member.data = texture;
            if (member.name == "war3map.doo") {
                auto placements = MapData::ParseDoodadPlacements(member.data);
                if (!placements.Succeeded() || !placements.value) return {};
                auto& bridge = placements.value->doodads.front(); bridge.position = {512, 512, 0};
                bridge.rotation = 1.57079637f; // wire90先吸附；fixedRot=0不能反过来改变吸附。
                const auto wire = placements.value->Serialize();
                if (!wire.Succeeded() || !wire.value) return {};
                member.data = *wire.value;
            }
        }
        // 独立字面矩阵：wire90吸附后，负值/90.25是(14,16)的4x2，零覆盖是(15,15)的2x4。
        // 标准270为另外的黑格布局；忽略覆盖、把零视为缺失或按最终角重新吸附都不能通过。
        constexpr std::array<bool, 8> zero{false, true, true, true, true, false, true, true};
        constexpr std::array<bool, 8> ninety{true, true, true, false, true, false, true, true};
        const auto& mask = degrees == 0 ? zero : ninety;
        const std::size_t x0 = degrees == 0 ? 15 : 14, y0 = degrees == 0 ? 15 : 16, width = degrees == 0 ? 2 : 4;
        for (std::size_t i = 0; i < mask.size(); ++i) suppressed[(y0 + i / width) * 32 + x0 + i % width] = mask[i];
        std::fill(members[2].data.begin() + 16, members[2].data.end(), std::byte{0x19});
        for (std::size_t i = 0; i < suppressed.size(); ++i) if (suppressed[i]) members[2].data[16 + i] = std::byte{0x11};
        for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < 18; ++x)
            members[2].data[16 + y * 32 + x] |= std::byte{4};
    } else {
        for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 16; x < 20; ++x) suppressed[y * 32 + x] = true;
    }
    fixture.editedPathing = members[2].data;
    // 水深或walkable0贴图仅向中心4x4追加02；桥不能清除普通阻挡04或来源保留位11。
    for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < 18; ++x)
        if (!suppressed[y * 32 + x]) fixture.editedPathing[16 + y * 32 + x] |= std::byte{2};
    return fixture;
}

WalkableNativeQtFixture WalkableCategoryCNativeQtMembers(bool four, bool custom, bool typed, bool turned) {
    using namespace ObjectData;
    const std::string rawcode = custom ? "NC01" : "BC01", parent = custom ? "Br01" : rawcode;
    auto fixture = WalkableDestructableNativeQtMembers("combined", custom ? rawcode : std::string{}, parent);
    auto& members = fixture.members;
    if (members.empty()) return {};
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Destructable);
    if (!native.Succeeded() || !native.value) return {};
    if (!(custom ? native.value->AddCustomObject(rawcode, parent) : native.value->AddOriginalObject(rawcode)).Succeeded()) return {};
    if (typed) {
        if (!native.value->AddModification({ObjectType::Destructable, rawcode}, {"bwal"},
            NativeValueType::Int32, std::int32_t{1}).Succeeded()) return {};
        if (!native.value->AddModification({ObjectType::Destructable, rawcode}, {"bptx"},
            NativeValueType::String, std::string("PathTextures/bridge.tga")).Succeeded()) return {};
        if (!native.value->AddModification({ObjectType::Destructable, rawcode}, {"bfxr"},
            NativeValueType::Unreal, turned ? 90.0f : 0.0f).Succeeded()) return {};
    }
    const auto nativeWire = NativeObjectCodec(ObjectType::Destructable).Serialize(*native.value);
    if (!nativeWire.Succeeded() || !nativeWire.value) return {};
    // 原创top-origin 4x2：黑/红/绿/蓝、蓝/黑/绿/红；非黑alpha0，黑alpha255。
    std::vector<std::byte> texture(18 + 8 * 4, std::byte{});
    texture[2] = std::byte{2}; texture[12] = std::byte{4}; texture[14] = std::byte{2};
    texture[16] = std::byte{32}; texture[17] = std::byte{40};
    constexpr std::array<std::array<unsigned, 3>, 8> colors{{
        {0, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 0, 0},
        {255, 0, 0}, {0, 0, 0}, {0, 255, 0}, {0, 0, 255}}};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        for (std::size_t channel = 0; channel < 3; ++channel)
            texture[18 + i * 4 + channel] = static_cast<std::byte>(colors[i][channel]);
        texture[21 + i * 4] = colors[i] == std::array<unsigned, 3>{0, 0, 0} ? std::byte{255} : std::byte{};
    }
    for (auto& member : members) {
        if (member.name == "Units/DestructableData.slk") {
            // category故意为E；C桥必须用DOO自己的第二字节，不能借可破坏物category或父ID。
            member.data = NativeDoodadQtTable({"destructableID", "code", "walkable", "pathTex", "fixedRot", "category"},
                {{parent, parent, typed ? "0" : "1", typed ? "PathTextures/unused-standard-bridge.tga" : "PathTextures/bridge.tga",
                    typed ? (turned ? "0" : "90") : (turned ? "90" : "0"), "E"}});
        } else if (member.name == "war3map.w3b") member.data = *nativeWire.value;
        else if (member.name == "PathTextures/bridge.tga") member.data = texture;
        else if (member.name == "war3map.doo") {
            auto placements = MapData::ParseDoodadPlacements(member.data);
            if (!placements.Succeeded() || !placements.value || placements.value->doodads.size() != 2) return {};
            auto& bridge = placements.value->doodads.front();
            bridge.rawcode = rawcode; bridge.position = {544, 544, 0}; bridge.rotation = 0.0f;
            const auto wire = placements.value->Serialize();
            if (!wire.Succeeded() || !wire.value) return {};
            member.data = *wire.value;
        } else if (member.name == "UI\\WorldEditData.txt") {
            // 地图含C不授予启动来源身份；实际无C的游戏目录仍必须选择2格。
            member.data = Tests::WaterText("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n"
                "[DoodadCategories]\nC=map-cannot-certify-startup\n");
        }
    }
    // 全矩阵字面oracle：wire0先吸附，fixedRot0/90只旋转图案；不用产品投影/生成器。
    // four仅选测试期望；保存资格必须由调用者创建的真实startup MPQ捕获。
    constexpr std::array<bool, 8> zero{false, true, true, false, true, true, true, true};
    constexpr std::array<bool, 8> ninety{true, true, true, false, true, true, false, true};
    const auto& mask = turned ? ninety : zero;
    const std::size_t width = turned ? 4 : 2;
    const std::size_t x0 = turned ? (four ? 14 : 15) : (four ? 15 : 16), y0 = turned ? 17 : 16;
    std::array<bool, 32 * 32> suppressed{};
    for (std::size_t i = 0; i < mask.size(); ++i) suppressed[(y0 + i / width) * 32 + x0 + i % width] = mask[i];
    auto& oldPathing = members[2].data;
    std::fill(oldPathing.begin() + 16, oldPathing.end(), std::byte{0x19});
    for (std::size_t i = 0; i < suppressed.size(); ++i) if (suppressed[i]) oldPathing[16 + i] = std::byte{0x11};
    for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < 18; ++x)
        oldPathing[16 + y * 32 + x] |= std::byte{4};
    fixture.editedPathing = oldPathing;
    for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < 18; ++x)
        if (!suppressed[y * 32 + x]) fixture.editedPathing[16 + y * 32 + x] |= std::byte{2};
    return fixture;
}

bool SetCustomBridgeQtSelectorMetadata(std::vector<MapArchiveMember>& members, std::string_view scenario) {
    std::vector<std::vector<std::string>> rows{
        {"bwal", "walkable", "DestructableData", "bool", "0", "", ""},
        {"bptx", "pathTex", "DestructableData", "pathingTexture", "0", "", ""},
        {"bfxr", "fixedRot", "DestructableData", "unreal", "0", "", ""}};
    if (scenario == "unused-selectors") {
        rows[1][5] = "Br01"; rows[2][6] = "Nb01";
    } else if (scenario == "unrelated-selector") {
        rows.push_back({"bnam", "Name", "DestructableData", "string", "0", "Z999", ""});
    } else {
        const auto field = scenario.find("walkable") != std::string_view::npos ? 0
            : scenario.find("path") != std::string_view::npos ? 1 : 2;
        const bool excluded = scenario.ends_with("exclude");
        rows[field][excluded ? 6 : 5] = excluded ? "Nb01" : "Br01";
    }
    const auto metadata = std::find_if(members.begin(), members.end(),
        [](const auto& member) { return member.name == "Units/DestructableMetaData.slk"; });
    if (metadata == members.end()) return false;
    metadata->data = NativeDoodadQtTable({"ID", "field", "slk", "type", "index", "useSpecific", "notSpecific"}, rows);
    return true;
}

enum class NativeDoodadQtMutation { None, FollowHeight, ResourceReplacement, SourceDrift };

std::vector<MapArchiveMember> TerrainJournalWaterMembers() {
    auto members = NativePathingQtMembers();
    const auto water = Tests::TerrainWaterMembers();
    for (auto& member : members) {
        if (member.name == "war3map.w3e") member.data = water.front().data;
        if (member.name == "war3map.wpm")
            std::fill(member.data.begin() + 16, member.data.end(), std::byte{0x51});
        if (member.name == "TerrainArt\\CliffTypes.slk") member.data = water[3].data;
        if (member.name == "UI\\MiscData.txt") member.data = Tests::WaterText(
            "[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
            "[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
    }
    // 必需的实际Water资源只放入合成游戏MPQ；缺失/漂移测试不修改工程基线或raw草稿。
    std::erase_if(members, [](const auto& member) { return member.name == "TerrainArt\\Water.slk"; });
    members.push_back({"war3mapUnits.doo", Tests::WaterTake(MapData::UnitPlacements{}.Serialize())});
    return members;
}

std::vector<MapArchiveMember> TerrainJournalWaterGameMembers() {
    auto members = NativePathingQtMembers();
    std::erase_if(members, [](const auto& member) { return member.name != "TerrainArt\\Water.slk"; });
    members.push_back({"fixture.txt", Tests::WaterText("owned journal game resource")});
    return members;
}

Result<Services::TerrainReplayCandidate> ReplayTerrainJournalWater(EditorProject& project) {
    const auto terrain = project.archiveSession_->ReadMember("war3map.w3e");
    if (!terrain.Succeeded()) return {{}, terrain.diagnostics};
    const auto pathing = project.archiveSession_->ReadMember("war3map.wpm");
    if (!pathing.Succeeded()) return {{}, pathing.diagnostics};
    MapData::TerrainStrokeIntent stroke; stroke.points = {{512, 512}};
    stroke.waterBrush = MapData::TerrainWaterStrokeOptions{};
    stroke.waterBrush->kind = MapData::TerrainWaterBrushKind::Deep;
    stroke.waterBrush->shape = MapData::TerrainBrushShape::Square;
    return Services::TerrainPathingSave::ReplayStrokes(*project.archiveSession_, *project.mapDocument_,
        *terrain.value, *pathing.value, std::span(&stroke, 1), project.war3GameRoot_);
}

std::map<std::string, std::vector<std::byte>> TerrainJournalDraftBytes(const MapData::MapDataEditSession& edits) {
    std::map<std::string, std::vector<std::byte>> result;
    for (auto& member : edits.Replacements()) result.emplace(std::move(member.name), std::move(member.data));
    return result;
}

bool BindPreparedTerrainJournal(EditorProject& project, PreparedProjectOpen& candidate,
    const std::filesystem::path& gameRoot) {
    // 本夹具没有物编/触发器覆盖；沿MainWindow的相同顺序安装真实候选与保存身份。
    if (!candidate.map || candidate.recovery || !candidate.mapDataEdits) return false;
    const auto source = candidate.sourceMapMatchesBaseline ? candidate.sourceMapPath : candidate.BaselinePath();
    if (!BindTerrainSaveProject(project, source)) return false;
    const auto originalMapScopes = project.editRevision_.Current();
    project.war3GameRoot_ = gameRoot;
    project.recordedSourceMapPath_ = candidate.sourceMapPath;
    project.restoredFiles_ = std::move(candidate.extraction);
    project.mapEdits_ = std::move(candidate.mapDataEdits);
    project.terrainJournalWarning_ = candidate.terrainJournalWarning;
    if (project.mapDataUndoStack_) for (auto& receipt : candidate.terrainReceipts)
        project.mapDataUndoStack_->push(new MapDataEditCommand(project, std::move(receipt), "Restored water", "恢复工程水刷"));
    project.lastProjectFingerprint_ = project.ProjectFingerprint();
    project.projectFilePath_ = candidate.sourcePath; project.projectFileSha256_ = candidate.sourceSha256;
    const auto scopes = project.CaptureEditScopes();
    if (!scopes) return false;
    project.editTrackingReady_ = project.editRevision_.Reset(*scopes, *scopes, originalMapScopes).Succeeded();
    project.editTrackingFailed_ = !project.editTrackingReady_;
    project.UpdateEditDirtyFlags();
    return project.editTrackingReady_;
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
    void SourceWatchIgnoresUnrelatedSiblingFiles();
    void SourceIdentityMismatchReturnsDiagnostics();
    void SaveAndRecoveryWorkWithoutUndoWidgets();
    void PreparedOpenPreservesSessionAndCleansCancelledFiles();
    void MapDataOverlaySurvivesProjectSaveAndPreflight();
    void TerrainJournalProjectRoundtripAndStrictManifest();
    void TerrainJournalEqualOutputStillTracksUndoPrefix();
    void TerrainJournalWaterFallbackKeepsRawAndWarning_data();
    void TerrainJournalWaterFallbackKeepsRawAndWarning();
    void TerrainJournalProjectSavesKeepCoreMapBaseline();
    void TerrainJournalEqualOutputMapSaveClearsHistoryDirty();
    void TerrainJournalRestoredWaterSavesAndReadsBack_data();
    void TerrainJournalRestoredWaterSavesAndReadsBack();
    void PaletteWaterProjectColdRestoreAndNativeSave_data();
    void PaletteWaterProjectColdRestoreAndNativeSave();
    void GroundPaletteAppendWaterProjectColdRestoreAndNativeSave_data();
    void GroundPaletteAppendWaterProjectColdRestoreAndNativeSave();
    void CliffGroundPaletteProjectColdRestoreAndNativeSave_data();
    void CliffGroundPaletteProjectColdRestoreAndNativeSave();
    void OrdinaryDoodadProjectRawColdRestoreAndNativeSave_data();
    void OrdinaryDoodadProjectRawColdRestoreAndNativeSave();
    void OrdinaryWaterJournalProjectColdRestoreAndNativeSave_data();
    void OrdinaryWaterJournalProjectColdRestoreAndNativeSave();
    void UnitsWaterJournalProjectColdRestoreAndNativeSave_data();
    void UnitsWaterJournalProjectColdRestoreAndNativeSave();
    void StartLocationsWaterJournalProjectColdRestoreAndNativeSave_data();
    void StartLocationsWaterJournalProjectColdRestoreAndNativeSave();
    void StartLocationsDescriptionsProjectColdRestore_data();
    void StartLocationsDescriptionsProjectColdRestore();
    void CornerRampProjectColdRestoreAndNativeSave_data();
    void CornerRampProjectColdRestoreAndNativeSave();
    void StraightRampProjectColdRestoreAndNativeSave_data();
    void StraightRampProjectColdRestoreAndNativeSave();
    void DualStageStaticPathingSaveRebase_data();
    void DualStageStaticPathingSaveRebase();
    void DualStageStaticPathingProjectColdRestore_data();
    void DualStageStaticPathingProjectColdRestore();
    void DualStageStaticPathingWithoutTerrainNet_data();
    void DualStageStaticPathingWithoutTerrainNet();
    void DualStageUnrelatedEditsPreserveUnresolvedPathing_data();
    void DualStageUnrelatedEditsPreserveUnresolvedPathing();
    void PlayableBoundsProjectColdRestoreAndNativeSave_data();
    void PlayableBoundsProjectColdRestoreAndNativeSave();
    void GeneratedObjectsSurviveCommitAndSecondSave();
    void NativeCloneOriginsSurviveProjectRestore();
    void PendingNativeCloneOriginsSurviveMissingOrEmptyMember();
    void SaveCommitRejectsRevisionDrift();
    void TerrainSaveReadbackGatesCommit_data();
    void TerrainSaveReadbackGatesCommit();
    void PathingIntentNativeSaveKeepsProjectState_data();
    void PathingIntentNativeSaveKeepsProjectState();
    void PlacementHeightAndSavedPathingWorkerRoundtrip();
    void NativePathingWorkerRebuildsAndRebinds_data();
    void NativePathingWorkerRebuildsAndRebinds();
    void NativePathingRemoveWaterProjectRoundtripAndSave_data();
    void NativePathingRemoveWaterProjectRoundtripAndSave();
    void TerrainUnitPlacementNativeSave_data();
    void TerrainUnitPlacementNativeSave();
    void BoundaryNativeSave_data();
    void BoundaryNativeSave();
    void NativePathingWorkerPreflightAndFailure_data();
    void NativePathingWorkerPreflightAndFailure();
    void CGridInvariantNormalSaveAndUndoRedo_data();
    void CGridInvariantNormalSaveAndUndoRedo();
    void CGridStartupDirectoryNormalSaveAndUndoRedo_data();
    void CGridStartupDirectoryNormalSaveAndUndoRedo();
    void CGridVariantSourceUsesActualDirectory_data();
    void CGridVariantSourceUsesActualDirectory();
    void CGridStartupDirectoryDriftRejectsBeforeWrite_data();
    void CGridStartupDirectoryDriftRejectsBeforeWrite();
    void CGridStartupDirectoryUnavailableRejectsBeforeWrite_data();
    void CGridStartupDirectoryUnavailableRejectsBeforeWrite();
    void EffectiveDoodadCategorySaveAndUndoRedo_data();
    void EffectiveDoodadCategorySaveAndUndoRedo();
    void EffectiveDoodadCategoryWorkerRejectsBeforeWrite_data();
    void EffectiveDoodadCategoryWorkerRejectsBeforeWrite();
    void EffectiveDoodadCategorySourceDriftRejectsBeforeWrite_data();
    void EffectiveDoodadCategorySourceDriftRejectsBeforeWrite();
    void FixedRotationNormalSaveAndUndoRedo_data();
    void FixedRotationNormalSaveAndUndoRedo();
    void FixedRotationWorkerRejectsBeforeWrite_data();
    void FixedRotationWorkerRejectsBeforeWrite();
    void WalkableDestructableNormalSaveAndUndoRedo_data();
    void WalkableDestructableNormalSaveAndUndoRedo();
    void WalkableDestructableOriginalSaveAndUndoRedo_data();
    void WalkableDestructableOriginalSaveAndUndoRedo();
    void WalkableDestructableOriginalWorkerRejectsBeforeWrite_data();
    void WalkableDestructableOriginalWorkerRejectsBeforeWrite();
    void WalkableDestructableCustomSaveAndUndoRedo_data();
    void WalkableDestructableCustomSaveAndUndoRedo();
    void WalkableDestructableCustomWorkerRejectsBeforeWrite_data();
    void WalkableDestructableCustomWorkerRejectsBeforeWrite();
    void WalkableCategoryCNormalSaveAndUndoRedo_data();
    void WalkableCategoryCNormalSaveAndUndoRedo();
    void WalkableCategoryCUnavailableRejectsBeforeWrite_data();
    void WalkableCategoryCUnavailableRejectsBeforeWrite();
    void WalkableCategoryCSourceDriftRejectsBeforeWrite_data();
    void WalkableCategoryCSourceDriftRejectsBeforeWrite();
    void WalkableCategoryCOldGridRejectsBeforeWrite_data();
    void WalkableCategoryCOldGridRejectsBeforeWrite();
    void WalkableDestructableWorkerRejectsBeforeWrite_data();
    void WalkableDestructableWorkerRejectsBeforeWrite();
    void NativeObjectCatalogReadLeaseBlocksSourceWrite();
    void PathingIntentLateEditStalesFrozenProject();
    void LegacyPathingIntentProjectKeepsStrictCompatibility();
    void TerrainSavePrepareStillRequiresToolchain();
    void TerrainShadowWorkerExplicitRequest_data();
    void TerrainShadowWorkerExplicitRequest();
    void TerrainShadowWorkerOrdinarySavePreserves_data();
    void TerrainShadowWorkerOrdinarySavePreserves();
    void TerrainShadowWorkerFailureCannotUseMinimapFallback_data();
    void TerrainShadowWorkerFailureCannotUseMinimapFallback();
    void AutomaticMinimapDefaultPrepareAndSaveCopy_data();
    void AutomaticMinimapDefaultPrepareAndSaveCopy();
    void AutomaticMinimapFinalSettingsDerivedWorkerAndCommit_data();
    void AutomaticMinimapFinalSettingsDerivedWorkerAndCommit();
    void MissingMinimapWorkerReadbackGatesCommit_data();
    void MissingMinimapWorkerReadbackGatesCommit();
    void MissingMinimapPrepareKeepsDefaultAndRejectsMixedChanges();
    void MissingMinimapWorkerFailureConsumesTask();
    void DeclaredMissingMinimapWorkerReadbackGatesCommit_data();
    void DeclaredMissingMinimapWorkerReadbackGatesCommit();
    void DeclaredMissingMinimapPrepareKeepsScopesAndOriginalGates();
    void DeclaredMissingMinimapWorkerFailureConsumesTask_data();
    void DeclaredMissingMinimapWorkerFailureConsumesTask();
    void BoundedAssetsPreserveCurrentPrefix();
    void ObjectQueriesDistinguishMissingFromUnparsedSources();
    void TriggerChangesCannotSkipUnpreparedFramework();
    void OwnedLuaProjectRecoveryPreservesPayloadRoleAndMapGate();
    void UnlistedTerrainReadPreservesNamedInventory();
    void UnlistedTerrainReadRejectsChangedSource();
    void UnlistedTerrainReadPreservesDecodeAndBudgetErrors();
    void ProjectRestoreReadsUnlistedTerrainAndPathing_data();
    void ProjectRestoreReadsUnlistedTerrainAndPathing();
    void ProjectRestoreKeepsMissingAndUnreadableTerrainDistinct();
private:
    void CheckNativeDoodadNormalSaveAndUndoRedo(const std::vector<MapArchiveMember>& members, bool inPlace,
        bool textureChange = false, const std::vector<std::byte>* expectedEditedPathing = nullptr,
        const std::filesystem::path& gameRoot = {});
    void CheckNativeDoodadWorkerRejectsBeforeWrite(const std::vector<MapArchiveMember>& members, bool inPlace,
        const char* expectedCode, NativeDoodadQtMutation mutation = NativeDoodadQtMutation::None,
        const std::filesystem::path& gameRoot = {}, const std::function<Status()>& beforePublish = {});
};

#include "EditorProjectOrdinaryDoodadRecoveryCases.h"
#include "TerrainOrdinaryJournalProjectCases.h"
#include "TerrainUnitsJournalProjectCases.h"
#include "TerrainStartLocationsProjectCases.h"
#include "TerrainGroundPaletteAppendProjectCases.h"
#include "TerrainCliffPaletteAppendProjectCases.h"
#include "EditorProjectRemoveWaterCases.h"
#include "TerrainStraightRampProjectCases.h"
#include "TerrainDualStagePathingProjectCases.h"

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

void EditorProjectTests::ProjectRestoreReadsUnlistedTerrainAndPathing_data() {
    QTest::addColumn<bool>("listTerrain"); QTest::addColumn<bool>("listPathing"); QTest::addColumn<bool>("draft");
    QTest::addColumn<bool>("terrainOverlay");
    QTest::newRow("unlisted-w3e-clean") << false << true << false << false;
    QTest::newRow("unlisted-w3e-apr") << false << true << true << false;
    QTest::newRow("unlisted-w3e-terrain") << false << true << true << true;
    QTest::newRow("unlisted-wpm-clean") << true << false << false << false;
    QTest::newRow("unlisted-wpm-apr") << true << false << true << false;
    QTest::newRow("unlisted-wpm-terrain") << true << false << true << true;
    QTest::newRow("both-unlisted-clean") << false << false << false << false;
    QTest::newRow("both-unlisted-apr") << false << false << true << false;
    QTest::newRow("both-unlisted-terrain") << false << false << true << true;
}

void EditorProjectTests::ProjectRestoreReadsUnlistedTerrainAndPathing() {
    QFETCH(bool, listTerrain); QFETCH(bool, listPathing); QFETCH(bool, draft); QFETCH(bool, terrainOverlay);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto terrain = UnlistedTerrainBytes();
    MapDataFixtures::Fixture encodedPathing; encodedPathing.Raw("MP3W"); encodedPathing.UInt(0); encodedPathing.UInt(4); encodedPathing.UInt(4);
    encodedPathing.bytes.resize(32, std::byte{0x82}); const auto& pathing = encodedPathing.bytes;
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    QVERIFY(WriteUnlistedTerrain(QString::fromStdWString(source.wstring()),terrain,0,0,&pathing,listTerrain,listPathing));
    auto opened = MapArchiveSession::Open(source); QVERIFY(opened.Succeeded());
    EditorProject project; project.sourceArchivePath_ = source;
    const auto workspace = root.filePath("workspace"); QVERIFY(QDir().mkpath(workspace));
    project.projectRoot_ = std::filesystem::path(workspace.toStdWString());
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    const auto sourceSha = project.archiveSession_->SourceSha256(); const auto namedCount = project.archiveSession_->Members().size();
    const auto opaqueCount = project.archiveSession_->OpaqueMemberCount();
    const auto list = project.archiveSession_->ReadMember("(listfile)"); QVERIFY(list.Succeeded());
    QCOMPARE(project.archiveSession_->HasMember("war3map.w3e"),listTerrain);
    QCOMPARE(project.archiveSession_->HasMember("war3map.wpm"),listPathing);
    project.LoadMapDocument(); QVERIFY(project.mapEdits_ && project.mapEdits_->Terrain() && project.mapEdits_->HasTerrainPathing());
    project.ResetEditTracking(); const MapData::TerrainStrokePoint point{-80,-80};
    if (draft) {
        if (terrainOverlay) {
            auto next = *project.mapEdits_->Terrain(); next.vertices[0].groundHeightRaw += 16;
            MapData::RefreshTerrainDerived(next,0,0,2,2);
            const auto bytes = MapData::SerializeMapTerrain(next); QVERIFY(bytes.Succeeded());
            QVERIFY(project.mapEdits_->SetTerrain(*bytes.value).Succeeded());
        } else {
            MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::PathingRestriction;
            brush.size = 1; brush.pathingRestrictions = 0x04; brush.value = true;
            QVERIFY(project.mapEdits_->EditTerrain(brush,std::span(&point,1)).Succeeded());
        }
        QVERIFY(project.ObserveMapDataEdit("unlisted terrain/pathing project recovery"));
        QCOMPARE(project.mapEdits_->VerifyTerrainPathingForSave().Succeeded(), !terrainOverlay);
    }
    const auto expectedTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(expectedTerrain.Succeeded());
    const auto expectedDraft = project.mapEdits_->SerializePathingRestrictionDraft(); QVERIFY(expectedDraft.Succeeded());
    const auto target = std::filesystem::path(root.filePath("reopen.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(),saved.diagnostics.empty() ? "project save" : saved.diagnostics.front().message.c_str());
    auto prepared = PrepareProjectOpen(target);
    QVERIFY2(prepared.Succeeded(),prepared.diagnostics.empty() ? "project reopen" : prepared.diagnostics.front().message.c_str());
    QVERIFY((*prepared.value)->mapDataEdits);
    auto& edits = *(*prepared.value)->mapDataEdits;
    QVERIFY(edits.Terrain() && edits.HasTerrainPathing()); QCOMPARE(edits.IsDirty(),draft);
    QCOMPARE(MapData::SerializeMapTerrain(*edits.Terrain()).value,expectedTerrain.value);
    QCOMPARE(edits.SerializePathingRestrictionDraft().value,expectedDraft.value);
    const auto sample = edits.InspectTerrainPoint(point); QVERIFY(sample.Succeeded());
    const auto expectedFlags = draft && !terrainOverlay ? 0x86 : 0x82;
    QCOMPARE(sample.value->pathingFlags,expectedFlags); QCOMPARE(sample.value->originalPathingFlags,0x82);
    if (draft) {
        QCOMPARE(edits.VerifyTerrainPathingForSave().Succeeded(), !terrainOverlay);
        QVERIFY(edits.Undo() && !edits.IsDirty()); QCOMPARE(edits.InspectTerrainPoint(point).value->pathingFlags,0x82);
        QVERIFY(edits.Redo() && edits.IsDirty()); QCOMPARE(edits.InspectTerrainPoint(point).value->pathingFlags,expectedFlags);
        QCOMPARE(edits.VerifyTerrainPathingForSave().Succeeded(), !terrainOverlay);
    } else QVERIFY(edits.Replacements().empty() && !edits.CanUndo() && edits.VerifyTerrainPathingForSave().Succeeded());
    auto baseline = MapArchiveSession::Open((*prepared.value)->BaselinePath()); QVERIFY(baseline.Succeeded());
    QCOMPARE(baseline.value->SourceSha256(),sourceSha); QCOMPARE(baseline.value->Members().size(),namedCount);
    QCOMPARE(baseline.value->OpaqueMemberCount(),opaqueCount); QCOMPARE(baseline.value->ReadMember("(listfile)").value,list.value);
    QCOMPARE(baseline.value->HasMember("war3map.w3e"),listTerrain); QCOMPARE(baseline.value->HasMember("war3map.wpm"),listPathing);
    QCOMPARE(project.archiveSession_->Members().size(),namedCount); QCOMPARE(project.archiveSession_->OpaqueMemberCount(),opaqueCount);
    QCOMPARE(Sha256File(source).value,std::optional{sourceSha});
}

void EditorProjectTests::ProjectRestoreKeepsMissingAndUnreadableTerrainDistinct() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto bytes = [](const std::string& text) { const auto span = std::as_bytes(std::span(text)); return std::vector<std::byte>(span.begin(),span.end()); };
    const auto manifest = bytes("{\"format\":\"forge-project\",\"version\":1,\"sourceKind\":\"archive\"}");
    for (const auto mode : {"missing-w3e","missing-wpm","unsupported","over-budget"}) {
        const auto source = std::filesystem::path(root.filePath(QString::fromLatin1(mode)+".w3x").toStdWString());
        if (std::string_view(mode) == "missing-w3e") {
            QVERIFY(WriteVerifiedArchive(source,std::vector<MapArchiveMember>{{"known.bin",{std::byte{1}}}}).Succeeded());
        } else if (std::string_view(mode) == "missing-wpm") {
            QVERIFY(WriteVerifiedArchive(source,std::vector<MapArchiveMember>{{"war3map.w3e",UnlistedTerrainBytes()}}).Succeeded());
        } else QVERIFY(WriteUnlistedTerrain(QString::fromStdWString(source.wstring()),UnlistedTerrainBytes(),
            std::string_view(mode) == "unsupported" ? Mpq::kFilePatch : Mpq::kFileCompress,
            std::string_view(mode) == "over-budget" ? 64u*1024u*1024u+1u : 0u));
        QFile original(QString::fromStdWString(source.wstring())); QVERIFY(original.open(QIODevice::ReadOnly)); const auto raw = original.readAll(); original.close();
        std::vector<std::byte> baseline(static_cast<std::size_t>(raw.size())); std::memcpy(baseline.data(),raw.constData(),baseline.size());
        const auto projectPath = std::filesystem::path(root.filePath(QString::fromLatin1(mode)+".forge").toStdWString());
        std::vector<MapArchiveMember> members{{"project.json",manifest},{"baseline/source.w3x",baseline}};
        if (std::string_view(mode) == "missing-w3e") members.push_back({"overlay/map/war3map.w3e",UnlistedTerrainBytes()});
        if (std::string_view(mode) == "missing-wpm") members.push_back({"overlay/map/war3map.wpm",{std::byte{1}}});
        QVERIFY(WriteVerifiedArchive(projectPath,members).Succeeded());
        const auto rejected = PrepareProjectOpen(projectPath); QVERIFY(!rejected.Succeeded() && !rejected.value);
        if (std::string_view(mode) == "unsupported" || std::string_view(mode) == "over-budget") {
            QVERIFY(HasReadDiagnostic(rejected,std::string_view(mode) == "unsupported" ? "EXACT_MEMBER_ENCODING_UNSUPPORTED" : "FILE_READ_BUDGET_EXCEEDED"));
            QVERIFY(!HasReadDiagnostic(rejected,"TERRAIN_ABSENT") && !HasReadDiagnostic(rejected,"PATHING_ABSENT"));
        } else {
            QVERIFY(HasReadDiagnostic(rejected,"PROJECT_OPEN_FAILED"));
            QVERIFY(rejected.diagnostics.front().message.find(std::string_view(mode) == "missing-w3e" ? "源 W3E" : "源 WPM") != std::string::npos);
            members.resize(2); const auto clean = std::filesystem::path(root.filePath(QString::fromLatin1(mode)+"-clean.forge").toStdWString());
            QVERIFY(WriteVerifiedArchive(clean,members).Succeeded()); auto restored = PrepareProjectOpen(clean); QVERIFY(restored.Succeeded());
            QCOMPARE((*restored.value)->mapDataEdits->Terrain() != nullptr,std::string_view(mode) != "missing-w3e");
            QVERIFY(!(*restored.value)->mapDataEdits->HasTerrainPathing() && !(*restored.value)->mapDataEdits->IsDirty());
        }
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

void EditorProjectTests::TerrainSaveReadbackGatesCommit_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::addColumn<QString>("mode");
    QTest::newRow("matching-copy") << false << QStringLiteral("match");
    QTest::newRow("matching-in-place") << true << QStringLiteral("match");
    QTest::newRow("throwing-readback-progress-in-place") << true << QStringLiteral("progress");
    QTest::newRow("mismatched-plan-copy") << false << QStringLiteral("mismatch");
    QTest::newRow("mismatched-plan-in-place") << true << QStringLiteral("mismatch");
    QTest::newRow("missing-copy-receipt") << false << QStringLiteral("missing");
    QTest::newRow("wrong-copy-output-sha") << false << QStringLiteral("stale");
    QTest::newRow("repeat-in-place-run") << true << QStringLiteral("repeat");
    QTest::newRow("new-edit-after-readback") << false << QStringLiteral("revision");
}

void EditorProjectTests::TerrainSaveReadbackGatesCommit() {
    QFETCH(bool, inPlace); QFETCH(QString, mode);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto members = TerrainSaveMembers(); QVERIFY(!members.empty());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto originalSha = Sha256File(source); QVERIFY(originalSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    QVERIFY(RaiseTerrainSaveProject(project));
    project.mapDataUndoStack_->push(new QUndoCommand("terrain save undo marker"));
    const auto undoCount = project.mapDataUndoStack_->count();
    const auto* originalSession = project.archiveSession_.get();
    const auto target = std::filesystem::path(root.filePath("candidate.w3x").toStdWString());
    auto prepared = MapData::TerrainSavePlan::Prepare(*project.archiveSession_, *project.mapEdits_); QVERIFY(prepared.Succeeded());
    const auto originalPlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*prepared.value));
    // 沿已有内部job夹具验证真实Run/回读/Commit；不伪装成通过生产Prepare脚本门。
    auto job = std::make_unique<MapSaveJob>();
    QVERIFY(job->FreezeArchive(*project.archiveSession_).Succeeded());
    job->target = inPlace ? source : target; job->inPlace = inPlace;
    if (inPlace) {
        job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath("backups").toStdWString());
        QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        job->inPlaceOptions.keepBackups = 0;
    }
    job->replacements = project.mapEdits_->Replacements(); job->removals = project.mapEdits_->Removals();
    job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(),
        project.editRevision_.Current()};
    job->terrainSavePlan = originalPlan;
    // 本夹具专门覆盖已冻结计划的Run/回读/Commit边界，不重跑派生准备。
    // 普通保存的图标重建另在worker集成用例中覆盖。
    job->derivedPreparationInstalled = true;
    const auto& diagnostics = originalPlan->Diagnostics();
    job->diagnostics.insert(job->diagnostics.end(), diagnostics.begin(), diagnostics.end());
    QVERIFY(originalPlan->VerifyForSave(*project.mapEdits_, job->sourceSha256, job->replacements, job->removals).Succeeded());
    const auto* task = job.get();
    job->beforePublish = [originalPlan, task] {
        return originalPlan->VerifyFrozenOutput(task->sourceSha256, task->replacements, task->removals);
    };
    QVERIFY(!job->script && !job->terrainReadbackStatus);
    QVERIFY(job->terrainSavePlan->SaveGate().Succeeded() && job->terrainSavePlan->RequiresNativeVerification());
    if (mode == QStringLiteral("mismatch")) {
        auto alternateMembers = members;
        for (auto& member : alternateMembers) if (member.name == "war3map.shd") member.data = {std::byte{255}, std::byte{255}};
        const auto alternate = std::filesystem::path(root.filePath("different-shadow.w3x").toStdWString());
        QVERIFY(WriteVerifiedArchive(alternate, alternateMembers).Succeeded());
        EditorProject other; QVERIFY(BindTerrainSaveProject(other, alternate)); QVERIFY(RaiseTerrainSaveProject(other));
        auto different = MapData::TerrainSavePlan::Prepare(*other.archiveSession_, *other.mapEdits_); QVERIFY(different.Succeeded());
        // 仅通过已有可见job字段注入另一冻结计划；writer闭包仍持原计划，不改真实保存文件。
        job->terrainSavePlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*different.value));
    }
    bool progressFaultReached = false;
    if (mode == QStringLiteral("progress")) job->progress = [&](const QString& text) {
        if (text == EditorProject::tr("正在核对保存后的地形…")) {
            progressFaultReached = true;
            throw std::runtime_error("readback progress display fixture");
        }
    };
    EditorProject::RunMapSave(*job);
    if (mode == QStringLiteral("progress")) QVERIFY(progressFaultReached);
    QVERIFY(job->executed && job->nextSession && std::filesystem::exists(job->written));
    QVERIFY(job->terrainReadbackStatus);
    QCOMPARE(job->terrainReadbackArchiveSha256, job->nextSession->SourceSha256());
    if (mode == QStringLiteral("mismatch")) {
        QVERIFY(!job->succeeded && !job->terrainReadbackStatus->Succeeded());
        QVERIFY(HasReadDiagnostic(*job->terrainReadbackStatus, "TERRAIN_SAVE_PLAN_MEMBER_MISMATCH"));
    } else {
        QVERIFY(job->succeeded && job->terrainReadbackStatus->Succeeded());
    }
    QString expectedCode = QStringLiteral("TERRAIN_SAVE_PLAN_READBACK_REJECTED");
    if (mode == QStringLiteral("missing")) {
        job->terrainReadbackStatus.reset(); expectedCode = QStringLiteral("TERRAIN_SAVE_PLAN_READBACK_MISSING");
    } else if (mode == QStringLiteral("stale")) {
        job->terrainReadbackArchiveSha256.assign(64, '0'); expectedCode = QStringLiteral("TERRAIN_SAVE_PLAN_READBACK_STALE");
    } else if (mode == QStringLiteral("repeat")) {
        EditorProject::RunMapSave(*job);
        QVERIFY(!job->succeeded && !job->terrainReadbackStatus && job->terrainReadbackArchiveSha256.empty());
        QVERIFY(HasReadDiagnostic(*job, "SAVE_TASK_ALREADY_EXECUTED"));
        expectedCode = QStringLiteral("TERRAIN_SAVE_PLAN_READBACK_MISSING");
    } else if (mode == QStringLiteral("revision")) {
        QVERIFY(RaiseTerrainSaveProject(project, 1)); expectedCode = QStringLiteral("SAVE_REVISION_CHANGED");
    }
    const auto currentScopes = project.editRevision_.Current();
    if (mode == QStringLiteral("match") || mode == QStringLiteral("progress")) {
        const auto savedSha = job->terrainReadbackArchiveSha256;
        QVERIFY(project.CommitMapSave(*job).has_value());
        QCOMPARE(project.archiveSession_->SourceSha256(), savedSha);
        QVERIFY(!project.mapEdits_->IsDirty() && !project.HasUnsavedMapChanges());
        QVERIFY(ModelHasCode(*project.diagnosticsModel_, QStringLiteral("TERRAIN_SAVE_SHADOW_NOT_REBUILT")));
        auto saved = MapArchiveSession::Open(project.sourceArchivePath_); QVERIFY(saved.Succeeded());
        QVERIFY(job->terrainSavePlan->VerifyReadback(*saved.value).Succeeded());
    } else {
        QVERIFY(!project.CommitMapSave(*job).has_value());
        QVERIFY(ModelHasCode(*project.diagnosticsModel_, expectedCode));
        QVERIFY(project.archiveSession_.get() == originalSession && project.sourceArchivePath_ == source);
        QVERIFY(project.mapEdits_->IsDirty() && project.HasUnsavedMapChanges());
        QCOMPARE(project.editRevision_.Current(), currentScopes);
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        QCOMPARE(project.mapDataUndoStack_->count(), undoCount);
        if (inPlace) QVERIFY(project.sourceChangeSuspected_ && project.sourceChangeConfirmed_);
        else QVERIFY(!project.sourceChangeConfirmed_);
    }
    if (inPlace) {
        QVERIFY(!job->backup.empty() && std::filesystem::exists(job->backup));
        QCOMPARE(Sha256File(job->backup).value, originalSha.value);
    } else QCOMPARE(Sha256File(source).value, originalSha.value);
}

void EditorProjectTests::TerrainSavePrepareStillRequiresToolchain() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto members = TerrainSaveMembers(); QVERIFY(!members.empty());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("blocked.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto originalSha = Sha256File(source); QVERIFY(originalSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); QVERIFY(RaiseTerrainSaveProject(project));
    project.compilerDataRoot_.clear(); project.activeUiDataRoot_.clear(); project.uiDataRoot_.clear();
    const auto scopes = project.editRevision_.Current();
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map);
    QVERIFY(baseline);
    QVERIFY(!project.PrepareMapSave(target, false));
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, QStringLiteral("REPACK_SCRIPT_REJECTED")));
    QVERIFY(!std::filesystem::exists(target) && project.HasUnsavedMapChanges());
    QCOMPARE(project.editRevision_.Current(), scopes);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
    QCOMPARE(Sha256File(source).value, originalSha.value);
}

void EditorProjectTests::PathingIntentNativeSaveKeepsProjectState_data() {
    QTest::addColumn<bool>("inPlace"); QTest::addColumn<bool>("lateEdit"); QTest::addColumn<bool>("blight");
    QTest::addColumn<int>("restriction"); QTest::addColumn<bool>("failPublish");
    for (const int flags : {2, 4}) for (const bool inPlace : {false, true}) {
        const auto row = QByteArray::number(flags) + (inPlace ? "-in-place" : "-copy");
        QTest::newRow(row.constData()) << inPlace << false << false << flags << false;
        QTest::newRow((row + "-late").constData()) << inPlace << true << false << flags << false;
        QTest::newRow((row + "-blight").constData()) << inPlace << false << true << flags << false;
        QTest::newRow((row + "-failure").constData()) << inPlace << false << false << flags << true;
    }
}

void EditorProjectTests::PathingIntentNativeSaveKeepsProjectState() {
    QFETCH(bool, inPlace); QFETCH(bool, lateEdit); QFETCH(bool, blight);
    QFETCH(int, restriction); QFETCH(bool, failPublish);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto members = TerrainSaveMembers(); QVERIFY(!members.empty());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto workspace = root.filePath("workspace"); QVERIFY(QDir().mkpath(workspace));
    project.projectRoot_ = std::filesystem::path(workspace.toStdWString());
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto sourceRevision = project.mapEdits_->TerrainSourceRevision();
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::PathingRestriction;
    brush.size = 1; brush.pathingRestrictions = static_cast<std::uint8_t>(restriction);
    const MapData::TerrainStrokePoint point{48, 48}; // 源WPM为0x82：2只改A，4同时改变WPM。
    QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    if (blight) {
        MapData::TerrainBrush terrainBrush; terrainBrush.kind = MapData::TerrainBrushKind::Blight; terrainBrush.size = 1;
        const MapData::TerrainStrokePoint vertex{256, 256};
        QVERIFY(project.mapEdits_->EditTerrain(terrainBrush, std::span(&vertex, 1), false).Succeeded());
    }
    QVERIFY(project.ObserveMapDataEdit("仅限制意图保存夹具"));
    const auto declaration = project.mapEdits_->SerializePathingRestrictionDraft(); QVERIFY(declaration.Succeeded());
    QVERIFY(!declaration.value->empty()); QCOMPARE(project.mapEdits_->Replacements().empty(), !blight && restriction == 2);
    QVERIFY(project.HasUnsavedMapChanges() && project.editRevision_.IsDirty(EditSaveTarget::Project));
    const auto fingerprint = project.ProjectFingerprint(); QVERIFY(!fingerprint.empty());
    auto prepared = MapData::TerrainSavePlan::Prepare(*project.archiveSession_, *project.mapEdits_); QVERIFY(prepared.Succeeded());
    auto plan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*prepared.value));
    QVERIFY(plan->SaveGate().Succeeded());
    // 与已有保存夹具相同：实际Run/回读/Commit，生产Prepare的脚本工具链仍由原门禁负责。
    auto job = std::make_unique<MapSaveJob>(); QVERIFY(job->FreezeArchive(*project.archiveSession_).Succeeded());
    job->target = inPlace ? source : std::filesystem::path(root.filePath("saved.w3x").toStdWString());
    job->inPlace = inPlace;
    if (inPlace) {
        job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath("backups").toStdWString());
        QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory)); job->inPlaceOptions.keepBackups = 0;
    }
    job->replacements = project.mapEdits_->Replacements(); job->removals = project.mapEdits_->Removals();
    job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(), project.editRevision_.Current()};
    job->terrainSavePlan = plan;
    job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
    job->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>(*declaration.value);
    const auto* task = job.get();
    job->beforePublish = [plan, task] { return plan->VerifyFrozenOutput(task->sourceSha256, task->replacements, task->removals); };
    if (failPublish) job->beforePublish = [] { return Status::Failure("Test", "REFUSED", "拒绝发布夹具"); };
    if (lateEdit) {
        const MapData::TerrainStrokePoint second{80, 48};
        QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&second, 1), false).Succeeded());
        QVERIFY(project.ProjectFingerprint() != fingerprint); // 未先通知observer也必须看见实际A。
    }
    EditorProject::RunMapSave(*job);
    if (failPublish) {
        QVERIFY(!job->succeeded && !project.CommitMapSave(*job));
        QCOMPARE(project.mapEdits_->SerializePathingRestrictionDraft().value, declaration.value);
        QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision);
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        QVERIFY(project.mapEdits_->CanUndo() && project.HasUnsavedMapChanges());
        QCOMPARE(Sha256File(source).value, sourceSha.value);
        return;
    }
    QVERIFY(job->succeeded && job->nextSession && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    if (lateEdit) {
        QVERIFY(!project.CommitMapSave(*job).has_value());
        QVERIFY(ModelHasCode(*project.diagnosticsModel_, QStringLiteral("SAVE_REVISION_CHANGED")));
        QVERIFY(project.HasUnsavedMapChanges() && project.mapEdits_->IsDirty());
        QCOMPARE(project.mapEdits_->QueryPathingRestrictions().value->totalCells, std::size_t{2});
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        return;
    }
    QVERIFY(project.CommitMapSave(*job).has_value());
    QVERIFY(!project.mapEdits_->IsDirty() && !project.HasUnsavedMapChanges());
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project));
    QVERIFY(project.mapEdits_->TerrainSourceRevision() > sourceRevision);
    const auto savedDeclaration = project.mapEdits_->SerializePathingRestrictionDraft(); QVERIFY(savedDeclaration.Succeeded());
    QVERIFY(savedDeclaration.value != declaration.value && (*savedDeclaration.value)[3] == std::byte{3});
    QCOMPARE(project.mapEdits_->InspectTerrainPoint(point).value->pathingFlags, 0x82 | restriction);
    if (blight) {
        QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("保存后撤销荒芜"));
        QCOMPARE(project.mapEdits_->QueryPathingRestrictions().value->totalCells, std::size_t{1});
    }
    QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("保存后撤销意图"));
    QVERIFY(project.HasUnsavedMapChanges() && !project.mapEdits_->SerializePathingRestrictionDraft().value->empty());
    QCOMPARE(project.mapEdits_->QueryPathingRestrictions().value->totalCells, std::size_t{0});
    QCOMPARE(project.mapEdits_->InspectTerrainPoint(point).value->pathingFlags, 0x82);
    QVERIFY(project.mapEdits_->Redo() && project.ObserveMapDataEdit("保存后重做意图"));
    if (blight) QVERIFY(project.mapEdits_->Redo() && project.ObserveMapDataEdit("保存后重做荒芜"));
    QVERIFY(!project.HasUnsavedMapChanges() && !project.mapEdits_->IsDirty());
    const auto forge = std::filesystem::path(root.filePath("intent.forge").toStdWString());
    QVERIFY(project.SaveProjectToPath(forge).Succeeded());
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Project) && !project.HasUnsavedMapChanges());
    auto saved = MapArchiveSession::Open(forge); QVERIFY(saved.Succeeded());
    QVERIFY(saved.value->HasMember("overlay/editor/pathing-restrictions.bin")
        && !saved.value->HasMember("overlay/map/war3map.wpm") && !saved.value->HasMember("overlay/map/war3map.w3e"));
    const auto manifest = saved.value->ReadMember("project.json"); QVERIFY(manifest.Succeeded());
    QCOMPARE(QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(manifest.value->data()), static_cast<qsizetype>(manifest.value->size())))
        .object()["pathingRestrictions"].toInt(), 3);
    auto reopened = PrepareProjectOpen(forge); QVERIFY(reopened.Succeeded());
    auto& edits = *(*reopened.value)->mapDataEdits;
    QCOMPARE(edits.SerializePathingRestrictionDraft().value, savedDeclaration.value);
    QCOMPARE(edits.InspectTerrainPoint(point).value->addedPathingRestrictions, restriction);
    QVERIFY(edits.Replacements().empty() && edits.VerifyTerrainPathingForSave().Succeeded());
    QVERIFY(!edits.IsDirty() && !edits.CanUndo());
    brush.value = false;
    QVERIFY(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    QCOMPARE(edits.InspectTerrainPoint(point).value->pathingFlags, 0x82);
    QVERIFY(edits.VerifyTerrainPathingForSave().Succeeded() && edits.Undo() && !edits.IsDirty());
    QCOMPARE(edits.InspectTerrainPoint(point).value->pathingFlags, 0x82 | restriction);
    if (inPlace) {
        QVERIFY(!job->backup.empty() && std::filesystem::exists(job->backup));
        QCOMPARE(Sha256File(job->backup).value, sourceSha.value);
    } else QCOMPARE(Sha256File(source).value, sourceSha.value);

    // 实际地图已包含S。第二次保存擦除，必须写回P而不是继续把S当来源保护。
    QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("擦除本工程已保存的限制"));
    auto removedPlan = MapData::TerrainSavePlan::Prepare(*project.archiveSession_, *project.mapEdits_);
    QVERIFY(removedPlan.Succeeded() && removedPlan.value->SaveGate().Succeeded());
    const auto eraseDeclaration = project.mapEdits_->SerializePathingRestrictionDraft();
    QVERIFY(eraseDeclaration.Succeeded() && (*eraseDeclaration.value)[3] == std::byte{3});
    auto erase = std::make_unique<MapSaveJob>(); QVERIFY(erase->FreezeArchive(*project.archiveSession_).Succeeded());
    erase->target = inPlace ? project.archiveSession_->SourcePath()
        : std::filesystem::path(root.filePath("erased.w3x").toStdWString());
    erase->inPlace = inPlace; erase->inPlaceOptions = job->inPlaceOptions;
    erase->replacements = project.mapEdits_->Replacements();
    erase->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>(*eraseDeclaration.value);
    erase->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(), project.editRevision_.Current()};
    erase->terrainSavePlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*removedPlan.value));
    erase->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
    const auto* eraseTask = erase.get();
    erase->beforePublish = [eraseTask] {
        return eraseTask->terrainSavePlan->VerifyFrozenOutput(eraseTask->sourceSha256, eraseTask->replacements, eraseTask->removals);
    };
    EditorProject::RunMapSave(*erase);
    QVERIFY(erase->succeeded && erase->nextSession && erase->terrainReadbackStatus && erase->terrainReadbackStatus->Succeeded());
    const auto erasedWpm = erase->nextSession->ReadMember("war3map.wpm"); QVERIFY(erasedWpm.Succeeded());
    QCOMPARE((*erasedWpm.value)[16 + 33], std::byte{0x82});
    QVERIFY(project.CommitMapSave(*erase));
    QVERIFY(!project.HasUnsavedMapChanges() && project.mapEdits_->SerializePathingRestrictionDraft().value->empty());
    QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("第二次保存后撤销擦除"));
    QCOMPARE(project.mapEdits_->InspectTerrainPoint(point).value->pathingFlags, 0x82 | restriction);
    QVERIFY(project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    QVERIFY(project.mapEdits_->Redo() && project.ObserveMapDataEdit("第二次保存后重做擦除") && !project.HasUnsavedMapChanges());
}

void EditorProjectTests::PlacementHeightAndSavedPathingWorkerRoundtrip() {
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = TerrainSaveMembers();
    MapData::UnitPlacements units;
    MapData::UnitPlacement unit;
    unit.rawcode = "u001"; unit.creationId = 1; unit.flags = 2;
    unit.position = {512, 512, 10}; unit.scale = {1, 1, 1}; units.units.push_back(unit);
    MapData::DoodadPlacements doodads;
    MapData::DoodadPlacement tree;
    tree.rawcode = "d001"; tree.creationId = 2; tree.flags = 2;
    tree.position = {512, 512, 5}; tree.scale = {1, 1, 1}; doodads.doodads.push_back(tree);
    const auto unitBytes = units.Serialize(), doodadBytes = doodads.Serialize();
    QVERIFY(unitBytes.Succeeded() && doodadBytes.Succeeded());
    members.push_back({"war3mapUnits.doo", *unitBytes.value});
    members.push_back({"war3map.doo", *doodadBytes.value});
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    // 这里只验证真实worker与UI提交，脚本工具链由Services原创地图专项单独覆盖。
    const auto freeze = [&](const char* name) -> std::unique_ptr<MapSaveJob> {
        auto job = std::make_unique<MapSaveJob>();
        if (!job->FreezeArchive(*project.archiveSession_).Succeeded()) return {};
        auto plan = MapData::TerrainSavePlan::Prepare(*project.archiveSession_, *project.mapEdits_);
        auto declaration = project.mapEdits_->SerializePathingRestrictionDraft();
        if (!plan.Succeeded() || !plan.value->SaveGate().Succeeded() || !declaration.Succeeded()) return {};
        job->target = std::filesystem::path(root.path().toStdWString()) / name;
        job->replacements = project.mapEdits_->Replacements();
        job->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>(*declaration.value);
        job->terrainSavePlan = std::make_shared<const MapData::TerrainSavePlan>(std::move(*plan.value));
        job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
        job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(), project.editRevision_.Current()};
        const auto* task = job.get();
        job->beforePublish = [task] {
            return task->terrainSavePlan->VerifyFrozenOutput(task->sourceSha256, task->replacements, task->removals);
        };
        return job;
    };
    MapData::TerrainBrush restriction; restriction.kind = MapData::TerrainBrushKind::PathingRestriction;
    restriction.size = 1; restriction.pathingRestrictions = 6;
    const MapData::TerrainStrokePoint point{512, 512}, second{544, 512};
    QVERIFY(project.mapEdits_->EditTerrain(restriction, std::span(&point, 1)).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("先保存通行限制"));
    auto initial = freeze("restricted.w3x"); QVERIFY(initial);
    EditorProject::RunMapSave(*initial); QVERIFY(initial->succeeded && project.CommitMapSave(*initial));
    MapData::TerrainBrush height; height.kind = MapData::TerrainBrushKind::Raise;
    height.radius = 0; height.strength = 32;
    QVERIFY(project.mapEdits_->EditTerrain(height, std::span(&point, 1), true).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("摆放随地面高度移动"));
    restriction.pathingRestrictions = 8;
    QVERIFY(project.mapEdits_->EditTerrain(restriction, std::span(&second, 1)).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("追加通行限制"));
    auto raised = freeze("raised.w3x"); QVERIFY(raised);
    QCOMPARE(raised->replacements.size(), std::size_t{4});
    QCOMPARE((*raised->pathingRestrictionDraft)[3], std::byte{3});
    EditorProject::RunMapSave(*raised);
    QVERIFY2(raised->succeeded, raised->diagnostics.empty() ? "worker failed" : raised->diagnostics.back().message.c_str());
    QVERIFY(raised->terrainReadbackStatus && raised->terrainReadbackStatus->Succeeded());
    QVERIFY(project.CommitMapSave(*raised));
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[2], 42.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Doodads()->doodads[0].position[2], 37.0f);
    QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("撤销追加限制"));
    QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("撤销已保存高度"));
    restriction.pathingRestrictions = 0x0e; restriction.value = false;
    QVERIFY(project.mapEdits_->EditTerrain(restriction, std::span(&point, 1)).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("擦除本工程限制，保留作者限制"));
    auto restored = freeze("restored.w3x"); QVERIFY(restored);
    EditorProject::RunMapSave(*restored);
    QVERIFY(restored->succeeded && restored->nextSession && project.CommitMapSave(*restored));
    for (const auto& member : members)
        QCOMPARE(project.archiveSession_->ReadMember(member.name).value, std::optional(member.data));
    QVERIFY(!project.HasUnsavedMapChanges());
    QVERIFY(project.mapEdits_->SerializePathingRestrictionDraft().value->empty());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::NativePathingWorkerRebuildsAndRebinds_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("preserve-copy") << 0;
    QTest::newRow("preserve-in-place") << 1;
    QTest::newRow("automatic-author-tga") << 2;
    QTest::newRow("automatic-imported-blp") << 3;
    QTest::newRow("automatic-minimap-unavailable") << 4;
    QTest::newRow("automatic-generated") << 5;
}

void EditorProjectTests::NativePathingWorkerRebuildsAndRebinds() {
    QFETCH(int, mode);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = NativePathingQtMembers(); QVERIFY(!members.empty());
    const std::vector<std::byte> originalMinimap{std::byte{17}, std::byte{2}, std::byte{199}};
    const auto minimapName = mode == 2 ? "war3mapMap.tga" : "war3mapMap.blp";
    members.push_back({minimapName, originalMinimap});
    if (mode == 3) {
        MapDataFixtures::Fixture imports; imports.UInt(1); imports.UInt(1);
        imports.Byte(13); imports.String("war3mapMap.blp");
        members.push_back({"war3map.imp", std::move(imports.bytes)});
    }
    if (mode == 4) std::erase_if(members, [](const auto& member) { return member.name == "Fixture\\Ground.blp"; });
    const auto source = std::filesystem::path(root.filePath("shallow.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto originalSha = Sha256File(source); QVERIFY(originalSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    QVERIFY(DeepenNativePathingQtProject(project));
    const auto sessionIdentity = project.mapEdits_->SessionIdentity();
    const auto sourceRevision = project.mapEdits_->TerrainSourceRevision();
    const auto minimap = mode < 2 ? Services::TerrainMinimapSaveChoice::Preserve : Services::TerrainMinimapSaveChoice::Automatic;
    auto job = FreezeNativePathingQtJob(project,
        mode == 1 ? source : std::filesystem::path(root.filePath("deep.w3x").toStdWString()), minimap);
    QVERIFY(job && job->terrainDerivationRequired && !job->terrainSavePlan);
    QVERIFY(!job->pathingRestrictionDraft); // 单纯水深修改没有本工程额外通行限制。
    if (mode == 1) {
        job->inPlace = true;
        job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath("backups").toStdWString());
        QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
    }
    int previousCalls = 0;
    job->prepareFinalDerived = [&](Services::MapSaveTask&) { ++previousCalls; return Status::Ok(); };
    InstallMapSaveDerivedPreparation(*job);
    InstallMapSaveDerivedPreparation(*job);
    QCOMPARE(previousCalls, 0); QVERIFY(!job->terrainSavePlan); // 准备阶段不读资源、不做重建。
    EditorProject::RunMapSave(*job);
    QVERIFY2(job->succeeded, job->diagnostics.empty() ? "native worker failed" : job->diagnostics.back().message.c_str());
    QCOMPARE(previousCalls, 1);
    QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
    QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    QCOMPARE(std::count_if(job->replacements.begin(), job->replacements.end(),
        [](const auto& member) { return member.name == "war3map.wpm"; }), 1);
    auto expected = members[2].data;
    // 最近顶点(4,4)覆盖通行格14..17；独立写定深水G1=0x0A，未知0x11保留。
    for (std::size_t y = 14; y <= 17; ++y) for (std::size_t x = 14; x <= 17; ++x)
        expected[16 + y * 32 + x] = std::byte{0x1b};
    QCOMPARE(job->nextSession->ReadMember("war3map.wpm").value, std::optional(expected));
    QCOMPARE(project.mapEdits_->InspectTerrainPoint({512, 512}).value->pathingFlags, 0x19);
    if (mode == 5) {
        QVERIFY(job->minimapSave && job->minimapSave->Plan() == job->terrainSavePlan);
        QVERIFY(job->nextSession->ReadMember(minimapName).value != std::optional(originalMinimap));
    } else {
        QVERIFY(!job->minimapSave);
        QCOMPARE(job->nextSession->ReadMember(minimapName).value, std::optional(originalMinimap));
        if (mode == 4) QVERIFY(job->minimapPreserve && HasReadDiagnostic(*job, "MINIMAP_GENERATION_SKIPPED"));
    }
    QVERIFY(project.CommitMapSave(*job));
    QCOMPARE(project.mapEdits_->SessionIdentity(), sessionIdentity);
    QVERIFY(project.mapEdits_->TerrainSourceRevision() > sourceRevision);
    const auto point = project.mapEdits_->InspectTerrainPoint({512, 512}); QVERIFY(point.Succeeded() && point.value);
    QCOMPARE(point.value->pathingFlags, 0x1b); QCOMPARE(point.value->originalPathingFlags, 0x1b);
    QVERIFY(!project.mapEdits_->IsDirty() && !project.HasUnsavedMapChanges());
    for (const auto* preserved : {"war3map.shd", "war3map.j"}) {
        const auto original = std::find_if(members.begin(), members.end(),
            [preserved](const auto& member) { return member.name == preserved; });
        QVERIFY(original != members.end());
        QCOMPARE(project.archiveSession_->ReadMember(preserved).value, std::optional(original->data));
    }
    // 原创夹具没有单位；完整v0 MMP是两个I32零，不沿用原来的零长度哨兵。
    QVERIFY(job->minimapMarkers && job->minimapMarkers->MarkerCount() == 0);
    QCOMPARE(project.archiveSession_->ReadMember("war3map.mmp").value,
        std::optional(std::vector<std::byte>(8, std::byte{0})));
    if (mode == 1) {
        QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, originalSha.value);
    } else QCOMPARE(Sha256File(source).value, originalSha.value);

    QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("撤销已保存的深水"));
    QCOMPARE(project.mapEdits_->Terrain()->vertices[4 * 9 + 4].waterBaseHeight, 48.0f);
    QVERIFY(project.HasUnsavedMapChanges() && project.mapEdits_->CanRedo());
    auto undone = FreezeNativePathingQtJob(project, std::filesystem::path(root.filePath("shallow-again.w3x").toStdWString()));
    QVERIFY(undone); EditorProject::RunMapSave(*undone);
    QVERIFY2(undone->succeeded, undone->diagnostics.empty() ? "undo save failed" : undone->diagnostics.back().message.c_str());
    QCOMPARE(undone->nextSession->ReadMember("war3map.wpm").value, std::optional(members[2].data));
    QVERIFY(project.CommitMapSave(*undone));
    QCOMPARE(project.mapEdits_->InspectTerrainPoint({512, 512}).value->originalPathingFlags, 0x19);
    QVERIFY(!project.HasUnsavedMapChanges());
    QVERIFY(project.mapEdits_->Redo() && project.ObserveMapDataEdit("重做已保存前的水深笔划"));
    QCOMPARE(project.mapEdits_->Terrain()->vertices[4 * 9 + 4].waterBaseHeight, 80.0f);
    QVERIFY(project.HasUnsavedMapChanges());
    QCOMPARE(project.mapEdits_->SessionIdentity(), sessionIdentity);
}

void EditorProjectTests::BoundaryNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("boundary-save-as") << false;
    QTest::newRow("boundary-save-in-place") << true;
}

void EditorProjectTests::BoundaryNativeSave() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = NativePathingQtMembers(); QVERIFY(!members.empty());
    members.push_back({"war3mapMap.tga", Tests::WaterText("author boundary minimap remains untouched")});
    const auto source = std::filesystem::path(root.filePath("boundary-source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto terrain = project.mapEdits_->Terrain(); QVERIFY(terrain);
    const MapData::TerrainStrokePoint point{terrain->header.offset[0] + 4 * 128, terrain->header.offset[1] + 4 * 128};
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Boundary;
    brush.shape = MapData::TerrainBrushShape::Square; brush.radius = 0; brush.value = true;
    QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("边界正式保存"));
    const auto finalTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(finalTerrain.Succeeded());
    auto expectedPathing = members[2].data;
    // 原版逐格取SW Boundary；只改(4,4)所属的4×4通行像素，保留作者0x11。
    for (std::size_t y = 16; y < 20; ++y) for (std::size_t x = 16; x < 20; ++x)
        expectedPathing[16 + y * 32 + x] = std::byte{0xdf};
    const auto session = project.mapEdits_->SessionIdentity();
    for (int phase = 0; phase < 3; ++phase) {
        if (phase == 1) { QVERIFY(project.mapEdits_->Undo()); QVERIFY(project.ObserveMapDataEdit("撤销已保存边界")); }
        if (phase == 2) { QVERIFY(project.mapEdits_->Redo()); QVERIFY(project.ObserveMapDataEdit("重做已保存边界")); }
        const auto before = project.sourceArchivePath_;
        const auto beforeSha = Sha256File(before); QVERIFY(beforeSha.Succeeded());
        const auto target = inPlace ? before
            : std::filesystem::path(root.filePath(QString("boundary-saved-%1.w3x").arg(phase)).toStdWString());
        auto job = FreezeNativePathingQtJob(project, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true;
            job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath(QString("boundary-backup-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        QCOMPARE(job->nextSession->ReadMember("war3map.w3e").value,
            phase == 1 ? std::optional(members[1].data) : finalTerrain.value);
        QCOMPARE(job->nextSession->ReadMember("war3map.wpm").value,
            std::optional(phase == 1 ? members[2].data : expectedPathing));
        for (const auto& member : members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
            && member.name != "war3map.mmp") QCOMPARE(job->nextSession->ReadMember(member.name).value, std::optional(member.data));
        QVERIFY(project.CommitMapSave(*job));
        QCOMPARE(project.mapEdits_->SessionIdentity(), session);
        QVERIFY(!project.HasUnsavedMapChanges() && !project.mapEdits_->IsDirty());
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(before).value, beforeSha.value);
    }
    if (!inPlace) QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::TerrainUnitPlacementNativeSave_data() {
    QTest::addColumn<int>("operation"); QTest::addColumn<bool>("inPlace");
    for (int operation = 0; operation < 4; ++operation) for (const bool inPlace : {false, true}) {
        const auto row = QString("unit-%1-%2").arg(operation).arg(inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(row.constData()) << operation << inPlace;
    }
}

void EditorProjectTests::TerrainUnitPlacementNativeSave() {
    QFETCH(int, operation); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = NativePathingQtMembers(); QVERIFY(!members.empty());
    MapData::UnitPlacements units;
    MapData::UnitPlacement originalUnit;
    originalUnit.rawcode = "hfoo"; originalUnit.position = {256, 384, 0};
    originalUnit.scale = {1, 1, 1}; originalUnit.owner = 0; originalUnit.creationId = 7;
    originalUnit.hitPoints = 73; originalUnit.mana = 29; originalUnit.unknownOwnerBytes = {17, 23};
    units.units.push_back(originalUnit);
    auto untouched = originalUnit; untouched.creationId = 19; untouched.owner = 2;
    untouched.position = {768, 640, 0}; untouched.rotation = 0.75f;
    units.units.push_back(untouched);
    const auto originalWire = units.Serialize(); QVERIFY(originalWire.Succeeded() && originalWire.value);
    members.push_back({"war3mapUnits.doo", *originalWire.value});
    members.push_back({"war3mapMap.tga", Tests::WaterText("author minimap remains untouched")});
    const auto source = std::filesystem::path(root.filePath("units-and-water.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    QVERIFY(DeepenNativePathingQtProject(project));
    if (operation == 0) units.units[0].position = {448, 576, 0};
    else if (operation == 1) units.units[0].rotation = 1.25f;
    else if (operation == 2) {
        auto clone = units.units[0]; clone.creationId = 37; clone.position = {640, 256, 0};
        units.units.push_back(std::move(clone));
    } else units.units.erase(units.units.begin());
    const auto editedWire = units.Serialize(); QVERIFY(editedWire.Succeeded() && editedWire.value);
    QVERIFY(project.mapEdits_->EditUnits(units).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("同次保存地形和单位摆放"));
    QVERIFY(!project.mapEdits_->VerifyTerrainPathingForSave().Succeeded()); // 旧保原通行门不放宽。
    const auto expectedTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain());
    QVERIFY(expectedTerrain.Succeeded() && expectedTerrain.value);
    auto expectedPathing = members[2].data;
    for (std::size_t y = 14; y <= 17; ++y) for (std::size_t x = 14; x <= 17; ++x)
        expectedPathing[16 + y * 32 + x] = std::byte{0x1b};
    const auto session = project.mapEdits_->SessionIdentity();
    for (int phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
            QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("撤销已保存的单位摆放"));
            QVERIFY(project.mapEdits_->Undo() && project.ObserveMapDataEdit("撤销已保存的地形"));
        }
        if (phase == 2) {
            QVERIFY(project.mapEdits_->Redo() && project.ObserveMapDataEdit("重做已保存的地形"));
            QVERIFY(project.mapEdits_->Redo() && project.ObserveMapDataEdit("重做已保存的单位摆放"));
        }
        const auto& expectedUnits = phase == 1 ? *originalWire.value : *editedWire.value;
        const auto beforeSource = project.sourceArchivePath_;
        const auto beforeSha = Sha256File(beforeSource); QVERIFY(beforeSha.Succeeded());
        const auto revision = project.mapEdits_->TerrainSourceRevision();
        const auto target = inPlace ? beforeSource
            : std::filesystem::path(root.filePath(QString("units-saved-%1.w3x").arg(phase)).toStdWString());
        // 此夹具仅替代脚本工具链；实际派生、写出、回读及前台提交使用生产链。
        auto job = FreezeNativePathingQtJob(project, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true;
            job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath(QString("backup-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        QCOMPARE(job->nextSession->ReadMember("war3mapUnits.doo").value, std::optional(expectedUnits));
        QCOMPARE(job->nextSession->ReadMember("war3map.w3e").value,
            phase == 1 ? std::optional(members[1].data) : expectedTerrain.value);
        QCOMPARE(job->nextSession->ReadMember("war3map.wpm").value,
            std::optional(phase == 1 ? members[2].data : expectedPathing));
        for (const auto& member : members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
            && member.name != "war3mapUnits.doo" && member.name != "war3map.mmp")
            QCOMPARE(job->nextSession->ReadMember(member.name).value, std::optional(member.data));
        QVERIFY(project.CommitMapSave(*job));
        QCOMPARE(project.mapEdits_->SessionIdentity(), session);
        QVERIFY(project.mapEdits_->TerrainSourceRevision() > revision);
        QVERIFY(!project.mapEdits_->IsDirty() && !project.HasUnsavedMapChanges());
        const auto savedDocument = project.mapEdits_->Snapshot();
        const auto readback = savedDocument.Units(); QVERIFY(readback);
        QCOMPARE(readback->Serialize().value, std::optional(expectedUnits));
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(beforeSource).value, beforeSha.value);
    }
    if (!inPlace) QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::NativePathingWorkerPreflightAndFailure_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("preflight-executes-final-derivation") << QStringLiteral("preflight");
    QTest::newRow("missing-pathing-rule-is-not-minimap-fallback") << QStringLiteral("missing-rule");
    QTest::newRow("special-pathing-missing-resources-is-not-minimap-fallback") << QStringLiteral("special");
    QTest::newRow("empty-pathing-declaration-is-rejected") << QStringLiteral("empty-declaration");
    QTest::newRow("late-edit-rejects-front-state-commit") << QStringLiteral("late-edit");
    QTest::newRow("missing-readback-rejects-front-state-commit") << QStringLiteral("missing-readback");
}

void EditorProjectTests::NativePathingWorkerPreflightAndFailure() {
    QFETCH(QString, scenario);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = NativePathingQtMembers(); QVERIFY(!members.empty());
    if (scenario == "missing-rule")
        std::erase_if(members, [](const auto& member) { return member.name == "TerrainArt\\Terrain.slk"; });
    if (scenario == "special") {
        MapData::DoodadPlacements doodads; MapData::SpecialDoodadPlacement placement; placement.rawcode = "SPC1";
        doodads.specialDoodads.push_back(placement);
        const auto wire = doodads.Serialize(); QVERIFY(wire.Succeeded() && wire.value);
        for (auto& member : members) if (member.name == "war3map.doo") member.data = *wire.value;
    }
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("saved.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    QVERIFY(DeepenNativePathingQtProject(project));
    const auto sessionIdentity = project.mapEdits_->SessionIdentity();
    const auto sourceRevision = project.mapEdits_->TerrainSourceRevision();
    const auto editRevision = project.mapEdits_->Revision();
    const auto commandIdentity = project.mapEdits_->LastEdit().CommandIdentity();
    const auto terrainBefore = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(terrainBefore.Succeeded());
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const bool rejectsBeforeWrite = scenario == "missing-rule" || scenario == "special" || scenario == "empty-declaration";
    const auto minimap = rejectsBeforeWrite
        ? Services::TerrainMinimapSaveChoice::Automatic : Services::TerrainMinimapSaveChoice::Preserve;
    auto job = FreezeNativePathingQtJob(project, target, minimap); QVERIFY(job);
    if (scenario == "empty-declaration")
        job->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>();
    InstallMapSaveDerivedPreparation(*job);
    if (scenario == "preflight") {
        const auto checked = Services::MapSaveService::Preflight(*job);
        QVERIFY2(checked.Succeeded(), checked.diagnostics.empty() ? "preflight failed" : checked.diagnostics.back().message.c_str());
        QVERIFY(job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(!job->nextSession && !job->terrainReadbackStatus && !std::filesystem::exists(target));
    } else {
        EditorProject::RunMapSave(*job);
        if (rejectsBeforeWrite) {
            QVERIFY(!job->succeeded && !job->minimapPreserve && !std::filesystem::exists(target));
            QVERIFY(!HasReadDiagnostic(*job, "MINIMAP_GENERATION_SKIPPED"));
            // 特殊模型已可使用实际元数据重建；这个夹具刻意没有对象表，仍须在写出前拒绝。
            const auto code = scenario == "empty-declaration" ? "PATHING_DRAFT_MISSING" : "TERRAIN_PATHING_RESOURCE_MISSING";
            QVERIFY(HasReadDiagnostic(*job, code));
            if (scenario == "special") {
                QVERIFY(std::any_of(job->diagnostics.begin(), job->diagnostics.end(), [](const auto& diagnostic) {
                    return diagnostic.code == "TERRAIN_PATHING_RESOURCE_MISSING"
                        && diagnostic.message.find("Units/DestructableData.slk") != std::string::npos;
                }));
            }
            QVERIFY(!project.CommitMapSave(*job));
        } else {
            QVERIFY2(job->succeeded, job->diagnostics.empty() ? "worker failed" : job->diagnostics.back().message.c_str());
            if (scenario == "late-edit") QVERIFY(RaiseTerrainSaveProject(project));
            else job->terrainReadbackStatus.reset();
            QVERIFY(!project.CommitMapSave(*job));
            QVERIFY(ModelHasCode(*project.diagnosticsModel_, scenario == "late-edit"
                ? QStringLiteral("SAVE_REVISION_CHANGED") : QStringLiteral("TERRAIN_SAVE_PLAN_READBACK_MISSING")));
        }
    }
    QCOMPARE(project.mapEdits_->SessionIdentity(), sessionIdentity);
    QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision);
    QCOMPARE(project.mapEdits_->InspectTerrainPoint({512, 512}).value->pathingFlags, 0x19);
    QCOMPARE(project.mapEdits_->InspectTerrainPoint({512, 512}).value->originalPathingFlags, 0x19);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
    if (scenario != "late-edit") {
        QCOMPARE(project.mapEdits_->Revision(), editRevision);
        QCOMPARE(project.mapEdits_->LastEdit().CommandIdentity(), commandIdentity);
        QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, terrainBefore.value);
    }
    QVERIFY(project.HasUnsavedMapChanges() && project.mapEdits_->CanUndo());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::CGridInvariantNormalSaveAndUndoRedo_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("c-grid-equivalent-save-as") << false;
    QTest::newRow("c-grid-equivalent-save-in-place") << true;
}

void EditorProjectTests::CGridInvariantNormalSaveAndUndoRedo() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, "[DoodadCategories]\nC=custom\n"));
    const auto members = CGridInvariantQtMembers(); QVERIFY(!members.empty());
    QCOMPARE(members[2].data[16 + 12 * 32 + 12], std::byte{0x1d});
    QCOMPARE(members[2].data[16 + 14 * 32 + 14] | std::byte{2}, std::byte{0x1f});
    CheckNativeDoodadNormalSaveAndUndoRedo(members, inPlace, false, nullptr, game);
}

void EditorProjectTests::CGridStartupDirectoryNormalSaveAndUndoRedo_data() {
    QTest::addColumn<QString>("profile"); QTest::addColumn<int>("footprintStart"); QTest::addColumn<bool>("inPlace");
    const std::pair<const char*, const char*> profiles[]{
        {"uppercase-C", "[DoodadCategories]\nC=custom\n"},
        {"no-C", "[DoodadCategories]\nE=environment\n"},
        {"lowercase-c", "[DoodadCategories]\nc=custom\n"}, {"profile-absent", ""}};
    for (const auto& [name, text] : profiles) for (const bool inPlace : {false, true}) {
        const auto row = QString("c-startup-%1-%2").arg(name, inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(row.constData()) << QString::fromUtf8(text) << (std::string_view(name) == "uppercase-C" ? 12 : 14) << inPlace;
    }
}

void EditorProjectTests::CGridStartupDirectoryNormalSaveAndUndoRedo() {
    QFETCH(QString, profile); QFETCH(int, footprintStart); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, profile.toStdString()));
    const auto members = StartupCategoryCQtMembers(static_cast<std::size_t>(footprintStart)); QVERIFY(!members.empty());
    const auto other = StartupCategoryCQtMembers(footprintStart == 12 ? 14 : 12); QVERIFY(!other.empty());
    QVERIFY(members[2].data != other[2].data);
    QCOMPARE(members[2].data[16 + 12 * 32 + 12], footprintStart == 12 ? std::byte{0x1d} : std::byte{0x19});
    QCOMPARE(members[2].data[16 + 21 * 32 + 21], footprintStart == 14 ? std::byte{0x1d} : std::byte{0x19});
    CheckNativeDoodadNormalSaveAndUndoRedo(members, inPlace, false, nullptr, game);
}

void EditorProjectTests::EffectiveDoodadCategorySaveAndUndoRedo_data() {
    QTest::addColumn<bool>("custom"); QTest::addColumn<QString>("standardCategory");
    QTest::addColumn<QString>("categoryOverride"); QTest::addColumn<bool>("hasOverride");
    QTest::addColumn<QString>("rawcode"); QTest::addColumn<int>("footprintStart"); QTest::addColumn<bool>("inPlace");
    struct Row { const char* name; bool custom; const char* standard; const char* overrideValue; const char* rawcode; int start; };
    const Row rows[]{
        {"original-E-to-C", false, "E", "C", "OE01", 12},
        {"original-C-to-E", false, "C", "E", "OC01", 14},
        {"custom-E-to-C", true, "E", "C", "ZE01", 12},
        {"custom-C-to-E", true, "C", "E", "ZC01", 14},
        {"custom-inherit-C", true, "C", nullptr, "ZE01", 12},
        {"custom-inherit-E", true, "E", nullptr, "ZC01", 14},
        {"standard-empty-new-id-C", false, "", nullptr, "OC01", 12},
        {"standard-underscore-new-id-E", false, "_", nullptr, "OE01", 14},
        {"custom-explicit-empty-new-id-E", true, "C", "", "ZE01", 14},
        {"custom-leading-underscore-new-id-C", true, "E", "_suffix", "ZC01", 12}};
    for (const auto& row : rows) for (const bool inPlace : {false, true}) {
        const auto name = QString("effective-category-%1-%2").arg(row.name, inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(name.constData()) << row.custom << QString::fromUtf8(row.standard)
            << QString::fromUtf8(row.overrideValue ? row.overrideValue : "") << (row.overrideValue != nullptr)
            << QString::fromUtf8(row.rawcode) << row.start << inPlace;
    }
}

void EditorProjectTests::EffectiveDoodadCategorySaveAndUndoRedo() {
    QFETCH(bool, custom); QFETCH(QString, standardCategory); QFETCH(QString, categoryOverride); QFETCH(bool, hasOverride);
    QFETCH(QString, rawcode); QFETCH(int, footprintStart); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, "[DoodadCategories]\nC=custom\n"));
    const std::optional<std::string> overrideValue = hasOverride ? std::optional(categoryOverride.toStdString()) : std::nullopt;
    const auto members = EffectiveCategoryQtMembers(custom, standardCategory.toStdString(), overrideValue,
        rawcode.toStdString(), static_cast<std::size_t>(footprintStart)); QVERIFY(!members.empty());
    const auto other = EffectiveCategoryQtMembers(custom, standardCategory.toStdString(), overrideValue,
        rawcode.toStdString(), footprintStart == 12 ? 14 : 12); QVERIFY(!other.empty());
    QVERIFY(members[2].data != other[2].data);
    QCOMPARE(members[2].data[16 + 12 * 32 + 12], footprintStart == 12 ? std::byte{0x1d} : std::byte{0x19});
    QCOMPARE(members[2].data[16 + 21 * 32 + 21], footprintStart == 14 ? std::byte{0x1d} : std::byte{0x19});
    // 空覆盖与未覆盖不同；Custom回退必须用DOO新ID，不能借父ID或父category。
    CheckNativeDoodadNormalSaveAndUndoRedo(members, inPlace, false, nullptr, game);
}

void EditorProjectTests::EffectiveDoodadCategoryWorkerRejectsBeforeWrite_data() {
    QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("inPlace"); QTest::addColumn<QString>("code");
    for (const auto* scenario : {"wrong-wire", "selector", "duplicate-category-address", "old-wpm-other-grid"})
        for (const bool inPlace : {false, true}) {
            const auto name = QString("effective-category-reject-%1-%2").arg(scenario, inPlace ? "in-place" : "save-as").toUtf8();
            const auto code = std::string_view(scenario) == "wrong-wire" ? "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE"
                : std::string_view(scenario) == "old-wpm-other-grid" ? "TERRAIN_PATHING_SOURCE_UNEXPLAINED"
                : "TERRAIN_NATIVE_OBJECT_METADATA";
            QTest::newRow(name.constData()) << QString::fromUtf8(scenario) << inPlace << QString::fromUtf8(code);
        }
}

void EditorProjectTests::EffectiveDoodadCategoryWorkerRejectsBeforeWrite() {
    using namespace ObjectData;
    QFETCH(QString, scenario); QFETCH(bool, inPlace); QFETCH(QString, code);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, "[DoodadCategories]\nC=custom\n"));
    const bool custom = scenario == "selector";
    const std::string rawcode = custom ? "ZE01" : "OE01";
    auto members = EffectiveCategoryQtMembers(custom, "E", std::string("C"), rawcode,
        scenario == "old-wpm-other-grid" ? 14 : 12); QVERIFY(!members.empty());
    if (scenario == "wrong-wire") {
        const auto w3d = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.w3d"; });
        QVERIFY(w3d != members.end());
        auto parsed = NativeObjectCodec(ObjectType::Doodad).Parse(w3d->data); QVERIFY(parsed.Succeeded());
        auto* native = dynamic_cast<NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
        const ObjectKey object{ObjectType::Doodad, rawcode};
        const auto* record = native->FindObject(object); QVERIFY(record && record->modifications.size() == 1);
        // 解析键含显式零索引；删除前复制完整身份，不能以省略索引的键替代。
        const auto categoryKey = record->modifications.front().key;
        QVERIFY(categoryKey.fieldId == "dcat" && !categoryKey.level && categoryKey.dataIndex == 0u && categoryKey.variation == 0u);
        QVERIFY(native->RemoveModification(object, categoryKey).Succeeded());
        QVERIFY(native->AddModification(object, categoryKey, NativeValueType::Int32, std::int32_t{67}).Succeeded());
        QCOMPARE(native->ModificationValueType(object, categoryKey), std::optional{NativeValueType::Int32});
        const auto wire = NativeObjectCodec(ObjectType::Doodad).Serialize(*native); QVERIFY(wire.Succeeded() && wire.value);
        w3d->data = *wire.value;
    } else if (scenario == "selector" || scenario == "duplicate-category-address") {
        const auto metadata = std::find_if(members.begin(), members.end(),
            [](const auto& member) { return member.name == "Doodads/DoodadMetaData.slk"; });
        QVERIFY(metadata != members.end());
        std::vector<std::vector<std::string>> rows{
            {"dptx", "pathTex", "DoodadData", "pathingTexture", "0", "0", "0", ""},
            {"dcat", "category", "DoodadData", "doodadCategory", "0", "0", "0", scenario == "selector" ? "PE01" : ""}};
        if (scenario == "duplicate-category-address")
            rows.push_back({"dca2", "category", "DoodadData", "doodadCategory", "0", "0", "0", ""});
        metadata->data = NativeDoodadQtTable({"ID", "field", "slk", "type", "repeat", "data", "index", "useSpecific"}, rows);
    }
    const auto expectedCode = code.toUtf8();
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, expectedCode.constData(), NativeDoodadQtMutation::None, game);
}

void EditorProjectTests::EffectiveDoodadCategorySourceDriftRejectsBeforeWrite_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("effective-category-drift-save-as") << false;
    QTest::newRow("effective-category-drift-in-place") << true;
}

void EditorProjectTests::EffectiveDoodadCategorySourceDriftRejectsBeforeWrite() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, "[DoodadCategories]\nC=custom\n"));
    const auto members = EffectiveCategoryQtMembers(true, "E", std::string("C"), "ZE01", 12); QVERIFY(!members.empty());
    int mutationCalls = 0;
    const auto mutate = [&]() -> Status {
        ++mutationCalls;
        if (!std::filesystem::remove(game / "war3.mpq"))
            return Status::Failure("Test", "CATEGORY_FIXTURE_MUTATION_FAILED", "无法改写有效category的实际启动来源。");
        const std::vector<MapArchiveMember> replacement{{"UI/WorldEditData.txt", Tests::WaterText("[DoodadCategories]\nE=environment\n")}};
        auto written = WriteVerifiedArchive(game / "war3.mpq", replacement);
        return written.Succeeded() ? Status::Ok() : Status{std::move(written.diagnostics)};
    };
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, "TERRAIN_CATEGORY_SOURCE_CHANGED",
        NativeDoodadQtMutation::None, game, mutate);
    QCOMPARE(mutationCalls, 1);
}

void EditorProjectTests::CheckNativeDoodadNormalSaveAndUndoRedo(const std::vector<MapArchiveMember>& members, bool inPlace,
    bool textureChange, const std::vector<std::byte>* expectedEditedPathing, const std::filesystem::path& gameRoot) {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("native-doodad-source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto originalSha = Sha256File(source); QVERIFY(originalSha.Succeeded());
    EditorProject project; QUndoStack undo; project.mapDataUndoStack_ = &undo;
    QVERIFY(BindTerrainSaveProject(project, source));
    project.war3GameRoot_ = gameRoot;
    if (textureChange) {
        MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Texture;
        brush.radius = 0; brush.groundTexture = "Tg02"; brush.variation = 0;
        const MapData::TerrainStrokePoint point{512, 512};
        const auto edited = project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false);
        QVERIFY2(edited.Succeeded(), ReadDiagnosticText(edited).c_str());
        QVERIFY(project.ObserveMapDataEdit("已有占地上的不可行走贴图编辑"));
    } else QVERIFY(DeepenNativePathingQtProject(project));
    PushMapDataEdit(project, "Native footprint terrain edit", "已有占地上的地形编辑");
    QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), 1);
    const auto session = project.mapEdits_->SessionIdentity();
    const auto parsed = MapData::ParseMapTerrain(members[1].data); QVERIFY(parsed.Succeeded() && parsed.value);
    auto deepGrid = *parsed.value->Snapshot();
    if (textureChange) deepGrid.vertices[4 * 9 + 4].textureAndFlagsRaw = 0x41;
    else deepGrid.vertices[4 * 9 + 4].waterHeightAndFlagsRaw = 8192 + 80 * 4;
    MapData::RefreshTerrainDerived(deepGrid, 0, 0, 9, 9);
    const auto deepTerrain = MapData::SerializeMapTerrain(deepGrid); QVERIFY(deepTerrain.Succeeded());
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, deepTerrain.value);
    auto deepPathing = expectedEditedPathing ? *expectedEditedPathing : members[2].data;
    // 原创占地的完整矩阵由调用者独立提供；普通装饰物仍沿既有深水追加02的预期。
    if (!expectedEditedPathing) for (std::size_t y = 14; y <= 17; ++y) for (std::size_t x = 14; x <= 17; ++x)
        deepPathing[16 + y * 32 + x] |= std::byte{2};
    // 不借占地支持放宽脚本门；仅沿既有合法worker夹具替代脚本准备，领域保存走生产链。
    project.compilerDataRoot_.clear(); project.activeUiDataRoot_.clear(); project.uiDataRoot_.clear();
    const auto blocked = std::filesystem::path(root.filePath("script-blocked.w3x").toStdWString());
    QVERIFY(!project.PrepareMapSave(blocked, false));
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "REPACK_SCRIPT_REJECTED")); QVERIFY(!std::filesystem::exists(blocked));
    for (int phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
            // Qt通过receipt追加逆命令；可重做资格属于Qt栈，不是Core游标。
            undo.undo(); QCOMPARE(undo.index(), 0); QVERIFY(undo.canRedo());
        } else if (phase == 2) {
            undo.redo(); QCOMPARE(undo.index(), 1); QVERIFY(undo.canUndo());
        }
        const bool deep = phase != 1;
        const auto& expectedTerrain = deep ? *deepTerrain.value : members[1].data;
        const auto& expectedPathing = deep ? deepPathing : members[2].data;
        QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::optional(expectedTerrain));
        QVERIFY(project.HasUnsavedMapChanges());
        const auto beforeSource = project.sourceArchivePath_;
        const auto beforeSha = Sha256File(beforeSource); QVERIFY(beforeSha.Succeeded());
        const auto beforeRevision = project.mapEdits_->TerrainSourceRevision();
        const auto target = inPlace ? beforeSource
            : std::filesystem::path(root.filePath(QString("native-doodad-saved-%1.w3x").arg(phase)).toStdWString());
        auto job = FreezeNativePathingQtJob(project, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
        if (inPlace) {
            job->inPlace = true;
            job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->nextSession && job->terrainSavePlan && job->terrainSavePlan->RebuiltPathing());
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto readback = MapArchiveSession::Open(target); QVERIFY(readback.Succeeded());
        QCOMPARE(readback.value->ReadMember("war3map.w3e").value, std::optional(expectedTerrain));
        QCOMPARE(readback.value->ReadMember("war3map.wpm").value, std::optional(expectedPathing));
        for (const auto& member : members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
            && member.name != "war3map.mmp")
            QCOMPARE(readback.value->ReadMember(member.name).value, std::optional(member.data));
        QVERIFY(job->minimapMarkers && job->minimapMarkers->MarkerCount() == 0);
        QCOMPARE(readback.value->ReadMember("war3map.mmp").value,
            std::optional(std::vector<std::byte>(8, std::byte{0})));
        QVERIFY(project.CommitMapSave(*job));
        QCOMPARE(project.mapEdits_->SessionIdentity(), session); QCOMPARE(project.sourceArchivePath_, target);
        QVERIFY(project.mapEdits_->TerrainSourceRevision() > beforeRevision);
        QVERIFY(!project.HasUnsavedMapChanges() && !project.mapEdits_->IsDirty());
        const auto centerFlags = std::to_integer<int>(expectedPathing[16 + 16 * 32 + 16]);
        QCOMPARE(project.mapEdits_->InspectTerrainPoint({512, 512}).value->pathingFlags, centerFlags);
        QCOMPARE(project.mapEdits_->InspectTerrainPoint({512, 512}).value->originalPathingFlags, centerFlags);
        QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), deep ? 1 : 0);
        if (inPlace) {
            QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value);
        } else QCOMPARE(Sha256File(beforeSource).value, beforeSha.value);
    }
    if (!inPlace) QCOMPARE(Sha256File(source).value, originalSha.value);
}

void EditorProjectTests::CGridVariantSourceUsesActualDirectory_data() {
    QTest::addColumn<int>("axis"); QTest::addColumn<bool>("occluded"); QTest::addColumn<bool>("inPlace");
    QTest::newRow("c-startup-x-source-wpm-mismatch-copy") << 0 << false << false;
    QTest::newRow("c-startup-y-source-wpm-mismatch-in-place") << 1 << false << true;
    QTest::newRow("c-startup-x-occluded-source-equivalent-copy") << 0 << true << false;
    QTest::newRow("c-startup-y-occluded-source-equivalent-in-place") << 1 << true << true;
}

void EditorProjectTests::CGridVariantSourceUsesActualDirectory() {
    QFETCH(int, axis); QFETCH(bool, occluded); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, "[DoodadCategories]\nC=custom\n"));
    const auto members = CGridInvariantQtMembers(axis, occluded); QVERIFY(!members.empty());
    if (occluded) {
        const auto sameAggregate = CGridInvariantQtMembers(-1, true); QVERIFY(!sameAggregate.empty());
        QCOMPARE(members[2].data, sameAggregate[2].data);
        // 已从实际启动目录确定snap4，OR相同不再因另一未采用网格而拒绝。
        CheckNativeDoodadNormalSaveAndUndoRedo(members, inPlace, false, nullptr, game);
    } else {
        // 来源WPM手写snap2；实际启动目录选snap4，必须按完整旧WPM解释门拒绝。
        CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, "TERRAIN_PATHING_SOURCE_UNEXPLAINED",
            NativeDoodadQtMutation::None, game);
    }
}

void EditorProjectTests::CGridStartupDirectoryDriftRejectsBeforeWrite_data() {
    QTest::addColumn<QString>("drift"); QTest::addColumn<bool>("inPlace");
    for (const auto* drift : {"same-category-profile-bytes", "remove-category", "add-patch", "add-local",
            "add-basename", "add-relative"}) for (const bool inPlace : {false, true}) {
        const auto row = QString("c-startup-drift-%1-%2").arg(drift, inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(row.constData()) << QString::fromUtf8(drift) << inPlace;
    }
}

void EditorProjectTests::CGridStartupDirectoryDriftRejectsBeforeWrite() {
    QFETCH(QString, drift); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    constexpr std::string_view originalProfile = "[DoodadCategories]\nC=custom\n";
    QVERIFY(WriteStartupCategoryQtGame(game, originalProfile));
    const auto members = StartupCategoryCQtMembers(12); QVERIFY(!members.empty());
    int mutationCalls = 0;
    const auto mutate = [&]() -> Status {
        ++mutationCalls;
        const auto fail = [] { return Status::Failure("Test", "CATEGORY_FIXTURE_MUTATION_FAILED", "无法建立实际启动来源漂移。"); };
        if (drift == "add-basename" || drift == "add-relative") {
            const auto directory = drift == "add-relative" ? game / "UI" : game;
            if (drift == "add-relative" && !std::filesystem::create_directory(directory)) return fail();
            QFile loose(QString::fromStdWString((directory / "WorldEditData.txt").wstring()));
            const auto bytes = QByteArray::fromStdString(std::string(originalProfile));
            if (!loose.open(QIODevice::WriteOnly | QIODevice::NewOnly) || loose.write(bytes) != bytes.size() || !loose.flush())
                return fail();
            return Status::Ok();
        }
        std::string profile(originalProfile);
        auto target = game / "war3.mpq";
        if (drift == "same-category-profile-bytes") profile = "[DoodadCategories]\nC=changed-value-same-category\n";
        else if (drift == "remove-category") profile = "[DoodadCategories]\nE=environment\n";
        else if (drift == "add-patch") target = game / "war3patch.mpq";
        else if (drift == "add-local") target = game / "war3local.mpq";
        else return fail();
        if (target.filename() == "war3.mpq" && !std::filesystem::remove(target)) return fail();
        // local只新增一个无关成员，仍须因五包入口从absent变present而拒绝。
        const std::vector<MapArchiveMember> replacement{{drift == "add-local" ? "owned-unrelated.bin" : "UI/WorldEditData.txt",
            Tests::WaterText(profile)}};
        auto written = WriteVerifiedArchive(target, replacement);
        return written.Succeeded() ? Status::Ok() : Status{std::move(written.diagnostics)};
    };
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, "TERRAIN_CATEGORY_SOURCE_CHANGED",
        NativeDoodadQtMutation::None, game, mutate);
    QCOMPARE(mutationCalls, 1);
}

void EditorProjectTests::CGridStartupDirectoryUnavailableRejectsBeforeWrite_data() {
    QTest::addColumn<bool>("rootIsFile"); QTest::addColumn<bool>("inPlace");
    for (const bool rootIsFile : {false, true}) for (const bool inPlace : {false, true}) {
        const auto row = QString("c-startup-%1-%2").arg(rootIsFile ? "root-is-file" : "root-required",
            inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(row.constData()) << rootIsFile << inPlace;
    }
}

void EditorProjectTests::CGridStartupDirectoryUnavailableRejectsBeforeWrite() {
    QFETCH(bool, rootIsFile); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    std::filesystem::path game;
    if (rootIsFile) {
        game = std::filesystem::path(root.filePath("not-a-directory").toStdWString());
        QFile file(QString::fromStdWString(game.wstring())); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("owned non-directory source"), qint64{26}); file.close();
    }
    const auto members = StartupCategoryCQtMembers(12); QVERIFY(!members.empty());
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace,
        rootIsFile ? "TERRAIN_CATEGORY_SOURCE_UNKNOWN" : "TERRAIN_CATEGORY_SOURCE_REQUIRED",
        NativeDoodadQtMutation::None, game);
}

void EditorProjectTests::CheckNativeDoodadWorkerRejectsBeforeWrite(const std::vector<MapArchiveMember>& members,
    bool inPlace, const char* expectedCode, NativeDoodadQtMutation mutation, const std::filesystem::path& gameRoot,
    const std::function<Status()>& beforePublish) {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("native-doodad-source.w3x").toStdWString());
    const auto target = inPlace ? source : std::filesystem::path(root.filePath("must-not-exist.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    auto expectedSourceSha = sourceSha.value;
    QString sourceDriftError = QStringLiteral("地形保存夹具绑定失败");
    const auto beforeLoad = [&] {
        if (mutation != NativeDoodadQtMutation::SourceDrift) return true;
        QFile external(QString::fromStdWString(source.wstring()));
        const auto fail = [&](const char* stage) {
            sourceDriftError = QString::fromUtf8(stage) + ": " + external.errorString(); return false;
        };
        if (!external.open(QIODevice::ReadWrite)) return fail("打开来源");
        const auto originalSize = external.size();
        if (!external.seek(32)) return fail("读取来源位置");
        char changedByte = 0;
        if (external.read(&changedByte, 1) != 1) return fail("读取来源字节");
        changedByte ^= 1;
        if (!external.seek(32) || external.write(&changedByte, 1) != 1 || !external.flush()) return fail("原位改写来源");
        if (external.size() != originalSize) return fail("来源长度改变");
        external.close();
        const auto driftedSha = Sha256File(source);
        if (!driftedSha.Succeeded() || !driftedSha.value || driftedSha.value == sourceSha.value) return fail("来源SHA未确认变化");
        expectedSourceSha = driftedSha.value;
        return true;
    };
    EditorProject project; QUndoStack undo; project.mapDataUndoStack_ = &undo;
    QVERIFY2(BindTerrainSaveProject(project, source, beforeLoad), qPrintable(sourceDriftError));
    project.war3GameRoot_ = gameRoot;
    QCOMPARE(project.archiveSession_->SourceSha256(), *sourceSha.value);
    if (mutation == NativeDoodadQtMutation::SourceDrift) {
        QVERIFY(expectedSourceSha != sourceSha.value);
        QVERIFY(HasReadDiagnostic(project.archiveSession_->VerifySourceUnchanged(), "SOURCE_CHANGED"));
    }
    if (mutation == NativeDoodadQtMutation::FollowHeight) {
        MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Raise;
        brush.radius = 0; brush.strength = 32;
        const MapData::TerrainStrokePoint point{512, 512};
        const auto edited = project.mapEdits_->EditTerrain(brush, std::span(&point, 1), true);
        QVERIFY2(edited.Succeeded(), ReadDiagnosticText(edited).c_str());
        QVERIFY(project.ObserveMapDataEdit("桥Z随地形改变"));
        QVERIFY(project.mapEdits_->Snapshot().Doodads()->doodads[0].position[2] != 0);
        PushMapDataEdit(project, "Bridge followed terrain height", "桥摆放高度随地形改变");
        // 纯高度通行不变仍属既有快路径；再改成深水，明确进入本单元的Native重建门。
        brush.kind = MapData::TerrainBrushKind::WaterHeight; brush.waterHeight = 160;
        const auto deepened = project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false);
        QVERIFY2(deepened.Succeeded(), ReadDiagnosticText(deepened).c_str());
        QVERIFY(project.ObserveMapDataEdit("已有桥Z变化与实际深水重建组合"));
        QCOMPARE(project.mapEdits_->Terrain()->vertices[4 * 9 + 4].groundHeightRaw, std::uint16_t{8320});
        QCOMPARE(project.mapEdits_->Terrain()->vertices[4 * 9 + 4].waterHeightAndFlagsRaw, std::uint16_t{8832});
    } else QVERIFY(DeepenNativePathingQtProject(project));
    PushMapDataEdit(project, "Rejected native doodad water depth", "非法装饰物来源必须在写出前拒绝");
    const auto* archive = project.archiveSession_.get(); const auto* edits = project.mapEdits_.get();
    const auto session = project.mapEdits_->SessionIdentity();
    const auto sourceRevision = project.mapEdits_->TerrainSourceRevision();
    const auto revision = project.mapEdits_->Revision();
    const auto command = project.mapEdits_->LastEdit().CommandIdentity();
    const auto terrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(terrain.Succeeded());
    const auto placementSnapshot = project.mapEdits_->Snapshot(); QVERIFY(placementSnapshot.Doodads());
    const auto doodadBytes = placementSnapshot.Doodads()->Serialize(); QVERIFY(doodadBytes.Succeeded() && doodadBytes.value);
    const auto draft = TerrainJournalDraftBytes(*project.mapEdits_);
    const auto scopes = project.editRevision_.Current();
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto trackingGeneration = project.editRevision_.Generation(), trackingRevision = project.editRevision_.Revision();
    auto job = FreezeNativePathingQtJob(project, target); QVERIFY(job && job->terrainDerivationRequired);
    QCOMPARE(job->sourceSha256, *sourceSha.value);
    job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
    if (inPlace) {
        job->inPlace = true;
        job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath("backups").toStdWString());
        QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
    }
    if (mutation == NativeDoodadQtMutation::ResourceReplacement) {
        const auto texture = std::find_if(members.begin(), members.end(),
            [](const auto& member) { return member.name == "PathTextures/bridge.tga"; });
        QVERIFY(texture != members.end());
        auto changed = texture->data; changed[18] ^= std::byte{1};
        job->replacements.push_back({texture->name, std::move(changed)});
    }
    if (beforePublish) {
        const auto* task = job.get();
        job->beforePublish = [task, beforePublish] {
            if (!task->terrainSavePlan)
                return Status::Failure("Test", "CATEGORY_FIXTURE_NO_PLAN", "来源漂移必须发生在真实保存计划捕获之后。");
            return beforePublish();
        };
    }
    InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
    QVERIFY(!job->succeeded && !job->nextSession && !job->terrainReadbackStatus && job->written.empty());
    QVERIFY2(HasReadDiagnostic(*job, expectedCode), ReadDiagnosticText(*job).c_str());
    QVERIFY(!project.CommitMapSave(*job)); QVERIFY(job->backup.empty());
    if (inPlace) QVERIFY(std::filesystem::is_empty(job->inPlaceOptions.backupDirectory));
    else QVERIFY(!std::filesystem::exists(target));
    QCOMPARE(project.archiveSession_.get(), archive); QCOMPARE(project.mapEdits_.get(), edits);
    QCOMPARE(project.mapEdits_->SessionIdentity(), session); QCOMPARE(project.sourceArchivePath_, source);
    QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision); QCOMPARE(project.mapEdits_->Revision(), revision);
    QCOMPARE(project.mapEdits_->LastEdit().CommandIdentity(), command);
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, terrain.value);
    QCOMPARE(project.mapEdits_->Snapshot().Doodads()->Serialize().value, doodadBytes.value);
    QCOMPARE(TerrainJournalDraftBytes(*project.mapEdits_), draft);
    QCOMPARE(project.editRevision_.Generation(), trackingGeneration); QCOMPARE(project.editRevision_.Revision(), trackingRevision);
    QCOMPARE(project.editRevision_.Current(), scopes);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->revision, baseline->revision);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
    QVERIFY(project.HasUnsavedMapChanges() && project.mapEdits_->CanUndo());
    const int expectedUndoCount = mutation == NativeDoodadQtMutation::FollowHeight ? 2 : 1;
    QCOMPARE(undo.count(), expectedUndoCount); QCOMPARE(undo.index(), expectedUndoCount);
    QVERIFY(undo.canUndo() && !undo.canRedo());
    QCOMPARE(Sha256File(source).value, expectedSourceSha);
}

void EditorProjectTests::FixedRotationNormalSaveAndUndoRedo_data() {
    QTest::addColumn<bool>("custom"); QTest::addColumn<float>("degrees"); QTest::addColumn<bool>("inPlace");
    for (const bool custom : {false, true}) for (const int degrees : {0, 90}) for (const bool inPlace : {false, true}) {
        const auto name = QString("fixed-rotation-%1-%2-%3")
            .arg(custom ? "custom" : "original").arg(degrees).arg(inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(name.constData()) << custom << static_cast<float>(degrees) << inPlace;
    }
}

void EditorProjectTests::FixedRotationNormalSaveAndUndoRedo() {
    QFETCH(bool, custom); QFETCH(float, degrees); QFETCH(bool, inPlace);
    const auto members = FixedRotationQtMembers(custom, degrees); QVERIFY(!members.empty());
    // 字面矩阵的代表点同时区分标准270回退、有效零覆盖和90覆盖；完整结果由共用循环逐字核对。
    QCOMPARE(members[2].data[16 + 15 * 32 + 15], degrees == 0 ? std::byte{0x19} : std::byte{0xdb});
    QCOMPARE(members[2].data[16 + 14 * 32 + 17], degrees == 0 ? std::byte{0x1d} : std::byte{0x19});
    CheckNativeDoodadNormalSaveAndUndoRedo(members, inPlace);
}

void EditorProjectTests::FixedRotationWorkerRejectsBeforeWrite_data() {
    QTest::addColumn<bool>("custom"); QTest::addColumn<float>("degrees");
    QTest::addColumn<bool>("wrongWireType"); QTest::addColumn<bool>("inPlace"); QTest::addColumn<QString>("code");
    QTest::newRow("fixed-rotation-above-domain-original-copy")
        << false << 361.0f << false << false << QStringLiteral("TERRAIN_DOODAD_FIXED_ROTATION");
    QTest::newRow("fixed-rotation-below-domain-custom-in-place")
        << true << -361.0f << false << true << QStringLiteral("TERRAIN_DOODAD_FIXED_ROTATION");
    QTest::newRow("fixed-rotation-int32-original-in-place")
        << false << 90.0f << true << true << QStringLiteral("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE");
    QTest::newRow("fixed-rotation-int32-custom-copy")
        << true << 90.0f << true << false << QStringLiteral("TERRAIN_NATIVE_OBJECT_DOODAD_TYPE");
    QTest::newRow("fixed-rotation-ambiguous-original-copy")
        << false << 45.0f << false << false << QStringLiteral("TERRAIN_DOODAD_ANGLE_AMBIGUOUS");
    QTest::newRow("fixed-rotation-ambiguous-custom-in-place")
        << true << 45.0f << false << true << QStringLiteral("TERRAIN_DOODAD_ANGLE_AMBIGUOUS");
}

void EditorProjectTests::FixedRotationWorkerRejectsBeforeWrite() {
    QFETCH(bool, custom); QFETCH(float, degrees); QFETCH(bool, wrongWireType); QFETCH(bool, inPlace); QFETCH(QString, code);
    const auto members = FixedRotationQtMembers(custom, degrees, wrongWireType); QVERIFY(!members.empty());
    const auto expectedCode = code.toUtf8();
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, expectedCode.constData());
}

void EditorProjectTests::WalkableDestructableNormalSaveAndUndoRedo_data() {
    QTest::addColumn<bool>("patterned"); QTest::addColumn<bool>("textureChange"); QTest::addColumn<bool>("inPlace");
    for (const bool patterned : {false, true}) for (const bool textureChange : {false, true}) for (const bool inPlace : {false, true}) {
        const auto name = QString("walkable-%1-%2-%3").arg(patterned ? "black-corners" : "uniform")
            .arg(textureChange ? "texture" : "water").arg(inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(name.constData()) << patterned << textureChange << inPlace;
    }
}

void EditorProjectTests::WalkableDestructableNormalSaveAndUndoRedo() {
    QFETCH(bool, patterned); QFETCH(bool, textureChange); QFETCH(bool, inPlace);
    auto members = WalkableDestructableQtMembers(patterned); QVERIFY(!members.empty());
    // 其它rawcode的空W3B记录不能使标准桥失去资格；桥本身没有任何W3B记录。
    auto unrelated = ObjectData::NativeObjectDocument::CreateEmpty(ObjectData::ObjectType::Destructable);
    QVERIFY(unrelated.Succeeded() && unrelated.value);
    QVERIFY(unrelated.value->AddOriginalObject("Un01").Succeeded());
    const auto unrelatedWire = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Destructable).Serialize(*unrelated.value);
    QVERIFY(unrelatedWire.Succeeded() && unrelatedWire.value);
    if (patterned) members.push_back({"war3map.w3b", *unrelatedWire.value});
    QCOMPARE(members[2].data[16 + 15 * 32 + 14], std::byte{0x1d}); // 仅普通阻挡。
    QCOMPARE(members[2].data[16 + 15 * 32 + 16], std::byte{0x15}); // 桥不能抹掉普通阻挡04。
    QCOMPARE(members[2].data[16 + 15 * 32 + 18], std::byte{0x11}); // RGB不能泄漏为C2/04/08。
    QCOMPARE(members[2].data[16 + 14 * 32 + 19], patterned ? std::byte{0x19} : std::byte{0x11});
    auto changedPathing = members[2].data;
    // 水深与walkable=0贴图都仅在最近顶点4×4增加02；桥非黑像素必须抑制这层。
    // 这是独立字面矩阵，不调用Capture/Generate/ProjectFootprint作为oracle。
    for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < 18; ++x) {
        const bool suppressed = x >= 16 && !(patterned && x == 16 && (y == 14 || y == 17));
        if (!suppressed) changedPathing[16 + y * 32 + x] |= std::byte{2};
    }
    QCOMPARE(changedPathing[16 + 15 * 32 + 14], std::byte{0x1f});
    QCOMPARE(changedPathing[16 + 15 * 32 + 16], std::byte{0x15});
    QCOMPARE(changedPathing[16 + 14 * 32 + 16], patterned ? std::byte{0x1f} : std::byte{0x15});
    CheckNativeDoodadNormalSaveAndUndoRedo(members, inPlace, textureChange, &changedPathing);
}

void EditorProjectTests::WalkableDestructableOriginalSaveAndUndoRedo_data() {
    QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("textureChange"); QTest::addColumn<bool>("inPlace");
    for (const auto* scenario : {"empty", "walkable", "path", "rotation-negative", "rotation-zero", "rotation-unreal", "renamed-real", "combined"})
        for (const bool textureChange : {false, true}) for (const bool inPlace : {false, true}) {
            const auto name = QString("walkable-original-%1-%2-%3").arg(scenario)
                .arg(textureChange ? "texture" : "water").arg(inPlace ? "in-place" : "save-as").toUtf8();
            QTest::newRow(name.constData()) << QString::fromLatin1(scenario) << textureChange << inPlace;
        }
}

void EditorProjectTests::WalkableDestructableOriginalSaveAndUndoRedo() {
    QFETCH(QString, scenario); QFETCH(bool, textureChange); QFETCH(bool, inPlace);
    const auto fixture = WalkableDestructableNativeQtMembers(scenario.toStdString()); QVERIFY(!fixture.members.empty());
    const auto w3b = std::find_if(fixture.members.begin(), fixture.members.end(), [](const auto& member) { return member.name == "war3map.w3b"; });
    QVERIFY(w3b != fixture.members.end());
    auto parsed = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Destructable).Parse(w3b->data);
    QVERIFY(parsed.Succeeded());
    const auto* native = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
    QCOMPARE(native->Version(), 2); QCOMPARE(native->OriginalObjectCount(), std::size_t{1});
    QCOMPARE(native->CustomObjectCount(), std::size_t{0});
    const auto* bridge = native->FindObject({ObjectData::ObjectType::Destructable, "Br01"}); QVERIFY(bridge);
    QCOMPARE(bridge->modifications.size(), scenario == "empty" ? std::size_t{0} : scenario == "combined" ? std::size_t{3} : std::size_t{1});
    if (scenario.startsWith("rotation-") || scenario == "renamed-real" || scenario == "combined") {
        // 两点加完整字面WPM核对，区分zero与wire90/90.25，并保留普通green阻挡04。
        QCOMPARE(fixture.members[2].data[16 + 15 * 32 + 16], scenario == "rotation-zero" ? std::byte{0x15} : std::byte{0x1d});
        QCOMPARE(fixture.members[2].data[16 + 16 * 32 + 14], scenario == "rotation-zero" ? std::byte{0x1d} : std::byte{0x15});
    }
    // 旧related-empty-w3b拒绝行改为成功覆盖：两种保存方式都必须经过保存后Qt Undo/Redo。
    // 共用循环还逐字保留W3B、metadata、完整DOO和其它成员，核对来源SHA及in-place备份。
    CheckNativeDoodadNormalSaveAndUndoRedo(fixture.members, inPlace, textureChange, &fixture.editedPathing);
}

void EditorProjectTests::WalkableDestructableOriginalWorkerRejectsBeforeWrite_data() {
    QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("inPlace"); QTest::addColumn<QString>("code");
    for (const bool inPlace : {false, true}) {
        const auto add = [&](const char* scenario, const char* code) {
            const auto name = QString("walkable-original-%1-%2").arg(scenario).arg(inPlace ? "in-place" : "save-as").toUtf8();
            QTest::newRow(name.constData()) << QString::fromLatin1(scenario) << inPlace << QString::fromLatin1(code);
        };
        for (const auto* scenario : {"other-field", "path-empty", "path-underscore", "path-none"})
            add(scenario, "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED");
        add("parent-with-fields", "TERRAIN_NATIVE_OBJECT_INHERITANCE");
        for (const auto* scenario : {"walkable-wire-real", "walkable-value-two"})
            add(scenario, "TERRAIN_NATIVE_OBJECT_WALKABLE_TYPE");
        for (const auto* scenario : {"path-wire-int", "rotation-wire-real"})
            add(scenario, "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE");
        for (const auto* scenario : {"metadata-path-table", "metadata-path-index", "metadata-rotation-alias", "metadata-repeat", "metadata-data", "metadata-missing-index"})
            add(scenario, "TERRAIN_NATIVE_OBJECT_METADATA");
        for (const auto* scenario : {"rotation-above-domain", "rotation-below-domain"})
            add(scenario, "TERRAIN_DOODAD_FIXED_ROTATION");
        add("rotation-ambiguous", "TERRAIN_DOODAD_ANGLE_AMBIGUOUS");
        add("follow-z", "TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED");
        add("pathing-resource-drift", "TERRAIN_NATIVE_PAIR_RULE_CHANGED");
        add("source-drift", "SOURCE_CHANGED");
    }
}

void EditorProjectTests::WalkableDestructableOriginalWorkerRejectsBeforeWrite() {
    using namespace ObjectData;
    QFETCH(QString, scenario); QFETCH(bool, inPlace); QFETCH(QString, code);
    auto fixture = WalkableDestructableNativeQtMembers("combined"); auto& members = fixture.members; QVERIFY(!members.empty());
    auto w3b = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.w3b"; });
    QVERIFY(w3b != members.end());
    auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(w3b->data); QVERIFY(parsed.Succeeded());
    auto* native = dynamic_cast<NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
    const ObjectKey bridge{ObjectType::Destructable, "Br01"};
    const auto replace = [&](std::string field, NativeValueType type, Value value) {
        return native->RemoveModification(bridge, {field}).Succeeded()
            && native->AddModification(bridge, {field}, type, std::move(value)).Succeeded();
    };
    if (scenario == "parent-with-fields") {
        auto custom = NativeObjectDocument::CreateEmpty(ObjectType::Destructable); QVERIFY(custom.Succeeded() && custom.value);
        QVERIFY(custom.value->AddCustomObject("Zb01", "Br01").Succeeded());
        const auto* original = native->FindObject(bridge); QVERIFY(original);
        for (const auto& modification : original->modifications) {
            const auto type = native->ModificationValueType(bridge, modification.key); QVERIFY(type);
            QVERIFY(custom.value->AddModification({ObjectType::Destructable, "Zb01"}, modification.key, *type, modification.value).Succeeded());
        }
        QVERIFY(custom.value->AddOriginalObject("Br01").Succeeded());
        QVERIFY(custom.value->AddModification(bridge, {"bwal"}, NativeValueType::Int32, std::int32_t{1}).Succeeded());
        *native = std::move(*custom.value);
        auto doo = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.doo"; });
        QVERIFY(doo != members.end());
        auto placements = MapData::ParseDoodadPlacements(doo->data); QVERIFY(placements.Succeeded() && placements.value);
        placements.value->doodads.front().rawcode = "Zb01";
        const auto wire = placements.value->Serialize(); QVERIFY(wire.Succeeded() && wire.value); doo->data = *wire.value;
    } else if (scenario == "other-field")
        QVERIFY(native->AddModification(bridge, {"bsuf"}, NativeValueType::String, std::string("Bridge suffix")).Succeeded());
    else if (scenario == "walkable-wire-real") QVERIFY(replace("bwal", NativeValueType::Real, 1.0f));
    else if (scenario == "walkable-value-two") QVERIFY(replace("bwal", NativeValueType::Int32, std::int32_t{2}));
    else if (scenario == "path-wire-int") QVERIFY(replace("bptx", NativeValueType::Int32, std::int32_t{1}));
    else if (scenario == "rotation-wire-real") QVERIFY(replace("bfxr", NativeValueType::Real, 90.25f));
    else if (scenario == "path-empty" || scenario == "path-underscore" || scenario == "path-none")
        QVERIFY(replace("bptx", NativeValueType::String, std::string(scenario == "path-empty" ? "" : scenario == "path-underscore" ? "_" : "none")));
    else if (scenario == "rotation-above-domain" || scenario == "rotation-below-domain" || scenario == "rotation-ambiguous")
        QVERIFY(replace("bfxr", NativeValueType::Unreal, scenario == "rotation-above-domain" ? 361.0f : scenario == "rotation-below-domain" ? -361.0f : 45.0f));
    const auto wire = NativeObjectCodec(ObjectType::Destructable).Serialize(*native); QVERIFY(wire.Succeeded() && wire.value);
    w3b->data = *wire.value;
    if (scenario.startsWith("metadata-") || scenario == "other-field") {
        std::vector<std::string> columns{"ID", "field", "slk", "type", "index"};
        std::vector<std::vector<std::string>> rows{
            {"bwal", "walkable", "DestructableData", "bool", "0"},
            {"bptx", "pathTex", "DestructableData", "pathingTexture", "0"},
            {"bfxr", "fixedRot", "DestructableData", "unreal", "0"}};
        if (scenario == "metadata-path-table") rows[1][2] = "DoodadData";
        else if (scenario == "metadata-path-index") rows[1][4] = "1";
        else if (scenario == "metadata-rotation-alias") rows.push_back({"bxr2", "fixedRot", "DestructableData", "unreal", "0"});
        else if (scenario == "metadata-repeat" || scenario == "metadata-data") {
            columns.push_back(scenario == "metadata-repeat" ? "repeat" : "data");
            for (auto& row : rows) row.push_back("0"); rows[2].back() = "1";
        } else if (scenario == "metadata-missing-index") {
            columns.pop_back(); for (auto& row : rows) row.pop_back();
        } else if (scenario == "other-field") rows.push_back({"bsuf", "EditorSuffix", "DestructableData", "string", "0"});
        auto metadata = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "Units/DestructableMetaData.slk"; });
        QVERIFY(metadata != members.end()); metadata->data = NativeDoodadQtTable(columns, rows);
    }
    const auto mutation = scenario == "follow-z" ? NativeDoodadQtMutation::FollowHeight
        : scenario == "pathing-resource-drift" ? NativeDoodadQtMutation::ResourceReplacement
        : scenario == "source-drift" ? NativeDoodadQtMutation::SourceDrift : NativeDoodadQtMutation::None;
    const auto expectedCode = code.toUtf8();
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, expectedCode.constData(), mutation);
}

void EditorProjectTests::WalkableDestructableCustomSaveAndUndoRedo_data() {
    QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("textureChange"); QTest::addColumn<bool>("inPlace");
    for (const auto* scenario : {"empty", "walkable", "path", "rotation-zero", "combined", "parent-c", "unused-selectors", "unrelated-selector"})
        for (const bool textureChange : {false, true}) for (const bool inPlace : {false, true}) {
            const auto name = QString("walkable-custom-%1-%2-%3").arg(scenario)
                .arg(textureChange ? "texture" : "water").arg(inPlace ? "in-place" : "save-as").toUtf8();
            QTest::newRow(name.constData()) << QString::fromLatin1(scenario) << textureChange << inPlace;
        }
}

void EditorProjectTests::WalkableDestructableCustomSaveAndUndoRedo() {
    using namespace ObjectData;
    QFETCH(QString, scenario); QFETCH(bool, textureChange); QFETCH(bool, inPlace);
    const auto parent = scenario == "parent-c" ? "SC01" : "Br01";
    const auto base = scenario == "parent-c" || scenario == "unused-selectors" ? "empty"
        : scenario == "unrelated-selector" ? "combined" : scenario.toStdString();
    auto fixture = WalkableDestructableNativeQtMembers(base, "Nb01", parent); QVERIFY(!fixture.members.empty());
    if (scenario == "unused-selectors" || scenario == "unrelated-selector")
        QVERIFY(SetCustomBridgeQtSelectorMetadata(fixture.members, scenario.toStdString()));
    const auto w3b = std::find_if(fixture.members.begin(), fixture.members.end(), [](const auto& member) { return member.name == "war3map.w3b"; });
    QVERIFY(w3b != fixture.members.end());
    auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(w3b->data); QVERIFY(parsed.Succeeded());
    const auto* native = dynamic_cast<const NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
    QCOMPARE(native->Version(), 2); QCOMPARE(native->OriginalObjectCount(), std::size_t{0});
    QCOMPARE(native->CustomObjectCount(), std::size_t{1});
    const auto* bridge = native->FindObject({ObjectType::Destructable, "Nb01"}); QVERIFY(bridge && bridge->parentRawcode);
    QCOMPARE(*bridge->parentRawcode, std::string(parent));
    QCOMPARE(bridge->modifications.size(), base == "empty" ? std::size_t{0} : base == "combined" ? std::size_t{3} : std::size_t{1});
    const auto doo = std::find_if(fixture.members.begin(), fixture.members.end(), [](const auto& member) { return member.name == "war3map.doo"; });
    QVERIFY(doo != fixture.members.end());
    auto placements = MapData::ParseDoodadPlacements(doo->data); QVERIFY(placements.Succeeded() && placements.value);
    QCOMPARE(placements.value->doodads.front().rawcode, std::string("Nb01"));
    // SC01父的第二字节C不能替换DOO子ID的b；仍须匹配同一独立字面WPM。
    // 共用真实保存循环核完整W3B/metadata/DOO、备份与保存后Qt Undo/Redo，不用产品生成器造oracle。
    CheckNativeDoodadNormalSaveAndUndoRedo(fixture.members, inPlace, textureChange, &fixture.editedPathing);
}

void EditorProjectTests::WalkableDestructableCustomWorkerRejectsBeforeWrite_data() {
    QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("inPlace"); QTest::addColumn<QString>("code");
    for (const bool inPlace : {false, true}) {
        const auto add = [&](const char* scenario, const char* code) {
            const auto name = QString("walkable-custom-%1-%2").arg(scenario).arg(inPlace ? "in-place" : "save-as").toUtf8();
            QTest::newRow(name.constData()) << QString::fromLatin1(scenario) << inPlace << QString::fromLatin1(code);
        };
        for (const auto* scenario : {"used-walkable-use", "used-walkable-exclude", "used-path-use", "used-path-exclude", "used-rotation-use", "used-rotation-exclude"})
            add(scenario, "TERRAIN_NATIVE_OBJECT_METADATA");
        add("child-c", "TERRAIN_CATEGORY_SOURCE_REQUIRED");
        add("parent-custom", "TERRAIN_NATIVE_OBJECT_INHERITANCE");
        add("parent-doodad", "TERRAIN_NATIVE_OBJECT_INHERITANCE");
        add("child-doodad-collision", "TERRAIN_NATIVE_OBJECT_CLASS_AMBIGUOUS");
        add("rotation-wire-real", "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE");
        add("rotation-above-domain", "TERRAIN_DOODAD_FIXED_ROTATION");
        add("follow-z", "TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED");
        add("pathing-resource-drift", "TERRAIN_NATIVE_PAIR_RULE_CHANGED");
        add("source-drift", "SOURCE_CHANGED");
    }
}

void EditorProjectTests::WalkableDestructableCustomWorkerRejectsBeforeWrite() {
    using namespace ObjectData;
    QFETCH(QString, scenario); QFETCH(bool, inPlace); QFETCH(QString, code);
    const auto child = scenario == "child-c" ? "NC01" : "Nb01";
    // walkable即使没有原生覆盖也参与分类；该字段的selector不能因空Custom而跳过。
    auto fixture = WalkableDestructableNativeQtMembers(scenario.startsWith("used-walkable") ? "empty" : "combined", child);
    auto& members = fixture.members; QVERIFY(!members.empty());
    if (scenario.startsWith("used-")) QVERIFY(SetCustomBridgeQtSelectorMetadata(members, scenario.toStdString()));
    const auto w3b = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.w3b"; });
    QVERIFY(w3b != members.end());
    auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(w3b->data); QVERIFY(parsed.Succeeded());
    auto* native = dynamic_cast<NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
    if (scenario == "parent-custom") QVERIFY(native->AddCustomObject("Br01", "Un01").Succeeded());
    else if (scenario == "parent-doodad") {
        auto parent = NativeObjectDocument::CreateEmpty(ObjectType::Doodad); QVERIFY(parent.Succeeded() && parent.value);
        QVERIFY(parent.value->AddOriginalObject("Br01").Succeeded());
        const auto wire = NativeObjectCodec(ObjectType::Doodad).Serialize(*parent.value); QVERIFY(wire.Succeeded() && wire.value);
        members.push_back({"war3map.w3d", *wire.value});
    } else if (scenario == "child-doodad-collision") {
        const auto table = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "Doodads/Doodads.slk"; });
        QVERIFY(table != members.end());
        table->data = NativeDoodadQtTable({"doodID", "code", "pathTex", "category", "fixedRot"},
            {{"Bl01", "Bl01", "PathTextures/blocker.tga", "E", "0"}, {child, child, "none", "E", "0"}});
    } else if (scenario == "rotation-wire-real" || scenario == "rotation-above-domain") {
        const ObjectKey bridge{ObjectType::Destructable, child};
        QVERIFY(native->RemoveModification(bridge, {"bfxr"}).Succeeded());
        QVERIFY(native->AddModification(bridge, {"bfxr"}, scenario == "rotation-wire-real" ? NativeValueType::Real : NativeValueType::Unreal,
            scenario == "rotation-wire-real" ? 90.25f : 361.0f).Succeeded());
    }
    // push_back可能使成员迭代器失效；写回前重新按名字取得W3B。
    const auto wire = NativeObjectCodec(ObjectType::Destructable).Serialize(*native); QVERIFY(wire.Succeeded() && wire.value);
    const auto output = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.w3b"; });
    QVERIFY(output != members.end()); output->data = *wire.value;
    const auto mutation = scenario == "follow-z" ? NativeDoodadQtMutation::FollowHeight
        : scenario == "pathing-resource-drift" ? NativeDoodadQtMutation::ResourceReplacement
        : scenario == "source-drift" ? NativeDoodadQtMutation::SourceDrift : NativeDoodadQtMutation::None;
    const auto expectedCode = code.toUtf8();
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, expectedCode.constData(), mutation);
}

void EditorProjectTests::WalkableCategoryCNormalSaveAndUndoRedo_data() {
    QTest::addColumn<bool>("custom"); QTest::addColumn<bool>("typed"); QTest::addColumn<bool>("four");
    QTest::addColumn<bool>("turned"); QTest::addColumn<bool>("textureChange"); QTest::addColumn<bool>("inPlace");
    struct Row { const char* name; bool custom, typed, four, turned, texture; };
    // 代表矩阵覆盖Original/Custom、空/实际覆盖、两种真实startup、0/90与两种地形编辑。
    constexpr std::array<Row, 4> rows{{
        {"original-empty-c4-zero", false, false, true, false, false},
        {"original-typed-c2-90", false, true, false, true, true},
        {"custom-empty-c2-zero", true, false, false, false, false},
        {"custom-typed-c4-90", true, true, true, true, true}}};
    for (const auto& row : rows) for (const bool inPlace : {false, true}) {
        const auto name = QString("walkable-c-grid-%1-%2").arg(row.name, inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(name.constData()) << row.custom << row.typed << row.four << row.turned << row.texture << inPlace;
    }
}

void EditorProjectTests::WalkableCategoryCNormalSaveAndUndoRedo() {
    using namespace ObjectData;
    QFETCH(bool, custom); QFETCH(bool, typed); QFETCH(bool, four);
    QFETCH(bool, turned); QFETCH(bool, textureChange); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, four ? "[DoodadCategories]\nC=bridge\n" : "[DoodadCategories]\nE=known-no-C\n"));
    const auto fixture = WalkableCategoryCNativeQtMembers(four, custom, typed, turned); QVERIFY(!fixture.members.empty());
    const auto otherGrid = WalkableCategoryCNativeQtMembers(!four, custom, typed, turned); QVERIFY(!otherGrid.members.empty());
    QVERIFY(fixture.members[2].data != otherGrid.members[2].data);
    QVERIFY(fixture.editedPathing != otherGrid.editedPathing);
    const auto w3b = std::find_if(fixture.members.begin(), fixture.members.end(), [](const auto& member) { return member.name == "war3map.w3b"; });
    QVERIFY(w3b != fixture.members.end());
    auto parsed = NativeObjectCodec(ObjectType::Destructable).Parse(w3b->data); QVERIFY(parsed.Succeeded());
    const auto* native = dynamic_cast<const NativeObjectDocument*>(parsed.document.get()); QVERIFY(native);
    QCOMPARE(native->Version(), 2);
    QCOMPARE(native->OriginalObjectCount(), custom ? std::size_t{0} : std::size_t{1});
    QCOMPARE(native->CustomObjectCount(), custom ? std::size_t{1} : std::size_t{0});
    const auto* bridge = native->FindObject({ObjectType::Destructable, custom ? "NC01" : "BC01"}); QVERIFY(bridge);
    QCOMPARE(bridge->modifications.size(), typed ? std::size_t{3} : std::size_t{0});
    if (custom) { QVERIFY(bridge->parentRawcode); QCOMPARE(*bridge->parentRawcode, std::string("Br01")); }
    // 共用Qt循环实际比较全WPM/W3E及每个其它成员，核source磁盘SHA、备份和保存后的Undo/Redo。
    CheckNativeDoodadNormalSaveAndUndoRedo(fixture.members, inPlace, textureChange, &fixture.editedPathing, game);
}

void EditorProjectTests::WalkableCategoryCUnavailableRejectsBeforeWrite_data() {
    QTest::addColumn<bool>("rootIsFile"); QTest::addColumn<bool>("inPlace");
    for (const bool rootIsFile : {false, true}) for (const bool inPlace : {false, true}) {
        const auto row = QString("walkable-c-startup-%1-%2").arg(rootIsFile ? "root-is-file" : "root-required",
            inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(row.constData()) << rootIsFile << inPlace;
    }
}

void EditorProjectTests::WalkableCategoryCUnavailableRejectsBeforeWrite() {
    QFETCH(bool, rootIsFile); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    std::filesystem::path game;
    if (rootIsFile) {
        game = std::filesystem::path(root.filePath("not-a-directory").toStdWString());
        QFile file(QString::fromStdWString(game.wstring())); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("owned non-directory source"), qint64{26}); file.close();
    }
    const auto fixture = WalkableCategoryCNativeQtMembers(true, true, true, false); QVERIFY(!fixture.members.empty());
    CheckNativeDoodadWorkerRejectsBeforeWrite(fixture.members, inPlace,
        rootIsFile ? "TERRAIN_CATEGORY_SOURCE_UNKNOWN" : "TERRAIN_CATEGORY_SOURCE_REQUIRED", NativeDoodadQtMutation::None, game);
}

void EditorProjectTests::WalkableCategoryCSourceDriftRejectsBeforeWrite_data() {
    QTest::addColumn<bool>("four"); QTest::addColumn<bool>("loose"); QTest::addColumn<bool>("inPlace");
    for (const bool four : {false, true}) for (const bool loose : {false, true}) for (const bool inPlace : {false, true}) {
        const auto row = QString("walkable-c-source-%1-%2-%3").arg(four ? "c4" : "c2")
            .arg(loose ? "same-result-loose-winner" : "same-result-profile-bytes").arg(inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(row.constData()) << four << loose << inPlace;
    }
}

void EditorProjectTests::WalkableCategoryCSourceDriftRejectsBeforeWrite() {
    QFETCH(bool, four); QFETCH(bool, loose); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    const std::string profile = four ? "[DoodadCategories]\nC=bridge\n" : "[DoodadCategories]\nE=known-no-C\n";
    QVERIFY(WriteStartupCategoryQtGame(game, profile));
    const auto fixture = WalkableCategoryCNativeQtMembers(four, true, true, true); QVERIFY(!fixture.members.empty());
    int mutations = 0;
    const auto mutate = [&]() -> Status {
        ++mutations;
        const auto fail = [] { return Status::Failure("Test", "CATEGORY_FIXTURE_MUTATION_FAILED", "无法建立C桥实际启动来源漂移。"); };
        if (loose) {
            // 最终C选择及profile内容不变，只有winner由MPQ改为原先缺席的basename来源。
            QFile file(QString::fromStdWString((game / "WorldEditData.txt").wstring()));
            const auto bytes = QByteArray::fromStdString(profile);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size() || !file.flush()) return fail();
            return Status::Ok();
        }
        const auto target = game / "war3.mpq";
        if (!std::filesystem::remove(target)) return fail();
        const std::vector<MapArchiveMember> replacement{{"UI/WorldEditData.txt", Tests::WaterText(
            four ? "[DoodadCategories]\nC=changed-bytes-same-C\n" : "[DoodadCategories]\nE=changed-bytes-still-no-C\n")}};
        auto written = WriteVerifiedArchive(target, replacement);
        return written.Succeeded() ? Status::Ok() : Status{std::move(written.diagnostics)};
    };
    CheckNativeDoodadWorkerRejectsBeforeWrite(fixture.members, inPlace, "TERRAIN_CATEGORY_SOURCE_CHANGED",
        NativeDoodadQtMutation::None, game, mutate);
    QCOMPARE(mutations, 1);
}

void EditorProjectTests::WalkableCategoryCOldGridRejectsBeforeWrite_data() {
    QTest::addColumn<bool>("four"); QTest::addColumn<bool>("inPlace");
    for (const bool four : {false, true}) for (const bool inPlace : {false, true}) {
        const auto row = QString("walkable-c-wrong-old-grid-%1-%2").arg(four ? "c4" : "c2", inPlace ? "in-place" : "save-as").toUtf8();
        QTest::newRow(row.constData()) << four << inPlace;
    }
}

void EditorProjectTests::WalkableCategoryCOldGridRejectsBeforeWrite() {
    QFETCH(bool, four); QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto game = std::filesystem::path(root.filePath("startup-game").toStdWString());
    QVERIFY(WriteStartupCategoryQtGame(game, four ? "[DoodadCategories]\nC=bridge\n" : "[DoodadCategories]\nE=known-no-C\n"));
    const auto fixture = WalkableCategoryCNativeQtMembers(!four, true, true, false); QVERIFY(!fixture.members.empty());
    // wrong-grid有明确差异；不能把不匹配网格贡献当成作者保护位从旧WPM吞掉。
    CheckNativeDoodadWorkerRejectsBeforeWrite(fixture.members, inPlace, "TERRAIN_PATHING_SOURCE_UNEXPLAINED",
        NativeDoodadQtMutation::None, game);
}

void EditorProjectTests::WalkableDestructableWorkerRejectsBeforeWrite_data() {
    QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("inPlace"); QTest::addColumn<QString>("code");
    for (const bool inPlace : {false, true}) {
        const auto add = [&](const char* scenario, const char* code) {
            const auto name = QString("walkable-%1-%2").arg(scenario).arg(inPlace ? "in-place" : "save-as").toUtf8();
            QTest::newRow(name.constData()) << QString::fromLatin1(scenario) << inPlace << QString::fromLatin1(code);
        };
        for (const auto* state : {"flags-zero", "flags-one", "flags-three", "flags-unknown", "dead", "damaged", "life-unknown"})
            add(state, "TERRAIN_WALKABLE_DESTRUCTABLE_STATE");
        add("follow-z", "TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED");
        add("pathing-resource-drift", "TERRAIN_NATIVE_PAIR_RULE_CHANGED");
        add("source-drift", "SOURCE_CHANGED");
    }
}

void EditorProjectTests::WalkableDestructableWorkerRejectsBeforeWrite() {
    QFETCH(QString, scenario); QFETCH(bool, inPlace); QFETCH(QString, code);
    auto members = WalkableDestructableQtMembers(false); QVERIFY(!members.empty());
    const auto doo = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.doo"; });
    QVERIFY(doo != members.end());
    auto placements = MapData::ParseDoodadPlacements(doo->data); QVERIFY(placements.Succeeded() && placements.value);
    auto& bridge = placements.value->doodads[0];
    if (scenario == "flags-zero") bridge.flags = 0;
    else if (scenario == "flags-one") bridge.flags = 1;
    else if (scenario == "flags-three") bridge.flags = 3;
    // >=0x20属于经典布局拒绝；0x1f可解析，才能检验未知桥状态的专用门。
    else if (scenario == "flags-unknown") bridge.flags = 0x1f;
    else if (scenario == "dead") bridge.lifePercent = 0;
    else if (scenario == "damaged") bridge.lifePercent = 99;
    else if (scenario == "life-unknown") bridge.lifePercent = 255;
    const auto wire = placements.value->Serialize(); QVERIFY(wire.Succeeded() && wire.value); doo->data = *wire.value;
    const auto mutation = scenario == "follow-z" ? NativeDoodadQtMutation::FollowHeight
        : scenario == "pathing-resource-drift" ? NativeDoodadQtMutation::ResourceReplacement
        : scenario == "source-drift" ? NativeDoodadQtMutation::SourceDrift : NativeDoodadQtMutation::None;
    const auto expectedCode = code.toUtf8();
    CheckNativeDoodadWorkerRejectsBeforeWrite(members, inPlace, expectedCode.constData(), mutation);
}

void EditorProjectTests::NativeObjectCatalogReadLeaseBlocksSourceWrite() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("catalog-guard-source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, WalkableDestructableQtMembers(false)).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded() && sourceSha.value);
    const auto attributes = GetFileAttributesW(source.c_str());
    QVERIFY(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_READONLY));
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto catalog = ProjectEffectiveObjectCatalog(project); QVERIFY(catalog);
    const auto verified = VerifyProjectObjectCatalog(project);
    QVERIFY2(verified.Succeeded(), ReadDiagnosticText(verified).c_str());
    QFile readable(QString::fromStdWString(source.wstring()));
    QVERIFY2(readable.open(QIODevice::ReadOnly), qPrintable(readable.errorString()));
    QCOMPARE(readable.read(1).size(), qsizetype{1}); readable.close();
    // 正常Load后持有真实物编来源只读租约；不是只读属性或失效路径造成不能写。
    QFile external(QString::fromStdWString(source.wstring()));
    QVERIFY2(!external.open(QIODevice::ReadWrite), "物编来源租约应阻止打开当前来源以写入");
    QVERIFY(external.error() != QFileDevice::NoError);
    QCOMPARE(ProjectEffectiveObjectCatalog(project), catalog);
    QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
    QCOMPARE(project.archiveSession_->SourceSha256(), *sourceSha.value);
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::PathingIntentLateEditStalesFrozenProject() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, TerrainSaveMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto workspace = root.filePath("workspace"); QVERIFY(QDir().mkpath(workspace));
    project.projectRoot_ = std::filesystem::path(workspace.toStdWString());
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::PathingRestriction;
    brush.size = 1; brush.pathingRestrictions = 2;
    const MapData::TerrainStrokePoint first{48, 48}, second{80, 48};
    QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&first, 1), false).Succeeded());
    const auto frozen = project.ProjectFingerprint();
    const auto target = std::filesystem::path(root.filePath("frozen.forge").toStdWString());
    auto prepared = project.PrepareProjectSave(target); QVERIFY(prepared.Succeeded());
    auto job = std::move(*prepared.value);
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Project); QVERIFY(baseline);
    QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&second, 1), false).Succeeded());
    QVERIFY(project.ProjectFingerprint() != frozen && project.mapEdits_->Replacements().empty());
    EditorProject::RunProjectSave(*job);
    const auto committed = project.CommitProjectSave(*job);
    QVERIFY(!committed.Succeeded() && !committed.diagnostics.empty());
    QCOMPARE(committed.diagnostics.front().code, std::string("PROJECT_SAVE_REVISION_CHANGED"));
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Project)->scopes, baseline->scopes);
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project) && project.HasUnsavedMapChanges());
    QCOMPARE(project.mapEdits_->QueryPathingRestrictions().value->totalCells, std::size_t{2});
    auto historical = PrepareProjectOpen(target); QVERIFY(historical.Succeeded());
    QCOMPARE((*historical.value)->mapDataEdits->QueryPathingRestrictions().value->totalCells, std::size_t{1});
}

void EditorProjectTests::LegacyPathingIntentProjectKeepsStrictCompatibility() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, TerrainSaveMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto workspace = root.filePath("workspace"); QVERIFY(QDir().mkpath(workspace));
    project.projectRoot_ = std::filesystem::path(workspace.toStdWString());
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::PathingRestriction;
    brush.size = 1; brush.pathingRestrictions = 4; const MapData::TerrainStrokePoint point{48, 48};
    QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    const auto target = std::filesystem::path(root.filePath("current.forge").toStdWString());
    QVERIFY(project.SaveProjectToPath(target).Succeeded());
    auto opened = MapArchiveSession::Open(target); QVERIFY(opened.Succeeded());
    for (int mode = 0; mode < 5; ++mode) {
        auto members = std::vector<MapArchiveMember>(opened.value->Members().begin(), opened.value->Members().end());
        auto manifest = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "project.json"; });
        auto declaration = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "overlay/editor/pathing-restrictions.bin"; });
        QVERIFY(manifest != members.end() && declaration != members.end());
        auto json = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(manifest->data.data()), static_cast<qsizetype>(manifest->data.size()))).object();
        json["pathingRestrictions"] = mode == 4 ? 3 : 1;
        const auto encoded = QJsonDocument(json).toJson();
        const auto encodedBytes = std::as_bytes(std::span(encoded.constData(), static_cast<std::size_t>(encoded.size())));
        manifest->data.assign(encodedBytes.begin(), encodedBytes.end());
        // 本夹具只有源WPM不存在的不可飞；APR1声明正文与APR2相同，只迁移已知差分。
        declaration->data[3] = mode == 1 ? std::byte{2} : mode == 4 ? std::byte{3} : std::byte{1};
        if (mode == 2) declaration->data.pop_back();
        if (mode == 3) declaration->data[4] ^= std::byte{1};
        const auto legacy = std::filesystem::path(root.filePath(QString("legacy-%1.forge").arg(mode)).toStdWString());
        QVERIFY(WriteVerifiedArchive(legacy, members).Succeeded());
        auto restored = PrepareProjectOpen(legacy);
        if (mode) { QVERIFY(!restored.Succeeded()); continue; }
        QVERIFY(restored.Succeeded());
        auto& edits = *(*restored.value)->mapDataEdits;
        QCOMPARE(edits.InspectTerrainPoint(point).value->addedPathingRestrictions, 4);
        QCOMPARE(edits.InspectTerrainPoint(point).value->pathingFlags, 0x86);
        QVERIFY((*edits.SerializePathingRestrictionDraft().value)[3] == std::byte{2});
        QVERIFY(edits.VerifyTerrainPathingForSave().Succeeded());
    }
}

void EditorProjectTests::TerrainShadowWorkerExplicitRequest_data() {
    QTest::addColumn<bool>("ridge"); QTest::addColumn<bool>("automatic");
    QTest::newRow("flat-preserve") << false << false;
    QTest::newRow("ridge-preserve") << true << false;
    QTest::newRow("flat-automatic-author-tga") << false << true;
    QTest::newRow("ridge-automatic-author-tga") << true << true;
}

void EditorProjectTests::TerrainShadowWorkerExplicitRequest() {
    QFETCH(bool, ridge); QFETCH(bool, automatic);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto members = TerrainShadowQtMembers(ridge, false); QVERIFY(!members.empty());
    const auto source = std::filesystem::path(root.filePath("original-shadow.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("calculated-shadow.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    project.war3GameRoot_ = TerrainShadowQtGame(root.path(), true); QVERIFY(!project.war3GameRoot_.empty());
    auto job = FreezeNativePathingQtJob(project, target, automatic ? Services::TerrainMinimapSaveChoice::Automatic
        : Services::TerrainMinimapSaveChoice::Preserve);
    QVERIFY(job && !job->terrainDerivationRequired && !job->rebuildTerrainShadow && !job->terrainShadow);
    job->rebuildTerrainShadow = true;
    int previousCalls = 0;
    job->prepareFinalDerived = [&](Services::MapSaveTask&) { ++previousCalls; return Status::Ok(); };
    InstallMapSaveDerivedPreparation(*job); InstallMapSaveDerivedPreparation(*job);
    QCOMPARE(previousCalls, 0); QVERIFY(!job->terrainShadow && !job->terrainSavePlan && !std::filesystem::exists(target));
    // 复用既有freeze仅跳过脚本工具链准备，实际最终worker、发布、归档回读及提交均走生产入口。
    EditorProject::RunMapSave(*job);
    QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str()); QCOMPARE(previousCalls, 1);
    QVERIFY(job->executed && job->nextSession && job->terrainShadow);
    QCOMPARE(job->terrainShadow->Width(), 32u); QCOMPARE(job->terrainShadow->Height(), 32u);
    QCOMPARE(job->terrainShadow->CasterCount(), std::size_t{0});
    const auto expected = LiteralTerrainShadowQtExpected(ridge);
    QCOMPARE(job->terrainShadow->Bytes(), expected);
    QCOMPARE(std::count_if(job->replacements.begin(), job->replacements.end(),
        [](const auto& member) { return member.name == "war3map.shd"; }), 1);
    auto saved = MapArchiveSession::Open(target); QVERIFY(saved.Succeeded() && saved.value);
    const auto shadow = saved.value->ReadMember("war3map.shd"); QVERIFY(shadow.Succeeded() && shadow.value);
    QCOMPARE(*shadow.value, expected); QVERIFY(job->terrainShadow->VerifyReadback(*saved.value).Succeeded());
    const auto parsed = MapData::ParseMapShadow(*shadow.value, 32, 32); QVERIFY(parsed.Succeeded() && parsed.value);
    QCOMPARE(parsed.value->width, 32u); QCOMPARE(parsed.value->height, 32u); QCOMPARE(parsed.value->intensities.size(), std::size_t{1024});
    for (const auto& member : members) if (member.name != "war3map.shd" && member.name != "war3map.mmp")
        QCOMPARE(saved.value->ReadMember(member.name).value, std::optional(member.data));
    QVERIFY(project.CommitMapSave(*job)); QVERIFY(!project.HasUnsavedMapChanges());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::TerrainShadowWorkerOrdinarySavePreserves_data() {
    QTest::addColumn<bool>("caster"); QTest::addColumn<bool>("automatic");
    QTest::newRow("plain-preserve-no-direction") << false << false;
    QTest::newRow("plain-automatic-no-direction") << false << true;
    QTest::newRow("caster-preserve-no-direction") << true << false;
    QTest::newRow("caster-automatic-no-direction") << true << true;
}

void EditorProjectTests::TerrainShadowWorkerOrdinarySavePreserves() {
    QFETCH(bool, caster); QFETCH(bool, automatic);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto members = TerrainShadowQtMembers(false, caster); QVERIFY(!members.empty());
    const auto source = std::filesystem::path(root.filePath("ordinary-shadow.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("ordinary-copy.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    project.war3GameRoot_ = TerrainShadowQtGame(root.path(), false); QVERIFY(!project.war3GameRoot_.empty());
    auto job = FreezeNativePathingQtJob(project, target, automatic ? Services::TerrainMinimapSaveChoice::Automatic
        : Services::TerrainMinimapSaveChoice::Preserve);
    QVERIFY(job && !job->rebuildTerrainShadow && !job->terrainShadow);
    EditorProject::RunMapSave(*job); QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
    QVERIFY(!job->rebuildTerrainShadow && !job->terrainShadow);
    QVERIFY(std::none_of(job->replacements.begin(), job->replacements.end(),
        [](const auto& member) { return member.name == "war3map.shd"; }));
    auto saved = MapArchiveSession::Open(target); QVERIFY(saved.Succeeded() && saved.value);
    for (const auto& member : members) if (member.name != "war3map.mmp")
        QCOMPARE(saved.value->ReadMember(member.name).value, std::optional(member.data));
    QVERIFY(!HasReadDiagnostic(*job, "TERRAIN_SHADOW_LIGHT")
        && !HasReadDiagnostic(*job, "TERRAIN_SHADOW_HEIGHT_BOUNDS_UNKNOWN"));
    QVERIFY(project.CommitMapSave(*job)); QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::TerrainShadowWorkerFailureCannotUseMinimapFallback_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("caster-no-bounds-with-author-tga") << QStringLiteral("caster-existing");
    QTest::newRow("caster-no-bounds-without-minimap") << QStringLiteral("caster-missing");
    QTest::newRow("missing-direction") << QStringLiteral("no-direction");
    QTest::newRow("conflicting-explicit-minimap") << QStringLiteral("minimap-conflict");
}

void EditorProjectTests::TerrainShadowWorkerFailureCannotUseMinimapFallback() {
    QFETCH(QString, mode);
    QTemporaryDir root; QVERIFY(root.isValid());
    const bool caster = mode.startsWith("caster-");
    const auto members = TerrainShadowQtMembers(false, caster, mode != "caster-missing"); QVERIFY(!members.empty());
    const auto source = std::filesystem::path(root.filePath("unsupported-shadow.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("must-not-publish.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    project.war3GameRoot_ = TerrainShadowQtGame(root.path(), mode != "no-direction"); QVERIFY(!project.war3GameRoot_.empty());
    const auto generation = project.editRevision_.Generation(), revision = project.editRevision_.Revision();
    const auto terrain = project.mapEdits_->Terrain(); const auto archiveSha = project.archiveSession_->SourceSha256();
    auto job = FreezeNativePathingQtJob(project, target, mode == "minimap-conflict"
        ? Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly : Services::TerrainMinimapSaveChoice::Automatic);
    QVERIFY(job && !job->rebuildTerrainShadow); job->rebuildTerrainShadow = true;
    EditorProject::RunMapSave(*job);
    const auto expectedCode = caster ? "TERRAIN_SHADOW_HEIGHT_BOUNDS_UNKNOWN"
        : mode == "no-direction" ? "TERRAIN_SHADOW_LIGHT" : "TERRAIN_SHADOW_MINIMAP_CONFLICT";
    QVERIFY2(!job->succeeded && HasReadDiagnostic(*job, expectedCode), ReadDiagnosticText(*job).c_str());
    QVERIFY(job->executed && !job->nextSession && job->written.empty() && job->backup.empty() && !job->terrainShadow);
    QVERIFY(!job->minimapSave && !std::filesystem::exists(target));
    // 原TGA的合法保留guard可能已安装；它不能把随后显式阴影失败改成成功或发布。
    if (mode == "caster-missing" || mode == "minimap-conflict") QVERIFY(!job->minimapPreserve);
    QVERIFY(!HasReadDiagnostic(*job, "MINIMAP_GENERATION_SKIPPED"));
    QVERIFY(!project.CommitMapSave(*job));
    QCOMPARE(project.editRevision_.Generation(), generation); QCOMPARE(project.editRevision_.Revision(), revision);
    QCOMPARE(project.mapEdits_->Terrain(), terrain); QCOMPARE(project.archiveSession_->SourceSha256(), archiveSha);
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::AutomaticMinimapDefaultPrepareAndSaveCopy_data() {
    QTest::addColumn<bool>("special"); QTest::addColumn<bool>("existing");
    QTest::addColumn<int>("pathingPhase"); QTest::addColumn<bool>("injectResource");
    QTest::newRow("generated") << false << false << 0 << false;
    QTest::newRow("unsupported-missing") << true << false << 0 << false;
    QTest::newRow("unsupported-existing") << true << true << 0 << true;
    QTest::newRow("worker-add-apr2") << false << false << 1 << false;
    QTest::newRow("worker-withdraw-apr3") << false << false << 2 << false;
    QTest::newRow("worker-bad-apr-identity") << false << false << 3 << false;
    QTest::newRow("worker-saved-apr3-and-placement-height") << true << true << 4 << false;
    QTest::newRow("worker-saved-apr3-unexplained-source-rejected") << true << true << 4 << true;
}

void EditorProjectTests::AutomaticMinimapDefaultPrepareAndSaveCopy() {
    QFETCH(bool, special); QFETCH(bool, existing); QFETCH(int, pathingPhase); QFETCH(bool, injectResource);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = DeclaredMissingMinimapQtMembers();
    for (auto& member : members) if (member.name == "war3map.w3i")
        member.data[73] |= std::byte{4}; // v25 flags0x400，使用已声明的补足相机边距
    const auto text = [](std::string_view value) {
        const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    for (auto& member : members) if (member.name == "TerrainArt\\CliffTypes.slk")
        member.data = text("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nC;X2;Y1;K\"texDir\"\nC;X3;Y1;K\"texFile\"\n"
            "C;X4;Y1;K\"cliffModelDir\"\nC;X5;Y1;K\"rampModelDir\"\nC;X6;Y1;K\"groundTile\"\nC;X7;Y1;K\"cliffClass\"\n"
            "C;X1;Y2;K\"Cl01\"\nC;X2;Y2;K\"Fixture\"\nC;X3;Y2;K\"Cliff\"\n"
            "C;X4;Y2;K\"Cliff\"\nC;X5;Y2;K\"Ramp\"\nC;X6;Y2;K\"Tg01\"\nC;X7;Y2;K\"0\"\nE\n");
    const auto ground = std::find_if(members.begin(), members.end(), [](const auto& member) { return member.name == "Fixture\\Ground.blp"; });
    QVERIFY(ground != members.end());
    const auto cliffBytes = ground->data;
    members.push_back({"Fixture\\Cliff.blp", cliffBytes});
    members.push_back({"UI\\MiscData.txt", text("[Water]\nDeepLevelPathing=64\n")});
    members.push_back({"UI\\WorldEditData.txt", text("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")});
    const auto oldMinimap = text("author minimap bytes must survive unsupported generation");
    if (existing) members.push_back({"war3mapMap.blp", oldMinimap});
    if (special) {
        // 无关导入必须由实际表证明不在静态依赖闭包内，不能仅凭NewIcon名字放行。
        // special不进入普通小地图层；能力不足必须实际来自普通对象所需的InvalidModel配置缺失。
        members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable(
            {"destructableID", "code", "walkable", "pathTex", "fixedRot"}, {{"Dst0", "Dst0", "0", "none", "0"}})});
        members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable(
            {"doodID", "code", "pathTex", "category", "fixedRot", "showInMM", "useMMColor", "MMRed", "MMGreen", "MMBlue", "file", "numVar"},
            {{"SPC1", "SPC1", "none", "S", "0", "0", "0", "0", "0", "0", "", "1"},
                {"d001", "d001", "none", "S", "0", "1", "1", "220", "40", "10", "", "1"}})});
        members.push_back({"Units/DestructableMetaData.slk", NativeDoodadQtTable(
            {"ID", "field", "slk", "type", "repeat", "data", "index"},
            {{"bwal", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
        members.push_back({"Doodads/DoodadMetaData.slk", NativeDoodadQtTable(
            {"ID", "field", "slk", "type", "repeat", "data", "index"},
            {{"dptx", "pathTex", "DoodadData", "pathingTexture", "0", "0", "0"}})});
    }
    if (special) {
        MapData::DoodadPlacements doodads;
        MapData::SpecialDoodadPlacement placement; placement.rawcode = "SPC1";
        doodads.specialDoodads.push_back(placement);
        MapData::DoodadPlacement tree;
        tree.rawcode = "d001"; tree.creationId = 1; tree.flags = 2;
        tree.position = {384, 512, 5}; tree.scale = {1, 1, 1};
        doodads.doodads.push_back(tree);
        if (pathingPhase == 4) {
            MapData::UnitPlacements units;
            MapData::UnitPlacement unit;
            unit.rawcode = "u001"; unit.creationId = 2; unit.flags = 2;
            unit.position = {384, 512, 10}; unit.scale = {1, 1, 1}; units.units.push_back(unit);
            const auto unitBytes = units.Serialize(); QVERIFY(unitBytes.Succeeded());
            members.push_back({"war3mapUnits.doo", *unitBytes.value});
        }
        const auto bytes = doodads.Serialize(); QVERIFY(bytes.Succeeded() && bytes.value);
        members.push_back({"war3map.doo", *bytes.value});
    }
    const auto source = std::filesystem::path(root.filePath("original-plain.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("default-save-copy.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    // 原三行走生产Prepare；新增行只测真实worker恢复APR，不绕过并冒称脚本工具链验收。
    const auto freezePathingJob = [&](const std::filesystem::path& destination) -> std::unique_ptr<MapSaveJob> {
        auto job = std::make_unique<MapSaveJob>();
        if (!job->FreezeArchive(*project.archiveSession_).Succeeded()) return {};
        auto plan = MapData::TerrainSavePlan::Prepare(*project.archiveSession_, *project.mapEdits_);
        auto declaration = project.mapEdits_->SerializePathingRestrictionDraft();
        if (!plan.Succeeded() || !plan.value || !plan.value->SaveGate().Succeeded()
            || !declaration.Succeeded() || !declaration.value) return {};
        job->target = destination; job->replacements = project.mapEdits_->Replacements();
        job->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>(std::move(*declaration.value));
        // 与生产Prepare相同：冻结完整输入，由worker按同一会话建立派生计划。
        job->terrainDerivationRequired = MapData::TerrainSavePlan::HasTerrainChanges(*project.mapEdits_);
        if (job->terrainDerivationRequired) job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
        job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(), project.editRevision_.Current()};
        const auto* task = job.get();
        job->beforePublish = [task] {
            return task->terrainSavePlan->VerifyFrozenOutput(task->sourceSha256, task->replacements, task->removals);
        };
        return job;
    };
    if (pathingPhase) {
        MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::PathingRestriction;
        brush.size = 1; brush.pathingRestrictions = 4;
        const MapData::TerrainStrokePoint point{48, 48};
        QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded());
        QVERIFY(project.ObserveMapDataEdit("自动小地图通行限制夹具"));
        if (pathingPhase == 2 || pathingPhase == 4) {
            auto first = freezePathingJob(std::filesystem::path(root.filePath("owned-restriction.w3x").toStdWString()));
            QVERIFY(first); EditorProject::RunMapSave(*first);
            QVERIFY(first->succeeded && project.CommitMapSave(*first));
            if (pathingPhase == 2) {
                brush.value = false;
                QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded());
                QVERIFY(project.ObserveMapDataEdit("自动小地图撤回已保存限制夹具"));
            } else {
                MapData::TerrainBrush height; height.kind = MapData::TerrainBrushKind::Raise;
                height.radius = 0; height.strength = 32;
                const MapData::TerrainStrokePoint center{384, 512}, extra{80, 48};
                QVERIFY(project.mapEdits_->EditTerrain(height, std::span(&center, 1), true).Succeeded());
                QVERIFY(project.ObserveMapDataEdit("自动小地图摆放跟随高度"));
                brush.pathingRestrictions = 8;
                QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&extra, 1), false).Succeeded());
                QVERIFY(project.ObserveMapDataEdit("自动小地图追加限制"));
                QCOMPARE(project.mapEdits_->Replacements().size(), std::size_t{4});
            }
        }
    }
    auto job = pathingPhase ? freezePathingJob(target) : project.PrepareMapSave(target, false);
    QVERIFY(job);
    if (pathingPhase) {
        job->minimapChoice = Services::TerrainMinimapSaveChoice::Automatic;
        QVERIFY(job->pathingRestrictionDraft && !job->pathingRestrictionDraft->empty());
        QCOMPARE((*job->pathingRestrictionDraft)[3], (pathingPhase == 2 || pathingPhase == 4) ? std::byte{3} : std::byte{2});
        if (pathingPhase == 3) {
            auto wrong = *job->pathingRestrictionDraft; wrong[4] ^= std::byte{1};
            job->pathingRestrictionDraft = std::make_shared<const std::vector<std::byte>>(std::move(wrong));
        }
    }
    QCOMPARE(job->minimapChoice, Services::TerrainMinimapSaveChoice::Automatic);
    QVERIFY(!job->minimapSave);
    // 新资源通过同一生产 worker 输入触发自动判定，不伪造生成结果。
    if (injectResource) job->replacements.push_back({"Fixture\\NewIcon.blp", text("unused resource")});
    const auto revisionBeforeRun = project.mapEdits_->Revision();
    const auto commandBeforeRun = project.mapEdits_->LastEdit().CommandIdentity();
    const auto archiveBeforeRun = project.archiveSession_->SourceSha256();
    EditorProject::RunMapSave(*job);
    QCOMPARE(project.archiveSession_->SourceSha256(), archiveBeforeRun);
    QCOMPARE(project.mapEdits_->Revision(), revisionBeforeRun);
    QCOMPARE(project.mapEdits_->LastEdit().CommandIdentity(), commandBeforeRun);
    if (pathingPhase == 4 && injectResource) {
        // 此夹具故意保留缺少通行字段的表，不能把它用作
        // 已完整解释旧WPM的混合保存正例。真实资源混合保存由独立专项覆盖。
        QVERIFY(!job->succeeded && HasReadDiagnostic(*job, "TERRAIN_PATHING_PROPERTY_MISSING"));
        QVERIFY(!job->minimapPreserve && !std::filesystem::exists(target));
        QVERIFY(!project.CommitMapSave(*job));
        QVERIFY(project.mapEdits_->IsDirty() && project.mapEdits_->CanUndo());
        QCOMPARE(Sha256File(source).value, sourceSha.value);
        return;
    }
    if (pathingPhase == 3) {
        QVERIFY(!job->succeeded && HasReadDiagnostic(*job, "PATHING_DRAFT_IDENTITY"));
        QVERIFY(!job->minimapPreserve && !std::filesystem::exists(target));
        QVERIFY(project.mapEdits_->IsDirty() && project.mapEdits_->CanUndo());
        QCOMPARE(Sha256File(source).value, sourceSha.value);
        return;
    }
    QVERIFY2(job->succeeded, job->diagnostics.empty() ? "default automatic save failed" : job->diagnostics.back().message.c_str());
    if (special) {
        QVERIFY(!job->minimapSave && job->minimapPreserve);
        QVERIFY(HasReadDiagnostic(*job, "CLASSIC_OBJECT_LAYER_INVALID_MODEL_CONFIG_MISSING"));
        QVERIFY(HasReadDiagnostic(*job, "MINIMAP_GENERATION_SKIPPED"));
        QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        QVERIFY(project.CommitMapSave(*job));
        if (pathingPhase == 4) {
            QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[2], 42.0f);
            QCOMPARE(project.mapEdits_->Snapshot().Doodads()->doodads[0].position[2], 37.0f);
            QCOMPARE(project.mapEdits_->InspectTerrainPoint({48, 48}).value->pathingFlags, 0x86);
            QCOMPARE(project.mapEdits_->InspectTerrainPoint({80, 48}).value->pathingFlags, 0x8a);
        }
        auto saved = MapArchiveSession::Open(target); QVERIFY(saved.Succeeded() && saved.value);
        if (injectResource) QCOMPARE(saved.value->ReadMember("Fixture\\NewIcon.blp").value, std::optional(text("unused resource")));
        QCOMPARE(saved.value->HasMember("war3mapMap.blp"), existing);
        if (existing) QCOMPARE(saved.value->ReadMember("war3mapMap.blp").value, std::optional(oldMinimap));
        QCOMPARE(Sha256File(source).value, sourceSha.value);
        return;
    }
    QVERIFY(job->minimapSave && job->minimapSave->ClassicManifest());
    QCOMPARE(job->minimapSave->Manifest().encodedMipCount, 1u);
    QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    const auto generated = job->minimapSave->MinimapReplacement().data;
    QVERIFY(project.CommitMapSave(*job));
    QVERIFY(!project.HasUnsavedMapChanges());
    auto saved = MapArchiveSession::Open(target); QVERIFY(saved.Succeeded());
    QCOMPARE(saved.value->ReadMember("war3mapMap.blp").value, std::optional(generated));
    if (pathingPhase) {
        const auto savedPathing = saved.value->ReadMember("war3map.wpm"); QVERIFY(savedPathing.Succeeded());
        QCOMPARE((*savedPathing.value)[16 + 33], pathingPhase == 2 ? std::byte{0x82} : std::byte{0x86});
    }
    QCOMPARE(Sha256File(source).value, sourceSha.value);
    // 第二次没有任何修改时沿用上次BLP，不再读配置、生成噪点或编码。
    const auto secondTarget = std::filesystem::path(root.filePath("unchanged-save-copy.w3x").toStdWString());
    auto second = project.PrepareMapSave(secondTarget, false); QVERIFY(second);
    EditorProject::RunMapSave(*second);
    QVERIFY2(second->succeeded, second->diagnostics.empty() ? "unchanged save failed" : second->diagnostics.back().message.c_str());
    QVERIFY(!second->minimapSave);
    auto unchanged = MapArchiveSession::Open(secondTarget); QVERIFY(unchanged.Succeeded());
    QCOMPARE(unchanged.value->ReadMember("war3mapMap.blp").value, std::optional(generated));
}

void EditorProjectTests::AutomaticMinimapFinalSettingsDerivedWorkerAndCommit_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("height-and-name-save-as") << false;
    QTest::newRow("height-and-name-in-place") << true;
}

void EditorProjectTests::AutomaticMinimapFinalSettingsDerivedWorkerAndCommit() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = DeclaredMissingMinimapQtMembers();
    members[0].data[73] |= std::byte{4}; // flags0x400，原始可玩矩形[1,2,5,7]。
    for (auto& member : members) if (member.name == "TerrainArt\\CliffTypes.slk")
        member.data = NativeDoodadQtTable(
            {"cliffID", "texDir", "texFile", "cliffModelDir", "rampModelDir", "groundTile", "cliffClass"},
            {{"Cl01", "Fixture", "Cliff", "Cliff", "Ramp", "Tg01", "0"}});
    const auto atlas = std::find_if(members.begin(), members.end(), [](const auto& member) {
        return member.name == "Fixture\\Ground.blp";
    });
    QVERIFY(atlas != members.end());
    const auto cliff = atlas->data;
    members.push_back({"Fixture\\Cliff.blp", cliff});
    members.push_back({"UI\\MiscData.txt", Tests::WaterText("[Water]\nDeepLevelPathing=64\n")});
    members.push_back({"UI\\WorldEditData.txt", Tests::WaterText("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")});
    MapData::UnitPlacements units;
    MapData::UnitPlacement start; start.rawcode = "sloc"; start.position = {512, 384, 0};
    start.scale = {1, 1, 1}; start.owner = 0; start.creationId = 1; units.units.push_back(start);
    const auto unitBytes = units.Serialize(); QVERIFY(unitBytes.Succeeded() && unitBytes.value);
    members.push_back({"war3mapUnits.doo", *unitBytes.value});
    // 原创单像素TGA；开始位置在4x5可玩区中的MMP坐标独立手算为(179,205)。
    std::vector<std::byte> teamColor(21);
    teamColor[2] = std::byte{2}; teamColor[12] = teamColor[14] = std::byte{1};
    teamColor[16] = std::byte{24}; teamColor[17] = std::byte{0x20};
    teamColor[18] = std::byte{3}; teamColor[19] = std::byte{2}; teamColor[20] = std::byte{1};
    members.push_back({"Fixture\\TeamColor00.tga", std::move(teamColor)});
    const auto source = std::filesystem::path(root.filePath("final-settings-source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto originalSha = Sha256File(source); QVERIFY(originalSha.Succeeded());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", std::vector<MapArchiveMember>{
        {"UI\\war3skins.txt", Tests::WaterText("[Default]\nTeamColor=Fixture/TeamColor\n")}}).Succeeded());
    EditorProject project; QUndoStack undo; project.mapDataUndoStack_ = &undo;
    QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
    const auto session = project.mapEdits_->SessionIdentity();
    const auto parsedSettings = MapData::MapSettings::Parse(members[0].data); QVERIFY(parsedSettings.Succeeded());
    auto settings = *parsedSettings.value; settings.mapName = "Qt final settings and terrain worker fixture";
    QVERIFY(project.mapEdits_->EditSettings(settings).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("同次地形保存修改地图名称"));
    PushMapDataEdit(project, "Map name", "同次地形保存修改地图名称");
    const auto finalSettings = settings.Serialize(); QVERIFY(finalSettings.Succeeded());
    QVERIFY(*finalSettings.value != members[0].data);
    QVERIFY(RaiseDeclaredMinimapProject(project, 10));
    PushMapDataEdit(project, "Terrain height", "同次地图名称与地形高度编辑");
    QCOMPARE(undo.count(), 2); QCOMPARE(undo.index(), 2);
    auto raised = members[1].data;
    // radius0在(384,512)仅抬高vertex(3,4)10世界单位，原始8192变为8232。
    raised[45 + (4 * 9 + 3) * 7] = std::byte{0x28};
    raised[45 + (4 * 9 + 3) * 7 + 1] = std::byte{0x20};
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::optional(raised));
    MapDataFixtures::Fixture expectedMarkers;
    expectedMarkers.UInt(0); expectedMarkers.UInt(1); expectedMarkers.UInt(2);
    expectedMarkers.UInt(179); expectedMarkers.UInt(205);
    expectedMarkers.Byte(3); expectedMarkers.Byte(2); expectedMarkers.Byte(1); expectedMarkers.Byte(255);
    std::optional<std::vector<std::byte>> firstMinimap;
    for (int phase = 0; phase < 3; ++phase) {
        if (phase == 1) { undo.undo(); QCOMPARE(undo.index(), 1); QVERIFY(undo.canRedo()); }
        else if (phase == 2) { undo.redo(); QCOMPARE(undo.index(), 2); QVERIFY(undo.canUndo()); }
        const auto& expectedTerrain = phase == 1 ? members[1].data : raised;
        const auto current = project.mapEdits_->Snapshot(); QVERIFY(current.Settings());
        QCOMPARE(current.Settings()->mapName, settings.mapName);
        QCOMPARE(current.Settings()->Serialize().value, finalSettings.value);
        QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::optional(expectedTerrain));
        QVERIFY(project.HasUnsavedMapChanges());
        const auto beforeSource = project.sourceArchivePath_;
        const auto beforeSha = Sha256File(beforeSource); QVERIFY(beforeSha.Succeeded());
        const auto beforeRevision = project.mapEdits_->Revision();
        const auto beforeSourceRevision = project.mapEdits_->TerrainSourceRevision();
        const auto target = inPlace ? beforeSource
            : std::filesystem::path(root.filePath(QString("final-settings-saved-%1.w3x").arg(phase)).toStdWString());
        // 沿既有脚本准备夹具；本例只证明真实Qt派生worker/回读/Commit，
        // 不冒充生产Prepare或外部脚本工具链验收。快照直接来自实际当前会话。
        auto job = FreezeNativePathingQtJob(project, target, Services::TerrainMinimapSaveChoice::Automatic);
        QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = project.mapEdits_->FreezeForSave();
        const auto frozen = job->terrainSessionSnapshot->Snapshot(); QVERIFY(frozen.Settings());
        QCOMPARE(frozen.Settings()->Serialize().value, finalSettings.value);
        if (inPlace) {
            job->inPlace = true;
            job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath(QString("backups-%1").arg(phase)).toStdWString());
            QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
        }
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->nextSession && job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        QVERIFY(job->minimapSave && job->minimapSave->ClassicManifest() && !job->minimapPreserve);
        QVERIFY(!HasReadDiagnostic(*job, "MINIMAP_GENERATION_SKIPPED"));
        QCOMPARE(job->minimapSave->Plan(), job->terrainSavePlan);
        QCOMPARE(job->terrainSavePlan->Revision(), beforeRevision);
        QCOMPARE(job->terrainSavePlan->TerrainSourceRevision(), beforeSourceRevision);
        QVERIFY((job->minimapSave->ClassicManifest()->sourceRectangle == std::array<std::uint32_t, 4>{1, 2, 5, 7}));
        QCOMPARE(job->minimapSave->Manifest().encodedMipCount, 1u);
        QCOMPARE(job->minimapSave->DecodedMip0().width, 256u); QCOMPARE(job->minimapSave->DecodedMip0().height, 256u);
        QVERIFY(job->minimapMarkers && job->minimapMarkers->MarkerCount() == 1);
        const auto dependencies = job->minimapMarkers->Dependencies();
        const auto settingsDependency = std::find_if(dependencies.begin(), dependencies.end(), [](const auto& dependency) {
            return dependency.name == "WAR3MAP.W3I";
        });
        QVERIFY(settingsDependency != dependencies.end());
        QCOMPARE(std::optional(settingsDependency->sha256), Sha256(*finalSettings.value).value);
        // worker不能提前修改前台基线；最终三个文件由真实输出归档独立回读。
        QCOMPARE(project.mapEdits_->Revision(), beforeRevision);
        QCOMPARE(project.mapEdits_->TerrainSourceRevision(), beforeSourceRevision);
        const auto saved = MapArchiveSession::Open(target); QVERIFY(saved.Succeeded());
        QCOMPARE(saved.value->ReadMember("war3map.w3i").value, finalSettings.value);
        QCOMPARE(saved.value->ReadMember("war3map.w3e").value, std::optional(expectedTerrain));
        QCOMPARE(saved.value->ReadMember("war3map.wpm").value, std::optional(members[2].data));
        QCOMPARE(saved.value->ReadMember("war3map.mmp").value, std::optional(expectedMarkers.bytes));
        const auto generated = job->minimapSave->MinimapReplacement().data;
        QCOMPARE(saved.value->ReadMember("war3mapMap.blp").value, std::optional(generated));
        QVERIFY(job->minimapSave->VerifyReadback(*job, *saved.value).Succeeded());
        QVERIFY(job->minimapMarkers->VerifyReadback(*saved.value).Succeeded());
        if (phase == 0) firstMinimap = generated;
        else if (phase == 2) QCOMPARE(std::optional(generated), firstMinimap);
        QVERIFY(project.CommitMapSave(*job));
        QCOMPARE(project.mapEdits_->SessionIdentity(), session); QCOMPARE(project.sourceArchivePath_, target);
        QVERIFY(project.mapEdits_->TerrainSourceRevision() > beforeSourceRevision);
        QVERIFY(!project.mapEdits_->IsDirty() && !project.HasUnsavedMapChanges());
        QCOMPARE(project.mapEdits_->Snapshot().Settings()->mapName, settings.mapName);
        QCOMPARE(undo.count(), 2); QCOMPARE(undo.index(), phase == 1 ? 1 : 2);
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, beforeSha.value); }
        else QCOMPARE(Sha256File(beforeSource).value, beforeSha.value);
    }
    if (!inPlace) QCOMPARE(Sha256File(source).value, originalSha.value);
}

void EditorProjectTests::MissingMinimapWorkerReadbackGatesCommit_data() {
    QTest::addColumn<QString>("mode");
    for (const auto* mode : {"match", "missing", "rejected", "stale", "revision", "repeat"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void EditorProjectTests::MissingMinimapWorkerReadbackGatesCommit() {
    QFETCH(QString, mode);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("candidate.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, MissingMinimapQtMembers()).Succeeded());
    const auto originalSha = Sha256File(source); QVERIFY(originalSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); QVERIFY(RaiseTerrainSaveProject(project));
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto* oldArchive = project.archiveSession_.get();
    project.mapDataUndoStack_->push(new QUndoCommand("missing minimap undo marker"));
    const auto undoCount = project.mapDataUndoStack_->count();
    // 内部job夹具只验证真实worker→回读→Commit；未绕过并冒称生产Prepare脚本门。
    auto job = std::make_unique<MapSaveJob>(); QVERIFY(job->FreezeArchive(*project.archiveSession_).Succeeded());
    job->target = target; job->replacements = project.mapEdits_->Replacements();
    job->minimapChoice = Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly;
    job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(),
        project.editRevision_.Current(), project.luaMapTargetGeneration_};
    QVERIFY(!job->minimapSave && !job->terrainSavePlan && !job->terrainReadbackStatus && job->replacements.size() == 1);
    std::atomic<unsigned> previousCalls{0}, generateCalls{0};
    QThread* generationThread = nullptr;
    job->beforePublish = [&previousCalls] { ++previousCalls; return Status::Ok(); };
    job->progress = [&](const QString& text) {
        if (text == EditorProject::tr("正在更新地形通行与小地图…")) { generationThread = QThread::currentThread(); ++generateCalls; }
    };
    std::unique_ptr<QThread> worker(QThread::create([task = job.get()] { EditorProject::RunMapSave(*task); }));
    worker->start();
    const auto finished = worker->wait(60000);
    if (!finished) {
        // 先收束真实线程，再让QVERIFY返回；不能析构运行中的worker或悬空其task/回调。
        worker->requestInterruption(); worker->quit(); worker->wait();
    }
    QVERIFY2(finished, "原创小地图worker超过测试期限；已等待线程收束，未强杀任务");
    QVERIFY2(job->succeeded, job->diagnostics.empty() ? "worker failed without diagnostics" : job->diagnostics.back().message.c_str());
    QVERIFY(generationThread == worker.get() && generationThread != QThread::currentThread());
    QCOMPARE(generateCalls.load(), 1u); QVERIFY(previousCalls.load() >= 1);
    QVERIFY(job->nextSession && job->minimapSave && job->terrainSavePlan == job->minimapSave->Plan());
    QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    QCOMPARE(job->terrainReadbackArchiveSha256, job->nextSession->SourceSha256());
    QVERIFY(job->terrainSavePlan->SessionIdentity() != project.mapEdits_->SessionIdentity());
    const auto independentlyOpened = MapArchiveSession::Open(target); QVERIFY(independentlyOpened.Succeeded());
    QVERIFY(job->minimapSave->VerifyReadback(*job, *independentlyOpened.value).Succeeded());
    QVERIFY(!job->minimapSave->Manifest().formalSaveAuthorized && !job->minimapSave->Manifest().nativeEditorOrGameVerified);
    if (mode == "missing") job->terrainReadbackStatus.reset();
    else if (mode == "rejected") {
        job->terrainReadbackStatus = Status::Failure("Test", "OWNED_READBACK_REJECTION", "自有夹具拒绝回读提交"); job->succeeded = false;
    } else if (mode == "stale") job->terrainReadbackArchiveSha256.assign(64, '0');
    else if (mode == "revision") QVERIFY(RaiseTerrainSaveProject(project, 1));
    else if (mode == "repeat") {
        EditorProject::RunMapSave(*job);
        QVERIFY(!job->succeeded && !job->terrainReadbackStatus && job->terrainReadbackArchiveSha256.empty() && !job->minimapSave);
        QVERIFY(HasReadDiagnostic(*job, "SAVE_TASK_ALREADY_EXECUTED")); QCOMPARE(generateCalls.load(), 1u);
    }
    const auto saved = project.CommitMapSave(*job);
    if (mode == "match") {
        QVERIFY(saved); QCOMPARE(project.archiveSession_->SourceSha256(), independentlyOpened.value->SourceSha256());
        QVERIFY(!project.HasUnsavedMapChanges());
    } else {
        QVERIFY(!saved && project.archiveSession_.get() == oldArchive && project.HasUnsavedMapChanges());
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        QCOMPARE(project.mapDataUndoStack_->count(), undoCount);
        QVERIFY(std::filesystem::exists(target));
    }
    QCOMPARE(Sha256File(source).value, originalSha.value);
}

void EditorProjectTests::MissingMinimapPrepareKeepsDefaultAndRejectsMixedChanges() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("source.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("candidate.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, MissingMinimapQtMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); QVERIFY(RaiseTerrainSaveProject(project));
    const auto choice = Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly;
    QVERIFY(!project.PrepareMapSave(target, true, choice));
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_TARGET"));
    project.objectDirty_[0] = true; QVERIFY(!project.PrepareMapSave(target, false, choice)); project.objectDirty_[0] = false;
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_DOMAIN"));
    project.pendingImports_.push_back({QStringLiteral("invalid.lua"), root.filePath("missing.lua"), 7});
    QVERIFY(!project.PrepareMapSave(target, false, choice));
    QVERIFY(!ModelHasCode(*project.diagnosticsModel_, "MAP_LUA_READ")); project.pendingImports_.clear();
    project.triggerDirty_ = true; QVERIFY(!project.PrepareMapSave(target, false, choice)); project.triggerDirty_ = false;
    project.compilerDataRoot_.clear(); project.activeUiDataRoot_.clear(); project.uiDataRoot_.clear();
    QVERIFY(!project.PrepareMapSave(target, false, choice));
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "REPACK_SCRIPT_REJECTED"));
    // 没有工具链时，Generate不能提前运行generator；原普通保存门也仍要求工具链。
    QVERIFY(!project.PrepareMapSave(target, false)); QVERIFY(project.HasUnsavedMapChanges() && !std::filesystem::exists(target));
    MapSaveJob untouched; QVERIFY(untouched.minimapChoice == Services::TerrainMinimapSaveChoice::Preserve);
}

void EditorProjectTests::MissingMinimapWorkerFailureConsumesTask() {
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = MissingMinimapQtMembers(); members.push_back({"war3mapMap.blp", {}});
    const auto source = std::filesystem::path(root.filePath("present-empty.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); QVERIFY(RaiseTerrainSaveProject(project));
    MapSaveJob job; QVERIFY(job.FreezeArchive(*project.archiveSession_).Succeeded());
    job.target = std::filesystem::path(root.filePath("rejected.w3x").toStdWString()); job.replacements = project.mapEdits_->Replacements();
    job.minimapChoice = Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly;
    EditorProject::RunMapSave(job);
    QVERIFY(job.executed && !job.succeeded && !job.nextSession && !job.terrainReadbackStatus && !job.minimapSave);
    QVERIFY(HasReadDiagnostic(job, "MINIMAP_SAVE_NOT_MISSING")); QVERIFY(!std::filesystem::exists(job.target));
    EditorProject::RunMapSave(job); QVERIFY(HasReadDiagnostic(job, "SAVE_TASK_ALREADY_EXECUTED"));
    QVERIFY(!project.CommitMapSave(job) && project.HasUnsavedMapChanges());
}

void EditorProjectTests::DeclaredMissingMinimapWorkerReadbackGatesCommit_data() {
    QTest::addColumn<QString>("mode");
    for (const auto* mode : {"match", "missing", "rejected", "stale", "revision", "repeat",
        "helper_missing", "plan_mismatch", "wrong_policy", "preserve_choice", "unknown_choice"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void EditorProjectTests::DeclaredMissingMinimapWorkerReadbackGatesCommit() {
    QFETCH(QString, mode);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("declared-source.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("declared-candidate.w3x").toStdWString());
    const auto members = DeclaredMissingMinimapQtMembers(); const auto originalTerrain = members[1].data;
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto originalSha = Sha256File(source); QVERIFY(originalSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); QVERIFY(RaiseDeclaredMinimapProject(project));
    QVERIFY(project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto* oldArchive = project.archiveSession_.get();
    project.mapDataUndoStack_->push(new QUndoCommand("declared minimap undo marker"));
    const auto undoCount = project.mapDataUndoStack_->count();
    // 此内部job只覆盖真实worker/回读/Commit；正式Prepare/脚本门另有scope和可选窗口测试。
    auto job = std::make_unique<MapSaveJob>(); QVERIFY(job->FreezeArchive(*project.archiveSession_).Succeeded());
    job->target = target; job->replacements = project.mapEdits_->Replacements();
    job->minimapChoice = Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly;
    job->preparedRevision = MapSaveRevisionBinding{project.editRevision_.Generation(), project.editRevision_.Revision(),
        project.editRevision_.Current(), project.luaMapTargetGeneration_};
    QVERIFY(job->replacements.size() == 1 && !job->minimapSave && !job->terrainSavePlan && !job->terrainReadbackStatus);
    const auto currentTerrain = job->replacements.front().data;
    QVERIFY(currentTerrain.size() == originalTerrain.size() && currentTerrain != originalTerrain);
    for (std::size_t vertex = 0; vertex < 81; ++vertex) {
        const auto at = 45 + vertex * 7;
        QVERIFY(std::equal(currentTerrain.begin() + at + 2, currentTerrain.begin() + at + 7, originalTerrain.begin() + at + 2));
    }
    std::atomic<unsigned> previousCalls{0}, generateCalls{0}; QThread* generationThread = nullptr;
    job->beforePublish = [&previousCalls] { ++previousCalls; return Status::Ok(); };
    job->progress = [&](const QString& text) {
        if (text == EditorProject::tr("正在更新地形通行与小地图…")) { generationThread = QThread::currentThread(); ++generateCalls; }
    };
    std::unique_ptr<QThread> worker(QThread::create([task = job.get()] { EditorProject::RunMapSave(*task); }));
    worker->start(); const auto finished = worker->wait(60000);
    if (!finished) { worker->requestInterruption(); worker->quit(); worker->wait(); }
    QVERIFY2(finished, "声明边距小地图worker超过测试期限；已等待真实线程收束，未强杀或释放其task");
    QVERIFY2(job->succeeded, job->diagnostics.empty() ? "worker failed without diagnostics" : job->diagnostics.back().message.c_str());
    QVERIFY(generationThread == worker.get() && generationThread != QThread::currentThread());
    QCOMPARE(generateCalls.load(), 1u); QVERIFY(previousCalls.load() >= 1);
    QVERIFY(job->nextSession && job->minimapSave && job->terrainSavePlan == job->minimapSave->Plan());
    QVERIFY(job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    QCOMPARE(job->terrainReadbackArchiveSha256, job->nextSession->SourceSha256());
    QVERIFY(job->terrainSavePlan->SessionIdentity() != project.mapEdits_->SessionIdentity());
    const auto& manifest = job->minimapSave->Manifest();
    QVERIFY(manifest.rangePolicy == Render::TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge);
    QCOMPARE(manifest.mapEdge.state, std::string("Validated"));
    QCOMPARE(manifest.mapEdge.sourceTileAnchors, std::size_t{64});
    QCOMPARE(manifest.mapEdge.declaredTileAnchors, std::size_t{20});
    QCOMPARE(manifest.mapEdge.exteriorMapEdgeAnchors, std::size_t{44});
    QCOMPARE(manifest.mapEdge.terminalClearVertices, std::size_t{17});
    QVERIFY((manifest.renderExtent == std::array<double, 4>{128, 256, 640, 896}));
    QVERIFY(!manifest.formalSaveAuthorized && !manifest.nativeEditorOrGameVerified);
    const auto independentlyOpened = MapArchiveSession::Open(target); QVERIFY(independentlyOpened.Succeeded());
    QVERIFY(job->minimapSave->VerifyReadback(*job, *independentlyOpened.value).Succeeded());
    const auto savedTerrain = independentlyOpened.value->ReadMember("war3map.w3e"); QVERIFY(savedTerrain.Succeeded());
    QVERIFY(*savedTerrain.value == currentTerrain);
    const auto savedSettings = independentlyOpened.value->ReadMember("war3map.w3i"); QVERIFY(savedSettings.Succeeded());
    QVERIFY(*savedSettings.value == members[0].data);
    QString expectedCode;
    if (mode == "missing") {
        job->terrainReadbackStatus.reset(); expectedCode = "TERRAIN_SAVE_PLAN_READBACK_MISSING";
    } else if (mode == "rejected") {
        job->terrainReadbackStatus = Status::Failure("Test", "OWNED_DECLARED_READBACK_REJECTION", "原创声明边距夹具拒绝回读提交");
        job->succeeded = false; expectedCode = "TERRAIN_SAVE_PLAN_READBACK_REJECTED";
    } else if (mode == "stale") {
        job->terrainReadbackArchiveSha256.assign(64, '0'); expectedCode = "TERRAIN_SAVE_PLAN_READBACK_STALE";
    } else if (mode == "revision") {
        QVERIFY(RaiseDeclaredMinimapProject(project, 1)); expectedCode = "SAVE_REVISION_CHANGED";
    } else if (mode == "repeat") {
        EditorProject::RunMapSave(*job);
        QVERIFY(!job->succeeded && !job->terrainReadbackStatus && job->terrainReadbackArchiveSha256.empty() && !job->minimapSave);
        QVERIFY(HasReadDiagnostic(*job, "SAVE_TASK_ALREADY_EXECUTED")); QCOMPARE(generateCalls.load(), 1u);
        expectedCode = "MINIMAP_SAVE_READBACK_MISSING";
    } else if (mode == "helper_missing") {
        job->minimapSave.reset(); expectedCode = "MINIMAP_SAVE_READBACK_MISSING";
    } else if (mode == "plan_mismatch") {
        job->terrainSavePlan.reset(); expectedCode = "MINIMAP_SAVE_READBACK_MISSING";
    } else if (mode == "wrong_policy") {
        job->minimapChoice = Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly; expectedCode = "MINIMAP_SAVE_POLICY_MISMATCH";
    } else if (mode == "preserve_choice") {
        job->minimapChoice = Services::TerrainMinimapSaveChoice::Preserve; expectedCode = "MINIMAP_SAVE_POLICY_MISMATCH";
    } else if (mode == "unknown_choice") {
        job->minimapChoice = static_cast<Services::TerrainMinimapSaveChoice>(42); expectedCode = "MINIMAP_SAVE_POLICY_MISMATCH";
    }
    const auto saved = project.CommitMapSave(*job);
    if (mode == "match") {
        QVERIFY(saved); QCOMPARE(project.archiveSession_->SourceSha256(), independentlyOpened.value->SourceSha256());
        QVERIFY(!project.HasUnsavedMapChanges());
        MapSaveJob next; QVERIFY(next.minimapChoice == Services::TerrainMinimapSaveChoice::Preserve && !next.minimapSave);
    } else {
        QVERIFY(!saved && project.archiveSession_.get() == oldArchive && project.HasUnsavedMapChanges());
        QVERIFY(ModelHasCode(*project.diagnosticsModel_, expectedCode));
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
        QCOMPARE(project.mapDataUndoStack_->count(), undoCount); QVERIFY(project.sourceArchivePath_ == source);
        QVERIFY(std::filesystem::exists(target));
    }
    QCOMPARE(Sha256File(source).value, originalSha.value);
}

void EditorProjectTests::DeclaredMissingMinimapPrepareKeepsScopesAndOriginalGates() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto source = std::filesystem::path(root.filePath("declared-scope-source.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("declared-must-not-exist.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, DeclaredMissingMinimapQtMembers()).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto choice = Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly;
    QVERIFY(!project.PrepareMapSave(target, false, choice));
    QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_DOMAIN"));
    QVERIFY(!project.HasUnsavedMapChanges());
    QVERIFY(RaiseDeclaredMinimapProject(project));
    const auto revision = project.editRevision_.Revision(); const auto scopes = project.editRevision_.Current();
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto clear = [&] { project.diagnosticsModel_->removeRows(0, project.diagnosticsModel_->rowCount()); };
    clear(); QVERIFY(!project.PrepareMapSave(target, true, choice)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_TARGET"));
    project.campaignScope_ = Services::CampaignSession::Create(); QVERIFY(project.campaignScope_);
    clear(); QVERIFY(!project.PrepareMapSave(target, false, choice)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_TARGET"));
    project.campaignScope_.reset();
    project.objectDirty_[0] = true;
    clear(); QVERIFY(!project.PrepareMapSave(target, false, choice)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_DOMAIN"));
    project.objectDirty_[0] = false;
    project.pendingImports_.push_back({QStringLiteral("invalid.lua"), root.filePath("missing.lua"), 7});
    clear(); QVERIFY(!project.PrepareMapSave(target, false, choice)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_DOMAIN"));
    QVERIFY(!ModelHasCode(*project.diagnosticsModel_, "MAP_LUA_READ")); project.pendingImports_.clear();
    project.assetEdits_ = {{"Fixture\\Ground.blp", {}}};
    clear(); QVERIFY(!project.PrepareMapSave(target, false, choice)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_DOMAIN"));
    project.assetEdits_.clear(); project.triggerDirty_ = true;
    clear(); QVERIFY(!project.PrepareMapSave(target, false, choice)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "MINIMAP_SAVE_DOMAIN"));
    project.triggerDirty_ = false; project.compilerDataRoot_.clear(); project.activeUiDataRoot_.clear(); project.uiDataRoot_.clear();
    clear(); QVERIFY(!project.PrepareMapSave(target, false, choice)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "REPACK_SCRIPT_REJECTED"));
    clear(); QVERIFY(!project.PrepareMapSave(target, false)); QVERIFY(ModelHasCode(*project.diagnosticsModel_, "REPACK_SCRIPT_REJECTED"));
    QVERIFY(!std::filesystem::exists(target) && project.HasUnsavedMapChanges());
    QCOMPARE(project.editRevision_.Revision(), revision); QCOMPARE(project.editRevision_.Current(), scopes);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
    QCOMPARE(Sha256File(source).value, sourceSha.value);
    MapSaveJob next; QVERIFY(next.minimapChoice == Services::TerrainMinimapSaveChoice::Preserve && !next.minimapSave);
}

void EditorProjectTests::DeclaredMissingMinimapWorkerFailureConsumesTask_data() {
    QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("code");
    QTest::newRow("old-full") << QString("old-full") << QString("TERRAIN_MINIMAP_MAP_EDGE");
    QTest::newRow("mask-hole") << QString("mask-hole") << QString("TERRAIN_MINIMAP_MAP_EDGE_MASK");
    QTest::newRow("missing-w3i") << QString("missing-w3i") << QString("TERRAIN_ASSETS_W3I_MISSING");
    QTest::newRow("present-empty") << QString("present-empty") << QString("MINIMAP_SAVE_NOT_MISSING");
    QTest::newRow("no-height") << QString("no-height") << QString("MINIMAP_SAVE_DOMAIN");
    QTest::newRow("height-gate") << QString("height-gate") << QString("TERRAIN_PATHING_PROPERTY_MISSING");
}

void EditorProjectTests::DeclaredMissingMinimapWorkerFailureConsumesTask() {
    QFETCH(QString, mode); QFETCH(QString, code);
    QTemporaryDir root; QVERIFY(root.isValid()); auto members = DeclaredMissingMinimapQtMembers();
    if (mode == "mask-hole") members[1].data[45 + 3] = std::byte{32}; // 只破坏远离height邻域的原外域SW0,0。
    else if (mode == "missing-w3i") members.erase(members.begin());
    else if (mode == "present-empty") members.push_back({"war3mapMap.blp", {}});
    else if (mode == "height-gate") {
        // R2 已按原版 dry-height 不变性替代旧156 raw幅度阈值。
        // 负例含水而实际Water.slk没有LSha行，在准备通行时即拒绝；不能猜水规则。
        // 仍要求完整拒绝且不产生归档。
        members[1].data[45 + (4 * 9 + 4) * 7 + 4] |= std::byte{0x40};
        // 原版通行先核真实来源DOO，缺失不等价于已解析的空对象集合。
        // 提供原创合法空表，才能到达本负例有意缺失的Water行。
        const auto emptyDoodads = MapData::DoodadPlacements{}.Serialize();
        QVERIFY(emptyDoodads.Succeeded() && emptyDoodads.value);
        members.push_back({"war3map.doo", *emptyDoodads.value});
    }
    const auto source = std::filesystem::path(root.filePath("declared-worker-rejected-source.w3x").toStdWString());
    const auto target = std::filesystem::path(root.filePath("declared-worker-rejected.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    if (mode != "no-height") QVERIFY(RaiseDeclaredMinimapProject(project, mode == "height-gate" ? 40 : 39));
    const auto dirty = project.HasUnsavedMapChanges(); const auto revision = project.editRevision_.Revision();
    const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(baseline);
    const auto undoCount = project.mapDataUndoStack_->count(); const auto* oldArchive = project.archiveSession_.get();
    MapSaveJob job; QVERIFY(job.FreezeArchive(*project.archiveSession_).Succeeded());
    job.target = target; job.replacements = project.mapEdits_->Replacements();
    job.minimapChoice = mode == "old-full" ? Services::TerrainMinimapSaveChoice::GenerateMissingGroundOnly
        : Services::TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly;
    EditorProject::RunMapSave(job);
    QVERIFY(job.executed && !job.succeeded && !job.nextSession && !job.terrainReadbackStatus && !job.minimapSave);
    QVERIFY2(HasReadDiagnostic(job, code.toStdString()), ReadDiagnosticText(job).c_str()); QVERIFY(!std::filesystem::exists(target));
    EditorProject::RunMapSave(job); QVERIFY(HasReadDiagnostic(job, "SAVE_TASK_ALREADY_EXECUTED"));
    QVERIFY(!project.CommitMapSave(job) && project.archiveSession_.get() == oldArchive && project.sourceArchivePath_ == source);
    QCOMPARE(project.HasUnsavedMapChanges(), dirty); QCOMPARE(project.editRevision_.Revision(), revision);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, baseline->scopes);
    QCOMPARE(project.mapDataUndoStack_->count(), undoCount); QCOMPARE(Sha256File(source).value, sourceSha.value);
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

void EditorProjectTests::SourceWatchIgnoresUnrelatedSiblingFiles() {
    QTemporaryDir root;
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(root.filePath(name));
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    QVERIFY(write("map.w3x", "original"));
    EditorProject project;
    project.sourceArchivePath_ = std::filesystem::path(root.filePath("map.w3x").toStdWString());
    project.RefreshProjectWatch();
    // 地图旁边放一份备份：文件夹有变化，但地图本身没变。
    QVERIFY(write("map-backup.w3x", "original"));
    QTest::qWait(1500);
    QVERIFY(!project.sourceChangeSuspected_);
    QVERIFY(write("map.w3x", "replaced by another program"));
    QTRY_VERIFY_WITH_TIMEOUT(project.sourceChangeSuspected_, 4000);
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

void EditorProjectTests::TerrainJournalProjectRoundtripAndStrictManifest() {
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, TerrainSaveMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    QVERIFY(RaiseTerrainSaveProject(project));
    const auto firstTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(firstTerrain.Succeeded());
    QVERIFY(RaiseTerrainSaveProject(project, 16));
    const auto finalTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(finalTerrain.Succeeded());
    const auto target = std::filesystem::path(root.filePath("journal.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    auto opened = MapArchiveSession::Open(target); QVERIFY(opened.Succeeded());
    QVERIFY(opened.value->HasMember("overlay/editor/terrain-strokes.bin"));
    const auto json = opened.value->ReadMember("project.json"); QVERIFY(json.Succeeded());
    auto manifest = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(json.value->data()), static_cast<qsizetype>(json.value->size()))).object();
    QCOMPARE(manifest["terrainStrokeJournal"].toInt(), 1);
    auto prepared = PrepareProjectOpen(target);
    QVERIFY2(prepared.Succeeded(), prepared.diagnostics.empty() ? "restore journal" : prepared.diagnostics.front().message.c_str());
    QCOMPARE((*prepared.value)->terrainReceipts.size(), std::size_t{2});
    QVERIFY((*prepared.value)->terrainJournalWarning.empty());
    EditorProject restored; QVERIFY(BindTerrainSaveProject(restored, source));
    QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    restored.mapEdits_ = std::move((*prepared.value)->mapDataEdits);
    for (const auto& receipt : (*prepared.value)->terrainReceipts)
        undo.push(new MapDataEditCommand(restored, receipt, "Restored terrain", "恢复工程笔划"));
    QCOMPARE(undo.count(), 2); QCOMPARE(undo.index(), 2);
    undo.undo(); QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, firstTerrain.value);
    undo.redo(); QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, finalTerrain.value);

    std::vector<MapArchiveMember> files(opened.value->Members().begin(), opened.value->Members().end());
    for (auto& member : files) if (member.name == "overlay/editor/terrain-strokes.bin") member.data[0] ^= std::byte{1};
    const auto corrupt = std::filesystem::path(root.filePath("corrupt.forge").toStdWString());
    QVERIFY(WriteVerifiedArchive(corrupt, files).Succeeded());
    const auto oldIdentity = project.mapEdits_->SessionIdentity(), oldRevision = project.mapEdits_->Revision();
    QVERIFY(!PrepareProjectOpen(corrupt).Succeeded());
    QCOMPARE(project.mapEdits_->SessionIdentity(), oldIdentity); QCOMPARE(project.mapEdits_->Revision(), oldRevision);
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, finalTerrain.value);

    // 旧工程保持raw完整恢复；删除声明和载荷后不能推断typed谱系。
    files = std::vector<MapArchiveMember>(opened.value->Members().begin(), opened.value->Members().end());
    files.erase(std::remove_if(files.begin(), files.end(), [](const auto& member) {
        return member.name == "overlay/editor/terrain-strokes.bin";
    }), files.end());
    manifest.remove("terrainStrokeJournal");
    const auto legacyJson = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    for (auto& member : files) if (member.name == "project.json") {
        const auto bytes = std::as_bytes(std::span(legacyJson.data(), static_cast<std::size_t>(legacyJson.size())));
        member.data.assign(bytes.begin(), bytes.end());
    }
    const auto legacy = std::filesystem::path(root.filePath("legacy.forge").toStdWString());
    QVERIFY(WriteVerifiedArchive(legacy, files).Succeeded());
    auto old = PrepareProjectOpen(legacy); QVERIFY(old.Succeeded());
    QVERIFY((*old.value)->terrainReceipts.empty());
    QCOMPARE(MapData::SerializeMapTerrain(*(*old.value)->mapDataEdits->Terrain()).value, finalTerrain.value);
    QVERIFY(!(*old.value)->mapDataEdits->ExportTerrainStrokeJournal().Succeeded());
    // 单独存在的声明/载荷必须拒绝，不能默默降级。
    manifest["terrainStrokeJournal"] = 1;
    const auto missingJson = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    for (auto& member : files) if (member.name == "project.json") {
        const auto bytes = std::as_bytes(std::span(missingJson.data(), static_cast<std::size_t>(missingJson.size())));
        member.data.assign(bytes.begin(), bytes.end());
    }
    const auto missing = std::filesystem::path(root.filePath("missing.forge").toStdWString());
    QVERIFY(WriteVerifiedArchive(missing, files).Succeeded()); QVERIFY(!PrepareProjectOpen(missing).Succeeded());
}

void EditorProjectTests::TerrainJournalEqualOutputStillTracksUndoPrefix() {
    QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, TerrainSaveMembers()).Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto original = project.ProjectFingerprint();
    QVERIFY(RaiseTerrainSaveProject(project, 16));
    MapData::TerrainBrush lower; lower.kind = MapData::TerrainBrushKind::Lower; lower.radius = 0; lower.strength = 16;
    const MapData::TerrainStrokePoint point{512, 512};
    QVERIFY(project.mapEdits_->EditTerrain(lower, std::span(&point, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("回到相同地形字节，保留可撤销前缀"));
    QVERIFY(project.mapEdits_->Replacements().empty());
    QVERIFY(project.ProjectFingerprint() != original);
    const auto target = std::filesystem::path(root.filePath("equal-output.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    auto opened = MapArchiveSession::Open(target); QVERIFY(opened.Succeeded());
    QVERIFY(!opened.value->HasMember("overlay/map/war3map.w3e") && opened.value->HasMember("overlay/editor/terrain-strokes.bin"));
    auto prepared = PrepareProjectOpen(target); QVERIFY(prepared.Succeeded());
    QCOMPARE((*prepared.value)->terrainReceipts.size(), std::size_t{2});
    QVERIFY((*prepared.value)->mapDataEdits->Replacements().empty());
    QVERIFY((*prepared.value)->mapDataEdits->Replay((*prepared.value)->terrainReceipts.back(), false).Succeeded());
    QVERIFY(!(*prepared.value)->mapDataEdits->Replacements().empty());
    // 新的普通大笔划仍完整保存raw，不能让journal4096点域反向限制既有UI编辑。
    std::vector<MapData::TerrainStrokePoint> points(4097, point);
    QVERIFY(project.mapEdits_->EditTerrain(lower, points, false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("超journal点域的完整普通草稿"));
    const auto oversized = std::filesystem::path(root.filePath("oversized-raw.forge").toStdWString());
    const auto savedOversized = project.SaveProjectToPath(oversized);
    QVERIFY2(savedOversized.Succeeded(), ReadDiagnosticText(savedOversized).c_str());
    auto raw = MapArchiveSession::Open(oversized); QVERIFY(raw.Succeeded());
    QVERIFY(!raw.value->HasMember("overlay/editor/terrain-strokes.bin"));
    const auto bytes = raw.value->ReadMember("overlay/map/war3map.w3e"); QVERIFY(bytes.Succeeded());
    QCOMPARE(bytes.value, MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value);
}

void EditorProjectTests::TerrainJournalWaterFallbackKeepsRawAndWarning_data() {
    QTest::addColumn<bool>("resourceDrift");
    QTest::newRow("required-water-resource-missing") << false;
    QTest::newRow("required-water-resource-drift") << true;
}

void EditorProjectTests::TerrainJournalWaterFallbackKeepsRawAndWarning() {
    QFETCH(bool, resourceDrift);
    QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(source, TerrainJournalWaterMembers()).Succeeded());
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
    auto replayed = ReplayTerrainJournalWater(project);
    QVERIFY2(replayed.Succeeded(), replayed.diagnostics.empty() ? "water replay" : replayed.diagnostics.front().message.c_str());
    project.mapEdits_ = std::move(replayed.value->session); QVERIFY(project.ObserveMapDataEdit("实际游戏Water资源水刷"));
    const auto draft = TerrainJournalDraftBytes(*project.mapEdits_); QVERIFY(!draft.empty());
    const auto restrictions = project.mapEdits_->SerializePathingRestrictionDraft(); QVERIFY(restrictions.Succeeded());
    const auto target = std::filesystem::path(root.filePath("water.forge").toStdWString());
    const auto initialSave = project.SaveProjectToPath(target);
    QVERIFY2(initialSave.Succeeded(), ReadDiagnosticText(initialSave).c_str());
    auto complete = PrepareProjectOpen(target, game); QVERIFY(complete.Succeeded());
    QCOMPARE((*complete.value)->terrainReceipts.size(), std::size_t{1});
    QVERIFY((*complete.value)->terrainJournalWarning.empty());

    const auto changedGame = std::filesystem::path(root.filePath("changed-game").toStdWString());
    QVERIFY(std::filesystem::create_directory(changedGame));
    auto changedMembers = TerrainJournalWaterGameMembers();
    if (resourceDrift) {
        // 合法SLK只增加空白：最终规则值不变也不能绕过真实依赖字节身份。
        changedMembers.front().data.push_back(std::byte{'\n'});
    } else {
        changedMembers.erase(changedMembers.begin());
    }
    QVERIFY(WriteVerifiedArchive(changedGame / "war3.mpq", changedMembers).Succeeded());
    auto fallback = PrepareProjectOpen(target, changedGame);
    QVERIFY2(fallback.Succeeded(), fallback.diagnostics.empty() ? "raw fallback" : fallback.diagnostics.front().message.c_str());
    QVERIFY(HasReadDiagnostic(fallback, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    QVERIFY((*fallback.value)->terrainReceipts.empty() && !(*fallback.value)->terrainJournalWarning.empty());
    QCOMPARE(TerrainJournalDraftBytes(*(*fallback.value)->mapDataEdits), draft);
    QCOMPARE((*fallback.value)->mapDataEdits->SerializePathingRestrictionDraft().value, restrictions.value);
    QVERIFY(!(*fallback.value)->mapDataEdits->ExportTerrainStrokeJournal().Succeeded());

    EditorProject restored; QVERIFY(BindPreparedTerrainJournal(restored, **fallback.value, changedGame));
    const auto warning = restored.terrainJournalWarning_;
    const auto resaved = std::filesystem::path(root.filePath("raw-resaved.forge").toStdWString());
    const auto saved = restored.SaveProjectToPath(resaved);
    QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QVERIFY(HasReadDiagnostic(saved, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    const auto savedWarning = std::find_if(saved.diagnostics.begin(), saved.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "TERRAIN_JOURNAL_DRAFT_ONLY";
    });
    QVERIFY(savedWarning != saved.diagnostics.end()); QCOMPARE(savedWarning->message, warning);
    const auto raw = MapArchiveSession::Open(resaved); QVERIFY(raw.Succeeded());
    QVERIFY(!raw.value->HasMember("overlay/editor/terrain-strokes.bin"));
    const auto manifestBytes = raw.value->ReadMember("project.json"); QVERIFY(manifestBytes.Succeeded());
    const auto manifest = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(manifestBytes.value->data()),
        static_cast<qsizetype>(manifestBytes.value->size()))).object();
    QCOMPARE(manifest["terrainJournalWarning"].toString().toStdString(), warning);
    for (const auto& [name, bytes] : draft) QCOMPARE(raw.value->ReadMember("overlay/map/" + name).value, std::optional(bytes));
    // 恢复了正确游戏目录后仍是已明确降级的raw草稿，不能从输出重新发明笔划来源。
    auto reopened = PrepareProjectOpen(resaved, game); QVERIFY(reopened.Succeeded());
    QVERIFY(HasReadDiagnostic(reopened, "TERRAIN_JOURNAL_DRAFT_ONLY"));
    QCOMPARE((*reopened.value)->terrainJournalWarning, warning);
    QVERIFY((*reopened.value)->terrainReceipts.empty());
    QCOMPARE(TerrainJournalDraftBytes(*(*reopened.value)->mapDataEdits), draft);
    QCOMPARE((*reopened.value)->mapDataEdits->SerializePathingRestrictionDraft().value, restrictions.value);
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::TerrainJournalProjectSavesKeepCoreMapBaseline() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, TerrainSaveMembers()).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    const auto mapBaseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(mapBaseline);
    const auto session = project.mapEdits_->SessionIdentity(), sourceRevision = project.mapEdits_->TerrainSourceRevision();
    QVERIFY(RaiseTerrainSaveProject(project, 16));
    const auto first = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(first.Succeeded());
    const auto firstJournal = project.mapEdits_->TerrainJournalStateFingerprint();
    const auto manual = std::filesystem::path(root.filePath("manual.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(manual);
    QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QCOMPARE(project.mapEdits_->SessionIdentity(), session); QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision);
    QCOMPARE(project.mapEdits_->TerrainJournalStateFingerprint(), firstJournal);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, mapBaseline->scopes);
    QVERIFY(project.HasUnsavedMapChanges() && !project.editRevision_.IsDirty(EditSaveTarget::Project));

    QVERIFY(RaiseTerrainSaveProject(project, 8));
    const auto secondReceipt = project.mapEdits_->LastEdit();
    const auto secondJournal = project.mapEdits_->TerrainJournalStateFingerprint();
    const auto revision = project.mapEdits_->Revision();
    const auto recovery = std::filesystem::path(root.filePath("recovery.forge").toStdWString());
    const auto recovered = project.SaveProjectToPath(recovery, true);
    QVERIFY2(recovered.Succeeded(), ReadDiagnosticText(recovered).c_str());
    QCOMPARE(project.projectFilePath_, saved.value->path); QCOMPARE(project.projectFileSha256_, saved.value->sha256);
    QCOMPARE(project.mapEdits_->Revision(), revision); QCOMPARE(project.mapEdits_->TerrainSourceRevision(), sourceRevision);
    QCOMPARE(project.mapEdits_->TerrainJournalStateFingerprint(), secondJournal);
    const auto recoveryPrefix = project.mapEdits_->ExportTerrainStrokeJournal(); QVERIFY(recoveryPrefix.Succeeded());
    QCOMPARE(recoveryPrefix.value->strokes.size(), std::size_t{2});
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, mapBaseline->scopes);
    QVERIFY(project.HasUnsavedMapChanges() && project.editRevision_.IsDirty(EditSaveTarget::Project));
    auto recoveryOpen = PrepareProjectOpen(recovery); QVERIFY(recoveryOpen.Succeeded());
    QVERIFY((*recoveryOpen.value)->recovery); QCOMPARE((*recoveryOpen.value)->terrainReceipts.size(), std::size_t{2});

    const auto overwritten = project.SaveProjectToPath(manual);
    QVERIFY2(overwritten.Succeeded(), ReadDiagnosticText(overwritten).c_str());
    QVERIFY(overwritten.value->backupPath);
    QCOMPARE(project.mapEdits_->TerrainJournalStateFingerprint(), secondJournal);
    QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Map)->scopes, mapBaseline->scopes);
    // Qt式逆命令仍可约简到第一笔，证明两类工程保存均未把笔划变成地图保存边界。
    QVERIFY(project.mapEdits_->Replay(secondReceipt, false).Succeeded());
    QCOMPARE(project.mapEdits_->TerrainJournalStateFingerprint(), firstJournal);
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, first.value);
    QVERIFY(project.ObserveMapDataEdit("工程保存后撤销第二笔"));
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project) && project.HasUnsavedMapChanges());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::TerrainJournalEqualOutputMapSaveClearsHistoryDirty() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath("source"));
    const auto members = TerrainSaveMembers();
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source));
    QVERIFY(!project.HasUnsavedMapChanges() && !project.editRevision_.IsDirty(EditSaveTarget::Map));
    QVERIFY(project.mapEdits_->Replacements().empty() && !project.mapEdits_->CanUndo());
    QVERIFY(RaiseTerrainSaveProject(project, 16));
    MapData::TerrainBrush lower; lower.kind = MapData::TerrainBrushKind::Lower; lower.radius = 0; lower.strength = 16;
    const MapData::TerrainStrokePoint point{512, 512};
    QVERIFY(project.mapEdits_->EditTerrain(lower, std::span(&point, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("最终raw相同但仍有两笔可撤销历史"));
    QVERIFY(project.mapEdits_->Replacements().empty());
    const auto prefix = project.mapEdits_->ExportTerrainStrokeJournal(); QVERIFY(prefix.Succeeded());
    QCOMPARE(prefix.value->strokes.size(), std::size_t{2});
    // MainWindow保存动作与退出提示共同使用此门；raw为空不能使正常“保存地图”提前返回。
    QVERIFY(project.HasUnsavedMapChanges() && project.editRevision_.IsDirty(EditSaveTarget::Map));
    const auto forge = std::filesystem::path(root.filePath("equal-output.forge").toStdWString());
    const auto initialSave = project.SaveProjectToPath(forge);
    QVERIFY2(initialSave.Succeeded(), ReadDiagnosticText(initialSave).c_str());
    QVERIFY(project.HasUnsavedMapChanges() && !project.editRevision_.IsDirty(EditSaveTarget::Project));

    // 仅沿既有夹具替代脚本准备；真实原位写出、回读、Commit及Core地图基线提交均执行生产路径。
    auto job = FreezeNativePathingQtJob(project, source); QVERIFY(job && !job->terrainDerivationRequired);
    QVERIFY(job->replacements.empty() && job->removals.empty() && !job->pathingRestrictionDraft);
    job->inPlace = true;
    job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath("backups").toStdWString());
    QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
    const auto session = project.mapEdits_->SessionIdentity();
    EditorProject::RunMapSave(*job);
    QVERIFY2(job->succeeded, job->diagnostics.empty() ? "equal output map save" : job->diagnostics.back().message.c_str());
    QVERIFY(job->nextSession && !job->terrainSavePlan);
    for (const auto& member : members) QCOMPARE(job->nextSession->ReadMember(member.name).value, std::optional(member.data));
    QVERIFY(project.CommitMapSave(*job));
    QCOMPARE(project.mapEdits_->SessionIdentity(), session);
    QVERIFY(!project.HasUnsavedMapChanges() && !project.editRevision_.IsDirty(EditSaveTarget::Map));
    const auto savedPrefix = project.mapEdits_->ExportTerrainStrokeJournal(); QVERIFY(savedPrefix.Succeeded());
    QVERIFY(savedPrefix.value->strokes.empty() && project.mapEdits_->Replacements().empty());
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project));
    QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, sourceSha.value);

    const auto savedMapSha = Sha256File(source); QVERIFY(savedMapSha.Succeeded());
    const auto savedAgain = project.SaveProjectToPath(forge);
    QVERIFY2(savedAgain.Succeeded(), ReadDiagnosticText(savedAgain).c_str());
    const auto archived = MapArchiveSession::Open(forge); QVERIFY(archived.Succeeded());
    QVERIFY(!archived.value->HasMember("overlay/editor/terrain-strokes.bin")
        && !archived.value->HasMember("overlay/map/war3map.w3e") && !archived.value->HasMember("overlay/map/war3map.wpm"));
    const auto baseline = archived.value->ReadMember("baseline/source.w3x"); QVERIFY(baseline.Succeeded());
    QCOMPARE(Sha256(*baseline.value).value, savedMapSha.value);
    auto opened = PrepareProjectOpen(forge); QVERIFY(opened.Succeeded());
    QVERIFY((*opened.value)->terrainReceipts.empty() && (*opened.value)->terrainJournalWarning.empty());
    EditorProject restored; QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, {}));
    QVERIFY(!restored.HasUnsavedMapChanges() && !restored.editRevision_.IsDirty(EditSaveTarget::Map));
    QVERIFY(restored.mapEdits_->Replacements().empty() && !restored.mapEdits_->CanUndo());
}

void EditorProjectTests::TerrainJournalRestoredWaterSavesAndReadsBack_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("restored-water-save-as") << false;
    QTest::newRow("restored-water-save-in-place") << true;
}

void EditorProjectTests::TerrainJournalRestoredWaterSavesAndReadsBack() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath("source"));
    const auto source = std::filesystem::path(root.filePath("source/map.w3x").toStdWString());
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(source, TerrainJournalWaterMembers()).Succeeded());
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
    const auto sourceWpm = project.archiveSession_->ReadMember("war3map.wpm"); QVERIFY(sourceWpm.Succeeded());
    const auto script = project.archiveSession_->ReadMember("war3map.j"); QVERIFY(script.Succeeded());
    auto replayed = ReplayTerrainJournalWater(project);
    QVERIFY2(replayed.Succeeded(), replayed.diagnostics.empty() ? "water replay" : replayed.diagnostics.front().message.c_str());
    project.mapEdits_ = std::move(replayed.value->session); QVERIFY(project.ObserveMapDataEdit("可恢复水刷保存夹具"));
    const auto waterTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(waterTerrain.Succeeded());
    const auto forge = std::filesystem::path(root.filePath("water.forge").toStdWString());
    const auto initialSave = project.SaveProjectToPath(forge);
    QVERIFY2(initialSave.Succeeded(), ReadDiagnosticText(initialSave).c_str());
    auto opened = PrepareProjectOpen(forge, game); QVERIFY(opened.Succeeded());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game));
    QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), 1);
    const auto journal = restored.mapEdits_->TerrainJournalStateFingerprint();
    undo.undo(); QVERIFY(restored.mapEdits_->Replacements().empty());
    undo.redo(); QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, waterTerrain.value);
    QCOMPARE(restored.mapEdits_->TerrainJournalStateFingerprint(), journal);

    const auto forgeAs = std::filesystem::path(root.filePath("water-save-as.forge").toStdWString());
    const auto savedAs = restored.SaveProjectToPath(forgeAs);
    QVERIFY2(savedAs.Succeeded(), ReadDiagnosticText(savedAs).c_str());
    QVERIFY(RaiseTerrainSaveProject(restored, 1));
    PushMapDataEdit(restored, "Raised after restore", "恢复后普通地形笔划"); QCOMPARE(undo.count(), 2);
    const auto finalTerrain = MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()); QVERIFY(finalTerrain.Succeeded());
    const auto savedAgain = restored.SaveProjectToPath(forgeAs);
    QVERIFY2(savedAgain.Succeeded(), ReadDiagnosticText(savedAgain).c_str());
    QVERIFY(savedAgain.value->backupPath);
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY(reopened.Succeeded());
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{2});
    QCOMPARE(MapData::SerializeMapTerrain(*(*reopened.value)->mapDataEdits->Terrain()).value, finalTerrain.value);

    // 未安装脚本工具链时真实Prepare仍须拒绝；下方沿既有合法worker夹具验证领域保存与回读。
    restored.compilerDataRoot_.clear(); restored.activeUiDataRoot_.clear(); restored.uiDataRoot_.clear();
    const auto blocked = std::filesystem::path(root.filePath("script-blocked.w3x").toStdWString());
    QVERIFY(!restored.PrepareMapSave(blocked, false));
    QVERIFY(ModelHasCode(*restored.diagnosticsModel_, "REPACK_SCRIPT_REJECTED")); QVERIFY(!std::filesystem::exists(blocked));
    const auto session = restored.mapEdits_->SessionIdentity();
    const auto target = inPlace ? source : std::filesystem::path(root.filePath("source/water-saved.w3x").toStdWString());
    auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
    job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
    if (inPlace) {
        job->inPlace = true;
        job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath("backups").toStdWString());
        QVERIFY(std::filesystem::create_directory(job->inPlaceOptions.backupDirectory));
    }
    InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
    QVERIFY2(job->succeeded, job->diagnostics.empty() ? "restored native worker" : job->diagnostics.back().message.c_str());
    QVERIFY(job->nextSession && job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    const auto map = MapArchiveSession::Open(target); QVERIFY(map.Succeeded());
    QCOMPARE(map.value->ReadMember("war3map.w3e").value, finalTerrain.value);
    QCOMPARE(map.value->ReadMember("war3map.j").value, script.value);
    const auto savedWpm = map.value->ReadMember("war3map.wpm"); QVERIFY(savedWpm.Succeeded()); QVERIFY(savedWpm.value != sourceWpm.value);
    for (const auto& member : job->replacements) QCOMPARE(map.value->ReadMember(member.name).value, std::optional(member.data));
    QVERIFY(restored.CommitMapSave(*job));
    QCOMPARE(restored.mapEdits_->SessionIdentity(), session); QCOMPARE(restored.sourceArchivePath_, target);
    QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
    const auto rebasedPrefix = restored.mapEdits_->ExportTerrainStrokeJournal(); QVERIFY(rebasedPrefix.Succeeded());
    QVERIFY(rebasedPrefix.value->strokes.empty());
    if (inPlace) {
        QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, sourceSha.value);
    } else QCOMPARE(Sha256File(source).value, sourceSha.value);

    // 真实地图保存后才建立新journal来源；新工程只需重放保存之后的一笔。
    QVERIFY(RaiseTerrainSaveProject(restored, 1));
    PushMapDataEdit(restored, "Raised after map save", "地图保存后的新笔划");
    const auto next = std::filesystem::path(root.filePath("new-baseline.forge").toStdWString());
    const auto savedNext = restored.SaveProjectToPath(next);
    QVERIFY2(savedNext.Succeeded(), ReadDiagnosticText(savedNext).c_str());
    auto nextOpen = PrepareProjectOpen(next, game); QVERIFY(nextOpen.Succeeded());
    QCOMPARE((*nextOpen.value)->terrainReceipts.size(), std::size_t{1});
    QCOMPARE(MapData::SerializeMapTerrain(*(*nextOpen.value)->mapDataEdits->Terrain()).value,
        MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value);
}

void EditorProjectTests::PaletteWaterProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("palette-water-save-as") << false;
    QTest::newRow("palette-water-save-in-place") << true;
}

void EditorProjectTests::PaletteWaterProjectColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace);
    QTemporaryDir root; QVERIFY(root.isValid());
    auto members = TerrainJournalWaterMembers();
    MapData::DoodadPlacements doodads;
    doodads.specialDoodads = {{"Doo0", 0, {2, 2}}, {"Doo1", 0, {5, 5}}, {"Doo2", 0, {6, 5}}};
    const auto originalDoo = doodads.Serialize(); QVERIFY(originalDoo.Succeeded());
    for (auto& member : members) {
        if (member.name == "war3map.doo") member.data = *originalDoo.value;
        if (member.name == "TerrainArt\\Terrain.slk") member.data = NativeDoodadQtTable(
            {"tileID", "dir", "file", "walkable", "flyable", "buildable"},
            {{"Tg01", "Fixture", "Ground", "1", "1", "1"}, {"Tg02", "Fixture", "Ground", "1", "1", "1"}});
        if (member.name == "TerrainArt\\CliffTypes.slk") member.data = NativeDoodadQtTable(
            {"cliffID", "texDir", "texFile", "cliffModelDir", "rampModelDir", "groundTile", "cliffClass"},
            {{"Cl01", "Fixture", "Ground", "Cliff", "Ramp", "Tg01", "0"},
                {"Cl02", "Fixture", "Ground", "Cliff", "Ramp", "Tg02", "0"}});
    }
    members.push_back({"Units/DestructableData.slk", NativeDoodadQtTable({"destructableID"}, {{"Dst0"}})});
    members.push_back({"Units/DestructableMetaData.slk", NativeDoodadQtTable(
        {"ID", "field", "slk", "type", "index"}, {{"p001", "pathTex", "DestructableData", "pathingTexture", "0"}})});
    members.push_back({"Doodads/Doodads.slk", NativeDoodadQtTable(
        {"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", "PathTextures/zero-special.tga", "C", "45"},
            {"Doo1", "Doo1", "PathTextures/zero-special.tga", "C", "45"},
            {"Doo2", "Doo2", "PathTextures/zero-special.tga", "C", "45"}})});
    // 特殊模型的全黑4x4占地替换地面通行，去掉原干地unfloat；保留作者11。
    for (auto& member : members) if (member.name == "war3map.wpm") {
        for (const auto [x0, y0] : std::array<std::pair<unsigned, unsigned>, 3>{{{8, 8}, {20, 20}, {24, 20}}})
            for (unsigned y = y0; y < y0 + 4; ++y) for (unsigned x = x0; x < x0 + 4; ++x)
                member.data[16 + y * 32 + x] = std::byte{0x11};
    }
    std::vector<std::byte> texture(18 + 16 * 3, std::byte{});
    texture[2] = std::byte{2}; texture[12] = texture[14] = std::byte{4}; texture[16] = std::byte{24};
    members.push_back({"PathTextures/zero-special.tga", std::move(texture)});
    const auto sourceDirectory = std::filesystem::path(root.filePath("map").toStdWString());
    QVERIFY(std::filesystem::create_directory(sourceDirectory));
    const auto source = sourceDirectory / "source.w3x";
    const auto game = std::filesystem::path(root.filePath("game").toStdWString());
    QVERIFY(std::filesystem::create_directory(game));
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", TerrainJournalWaterGameMembers()).Succeeded());
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; QVERIFY(BindTerrainSaveProject(project, source)); project.war3GameRoot_ = game;
    QUndoStack initialUndo; project.mapDataUndoStack_ = &initialUndo;
    const auto baselineTerrain = project.archiveSession_->ReadMember("war3map.w3e"); QVERIFY(baselineTerrain.Succeeded());
    const auto ground = Services::TerrainPathingSave::EditTextureSlot(*project.archiveSession_, *project.mapEdits_,
        MapData::TerrainTextureTable::Ground, 0, "Tg02", game);
    QVERIFY2(ground.Succeeded(), ReadDiagnosticText(ground).c_str());
    QVERIFY(project.ObserveMapDataEdit("可恢复地面槽替换")); PushMapDataEdit(project, "Ground slot", "替换地面槽");
    auto baseline = MapData::ParseMapTerrain(*baselineTerrain.value); QVERIFY(baseline.Succeeded());
    const auto current = project.mapEdits_->Snapshot(); QVERIFY(current.Doodads());
    const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*project.archiveSession_, *baseline.value->Snapshot(),
        *project.mapEdits_->Terrain(), *current.Doodads(), game, true, 8u * 1024u * 1024u);
    QVERIFY2(rules.Succeeded(), ReadDiagnosticText(rules).c_str());
    const MapData::TerrainStrokePoint point{256, 256};
    const auto anchor = MapData::CaptureTerrainWaterStrokeAnchor(*project.mapEdits_->Terrain(), point,
        rules.value->WaterBrushRules()->maximumCliffLevel); QVERIFY(anchor.Succeeded());
    MapData::TerrainWaterBrush water{MapData::TerrainWaterBrushKind::Shallow, MapData::TerrainBrushShape::Square, 1, 0, 1, *anchor.value};
    const auto plan = project.mapEdits_->PrepareTerrainWaterEdit(*rules.value, water, std::span(&point, 1), false);
    QVERIFY2(plan.Succeeded(), ReadDiagnosticText(plan).c_str());
    QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(*plan.value).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("水刷删除特殊模型来源")); PushMapDataEdit(project, "Water", "水刷删除特殊模型");
    auto expectedDoo = doodads; expectedDoo.specialDoodads = {doodads.specialDoodads[2], doodads.specialDoodads[1]};
    const auto deletedDoo = expectedDoo.Serialize(); QVERIFY(deletedDoo.Succeeded());
    QCOMPARE(project.mapEdits_->Snapshot().Doodads()->Serialize().value, deletedDoo.value);
    const auto cliff = Services::TerrainPathingSave::EditTextureSlot(*project.archiveSession_, *project.mapEdits_,
        MapData::TerrainTextureTable::Cliff, 0, "Cl02", game);
    QVERIFY2(cliff.Succeeded(), ReadDiagnosticText(cliff).c_str());
    QVERIFY(project.ObserveMapDataEdit("可恢复悬崖槽替换")); PushMapDataEdit(project, "Cliff slot", "替换悬崖槽");
    const auto finalTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(finalTerrain.Succeeded());
    const auto forge = std::filesystem::path(root.filePath("palette-water.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{3});
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game)); QCOMPARE(undo.count(), 3);
    const auto journal = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
    QVERIFY2(journal.Succeeded(), ReadDiagnosticText(journal).c_str());
    const auto wire = MapData::EncodeTerrainStrokeJournal(*journal.value); QVERIFY(wire.Succeeded());
    QCOMPARE(wire.value->at(3), std::byte{'6'});
    for (int i = 0; i < 3; ++i) undo.undo();
    QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, baselineTerrain.value);
    QCOMPARE(restored.mapEdits_->Snapshot().Doodads()->Serialize().value, originalDoo.value);
    for (int i = 0; i < 3; ++i) undo.redo();
    QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, finalTerrain.value);
    QCOMPARE(restored.mapEdits_->Snapshot().Doodads()->Serialize().value, deletedDoo.value);
    const auto forgeAs = std::filesystem::path(root.filePath("palette-water-copy.forge").toStdWString());
    QVERIFY(restored.SaveProjectToPath(forgeAs).Succeeded());
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY2(reopened.Succeeded(), ReadDiagnosticText(reopened).c_str());
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{3});
    // 真正冷恢复后的私有special谱系还必须通过生产派生保存，不以raw字节相等代替。
    const auto target = inPlace ? source : std::filesystem::path(root.filePath("saved.w3x").toStdWString());
    auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
    job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
    job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = std::filesystem::path(root.filePath("backups").toStdWString());
    InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
    QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
    QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
    const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
    QCOMPARE(actual.value->ReadMember("war3map.w3e").value, finalTerrain.value);
    QCOMPARE(actual.value->ReadMember("war3map.doo").value, deletedDoo.value);
    QVERIFY(restored.CommitMapSave(*job)); QVERIFY(!restored.HasUnsavedMapChanges());
    if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, sourceSha.value); }
    else QCOMPARE(Sha256File(source).value, sourceSha.value);
}

void EditorProjectTests::CornerRampProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("corner-project-save-as") << false;
    QTest::newRow("corner-project-save-in-place") << true;
}

void EditorProjectTests::CornerRampProjectColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace);
    QTemporaryDir projectFiles; QVERIFY(projectFiles.isValid());
    TerrainCornerRampFixtures::Fixture fixture;
    const auto source = fixture.source->SourcePath(), game = fixture.game;
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    EditorProject project; project.war3GameRoot_ = game;
    QVERIFY(BindTerrainSaveProject(project, source));
    QUndoStack originalUndo; project.mapDataUndoStack_ = &originalUndo;
    const auto snapshot = project.mapEdits_->Snapshot(); QVERIFY(snapshot.Doodads());
    const auto rules = Services::TerrainPathingSave::CaptureWaterBrushRules(*project.archiveSession_,
        fixture.grid, *project.mapEdits_->Terrain(), *snapshot.Doodads(), game, true, TerrainCornerRampFixtures::kBudget);
    QVERIFY2(rules.Succeeded(), ReadDiagnosticText(rules).c_str());
    const auto frozen = project.mapEdits_->FreezeForSave(TerrainCornerRampFixtures::kBudget);
    QVERIFY(frozen.Succeeded());
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::CornerRamp; brush.value = true;
    const auto prepared = (*frozen.value)->PrepareTerrainCornerRampEdit(*rules.value, brush, std::span(&fixture.point, 1), true);
    QVERIFY2(prepared.Succeeded(), ReadDiagnosticText(prepared).c_str());
    QVERIFY(project.mapEdits_->ApplyPreparedTerrainEdit(*prepared.value).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("拐角坡道工程夹具"));
    PushMapDataEdit(project, "Corner ramp", "拐角坡道及摆放联动"); QCOMPARE(originalUndo.count(), 1);
    fixture.CheckState(*project.mapEdits_, true);
    const auto forge = std::filesystem::path(projectFiles.filePath("corner.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(forge);
    QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QCOMPARE(Sha256File(source).value, sourceSha.value);

    // 真正写盘后重新解析工程及来源，不能用内存journal回放代替冷恢复。
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{1});
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game));
    QCOMPARE(undo.count(), 1); QCOMPARE(undo.index(), 1);
    fixture.CheckState(*restored.mapEdits_, true);
    const auto journal = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
    QVERIFY2(journal.Succeeded(), ReadDiagnosticText(journal).c_str());
    const auto wire = MapData::EncodeTerrainStrokeJournal(*journal.value);
    QVERIFY2(wire.Succeeded(), ReadDiagnosticText(wire).c_str());
    QCOMPARE(wire.value->at(3), std::byte{'8'});
    undo.undo(); fixture.CheckState(*restored.mapEdits_, false);
    undo.redo(); fixture.CheckState(*restored.mapEdits_, true);
    const auto forgeAs = std::filesystem::path(projectFiles.filePath("corner-copy.forge").toStdWString());
    const auto savedAs = restored.SaveProjectToPath(forgeAs);
    QVERIFY2(savedAs.Succeeded(), ReadDiagnosticText(savedAs).c_str());
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY(reopened.Succeeded());
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{1});
    fixture.CheckState(*(*reopened.value)->mapDataEdits, true);

    // 这里只替代已有脚本准备夹具；派生、归档、回读和前台提交均为真实生产入口。
    const auto session = restored.mapEdits_->SessionIdentity();
    for (int state = 0; state < 3; ++state) {
        if (state == 1) undo.undo();
        if (state == 2) undo.redo();
        const bool created = state != 1;
        fixture.CheckState(*restored.mapEdits_, created);
        const auto target = inPlace ? source : fixture.root / ("corner-saved-" + std::to_string(state) + ".w3x");
        const auto beforeSha = restored.archiveSession_->SourceSha256();
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
        job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = fixture.root / "backups";
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, std::optional(fixture.TerrainLiteral(created)));
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(TerrainCornerRampFixtures::PathingLiteral(created)));
        QCOMPARE(actual.value->ReadMember("war3mapUnits.doo").value, fixture.UnitsLiteral(created).Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.doo").value, fixture.DoodadsLiteral(created).Serialize().value);
        QCOMPARE(actual.value->ReadMember("war3map.j").value, fixture.source->ReadMember("war3map.j").value);
        QVERIFY(restored.CommitMapSave(*job));
        QCOMPARE(restored.mapEdits_->SessionIdentity(), session);
        QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, std::optional(beforeSha)); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
        QCOMPARE(undo.count(), 1);
    }
}

void EditorProjectTests::PlayableBoundsProjectColdRestoreAndNativeSave_data() {
    QTest::addColumn<bool>("inPlace");
    QTest::newRow("bounds-project-save-as") << false;
    QTest::newRow("bounds-project-save-in-place") << true;
}

void EditorProjectTests::PlayableBoundsProjectColdRestoreAndNativeSave() {
    QFETCH(bool, inPlace);
    QTemporaryDir projectFiles; QVERIFY(projectFiles.isValid());
    AudastraTest::TerrainBounds::Fixture fixture;
    const auto source = fixture.archive->SourcePath();
    const auto sourceSha = Sha256File(source); QVERIFY(sourceSha.Succeeded());
    const auto game = fixture.directory / "game"; QVERIFY(std::filesystem::create_directory(game));
    const std::array<MapArchiveMember, 1> gameMembers{{
        {"fixture.txt", AudastraTest::TerrainBounds::Text("owned bounds resources")}}};
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", gameMembers).Succeeded());
    EditorProject project; project.war3GameRoot_ = game;
    QVERIFY(BindTerrainSaveProject(project, source));
    QUndoStack initialUndo; project.mapDataUndoStack_ = &initialUndo;
    auto settings = *project.mapEdits_->Snapshot().Settings(); settings.description = "description before bounds";
    QVERIFY(project.mapEdits_->EditSettings(settings).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("范围前的说明修改")); PushMapDataEdit(project, "Description", "范围前的说明修改");
    const auto prepared = project.mapEdits_->PrepareTerrainBoundsEdit(fixture.Capture(), {{1, 1, 9, 7}});
    QVERIFY2(prepared.Succeeded(), ReadDiagnosticText(prepared).c_str());
    QVERIFY(project.mapEdits_->ApplyPreparedTerrainBoundsEdit(*prepared.value).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("可玩区域")); PushMapDataEdit(project, "Playable bounds", "可玩区域");
    // 独立完整字节预期先证明范围命令；后面的高度笔划另由既有笔刷合同覆盖。
    auto boundsTerrain = fixture.members[0].data;
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 10; ++x)
        if (x < 1 || x >= 9 || y < 1 || y >= 7) boundsTerrain[45 + (y * 11 + x) * 7 + 3] |= std::byte{0x40};
    QCOMPARE(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::optional(boundsTerrain));
    MapData::TerrainBrush raise; raise.kind = MapData::TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 8;
    const MapData::TerrainStrokePoint point{0, 0};
    QVERIFY(project.mapEdits_->EditTerrain(raise, std::span(&point, 1), false).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("范围后的高度")); PushMapDataEdit(project, "Height", "范围后的高度");
    settings = *project.mapEdits_->Snapshot().Settings(); settings.author = "description after terrain";
    QVERIFY(project.mapEdits_->EditSettings(settings).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("地形后的说明修改")); PushMapDataEdit(project, "Author", "地形后的说明修改");
    QCOMPARE(initialUndo.count(), 4);
    const auto finalDraft = TerrainJournalDraftBytes(*project.mapEdits_);
    const auto finalTerrain = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(finalTerrain.Succeeded());
    const auto finalSettings = settings.Serialize(); QVERIFY(finalSettings.Succeeded());
    auto boundsPathing = fixture.members[1].data;
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 40; ++x)
        if (x / 4 < 1 || x / 4 >= 9 || y / 4 < 1 || y / 4 >= 7) boundsPathing[16 + y * 40 + x] = std::byte{0xdf};
    const auto forge = std::filesystem::path(projectFiles.filePath("bounds.forge").toStdWString());
    const auto saved = project.SaveProjectToPath(forge); QVERIFY2(saved.Succeeded(), ReadDiagnosticText(saved).c_str());
    QCOMPARE(Sha256File(source).value, sourceSha.value);
    auto opened = PrepareProjectOpen(forge, game); QVERIFY2(opened.Succeeded(), ReadDiagnosticText(opened).c_str());
    QVERIFY(HasReadDiagnostic(opened, "TERRAIN_JOURNAL_RESTORED"));
    QCOMPARE((*opened.value)->terrainReceipts.size(), std::size_t{3}); // bounds、height、合并说明均有真实撤销记录。
    EditorProject restored; QUndoStack undo; restored.mapDataUndoStack_ = &undo;
    QVERIFY(BindPreparedTerrainJournal(restored, **opened.value, game));
    QCOMPARE(undo.count(), 3); QCOMPARE(undo.index(), 3);
    QCOMPARE(TerrainJournalDraftBytes(*restored.mapEdits_), finalDraft);
    const auto journal = Services::TerrainPathingSave::CaptureJournal(*restored.archiveSession_, *restored.mapEdits_);
    QVERIFY2(journal.Succeeded(), ReadDiagnosticText(journal).c_str());
    const auto wire = MapData::EncodeTerrainStrokeJournal(*journal.value);
    QVERIFY2(wire.Succeeded(), ReadDiagnosticText(wire).c_str());
    QCOMPARE(wire.value->at(3), std::byte{'5'});
    undo.undo(); QCOMPARE(restored.mapEdits_->Snapshot().Settings()->author, std::string{});
    undo.undo(); QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, std::optional(boundsTerrain));
    undo.undo();
    QVERIFY(!restored.mapEdits_->IsDirty() && restored.mapEdits_->Replacements().empty());
    QCOMPARE(MapData::SerializeMapTerrain(*restored.mapEdits_->Terrain()).value, std::optional(fixture.members[0].data));
    QCOMPARE(restored.mapEdits_->Snapshot().Settings()->Serialize().value, std::optional(fixture.members[2].data));
    for (int index = 0; index < 3; ++index) undo.redo();
    QCOMPARE(TerrainJournalDraftBytes(*restored.mapEdits_), finalDraft);
    const auto forgeAs = std::filesystem::path(projectFiles.filePath("bounds-copy.forge").toStdWString());
    const auto copied = restored.SaveProjectToPath(forgeAs); QVERIFY2(copied.Succeeded(), ReadDiagnosticText(copied).c_str());
    auto reopened = PrepareProjectOpen(forgeAs, game); QVERIFY(reopened.Succeeded());
    QCOMPARE((*reopened.value)->terrainReceipts.size(), std::size_t{3});
    QCOMPARE(TerrainJournalDraftBytes(*(*reopened.value)->mapDataEdits), finalDraft);

    // 只替代脚本工具准备；地图写盘、派生通行、回读和提交均走正式保存链。
    const auto session = restored.mapEdits_->SessionIdentity();
    for (int state = 0; state < 3; ++state) {
        if (state == 1) for (int index = 0; index < 3; ++index) undo.undo();
        if (state == 2) for (int index = 0; index < 3; ++index) undo.redo();
        const bool inset = state != 1;
        const auto target = inPlace ? source : fixture.directory / ("bounds-saved-" + std::to_string(state) + ".w3x");
        const auto beforeSha = restored.archiveSession_->SourceSha256();
        auto job = FreezeNativePathingQtJob(restored, target); QVERIFY(job && job->terrainDerivationRequired);
        job->terrainSessionSnapshot = restored.mapEdits_->FreezeForSave(); job->minimapGameRoot = game;
        job->inPlace = inPlace; job->inPlaceOptions.backupDirectory = fixture.directory / "backups";
        InstallMapSaveDerivedPreparation(*job); EditorProject::RunMapSave(*job);
        QVERIFY2(job->succeeded, ReadDiagnosticText(*job).c_str());
        QVERIFY(job->terrainSavePlan && job->terrainReadbackStatus && job->terrainReadbackStatus->Succeeded());
        const auto actual = MapArchiveSession::Open(target); QVERIFY(actual.Succeeded());
        QCOMPARE(actual.value->ReadMember("war3map.w3e").value, inset ? finalTerrain.value : std::optional(fixture.members[0].data));
        QCOMPARE(actual.value->ReadMember("war3map.w3i").value, inset ? finalSettings.value : std::optional(fixture.members[2].data));
        QCOMPARE(actual.value->ReadMember("war3map.wpm").value, std::optional(inset ? boundsPathing : fixture.members[1].data));
        for (const auto* name : {"war3map.doo", "war3mapUnits.doo", "war3mapMap.tga"})
            QCOMPARE(actual.value->ReadMember(name).value, fixture.archive->ReadMember(name).value);
        QVERIFY(restored.CommitMapSave(*job));
        QCOMPARE(restored.mapEdits_->SessionIdentity(), session);
        QVERIFY(!restored.HasUnsavedMapChanges() && restored.mapEdits_->Replacements().empty());
        if (inPlace) { QVERIFY(!job->backup.empty()); QCOMPARE(Sha256File(job->backup).value, std::optional(beforeSha)); }
        else QCOMPARE(Sha256File(source).value, sourceSha.value);
        QCOMPARE(undo.count(), 3);
    }
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
    auto arguments = QCoreApplication::arguments();
    QString group;
    for (qsizetype index = 1; index < arguments.size();) {
        if (!arguments[index].startsWith("--case-group=")) { ++index; continue; }
        if (!group.isEmpty()) return 2;
        group = arguments.takeAt(index).mid(qsizetype(sizeof("--case-group=") - 1));
        if (group != "base" && group != "native-pathing" && group != "native-objects" && group != "derived" && group != "terrain-units"
            && group != "terrain-palette" && group != "terrain-doodads" && group != "terrain-static-inputs") return 2;
    }
    if (!group.isEmpty()) {
        // 按实际Qt槽自动分区，新增测试默认进入base，不能因手写函数清单漏跑。
        // 单组保留原30秒上限；不传分组参数时仍可直接运行完整套件。
        const auto groupOf = [](const QByteArray& name) {
            if (name.startsWith("DualStageStaticPathingWithoutTerrainNet")
                || name.startsWith("DualStageUnrelatedEditsPreserveUnresolvedPathing"))
                return QStringLiteral("terrain-static-inputs");
            if (name.startsWith("OrdinaryDoodadProject") || name.startsWith("OrdinaryWaterJournalProject"))
                return QStringLiteral("terrain-doodads");
            if (name.startsWith("PaletteWaterProject") || name.startsWith("GroundPaletteAppendWaterProject"))
                return QStringLiteral("terrain-palette");
            if (name.startsWith("TerrainUnitPlacement") || name.startsWith("BoundaryNative")
                || name.startsWith("UnitsWaterJournalProject") || name.startsWith("StartLocations"))
                return QStringLiteral("terrain-units");
            for (const auto* prefix : {"TerrainShadow", "AutomaticMinimap", "MissingMinimap", "DeclaredMissingMinimap"})
                if (name.startsWith(prefix)) return QStringLiteral("derived");
            for (const auto* prefix : {"CGrid", "EffectiveDoodad", "FixedRotation", "Walkable", "PlacementHeight", "NativeObjectCatalog"})
                if (name.startsWith(prefix)) return QStringLiteral("native-objects");
            for (const auto* prefix : {"NativePathing", "PathingIntent", "LegacyPathing", "StraightRamp", "DualStage"})
                if (name.startsWith(prefix)) return QStringLiteral("native-pathing");
            return QStringLiteral("base");
        };
        const auto& meta = EditorProjectTests::staticMetaObject;
        qsizetype selected = 0;
        for (int index = meta.methodOffset(); index < meta.methodCount(); ++index) {
            const auto method = meta.method(index); const auto name = method.name();
            if (method.methodType() != QMetaMethod::Slot || method.parameterCount() != 0 || name.endsWith("_data")
                || name == "initTestCase" || name == "cleanupTestCase" || name == "init" || name == "cleanup") continue;
            if (groupOf(name) == group) { arguments.append(QString::fromLatin1(name)); ++selected; }
        }
        if (!selected) return 2;
    }
    return QTest::qExec(&tests, arguments);
}
#include "EditorProjectTests.moc"
