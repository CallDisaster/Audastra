#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Resources/TerrainTable.h"
#include "War3MapCore/Services/MapCreateService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Services/TerrainMinimapMarkersSave.h"
#include "War3MapCore/Services/TerrainMinimapSave.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Check(const Status& status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& item : status.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing cliff palette result" : message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        Check({result.diagnostics}); throw std::runtime_error("missing cliff palette value");
    }
    return std::move(*result.value);
}
std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD capacity = GetEnvironmentVariableW(name, nullptr, 0);
    if (!capacity) return {};
    std::wstring value(capacity, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), capacity);
    Require(length > 0 && length < capacity, "environment path changed while reading");
    value.resize(length); return value;
}
struct OwnedDirectory {
    std::filesystem::path path;
    bool keep = false;
    OwnedDirectory() {
        const auto evidence = EnvironmentPath(L"FORGE_TERRAIN_SAVE_EVIDENCE_DIR");
        keep = !evidence.empty();
        const auto parent = keep ? evidence : std::filesystem::temp_directory_path();
        std::filesystem::create_directories(parent);
        path = parent / ("cliff-palette-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(path), "exclusive cliff palette evidence directory");
    }
    ~OwnedDirectory() {
        if (!keep) { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    }
};

Resources::TerrainTableSnapshot ActualCliffTable(const Game::GameDataSource& game) {
    Resources::ResourceResolver resolver;
    Resources::ResourceSource source;
    source.id = "real-cliff-palette-game";
    source.layer = Resources::ResourceLayer::Game;
    source.priority = Resources::ResourcePriority::GamePatch;
    source.probe = [&game](std::string_view name) {
        const std::string path(name);
        return Result<Resources::ResourceProbe>::Success({game.HasFile(path)
            ? Resources::ResourceProbeState::Found : Resources::ResourceProbeState::Missing, path});
    };
    source.readWithProvenance = [&game](std::string_view path) {
        auto read = game.ReadFileBoundedWithSource(std::string(path), 2u * 1024u * 1024u);
        if (!read.Succeeded() || !read.value) {
            Result<Resources::ResourceReadResult> failed;
            failed.diagnostics = std::move(read.diagnostics); return failed;
        }
        Resources::ResourceReadProvenance origin{Resources::ResourceReadOrigin::GameArchive,
            read.value->archiveName, std::string(path), IO::PathUtf8(game.Root())};
        return Result<Resources::ResourceReadResult>::Success({std::move(read.value->bytes), std::move(origin)});
    };
    Check(resolver.AddSource(std::move(source)));
    return Take(Resources::CaptureTerrainTable(resolver, "TerrainArt/CliffTypes.slk", "cliffID"));
}
std::string Field(const Resources::TerrainTableRow& row, std::string_view key) {
    const auto found = row.fields.find(key);
    return found == row.fields.end() ? std::string{} : found->second;
}
std::string TexturePath(const Resources::TerrainTableRow& row) {
    const auto directory = Field(row, "texdir"), file = Field(row, "texfile");
    if (directory.empty() || file.empty() || directory == "_" || file == "_") return {};
    auto path = directory + '\\' + file;
    if (!Text::LowerAscii(path).ends_with(".blp")) path += ".blp";
    return path;
}

// 字面期望只改W3E四字节表项。完整vertex正文、WPM和脚本都来自编辑前来源，
// 不用保存产物或after生成器回填expected。
Bytes ExpectedPaletteWire(const Bytes& before, const TerrainHeader& header, std::string_view rawcode) {
    Require(rawcode.size() == 4 && !header.cliffTilesets.empty(), "literal palette replacement has a concrete slot");
    auto expected = before;
    const auto cliffCountOffset = 17u + header.groundTilesets.size() * 4u;
    const auto firstCliffOffset = cliffCountOffset + 4u;
    Require(expected.size() >= firstCliffOffset + 4u && expected[9] == std::byte{1},
        "source uses a real custom palette before the tested edit");
    for (std::size_t i = 0; i < 4; ++i) expected[firstCliffOffset + i] = std::byte(rawcode[i]);
    return expected;
}

MapArchiveSession SavePalette(MapArchiveSession& source, MapDataEditSession& edits,
    const std::filesystem::path& target, const std::filesystem::path& tool,
    const std::filesystem::path& game, const Bytes& expectedTerrain,
    const Bytes& expectedPathing, const Bytes& expectedScript) {
    MapSaveTask task;
    Check(task.FreezeArchive(source));
    task.target = target;
    task.requireGeneratedNativeByteEquality = true;
    task.preserveUnaffectedScript = true;
    for (auto& member : edits.Replacements())
        task.replacements.push_back({std::move(member.name), std::move(member.data)});
    auto catalog = Take(Trigger::UiCatalog::Load(tool));
    auto triggers = Take(Trigger::WtgWctCodec{}.Parse(Take(source.ReadMember("war3map.wtg")),
        Take(source.ReadMember("war3map.wct")), catalog));
    ScriptSaveInputSources inputs;
    inputs.layers = catalog.Layers(); inputs.expectedEffectiveLayers = inputs.layers;
    inputs.toolRoot = tool; inputs.uiRoot = tool; inputs.gameRoot = game;
    inputs.catalog = std::make_unique<Trigger::UiCatalog>(std::move(catalog));
    inputs.triggers = std::make_unique<Trigger::TriggerDocument>(std::move(triggers));
    inputs.scriptMember = "war3map.j"; inputs.documentRevision = edits.Revision();
    Require(Take(PrepareScriptSaveInputs(task, std::move(inputs))) == ScriptSavePreparationState::Ready,
        "actual cliff palette toolchain is ready, not skipped");
    std::shared_ptr<const TerrainSavePlan> plan;
    std::shared_ptr<const TerrainMinimapSave> minimap;
    std::shared_ptr<const TerrainMinimapPreserveGuard> preserved;
    task.prepareFinalDerived = [&](MapSaveTask&) {
        auto prepared = Take(TerrainPathingSave::PrepareForSave(*task.archive, edits, game,
            {}, task.replacements, task.removals));
        Check(prepared.SaveGate());
        Check(prepared.InstallPathingOutput(edits, task.replacements));
        Check(prepared.VerifyForSave(edits, task.sourceSha256, task.replacements, task.removals));
        plan = std::make_shared<const TerrainSavePlan>(std::move(prepared));
        Check(PrepareTerrainMinimapMarkers(task, game, &edits, plan));
        auto automaticResult = TerrainMinimapSave::PrepareAutomaticOrPreserve(task, edits, game, {}, plan);
        task.diagnostics.insert(task.diagnostics.end(), automaticResult.diagnostics.begin(), automaticResult.diagnostics.end());
        auto automatic = Take(std::move(automaticResult));
        minimap = std::move(automatic.generated); preserved = std::move(automatic.preserved);
        if (minimap) {
            task.replacements.push_back(minimap->MinimapReplacement()); plan = minimap->Plan();
            Check(minimap->VerifyCurrent(task, edits, game));
        }
        if (preserved) Check(preserved->VerifyCurrent(task, edits));
        task.beforePublish = [&] {
            Check(plan->VerifyFrozenOutput(task.sourceSha256, task.replacements, task.removals));
            if (minimap) Check(minimap->VerifyFrozenOutput(task));
            if (preserved) Check(preserved->VerifyFrozenOutput(task));
            return Status::Ok();
        };
        return Status::Ok();
    };
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.scriptRebuilt && !task.scriptSkipped && task.scriptPreserved && task.nextSession,
        "trusted-host palette save completes the actual script and archive chain");
    Require(plan && task.minimapMarkers && minimap && !preserved,
        "real palette fixture completes automatic markers and minimap generation");
    Check(plan->VerifyReadback(*task.nextSession));
    Check(minimap->VerifyReadback(task, *task.nextSession));
    Require(Take(task.nextSession->ReadMember("war3map.w3e")) == expectedTerrain,
        "actual palette save matches the complete independent literal W3E");
    Require(Take(task.nextSession->ReadMember("war3map.wpm")) == expectedPathing,
        "actual palette save preserves every baseline WPM byte");
    Require(Take(task.nextSession->ReadMember("war3map.j")) == expectedScript,
        "actual palette save preserves every checked original script byte");
    for (const auto& member : source.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
        Require(Take(task.nextSession->ReadMember(member.name)) == member.data,
            "actual palette save preserves all unrelated source members");
    }
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "actual palette save leaves its input copy unchanged");
    Check(plan->CommitSaved(edits, MapDocument::FromArchive(*task.nextSession)));
    std::cout << "cliff_palette_save_passed candidate_sha256=" << task.nextSession->SourceSha256()
        << " w3e_sha256=" << Take(Sha256(expectedTerrain)) << " wpm_sha256=" << Take(Sha256(expectedPathing))
        << " script_preserved=true automatic_minimap=true native_editor_verified=false game_verified=false\n";
    FinalizeScriptWorkspace(task);
    return std::move(*task.nextSession);
}

void RunRealPaletteSave(const std::filesystem::path& tool, const std::filesystem::path& game, OwnedDirectory& directory) {
    MapCreateRequest create;
    create.options.name = "Cliff palette save regression";
    create.options.initTriggerName = "Palette regression init";
    create.options.initCustomScript = {"call FogMaskEnable(false)", "call FogEnable(false)", "call SetCameraPosition(0.0, 0.0)"};
    create.target = directory.path / "created.w3x"; create.workspace = directory.path / "create-work";
    create.toolRoot = tool; create.gameRoot = game;
    const auto created = Take(CreateMapCandidate(create));
    Require(created.scriptRebuilt, "original fixture has a script checked by the actual local toolchain");
    auto original = Take(MapArchiveSession::Open(created.path));
    const auto createdSha = original.SourceSha256();
    auto before = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(before.header.width == 65 && before.header.height == 65 && !before.header.cliffTilesets.empty(),
        "real cliff palette fixture begins with the known 64-cell original template");
    const auto mounted = Take(Game::GameDataSource::MountStrict(game, before.header.mainTileset));
    const auto table = ActualCliffTable(mounted);
    Require(table.Location().readProvenance
        && table.Location().readProvenance->origin == Resources::ResourceReadOrigin::GameArchive,
        "actual cliff table reports its winning game archive");
    const auto oldId = before.header.cliffTilesets[0];
    const auto oldRow = std::find_if(table.Rows().begin(), table.Rows().end(), [&](const auto& row) { return row.id == oldId; });
    Require(oldRow != table.Rows().end(), "old selected cliff palette row exists in the actual strict table");
    const auto oldTexture = TexturePath(*oldRow);
    Require(!oldTexture.empty(), "old actual cliff row declares a texture");
    const auto oldTextureBytes = Take(mounted.ReadFileBoundedWithSource(oldTexture, 16u * 1024u * 1024u));
    const auto newRow = std::find_if(table.Rows().begin(), table.Rows().end(), [&](const auto& row) {
        return row.id.size() == 4 && std::find(before.header.cliffTilesets.begin(), before.header.cliffTilesets.end(), row.id)
            == before.header.cliffTilesets.end() && Field(row, "cliffmodeldir") == Field(*oldRow, "cliffmodeldir")
            && !TexturePath(row).empty() && Text::LowerAscii(TexturePath(row)) != Text::LowerAscii(oldTexture)
            && mounted.HasFile(TexturePath(row));
    });
    Require(newRow != table.Rows().end(), "actual game offers another cliff material with the same ordinary wall model directory");
    const auto newTexture = TexturePath(*newRow);
    const auto newTextureBytes = Take(mounted.ReadFileBoundedWithSource(newTexture, 16u * 1024u * 1024u));
    Require(!oldTextureBytes.bytes.empty() && !newTextureBytes.bytes.empty()
        && oldTextureBytes.bytes != newTextureBytes.bytes, "both actual selected palette textures are readable and distinct");
    before.header.usesCustomTileset = true; // 本来已是自定义表，避免借custom flag变化触发额外保存门。
    for (std::uint32_t y = 0; y < before.header.height; ++y) for (std::uint32_t x = 0; x < before.header.width; ++x) {
        auto& vertex = before.vertices[std::size_t{y} * before.header.width + x];
        Require(!vertex.IsRamp() && !vertex.HasWater(), "ordinary wall fixture is dry and contains no ramps");
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>(x <= 31 ? 3 : 2); // 真实slot0，普通异层墙面。
        vertex.variationRaw &= 0x1f;
    }
    RefreshTerrainDerived(before, 0, 0, before.header.width, before.header.height);
    Require(before.vertices[32 * 65 + 31].CliffLevel() != before.vertices[32 * 65 + 32].CliffLevel()
        && before.vertices[32 * 65 + 31].CliffTextureIndex() == 0,
        "selected slot is used by an ordinary nonflat wall, not an unused flat palette");
    const auto sourceW3e = Take(SerializeMapTerrain(before));
    auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(directory.path / "wall-seeded.w3x",
        std::array<MemberReplacement, 1>{{{"war3map.w3e", sourceW3e}}})).path));
    const auto owner = Take(TerrainPathingSave::CaptureNative(seeded, before, before, game, kBytes));
    const auto native = Take(owner.GenerateNative(before, kBytes));
    auto sourceWpm = Take(seeded.ReadMember("war3map.wpm"));
    Require(sourceWpm.size() == native.size() + 16, "ordinary wall source uses the full template WPM shape");
    // 只规范化原创夹具的来源WPM；这是产品生成器，不是独立原版算法证据。
    for (std::size_t i = 0; i < native.size(); ++i)
        sourceWpm[16 + i] = (sourceWpm[16 + i] & std::byte{0x11}) | native[i];
    auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(directory.path / "source.w3x",
        std::array<MemberReplacement, 1>{{{"war3map.wpm", sourceWpm}}})).path));
    const auto script = Take(source.ReadMember("war3map.j"));
    MapDataEditSession edits(MapDocument::FromArchive(source));
    Check(edits.AttachTerrain(sourceW3e)); Check(edits.AttachPathing(sourceWpm));
    Require(Take(edits.EditTerrainTextureSlot(TerrainTextureTable::Cliff, 0, newRow->id)).headerChanged,
        "real existing cliff slot replacement makes a real edit");
    const auto expected = ExpectedPaletteWire(sourceW3e, before.header, newRow->id);
    Require(Take(SerializeMapTerrain(*edits.Terrain())) == expected, "slot command has the complete literal W3E result before saving");
    const auto both = Take(TerrainPathingSave::CaptureNative(source, before, *edits.Terrain(), game, kBytes));
    Require(Take(both.GenerateNative(before, TerrainNativeStage::Before, kBytes)) == native
        && Take(both.GenerateNative(*edits.Terrain(), TerrainNativeStage::After, kBytes)) == native,
        "complete actual-resource before and after native masks remain identical for the material replacement");
    std::cout << "cliff_palette_real_source table_sha256=" << table.SourceSha256()
        << " table_winner=" << table.Location().readProvenance->containerIdentity
        << " old_id=" << oldId << " new_id=" << newRow->id
        << " old_texture=" << oldTexture << " new_texture=" << newTexture
        << " old_texture_sha256=" << Take(Sha256(oldTextureBytes.bytes))
        << " old_texture_winner=" << oldTextureBytes.archiveName
        << " new_texture_sha256=" << Take(Sha256(newTextureBytes.bytes))
        << " new_texture_winner=" << newTextureBytes.archiveName
        << " expected_uses_product_core=true expected_uses_native_generator=true native_oracle=false\n";
    source = SavePalette(source, edits, directory.path / "palette-saved.w3x", tool, game, expected, sourceWpm, script);
    Require(edits.Undo(), "same session supports Undo after the actual palette save");
    source = SavePalette(source, edits, directory.path / "palette-undo.w3x", tool, game, sourceW3e, sourceWpm, script);
    Require(edits.Redo(), "same session supports Redo after saved Undo");
    source = SavePalette(source, edits, directory.path / "palette-redo.w3x", tool, game, expected, sourceWpm, script);
    Require(Take(Sha256File(created.path)) == createdSha, "all palette saves preserve the original created map");
}
} // namespace

int main() {
    try {
        const auto tool = EnvironmentPath(L"FORGE_YDWE_ROOT"), game = EnvironmentPath(L"FORGE_WORLD_GAME_ROOT");
        if (tool.empty() || game.empty()) {
            std::cout << "SKIP real cliff palette save: FORGE_YDWE_ROOT / FORGE_WORLD_GAME_ROOT unset\n";
            return 77;
        }
        OwnedDirectory directory;
        try { RunRealPaletteSave(tool, game, directory); }
        catch (...) {
            directory.keep = true;
            std::cerr << "Real cliff palette failure artifacts preserved at " << IO::PathUtf8(directory.path) << '\n';
            throw;
        }
        if (directory.keep) std::cout << "cliff_palette_artifacts=" << IO::PathUtf8(directory.path) << '\n';
        std::cout << "real cliff palette save, saved Undo and Redo passed; native_editor_verified=false game_verified=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
