#include "MapCreateCommand.h"
#include "War3MapCore/Services/MapCreateService.h"
#include <filesystem>
#include <iostream>

int RunMapCreate(int argc, wchar_t** argv) {
    if (argc != 8 && argc != 9) return 2;
    const auto utf8 = [](const wchar_t* value) {
        const auto text = std::filesystem::path(value).u8string();
        return std::string(text.begin(), text.end());
    };
    War3Map::Services::MapCreateRequest request;
    request.options.profile = utf8(argv[2]); request.target = argv[3]; request.workspace = argv[4];
    request.toolRoot = argv[5]; request.gameRoot = argv[6]; request.options.name = utf8(argv[7]);
    if (argc == 9) request.options.author = utf8(argv[8]);
    const auto result = War3Map::Services::CreateMapCandidate(request);
    for (const auto& d : result.diagnostics) std::cerr << d.subsystem << '\t' << d.code << '\t' << d.message << '\n';
    if (!result.Succeeded() || !result.value) return 1;
    std::cout << "created=true\nprofile=" << result.value->profile << "\nsha256=" << result.value->sha256
        << "\nmembers=" << result.value->memberCount << "\nscript_rebuilt=true\noriginal_editor_verified=false\nruntime_verified=false\n";
    return 0;
}
