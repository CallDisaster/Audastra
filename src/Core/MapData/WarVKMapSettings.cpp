#include "War3MapCore/MapData/WarVKMapSettings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace War3Map::MapData {
namespace {
constexpr std::string_view kDomain = "MapData.WarVK";

Status Fail(const char* code, std::string message) { return Status::Failure(std::string(kDomain), code, std::move(message)); }

// 物编 ID：恰好四个可打印 ASCII 字符，且不含指令分隔用的 , ; : 与引号、反斜杠。
bool ValidRawcode(std::string_view code) {
    return code.size() == 4 && std::all_of(code.begin(), code.end(), [](char c) {
        return c > 0x20 && c < 0x7f && c != ',' && c != ';' && c != ':' && c != '"' && c != '\\' && c != '=';
    });
}

// 天空模型：相对路径，以 .mdx 或 .mdl 结尾；不含控制字符、引号、盘符冒号、正斜杠，不以分隔符或空格开头/结尾。
bool ValidSkyModel(std::string_view path) {
    if (path.size() < 5 || path.front() == '\\' || path.front() == ' ' || path.back() == ' ') return false;
    if (std::any_of(path.begin(), path.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7f || c == '"' || c == ':' || c == '/';
    })) return false;
    std::string extension(path.substr(path.size() - 4));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return extension == ".mdx" || extension == ".mdl";
}

// 最短往返的定点十进制（300000、4、0.5），不用指数写法。
std::string Number(double value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    return written.ec == std::errc{} ? std::string(buffer, written.ptr) : std::string{};
}

bool ParseNumber(std::string_view text, double& value) {
    if (text.empty() || text.size() > 32) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}

// 能按 Number 写出且读回（ParseNumber 只收 32 字符以内）。
bool Writable(double value) {
    const auto text = Number(value);
    return !text.empty() && text.size() <= 32;
}

bool InRange(double value, double low, double high) { return std::isfinite(value) && value >= low && value <= high; }

// 方向分量：6 位小数、去掉末尾 0，负零写成 0（-0.994522、0、-0.104528）。
std::string Component(double value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6);
    if (written.ec != std::errc{}) return {};
    std::string text(buffer, written.ptr);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text == "-0" ? std::string("0") : text;
}

std::optional<bool> ParseSwitch(std::string_view value) {
    if (value == "1") return true;
    if (value == "0") return false;
    return std::nullopt;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        const auto end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) return parts;
        start = end + 1;
    }
}

bool ParseNumbers(std::string_view value, std::span<double> output) {
    const auto parts = Split(value, ',');
    if (parts.size() != output.size()) return false;
    for (std::size_t i = 0; i < parts.size(); ++i) if (!ParseNumber(parts[i], output[i])) return false;
    return true;
}

constexpr double kDegrees = 3.14159265358979323846 / 180.0;
} // namespace

Status ValidateWarVKMapSettings(const WarVKMapSettings& settings) {
    if (settings.cameraLimits) {
        const auto& limits = *settings.cameraLimits;
        if (!std::isfinite(limits.maxDistance) || limits.maxDistance < kWarVKCameraMaxDistanceMin
            || limits.maxDistance > kWarVKCameraMaxDistanceMax)
            return Fail("WARVK_CAMERA_DISTANCE_RANGE", "镜头远距上限须在 10000–400000 之间。");
        if (!std::isfinite(limits.nearClip) || limits.nearClip < kWarVKCameraNearClipMin || limits.nearClip > kWarVKCameraNearClipMax)
            return Fail("WARVK_CAMERA_NEAR_RANGE", "近裁剪须在 1–100 之间。");
        if (Number(limits.maxDistance).empty() || Number(limits.nearClip).empty())
            return Fail("WARVK_CAMERA_NUMBER", "镜头限制无法写成十进制数。");
    }
    if (settings.farDoodadTypes.size() > kWarVKMaxFarDoodadTypes)
        return Fail("WARVK_FAR_TYPES_LIMIT", "远景装饰物类型最多 16 个，当前 " + std::to_string(settings.farDoodadTypes.size())
            + " 个；请先取消多余的标记。");
    for (std::size_t i = 0; i < settings.farDoodadTypes.size(); ++i) {
        const auto& code = settings.farDoodadTypes[i];
        if (!ValidRawcode(code)) return Fail("WARVK_FAR_TYPE_INVALID", "远景装饰物类型“" + code + "”不是四字符物编 ID。");
        if (i && settings.farDoodadTypes[i - 1] >= code)
            return Fail("WARVK_FAR_TYPE_ORDER", "远景装饰物类型须排序且不重复（“" + code + "”）。");
    }
    if (!settings.skyModel.empty()) {
        if (settings.skyModel.size() > kWarVKSkyModelMaxBytes)
            return Fail("WARVK_SKY_MODEL_LENGTH", "天空模型路径 " + std::to_string(settings.skyModel.size())
                + " 字节，超过 MPQ 路径上限（含结尾 0 共 260 字节）；请缩短路径。");
        if (!ValidSkyModel(settings.skyModel))
            return Fail("WARVK_SKY_MODEL_INVALID", "天空模型路径“" + settings.skyModel
                + "”须是以 .mdx 或 .mdl 结尾的相对路径（反斜杠分隔），不含引号、冒号或控制字符。");
    }
    if (settings.sunDirection) {
        const auto& sun = *settings.sunDirection;
        if (!std::isfinite(sun.azimuth) || sun.azimuth < 0 || sun.azimuth >= kWarVKSunAzimuthMax)
            return Fail("WARVK_SUN_AZIMUTH_RANGE", "太阳方位角须在 0–360° 之间（不含 360）。");
        if (!InRange(sun.elevation, kWarVKSunElevationMin, kWarVKSunElevationMax))
            return Fail("WARVK_SUN_ELEVATION_RANGE", "太阳仰角须在 -90–90° 之间。");
        if (!Writable(sun.azimuth) || !Writable(sun.elevation)) return Fail("WARVK_SUN_NUMBER", "太阳方向无法写成十进制数。");
    }
    if (settings.sunColor) {
        const auto& sun = *settings.sunColor;
        if (!InRange(sun.red, 0, 1) || !InRange(sun.green, 0, 1) || !InRange(sun.blue, 0, 1))
            return Fail("WARVK_SUN_COLOR_RANGE", "太阳颜色各分量须在 0–1 之间。");
        if (!InRange(sun.intensity, 0, kWarVKSunIntensityMax)) return Fail("WARVK_SUN_INTENSITY_RANGE", "太阳强度须在 0–10 之间。");
        if (!Writable(sun.red) || !Writable(sun.green) || !Writable(sun.blue) || !Writable(sun.intensity))
            return Fail("WARVK_SUN_NUMBER", "太阳颜色或强度无法写成十进制数。");
    }
    if (settings.timing != WarVKInitTiming::AfterInitialization && settings.timing != WarVKInitTiming::MainStart)
        return Fail("WARVK_TIMING_INVALID", "未知的 WarVK 指令执行时机。");
    return Status::Ok();
}

Result<std::vector<std::string>> SetWarVKFarDoodadType(std::vector<std::string> types, std::string_view rawcode, bool far) {
    using Output = Result<std::vector<std::string>>;
    if (!ValidRawcode(rawcode)) return Output::Failure(std::string(kDomain), "WARVK_FAR_TYPE_INVALID",
        "“" + std::string(rawcode) + "”不是四字符物编 ID。");
    std::set<std::string> set(types.begin(), types.end());
    if (far) set.emplace(rawcode); else set.erase(std::string(rawcode));
    if (set.size() > kWarVKMaxFarDoodadTypes)
        return Output::Failure(std::string(kDomain), "WARVK_FAR_TYPES_LIMIT",
            "远景装饰物类型最多 16 个，已标记 16 个；请先取消一个再标记“" + std::string(rawcode) + "”。");
    return Output::Success(std::vector<std::string>(set.begin(), set.end()));
}

Result<WarVKMapSettings> ParseWarVKMapSettings(std::span<const std::byte> bytes) {
    using Output = Result<WarVKMapSettings>;
    const auto fail = [](const char* code, std::string message) { return Output::Failure(std::string(kDomain), code, std::move(message)); };
    if (bytes.empty() || bytes.size() > kWarVKSettingsMaxBytes) return fail("WARVK_SETTINGS_SIZE", "WarVK 设置为空或超过 4KiB。");
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    WarVKMapSettings settings;
    std::set<std::string_view> seen;
    bool schema = false;
    for (auto line : Split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) return fail("WARVK_SETTINGS_SYNTAX", "WarVK 设置行缺少“=”。");
        const auto key = line.substr(0, equals); const auto value = line.substr(equals + 1);
        if (!seen.insert(key).second) return fail("WARVK_SETTINGS_DUPLICATE", "WarVK 设置键“" + std::string(key) + "”重复。");
        if (key == "schema") {
            if (value != "1") return fail("WARVK_SETTINGS_SCHEMA", "只支持 schema=1 的 WarVK 设置。");
            schema = true;
        } else if (key == "timing") {
            if (value == "timer") settings.timing = WarVKInitTiming::AfterInitialization;
            else if (value == "main") settings.timing = WarVKInitTiming::MainStart;
            else return fail("WARVK_TIMING_INVALID", "timing 只能是 timer 或 main。");
        } else if (key == "camera.limits") {
            const auto parts = Split(value, ',');
            WarVKCameraLimits limits;
            if (parts.size() != 2 || !ParseNumber(parts[0], limits.maxDistance) || !ParseNumber(parts[1], limits.nearClip))
                return fail("WARVK_CAMERA_NUMBER", "camera.limits 须为“远距上限,近裁剪”两个十进制数。");
            settings.cameraLimits = limits;
        } else if (key == "doodad.far") {
            if (!value.empty()) for (const auto part : Split(value, ',')) settings.farDoodadTypes.emplace_back(part);
        } else if (key == "sky.model") {
            if (value.empty()) return fail("WARVK_SKY_MODEL_INVALID", "sky.model 不能为空；不设置天空模型时省略该行。");
            settings.skyModel = std::string(value);
        } else if (key == "sun.direction") {
            double numbers[2]{};
            if (!ParseNumbers(value, numbers))
                return fail("WARVK_SUN_NUMBER", "sun.direction 须为“方位角,仰角”两个十进制数（度）。");
            settings.sunDirection = WarVKSunDirection{numbers[0], numbers[1]};
        } else if (key == "sun.color") {
            double numbers[4]{};
            if (!ParseNumbers(value, numbers))
                return fail("WARVK_SUN_NUMBER", "sun.color 须为“红,绿,蓝,强度”四个十进制数。");
            settings.sunColor = WarVKSunColor{numbers[0], numbers[1], numbers[2], numbers[3]};
        } else if (key == "lighting.celestialMotion" || key == "lighting.timeColorGrading") {
            const auto enabled = ParseSwitch(value);
            if (!enabled) return fail("WARVK_LIGHTING_SWITCH", std::string(key) + " 只能是 0 或 1；不设置时省略该行。");
            (key == "lighting.celestialMotion" ? settings.celestialMotion : settings.timeColorGrading) = *enabled;
        } else {
            return fail("WARVK_SETTINGS_UNKNOWN_KEY", "未知的 WarVK 设置键“" + std::string(key) + "”；不忽略未知内容。");
        }
    }
    if (!schema) return fail("WARVK_SETTINGS_SCHEMA", "WarVK 设置缺少 schema=1。");
    const auto valid = ValidateWarVKMapSettings(settings);
    if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    return Output::Success(std::move(settings));
}

Result<std::vector<std::byte>> SerializeWarVKMapSettings(const WarVKMapSettings& settings) {
    using Output = Result<std::vector<std::byte>>;
    const auto valid = ValidateWarVKMapSettings(settings);
    if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    std::string text = "schema=1\n";
    text += settings.timing == WarVKInitTiming::MainStart ? "timing=main\n" : "timing=timer\n";
    if (settings.cameraLimits)
        text += "camera.limits=" + Number(settings.cameraLimits->maxDistance) + "," + Number(settings.cameraLimits->nearClip) + "\n";
    if (!settings.farDoodadTypes.empty()) {
        text += "doodad.far=";
        for (std::size_t i = 0; i < settings.farDoodadTypes.size(); ++i) text += (i ? "," : "") + settings.farDoodadTypes[i];
        text += "\n";
    }
    if (!settings.skyModel.empty()) text += "sky.model=" + settings.skyModel + "\n";
    if (settings.sunDirection)
        text += "sun.direction=" + Number(settings.sunDirection->azimuth) + "," + Number(settings.sunDirection->elevation) + "\n";
    if (const auto& sun = settings.sunColor)
        text += "sun.color=" + Number(sun->red) + "," + Number(sun->green) + "," + Number(sun->blue) + "," + Number(sun->intensity) + "\n";
    if (settings.celestialMotion) text += std::string("lighting.celestialMotion=") + (*settings.celestialMotion ? "1" : "0") + "\n";
    if (settings.timeColorGrading) text += std::string("lighting.timeColorGrading=") + (*settings.timeColorGrading ? "1" : "0") + "\n";
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return Output::Success(std::vector<std::byte>(first, first + text.size()));
}

Result<std::vector<std::string>> WarVKInitCommands(const WarVKMapSettings& settings) {
    using Output = Result<std::vector<std::string>>;
    const auto valid = ValidateWarVKMapSettings(settings);
    if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    std::vector<std::string> commands;
    if (settings.cameraLimits)
        commands.push_back("warvk:v1;camera.setLimits;r:" + Number(settings.cameraLimits->maxDistance) + ";r:"
            + Number(settings.cameraLimits->nearClip));
    if (!settings.farDoodadTypes.empty()) {
        std::string line = "warvk:v1;doodad.setFarTypes;s:";
        for (std::size_t i = 0; i < settings.farDoodadTypes.size(); ++i) line += (i ? "," : "") + settings.farDoodadTypes[i];
        commands.push_back(std::move(line));
    }
    if (settings.celestialMotion)
        commands.push_back(std::string("warvk:v1;lightingCycle.setCelestialMotionEnabled;b:") + (*settings.celestialMotion ? "1" : "0"));
    if (settings.timeColorGrading)
        commands.push_back(std::string("warvk:v1;lightingCycle.setTimeColorGradingEnabled;b:") + (*settings.timeColorGrading ? "1" : "0"));
    if (settings.sunDirection) {
        const double azimuth = settings.sunDirection->azimuth * kDegrees, elevation = settings.sunDirection->elevation * kDegrees;
        const double x = -std::cos(elevation) * std::cos(azimuth), y = -std::cos(elevation) * std::sin(azimuth);
        const double z = -std::sin(elevation);
        commands.push_back("warvk:v1;sun.setDirection;r:" + Component(x) + ";r:" + Component(y) + ";r:" + Component(z));
    }
    if (const auto& sun = settings.sunColor)
        commands.push_back("warvk:v1;sun.setColorIntensity;r:" + Number(sun->red) + ";r:" + Number(sun->green) + ";r:"
            + Number(sun->blue) + ";r:" + Number(sun->intensity));
    return Output::Success(std::move(commands));
}

} // namespace War3Map::MapData
