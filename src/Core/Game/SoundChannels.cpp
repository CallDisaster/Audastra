#include "War3MapCore/Game/SoundChannels.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>

namespace War3Map::Game {

std::vector<SoundChannel> ParseSoundChannels(std::string_view worldEditData, const WorldEditStrings* strings) {
    if (worldEditData.starts_with("\xEF\xBB\xBF")) worldEditData.remove_prefix(3);
    std::vector<SoundChannel> channels;
    const auto sections = Text::ParseProfile(worldEditData);
    const auto section = sections.find("soundchannels");
    if (section == sections.end()) return channels;
    for (const auto& [key, value] : section->second) {
        SoundChannel channel;
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), channel.id);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || channel.id < 0) continue;
        const auto fields = Text::SplitPlain(value, ',');
        if (fields.size() != 2) continue;
        const auto flag = Text::TrimCWhitespace(fields[0]);
        if (flag != "0" && flag != "1") continue;
        channel.supports3d = flag == "1";
        channel.nameKey = std::string(Text::TrimCWhitespace(fields[1]));
        if (channel.nameKey.empty()) continue;
        channel.name = strings ? strings->Resolve(channel.nameKey) : channel.nameKey;
        channels.push_back(std::move(channel));
    }
    std::sort(channels.begin(), channels.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    // 同一编号写了两次（不同前导零）时不猜哪一行有效，整组去掉。
    std::vector<SoundChannel> unique;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const bool duplicated = (i > 0 && channels[i - 1].id == channels[i].id)
            || (i + 1 < channels.size() && channels[i + 1].id == channels[i].id);
        if (!duplicated) unique.push_back(std::move(channels[i]));
    }
    return unique;
}

} // namespace War3Map::Game
