#pragma once
#include "War3MapCore/HLua/ObjectMapping.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/MapCreateService.h"

namespace War3Map::HLua {
enum class BuildTarget { Test, Build, Dist };
struct BuildRequest {
    FrameworkLocation framework;
    std::filesystem::path project;
    std::filesystem::path baseMap; // 空路径使用工程内 map 三件套。
    std::filesystem::path target;
    std::filesystem::path workspace; // 必须不存在；失败证据由调用方保留。
    std::filesystem::path gameRoot;
    BuildTarget mode = BuildTarget::Test;
    bool idsOnly = false;
    HslkLimits limits;
    std::string expectedProjectSha256;
};
struct InspectReceipt {
    std::string frameworkSha256, projectSha256;
    std::size_t runtimeFiles = 0, hslkFiles = 0;
    std::vector<std::string> jassModules;
    std::string commonSha256;
    std::size_t commonNamesChecked=0;
    std::vector<std::string> missingCommonNames;
    std::vector<std::string> japiNames;
};
struct BuildReceipt {
    InspectReceipt inputs;
    std::string baseSha256, outputSha256;
    std::filesystem::path output;
    std::vector<HslkObject> objects;
    std::vector<std::string> differences;
    std::string console;
    bool idReplayPassed = false;
    bool archiveReadbackPassed = false;
    std::size_t peakLuaBytes = 0;
    std::uint64_t luaInstructions = 0;
    std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] Result<InspectReceipt> InspectProject(const FrameworkLocation& framework,
    const std::filesystem::path& project, const std::filesystem::path& gameRoot = {});
[[nodiscard]] Result<BuildReceipt> BuildProject(const BuildRequest& request,
    const Services::MapSaveService::Progress& progress = {});
[[nodiscard]] std::string InspectReceiptJson(const InspectReceipt& receipt,std::size_t offset=0,std::size_t limit=128);
[[nodiscard]] std::string BuildReceiptJson(const BuildReceipt& receipt, std::size_t offset = 0,
    std::size_t limit = 128);
struct NewProjectRequest {
    FrameworkLocation framework;
    std::filesystem::path directory;
    std::filesystem::path toolRoot, gameRoot, uiRoot;
};
[[nodiscard]] Result<Services::MapCreateReceipt> CreateProject(const NewProjectRequest& request);
} // namespace War3Map::HLua
