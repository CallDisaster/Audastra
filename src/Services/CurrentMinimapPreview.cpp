#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"
#include "War3MapCore/Render/ClassicMinimapPathing.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/MapData/NativeMinimapMarkers.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/Services/MinimapMarkerEdits.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/ProjectSaveControl.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

namespace War3Map::Services {
namespace {
struct Failure {
    std::vector<Diagnostic> diagnostics;
};
template <class T> T Take(Result<T> value) {
    if (!value.Succeeded() || !value.value)
        throw Failure{std::move(value.diagnostics)};
    return std::move(*value.value);
}
void Fail(const char* code, std::string text) {
    throw Failure{Status::Failure("Minimap.Preview", code, std::move(text)).diagnostics};
}
void Check(Status value) {
    if (!value.Succeeded())
        throw Failure{std::move(value.diagnostics)};
}

// 与保存同一入口取得本次保存会写出的 WPM：有相关变化时由保存计划重建，否则沿用会话草稿或来源原图。
// 不另行推算通行；保存计划拒绝的情况（来源不可解释、资源缺失等）原样报告。
MapData::PathingGridSnapshot FinalPathing(const CurrentMinimapPreviewInput& input, const MapArchiveSession& source,
                                          std::vector<MemberReplacement> finalMembers,
                                          const MapData::TerrainHeader& header) {
    auto plan = Take(TerrainPathingSave::PrepareFinalForSave(source, input.edits.get(), input.gameRoot, {},
                                                             finalMembers, input.removals));
    if (plan)
        Check(plan->InstallPathingOutput(*input.edits, finalMembers));
    std::optional<std::vector<std::byte>> bytes;
    for (auto& member : finalMembers) {
        if (Take(Resources::CanonicalImportPath(member.name)) == "WAR3MAP.WPM") {
            bytes = std::move(member.data);
            break;
        }
    }
    if (!bytes) {
        // 固定快照不复制 WPM；在同一已固定的只读来源租约内按名精确捕获。
        const std::array<std::string, 1> names{"war3map.wpm"};
        auto captured = Take(source.CaptureExactMembers(names));
        if (captured.size() != 1 || !captured.front().present)
            Fail("MINIMAP_CURRENT_PATHING_MISSING", "地图没有通行图（war3map.wpm），无法显示通行层。");
        bytes = std::move(captured.front().bytes);
    }
    const auto parsed = Take(MapData::ParseMapPathing(*bytes, &header));
    if (!parsed.Snapshot())
        Fail("MINIMAP_CURRENT_PATHING", "通行图未能读取。");
    return *parsed.Snapshot();
}

// 256 像素小地图上图标的边长；与游戏里金矿、开始位置图标相对整张小地图的大小一致。
constexpr int kMinimapIconSize = 16;

// 把图标贴在标记中心：图标颜色乘以标记颜色（开始位置取玩家色），按图标 alpha 叠加。
// 缩小按区域平均（预乘 alpha），避免最近邻丢细线。
void DrawMinimapIcon(Image::DecodedImage& target, const Image::DecodedImage& icon, int centerX, int centerY, int size,
                     const std::array<std::uint8_t, 4>& bgra) {
    if (!icon.Valid() || !target.Valid() || size <= 0)
        return;
    const int left = centerX - size / 2, top = centerY - size / 2;
    const std::array<double, 3> tint{bgra[2] / 255.0, bgra[1] / 255.0, bgra[0] / 255.0};
    const auto span = [size](int index, std::uint32_t extent) {
        const auto begin = static_cast<std::uint32_t>(std::uint64_t(index) * extent / size);
        const auto end = static_cast<std::uint32_t>(std::uint64_t(index + 1) * extent / size);
        return std::pair{begin, std::max(end, begin + 1)};
    };
    for (int ty = 0; ty < size; ++ty) {
        const int y = top + ty;
        if (y < 0 || y >= static_cast<int>(target.height))
            continue;
        const auto [y0, y1] = span(ty, icon.height);
        for (int tx = 0; tx < size; ++tx) {
            const int x = left + tx;
            if (x < 0 || x >= static_cast<int>(target.width))
                continue;
            const auto [x0, x1] = span(tx, icon.width);
            std::array<double, 3> sum{};
            double alpha = 0;
            std::size_t samples = 0;
            for (auto sy = y0; sy < y1 && sy < icon.height; ++sy)
                for (auto sx = x0; sx < x1 && sx < icon.width; ++sx) {
                    const auto at = (static_cast<std::size_t>(sy) * icon.width + sx) * 4;
                    const double a = std::to_integer<unsigned>(icon.rgba[at + 3]) / 255.0;
                    for (std::size_t c = 0; c < 3; ++c)
                        sum[c] += std::to_integer<unsigned>(icon.rgba[at + c]) * a;
                    alpha += a;
                    ++samples;
                }
            if (samples == 0 || alpha <= 0)
                continue;
            const double coverage = alpha / samples * (bgra[3] / 255.0);
            const auto at = (static_cast<std::size_t>(y) * target.width + x) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                const double source = sum[c] / alpha * tint[c];
                const double base = std::to_integer<unsigned>(target.rgba[at + c]);
                target.rgba[at + c] = static_cast<std::byte>(
                    std::clamp(static_cast<int>(std::lround(base + (source - base) * coverage)), 0, 255));
            }
            target.rgba[at + 3] = std::byte{255};
        }
    }
}
} // namespace

Result<MinimapPreview> GenerateCurrentMinimapPreview(const CurrentMinimapPreviewInput& input) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<Diagnostic> warnings;
    try {
        const auto checkpoint = [&] {
            if (input.control && !input.control->Checkpoint(ProjectSaveStage::Copy))
                throw Failure{ProjectSaveCancelledStatus().diagnostics};
        };
        checkpoint();
        if (!input.archive || !input.edits || !input.edits->Terrain())
            Fail("MINIMAP_CURRENT_SOURCE", "当前地形未就绪。");
        if (input.layers & ~0x4afu)
            Fail("MINIMAP_CURRENT_LAYERS", "小地图图层参数无效。");
        if (input.replacements.size() > 8192 || input.removals.size() > 8192)
            Fail("MINIMAP_PREVIEW_BUDGET", "显示资源覆盖超过数量预算。");
        std::size_t overlayBytes = 0;
        for (const auto& member : input.replacements) {
            if (Text::LowerAscii(member.name).ends_with(".ai"))
                Fail("MINIMAP_CURRENT_RESOURCE", "引导成员不能作为小地图资源覆盖。");
            if (member.name.size() > 260 || member.data.size() > 128U * 1024U * 1024U - overlayBytes)
                Fail("MINIMAP_PREVIEW_BUDGET", "显示资源覆盖超过128MiB预算。");
            overlayBytes += member.data.size();
        }
        auto source = Take(input.archive->FreezeMinimapSnapshot(input.control.get()));
        Check(source.PinSourceForExactCaptures(input.control.get()));
        checkpoint();
        auto replacements = input.replacements;
        for (auto& member : input.edits->Replacements()) {
            const auto key = Text::LowerAscii(member.name);
            const auto duplicate = std::find_if(replacements.begin(), replacements.end(), [&](const auto& r) {
                return Text::LowerAscii(r.name) == key;
            });
            if (duplicate != replacements.end())
                Fail("MINIMAP_CURRENT_OVERLAY_CONFLICT", "导入覆盖与当前文档重复：" + member.name);
            replacements.push_back({std::move(member.name), std::move(member.data)});
        }
        auto terrain = Take(Render::CaptureTerrainMinimapAssets(source, *input.edits, input.gameRoot, {},
                                                                Render::TerrainMinimapRangePolicy::ClassicPlayable,
                                                                replacements, input.removals));
        checkpoint();
        auto assets = Take(Render::CaptureClassicMinimapObjectAssets(std::move(terrain), source, *input.edits, {},
                                                                     replacements, input.removals));
        checkpoint();
        const auto document = input.edits->Snapshot();
        const auto grid = input.edits->Terrain();
        ObjectCatalogSources sources;
        sources.archive = &source;
        sources.gameRoot = input.gameRoot;
        sources.replacements = replacements;
        sources.removals = input.removals;
        std::size_t extraBytes = 0;
        const auto extra = [&](std::string name) {
            checkpoint();
            const auto lower = Text::LowerAscii(name);
            if (lower != "ui/worldeditdata.txt" && lower != "ui\\worldeditdata.txt" &&
                !(lower.ends_with(".blp") &&
                  (lower.starts_with("terrainart") || lower.starts_with("replaceabletextures") ||
                   lower.starts_with("ui/minimap/minimapicon/"))))
                Fail("MINIMAP_CURRENT_RESOURCE", "小地图显示层拒绝非贴图来源。");
            const std::array<std::string, 1> names{name};
            auto capture = Take(CaptureObjectCatalogSources(sources, names, 4U * 1024U * 1024U));
            auto file = Take(capture.Read(name));
            if (!file)
                Fail("MINIMAP_CURRENT_RESOURCE", "小地图显示层缺少资源：" + name);
            if (file->bytes.size() > 16U * 1024U * 1024U - extraBytes)
                Fail("MINIMAP_PREVIEW_BUDGET", "小地图显示层资源超过16MiB。");
            extraBytes += file->bytes.size();
            Check(capture.Verify());
            return std::move(file->bytes);
        };
        Render::ClassicMinimapViewOptions view;
        view.layers = input.layers;
        Image::BlpDecodeLimits extraDecode;
        extraDecode.maxInputBytes = 4U * 1024U * 1024U;
        extraDecode.maxDimension = 1024;
        extraDecode.maxWorkingBytes = 32U * 1024U * 1024U;
        if (input.layers & 0x400u) {
            const auto configBytes = extra("UI/WorldEditData.txt");
            const auto profile = Text::ParseProfile(
                std::string_view(reinterpret_cast<const char*>(configBytes.data()), configBytes.size()));
            const auto section = profile.find("tilesets");
            const auto key = Text::LowerAscii(std::string(1, static_cast<char>(grid->header.mainTileset)));
            if (section == profile.end() || !section->second.contains(key))
                Fail("MINIMAP_CURRENT_BLIGHT", "当前地形集缺少荒芜纹理定义。");
            const auto& value = section->second.at(key);
            const auto comma = value.find(',');
            if (comma == value.npos)
                Fail("MINIMAP_CURRENT_BLIGHT", "荒芜纹理定义不完整。");
            auto path = Text::TrimCWhitespaceCopy(value.substr(comma + 1));
            if (!Text::LowerAscii(path).ends_with(".blp"))
                path += ".blp";
            auto image = Take(Image::DecodeBlpBudgeted(extra(path), extraDecode)).image;
            // 原版荒芜代表色是整张纹理RGB整数均色；与TeamColor共用已有均色器。
            const auto bgra = Take(MapData::AverageNativeMinimapTeamColor(image.rgba));
            view.blightColor = {bgra[2], bgra[1], bgra[0], 255};
        }
        view.overlay = [&](Image::DecodedImage& raw, const auto& rectangle, const auto& header) -> Status {
            if (!(input.layers & 0x20u))
                return Status::Ok();
            if (!document.Regions()) {
                if (document.Member(MapDataKind::Regions).state == MapDataState::Missing)
                    return Status::Ok();
                return Status::Failure("Minimap.Preview", "MINIMAP_CURRENT_REGIONS", "区域来源未完整读取。");
            }
            std::size_t work = 0;
            for (const auto& region : document.Regions()->entries) {
                if (!std::isfinite(region.left) || !std::isfinite(region.right) || !std::isfinite(region.top) ||
                    !std::isfinite(region.bottom) || region.left >= region.right || region.bottom >= region.top)
                    return Status::Failure("Minimap.Preview", "MINIMAP_CURRENT_REGIONS", "区域边界无效。");
                const auto edge = [](float coordinate, float offset, std::uint32_t start, std::uint32_t limit) {
                    return static_cast<std::uint32_t>(
                        std::clamp(std::ceil((double(coordinate) - offset) / 128.0 - start), 0.0, double(limit)));
                };
                const auto x0 = edge(region.left, header.offset[0], rectangle[0], raw.width),
                           x1 = edge(region.right, header.offset[0], rectangle[0], raw.width);
                const auto y0 = edge(region.bottom, header.offset[1], rectangle[1], raw.height),
                           y1 = edge(region.top, header.offset[1], rectangle[1], raw.height);
                for (std::uint32_t y = y0; y < y1; ++y)
                    for (std::uint32_t x = x0; x < x1; ++x) {
                        if (++work > 8U * 1024U * 1024U)
                            return Status::Failure("Minimap.Preview", "MINIMAP_PREVIEW_BUDGET",
                                                   "区域叠加超过工作预算。");
                        const auto wx = header.offset[0] + static_cast<double>(rectangle[0] + x) * 128;
                        const auto wy = header.offset[1] + static_cast<double>(rectangle[1] + y) * 128;
                        if (wx < region.left || wx >= region.right || wy < region.bottom || wy >= region.top)
                            continue;
                        const auto at = (static_cast<std::size_t>(y) * raw.width + x) * 4;
                        for (std::size_t c = 0; c < 3; ++c)
                            raw.rgba[at + c] = static_cast<std::byte>(
                                (3U * std::to_integer<unsigned>(raw.rgba[at + c]) + region.colorBgra[2 - c]) / 4U);
                    }
            }
            return Status::Ok();
        };
        Render::TerrainMinimapPostprocessSettings post;
        post.noiseSeed = Take(MinimapContentNoiseSeed(assets, post, 128U * 1024U * 1024U));
        checkpoint();
        auto candidate = Take(Render::BuildClassicMinimapView(assets, view, post));
        checkpoint();
        MinimapPreview output;
        output.current = output.present = true;
        output.sourceSha256 = input.archive->SourceSha256();
        output.documentRevision = input.documentRevision;
        output.layers = input.layers;
        output.sourceRectangle = candidate.manifest.sourceRectangle;
        output.terrainOffset = grid->header.offset;
        const auto frame = Take(MapData::NativeMinimapFrame(*grid, output.sourceRectangle));
        output.worldRectangle = frame.worldRectangle;
        output.pixelRectangle = frame.pixelRectangle;
        // 与保存后真正解码的JPEG保持同一像素语义，不拿编码前RGBA冒充保存图。
        output.image = Take(Image::DecodeBlp(candidate.encoded.bytes));
        if (input.layers & 4u) {
            // 原版在底图后处理完成后叠通行层，图标与点在其后绘制。
            const auto pathing = FinalPathing(input, source, replacements, grid->header);
            checkpoint();
            Check(Render::CompositeClassicMinimapPathing(output.image, candidate.manifest.postprocessFacts,
                                                         output.sourceRectangle, pathing));
        }
        if (input.units || input.starts || input.icons) {
            // 原版小地图：普通单位是 3×3 的点，建筑是更大的 5×5 方块。
            const auto drawDot = [&](int x, int y, std::array<std::uint8_t, 4> bgra, int radius = 1) {
                if (x < -radius || x > 255 + radius || y < -radius || y > 255 + radius)
                    return;
                for (int dy = -radius; dy <= radius; ++dy)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (x + dx < 0 || x + dx >= 256 || y + dy < 0 || y + dy >= 256)
                            continue;
                        const auto at = (static_cast<std::size_t>(y + dy) * 256 + x + dx) * 4;
                        for (std::size_t c = 0; c < 3; ++c)
                            output.image.rgba[at + c] = static_cast<std::byte>(bgra[2 - c]);
                        output.image.rgba[at + 3] = std::byte{255};
                    }
            };
            const auto& placed = assets.Units().units;
            std::vector<bool> buildings;
            if (input.units) {
                // 与保存时生成小地图图标同一套物编解释判断建筑；个别解释不了的按普通单位画并带回警告。
                auto classified = ClassifyMinimapBuildings(source, input.gameRoot, grid->header.mainTileset, placed,
                                                           replacements, input.removals);
                warnings.insert(warnings.end(), classified.diagnostics.begin(), classified.diagnostics.end());
                buildings = Take(std::move(classified));
            }
            std::array<std::optional<std::array<std::uint8_t, 4>>, 16> colors;
            for (std::size_t index = 0; index < placed.size(); ++index) {
                const auto& unit = placed[index];
                const bool isStart = unit.rawcode == "sloc";
                if (isStart ? !input.starts : !input.units)
                    continue;
                if (unit.owner < 0 || unit.owner >= 16)
                    Fail("MINIMAP_CURRENT_PLAYER", "单位玩家编号无法解释。");
                auto &color = colors[unit.owner];
                if (!color) {
                    char path[80];
                    std::snprintf(path, sizeof(path), "ReplaceableTextures/TeamColor/TeamColor%02d.blp", unit.owner);
                    auto image = Take(Image::DecodeBlpBudgeted(extra(path), extraDecode)).image;
                    color = Take(MapData::AverageNativeMinimapTeamColor(image.rgba));
                }
                const MapData::NativeMinimapMarkerCandidate marker{0, {unit.position[0], unit.position[1]}, *color};
                const auto dots = Take(MapData::BuildNativeMinimapMarkers(frame, std::span(&marker, 1)));
                const bool building = index < buildings.size() && buildings[index];
                drawDot(dots.records[0].positionRaw[0], dots.records[0].positionRaw[1], *color, building ? 2 : 1);
            }
            // 金矿、中立建筑、开始位置用游戏里真实的小地图图标；未知类型仍画点。
            std::array<std::optional<Image::DecodedImage>, 3> iconArt;
            const auto drawMarker = [&](const MapData::MinimapMarkerRecord& icon) {
                static constexpr std::array<const char*, 3> files{"UI/MiniMap/MiniMapIcon/MinimapIconGold.blp",
                    "UI/MiniMap/MiniMapIcon/MinimapIconNeutralBuilding.blp", "UI/MiniMap/MiniMapIcon/MinimapIconStartLoc.blp"};
                if (icon.typeRaw < 0 || icon.typeRaw >= static_cast<std::int32_t>(files.size())) {
                    drawDot(icon.positionRaw[0], icon.positionRaw[1], icon.colorBgra);
                    return;
                }
                auto& art = iconArt[static_cast<std::size_t>(icon.typeRaw)];
                if (!art)
                    art = Take(Image::DecodeBlpBudgeted(extra(files[static_cast<std::size_t>(icon.typeRaw)]), extraDecode)).image;
                DrawMinimapIcon(output.image, *art, icon.positionRaw[0], icon.positionRaw[1], kMinimapIconSize, icon.colorBgra);
            };
            if (input.icons) {
                const auto pending = std::find_if(input.replacements.begin(), input.replacements.end(), [](const auto& member) {
                    auto key = Resources::CanonicalImportPath(member.name);
                    return key.Succeeded() && key.value && *key.value == "AUDASTRA\\MINIMAP-MARKERS.BIN";
                });
                if (pending != input.replacements.end()) {
                    const auto edits = Take(MinimapMarkerEdits::Read(source, pending->data));
                    for (const auto &icon : Take(edits.View()).records)
                        drawMarker(icon);
                } else {
                    const std::array<std::string, 1> names{"war3map.mmp"};
                    auto captured = Take(CaptureObjectCatalogSources(sources, names, 4U * 1024U * 1024U));
                    auto file = Take(captured.Read(names[0]));
                    if (file) {
                        auto icons = Take(MapData::ParseMapMinimapMarkers(file->bytes));
                        for (const auto &icon : icons.records)
                            drawMarker(icon);
                    }
                    Check(captured.Verify());
                }
            }
        }
        output.rgbaSha256 = Take(Sha256(output.image.rgba));
        Check(source.VerifySourceUnchanged());
        checkpoint();
        output.generationMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        auto result = Result<MinimapPreview>::Success(std::move(output));
        result.diagnostics = std::move(warnings);
        return result;
    } catch (Failure& failure) {
        return {{}, std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Result<MinimapPreview>::Failure("Minimap.Preview", "MINIMAP_PREVIEW_BUDGET", "当前小地图预算不足。");
    }
}

Result<MinimapPreview> OpenCurrentMinimapPreview(const std::filesystem::path &path, std::string_view expected,
                                                 const std::filesystem::path &gameRoot, std::uint32_t layers,
                                                 bool units, bool starts, bool icons) {
    if (expected.size() != 64 || !std::all_of(expected.begin(), expected.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        return Result<MinimapPreview>::Failure("Minimap.Preview", "MINIMAP_PREVIEW_IDENTITY", "需要小写完整SHA-256。");
    const auto extension = Text::LowerAscii(path.extension().string());
    if (extension != ".w3x" && extension != ".w3m")
        return Result<MinimapPreview>::Failure("Minimap.Preview", "MINIMAP_PREVIEW_SOURCE_TYPE", "来源须为地图归档。");
    auto archive = MapArchiveSession::OpenMinimapAudit(path);
    if (!archive.Succeeded())
        return {{}, archive.diagnostics};
    if (archive.value->SourceSha256() != expected)
        return Result<MinimapPreview>::Failure("Minimap.Preview", "MINIMAP_PREVIEW_STALE", "来源身份已变化。");
    auto owner = std::make_shared<const MapArchiveSession>(std::move(*archive.value));
    auto document = MapDocument::FromMembers(owner->Members(), owner->SourcePath());
    MapData::MapDataEditSession edits(document);
    const std::array<std::string, 2> names{"war3map.w3e", "war3map.wpm"};
    auto bytes = owner->CaptureExactMembers(names);
    if (!bytes.Succeeded())
        return {{}, bytes.diagnostics};
    if (!bytes.value->at(0).present)
        return Result<MinimapPreview>::Failure("Minimap.Preview", "MINIMAP_CURRENT_SOURCE", "没有地形。");
    auto status = edits.AttachTerrain(bytes.value->at(0).bytes);
    if (!status.Succeeded())
        return {{}, status.diagnostics};
    if (bytes.value->at(1).present) {
        status = edits.AttachPathing(bytes.value->at(1).bytes);
        if (!status.Succeeded())
            return {{}, status.diagnostics};
    }
    auto frozen = edits.FreezeForSave(128U * 1024U * 1024U);
    if (!frozen.Succeeded())
        return {{}, frozen.diagnostics};
    CurrentMinimapPreviewInput input;
    input.archive = std::move(owner);
    input.edits = *frozen.value;
    input.gameRoot = gameRoot;
    input.layers = layers;
    input.units = units;
    input.starts = starts;
    input.icons = icons;
    return GenerateCurrentMinimapPreview(input);
}
} // namespace War3Map::Services
