#include "WorldSelectionPresentation.h"

#include "War3MapCore/Render/TerrainPicking.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>

namespace War3Map::Modules {
namespace {
std::optional<float> Number(std::string_view text) {
    float value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}
std::string_view Field(const Text::ProfileSections& profile, const char* section, const std::string& key) {
    const auto found = profile.find(section);
    if (found == profile.end()) return {};
    const auto value = found->second.find(key);
    return value == found->second.end() ? std::string_view{} : std::string_view(value->second);
}
}

Result<std::vector<War3RenderOverlayLineV2>> BuildWorldSelectionOutline(
    const Render::ViewProjectionFrame& completed, const Render::ViewLogicalMetrics& metrics,
    Render::LogicalViewPoint anchor, Render::LogicalViewPoint cursor) {
    using Output = Result<std::vector<War3RenderOverlayLineV2>>;
    const auto fail = [] { return Output::Failure("World.Selection", "WORLD_SELECTION_OUTLINE_INVALID",
        "选框的完成视图、DPI或坐标无效，未绘制旧屏幕边框。"); };
    if (!completed.identity.surfaceEpoch || !completed.identity.sceneEpoch || !completed.identity.viewEpoch ||
        !completed.identity.frameCount || !metrics.width || !metrics.height || !completed.physicalWidth ||
        !completed.physicalHeight || !std::isfinite(metrics.devicePixelRatio) || metrics.devicePixelRatio <= 0 ||
        !std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(cursor.x) || !std::isfinite(cursor.y)) return fail();
    // 鼠标捕获可越出窗口；只截绘制矩形，不伪造拾取坐标。分数DPR须尊重实际栅格上界。
    const double maximumX = std::min(std::nextafter(static_cast<double>(metrics.width), 0.0),
        std::nextafter(static_cast<double>(completed.physicalWidth), 0.0) / metrics.devicePixelRatio);
    const double maximumY = std::min(std::nextafter(static_cast<double>(metrics.height), 0.0),
        std::nextafter(static_cast<double>(completed.physicalHeight), 0.0) / metrics.devicePixelRatio);
    const double left = std::clamp(std::min(anchor.x, cursor.x), 0.0, maximumX);
    const double right = std::clamp(std::max(anchor.x, cursor.x), 0.0, maximumX);
    const double top = std::clamp(std::min(anchor.y, cursor.y), 0.0, maximumY);
    const double bottom = std::clamp(std::max(anchor.y, cursor.y), 0.0, maximumY);
    const std::array<Render::LogicalViewPoint, 4> corners{{{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
    std::array<std::array<float, 3>, 4> points{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto ray = Render::MakeViewRayFromLogical(completed, metrics, corners[i]);
        if (!ray.Succeeded()) return {std::nullopt, ray.diagnostics};
        // 略进near面，避免double→float的舍入把线截到near外；沿同一像素射线不改变屏幕落点。
        const double advance = std::min(1.0, ray.value->maxDistance * 0.001);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double value = ray.value->ray.origin[axis] + ray.value->ray.direction[axis] * advance;
            if (!std::isfinite(value) || std::abs(value) > 10000000) return fail();
            points[i][axis] = static_cast<float>(value);
        }
    }
    std::vector<War3RenderOverlayLineV2> lines(4);
    constexpr std::array<float, 3> color{74.0f / 255, 222.0f / 255, 128.0f / 255};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::copy(points[i].begin(), points[i].end(), lines[i].from);
        const auto next = (i + 1) % points.size();
        std::copy(points[next].begin(), points[next].end(), lines[i].to);
        std::copy(color.begin(), color.end(), lines[i].color);
    }
    return Output::Success(std::move(lines));
}

Result<WorldSelectionCircleAssets> LoadWorldSelectionCircleAssets(Render::AnimationSession session,
    const Render::AnimationResourceReader& read) {
    using Output = Result<WorldSelectionCircleAssets>;
    if (!read || !session.generation || session.sourceIdentity.empty())
        return Output::Failure("World.Selection", "WORLD_SELECTION_SOURCE_INVALID", "选取圈来源或代次无效。");
    WorldSelectionCircleAssets output;
    const auto profile = [&](const char* path) -> Result<Text::ProfileSections> {
        auto resource = read(path, 2ull * 1024 * 1024);
        if (!resource.Succeeded() || !resource.value) return {std::nullopt, std::move(resource.diagnostics)};
        if (resource.value->bytes.size() > 2ull * 1024 * 1024 || resource.value->digest.empty() || resource.value->sourceLayer.empty())
            return Result<Text::ProfileSections>::Failure("World.Selection", "WORLD_SELECTION_SOURCE_INVALID", "选取圈配置缺少可信来源或超预算。");
        output.sources.push_back({path, resource.value->sourceLayer, resource.value->digest});
        auto text = std::string_view(reinterpret_cast<const char*>(resource.value->bytes.data()), resource.value->bytes.size());
        if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        return Result<Text::ProfileSections>::Success(Text::ParseProfile(text));
    };
    auto ui = profile("UI\\MiscData.txt"), units = profile("Units\\MiscData.txt");
    if (!ui.Succeeded() || !units.Succeeded()) {
        auto notes = std::move(ui.diagnostics); notes.insert(notes.end(), units.diagnostics.begin(), units.diagnostics.end());
        return {std::nullopt, std::move(notes)};
    }
    const auto count = Number(Field(*ui.value, "selectioncircle", "numsizes"));
    const auto factor = Number(Field(*ui.value, "selectioncircle", "scalefactor"));
    const auto base = Number(Field(*units.value, "misc", "selectioncirclebasez"));
    if (!count || *count < 1 || *count > 16 || std::floor(*count) != *count || !factor || *factor <= 0 || *factor > 1000000 || !base || std::abs(*base) > 1000000)
        return Output::Failure("World.Selection", "WORLD_SELECTION_CONFIG_INVALID", "选取圈数量、比例或基础高度缺失/无效，未猜默认圈。");
    output.scaleFactor = *factor; output.baseHeight = *base;
    const auto color = Text::SplitPlain(Field(*ui.value, "selectioncircle", "colorfriend"), ',');
    if (color.size() != 4) return Output::Failure("World.Selection", "WORLD_SELECTION_CONFIG_INVALID", "选取圈友方颜色无效。");
    for (std::size_t i = 0; i < 4; ++i) {
        const auto channel = Number(Text::TrimHorizontal(color[i]));
        if (!channel || *channel < 0 || *channel > 255) return Output::Failure("World.Selection", "WORLD_SELECTION_CONFIG_INVALID", "选取圈颜色分量无效。");
        if (i == 0) output.alpha = *channel / 255; else output.tint[i - 1] = *channel / 255;
    }
    auto scene = std::make_shared<Render::AnimatedModelScene>();
    scene->sceneVersion = Render::NextAnimationSerial(); scene->generation = session.generation;
    scene->documentRevision = session.documentRevision; scene->sourceIdentity = session.sourceIdentity;
    const std::array<War3RenderModelVertexV2, 4> plane{{
        {-0.5f,-0.5f,0,0,0,1,0,1}, {0.5f,-0.5f,0,0,0,1,1,1},
        {-0.5f,0.5f,0,0,0,1,0,0}, {0.5f,0.5f,0,0,0,1,1,0}}};
    for (std::uint32_t i = 0; i < plane.size(); ++i) {
        scene->vertices.push_back({plane[i], i}); scene->geometry.vertices.push_back(plane[i]);
    }
    float previousMaximum = 0;
    std::map<std::string, std::uint32_t> textures;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(*count); ++i) {
        const auto suffix = (i < 10 ? std::string("0") : std::string{}) + std::to_string(i);
        const auto maximum = i + 1 == static_cast<std::uint32_t>(*count) ? std::optional<float>(std::numeric_limits<float>::max())
            : Number(Field(*ui.value, "selectioncircle", "size" + suffix));
        if (!maximum || *maximum <= previousMaximum)
            return Output::Failure("World.Selection", "WORLD_SELECTION_CONFIG_INVALID", "选取圈尺寸分档不是严格递增。");
        previousMaximum = *maximum;
        std::string path(Field(*ui.value, "selectioncircle", "texture" + suffix));
        if (path.empty()) return Output::Failure("World.Selection", "WORLD_SELECTION_CONFIG_INVALID", "选取圈纹理路径缺失。");
        const auto slash = path.find_last_of("\\/");
        if (path.find('.', slash == std::string::npos ? 0 : slash) == std::string::npos) path += ".blp";
        auto found = textures.find(path);
        if (found == textures.end()) {
            auto resource = read(path, 16ull * 1024 * 1024);
            if (!resource.Succeeded() || !resource.value) return {std::nullopt, std::move(resource.diagnostics)};
            if (resource.value->bytes.size() > 16ull * 1024 * 1024 || resource.value->digest.empty() || resource.value->sourceLayer.empty())
                return Output::Failure("World.Selection", "WORLD_SELECTION_SOURCE_INVALID", "选取圈纹理缺少可信来源或超预算。");
            auto decoded = Render::DecodeModelTexture(path, resource.value->bytes, 512);
            if (!decoded.Succeeded()) return {std::nullopt, std::move(decoded.diagnostics)};
            output.sources.push_back({path, resource.value->sourceLayer, resource.value->digest});
            found = textures.emplace(path, static_cast<std::uint32_t>(scene->geometry.textures.size())).first;
            scene->geometry.textures.push_back(std::move(*decoded.value));
        }
        scene->geometry.layers.push_back({found->second, 2, 5, 1}); // 真实alpha纹理，无光照且不写深度。
        // 渲染核心要求各网格的索引区间首尾相接、互不重叠：每档圈复制一份 6 个索引，顶点仍共用。
        const auto firstIndex = static_cast<std::uint32_t>(scene->geometry.indices.size());
        for (const auto index : {0,1,2,1,3,2}) scene->geometry.indices.push_back(static_cast<std::uint16_t>(index));
        const War3RenderModelMeshV2 mesh{firstIndex,6,0,i,1};
        scene->geometry.meshes.push_back(mesh); scene->geometry.models.push_back({i,1});
        scene->meshes.push_back({mesh,0,4}); scene->models.push_back({{i,1},4});
        output.bands.push_back({*maximum, i});
    }
    output.scene = std::move(scene);
    return Output::Success(std::move(output));
}

Result<WorldSelectionCircleFrame> BuildWorldSelectionCircleFrame(const WorldSelectionCircleAssets& assets,
    std::span<const Render::ModelInstanceRequest> requests, const MapData::TerrainGridSnapshot* terrain,
    std::uint64_t contentVersion, const std::function<std::optional<float>(double, double)>& waterHeight) {
    using Output = Result<WorldSelectionCircleFrame>;
    if (!assets.scene || assets.bands.empty() || !contentVersion || !std::isfinite(assets.scaleFactor) || assets.scaleFactor <= 0 ||
        !std::isfinite(assets.baseHeight) || std::abs(assets.baseHeight) > 1000000 ||
        !std::isfinite(assets.alpha) || assets.alpha < 0 || assets.alpha > 1 ||
        !std::all_of(assets.tint.begin(), assets.tint.end(), [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; }))
        return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_INVALID", "选取圈资产或帧身份无效。");
    float previousMaximum = 0;
    for (const auto& band : assets.bands) {
        if (!std::isfinite(band.maximumSize) || band.maximumSize <= previousMaximum || band.model >= assets.scene->models.size())
            return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_INVALID", "选取圈分档或模型身份无效。");
        previousMaximum = band.maximumSize;
    }
    // 限制本次实际帧载荷，不限制地图实例总数，也不预留固定实例容量。
    constexpr std::size_t perInstance = sizeof(War3RenderSkinnedModelInstanceV2) + 4 * sizeof(War3RenderGroupMatrixV2) + sizeof(float);
    if (requests.size() > (64ull * 1024 * 1024) / perInstance || requests.size() > 1048576 / 4)
        return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_BUDGET", "本次选取圈帧超过实际字节或矩阵预算。");
    struct Entry { std::uint32_t request, model; float size; };
    WorldSelectionCircleFrame output; std::vector<Entry> entries;
    for (std::uint32_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (!request.selection || !request.selection->circleScaleKnown) { ++output.unknownScale; continue; }
        const float size = request.selection->circleScale * assets.scaleFactor;
        if (!std::isfinite(size) || size < 0 || size > 10000000 || !std::isfinite(request.selection->circleHeight))
            return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_INVALID", "选取圈尺寸或高度不是可显示的有限值。");
        if (size == 0) continue;
        const auto band = std::find_if(assets.bands.begin(), assets.bands.end(), [size](const auto& item) { return size <= item.maximumSize; });
        if (band == assets.bands.end()) return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_INVALID", "选取圈尺寸没有合法分档。");
        entries.push_back({i, band->model, size});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.model < b.model; });
    auto frame = std::make_shared<Render::AnimationFrame>();
    frame->sceneVersion = assets.scene->sceneVersion; frame->documentRevision = assets.scene->documentRevision;
    frame->contentVersion = contentVersion;
    for (const auto& entry : entries) {
        const auto& request = requests[entry.request]; const auto& selection = *request.selection;
        if (!std::all_of(request.position.begin(), request.position.end(), [](float v) { return std::isfinite(v); }))
            return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_INVALID", "选取圈位置不是有限值。");
        const auto x = request.position[0], y = request.position[1];
        const auto center = terrain ? Render::SampleTerrainHeight(*terrain, x, y) : std::nullopt;
        float rawZ = request.position[2] - (selection.heightPreviewApproximate ? selection.flyHeightOffset : 0);
        // doo原Z可以低于当前显示地表。圈采用采样地面作近似锚点，保留更高的原Z及selZ，绝不写回地图。
        if (center) rawZ = std::max(rawZ, *center);
        bool onWater = false;
        if (selection.circleOnWater) {
            if (waterHeight) {
                if (auto water = waterHeight(x, y); water && std::isfinite(*water) && *water > rawZ) { rawZ = *water; onWater = true; }
            } else ++output.waterSemanticsPending;
        }
        const float z = rawZ + selection.circleHeight + assets.baseHeight;
        if (!std::isfinite(z)) return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_INVALID", "选取圈最终高度无效。");
        const auto firstGroup = static_cast<std::uint32_t>(frame->groups.size()), firstAlpha = static_cast<std::uint32_t>(frame->alphas.size());
        frame->instances.push_back({{entry.model,{entry.size,0,0,x,0,entry.size,0,y,0,0,1,z},
            {assets.tint[0],assets.tint[1],assets.tint[2]}},firstGroup,firstAlpha});
        constexpr std::array<std::array<float,2>,4> corners{{{-0.5f,-0.5f}, {0.5f,-0.5f}, {-0.5f,0.5f}, {0.5f,0.5f}}};
        for (const auto& corner : corners) {
            const auto ground = terrain && !onWater ? Render::SampleTerrainHeight(*terrain, x + corner[0] * entry.size, y + corner[1] * entry.size) : std::nullopt;
            const float lift = ground && center ? *ground - *center : 0;
            if (!std::isfinite(lift) || std::abs(lift) > 10000000)
                return Output::Failure("World.Selection", "WORLD_SELECTION_FRAME_INVALID", "选取圈地表采样高度无效。");
            frame->groups.push_back({{1,0,0,0,1,0,0,0,1,0,0,lift}});
            if (!onWater && !ground) ++output.terrainUnavailable;
        }
        frame->alphas.push_back(assets.alpha); output.requestIndices.push_back(entry.request);
    }
    output.frame = std::move(frame); return Output::Success(std::move(output));
}
} // namespace War3Map::Modules
