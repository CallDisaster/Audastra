#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Services/MapCreateService.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace War3Map;
using namespace War3Map::Services;

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const auto* value = _wgetenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

std::string DiagnosticText(const std::vector<Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) {
        if (!text.empty()) text += " | ";
        text += diagnostic.code;
    }
    return text;
}

void NewMapGetsAutomaticMinimap() {
    const auto toolRoot = EnvironmentPath(L"FORGE_YDWE_ROOT");
    const auto gameRoot = EnvironmentPath(L"FORGE_WORLD_GAME_ROOT");
    if (toolRoot.empty() || gameRoot.empty()) {
        std::cout << "SKIP create minimap: FORGE_YDWE_ROOT / FORGE_WORLD_GAME_ROOT unset\n";
        return;
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("audastra-map-create-minimap-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    try {
        MapCreateRequest request;
        request.options.name = "Automatic minimap fixture";
        request.options.initTriggerName = "Init";
        request.options.initCustomScript = {"call DoNothing()"};
        request.target = root / "created.w3x";
        request.workspace = root / "workspace";
        request.toolRoot = toolRoot;
        request.gameRoot = gameRoot;
        const auto created = CreateMapCandidate(request);
        Require(created.Succeeded() && created.value, DiagnosticText(created.diagnostics).c_str());

        const auto opened = MapArchiveSession::Open(created.value->path);
        Require(opened.Succeeded() && opened.value, "新建地图可以重新打开");
        Require(opened.value->HasMember("war3mapMap.blp"), "新建地图首存生成war3mapMap.blp");
        Require(opened.value->HasMember("war3map.j"), "最终脚本成员保留在新建地图");
        const auto script = opened.value->ReadMember("war3map.j");
        Require(script.Succeeded() && script.value, "最终脚本成员可读取");
        const std::string scriptText(reinterpret_cast<const char*>(script.value->data()), script.value->size());
        Require(scriptText.find("DoNothing") != std::string::npos, "最终脚本替换没有被小地图派生阶段覆盖");
        const auto minimap = opened.value->ReadMember("war3mapMap.blp");
        Require(minimap.Succeeded() && minimap.value && minimap.value->size() >= 160,
            "新建地图小地图成员可完整读取");
        const auto* magic = reinterpret_cast<const char*>(minimap.value->data());
        Require(std::string(magic, magic + 4) == "BLP1", "新建地图小地图是BLP1编码");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
} // namespace

int main() {
    if (EnvironmentPath(L"FORGE_YDWE_ROOT").empty() || EnvironmentPath(L"FORGE_WORLD_GAME_ROOT").empty()) {
        std::cout << "SKIP create minimap: FORGE_YDWE_ROOT / FORGE_WORLD_GAME_ROOT unset\n";
        return 77;
    }
    try {
        NewMapGetsAutomaticMinimap();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
