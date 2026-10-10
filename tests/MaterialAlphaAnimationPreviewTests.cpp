// KMTA 宿主回归：复用原创模型/贴图写入器，不依赖作者地图或游戏目录。
#include "AnimationParticleContinuityTests.h"
#include "../src/Render/Animation/AnimationPoseWarnings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
using AnimationParticleContinuityTest::Good;
using War3RenderCore::Code;

bool Near(float actual, float expected) {
    return std::isfinite(actual) && std::abs(actual - expected) <= 0.0002f;
}

std::vector<std::byte> DuplicateAlphaModel(bool lateLayerBoundary = false) {
    // 仅替换共享原创夹具的 MTLS；Stand [0,1000]、第二动作 [2000,3000] 和骨架保留。
    const auto base = AnimationTest::Bytes();
    MdxTest::Writer output;
    output.Tag("MDLX");
    for (std::size_t at = 4; at + 8 <= base.size();) {
        std::uint32_t size = 0;
        std::memcpy(&size, base.data() + at + 4, 4);
        Check(size <= base.size() - at - 8, "original MTLS fixture chunk truncated");
        if (std::memcmp(base.data() + at, "MTLS", 4)) output.Raw(base.data() + at, size + 8);
        else {
            output.Tag("MTLS");
            const auto chunk = output.Begin(), material = output.Begin();
            output.U32(0); output.U32(0); output.Tag("LAYS"); output.U32(4);
            for (std::uint32_t layerIndex = 0; layerIndex < 4; ++layerIndex) {
                const auto layer = output.Begin();
                output.U32(layerIndex % 2); output.U32(0); output.U32(0);
                output.U32(UINT32_MAX); output.U32(0); output.F32(1);
                output.Tag("KMTA"); output.U32(7); output.U32(1); output.U32(UINT32_MAX);
                for (auto frame : {0u, 250u, 250u, 1000u, 2000u, 2500u, 3000u}) {
                    const auto valueFrame = frame;
                    if (lateLayerBoundary && layerIndex == 3 && frame == 2000) ++frame;
                    const float upper = valueFrame <= 2000 ? 1.0f : valueFrame == 2500 ? .5f : 0.0f;
                    output.U32(frame); output.F32(layerIndex < 2 ? upper : 1.0f - upper);
                }
                output.EndInclusive(layer);
            }
            output.EndInclusive(material); output.EndChunk(chunk);
        }
        at += size + 8;
    }
    return std::move(output.bytes);
}

struct Fixture {
    std::vector<std::byte> model;
    const std::vector<std::byte> original;
    std::vector<std::byte> texture = AnimationParticleContinuityTest::Texture();
    AnimationInstanceSpec spec;
    explicit Fixture(bool boundary = false) : model(DuplicateAlphaModel(boundary)), original(model) {
        spec.identity = {17, 9}; spec.hostKey = "original-kmta-owner:17";
        spec.model.modelPath = "original-kmta-duplicates.mdx";
        spec.model.position = {123, 456, 789};
        spec.sequenceIndex = 0; spec.elapsedMs = 500; spec.globalMs = 73;
    }
    auto Create(const AnimationLimits& limits, std::span<const AnimationInstanceSpec> specs) const {
        return ModelAnimationService::Create({9, 27, "original-kmta-host-fixture"}, specs,
            [this](std::string_view path, std::uint64_t maximum) {
                const auto& bytes = path == spec.model.modelPath ? model : texture;
                if (bytes.size() > maximum) return Result<AnimationResource>::Failure(
                    "Test", "FIXTURE_RESOURCE_LIMIT", "原创资源超过读取预算。");
                return Result<AnimationResource>::Success({bytes, "original-kmta-host-fixture", std::string(64, 'd')});
            }, limits);
    }
    auto Create(const AnimationLimits& limits) const { return Create(limits, std::span(&spec, 1)); }
    void Unchanged() const { Check(model == original, "host altered original KMTA source bytes"); }
};

AnimationLimits PreviewLimits() {
    AnimationLimits limits;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    limits.allowIdenticalMaterialAlphaKeysPreview = true;
    return limits;
}

void RequireAlpha(const AnimatedModelScene& scene, const AnimationFrame& frame,
    const AnimationPoseLink& pose, std::uint32_t materialIndex, float upper) {
    Check(!pose.hidden && pose.poseVersion && (pose.animated || pose.staticForBudget) &&
        frame.dynamicMaterialAlphas && scene.dynamicMaterialAlphas,
        "KMTA preview did not expose actual successful dynamic layer alpha");
    Check(pose.renderedInstance < frame.instances.size(), "draw owner outside frame");
    const auto modelIndex = frame.instances[pose.renderedInstance].instance.model;
    Check(modelIndex < scene.pickModels.size() && scene.pickModels[modelIndex].source &&
        modelIndex < scene.modelLayerRanges.size(), "draw owner lost original parsed material source");
    const auto& parsed = *scene.pickModels[modelIndex].source;
    Check(materialIndex < parsed.materials.size() && parsed.materials[materialIndex].layers.size() == 4,
        "four-layer source material missing");
    std::uint32_t flat = 0;
    for (std::uint32_t i = 0; i < materialIndex; ++i) flat += static_cast<std::uint32_t>(parsed.materials[i].layers.size());
    const auto draw = scene.modelLayerRanges[modelIndex];
    Check(draw.count == pose.materialAlphas.count && draw.offset <= scene.drawLayerBindings.size() &&
        draw.count <= scene.drawLayerBindings.size() - draw.offset &&
        pose.materialAlphas.offset <= frame.layerAlphas.size() &&
        pose.materialAlphas.count <= frame.layerAlphas.size() - pose.materialAlphas.offset,
        "actual draw material alpha mapping truncated");
    std::array<bool, 4> seen{};
    for (std::uint32_t i = 0; i < draw.count; ++i) {
        const auto binding = scene.drawLayerBindings[draw.offset + i].materialLayer;
        if (binding < flat || binding - flat >= 4) continue;
        const auto layer = binding - flat;
        Check(Near(frame.layerAlphas[pose.materialAlphas.offset + i], layer < 2 ? upper : 1 - upper),
            "actual host draw alpha used static fallback or lost complementary fade");
        seen[layer] = true;
    }
    Check(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
        "four original material layers were not present in host draw output");
}

// 核心 0.1.18 起 KMTA 非全局轨迹按原版逐动作区间取值：同帧键直接合法，不再产生同帧同值预览警告。
AnimationPoseWarningView NoMaterialWarnings(const AnimatedModelScene& scene, const AnimationFrame& frame,
    const AnimationPoseLink& pose) {
    const auto view = Good(ReadAnimationPoseWarnings(scene, frame, pose));
    Check(view.records.size() == view.materialRecords.size() && !view.omitted, "warning view lost parallel typed records");
    for (std::size_t i = 0; i < view.records.size(); ++i)
        Check(view.records[i].code != Code::MaterialAlphaIdenticalDuplicatesPreview &&
            view.materialRecords[i].materialIndex == War3RenderCore::NoIndex,
            "game-range KMTA still published duplicate material warnings");
    return view;
}

void Flags() {
    Check(!AnimationLimits{}.allowIdenticalMaterialAlphaKeysPreview, "material duplicate preview enabled by default");
    for (const auto policy : {AnimationPlaybackPolicy::StrictReplay, AnimationPlaybackPolicy::InteractivePreview}) {
        for (const bool flag : {false, true}) {
            for (const bool retain : {false, true}) {
                Fixture fixture; AnimationLimits limits;
                limits.playbackPolicy = policy; limits.allowIdenticalMaterialAlphaKeysPreview = flag;
                limits.retainUnsupportedStatic = retain;
                const auto service = Good(fixture.Create(limits));
                const auto frame = Good(service->Prepare(nullptr));
                Check(frame->poses.size() == 1 && frame->animatedInstances == 1 && !frame->hiddenInstances && !frame->poses[0].hidden,
                    "same-frame KMTA keys rejected although the game accepts them");
                RequireAlpha(*service->Scene(), *frame, frame->poses[0], 0, 1.0f);
                NoMaterialWarnings(*service->Scene(), *frame, frame->poses[0]);
                fixture.Unchanged();
            }
        }
    }
    Fixture fixture;
    std::array specs{fixture.spec, fixture.spec};
    specs[1].identity.value = 18; specs[1].hostKey = "original-kmta-owner:18";
    specs[1].sequenceIndex = 1; specs[1].elapsedMs = 500; specs[1].globalMs = 91;
    const auto service = Good(fixture.Create(PreviewLimits(), specs));
    auto frame = Good(service->Prepare(nullptr)); const auto scene = service->Scene();
    Check(frame->poses.size() == 2 && frame->animatedInstances == 2 && !frame->hiddenInstances,
        "material preview failed actual owners");
    for (const auto& pose : frame->poses) {
        const auto r = pose.requestIndex;
        RequireAlpha(*scene, *frame, pose, 0, r ? .5f : 1.0f); // 两个实例各自的动作/时钟决定透明度。
        NoMaterialWarnings(*scene, *frame, pose);
    }
    AnimationAdvance advance; AnimationEdit edit;
    edit.identity = specs[1].identity; edit.elapsedMs = 750; advance.edits.push_back(edit);
    frame = Good(service->Prepare(frame.get(), advance));
    for (const auto& pose : frame->poses) RequireAlpha(*scene, *frame, pose, 0, pose.requestIndex ? .25f : 1.0f);
    fixture.Unchanged();
    std::cout << "all_policies_game_ranges=1 no_duplicate_warnings=1 owner_clocks_independent=1\n";
}

void Warnings() {
    Fixture fixture; const auto service = Good(fixture.Create(PreviewLimits()));
    const auto frame = Good(service->Prepare(nullptr)); const auto scene = service->Scene();
    NoMaterialWarnings(*scene, *frame, frame->poses[0]);
    auto query = Good(AnimationDiagnosticQuery::Create(scene)); Check(query->SetFrame(frame).Succeeded(), "warning frame Query failed");
    const auto page = Good(query->Query(Good(query->Binding(0, fixture.spec.identity))));
    for (const auto& issue : page.issues)
        Check(issue.code != War3RenderCore::CodeName(Code::MaterialAlphaIdenticalDuplicatesPreview), "Inspector listed obsolete duplicate warning");
    // 第二动作首键改到 2001：原版区间内首键之前回绕到末键，不再是边界错误。
    Fixture shifted(true); auto limits = PreviewLimits(); limits.retainUnsupportedStatic = true;
    const auto shiftedService = Good(shifted.Create(limits));
    auto healthy = Good(shiftedService->Prepare(nullptr));
    AnimationAdvance advance; AnimationEdit edit; edit.identity = shifted.spec.identity;
    edit.sequenceIndex = 1; edit.elapsedMs = 0; advance.edits.push_back(edit);
    const auto moved = Good(shiftedService->Prepare(healthy.get(), advance));
    Check(!moved->poses[0].hidden && moved->poses[0].poseVersion, "pre-first-key KMTA time hid the instance instead of wrapping");
    fixture.Unchanged(); shifted.Unchanged();

    // 复用原序宿主测试的有界共享Stand场景，实际触发缓存与动态槽混用。
    std::vector<AnimationInstanceSpec> specs(1024, fixture.spec);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        specs[i].identity.value = i + 1; specs[i].hostKey = "original-kmta-cache:" + std::to_string(i);
        specs[i].sequenceIndex = 1; specs[i].elapsedMs = 400 + i % 200;
    }
    limits = PreviewLimits(); limits.retainUnsupportedStatic = true;
    const auto metadata = specs.size() * (sizeof(War3RenderSkinnedModelInstanceV2) + sizeof(AnimationPoseLink) +
        sizeof(AnimationInstanceState) + sizeof(AnimationPickBounds));
    limits.maxFrameBytes = static_cast<std::uint64_t>(metadata) + 131072; limits.maxActivePoseBytes = 4096;
    const auto cachedService = Good(fixture.Create(limits, specs));
    const auto cachedFrame = Good(cachedService->Prepare(nullptr)); const auto cachedScene = cachedService->Scene();
    Check(cachedFrame->sharedPoseRanges && cachedFrame->budgetStaticInstances && cachedFrame->animatedInstances,
        "material cache fixture did not really mix cached Stand and active pose");
    std::size_t cached = cachedFrame->poses.size();
    for (std::size_t i = 0; i < cachedFrame->poses.size(); ++i) {
        const auto& pose = cachedFrame->poses[i]; NoMaterialWarnings(*cachedScene, *cachedFrame, pose);
        // 缓存 Stand 用 Stand 中点透明度；活动实例按自己第二动作时刻取值。
        if (pose.staticForBudget) { cached = i; RequireAlpha(*cachedScene, *cachedFrame, pose, 0, 1); }
        else RequireAlpha(*cachedScene, *cachedFrame, pose, 0, 1.0f - static_cast<float>(*specs[pose.requestIndex].elapsedMs) / 1000.0f);
    }
    Check(cached < cachedFrame->poses.size(), "actual cached Stand pose not found");
    fixture.Unchanged();
    std::cout << "no_duplicate_warnings=1 pre_first_key_wraps=1 cached_stand_and_active_alpha=1\n";
}

std::filesystem::path ModelEnvironment() {
#ifdef _WIN32
    const auto size = GetEnvironmentVariableW(L"FORGE_KMTA_MODEL", nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const auto count = GetEnvironmentVariableW(L"FORGE_KMTA_MODEL", value.data(), size);
    Check(count && count < size, "FORGE_KMTA_MODEL changed during capture");
    value.resize(count);
    return value;
#else
    const auto* value = std::getenv("FORGE_KMTA_MODEL");
    return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

int RealModel() {
    const auto localPath = ModelEnvironment();
    if (localPath.empty()) { std::cout << "SKIP FORGE_KMTA_MODEL is unset\n"; return 77; }
    std::ifstream input(localPath, std::ios::binary | std::ios::ate);
    Check(static_cast<bool>(input), "local KMTA model could not be read");
    const auto size = input.tellg(); Check(size == 1579976, "local KMTA model identity size changed; root must bind SHA");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size)); input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(input), "local KMTA model read truncated");
    const auto parsed = Good(ParseMdx(bytes));
    Check(parsed.materials.size() == 14 && parsed.sequences.size() == 24 && parsed.materials[3].layers.size() == 4 &&
        parsed.sequences[6].start == 81841 && parsed.sequences[6].end == 86010 &&
        parsed.sequences[17].start == 159238 && parsed.sequences[17].end == 167590,
        "local Lich material/action identity changed");
    const auto original = bytes; const auto image = AnimationParticleContinuityTest::Texture();
    // 此组仅验CPU宿主姿势/来源，贴图使用明确原创占位；不是游戏或真实贴图渲染验收。
    AnimationInstanceSpec spec; spec.identity = {17, 9}; spec.hostKey = "units:16:55656172:14";
    spec.model.modelPath = "local-lich-kmta-model.mdx"; spec.sequenceIndex = 6; spec.elapsedMs = 838; spec.globalMs = 1525;
    auto limits = PreviewLimits(); limits.allowSourceOrderNodePreview = true; limits.retainUnsupportedStatic = true;
    const auto service = Good(ModelAnimationService::Create({9, 27, "local-lich-kmta-cpu-test"}, std::span(&spec, 1),
        [&](std::string_view path, std::uint64_t maximum) {
            const auto& resource = path == spec.model.modelPath ? bytes : image;
            if (resource.size() > maximum) return Result<AnimationResource>::Failure("Test", "FIXTURE_RESOURCE_LIMIT", "本机资源超过读取预算。");
            return Result<AnimationResource>::Success({resource, path == spec.model.modelPath ? "local-readonly-kmta-model" : "original-cpu-test-texture",
                std::string(64, path == spec.model.modelPath ? 'd' : 'a')});
        }, limits));
    const auto scene = service->Scene(); auto frame = Good(service->Prepare(nullptr));
    std::uint32_t flat = 0;
    for (std::uint32_t i = 0; i < 3; ++i) flat += static_cast<std::uint32_t>(parsed.materials[i].layers.size());
    // 宿主实际绘制透明度须等于共用原版区间参考（核心另有逐位比对）；不在此写死游戏数值。
    const auto expectLayers = [&](std::uint32_t sequence, std::uint64_t elapsed, std::uint64_t global) {
        const auto reference = Good(SampleMdxLayerAlphas(parsed, {0, sequence, elapsed, global}));
        const auto& pose = frame->poses[0];
        Check(!pose.hidden && pose.poseVersion, "real Lich KMTA pose hidden");
        const auto modelIndex = frame->instances[pose.renderedInstance].instance.model;
        const auto draw = scene->modelLayerRanges[modelIndex];
        std::uint32_t seen = 0;
        for (std::uint32_t i = 0; i < draw.count; ++i) {
            const auto binding = scene->drawLayerBindings[draw.offset + i].materialLayer;
            if (binding < flat || binding - flat >= 4) continue;
            Check(Near(frame->layerAlphas[pose.materialAlphas.offset + i], reference[binding]), "real Lich host alpha differs from game-range reference");
            ++seen;
        }
        Check(seen >= 4, "real Lich material 3 layers missing from draw output");
        NoMaterialWarnings(*scene, *frame, pose);
    };
    // 该模型 TXAN 两条均为全局序列 KTAT（滚动贴图）：宿主须逐帧发布非单位且随全局时钟变化的坐标仿射。
    Check(scene->dynamicMaterialStates && parsed.textureAnimations.size() == 2, "real Lich TXAN did not enable material states");
    const auto uvStates = [&] {
        std::vector<float> values; const auto& pose = frame->poses[0];
        for (std::uint32_t i = 0; i < pose.materialStates.count; ++i) {
            const auto& state = frame->materialStates[pose.materialStates.offset + i];
            values.insert(values.end(), std::begin(state.uvTransform), std::end(state.uvTransform));
        }
        return values;
    };
    const auto identityOnly = [](const std::vector<float>& values) {
        for (std::size_t i = 0; i < values.size(); i += 6)
            if (values[i] != 1 || values[i + 1] != 0 || values[i + 2] != 0 || values[i + 3] != 0 || values[i + 4] != 1 || values[i + 5] != 0)
                return false;
        return true;
    };
    expectLayers(6, 838, 1525);
    const auto firstUv = uvStates(); bool uvMoved = false;
    for (const auto time : {0u, 407u, 408u, 814u, 815u, 816u, 8352u}) {
        AnimationAdvance advance; AnimationEdit edit; edit.identity = spec.identity;
        edit.sequenceIndex = 17; edit.elapsedMs = time; edit.globalMs = 1525 + time; advance.edits.push_back(edit);
        frame = Good(service->Prepare(frame.get(), advance));
        expectLayers(17, time, 1525 + time);
        uvMoved |= uvStates() != firstUv;
    }
    Check(!identityOnly(firstUv) && uvMoved, "real Lich TXAN scrolling did not reach the host material state");
    Check(bytes == original, "CPU host test altered local model bytes");
    std::cout << "real_local_lich_host_matches_game_range_reference=1 points=8 txan_uv_moves=1 cpu_host_only=1\n";
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected flags, warnings or real-model group");
        const std::string_view group = argv[1];
        if (group == "flags") Flags();
        else if (group == "warnings") Warnings();
        else if (group == "real-model") return RealModel();
        else throw std::runtime_error("unknown material alpha host test group");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MaterialAlphaAnimationPreviewTests: " << error.what() << '\n';
        return 1;
    }
}
