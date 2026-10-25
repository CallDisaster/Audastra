#include "War3MapCore/MapData/MapDataEditSession.h"
#include "TerrainHistorySourceShadow.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr std::size_t BindingAllowance = 16u * 1024u;
template<class T> Result<T> Failure(const char* code, const char* message) {
    return Result<T>::Failure("Terrain.History", code, message);
}
} // namespace

bool MapDataEditSession::TerrainHistorySaveLease::NeedsSourceShadowBinding() const noexcept {
    return state_ && state_->baseline && checkpoint_
        && !state_->sourceShadow && !state_->baseline->savedResizeState_;
}

Result<std::size_t> MapDataEditSession::TerrainHistorySaveLease::SourceShadowReadBudget(
    std::size_t maximumBytes) const {
    if (!state_ || !state_->baseline || !checkpoint_ || !sessionIdentity_
        || !state_->baseline->terrainOriginal_ || sourceRevision_ == std::numeric_limits<std::uint64_t>::max()
        || state_->savedSourceRevision != sourceRevision_ + 1)
        return Failure<std::size_t>("TERRAIN_HISTORY_LEASE_INVALID", "保存前旧SHD捕获缺少真实历史lease和原始地形。");
    if (!maximumBytes || maximumBytes > TerrainJournalMaximumBytes
        || retainedBytes_ > maximumBytes || preparationBytes_ > maximumBytes)
        return Failure<std::size_t>("TERRAIN_HISTORY_BUDGET", "保存前旧SHD捕获与历史冻结的累计预算超过上限。");
    if (!NeedsSourceShadowBinding()) return Result<std::size_t>::Success(0);
    if (previousEpoch_)
        return Failure<std::size_t>("TERRAIN_HISTORY_SOURCE_SHADOW_UNAVAILABLE",
            "旧保存站未捕获最早SHD，不能用较新的物理来源冒充旧历史；地图仍可正常保存。");
    if (maximumBytes - preparationBytes_ <= BindingAllowance)
        return Failure<std::size_t>("TERRAIN_HISTORY_BUDGET", "保存前旧SHD捕获没有剩余元数据和读取预算。");
    // 原lease、两成员正文、W3E核对序列化、原字SHD驻留与小state副本同时存在。
    // 四倍读取容量预留包含writer扩容峰值；不能等分配后才发现超帽。
    const auto available = (maximumBytes - preparationBytes_ - BindingAllowance) / 4;
    if (!available)
        return Failure<std::size_t>("TERRAIN_HISTORY_BUDGET", "保存前旧SHD捕获没有剩余正文预算。");
    return Result<std::size_t>::Success(available);
}

Result<MapDataEditSession::TerrainHistorySaveLease> MapDataEditSession::TerrainHistorySaveLease::BindSourceShadow(
    std::vector<ArchiveExactMember> source, std::size_t maximumBytes) const try {
    using Output = Result<TerrainHistorySaveLease>;
    const auto budget = SourceShadowReadBudget(maximumBytes);
    if (!budget.Succeeded() || !budget.value) return {{}, budget.diagnostics};
    if (!*budget.value) return Output::Success(*this);
    if (source.size() != 2 || source[0].name != "war3map.w3e" || !source[0].present
        || source[1].name != "war3map.shd" || (!source[1].present && !source[1].bytes.empty()))
        return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_SOURCE_SHADOW_INVALID",
            "旧W3E和SHD必须来自完整、唯一的同次来源读取；缺席不能带正文。");
    std::size_t sourceCapacity = 0;
    for (const auto& member : source) {
        if (member.bytes.capacity() > *budget.value - sourceCapacity)
            return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_BUDGET", "旧W3E和SHD实际驻留容量超过预收预算。");
        sourceCapacity += member.bytes.capacity();
    }
    const auto& original = *state_->baseline->terrainOriginal_;
    // 核对writer的精确长度后才分配序列化副本。错误的小来源不得诱导大旧网格越过读取帽。
    std::size_t terrainBytes = 37;
    for (const auto* palette : {&original.header.groundTilesets, &original.header.cliffTilesets}) {
        if (terrainBytes > *budget.value || palette->size() > (*budget.value - terrainBytes) / 4)
            return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_BUDGET", "旧W3E完整序列化副本超过预收预算。");
        terrainBytes += palette->size() * 4;
    }
    if (original.vertices.size() > (*budget.value - terrainBytes) / 7)
        return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_BUDGET", "旧W3E顶点序列化副本超过预收预算。");
    terrainBytes += original.vertices.size() * 7;
    if (source[0].bytes.size() != terrainBytes)
        return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_SOURCE_MISMATCH", "实际旧W3E长度与保存站最早完整地形不同。");
    if (original.header.width < 2 || original.header.height < 2
        || std::size_t{original.header.width - 1} > std::numeric_limits<std::size_t>::max() / 16
        || std::size_t{original.header.height - 1} > std::numeric_limits<std::size_t>::max()
            / (std::size_t{original.header.width - 1} * 16))
        return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_SOURCE_SHADOW_INVALID", "旧地形尺寸不能形成完整SHD画布。");
    const auto expectedShadowBytes = std::size_t{original.header.width - 1} * 16 * (original.header.height - 1);
    if (source[1].present && source[1].bytes.size() != expectedShadowBytes)
        return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_SOURCE_SHADOW_INVALID",
            "实际旧SHD长度与旧W3E全文尺寸不符，未把损坏阴影当作缺失。");
    const auto terrain = SerializeMapTerrain(original);
    if (!terrain.Succeeded() || !terrain.value) return {{}, terrain.diagnostics};
    if (*terrain.value != source[0].bytes)
        return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_SOURCE_MISMATCH",
            "实际旧W3E全文与保存站最早原始地形不同，不能只凭相同尺寸绑定SHD。");
    // 不解析或归一SHD。非00/FF字节和明确缺失都是旧epoch重放的原始输入。
    auto state = std::make_shared<TerrainHistoryState>(*state_);
    state->sourceShadow = std::shared_ptr<const TerrainHistorySourceShadow>(
        new TerrainHistorySourceShadow(std::move(source[1])));
    const auto retained = state->sourceShadow->member_.bytes.capacity() + BindingAllowance;
    if (retained > maximumBytes - retainedBytes_)
        return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_BUDGET", "原字旧SHD与历史基线累计驻留超过预算。");
    state->retainedBytes += retained;
    TerrainHistorySaveLease bound = *this;
    bound.state_ = std::move(state); bound.retainedBytes_ += retained;
    bound.preparationBytes_ += BindingAllowance + sourceCapacity * 4;
    return Output::Success(std::move(bound));
} catch (const std::bad_alloc&) {
    return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_BUDGET", "旧SHD不可变绑定分配失败，原历史lease保持不变。");
} catch (const std::length_error&) {
    return Failure<TerrainHistorySaveLease>("TERRAIN_HISTORY_BUDGET", "旧SHD不可变绑定长度超过预算，原历史lease保持不变。");
}

} // namespace War3Map::MapData
