#include "War3MapCore/MapData/TerrainRowScan.h"
#include "MapTerrainDecode.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace War3Map::MapData {
namespace {
constexpr char kSubsystem[] = "MapTerrainRows";
constexpr std::size_t kRows = 7, kRadius = 3, kScratch = 64u * 1024u;

Status BudgetFailure() { return Status::Failure(kSubsystem, "TERRAIN_ROW_WORK_BUDGET", "整次行扫描的累计访问工作超过预算，未按行重置计数。"); }

bool Valid(const TerrainRowScanLimits& limits) {
    const TerrainRowScanLimits hard;
    return limits.maxInputBytes && limits.maxInputBytes <= hard.maxInputBytes
        && limits.maxVertexDecodes && limits.maxVertexDecodes <= hard.maxVertexDecodes
        && limits.maxTileAnchors && limits.maxTileAnchors <= hard.maxTileAnchors
        && limits.maxWorkUnits && limits.maxWorkUnits <= hard.maxWorkUnits
        && limits.maxResidentBytes && limits.maxResidentBytes <= hard.maxResidentBytes;
}

std::size_t HeaderCapacity(const TerrainHeader& header) {
    std::size_t bytes = sizeof(header);
    for (const auto* table : {&header.groundTilesets, &header.cliffTilesets}) {
        bytes += table->capacity() * sizeof(std::string);
        for (const auto& text : *table) bytes += text.capacity() + 1;
    }
    return bytes;
}

struct ScanFailure { std::vector<Diagnostic> diagnostics; };
void Check(Status status) { if (!status.Succeeded()) throw ScanFailure{std::move(status.diagnostics)}; }
}

Status TerrainRowScanBudget::ChargeConsumerWork(std::size_t units) {
    if (units > maximum_ - facts_.workUnits) return BudgetFailure();
    facts_.workUnits += units; facts_.consumerWorkUnits += units;
    return Status::Ok();
}

Status ValidateTerrainRowScanLimits(const TerrainRowScanLimits& limits) {
    return Valid(limits) ? Status::Ok()
        : Status::Failure(kSubsystem, "TERRAIN_ROW_LIMITS", "行扫描各项非零预算只能收紧既有硬上限。");
}

Result<TerrainRowSource> OpenTerrainRowSource(std::span<const std::byte> bytes, const TerrainRowScanLimits& limits) {
    TerrainDecode::Reader reader(bytes);
    try {
        const auto checked = ValidateTerrainRowScanLimits(limits);
        if (!checked.Succeeded()) return {std::nullopt, checked.diagnostics};
        TerrainReadLimits read;
        read.maxBytes = limits.maxInputBytes; read.maxVertices = limits.maxVertexDecodes;
        read.maxTiles = limits.maxTileAnchors; read.maxDecodedBytes = limits.maxResidentBytes;
        if (bytes.size() > read.maxBytes) reader.Fail("TERRAIN_BYTE_LIMIT", "地形成员超过行扫描输入字节预算");
        std::size_t metadata = sizeof(TerrainRowSource);
        auto layout = TerrainDecode::ReadLayout(reader, read, metadata);
        std::size_t resident = kScratch + sizeof(TerrainRowSource) + sizeof(TerrainGridSnapshot)
            + 2 * HeaderCapacity(layout.header);
        const auto rowCount = (std::min)(kRows, static_cast<std::size_t>(layout.header.height));
        const auto vertices = static_cast<std::size_t>(layout.header.width) * rowCount;
        if (resident > limits.maxResidentBytes || vertices > (limits.maxResidentBytes - resident) / sizeof(TerrainVertex))
            return Result<TerrainRowSource>::Failure(kSubsystem, "TERRAIN_ROW_RESIDENT_BUDGET", "最多七行的顶点窗口与格式元数据超过驻留预算。");
        resident += vertices * sizeof(TerrainVertex);
        // 最小完整工作量在任何顶点解码前预收；后续移位与消费者邻域查询继续用同一账。
        if (layout.vertexCount > limits.maxWorkUnits || layout.tileCount > limits.maxWorkUnits - layout.vertexCount)
            return Result<TerrainRowSource>::Failure(kSubsystem, "TERRAIN_ROW_WORK_BUDGET", "完整输入顶点与地格最小累计工作已超过预算。");
        TerrainRowSource result;
        result.bytes_ = bytes; result.header_ = std::move(layout.header); result.limits_ = limits; result.readLimits_ = read;
        result.recordsOffset_ = reader.Offset(); result.vertexCount_ = layout.vertexCount; result.tileCount_ = layout.tileCount;
        result.residentBytes_ = resident;
        return Result<TerrainRowSource>::Success(std::move(result));
    } catch (const TerrainDecode::TerrainError& error) {
        return {std::nullopt, {error.diagnostic}};
    } catch (const std::bad_alloc&) {
        return Result<TerrainRowSource>::Failure(kSubsystem, "TERRAIN_ROW_ALLOCATION", "无法分配行扫描格式元数据。");
    } catch (const std::length_error&) {
        return Result<TerrainRowSource>::Failure(kSubsystem, "TERRAIN_ROW_ALLOCATION", "行扫描格式元数据容量异常。");
    }
}

Result<TerrainRowScanFacts> ScanTerrainRows(const TerrainRowSource& source,
    const std::function<Status(const TerrainGridSnapshot&, std::uint32_t, std::uint32_t, TerrainRowScanBudget&)>& visitor) {
    try {
        if (!visitor || !source.vertexCount_ || source.recordsOffset_ > source.bytes_.size())
            return Result<TerrainRowScanFacts>::Failure(kSubsystem, "TERRAIN_ROW_SOURCE", "行扫描需要工厂建立的完整原字布局与访问者。");
        TerrainRowScanBudget budget(source.limits_.maxWorkUnits);
        budget.facts_.inputBytes = source.bytes_.size();
        TerrainGridSnapshot window;
        window.header = source.header_;
        const auto width = source.header_.width, height = source.header_.height;
        const auto rows = (std::min)(kRows, static_cast<std::size_t>(height));
        window.vertices.reserve(static_cast<std::size_t>(width) * rows);
        const auto resident = kScratch + sizeof(source) + sizeof(window) + HeaderCapacity(source.header_)
            + HeaderCapacity(window.header) + window.vertices.capacity() * sizeof(TerrainVertex);
        if (resident > source.limits_.maxResidentBytes)
            return Result<TerrainRowScanFacts>::Failure(kSubsystem, "TERRAIN_ROW_RESIDENT_BUDGET", "实际顶点窗口capacity超过驻留预算。");
        budget.facts_.maximumResidentBytes = resident;
        TerrainDecode::Reader reader(source.bytes_, source.recordsOffset_);
        std::uint32_t first = 0, next = 0;
        const auto charge = [&](std::size_t units) {
            if (units > source.limits_.maxWorkUnits - budget.facts_.workUnits) Check(BudgetFailure());
            budget.facts_.workUnits += units;
        };
        const auto emit = [&](std::uint32_t y) {
            charge(width - 1); budget.facts_.tileAnchors += width - 1;
            Check(visitor(window, first, y, budget));
        };
        for (std::uint32_t y = 0; y < height; ++y) {
            if (window.vertices.size() == width * kRows) {
                const auto moved = window.vertices.size() - width;
                charge(moved); budget.facts_.rowVertexMoves += moved;
                std::move(window.vertices.begin() + width, window.vertices.end(), window.vertices.begin());
                window.vertices.resize(moved); ++first;
            }
            charge(width); budget.facts_.vertexDecodes += width;
            for (std::uint32_t x = 0; x < width; ++x)
                window.vertices.push_back(TerrainDecode::ReadVertex(reader, source.header_, x, y, source.readLimits_));
            window.header.height = static_cast<std::uint32_t>(window.vertices.size() / width);
            budget.facts_.maximumResidentRows = (std::max)(budget.facts_.maximumResidentRows,
                static_cast<std::size_t>(window.header.height));
            if (y >= kRadius && next + 1 < height) emit(next++);
        }
        while (next + 1 < height) emit(next++);
        if (reader.Remaining() || budget.facts_.vertexDecodes != source.vertexCount_ || budget.facts_.tileAnchors != source.tileCount_)
            return Result<TerrainRowScanFacts>::Failure(kSubsystem, "TERRAIN_ROW_INCOMPLETE", "扫描没有消费完整原字或全部地格，拒绝部分成功。");
        return Result<TerrainRowScanFacts>::Success(budget.facts_);
    } catch (const ScanFailure& failure) {
        return {std::nullopt, failure.diagnostics};
    } catch (const TerrainDecode::TerrainError& error) {
        return {std::nullopt, {error.diagnostic}};
    } catch (const std::bad_alloc&) {
        return Result<TerrainRowScanFacts>::Failure(kSubsystem, "TERRAIN_ROW_ALLOCATION", "顶点窗口分配失败，不返回部分快照。");
    } catch (const std::length_error&) {
        return Result<TerrainRowScanFacts>::Failure(kSubsystem, "TERRAIN_ROW_ALLOCATION", "顶点窗口容量异常，不返回部分快照。");
    }
}

} // namespace War3Map::MapData
