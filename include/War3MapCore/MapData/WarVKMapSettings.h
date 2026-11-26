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
    // 昼夜光照循环的两个开关（lightingCycle.setCelestialMotionEnabled / setTimeColorGradingEnabled）；空为不设置，
    // 保持 WarVK 默认。固定太阳需要两个都关。
    std::optional<bool> celestialMotion;
    std::optional<bool> timeColorGrading;
    [[nodiscard]] bool HasCommands() const noexcept {
        return cameraLimits.has_value() || !farDoodadTypes.empty() || !skyModel.empty() || sunDirection.has_value()
            || sunColor.has_value() || celestialMotion.has_value() || timeColorGrading.has_value();
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
// 按执行顺序给出 Preloader 参数文本（不含引号）：镜头限制、远景类型、两个光照循环开关、太阳方向、太阳颜色强度。
// 设置无效时报错，不输出部分指令。
[[nodiscard]] Result<std::vector<std::string>> WarVKInitCommands(const WarVKMapSettings& settings);
// GetLocalizedHotkey 查询串：协议版本（WarVK 接管时非零，未接管为 0，先读它再看各条结果才有意义）、
// 最近一次指令的错误码（0 为接受）。
inline constexpr std::string_view kWarVKProtocolQuery = "warvk:v1;system.protocolVersion";
inline constexpr std::string_view kWarVKLastErrorQuery = "warvk:v1;system.lastErrorCode";

} // namespace War3Map::MapData
