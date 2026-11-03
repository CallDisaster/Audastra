#pragma once

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapArchiveSession.h"

namespace War3Map::Services {

// 实际归档中的已保存底图，不生成、不合成图标、不代替保存资格。
struct MinimapPreview {
    std::string sourceSha256;
    bool present = false;
    std::string memberSha256;
    std::string rgbaSha256;
    Image::DecodedImage image;
};

// 唯一读取名war3mapMap.blp，含无listfile的精确查询；缺失与坏图严格区分。
// 输入最多4MiB，图边长最多512，解码工作预算64MiB；不缩放或猜测图片方向。
[[nodiscard]] Result<MinimapPreview> CaptureMinimapPreview(
    const MapArchiveSession& source, std::string_view expectedSourceSha256);
[[nodiscard]] Result<MinimapPreview> OpenMinimapPreview(
    const std::filesystem::path& source, std::string_view expectedSourceSha256);

// 显式导出查看副本，须为尚不存在的文件；PPM以黑色合成alpha，原RGBA不变。
[[nodiscard]] Status ExportMinimapPreviewPpm(const MinimapPreview& preview,
    const std::filesystem::path& output);
} // namespace War3Map::Services
