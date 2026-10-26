#include "MapRuntimeCommand.h"
#include "War3MapCore/ApplicationPaths.h"
#include "War3MapCore/Services/MapRuntimePreparation.h"
#include <iostream>

int RunMapRuntimeQuery(int argc, wchar_t** argv) {
    using namespace War3Map;
    if (argc != 4) { std::cerr << "usage: map-runtime-query <map> <source-sha256>\n"; return 2; }
    const auto sources = Services::LoadInstalledMapRuntimeSources(CurrentInstallationRoot());
    const auto diagnostics = [](const auto& result) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    };
    if (!sources.Succeeded()) { diagnostics(sources); return 1; }
    const auto sha = std::filesystem::path(argv[3]).u8string();
    const auto state = Services::QueryMapRuntimeArchiveState(argv[2], std::string(sha.begin(), sha.end()), *sources.value);
    if (!state.Succeeded()) { diagnostics(state); return 1; }
    std::cout << Services::SerializeMapRuntimeArchiveState(*state.value) << '\n';
    return 0;
}
