#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
Status Refuse(const char* code, const char* message) {
    return Status::Failure("Terrain.PathingResources", code, message);
}
}

Status TerrainPathingResources::VerifyTerrainBoundsInput(const TerrainGridSnapshot& sourceTerrain,
    const TerrainGridSnapshot& currentTerrain, std::span<const std::byte> sourceSettings) const try {
    if (!nativeBounds_ || !nativeCameraMargins_)
        return Refuse("TERRAIN_BOUNDS_RULES_UNAVAILABLE", "范围编辑需要从实际来源捕获完整原版规则。");
    const auto settings = std::find_if(dependencies_.begin(), dependencies_.end(),
        [](const auto& item) { return item.name == "war3map.w3i"; });
    const auto sourceSha = Sha256(sourceSettings);
    if (settings == dependencies_.end() || !settings->mapPresent || settings->bytes != sourceSettings.size()
        || !sourceSha.Succeeded() || !sourceSha.value || *sourceSha.value != settings->sha256)
        return Refuse("TERRAIN_BOUNDS_SOURCE_CHANGED", "范围编辑的W3I原始字节不属于实际捕获来源。");
    for (std::size_t stage = 0; stage < 2; ++stage) {
        auto wire = SerializeMapTerrain(stage ? currentTerrain : sourceTerrain);
        if (!wire.Succeeded() || !wire.value) return {wire.diagnostics};
        if (wire.value->size() > capturedMaximumBytes_)
            return Refuse("TERRAIN_BOUNDS_BUDGET", "范围编辑的W3E身份超过规则捕获预算。");
        const auto sha = Sha256(*wire.value);
        if (wire.value->size() != nativeTerrainBytes_[stage] || !sha.Succeeded() || !sha.value
            || *sha.value != nativeTerrainSha_[stage])
            return Refuse("TERRAIN_BOUNDS_SOURCE_CHANGED", "范围编辑的原始或当前W3E不属于实际捕获阶段。");
    }
    return Status::Ok();
} catch (const std::bad_alloc&) {
    return Refuse("TERRAIN_BOUNDS_BUDGET", "范围输入身份内存不足，未修改会话。");
} catch (const std::length_error&) {
    return Refuse("TERRAIN_BOUNDS_BUDGET", "范围输入身份容量超限，未修改会话。");
}

std::size_t TerrainPathingResources::TerrainBoundsDependencyBudgetBytes() const noexcept {
    if (!nativeBounds_ || !nativeCameraMargins_ || dependencies_.size() > 4096)
        return 0;
    constexpr std::size_t maximum = 1024u * 1024u;
    std::size_t budget = 8 + std::string_view("terrain-bounds-dependencies-v1").size() + 4 * 8 + 8;
    const auto add = [&](std::size_t bytes) {
        if (bytes > maximum - budget) return false;
        budget += bytes; return true;
    };
    for (const auto& sha : nativeTerrainSha_) if (!add(8) || !add(sha.size()) || !add(8)) return 0;
    for (const auto& dependency : dependencies_) {
        for (const auto* value : {&dependency.name, &dependency.sha256, &dependency.gameArchive}) {
            if (!add(8) || !add(value->size())) return 0;
        }
        if (!add(4 * 8)) return 0;
    }
    return budget;
}

Result<std::string> TerrainPathingResources::TerrainBoundsDependencyFingerprint(std::size_t maximumBytes) const try {
    const auto budget = TerrainBoundsDependencyBudgetBytes();
    if (!budget)
        return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_BOUNDS_RULES_UNAVAILABLE", "范围记录缺少实际原版规则或依赖摘要超过硬上限。");
    if (budget > maximumBytes)
        return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_BOUNDS_BUDGET", "范围规则摘要超过本次剩余预算。");
    std::vector<std::byte> wire;
    wire.reserve(budget);
    const auto number = [&](std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) wire.push_back(std::byte((value >> (i * 8)) & 255));
    };
    const auto text = [&](std::string_view value) {
        number(value.size()); const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        wire.insert(wire.end(), bytes.begin(), bytes.end());
    };
    text("terrain-bounds-dependencies-v1");
    for (std::size_t i = 0; i < 2; ++i) { text(nativeTerrainSha_[i]); number(nativeTerrainBytes_[i]); }
    for (const auto margin : *nativeCameraMargins_) number(std::bit_cast<std::uint32_t>(margin));
    number(dependencies_.size());
    for (const auto& dependency : dependencies_) {
        text(dependency.name); text(dependency.sha256); text(dependency.gameArchive); number(dependency.bytes);
        number(dependency.mapPresent); number(dependency.mapOnly); number(dependency.gameAbsent);
    }
    return Sha256(wire);
} catch (const std::bad_alloc&) {
    return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_BOUNDS_BUDGET", "范围规则摘要内存不足。");
} catch (const std::length_error&) {
    return Result<std::string>::Failure("Terrain.Journal", "TERRAIN_BOUNDS_BUDGET", "范围规则摘要容量超限。");
}

Status TerrainPathingResources::BindNativeSettingsOutput(const MapDataEditSession& edits) {
    return BindNativeSettingsOutputImpl(edits, false);
}

Status TerrainPathingResources::BindNativeResizeSettingsOutput(const MapDataEditSession& edits) {
    const auto proof = edits.VerifyTerrainResizeForSave();
    if (!proof.Succeeded()) return proof;
    return BindNativeSettingsOutputImpl(edits, true);
}

Status TerrainPathingResources::BindNativeSettingsOutputImpl(const MapDataEditSession& edits, bool resize) try {
    const auto index = MapDataEditSession::Index(MapDataKind::Settings);
    const auto& source = edits.original_[index];
    const auto& output = edits.current_[index];
    if (!source || !output || !edits.terrainOriginal_ || !edits.terrain_)
        return Refuse("TERRAIN_PATHING_RESOURCE_CHANGED", "最终W3I缺少完整会话来源。");
    const auto input = VerifyTerrainBoundsInput(*edits.terrainOriginal_,
        resize ? *edits.terrainOriginal_ : *edits.terrain_, *source);
    if (!input.Succeeded()) return input;
    if (output->size() > capturedMaximumBytes_)
        return Refuse("TERRAIN_PATHING_RESOURCE_BUDGET", "最终W3I身份超过本次捕获预算。");
    const auto invariant = edits.VerifyTerrainSettingsForSave();
    if (!invariant.Succeeded()) return invariant;
    auto settings = MapSettings::Parse(*output);
    if (!settings.Succeeded() || !settings.value) return {settings.diagnostics};
    auto after = ComputeTerrainNativePlayableBounds(edits.terrain_->header, *settings.value, *nativeCameraMargins_);
    if (!after.Succeeded() || !after.value) return {after.diagnostics};
    auto sha = Sha256(*output);
    if (!sha.Succeeded() || !sha.value) return {sha.diagnostics};
    // 所有可失败步骤完成后才安装最终阶段。四说明修改不改变范围，不要求兼容调用显式stage。
    const auto& a = *after.value;
    const auto& b = *nativeBounds_;
    if (a.left != b.left || a.bottom != b.bottom || a.right != b.right || a.top != b.top)
        nativeAfterBounds_ = a;
    else nativeAfterBounds_.reset();
    nativeSettingsOutputBytes_ = output->size();
    nativeSettingsOutputSha_ = std::move(*sha.value);
    return Status::Ok();
} catch (const std::bad_alloc&) {
    return Refuse("TERRAIN_PATHING_RESOURCE_BUDGET", "最终W3I身份内存不足。");
} catch (const std::length_error&) {
    return Refuse("TERRAIN_PATHING_RESOURCE_BUDGET", "最终W3I身份容量超限。");
}

} // namespace War3Map::MapData
