#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"
#include "War3MapCore/MapData/NativeMinimapMarkers.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace War3Map::Services {
namespace {
struct Failure { std::vector<Diagnostic> diagnostics; };
template<class T> T Take(Result<T> value) {
    if (!value.Succeeded() || !value.value) throw Failure{std::move(value.diagnostics)};
    return std::move(*value.value);
}
void Fail(const char* code, std::string text) { throw Failure{Status::Failure("Minimap.Preview", code, std::move(text)).diagnostics}; }
void Check(Status value) { if (!value.Succeeded()) throw Failure{std::move(value.diagnostics)}; }
}

Result<MinimapPreview> GenerateCurrentMinimapPreview(const CurrentMinimapPreviewInput& input) {
    const auto start = std::chrono::steady_clock::now();
    try {
        if (!input.archive || !input.edits || !input.edits->Terrain()) Fail("MINIMAP_CURRENT_SOURCE", "当前地形未就绪。");
        if (input.layers & ~0x4afu) Fail("MINIMAP_CURRENT_LAYERS", "小地图图层参数无效。");
        // 通行位的配色需独立原版证据；不能从位号猜颜色后冒充原版。
        if (input.layers & 4u) Fail("MINIMAP_CURRENT_PATHING_UNVERIFIED", "通行层的原版配色尚未核定。");
        auto replacements = input.replacements;
        for (auto& member : input.edits->Replacements()) {
            const auto key = Text::LowerAscii(member.name);
            const auto duplicate = std::find_if(replacements.begin(), replacements.end(), [&](const auto& r) { return Text::LowerAscii(r.name) == key; });
            if (duplicate != replacements.end()) Fail("MINIMAP_CURRENT_OVERLAY_CONFLICT", "导入覆盖与当前文档重复：" + member.name);
            replacements.push_back({std::move(member.name), std::move(member.data)});
        }
        auto terrain = Take(Render::CaptureTerrainMinimapAssets(*input.archive, *input.edits, input.gameRoot, {},
            Render::TerrainMinimapRangePolicy::ClassicPlayable, replacements, input.removals));
        auto assets = Take(Render::CaptureClassicMinimapObjectAssets(std::move(terrain), *input.archive, *input.edits, {}, replacements, input.removals));
        const auto document = input.edits->Snapshot();
        const auto grid = input.edits->Terrain();
        ObjectCatalogSources sources; sources.archive = input.archive.get(); sources.gameRoot = input.gameRoot;
        sources.replacements = replacements; sources.removals = input.removals;
        std::size_t extraBytes = 0;
        const auto extra = [&](std::string name) {
            const auto lower = Text::LowerAscii(name);
            if (lower != "ui/worldeditdata.txt" && lower != "ui\\worldeditdata.txt"
                && !(lower.ends_with(".blp") && (lower.starts_with("terrainart") || lower.starts_with("replaceabletextures"))))
                Fail("MINIMAP_CURRENT_RESOURCE", "小地图显示层拒绝非贴图来源。");
            const std::array<std::string, 1> names{name};
            auto capture = Take(CaptureObjectCatalogSources(sources, names, 4U * 1024U * 1024U));
            auto file = Take(capture.Read(name));
            if (!file) Fail("MINIMAP_CURRENT_RESOURCE", "小地图显示层缺少资源：" + name);
            if (file->bytes.size() > 16U * 1024U * 1024U - extraBytes) Fail("MINIMAP_PREVIEW_BUDGET", "小地图显示层资源超过16MiB。");
            extraBytes += file->bytes.size();
            Check(capture.Verify());
            return std::move(file->bytes);
        };
        Render::ClassicMinimapViewOptions view; view.layers = input.layers;
        Image::BlpDecodeLimits extraDecode;extraDecode.maxInputBytes=4U*1024U*1024U;
        extraDecode.maxDimension=1024;extraDecode.maxWorkingBytes=32U*1024U*1024U;
        if (input.layers & 0x400u) {
            const auto configBytes = extra("UI/WorldEditData.txt");
            const auto profile = Text::ParseProfile(std::string_view(reinterpret_cast<const char*>(configBytes.data()), configBytes.size()));
            const auto section = profile.find("tilesets");
            const auto key = Text::LowerAscii(std::string(1, static_cast<char>(grid->header.mainTileset)));
            if (section == profile.end() || !section->second.contains(key)) Fail("MINIMAP_CURRENT_BLIGHT", "当前地形集缺少荒芜纹理定义。");
            const auto& value = section->second.at(key); const auto comma = value.find(',');
            if (comma == value.npos) Fail("MINIMAP_CURRENT_BLIGHT", "荒芜纹理定义不完整。");
            auto path = Text::TrimCWhitespaceCopy(value.substr(comma + 1));
            if (!Text::LowerAscii(path).ends_with(".blp")) path += ".blp";
            auto image = Take(Image::DecodeBlpBudgeted(extra(path), extraDecode)).image;
            // 原版荒芜代表色是整张纹理RGB整数均色；与TeamColor共用已有均色器。
            const auto bgra = Take(MapData::AverageNativeMinimapTeamColor(image.rgba));
            view.blightColor = {bgra[2], bgra[1], bgra[0], 255};
        }
        view.overlay = [&](Image::DecodedImage& raw, const auto& rectangle, const auto& header) -> Status {
            if (!(input.layers & 0x20u)) return Status::Ok();
            if (!document.Regions()) {
                if (document.Member(MapDataKind::Regions).state == MapDataState::Missing) return Status::Ok();
                return Status::Failure("Minimap.Preview", "MINIMAP_CURRENT_REGIONS", "区域来源未完整读取。");
            }
            std::size_t work = 0;
            for (const auto& region : document.Regions()->entries) {
                if (!std::isfinite(region.left) || !std::isfinite(region.right) || !std::isfinite(region.top)
                    || !std::isfinite(region.bottom) || region.left >= region.right || region.bottom >= region.top)
                    return Status::Failure("Minimap.Preview", "MINIMAP_CURRENT_REGIONS", "区域边界无效。");
                for (std::uint32_t y = 0; y < raw.height; ++y) for (std::uint32_t x = 0; x < raw.width; ++x) {
                    if (++work > 8U * 1024U * 1024U) return Status::Failure("Minimap.Preview", "MINIMAP_PREVIEW_BUDGET", "区域叠加超过工作预算。");
                    const auto wx = header.offset[0] + static_cast<double>(rectangle[0] + x) * 128;
                    const auto wy = header.offset[1] + static_cast<double>(rectangle[1] + y) * 128;
                    if (wx < region.left || wx >= region.right || wy < region.bottom || wy >= region.top) continue;
                    const auto at = (static_cast<std::size_t>(y) * raw.width + x) * 4;
                    for (std::size_t c = 0; c < 3; ++c) raw.rgba[at+c] = static_cast<std::byte>(
                        (3U * std::to_integer<unsigned>(raw.rgba[at+c]) + region.colorBgra[2-c]) / 4U);
                }
            }
            return Status::Ok();
        };
        Render::TerrainMinimapPostprocessSettings post;
        post.noiseSeed = Take(MinimapContentNoiseSeed(assets, post, 128U * 1024U * 1024U));
        auto candidate = Take(Render::BuildClassicMinimapView(assets, view, post));
        MinimapPreview output; output.current = output.present = true; output.sourceSha256 = input.archive->SourceSha256();
        output.documentRevision = input.documentRevision; output.layers = input.layers;
        output.sourceRectangle = candidate.manifest.sourceRectangle; output.terrainOffset = grid->header.offset;
        const auto frame = Take(MapData::NativeMinimapFrame(*grid, output.sourceRectangle));
        output.worldRectangle = frame.worldRectangle; output.pixelRectangle = frame.pixelRectangle;
        // 与保存后真正解码的JPEG保持同一像素语义，不拿编码前RGBA冒充保存图。
        output.image = Take(Image::DecodeBlp(candidate.encoded.bytes));
        if (input.units || input.starts || input.icons) {
            const auto drawDot = [&](int x, int y, std::array<std::uint8_t,4> bgra) {
                for (int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                    if(x+dx<0||x+dx>=256||y+dy<0||y+dy>=256) continue;
                    const auto at=(static_cast<std::size_t>(y+dy)*256+x+dx)*4;
                    for(std::size_t c=0;c<3;++c) output.image.rgba[at+c]=static_cast<std::byte>(bgra[2-c]);
                    output.image.rgba[at+3]=std::byte{255};
                }
            };
            std::array<std::optional<std::array<std::uint8_t,4>>,16> colors;
            for(const auto& unit:assets.Units().units) {
                const bool isStart=unit.rawcode=="sloc";
                if(isStart ? !input.starts : !input.units) continue;
                if(unit.owner<0||unit.owner>=16) Fail("MINIMAP_CURRENT_PLAYER", "单位玩家编号无法解释。");
                auto& color=colors[unit.owner];
                if(!color){char path[80];std::snprintf(path,sizeof(path),"ReplaceableTextures/TeamColor/TeamColor%02d.blp",unit.owner);
                    auto image=Take(Image::DecodeBlpBudgeted(extra(path),extraDecode)).image;color=Take(MapData::AverageNativeMinimapTeamColor(image.rgba));}
                const MapData::NativeMinimapMarkerCandidate marker{0,{unit.position[0],unit.position[1]},*color};
                const auto dots=Take(MapData::BuildNativeMinimapMarkers(frame,std::span(&marker,1)));
                drawDot(dots.records[0].positionRaw[0],dots.records[0].positionRaw[1],*color);
            }
            if(input.icons){const std::array<std::string,1> names{"war3map.mmp"};auto captured=Take(CaptureObjectCatalogSources(sources,names,4U*1024U*1024U));
                auto file=Take(captured.Read(names[0]));
                if(file){auto icons=Take(MapData::ParseMapMinimapMarkers(file->bytes));for(const auto& icon:icons.records)drawDot(icon.positionRaw[0],icon.positionRaw[1],icon.colorBgra);}
                Check(captured.Verify());}
        }
        output.rgbaSha256 = Take(Sha256(output.image.rgba));
        Check(input.archive->VerifySourceUnchanged());
        output.generationMilliseconds = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        return Result<MinimapPreview>::Success(std::move(output));
    } catch (Failure& failure) { return {{},std::move(failure.diagnostics)}; }
    catch(const std::bad_alloc&){return Result<MinimapPreview>::Failure("Minimap.Preview","MINIMAP_PREVIEW_BUDGET","当前小地图预算不足。");}
}

Result<MinimapPreview> OpenCurrentMinimapPreview(const std::filesystem::path& path,std::string_view expected,
    const std::filesystem::path& gameRoot,std::uint32_t layers,bool units,bool starts,bool icons) {
    if(expected.size()!=64 || !std::all_of(expected.begin(),expected.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))
        return Result<MinimapPreview>::Failure("Minimap.Preview","MINIMAP_PREVIEW_IDENTITY","需要小写完整SHA-256。");
    const auto extension=Text::LowerAscii(path.extension().string());
    if(extension!=".w3x"&&extension!=".w3m")return Result<MinimapPreview>::Failure("Minimap.Preview","MINIMAP_PREVIEW_SOURCE_TYPE","来源须为地图归档。");
    auto archive=MapArchiveSession::OpenMinimapAudit(path);if(!archive.Succeeded())return {{},archive.diagnostics};
    if(archive.value->SourceSha256()!=expected)return Result<MinimapPreview>::Failure("Minimap.Preview","MINIMAP_PREVIEW_STALE","来源身份已变化。");
    auto owner=std::make_shared<const MapArchiveSession>(std::move(*archive.value));
    auto document=MapDocument::FromMembers(owner->Members(),owner->SourcePath());
    MapData::MapDataEditSession edits(document);const std::array<std::string,2> names{"war3map.w3e","war3map.wpm"};
    auto bytes=owner->CaptureExactMembers(names);if(!bytes.Succeeded())return {{},bytes.diagnostics};
    if(!bytes.value->at(0).present)return Result<MinimapPreview>::Failure("Minimap.Preview","MINIMAP_CURRENT_SOURCE","没有地形。");
    auto status=edits.AttachTerrain(bytes.value->at(0).bytes);if(!status.Succeeded())return {{},status.diagnostics};
    if(bytes.value->at(1).present){status=edits.AttachPathing(bytes.value->at(1).bytes);if(!status.Succeeded())return {{},status.diagnostics};}
    auto frozen=edits.FreezeForSave(128U*1024U*1024U);if(!frozen.Succeeded())return {{},frozen.diagnostics};
    CurrentMinimapPreviewInput input;input.archive=std::move(owner);input.edits=*frozen.value;input.gameRoot=gameRoot;
    input.layers=layers;input.units=units;input.starts=starts;input.icons=icons;
    return GenerateCurrentMinimapPreview(input);
}
} // namespace War3Map::Services
