#include "War3MapCore/Render/AnimationPreviewBudget.h"
#include "AnimationParticleContinuityTests.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
AnimationPreviewBudgetSample Sample(std::uint64_t number, std::uint64_t cost) {
    AnimationPreviewBudgetSample sample;
    sample.frameNumber = sample.contentVersion = number;
    sample.completedSteadyNs = 1000000000ull + number * 33000000ull;
    sample.completedCpuNs = cost; sample.animationCpuNs = cost / 2;
    sample.completed = sample.visible = true; return sample;
}
void Controller() {
    AnimationPreviewBudgetController controller;
    Check(controller.Status().quality == AnimationPreviewQuality::Balanced && controller.Status().instanceLimit == 2048 &&
        !controller.Status().hasCompletedFrameSample, "default quality fabricated a measured sample");
    Check(!AnimationPreviewQualityFromName("unknown") && AnimationPreviewQualityFromName("power-saving") ==
        AnimationPreviewQuality::PowerSaving, "quality parser accepted an unknown preference");
    controller.SetOmissions(997, 1100);
    auto invalid = Sample(1, 8000000); invalid.completed = false;
    Check(!controller.Observe(invalid) && !controller.Status().hasCompletedFrameSample, "rejected frame changed budget evidence");
    invalid = Sample(1, 8000000); invalid.visible = false;
    Check(!controller.Observe(invalid), "occluded frame influenced adaptation");
    for (std::uint64_t i = 1; i <= 11; ++i) (void)controller.Observe(Sample(i, 8000000));
    Check(controller.Status().instanceLimit == 2048, "one fast sample raised the budget without hysteresis");
    Check(controller.Observe(Sample(12, 8000000)) && controller.Status().instanceLimit == 2560,
        "completed fast frames did not expand a visibly omitted budget");
    Check(!controller.Status().gpuDrawNs && controller.Status().completedCpuNs == 8000000,
        "CPU upload/draw/present was mislabeled as GPU time");
    auto duplicate = Sample(13, 100000000); duplicate.contentVersion = 12;
    Check(!controller.Observe(duplicate) && controller.Status().completedCpuNs == 8000000,
        "a repeated pose was charged repeatedly by renderer frames");
    controller.SetQuality(AnimationPreviewQuality::PowerSaving);
    Check(controller.Status().instanceLimit == 256 && controller.Status().minimumIntervalMs == 66,
        "power-saving did not use its own bounded work/cadence");
    for (std::uint64_t i = 1; i <= 3; ++i) (void)controller.Observe(Sample(i, 100000000));
    Check(controller.Status().instanceLimit == 192, "sustained slow completed frames did not reduce admission");
    controller.SetTemporaryFull(true);
    Check(controller.ParticleBudget().maxInstances == UINT32_MAX && controller.ParticleBudget().maxAnchorSamples ==
        kMaximumPreviewAnchorSamples, "temporary full removed structural safety limits or retained a time cap");
    for (std::uint64_t i = 4; i <= 20; ++i) (void)controller.Observe(Sample(i, 100000000));
    controller.SetTemporaryFull(false);
    Check(controller.Status().instanceLimit == 192, "temporary full secretly changed the selected adaptive limit");
    controller.ResetMeasurements();
    Check(controller.Status().quality == AnimationPreviewQuality::PowerSaving && !controller.Status().temporaryFull &&
        !controller.Status().hasCompletedFrameSample && controller.Status().instanceLimit == 256,
        "scene reset persisted temporary full or lost the author quality preference");
    controller.SetQuality(AnimationPreviewQuality::Complete);
    Check(controller.ParticleBudget().maxInstances == UINT32_MAX, "complete mode retained time-based omissions");
    AnimationPreviewBudgetController idle;
    for (std::uint64_t i = 1; i <= 120; ++i) (void)idle.Observe(Sample(i, 1000000));
    Check(idle.Status().instanceLimit == 2048, "budget expanded despite no omitted effects");
    AnimationPreviewBudgetController gpu;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        auto actual = Sample(i, 1000000); actual.gpuDrawNs = 100000000;
        (void)gpu.Observe(actual);
    }
    Check(gpu.Status().instanceLimit == 1536 && gpu.Status().gpuDrawNs == 100000000,
        "actual GPU timestamp was ignored or confused with CPU time");
    const auto cool = gpu.Status().instanceLimit;
    for (std::uint64_t i = 4; i <= 8; ++i) (void)gpu.Observe(Sample(i, 100000000));
    Check(gpu.Status().instanceLimit == cool, "budget oscillated within the 250ms hold period");
    std::cout << "controller: measured feedback, hysteresis, modes, unknown/duplicate frames and scene reset\n";
}
void Admission() {
    using AnimationParticleContinuityTest::Model;
    using AnimationParticleContinuityTest::Texture;
    using AnimationParticleContinuityTest::Good;
    auto model = Model();
    Image::DecodedImage image; image.width=image.height=8; image.rgba.assign(8*8*4,std::byte{255});
    const auto texture = Good(Image::EncodeBlpPaletted(image));
    // 降低原创夹具的发射率，单独命中全图锚样瓶颈，而非总活粒子内存门。
    const auto word = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(model[offset + i]) << (i * 8);
        return value;
    };
    bool changedRate = false;
    for (std::size_t at = 4; at + 8 <= model.size();) {
        const auto size = word(at + 4);
        if (std::memcmp(model.data() + at, "PRE2", 4) == 0) {
            const auto nodeSize = word(at + 12);
            const auto rateOffset = at + 12 + nodeSize + 20;
            Check(rateOffset + 4 <= at + 8 + size, "fixture PRE2 rate offset was invalid");
            const auto raw = std::bit_cast<std::uint32_t>(1.0f);
            for (unsigned i = 0; i < 4; ++i) model[rateOffset + i] = static_cast<std::byte>((raw >> (i * 8)) & 255u);
            changedRate = true; break;
        }
        at += 8 + size;
    }
    Check(changedRate, "fixture did not contain PRE2");
    const auto read = [&](std::string_view path, std::uint64_t) {
        return Result<AnimationResource>::Success({path == "budget.mdx" ? model : texture,
            "budget-admission-fixture", std::string(64, 'b')});
    };
    constexpr std::size_t count = 800;
    std::vector<AnimationInstanceSpec> specs(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto& spec = specs[i]; spec.identity = {i + 1, 7}; spec.hostKey = "budget:" + std::to_string(i);
        spec.model.modelPath = "budget.mdx"; spec.sequenceIndex = 1;
        spec.elapsedMs = 500; spec.globalMs = 0; spec.playing = true;
    }
    AnimationLimits limits; limits.enableParticles = limits.retainUnsupportedStatic = true;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    auto service = Good(ModelAnimationService::Create({7, 1, "budget-fixture"}, specs, read, limits));
    const auto residentScene=service->Scene();
    Check(!residentScene->geometry.textures.empty() && !residentScene->particleTextures.empty(),
        "source mip budget fixture did not create actual model and particle texture owners");
    std::uint64_t fullTextureBytes=0,topTextureBytes=0;
    for (const auto* textures:{&residentScene->geometry.textures,&residentScene->particleTextures})
        for (const auto& decoded:*textures) {
            Check(decoded.additionalMips.size()==3 && ModelTextureRgbaBytes(decoded)==340,
                "model/particle admission discarded lower source mip levels");
            fullTextureBytes+=ModelTextureRgbaBytes(decoded); topTextureBytes+=decoded.rgba.size();
        }
    auto tightTextures=limits; tightTextures.geometry.maxTotalTextureBytes=fullTextureBytes-1;
    Check(topTextureBytes<=tightTextures.geometry.maxTotalTextureBytes,
        "source mip budget fixture would already exceed its old top-only bill");
    const auto rejectedTextures=ModelAnimationService::Create({7,1,"budget-fixture"},std::span(specs.data(),1),read,tightTextures);
    Check(!rejectedTextures.Succeeded() && !rejectedTextures.diagnostics.empty() &&
        rejectedTextures.diagnostics.front().code=="ANIMATION_PARTICLE_TEXTURE_BUDGET",
        "particle copy admitted a complete source chain using the old top-only budget");
    AnimationAdvance advance; advance.deltaMs = 33; advance.playAll = true;
    advance.worldCamera = War3RenderCore::LocalCamera{{0, 0, 100}, {0, 0, 1}};
    auto baseline = Good(service->Prepare(nullptr, advance));
    Check(baseline->budgetParticleInstances > 0 && baseline->timings.particleSimulationRequests < count,
        "fixture failed to reproduce original world anchor admission omissions");
    advance.previewParticleBudget = AnimationParticleFrameBudget{2, 18};
    auto limited = Good(service->Prepare(baseline.get(), advance));
    Check(limited->timings.particleSimulationRequests == 2 && limited->budgetParticleInstances == count - 2 &&
        limited->budgetParticleEmitters == count - 2 && limited->instances.size() == count,
        "soft effect admission hid models or counted PREM/offscreen as omitted supported emitters");
    const auto frozen = std::find_if(limited->particleLinks.begin(), limited->particleLinks.end(), [](const auto& link) {
        return link.budgetLimited && link.frozen;
    });
    Check(frozen != limited->particleLinks.end(), "budget omitted a prior particle state without retaining its checkpoint");
    advance.previewParticleBudget = AnimationParticleFrameBudget{UINT32_MAX, kMaximumPreviewAnchorSamples};
    auto full = Good(service->Prepare(limited.get(), advance));
    Check(full->timings.particleSimulationRequests == count && full->budgetParticleInstances == 0 &&
        full->budgetParticleEmitters == 0 && full->instances.size() == count && full->states.size() == count,
        "explicit complete preview did not resolve a safe anchor-budget bottleneck");
    Check(full->states.front().elapsedMs == baseline->states.front().elapsedMs + 66,
        "soft/full transition reset the animation clock or advanced unaccepted history");
    const auto savedVersion = full->contentVersion;
    advance.previewParticleBudget->maxAnchorSamples = kMaximumPreviewAnchorSamples + 1;
    const auto invalid = service->Prepare(full.get(), advance);
    Check(!invalid.Succeeded() && invalid.diagnostics.front().code == "ANIMATION_PREVIEW_BUDGET_INVALID" &&
        full->contentVersion == savedVersion, "oversized preview anchors bypassed safety or mutated accepted frame");
    AnimationLimits strictLimits;
    auto strictSpec = specs.front(); strictSpec.identity.generation = 8;
    auto strict = Good(ModelAnimationService::Create({8, 1, "strict-fixture"}, std::span(&strictSpec, 1), read, strictLimits));
    advance.previewParticleBudget = AnimationParticleFrameBudget{};
    Check(!strict->Prepare(nullptr, advance).Succeeded(), "preview soft policy silently weakened strict replay");
    auto constrained = limits; constrained.particles.maxAnchorNodeVisits = 1;
    auto hardSpec = specs.front(); hardSpec.identity.generation = 9;
    auto hard = Good(ModelAnimationService::Create({9, 1, "hard-node-fixture"}, std::span(&hardSpec, 1), read, constrained));
    auto retained = Good(hard->Prepare(nullptr, advance));
    Check(retained->budgetParticleInstances == 1 && retained->budgetParticleEmitters == 1 &&
        retained->timings.particleSimulationRequests == 0, "complete preview bypassed the node safety budget");
    std::cout << "admission: original anchor bottleneck, model retention, frozen checkpoint, full recovery and hard/strict gates\n";
}
} // namespace
int main(int argc, char** argv) try {
    const std::string_view group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "controller") Controller();
    if (group == "all" || group == "admission") Admission();
    if (group != "all" && group != "controller" && group != "admission") throw std::runtime_error("unknown budget test group");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
