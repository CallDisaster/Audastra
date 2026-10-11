#include "War3MapCore/Services/ObjectResourceQuery.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "MdxTestModel.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <chrono>
#include <filesystem>

using namespace War3Map;
using namespace War3Map::Services;
using namespace War3Map::ObjectData;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
using Row = std::map<std::string, std::string>;
std::string Table(const std::vector<Row>& rows) {
    std::vector<std::string> columns{"ID"};
    for (const auto& row : rows) for (const auto& [name, value] : row)
        if (std::find(columns.begin(), columns.end(), name) == columns.end()) columns.push_back(name);
    std::string result = "ID;PWXL\n";
    const auto cell = [&](std::size_t x, std::size_t y, std::string_view value) {
        std::string escaped;
        for (const auto c : value) { escaped += c; if (c == '"') escaped += c; }
        return "C;X" + std::to_string(x) + ";Y" + std::to_string(y) + ";K\"" + escaped + "\"\n";
    };
    for (std::size_t x = 0; x < columns.size(); ++x) result += cell(x + 1, 1, columns[x]);
    for (std::size_t y = 0; y < rows.size(); ++y) for (std::size_t x = 0; x < columns.size(); ++x)
        if (const auto found = rows[y].find(columns[x]); found != rows[y].end()) result += cell(x + 1, y + 2, found->second);
    return result + "E\n";
}
struct Fixture {
    ObjectFieldSchema schema;
    StandardObjectCatalog standard;
    Fixture() {
        const std::map<std::string, std::string> files{
            {"fields.slk", Table({
                {{"ID", "umdl"}, {"field", "file"}, {"slk", "UnitUI"}, {"type", "model"}},
                {{"ID", "uico"}, {"field", "Art"}, {"slk", "Profile"}, {"type", "icon"}, {"index", "0"}},
                {{"ID", "umis"}, {"field", "Missiles"}, {"slk", "UnitUI"}, {"type", "modelList"}},
                {{"ID", "usnd"}, {"field", "Sound"}, {"slk", "UnitUI"}, {"type", "soundLabel"}},
                {{"ID", "unam"}, {"field", "Name"}, {"slk", "Profile"}, {"type", "string"}, {"index", "0"}}
            })},
            {"objects.slk", Table({{{"ID", "u001"}, {"Name", "合成单位"}, {"file", "Models/Base.mdl"},
                {"Art", "\"Icons/Base.blp\",\"Icons/Other.blp\""}, {"Missiles", "Models/First.mdx,Models/Second.mdx"}, {"Sound", "SyntheticSound"}}})},
            {"strings.txt", "[WorldEditStrings]\n"}, {"world.txt", "[ObjectEditorCategories]\n"}, {"editor.txt", "[test]\n"}
        };
        const auto reader = [&files](const std::string& path) {
            const auto found = files.find(path);
            return found == files.end() ? Result<std::vector<std::byte>>::Failure("Test", "MISSING", path)
                : Result<std::vector<std::byte>>::Success(Bytes(found->second));
        };
        ObjectFieldSchemaOptions schemaOptions;
        schemaOptions.sources = {{ObjectType::Unit, "fields.slk"}};
        schemaOptions.stringFiles = {"strings.txt"}; schemaOptions.worldEditDataFile = "world.txt";
        schemaOptions.unitEditorDataFile = "editor.txt";
        auto loaded = ObjectFieldSchema::Load(reader, schemaOptions); Require(loaded.Succeeded(), "fixture metadata"); schema = std::move(*loaded.value);
        CatalogLoadOptions catalogOptions;
        catalogOptions.sources = {{ObjectType::Unit, {"objects.slk"}, {}}}; catalogOptions.stringFiles = {"strings.txt"};
        auto catalog = StandardObjectCatalog::Load(reader, catalogOptions); Require(catalog.Succeeded(), "fixture catalog"); standard = std::move(*catalog.value);
    }
    ObjectResourceContext Context(std::span<const ProjectSearchObjectSource> sources = {}) const {
        ObjectResourceContext context;
        context.revision = "fixture:1"; context.schema = &schema; context.names.standardObjects = &standard;
        context.names.objects = sources; return context;
    }
};
void CaptureAndInheritance() {
    Fixture fixture;
    ObjectRecord custom; custom.key = {ObjectType::Unit, "u002"}; custom.parentRawcode = "u001";
    custom.modifications = {{{"unam"}, std::string("新单位")}, {{"umdl"}, std::string("Models/Custom.mdx")}, {{"uico"}, std::string{}}};
    ProjectSearchObjectSource source{ObjectType::Unit, std::span(&custom, 1), ProjectSearchState::Ready, ProjectSearchObjectOrigin::Native};
    auto context = fixture.Context(std::span(&source, 1));
    auto captured = CaptureObjectResourceCatalog(context); Require(captured.Succeeded(), "capture effective object resources");
    const auto& rows = (*captured.value)->rows;
    Require(std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.object->rawcode == "u001"
        && row.kind == ObjectResourceKind::Icon && row.value == "Icons/Base.blp"; }), "profile indexed icon reads only its component");
    Require(std::any_of(rows.begin(), rows.end(), [](const auto& row) { return row.object->rawcode == "u002"
        && row.value == "Models/Custom.mdx" && row.title == "新单位" && row.customObject; }), "native model override and name resolution");
    Require(std::none_of(rows.begin(), rows.end(), [](const auto& row) { return row.object->rawcode == "u002" && row.kind == ObjectResourceKind::Icon; }), "empty override does not restore base icon");
    std::vector<std::string> missiles;
    for (const auto& row : rows) if (row.object->rawcode == "u002" && row.fieldId == "umis") missiles.push_back(row.value);
    Require(missiles == std::vector<std::string>{"Models/First.mdx", "Models/Second.mdx"}, "inherited list retains order");
    const auto sound = DescribeObjectResourceField(*fixture.schema.FindNative(ObjectType::Unit, "usnd"));
    Require(sound && sound->kind == ObjectResourceKind::SoundLabel && !sound->storesPath, "sound label is not a file path");
    custom.modifications.push_back({{"umdl"}, std::string("Models/Conflict.mdx")});
    auto conflict = CaptureObjectResourceCatalog(context); Require(conflict.Succeeded() && !(*conflict.value)->complete, "duplicate resource reported partial");
    Require(std::none_of((*conflict.value)->rows.begin(), (*conflict.value)->rows.end(), [](const auto& row) {
        return row.object->rawcode == "u002" && row.fieldId == "umdl"; }), "ambiguous override does not expose base model");
    source.state = ProjectSearchState::Unavailable;
    auto unavailable = CaptureObjectResourceCatalog(context);
    Require(unavailable.Succeeded() && (*unavailable.value)->rows.empty() && !(*unavailable.value)->complete, "unknown map override does not silently fall back");
    ObjectResourceCatalogLimits cancelled; cancelled.progress = [](auto) { return false; };
    Require(!CaptureObjectResourceCatalog(context, cancelled).Succeeded(), "cancel does not report a successful partial index");
}
void PaginationAndIdentity() {
    Fixture fixture;
    auto captured = CaptureObjectResourceCatalog(fixture.Context()); Require(captured.Succeeded(), "capture for paging");
    ObjectResourceQuery query; query.expectedRevision = "fixture:1"; query.kind = ObjectResourceKind::Model;
    query.limit = 1; query.maxExamined = 1;
    std::set<std::string> ids;
    std::optional<ObjectResourceCursor> cursor;
    std::size_t examined = 0;
    do {
        auto page = QueryObjectResources(**captured.value, query, cursor); Require(page.Succeeded(), "bounded page");
        Require(page.value->rows.size() <= 1 && page.value->examined <= 1, "hard page and scan budget");
        examined += page.value->examined;
        for (const auto& row : page.value->rows) Require(ids.insert(row.id).second, "pagination no duplicate rows");
        cursor = page.value->next;
    } while (cursor);
    Require(ids.size() == 3 && examined == (*captured.value)->rows.size(), "all model rows across sparse pages");
    auto first = QueryObjectResources(**captured.value, query); Require(first.value->next.has_value(), "first cursor");
    query.search = "Custom";
    Require(!QueryObjectResources(**captured.value, query, first.value->next).Succeeded(), "cursor binds filter");
    query.expectedRevision = "fixture:2";
    Require(!QueryObjectResources(**captured.value, query).Succeeded(), "stale revision denied");
    query.expectedRevision = "fixture:1"; query.limit = 129;
    Require(!QueryObjectResources(**captured.value, query).Succeeded(), "limit cannot be widened");
}
void SourceFailureAndInfo() {
    Resources::ResourceResolver resolver;
    int lowerReads = 0;
    auto probe = [](std::string_view path) { return Result<Resources::ResourceProbe>::Success({Resources::ResourceProbeState::Found, std::string(path)}); };
    Require(resolver.AddSource({"base", Resources::ResourceLayer::Game, 300, probe, [&](std::string_view) {
        ++lowerReads; return Result<std::vector<std::byte>>::Success({}); }}).Succeeded(), "add base");
    Require(resolver.AddSource({"map", Resources::ResourceLayer::MapImport, 500, probe, [](std::string_view) {
        return Result<std::vector<std::byte>>::Failure("Test", "BROKEN_MAP_RESOURCE", "合成损坏覆盖"); }}).Succeeded(), "add map");
    auto broken = InspectObjectResource(resolver, "Icons/Test.tga", ObjectResourceKind::Texture);
    Require(!broken.Succeeded() && lowerReads == 0, "broken high layer never reads lower layer");
    (void)resolver.RemoveSource("map"); (void)resolver.RemoveSource("base");
    std::vector<std::byte> tga(18 + 2 * 3 * 3, std::byte{0});
    tga[2] = std::byte{2}; tga[12] = std::byte{2}; tga[14] = std::byte{3}; tga[16] = std::byte{24};
    Require(resolver.AddSource({"map", Resources::ResourceLayer::MapImport, 500, probe, [tga](std::string_view) { return Result<std::vector<std::byte>>::Success(tga); }}).Succeeded(), "add readable map image");
    auto info = InspectObjectResource(resolver, "Icons/Test.tga", ObjectResourceKind::Texture);
    Require(info.Succeeded() && info.value->source.sourceId == "map" && info.value->width == 2u && info.value->height == 3u, "decoded dimensions and exact source");
    Require(!InspectObjectResource(resolver, "../Test.tga", ObjectResourceKind::Texture).Succeeded(), "unsafe resource denied");
    Require(!InspectObjectResource(resolver, "Icons/Test.tga", ObjectResourceKind::Texture, 5).Succeeded(), "read byte limit preserved");
}
void TgaPacketsAndOrientation() {
    std::vector<std::byte> data(18, std::byte{});
    data[2] = std::byte{10}; data[12] = std::byte{2}; data[14] = std::byte{1}; data[16] = std::byte{24};
    data[17] = std::byte{0x30}; // 上边、右边为原点：首个红像素应出现在输出右侧。
    for (const auto byte : {1, 0, 0, 255, 255, 0, 0}) data.push_back(static_cast<std::byte>(byte));
    auto image = Image::DecodeTexture("pair.tga", data); Require(image.Succeeded(), "RLE raw packet decoded");
    Require(image.value->rgba[2] == std::byte{255} && image.value->rgba[4] == std::byte{255}, "TGA right origin is preserved");
    data[18] = std::byte{0x82}; Require(!Image::DecodeTexture("pair.tga", data).Succeeded(), "RLE run cannot exceed image");
    data[18] = std::byte{0x81}; data.resize(22); image = Image::DecodeTexture("pair.tga", data);
    Require(image.Succeeded() && image.value->rgba[0] == std::byte{255} && image.value->rgba[4] == std::byte{255}, "RLE repeated packet decoded");
    data.pop_back(); Require(!Image::DecodeTexture("pair.tga", data).Succeeded(), "truncated packet rejected");
}
void ModelSequencesAndSoundLabels() {
    std::map<std::string, std::vector<std::byte>> files{
        {"Models/Fixture.mdx", MdxTest::BuildTestMdx({.animatedHelper = true})},
        {"UI/SoundInfo/AbilitySounds.slk", Bytes(Table({{{"ID", "SyntheticSound"}, {"DirectoryBase", "Sound/Units/"}, {"FileNames", "First.wav,Second.wav"}}}))},
        {"Sound/Units/First.wav", Bytes("owned sound fixture")}};
    Resources::ResourceResolver resolver;
    Require(resolver.AddSource({"owned-map", Resources::ResourceLayer::MapImport, 500,
        [&](std::string_view path) {
            for (const auto& [name, bytes] : files) {
                const auto canonical = Resources::CanonicalImportPath(name);
                if (canonical.Succeeded() && *canonical.value == path)
                    return Result<Resources::ResourceProbe>::Success({Resources::ResourceProbeState::Found, name});
            }
            return Result<Resources::ResourceProbe>::Success({Resources::ResourceProbeState::Missing, {}});
        },
        [&](std::string_view path) { return Result<std::vector<std::byte>>::Success(files.at(std::string(path))); }}).Succeeded(), "owned resource fixtures");
    auto model = InspectObjectResource(resolver, "Models/Fixture.mdl", ObjectResourceKind::Model);
    Require(model.Succeeded() && model.value->resourcePath == "Models/Fixture.mdx", "model uses same MDL to MDX normalization");
    Require(model.value->sequences.size() == 2 && model.value->sequences[0].name == "Stand - 1"
        && model.value->sequences[0].startMs == 0 && model.value->sequences[0].endMs == 1000
        && model.value->sequences[0].loops && model.value->sequences[0].usable, "actual parsed animation interval and playback flags");
    auto sound = InspectObjectResource(resolver, "SyntheticSound", ObjectResourceKind::SoundLabel);
    Require(sound.Succeeded() && sound.value->requestedValue == "SyntheticSound" && sound.value->resourcePath == "Sound/Units/First.wav"
        && sound.value->source.sourceId == "owned-map" && !sound.value->diagnostics.empty(), "sound label resolves first actual file and reports selection rule");
    files["UI/SoundInfo/UISounds.slk"] = Bytes(Table({{{"ID", "SyntheticSound"}, {"DirectoryBase", "Sound/Other"}, {"FileNames", "Other.wav"}}}));
    sound = InspectObjectResource(resolver, "SyntheticSound", ObjectResourceKind::SoundLabel);
    Require(!sound.Succeeded() && sound.diagnostics.front().code == "OBJECT_RESOURCE_SOUND_LABEL_AMBIGUOUS", "conflicting sound table definitions rejected");
    files["UI/SoundInfo/UISounds.slk"] = Bytes("damaged table");
    Require(!InspectObjectResource(resolver, "SyntheticSound", ObjectResourceKind::SoundLabel).Succeeded(), "corrupt upper table cannot fall back to another matching label");
    Require(!InspectObjectResource(resolver, "SyntheticGroup", ObjectResourceKind::UnitSoundSet).Succeeded(), "group name is not guessed as a sound file");
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-resource-query-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { Require(std::filesystem::create_directory(root), "create owned fixture directory"); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root, error); }
};
void ImportAndGameFolders() {
    Temp temp;
    auto imp = Resources::ImportTable::CreateEmpty(); Require(imp.Upsert("Icons/Missing.blp").Succeeded(), "fixture missing import");
    auto impBytes = imp.Serialize(); Require(impBytes.Succeeded(), "serialize import table");
    std::filesystem::create_directory(temp.root / "Icons");
    Require(IO::WriteFileBytes(temp.root / "war3map.imp", *impBytes.value).Succeeded(), "write IMP fixture");
    Require(IO::WriteFileBytes(temp.root / "Icons" / "Present.tga", Bytes("image fixture")).Succeeded(), "write physical texture");
    auto directory = DirectoryWorkspace::Open(temp.root); Require(directory.Succeeded(), "open owned directory");
    ImportInventoryBinding binding{"picker", 1, "imports:1", directory.value->Snapshot().aggregateSha256, "assets:1", "effective-overlay"};
    ImportInventoryOverlay overlay{true, {}, {{"Models/Pending.mdx", 123}}};
    auto inventory = CaptureImportInventory(*directory.value, binding, overlay); Require(inventory.Succeeded(), "capture actual import inventory");
    auto catalog = CaptureImportResourceCatalog(**inventory.value, binding); Require(catalog.Succeeded(), "capture imported resources");
    ObjectResourceQuery query; query.expectedRevision = binding.revision; query.kind = ObjectResourceKind::Icon;
    auto page = QueryObjectResources(**catalog.value, query); Require(page.Succeeded() && page.value->rows.size() == 2, "IMP missing and physical image both listed");
    Require(std::any_of(page.value->rows.begin(), page.value->rows.end(), [](const auto& row) { return row.state == "missing"; }), "missing resource not suppressed");
    query.kind = ObjectResourceKind::Model;
    page = QueryObjectResources(**catalog.value, query); Require(page.Succeeded() && page.value->rows.size() == 1
        && page.value->rows.front().state == "queued-unfrozen", "pending resource retains unfrozen state");
    const std::vector<Mpq::MpqBuildFile> baseFiles{{"Models/Basic.mdx", Bytes("base"), false}, {"Icons/Basic.blp", Bytes("base image"), false}};
    const std::vector<Mpq::MpqBuildFile> patchFiles{{"Models/Basic.mdx", Bytes("patch"), false}};
    Require(Mpq::WriteMpqArchive(temp.root / "war3.mpq", baseFiles).Succeeded(), "write synthetic base archive");
    Require(Mpq::WriteMpqArchive(temp.root / "war3patch.mpq", patchFiles).Succeeded(), "write synthetic patch archive");
    auto game = Game::GameDataSource::MountStrict(temp.root); Require(game.Succeeded(), "mount strict synthetic game");
    auto gameCatalog = CaptureGameResourceCatalog(*game.value, "game:1"); Require(gameCatalog.Succeeded(), "capture actual MPQ file listing");
    query.expectedRevision = "game:1"; query.directory = "";
    page = QueryObjectResources(**gameCatalog.value, query); Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->rows.front().directory, "root contains matching model directory");
    query.directory = "Models";
    page = QueryObjectResources(**gameCatalog.value, query); Require(page.Succeeded() && page.value->rows.size() == 1
        && page.value->rows.front().sourceId == "war3patch.mpq", "game directory retains patch source and deduplicates base");
}
}
int main() {
    try { CaptureAndInheritance(); PaginationAndIdentity(); SourceFailureAndInfo(); TgaPacketsAndOrientation(); ModelSequencesAndSoundLabels(); ImportAndGameFolders(); std::cout << "Object resource query tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
