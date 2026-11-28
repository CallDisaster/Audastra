#pragma once

#include "War3MapCore/Render/SurfaceMaterialImage.h"
#include "War3MapCore/Services/SurfaceMaterialTableDocument.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace War3Map::Services {
// 材质表校验：界面、MCP 与保存前检查共用。诊断码稳定，文字为中文；“图片不存在”“格式游戏读不了”
// “任一边超过 8192”是错误，其余是提醒或信息。不猜法线是 gl 还是 dx。

// 一张图的来源：Found 带字节；Missing 表示既不在地图里也不在待导入文件里；Failed 带原因。
struct SurfaceMaterialImageSource {
    enum class State : std::uint8_t { Found, Missing, Failed };
    State state = State::Missing;
    std::shared_ptr<const std::vector<std::byte>> bytes;
    std::vector<Diagnostic> problems;
};

// 一张图本身的检查（格式、尺寸、通道统计与按键的提醒），与路径和引用它的类型无关。
struct SurfaceMaterialImageInspection {
    std::string sha256;
    std::size_t bytes = 0;
    Render::SurfaceMaterialImageHeader header;
    std::optional<Render::SurfaceMaterialPixelStats> stats; // 编辑器能解且在统计上限内时
    std::vector<Diagnostic> diagnostics;                     // 文字不含路径
};
// key 为图片键（normal、orm、detailNormal、detailOrm、detailAlbedo）。
[[nodiscard]] SurfaceMaterialImageInspection InspectSurfaceMaterialImage(std::string_view key, std::string_view path,
    std::span<const std::byte> bytes);

// 按“内容哈希 + 键”缓存图片检查；换了图哈希就变，旧项不再命中。线程安全，超出容量时丢最久未用的项。
class SurfaceMaterialImageCache {
public:
    explicit SurfaceMaterialImageCache(std::size_t capacity = 256) : capacity_(capacity ? capacity : 1) {}
    [[nodiscard]] std::shared_ptr<const SurfaceMaterialImageInspection> Inspect(std::string_view key, std::string_view path,
        std::span<const std::byte> bytes);
    void Clear();

private:
    using Order = std::list<std::string>;
    struct Entry { std::shared_ptr<const SurfaceMaterialImageInspection> value; Order::iterator order; };
    std::mutex mutex_;
    std::size_t capacity_;
    Order order_;
    std::unordered_map<std::string, Entry> entries_;
};

enum class SurfaceMaterialTypeCategory : std::uint8_t { Unknown, Doodad, Destructible, Ambiguous };
struct SurfaceMaterialTypeInfo {
    SurfaceMaterialTypeCategory category = SurfaceMaterialTypeCategory::Unknown;
    std::string name;
    std::size_t placements = 0;     // war3map.doo（含未保存的摆放修改）里这一类型的摆放数
    std::optional<bool> blendLayer; // 模型有 Blend 层；没读模型时为空
};

struct SurfaceMaterialValidationSources {
    std::function<SurfaceMaterialImageSource(std::string_view path)> readImage; // 为空时不查图片
    std::function<SurfaceMaterialTypeInfo(std::string_view rawcode)> typeInfo;  // 为空时不查类型
    SurfaceMaterialImageCache* cache = nullptr;
};

struct SurfaceMaterialImageReport {
    std::string key, path;
    SurfaceMaterialImageSource::State state = SurfaceMaterialImageSource::State::Missing;
    std::shared_ptr<const SurfaceMaterialImageInspection> inspection; // Found 时
};
struct SurfaceMaterialTypeValidation {
    std::string rawcode;
    std::optional<SurfaceMaterialTypeInfo> type; // 查了类型时
    std::vector<SurfaceMaterialImageReport> images;
    std::vector<Diagnostic> diagnostics; // 本类型的类型级与图片级诊断，文字带类型与路径
};
struct SurfaceMaterialValidation {
    std::vector<Diagnostic> table; // 表级：坏行、未知键、同名节、超过 256 种、表面类型没有图片
    std::vector<SurfaceMaterialTypeValidation> types;
    [[nodiscard]] std::size_t Count(DiagnosticSeverity severity) const;
};
// rawcode 为空时校验整表；给出时只校验这一类型（表级诊断仍全部给出）。
[[nodiscard]] SurfaceMaterialValidation ValidateSurfaceMaterials(const SurfaceMaterialTableDocument& document,
    std::string_view rawcode, const SurfaceMaterialValidationSources& sources);
}
