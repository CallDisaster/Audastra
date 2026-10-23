#include "War3MapCore/GameUi/UiSkin.h"

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

std::optional<std::string> UiSkin::MapFrameString(std::string_view key) const {
    if (const auto section = map_.find("framedef"); section != map_.end()) {
        if (const auto entry = section->second.find(Text::LowerAscii(key)); entry != section->second.end()) {
            return entry->second;
        }
    }
    return std::nullopt;
}

} // namespace War3Map::GameUi
