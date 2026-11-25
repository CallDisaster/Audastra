#include "ObjectCommandsFixture.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/Services/PlacementImport.h"

#include <Windows.h>
#include <cmath>
#include <iostream>
#include <numbers>

namespace {
using namespace War3Map;
using namespace War3Map::Tests;
using ObjectData::ObjectType;
using MapData::PlacementManifestKind;

void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "unexpected failure";
        for (const auto& diagnostic : result.diagnostics) message += "\n" + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
template<class T> bool HasCode(const T& value, std::string_view code) {
    return std::any_of(value.diagnostics.begin(), value.diagnostics.end(), [&](const auto& item) { return item.code == code; });
}
std::vector<std::byte> Json(std::string_view text) { return ObjectBytes(text); }

constexpr std::string_view kManifest = R"({"format":"sonne-placements/v1",
 "files":[{"source":"mdx/rock.mdx","archivePath":"Sonne\\rock.mdx"},{"source":"tex/rock.blp","archivePath":"Sonne\\Tex\\rock.blp"}],
 "types":[{"key":"rock","kind":"doodad","model":"Sonne\\rock.mdx","base":"D001"},
  {"key":"tree","kind":"doodad","model":"Doodads\\Tree.mdl","base":"D001","name":"Synthetic tree","followGround":true},
  {"key":"hero","kind":"unit","model":"SONNE\\ROCK.MDX","base":"u001"}],
 "instances":[{"type":"rock","x":0,"y":0,"z":310.5,"facing":90,"scale":[1,2,3]},
  {"type":"tree","x":128,"y":-128,"z":5,"facing":-90,"scale":[1.25,1.25,1.25],"variation":2},
  {"type":"hero","x":-256,"y":256,"z":12,"facing":720,"scale":[1,1,1]},
  {"type":"rock","x":1024,"y":-1024,"z":0,"facing":0,"scale":[0.5,0.5,0.5]}]})";

void ManifestRules() {
    const auto manifest = Take(MapData::ParsePlacementManifest(Json(kManifest)));
    Require(manifest.files.size() == 2 && manifest.files[1].archivePath == "Sonne\\Tex\\rock.blp" && manifest.files[1].bytes.empty()
        && manifest.types.size() == 3 && manifest.types[1].name == "Synthetic tree" && manifest.types[0].name == "rock"
        && manifest.types[1].followGround && !manifest.types[0].followGround && manifest.types[2].kind == PlacementManifestKind::Unit
        && manifest.instances.size() == 4 && manifest.instances[1].type == 1 && manifest.instances[1].variation == 2
        && manifest.instances[0].position == std::array<float, 3>{0, 0, 310.5f} && manifest.instances[0].scale == std::array<float, 3>{1, 2, 3},
        "清单字段逐项读出，name 缺省为 key，Sonne 模型路径不分大小写引用 files");
    const auto rejects = [](std::string text, std::string_view code) {
        Require(HasCode(MapData::ParsePlacementManifest(Json(text)), code), "非法清单给出对应诊断码");
    };
    const auto with = [](std::string_view from, std::string_view to) {
        std::string text(kManifest);
        const auto at = text.find(from); Require(at != std::string::npos, "test edit anchor");
        return text.replace(at, from.size(), to);
    };
    rejects(with("sonne-placements/v1", "other/v1"), "PLACEMENT_MANIFEST_FORMAT");
    rejects(with(R"("archivePath":"Sonne\\rock.mdx")", R"("archivePath":"Other\\rock.mdx")"), "PLACEMENT_MANIFEST_ARCHIVE_PATH");
    rejects(with(R"("archivePath":"Sonne\\Tex\\rock.blp")", R"("archivePath":"sonne\\ROCK.mdx")"), "PLACEMENT_MANIFEST_ARCHIVE_DUPLICATE");
    rejects(with(R"("archivePath":"Sonne\\Tex\\rock.blp")", R"("archivePath":"Sonne\\..\\x.blp")"), "PLACEMENT_MANIFEST_ARCHIVE_PATH");
    rejects(with(R"("source":"mdx/rock.mdx")", R"("source":"../rock.mdx")"), "PLACEMENT_MANIFEST_SOURCE_PATH");
    rejects(with(R"("model":"Sonne\\rock.mdx")", R"("model":"Sonne\\missing.mdx")"), "PLACEMENT_MANIFEST_MODEL");
    rejects(with(R"(,"base":"u001")", ""), "PLACEMENT_MANIFEST_BASE");
    rejects(with(R"("base":"u001")", R"("base":"u001","followGround":false)"), "PLACEMENT_MANIFEST_SHAPE");
    rejects(with(R"("kind":"unit")", R"("kind":"item")"), "PLACEMENT_MANIFEST_KIND");
    rejects(with(R"("key":"rock")", R"("key":"tree")"), "PLACEMENT_MANIFEST_KEY");
    rejects(with(R"({"type":"rock","x":0)", R"({"type":"stone","x":0)"), "PLACEMENT_MANIFEST_TYPE_REFERENCE");
    rejects(with(R"("scale":[1,2,3])", R"("scale":[1,0,3])"), "PLACEMENT_MANIFEST_SCALE");
    rejects(with(R"("scale":[1,2,3])", R"("scale":[1,2])"), "PLACEMENT_MANIFEST_SCALE");
    rejects(with(R"("variation":2)", R"("variation":-1)"), "PLACEMENT_MANIFEST_VARIATION");
    rejects(with(R"("z":310.5)", R"("z":310.5,"pitch":3)"), "PLACEMENT_MANIFEST_SHAPE");
    rejects(with(R"("x":128)", R"("x":1e7)"), "PLACEMENT_MANIFEST_NUMBER");
}

void ReadLoadsFilesRelativeToManifest() {
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-placement-import-"
        + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root / "mdx"); std::filesystem::create_directories(root / "tex");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    Require(IO::WriteFileBytes(root / "placements.json", Json(kManifest)).Succeeded()
        && IO::WriteFileBytes(root / "mdx" / "rock.mdx", Json("MDLX-synthetic")).Succeeded(), "write synthetic inputs");
    Require(!MapData::ReadPlacementManifest(root / "placements.json").Succeeded(), "缺少引用的文件时整份拒绝");
    Require(IO::WriteFileBytes(root / "tex" / "rock.blp", Json("BLP1")).Succeeded(), "write texture");
    const auto read = Take(MapData::ReadPlacementManifest(root / "placements.json"));
    Require(read.files[0].bytes == Json("MDLX-synthetic") && read.files[1].bytes == Json("BLP1"), "文件按清单目录相对路径装入");
}

MapData::TerrainHeader Header() {
    MapData::TerrainHeader header; header.width = header.height = 17; header.offset = {-1024, -1024}; return header;
}

std::optional<std::string> Field(const ObjectCommandsFixture& fixture, ObjectType type,
    const ObjectData::LniObjectDocument& document, const std::string& id, std::string_view fieldId) {
    auto context = fixture.Context(type); context.document = &document;
    context.documents[static_cast<std::size_t>(type)] = &document;
    for (const auto& row : Take(Services::ReadObjectFields(context, id)))
        if (row.key.fieldId == fieldId && row.value) {
            const auto* text = std::get_if<std::string>(&*row.value);
            return text ? *text : std::string("<non-text>");
        }
    return std::nullopt;
}

void PlanCreatesTypesAndRecords() {
    ObjectCommandsFixture fixture(true);
    const auto manifest = Take(MapData::ParsePlacementManifest(Json(kManifest)));
    const Services::PlacementImportInputs inputs{fixture.Context(ObjectType::Doodad), fixture.Context(ObjectType::Unit), Header(),
        [](std::string_view name) { return name == "Sonne\\rock.mdx"; }};
    const auto plan = Take(Services::PlanPlacementImport(manifest, inputs));
    const auto& report = plan.report;
    // 自动编号：base 首字母 + 三位 36 进制，跳过已有的 D001。
    Require(report.types.size() == 3 && report.types[0].rawcode == "D000" && report.types[1].rawcode == "D002"
        && report.types[2].rawcode == "u000" && report.types[0].base == "D001" && report.types[0].instances == 2
        && report.types[1].instances == 1 && report.types[2].instances == 1, "类型依次新建并报告 key→rawcode 与实例数");
    Require(report.files == 2 && report.replacedFiles == 1 && report.doodads == 3 && report.fixedHeightDoodads == 2
        && report.units == 1, "报告文件、替换、装饰物、保持高度与单位数量");
    Require(plan.doodadObjects && plan.unitObjects && plan.doodadObjects->before.Objects().empty()
        && plan.doodadObjects->after.Objects().size() == 2 && plan.unitObjects->after.Objects().size() == 1
        && plan.doodadObjects->label == "导入摆放清单", "物编计划 before 为输入文档，after 含全部新类型");
    const auto& doodads = plan.doodadObjects->after;
    const auto model = Field(fixture, ObjectType::Doodad, doodads, "D000", "dfil");
    const auto treeModel = Field(fixture, ObjectType::Doodad, doodads, "D002", "dfil");
    const auto name = Field(fixture, ObjectType::Doodad, doodads, "D002", "dnam");
    const auto unitModel = Field(fixture, ObjectType::Unit, plan.unitObjects->after, "u000", "umdl");
    Require(model && *model == "Sonne\\rock.mdx" && treeModel && *treeModel == "Doodads\\Tree.mdl"
        && name && *name == "Synthetic tree" && unitModel && *unitModel == "SONNE\\ROCK.MDX",
        "新类型写入模型路径与显示名");
    Require(plan.doodads.size() == 3 && plan.units.size() == 1, "实例按类别分开");
    const auto& rock = plan.doodads[0];
    Require(rock.rawcode == "D000" && rock.flags == 6 && rock.position == std::array<float, 3>{0, 0, 310.5f}
        && std::abs(rock.rotation - float(std::numbers::pi / 2)) < 1e-6f && rock.scale == std::array<float, 3>{1, 2, 3}
        && rock.lifePercent == 100 && rock.variation == 0, "绝对高度装饰物带 0x04，朝向换成弧度");
    const auto& tree = plan.doodads[1];
    Require(tree.rawcode == "D002" && tree.flags == 2 && std::abs(tree.rotation - float(3 * std::numbers::pi / 2)) < 1e-5f
        && tree.variation == 2, "贴地装饰物不带 0x04，负角度回绕到 0..2π");
    Require(plan.doodads[2].rawcode == "D000" && plan.doodads[2].position == std::array<float, 3>{1024, -1024, 0},
        "地图边缘上的实例在范围内");
    const auto& hero = plan.units[0];
    Require(hero.rawcode == "u000" && hero.owner == 15 && hero.rotation == 0.0f && hero.position == std::array<float, 3>{-256, 256, 12}
        && hero.flags == 2 && hero.heroLevel == 1, "单位归中立被动，720 度回到 0");
    Require(fixture.documents[static_cast<std::size_t>(ObjectType::Doodad)]->Objects().empty(), "规划不改输入物编文档");

    auto outside = manifest;
    outside.instances[1].position[0] = 1024.5f; outside.instances[3].position[1] = -2000.0f;
    const auto rejected = Services::PlanPlacementImport(outside, inputs);
    Require(HasCode(rejected, "PLACEMENT_IMPORT_OUT_OF_BOUNDS") && rejected.diagnostics.front().message.find("#1 (1024.5, -128)") != std::string::npos
        && rejected.diagnostics.front().message.find("#3 (1024, -2000)") != std::string::npos
        && rejected.diagnostics.front().message.starts_with("2 个实例"), "越界实例整份拒绝并列出序号与坐标");
    auto defaultBase = manifest; defaultBase.types[0].base.reset();
    const auto missingBase = Services::PlanPlacementImport(defaultBase, inputs);
    Require(!missingBase.Succeeded() && missingBase.diagnostics.front().message.starts_with("类型 rock："),
        "默认基础装饰物不在目录里时按类型报告，不猜替代");
    auto noSource = inputs; noSource.units.document = nullptr;
    Require(HasCode(Services::PlanPlacementImport(manifest, noSource), "PLACEMENT_IMPORT_OBJECT_SOURCE"), "缺少物编来源时拒绝");
}

void BatchCommitsBothFilesInOneRevision() {
    ObjectCommandsFixture fixture(true);
    const auto manifest = Take(MapData::ParsePlacementManifest(Json(kManifest)));
    const auto plan = Take(Services::PlanPlacementImport(manifest,
        {fixture.Context(ObjectType::Doodad), fixture.Context(ObjectType::Unit), Header(), {}}));
    MapData::UnitPlacements units; MapData::DoodadPlacements doodads;
    MapData::DoodadPlacement existing; existing.rawcode = "D001"; existing.scale = {1, 1, 1}; existing.flags = 2; existing.creationId = 41;
    doodads.doodads.push_back(existing);
    const auto document = MapDocument::FromMembers(std::vector<MapArchiveMember>{
        {"war3mapUnits.doo", Take(units.Serialize())}, {"war3map.doo", Take(doodads.Serialize())}});
    MapData::MapDataEditSession session(document);
    const auto before = session.Revision();
    const auto ids = Take(MapData::AddPlacementBatch(session, plan.units, plan.doodads));
    Require(ids.size() == 4 && session.Revision() != before && session.CanUndo(), "两类摆放一次修订提交");
    const auto snapshot = session.Snapshot();
    Require(snapshot.Units()->units.size() == 1 && snapshot.Units()->units[0].creationId == 0
        && snapshot.Doodads()->doodads.size() == 4 && snapshot.Doodads()->doodads[1].creationId == 42
        && snapshot.Doodads()->doodads[3].creationId == 44 && snapshot.Doodads()->doodads[1].flags == 6,
        "两个文件各自续接创建编号，字段原样写入");
    Require(session.Undo() && session.Snapshot().Doodads()->doodads.size() == 1 && session.Snapshot().Units()->units.empty(),
        "一次撤销撤回整批");
    Require(HasCode(MapData::AddPlacementBatch(session, {}, {}), "PLACEMENT_BATCH_LIMIT"), "空批次拒绝");
}
} // namespace

int main() {
    try {
        ManifestRules(); ReadLoadsFilesRelativeToManifest(); PlanCreatesTypesAndRecords(); BatchCommitsBothFilesInOneRevision();
        std::cout << "PlacementImportTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
