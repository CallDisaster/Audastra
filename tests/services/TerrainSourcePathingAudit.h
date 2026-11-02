#pragma once
#include <nlohmann/json.hpp>

// 只读生成诊断G0，不授予保存资格；不将不可解释的旧WPM替换进地图。
int AuditTerrainSourcePathing(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& game, const std::filesystem::path& output) {
    Require(!std::filesystem::exists(output), "fresh source audit directory required");
    // 复用固定成员只读快照与有界资源延迟读取，不为诊断展开脚本或不透明引导成员。
    const auto source = Take(MapArchiveSession::OpenMinimapAudit(path));
    Require(expected.size() == 64 && source.SourceSha256() == expected, "source pathing audit identity differs");
    const auto terrainBytes = Take(source.ReadMember("war3map.w3e"));
    const auto before = *Take(ParseMapTerrain(terrainBytes)).Snapshot();
    const auto rules = Take(TerrainPathingSave::CaptureNative(source, before, before, game, kSaveBytes));
    const auto native = Take(rules.GenerateNative(before, kSaveBytes));
    const std::array<std::string, 1> pathingName{"war3map.wpm"};
    const auto capturedPathing = Take(source.CaptureExactMembers(pathingName));
    Require(capturedPathing.front().present, "source pathing audit requires an actual WPM");
    const auto& old = capturedPathing.front().bytes;
    Require(old.size() == native.size() + 16, "source WPM dimensions differ from generated grid");
    std::filesystem::create_directories(output);
    auto generated = old;
    std::copy(native.begin(), native.end(), generated.begin() + 16);
    Check(IO::WriteFileBytes(output / "generated-native.wpm", generated));
    Check(IO::WriteFileBytes(output / "source.wpm", old));
    Check(IO::WriteFileBytes(output / "terrain.w3e", terrainBytes));
    nlohmann::json samples = nlohmann::json::array(); std::size_t differences = 0;
    const auto width = (before.header.width - 1) * 4;
    for (std::size_t i = 0; i < native.size(); ++i) {
        const auto actual = old[i + 16] & std::byte{0xee};
        if (actual == native[i]) continue;
        ++differences;
        if (samples.size() < 32) samples.push_back({{"x", i % width}, {"y", i / width},
            {"world_x", before.header.offset[0] + (i % width + 0.5) * 32},
            {"world_y", before.header.offset[1] + (i / width + 0.5) * 32},
            {"source", std::to_integer<unsigned>(actual)}, {"generated", std::to_integer<unsigned>(native[i])},
            {"difference", std::to_integer<unsigned>(actual ^ native[i])}});
    }
    nlohmann::json nearbyDoodads = nlohmann::json::array(), nearbyUnits = nlohmann::json::array();
    const auto nearby = [&](const auto& placements) {
        nlohmann::json result = nlohmann::json::array();
        if (samples.empty()) return result;
        const double x = samples.front().at("world_x"), y = samples.front().at("world_y");
        std::vector<std::pair<double, std::size_t>> order;
        order.reserve(placements.size());
        for (std::size_t index = 0; index < placements.size(); ++index) {
            const auto& item = placements[index];
            Require(std::isfinite(item.position[0]) && std::isfinite(item.position[1]), "audit placement coordinate is nonfinite");
            const auto dx = static_cast<double>(item.position[0]) - x, dy = static_cast<double>(item.position[1]) - y;
            order.emplace_back(dx * dx + dy * dy, index);
        }
        const auto count = std::min<std::size_t>(16, order.size());
        std::partial_sort(order.begin(), order.begin() + count, order.end());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& item = placements[order[index].second];
            result.push_back({{"source_row", order[index].second}, {"rawcode", item.rawcode},
                {"creation_id", item.creationId}, {"position", item.position}, {"rotation", item.rotation},
                {"scale", item.scale}, {"distance_squared", order[index].first}});
        }
        return result;
    };
    if (source.HasMember("war3map.doo"))
        nearbyDoodads = nearby(Take(ParseDoodadPlacements(Take(source.ReadMember("war3map.doo")))).doodads);
    if (source.HasMember("war3mapUnits.doo"))
        nearbyUnits = nearby(Take(ParseUnitPlacements(Take(source.ReadMember("war3mapUnits.doo")))).units);
    nlohmann::json memberDigests = nlohmann::json::object();
    memberDigests["war3map.wpm"] = Take(Sha256(old));
    for (const auto member : {"war3map.w3e", "war3map.doo", "war3mapUnits.doo", "war3map.w3b", "war3map.w3d"})
        if (source.HasMember(member)) memberDigests[member] = Take(Sha256(Take(source.ReadMember(member))));
    Check(source.VerifySourceUnchanged());
    const auto report = nlohmann::json{{"source_sha256", expected}, {"source_unchanged", true},
        {"width", width}, {"height", (before.header.height - 1) * 4},
        {"differences", differences}, {"samples", samples}, {"nearby_doodads", nearbyDoodads},
        {"nearby_units", nearbyUnits}, {"member_sha256", memberDigests}, {"map_written", false}}.dump(2);
    Check(IO::WriteFileBytes(output / "report.json", std::as_bytes(std::span(report))));
    std::cout << report << '\n'; return 0;
}

// 独立原版诊断输入：只移除派生WPM，其余具名成员保持；不是正常保存候选。
int PrepareTerrainSourceOracle(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& target, bool withoutSpecials = false) {
    Require(!std::filesystem::exists(target), "fresh native source oracle required");
    const auto source = OpenOracleSource(path, expected);
    const std::vector<std::string> removals = withoutSpecials ? std::vector<std::string>{} : std::vector<std::string>{"war3map.wpm"};
    std::vector<MemberReplacement> replacements;
    if (withoutSpecials) {
        auto placements = Take(ParseDoodadPlacements(Take(source.ReadMember("war3map.doo"))));
        Require(!placements.specialDoodads.empty(), "diagnostic needs actual special records");
        placements.specialDoodads.clear();
        replacements.push_back({"war3map.doo", Take(placements.Serialize())});
    }
    const auto saved = Take(source.SaveCandidate(target, replacements, removals));
    const auto checked = OpenOracleSource(saved.path, Take(Sha256File(saved.path)));
    Require(withoutSpecials || !checked.HasMember("war3map.wpm"), "oracle WPM removal failed");
    for (const auto& member : source.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if ((!withoutSpecials && name == "war3map.wpm") || (withoutSpecials && name == "war3map.doo") || name == "(listfile)") continue;
        Require(Take(checked.ReadMember(member.name)) == member.data, "oracle changed a retained member");
    }
    Check(source.VerifySourceUnchanged());
    std::cout << "oracle_input_sha256=" << checked.SourceSha256() << "\nsource_unchanged=true\n";
    return 0;
}

// 原创原版对照输入：只翻转WPM中心格的一个通行位，完整回读核对后交给原版另存。
int PrepareTerrainPathingBitOracle(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& target) {
    Require(!std::filesystem::exists(target), "fresh pathing bit oracle required");
    const auto source = OpenOracleSource(path, expected);
    const auto terrain = *Take(ParseMapTerrain(Take(source.ReadMember("war3map.w3e")))).Snapshot();
    auto bytes = Take(source.ReadMember("war3map.wpm"));
    const auto width = static_cast<std::size_t>(terrain.header.width - 1) * 4;
    const auto height = static_cast<std::size_t>(terrain.header.height - 1) * 4;
    Require(width && height && width <= kSaveBytes / height && bytes.size() == 16 + width * height,
        "original bit oracle needs a complete bounded pathing grid");
    const auto offset = 16 + (height / 2) * width + width / 2;
    const auto before = bytes[offset]; bytes[offset] ^= std::byte{2};
    const std::array<MemberReplacement, 1> replacements{{{"war3map.wpm", bytes}}};
    const auto saved = Take(source.SaveCandidate(target, replacements));
    const auto checked = OpenOracleSource(saved.path, Take(Sha256File(saved.path)));
    Require(Take(checked.ReadMember("war3map.wpm")) == bytes, "pathing bit oracle readback differs");
    for (const auto& member : source.Members()) if (member.name != "war3map.wpm" && member.name != "(listfile)")
        Require(Take(checked.ReadMember(member.name)) == member.data, "pathing bit oracle changed another member");
    Check(source.VerifySourceUnchanged());
    std::cout << nlohmann::json{{"source_sha256", expected}, {"input_sha256", checked.SourceSha256()},
        {"source_unchanged", true}, {"x", width / 2}, {"y", height / 2}, {"offset", offset},
        {"mask", 2}, {"before", std::to_integer<unsigned>(before)},
        {"changed", std::to_integer<unsigned>(bytes[offset])}}.dump(2) << '\n';
    return 0;
}
