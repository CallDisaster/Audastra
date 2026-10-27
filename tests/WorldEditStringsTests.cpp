#include "War3MapCore/Game/SoundChannels.h"
#include "War3MapCore/Game/WorldEditStringLoader.h"
#include "War3MapCore/Game/WorldEditStrings.h"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// 字符串表：逐键合并、链式解析、YDWE 覆盖层目录发现与英文回退。样例均为原创合成文本。
namespace {

using War3Map::Game::WorldEditStrings;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Forge-strings-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void TestParsingAndPriority() {
    WorldEditStrings strings;
    const auto added = strings.AddText("first",
        "\xEF\xBB\xBF[WorldEditStrings]\r\n"
        "// comment\r\n"
        "WESTRING_UNIT=单位\r\n"
        "WESTRING_QUOTED=\"--\"\r\n"
        "  westring_spaced  =  两侧空白  \r\n"
        "WESTRING_ALIAS=WESTRING_UNIT\r\n"
        "WESTRING_LOOP_A=WESTRING_LOOP_B\r\n"
        "WESTRING_LOOP_B=WESTRING_LOOP_A\r\n"
        "WESTRING_DANGLING=WESTRING_NOWHERE\r\n"
        "NOT_A_ASSIGNMENT\r\n");
    Require(added == 7, "seven entries parsed");
    strings.AddText("second", "[WorldEditStrings]\nWESTRING_UNIT=Unit\nWESTRING_EXTRA=补充\n");
    Require(strings.Resolve("WESTRING_UNIT") == "单位", "earlier source wins");
    Require(strings.Resolve("westring_unit") == "单位", "keys are case-insensitive");
    Require(strings.Resolve("WESTRING_EXTRA") == "补充", "later source fills gaps");
    Require(strings.Resolve("WESTRING_QUOTED") == "--", "surrounding quotes removed");
    Require(strings.Resolve("WESTRING_SPACED") == "两侧空白", "whitespace trimmed");
    Require(strings.Resolve("WESTRING_ALIAS") == "单位", "chained key resolved");
    Require(strings.Resolve("WESTRING_LOOP_A") == "WESTRING_LOOP_A", "cycle stops with the original key");
    Require(strings.Resolve("WESTRING_DANGLING") == "WESTRING_DANGLING", "dangling chain keeps original key");
    Require(!strings.CanResolve("WESTRING_MISSING") && strings.CanResolve("WESTRING_UNIT"), "resolvability reported");
    Require(strings.Resolve("普通文本") == "普通文本", "non-key text unchanged");
    Require(!WorldEditStrings::IsKey("WESTRING_") && !WorldEditStrings::IsKey("WESTRING_A B")
        && WorldEditStrings::IsKey("WESTRING_TRIGCAT_UNIT"), "key shape");
    Require(strings.Sources().size() == 2 && strings.Sources().front() == "first", "sources kept in priority order");
}

void TestOverlayDiscoveryAndFallback() {
    TempDirectory root;
    const auto ydwe = root.Path() / "YDWE";
    WriteText(ydwe / "ui" / "config", "ydwe\n");
    // 目录名大小写与游戏内路径不同：覆盖层按段不区分大小写查找。
    WriteText(ydwe / "share" / "zh-CN" / "mpq" / "ui" / "WorldEditStrings.txt",
        "[WorldEditStrings]\nWESTRING_TRIGCAT_UNIT=单位\nWESTRING_SHARED=覆盖层\n");
    WriteText(ydwe / "share" / "zh-CN" / "mpq" / "UI" / "worldeditgamestrings.txt",
        "[WorldEditGameStrings]\nWESTRING_GAME_ONLY=游戏字符串\n");
    WriteText(ydwe / "share" / "en-US" / "mpq" / "UI" / "WorldEditStrings.txt",
        "[WorldEditStrings]\nWESTRING_SHARED=English overlay\nWESTRING_ENGLISH_ONLY=Only in English\n");

    Require(War3Map::Game::YdweOverlayRoots(ydwe).size() == 1, "zh-CN overlay found from tool root");
    Require(War3Map::Game::YdweOverlayRoots(ydwe / "ui").size() == 1, "zh-CN overlay found from ui directory");
    Require(War3Map::Game::YdweEnglishRoots(ydwe).size() == 1, "en-US overlay found");
    Require(War3Map::Game::YdweOverlayRoots(root.Path() / "missing").empty(), "missing root yields no overlay");

    const auto roots = War3Map::Game::YdweOverlayRoots(ydwe);
    Require(War3Map::Game::ReadOverlayFile(roots, "UI\\WorldEditStrings.txt").has_value(), "backslash game path");
    Require(War3Map::Game::ReadOverlayFile(roots, "ui//WORLDEDITSTRINGS.TXT").has_value(), "doubled separators and case");
    Require(!War3Map::Game::ReadOverlayFile(roots, "..\\share\\zh-CN\\mpq\\ui\\WorldEditStrings.txt").has_value(),
        "parent traversal rejected");
    Require(!War3Map::Game::ReadOverlayFile(roots, "UI\\Missing.txt").has_value(), "missing file");

    War3Map::Game::WorldEditStringSources sources;
    sources.toolRoot = ydwe / "ui";
    const auto strings = War3Map::Game::LoadWorldEditStrings(sources);
    Require(strings.Resolve("WESTRING_TRIGCAT_UNIT") == "单位", "overlay string");
    Require(strings.Resolve("WESTRING_GAME_ONLY") == "游戏字符串", "second overlay file merged");
    Require(strings.Resolve("WESTRING_SHARED") == "覆盖层", "Chinese overlay beats English");
    Require(strings.Resolve("WESTRING_ENGLISH_ONLY") == "Only in English", "English original fills missing keys");
    Require(strings.Sources().size() == 3, "three files loaded");

    sources.language = "en-US";
    const auto english = War3Map::Game::LoadWorldEditStrings(sources);
    Require(english.Resolve("WESTRING_SHARED") == "English overlay", "selected language first");
    Require(english.Resolve("WESTRING_TRIGCAT_UNIT") == "单位", "zh-CN overlay still fills gaps like YDWE");
}

void TestDecoding() {
    const std::string utf8 = "\xEF\xBB\xBF" "单位";
    const auto* bytes = reinterpret_cast<const std::byte*>(utf8.data());
    Require(War3Map::Game::DecodeGameText({bytes, utf8.size()}) == "单位", "UTF-8 BOM stripped");
    // GB18030 的“单位”：B5 A5 CE BB。
    const std::byte gb[] = {std::byte{0xB5}, std::byte{0xA5}, std::byte{0xCE}, std::byte{0xBB}};
    Require(War3Map::Game::DecodeGameText(gb) == "单位", "GB18030 text converted to UTF-8");
}

// 声音通道表：编号排序、3D 标记、名称经字符串表解析；格式不符与重复编号不猜测。
void TestSoundChannels() {
    WorldEditStrings strings;
    strings.AddEntry("WESTRING_CHANNEL_A", "通道甲");
    const std::string data = "\xEF\xBB\xBF[Other]\n00=1,WESTRING_CHANNEL_A\n[SoundChannels]\n"
        "02=0,WESTRING_CHANNEL_MISSING\n00=1,WESTRING_CHANNEL_A\n"
        "x1=1,WESTRING_BAD_ID\n-3=1,WESTRING_NEGATIVE\n04=2,WESTRING_BAD_FLAG\n05=1\n"
        "06=1,WESTRING_TWICE\n6=0,WESTRING_TWICE_AGAIN\n";
    const auto channels = War3Map::Game::ParseSoundChannels(data, &strings);
    Require(channels.size() == 2, "only well-formed unique channel rows kept");
    Require(channels[0].id == 0 && channels[0].supports3d && channels[0].name == "通道甲", "name resolved via strings");
    Require(channels[1].id == 2 && !channels[1].supports3d && channels[1].name == "WESTRING_CHANNEL_MISSING"
        && channels[1].nameKey == "WESTRING_CHANNEL_MISSING", "unresolved key kept as name");
    Require(War3Map::Game::ParseSoundChannels("[TileSets]\nA=1,x\n", &strings).empty(), "no section yields no channels");
}

} // namespace

int main() {
    try {
        TestParsingAndPriority();
        TestOverlayDiscoveryAndFallback();
        TestDecoding();
        TestSoundChannels();
        std::cout << "WorldEditStrings: 4 scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
