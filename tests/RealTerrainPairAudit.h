#pragma once

#include "War3MapCore/RenderPluginV2.h"
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Tests {

// 测试私有桥：主驱动仍使用原 RendererOwner/CaptureFrame/SaveBmp。
struct RealTerrainPairFrame {
    std::vector<std::uint8_t> pixels;
    War3RenderCapturedFrameV2 receipt{};
    War3RenderStatusV2 status{};
    std::string providerSha256; // 新cliff-ground入口填写；旧flat合同不依赖此追加字段。
};
using RealTerrainPairCapture = std::function<RealTerrainPairFrame(
    const War3RenderTerrainMipSceneV2&, const War3RenderCameraV2&, std::uint32_t, std::uint32_t)>;
using RealTerrainPairSaveBmp = std::function<void(const std::filesystem::path&,
    const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t)>;
// 独立窄入口：两份生产packet必须在同一个renderer中完成后才取帧。
using RealCliffGroundCapture = std::function<RealTerrainPairFrame(
    const War3RenderTerrainMipSceneV2&, const War3RenderModelSceneV2&,
    const War3RenderCameraV2&, std::uint32_t, std::uint32_t)>;

// 原创小输入仅验证资格/拒绝合同，无真实文件、解码、GPU 或像素预期。
void RunRealTerrainPairValidationTests();
void RunRealTerrainPairAudit(const std::filesystem::path& manifest, std::string_view manifestSha256,
    const std::filesystem::path& outputDirectory, const RealTerrainPairCapture& capture,
    const RealTerrainPairSaveBmp& saveBmp);
void RunRealCliffGroundValidationTests();
void RunRealCliffGroundAudit(const std::filesystem::path& manifest, std::string_view manifestSha256,
    const std::filesystem::path& outputDirectory, const RealCliffGroundCapture& capture,
    const RealTerrainPairSaveBmp& saveBmp);

} // namespace War3Map::Tests
