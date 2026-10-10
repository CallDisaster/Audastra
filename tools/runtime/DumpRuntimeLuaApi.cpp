#include "War3MapCore/Runtime/RuntimeLuaApiInventory.h"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using Json = nlohmann::ordered_json;
std::filesystem::path EnvironmentRoot() {
    const auto count = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", nullptr, 0);
    if (!count || count > 32768) return {};
    std::wstring value(count, L'\0');
    const auto read = GetEnvironmentVariableW(L"FORGE_YDWE_ROOT", value.data(), count);
    if (!read || read >= count) return {};
    value.resize(read); return value;
}
void WriteNew(const std::filesystem::path& path, const std::string& text) {
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("清单输出路径已存在或不可写");
    DWORD written = 0;
    const auto ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
        && written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) throw std::runtime_error("清单写入失败，保留本次部分证据");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        std::map<std::wstring, std::filesystem::path> options;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::runtime_error("参数需要明确值");
            const std::wstring name = argv[i];
            if ((name != L"--root" && name != L"--common" && name != L"--out") ||
                !options.emplace(name, argv[i + 1]).second) throw std::runtime_error("未知或重复参数");
        }
        const auto root = options.contains(L"--root") ? options.at(L"--root") : EnvironmentRoot();
        if (root.empty()) { std::cerr << "SKIP: set FORGE_YDWE_ROOT or --root\n"; return 77; }
        const auto inventory = War3Map::Runtime::CaptureRuntimeLuaApiInventory(root, options[L"--common"], {},
            std::chrono::steady_clock::now() + std::chrono::seconds(30));
        Json report{{"schema", "audastra.lua-runtime-api.static.v1"}, {"method", inventory->method},
            {"trusted", inventory->trusted}, {"binding", inventory->binding}, {"reason", inventory->reason},
            {"modulesComplete", inventory->modulesComplete}, {"initializeReachabilityVerified", false},
            {"dllLoaded", false}, {"luaExecuted", false},
            {"gameStarted", false}, {"savePermission", false}, {"sources", Json::array()}, {"modules", Json::array()}};
        for (const auto& source : inventory->sources)
            report["sources"].push_back({{"relativePath", source.relativePath}, {"sha256", source.sha256}, {"size", source.size}});
        for (const auto& module : inventory->modules) {
            Json row{{"name", module.name}, {"registrationRva", module.registrationRva},
                {"openFunctionRva", module.openFunctionRva}, {"fieldsComplete", module.fieldsComplete},
                {"commonLookup", module.commonLookup}, {"availability", "registration-code-present"}, {"fields", Json::array()}};
            for (const auto& field : module.fields) {
                Json entry{{"name", field.name}, {"native", field.native}, {"registrationRva", field.registrationRva},
                    {"functionRva", field.functionRva}, {"argumentCount", nullptr},
                    {"source", field.native ? "bound-common.j-native-declaration-and-static-bridge" : "static-luaL_Reg-or-C-registration"}};
                if (field.argumentCount) entry["argumentCount"] = *field.argumentCount;
                row["fields"].push_back(std::move(entry));
            }
            report["modules"].push_back(std::move(row));
        }
        report["commonGlobals"] = inventory->commonGlobals;
        report["registrationCodeEvidence"] = Json::array();
        for (const auto& proof : inventory->evidence)
            report["registrationCodeEvidence"].push_back({{"kind", proof.kind}, {"rva", proof.rva},
                {"size", proof.size}, {"sha256", proof.sha256}});
        const auto text = report.dump(2) + '\n';
        if (options.contains(L"--out")) WriteNew(options.at(L"--out"), text);
        else std::cout << text;
        std::cerr << "trusted=" << inventory->trusted << " modules=" << inventory->modules.size()
            << " binding=" << inventory->binding << '\n';
        return inventory->trusted ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
