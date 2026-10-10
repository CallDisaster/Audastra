#pragma once

#include <filesystem>

// 只读验证经典公共地图成员的类型化写出，不生成候选、不修改源地图。
[[nodiscard]] int RunMapDataRoundtrip(const std::filesystem::path& archivePath);
