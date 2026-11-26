#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::MapData {

// 编辑器自有的地图成员：WarVK 渲染设置。游戏不读取它；保存时由脚本生成写出受管的初始化指令。
inline constexpr std::string_view kWarVKSettingsMember = "audastra\\warvk.ini";
inline constexpr std::size_t kWarVKSettingsMaxBytes = 4096;
inline constexpr std::size_t kWarVKMaxFarDoodadTypes = 16;
// camera.setLimits：镜头远距上限（原版镜头字段 0/1 的上限）与近裁剪面（字段 7）。
inline constexpr double kWarVKCameraMaxDistanceMin = 10000, kWarVKCameraMaxDistanceMax = 400000;
inline constexpr double kWarVKCameraNearClipMin = 1, kWarVKCameraNearClipMax = 100;
// 天空模型是地图内或游戏路径，按 MPQ 一般上限：含结尾 0 不超过 260 字节；超出直接拒绝，不截断。
// （1.27a 的 53 字节只限制地图文件自己的部署路径，不管地图内成员路径。）
inline constexpr std::size_t kWarVKSkyModelMaxBytes = 259;
// sun.setDirection：方位角与单位朝向同一约定（0° 东、90° 北，逆时针），取 [0, 360)；仰角 -90°–90°。
inline constexpr double kWarVKSunAzimuthMax = 360, kWarVKSunElevationMin = -90, kWarVKSunElevationMax = 90;
// sun.setColorIntensity：颜色各分量 0–1，强度 0–10（WarVK 只要求非负；上限防止误填）。
inline constexpr double kWarVKSunIntensityMax = 10;
// localFog.createBox：坐标绝对值上限；云海盒的水平全尺寸取地图宽高的 1.5 倍并限在 4096–200000，
// 盒底在地图最低地面以下 100，羽化固定 0.15，覆盖度即雾盒密度（云海只用 0–1）。
inline constexpr double kWarVKCoordinateMax = 1000000;
inline constexpr double kWarVKCloudSeaSizeMin = 4096, kWarVKCloudSeaSizeMax = 200000, kWarVKCloudSeaSizeScale = 1.5;
inline constexpr double kWarVKCloudSeaGroundMargin = 100, kWarVKCloudSeaFeather = 0.15;

struct WarVKCameraLimits {
    double maxDistance = 0;
    double nearClip = 0;
    bool operator==(const WarVKCameraLimits&) const = default;
};

// 太阳方向（度）。写出的是光线方向（从太阳射向地面）：(-cos仰角·cos方位, -cos仰角·sin方位, -sin仰角)，东为 +X、北为 +Y。
struct WarVKSunDirection {
    double azimuth = 0;
    double elevation = 0;
    bool operator==(const WarVKSunDirection&) const = default;
};

struct WarVKSunColor {
    double red = 1, green = 1, blue = 1;
    double intensity = 1;
    bool operator==(const WarVKSunColor&) const = default;
};

// 云海：玩家开着物理天空与云层、地图未开体积光时，WarVK 把地图里最大的扁平局部雾盒画成云海。
// 盒顶为顶高（世界 z），密度为覆盖度；玩家未开云层时该雾盒不显示、无其他作用。
struct WarVKCloudSea {
    double top = 0;
    double cover = 0;
    bool operator==(const WarVKCloudSea&) const = default;
};

// 地图地面范围（W3E 原始层高网格）：西南角与东北角的世界坐标、最低地面 z。
struct WarVKMapGround {
    double west = 0, south = 0, east = 0, north = 0;
    double lowest = 0;
};

// 写给 localFog.createBox 的云海盒：中心、全尺寸、密度与羽化，不旋转。
struct WarVKCloudSeaBox {
    double centerX = 0, centerY = 0, centerZ = 0;
    double sizeX = 0, sizeY = 0, sizeZ = 0;
    double bottom = 0;
    double cover = 0, feather = kWarVKCloudSeaFeather;
};

// 受管指令的执行时机：默认在地图初始化完成后的 0 秒计时器里（原版镜头控制器已建立）；也可放在 main 开头。
enum class WarVKInitTiming { AfterInitialization, MainStart };

struct WarVKMapSettings {
    WarVKInitTiming timing = WarVKInitTiming::AfterInitialization;
    std::optional<WarVKCameraLimits> cameraLimits;
    // 标为远景的装饰物类型（四字符物编 ID），按字节序排序且不重复；写出时合成一行。
    std::vector<std::string> farDoodadTypes;
    // 天空模型（地图内或游戏路径，反斜杠分隔，如 Sonne\Sky\SonneDawnM11.mdx）；空为不设置。
    // 受管函数在各条指令之后调用 SetSkyModel。
    std::string skyModel;
    std::optional<WarVKSunDirection> sunDirection;
    std::optional<WarVKSunColor> sunColor;
    // 昼夜光照循环的两个开关（lightingCycle.setCelestialMotionEnabled / setTimeColorGradingEnabled）；空为不设置。
    // 天体运行开着时日夜循环每帧覆盖太阳方向，按时间调色开着时每帧覆盖太阳颜色；按时间调色同时开关整个昼夜系统。
    // 实际写出值见 WarVKEffectiveCelestialMotion / WarVKEffectiveTimeColorGrading。
    std::optional<bool> celestialMotion;
    std::optional<bool> timeColorGrading;
    std::optional<WarVKCloudSea> cloudSea;
    [[nodiscard]] bool HasCommands() const noexcept {
        return cameraLimits.has_value() || !farDoodadTypes.empty() || !skyModel.empty() || sunDirection.has_value()
            || sunColor.has_value() || celestialMotion.has_value() || timeColorGrading.has_value() || cloudSea.has_value();
    }
    bool operator==(const WarVKMapSettings&) const = default;
};

// 严格读取：schema=1，只接受已知键、不接受重复键与空行以外的杂项；超出范围、非法 ID、超过 16 个远景类型均报错。
[[nodiscard]] Result<WarVKMapSettings> ParseWarVKMapSettings(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>> SerializeWarVKMapSettings(const WarVKMapSettings& settings);
[[nodiscard]] Status ValidateWarVKMapSettings(const WarVKMapSettings& settings);
// 远景类型集合改动（标记/取消）：返回排序去重后的新集合；第 17 个类型拒绝并说明，不截断。
[[nodiscard]] Result<std::vector<std::string>> SetWarVKFarDoodadType(std::vector<std::string> types,
    std::string_view rawcode, bool far);
// 光照循环开关的实际写出值：设了太阳方向而天体运行不设置时按关写出（否则方向在游戏里看不出效果），
// 设了太阳颜色而按时间调色不设置时同样按关写出；明确选的值照写。
[[nodiscard]] std::optional<bool> WarVKEffectiveCelestialMotion(const WarVKMapSettings& settings) noexcept;
[[nodiscard]] std::optional<bool> WarVKEffectiveTimeColorGrading(const WarVKMapSettings& settings) noexcept;
// 不阻止保存的提醒：明确开着天体运行时太阳方向被覆盖（WARVK_SUN_DIRECTION_OVERRIDDEN），
// 明确开着按时间调色时太阳颜色被覆盖（WARVK_SUN_COLOR_OVERRIDDEN）。
[[nodiscard]] std::vector<Diagnostic> WarVKSettingsWarnings(const WarVKMapSettings& settings);
// 按执行顺序给出 Preloader 参数文本（不含引号）：镜头限制、远景类型、两个光照循环开关（实际写出值）、太阳方向、
// 太阳颜色强度。云海不在其中：它是返回 id 的 GetLocalizedHotkey 查询，见 WarVKCloudSeaQuery。
// 设置无效时报错，不输出部分指令。
[[nodiscard]] Result<std::vector<std::string>> WarVKInitCommands(const WarVKMapSettings& settings);
// 由地形网格取地面范围；网格须至少 2×2 顶点。
struct TerrainGridSnapshot;
[[nodiscard]] Result<WarVKMapGround> WarVKMapGroundOf(const TerrainGridSnapshot& terrain);
// 云海盒：中心在地图中心，水平全尺寸为地图宽高的 1.5 倍（4096–200000），盒底为最低地面以下 100，
// 盒顶为顶高。顶高不高于盒底、盒高超过水平全尺寸较小者或坐标越界时拒绝并说明，不改成边界值。
[[nodiscard]] Result<WarVKCloudSeaBox> WarVKCloudSeaBoxFor(const WarVKCloudSea& sea, const WarVKMapGround& ground);
// localFog.createBox 查询串（不含引号），GetLocalizedHotkey 返回雾盒 id。
[[nodiscard]] std::string WarVKCloudSeaQuery(const WarVKCloudSeaBox& box);
// GetLocalizedHotkey 查询串：协议版本（WarVK 接管时非零，未接管为 0，先读它再看各条结果才有意义）、
// 最近一次指令的错误码（0 为接受）。
inline constexpr std::string_view kWarVKProtocolQuery = "warvk:v1;system.protocolVersion";
inline constexpr std::string_view kWarVKLastErrorQuery = "warvk:v1;system.lastErrorCode";

} // namespace War3Map::MapData
