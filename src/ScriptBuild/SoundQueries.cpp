#include "War3MapCore/Script/ScriptQueries.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Game/SoundInfoTables.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Text/SlkCells.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

// 声音标签与时长（WE 输出规则，按参考脚本核对）：
//   标签：游戏 UI\SoundInfo 各表（按表名排序、表内按行序）中 DirectoryBase + FileNames 与路径相同的第一个 SoundName；
//   时长（毫秒，就近取偶）：WAV 取 data 块字节数 / 每秒字节数；MP3 取（文件大小 − 首帧偏移）× 8 / 首帧码率；读不到文件时不写。
namespace War3Map::Script {
namespace {

std::string Key(std::string_view path) {
    std::string key;
    key.reserve(path.size());
    for (const char c : path) {
        const auto u = static_cast<unsigned char>(c);
        key += c == '/' ? '\\' : (u >= 'A' && u <= 'Z') ? static_cast<char>(u - 'A' + 'a') : c;
    }
    return key;
}

std::string_view Text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::optional<int> WavDuration(std::string_view bytes) {
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16 |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24;
    };
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE") return std::nullopt;
    std::uint32_t byteRate = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const std::string_view id = bytes.substr(at, 4);
        const std::uint32_t size = u32(at + 4);
        if (id == "fmt " && at + 16 <= bytes.size()) byteRate = u32(at + 16);
        if (id == "data") {
            if (byteRate == 0) return std::nullopt;
            return static_cast<int>(std::nearbyint(static_cast<double>(size) * 1000.0 / byteRate));
        }
        at += 8 + size + (size & 1u);
    }
    return std::nullopt;
}

std::optional<int> Mp3Duration(std::string_view bytes) {
    std::size_t at = 0;
    if (bytes.size() >= 10 && bytes.substr(0, 3) == "ID3") {
        const auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]) & 0x7F); };
        at = 10 + (b(6) << 21 | b(7) << 14 | b(8) << 7 | b(9));
    }
    static constexpr int kBitrates[2][3][15] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
            {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
            {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
    for (; at + 4 <= bytes.size(); ++at) {
        const auto b0 = static_cast<unsigned char>(bytes[at]);
        const auto b1 = static_cast<unsigned char>(bytes[at + 1]);
        const auto b2 = static_cast<unsigned char>(bytes[at + 2]);
        if (b0 != 0xFF || (b1 & 0xE0) != 0xE0) continue;
        const int version = (b1 >> 3) & 3; // 3 = MPEG1，2 = MPEG2，0 = MPEG2.5
        const int layer = (b1 >> 1) & 3;   // 3 = Layer I，1 = Layer III
        const int bitrateIndex = (b2 >> 4) & 15;
        if (version == 1 || layer == 0 || bitrateIndex == 0 || bitrateIndex == 15 || ((b2 >> 2) & 3) == 3) continue;
        const int bitrate = kBitrates[version == 3 ? 0 : 1][3 - layer][bitrateIndex] * 1000;
        // 参考图 140 个 MP3 中 135 个一致；其余 5 个（256kbps）比 WE 少约 4 毫秒，规律未明。
        const double milliseconds = static_cast<double>(bytes.size() - at) * 8.0 * 1000.0 / bitrate;
        return static_cast<int>(std::nearbyint(milliseconds));
    }
    return std::nullopt;
}

struct SoundState {
    const Game::GameDataSource* game = nullptr;
    const MapArchiveSession* archive = nullptr;
    std::once_flag labelsLoaded;
    std::unordered_map<std::string, std::string> labels;

    void LoadLabels() {
        if (game == nullptr) return;
        for (const auto table : Game::kSoundInfoTables) {
            const auto bytes = game->ReadFile("UI\\SoundInfo\\" + std::string(table) + ".slk");
            if (!bytes.Succeeded() || !bytes.value) continue;
            // 保留旧脚本标签的宽松 SYLK 行为；目录发现另用严格模式校验。
            const auto parsed = War3Map::Text::ParseSlkCells(
                Text(*bytes.value), War3Map::Text::SlkCellMode::SoundQueriesLegacy);
            if (!parsed.Succeeded() || !parsed.value) continue;
            const auto& cells = *parsed.value;
            const auto header = cells.find(1);
            if (header == cells.end()) continue;
            int nameColumn = 0, filesColumn = 0, baseColumn = 0;
            for (const auto& [column, title] : header->second) {
                if (title == "SoundName") nameColumn = column;
                else if (title == "FileNames") filesColumn = column;
                else if (title == "DirectoryBase") baseColumn = column;
            }
            if (nameColumn == 0 || filesColumn == 0) continue;
            for (const auto& [row, values] : cells) {
                if (row == 1) continue;
                const auto name = values.find(nameColumn);
                const auto files = values.find(filesColumn);
                if (name == values.end() || files == values.end()) continue;
                const auto base = values.find(baseColumn);
                const std::string directory = base == values.end() ? std::string{} : base->second;
                std::size_t start = 0;
                while (start <= files->second.size()) {
                    std::size_t comma = files->second.find(',', start);
                    if (comma == std::string::npos) comma = files->second.size();
                    if (comma > start) labels.try_emplace(Key(directory + files->second.substr(start, comma - start)), name->second);
                    start = comma + 1;
                }
            }
        }
    }

    // 地图导入文件优先于游戏文件。WAV 只需文件头（游戏 WAV 的后续扇区是有损压缩）。
    std::optional<std::string> ReadAudio(const std::string& path, bool headOnly) const {
        if (archive != nullptr && archive->HasMember(path)) {
            if (auto bytes = archive->ReadMember(path); bytes.Succeeded() && bytes.value) return std::string(Text(*bytes.value));
            return std::nullopt;
        }
        if (game == nullptr) return std::nullopt;
        auto bytes = headOnly ? game->ReadFileHead(path, 4096) : game->ReadFile(path);
        if (!bytes.Succeeded() || !bytes.value) return std::nullopt;
        return std::string(Text(*bytes.value));
    }
};

} // namespace

void AttachSoundQueries(ObjectQueries& queries, const Game::GameDataSource* game, const MapArchiveSession* archive) {
    auto state = std::make_shared<SoundState>();
    state->game = game;
    state->archive = archive;
    queries.soundLabel = [state](std::string_view path) -> std::optional<std::string> {
        std::call_once(state->labelsLoaded, [&] { state->LoadLabels(); });
        const auto found = state->labels.find(Key(path));
        if (found == state->labels.end()) return std::nullopt;
        return found->second;
    };
    queries.soundDuration = [state](std::string_view path) -> std::optional<int> {
        const std::string key = Key(path);
        const bool wav = key.ends_with(".wav");
        if (!wav && !key.ends_with(".mp3")) return std::nullopt;
        // 格式按内容判断（地图里有扩展名为 .wav 的 MP3）；MP3 需要完整文件大小，只读了文件头时重读整个文件。
        auto bytes = state->ReadAudio(std::string(path), wav);
        if (!bytes) return std::nullopt;
        if (bytes->starts_with("RIFF")) return WavDuration(*bytes);
        if (wav && bytes->size() >= 4096) bytes = state->ReadAudio(std::string(path), false);
        return bytes ? Mp3Duration(*bytes) : std::nullopt;
    };
}

} // namespace War3Map::Script
