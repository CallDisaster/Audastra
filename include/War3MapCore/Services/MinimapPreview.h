#pragma once

#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Render/ClassicMinimapCandidate.h"

namespace War3Map::Services {

// 宿主拥有的已打开快照字节。此值不是来源授权或保存资格。
// nullopt仅用于宿主已证明缺失；不完整清单不能据此填成缺失。
struct MinimapPreviewInput {
    std::string sourceSha256;
    std::optional<std::vector<std::byte>> member;
};

// 实际归档中的已保存底图，不生成、不合成图标、不代替保存资格。
struct MinimapPreview {
    std::string sourceSha256;
    bool present = false;
    std::string memberSha256;
    std::string rgbaSha256;
    Image::DecodedImage image;
    bool current = false;
    std::uint64_t documentRevision = 0;
    std::uint32_t layers = 0x8b;
    std::array<std::uint32_t, 4> sourceRectangle{};
    std::array<float, 2> terrainOffset{};
    std::array<float, 4> worldRectangle{};
    std::array<std::int32_t, 4> pixelRectangle{};
    double generationMilliseconds = 0;
};

struct CurrentMinimapPreviewInput {
    std::shared_ptr<const MapArchiveSession> archive;
    std::shared_ptr<const MapData::MapDataEditSession> edits;
    std::filesystem::path gameRoot;
    std::uint64_t documentRevision = 0;
    std::uint32_t layers = 0x8b;
    bool units = false, starts = false, icons = false;
    std::vector<MemberReplacement> replacements;
    std::vector<std::string> removals;
};
// 只读生成；只消费宿主拥有的不可变快照，不写归档、不产生编辑/撤销修订。
[[nodiscard]] Result<MinimapPreview> GenerateCurrentMinimapPreview(const CurrentMinimapPreviewInput& input);
[[nodiscard]] Result<MinimapPreview> OpenCurrentMinimapPreview(const std::filesystem::path& source,
    std::string_view expectedSourceSha256, const std::filesystem::path& gameRoot,
    std::uint32_t layers = 0x8b, bool units = false, bool starts = false, bool icons = false);
// 与自动保存共用同内容噪点种子，默认图层的当前/保存图可逐像素比较。
[[nodiscard]] Result<std::uint32_t> MinimapContentNoiseSeed(const Render::ClassicMinimapObjectAssets& assets,
    const Render::TerrainMinimapPostprocessSettings& settings, std::size_t maximumWorkingBytes);

[[nodiscard]] Result<MinimapPreview> DecodeMinimapPreview(const MinimapPreviewInput& input);

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
