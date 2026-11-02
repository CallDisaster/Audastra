#pragma once
#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "War3MapCore/Game/NativeDoodadCategoryDirectory.h"

// 仅解释诊断点的普通DOO贡献；纯投影不授予旧WPM的保存资格。
nlohmann::json AuditFootprintPoint(const MapArchiveSession& source, const TerrainGridSnapshot& terrain,
    const std::filesystem::path& game, std::uint32_t x, std::uint32_t y) {
    constexpr std::size_t tableBytes = 2u * 1024u * 1024u, workBytes = 64u * 1024u * 1024u;
    const auto placements = Take(ParseDoodadPlacements(Take(source.ReadMember("war3map.doo"))));
    auto mounted = Take(Game::GameDataSource::MountStrict(game, terrain.header.mainTileset));
    const std::array<std::string, 6> names{"war3map.w3b", "war3map.w3d", "Units/DestructableData.slk",
        "Doodads/Doodads.slk", "Units/DestructableMetaData.slk", "Doodads/DoodadMetaData.slk"};
    ArchiveExactMemberLimits limits; limits.memberBytes = tableBytes; limits.totalBytes = 16u * 1024u * 1024u;
    auto inputs = Take(source.CaptureExactMembers(names, limits));
    nlohmann::json sources = nlohmann::json::array();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        std::string winner = inputs[i].present ? "map" : "missing";
        if (!inputs[i].present && i >= 2) {
            auto file = Take(mounted.ReadFileBoundedWithSource(names[i], tableBytes));
            inputs[i].present = true; inputs[i].bytes = std::move(file.bytes); winner = std::move(file.archiveName);
        }
        sources.push_back({{"name", names[i]}, {"winner", winner}, {"sha256", Take(Sha256(inputs[i].bytes))}});
    }
    std::vector<std::string_view> rawcodes, special;
    for (const auto& item : placements.doodads) rawcodes.push_back(item.rawcode);
    for (const auto& item : placements.specialDoodads) special.push_back(item.rawcode);
    const auto types = Take(ObjectData::InspectNativeTerrainObjects(rawcodes, special, inputs, workBytes));
    const auto directory = Take(Game::NativeDoodadCategoryDirectory::Capture(game,
        Game::NativeEditorStartupRules::Classic127aExpansion, workBytes));
    const auto width = static_cast<std::size_t>(terrain.header.width - 1) * 4;
    const auto height = static_cast<std::size_t>(terrain.header.height - 1) * 4;
    Require(width && height && width <= workBytes / height && x < width && y < height, "bounded footprint audit grid required");
    std::vector<std::uint8_t> grid(width * height);
    nlohmann::json hits = nlohmann::json::array();
    std::size_t textureRead = 0;
    for (const auto& type : types.footprints) {
        const std::array<std::string, 1> path{type.pathTexture};
        auto file = Take(source.CaptureExactMembers(path, limits)).front();
        std::string winner = "map";
        if (!file.present) {
            auto read = Take(mounted.ReadFileBoundedWithSource(path.front(), tableBytes));
            file.bytes = std::move(read.bytes); winner = std::move(read.archiveName);
        }
        Require(file.bytes.size() <= 16u * 1024u * 1024u - textureRead, "footprint audit cumulative texture budget exceeded");
        textureRead += file.bytes.size();
        auto image = Text::LowerAscii(path.front()).ends_with(".blp")
            ? Take(DecodeDoodadFootprintBlpBudgeted(file.bytes, tableBytes))
            : Take(DecodeDoodadFootprintTgaBudgeted(file.bytes, tableBytes));
        const auto snap = type.gridPolicy == TerrainDoodadGridPolicy::StartupCategoryC ? directory.CategoryCSnapCells() : 2;
        for (std::size_t row = 0; row < placements.doodads.size(); ++row) {
            const auto& item = placements.doodads[row];
            if (item.rawcode != type.rawcode) continue;
            std::fill(grid.begin(), grid.end(), 0);
            Check(AddDoodadFootprint(grid, terrain.header, item, image.footprint, snap, type.fixedRotationDegrees, type.contribution));
            if (!grid[y * width + x]) continue;
            hits.push_back({{"row", row}, {"rawcode", item.rawcode}, {"creation_id", item.creationId},
                {"flags", item.flags}, {"life_percent", item.lifePercent}, {"position", item.position},
                {"path", path.front()}, {"winner", winner}, {"texture_sha256", Take(Sha256(file.bytes))},
                {"selector2_attributes", grid[y * width + x]},
                {"walkable_suppression", type.contribution == TerrainNativeFootprintContribution::WalkableTerrainSuppression}});
        }
    }
    return {{"x", x}, {"y", y}, {"sources", sources}, {"ordinary_footprint_hits", hits}, {"qualification_granted", false}};
}
