#pragma once

#include <nlohmann/json.hpp>

// 原创地表输入，只通过既有生产ABI/CaptureFrame观察；CPU几何仅用于ROI和输入资格，不渲染参考图。
namespace TerrainObliqueTest {
using Json = nlohmann::ordered_json;
constexpr std::uint32_t kViewport = 512, kAtlas = 512, kMaximumMip = 7;
constexpr double kContrast = 112.0;

struct Source {
    std::vector<std::vector<std::uint8_t>> bytes;
    std::vector<War3RenderMipRgbaV2> levels;
    Json identity = Json::array();
    War3RenderTextureMipChainV2 chain{};
    bool qualityMarker = true;
};

Source MakeSource(std::uint32_t period, bool alternateOutside = false, bool solid = false,
    std::uint8_t alpha = 255, bool constantColor = false) {
    Source output; output.qualityMarker = !constantColor; output.bytes.reserve(10); output.levels.reserve(10);
    for (std::uint32_t level = 0, size = kAtlas;; ++level, size = std::max(1u,size / 2)) {
        auto& pixels = output.bytes.emplace_back(static_cast<std::size_t>(size) * size * 4);
        const std::array<std::uint8_t,4> outside = alternateOutside
            ? std::array<std::uint8_t,4>{255,0,255,0} : std::array<std::uint8_t,4>{0,255,0,255};
        for (std::size_t at = 0; at < pixels.size(); at += 4)
            std::copy(outside.begin(),outside.end(),pixels.begin() + static_cast<std::ptrdiff_t>(at));
        if (level <= kMaximumMip) {
            const auto cell = size / 4;
            for (std::uint32_t y = cell; y < 2 * cell; ++y) for (std::uint32_t x = cell; x < 2 * cell; ++x) {
                const auto at = (static_cast<std::size_t>(y) * size + x) * 4;
                const auto stripe = period == 2 ? 1u : 2u;
                const bool detailed = level < (period == 2 ? 1u : 2u);
                pixels[at] = constantColor ? 200 : solid || !detailed ? 128
                    : (((x - cell) / (stripe >> level)) & 1u) ? 16 : 240;
                pixels[at + 1] = constantColor ? 120 : 96;
                pixels[at + 2] = constantColor ? 40 : period == 2 ? (level == 0 ? 160 : 80)
                    : level == 0 ? 160 : level == 1 ? 96 : 32;
                pixels[at + 3] = alpha;
            }
        }
        output.levels.push_back({sizeof(War3RenderMipRgbaV2),size,size,pixels.size(),pixels.data()});
        const auto hash = War3Map::Sha256(std::as_bytes(std::span(pixels)));
        Require(hash.Succeeded(),"terrain oblique original source hash failed");
        output.identity.push_back({{"level",level},{"width",size},{"height",size},{"rgba_sha256",*hash.value}});
        if (size == 1) break;
    }
    output.chain = {sizeof(War3RenderTextureMipChainV2),static_cast<std::uint32_t>(output.levels.size()),
        kMaximumMip,4,4,output.levels.data()};
    return output;
}

struct Point { double x, y; };
Point PlanePoint(const War3RenderViewInfoV2& view, double x, double y) {
    const auto* m = view.viewProjection;
    const double sx = 2 * x / view.width - 1, sy = 1 - 2 * y / view.height;
    const double a = m[0] - sx * m[3], b = m[4] - sx * m[7], c = sx * m[15] - m[12];
    const double d = m[1] - sy * m[3], e = m[5] - sy * m[7], f = sy * m[15] - m[13], determinant = a * e - b * d;
    Require(std::isfinite(determinant) && determinant != 0,"terrain oblique plane projection singular");
    return {(c * e - b * f) / determinant,(a * f - c * d) / determinant};
}

struct Footprint {
    double u, v, major, minor, lod;
    std::array<double,2> axis;
    std::uint32_t taps;
};
Footprint QualificationFootprint(const War3RenderViewInfoV2& view, std::uint32_t x, std::uint32_t y) {
    const auto p = PlanePoint(view,x + .5,y + .5), r = PlanePoint(view,x + 1.5,y + .5), d = PlanePoint(view,x + .5,y + 1.5);
    const double dxU = (r.x - p.x) * 127 / 128, dxV = -(r.y - p.y) * 127 / 128;
    const double dyU = (d.x - p.x) * 127 / 128, dyV = -(d.y - p.y) * 127 / 128;
    const double a = dxU * dxU + dyU * dyU, b = dxU * dxV + dyU * dyV, c = dxV * dxV + dyV * dyV;
    const double eigenvalue = .5 * (a + c + std::hypot(a - c,2 * b)), major = std::sqrt(eigenvalue);
    const double minor = std::abs(dxU * dyV - dxV * dyU) / major;
    std::array<double,2> axis{b,eigenvalue - a};
    if (std::hypot(axis[0],axis[1]) < std::hypot(eigenvalue - c,b)) axis = {eigenvalue - c,b};
    const double length = std::hypot(axis[0],axis[1]); axis[0] /= length; axis[1] /= length;
    const double width = std::max({minor,major / 4,1.0});
    return {(p.x + 64) / 128,(64 - p.y) / 128,major,minor,std::clamp(std::log2(width),0.0,7.0),axis,
        static_cast<std::uint32_t>(std::clamp(std::ceil(major / width),1.0,4.0))};
}
Footprint ExpectedFootprint(const War3RenderViewInfoV2& view, std::uint32_t x, std::uint32_t y) {
    // DXBC使用coarse导数：quad左上中心求dx/dy，当前fragment仍保留自己的采样中心。
    auto output = QualificationFootprint(view,x & ~1u,y & ~1u);
    const auto p = PlanePoint(view,x + .5,y + .5);
    output.u = (p.x + 64) / 128; output.v = (64 - p.y) / 128;
    return output;
}

struct Analysis {
    std::vector<std::size_t> roi, white, black;
    std::array<std::size_t,4> edge{}, corner{}; // W,E,S,N；SW,SE,NW,NE。仅输入足迹资格。
    double majorMin = 0, majorMax = 0, minorMin = 0, minorMax = 0, lodMin = 0, lodMax = 0;
    std::uint32_t tapMin = 0, tapMax = 0;
};
Analysis Analyze(const War3RenderViewInfoV2& view, std::uint32_t period, bool oblique) {
    Analysis output;
    for (std::uint32_t y = 0; y < view.height; ++y) for (std::uint32_t x = 0; x < view.width; ++x) {
        // 原forward仅用于输入资格，保持原UV ROI/phase选择，不冒充GPU导数遥测。
        const auto f = QualificationFootprint(view,x,y);
        if (!std::isfinite(f.u) || !std::isfinite(f.v) || f.u < 0 || f.u > 1 || f.v < 0 || f.v > 1) continue;
        Require(std::isfinite(f.major) && std::isfinite(f.minor) && f.minor > 0,"terrain oblique input footprint invalid");
        const auto expected = ExpectedFootprint(view,x,y);
        std::array<bool,4> edges{}, corners{};
        const auto low = static_cast<std::uint32_t>(std::floor(expected.lod)), high = std::min(kMaximumMip,low + 1);
        for (const auto level : {low,high}) {
            if (level == high && (high == low || expected.lod == std::floor(expected.lod))) continue; // 零权重层不冒充边界覆盖。
            const double scale = double(1u << level), minimum = 128 + .5 * scale, maximum = 256 - .5 * scale;
            for (std::uint32_t tap = 0; tap < expected.taps; ++tap) {
                const double offset = ((tap + .5) / expected.taps - .5) * expected.major;
                const double px = 128.5 + expected.u * 127 + expected.axis[0] * offset;
                const double py = 128.5 + expected.v * 127 + expected.axis[1] * offset;
                const bool w = px < minimum, e = px > maximum, n = py < minimum, s = py > maximum;
                edges[0] |= w; edges[1] |= e; edges[2] |= s; edges[3] |= n;
                corners[0] |= w && s; corners[1] |= e && s; corners[2] |= w && n; corners[3] |= e && n;
            }
        }
        for (std::size_t i = 0; i < 4; ++i) { output.edge[i] += edges[i]; output.corner[i] += corners[i]; }
        if (f.u < .3 || f.u > .7 || f.v < .3 || f.v > .7) continue;
        const double ratio = f.major / f.minor;
        if (oblique && (f.major < (period == 2 ? 2.25 : 4.5) || f.major > (period == 2 ? 3 : 6)
            || f.minor < (period == 2 ? .25 : .5) || f.minor > (period == 2 ? .375 : .75) || ratio < 6 || ratio > 10)) continue;
        if (output.roi.empty()) {
            output.majorMin = output.majorMax = expected.major; output.minorMin = output.minorMax = expected.minor;
            output.lodMin = output.lodMax = expected.lod; output.tapMin = output.tapMax = expected.taps;
        } else {
            output.majorMin = std::min(output.majorMin,expected.major); output.majorMax = std::max(output.majorMax,expected.major);
            output.minorMin = std::min(output.minorMin,expected.minor); output.minorMax = std::max(output.minorMax,expected.minor);
            output.lodMin = std::min(output.lodMin,expected.lod); output.lodMax = std::max(output.lodMax,expected.lod);
            output.tapMin = std::min(output.tapMin,expected.taps); output.tapMax = std::max(output.tapMax,expected.taps);
        }
        const auto at = (static_cast<std::size_t>(y) * view.width + x) * 4;
        output.roi.push_back(at);
        const double phase = std::fmod(.5 + f.u * 127,double(period)), margin = double(period) / 16;
        if (std::abs(phase - double(period) / 4) <= margin) output.white.push_back(at);
        if (std::abs(phase - double(period) * .75) <= margin) output.black.push_back(at);
    }
    return output;
}

struct Observation {
    std::string name;
    CapturedFrame frame;
    Analysis analysis;
    double white = 0, black = 0;
    std::size_t foreground = 0;
    bool opaque = true, covered = true;
};
bool Foreground(const std::vector<std::uint8_t>& pixels, std::size_t at) { return pixels[at + 1] == 96; }
Observation Observe(std::string name, CapturedFrame frame, std::uint32_t period, bool oblique) {
    Observation output; output.name = std::move(name); output.frame = std::move(frame);
    output.analysis = Analyze(output.frame.receipt.view,period,oblique);
    for (std::size_t at = 0; at < output.frame.pixels.size(); at += 4) {
        output.opaque &= output.frame.pixels[at + 3] == 255; output.foreground += Foreground(output.frame.pixels,at);
    }
    for (const auto at : output.analysis.roi) output.covered &= Foreground(output.frame.pixels,at);
    const auto mean = [&](const auto& positions) {
        std::uint64_t sum = 0; for (const auto at : positions) sum += output.frame.pixels[at];
        return positions.empty() ? 0.0 : double(sum) / double(positions.size());
    };
    output.white = mean(output.analysis.white); output.black = mean(output.analysis.black);
    return output;
}

std::filesystem::path OutputDirectory() {
    constexpr auto variable = L"WAR3_RENDER_TERRAIN_OBLIQUE_AUDIT_OUTPUT";
    SetLastError(ERROR_SUCCESS); const auto count = GetEnvironmentVariableW(variable,nullptr,0);
    if (!count) {
        Require(GetLastError() == ERROR_SUCCESS || GetLastError() == ERROR_ENVVAR_NOT_FOUND,"terrain oblique output environment unavailable");
        return {};
    }
    Require(count < 32768,"terrain oblique output path too long");
    std::vector<wchar_t> buffer(count);
    Require(GetEnvironmentVariableW(variable,buffer.data(),count) == count - 1,"terrain oblique output environment changed");
    const std::filesystem::path path(buffer.data());
    Require(path.is_absolute() && !std::filesystem::exists(path) && std::filesystem::create_directory(path),
        "terrain oblique output must be a new absolute directory");
    return path;
}

void Export(const std::filesystem::path& directory, const Observation& observation, const Source& source,
    const char* policy, std::uint32_t period, std::uint32_t software,
    const War3RenderTextureMipChainV2* submitted = nullptr, bool textureProvided = true) {
    if (directory.empty()) return;
    const auto& frame = observation.frame; const auto& receipt = frame.receipt; const auto& camera = receipt.camera;
    std::ofstream raw(directory / (observation.name + ".rgba"),std::ios::binary);
    raw.write(reinterpret_cast<const char*>(frame.pixels.data()),static_cast<std::streamsize>(frame.pixels.size()));
    raw.close(); Require(raw.good(),"terrain oblique raw export failed");
    SaveBmp(directory / (observation.name + ".bmp"),frame.pixels,kViewport,kViewport);
    const auto hash = War3Map::Sha256(std::as_bytes(std::span(frame.pixels)));
    Require(hash.Succeeded(),"terrain oblique frame hash failed");
    const auto& a = observation.analysis;
    Json result{{"schema","terrain-k4-oblique-fixture-v1"},{"case",observation.name},
        {"scope","original-bounded-terrain-filter-not-native-map-acceptance"},{"requested_policy",policy},{"software",software},
        {"rgba_sha256",*hash.value},{"width",receipt.capture.width},{"height",receipt.capture.height},
        {"frame",receipt.capture.frameCount},{"ticket",receipt.capture.ticket},{"revision",receipt.capture.documentRevision},
        {"surface_epoch",receipt.view.surfaceEpoch},{"scene_epoch",receipt.view.sceneEpoch},{"view_epoch",receipt.view.viewEpoch},
        {"camera",{camera.targetX,camera.targetY,camera.targetZ,camera.distance,camera.yawRadians,camera.pitchRadians}},
        {"view_projection",std::vector<float>(std::begin(receipt.view.viewProjection),std::end(receipt.view.viewProjection))},
        {"source",{{"generated_atlas_columns",4},{"generated_atlas_rows",4},{"cell",{1,1}},{"period",period},
            {"generated_mips",source.identity},{"texture_provided",textureProvided}}},
        {"roi",{{"count",a.roi.size()},{"white_count",a.white.size()},{"black_count",a.black.size()},
            {"qualification_derivative_definition","per-pixel-forward-plane-difference-geometry-only"},
            {"expectation_derivative_definition","quad-top-left-coarse"},{"quad_anchor","(x & ~1u, y & ~1u)"},
            {"expected_major_range",{a.majorMin,a.majorMax}},{"expected_minor_range",{a.minorMin,a.minorMax}},
            {"expected_candidate_lod_range",{a.lodMin,a.lodMax}},{"expected_candidate_taps",{a.tapMin,a.tapMax}},
            {"expectation_is_gpu_telemetry",false},{"clamp_sensitive_edges_w_e_s_n",a.edge},{"clamp_sensitive_corners_sw_se_nw_ne",a.corner}}},
        {"white_mean",observation.white},{"black_mean",observation.black},{"contrast",observation.white - observation.black},
        {"contrast_threshold",kContrast},{"minimum_phase_samples",period == 2 ? 128 : 64},
        {"opaque",observation.opaque},{"quality_marker_applicable",source.qualityMarker && textureProvided},
        {"roi_covered",source.qualityMarker && textureProvided ? Json(observation.covered) : Json(nullptr)},
        {"foreground_count",source.qualityMarker && textureProvided ? Json(observation.foreground) : Json(nullptr)}};
    if (textureProvided) {
        const auto& descriptor = submitted ? *submitted : source.chain;
        result["source"]["submitted_descriptor"] = {{"mip_count",descriptor.mipCount},{"max_sample_mip",descriptor.maxSampleMip},
            {"atlas_columns",descriptor.atlasColumns},{"atlas_rows",descriptor.atlasRows}};
    } else result["source"]["submitted_descriptor"] = nullptr;
    std::ofstream stream(directory / (observation.name + ".json"),std::ios::binary);
    stream << result.dump(2) << '\n'; stream.close(); Require(stream.good(),"terrain oblique metrics export failed");
}

bool SameCoverage(const Observation& a, const Observation& b) {
    if (a.frame.pixels.size() != b.frame.pixels.size()) return false;
    for (std::size_t at = 0; at < a.frame.pixels.size(); at += 4)
        if (Foreground(a.frame.pixels,at) != Foreground(b.frame.pixels,at)) return false;
    return true;
}

} // namespace TerrainObliqueTest

void TestTerrainObliqueFilter(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& originalInfo) {
    using namespace TerrainObliqueTest;
    auto info = originalInfo; info.width = info.height = kViewport;
    RendererOwner renderer{api,api->createRenderer(&info)};
    Require(renderer.handle && api->submitTerrainMipScene && api->submitTerrainScene && api->setEnvironment,
        "terrain oblique production capabilities missing");
    const auto directory = OutputDirectory();
    const War3RenderEnvironmentV2 environment{sizeof(environment),{0,0,1},{1,1,1},{0,0,0},
        WAR3_RENDER_FOG_NONE,0,1,0,{0,0,0},0,0,0,0,1,0,nullptr};
    Require(api->setEnvironment(renderer.handle,&environment) == 0,"terrain oblique environment rejected");
    auto fine = MakeSource(2), fineOutside = MakeSource(2,true), fractional = MakeSource(4), fractionalOutside = MakeSource(4,true);
    auto solid = MakeSource(2,false,true), alpha0 = MakeSource(2,false,true,0,true), alpha128 = MakeSource(2,false,true,128,true);
    constexpr float lowUv = 128.5f / 512, highUv = 255.5f / 512;
    const std::array<War3RenderTerrainVertexV2,4> vertices{{
        {-64,-64,0,1,1,1,lowUv,highUv},{64,-64,0,1,1,1,highUv,highUv},
        {-64,64,0,1,1,1,lowUv,lowUv},{64,64,0,1,1,1,highUv,lowUv}}};
    const std::array<std::uint32_t,6> indices{0,1,2,1,3,2};
    War3RenderCameraV2 camera{sizeof(camera),0,0,0,200,-1.5707963267948966f,.12532783f};
    auto farCamera = camera; farCamera.distance = 400;
    auto reverse = farCamera; reverse.yawRadians = 1.5707963267948966f;
    auto front = camera; front.pitchRadians = 1.4f;
    std::uint64_t revision = 9940;
    const auto capture = [&](const char* name, Source& source, const War3RenderCameraV2& view, std::uint32_t period,
        bool oblique = true, bool legacy = false, bool mip0 = false, bool translucent = false) {
        const War3RenderTerrainBatchV2 batch{0,6,translucent ? WAR3_RENDER_TERRAIN_TRANSLUCENT : 0u};
        auto chain = source.chain; if (mip0) chain.maxSampleMip = 0;
        if (legacy) { chain.mipCount = 1; chain.maxSampleMip = 0; chain.atlasColumns = chain.atlasRows = 1; }
        Require(api->setCamera(renderer.handle,&view) == 0,"terrain oblique camera rejected");
        const auto documentRevision = ++revision;
        if (legacy) {
            const auto& top = source.levels.front();
            const War3RenderTextureRgbaV2 texture{sizeof(texture),top.width,top.height,top.rgbaByteCount,top.rgba};
            const War3RenderTerrainSceneV2 scene{sizeof(scene),4,6,1,1,documentRevision,vertices.data(),indices.data(),&texture,&batch};
            Require(api->submitTerrainScene(renderer.handle,&scene) == 0,"terrain oblique legacy input rejected");
        } else {
            const War3RenderTerrainMipSceneV2 scene{sizeof(scene),4,6,1,1,documentRevision,vertices.data(),indices.data(),&chain,&batch};
            Require(api->submitTerrainMipScene(renderer.handle,&scene) == 0,"terrain oblique modern input rejected");
        }
        auto result = Observe(name,CaptureFrame(renderer,kViewport,kViewport,documentRevision,view),period,oblique);
        War3RenderStatusV2 status{sizeof(status)};
        Require(api->getStatus(renderer.handle,&status) == 0 && !status.result && status.deviceReady == 1
            && status.software == info.forceWarp && status.documentRevision == documentRevision,
            "terrain oblique completed device identity mismatch");
        Export(directory,result,source,legacy ? "legacy-exact-one" : mip0 ? "modern-mip0-exact-one"
            : translucent ? "explicit-translucent-exact-one" : "modern-nontranslucent-k4",period,info.forceWarp,&chain);
        std::cout << name << " contrast=" << result.white - result.black << " samples=" << result.analysis.white.size()
            << '/' << result.analysis.black.size() << " covered=" << result.covered << '\n';
        return result;
    };
    // 全部控制、哨兵和alpha帧在最终质量失败之前完成，旧provider反证也保有完整工件。
    const auto fineFrame = capture("oblique-period2",fine,camera,2);
    const auto fineSentinel = capture("oblique-period2-alternate-outside",fineOutside,camera,2);
    const auto fractionalFrame = capture("oblique-period4-fractional",fractional,farCamera,4);
    const auto fractionalSentinel = capture("oblique-period4-alternate-outside",fractionalOutside,farCamera,4);
    const auto reverseFrame = capture("oblique-period4-reverse",fractional,reverse,4);
    const auto reverseSentinel = capture("oblique-period4-reverse-outside",fractionalOutside,reverse,4);
    const auto frontFrame = capture("front-n1-control",fine,front,2,false);
    const auto solidFrame = capture("oblique-solid-control",solid,camera,2);
    const auto legacyFrame = capture("legacy-control",fine,camera,2,true,true);
    const auto mip0Frame = capture("modern-mip0-control",fine,camera,2,true,false,true);
    const auto translucentFrame = capture("translucent-control",fine,camera,2,true,false,false,true);

    std::array<War3RenderTerrainVertexV2,8> alphaVertices{};
    std::copy(vertices.begin(),vertices.end(),alphaVertices.begin()); std::copy(vertices.begin(),vertices.end(),alphaVertices.begin() + 4);
    constexpr std::array<int,3> baseColor{20,40,60}, sourceColor{200,120,40};
    for (std::size_t i = 0; i < 4; ++i) {
        alphaVertices[i].red = float(baseColor[0]) / 255; alphaVertices[i].green = float(baseColor[1]) / 255;
        alphaVertices[i].blue = float(baseColor[2]) / 255;
    }
    const std::array<std::uint32_t,12> alphaIndices{0,1,2,1,3,2,4,5,6,5,7,6};
    const std::array<War3RenderTerrainBatchV2,2> alphaBatches{{{0,6,WAR3_RENDER_HEIGHT_COLOR_TEXTURE},{6,6,WAR3_RENDER_TERRAIN_ALPHA_BLEND}}};
    const auto captureAlpha = [&](const char* name, Source& source, bool overlay) {
        const auto documentRevision = ++revision;
        const War3RenderTerrainMipSceneV2 scene{sizeof(scene),8,overlay ? 12u : 6u,overlay ? 1u : 0u,overlay ? 2u : 1u,
            documentRevision,alphaVertices.data(),alphaIndices.data(),overlay ? &source.chain : nullptr,alphaBatches.data()};
        Require(api->setCamera(renderer.handle,&camera) == 0 && api->submitTerrainMipScene(renderer.handle,&scene) == 0,
            "terrain oblique alpha scene rejected");
        auto result = Observe(name,CaptureFrame(renderer,kViewport,kViewport,documentRevision,camera),2,true);
        Export(directory,result,source,overlay ? "source-alpha-over-k4" : "height-color-backing",2,info.forceWarp,nullptr,overlay);
        return result;
    };
    const auto backing = captureAlpha("alpha-backing",alpha0,false);
    const auto zero = captureAlpha("alpha-zero-overlay",alpha0,true);
    const auto half = captureAlpha("alpha-half-overlay",alpha128,true);

    Require(fineFrame.frame.pixels == fineSentinel.frame.pixels && fractionalFrame.frame.pixels == fractionalSentinel.frame.pixels
        && reverseFrame.frame.pixels == reverseSentinel.frame.pixels,"terrain oblique taps read another cell or retained tail");
    for (std::size_t i = 0; i < 4; ++i)
        Require(fractionalFrame.analysis.edge[i] + reverseFrame.analysis.edge[i] > 0
            && fractionalFrame.analysis.corner[i] + reverseFrame.analysis.corner[i] > 0,
            "terrain oblique sentinel views do not exercise all four edges/corners");
    Require(legacyFrame.frame.pixels == mip0Frame.frame.pixels,"terrain mip0 and legacy exact paths disagree");
    Require(backing.frame.pixels == zero.frame.pixels,"terrain alpha-zero overlay changed original backing pixels");
    Require(!half.analysis.roi.empty() && half.opaque,"terrain alpha oracle has no opaque ROI");
    for (const auto at : half.analysis.roi) for (std::size_t channel = 0; channel < 3; ++channel) {
        const double expected = (128.0 * sourceColor[channel] + 127.0 * baseColor[channel]) / 255;
        Require(std::abs(double(half.frame.pixels[at + channel]) - expected) <= 1,"terrain constant source-alpha over is wrong");
    }
    for (const auto at : translucentFrame.analysis.roi)
        Require(translucentFrame.frame.pixels[at] == 128 && translucentFrame.frame.pixels[at + 2] == 80,
            "explicit translucent batch lost original isotropic mip policy");
    Require(fineFrame.opaque && fractionalFrame.opaque && frontFrame.opaque && solidFrame.opaque
        && SameCoverage(fineFrame,solidFrame) && SameCoverage(fineSentinel,solidFrame),
        "terrain filter changed opaque silhouette coverage");
    const auto quality = [](const Observation& observed, std::size_t count, bool fractionalMarker) {
        Require(observed.covered && observed.analysis.white.size() >= count && observed.analysis.black.size() >= count,
            "terrain oblique quality ROI missing, occluded or below frozen sample count");
        Require(observed.white - observed.black >= kContrast,"terrain lost resolvable short-axis source detail");
        for (const auto at : observed.analysis.roi) {
            if (!fractionalMarker) {
                Require(observed.frame.pixels[at + 2] >= 159 && observed.frame.pixels[at + 2] <= 161,
                    "terrain explicit mip marker disagrees with natural/fractional LOD");
            } else {
                const auto& view = observed.frame.receipt.view;
                const auto pixel = at / 4;
                const auto expected = ExpectedFootprint(view,static_cast<std::uint32_t>(pixel % view.width),
                    static_cast<std::uint32_t>(pixel / view.width));
                Require(expected.lod > 0 && expected.lod < 1,"terrain coarse marker left original mip bracket");
                // 仅用原级常量B160/96验证LOD；不重建多tap RGB/alpha，也不从输出倒推预期。
                const double expectedBlue = 160.0 + (96.0 - 160.0) * expected.lod;
                Require(std::abs(double(observed.frame.pixels[at + 2]) - expectedBlue) <= 1.0,
                    "terrain explicit mip marker disagrees with natural/fractional LOD");
            }
        }
    };
    Require(frontFrame.covered && frontFrame.analysis.white.size() >= 128 && frontFrame.analysis.black.size() >= 128
        && frontFrame.white - frontFrame.black >= kContrast,"terrain N1 front control lost resolvable bars");
    quality(fineFrame,128,false);
    quality(fractionalFrame,64,true);
    std::cout << "terrain K4: original cell mips, short-axis/fractional detail, four-edge sentinel, alpha and exact policy controls passed\n";
}
