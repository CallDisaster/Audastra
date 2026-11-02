#pragma once
#include "War3MapCore/Resources/ImportTable.h"

// 原创单对象原版对照输入；不是语料保存成功结果，不改变给定源图。
int PrepareFixedShadowOracle(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& game, const std::filesystem::path& target,
    const std::filesystem::path& model = {}) {
    Require(!std::filesystem::exists(target), "fresh fixed-shadow oracle required");
    const auto source = OpenOracleSource(path, expected);
    const auto terrain = *Take(ParseMapTerrain(Take(source.ReadMember("war3map.w3e")))).Snapshot();
    Require(terrain.header.width == 65 && terrain.header.height == 65, "fixed-shadow fixture requires the complete 64-cell grid");
    Require(std::all_of(terrain.vertices.begin(), terrain.vertices.end(), [](const auto& vertex) {
        return vertex.groundHeightRaw == 8192 && vertex.CliffLevel() == 2 && !vertex.HasWater() && !vertex.IsRamp();
    }), "fixed-shadow fixture must be ordinary flat dry terrain");
    DoodadPlacements placements;
    DoodadPlacement placement;
    placement.rawcode = "YS14"; placement.variation = 0; placement.position = {0, 0, 0};
    placement.rotation = 0.75f; placement.scale = {1, 1, 1}; placement.flags = 2;
    placement.lifePercent = 100; placement.creationId = 1; placements.doodads.push_back(placement);
    (void)game;
    Require(Take(ParseDoodadPlacements(Take(source.ReadMember("war3map.doo")))).doodads.empty(),
        "fixed-shadow seed must not remove script-bound original objects");
    std::vector<MemberReplacement> replacements{{"war3map.doo", Take(placements.Serialize())},
        {"war3map.shd", std::vector<std::byte>(256u * 256u, std::byte{37})}};
    if (!model.empty()) {
        Require(Text::LowerAscii(model.extension().string()) == ".mdx"
            && std::filesystem::file_size(model) <= 512u * 1024u, "bounded authored MDX fixture required");
        const std::string member = "Buildings/Other/CityBuildingLarge_90/CityBuildingLarge_90.mdx";
        replacements.push_back({member, Take(IO::ReadFileBytes(model))});
        auto imports = source.HasMember("war3map.imp")
            ? Take(Resources::ImportTable::Parse(Take(source.ReadMember("war3map.imp"))))
            : Resources::ImportTable::CreateEmpty();
        Check(imports.Upsert(member, [&](std::string_view name) { return source.HasMember(name); }));
        replacements.push_back({"war3map.imp", Take(imports.Serialize())});
    }
    const auto saved = Take(source.SaveCandidate(target, replacements));
    const auto readback = OpenOracleSource(saved.path, Take(Sha256File(saved.path)));
    const auto observed = Take(ParseDoodadPlacements(Take(readback.ReadMember("war3map.doo"))));
    Require(observed.doodads.size() == 1 && observed.doodads[0].rawcode == placement.rawcode
        && observed.doodads[0].rotation == placement.rotation && observed.specialDoodads.empty(),
        "fixed-shadow oracle actual instance differs");
    Require(Take(readback.ReadMember("war3map.w3e")) == Take(source.ReadMember("war3map.w3e")),
        "fixed-shadow fixture must not substitute terrain");
    Require(Take(readback.ReadMember("war3mapUnits.doo")) == Take(source.ReadMember("war3mapUnits.doo")),
        "fixed-shadow seed must retain original unit identities used by triggers");
    Check(source.VerifySourceUnchanged());
    std::cout << nlohmann::json{{"source_sha256", expected}, {"oracle_sha256", readback.SourceSha256()},
        {"rawcode", placement.rawcode}, {"wire_rotation", placement.rotation},
        {"ordinary_flat_dry", true}, {"single_caster", true}, {"authored_model", !model.empty()}}.dump() << '\n';
    return 0;
}
