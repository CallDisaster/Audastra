#pragma once

// 在现有D3D11测试私有命名空间中复用真实provider、CaptureFrame与SaveBmp。
namespace ModelSourceMipTest {

using namespace War3Map;
using namespace War3Map::Render;
constexpr std::uint32_t kTextureSize = 256, kMipCount = 9, kWidth = 320, kHeight = 240;
constexpr std::uint64_t kRevision = 9861;
constexpr float kHalfExtent = 512, kUvSpan = 16;
constexpr std::uint32_t kRoiX = 152, kRoiY = 112, kRoiSize = 16;
constexpr std::size_t kMinimumSamples = 256;
static_assert(kMinimumSamples == kRoiSize * kRoiSize);
constexpr int kColorTolerance = 8; // 首次执行前冻结；内部ROI必须全部命中预期纯色。
constexpr double kMinimumTexelsPerPixel = 8;
constexpr std::string_view kTexturePath = "Tests\\OriginalSourceMip.blp";

void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned part = 0; part < 4; ++part)
        bytes[offset + part] = static_cast<std::byte>((value >> (part * 8)) & 255);
}

std::vector<std::byte> OriginalBlp() {
    std::vector<std::byte> bytes(1180);
    bytes[0] = std::byte{'B'}; bytes[1] = std::byte{'L'}; bytes[2] = std::byte{'P'}; bytes[3] = std::byte{'1'};
    Put32(bytes, 4, 1); Put32(bytes, 12, kTextureSize); Put32(bytes, 16, kTextureSize);
    Put32(bytes, 156, 0x00ff0000); Put32(bytes, 160, 0x0000ff00);
    // 原创调色板链：top全红，所有原始低级全绿；绿不能由红top的GenerateMips产生。
    for (std::uint32_t level = 0, size = kTextureSize; level < kMipCount; ++level, size = std::max(1u, size / 2)) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(bytes.size()));
        Put32(bytes, 92 + level * 4, size * size);
        bytes.insert(bytes.end(), static_cast<std::size_t>(size) * size, static_cast<std::byte>(level != 0));
    }
    return bytes;
}

std::string Digest(std::span<const std::byte> bytes) {
    auto hash = Sha256(bytes); Require(hash.Succeeded(), "source-mip fixture SHA unavailable");
    return std::move(*hash.value);
}

template<class Pixels> void RequireSolid(const Pixels& pixels, unsigned channel) {
    const auto bytes = std::as_bytes(std::span(pixels));
    Require(!bytes.empty() && bytes.size() % 4 == 0, "source-mip fixture RGBA byte count invalid");
    for (std::size_t at = 0; at < bytes.size(); at += 4) {
        for (unsigned component = 0; component < 4; ++component)
            Require(bytes[at + component] == static_cast<std::byte>(component == channel || component == 3 ? 255 : 0),
                "source-mip fixture original RGBA changed");
    }
}

void RequireSourceChain(const ModelTextureRgba& texture) {
    Require(texture.path == kTexturePath && texture.width == kTextureSize && texture.height == kTextureSize
        && texture.ownedIdentity.empty() && texture.additionalMips.size() + 1 == kMipCount,
        "production model decode did not retain the original BLP chain");
    RequireSolid(texture.rgba, 0);
    for (std::size_t level = 0; level < texture.additionalMips.size(); ++level) {
        const auto& mip = texture.additionalMips[level];
        const auto size = kTextureSize >> (level + 1);
        Require(mip.Valid() && mip.width == size && mip.height == size, "production model source mip dimensions changed");
        RequireSolid(mip.rgba, 1);
    }
}

void RequireDescriptor(const ModelSceneDescriptor& descriptor, const ModelTextureRgba& texture, std::uint32_t levels) {
    Require(descriptor.StaticScene() && descriptor.chains.size() == 1 && descriptor.mips.size() == levels,
        "production model descriptor omitted original mip levels");
    const auto& chain = descriptor.chains.front();
    Require(chain.structSize == sizeof(chain) && chain.mipCount == levels && chain.maxSampleMip == levels - 1
        && chain.atlasColumns == 1 && chain.atlasRows == 1 && chain.mips == descriptor.mips.data(),
        "production model descriptor chain/pointer policy differs");
    for (std::uint32_t level = 0; level < levels; ++level) {
        const auto& mip = descriptor.mips[level];
        const auto bytes = level == 0 ? std::as_bytes(std::span(texture.rgba))
            : std::as_bytes(std::span(texture.additionalMips[level - 1].rgba));
        Require(mip.structSize == sizeof(mip) && mip.width == (kTextureSize >> level)
            && mip.height == (kTextureSize >> level) && mip.rgbaByteCount == bytes.size()
            && static_cast<const void*>(mip.rgba) == static_cast<const void*>(bytes.data()),
            "production upload descriptor is not bound to the retained source bytes");
    }
}

std::vector<std::byte> GeometryBytes(const WorldModelScene& scene) {
    std::vector<std::byte> bytes;
    const auto append = [&](const auto& values) {
        const auto part = std::as_bytes(std::span(values.data(), values.size()));
        bytes.insert(bytes.end(), part.begin(), part.end());
    };
    append(scene.vertices); append(scene.indices); append(scene.layers); append(scene.layerSamplerFlags);
    append(scene.meshes); append(scene.models); append(scene.instances); append(scene.instanceRequests);
    return bytes;
}

std::array<double, 2> PlanePoint(const War3RenderViewInfoV2& view, double pixelX, double pixelY) {
    const auto* m = view.viewProjection;
    const double x = 2 * pixelX / view.width - 1, y = 1 - 2 * pixelY / view.height;
    const double a = m[0] - x * m[3], b = m[4] - x * m[7], c = x * m[15] - m[12];
    const double d = m[1] - y * m[3], e = m[5] - y * m[7], f = y * m[15] - m[13];
    const double determinant = a * e - b * d;
    Require(std::isfinite(determinant) && determinant != 0, "source-mip fixture plane is singular");
    return {(c * e - b * f) / determinant, (a * f - c * d) / determinant};
}

double RequireMinifiedInterior(const War3RenderViewInfoV2& view) {
    double minimum = std::numeric_limits<double>::max();
    constexpr double scale = kTextureSize * kUvSpan / (2 * kHalfExtent);
    for (std::uint32_t y = kRoiY; y < kRoiY + kRoiSize; ++y) for (std::uint32_t x = kRoiX; x < kRoiX + kRoiSize; ++x) {
        const auto point = PlanePoint(view, x + .5, y + .5);
        const auto right = PlanePoint(view, x + 1.5, y + .5), down = PlanePoint(view, x + .5, y + 1.5);
        Require(std::abs(point[0]) < kHalfExtent / 2 && std::abs(point[1]) < kHalfExtent / 2,
            "source-mip frozen ROI is not inside the actual completed quad");
        const auto rho = std::min(std::hypot(right[0] - point[0], right[1] - point[1]),
            std::hypot(down[0] - point[0], down[1] - point[1])) * scale;
        Require(std::isfinite(rho) && rho >= kMinimumTexelsPerPixel,
            "source-mip frozen ROI did not minify beyond the red top mip");
        minimum = std::min(minimum, rho);
    }
    return minimum;
}

std::size_t MatchingSamples(const CapturedFrame& frame, unsigned channel) {
    std::size_t matched = 0;
    for (std::uint32_t y = kRoiY; y < kRoiY + kRoiSize; ++y) for (std::uint32_t x = kRoiX; x < kRoiX + kRoiSize; ++x) {
        const auto at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
        bool matches = frame.pixels[at + 3] == 255;
        for (unsigned component = 0; component < 3; ++component)
            matches &= std::abs(int(frame.pixels[at + component]) - (component == channel ? 255 : 0)) <= kColorTolerance;
        matched += matches;
    }
    return matched;
}

void WriteBytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close(); Require(stream.good(), "source-mip fixture export failed");
}

void Export(const std::filesystem::path& directory, const std::vector<std::byte>& source, const std::string& providerSha,
    const std::string& geometrySha, const std::array<CapturedFrame, 2>& frames,
    const std::array<War3RenderEditorPresentationTargetV2, 2>& targetsBefore,
    const std::array<War3RenderEditorPresentationTargetV2, 2>& targetsAfter,
    const std::array<std::size_t, 2>& matches, double minimumFootprint, const ModelTextureRgba& original) {
    if (directory.empty()) return;
    WriteBytes(directory / "original.blp", source);
    const std::array<const char*, 2> names{"source-chain", "generated-control"};
    std::ofstream receipt(directory / "result.json", std::ios::binary); receipt.precision(17);
    receipt << "{\n\"schema\":\"model-source-mip-causal-v1\",\n"
        << "\"scope\":\"original-host-decode-to-baked-scene-and-provider-not-warcraft-acceptance\",\n"
        << "\"provider_sha256\":\"" << providerSha << "\",\n\"source_blp_sha256\":\"" << Digest(source)
        << "\",\n\"source_top_sha256\":\"" << Digest(std::as_bytes(std::span(original.rgba)))
        << "\",\n\"geometry_sha256\":\"" << geometrySha << "\",\n"
        << "\"same_top_geometry_camera_provider\":true,\n"
        << "\"model_target_scope\":\"accepted CPU slot before/after capture; not per-frame GPU modelEpoch readback\",\n"
        << "\"source_mip_count\":" << kMipCount
        << ",\n\"source_low_mips\":[";
    for (std::size_t level = 0; level < original.additionalMips.size(); ++level) {
        const auto& mip = original.additionalMips[level];
        receipt << (level ? "," : "") << "{\"level\":" << level + 1 << ",\"width\":" << mip.width
            << ",\"height\":" << mip.height << ",\"rgba_sha256\":\"" << Digest(mip.rgba) << "\"}";
    }
    receipt << "],\n\"roi\":{\"x\":" << kRoiX << ",\"y\":" << kRoiY << ",\"size\":" << kRoiSize
        << ",\"required_samples\":" << kMinimumSamples << ",\"color_tolerance\":" << kColorTolerance
        << ",\"minimum_required_texels_per_pixel\":" << kMinimumTexelsPerPixel
        << ",\"observed_minimum_texels_per_pixel\":" << minimumFootprint << "},\n\"frames\":[";
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index]; const auto& view = frame.receipt.view; const auto& camera = frame.receipt.camera;
        WriteBytes(directory / (std::string(names[index]) + ".rgba"), std::as_bytes(std::span(frame.pixels)));
        SaveBmp(directory / (std::string(names[index]) + ".bmp"), frame.pixels, kWidth, kHeight);
        receipt << (index ? "," : "") << "{\"case\":\"" << names[index] << "\",\"rgba_sha256\":\""
            << Digest(std::as_bytes(std::span(frame.pixels))) << "\",\"mip_count\":" << (index ? 1 : kMipCount)
            << ",\"software\":1,\"revision\":" << view.documentRevision << ",\"frame\":" << view.frameCount
            << ",\"ticket\":" << frame.receipt.capture.ticket << ",\"surface_epoch\":" << view.surfaceEpoch
            << ",\"scene_epoch\":" << view.sceneEpoch << ",\"view_epoch\":" << view.viewEpoch
            << ",\"cpu_model_epoch_before\":" << targetsBefore[index].modelEpoch
            << ",\"cpu_model_epoch_after\":" << targetsAfter[index].modelEpoch
            << ",\"matching_roi_samples\":" << matches[index] << ",\"expected_channel\":\"" << (index ? "red" : "green")
            << "\",\"passed\":" << (matches[index] >= kMinimumSamples ? "true" : "false") << ",\"camera\":["
            << camera.targetX << ',' << camera.targetY << ',' << camera.targetZ << ',' << camera.distance << ','
            << camera.yawRadians << ',' << camera.pitchRadians << "],\"view_projection\":[";
        for (std::size_t part = 0; part < 16; ++part) receipt << (part ? "," : "") << view.viewProjection[part];
        receipt << "]}";
    }
    receipt << "]\n}\n"; receipt.close(); Require(receipt.good(), "source-mip receipt export failed");
}

void Run(const War3RenderPluginApiV2* api, const std::filesystem::path& provider, const std::filesystem::path& output) {
    const auto source = OriginalBlp(); const auto sourceSha = Digest(source);
    const auto decoded = DecodeModelTexture(std::string(kTexturePath), source, 512);
    Require(decoded.Succeeded(), "production model decode rejected original source-mip fixture");
    RequireSourceChain(*decoded.value);
    BakedModelMesh quad; quad.texturePath = kTexturePath; quad.samplerFlags = 3;
    quad.vertices = {{-kHalfExtent,-kHalfExtent,0,0,0,1,0,0}, {kHalfExtent,-kHalfExtent,0,0,0,1,kUvSpan,0},
        {-kHalfExtent,kHalfExtent,0,0,0,1,0,kUvSpan}, {kHalfExtent,kHalfExtent,0,0,0,1,kUvSpan,kUvSpan}};
    quad.indices = {0,1,2,1,3,2};
    std::size_t reads = 0;
    auto built = BuildWorldModelScene({}, [&](std::string_view path) {
        Require(path == kTexturePath, "baked scene requested an unexpected fixture resource"); ++reads;
        return Result<std::vector<std::byte>>::Success(source);
    }, {}, std::span(&quad, 1));
    Require(built.Succeeded() && reads == 1 && built.value->textures.size() == 1 && built.value->warnings.empty()
        && !built.value->untexturedLayers && built.value->instances.size() == 1
        && built.value->instanceRequests.front() == kBakedInstance, "production baked scene did not load the original resource");
    const auto& sourceScene = std::as_const(*built.value);
    const auto& sourceTexture = sourceScene.textures.front(); RequireSourceChain(sourceTexture);
    Require(sourceTexture.rgba == decoded.value->rgba, "direct and baked production decode top bytes differ");
    for (std::size_t level = 0; level < sourceTexture.additionalMips.size(); ++level)
        Require(sourceTexture.additionalMips[level].rgba == decoded.value->additionalMips[level].rgba,
            "baked scene replaced the decoded original low mips");
    auto controlScene = sourceScene;
    ModelTextureRgba control; control.path = sourceTexture.path; control.width = sourceTexture.width;
    control.height = sourceTexture.height; control.rgba = sourceTexture.rgba;
    controlScene.textures[0] = std::move(control); // 明确测试控制：只清保留链/来源元数据，不另读源或换几何。
    const auto& controlTexture = std::as_const(controlScene).textures.front();
    Require(controlTexture.additionalMips.empty() && sourceTexture.rgba == controlTexture.rgba,
        "generated-mip control changed source top pixels");
    const auto geometry = GeometryBytes(sourceScene);
    Require(geometry == GeometryBytes(controlScene) && Digest(source) == sourceSha, "A/B source bytes or geometry differ");
    const auto sourceTextures = DescribeModelTextures(sourceScene.textures);
    RequireDescriptor(sourceTextures, sourceTexture, kMipCount);
    const auto sourceDescriptor = DescribeModelScene(sourceScene), controlDescriptor = DescribeModelScene(controlScene);
    RequireDescriptor(sourceDescriptor, sourceTexture, kMipCount); RequireDescriptor(controlDescriptor, controlTexture, 1);
    const auto providerSha = Sha256File(provider); Require(providerSha.Succeeded(), "actual source-mip provider SHA unavailable");
    if (!output.empty()) {
        Require(output.is_absolute() && !std::filesystem::exists(output)
            && std::filesystem::create_directory(output), "source-mip output requires a new absolute directory");
    }
    Require(api->submitModelScene && api->setEnvironment, "source-mip production model/environment capability missing");
    const War3RenderCreateInfoV2 info{sizeof(info), kWidth, kHeight, 1};
    RendererOwner renderer{api, api->createRenderer(&info)}; Require(renderer.handle != nullptr, "source-mip WARP renderer unavailable");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,5000,-1.5707963f,1.5f};
    const War3RenderEnvironmentV2 environment{sizeof(environment),{0,0,1},{1,1,1},{0,0,0},
        WAR3_RENDER_FOG_NONE,0,1,0,{0,0,0},0,0,0,0,1,0,nullptr};
    // 空mesh会在z=0画默认grid，与测试平面共面；明确的相机外背景保留原平面/深度及完整ROI。
    const std::array<War3RenderVertexV2, 3> background{{
        {10000,10000,0,.045f,.060f,.075f}, {10100,10000,0,.045f,.060f,.075f},
        {10000,10100,0,.045f,.060f,.075f}}};
    const std::array<std::uint32_t, 3> backgroundIndices{0,1,2};
    const War3RenderMeshV2 ground{sizeof(ground),3,3,kRevision,background.data(),backgroundIndices.data()};
    Require(api->setEnvironment(renderer.handle,&environment) == 0 && api->setCamera(renderer.handle,&camera) == 0
        && api->submitMesh(renderer.handle,&ground) == 0, "source-mip camera/environment/controlled background rejected");
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, getEditorPresentationTarget)
        + sizeof(api->getEditorPresentationTarget) && api->getEditorPresentationTarget,
        "source-mip accepted model-slot identity capability missing");
    const auto modelTarget = [&] {
        War3RenderEditorPresentationTargetV2 target{sizeof(target)};
        Require(api->getEditorPresentationTarget(renderer.handle, &target) == 0 && target.structSize == sizeof(target)
            && target.mode == WAR3_EDITOR_PRESENTATION_STATIC && target.instanceCount == 1
            && target.particleDrawCount == 0 && target.modelEpoch != 0 && !target.sceneVersion && !target.contentVersion,
            "source-mip accepted model-slot identity differs from the static fixture");
        return target;
    };
    std::array<CapturedFrame, 2> frames;
    std::array<War3RenderEditorPresentationTargetV2, 2> targetsBefore{}, targetsAfter{};
    const std::array<const ModelSceneDescriptor*, 2> descriptors{&sourceDescriptor, &controlDescriptor};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        Require(api->submitModelScene(renderer.handle,descriptors[index]->StaticScene()) == 0, "source-mip production descriptor rejected");
        targetsBefore[index] = modelTarget();
        frames[index] = CaptureFrame(renderer,kWidth,kHeight,kRevision,camera);
        targetsAfter[index] = modelTarget();
        const auto& before = targetsBefore[index]; const auto& after = targetsAfter[index];
        Require(before.structSize == after.structSize && before.mode == after.mode
            && before.instanceCount == after.instanceCount && before.particleDrawCount == after.particleDrawCount
            && before.modelEpoch == after.modelEpoch && before.sceneVersion == after.sceneVersion
            && before.contentVersion == after.contentVersion, "accepted CPU model slot changed during source-mip capture");
        War3RenderStatusV2 status{sizeof(status)};
        Require(api->getStatus(renderer.handle,&status) == 0 && status.result == 0 && status.software == 1
            && status.documentRevision == kRevision && status.width == kWidth && status.height == kHeight,
            "source-mip captured frame device identity mismatch");
    }
    const bool sameMvp = std::memcmp(frames[0].receipt.view.viewProjection,frames[1].receipt.view.viewProjection,
        sizeof(frames[0].receipt.view.viewProjection)) == 0;
    std::cerr << "source_mip_identity mvp_equal=" << sameMvp
        << " A_surface=" << frames[0].receipt.view.surfaceEpoch << " B_surface=" << frames[1].receipt.view.surfaceEpoch
        << " A_scene=" << frames[0].receipt.view.sceneEpoch << " B_scene=" << frames[1].receipt.view.sceneEpoch
        << " A_frame=" << frames[0].receipt.view.frameCount << " B_frame=" << frames[1].receipt.view.frameCount
        << " A_model_epoch=" << targetsBefore[0].modelEpoch << " B_model_epoch=" << targetsBefore[1].modelEpoch
        << " A_ticket=" << frames[0].receipt.capture.ticket << " B_ticket=" << frames[1].receipt.capture.ticket << '\n';
    Require(std::memcmp(frames[0].receipt.view.viewProjection,frames[1].receipt.view.viewProjection,
        sizeof(frames[0].receipt.view.viewProjection)) == 0
        && frames[0].receipt.view.surfaceEpoch == frames[1].receipt.view.surfaceEpoch
        // sceneEpoch属于未改变的ground；模型完整替换的身份是独立modelEpoch。
        && frames[1].receipt.view.sceneEpoch == frames[0].receipt.view.sceneEpoch
        && frames[1].receipt.view.viewEpoch == frames[0].receipt.view.viewEpoch
        && targetsBefore[1].modelEpoch > targetsBefore[0].modelEpoch
        && frames[1].receipt.capture.ticket > frames[0].receipt.capture.ticket
        && frames[1].receipt.view.frameCount > frames[0].receipt.view.frameCount, "A/B did not complete the same camera/geometry revision");
    const auto minimumFootprint = RequireMinifiedInterior(frames[0].receipt.view);
    const std::array<std::size_t, 2> matches{MatchingSamples(frames[0],1), MatchingSamples(frames[1],0)};
    Export(output,source,*providerSha.value,Digest(geometry),frames,targetsBefore,targetsAfter,matches,minimumFootprint,sourceTexture);
    Require(matches[0] >= kMinimumSamples && matches[1] >= kMinimumSamples,
        "original source low mips must render green while the identical red-top GenerateMips control stays red");
    std::cout << "model source mips: production decode/baked scene/descriptors, identical top/geometry/camera/provider, "
        << "two atomic WARP captures; green=" << matches[0] << " red=" << matches[1] << " ROI samples passed\n";
}

} // namespace ModelSourceMipTest
