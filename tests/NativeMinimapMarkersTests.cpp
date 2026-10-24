#include "War3MapCore/MapData/NativeMinimapMarkers.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw std::runtime_error("原创MMP计算未返回完整结果");
    return std::move(*result.value);
}
template<class T> void Rejected(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "失败不能留下部分MMP记录");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code;
    }), "必须由预期的具体边界拒绝");
}
void CoordinatesAndWire() {
    NativeMinimapMarkerFrame frame{{-512, -256, 512, 256}, {0, 64, 256, 192}};
    const std::vector<NativeMinimapMarkerCandidate> input{
        {0, {-512, -256}, {255, 255, 255, 255}},
        {1, {512, 256}, {255, 255, 255, 255}},
        {2, {0, 0}, {1, 2, 3, 255}},
        {0, {-513, -258}, {255, 255, 255, 255}},
        {1, {516, 260}, {255, 255, 255, 255}},
    };
    auto result = Take(BuildNativeMinimapMarkers(frame, input));
    const std::array<std::array<std::int32_t, 2>, 5> positions{{{0, 192}, {256, 64}, {128, 128}, {0, 192}, {257, 63}}};
    Require(result.records.size() == positions.size(), "不排序、不丢弃、不合并域外或同坐标记录");
    for (std::size_t index = 0; index < positions.size(); ++index) {
        Require(result.records[index].positionRaw == positions[index], "位置采用朝零截断和Y翻转，域外不clamp");
        Require(result.records[index].typeRaw == input[index].type, "类型保持候选枚举顺序");
        Require(result.records[index].colorBgra == input[index].colorBgra, "颜色按BGRA原样写出");
    }
    const auto bytes = Take(SerializeMapMinimapMarkers(result));
    const auto decoded = Take(ParseMapMinimapMarkers(bytes));
    Require(bytes.size() == 88 && bytes[4] == std::byte{5}, "MMP v0头和五条16字节记录");
    Require(decoded.records[2].colorBgra == std::array<std::uint8_t, 4>{1, 2, 3, 255}, "BGRA逐字往返");
}
void InvalidInputAndBudgets() {
    NativeMinimapMarkerFrame frame{{0, 0, 128, 128}};
    std::vector<NativeMinimapMarkerCandidate> candidates{{0, {64, 64}, {255, 255, 255, 255}}};
    auto limits = MinimapMarkerLimits{}; limits.maxBytes = 23;
    Rejected(BuildNativeMinimapMarkers(frame, candidates, limits), "NATIVE_MMP_BUDGET");
    limits.maxBytes = 24;
    Require(Take(BuildNativeMinimapMarkers(frame, candidates, limits)).records.size() == 1, "精确24字节预算可写一记录");
    limits.maxMarkers = 0;
    Rejected(BuildNativeMinimapMarkers(frame, candidates, limits), "NATIVE_MMP_BUDGET");
    frame.worldRectangle[2] = 0;
    Rejected(BuildNativeMinimapMarkers(frame, candidates), "NATIVE_MMP_WORLD_RECTANGLE");
    frame.worldRectangle[2] = 128; frame.pixelRectangle[3] = 257;
    Rejected(BuildNativeMinimapMarkers(frame, candidates), "NATIVE_MMP_PIXEL_RECTANGLE");
    frame.pixelRectangle[3] = 256;
    candidates.front().position[0] = std::numeric_limits<float>::infinity();
    Rejected(BuildNativeMinimapMarkers(frame, candidates), "NATIVE_MMP_POSITION");
    candidates.front().position[0] = std::numeric_limits<float>::max();
    Rejected(BuildNativeMinimapMarkers(frame, candidates), "NATIVE_MMP_POSITION_OVERFLOW");
    candidates.front().position[0] = 64; candidates.front().type = 3;
    Rejected(BuildNativeMinimapMarkers(frame, candidates), "NATIVE_MMP_TYPE");
}
void ActualTeamColorAverage() {
    // 四像素整图均色，不是左上角、首像素、最低mip或固定调色板。
    const std::array<std::byte, 16> rgba{
        std::byte{10}, std::byte{20}, std::byte{30}, std::byte{0},
        std::byte{20}, std::byte{30}, std::byte{40}, std::byte{1},
        std::byte{30}, std::byte{40}, std::byte{50}, std::byte{100},
        std::byte{41}, std::byte{51}, std::byte{61}, std::byte{255}};
    Require(Take(AverageNativeMinimapTeamColor(rgba)) == std::array<std::uint8_t, 4>{45, 35, 25, 255},
        "整图RGB整数均色并转BGRA，源alpha不影响结果");
    Rejected(AverageNativeMinimapTeamColor(rgba, 3), "NATIVE_MMP_COLOR_IMAGE");
    Require(Take(AverageNativeMinimapTeamColor(rgba, 4))[0] == 45, "精确像素预算通过");
    Rejected(AverageNativeMinimapTeamColor(std::span(rgba).first(15)), "NATIVE_MMP_COLOR_IMAGE");
    Rejected(AverageNativeMinimapTeamColor({}), "NATIVE_MMP_COLOR_IMAGE");
}
void PlayableRectangleAndAspect() {
    TerrainGridSnapshot terrain;
    terrain.header.width = 21; terrain.header.height = 11; terrain.header.offset = {-1024, 256};
    MapSettings settings; settings.formatVersion = 25; settings.custom.emplace(); settings.flags = 0x400;
    settings.cameraMargins = {1, 3, 2, 1};
    const auto rectangle = Take(NativeMinimapPlayableRectangle(terrain, settings));
    Require(rectangle == std::array<std::uint32_t, 4>{1, 2, 17, 9}, "补足边距按LRBT抵消，输出普通XY矩形");
    const auto frame = Take(NativeMinimapFrame(terrain, rectangle));
    Require(frame.worldRectangle == std::array<float, 4>{-896, 512, 1152, 1408}, "内部转置布局封装为普通世界XY");
    Require(frame.pixelRectangle == std::array<std::int32_t, 4>{0, 72, 256, 184}, "宽图上下同宽黑边并朝零截断");
    settings.flags = 0; settings.cameraBounds = {-959, 321, 1087, 1343, 9000, 9000, -9000, -9000};
    Rejected(NativeMinimapPlayableRectangle(terrain, settings), "NATIVE_MMP_CAMERA_CONFIG");
    const auto custom = Take(NativeMinimapPlayableRectangle(terrain, settings, std::array<double, 4>{0, 0, 0, 0}));
    Require(custom == std::array<std::uint32_t, 4>{1, 1, 16, 8}, "只用前两相机角，tile+0.5截断");
    settings.cameraBounds = {-50000, -50000, 50000, 50000, 0, 0, 0, 0};
    Require(Take(NativeMinimapPlayableRectangle(terrain, settings, std::array<double, 4>{0, 0, 0, 0}))
        == std::array<std::uint32_t, 4>{0, 0, 20, 10}, "越界相机先夹世界边界");
    Rejected(NativeMinimapFrame(terrain, {0, 0, 21, 10}), "NATIVE_MMP_FRAME");
    terrain.header.width = 11; terrain.header.height = 21;
    Require(Take(NativeMinimapFrame(terrain, {0, 0, 10, 20})).pixelRectangle
        == std::array<std::int32_t, 4>{64, 0, 192, 256}, "高图左右黑边不交换单位XY");
}
}

int main() {
    try {
        CoordinatesAndWire(); InvalidInputAndBudgets(); ActualTeamColorAverage(); PlayableRectangleAndAspect();
        std::cout << "Native MMP computation tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
