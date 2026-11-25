#pragma once

void TerrainHeightfieldReadsManifestBeforeSharedSave() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / ("Audastra-heightfield-cli-"
        + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(fs::create_directory(directory), "private heightfield CLI directory");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{directory};
    const auto write = [&](const char* name, std::string_view text) {
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        Require(War3Map::IO::WriteFileBytes(directory / name, bytes).Succeeded(), "write heightfield CLI input");
    };
    std::string samples;
    for (unsigned i = 0; i < 6; ++i) {
        const auto bits = std::bit_cast<std::uint32_t>(float(i) * 8.0f);
        for (unsigned b = 0; b < 4; ++b) samples.push_back(char((bits >> (8 * b)) & 255));
    }
    write("hf.json", R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":3,"rows":2,"originWorld":[0,0],"fillHeight":0,"data":"hf.f32"})");
    write("hf.f32", samples);
    write("short.json", R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":3,"rows":2,"originWorld":[0,0],"fillHeight":0,"data":"short.f32"})");
    write("short.f32", std::string_view(samples).substr(0, 20));
    write("bad.json", R"({"format":"sonne-heightfield/v1","cellSize":64,"columns":3,"rows":2,"originWorld":[0,0],"fillHeight":0})");
    const auto base = [&](const char* manifest, const wchar_t* level = L"5", const wchar_t* sink = L"24") {
        auto args = Arguments();
        args.insert(args.end(), {L"--terrain-heightfield", (directory / manifest).wstring(), level, sink});
        return args;
    };
    const auto valid = Invoke(base("hf.json"));
    Require(valid.exitCode != 0 && valid.output.empty() && valid.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "a complete heightfield is read locally and reaches the shared source gate");
    auto follow = base("hf.json"); follow.push_back(L"--terrain-heightfield-follow");
    Require(Invoke(follow).error.find("SOURCE_LEASE_FAILED") != std::string::npos, "follow modifies the preceding import");
    auto mixed = base("hf.json"); mixed.insert(mixed.end(), {L"--stroke", L"raise", L"circle", L"4", L"4", L"0", L"8"});
    Require(Invoke(mixed).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "a heightfield and later brushes share one ordered service request");
    for (const auto* level : {L"15", L"-1", L"1.5", L"x"}) Rejected(Invoke(base("hf.json", level)), "cliff level 0..14");
    for (const auto* sink : {L"nan", L"inf", L"70000", L"x"}) Rejected(Invoke(base("hf.json", L"5", sink)), "cliff level 0..14");
    auto incomplete = base("hf.json"); incomplete.pop_back(); Rejected(Invoke(incomplete), "requires <heightfield.json>");
    auto orphan = Arguments(); orphan.push_back(L"--terrain-heightfield-follow");
    Rejected(Invoke(orphan), "must follow --terrain-heightfield");
    const auto shortData = Invoke(base("short.json"));
    Require(shortData.exitCode == 2 && shortData.output.empty()
        && shortData.error.find("TERRAIN_HEIGHTFIELD_DATA_SIZE") != std::string::npos, "truncated samples fail before any save");
    const auto badManifest = Invoke(base("bad.json"));
    Require(badManifest.exitCode == 2 && badManifest.error.find("TERRAIN_HEIGHTFIELD_CELL_SIZE") != std::string::npos,
        "manifest diagnostics are printed with their own code");
    const auto missing = Invoke(base("absent.json"));
    Require(missing.exitCode == 2 && missing.output.empty() && missing.error.find("SOURCE_LEASE_FAILED") == std::string::npos,
        "a missing manifest is reported before the source is opened");
}
