#pragma once
#include "AnimationTestModel.h"
#include "../src/Render/Animation/AnimationParticleMaterial.h"
#include "../src/Render/Animation/AnimationParticles.h"
#include "War3MapCore/Hash.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace AnimationRibbonMaterialTest {
using namespace War3Map;
using namespace War3Map::Render;

inline void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> inline T Good(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty()
        ? "empty ribbon material failure" : result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    return std::move(*result.value);
}

// 原创骨架/网格夹具上写入一个PRE2、两个RIBB；两条带仅引用不同单层材质。
// 两个版本的唯一源字节差是第一材质层的0x20位，便于检查模拟与表达互不混淆。
inline std::vector<std::byte> Model(bool firstUnfogged) {
    const auto source = AnimationTest::Bytes();
    MdxTest::Writer writer;
    writer.Tag("MDLX");
    bool materials = false, pivots = false;
    for (std::size_t at = 4; at + 8 <= source.size();) {
        std::uint32_t size = 0;
        std::memcpy(&size, source.data() + at + 4, sizeof(size));
        Check(size <= source.size() - at - 8, "ribbon material source chunk truncated");
        const std::string_view tag(reinterpret_cast<const char*>(source.data() + at), 4);
        if (tag == "MTLS") {
            writer.Tag("MTLS");
            const auto chunk = writer.Begin();
            for (std::uint32_t index = 0; index < 2; ++index) {
                const auto material = writer.Begin();
                writer.U32(0); writer.U32(0); writer.Tag("LAYS"); writer.U32(1);
                const auto layer = writer.Begin();
                writer.U32(2); // MDX Blend；PRE2下方使用其独立枚举0。
                writer.U32(index == 0 && firstUnfogged ? 0x21u : 1u);
                writer.U32(0); writer.U32(UINT32_MAX); writer.U32(0); writer.F32(1);
                writer.EndInclusive(layer); writer.EndInclusive(material);
            }
            writer.EndChunk(chunk);
            materials = true;
        } else if (tag == "PIVT") {
            writer.Tag("PIVT"); writer.U32(size + 36); writer.Raw(source.data() + at + 8, size);
            for (std::uint32_t node = 2; node < 5; ++node) {
                writer.F32(static_cast<float>(node) * 10); writer.F32(0); writer.F32(1);
            }
            pivots = true;
        } else {
            writer.Raw(source.data() + at, size + 8);
        }
        at += size + 8;
    }
    Check(materials && pivots, "ribbon material fixture lost material or pivot chunks");
    writer.Tag("PRE2");
    {
        const auto chunk = writer.Begin(), object = writer.Begin(), node = writer.Begin();
        writer.Fixed("Material-neutral PRE2", 80); writer.U32(2); writer.U32(0); writer.U32(0x1000);
        writer.EndInclusive(node);
        for (const auto value : {0.f, 0.f, 0.f, 0.f, 2.f, 100.f, 0.f, 0.f}) writer.F32(value);
        writer.U32(0); writer.U32(1); writer.U32(1); writer.U32(0); writer.F32(0); writer.F32(.5f);
        for (unsigned color = 0; color < 3; ++color) {
            writer.F32(.2f); writer.F32(.4f); writer.F32(.8f);
        }
        const unsigned char alpha[]{255, 255, 255}; writer.Raw(alpha, sizeof(alpha));
        for (unsigned scale = 0; scale < 3; ++scale) writer.F32(1);
        for (unsigned interval = 0; interval < 4; ++interval) {
            writer.U32(0); writer.U32(0); writer.U32(1);
        }
        writer.U32(0); writer.U32(0); writer.U32(0); writer.U32(0);
        writer.EndInclusive(object); writer.EndChunk(chunk);
    }
    writer.Tag("RIBB");
    {
        const auto chunk = writer.Begin();
        for (std::uint32_t ribbon = 0; ribbon < 2; ++ribbon) {
            const auto object = writer.Begin(), node = writer.Begin();
            writer.Fixed("Independent ribbon " + std::to_string(ribbon), 80);
            writer.U32(3 + ribbon); writer.U32(0); writer.U32(0x4000); writer.EndInclusive(node);
            for (const auto value : {3.f, 2.f, .5f, .2f, .4f, .8f, 2.f}) writer.F32(value);
            writer.U32(0); writer.U32(100); writer.U32(1); writer.U32(1); writer.U32(ribbon); writer.F32(0);
            writer.EndInclusive(object);
        }
        writer.EndChunk(chunk);
    }
    return std::move(writer.bytes);
}

inline std::vector<std::byte> Texture() {
    std::vector<std::byte> bytes(1181);
    std::memcpy(bytes.data(), "BLP1", 4); bytes[158] = std::byte{255};
    const auto word = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::byte>((value >> (8 * i)) & 255u);
    };
    for (const auto offset : {4u, 12u, 16u, 92u}) word(offset, 1);
    word(28, 1180);
    return bytes;
}

struct CapabilityProbe {
    std::uint32_t format = WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1, mask = WAR3_PARTICLE_UNFOGGED;
    std::int32_t callResult = 0, completedResult = 0;
    unsigned calls = 0;
    bool legacy = false, cleanTail = true, completeSize = true;
};
inline std::int32_t WAR3_RENDER_CALL GetCapabilities(War3RendererHandle handle, War3RenderSkinStatusV2* output) {
    auto& probe = *static_cast<CapabilityProbe*>(handle);
    ++probe.calls;
    probe.completeSize &= output->structSize == sizeof(War3RenderParticleMaterialCapabilitiesV2);
    if (output->structSize < sizeof(War3RenderParticleMaterialCapabilitiesV2)) return 1;
    auto& capabilities = *reinterpret_cast<War3RenderParticleMaterialCapabilitiesV2*>(output);
    probe.cleanTail &= capabilities.particleMaterialFlagsFormat == 0 && capabilities.particleMaterialFlagsMask == 0;
    War3RenderSkinStatusV2 prefix{};
    prefix.structSize = sizeof(prefix); prefix.result = probe.completedResult;
    *output = prefix; // 旧provider只写已知前缀；调用方必须每次把新尾部清零。
    if (!probe.legacy) {
        capabilities.particleMaterialFlagsFormat = probe.format;
        capabilities.particleMaterialFlagsMask = probe.mask;
    }
    return probe.callResult;
}

inline void Capabilities() {
    CapabilityProbe probe;
    War3RenderPluginApiV2 api{};
    api.structSize = sizeof(api); api.apiVersion = WAR3_RENDER_PLUGIN_API_V2; api.getSkinStatus = GetCapabilities;
    Check(!AnimationParticleMaterialSupported(nullptr, &probe)
        && !AnimationParticleMaterialSupported(&api, nullptr), "null particle material provider accepted");
    api.apiVersion = 1;
    Check(!AnimationParticleMaterialSupported(&api, &probe), "wrong particle material API version accepted");
    api.apiVersion = WAR3_RENDER_PLUGIN_API_V2;
    for (const auto size : {offsetof(War3RenderPluginApiV2, getSkinStatus),
            offsetof(War3RenderPluginApiV2, getSkinStatus) + sizeof(api.getSkinStatus) - 1}) {
        api.structSize = static_cast<std::uint32_t>(size);
        Check(!AnimationParticleMaterialSupported(&api, &probe), "short particle material API tail accepted");
    }
    api.structSize = sizeof(api); api.getSkinStatus = nullptr;
    Check(!AnimationParticleMaterialSupported(&api, &probe) && probe.calls == 0,
        "missing capability function called or advertised support");
    api.getSkinStatus = GetCapabilities;
    probe.legacy = true;
    Check(!AnimationParticleMaterialSupported(&api, &probe), "zero legacy capability tail guessed as support");
    probe.legacy = false;
    for (const auto format : {0u, 2u}) {
        probe.format = format;
        Check(!AnimationParticleMaterialSupported(&api, &probe), "unknown particle material format accepted");
    }
    probe.format = WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1;
    for (const auto mask : {0u, 2u}) {
        probe.mask = mask;
        Check(!AnimationParticleMaterialSupported(&api, &probe), "mask without Unfogged accepted");
    }
    probe.mask = WAR3_PARTICLE_UNFOGGED; probe.callResult = 1;
    Check(!AnimationParticleMaterialSupported(&api, &probe), "failed capability call advertised particle material support");
    probe.callResult = 0; probe.completedResult = -1;
    Check(AnimationParticleMaterialSupported(&api, &probe), "pending/failed completed frame hid static provider capability");
    probe.mask = WAR3_PARTICLE_UNFOGGED | 2u;
    Check(AnimationParticleMaterialSupported(&api, &probe), "capability ignored supported known bit within a larger mask");
    probe.legacy = true;
    Check(!AnimationParticleMaterialSupported(&api, &probe) && probe.cleanTail && probe.completeSize,
        "particle capability reused stale tail bytes or sent a truncated output descriptor");
}

inline void OutputCapacity() {
    AnimationFrame frame;
    frame.particleVertices.reserve(9); frame.particleDraws.reserve(3); frame.particleMaterialFlags.reserve(37);
    frame.particleVertices.resize(3); frame.particleDraws.resize(1); frame.particleMaterialFlags.resize(1);
    const auto expected = std::uint64_t(frame.particleVertices.capacity()) * sizeof(War3RenderParticleVertexV2)
        + std::uint64_t(frame.particleDraws.capacity()) * sizeof(War3RenderParticleDrawV2)
        + std::uint64_t(frame.particleMaterialFlags.capacity()) * sizeof(std::uint32_t);
    const auto retained = AnimationParticleOutputBytes(frame);
    Check(retained == expected && frame.particleMaterialFlags.capacity() > frame.particleMaterialFlags.size(),
        "particle output budget counted flags size instead of retained capacity");
    frame.particleVertices.clear(); frame.particleDraws.clear(); frame.particleMaterialFlags.clear();
    Check(AnimationParticleOutputBytes(frame) == retained, "shrinking particle output silently refunded retained storage");
    Check(AnimationParticleOutputBytes(0, 0, 65536) == 256ull * 1024,
        "full particle material array exceeded or escaped its original draw budget");
}

inline void Descriptors(const AnimationFrame& frame, const AnimationFrame& empty) {
    Check(CheckAnimationParticleMaterialFrame(frame).Succeeded(), "host rejected valid per-draw particle materials");
    auto pose = frame.Describe();
    const auto legacy = frame.DescribeParticles(&pose);
    const auto material = frame.DescribeParticleMaterial(&pose);
    Check(legacy.structSize == sizeof(War3RenderParticleUpdateV2)
        && legacy.drawCount == frame.particleDraws.size() && legacy.draws == frame.particleDraws.data()
        && material.base.structSize == sizeof(War3RenderParticleMaterialUpdateV2)
        && material.materialFlagsFormat == WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1
        && material.materialFlagsCount == legacy.drawCount && material.materialFlags == frame.particleMaterialFlags.data()
        && material.base.vertices == legacy.vertices && material.base.draws == legacy.draws
        && material.base.sceneVersion == frame.sceneVersion && material.base.contentVersion == frame.contentVersion
        && material.base.modelPose == &pose, "particle legacy/material descriptors lost their owned frame identity");
    const AnimationParticleDescriptor oldProvider(frame, &pose, false), newProvider(frame, &pose, true);
    Check(oldProvider.Get()->structSize == sizeof(War3RenderParticleUpdateV2)
        && newProvider.Get()->structSize == sizeof(War3RenderParticleMaterialUpdateV2)
        && newProvider.Get()->modelPose == &pose, "particle provider selection truncated or invented an extension");
    auto emptyPose = empty.Describe();
    const auto emptyMaterial = empty.DescribeParticleMaterial(&emptyPose);
    Check(CheckAnimationParticleMaterialFrame(empty).Succeeded() && emptyMaterial.base.drawCount == 0
        && emptyMaterial.materialFlagsCount == 0 && emptyMaterial.materialFlags == nullptr,
        "empty particle material descriptor retained a pointer or old flag count");
    auto invalid = frame;
    invalid.particleMaterialFlags.pop_back();
    const auto mismatch = CheckAnimationParticleMaterialFrame(invalid);
    Check(!mismatch.Succeeded() && !mismatch.diagnostics.empty()
        && mismatch.diagnostics.front().code == "ANIMATION_PARTICLE_MATERIAL_MAPPING",
        "host particle material count mismatch was accepted");
    invalid = frame; invalid.particleMaterialFlags[0] = 2;
    Check(!CheckAnimationParticleMaterialFrame(invalid).Succeeded()
        && frame.particleMaterialFlags[0] == 0, "unknown host material bit accepted or changed an immutable frame");
}

inline void Run() {
    Capabilities(); OutputCapacity();
    const auto source = Model(true), ordinary = Model(false), original = source, ordinaryOriginal = ordinary;
    Check(source.size() == ordinary.size()
        && std::inner_product(source.begin(), source.end(), ordinary.begin(), std::size_t{}, std::plus<>{},
            [](std::byte a, std::byte b) { return std::size_t(a != b); }) == 1,
        "ribbon material A/B source differs beyond its single Unfogged bit");
    const auto texture = Texture(), textureOriginal = texture;
    const auto digest = Good(Sha256(source)), ordinaryDigest = Good(Sha256(ordinary)), textureDigest = Good(Sha256(texture));
    std::array<AnimationInstanceSpec, 2> specs;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        specs[i].identity = {i + 1, 7}; specs[i].hostKey = "ribbon-material:" + std::to_string(i);
        specs[i].model.modelPath = "ribbons.mdx"; specs[i].model.position = {static_cast<float>(i) * 200, 0, 0};
        specs[i].sequenceIndex = 0; specs[i].elapsedMs = 0; specs[i].globalMs = 0;
    }
    unsigned sourceReads = 0;
    const auto read = [&](std::string_view path, std::uint64_t) {
        if (path == "ribbons.mdx") ++sourceReads;
        return Result<AnimationResource>::Success({path == "ribbons.mdx" ? source : texture,
            "original-ribbon-material-fixture", path == "ribbons.mdx" ? digest : textureDigest});
    };
    const auto ordinaryRead = [&](std::string_view path, std::uint64_t) {
        return Result<AnimationResource>::Success({path == "ribbons.mdx" ? ordinary : texture,
            "original-ribbon-material-fixture", path == "ribbons.mdx" ? ordinaryDigest : textureDigest});
    };
    AnimationLimits limits; limits.enableParticles = true;
    const auto service = Good(ModelAnimationService::Create({7, 33, digest}, specs, read, limits));
    const auto allFogged = Good(ModelAnimationService::Create({7, 33, ordinaryDigest}, specs, ordinaryRead, limits));
    Check(service->Scene()->particleUnfogged && !allFogged->Scene()->particleUnfogged
        && service->Scene()->particleInstanceCount == 2, "scene material summary ignored actual accepted ribbon bindings");
    for (const auto& capability : service->Scene()->particleCapabilities)
        Check(capability.available && capability.particle2Count == 1 && capability.ribbonCount == 2
            && (!capability.skippedEmitters || capability.skippedEmitters->empty()),
            "valid PRE2/two-RIBB fixture lost a source emitter");
    const auto fallbackWarning = AnimationParticleMaterialWarning(*service->Scene(), false);
    Check(fallbackWarning.Succeeded() && fallbackWarning.diagnostics.size() == 1
        && fallbackWarning.diagnostics.front().severity == DiagnosticSeverity::Warning
        && fallbackWarning.diagnostics.front().code == "PARTICLE_UNFOGGED_PREVIEW"
        && AnimationParticleMaterialWarning(*service->Scene(), true).diagnostics.empty()
        && AnimationParticleMaterialWarning(*allFogged->Scene(), false).diagnostics.empty(),
        "particle material warning concealed old-provider approximation or warned for ordinary ribbons");
    AnimationAdvance advance;
    advance.worldCamera = War3RenderCore::LocalCamera{{0, -100, 100}, {0, 0, 1}};
    const auto empty = Good(service->Prepare(nullptr, advance));
    const auto ordinaryEmpty = Good(allFogged->Prepare(nullptr, advance));
    Check(empty->particleVertices.empty() && empty->particleDraws.empty() && empty->particleMaterialFlags.empty(),
        "zero-time ribbon material fixture already emitted geometry");
    for (std::size_t i = 0; i < specs.size(); ++i) {
        AnimationEdit edit; edit.identity = specs[i].identity;
        edit.elapsedMs = 150 + 50 * i; edit.globalMs = 150 + 50 * i; advance.edits.push_back(edit);
    }
    const auto frame = Good(service->Prepare(empty.get(), advance));
    const auto ordinaryFrame = Good(allFogged->Prepare(ordinaryEmpty.get(), advance));
    Check(frame->particleLinks.size() == 2 && ordinaryFrame->particleLinks.size() == 2 && frame->particleDraws.size() == 6
        && frame->particleMaterialFlags.size() == frame->particleDraws.size(),
        "actual host output omitted an instance or emitter material draw");
    for (std::size_t i = 0; i < frame->particleLinks.size(); ++i) {
        const auto& link = frame->particleLinks[i];
        Check(link.simulated && !link.hidden && link.drawCount == 3 && link.particleCount > 0 && link.ribbonSegmentCount > 2
            && link.firstDraw <= frame->particleMaterialFlags.size()
            && link.drawCount <= frame->particleMaterialFlags.size() - link.firstDraw
            && frame->particleMaterialFlags[link.firstDraw] == 0
            && frame->particleMaterialFlags[link.firstDraw + 1] == WAR3_PARTICLE_UNFOGGED
            && frame->particleMaterialFlags[link.firstDraw + 2] == 0,
            "PRE2/RIBB flags were asset-ORed, reordered or coupled across instances");
        Check(link.stateHash == ordinaryFrame->particleLinks[i].stateHash
            && link.geometryHash != ordinaryFrame->particleLinks[i].geometryHash,
            "Unfogged changed simulation/RNG state or failed to enter geometry identity");
        for (std::uint32_t draw = link.firstDraw; draw < link.firstDraw + link.drawCount; ++draw)
            Check(frame->particleDraws[draw].vertexCount > 0 && frame->particleDraws[draw].filter == WAR3_PARTICLE_BLEND
                && frame->particleDraws[draw].texture == frame->particleDraws.front().texture,
                "material-only fixture changed filter, texture or nonempty draw coverage");
    }
    Check(frame->particleVertices.size() == ordinaryFrame->particleVertices.size()
        && frame->particleDraws.size() == ordinaryFrame->particleDraws.size()
        && std::memcmp(frame->particleVertices.data(), ordinaryFrame->particleVertices.data(),
            frame->particleVertices.size() * sizeof(War3RenderParticleVertexV2)) == 0
        && std::memcmp(frame->particleDraws.data(), ordinaryFrame->particleDraws.data(),
            frame->particleDraws.size() * sizeof(War3RenderParticleDrawV2)) == 0
        && std::all_of(ordinaryFrame->particleMaterialFlags.begin(), ordinaryFrame->particleMaterialFlags.end(),
            [](std::uint32_t value) { return value == 0; }),
        "Unfogged changed geometry numbers, draw layout or ordinary material flags");
    const auto snapshotFlags = frame->particleMaterialFlags;
    const auto readsBeforePause = sourceReads;
    const auto repeated = Good(service->Prepare(frame.get()));
    Check(repeated->particleMaterialFlags == snapshotFlags && frame->particleMaterialFlags == snapshotFlags
        && repeated->particleMaterialFlags.data() != frame->particleMaterialFlags.data()
        && repeated->particleLinks[0].stateHash == frame->particleLinks[0].stateHash
        && repeated->particleLinks[0].geometryHash == frame->particleLinks[0].geometryHash && sourceReads == readsBeforePause,
        "paused redraw borrowed flags, reread source or changed particle state/geometry");
    Descriptors(*frame, *empty);
    Check(source == original && ordinary == ordinaryOriginal && texture == textureOriginal
        && Good(Sha256(source)) == digest && Good(Sha256(ordinary)) == ordinaryDigest,
        "particle material preparation rewrote original MDX/texture source bytes");
    std::cout << "ribbon material CPU: PRE2+twoRIBB x2, per-draw flags, source exact, geometry-only identity, owned descriptors, capacity budget, capabilities\n";
}
} // namespace AnimationRibbonMaterialTest
