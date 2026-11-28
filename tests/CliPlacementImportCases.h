#pragma once

void PlacementImportReadsManifestBeforeSharedSave() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / ("Audastra-placements-cli-"
        + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(fs::create_directory(directory), "private placements CLI directory");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{directory};
    const auto write = [&](const char* name, std::string_view text) {
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        Require(War3Map::IO::WriteFileBytes(directory / name, bytes).Succeeded(), "write placements CLI input");
    };
    write("rock.mdx", "MDLX-synthetic");
    write("ok.json", R"({"format":"sonne-placements/v1","files":[{"source":"rock.mdx","archivePath":"Sonne\\rock.mdx"}],
        "types":[{"key":"rock","kind":"doodad","model":"Sonne\\rock.mdx"}],
        "instances":[{"type":"rock","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]}]})");
    write("missing-file.json", R"({"format":"sonne-placements/v1","files":[{"source":"absent.mdx","archivePath":"Sonne\\absent.mdx"}],
        "types":[{"key":"rock","kind":"doodad","model":"Sonne\\absent.mdx"}],
        "instances":[{"type":"rock","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]}]})");
    write("bad.json", R"({"format":"sonne-placements/v1","types":[{"key":"hero","kind":"unit","model":"x.mdx"}],
        "instances":[{"type":"hero","x":0,"y":0,"z":0,"facing":0,"scale":[1,1,1]}]})");
    const auto base = [&](const char* manifest) {
        auto args = Arguments(); args[4] = L"-";
        args.insert(args.end(), {L"--placements", (directory / manifest).wstring()});
        return args;
    };
    const auto valid = Invoke(base("ok.json"));
    Require(valid.exitCode != 0 && valid.output.empty() && valid.error.find("SAVE_ARGUMENT_INVALID") == std::string::npos
        && valid.error.find("PLACEMENT_MANIFEST") == std::string::npos,
        "a complete placement list is read locally and handed to the shared save service");
    auto twice = base("ok.json"); twice.insert(twice.end(), {L"--placements", (directory / "ok.json").wstring()});
    Rejected(Invoke(twice), "may appear only once");
    auto incomplete = Arguments(); incomplete.push_back(L"--placements"); Rejected(Invoke(incomplete), "requires <placements.json>");
    // 写明范围检查选项只随 --placements 使用；加宽须给观察点；余量须为正。
    auto stray = Arguments(); stray.insert(stray.end(), {L"--placements-standpoint", L"0", L"0", L"0"});
    Rejected(Invoke(stray), "need --placements");
    auto widenOnly = base("ok.json"); widenOnly.push_back(L"--placements-widen-bounds");
    Rejected(Invoke(widenOnly), "needs --placements-standpoint");
    auto badMargin = base("ok.json"); badMargin.insert(badMargin.end(), {L"--placements-margin", L"0"});
    Rejected(Invoke(badMargin), "--placements-margin requires");
    auto shortPoint = base("ok.json"); shortPoint.insert(shortPoint.end(), {L"--placements-standpoint", L"0", L"0"});
    Rejected(Invoke(shortPoint), "--placements-standpoint requires");
    auto bounded = base("ok.json");
    bounded.insert(bounded.end(), {L"--placements-standpoint", L"0", L"0", L"200", L"--placements-margin", L"256", L"--placements-widen-bounds"});
    const auto accepted = Invoke(bounded);
    Require(accepted.exitCode != 0 && accepted.error.find("SAVE_ARGUMENT_INVALID") == std::string::npos,
        "standpoint, margin and widen options are accepted with a placement list and handed to the save service");
    const auto missing = Invoke(base("missing-file.json"));
    Require(missing.exitCode == 2 && missing.output.empty() && missing.error.find("SAVE_ARGUMENT_INVALID") == std::string::npos,
        "an imported file that cannot be read stops before the save service");
    const auto bad = Invoke(base("bad.json"));
    Require(bad.exitCode == 2 && bad.error.find("PLACEMENT_MANIFEST_BASE") != std::string::npos,
        "manifest diagnostics are printed with their own code");
}
