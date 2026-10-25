#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Jass/VJass.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
std::string Text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string ReadText(const std::filesystem::path& path) {
    // 仅明确列出的本项目源文件；拒绝把 AI 当文本读入。
    Require(path.extension() != L".ai" && path.extension() != L".AI", "AI is not a text source");
    auto bytes = IO::ReadFileBytes(path, 1024 * 1024);
    Require(bytes.Succeeded(), "read original source"); return Text(*bytes.value);
}
}
// 只打包原创 map-create 种子；不启动游戏、不改作者地图。
// opaque-loader 仅按文档作为不透明导入交给 MPQ writer；仅校验大小与摘要，不解析或输出源码。
int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 8 || argc == 9, "usage: map-runtime-fixture <seed.w3x> <seed-sha> <game-root> <repo-root> <runtime.dll-or-control.blp-or-dash> <opaque-loader> <new-output-dir> [probe|probe-early|probe-thread|probe-thread-p0|control-tiara|raw-probe|raw-runtime]");
        const std::wstring_view mode = argc == 9 ? argv[8] : L"runtime";
        const bool rawRuntime = mode == L"raw-runtime";
        const bool probe = mode != L"runtime" && !rawRuntime;
        const bool control = mode == L"control-tiara";
        const bool rawProbe = mode == L"raw-probe";
        const bool hasRuntime = !probe || rawProbe;
        const bool playerZeroProbe = mode == L"probe-thread-p0";
        const bool threadProbe = playerZeroProbe || mode == L"probe-thread";
        const bool earlyProbe = control || rawProbe || threadProbe || mode == L"probe-early";
        const bool earlyStart = earlyProbe || !probe;
        Require(!probe || earlyProbe || mode == L"probe", "unknown fixture mode");
        const std::filesystem::path seed(argv[1]), game(argv[3]), repo(argv[4]), runtime(argv[5]), loader(argv[6]), out(argv[7]);
        const bool rawDebug = rawRuntime && loader.filename() == L"warvk_loader_raw_debug.ai";
        const std::string loaderImport = control ? "tiara.ai" : rawProbe ? "warvk_raw_probe.ai"
            : rawRuntime ? (rawDebug ? "warvk_loader_raw_debug.ai" : "warvk_loader_raw.ai")
            : threadProbe ? "warvk_probe_thread.ai" : probe ? "warvk_probe.ai" : "warvk_loader.ai";
        const std::string probePlayer = playerZeroProbe ? "0" : "PLAYER_NEUTRAL_AGGRESSIVE";
        const std::string probeCall = "call StartCampaignAI(Player(" + probePlayer + "), \"" + loaderImport + "\")";
        const auto expected = IO::PathUtf8(argv[2]);
        auto identity = Sha256File(seed);
        Require(identity.Succeeded() && *identity.value == expected, "original seed SHA mismatch");
        Require(!std::filesystem::exists(out) && std::filesystem::is_directory(out.parent_path()), "new output directory required");
        Require(probe ? loader.filename() == (control ? L"tiara.ai" : rawProbe ? L"warvk_raw_probe.ai"
                : threadProbe ? L"warvk_probe_thread.ai" : L"warvk_probe.ai")
            : rawRuntime ? (loader.filename() == L"warvk_loader_raw.ai" || rawDebug)
            : (loader.filename() == L"warvk_loader.ai" || loader.filename() == L"warvk_loader_debug.ai"),
            "documented opaque loader name required");
        Require(control ? runtime.filename() == L"tiara.blp" : rawProbe ? runtime.filename() == L"warvk_runtime.blp"
            : !probe || runtime == L"-", "probe payload must match documented mode");
        auto source = Mpq::MpqArchive::OpenReadOnly(seed);
        Require(source.Succeeded(), "open original seed");
        auto names = source.value->ListFilesStrict();
        const std::set<std::string> allowed{"war3map.w3i", "war3map.w3e", "war3map.wpm", "war3mapUnits.doo",
            "war3map.doo", "war3map.wtg", "war3map.wct", "war3map.w3r", "war3map.w3c", "war3map.wts", "war3map.shd", "war3map.j"};
        Require(names.Succeeded() && names.value->size() <= allowed.size() + 1, "bounded original seed members");
        std::vector<Mpq::MpqBuildFile> members;
        std::string script;
        for (const auto& name : *names.value) {
            if (name.name == "(listfile)") continue;
            Require(allowed.contains(name.name), "non-original seed member");
            auto bytes = source.value->ReadFileBounded(name.name, 1024 * 1024);
            Require(bytes.Succeeded(), "read original seed member");
            if (name.name == "war3map.j") { script = Text(*bytes.value); continue; }
            if (name.name == "war3map.w3i") Require(Text(*bytes.value).find("Audastra Lua Runtime Fixture") != std::string::npos,
                "requires the named original runtime seed");
            members.push_back({name.name, std::move(*bytes.value)});
        }
        const auto main = script.find("function main takes nothing returns nothing");
        const auto end = main == std::string::npos ? std::string::npos : script.find("endfunction", main);
        Require(end != std::string::npos && script.find("StartCampaignAI") == std::string::npos, "original entry required");
        if (earlyProbe) {
            // 文档允许纯 JASS 直接插入 main 首行；保留历史延迟探针以供对照。
            // 调用端提示不冒充黑盒输出；本模式完全不包含延迟启动计时器。
            script.insert(end, control
                ? "    call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, \"AUD_CONTROL: main-first call returned; observe child TEMP\")\n"
                : rawProbe
                ? "    call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, \"AUD_RAW_PROBE: main-first call returned; observe WARVKPRB in child TEMP\")\n"
                : "    call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, \"AUD_PROBE: main-first call returned; awaiting black-box marker\")\n");
            const auto body = script.find('\n', main);
            Require(body != std::string::npos, "original main body required");
            script.insert(body + 1, "    " + probeCall + '\n');
        } else if (probe) {
            script.insert(end, "    call AjAiProbeStart()\n");
            script = ReadText(repo / "sdk/japi/sample/AiProbeSmoke.j") + '\n' + script;
        } else {
            script.insert(end, "    call AjMapSmokeStart()\n");
            const auto body = script.find('\n', main);
            Require(body != std::string::npos, "original main body required");
            script.insert(body + 1, "    " + probeCall + '\n');
            script = "library AudastraJapi\n" + ReadText(repo / "sdk/japi/jass/AudastraJapi.j") + "\nendlibrary\n"
                + ReadText(repo / "assets/ui-library/AudastraUI/jass/AudastraUI.j") + '\n'
                + ReadText(repo / "sdk/japi/sample/MapRuntimeSmoke.j") + '\n' + script;
        }
        auto mounted = Game::GameDataSource::MountStrict(game);
        Require(mounted.Succeeded(), "mount dedicated stock game scripts");
        auto commonBytes = mounted.value->ReadFileBounded("Scripts\\common.j", 4 * 1024 * 1024);
        auto blizzardBytes = mounted.value->ReadFileBounded("Scripts\\blizzard.j", 4 * 1024 * 1024);
        Require(commonBytes.Succeeded() && blizzardBytes.Succeeded(), "read stock script declarations");
        const auto common = Text(*commonBytes.value), blizzard = Text(*blizzardBytes.value);
        Jass::VJassOptions options;
        options.baseScripts = {{"common.j", common}, {"blizzard.j", blizzard}};
        options.check.threads = 1;
        auto compiled = Jass::TranslateVJass(script, "MapRuntimeSmoke.vjass", options);
        for (const auto& d : compiled.diagnostics) std::cerr << d.message << '\n';
        Require(compiled.Succeeded() && compiled.completed, "vJASS compilation failed");
        if (earlyStart) {
            const std::string entry = "function main takes nothing returns nothing";
            const auto entryAt = compiled.text.find(entry);
            Require(entryAt != std::string::npos, "compiled main required");
            const auto first = compiled.text.find_first_not_of(" \t\r\n", entryAt + entry.size());
            Require(first != std::string::npos && compiled.text.compare(first, probeCall.size(), probeCall) == 0,
                "AI must be the first compiled main statement");
            const auto next = compiled.text.find_first_not_of(" \t\r\n", first + probeCall.size());
            Require(next != std::string::npos && compiled.text.compare(next, 21, "call SetCameraBounds(") == 0,
                "camera bounds must follow early AI call");
            Require(compiled.text.find("call StartCampaignAI(") == first
                && compiled.text.find("call StartCampaignAI(", first + 1) == std::string::npos,
                "exactly one AI call required");
            if (earlyProbe) Require(compiled.text.find("TimerStart") == std::string::npos
                && compiled.text.find("AjAiProbeStart") == std::string::npos,
                "early probe must exclude delayed startup");
        }
        auto sources = options.baseScripts;
        sources.push_back({"war3map.j", compiled.text});
        const auto checked = Jass::CheckJass(sources, options.check);
        for (const auto& d : checked.diagnostics) std::cerr << d.message << '\n';
        Require(checked.Succeeded() && checked.completed && checked.semanticChecked, "stock JASS semantic check failed");
        members.push_back({"war3map.j", Bytes(compiled.text)});
        std::string runtimeHash;
        std::string controlPayloadHash;
        if (hasRuntime) {
            auto dll = IO::ReadFileBytes(runtime, 16 * 1024 * 1024);
            Require(dll.Succeeded(), "read built runtime DLL");
            const auto runtimeIdentity = Sha256(*dll.value);
            Require(runtimeIdentity.Succeeded(), "hash runtime import");
            runtimeHash = *runtimeIdentity.value;
            if (rawProbe || rawRuntime) Require(dll.value->size() == 419840 && runtimeHash ==
                "24272e6010512462d8546fd277f15b82bff78f67167760e523acec7734abbf82",
                "raw modes require frozen runtime payload size/SHA");
            members.push_back({"warvk_runtime.blp", std::move(*dll.value)});
        }
        if (control) {
            // 第三方阳性候选仅作黑盒对照；不解释或执行载荷内容，不混入本方runtime。
            auto payload = IO::ReadFileBytes(runtime, 4 * 1024 * 1024);
            Require(payload.Succeeded() && payload.value->size() == 3577344, "control payload size mismatch");
            auto payloadIdentity = Sha256(*payload.value);
            Require(payloadIdentity.Succeeded() && *payloadIdentity.value ==
                "8ad46cd5df63b4ea92a031933907b4f9a13c5cc85041e3d0a51ff5d4d56e6a47", "control payload SHA mismatch");
            controlPayloadHash = *payloadIdentity.value;
            members.push_back({"tiara.blp", std::move(*payload.value)});
        }
        // 黑盒仅整体打包和摘要校验；绝不转为文本或进行源码分析。
        auto opaque = IO::ReadFileBytes(loader, 1024 * 1024);
        Require(opaque.Succeeded(), "import opaque loader");
        const auto loaderSize = opaque.value->size();
        const auto loaderIdentity = Sha256(*opaque.value);
        Require(loaderIdentity.Succeeded(), "hash opaque loader import");
        if (earlyProbe) {
            const size_t expectedSize = control ? 26880 : rawProbe ? 26772 : threadProbe ? 381 : 180;
            const std::string_view expectedSha = control
                ? "f89956db8af169934feffee3d4cbd144a4fdae277c2eaf7e8fe321c8970077c0" : rawProbe
                ? "38e9af50741f146fe57780614425a285cd9bf6808202cab181469ed5d934fbb5" : threadProbe
                ? "c32351783972b4e0943305226b9c85aa1ef3ba4ebe91d6a3d2df13262d6a8fd5"
                : "c75dba8d51b1fdd595ce5f4c64e098b64f2f324d78156219b55b83519cd2ada6";
            Require(loaderSize == expectedSize && *loaderIdentity.value == expectedSha,
                "early probe requires the unchanged documented name/size/SHA");
        }
        if (rawRuntime) Require(loaderSize == (rawDebug ? 26951 : 26772) && *loaderIdentity.value == (rawDebug
            ? "67f80aaa9edfe70bf26c0c6771975f7e6cd813bda7014088fcd755594b11952d"
            : "7bbc430b8b2e4960dd6eb05ddd8f0f0607e5a1d8c9e1af12b726955848894eea"),
            "raw runtime requires unchanged documented name/size/SHA");
        members.push_back({loaderImport, std::move(*opaque.value)});
        if (!probe) {
            members.push_back({"AudastraSmoke.toc", Bytes("AudastraSmoke.fdf\r\n")});
            members.push_back({"AudastraSmoke.fdf", Bytes(
                "Frame \"TEXT\" \"AjSmokeText\" {\r\n    DecorateFileNames,\r\n"
                "    FrameFont \"MasterFont\", 0.016, \"\",\r\n"
                "    FontJustificationH JUSTIFYCENTER,\r\n    FontJustificationV JUSTIFYMIDDLE,\r\n}\r\n")});
        }
        members.push_back({"LICENSE-MEMHACK.txt", Bytes(ReadText(repo / "sdk/japi/ai/LICENSE-MEMHACK.txt"))});
        // 导入清单只写固定路径，绝不枚举或解释黑盒内部。
        const std::vector<std::string> importNames = control
            ? std::vector<std::string>{"tiara.ai", "tiara.blp", "LICENSE-MEMHACK.txt"} : rawProbe
            ? std::vector<std::string>{loaderImport, "warvk_runtime.blp", "LICENSE-MEMHACK.txt"} : probe
            ? std::vector<std::string>{loaderImport, "LICENSE-MEMHACK.txt"}
            : std::vector<std::string>{"warvk_runtime.blp", loaderImport, "AudastraSmoke.toc", "AudastraSmoke.fdf", "LICENSE-MEMHACK.txt"};
        std::string imports("\x01\0\0\0\0\0\0\0", 8);
        imports[4] = static_cast<char>(importNames.size());
        for (const auto& name : importNames) {
            imports += '\x0d'; imports += name; imports += '\0';
        }
        members.push_back({"war3map.imp", Bytes(imports)});
        auto prefix = IO::ReadFileBytes(seed, 4 * 1024 * 1024);
        Require(prefix.Succeeded() && prefix.value->size() >= 512, "original map header");
        std::filesystem::create_directory(out);
        Require(IO::WriteFileBytes(out / "input.vjass", Bytes(script)).Succeeded()
            && IO::WriteFileBytes(out / "war3map.j", Bytes(compiled.text)).Succeeded(), "write original script evidence");
        const auto map = out / "fixture.w3x";
        Require(Mpq::WriteMpqArchive(map, members, std::span(*prefix.value).first(512)).Succeeded(), "write original test map");
        auto verify = Mpq::MpqArchive::OpenReadOnly(map);
        Require(verify.Succeeded(), "read back map directory");
        for (const auto& member : members) {
            if (control && member.name == "tiara.blp") {
                auto actual = verify.value->ReadFileBounded(member.name, 4 * 1024 * 1024);
                Require(actual.Succeeded() && actual.value->size() == 3577344, "control payload member size");
                auto actualIdentity = Sha256(*actual.value);
                Require(actualIdentity.Succeeded() && *actualIdentity.value == controlPayloadHash, "control payload member SHA mismatch");
                std::cout << "control_payload_import_name=tiara.blp\ncontrol_payload_bytes=3577344\n"
                    << "control_payload_sha256=" << *actualIdentity.value << '\n';
                continue;
            }
            if (member.name == loaderImport) {
                auto actual = verify.value->ReadFileBounded(member.name, 1024 * 1024);
                Require(actual.Succeeded() && actual.value->size() == loaderSize, "opaque loader member size");
                const auto actualIdentity = Sha256(*actual.value);
                Require(actualIdentity.Succeeded() && *actualIdentity.value == *loaderIdentity.value,
                    "opaque loader member SHA mismatch");
                std::cout << "loader_import_name=" << member.name << "\nloader_import_bytes=" << loaderSize
                    << "\nloader_import_sha256=" << *actualIdentity.value << '\n';
                continue;
            }
            auto actual = verify.value->ReadFileBounded(member.name, 16 * 1024 * 1024);
            Require(actual.Succeeded() && *actual.value == member.data, "own member exact roundtrip");
        }
        if (probe) {
            Require(verify.value->HasFile("warvk_runtime.blp") == rawProbe && !verify.value->HasFile("warvk_loader.ai")
                && !verify.value->HasFile("AudastraSmoke.toc"), "probe payload presence or loader/UI exclusion mismatch");
        }
        if (rawProbe) Require(!verify.value->HasFile("tiara.ai") && !verify.value->HasFile("tiara.blp")
            && compiled.text.find("AjInitialize") == std::string::npos
            && compiled.text.find("AjMapSmokeStart") == std::string::npos, "raw probe excludes control/JAPI/UI");
        if (control) Require(!verify.value->HasFile("warvk_probe.ai") && !verify.value->HasFile("warvk_probe_thread.ai"),
            "control must exclude our probes");
        identity = Sha256File(map);
        Require(identity.Succeeded(), "hash completed map");
        std::cout << "map_sha256=" << *identity.value << "\nvjass=true\nstock_jass_semantic=true\n"
            << "own_members_roundtrip=true\nopaque_loader=import-and-digest-only\n"
            << "fixture_mode=" << (control ? "control-tiara" : rawProbe ? "raw-probe" : rawRuntime ? "raw-runtime" : playerZeroProbe ? "probe-thread-p0" : threadProbe ? "probe-thread"
                : earlyProbe ? "probe-early" : probe ? "probe" : "runtime") << '\n';
        if (earlyStart) std::cout << "main_first_statement=StartCampaignAI\nnext_statement=SetCameraBounds\n"
            << "ai_call_count=1\ndelayed_ai_startup_absent=true\nprobe_player=" << probePlayer << '\n';
        if (earlyProbe) std::cout << "delayed_startup_absent=true\n";
        if (hasRuntime) std::cout << "runtime_import_name=warvk_runtime.blp\nruntime_import_sha256=" << runtimeHash << '\n';
        else std::cout << "runtime_import_absent=true\n";
        std::cout << "game_evidence=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
