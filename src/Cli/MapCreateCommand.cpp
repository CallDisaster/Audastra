#include "MapCreateCommand.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Services/MapCreateService.h"
#include <filesystem>
#include <iostream>

int RunMapCreate(int argc, wchar_t** argv) {
    if (argc < 8) return 2;
    const auto utf8 = [](const wchar_t* value) {
        const auto text = std::filesystem::path(value).u8string();
        return std::string(text.begin(), text.end());
    };
    War3Map::Services::MapCreateRequest request;
    request.options.profile = utf8(argv[2]); request.target = argv[3]; request.workspace = argv[4];
    request.toolRoot = argv[5]; request.gameRoot = argv[6]; request.options.name = utf8(argv[7]);
    int index = 8;
    if (index < argc && std::wstring_view(argv[index]).substr(0, 2) != L"--") request.options.author = utf8(argv[index++]);
    // 可选：初始化触发器的自定义代码行（例如 exec-lua 入口）与随图写入的文件。
    for (; index < argc; ++index) {
        const std::wstring_view option = argv[index];
        if (option == L"--init-trigger" && index + 1 < argc) request.options.initTriggerName = utf8(argv[++index]);
        else if (option == L"--init-code" && index + 1 < argc) request.options.initCustomScript.push_back(utf8(argv[++index]));
        else if (option == L"--add-file" && index + 2 < argc) {
            const auto member = utf8(argv[index + 1]);
            auto bytes = War3Map::IO::ReadFileBytes(argv[index + 2], 16u * 1024u * 1024u);
            if (!bytes.Succeeded()) {
                for (const auto& d : bytes.diagnostics) std::cerr << d.subsystem << '\t' << d.code << '\t' << d.message << '\n';
                return 1;
            }
            request.options.extraMembers.push_back({member, std::move(*bytes.value)});
            index += 2;
        } else return 2;
    }
    const auto result = War3Map::Services::CreateMapCandidate(request);
    for (const auto& d : result.diagnostics) std::cerr << d.subsystem << '\t' << d.code << '\t' << d.message << '\n';
    if (!result.Succeeded() || !result.value) return 1;
    std::cout << "created=true\nprofile=" << result.value->profile << "\nsha256=" << result.value->sha256
        << "\nmembers=" << result.value->memberCount << "\nscript_rebuilt=true\noriginal_editor_verified=false\nruntime_verified=false\n";
    return 0;
}
