#include "MapCustomSettingsCommand.h"
#include "War3MapCore/Services/MapCustomSettingsQuery.h"
#include "War3MapCore/IO/FileBytes.h"
#include <iostream>

int RunMapCustomSettingsQuery(int argc, wchar_t** argv) {
    using namespace War3Map;
    if (argc != 4 && argc != 5) {
        std::cerr << "map-settings-custom-query <map> <source-sha256> [query-json-file]\n"; return 2;
    }
    const auto path = std::filesystem::path(argv[2]);
    const auto sha = IO::PathUtf8(std::filesystem::path(argv[3]));
    std::string query = R"({"section":"summary"})";
    if (argc == 5) {
        const auto bytes = IO::ReadFileBytes(argv[4], 1024 * 1024);
        if (!bytes.Succeeded()) { for (const auto& d : bytes.diagnostics) std::cerr << d.code << ": " << d.message << '\n'; return 1; }
        query.assign(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size());
    }
    const auto result = Services::QueryArchiveMapCustomSettings(path, sha, query);
    if (!result.Succeeded()) { for (const auto& d : result.diagnostics) std::cerr << d.code << ": " << d.message << '\n'; return 1; }
    std::cout << *result.value << '\n'; return 0;
}
