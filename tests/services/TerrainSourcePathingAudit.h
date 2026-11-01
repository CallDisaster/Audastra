#pragma once
#include <nlohmann/json.hpp>

// 只读生成诊断G0，不授予保存资格；不将不可解释的旧WPM替换进地图。
int AuditTerrainSourcePathing(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& game, const std::filesystem::path& output) {
    Require(!std::filesystem::exists(output), "fresh source audit directory required");
    const auto source = OpenOracleSource(path, expected);
    const auto terrainBytes = Take(source.ReadMember("war3map.w3e"));
    const auto before = *Take(ParseMapTerrain(terrainBytes)).Snapshot();
    const auto rules = Take(TerrainPathingSave::CaptureNative(source, before, before, game, kSaveBytes));
    const auto native = Take(rules.GenerateNative(before, kSaveBytes));
    const auto old = Take(source.ReadMember("war3map.wpm"));
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
    Check(source.VerifySourceUnchanged());
    const auto report = nlohmann::json{{"source_sha256", expected}, {"source_unchanged", true},
        {"width", width}, {"height", (before.header.height - 1) * 4},
        {"differences", differences}, {"samples", samples}, {"map_written", false}}.dump(2);
    Check(IO::WriteFileBytes(output / "report.json", std::as_bytes(std::span(report))));
    std::cout << report << '\n'; return 0;
}

// 独立原版诊断输入：只移除派生WPM，其余具名成员保持；不是正常保存候选。
int PrepareTerrainSourceOracle(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& target) {
    Require(!std::filesystem::exists(target), "fresh native source oracle required");
    const auto source = OpenOracleSource(path, expected);
    const std::array<std::string, 1> removals{{"war3map.wpm"}};
    const auto saved = Take(source.SaveCandidate(target, {}, removals));
    const auto checked = OpenOracleSource(saved.path, Take(Sha256File(saved.path)));
    Require(!checked.HasMember("war3map.wpm"), "oracle WPM removal failed");
    for (const auto& member : source.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.wpm" || name == "(listfile)") continue;
        Require(Take(checked.ReadMember(member.name)) == member.data, "oracle changed a retained member");
    }
    Check(source.VerifySourceUnchanged());
    std::cout << "oracle_input_sha256=" << checked.SourceSha256() << "\nsource_unchanged=true\n";
    return 0;
}
