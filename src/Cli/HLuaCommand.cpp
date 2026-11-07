#include "HLuaCommand.h"
#include "MapTestCommand.h"
#include "War3MapCore/HLua/Build.h"
#include "War3MapCore/HLua/Test.h"
#include "War3MapCore/Services/MapCreateService.h"
#include "War3MapCore/IO/FileBytes.h"
#include <Windows.h>
#include <iostream>
#include <set>

namespace {
using namespace War3Map;
std::string Utf8(const wchar_t* value) { return IO::PathUtf8(value); }
int Failed(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& d : diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    return 1;
}
int Bad() {
    std::cerr << "HLUA_ARGUMENT: hlua-inspect|hlua-build|hlua-ids|hlua-test <project> --framework <root> "
        "[--runtime-framework <root> --build-framework <root>] [--base <map>] --game <root> --workspace <new-dir> "
        "[--output <map>] [--replace-sha256 <sha>] [--target test|build|dist] [--expected-project <sha>] [--offset N --limit N] "
        "[--helpers <dir> -- <map-test options>]\n"
        "hlua-new <new-directory> --framework <root> --tool <root> --game <root> [--ui-root <root>]\n";
    return 2;
}
}
int RunHLua(int argc, wchar_t** argv) {
    if (argc < 3) return Bad();
    const std::wstring command = argv[1];
    HLua::BuildRequest request; request.project = argv[2];
    std::filesystem::path tool, ui, helpers;
    std::size_t offset = 0, limit = 128;
    std::vector<std::wstring> testOptions;
    std::wstring engine=L"owned";
    std::set<std::wstring> seen;
    try {
        for (int i = 3; i < argc; ++i) {
            const std::wstring option = argv[i];
            if (option == L"--") { for (++i; i < argc; ++i) testOptions.emplace_back(argv[i]); break; }
            if (i + 1 == argc || !seen.insert(option).second) return Bad();
            const auto* value = argv[++i];
            if (option == L"--framework") request.framework.repository = value;
            else if (option == L"--runtime-framework") request.framework.runtime = value;
            else if (option == L"--build-framework") request.framework.build = value;
            else if (option == L"--base") request.baseMap = value;
            else if (option == L"--game") request.gameRoot = value;
            else if (option == L"--workspace") request.workspace = value;
            else if (option == L"--output") request.target = value;
            else if (option == L"--tool") tool = value;
            else if (option == L"--ui-root") ui = value;
            else if (option == L"--helpers") helpers = value;
            else if (option == L"--engine") { engine=value; if(engine!=L"owned" && engine!=L"ydwe") return Bad(); }
            else if (option == L"--expected-project") request.expectedProjectSha256 = Utf8(value);
            else if (option == L"--replace-sha256") request.expectedTargetSha256 = Utf8(value);
            else if (option == L"--offset" || option == L"--limit") {
                std::size_t end = 0; const auto number = std::stoull(value, &end);
                if (end != std::wstring_view(value).size() || number > 65536 || (option==L"--limit" && (!number || number>128))) return Bad();
                (option == L"--offset" ? offset : limit) = static_cast<std::size_t>(number);
            } else if (option == L"--target") {
                const std::wstring target = value;
                if (target != L"test" && target != L"build" && target != L"dist") return Bad();
                request.mode = target == L"test" ? HLua::BuildTarget::Test : target == L"build" ? HLua::BuildTarget::Build : HLua::BuildTarget::Dist;
            } else return Bad();
        }
        if (command == L"hlua-inspect") {
            auto result = HLua::InspectProject(request.framework, request.project, request.gameRoot);
            if (!result.Succeeded()) return Failed(result.diagnostics);
            std::cout << HLua::InspectReceiptJson(*result.value,offset,limit) << '\n'; return 0;
        }
        if (command == L"hlua-new") {
            auto created = HLua::CreateProject({request.framework, request.project, tool, request.gameRoot, ui});
            if (!created.Succeeded()) return Failed(created.diagnostics);
            std::cout << "created=true\nbase_sha256=" << created.value->sha256 << "\n"; return 0;
        }
        if (command == L"hlua-ids") request.idsOnly = true;
        else if (command != L"hlua-build" && command != L"hlua-test") return Bad();
        if (!request.idsOnly && request.workspace.empty()) return Bad();
        if (command == L"hlua-test" && (helpers.empty() || request.mode != HLua::BuildTarget::Test)) return Bad();
        auto result = HLua::BuildProject(request);
        if (!result.Succeeded()) return Failed(result.diagnostics);
        std::cout << HLua::BuildReceiptJson(*result.value, offset, limit) << '\n' << std::flush;
        if (command != L"hlua-test") return 0;
        bool broad=engine==L"ydwe";
        for(std::size_t i=0;i+1<testOptions.size();++i) if(testOptions[i]==L"--lua-engine" && testOptions[i+1]==L"ydwe") broad=true;
        const auto permitted=HLua::ValidateTestEngineSources(result.value->output,result.value->outputSha256,
            broad ? Services::MapTestLuaEngine::Ydwe : Services::MapTestLuaEngine::Owned);
        if(!permitted.Succeeded()) return Failed(permitted.diagnostics);
        for(const auto& diagnostic:permitted.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
        std::vector<std::wstring> args{argv[0], L"map-test", result.value->output.wstring(), request.gameRoot.wstring(), helpers.wstring()};
        args.insert(args.end(),{L"--lua-engine",engine});
        args.insert(args.end(), testOptions.begin(), testOptions.end());
        std::vector<wchar_t*> pointers; for (auto& arg : args) pointers.push_back(arg.data());
        return RunMapTest(static_cast<int>(pointers.size()), pointers.data());
    } catch (const std::exception& e) { std::cerr << "HLUA_ARGUMENT: " << e.what() << '\n'; return 2; }
}
