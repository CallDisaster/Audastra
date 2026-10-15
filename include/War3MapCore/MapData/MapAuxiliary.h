#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::MapData {

// W3S 的默认值是文件中的精确哨兵，不是可供计算的数值。
inline constexpr std::int32_t kSoundDefaultInteger = -1;
inline constexpr std::uint32_t kSoundDefaultRealBits = 0x4f800000u;
[[nodiscard]] inline bool IsSoundDefaultInteger(std::int32_t value) noexcept {
    return value == kSoundDefaultInteger;
}
[[nodiscard]] inline bool IsSoundDefaultReal(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value) == kSoundDefaultRealBits;
}

struct RegionRecord {
    float left;
    float bottom;
    float right;
    float top;
    std::string name;
    std::int32_t creationNumber;
    std::array<std::byte, 4> weatherId;
    std::string ambientSound;
    // 文件顺序是 B、G、R、保留字节；末字节不强制解释为 alpha。
    std::array<std::uint8_t, 4> colorBgra;
};

struct CameraRecord {
    float targetX;
    float targetY;
    float zOffset;
    float rotation;
    float angleOfAttack;
    float targetDistance;
    float roll;
    float fieldOfView;
    float farClippingPlane;
    float nearClippingPlane;
    std::string name;
    // v0 扩展模式中位于名称之前的三个额外 F32，缺失不等于默认零。
    std::optional<std::array<float, 3>> localRotation;
};

enum class CameraLayout : std::uint8_t { Classic, Extended, UnknownEmpty };

struct SoundExtendedRecord {
    std::string repeatVariableName;
    std::string soundName;
    std::string repeatSoundPath;
    std::int32_t dialogueTextKey;
    std::string unknown2;
    std::int32_t dialogueSpeakerNameKey;
    std::int32_t unknown4;
    std::optional<std::string> unknown5;
    std::string unknown6;
    std::string unknown7;
    std::string facialAnimationLabel;
    std::string facialAnimationGroupLabel;
    std::string facialAnimationSetFilepath;
    std::optional<std::int32_t> unknown11;
};

struct SoundRecord {
    std::string name;
    std::string filePath;
    std::string eaxSetting;
    std::uint32_t flags;
    std::int32_t fadeInRate;
    std::int32_t fadeOutRate;
    std::int32_t volume;
    float pitch;
    float pitchVariance;
    std::int32_t priority;
    std::int32_t channel;
    float minDistance;
    float maxDistance;
    float distanceCutoff;
    float coneAngleInside;
    float coneAngleOutside;
    std::int32_t coneOutsideVolume;
    std::array<float, 3> coneOrientation;
    // v2/v3 扩展只读；重复变量名/路径异常保留，不自动修正。
    std::optional<SoundExtendedRecord> extended;

    [[nodiscard]] bool IsLooping() const noexcept { return (flags & 1u) != 0; }
    [[nodiscard]] bool Is3D() const noexcept { return (flags & 2u) != 0; }
    [[nodiscard]] bool StopsOutOfRange() const noexcept { return (flags & 4u) != 0; }
    [[nodiscard]] bool IsMusic() const noexcept { return (flags & 8u) != 0; }
};

// 只读解析视图。原文仅供归档保留；修改 entries 不代表已经实现对应原生写入器。
struct RegionsDocument {
    std::uint32_t version = 5;
    std::vector<RegionRecord> entries;
    std::vector<std::byte> originalBytes;
    [[nodiscard]] Result<std::vector<std::byte>> Serialize() const;
};

struct CamerasDocument {
    std::uint32_t version = 0;
    std::vector<CameraRecord> entries;
    std::vector<std::byte> originalBytes;
    CameraLayout layout = CameraLayout::Classic;
    [[nodiscard]] Result<std::vector<std::byte>> Serialize() const;
};

struct SoundsDocument {
    std::uint32_t version = 1;
    std::vector<SoundRecord> entries;
    std::vector<std::byte> originalBytes;
    [[nodiscard]] Result<std::vector<std::byte>> Serialize() const;
};

struct StringTableEntry {
    std::uint32_t id;
    std::string value;
    std::size_t sourceOffset;
    // 仅从解析器取得；编辑原有值时替换正文跨度，保留周围注释与空白。
    std::size_t contentOffset = 0;
    std::size_t contentLength = 0;
};

struct StringTableDocument {
    std::vector<StringTableEntry> entries;
    std::vector<std::byte> originalBytes;
    [[nodiscard]] Result<std::vector<std::byte>> Serialize() const;
    [[nodiscard]] Result<std::uint32_t> AddString(std::string value);

    // 仅解析完整的 TRIGSTR_<十进制ID>，不替换句中内容、不递归、不猜测缺失或重复ID。
    [[nodiscard]] Result<std::string> Resolve(std::string_view value) const;
};

[[nodiscard]] Result<RegionsDocument> ParseRegions(std::span<const std::byte> bytes);
// 已知经典地图的 v0 布局。新版或来源不明时应使用 ReadCamerasVersioned。
[[nodiscard]] Result<CamerasDocument> ParseCameras(std::span<const std::byte> bytes);
// v0 双布局完整试读；扩展和无来源空表只读，双解成功且含记录时拒绝猜测。
[[nodiscard]] Result<CamerasDocument> ReadCamerasVersioned(std::span<const std::byte> bytes);
// v1 可写；v2/v3 仅完整读取、原字节保留，写出继续拒绝。
[[nodiscard]] Result<SoundsDocument> ParseSounds(std::span<const std::byte> bytes);
[[nodiscard]] Result<StringTableDocument> ParseStringTable(std::span<const std::byte> bytes);

// W3R 环境音保存的是 W3S 已有声音变量。生成初始化之前核对唯一符号，禁止把任意文字当代码。
[[nodiscard]] Status ValidateRegionAmbientSound(const RegionRecord& region, const SoundsDocument* sounds);
// 新写入的天气 ID 须能安全生成经典脚本 rawcode；空值表示无天气，原有未知值只允许保留。
[[nodiscard]] bool IsEditableRegionWeatherId(std::string_view value) noexcept;

} // namespace War3Map::MapData
