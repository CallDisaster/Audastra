#include "War3MapCore/Services/BuildingSplatService.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
Resources::ResolvedResource Source(std::string text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {{"splats/ubersplatdata.slk", Resources::ResourceLayer::Game, 330, "fixture-game-patch", "Splats/UberSplatData.slk"},
        {bytes.begin(), bytes.end()}};
}
std::string Fixture(std::string_view middleAlpha = "255", std::string_view endRed = "255") {
    std::string text = "ID;PWXL;N;E\n";
    const std::array<std::string_view, 21> fields{"Name", "Dir", "file", "BlendMode", "Scale", "BirthTime", "PauseTime", "Decay",
        "StartR", "StartG", "StartB", "StartA", "MiddleR", "MiddleG", "MiddleB", "MiddleA", "EndR", "EndG", "EndB", "EndA", "Sound"};
    for (std::size_t x = 0; x < fields.size(); ++x) text += "C;X" + std::to_string(x + 1) + ";Y1;K\"" + std::string(fields[x]) + "\"\n";
    // 实际HMED/USMA定义的尺寸、颜色、时间；按列标题读取，不靠原文件列号。
    for (int y = 2; y <= 3; ++y) {
        const std::array<std::string_view, 21> values{y == 2 ? "HMED" : "USMA", "ReplaceableTextures/Splats",
            y == 2 ? "HumanUberSplat" : "UndeadUberSplat", "0", y == 2 ? "190" : "170", "1", "0", "2",
            "255", "255", "255", "0", "255", "255", "255", middleAlpha, endRed, "255", "255", "0", "_"};
        for (std::size_t x = 0; x < values.size(); ++x) text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + std::string(values[x]) + "\"\n";
    }
    return text + "E\n";
}
}
int main(int argc, char** argv) try {
    auto source = Source(Fixture());
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate); Require(bool(input), "actual table missing");
        const auto size = static_cast<std::streamoff>(input.tellg()); Require(size >= 0 && size <= 2 * 1024 * 1024, "actual table over input budget");
        std::string text(static_cast<std::size_t>(size), '\0'); input.seekg(0);
        Require(bool(input.read(text.data(), static_cast<std::streamsize>(text.size()))), "actual table read failed"); source = Source(std::move(text));
    } else Require(argc == 1, "usage: building-splat-test [actual table copy]");
    auto parsed = ParseBuildingSplatCatalog(std::move(source));
    if (!parsed.Succeeded()) { for (const auto& d : parsed.diagnostics) std::cerr << d.code << ": " << d.message << '\n'; }
    Require(parsed.Succeeded(), "catalog parse failed"); const auto catalog = *parsed.value;
    Require(catalog->definitions.at("HMED").scale == 190 && catalog->definitions.at("USMA").scale == 170, "real scale not retained");
    Require(catalog->definitions.at("HMED").end[3] == 0 && catalog->definitions.at("HMED").rawFields.contains("sound"), "raw color/capability metadata lost");
    const WorldSelectionVersion version{{"fixture-map", 1, 7, "fixture-source-sha"}, 3, 4};
    std::vector<BuildingSplatUnitInput> units(4);
    for (std::uint32_t i = 0; i < units.size(); ++i) {
        auto& unit = units[i]; unit.owner = {MapData::PlacementFile::Units, i, "hars", static_cast<std::int32_t>(i + 1)};
        unit.version = version; unit.position = {100, 200, 900}; unit.fields.complete = true; unit.fields.tint = {17, 51, 203};
        for (const auto field : {"uubs", "uclr", "uclg", "uclb"}) unit.fields.origins.push_back({field, "hars", "standard", "Units/UnitUI.slk", {}});
    }
    units[0].fields.uubs = "HMED"; units[1].fields.uubs = "USMA"; units[2].fields.uubs = "_"; units[3].fields.uubs = "ZZZZ";
    const BuildingSplatQueryContext context{version, "owned-unit-sequence-1", units};
    auto page = QueryBuildingSplatFootprints(catalog, context, version, catalog->identitySha256, {2, 65536});
    Require(page.Succeeded() && page.value->rows.size() == 2 && page.value->next.has_value(), "bounded first page failed");
    const auto& footprint = *page.value->rows[0].footprint;
    Require(footprint.fullWidth == 380 && footprint.minimum[0] == -90 && page.value->rows[1].footprint->fullWidth == 340, "native independent size incorrect");
    Require(footprint.color == std::array<std::uint8_t,4>{17,51,203,255} && footprint.buildingBirth == 0 && footprint.buildingPause == 0
        && !footprint.blendVerified && !footprint.projectionVerified && !footprint.lifecycleVerified, "runtime tint or evidence boundary wrong");
    auto tail = QueryBuildingSplatFootprints(catalog, context, version, catalog->identitySha256, {2, 65536}, page.value->next);
    Require(tail.Succeeded() && tail.value->rows[0].availability == BuildingSplatAvailability::Absent
        && tail.value->rows[1].availability == BuildingSplatAvailability::Unavailable && !tail.value->next, "absent and unknown key conflated");
    Require(tail.value->rows[0].reasonCode.empty() && tail.value->rows[1].reasonCode == "BUILDING_SPLAT_KEY_UNKNOWN",
        "known absence and unknown catalog key keep distinct machine-readable diagnostic states");
    auto nonBuilding = units[0]; nonBuilding.fields = {};
    nonBuilding.fields.complete = true;
    nonBuilding.fields.configuration = BuildingSplatConfiguration::NotApplicableToNonBuilding;
    nonBuilding.fields.reason = "完整链Absent，ubdg=0，schema building=1/unit=0";
    nonBuilding.fields.origins = {{"ubdg", "hars", "fixture-standard", "unitbalance.slk", {}},
        {"hero", "hars", "fixture-standard", "unit-category.slk", {}},
        {"creep", "hars", "fixture-standard", "unit-category.slk", {}},
        {"uubs-applicability-schema", "hars", "fixture-schema", "declared-table:UnitUI:field:uberSplat", {}}};
    const auto notApplicable = [&](const BuildingSplatUnitInput& input) {
        const std::array one{input};
        return QueryBuildingSplatFootprints(catalog, {version, "not-applicable-input", one}, version, catalog->identitySha256);
    };
    const auto classified = notApplicable(nonBuilding);
    Require(classified.Succeeded() && classified.value->rows[0].availability == BuildingSplatAvailability::Absent
        && classified.value->rows[0].reasonCode == "BUILDING_SPLAT_NOT_APPLICABLE" && !classified.value->rows[0].footprint
        && classified.value->rows[0].origins.size() == 4 && classified.value->rows[0].origins[0].field == "ubdg",
        "typed non-building applicability must stay distinct from Known empty uubs");
    for (int missing = 0; missing < 8; ++missing) {
        auto invalid = nonBuilding;
        if (missing == 0) invalid.fields.complete = false;
        if (missing == 1) invalid.fields.origins.erase(invalid.fields.origins.begin());
        if (missing == 2) invalid.fields.origins.pop_back();
        if (missing == 3) invalid.fields.uubs = "HMED";
        if (missing == 4) invalid.fields.origins.push_back({"uubs", "hars", "fixture", "unitui.slk", {}});
        if (missing == 5) invalid.fields.configuration = static_cast<BuildingSplatConfiguration>(255);
        if (missing == 6) invalid.fields.origins.erase(invalid.fields.origins.begin() + 1);
        if (missing == 7) invalid.fields.origins.erase(invalid.fields.origins.begin() + 2);
        const auto refused = notApplicable(invalid);
        Require(refused.Succeeded() && refused.value->rows[0].availability == BuildingSplatAvailability::Unavailable
            && refused.value->rows[0].reasonCode == "BUILDING_SPLAT_DATA_UNAVAILABLE" && !refused.value->rows[0].footprint,
            "incomplete or contradictory NotApplicable evidence cannot become absence");
    }
    auto stale = version; ++stale.document.revision;
    Require(!QueryBuildingSplatFootprints(catalog, context, stale, catalog->identitySha256).Succeeded(), "stale source accepted");
    Require(!QueryBuildingSplatFootprints(catalog, context, version, "other-source").Succeeded(), "stale catalog accepted");
    auto changedSequence = context; changedSequence.inputIdentity = "replaced-unit-sequence";
    Require(!QueryBuildingSplatFootprints(catalog, changedSequence, version, catalog->identitySha256, {2,65536}, page.value->next).Succeeded(), "stale cursor accepted");
    Require(!QueryBuildingSplatFootprints(catalog, context, version, catalog->identitySha256, {2, 1}).Succeeded(), "output budget ignored");
    const auto raw = ParseBuildingSplatCatalog(Source(Fixture("256", "256")));
    Require(raw.Succeeded() && (*raw.value)->definitions.at("HMED").middle[3] == 256
        && (*raw.value)->definitions.at("HMED").end[0] == 256 && (*raw.value)->definitions.at("HMED").rawFields.at("middlea") == "256"
        && (*raw.value)->originalBytes == Source(Fixture("256", "256")).bytes, "raw lifecycle integer truncated or changed");
    const auto rawPage = QueryBuildingSplatFootprints(*raw.value, context, version, (*raw.value)->identitySha256);
    Require(rawPage.Succeeded() && rawPage.value->rows[0].footprint->color == footprint.color
        && !rawPage.value->rows[0].footprint->lifecycleVerified, "unproven raw lifecycle executed as object RGBA");
    for (const auto invalid : {"-1", "nan", "4294967296", "1.0"})
        Require(!ParseBuildingSplatCatalog(Source(Fixture(invalid))).Succeeded(), "invalid uint32 lifecycle input accepted");
    units[0].fields.tint[0] = 256;
    const auto invalidTint = QueryBuildingSplatFootprints(catalog, context, version, catalog->identitySha256);
    Require(invalidTint.Succeeded() && invalidTint.value->rows[0].availability == BuildingSplatAvailability::Unavailable
        && !invalidTint.value->rows[0].footprint && invalidTint.value->rows[1].availability == BuildingSplatAvailability::Available,
        "object RGB overflow truncated or cleared legal rows"); units[0].fields.tint[0] = 17;
    units[1].owner = units[0].owner;
    const auto duplicate = QueryBuildingSplatFootprints(catalog, context, version, catalog->identitySha256);
    Require(!duplicate.Succeeded() && !duplicate.value, "duplicate owner returned partial data");
    auto broken = Fixture(); const auto value = broken.find("K\"190\""); Require(value != std::string::npos, "fixture scale missing");
    broken.replace(value, 6, "K\"nan\""); Require(!ParseBuildingSplatCatalog(Source(std::move(broken))).Succeeded(), "nonfinite table scale accepted");
    auto changedSource = Source(Fixture()); changedSource.location.sourceId = "different-game-layer";
    const auto other = ParseBuildingSplatCatalog(std::move(changedSource)); Require(other.Succeeded() && other.value.value()->identitySha256 != catalog->identitySha256, "same bytes masked different source");
    std::cout << "building splat CPU: definitions=" << catalog->definitions.size() << " HMED=380 USMA=340 tint=17,51,203,255 evidence=metadata-only\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
