#include "War3MapCore/Game/SoundChannels.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <map>

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

std::vector<SoundEaxSetting> ParseSoundEaxSettings(std::string_view eaxDefs, const WorldEditStrings* strings) {
    if (eaxDefs.starts_with("\xEF\xBB\xBF")) eaxDefs.remove_prefix(3);
    std::vector<SoundEaxSetting> settings;
    const auto cells = Text::ParseSlkCells(eaxDefs, Text::SlkCellMode::CatalogStrict);
    if (!cells.Succeeded() || !cells.value) return settings;
    const auto headers = cells.value->find(1);
    if (headers == cells.value->end()) return settings;
    int labelColumn = 0, textColumn = 0;
    for (const auto& [column, value] : headers->second) {
        if (value == "EAXLabel") labelColumn = column;
        else if (value == "DisplayText") textColumn = column;
    }
    if (labelColumn == 0 || textColumn == 0) return settings;
    std::map<std::string, int> counts;
    for (const auto& [rowIndex, row] : *cells.value) {
        if (rowIndex == 1) continue;
        const auto label = row.find(labelColumn);
        const auto text = row.find(textColumn);
        if (label == row.end() || text == row.end()) continue;
        SoundEaxSetting setting;
        setting.label = std::string(Text::TrimCWhitespace(label->second));
        setting.nameKey = std::string(Text::TrimCWhitespace(text->second));
        if (setting.label.empty() || setting.nameKey.empty()) continue;
        setting.name = strings ? strings->Resolve(setting.nameKey) : setting.nameKey;
        ++counts[setting.label];
        settings.push_back(std::move(setting));
    }
    std::erase_if(settings, [&](const auto& setting) { return counts[setting.label] > 1; });
    return settings;
}

} // namespace War3Map::Game
