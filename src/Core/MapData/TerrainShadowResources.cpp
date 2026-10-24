#include "War3MapCore/MapData/TerrainShadowResources.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/TerrainShadowRebuild.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.ShadowResources";
constexpr std::size_t kOwnerBytes = 256 * 1024;

bool EqualAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto c = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (c != b[i]) return false;
    }
    return true;
}

// 409FA0保持行首section/key、不trim；40A190按CSV追加值，带引号时剥外引号。
// 这里只接收完整有限数字子集；拒绝未知数值语法，绝不选择默认方向。
Status AppendDirection(std::span<const std::byte> bytes, std::array<float, 3>& values, std::size_t& count) {
    auto text = bytes.empty() ? std::string_view{}
        : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.find('\0') != std::string_view::npos || text.starts_with("\xff\xfe") || text.starts_with("\xfe\xff"))
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_PROFILE_ENCODING", "方向profile含NUL或不支持的宽字符编码。");
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    bool light = false;
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.starts_with("//")) continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close != std::string_view::npos) light = EqualAscii(line.substr(1, close - 1), "light");
            continue;
        }
        const auto equal = line.find('=');
        if (!light || equal == std::string_view::npos || equal + 1 == line.size()
            || !EqualAscii(line.substr(0, equal), "direction")) continue;
        auto remaining = line.substr(equal + 1);
        while (!remaining.empty() && count < values.size()) {
            std::string_view token;
            if (remaining.front() == '"') {
                remaining.remove_prefix(1);
                const auto close = remaining.find('"');
                if (close == std::string_view::npos)
                    return Status::Failure(kSubsystem, "TERRAIN_SHADOW_LIGHT", "实际Direction含未闭合引号。");
                token = remaining.substr(0, close);
                remaining.remove_prefix(close + 1);
                if (!remaining.empty() && remaining.front() != ',')
                    return Status::Failure(kSubsystem, "TERRAIN_SHADOW_LIGHT", "实际Direction引号后含未知值语法。");
                if (!remaining.empty()) remaining.remove_prefix(1);
            } else {
                const auto comma = remaining.find(',');
                token = remaining.substr(0, comma);
                remaining = comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
            }
            token = Text::TrimCWhitespace(token);
            if (token.starts_with('+')) {
                token.remove_prefix(1);
                if (token.starts_with('+') || token.starts_with('-'))
                    return Status::Failure(kSubsystem, "TERRAIN_SHADOW_LIGHT", "实际Direction含未知重复符号。");
            }
            float number = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
            if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(number))
                return Status::Failure(kSubsystem, "TERRAIN_SHADOW_LIGHT", "实际Direction前三value不是完整有限数值。");
            values[count++] = number;
        }
    }
    return Status::Ok();
}
} // namespace

Result<TerrainShadowResources> TerrainShadowResources::Capture(const std::filesystem::path& gameRoot,
    Game::NativeEditorStartupRules rules, std::size_t maximumBytes) try {
    using Output = Result<TerrainShadowResources>;
    if (maximumBytes < kOwnerBytes)
        return Output::Failure(kSubsystem, "TERRAIN_SHADOW_BUDGET", "阴影方向来源缺少完整捕获与摘要工作预算。");
    TerrainShadowResources result;
    std::array<float, 3> editorDirection{};
    std::size_t count = 0;
    auto captured = Game::NativeDoodadCategoryDirectory::CaptureWithProfiles(gameRoot, rules, maximumBytes - kOwnerBytes,
        [&](std::string_view, std::span<const std::byte> bytes) { return AppendDirection(bytes, editorDirection, count); });
    if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
    if (count != editorDirection.size())
        return Output::Failure(kSubsystem, "TERRAIN_SHADOW_LIGHT", "完整经典profile追加链未提供Direction前三value，不能猜默认方向。");
    auto direction = PrepareTerrainShadowLightDirection(editorDirection);
    if (!direction.Succeeded() || !direction.value) return {{}, std::move(direction.diagnostics)};
    result.lightDirection_ = *direction.value;

    // 有界长度前缀摘要包括实际来源winner、known absence与正文SHA；不是路径猜测。
    std::vector<std::byte> wire;
    wire.reserve(kOwnerBytes / 2);
    const auto number = [&wire](std::uint64_t value) {
        if (wire.size() > kOwnerBytes / 2 - 8) throw std::length_error("shadow profile identity");
        for (unsigned i = 0; i < 8; ++i) wire.push_back(std::byte((value >> (8 * i)) & 255));
    };
    const auto field = [&wire, &number](std::string_view value) {
        if (wire.size() > kOwnerBytes / 2 - 8 || value.size() > kOwnerBytes / 2 - wire.size() - 8)
            throw std::length_error("shadow profile identity");
        number(value.size());
        const auto raw = std::as_bytes(std::span(value.data(), value.size()));
        wire.insert(wire.end(), raw.begin(), raw.end());
    };
    field("terrain-shadow-fresh-classic-profile-v1");
    number(static_cast<std::uint8_t>(rules));
    field(IO::GenericPathUtf8(captured.value->canonicalRoot_));
    number(captured.value->archivePaths_.size());
    for (const auto& archive : captured.value->archivePaths_) field(archive);
    number(captured.value->profiles_.size());
    for (const auto& profile : captured.value->profiles_) {
        field(profile.logicalPath); field(profile.winner); field(profile.sha256); number(profile.bytes);
    }
    auto fingerprint = Sha256(wire);
    if (!fingerprint.Succeeded() || !fingerprint.value) return {{}, std::move(fingerprint.diagnostics)};
    result.fingerprint_ = std::move(*fingerprint.value);
    result.sourceBytes_ = captured.value->SourceBytes();
    result.captureBudgetBytes_ = captured.value->CaptureBudgetBytes() + kOwnerBytes;
    result.profileOwner_.emplace(std::move(*captured.value));
    return Output::Success(std::move(result));
} catch (const std::bad_alloc&) {
    return Result<TerrainShadowResources>::Failure(kSubsystem, "TERRAIN_SHADOW_BUDGET", "阴影方向来源分配失败，未返回部分owner。");
} catch (const std::length_error&) {
    return Result<TerrainShadowResources>::Failure(kSubsystem, "TERRAIN_SHADOW_BUDGET", "阴影方向来源摘要超过有界容量。");
}

Status TerrainShadowResources::VerifySources(std::size_t maximumBytes) const {
    if (!profileOwner_ || fingerprint_.empty())
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_SOURCE", "阴影方向没有完整实际来源owner。");
    return profileOwner_->VerifySources(maximumBytes);
}

} // namespace War3Map::MapData
