#include "../src/Render/Tools/MdxScan/ScanPreview.h"
#include "AnimationParticleContinuityTests.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include <Windows.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Render::Scan;
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Good(Result<T> value) {
    if (!value.Succeeded()) throw std::runtime_error(value.diagnostics.empty() ? "fixture failure" : value.diagnostics.front().code);
    return std::move(*value.value);
}
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        path = std::filesystem::temp_directory_path() / ("Audastra-scan-scope-" +
            std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Check(std::filesystem::create_directory(path), "original fixture directory already exists");
    }
    ~Temporary() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::filesystem::path Companion(std::filesystem::path path, const char* suffix) { path += suffix; return path; }
std::vector<std::byte> ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Check(static_cast<bool>(input), "fixture file is unavailable");
    const auto size = input.tellg();
    Check(size >= 0 && size <= 1024 * 1024, "fixture file exceeded one MiB");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size)); input.seekg(0);
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(input), "fixture file read failed");
    return bytes;
}
std::string ReadText(const std::filesystem::path& path) {
    const auto bytes = ReadBytes(path);
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size())); output.close();
    Check(static_cast<bool>(output), "fixture list write failed");
}
void Archive(const std::filesystem::path& path, std::vector<Mpq::MpqBuildFile> files) {
    const auto result = Mpq::WriteMpqArchive(path, files);
    Check(result.Succeeded(), "original MPQ fixture write failed");
}
std::vector<std::byte> Model(char marker) {
    auto bytes = AnimationTest::Bytes();
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        std::uint32_t size{}; std::memcpy(&size, bytes.data() + at + 4, 4);
        Check(size <= bytes.size() - at - 8, "original MDX fixture chunk overflow");
        if (std::memcmp(bytes.data() + at, "MODL", 4) == 0) {
            Check(size >= 80, "original MDX fixture model name missing");
            bytes[at + 8] = static_cast<std::byte>(marker); return bytes;
        }
        at += size + 8;
    }
    throw std::runtime_error("original MDX fixture has no model header");
}
std::vector<std::byte> Terrain(char tileset) {
    // 原创 2x2 W3E v11；只为测试地图声明与显式上下文冲突。
    MdxTest::Writer writer; writer.Tag("W3E!"); writer.U32(11);
    writer.Raw(&tileset, 1); writer.U32(0);
    writer.U32(1); writer.Tag("Tst1"); writer.U32(1); writer.Tag("Clf1");
    writer.U32(2); writer.U32(2); writer.F32(0); writer.F32(0);
    const unsigned char flags[]{0, 0, 2};
    for (int i = 0; i < 4; ++i) { writer.U16(8192); writer.U16(8192); writer.Raw(flags, 3); }
    return std::move(writer.bytes);
}
struct Row { std::string model, sha, source, policy, state; };
std::vector<Row> Rows(const std::filesystem::path& path) {
    const auto lines = ReadLines(path, 1024 * 1024, 128);
    Check(!lines.empty() && lines.front().starts_with("model\tsha256\tsource\t"), "scope scan report header invalid");
    std::vector<Row> result;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::vector<std::string> cells;
        for (std::size_t at = 0; at <= lines[i].size();) {
            const auto end = lines[i].find('\t', at);
            cells.push_back(lines[i].substr(at, end == std::string::npos ? end : end - at));
            if (end == std::string::npos) break;
            at = end + 1;
        }
        Check(cells.size() == 10, "scope scan report row invalid");
        result.push_back({cells[0], cells[1], cells[2], cells[3], cells[4]});
    }
    return result;
}
void Identity(const std::vector<Row>& rows, std::string_view model, std::string_view sha, std::string_view source) {
    std::set<std::string> policies;
    for (const auto& row : rows) if (row.model == model) {
        Check(row.sha == sha && row.source == source, "model lost its exact resource layer or byte identity");
        Check(row.state == "complete", "original scope fixture did not finish observation");
        policies.insert(row.policy);
    }
    Check(policies == std::set<std::string>{"strict", "interactive"}, "scope scan lost a model or policy");
}
void Contains(const std::string& text, std::string_view expected, const char* message) {
    Check(text.find(expected) != std::string::npos, message);
}
void Run(const std::filesystem::path& scanner) {
    Temporary temp;
    const auto gameRoot = temp.path / "game";
    Check(std::filesystem::create_directory(gameRoot), "original game directory creation failed");
    const auto baseModel = Model('B'), nestedModel = Model('A'), patchModel = Model('P'), mapModel = Model('M');
    const auto texture = AnimationParticleContinuityTest::Texture();
    const std::vector<std::byte> damagedTexture(8);
    Archive(temp.path / "lower-a.mpq", {{"lower-only.mdx", baseModel}});
    Archive(temp.path / "a.mpq", {{"common.mdx", nestedModel}, {"variant.mdx", nestedModel},
        {"patched.mdx", nestedModel}, {"a-only.mdx", nestedModel}, {"Textures/Crate.blp", texture}});
    // 相同模型字节、不同依赖：不能按模型 SHA 将 Y 上下文合并进 A。
    Archive(temp.path / "y.mpq", {{"common.mdx", nestedModel}, {"y-only.mdx", nestedModel},
        {"Textures/Crate.blp", damagedTexture}});
    const auto aBytes = ReadBytes(temp.path / "a.mpq"), yBytes = ReadBytes(temp.path / "y.mpq");
    Archive(gameRoot / "war3.mpq", {{"base.mdx", baseModel}, {"variant.mdx", baseModel}, {"patched.mdx", baseModel},
        {"A.mpq", ReadBytes(temp.path / "lower-a.mpq")}, {"Z.mpq", std::vector<std::byte>(8)},
        {"Textures/Crate.blp", texture}, {"ReplaceableTextures/TeamColor/TeamColor00.blp", texture}});
    Archive(gameRoot / "war3x.mpq", {{"expansion.mdx", baseModel}, {"A.mpq", aBytes}, {"Y.mpq", yBytes}});
    Archive(gameRoot / "war3xlocal.mpq", {{"local.mdx", baseModel}});
    Archive(gameRoot / "war3patch.mpq", {{"patched.mdx", patchModel}});
    const auto map = temp.path / "original-map.w3x";
    Archive(map, {{"war3map.w3e", Terrain('A')}, {"common.mdx", mapModel}});
    std::map<std::filesystem::path, std::string> inputHashes;
    for (const auto& item : std::filesystem::directory_iterator(gameRoot)) inputHashes.emplace(item.path(), Good(Sha256File(item.path())));
    inputHashes.emplace(map, Good(Sha256File(map)));

    Options options; options.executable = scanner; options.gameRoot = gameRoot;
    options.tsv = temp.path / "default-names.txt"; options.mode = "enumerate";
    Check(RunPreview(options, {}) == 0, "default source enumeration failed");
    const auto defaultNames = ReadText(options.tsv);
    Contains(defaultNames, "base.mdx\n", "default enumeration lost base model");
    Check(defaultNames.find("common.mdx") == std::string::npos, "default context silently mounted a tileset");
    Contains(ReadText(Companion(options.tsv, ".scope.txt")), "game_context=default\n", "default context scope missing");

    options.mode.clear(); options.tsv = temp.path / "a.tsv";
    Check(RunPreview(options, {}, 'A') == 2, "game listfile coverage limit must remain explicit");
    const auto aRows = Rows(options.tsv);
    Check(aRows.size() == 14, "A merged context enumerated a wrong model set");
    const auto aLayer = "game:war3x.mpq|A.mpq|sha256:" + Good(Sha256(aBytes));
    Identity(aRows, "common.mdx", Good(Sha256(nestedModel)), aLayer);
    Identity(aRows, "variant.mdx", Good(Sha256(nestedModel)), aLayer);
    Identity(aRows, "patched.mdx", Good(Sha256(patchModel)), "game:war3patch.mpq");
    Identity(aRows, "base.mdx", Good(Sha256(baseModel)), "game:war3.mpq");
    Identity(aRows, "local.mdx", Good(Sha256(baseModel)), "game:war3xlocal.mpq");
    for (const auto& row : aRows) Check(row.model != "lower-only.mdx" && row.model != "y-only.mdx", "another tileset/archive leaked into A");
    const auto aScope = ReadText(Companion(options.tsv, ".scope.txt"));
    Contains(aScope, "main_tileset=65\nmain_tileset_letter=A\n", "A scope lost the actual tileset");
    Contains(aScope, "tileset_selection=explicit\n", "A scope lost explicit selection");
    Contains(aScope, "tileset_archive_state=mounted\n", "A scope did not record mounted nested archive");
    const auto aDiagnostics = ReadText(Companion(options.tsv, ".diagnostics.tsv"));
    Check(aDiagnostics.find("SCAN_RESOURCE_UNTEXTURED") == std::string::npos, "healthy A texture dependency was not resolved");

    options.list = temp.path / "common.txt"; WriteText(options.list, "common.mdx\n");
    options.tsv = temp.path / "y.tsv";
    Check(RunPreview(options, {}, 'Y') == 0, "explicit Y list scan failed");
    const auto yRows = Rows(options.tsv);
    Check(yRows.size() == 2, "explicit list was not passed to enumeration child");
    Identity(yRows, "common.mdx", Good(Sha256(nestedModel)), "game:war3x.mpq|Y.mpq|sha256:" + Good(Sha256(yBytes)));
    Contains(ReadText(Companion(options.tsv, ".scope.txt")), "game_context=tileset:Y\n", "Y context was omitted from scope");
    Contains(ReadText(Companion(options.tsv, ".diagnostics.tsv")), "SCAN_RESOURCE_UNTEXTURED", "Y dependency incorrectly used A/base texture");

    options.list.clear(); options.tsv = temp.path / "map.tsv";
    Check(RunPreview(options, map, 'A') == 0, "matching explicit/map terrain context was rejected");
    const auto mapRows = Rows(options.tsv);
    Check(mapRows.size() == 2, "map scan was expanded into the game corpus");
    Identity(mapRows, "common.mdx", Good(Sha256(mapModel)), "map:original-map.w3x");
    options.mode = "enumerate"; options.tsv = temp.path / "map-auto.txt";
    Check(RunPreview(options, map) == 0, "existing automatic map terrain context regressed");
    Contains(ReadText(Companion(options.tsv, ".scope.txt")), "tileset_selection=map-terrain\n", "automatic terrain origin missing");
    options.tsv = temp.path / "map-conflict.txt";
    bool conflict = false;
    try { static_cast<void>(RunPreview(options, map, 'Y')); }
    catch (const std::exception& error) { conflict = std::string_view(error.what()) == "SCAN_SOURCE_TILESET_MAP_CONFLICT"; }
    Check(conflict && !std::filesystem::exists(options.tsv), "explicit tileset silently replaced map W3E");

    options.tsv = temp.path / "missing.txt";
    Check(RunPreview(options, {}, 'Q') == 0, "absent optional tileset changed GameDataSource fallback");
    const auto missingScope = ReadText(Companion(options.tsv, ".scope.txt"));
    Contains(missingScope, "tileset_archive_state=absent\n", "absent nested archive falsely reported mounted");
    Contains(missingScope, "scope_notice=tileset-archive-absent\n", "missing tileset notice lost");
    options.tsv = temp.path / "corrupt.txt";
    bool corrupt = false;
    try { static_cast<void>(RunPreview(options, {}, 'Z')); } catch (const std::exception&) { corrupt = true; }
    Check(corrupt && !std::filesystem::exists(options.tsv), "corrupt selected tileset was skipped");

    // 真实命令行门禁：非法值、非预览模式、缺少游戏根以及重复参数都在读取前拒绝。
    int invalidIndex = 0;
    const auto rejected = [&](std::vector<std::string> arguments) {
        const auto output = temp.path / ("invalid-" + std::to_string(invalidIndex++) + ".txt");
        arguments.insert(arguments.end(), {"--tsv", Utf8(output), "--internal", "enumerate"});
        Check(RunChild(options, arguments, 5000) == "SCAN_CHILD_EXIT_1", "invalid tileset command line accepted");
        Check(!std::filesystem::exists(output), "invalid tileset command line produced a model list");
    };
    for (const auto* invalid : {"a", "AA", "../A"})
        rejected({"--preview", "1", "--game-root", Utf8(gameRoot), "--tileset", invalid});
    rejected({"--game-root", Utf8(gameRoot), "--tileset", "A"});
    rejected({"--preview", "1", "--list", Utf8(temp.path / "common.txt"), "--tileset", "A"});
    rejected({"--preview", "1", "--game-root", Utf8(gameRoot), "--tileset", "A", "--tileset", "Y"});
    for (const auto& [path, sha] : inputHashes) Check(Good(Sha256File(path)) == sha, "scan modified its source fixture");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "pass the built war3-mdx-scan executable");
        Run(std::filesystem::absolute(argv[1]));
        std::cout << "PASS original model scan source contexts and child arguments\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
