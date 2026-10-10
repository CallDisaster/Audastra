#include "War3MapCore/MapData/LniMapSettings.h"
#include "LniMapSettingsFixtures.h"

#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
using namespace LniMapSettingsFixtures;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
auto Parse(std::string_view text) { return ParseLniMapSettings(Bytes(text)); }
void TestTypedSettings() {
    const auto text = Complete();
    const auto result = Parse(text);
    if (!result.Succeeded()) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.message << '\n';
    }
    Require(result.Succeeded() && result.value->fullyParsed, "complete LNI must parse");
    const auto& doc = *result.value;
    const auto& s = doc.settings;
    Require(doc.originalBytes == Bytes(text) && s.OriginalBytes().empty(), "LNI bytes preserved without native byte provenance");
    Require(s.formatVersion == 25 && s.saves == 7 && s.editorVersion == 6052, "header values");
    Require(s.mapName == "LNI fixture" && s.description == "Line one\nLine two", "text and long strings");
    Require(s.loadingScreen.path == "Loading\\screen.mdx" && s.loadingScreen.text == "Loading\ntext", "escaped strings");
    Require(s.playableWidth == 32 && s.playableHeight == 64 && s.cameraMargins[3] == 4, "terrain and camera");
    Require(s.flags == 0x3fffffU && s.forces[0].flags == 59, "map bits and sparse team bits");
    Require(s.forces[0].playerMask == 8, "declared force mask only, no implicit players");
    Require(s.players[0].number == 3 && s.players[0].flags == 3 && s.players[0].allyLowPriorityMask == 0x80000001U, "raw player flags and one based masks");
    Require(s.environment.fogColor.blue == 11 && s.environment.fogColor.red == 33 && s.environment.fogColor.alpha == 44, "BGRA order");
    Require(s.environment.weather == SettingsRawcode{}, "zero rawcode escapes");
    Require(s.custom && s.custom->upgrades.size() == 1 && s.custom->upgrades[0].level == 2 && s.custom->techRestrictions.size() == 1, "custom technology data");
    Require(s.custom->randomUnits[0].number == 0 && s.custom->randomUnits[0].rows.size() == 2 && s.custom->randomUnits[0].rows[1].ids.size() == 2, "random unit matrix");
    Require(s.custom->randomItems[0].number == 0 && s.custom->randomItems[0].sets.size() == 2 && s.custom->randomItems[0].sets[1][1].chance == 25, "nested numbered item sets");
}
void TestLocalizedAndUnknown() {
    auto text = Replace(Complete(), "[map]", "[地图]");
    text = Replace(text, "file_version", "文件版本");
    text = Replace(text, "[player1]", "[玩家1]");
    text = Replace(text, "player = 3", "玩家 = 3");
    text = "\xef\xbb\xbf; header comment\n" + text;
    Require(Parse(text).Succeeded(), "Chinese and English aliases with UTF8 BOM");
    text += "[extension]\nopaque = {value = \"kept\"}\n";
    auto result = Parse(text);
    Require(result.Succeeded() && !result.value->fullyParsed && result.value->unknownFields.size() == 1, "unknown section retained partial");
    Require(result.value->originalBytes == Bytes(text), "unknown original preserved");
    text = Replace(Complete(), "chance = 100, ID = \"i001\"", "chance = 100, ID = \"i001\", extension = 42");
    result = Parse(text);
    Require(result.Succeeded() && !result.value->fullyParsed && result.value->unknownFields[0].find("extension") != std::string::npos, "nested unknown retained partial");
}
void TestInvalidData() {
    const auto base = Complete();
    const std::pair<std::string_view, std::string_view> invalid[] = {
        {"file_version = 25", "file_version = 31"}, {"map_width = 32", "map_width = -1"},
        {"file_version = 25", ""}, {"map_width = 32", "map_width = 32.5"},
        {"density = 0.2", "density = 1e9999"}, {"ally_low_flag = {1, 32}", "ally_low_flag = {1, 33}"},
        {"ally_low_flag = {1, 32}", "ally_low_flag = {1, 1}"},
        {"color = {11, 22, 33, 44}", "color = {11, 22, 33, 256}"},
        {"ID = \"R001\"", "ID = \"bad\""}, {"player_count = 1", "player_count = 2"},
        {"[random_item1]", "[random_item2]"}, {"1 = {{chance", "3 = {{chance"},
        {"position_type = {0, 2}", "position_type = {0}"},
        {"file_version = 25", "file_version = 25\n文件版本 = 25"},
        {"[camera]", "[map]"}, {"map_name = \"LNI fixture\"", "map_name = os.execute(\"touch\")"},
        {"map_name = \"LNI fixture\"", "map_name = \"text\" .. \"suffix\""},
        {"map_name = \"LNI fixture\"", "map_name = \"\\999\""},
        {"map_name = \"LNI fixture\"", "map_name = \"\\q\""},
        {"[player1]", "[player01]"}
    };
    for (const auto& [before, after] : invalid) {
        const auto result = Parse(Replace(base, before, after));
        if (result.Succeeded()) std::cerr << "Unexpected acceptance: " << after << '\n';
        Require(!result.Succeeded() && !result.value, "invalid input has no fabricated settings");
        Require(!result.diagnostics.empty() && result.diagnostics.front().origin.has_value(), "failure points to source offset");
    }
    for (const auto end : {std::size_t(0), std::size_t(5), base.find("Line two"), base.find("camera_bound") + 30, base.size() - 3})
        Require(!Parse(std::string_view(base).substr(0, end)).Succeeded(), "truncation rejected");
    auto deep = base + "[extension]\nvalue = " + std::string(34, '{') + "0" + std::string(34, '}') + "\n";
    Require(!Parse(deep).Succeeded(), "recursive budget");
    auto nullText = base; nullText.push_back('\0');
    Require(!Parse(nullText).Succeeded(), "raw NUL rejected");
    Require(!Parse(std::string(16 * 1024 * 1024 + 1, ' ')).Succeeded(), "input byte budget");
}
}
int main() {
    try { TestTypedSettings(); TestLocalizedAndUnknown(); TestInvalidData(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "LNI settings typed values, preservation, provenance and malformed-input checks passed\n";
}
