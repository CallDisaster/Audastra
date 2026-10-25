#include "TerrainHistoryCodec.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr std::size_t FixedAllowance = 128u * 1024u;
template<class T> Result<T> InvalidHistory() {
    return Result<T>::Failure("Terrain.History", "TERRAIN_HISTORY_INVALID",
        "FTJE一座保存站记录的版本、结构、索引、历史成员或累计预算无效，未恢复任何命令。");
}
bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
struct HistoryWire {
    std::vector<std::byte> data;
    void Number(std::uint64_t value, unsigned count) {
        if (count > TerrainJournalMaximumBytes - data.size()) throw std::length_error("history");
        for (unsigned i = 0; i < count; ++i) data.push_back(std::byte((value >> (8 * i)) & 255));
    }
    void Bytes(std::span<const std::byte> value) {
        if (value.size() > TerrainJournalMaximumBytes - data.size() || TerrainJournalMaximumBytes - data.size() - value.size() < 4)
            throw std::length_error("history");
        Number(value.size(), 4); data.insert(data.end(), value.begin(), value.end());
    }
    void Text(std::string_view value) { Bytes(std::as_bytes(std::span(value.data(), value.size()))); }
};
struct HistoryReader {
    std::span<const std::byte> data;
    std::size_t cursor = 0;
    std::uint64_t Number(unsigned count) {
        if (count > data.size() - cursor) throw std::invalid_argument("history");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(std::to_integer<unsigned>(data[cursor++])) << (8 * i);
        return value;
    }
    bool Bool() { const auto value = Number(1); if (value > 1) throw std::invalid_argument("history"); return value != 0; }
    std::span<const std::byte> Bytes(std::size_t maximum) {
        const auto count = Number(4);
        if (count > maximum || count > data.size() - cursor) throw std::invalid_argument("history");
        auto value = data.subspan(cursor, static_cast<std::size_t>(count)); cursor += static_cast<std::size_t>(count); return value;
    }
    std::string Text(std::size_t maximum) {
        const auto bytes = Bytes(maximum); return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
};
bool HistoryShape(const TerrainStrokeJournal& journal) {
    if (!journal.history) return false;
    const auto& history = *journal.history;
    if (history.frozenBaseline || history.frozenRetainedBytes || !history.savedIndex
        || history.savedIndex > journal.strokes.size() || history.currentIndex > journal.strokes.size()
        || history.baselineMembers.size() != TerrainJournalMemberNames.size()
        || journal.strokes.empty() || journal.strokes.size() > 128) return false;
    std::size_t remaining = TerrainJournalMaximumBytes - FixedAllowance;
    for (const auto* layers : {&history.baselinePathingLayers, &journal.baselinePathingLayers}) {
        if (layers->empty() || layers->size() > remaining) return false;
        remaining -= layers->size();
    }
    for (std::size_t i = 0; i < history.baselineMembers.size(); ++i) {
        const auto& member = history.baselineMembers[i];
        if (member.name != TerrainJournalMemberNames[i] || (i < 2 && !member.present)
            || (!member.present && !member.bytes.empty()) || member.bytes.size() > remaining) return false;
        remaining -= member.bytes.size();
    }
    for (const auto& stroke : journal.strokes) {
        for (const auto* delta : {stroke.ordinaryDoodads ? static_cast<const TerrainPlacementJournalDelta*>(&*stroke.ordinaryDoodads) : nullptr,
                 stroke.units ? static_cast<const TerrainPlacementJournalDelta*>(&*stroke.units) : nullptr,
                 stroke.startLocations ? static_cast<const TerrainPlacementJournalDelta*>(&stroke.startLocations->units) : nullptr,
                 stroke.startLocations ? static_cast<const TerrainPlacementJournalDelta*>(&stroke.startLocations->settings) : nullptr}) {
            if (!delta) continue;
            if (delta->inserted.size() > remaining) return false;
            remaining -= delta->inserted.size();
        }
    }
    return true;
}
} // namespace

Result<TerrainStrokeJournal> TerrainHistoryBaselineJournal(const TerrainStrokeJournal& journal) {
    if (!HistoryShape(journal)) return InvalidHistory<TerrainStrokeJournal>();
    try {
        TerrainStrokeJournal baseline;
        baseline.strokes = journal.strokes;
        baseline.baselinePathingLayers = journal.history->baselinePathingLayers;
        HistoryWire identity;
        identity.Text("FTJE-historical-structure-v1");
        for (const auto& member : journal.history->baselineMembers) {
            TerrainJournalMember manifest;
            manifest.name = member.name; manifest.present = member.present; manifest.bytes = member.bytes.size();
            if (member.present) {
                auto hash = Sha256(member.bytes); if (!hash.Succeeded() || !hash.value) return {{}, hash.diagnostics};
                manifest.sha256 = std::move(*hash.value);
            }
            identity.Text(manifest.name); identity.Number(manifest.present, 1);
            identity.Text(manifest.sha256); identity.Number(manifest.bytes, 8);
            baseline.members.push_back(std::move(manifest));
        }
        auto digest = Sha256(identity.data); if (!digest.Succeeded() || !digest.value) return {{}, digest.diagnostics};
        baseline.sourceSha256 = std::move(*digest.value);
        const auto valid = ValidateTerrainStrokeJournal(baseline);
        if (!valid.Succeeded()) return {{}, valid.diagnostics};
        return Result<TerrainStrokeJournal>::Success(std::move(baseline));
    } catch (const std::bad_alloc&) { return InvalidHistory<TerrainStrokeJournal>(); }
      catch (const std::length_error&) { return InvalidHistory<TerrainStrokeJournal>(); }
}

namespace detail {
Status ValidateTerrainHistoryJournal(const TerrainStrokeJournal& journal) {
    if (!HistoryShape(journal) || !Sha(journal.sourceSha256)) return {InvalidHistory<TerrainStrokeJournal>().diagnostics};
    try {
        // 两层manifest分别验证：外层必须遵守原物理锚域，不能借history放宽旧variant。
        const auto valid = ValidateTerrainJournalParts(journal.sourceSha256, journal.members,
            journal.baselinePathingLayers, journal.strokes);
        if (!valid.Succeeded()) return valid;
        // 这里只验证旧manifest形状、presence和typed域。raw SHA及结构digest在
        // 后续已预收的BaselineJournal/Decode中生成，不在小预算恢复预检时复制正文。
        std::array<TerrainJournalMember, TerrainJournalMemberNames.size()> baseline;
        constexpr std::string_view placeholder = "0000000000000000000000000000000000000000000000000000000000000000";
        for (std::size_t i = 0; i < baseline.size(); ++i) {
            const auto& member = journal.history->baselineMembers[i];
            baseline[i] = {member.name, member.present, member.present ? std::string(placeholder) : std::string{}, member.bytes.size()};
        }
        return ValidateTerrainJournalParts(placeholder, baseline, journal.history->baselinePathingLayers, journal.strokes);
    } catch (const std::bad_alloc&) { return {InvalidHistory<TerrainStrokeJournal>().diagnostics}; }
      catch (const std::length_error&) { return {InvalidHistory<TerrainStrokeJournal>().diagnostics}; }
}

Result<std::vector<std::byte>> EncodeTerrainHistoryJournal(const TerrainStrokeJournal& journal) {
    const auto valid = ValidateTerrainHistoryJournal(journal);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    try {
        // typed正文始终调用原FTJ1-D codec；FTJE只封装保存站和旧结构输入，不复制笔刷解析器。
        auto baseline = TerrainHistoryBaselineJournal(journal);
        if (!baseline.Succeeded() || !baseline.value) return {{}, baseline.diagnostics};
        auto typed = EncodeTerrainStrokeJournal(*baseline.value);
        if (!typed.Succeeded() || !typed.value) return {{}, typed.diagnostics};
        HistoryWire wire;
        wire.Number(0x454a5446u, 4); wire.Text(journal.sourceSha256);
        wire.Number(journal.members.size(), 1);
        for (const auto& member : journal.members) {
            wire.Text(member.name); wire.Number(member.present, 1); wire.Text(member.sha256); wire.Number(member.bytes, 8);
        }
        wire.Bytes(journal.baselinePathingLayers);
        const auto& history = *journal.history;
        wire.Number(history.savedIndex, 4); wire.Number(history.currentIndex, 4);
        wire.Number(history.baselineMembers.size(), 1);
        for (const auto& member : history.baselineMembers) {
            wire.Text(member.name); wire.Number(member.present, 1); wire.Bytes(member.bytes);
        }
        // oldP已在原typed内，只有一份wire；外层数据字段保持显式，便于Services预算。
        wire.Bytes(*typed.value);
        return Result<std::vector<std::byte>>::Success(std::move(wire.data));
    } catch (const std::bad_alloc&) { return InvalidHistory<std::vector<std::byte>>(); }
      catch (const std::length_error&) { return InvalidHistory<std::vector<std::byte>>(); }
}

Result<TerrainStrokeJournal> DecodeTerrainHistoryJournal(std::span<const std::byte> bytes) {
    if (bytes.size() > TerrainJournalMaximumBytes) return InvalidHistory<TerrainStrokeJournal>();
    try {
        HistoryReader reader{bytes}; TerrainStrokeJournal journal; TerrainJournalHistory history;
        if (reader.Number(4) != 0x454a5446u) return InvalidHistory<TerrainStrokeJournal>();
        journal.sourceSha256 = reader.Text(64);
        if (reader.Number(1) != TerrainJournalMemberNames.size()) return InvalidHistory<TerrainStrokeJournal>();
        for (const auto name : TerrainJournalMemberNames) {
            TerrainJournalMember member;
            member.name = reader.Text(64); if (member.name != name) return InvalidHistory<TerrainStrokeJournal>();
            member.present = reader.Bool(); member.sha256 = reader.Text(64); member.bytes = reader.Number(8);
            journal.members.push_back(std::move(member));
        }
        const auto currentLayers = reader.Bytes(TerrainJournalMaximumBytes);
        journal.baselinePathingLayers.assign(currentLayers.begin(), currentLayers.end());
        history.savedIndex = static_cast<std::uint32_t>(reader.Number(4));
        history.currentIndex = static_cast<std::uint32_t>(reader.Number(4));
        if (reader.Number(1) != TerrainJournalMemberNames.size()) return InvalidHistory<TerrainStrokeJournal>();
        for (const auto name : TerrainJournalMemberNames) {
            ArchiveExactMember member;
            member.name = reader.Text(64); if (member.name != name) return InvalidHistory<TerrainStrokeJournal>();
            member.present = reader.Bool(); const auto payload = reader.Bytes(TerrainJournalMaximumBytes);
            if (!member.present && !payload.empty()) return InvalidHistory<TerrainStrokeJournal>();
            member.bytes.assign(payload.begin(), payload.end()); history.baselineMembers.push_back(std::move(member));
        }
        const auto typedBytes = reader.Bytes(TerrainJournalMaximumBytes);
        if (reader.cursor != bytes.size() || typedBytes.size() < 4 || typedBytes[3] == std::byte{'E'})
            return InvalidHistory<TerrainStrokeJournal>(); // 禁止嵌套history/递归预算。
        auto typed = DecodeTerrainStrokeJournal(typedBytes);
        if (!typed.Succeeded() || !typed.value || typed.value->history) return InvalidHistory<TerrainStrokeJournal>();
        history.baselinePathingLayers = typed.value->baselinePathingLayers;
        journal.strokes = typed.value->strokes;
        journal.history = std::move(history);
        auto expected = TerrainHistoryBaselineJournal(journal);
        if (!expected.Succeeded() || !expected.value || expected.value->sourceSha256 != typed.value->sourceSha256
            || expected.value->members.size() != typed.value->members.size()) return InvalidHistory<TerrainStrokeJournal>();
        for (std::size_t i = 0; i < expected.value->members.size(); ++i) {
            const auto& a = expected.value->members[i]; const auto& b = typed.value->members[i];
            if (a.name != b.name || a.present != b.present || a.sha256 != b.sha256 || a.bytes != b.bytes)
                return InvalidHistory<TerrainStrokeJournal>();
        }
        const auto valid = ValidateTerrainHistoryJournal(journal);
        if (!valid.Succeeded()) return {{}, valid.diagnostics};
        return Result<TerrainStrokeJournal>::Success(std::move(journal));
    } catch (const std::bad_alloc&) { return InvalidHistory<TerrainStrokeJournal>(); }
      catch (const std::length_error&) { return InvalidHistory<TerrainStrokeJournal>(); }
      catch (const std::invalid_argument&) { return InvalidHistory<TerrainStrokeJournal>(); }
}
} // namespace detail
} // namespace War3Map::MapData
