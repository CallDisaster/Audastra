#pragma once

// 原创原版对照输入，不是产品保存出口；只替换唯一空DOO并移除派生WPM。
int PrepareBridgeStateOracle(const std::filesystem::path& path, const std::string& expected,
    const std::filesystem::path& target, std::uint8_t flags,
    bool elevator = false, std::uint8_t life = 100) {
    Require(expected.size() == 64 && std::all_of(expected.begin(), expected.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "oracle SHA must be canonical");
    Require(flags == 0 || flags == 2 || flags == 6, "oracle compares empty, normal and absolute-height states only");
    Require(life == 0 || life == 100, "oracle compares explicit dead and intact states only");
    Require(!std::filesystem::exists(target), "oracle output must be fresh");
    Require(Take(Sha256File(path)) == expected, "oracle source identity differs");
    const auto metadata = Take(Mpq::MpqArchive::OpenReadOnly(path));
    const auto files = Take(metadata.ListFilesStrict());
    Require(files.size() <= 4096, "oracle member count budget");
    std::uint64_t bytes = 0;
    for (const auto& file : files) {
        auto name = file.name;
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : char(c); });
        Require(!name.ends_with(".ai"), "excluded member prevents oracle preparation");
        Require(file.fileSize <= kSaveBytes - bytes, "oracle decoded member budget"); bytes += file.fileSize;
    }
    const auto source = Take(MapArchiveSession::Open(path));
    Require(source.SourceSha256() == expected, "oracle source changed while opening");
    const auto terrain = Take(source.ReadMember("war3map.w3e"));
    auto placements = Take(ParseDoodadPlacements(Take(source.ReadMember("war3map.doo"))));
    Require(placements.recordLayout == PlacementRecordLayout::Classic && placements.doodads.empty()
        && placements.specialDoodads.empty(), "oracle needs an empty classic source object layer");
    if (flags) {
        DoodadPlacement bridge;
        bridge.rawcode = elevator ? "DTep" : "YT20";
        bridge.creationId = 0; bridge.flags = flags; bridge.lifePercent = life;
        bridge.position = {0.f, 0.f, 384.f}; bridge.scale = {1.f, 1.f, 1.f}; bridge.rotation = 0;
        placements.doodads.push_back(bridge);
    }
    const auto wire = Take(placements.Serialize());
    const std::array<MemberReplacement, 1> replacement{{{"war3map.doo", wire}}};
    const std::array<std::string, 1> removals{{"war3map.wpm"}};
    const auto saved = Take(source.SaveCandidate(target, replacement, removals));
    const auto checked = Take(MapArchiveSession::Open(saved.path));
    Require(Take(checked.ReadMember("war3map.w3e")) == terrain && !checked.HasMember("war3map.wpm")
        && Take(checked.ReadMember("war3map.doo")) == wire, "oracle publication must retain exact independent inputs");
    Check(source.VerifySourceUnchanged());
    std::cout << "oracle_input_sha256=" << checked.SourceSha256() << "\nsource_unchanged=true\npathing_removed=true\n";
    return 0;
}
