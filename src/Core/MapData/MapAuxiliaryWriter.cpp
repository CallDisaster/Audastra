#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/ClassicMapLayouts.h"
#include "MapDataWriter.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {

constexpr std::size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxRecords = 100000u;
constexpr std::size_t kMaxStringBytes = 1024u * 1024u;

template <typename Document, typename WriteRecord>
Result<std::vector<std::byte>> SerializeBinary(const Document& document,
    std::uint32_t expectedVersion, std::string_view member, WriteRecord writeRecord) {
    if (document.version != expectedVersion)
        return Result<std::vector<std::byte>>::Failure("MapData.Write", "AUX_VERSION_UNSUPPORTED",
            std::string(member) + "：不支持写出该版本");
    try {
        Detail::Writer writer(kMaxBytes);
        writer.U32(document.version);
        writer.Count(document.entries.size(), kMaxRecords);
        for (const auto& record : document.entries) writeRecord(writer, record);
        auto bytes = std::move(writer).Finish();
        // 经典二进制成员的已知字段均有明确边界；相同字节直接返回来源。
        if (bytes == document.originalBytes) return Result<std::vector<std::byte>>::Success(document.originalBytes);
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    } catch (const Detail::WriteFailure& error) {
        return Detail::Failed<std::vector<std::byte>>(member, error);
    } catch (const std::bad_alloc&) {
        return Result<std::vector<std::byte>>::Failure("MapData.Write", "AUX_ALLOCATION_FAILED",
            std::string(member) + "：无法分配输出内存");
    } catch (const std::length_error&) {
        return Result<std::vector<std::byte>>::Failure("MapData.Write", "AUX_CONTAINER_LIMIT",
            std::string(member) + "：输出超过容器容量");
    }
}

} // namespace

Result<std::vector<std::byte>> RegionsDocument::Serialize() const {
    return SerializeBinary(*this, 5, "war3map.w3r", [](Detail::Writer& writer, const RegionRecord& record) {
        writer.F32(record.left);
        writer.F32(record.bottom);
        writer.F32(record.right);
        writer.F32(record.top);
        writer.String(record.name, kMaxStringBytes);
        writer.I32(record.creationNumber);
        writer.Raw(record.weatherId, 4);
        writer.String(record.ambientSound, kMaxStringBytes);
        for (const auto value : record.colorBgra) writer.Byte(value);
    });
}

Result<std::vector<std::byte>> CamerasDocument::Serialize() const {
    if (layout == CameraLayout::UnknownEmpty && entries.empty() && version == 0
        && originalBytes.size() == 8
        && std::all_of(originalBytes.begin(), originalBytes.end(), [](std::byte value) { return value == std::byte{0}; })) {
        return Result<std::vector<std::byte>>::Success(originalBytes);
    }
    if (layout != CameraLayout::Classic) {
        return Result<std::vector<std::byte>>::Failure("MapAuxiliary", "W3C_LAYOUT_READ_ONLY",
            "扩展或空镜头表的记录模式尚无写出证明，已拒绝重建 war3map.w3c。");
    }
    if (std::any_of(entries.begin(), entries.end(), [](const CameraRecord& record) { return record.localRotation.has_value(); })) {
        return Result<std::vector<std::byte>>::Failure("MapAuxiliary", "W3C_LAYOUT_MISMATCH",
            "经典镜头文档含扩展角度，已拒绝丢弃扩展字段。");
    }
    return SerializeBinary(*this, 0, "war3map.w3c", [](Detail::Writer& writer, const CameraRecord& record) {
        writer.F32(record.targetX);
        writer.F32(record.targetY);
        writer.F32(record.zOffset);
        writer.F32(record.rotation);
        writer.F32(record.angleOfAttack);
        writer.F32(record.targetDistance);
        writer.F32(record.roll);
        writer.F32(record.fieldOfView);
        writer.F32(record.farClippingPlane);
        writer.F32(record.nearClippingPlane);
        writer.String(record.name, kMaxStringBytes);
    });
}

Result<std::vector<std::byte>> SoundsDocument::Serialize() const {
    const auto layout = ValidateClassicMapLayouts({nullptr, nullptr, nullptr, this});
    if (!layout.Succeeded()) return {{}, layout.diagnostics};
    return SerializeBinary(*this, 1, "war3map.w3s", [](Detail::Writer& writer, const SoundRecord& record) {
        writer.String(record.name, kMaxStringBytes);
        writer.String(record.filePath, kMaxStringBytes);
        writer.String(record.eaxSetting, kMaxStringBytes);
        writer.U32(record.flags);
        writer.I32(record.fadeInRate);
        writer.I32(record.fadeOutRate);
        writer.I32(record.volume);
        writer.F32(record.pitch);
        writer.F32(record.pitchVariance);
        writer.I32(record.priority);
        writer.I32(record.channel);
        writer.F32(record.minDistance);
        writer.F32(record.maxDistance);
        writer.F32(record.distanceCutoff);
        writer.F32(record.coneAngleInside);
        writer.F32(record.coneAngleOutside);
        writer.I32(record.coneOutsideVolume);
        for (const auto value : record.coneOrientation) writer.F32(value);
    });
}

} // namespace War3Map::MapData
