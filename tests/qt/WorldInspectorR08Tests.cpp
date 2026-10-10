#include "WorldSelectionProperties.h"
#include "WorldOutlinerModel.h"
#include "../MapDataFixtures.h"
#include <QTest>

#include <array>
#include <cmath>

using namespace War3Map;
using namespace War3Map::Modules;

class WorldInspectorR08Tests final : public QObject {
    Q_OBJECT
private slots:
    void InstanceDiagnosticsAreLazyAndClearOnSourceReplacement() {
        MapData::PlacementId id{MapData::PlacementFile::Units, 3, "hfoo", 17};
        auto other = id; other.creationId = 18;
        QVERIFY(WorldPlacementHostKey(id) != WorldPlacementHostKey(other));
        other = id; other.file = MapData::PlacementFile::Doodads;
        QVERIFY(WorldPlacementHostKey(id) != WorldPlacementHostKey(other));
        Services::WorldSelectionVersion version{{"map", 1, 0, std::string(64, 'a')}, 1, 0}, live = version;
        std::vector<MapDataKind> kinds;
        const auto reader = [&](MapDataKind kind, const auto&, const auto&, const auto&) {
            kinds.push_back(kind);
            Services::WorldObjectPage page; page.version = version; page.member.kind = kind;
            if (kind == MapDataKind::Units) {
                Services::WorldObjectRow row; row.id = id; row.category = Services::WorldCategory::Unit;
                row.rowKey = "typed-placement"; page.rows.push_back(std::move(row));
            }
            return Result<Services::WorldObjectPage>::Success(std::move(page));
        };
        WorldOutlinerModel model; model.SetSource(reader); std::size_t calls = 0;
        model.SetDiagnosticReader([&](const auto& expected, const auto& selected) {
            ++calls;
            return expected.document == live.document && selected == Services::WorldObjectId{id}
                ? QStringLiteral("PRE2 source 2 <material>") : QString{};
        });
        QCOMPARE(calls, std::size_t{0});
        const auto tooltip = model.Find(id).data(Qt::ToolTipRole).toString();
        QVERIFY(tooltip.contains("PRE2 source 2") && tooltip.contains("&lt;material&gt;"));
        QCOMPARE(calls, std::size_t{1});
        ++live.document.generation;
        QVERIFY(!model.Find(id).data(Qt::ToolTipRole).toString().contains("PRE2 source 2"));
        version = live; model.SetSource(reader);
        QVERIFY(!model.Find(id).data(Qt::ToolTipRole).toString().contains("PRE2 source 2"));
        QCOMPARE(calls, std::size_t{2}); // 旧 provider 不再持有或返回原场景原因。
        const std::array routes{std::pair{Services::WorldCategory::Unit, MapDataKind::Units},
            std::pair{Services::WorldCategory::Item, MapDataKind::Units},
            std::pair{Services::WorldCategory::StartLocation, MapDataKind::Units},
            std::pair{Services::WorldCategory::Destructable, MapDataKind::Doodads},
            std::pair{Services::WorldCategory::Doodad, MapDataKind::Doodads},
            std::pair{Services::WorldCategory::Region, MapDataKind::Regions},
            std::pair{Services::WorldCategory::Camera, MapDataKind::Cameras}};
        for (const auto& [category, kind] : routes) {
            kinds.clear(); Services::WorldObjectFilter filter; filter.category = category; model.SetFilter(filter);
            QCOMPARE(kinds, std::vector<MapDataKind>{kind}); QVERIFY(!model.canFetchMore({}));
        }
        for (const auto category : {std::optional{Services::WorldCategory::Unknown}, std::optional<Services::WorldCategory>{}}) {
            kinds.clear(); Services::WorldObjectFilter filter; filter.category = category; model.SetFilter(filter);
            while (model.canFetchMore({})) model.fetchMore({});
            QCOMPARE(kinds, (std::vector<MapDataKind>{MapDataKind::Units, MapDataKind::Doodads, MapDataKind::Regions, MapDataKind::Cameras}));
        }
    }

    void MixedTransformsPlanOneTransactionAndRejectWholeInvalidGroup() {
        auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
        QVERIFY(units.Succeeded());
        MapData::UnitPlacement first;
        first.rawcode = "hfoo"; first.creationId = 9; first.flags = 2; first.scale = {1, 1, 1};
        first.position = {16, 20, 23};
        auto second = first; second.creationId = 10; second.position = {32, 40, 45};
        second.rotation = std::nextafter(0.0f, 1.0f); // 显示舍入相同，原始位型仍不同。
        units.value->units = {first, second};
        const auto bytes = units.value->Serialize(); QVERIFY(bytes.Succeeded());
        const std::array<MapArchiveMember, 1> members{{{"war3mapUnits.doo", *bytes.value}}};
        const auto document = MapDocument::FromMembers(members);
        const std::array ids{MapData::MakePlacementId(MapData::PlacementFile::Units, 0, first),
            MapData::MakePlacementId(MapData::PlacementFile::Units, 1, second)};
        const auto values = CaptureWorldPlacementProperties(document, ids);
        QVERIFY(values.Succeeded());
        QVERIFY(!values.value->Same(WorldPlacementProperty::Y));
        QVERIFY(!values.value->Same(WorldPlacementProperty::Facing));
        MapData::TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 2;
        terrain.vertices.resize(4); terrain.tiles.push_back({{0, 1, 2, 3}});
        terrain.vertices[0].position = {0, 0, 0}; terrain.vertices[1].position = {128, 0, 0};
        terrain.vertices[2].position = {0, 128, 128}; terrain.vertices[3].position = {128, 128, 128};
        const auto moves = PlanWorldPlacementProperty(document, terrain, ids, WorldPlacementProperty::Y, 96);
        QVERIFY(moves.Succeeded());
        QCOMPARE(moves.value->at(0).position, (std::array<float, 3>{16, 96, 99}));
        QCOMPARE(moves.value->at(1).position, (std::array<float, 3>{32, 96, 101}));
        MapData::MapDataEditSession edits(document);
        QVERIFY(MapData::MovePlacements(edits, *moves.value).Succeeded());
        QCOMPARE(edits.Revision(), std::uint64_t{1});
        QVERIFY(edits.Undo()); QVERIFY(!edits.IsDirty());
        QVERIFY(edits.Redo());
        auto stale = ids; stale[1].creationId = 99;
        QVERIFY(!PlanWorldPlacementProperty(document, terrain, stale, WorldPlacementProperty::Y, 96).Succeeded());
        QVERIFY(!PlanWorldPlacementProperty(document, terrain, ids, WorldPlacementProperty::X, 129).Succeeded());
        const std::array duplicate{ids[0], ids[0]};
        QVERIFY(!CaptureWorldPlacementProperties(document, duplicate).Succeeded());
        QCOMPARE(document.Units()->units[0].position, first.position);
        const auto rotation = CaptureWorldPlacementRotationOrigin(document, ids, {64, 30});
        QVERIFY(rotation.Succeeded());
        const auto rotated = BuildSelectedPlacementRotationMoves(document, terrain, *rotation.value, {24, 70}, 1.5707963267948966);
        QVERIFY(rotated.Succeeded());
        QVERIFY(std::abs(rotated.value->at(0).position[0] - 34) < 0.001f);
        QVERIFY(std::abs(rotated.value->at(0).position[1] - 22) < 0.001f);
        MapData::MapDataEditSession group(document);
        QVERIFY(MapData::MovePlacements(group, *rotated.value).Succeeded());
        QCOMPARE(group.Revision(), std::uint64_t{1});
        QVERIFY(group.Undo()); QVERIFY(!group.IsDirty());
        QVERIFY(!BuildSelectedPlacementRotationMoves(edits.Snapshot(), terrain, *rotation.value, {24, 70}).Succeeded());
    }
};

QTEST_GUILESS_MAIN(WorldInspectorR08Tests)
#include "WorldInspectorR08Tests.moc"
