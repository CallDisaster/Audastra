#pragma once

#include <filesystem>
#include <string_view>

// 只读归档适配器。区块命令必须带上先前快照的两个 SHA，避免读取到变化后的地图。
int RunTerrainSnapshot(const std::filesystem::path& archivePath);
int RunTerrainBlock(const std::filesystem::path& archivePath,
    std::string_view expectedSourceSha256, std::string_view expectedTerrainSha256,
    std::string_view x, std::string_view y, std::string_view width, std::string_view height);
