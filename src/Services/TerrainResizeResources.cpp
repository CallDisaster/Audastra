#include "War3MapCore/MapData/TerrainResizeResources.h"

#include "../Core/MapData/TerrainResizeResourceState.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.ResizeResources";
using TerrainResizeResourceDetail::kSpatialKinds;
using TerrainResizeResourceDetail::kSourceNames;
constexpr std::size_t kFixedBytes = 256u * 1024u;
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
std::string Key(std::string_view name) {
    auto value = Text::LowerAscii(name); std::replace(value.begin(), value.end(), '\\', '/'); return value;
}
ArchiveExactMemberLimits ReadLimits(std::size_t count, std::size_t bytes) {
    ArchiveExactMemberLimits result; result.memberCount = count;
    result.memberBytes = result.totalBytes = std::min(bytes, ArchiveExactMemberLimits{}.totalBytes);
    return result;
}
void Limits(const TerrainResizeLimits& limits) {
    const TerrainResizeLimits hard;
    if (limits.maximumCandidateBytes < kFixedBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork)
        Fail("TERRAIN_RESIZE_LIMITS", "尺寸资源预算只能收紧固定硬帽，且须容纳完整来源元数据。");
}

// 原版profile按固定链追加value；首值不被后来同名键覆盖。只接受有证据的
// 完整有限数值子集，保留行首section/key规则；未知语法不退回猜测默认值。
struct StartupValues {
    std::array<std::optional<std::uint32_t>, 6> values;
    void Append(std::span<const std::byte> bytes) {
        auto text = bytes.empty() ? std::string_view{}
            : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (text.find('\0') != std::string_view::npos || text.starts_with("\xff\xfe") || text.starts_with("\xfe\xff"))
            Fail("TERRAIN_RESIZE_PROFILE_ENCODING", "实际启动profile含NUL或未支持的宽字符编码。");
        if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
        std::string section;
        while (!text.empty()) {
            const auto end = text.find_first_of("\r\n"); const auto line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            if (line.empty() || line.starts_with("//")) continue;
            if (line.front() == '[') {
                const auto close = line.find(']');
                if (close != std::string_view::npos) section = Text::LowerAscii(line.substr(1, close - 1));
                continue;
            }
            const auto equal = line.find('=');
            if (equal == std::string_view::npos || equal + 1 == line.size()) continue;
            const auto key = Text::LowerAscii(line.substr(0, equal));
            std::size_t slot = values.size();
            if (section == "worldeditmisc") slot = key == "minimummapsize" ? 0 : key == "maximummapsize" ? 1 : slot;
            if (section == "cameramargins") {
                constexpr std::array<std::string_view, 4> keys{"left", "right", "bottom", "top"};
                for (std::size_t i = 0; i < keys.size(); ++i) if (key == keys[i]) slot = i + 2;
            }
            if (slot == values.size() || values[slot]) continue;
            auto token = line.substr(equal + 1);
            if (token.front() == '"') {
                token.remove_prefix(1); const auto close = token.find('"');
                if (close == std::string_view::npos || (close + 1 != token.size() && token[close + 1] != ','))
                    Fail("TERRAIN_RESIZE_PROFILE_VALUE", "实际尺寸或相机边距首value含未知引号语法。");
                token = token.substr(0, close);
            } else token = token.substr(0, token.find(','));
            token = Text::TrimCWhitespace(token);
            if (token.starts_with('+')) token.remove_prefix(1);
            std::uint32_t value = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
            if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 2048)
                Fail("TERRAIN_RESIZE_PROFILE_VALUE", "实际尺寸或相机边距首value不是0..2048完整整数。");
            values[slot] = value;
        }
    }
};
} // namespace

Result<TerrainResizeResources> TerrainResizeResources::CaptureInputs(const MemberReader& reader,
    const MapDataEditSession& session, const std::filesystem::path& gameRoot, const TerrainResizeLimits& limits) try {
    Limits(limits);
    const auto terrain = session.Terrain();
    if (!terrain || terrain->header.version != 11 || !session.HasTerrainPathing())
        Fail("TERRAIN_RESIZE_SOURCE", "尺寸调整需要已附入真实W3E/WPM的经典地形会话。");
    const auto revision = session.Revision(), sourceRevision = session.TerrainSourceRevision();
    std::size_t bytes = kFixedBytes, work = terrain->vertices.size();
    const auto reserve = [&](std::size_t count, std::size_t each = 1) {
        if (bytes > limits.maximumCandidateBytes || count > (limits.maximumCandidateBytes - bytes) / each)
            Fail("TERRAIN_RESIZE_BUDGET", "尺寸资源、完整空间成员或解析工作超过候选预算。");
        bytes += count * each;
    };
    std::vector<std::string> names; for (const auto name : kSourceNames) names.emplace_back(name);
    auto actual = Take(reader(names, ReadLimits(names.size(), limits.maximumCandidateBytes - bytes)));
    if (actual.size() != names.size()) Fail("TERRAIN_RESIZE_SOURCE", "尺寸捕获没有返回完整固定成员及absence。");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (Key(actual[i].name) != Key(names[i]) || (!actual[i].present && !actual[i].bytes.empty()))
            Fail("TERRAIN_RESIZE_SOURCE", "尺寸来源成员的顺序、名称或absence载荷不符。");
        reserve(actual[i].bytes.size(), 2);
    }
    if (!actual[0].present) Fail("TERRAIN_RESIZE_SOURCE", "实际来源缺少完整W3E。");
    Check(session.VerifyTerrainSourceBaseline(actual[0].bytes));
    // 完整WPM最终仍由SavePlan作G0逐格解释；这里先绑定真正物理基线，
    // 不允许外部附入另一张图的同尺寸WPM来取得resize命令资格。
    if (!actual[1].present || !session.pathingOriginal_ || actual[1].bytes != *session.pathingOriginal_)
        Fail("TERRAIN_RESIZE_PATHING_SOURCE", "会话原始WPM与实际来源不一致。");
    reserve(actual[0].bytes.size(), 16);
    auto sourceTerrain = Take(ParseMapTerrain(actual[0].bytes));
    const auto original = sourceTerrain.Snapshot();
    if (!original) Fail("TERRAIN_RESIZE_SOURCE", "实际来源地形不能完整解析。");
    if (actual.back().present) {
        const auto shadowBytes = std::size_t{original->header.width - 1} * 4 * (original->header.height - 1) * 4;
        if (actual.back().bytes.size() != shadowBytes)
            Fail("TERRAIN_RESIZE_SHADOW_SOURCE", "实际SHD长度与原始W3E尺寸不符，不能猜测旧阴影形状。");
    }
    std::vector<MapArchiveMember> spatialMembers;
    std::array<ArchiveExactMemberIdentity, kSpatialKinds.size()> current;
    for (std::size_t i = 0; i < kSpatialKinds.size(); ++i) {
        const auto& originalMember = session.original_[MapDataEditSession::Index(kSpatialKinds[i])];
        const auto& actualMember = actual[i + 2];
        if (actualMember.present != originalMember.has_value()
            || (originalMember && actualMember.bytes != *originalMember))
            Fail("TERRAIN_RESIZE_SOURCE", "会话空间成员的原始来源与实际地图不同：" + names[i + 2]);
        const auto amount = Take(session.SnapshotByteSize(kSpatialKinds[i])); reserve(amount, 32);
        if (amount > limits.maximumWork - std::min(work, limits.maximumWork))
            Fail("TERRAIN_RESIZE_WORK", "完整空间成员解析超过工作预算。");
        work += amount;
        auto snapshot = Take(session.Snapshot(kSpatialKinds[i], limits.maximumCandidateBytes - bytes + amount * 32));
        const auto& member = snapshot.Member(kSpatialKinds[i]);
        const bool present = member.state == MapDataState::Ready;
        if ((!present && member.state != MapDataState::Missing) || (present && member.sources.size() != 1))
            Fail("TERRAIN_RESIZE_SPATIAL_LAYOUT", "尺寸调整需要完整唯一的空间成员或明确缺失：" + member.memberName);
        current[i] = {member.memberName, present, present ? std::optional<std::uint64_t>{member.sources[0].data.size()} : std::nullopt, {}};
        if (present) {
            current[i].sha256 = Take(Sha256(member.sources[0].data));
            spatialMembers.push_back(member.sources[0]);
        }
    }
    auto state = std::make_shared<State>(MapDocument::FromMembers(spatialMembers));
    state->terrain = terrain; state->session = session.SessionIdentity(); state->revision = revision;
    state->sourceRevision = sourceRevision; state->current = std::move(current); state->work = work;
    if (!state->spatial.Settings() || !state->spatial.Settings()->FullyParsed()
        || state->spatial.Settings()->formatVersion != 25)
        Fail("TERRAIN_RESIZE_SETTINGS", "尺寸调整需要完整经典v25 W3I，不能用LNI或部分设置替代。");
    StartupValues startup;
    auto profiles = Take(Game::NativeDoodadCategoryDirectory::CaptureWithProfiles(gameRoot,
        Game::NativeEditorStartupRules::Classic127aExpansion, limits.maximumCandidateBytes - bytes,
        [&](std::string_view, std::span<const std::byte> input) { startup.Append(input); return Status::Ok(); }));
    reserve(profiles.CaptureBudgetBytes());
    for (const auto& value : startup.values) if (!value)
        Fail("TERRAIN_RESIZE_RULES_MISSING", "实际启动profile缺少最小/最大尺寸或相机四边首值，不能使用猜测默认值。");
    state->dimensions = {*startup.values[0], *startup.values[1], 32, 256u * 256u};
    if (!state->dimensions.minimumSide || state->dimensions.minimumSide > state->dimensions.maximumSide)
        Fail("TERRAIN_RESIZE_RULES", "实际启动profile的最小与最大地图尺寸无效。");
    for (std::size_t i = 0; i < 4; ++i) state->margins[i] = static_cast<float>(*startup.values[i + 2]);
    state->startup.emplace(std::move(profiles));
    auto wire = Take(SerializeMapTerrain(*terrain)); reserve(wire.size(), 2);
    std::vector<ArchiveExactMember> overlay;
    overlay.reserve(1 + state->current.size());
    overlay.push_back({"war3map.w3e", true, std::move(wire)});
    for (std::size_t i = 0; i < state->current.size(); ++i) {
        const auto& member = state->spatial.Member(kSpatialKinds[i]);
        overlay.push_back({member.memberName, state->current[i].present,
            state->current[i].present ? member.sources.front().data : std::vector<std::byte>{}});
    }
    const MemberReader currentReader = [&](auto requested, const auto& readLimits) -> Result<std::vector<ArchiveExactMember>> {
        auto result = Take(reader(requested, readLimits));
        if (result.size() != requested.size()) Fail("TERRAIN_RESIZE_SOURCE", "当前来源视图没有返回完整请求。");
        std::size_t total = 0;
        for (std::size_t i = 0; i < requested.size(); ++i) {
            const auto found = std::find_if(overlay.begin(), overlay.end(), [&](const auto& member) { return Key(member.name) == Key(requested[i]); });
            if (found != overlay.end()) result[i] = {requested[i], found->present, found->bytes};
            if (result[i].bytes.size() > readLimits.memberBytes || result[i].bytes.size() > readLimits.totalBytes - std::min(total, readLimits.totalBytes))
                Fail("TERRAIN_RESIZE_BUDGET", "当前来源视图超过精确读取预算。");
            total += result[i].bytes.size();
        }
        return Result<std::vector<ArchiveExactMember>>::Success(std::move(result));
    };
    const TerrainPathingResources::NativeObjectValidator validator = ObjectData::InspectNativeTerrainObjects;
    auto native = Take(TerrainPathingResources::CaptureInputs(currentReader, *terrain, *terrain, gameRoot,
        std::min<std::size_t>(64u * 1024u * 1024u, limits.maximumCandidateBytes - bytes), true, &validator, nullptr, false, true));
    reserve(native.CaptureBudgetBytes());
    if (!native.NativeCameraMargins() || *native.NativeCameraMargins() != state->margins)
        Fail("TERRAIN_RESIZE_CAMERA_SOURCE", "当前地图与实际启动profile的相机边距来源不一致，不能混用两套范围。");
    state->special = native.waterSpecialShapes_;
    for (const auto& shape : state->special) reserve(shape.relativeCliffLevels.capacity() + shape.rawcode.capacity() + 1);
    reserve(state->special.capacity(), sizeof(TerrainWaterSpecialDoodadShape));
    state->native.emplace(std::move(native));
    state->shadow = std::move(actual.back());
    std::vector<std::byte> identity; identity.reserve(kFixedBytes / 2);
    const auto number = [&](std::uint64_t value) {
        if (identity.size() > kFixedBytes / 2 - 8) Fail("TERRAIN_RESIZE_BUDGET", "尺寸来源摘要超出固定容量。");
        for (unsigned i = 0; i < 8; ++i) identity.push_back(std::byte((value >> (8 * i)) & 255));
    };
    const auto text = [&](std::string_view value) {
        number(value.size());
        if (value.size() > kFixedBytes / 2 - identity.size()) Fail("TERRAIN_RESIZE_BUDGET", "尺寸来源摘要超出固定容量。");
        const auto input = std::as_bytes(std::span(value.data(), value.size())); identity.insert(identity.end(), input.begin(), input.end());
    };
    text("terrain-resize-classic127a-expansion-v1");
    text(IO::GenericPathUtf8(state->startup->canonicalRoot_));
    for (const auto& archive : state->startup->archivePaths_) text(archive);
    for (const auto& profile : state->startup->profiles_) {
        text(profile.logicalPath); text(profile.winner); text(profile.sha256); number(profile.bytes);
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& member = i + 1 == actual.size() ? state->shadow : actual[i];
        state->physical[i] = {names[i], member.present,
            member.present ? std::optional<std::uint64_t>{member.bytes.size()} : std::nullopt,
            member.present ? std::optional<std::string>{Take(Sha256(member.bytes))} : std::nullopt};
        text(Key(names[i])); number(member.present); number(member.bytes.size());
        if (state->physical[i].sha256) text(*state->physical[i].sha256);
    }
    for (const auto& member : state->current) {
        text(Key(member.name)); number(member.present); number(member.byteSize.value_or(0)); if (member.sha256) text(*member.sha256);
    }
    text(Take(state->native->TerrainBoundsDependencyFingerprint()));
    number(state->dimensions.minimumSide); number(state->dimensions.maximumSide);
    number(state->dimensions.sideStep); number(state->dimensions.maximumArea);
    for (const auto value : state->margins) number(std::bit_cast<std::uint32_t>(value));
    state->fingerprint = Take(Sha256(identity));
    state->captureBytes = bytes;
    // 已预收的空间解析保留预算包含真实wire、解析对象和描述符；共享网格不再复制。
    state->ownedBytes = bytes;
    if (session.Revision() != revision || session.TerrainSourceRevision() != sourceRevision || session.Terrain() != terrain)
        Fail("TERRAIN_EDIT_STALE", "尺寸资源捕获期间会话已变化，未返回过期owner。");
    return Result<TerrainResizeResources>::Success(TerrainResizeResources(std::move(state)));
} catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
catch (const std::bad_alloc&) { return Result<TerrainResizeResources>::Failure(kSubsystem, "TERRAIN_RESIZE_BUDGET", "尺寸资源捕获内存不足。"); }
catch (const std::length_error&) { return Result<TerrainResizeResources>::Failure(kSubsystem, "TERRAIN_RESIZE_BUDGET", "尺寸资源捕获容量超限。"); }

Status TerrainResizeResources::VerifySources(const MemberReader& reader, const MapDataEditSession& session,
    const TerrainResizeLimits& limits) const {
    if (!state_ || !state_->startup || !state_->native || session.SessionIdentity() != state_->session
        || session.Revision() != state_->revision || session.TerrainSourceRevision() != state_->sourceRevision || session.Terrain() != state_->terrain)
        return Status::Failure(kSubsystem, "TERRAIN_EDIT_STALE", "尺寸来源复核的会话修订已变化。");
    return VerifySourceInputs(reader, limits);
}

Status TerrainResizeResources::VerifySourceInputs(const MemberReader& reader,
    const TerrainResizeLimits& limits) const try {
    Limits(limits);
    if (!state_ || !state_->startup || !state_->native)
        Fail("TERRAIN_EDIT_STALE", "尺寸来源复核缺少完整捕获状态。");
    if (state_->captureBytes > limits.maximumCandidateBytes || state_->work > limits.maximumWork)
        Fail("TERRAIN_RESIZE_BUDGET", "尺寸来源复核超过完整预算。");
    Check(state_->startup->VerifySources(limits.maximumCandidateBytes));
    std::vector<std::string> names; for (const auto name : kSourceNames) names.emplace_back(name);
    auto actual = Take(reader(names, ReadLimits(names.size(), limits.maximumCandidateBytes)));
    if (actual.size() != names.size()) Fail("TERRAIN_RESIZE_SOURCE_CHANGED", "尺寸来源复核缺少完整固定成员。");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& member = actual[i]; const auto& expected = state_->physical[i];
        if (Key(member.name) != Key(expected.name) || member.present != expected.present || (!member.present && !member.bytes.empty())
            || (member.present && (member.bytes.size() != expected.byteSize || Take(Sha256(member.bytes)) != expected.sha256)))
            Fail("TERRAIN_RESIZE_SOURCE_CHANGED", "尺寸来源的内容、存在性或身份已变化：" + names[i]);
    }
    // native owner绑定当前overlay，先检查真正磁盘地图依赖，再用当前会话覆盖
    // 几何/空间成员后复核同一次实际规则；不能把最终输出当成物理原始来源。
    const MemberReader currentReader = [&](auto requested, const auto& readLimits) -> Result<std::vector<ArchiveExactMember>> {
        auto result = Take(reader(requested, readLimits));
        if (result.size() != requested.size()) Fail("TERRAIN_RESIZE_SOURCE_CHANGED", "尺寸复核来源返回不完整。");
        std::size_t total = 0;
        for (std::size_t i = 0; i < requested.size(); ++i) {
            if (Key(requested[i]) == "war3map.w3e") result[i] = {requested[i], true, Take(SerializeMapTerrain(*state_->terrain))};
            for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) if (Key(requested[i]) == Key(state_->current[k].name)) {
                const auto& member = state_->spatial.Member(kSpatialKinds[k]);
                result[i] = {requested[i], state_->current[k].present,
                    state_->current[k].present ? member.sources.front().data : std::vector<std::byte>{}};
            }
            if ((!result[i].present && !result[i].bytes.empty()) || result[i].bytes.size() > readLimits.memberBytes
                || total > readLimits.totalBytes || result[i].bytes.size() > readLimits.totalBytes - total)
                Fail("TERRAIN_RESIZE_BUDGET", "尺寸复核当前覆盖的逐项或累计读取量超过本次限制。");
            total += result[i].bytes.size();
        }
        return Result<std::vector<ArchiveExactMember>>::Success(std::move(result));
    };
    Check(state_->native->VerifyReadbackMembers(currentReader, limits.maximumCandidateBytes, true));
    return Status::Ok();
} catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_RESIZE_BUDGET", "尺寸来源复核内存不足。"); }
catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_RESIZE_BUDGET", "尺寸来源复核容量超限。"); }

Status TerrainResizeResources::VerifySources(const MapArchiveSession& source, const MapDataEditSession& session,
    const TerrainResizeLimits& limits) const {
    return VerifySources([&](auto names, const auto& readLimits) { return source.CaptureExactMembers(names, readLimits); }, session, limits);
}
Status TerrainResizeResources::VerifySources(const WorkspaceSnapshot& source, const MapDataEditSession& session,
    const TerrainResizeLimits& limits) const {
    return VerifySources([&](auto names, const auto& readLimits) { return TerrainPathingResources::ReadDirectoryMembers(source, names, readLimits); }, session, limits);
}

Status TerrainResizeResources::VerifySourceInputs(const MapArchiveSession& source, const TerrainResizeLimits& limits) const {
    return VerifySourceInputs([&](auto names, const auto& readLimits) { return source.CaptureExactMembers(names, readLimits); }, limits);
}
Status TerrainResizeResources::VerifySourceInputs(const WorkspaceSnapshot& source, const TerrainResizeLimits& limits) const {
    return VerifySourceInputs([&](auto names, const auto& readLimits) { return TerrainPathingResources::ReadDirectoryMembers(source, names, readLimits); }, limits);
}

} // namespace War3Map::MapData
