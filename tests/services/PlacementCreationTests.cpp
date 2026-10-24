#include "ObjectCommandsFixture.h"
#include "War3MapCore/Services/PlacementCreation.h"
#include <iostream>
#include <limits>

namespace {
using namespace War3Map;
using namespace Services;
using ObjectData::ObjectType;
void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
MapData::TerrainPointSample Sample() {
    MapData::TerrainPointSample sample; sample.point = {64, -128}; sample.groundHeight = 30; return sample;
}
void FourKindsAndNativeFields() {
    Tests::ObjectCommandsFixture fixture;
    for (const auto& kind : Tests::ObjectCommandsFixture::kinds) {
        if (kind.type != ObjectType::Unit && kind.type != ObjectType::Item
            && kind.type != ObjectType::Doodad && kind.type != ObjectType::Destructable) continue;
        auto context = fixture.Context(kind.type);
        const auto custom = PlanObjectCreation(context, kind.base, std::string(kind.custom), {});
        Require(custom.Succeeded(), "real custom parent fixture");
        *fixture.documents[static_cast<std::size_t>(kind.type)] = custom.value->after;
        for (const auto* id : {kind.base, kind.custom}) {
            PlacementCreateRequest request; request.type = kind.type; request.rawcode = id;
            request.point = Sample().point; request.heightOffset = 12; request.facingRadians = 1.25f;
            request.variation = 3; request.scale = {1, 2, 0.5f};
            const auto result = PreparePlacementCreation(request, context, Sample());
            Require(result.Succeeded(), "four standard/custom kinds accepted");
            std::visit([&](const auto& row) {
                Require(row.rawcode == id && row.position == std::array<float, 3>{64, -128, 42}
                    && row.rotation == 1.25f && row.variation == 3 && row.scale == request.scale,
                    "native position, rotation and variation preserved");
            }, *result.value);
            if (kind.type == ObjectType::Item) Require(std::get<MapData::UnitPlacement>(*result.value).owner == 15, "items neutral owner");
        }
    }
    PlacementCreateRequest request; request.rawcode = "u001"; request.point = Sample().point;
    request.owner = 15; request.heroLevel = 25; request.hitPointsPercent = 75; request.mana = 240;
    auto context = fixture.Context(ObjectType::Unit), items = fixture.Context(ObjectType::Item);
    request.itemSets = {{{{"i001", 40}, {"i001", 60}}}};
    const auto record = PreparePlacementCreation(request, context, Sample(), &items);
    Require(record.Succeeded(), "actual item directory validates inline drop groups");
    const auto& unit = std::get<MapData::UnitPlacement>(*record.value);
    Require(unit.owner == 15 && unit.heroLevel == 25 && unit.hitPoints == 75 && unit.mana == 240
        && unit.itemSets.size() == 1 && unit.itemSets[0].items.size() == 2 && unit.droppedItemTable == -1,
        "mana remains absolute; native inline drops unchanged");
    Require(context.document->Objects().size() == 1, "preparing placements preserves the existing custom object");
}
void RejectUnknownAndInvalidInputs() {
    Tests::ObjectCommandsFixture fixture; auto context = fixture.Context(ObjectType::Unit);
    auto items = fixture.Context(ObjectType::Item);
    PlacementCreateRequest valid; valid.rawcode = "u001"; valid.point = Sample().point;
    auto approximate = Sample(); approximate.groundHeightApproximate = true;
    const auto imprecise = PreparePlacementCreation(valid, context, approximate);
    Require(!imprecise.Succeeded() && !imprecise.value && imprecise.diagnostics.size() == 1
        && imprecise.diagnostics.front().code == "PLACEMENT_TERRAIN_APPROXIMATE",
        "precise creation cannot consume an unbound approximate ramp height");
    auto reject = [&](PlacementCreateRequest request) {
        Require(!PreparePlacementCreation(request, context, Sample(), &items).Succeeded(), "bad request must fail");
    };
    auto bad = valid; bad.rawcode = "xxxx"; reject(bad);
    bad = valid; bad.rawcode = "u00"; reject(bad);
    bad = valid; bad.type = ObjectType::Ability; reject(bad);
    bad = valid; bad.type = ObjectType::Item; reject(bad);
    bad = valid; bad.point.x += 1; reject(bad);
    bad = valid; bad.heightOffset = std::numeric_limits<float>::infinity(); reject(bad);
    bad = valid; bad.scale[1] = 0; reject(bad);
    bad = valid; bad.scale[1] = 1001; reject(bad);
    bad = valid; bad.facingRadians = -0.01f; reject(bad);
    bad = valid; bad.facingRadians = 6.28318530718f; reject(bad);
    bad = valid; bad.owner = 16; reject(bad);
    bad = valid; bad.heroLevel = 0; reject(bad);
    bad = valid; bad.hitPointsPercent = 101; reject(bad);
    bad = valid; bad.mana = -2; reject(bad);
    bad = valid; bad.itemSets = {{{{"i001", 101}}}}; reject(bad);
    bad = valid; bad.itemSets = {{{{"i001", 60}, {"i001", 50}}}}; reject(bad);
    bad = valid; bad.itemSets = {{{{"xxxx", 50}}}}; reject(bad);
    bad = valid; bad.itemSets.resize(33); reject(bad);
    bad = valid; bad.itemSets = {{{{"i001", 100}}}};
    Require(!PreparePlacementCreation(bad, context, Sample()).Succeeded(), "missing real drops source rejected");
    context.standard = nullptr;
    Require(!PreparePlacementCreation(valid, context, Sample()).Succeeded(), "missing standard source rejected");
    auto item = fixture.Context(ObjectType::Item); bad = valid; bad.type = ObjectType::Item; bad.rawcode = "i001"; bad.owner = 0;
    Require(!PreparePlacementCreation(bad, item, Sample()).Succeeded(), "item owner is not silently discarded");
    bad.owner.reset(); bad.mana = 1;
    Require(!PreparePlacementCreation(bad, item, Sample()).Succeeded(), "item mana not silently discarded");
    auto doodad = fixture.Context(ObjectType::Doodad); bad = valid; bad.type = ObjectType::Doodad; bad.rawcode = "D001"; bad.owner = 0;
    Require(!PreparePlacementCreation(bad, doodad, Sample()).Succeeded(), "doodad owner rejected");
    // 循环/断裂父链由同一物编命令服务拒绝，不能只看到自定义 rawcode 就放行。
    auto parsed = ObjectData::LniObjectCodec(ObjectType::Unit).Parse(Tests::ObjectBytes("[U001]\n_parent=\"U002\"\n[U002]\n_parent=\"U001\"\n"));
    auto* cyclic = parsed.Succeeded() ? dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(cyclic != nullptr, "cycle fixture parses");
    context = fixture.Context(ObjectType::Unit); context.document = cyclic; valid.rawcode = "U001";
    Require(!PreparePlacementCreation(valid, context, Sample()).Succeeded(), "cyclic inheritance rejected");
}
}
int main() {
    try { FourKindsAndNativeFields(); RejectUnknownAndInvalidInputs();
        std::cout << "PlacementCreation tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
