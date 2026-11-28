#include "ObjectCommandsFixture.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/Services/PlacementImport.h"
#include "War3MapCore/Render/MdxDeclaredExtents.h"
#include "War3MapCore/Render/MdxModel.h"
#include "../support/GeosetLimitMdxFixture.h"

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

// 写明范围检查：地图 x/y 为 -1024…1024。rock 在轴心附近；wall 的范围偏到 x 100000 外（整个在地图外）；
// deep 的范围深埋 z -6000…-5000（在地图内但离观察点远），摆放两处（第二处朝向 90°、缩放 2）。
constexpr std::string_view kBoundsManifest = R"({"format":"sonne-placements/v1",
 "files":[{"source":"rock.mdx","archivePath":"Sonne\\rock.mdx"},{"source":"wall.mdx","archivePath":"Sonne\\wall.mdx"},
  {"source":"deep.mdx","archivePath":"Sonne\\deep.mdx"},{"source":"bad.mdx","archivePath":"Sonne\\bad.mdx"}],
 "types":[{"key":"rock","kind":"doodad","model":"Sonne\\rock.mdx","base":"D001"},
  {"key":"wall","kind":"doodad","model":"Sonne\\wall.mdx","base":"D001"},
  {"key":"deep","kind":"doodad","model":"SONNE\\DEEP.MDX","base":"D001"},
  {"key":"pebble","kind":"doodad","model":"Sonne\\rock.mdx","base":"D001","followGround":true},
  {"key":"bad","kind":"doodad","model":"Sonne\\bad.mdx","base":"D001"},
  {"key":"tree","kind":"doodad","model":"Doodads\\Tree.mdl","base":"D001"},
  {"key":"hero","kind":"unit","model":"Sonne\\rock.mdx","base":"u001"}],
 "instances":[{"type":"rock","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]},
  {"type":"wall","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]},
  {"type":"deep","x":512,"y":0,"z":0,"facing":0,"scale":[1,1,1]},
  {"type":"deep","x":-512,"y":256,"z":0,"facing":90,"scale":[2,2,2]},
  {"type":"pebble","x":0,"y":0,"z":-9000,"facing":0,"scale":[1,1,1]},
  {"type":"bad","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]},
  {"type":"tree","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]},
  {"type":"hero","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]}]})";

std::vector<std::byte> ModelWithBox(std::array<float, 3> minimum, std::array<float, 3> maximum) {
    GeosetLimitFixture::Extent extent{100, {minimum[0], minimum[1], minimum[2]}, {maximum[0], maximum[1], maximum[2]}};
    GeosetLimitFixture::Extents extents;
    extents.model = extents.sequence = extents.geoset = extents.geosetSequence = extent;
    return GeosetLimitFixture::BuildMdx({{}}, extents);
}

MapData::PlacementManifest BoundsManifest() {
    auto manifest = Take(MapData::ParsePlacementManifest(Json(kBoundsManifest)));
    manifest.files[0].bytes = ModelWithBox({-64, -64, 0}, {64, 64, 128});
    manifest.files[1].bytes = ModelWithBox({100000, -64, 0}, {100100, 64, 128});
    manifest.files[2].bytes = ModelWithBox({-64, -64, -6000}, {64, 64, -5000});
    manifest.files[3].bytes = Json("MDLX-synthetic");
    return manifest;
}

std::size_t CountCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [&](const auto& item) { return item.code == code; }));
}
const Diagnostic& WithCode(const std::vector<Diagnostic>& diagnostics, std::string_view code, std::string_view text) {
    for (const auto& item : diagnostics) if (item.code == code && item.message.find(text) != std::string::npos) return item;
    throw std::runtime_error("missing diagnostic " + std::string(code) + " with " + std::string(text));
}

void BoundsCheckReportsAndWidens() {
    ObjectCommandsFixture fixture(true);
    const auto manifest = BoundsManifest();
    Services::PlacementImportInputs inputs{fixture.Context(ObjectType::Doodad), fixture.Context(ObjectType::Unit), Header(), {}};
    inputs.groundHeight = [](double, double) { return std::optional<float>(0.0f); };

    // 没给观察点：只查地图外。
    const auto plain = Take(Services::PlanPlacementImport(manifest, inputs));
    const auto& bounds = plain.report.bounds;
    Require(bounds.checkedInstances == 5 && bounds.uncheckedInstances == 3 && bounds.offMap == 1 && bounds.farFromStandpoint == 0
        && bounds.both == 0 && bounds.findings.size() == 1 && bounds.findings[0].instance == 1
        && bounds.findings[0].problem == Services::PlacementBoundsProblem::OffMap && bounds.findings[0].box[0] == 100000,
        "without a standpoint only the wall placement is reported, off the map");
    Require(plain.report.types[1].offMap == 1 && plain.report.types[0].boundsChecked && !plain.report.types[4].boundsChecked
        && !plain.report.types[5].boundsChecked && !plain.report.types[6].boundsChecked, "per-type summary and coverage");
    Require(bounds.models.size() == 4 && bounds.models[3].archivePath == "Sonne\\bad.mdx" && !bounds.models[3].checked
        && !bounds.models[3].uncheckedReason.empty() && bounds.models[0].placements == 2, "models in file order, shared by two types");
    WithCode(plain.report.warnings, "PLACEMENT_IMPORT_BOUNDS_OFF_MAP", "类型 wall（模型 Sonne\\wall.mdx）：1 个摆放");
    WithCode(plain.report.warnings, "PLACEMENT_IMPORT_BOUNDS_UNCHECKED", "Sonne\\bad.mdx");
    Require(CountCode(plain.report.warnings, "PLACEMENT_IMPORT_BOUNDS_FAR") == 0 && plain.fileRewrites.empty()
        && &Services::PlacementImportFileBytes(manifest, plain, 1) == &manifest.files[1].bytes, "no standpoint, no far check, no rewrite");

    // 观察点 (0, 0, 200)、余量 512：范围须包住 P±512。wall 两项都有；deep 两处深埋；rock 与贴地的 pebble 的范围
    // 只到 z 128，P 在范围外 72，同样没包住。深度为负值是 P 在范围外的距离。
    auto withPoint = inputs; withPoint.bounds.standpoint = std::array<double, 3>{0, 0, 200};
    const auto checked = Take(Services::PlanPlacementImport(manifest, withPoint));
    const auto& found = checked.report.bounds.findings;
    using Services::PlacementBoundsProblem;
    Require(found.size() == 5 && found[0].instance == 0 && found[0].problem == PlacementBoundsProblem::FarFromStandpoint
        && std::abs(found[0].clearance + 72) < 1e-6 && found[1].instance == 1 && found[1].problem == PlacementBoundsProblem::Both
        && found[2].instance == 2 && found[2].problem == PlacementBoundsProblem::FarFromStandpoint
        && std::abs(found[2].clearance + 5200) < 1e-6 && found[3].instance == 3 && std::abs(found[3].clearance + 10200) < 1e-3
        && found[4].instance == 4 && std::abs(found[4].clearance + 72) < 1e-6,
        "every placement whose extents do not hold P +- 512 is far, with the depth of P inside its extents");
    Require(checked.report.bounds.both == 1 && checked.report.bounds.farFromStandpoint == 4 && checked.report.bounds.offMap == 0
        && checked.report.types[2].farFromStandpoint == 2 && checked.report.types[1].both == 1
        && checked.report.types[0].farFromStandpoint == 1 && checked.report.types[3].farFromStandpoint == 1, "exclusive counts");
    WithCode(checked.report.warnings, "PLACEMENT_IMPORT_BOUNDS_FAR", "类型 deep（模型 Sonne\\deep.mdx）：2 个摆放");
    WithCode(checked.report.warnings, "PLACEMENT_IMPORT_BOUNDS_FAR", "#2（-5200）、#3（-10200）");
    WithCode(checked.report.warnings, "PLACEMENT_IMPORT_BOUNDS_OFF_MAP", "类型 wall");
    auto noGround = withPoint; noGround.groundHeight = {};
    const auto floating = Take(Services::PlanPlacementImport(manifest, noGround));
    Require(floating.report.bounds.findings.size() == 5 && floating.report.bounds.findings[4].instance == 4
        && std::abs(floating.report.bounds.findings[4].clearance + 9072) < 1e-6,
        "without a ground height the follow-ground placement is checked at its listed z");

    // P 正好深入范围 512（余量为 512）时不报；差一点就报，加宽后补足。
    auto inner = manifest;
    inner.files[2].bytes = ModelWithBox({-1024, -512, -312}, {0, 512, 712});
    auto deepOnly = withPoint;
    const auto exact = Take(Services::PlanPlacementImport(inner, deepOnly));
    Require(std::none_of(exact.report.bounds.findings.begin(), exact.report.bounds.findings.end(),
        [](const auto& finding) { return finding.instance == 2; }), "a box that holds P exactly 512 deep is not reported");
    inner.files[2].bytes = ModelWithBox({-1024, -512, -312}, {0, 512, 700});
    const auto shallow = Take(Services::PlanPlacementImport(inner, deepOnly));
    const auto shallowFinding = std::find_if(shallow.report.bounds.findings.begin(), shallow.report.bounds.findings.end(),
        [](const auto& finding) { return finding.instance == 2; });
    Require(shallowFinding != shallow.report.bounds.findings.end() && std::abs(shallowFinding->clearance - 500) < 1e-6,
        "a box that holds P only 500 deep is far even though P is inside");

    // 加宽：只改有问题的 rock、wall 与 deep；deep 取两处摆放各自局部坐标里 P±512 的并集。读不了范围的 bad 不动。
    auto widen = withPoint; widen.bounds.widen = true;
    const auto widened = Take(Services::PlanPlacementImport(manifest, widen));
    Require(widened.fileRewrites.size() == 3 && widened.fileRewrites[0].file == 0 && widened.fileRewrites[1].file == 1
        && widened.fileRewrites[2].file == 2 && widened.report.bounds.widenedModels == 3
        && widened.report.bounds.models[2].widenedExtents == 4
        && &Services::PlacementImportFileBytes(manifest, widened, 3) == &manifest.files[3].bytes
        && &Services::PlacementImportFileBytes(manifest, widened, 2) == &widened.fileRewrites[2].bytes,
        "only the flagged models are rewritten; the others keep the manifest bytes");
    WithCode(widened.report.warnings, "PLACEMENT_IMPORT_BOUNDS_WIDENED", "模型 Sonne\\deep.mdx：已在导入的副本里加宽 4 条");
    const auto& deep = widened.fileRewrites[2].bytes;
    const auto deepExtents = Take(Render::ReadMdxDeclaredExtents(deep, Render::MdxExtentScope::All));
    const auto& box = deepExtents.model;
    const auto near1 = [](float value, double expected) { return std::abs(double(value) - expected) < 1e-2; };
    Require(near1(box.minimum[0], -1024) && near1(box.maximum[0], 128) && near1(box.minimum[1], -512) && near1(box.maximum[1], 512)
        && near1(box.minimum[2], -6000) && near1(box.maximum[2], 712), "union of (P - pivot) ± 512 over both placements in local space");
    const auto original = Take(Render::ReadMdxDeclaredExtents(manifest.files[2].bytes, Render::MdxExtentScope::All));
    std::vector<char> field(deep.size(), 0);
    for (const auto* extent : {&original.model, &original.sequences[0], &original.geosets[0], &original.geosetSequences[0]})
        std::fill_n(field.begin() + static_cast<std::ptrdiff_t>(extent->offset), Render::kMdxExtentBytes, 1);
    for (std::size_t at = 0; at < deep.size(); ++at)
        Require(field[at] || deep[at] == manifest.files[2].bytes[at], "only extent bytes change in the imported copy");
    Require(Render::ParseMdx(deep).Succeeded() && Render::ParseMdx(widened.fileRewrites[0].bytes).Succeeded(),
        "widened copies still parse as complete models");
    auto fixed = manifest;
    for (const auto& rewrite : widened.fileRewrites) fixed.files[rewrite.file].bytes = rewrite.bytes;
    const auto rechecked = Take(Services::PlanPlacementImport(fixed, withPoint));
    Require(rechecked.report.bounds.findings.empty() && rechecked.report.bounds.checkedInstances == 5,
        "after widening every checked placement holds the standpoint +- 512 and reaches the map");

    // 选项：加宽须给观察点；观察点须在地图内；余量须为正。
    auto noPoint = inputs; noPoint.bounds.widen = true;
    Require(HasCode(Services::PlanPlacementImport(manifest, noPoint), "PLACEMENT_IMPORT_BOUNDS_OPTIONS"), "widen needs a standpoint");
    auto outside = withPoint; outside.bounds.standpoint = std::array<double, 3>{5000, 0, 0};
    Require(HasCode(Services::PlanPlacementImport(manifest, outside), "PLACEMENT_IMPORT_BOUNDS_OPTIONS"), "standpoint off the map");
    auto zero = withPoint; zero.bounds.margin = 0;
    Require(HasCode(Services::PlanPlacementImport(manifest, zero), "PLACEMENT_IMPORT_BOUNDS_OPTIONS"), "margin must be positive");
}
} // namespace

int main() {
    try {
        ManifestRules(); ReadLoadsFilesRelativeToManifest(); PlanCreatesTypesAndRecords(); BatchCommitsBothFilesInOneRevision();
        BoundsCheckReportsAndWidens();
        std::cout << "PlacementImportTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
