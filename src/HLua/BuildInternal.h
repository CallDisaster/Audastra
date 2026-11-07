#pragma once
#include "War3MapCore/HLua/Build.h"
#include <map>

namespace War3Map::HLua::Detail {
struct TreeFile { std::filesystem::path source; std::string name, sha256; std::vector<std::byte> bytes; };
struct BaseSnapshot {
    MapArchiveSession archive;
    std::vector<HslkObject> f6Objects;
    std::vector<TreeFile> files;
    std::string inputSha256;
};
[[nodiscard]] Result<std::vector<TreeFile>> CaptureTree(const std::filesystem::path& root);
[[nodiscard]] Status VerifyTree(std::span<const TreeFile> files);
[[nodiscard]] Result<BaseSnapshot> CaptureBase(const BuildRequest& request);
[[nodiscard]] Result<std::vector<std::string>> CaptureExistingIds(const BuildRequest& request);
[[nodiscard]] Result<std::vector<MapArchiveMember>> RuntimeMembers(const FrameworkSnapshot& framework,
    const ProjectSnapshot& project, const HslkOutput& hslk, BuildTarget target);
[[nodiscard]] Status CheckScript(const MapArchiveSession& archive, std::string_view script,
    const Game::GameDataSource& game);
} // namespace War3Map::HLua::Detail
