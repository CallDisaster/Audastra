#pragma once
#include "War3MapCore/Text/SlkCells.h"

// 固定成员只读对照；列出实际槽位和实际命中表，不补写缺失rawcode。
int AuditTerrainCliffTable(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& game, const std::filesystem::path& output) {
    Require(!std::filesystem::exists(output), "fresh cliff table audit required");
    const auto source = Take(MapArchiveSession::OpenMinimapAudit(path));
    Require(source.SourceSha256() == expected && expected.size() == 64, "cliff table audit source differs");
    const auto terrainBytes = Take(source.ReadMember("war3map.w3e"));
    const auto terrain = Take(ParseMapTerrain(terrainBytes)).Snapshot();
    const std::array<std::string, 1> names{"TerrainArt/CliffTypes.slk"};
    ArchiveExactMemberLimits limits; limits.memberBytes = limits.totalBytes = 2u * 1024u * 1024u;
    auto actual = Take(source.CaptureExactMembers(names, limits)).front();
    std::string winner = "map";
    if (!actual.present) {
        const auto mounted = Take(Game::GameDataSource::MountStrict(game, terrain->header.mainTileset));
        auto read = Take(mounted.ReadFileBoundedWithSource(names.front(), limits.memberBytes));
        actual.present = true; actual.bytes = std::move(read.bytes); winner = std::move(read.archiveName);
    }
    const std::string_view text(reinterpret_cast<const char*>(actual.bytes.data()), actual.bytes.size());
    const auto cells = Take(Text::ParseSlkCells(text, Text::SlkCellMode::CatalogUnique));
    Require(cells.size() <= 4096 && cells.contains(1), "bounded cliff table header required");
    nlohmann::json rows = nlohmann::json::array(), slots = nlohmann::json::array();
    std::set<std::string> ids;
    for (const auto& [row, values] : cells) {
        if (row == 1) continue;
        Require(values.contains(1) && ids.insert(values.at(1)).second, "unique actual cliff rows required");
        nlohmann::json fields = nlohmann::json::object();
        for (const auto& [column, value] : values) {
            Require(cells.at(1).contains(column), "declared cliff table column required");
            fields[cells.at(1).at(column)] = value;
        }
        rows.push_back({{"row", row}, {"fields", fields}});
    }
    std::array<std::size_t, 16> counts{};
    for (const auto& vertex : terrain->vertices) ++counts[vertex.CliffTextureIndex()];
    for (std::size_t slot = 0; slot < terrain->header.cliffTilesets.size(); ++slot)
        slots.push_back({{"slot", slot}, {"rawcode", terrain->header.cliffTilesets[slot]},
            {"actual_row_present", ids.contains(terrain->header.cliffTilesets[slot])},
            {"vertices", counts[slot]}});
    std::filesystem::create_directories(output);
    Check(IO::WriteFileBytes(output / "actual-CliffTypes.slk", actual.bytes));
    Check(IO::WriteFileBytes(output / "terrain.w3e", terrainBytes));
    Check(source.VerifySourceUnchanged());
    const auto report = nlohmann::json{{"source_sha256", expected}, {"source_unchanged", true}, {"map_written", false},
        {"main_tileset", terrain->header.mainTileset}, {"winner", winner}, {"table_sha256", Take(Sha256(actual.bytes))},
        {"w3e_sha256", Take(Sha256(terrainBytes))}, {"slots", slots}, {"rows", rows}}.dump(2);
    Check(IO::WriteFileBytes(output / "report.json", std::as_bytes(std::span(report))));
    std::cout << report << '\n'; return 0;
}
