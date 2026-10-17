#pragma once

// 此头与其它 D3D11 fixture 一样，在主测试的私有命名空间中复用真实 CaptureFrame/SaveBmp。
namespace ModelObliqueSamplerTest {

constexpr std::uint32_t kSize = 512;
constexpr std::uint64_t kRevision = 9831;
constexpr double kRequiredContrast = 112.0; // 原创条带满幅224的一半；首次执行前冻结。
constexpr std::size_t kMinimumPhaseSamples = 256;

struct Texture {
    std::vector<std::vector<std::uint8_t>> pixels;
    std::vector<War3RenderMipRgbaV2> mips;
    std::vector<std::string> hashes;
    War3RenderTextureMipChainV2 chain{};
};

Texture MakeTexture(bool solid) {
    Texture output;
    output.pixels.reserve(10); output.mips.reserve(10); output.hashes.reserve(10);
    for (std::uint32_t level = 0, size = kSize;; ++level, size = std::max(1u, size / 2)) {
        auto& pixels = output.pixels.emplace_back(static_cast<std::size_t>(size) * size * 4);
        for (std::uint32_t y = 0; y < size; ++y) for (std::uint32_t x = 0; x < size; ++x) {
            const auto at = (static_cast<std::size_t>(y) * size + x) * 4;
            // 显式原创链：mip3以后精确中灰，三线性按长轴取级会失去U短轴条纹。
            pixels[at] = solid || level >= 3 ? 128 : ((x / (4u >> level)) & 1u) ? 16 : 240;
            pixels[at + 1] = 96; pixels[at + 2] = 160; pixels[at + 3] = 255;
        }
        output.mips.push_back({sizeof(War3RenderMipRgbaV2), size, size, pixels.size(), pixels.data()});
        auto hash = War3Map::Sha256(std::as_bytes(std::span(pixels)));
        Require(hash.Succeeded(), "oblique fixture source hash failed");
        output.hashes.push_back(std::move(*hash.value));
        if (size == 1) break;
    }
    output.chain = {sizeof(War3RenderTextureMipChainV2), static_cast<std::uint32_t>(output.mips.size()),
        static_cast<std::uint32_t>(output.mips.size() - 1), 1, 1, output.mips.data()};
    return output;
}

struct Uv {
    double u = 0, v = 0;
};

// 仅将实际回执MVP的屏幕点反解到已知z=0平面，用于划分ROI/条带相位，不产生参考图。
Uv PlaneUv(const War3RenderViewInfoV2& view, double pixelX, double pixelY) {
    const auto* m = view.viewProjection;
    const double x = 2.0 * pixelX / view.width - 1.0, y = 1.0 - 2.0 * pixelY / view.height;
    const double a = m[0] - x * m[3], b = m[4] - x * m[7], c = x * m[15] - m[12];
    const double d = m[1] - y * m[3], e = m[5] - y * m[7], f = y * m[15] - m[13];
    const double determinant = a * e - b * d;
    Require(std::isfinite(determinant) && determinant != 0, "oblique fixture projection is singular");
    return {((c * e - b * f) / determinant + 256.0) / 512.0,
        ((a * f - c * d) / determinant + 256.0) / 512.0};
}

struct Samples {
    std::vector<std::size_t> white, black;
    std::size_t roi = 0;
    double minimumMajor = 0, maximumMajor = 0, minimumMinor = 0, maximumMinor = 0;
    double minimumRatio = 0, maximumRatio = 0;
};

Samples SelectSamples(const War3RenderViewInfoV2& view, bool oblique) {
    Samples output;
    for (std::uint32_t y = 0; y < view.height; ++y) for (std::uint32_t x = 0; x < view.width; ++x) {
        const auto uv = PlaneUv(view, x + 0.5, y + 0.5);
        if (uv.u < 0.3 || uv.u > 0.7 || uv.v < 0.3 || uv.v > 0.7) continue;
        const auto right = PlaneUv(view, x + 1.5, y + 0.5), down = PlaneUv(view, x + 0.5, y + 1.5);
        const double duX = (right.u - uv.u) * kSize, dvX = (right.v - uv.v) * kSize;
        const double duY = (down.u - uv.u) * kSize, dvY = (down.v - uv.v) * kSize;
        const double rhoX = std::hypot(duX, dvX), rhoY = std::hypot(duY, dvY);
        const double major = std::max(rhoX, rhoY), minor = std::min(rhoX, rhoY), ratio = major / minor;
        Require(std::isfinite(major) && std::isfinite(minor) && std::isfinite(ratio), "oblique fixture footprint invalid");
        if (oblique && (major < 9 || major > 12 || minor < 1 || minor > 1.5 || ratio < 6 || ratio > 10
            || std::abs(duX) < 0.95 * minor || std::abs(dvY) < 0.95 * major)) continue;
        if (!output.roi) {
            output.minimumMajor = output.maximumMajor = major; output.minimumMinor = output.maximumMinor = minor;
            output.minimumRatio = output.maximumRatio = ratio;
        } else {
            output.minimumMajor = std::min(output.minimumMajor, major); output.maximumMajor = std::max(output.maximumMajor, major);
            output.minimumMinor = std::min(output.minimumMinor, minor); output.maximumMinor = std::max(output.maximumMinor, minor);
            output.minimumRatio = std::min(output.minimumRatio, ratio); output.maximumRatio = std::max(output.maximumRatio, ratio);
        }
        ++output.roi;
        const double phase = std::fmod(uv.u * kSize, 8.0);
        const auto at = (static_cast<std::size_t>(y) * view.width + x) * 4;
        if (phase >= 1 && phase <= 3) output.white.push_back(at);
        if (phase >= 5 && phase <= 7) output.black.push_back(at);
    }
    return output;
}

bool Foreground(const std::vector<std::uint8_t>& pixels, std::size_t at) {
    return pixels[at + 1] >= 80 && pixels[at + 2] >= 140;
}

struct Metrics {
    double whiteMean = 0, blackMean = 0, contrast = 0;
    std::size_t foreground = 0;
    bool opaque = true, roiCovered = true, solidColor = true, sameCoverage = true;
};

Metrics Measure(const CapturedFrame& frame, const Samples& samples, const CapturedFrame* solid = nullptr) {
    Metrics output;
    for (std::size_t at = 0; at < frame.pixels.size(); at += 4) {
        output.opaque &= frame.pixels[at + 3] == 255;
        const bool foreground = Foreground(frame.pixels, at);
        output.foreground += foreground;
        if (foreground) {
            output.solidColor &= std::abs(int(frame.pixels[at]) - 128) <= 1
                && std::abs(int(frame.pixels[at + 1]) - 96) <= 1 && std::abs(int(frame.pixels[at + 2]) - 160) <= 1;
        }
        if (solid) output.sameCoverage &= foreground == Foreground(solid->pixels, at);
    }
    const auto mean = [&](const auto& indices) {
        std::uint64_t sum = 0;
        for (const auto at : indices) {
            sum += frame.pixels[at]; output.roiCovered &= Foreground(frame.pixels, at);
        }
        return indices.empty() ? 0.0 : static_cast<double>(sum) / static_cast<double>(indices.size());
    };
    output.whiteMean = mean(samples.white); output.blackMean = mean(samples.black);
    output.contrast = output.whiteMean - output.blackMean;
    return output;
}

bool HasContrast(const Samples& samples, const Metrics& metrics) {
    return samples.white.size() >= kMinimumPhaseSamples && samples.black.size() >= kMinimumPhaseSamples
        && metrics.roiCovered && metrics.contrast >= kRequiredContrast;
}

std::filesystem::path OutputDirectory() {
    constexpr auto variable = L"WAR3_RENDER_OBLIQUE_AUDIT_OUTPUT";
    SetLastError(ERROR_SUCCESS);
    const auto size = GetEnvironmentVariableW(variable, nullptr, 0);
    if (!size) {
        Require(GetLastError() == ERROR_SUCCESS || GetLastError() == ERROR_ENVVAR_NOT_FOUND,
            "oblique export environment unavailable");
        return {};
    }
    Require(size < 32768, "oblique export directory is too long");
    std::vector<wchar_t> value(size);
    Require(GetEnvironmentVariableW(variable, value.data(), size) == size - 1, "oblique export environment changed");
    const std::filesystem::path path(value.data());
    Require(path.is_absolute() && !std::filesystem::exists(path), "oblique export requires a new absolute directory");
    Require(std::filesystem::create_directory(path), "oblique export directory could not be created");
    return path;
}

void Export(const std::filesystem::path& directory, const char* name, const CapturedFrame& frame,
    const Texture& texture, const Samples& samples, const Metrics& metrics, bool front, bool solid,
    bool explicitSampler, std::uint32_t samplerFlags, std::uint32_t software,
    const War3RenderParticleStatusV2* particle = nullptr, bool additionalLayer = false) {
    if (directory.empty()) return;
    const std::string stem(name);
    std::ofstream raw(directory / (stem + ".rgba"), std::ios::binary);
    raw.write(reinterpret_cast<const char*>(frame.pixels.data()), static_cast<std::streamsize>(frame.pixels.size()));
    raw.close(); Require(raw.good(), "oblique raw frame export failed");
    SaveBmp(directory / (stem + ".bmp"), frame.pixels, static_cast<LONG>(kSize), static_cast<LONG>(kSize));
    auto frameHash = War3Map::Sha256(std::as_bytes(std::span(frame.pixels)));
    Require(frameHash.Succeeded(), "oblique frame hash failed");
    std::ofstream stream(directory / (stem + ".json"), std::ios::binary);
    stream.precision(17);
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    const auto& capture = frame.receipt.capture; const auto& view = frame.receipt.view; const auto& camera = frame.receipt.camera;
    stream << "{\n\"schema\":\"model-oblique-sampler-fixture-v1\",\n\"case\":\"" << name << "\",\n"
        << "\"scope\":\"original-model-filter-quality-not-warcraft-or-cliff-acceptance\",\n"
        << "\"front_control\":" << boolean(front) << ",\n\"solid_control\":" << boolean(solid)
        << ",\n\"explicit_sampler\":" << boolean(explicitSampler) << ",\n\"sampler_flags\":" << samplerFlags
        << ",\n\"software\":" << software << ",\n\"rgba_sha256\":\"" << *frameHash.value << "\",\n"
        << "\"width\":" << capture.width << ",\n\"height\":" << capture.height
        << ",\n\"ticket\":" << capture.ticket << ",\n\"frame\":" << capture.frameCount
        << ",\n\"revision\":" << capture.documentRevision << ",\n\"surface_epoch\":" << view.surfaceEpoch
        << ",\n\"scene_epoch\":" << view.sceneEpoch << ",\n\"view_epoch\":" << view.viewEpoch
        << ",\n\"camera\":[" << camera.targetX << ',' << camera.targetY << ',' << camera.targetZ << ','
        << camera.distance << ',' << camera.yawRadians << ',' << camera.pitchRadians << "],\n\"view_projection\":[";
    for (std::size_t i = 0; i < 16; ++i) stream << (i ? "," : "") << view.viewProjection[i];
    stream << "],\n\"source\":{\"definition\":\"512-square-R16-240-U-bars-period8-G96-B160-A255;"
        << "mip1-period4,mip2-period2,mip3-plus-R128;solid-all-R128\",\"explicit_mips\":[";
    for (std::size_t i = 0; i < texture.mips.size(); ++i) {
        const auto& mip = texture.mips[i];
        stream << (i ? "," : "") << "{\"level\":" << i << ",\"width\":" << mip.width << ",\"height\":"
            << mip.height << ",\"rgba_sha256\":\"" << texture.hashes[i] << "\"}";
    }
    stream << "]},\n\"roi\":{\"uv_min\":0.3,\"uv_max\":0.7,\"count\":" << samples.roi
        << ",\"white_count\":" << samples.white.size() << ",\"black_count\":" << samples.black.size()
        << ",\"major_range\":[" << samples.minimumMajor << ',' << samples.maximumMajor
        << "],\"minor_range\":[" << samples.minimumMinor << ',' << samples.maximumMinor
        << "],\"ratio_range\":[" << samples.minimumRatio << ',' << samples.maximumRatio << "]},\n"
        << "\"white_mean\":" << metrics.whiteMean << ",\n\"black_mean\":" << metrics.blackMean
        << ",\n\"contrast\":" << metrics.contrast << ",\n\"required_contrast\":" << kRequiredContrast
        << ",\n\"minimum_phase_samples\":" << kMinimumPhaseSamples
        << ",\n\"contrast_gate_required\":" << boolean(!solid && !particle)
        << ",\n\"contrast_gate_passed\":" << boolean(HasContrast(samples, metrics))
        << ",\n\"opaque\":" << boolean(metrics.opaque) << ",\n\"roi_covered\":" << boolean(metrics.roiCovered)
        << ",\n\"foreground_count\":" << metrics.foreground << ",\n\"coverage_matches_solid\":";
    if (front || solid || particle) stream << "null"; else stream << boolean(metrics.sameCoverage);
    stream << ",\n\"solid_color_within_one\":";
    if (solid) stream << boolean(metrics.solidColor); else stream << "null";
    stream << ",\n\"particle_trilinear_control\":" << boolean(particle != nullptr)
        << ",\n\"additional_layer\":" << boolean(additionalLayer);
    if (particle) stream << ",\n\"particle_status\":{\"scene_version\":" << particle->sceneVersion
        << ",\"content_version\":" << particle->contentVersion << ",\"texture_uploads\":" << particle->textureUploadCount
        << ",\"dynamic_uploads\":" << particle->dynamicUploadCount << ",\"vertex_count\":" << particle->vertexCount
        << ",\"draw_count\":" << particle->drawCount << '}';
    stream << "\n}\n";
    stream.close(); Require(stream.good(), "oblique metrics export failed");
}

void TestParticlePreservation(const RendererOwner& renderer, const War3RenderCameraV2& camera,
    const Texture& texture, const Samples& samples, const std::filesystem::path& directory, std::uint32_t software) {
    const auto* api = renderer.api;
    Require(api->submitParticleModelScene && api->captureParticleFrameRgba && api->submitModelSceneLayer
        && api->captureLayeredFrameRgba && api->getModelLayersTarget, "oblique particle/layer production API unavailable");
    War3RenderSkinUpdateV2 pose{sizeof(pose),0,0,0,101,1,WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4,48,0,0,nullptr,nullptr,nullptr};
    War3RenderSkinnedModelSceneV2 models{sizeof(models),0,0,0,0,0,0,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&pose};
    const std::array<War3RenderParticleVertexV2,6> vertices{{
        {{-256,-256,0},{0,0},{1,1,1,1}}, {{256,-256,0},{1,0},{1,1,1,1}}, {{-256,256,0},{0,1},{1,1,1,1}},
        {{256,-256,0},{1,0},{1,1,1,1}}, {{256,256,0},{1,1},{1,1,1,1}}, {{-256,256,0},{0,1},{1,1,1,1}}}};
    const War3RenderParticleDrawV2 draw{0,6,0,WAR3_PARTICLE_OPAQUE};
    const War3RenderParticleUpdateV2 update{sizeof(update),6,1,0,101,1,vertices.data(),&draw,&pose};
    const War3RenderParticleSceneV2 scene{sizeof(scene),1,&texture.chain,&models,&update};
    Require(api->setCamera(renderer.handle,&camera) == 0 && api->submitParticleModelScene(renderer.handle,&scene) == 0,
        "oblique original particle scene rejected");
    CapturedFrame main;
    main.pixels.resize(static_cast<std::size_t>(kSize) * kSize * 4);
    War3RenderParticleCapturedFrameV2 mainReceipt{sizeof(mainReceipt)};
    Require(api->captureParticleFrameRgba(renderer.handle,main.pixels.data(),main.pixels.size(),&mainReceipt) == 0,
        "oblique particle capture failed");
    main.receipt = mainReceipt.models.frame;
    Require(mainReceipt.models.skin.active && !mainReceipt.models.skin.result
        && mainReceipt.models.skin.sceneVersion == 101 && mainReceipt.models.skin.contentVersion == 1
        && mainReceipt.models.skin.frameCount == main.receipt.capture.frameCount
        && mainReceipt.particles.sceneVersion == 101 && mainReceipt.particles.contentVersion == 1
        && mainReceipt.particles.vertexCount == 6 && mainReceipt.particles.drawCount == 1
        && mainReceipt.particles.textureUploadCount == 1 && mainReceipt.particles.dynamicUploadCount == 1,
        "oblique particle completed status mismatch");
    const auto checkFrame = [&](const CapturedFrame& frame) {
        const auto& receipt = frame.receipt;
        Require(receipt.capture.ticket && receipt.capture.frameCount && receipt.capture.width == kSize
            && receipt.capture.height == kSize && receipt.capture.documentRevision == kRevision
            && receipt.view.frameCount == receipt.capture.frameCount && receipt.view.documentRevision == kRevision
            && receipt.view.surfaceEpoch && receipt.view.sceneEpoch && receipt.view.viewEpoch
            && std::memcmp(&receipt.camera,&camera,sizeof(camera)) == 0,
            "oblique particle pixels/camera/view do not share identity");
        RequireExactMatrix(receipt.view,camera);
    };
    checkFrame(main);
    const auto mainMetrics = Measure(main,samples);
    Export(directory,"oblique-particle-main",main,texture,samples,mainMetrics,false,false,false,3,software,&mainReceipt.particles);

    // 清空主槽后再放同源粒子到child，避免主粒子遮住child而让缺sampler假通过。
    auto parentPose = pose; parentPose.sceneVersion = 102;
    auto parent = models; parent.initialPose = &parentPose;
    Require(api->submitSkinnedModelScene(renderer.handle,&parent) == 0, "oblique layer parent clear rejected");
    const auto background = CaptureFrame(renderer,kSize,kSize,kRevision,camera);
    const auto backgroundMetrics = Measure(background,samples);
    Require(backgroundMetrics.foreground == 0, "oblique particle child is hidden by a foreground parent");
    War3RenderModelLayersTargetV2 target{sizeof(target)};
    Require(api->getModelLayersTarget(renderer.handle,&target) == 0 && target.parentModelEpoch,
        "oblique particle parent target missing");
    const War3RenderModelSceneLayerV2 layer{sizeof(layer),0,9832,target.parentModelEpoch,1,0,nullptr,&scene,
        WAR3_RENDER_MODEL_LAYER_MODEL,0};
    Require(api->submitModelSceneLayer(renderer.handle,&layer) == 0
        && api->getModelLayersTarget(renderer.handle,&target) == 0 && target.layerCount == 1,
        "oblique particle child submit rejected");
    CapturedFrame child; child.pixels.resize(main.pixels.size());
    War3RenderLayeredCapturedFrameV2 childReceipt{sizeof(childReceipt)};
    Require(api->captureLayeredFrameRgba(renderer.handle,target.layersRevision,child.pixels.data(),child.pixels.size(),&childReceipt) == 0,
        "oblique particle child capture failed");
    child.receipt = childReceipt.world.frame; checkFrame(child);
    const auto& status = childReceipt.layers[0];
    Require(childReceipt.layerCount == 1 && childReceipt.layersRevision == target.layersRevision
        && status.layerId == 9832 && status.parentModelEpoch == target.parentModelEpoch && status.revision == 1 && !status.result
        && status.skin.active && !status.skin.result && status.skin.sceneVersion == 101 && status.skin.contentVersion == 1
        && status.particles.sceneVersion == 101 && status.particles.contentVersion == 1
        && status.particles.vertexCount == 6 && status.particles.drawCount == 1
        && status.particles.textureUploadCount == 1 && status.particles.dynamicUploadCount == 1,
        "oblique particle child completed status mismatch");
    const auto childMetrics = Measure(child,samples);
    Export(directory,"oblique-particle-child",child,texture,samples,childMetrics,false,false,false,3,software,&status.particles,true);
    Require(mainMetrics.opaque && mainMetrics.roiCovered && mainMetrics.foreground > 1000
        && std::abs(mainMetrics.whiteMean - 128) <= 1 && std::abs(mainMetrics.blackMean - 128) <= 1,
        "particle filter changed from original trilinear gray");
    Require(childMetrics.opaque && childMetrics.roiCovered && child.pixels == main.pixels,
        "additional particle layer changed original sampler pixels");
    std::cout << "oblique particle controls: original trilinear gray and byte-identical main/child frames passed\n";
}

} // namespace ModelObliqueSamplerTest

void TestModelObliqueSampler(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& originalInfo) {
    using namespace ModelObliqueSamplerTest;
    auto info = originalInfo; info.width = info.height = kSize;
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle && api->setEnvironment && api->getStaticModelSamplerCapabilities,
        "oblique fixture production model/capture capabilities missing");
    War3RenderStaticSamplerCapabilitiesV2 capability{sizeof(capability)};
    Require(api->getStaticModelSamplerCapabilities(renderer.handle, &capability) == 0
        && capability.samplerFormat == WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1 && capability.supportedSamplerFlags == 3,
        "oblique fixture static sampler format unavailable");
    const auto directory = OutputDirectory();
    // 空mesh会启用z=0默认调试grid；相机外非空triangle关闭grid，不改变测试平面和深度。
    const std::array<War3RenderVertexV2,3> background{{
        {10000,10000,0,.045f,.060f,.075f}, {10100,10000,0,.045f,.060f,.075f},
        {10000,10100,0,.045f,.060f,.075f}}};
    const std::array<std::uint32_t,3> backgroundIndices{0,1,2};
    const War3RenderMeshV2 backgroundMesh{sizeof(backgroundMesh),3,3,kRevision,background.data(),backgroundIndices.data()};
    const War3RenderEnvironmentV2 environment{sizeof(environment), {0,0,1}, {1,1,1}, {0,0,0},
        WAR3_RENDER_FOG_NONE, 0,1,0, {0,0,0}, 0,0,0,0,1,0,nullptr};
    Require(api->submitMesh(renderer.handle, &backgroundMesh) == 0 && api->setEnvironment(renderer.handle, &environment) == 0,
        "oblique fixture controlled background/environment rejected");
    auto grating = MakeTexture(false), solidTexture = MakeTexture(true);
    const std::array<War3RenderModelVertexV2, 4> vertices{{
        {-256,-256,0, 0,0,1, 0,0}, {256,-256,0, 0,0,1, 1,0},
        {-256,256,0, 0,0,1, 0,1}, {256,256,0, 0,0,1, 1,1}}};
    const std::array<std::uint16_t, 6> indices{0,1,2,1,3,2};
    const War3RenderModelLayerV2 layer{0,0,1,1}; // 不透明、Unshaded；源alpha恒255。
    const War3RenderModelMeshV2 mesh{0,6,0,0,1};
    const War3RenderModelV2 model{0,1};
    const War3RenderModelInstanceV2 instance{0,{1,0,0,0, 0,1,0,0, 0,0,1,0},{1,1,1}};
    War3RenderModelSceneV2 legacy{sizeof(legacy),4,6,1,1,1,1,1,vertices.data(),indices.data(),&layer,&mesh,&model,&instance,&grating.chain};
    const War3RenderCameraV2 oblique{sizeof(oblique),0,0,0,800,-1.5707963267948966f,0.12532783f};
    auto front = oblique; front.pitchRadians = 1.4f;
    const auto capture = [&](const War3RenderCameraV2& camera, const Texture& texture, bool explicitSampler, std::uint32_t flags) {
        legacy.textures = &texture.chain;
        War3RenderStaticSamplerModelSceneV2 scene{legacy,WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1,1,&flags};
        scene.base.structSize = sizeof(scene);
        Require(api->setCamera(renderer.handle, &camera) == 0
            && api->submitModelScene(renderer.handle, explicitSampler ? &scene.base : &legacy) == 0,
            "oblique fixture production camera/model rejected");
        auto frame = CaptureFrame(renderer,kSize,kSize,kRevision,camera);
        War3RenderStatusV2 status{sizeof(status)};
        Require(api->getStatus(renderer.handle, &status) == 0 && !status.result && status.deviceReady == 1
            && status.software == info.forceWarp && status.width == kSize && status.height == kSize
            && status.documentRevision == kRevision, "oblique fixture device/frame identity mismatch");
        return frame;
    };
    // 控制先于质量断言；旧provider失败时仍保有正视、纯色和全部斜视完成帧。
    const auto frontFrame = capture(front,grating,false,3);
    const auto frontSamples = SelectSamples(frontFrame.receipt.view,false);
    const auto frontMetrics = Measure(frontFrame,frontSamples);
    Export(directory,"front-legacy3",frontFrame,grating,frontSamples,frontMetrics,true,false,false,3,info.forceWarp);
    const auto solidFrame = capture(oblique,solidTexture,false,3);
    const auto samples = SelectSamples(solidFrame.receipt.view,true);
    const auto solidMetrics = Measure(solidFrame,samples);
    Export(directory,"oblique-solid",solidFrame,solidTexture,samples,solidMetrics,false,true,false,3,info.forceWarp);
    std::cout << "oblique controls: front contrast=" << frontMetrics.contrast << " solid coverage="
        << solidMetrics.foreground << " roi=" << samples.roi << '\n';
    std::array<Metrics,5> observed{};
    for (std::size_t index = 0; index < observed.size(); ++index) {
        const bool explicitSampler = index != 0;
        const auto flags = explicitSampler ? static_cast<std::uint32_t>(index - 1) : 3u;
        const auto frame = capture(oblique,grating,explicitSampler,flags);
        Require(std::memcmp(frame.receipt.view.viewProjection, solidFrame.receipt.view.viewProjection,
            sizeof(frame.receipt.view.viewProjection)) == 0, "oblique control and grating MVP differ");
        observed[index] = Measure(frame,samples,&solidFrame);
        const std::array<const char*,5> names{"oblique-legacy3","oblique-flags0","oblique-flags1","oblique-flags2","oblique-flags3"};
        Export(directory,names[index],frame,grating,samples,observed[index],false,false,explicitSampler,flags,info.forceWarp);
        std::cout << names[index] << " contrast=" << observed[index].contrast << " white=" << samples.white.size()
            << " black=" << samples.black.size() << " coverage=" << observed[index].sameCoverage << '\n';
    }
    TestParticlePreservation(renderer,oblique,grating,samples,directory,info.forceWarp);
    Require(frontMetrics.opaque && HasContrast(frontSamples,frontMetrics), "front control lost resolvable source bars");
    Require(solidMetrics.opaque && solidMetrics.roiCovered && solidMetrics.foreground > 1000 && solidMetrics.solidColor,
        "oblique solid control changed color, alpha or geometric coverage");
    for (const auto& metrics : observed) {
        Require(metrics.opaque && metrics.sameCoverage && metrics.foreground == solidMetrics.foreground,
            "oblique filter changed opaque silhouette coverage");
        Require(HasContrast(samples,metrics), "oblique model lost resolvable short-axis source detail");
    }
    std::cout << "model oblique filter: original source chain, exact-MVP interior contrast, four flags and solid silhouette controls passed\n";
}
