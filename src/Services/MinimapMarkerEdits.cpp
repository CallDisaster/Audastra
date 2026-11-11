#include "War3MapCore/Services/MinimapMarkerEdits.h"

#include "War3MapCore/Hash.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <new>
#include <ranges>

namespace War3Map::Services {
namespace {
using Bytes = std::vector<std::byte>;
using Rows = std::vector<MapData::MinimapMarkerRecord>;
constexpr std::size_t kMaximumRows = 65536;
constexpr std::size_t kMmpBytes = 8 + kMaximumRows * 16;
constexpr std::size_t kHeaderBytes = 80;
constexpr std::size_t kSidecarBytes = kHeaderBytes + 3 * kMmpBytes;
constexpr auto kArea = "Minimap.MarkerEdits";
constexpr auto kConflict = "MINIMAP_MARKER_PROVENANCE";
struct Error { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, const char* message) {
    throw Error{Status::Failure(kArea, code, message).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Error{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class T, class F> Result<T> Checked(F work) {
    try { return Result<T>::Success(work()); }
    catch (Error& error) { return {{}, std::move(error.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Result<T>::Failure(kArea, "MINIMAP_MARKER_BUDGET", "小地图图标分配失败。");
    }
}
void Append(Bytes& target, std::string_view text) {
    const auto input = std::as_bytes(std::span(text));
    target.insert(target.end(), input.begin(), input.end());
}
void Put32(Bytes& target, std::size_t value) {
    for (unsigned i = 0; i < 4; ++i) target.push_back(static_cast<std::byte>((value >> (i * 8)) & 255));
}
std::size_t Get32(std::span<const std::byte> data, std::size_t offset) {
    std::size_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= std::to_integer<std::size_t>(data[offset + i]) << (8 * i);
    return value;
}
Bytes Mmp(const Rows& rows) {
    return Take(MapData::SerializeMapMinimapMarkers({0, rows}, {kMmpBytes, kMaximumRows}));
}
Rows ParseMmp(std::span<const std::byte> bytes) {
    return Take(MapData::ParseMapMinimapMarkers(bytes, {kMmpBytes, kMaximumRows})).records;
}
struct Document {
    bool managed = false;
    std::string identity;
    Rows visible, author, suppressed;
};
Bytes Encode(const Document& document, bool pending) {
    const auto visible = Mmp(document.visible);
    const auto author = Mmp(document.author);
    const auto suppressed = Mmp(document.suppressed);
    Bytes data;
    data.reserve(kHeaderBytes + visible.size() + author.size() + suppressed.size());
    Append(data, pending ? "AME1" : "AMM1");
    Append(data, document.identity);
    Put32(data, document.managed ? 1 : 0);
    Put32(data, visible.size());
    Put32(data, author.size());
    for (const auto* part : {&visible, &author, &suppressed}) data.insert(data.end(), part->begin(), part->end());
    return data;
}
Document Decode(std::span<const std::byte> bytes, bool pending) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kSidecarBytes ||
        std::memcmp(bytes.data(), pending ? "AME1" : "AMM1", 4) != 0)
        Fail(kConflict, "小地图图标归属记录版本或长度无效。");
    Document output;
    output.identity.assign(reinterpret_cast<const char*>(bytes.data() + 4), 64);
    const auto flags = Get32(bytes, 68), visible = Get32(bytes, 72), author = Get32(bytes, 76);
    bytes = bytes.subspan(kHeaderBytes);
    if (flags > 1 || visible > bytes.size() || author > bytes.size() - visible)
        Fail(kConflict, "小地图图标归属记录长度无效。");
    output.managed = flags != 0;
    output.visible = ParseMmp(bytes.first(visible));
    output.author = ParseMmp(bytes.subspan(visible, author));
    output.suppressed = ParseMmp(bytes.subspan(visible + author));
    if ((!output.managed && !output.suppressed.empty()) ||
        output.visible.size() + output.author.size() > kMaximumRows)
        Fail(kConflict, "小地图图标归属记录互相矛盾。");
    return output;
}
Rows Join(const Rows& prefix, const Rows& author) {
    if (prefix.size() + author.size() > kMaximumRows)
        Fail("MINIMAP_MARKER_BUDGET", "生成与手动图标总数超过上限。");
    Rows rows = prefix;
    rows.insert(rows.end(), author.begin(), author.end());
    return rows;
}
bool ValidPosition(const MapData::MinimapMarkerRecord& value) {
    return std::ranges::all_of(value.positionRaw, [](auto coordinate) { return coordinate >= 0 && coordinate <= 255; });
}
} // namespace

Result<MinimapMarkerEdits> MinimapMarkerEdits::Read(const MapArchiveSession& archive,
    std::span<const std::byte> pending) {
    return Checked<MinimapMarkerEdits>([&] {
        const std::array<std::string, 2> names{"war3map.mmp", kMinimapMarkerProvenance};
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size(); limits.memberBytes = kSidecarBytes;
        limits.totalBytes = kMmpBytes + kSidecarBytes;
        const auto files = Take(archive.CaptureExactMembers(names, limits));
        const auto& native = files[0];
        const auto& metadata = files[1];
        // A missing or empty member contains no records to reclassify. A
        // nonempty member must still pass the native codec before any edit.
        auto original = native.present && !native.bytes.empty() ? ParseMmp(native.bytes) : Rows{};
        MinimapMarkerEdits output;
        output.archiveIdentity_ = archive.SourceSha256();
        output.sourceIdentity_ = Take(Sha256(native.bytes));
        Document state{original.empty(), output.sourceIdentity_, std::move(original), {}, {}};
        if (metadata.present) {
            auto loaded = Decode(metadata.bytes, false);
            if (!native.present || loaded.identity != state.identity ||
                Join(loaded.visible, loaded.author) != state.visible)
                Fail(kConflict, "当前图标与归属记录不符；保留地图中的原图标。");
            state = std::move(loaded);
        }
        if (!pending.empty()) {
            auto staged = Decode(pending, true);
            if (staged.identity != state.identity || staged.managed != state.managed)
                Fail("MINIMAP_MARKER_STALE", "图标编辑来源已变化，请重新读取。");
            state = std::move(staged);
            output.pending_ = true;
        }
        output.managed_ = state.managed;
        output.visibleGenerated_ = std::move(state.visible);
        output.author_ = std::move(state.author);
        output.suppressed_ = std::move(state.suppressed);
        return output;
    });
}

Result<MinimapMarkerView> MinimapMarkerEdits::View() const {
    return Checked<MinimapMarkerView>([&] {
        MinimapMarkerView view;
        view.legacy = !managed_;
        view.records = Join(visibleGenerated_, author_);
        view.origins.assign(visibleGenerated_.size(), managed_ ? MinimapMarkerOrigin::Generated : MinimapMarkerOrigin::Legacy);
        view.origins.resize(view.records.size(), MinimapMarkerOrigin::Author);
        auto bytes = Encode({managed_, sourceIdentity_, visibleGenerated_, author_, suppressed_}, true);
        Append(bytes, archiveIdentity_);
        view.revision = Take(Sha256(bytes));
        return view;
    });
}

Result<std::vector<std::byte>> MinimapMarkerEdits::Apply(std::string_view revision,
    std::span<const MinimapMarkerEdit> edits) const {
    return Checked<Bytes>([&] {
        if (Take(View()).revision != revision)
            Fail("MINIMAP_MARKER_STALE", "图标行已变化，请重新读取。");
        if (edits.empty() || edits.size() > 1024)
            Fail("MINIMAP_MARKER_BUDGET", "一次编辑须包含 1 至 1024 条图标操作。");
        Document result{managed_, sourceIdentity_, visibleGenerated_, author_, suppressed_};
        for (const auto& edit : edits) {
            if (edit.operation == MinimapMarkerOperation::Add) {
                if (edit.value.typeRaw < 0 || edit.value.typeRaw > 2 || !ValidPosition(edit.value))
                    Fail("MINIMAP_MARKER_VALUE", "新图标类型须为 0 至 2，坐标须为 0 至 255。");
                result.author.push_back(edit.value);
            } else if (edit.operation == MinimapMarkerOperation::Move || edit.operation == MinimapMarkerOperation::Remove) {
                const auto count = result.visible.size();
                const bool move = edit.operation == MinimapMarkerOperation::Move;
                if (edit.row >= count + result.author.size() || (move && !ValidPosition(edit.value)))
                    Fail("MINIMAP_MARKER_VALUE", "图标行不存在，或坐标超出 0 至 255。");
                if (edit.row < count) {
                    auto position = result.visible.begin() + edit.row;
                    auto moved = *position;
                    if (move && moved.positionRaw == edit.value.positionRaw) continue;
                    if (managed_) result.suppressed.push_back(moved);
                    result.visible.erase(position);
                    if (move) {
                        moved.positionRaw = edit.value.positionRaw;
                        result.author.push_back(moved);
                    }
                } else {
                    auto position = result.author.begin() + (edit.row - count);
                    if (move) position->positionRaw = edit.value.positionRaw;
                    else result.author.erase(position);
                }
            } else {
                Fail("MINIMAP_MARKER_VALUE", "未知图标操作。");
            }
            if (result.visible.size() + result.author.size() > kMaximumRows || result.suppressed.size() > kMaximumRows)
                Fail("MINIMAP_MARKER_BUDGET", "小地图图标超过记录预算。");
        }
        return Encode(result, true);
    });
}

Result<MinimapMarkerProjection> MinimapMarkerEdits::Project(std::span<const std::byte> generated) const {
    return Checked<MinimapMarkerProjection>([&] {
        auto prefix = managed_ && !generated.empty() ? ParseMmp(generated) : visibleGenerated_;
        if (managed_ && !generated.empty()) {
            std::map<MapData::MinimapMarkerRecord, std::size_t> counts;
            for (const auto& row : suppressed_) ++counts[row];
            std::erase_if(prefix, [&](const auto& row) {
                const auto found = counts.find(row);
                if (found == counts.end() || found->second == 0) return false;
                --found->second;
                return true;
            });
        }
        const auto rows = Join(prefix, author_);
        auto markers = Mmp(rows);
        auto sidecar = Encode({managed_, Take(Sha256(markers)), std::move(prefix), author_, suppressed_}, false);
        return MinimapMarkerProjection{std::move(markers), std::move(sidecar), rows.size()};
    });
}

} // namespace War3Map::Services
