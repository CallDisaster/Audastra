#include "War3MapCore/Campaign/CampaignInfo.h"

#include <bit>
#include <limits>
#include <stdexcept>

namespace War3Map::Campaign {
namespace {
// 格式事实交叉核对 wc3libs W3F.java（Apache-2.0）与公开 w3n/w3f 说明。
// 本解析器为独立实现；未复制 wc3lib（GPL）的源码。
constexpr std::size_t kStringLimit = 1024u * 1024u;
struct Failure { std::string message; };
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t position = 0;
    std::uint8_t Byte() {
        if (position == bytes.size()) throw Failure{"战役信息在偏移 " + std::to_string(position) + " 处截断。"};
        return std::to_integer<std::uint8_t>(bytes[position++]);
    }
    std::uint32_t U32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(Byte()) << shift;
        return value;
    }
    std::int32_t I32() { return std::bit_cast<std::int32_t>(U32()); }
    std::string Text() {
        std::string result;
        for (;;) {
            const auto next = Byte();
            if (!next) return result;
            if (result.size() == kStringLimit) throw Failure{"战役文本超过 1 MiB。"};
            result.push_back(static_cast<char>(next));
        }
    }
    std::size_t Count() {
        const auto count = U32();
        if (count > kMaximumCampaignEntries) throw Failure{"战役条目超过 4096 项。"};
        return count;
    }
};
struct Writer {
    std::vector<std::byte> bytes;
    void Byte(std::uint8_t value) {
        if (bytes.size() >= kMaximumCampaignInfoBytes) throw Failure{"战役信息超过 16 MiB。"};
        bytes.push_back(static_cast<std::byte>(value));
    }
    void U32(std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) Byte(static_cast<std::uint8_t>(value >> shift)); }
    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Text(const std::string& value) {
        if (value.size() > kStringLimit || value.find('\0') != std::string::npos) throw Failure{"战役文本含 NUL 或超过 1 MiB。"};
        for (unsigned char c : value) Byte(c);
        Byte(0);
    }
};
} // namespace

Result<CampaignInfo> ParseInfo(std::span<const std::byte> bytes) {
    if (bytes.size() > kMaximumCampaignInfoBytes)
        return Result<CampaignInfo>::Failure("战役", "CAMPAIGN_INFO_BUDGET", "战役信息超过 16 MiB。");
    try {
        Reader read{bytes}; CampaignInfo info;
        info.formatVersion = read.I32();
        if (info.formatVersion != 1)
            return Result<CampaignInfo>::Failure("战役", "CAMPAIGN_VERSION_UNSUPPORTED", "仅支持经典 war3campaign.w3f 版本 1，未知版本保持原文件不变。");
        info.saves = read.I32(); info.editorVersion = read.I32();
        info.name = read.Text(); info.difficulty = read.Text(); info.author = read.Text(); info.description = read.Text();
        info.flags = read.U32(); info.background = read.I32(); info.backgroundPath = read.Text();
        info.minimapPath = read.Text(); info.ambientSound = read.I32(); info.ambientSoundPath = read.Text();
        info.fogStyle = read.I32();
        for (auto& value : info.fogBits) value = read.U32();
        for (auto& value : info.fogColor) value = read.Byte();
        info.cursor = read.I32();
        const auto buttons = read.Count();
        for (std::size_t i = 0; i < buttons; ++i)
            info.buttons.push_back({static_cast<StableId>(i + 1), read.I32(), read.Text(), read.Text(), read.Text()});
        const auto maps = read.Count();
        for (std::size_t i = 0; i < maps; ++i)
            info.maps.push_back({static_cast<StableId>(buttons + i + 1), read.Text(), read.Text()});
        info.trailing.assign(bytes.begin() + static_cast<std::ptrdiff_t>(read.position), bytes.end());
        auto result = Result<CampaignInfo>::Success(std::move(info));
        if (!result.value->trailing.empty()) result.diagnostics.push_back({DiagnosticSeverity::Warning, "战役", "CAMPAIGN_TRAILING_PRESERVED", "未知尾部原样保留；本编辑器不解释其语义。"});
        if (!result.value->maps.empty()) result.diagnostics.push_back({DiagnosticSeverity::Info, "战役", "CAMPAIGN_MAP_PREFIX_PRESERVED", "地图列表前置字符串原样保留，未猜测其用途。"});
        return result;
    } catch (const Failure& failure) {
        return Result<CampaignInfo>::Failure("战役", "CAMPAIGN_INFO_INVALID", failure.message);
    } catch (const std::bad_alloc&) {
        return Result<CampaignInfo>::Failure("战役", "CAMPAIGN_INFO_MEMORY", "战役信息内存分配失败。");
    }
}

Result<std::vector<std::byte>> SerializeInfo(const CampaignInfo& info) {
    using Output = Result<std::vector<std::byte>>;
    if (info.formatVersion != 1 || info.buttons.size() > kMaximumCampaignEntries || info.maps.size() > kMaximumCampaignEntries)
        return Output::Failure("战役", "CAMPAIGN_INFO_UNSUPPORTED", "战役信息版本或条目数量不支持写出。");
    try {
        Writer write;
        write.I32(info.formatVersion); write.I32(info.saves); write.I32(info.editorVersion);
        write.Text(info.name); write.Text(info.difficulty); write.Text(info.author); write.Text(info.description);
        write.U32(info.flags); write.I32(info.background); write.Text(info.backgroundPath); write.Text(info.minimapPath);
        write.I32(info.ambientSound); write.Text(info.ambientSoundPath); write.I32(info.fogStyle);
        for (const auto value : info.fogBits) write.U32(value);
        for (const auto value : info.fogColor) write.Byte(value);
        write.I32(info.cursor); write.U32(static_cast<std::uint32_t>(info.buttons.size()));
        for (const auto& button : info.buttons) {
            write.I32(button.visible); write.Text(button.chapterTitle); write.Text(button.mapTitle); write.Text(button.mapPath);
        }
        write.U32(static_cast<std::uint32_t>(info.maps.size()));
        for (const auto& map : info.maps) { write.Text(map.unknown); write.Text(map.path); }
        for (const auto value : info.trailing) write.Byte(std::to_integer<std::uint8_t>(value));
        return Output::Success(std::move(write.bytes));
    } catch (const Failure& failure) { return Output::Failure("战役", "CAMPAIGN_INFO_INVALID", failure.message); }
    catch (const std::bad_alloc&) { return Output::Failure("战役", "CAMPAIGN_INFO_MEMORY", "战役信息内存分配失败。"); }
}
} // namespace War3Map::Campaign
