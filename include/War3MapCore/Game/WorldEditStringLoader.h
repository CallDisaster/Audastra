#pragma once

#include "War3MapCore/Game/WorldEditStrings.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 字符串表与 YDWE 覆盖层的载入。
//
// YDWE 把 <YDWE 根>/share/<语言>/mpq 当作叠加在游戏 MPQ 之上的虚拟层（同名文件整文件替代，
// 语言不是 zh-CN 时再回退到 zh-CN）。字符串表在此基础上逐键合并：
//   1. share/<语言>/mpq（及 zh-CN）   2. 游戏 MPQ   3. share/en-US/mpq（英文原版，补缺）
// 先载入者优先；三处都没有的键保持原样，由界面标为未解析。
namespace War3Map::Game {

class GameDataSource;

// toolRoot 可以是 YDWE 根目录，也可以是其 ui 子目录。按优先级返回存在的 share/<语言>/mpq 目录。
[[nodiscard]] std::vector<std::filesystem::path> YdweOverlayRoots(
    const std::filesystem::path& toolRoot, std::string_view language = "zh-CN");
// YDWE 根目录本身（toolRoot 为 ui 目录时依次为 ui 目录与其上一级）；函数库图标常写成相对这里的路径。
[[nodiscard]] std::vector<std::filesystem::path> YdweRootDirectories(const std::filesystem::path& toolRoot);
// 英文原版覆盖层（share/en-US/mpq），不存在时为空。
[[nodiscard]] std::vector<std::filesystem::path> YdweEnglishRoots(const std::filesystem::path& toolRoot);

// 从覆盖层目录读取游戏内路径（'\\' 与 '/' 等价，重复分隔符视为一个，逐段不区分大小写，
// 拒绝 .. 与绝对路径，经链接指到根外的文件也拒绝）。
[[nodiscard]] std::optional<std::vector<std::byte>> ReadOverlayFile(
    std::span<const std::filesystem::path> roots, std::string_view gamePath,
    std::size_t maximumBytes = 32 * 1024 * 1024);

// 游戏文本解码为 UTF-8：UTF-8（可带 BOM）原样返回，否则按 GB18030 解码（旧版中文数据）。
[[nodiscard]] std::string DecodeGameText(std::span<const std::byte> bytes);

struct WorldEditStringSources {
    std::filesystem::path toolRoot; // YDWE 根目录（或其 ui 目录）；为空时只用游戏数据
    std::string language = "zh-CN";
    const GameDataSource* game = nullptr;
    std::vector<std::string> files{"UI\\WorldEditStrings.txt", "UI\\WorldEditGameStrings.txt"};
};

[[nodiscard]] WorldEditStrings LoadWorldEditStrings(const WorldEditStringSources& sources);

} // namespace War3Map::Game
