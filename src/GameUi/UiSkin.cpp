#include "War3MapCore/GameUi/UiSkin.h"

#include "War3MapCore/Image/TextureImage.h"

#include <charconv>
#include <cstdio>
#include <string>

namespace War3Map::GameUi {
namespace {

std::string_view StripBom(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    return text;
}

// 预览读取的队伍颜色种数上限（游戏为 16）。
constexpr int kMaxTeamColors = 32;

std::optional<SkinLookup> LookupInSection(const Text::ProfileSections& sections, const std::string& section,
    const std::string& key, bool expansion, std::string_view sourcePrefix) {
    const auto found = sections.find(section);
    if (found == sections.end()) return std::nullopt;
    if (expansion) {
        const std::string versioned = key + "_v1";
        if (const auto entry = found->second.find(versioned); entry != found->second.end()) {
            return SkinLookup{entry->second, std::string(sourcePrefix) + section, versioned};
        }
    }
    if (const auto entry = found->second.find(key); entry != found->second.end()) {
        return SkinLookup{entry->second, std::string(sourcePrefix) + section, key};
    }
    return std::nullopt;
}

} // namespace

void UiSkin::SetGameSkins(std::string_view war3skinsText) {
    game_ = Text::ParseProfile(StripBom(war3skinsText));
}

void UiSkin::SetMapSkin(std::string_view war3mapSkinText) {
    map_ = Text::ParseProfile(StripBom(war3mapSkinText));
}

std::vector<std::string> UiSkin::Races() const {
    if (const auto main = game_.find("main"); main != game_.end()) {
        if (const auto skins = main->second.find("skins"); skins != main->second.end()) {
            std::vector<std::string> races;
            for (const auto item : Text::SplitPlain(skins->second, ',')) {
                const auto trimmed = Text::TrimHorizontal(item);
                if (!trimmed.empty()) races.emplace_back(trimmed);
            }
            if (!races.empty()) return races;
        }
    }
    return {"Human", "Orc", "NightElf", "Undead"};
}

std::optional<SkinLookup> UiSkin::Lookup(std::string_view key, std::string_view race, bool expansion) const {
    if (key.empty()) return std::nullopt;
    const std::string lowerKey = Text::LowerAscii(key);
    if (auto found = LookupInSection(map_, "customskin", lowerKey, expansion, "map:")) return found;
    if (!race.empty()) {
        if (auto found = LookupInSection(game_, Text::LowerAscii(race), lowerKey, expansion, "game:")) return found;
    }
    return LookupInSection(game_, "default", lowerKey, expansion, "game:");
}

void UiSkin::SetMiscUi(std::string_view miscUiText) {
    misc_ = Text::ParseProfile(StripBom(miscUiText));
}

std::optional<float> UiSkin::FontHeight(std::string_view key) const {
    const auto section = misc_.find("fontheights");
    if (section == misc_.end()) return std::nullopt;
    const auto entry = section->second.find(Text::LowerAscii(key));
    if (entry == section->second.end()) return std::nullopt;
    // 值后面常跟 “// 说明”，只取开头的数值。
    const std::string& text = entry->second;
    float value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end == text.data() || !(value > 0.0f) || value > 0.1f) return std::nullopt;
    return value;
}

std::optional<std::string> UiSkin::MapFrameString(std::string_view key) const {
    if (const auto section = map_.find("framedef"); section != map_.end()) {
        if (const auto entry = section->second.find(Text::LowerAscii(key)); entry != section->second.end()) {
            return entry->second;
        }
    }
    return std::nullopt;
}

void UiSkin::SetTeamColors(std::vector<TeamColor> colors) {
    teamColors_ = std::move(colors);
}

const TeamColor* UiSkin::PlayerTeamColor(std::uint32_t player) const {
    if (teamColors_.empty()) return nullptr;
    const std::size_t count = teamColors_.size();
    return &teamColors_[player >= 16 ? count - 1 : player % count];
}

std::vector<TeamColor> ReadTeamColors(const UiSkin& skin, const SkinFileReader& read, std::vector<Diagnostic>& diagnostics) {
    const auto warn = [&](std::string message) {
        diagnostics.push_back(Diagnostic{DiagnosticSeverity::Warning, "GameUi", "GAMEUI_TEAM_COLOR", std::move(message), std::nullopt});
    };
    const auto count = skin.Lookup("TeamColors", {});
    const auto prefix = skin.Lookup("TeamColor", {});
    if (!count || !prefix) {
        warn("界面皮肤里没有 TeamColors 或 TeamColor，排行榜的玩家颜色按白色显示");
        return {};
    }
    int total = 0;
    const std::string& text = count->value;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), total);
    if (error != std::errc() || total <= 0) {
        warn("界面皮肤的 TeamColors 不是正整数：" + text);
        return {};
    }
    if (total > kMaxTeamColors) {
        warn("界面皮肤的 TeamColors 为 " + text + "，预览只读前 " + std::to_string(kMaxTeamColors) + " 种");
        total = kMaxTeamColors;
    }
    std::vector<TeamColor> colors(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        char number[8];
        std::snprintf(number, sizeof(number), "%02d", i);
        const std::string base = prefix->value + number;
        std::optional<std::vector<std::byte>> bytes;
        for (const char* extension : {".tga", ".blp"}) {
            if ((bytes = read(base + extension))) {
                colors[i].path = base + extension;
                break;
            }
        }
        if (!bytes) {
            warn("找不到队伍颜色贴图 " + base + "（.tga / .blp）");
            continue;
        }
        auto image = Image::DecodeTexture(colors[i].path, *bytes);
        if (!image.Succeeded() || !image.value || !image.value->Valid()) {
            warn("队伍颜色贴图 " + colors[i].path + " 无法解码");
            continue;
        }
        for (std::size_t k = 0; k < 4; ++k) colors[i].rgba[k] = static_cast<std::uint8_t>(image.value->rgba[k]);
    }
    return colors;
}

} // namespace War3Map::GameUi
