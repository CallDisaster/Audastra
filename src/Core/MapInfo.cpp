#include "War3MapCore/MapInfo.h"

#include <cstring>

namespace War3Map {
namespace {

std::int32_t ReadI32(const std::byte* p) {
    std::int32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

void WriteI32(std::vector<std::byte>& out, std::int32_t v) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    std::memcpy(out.data() + at, &v, 4);
}

// 读取 null 终止字符串，返回消耗的字节数（含 \0）。
std::size_t ReadString(const std::byte* data, std::size_t size, std::size_t pos,
                       std::string& out) {
    out.clear();
    while (pos < size) {
        const auto ch = static_cast<char>(static_cast<unsigned char>(data[pos]));
        ++pos;
        if (ch == '\0') return pos;
        out.push_back(ch);
    }
    return size + 1;
}

void WriteString(std::vector<std::byte>& out, const std::string& s) {
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    out.push_back(std::byte{0});
}

} // namespace

Result<MapInfo> MapInfo::Parse(std::span<const std::byte> data) {
    // 最小长度：3 个 int32 + 4 个空字符串（各 1 字节 \0）= 16。
    if (data.size() < 16) {
        return Result<MapInfo>::Failure(
            "MapInfo", "W3I_TOO_SHORT",
            "war3map.w3i 数据过短，无法解析头部。");
    }

    MapInfo info;
    info.formatVersion = ReadI32(data.data() + 0);
    info.saves = ReadI32(data.data() + 4);
    info.editorVersion = ReadI32(data.data() + 8);

    std::size_t pos = 12;
    pos = ReadString(data.data(), data.size(), pos, info.mapName);
    pos = ReadString(data.data(), data.size(), pos, info.author);
    pos = ReadString(data.data(), data.size(), pos, info.description);
    pos = ReadString(data.data(), data.size(), pos, info.recommendedPlayers);

    if (pos > data.size()) {
        return Result<MapInfo>::Failure(
            "MapInfo", "W3I_TRUNCATED",
            "war3map.w3i 头部字符串超出数据范围。");
    }

    // 剩余数据原样保留。
    info.trailing.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
    return Result<MapInfo>::Success(std::move(info));
}

std::vector<std::byte> MapInfo::Serialize() const {
    std::vector<std::byte> out;
    out.reserve(12 + mapName.size() + author.size() +
                description.size() + recommendedPlayers.size() + 4 +
                trailing.size());
    WriteI32(out, formatVersion);
    WriteI32(out, saves);
    WriteI32(out, editorVersion);
    WriteString(out, mapName);
    WriteString(out, author);
    WriteString(out, description);
    WriteString(out, recommendedPlayers);
    out.insert(out.end(), trailing.begin(), trailing.end());
    return out;
}

} // namespace War3Map
