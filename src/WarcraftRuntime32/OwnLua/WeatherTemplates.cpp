#include "WeatherTemplates.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Text/SlkCells.h"

#include <algorithm>
#include <iterator>

namespace Audastra::Runtime32::OwnLua {
namespace {
constexpr char kMember[] = "TerrainArt\\Weather.slk";
constexpr std::size_t kWeatherBytes = 1024 * 1024;
constexpr std::size_t kWeatherRows = 4096;

std::string Reason(const std::vector<War3Map::Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "来源未返回有效数据。";
    return diagnostics.front().code + " " + diagnostics.front().message;
}
}

WeatherTemplates WeatherTemplates::Read(const LuaModuleReader& map, const std::filesystem::path& gameRoot) {
    WeatherTemplates result;
    try {
        const auto local = map(kMember, kWeatherBytes);
        if (!local.error.empty()) {
            result.error_ = "地图 TerrainArt/Weather.slk 读取失败：" + local.error;
            return result;
        }
        if (local.found) return Parse(local.source, "地图");

        const auto game = War3Map::Game::GameDataSource::MountStrict(gameRoot);
        if (!game.Succeeded()) {
            result.error_ = "无法挂载游戏天气表来源：" + Reason(game.diagnostics);
            return result;
        }
        const auto table = game.value->ReadFileBoundedWithSource(kMember, kWeatherBytes);
        if (!table.Succeeded()) {
            result.error_ = "游戏 TerrainArt/Weather.slk 缺失或读取失败：" + Reason(table.diagnostics);
            return result;
        }
        const auto& bytes = table.value->bytes;
        const auto text = bytes.empty() ? std::string_view{}
            : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return Parse(text, table.value->archiveName);
    } catch (const std::exception& error) {
        result.error_ = std::string("天气表读取异常：") + error.what();
    }
    return result;
}

WeatherTemplates WeatherTemplates::Parse(std::string_view source, std::string origin) {
    WeatherTemplates result;
    result.origin_ = std::move(origin);
    const auto corrupt = [&](std::string reason) {
        result.error_ = result.origin_ + " 的 TerrainArt/Weather.slk 无效：" + std::move(reason);
        return result;
    };
    if (source.empty() || source.size() > kWeatherBytes || source.find('\0') != source.npos)
        return corrupt("表为空、超出读取预算或包含 NUL。");
    const auto parsed = War3Map::Text::ParseSlkCells(source, War3Map::Text::SlkCellMode::CatalogUnique);
    if (!parsed.Succeeded()) return corrupt(Reason(parsed.diagnostics));
    const auto& rows = *parsed.value;
    const auto& header = rows.at(1);
    if (header.at(1) != "effectID" || std::count_if(header.begin(), header.end(),
            [](const auto& cell) { return cell.second == "effectID"; }) != 1)
        return corrupt("首列没有唯一的 effectID 标题。");
    if (rows.size() <= 1 || rows.size() > kWeatherRows + 1)
        return corrupt("天气行为空或超过 4096 条预算。");

    std::set<std::uint32_t> ids;
    for (auto row = std::next(rows.begin()); row != rows.end(); ++row) {
        const auto cell = row->second.find(1);
        if (cell == row->second.end() || cell->second.size() != 4
            || !std::all_of(cell->second.begin(), cell->second.end(),
                [](unsigned char c) { return c >= 0x20 && c <= 0x7e; }))
            return corrupt("第 " + std::to_string(row->first) + " 行没有四字节 effectID。");
        std::uint32_t id = 0;
        for (const unsigned char c : cell->second) id = (id << 8) | c;
        if (!ids.insert(id).second) return corrupt("effectID 重复：" + cell->second);
    }
    result.ids_ = std::move(ids);
    result.error_.clear();
    return result;
}

std::string WeatherTemplates::CheckCall(std::string_view name, const NativeSignature& signature,
    std::span<const NativeValue> supplied) const {
    if (name == "AddWeatherEffect") {
        static const auto expected = ParseNativeSignature("(Hrect;I)Hweathereffect;");
        if (signature != *expected || supplied.size() != 2 || !std::holds_alternative<std::int32_t>(supplied[1]))
            return "AddWeatherEffect 的登记签名或编号参数不符，未调用 native。";
        const auto id = std::get<std::int32_t>(supplied[1]);
        if (!ids_.contains(static_cast<std::uint32_t>(id))) {
            const auto reason = error_.empty() ? "不在 " + origin_ + " 的 TerrainArt/Weather.slk 中。" : error_;
            return "AddWeatherEffect 天气编号 " + std::to_string(id) + " 被拒绝，未调用 native。" + reason;
        }
        const auto* rect = std::get_if<NativeHandle>(&supplied[0]);
        if (!rect || !rect->value || rect->kind != NativeHandleKind::Reference)
            return "AddWeatherEffect 需要当前地图的有效 rect，未调用 native。";
        return {};
    }
    if (name == "EnableWeatherEffect" || name == "RemoveWeatherEffect") {
        static const auto enable = ParseNativeSignature("(Hweathereffect;B)V");
        static const auto remove = ParseNativeSignature("(Hweathereffect;)V");
        if (signature == *(name == "EnableWeatherEffect" ? enable : remove)) return {};
    } else if (!UsesWeatherEffect(signature)) return {};
    return "weathereffect native 的名称或签名尚未核验，未调用 native。";
}

} // namespace Audastra::Runtime32::OwnLua
