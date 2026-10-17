// PREM 宿主 CPU 回归：模型经正式 byte parser / service，不使用模型 DTO 或假节点采样器。
#include "AnimationParticleContinuityTests.h"
#include "War3MapCore/Render/AnimationPremFrame.h"
#include "War3MapCore/Render/AnimationParticleCoverage.h"
#include "War3MapCore/Hash.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
constexpr std::string_view kParentPath = "OriginalPremParent.mdx";
constexpr std::string_view kChildPath = "Particles\\OriginalChild.MDL";

bool Near(float value, float expected) { return std::isfinite(value) && std::abs(value - expected) < .003f; }
std::uint32_t Word(const std::vector<std::byte>& bytes, std::size_t at) {
    Check(at <= bytes.size() && bytes.size() - at >= 4, "original fixture word outside bytes");
    std::uint32_t value = 0; std::memcpy(&value, bytes.data() + at, 4); return value;
}
void Patch(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    Check(at <= bytes.size() && bytes.size() - at >= 4, "original fixture patch outside bytes");
    std::memcpy(bytes.data() + at, &value, 4);
}
std::string ChildPath(std::uint32_t index) {
    return index ? "Particles\\OriginalChild" + std::to_string(index) + ".MDL" : std::string(kChildPath);
}

std::vector<std::byte> ParentModel(std::uint32_t emitters = 1, float rate = 20, bool transparent = false,
    std::uint32_t childPathSeed = 0) {
    auto base = AnimationTest::Bytes();
    MdxTest::Writer writer; writer.Tag("MDLX");
    for (std::size_t at = 4; at + 8 <= base.size();) {
        const auto size = Word(base, at + 4); Check(size <= base.size() - at - 8, "original parent chunk truncated");
        const std::string_view tag(reinterpret_cast<const char*>(base.data() + at), 4);
        if (tag == "SEQS") {
            Check(size == 264, "original two-sequence fixture changed");
            // 共享夹具按时间顺序：Stand=0 [0,1000]、第二动作=1 [2000,3000]；后者改为不循环的 Death。
            const auto death = at + 8 + 132;
            Check(Word(base, death + 80) == 2000 && Word(base, death + 84) == 3000, "original [2000,3000] sequence moved");
            std::fill_n(base.begin() + static_cast<std::ptrdiff_t>(death), 80, std::byte{});
            std::memcpy(base.data() + death, "Death", 5); Patch(base, death + 92, 1);
        }
        if (tag == "GEOA" && transparent) {
            writer.Tag("GEOA"); const auto chunk = writer.Begin();
            for (std::uint32_t mesh = 0; mesh < 2; ++mesh) {
                const auto record = writer.Begin(); writer.F32(0); writer.U32(0);
                writer.F32(1); writer.F32(1); writer.F32(1); writer.U32(mesh); writer.EndInclusive(record);
            }
            writer.EndChunk(chunk);
        } else if (tag != "PIVT") writer.Raw(base.data() + at, size + 8);
        at += size + 8;
    }
    writer.Tag("PREM"); const auto chunk = writer.Begin();
    for (std::uint32_t i = 0; i < emitters; ++i) {
        const auto record = writer.Begin(), node = writer.Begin();
        writer.Fixed("OriginalHostEmitter" + std::to_string(i), 80);
        writer.U32(i + 2); writer.U32(1); writer.U32(0x9000); writer.EndInclusive(node);
        writer.F32(rate); writer.F32(0); writer.F32(0); writer.F32(0);
        writer.Fixed(ChildPath(i + childPathSeed), 260); writer.F32(1.2f); writer.F32(0);
        writer.Tag("KPEV"); writer.U32(5); writer.U32(0); writer.U32(UINT32_MAX);
        for (const auto [time, visible] : std::array<std::pair<std::uint32_t, float>, 5>{
            {{0, 0.0f}, {1000, 0.0f}, {2000, 1.0f}, {2300, 0.0f}, {3000, 0.0f}}}) {
            writer.U32(time); writer.F32(visible);
        }
        writer.EndInclusive(record);
    }
    writer.EndChunk(chunk); writer.Tag("PIVT"); writer.U32((emitters + 2) * 12);
    for (std::uint32_t i = 0; i < emitters + 2; ++i) {
        writer.F32(i < 2 ? 0.0f : 4.0f); writer.F32(i < 2 ? 0.0f : 5.0f); writer.F32(i < 2 ? 0.0f : 6.0f);
    }
    return std::move(writer.bytes);
}
std::vector<std::byte> ChildModel() {
    return AnimationTest::WithoutSyntheticConstantKmtf(MdxTest::BuildTestMdx());
}
// 两种发射器均来自原创 MDX 字节，经生产 parser；PRE2 节点3有真实第四个pivot。
std::vector<std::byte> MixedParentModel() {
    auto bytes = ParentModel(); const auto particle = AnimationParticleContinuityTest::Model();
    bool copied = false, enlarged = false;
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        const auto size = Word(bytes, at + 4); Check(size <= bytes.size() - at - 8, "mixed parent chunk truncated");
        if (std::memcmp(bytes.data() + at, "PIVT", 4) == 0) {
            Check(size == 36, "mixed original pivot shape changed"); Patch(bytes, at + 4, size + 12);
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + size), 12, std::byte{}); enlarged = true; break;
        }
        at += size + 8;
    }
    for (std::size_t at = 4; at + 8 <= particle.size();) {
        const auto size = Word(particle, at + 4); Check(size <= particle.size() - at - 8, "mixed PRE2 chunk truncated");
        if (std::memcmp(particle.data() + at, "PRE2", 4) == 0) {
            auto block = std::vector<std::byte>(particle.begin() + static_cast<std::ptrdiff_t>(at),
                particle.begin() + static_cast<std::ptrdiff_t>(at + 8 + size));
            Check(Word(block, 8 + 4 + 4 + 80) == 2, "original PRE2 node offset changed");
            Patch(block, 8 + 4 + 4 + 80, 3); bytes.insert(bytes.end(), block.begin(), block.end()); copied = true; break;
        }
        at += size + 8;
    }
    Check(copied && enlarged, "mixed original emitter/pivot missing"); return bytes;
}

struct Fixture {
    std::map<std::string, AnimationResource> resources;
    mutable std::map<std::string, std::uint32_t> reads;
    AnimationInstanceSpec spec;
    explicit Fixture(std::uint32_t emitters = 1, float rate = 20, bool transparent = false) {
        Add(std::string(kParentPath), ParentModel(emitters, rate, transparent), "original-parent-bytes");
        for (std::uint32_t i = 0; i < emitters; ++i) Add(ChildPath(i), ChildModel(), "original-child-bytes");
        spec.identity = {17, 9}; spec.hostKey = "original-prem-host:17";
        spec.model.modelPath = kParentPath; spec.model.position = {123, 456, 789};
        spec.model.scale = {2, 3, 4}; spec.model.angle = 1.57079632679f;
        spec.model.tint = {.25f, .5f, .75f};
        spec.sequenceIndex = 1; spec.elapsedMs = 250; spec.globalMs = 1250; spec.playing = true;
    }
    void Add(std::string path, std::vector<std::byte> bytes, std::string layer) {
        const auto sha = Good(Sha256(bytes)); resources.insert_or_assign(std::move(path), AnimationResource{std::move(bytes), std::move(layer), sha});
    }
    Result<AnimationResource> Read(std::string_view path, std::uint64_t maximum) const {
        ++reads[std::string(path)]; const auto found = resources.find(std::string(path));
        if (found != resources.end()) {
            if (found->second.bytes.size() > maximum) return Result<AnimationResource>::Failure("Test", "FIXTURE_RESOURCE_LIMIT", "原创资源超过读取预算。");
            return Result<AnimationResource>::Success(found->second);
        }
        auto key = std::string(path); std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key.ends_with(".blp")) {
            auto bytes = AnimationParticleContinuityTest::Texture(); const auto sha = Good(Sha256(bytes));
            return Result<AnimationResource>::Success({std::move(bytes), "original-test-blp-not-game-texture", sha});
        }
        return Result<AnimationResource>::Failure("Test", "RESOURCE_NOT_FOUND", "本原创测试资源不存在。");
    }
    auto Create(AnimationLimits limits, std::span<const AnimationInstanceSpec> specs) const {
        return ModelAnimationService::Create({9, 27, "original-prem-host-sources"}, specs,
            [this](std::string_view path, std::uint64_t maximum) { return Read(path, maximum); }, limits);
    }
    auto Create(AnimationLimits limits) const { return Create(limits, std::span(&spec, 1)); }
    void Unchanged() const {
        for (const auto& [path, resource] : resources) {
            (void)path; Check(Good(Sha256(resource.bytes)) == resource.digest, "host changed readonly input bytes or SHA");
        }
    }
};
AnimationLimits Limits() {
    AnimationLimits limits; limits.enablePremPreview = true;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview; return limits;
}
const AnimationPremFrame& Prem(const AnimatedModelScene& scene, const AnimationFrame& frame) {
    Check(bool(frame.prem), "formal host did not publish PREM frame"); const auto& prem = *frame.prem;
    Check(prem.parentSceneVersion == scene.sceneVersion && prem.parentContentVersion == frame.contentVersion &&
        prem.parentDocumentRevision == scene.documentRevision && prem.parentGeneration == scene.generation &&
        prem.parentSourceIdentity == scene.sourceIdentity && prem.parentEvaluationEpoch && prem.capabilities,
        "PREM parent frame/source/epoch identity missing");
    for (const auto& clock : prem.requestClocks) Check(clock.requestIndex < frame.states.size() &&
        clock.identity == frame.states[clock.requestIndex].identity &&
        clock.parentElapsedMs == frame.states[clock.requestIndex].elapsedMs &&
        clock.parentGlobalMs == frame.states[clock.requestIndex].globalMs,
        "PREM independent clock checkpoint lost the current accepted parent state");
    return prem;
}
const AnimationPremRequestClock& Clock(const AnimationPremFrame& prem) {
    Check(prem.requestClocks.size() == 1 && prem.requestClocks[0].requestIndex == 0 &&
        prem.requestClocks[0].identity == War3RenderCore::InstanceIdentity{17, 9}, "independent owner tail clock missing");
    return prem.requestClocks[0];
}
void CheckSlots(const AnimatedModelScene& scene, const AnimationFrame& frame) {
    const auto& prem = Prem(scene, frame); Check(prem.childStandFrozenApproximation, "child Stand approximation was not disclosed");
    Check(prem.childScene && prem.childScene == scene.premGeometry && prem.childFrame &&
        !prem.childFrame->sharedPoseRanges && prem.childScene->specs.size() % 32 == 0,
        "child pool missing actual independent frame/ranges");
    std::set<std::uint32_t> filled;
    for (const auto& slot : prem.childSlots) {
        Check(slot.childIndex < prem.emission.children.size() && slot.childRequestIndex < prem.childFrame->states.size() &&
            filled.insert(slot.childRequestIndex).second, "child metadata borrowed an invalid/duplicate slot");
        const auto& child = prem.emission.children[slot.childIndex]; const auto& state = prem.childFrame->states[slot.childRequestIndex];
        const auto pose = std::find_if(prem.childFrame->poses.begin(), prem.childFrame->poses.end(), [&](const auto& candidate) {
            return candidate.requestIndex == slot.childRequestIndex && candidate.identity == state.identity;
        });
        Check(pose != prem.childFrame->poses.end() && pose->animated && !pose->hidden && !pose->outsideView &&
            !pose->staticForBudget && !pose->noVisibleGeometry && pose->poseVersion &&
            pose->renderedInstance < prem.childFrame->instances.size() &&
            std::equal(child.transform.begin(), child.transform.end(), prem.childFrame->instances[pose->renderedInstance].instance.transform),
            "filled child did not reach an actual independent successful draw pose/transform");
        Check(pose->firstAlpha <= prem.childFrame->alphas.size() && pose->alphaCount <= prem.childFrame->alphas.size() - pose->firstAlpha &&
            std::any_of(prem.childFrame->alphas.begin() + pose->firstAlpha,
                prem.childFrame->alphas.begin() + pose->firstAlpha + pose->alphaCount, [](float value) { return value > 0; }),
            "filled child lost all actual owned geoset alpha");
        Check(child.parentRequestIndex < frame.states.size() && child.parentIdentity == frame.states[child.parentRequestIndex].identity &&
            child.emitter && child.emitter->Binding().parentAsset == frame.poses[child.parentRequestIndex].asset &&
            frame.poses[child.parentRequestIndex].evaluationEpoch == prem.parentEvaluationEpoch &&
            state.transform == child.transform && state.tint == frame.states[child.parentRequestIndex].tint,
            "child birth clock, transform, owner/source or tint differed from accepted parent");
        const auto clock = std::find_if(prem.requestClocks.begin(), prem.requestClocks.end(), [&](const auto& current) {
            return current.requestIndex == child.parentRequestIndex && current.identity == child.parentIdentity &&
                current.asset == child.emitter->Binding().parentAsset;
        });
        Check(clock != prem.requestClocks.end() && child.birthElapsedMs + child.ageMs == clock->elapsedMs &&
            child.birthGlobalMs + child.ageMs == clock->globalMs, "child birth clock belonged to another parent request");
    }
    Check(!filled.empty(), "visible child test had no filled slots");
    for (const auto& pose : prem.childFrame->poses) {
        Check(pose.requestIndex < prem.childFrame->states.size() && pose.firstAlpha <= prem.childFrame->alphas.size() &&
            pose.alphaCount <= prem.childFrame->alphas.size() - pose.firstAlpha, "child owned alpha bounds broken");
        if (!filled.contains(pose.requestIndex)) {
            Check(std::all_of(prem.childFrame->alphas.begin() + pose.firstAlpha,
                prem.childFrame->alphas.begin() + pose.firstAlpha + pose.alphaCount, [](float value) { return value == 0; }),
                "unused child slot retained visible alpha");
            Check(pose.materialAlphas.offset <= prem.childFrame->layerAlphas.size() &&
                pose.materialAlphas.count <= prem.childFrame->layerAlphas.size() - pose.materialAlphas.offset,
                "unused child material alpha bounds broken");
            Check(std::all_of(prem.childFrame->layerAlphas.begin() + pose.materialAlphas.offset,
                prem.childFrame->layerAlphas.begin() + pose.materialAlphas.offset + pose.materialAlphas.count,
                [](float value) { return value == 0; }), "unused child slot retained material alpha");
        }
    }
}
AnimationAdvance View(bool outside, std::uint64_t delta = 0) {
    auto advance = AnimationParticleContinuityTest::View(outside ? 1000000.0f : 123.0f);
    advance.playAll.reset(); advance.deltaMs = delta; return advance;
}
void Lifecycle() {
    Fixture fixture; auto service = Good(fixture.Create(Limits())); const auto scene = service->Scene();
    auto frame = Good(service->Prepare(nullptr));
    Check(frame->poses.size() == 1 && frame->poses[0].animated && !frame->poses[0].hidden &&
        Prem(*scene, *frame).emission.children.size() == 5 && Prem(*scene, *frame).sampleErrors.empty(),
        "formal typed/source host failed initial Death250 visible window");
    CheckSlots(*scene, *frame);
    for (const auto& child : frame->prem->emission.children) {
        const float localX = 24.0f + 40.0f * static_cast<float>(child.birthElapsedMs) / 1000.0f;
        Check(Near(child.transform[3], 108) && Near(child.transform[7], 456 + 2 * localX) && Near(child.transform[11], 813),
            "real node-at-birth/pivot/nonuniform parent rotation/translation was applied more than once");
    }
    AnimationAdvance tick; tick.deltaMs = 1000; frame = Good(service->Prepare(frame.get(), tick));
    Check(frame->states[0].elapsedMs == 1000 && !frame->states[0].playing && Clock(*frame->prem).tailAging &&
        Clock(*frame->prem).elapsedMs == 1250 && !frame->prem->emission.children.empty(), "Death tail did not continue after parent clamp");
    AnimationAdvance pause; pause.playAll = false; pause.deltaMs = 400;
    frame = Good(service->Prepare(frame.get(), pause)); const auto paused = Clock(*frame->prem);
    Check(paused.tailPaused && paused.elapsedMs == 1250, "explicit tail pause did not freeze clock");
    for (unsigned i = 0; i < 3; ++i) {
        frame = Good(service->Prepare(frame.get(), View(true, 500)));
        Check(frame->poses[0].outsideView && Clock(*frame->prem).tailPaused && Clock(*frame->prem).elapsedMs == paused.elapsedMs &&
            frame->prem->emission.children.empty(), "out-of-view pause lost checkpoint or emitted hidden children");
    }
    frame = Good(service->Prepare(frame.get(), View(false)));
    Check(!frame->poses[0].outsideView && Clock(*frame->prem).elapsedMs == paused.elapsedMs && Clock(*frame->prem).tailPaused &&
        !frame->prem->emission.children.empty(), "pause across visibility boundary changed lifespan");
    AnimationAdvance resume; resume.playAll = true;
    frame = Good(service->Prepare(frame.get(), resume));
    Check(!Clock(*frame->prem).tailPaused && Clock(*frame->prem).elapsedMs == paused.elapsedMs,
        "zero-delta resume reset tail clock to clamped parent duration");
    tick.deltaMs = 1000; frame = Good(service->Prepare(frame.get(), tick));
    Check(Clock(*frame->prem).elapsedMs == 2250 && frame->prem->emission.children.empty() && !frame->prem->childFrame,
        "children froze forever at nonloop Death end instead of expiring");
    for (unsigned i = 0; i < 3; ++i) frame = Good(service->Prepare(frame.get(), View(true, 300)));
    const auto aged = Clock(*frame->prem).elapsedMs; frame = Good(service->Prepare(frame.get(), View(false)));
    Check(aged > 2250 && Clock(*frame->prem).elapsedMs == aged && frame->prem->emission.children.empty(),
        "expired children revived after returning from outside view");
    AnimationAdvance seek; AnimationEdit edit; edit.identity = fixture.spec.identity; edit.elapsedMs = 80; edit.globalMs = 1080;
    seek.edits.push_back(edit); frame = Good(service->Prepare(frame.get(), seek));
    Check(!Clock(*frame->prem).tailAging && Clock(*frame->prem).elapsedMs == 80 && frame->prem->emission.children.size() == 1,
        "explicit seek did not reset independent PREM timeline");
    seek.edits[0].sequenceIndex = 0; seek.edits[0].elapsedMs = 500;
    frame = Good(service->Prepare(frame.get(), seek));
    Check(Clock(*frame->prem).sequenceIndex == 0 && frame->prem->emission.children.empty(), "Stand KPEV-off borrowed Death births");
    {
        // 不循环 Death 播完：内部 playAll（相机刷新等）不重播；用户点播放（restartFinished）从头开始。
        Fixture replay; auto replayService = Good(replay.Create(Limits()));
        auto replayFrame = Good(replayService->Prepare(nullptr));
        AnimationAdvance finish; finish.deltaMs = 1000; replayFrame = Good(replayService->Prepare(replayFrame.get(), finish));
        Check(replayFrame->states[0].elapsedMs == 1000 && !replayFrame->states[0].playing, "nonloop Death did not stop at end");
        AnimationAdvance internal; internal.playAll = true; internal.deltaMs = 33;
        replayFrame = Good(replayService->Prepare(replayFrame.get(), internal));
        Check(replayFrame->states[0].elapsedMs == 1000 && !replayFrame->states[0].playing, "internal playAll replayed finished nonloop");
        AnimationAdvance play; play.playAll = true; play.restartFinished = true; play.deltaMs = 33;
        replayFrame = Good(replayService->Prepare(replayFrame.get(), play));
        Check(replayFrame->states[0].elapsedMs == 33 && replayFrame->states[0].playing, "user play did not restart finished nonloop");
        replay.Unchanged();
    }
    Fixture transparent(1, 20, true); auto transparentService = Good(transparent.Create(Limits()));
    const auto clearParent = Good(transparentService->Prepare(nullptr));
    Check(clearParent->poses[0].noVisibleGeometry && !clearParent->poses[0].hidden &&
        !Prem(*transparentService->Scene(), *clearParent).emission.children.empty(), "legal transparent parent GEOA incorrectly blocked PREM");
    fixture.Unchanged(); transparent.Unchanged();
    std::cout << "actual_service_typed_bytes=1 parent_single_transform=1 tail_lifetime=1 pause_outside_resume=1 outside_no_resurrection=1 seek_sequence_reset=1 nonloop_replay=1 unused_alpha_zero=1 transparent_parent=1\n";
}

void Sources() {
    Fixture disabled; AnimationLimits off;
    const auto old = Good(disabled.Create(off)); const auto frame = Good(old->Prepare(nullptr));
    Check(!off.enablePremPreview && !frame->prem && !old->Scene()->premGeometry && !disabled.reads.contains(std::string(kChildPath)),
        "default service read or installed a child PREM layer");
    Fixture missing; auto child = missing.resources.extract(std::string(kChildPath));
    Check(!child.empty(), "test child missing"); child.key() = "Particles\\OriginalChild.mdx"; missing.resources.insert(std::move(child));
    const auto aliasService = Good(missing.Create(Limits())); const auto alias = Good(aliasService->Prepare(nullptr));
    Check(Prem(*aliasService->Scene(), *alias).supportedEmitterInstances == 1 && !alias->prem->emission.children.empty() &&
        missing.reads.contains(std::string(kChildPath)) && missing.reads.contains("Particles\\OriginalChild.mdx") &&
        alias->prem->capabilities->at(0).originalPath == kChildPath &&
        alias->prem->capabilities->at(0).resolvedPath == "Particles\\OriginalChild.mdx", "missing MDL did not use bounded explicit MDX alias");
    Fixture bad; bad.Add(std::string(kChildPath), std::vector<std::byte>(32, std::byte{0x5a}), "exact-upper-corrupt-mdl");
    bad.Add("Particles\\OriginalChild.mdx", ChildModel(), "lower-mdx-must-not-be-used");
    const auto badService = Good(bad.Create(Limits())); const auto rejected = Good(badService->Prepare(nullptr));
    Check(rejected->poses[0].animated && !rejected->poses[0].hidden && Prem(*badService->Scene(), *rejected).unsupportedEmitterInstances == 1 &&
        !bad.reads.contains("Particles\\OriginalChild.mdx") && rejected->prem->emission.children.empty() &&
        !rejected->prem->capabilities->at(0).prepared, "invalid exact MDL silently aliased or hid parent geometry");
    Fixture recursive(2); recursive.Add(std::string(kChildPath), ParentModel(), "recursive-original-child");
    const auto recursiveService = Good(recursive.Create(Limits())); const auto recursiveFrame = Good(recursiveService->Prepare(nullptr));
    Check(recursiveFrame->poses[0].animated && !recursiveFrame->poses[0].hidden &&
        Prem(*recursiveService->Scene(), *recursiveFrame).capabilities->at(0).rejectionCode == "PREM_CHILD_RECURSIVE_UNSUPPORTED" &&
        recursiveFrame->prem->capabilities->at(1).prepared && recursiveFrame->prem->supportedEmitterInstances == 1 &&
        recursiveFrame->prem->unsupportedEmitterInstances == 1 && !recursiveFrame->prem->emission.children.empty() &&
        std::all_of(recursiveFrame->prem->emission.children.begin(), recursiveFrame->prem->emission.children.end(), [](const auto& child) {
            return child.emitter && child.emitter->Source().sourceIndex == 1;
        }), "recursive child escaped local capability gate, hid parent or rejected a separate valid emitter");
    Fixture staleSha; const auto originalChild = staleSha.resources.at(std::string(kChildPath)).bytes;
    staleSha.resources.at(std::string(kChildPath)).digest[0] = staleSha.resources.at(std::string(kChildPath)).digest[0] == '0' ? '1' : '0';
    const auto staleService = Good(staleSha.Create(Limits())); const auto staleFrame = Good(staleService->Prepare(nullptr));
    Check(staleFrame->poses[0].animated && !staleFrame->poses[0].hidden &&
        Prem(*staleService->Scene(), *staleFrame).capabilities->at(0).rejectionCode == "PREM_CHILD_SOURCE_SHA_MISMATCH" &&
        staleFrame->prem->unsupportedEmitterInstances == 1 && staleFrame->prem->emission.children.empty() &&
        staleSha.resources.at(std::string(kChildPath)).bytes == originalChild, "stale advertised SHA bypassed actual frozen resource gate");
    Fixture assets(9); const auto assetService = Good(assets.Create(Limits())); const auto assetFrame = Good(assetService->Prepare(nullptr));
    const auto& accepted = Prem(*assetService->Scene(), *assetFrame);
    Check(accepted.capabilities->size() == 9 && accepted.supportedEmitterInstances == 8 && accepted.unsupportedEmitterInstances == 1 &&
        accepted.childScene && accepted.childScene->specs.size() == 8 * 32 &&
        accepted.capabilities->at(8).rejectionCode == "PREM_CHILD_ASSET_LIMIT", "eight asset / thirty-two slot hard cap enlarged or parent failed");
    disabled.Unchanged(); missing.Unchanged(); bad.Unchanged(); recursive.Unchanged(); assets.Unchanged();
    std::cout << "default_no_child_read=1 missing_only_mdl_alias=1 bad_exact_no_alias=1 recursive_local_reject_other_survives=1 sha_mismatch_rejected=1 assets8_slots32=1\n";
}

void Coverage() {
    Fixture fixture; fixture.Add(std::string(kParentPath), MixedParentModel(), "original-mixed-prem-pre2");
    const auto parsed = Good(ParseMdx(fixture.resources.at(std::string(kParentPath)).bytes));
    Check(parsed.premEmitters.size() == 1 && std::count_if(parsed.otherNodes.begin(), parsed.otherNodes.end(),
        [](const auto& node) { return node.kind == MdxNodeKind::ParticleEmitter2 && node.objectId == 3; }) == 1 && parsed.pivots.size() == 4,
        "mixed original bytes did not actually declare both emitter kinds and distinct pivots");
    for (const bool particles : {false, true}) for (const bool prem : {false, true}) {
        auto limits = Limits(); limits.enableParticles = particles; limits.enablePremPreview = prem;
        const auto service = Good(fixture.Create(limits)); const auto scene = service->Scene();
        const auto legacyNotice = std::any_of(scene->geometry.warnings.begin(), scene->geometry.warnings.end(), [](const auto& item) {
            return item.code == "PARTICLE_PREM_UNSUPPORTED";
        });
        Check(legacyNotice == (particles && !prem),
            "enabled PREM host kept the PRE2 reader's unsupported notice, or disabled PREM lost its notice");
        const auto view = AnimationParticleContinuityTest::View(fixture.spec.model.position[0]);
        auto camera = view.worldView->camera;
        camera.targetY = fixture.spec.model.position[1]; camera.targetZ = fixture.spec.model.position[2];
        AnimationAdvance tick; tick.worldCamera = AnimationWorldCamera(camera); // 显式夹具相机，不启用视野剔除或生产fallback。
        auto frame = Good(service->Prepare(nullptr, tick)); tick.deltaMs = 33;
        frame = Good(service->Prepare(frame.get(), tick));
        const auto coverage = Good(QueryAnimationParticleCoverage(*scene, *frame));
        const auto disabled = static_cast<std::uint64_t>(!particles) + static_cast<std::uint64_t>(!prem && !particles);
        const auto unsupported = static_cast<std::uint64_t>(!prem && particles);
        Check(scene->particlesEnabled == particles && scene->premEnabled == prem && coverage.declared == 2 &&
            coverage.preparedPrem == static_cast<std::uint64_t>(prem) && coverage.disabled == disabled &&
            coverage.unsupportedPrem == unsupported && coverage.notShown == disabled + unsupported &&
            !coverage.isolated && !coverage.assetRejected && !coverage.runtimeRejected && !coverage.budgetRejected && !coverage.outsideView,
            "PREM/PRE2 independent switches lost declaration, disabled, prepared or legacy unsupported coverage");
        Check(frame->poses.size() == 1 && frame->poses[0].animated && !frame->poses[0].hidden,
            "coverage switch fixture lost accepted parent pose");
        if (particles) Check(frame->particleLinks.size() == 1 && frame->particleLinks[0].simulated &&
            frame->particleLinks[0].particleCount && !frame->particleVertices.empty(), "PRE2-on coverage had no actual simulated continuation");
        else Check(frame->particleLinks.empty() && frame->particleVertices.empty(), "PRE2-off generated simulated geometry");
        if (prem) Check(!Prem(*scene, *frame).emission.children.empty() && !frame->prem->childSlots.empty(),
            "PREM-on coverage had no independent emitted child");
        else Check(!frame->prem && !scene->premGeometry, "PREM-off unexpectedly installed child layer");
        std::cout << "coverage PRE2=" << particles << " PREM=" << prem << " declared=" << coverage.declared
            << " preparedPrem=" << coverage.preparedPrem << " unsupportedPrem=" << coverage.unsupportedPrem
            << " disabled=" << coverage.disabled << " notShown=" << coverage.notShown << '\n';
    }
    Fixture unsupported; unsupported.Add(std::string(kParentPath), MixedParentModel(), "original-mixed-unsupported-child");
    unsupported.Add(std::string(kChildPath), ParentModel(), "original-recursive-child");
    auto limits = Limits(); limits.enableParticles = true;
    const auto rejectedService = Good(unsupported.Create(limits));
    AnimationAdvance camera; camera.worldCamera = War3RenderCore::LocalCamera{{0,-100,100},{0,0,1}};
    const auto rejectedFrame = Good(rejectedService->Prepare(nullptr, camera));
    const auto& rejected = Prem(*rejectedService->Scene(), *rejectedFrame);
    Check(rejected.capabilities->size() == 1 && !rejected.capabilities->front().prepared &&
        rejected.capabilities->front().rejectionCode == "PREM_CHILD_RECURSIVE_UNSUPPORTED" &&
        rejected.unsupportedEmitterInstances == 1 && rejectedService->Scene()->particleCapabilities[0].rejectionCode ==
            "PREM_CHILD_RECURSIVE_UNSUPPORTED",
        "removing the generic reader notice hid the actual PREM host rejection");
    Check(std::none_of(rejectedService->Scene()->geometry.warnings.begin(), rejectedService->Scene()->geometry.warnings.end(),
        [](const auto& item) { return item.code == "PARTICLE_PREM_UNSUPPORTED"; }),
        "actual PREM rejection still carried the incorrect generic unimplemented claim");
    unsupported.Unchanged();
    fixture.Unchanged();
}

const AnimationPremCapability& CapabilityFor(const AnimationPremFrame& prem, std::string_view path) {
    const auto key = AnimationPathKey(path); // 正式 AssetSource 绑定归一化key，输入显示路径保留大小写。
    const auto found = std::find_if(prem.capabilities->begin(), prem.capabilities->end(), [&](const auto& capability) {
        return capability.parentSource.virtualPath == key && capability.sourceIndex == 0;
    });
    Check(found != prem.capabilities->end(), "actual parent source lacked a typed PREM capability"); return *found;
}
std::uint32_t RequestFor(const AnimatedModelScene& scene, War3RenderCore::InstanceIdentity identity) {
    const auto found = std::find_if(scene.specs.begin(), scene.specs.end(), [&](const auto& spec) { return spec.identity == identity; });
    Check(found != scene.specs.end(), "resident delta lost concrete owner identity");
    return static_cast<std::uint32_t>(found - scene.specs.begin());
}
void MultipleParents() {
    Fixture fixture;
    const std::string unusedPath = "UnusedBirthCatalog.mdx";
    fixture.Add(unusedPath, ChildModel(), "original-unused-birth-parent");
    auto unused = fixture.spec; unused.identity = {16, 9}; unused.hostKey = "unused-birth-parent";
    unused.model.modelPath = unusedPath;
    const std::array initialSpecs{unused, fixture.spec};
    const auto originalService = Good(fixture.Create(Limits(), initialSpecs));
    const auto originalScene = originalService->Scene(); const auto original = Good(originalService->Prepare(nullptr));
    const auto& originalPrem = Prem(*originalScene, *original);
    Check(originalPrem.supportedEmitterInstances == 1 && originalPrem.sampleErrors.empty(),
        "unused catalog prefix prevented the original actual emitter from sampling");
    std::array<AnimationInstanceSpec, 2> emitting;
    for (std::uint32_t i = 0; i < emitting.size(); ++i) {
        const auto path = "MappedParent" + std::to_string(i + 1) + ".mdx";
        fixture.Add(path, ParentModel(1, 20, false, i + 1), "original-mapped-parent");
        fixture.Add(ChildPath(i + 1), ChildModel(), "original-mapped-child");
        auto spec = fixture.spec; spec.identity = {1000u + i, 9}; spec.hostKey = "mapped-parent:" + std::to_string(i);
        spec.model.modelPath = path;
        for (auto& coordinate : spec.model.position) coordinate += 300.0f * static_cast<float>(i);
        spec.model.tint = {.25f + .25f * static_cast<float>(i), .5f, .75f}; emitting[i] = std::move(spec);
    }
    const auto read = [&fixture](std::string_view path, std::uint64_t maximum) { return fixture.Read(path, maximum); };
    AnimationInstanceDelta replace; replace.session = {9, 28, "original-prem-host-sources"};
    replace.removals.push_back(fixture.spec.identity); replace.additions.assign(emitting.begin(), emitting.end());
    const auto plan = Good(originalService->PrepareInstanceCatalog(replace, read));
    const auto replaced = Good(originalService->PrepareInstanceDelta(*original, *plan)); const auto scene = replaced.service->Scene();
    const auto& current = Prem(*scene, *replaced.frame);
    Check(scene->specs.size() == 3 && scene->specs.front().identity == unused.identity && current.capabilities->size() == 2 &&
        current.supportedEmitterInstances == 2 && !current.unsupportedEmitterInstances && current.sampleErrors.empty() &&
        current.emission.children.size() == 10 && current.childSlots.size() == 10 && current.childScene &&
        current.childScene->specs.size() == 64 && current.requestClocks.size() == 2,
        "multiple actual parents lost compact birth asset mapping, child slots or current request clocks");
    CheckSlots(*scene, *replaced.frame);
    for (std::uint32_t i = 0; i < emitting.size(); ++i) {
        const auto& spec = emitting[i]; const auto request = RequestFor(*scene, spec.identity);
        const auto& capability = CapabilityFor(current, spec.model.modelPath);
        Check(capability.prepared && capability.parentAssetIndex >= 2 &&
            capability.parentSource.contentDigest == fixture.resources.at(spec.model.modelPath).digest &&
            capability.childSource.contentDigest == fixture.resources.at(ChildPath(i + 1)).digest &&
            std::count_if(current.emission.children.begin(), current.emission.children.end(), [&](const auto& child) {
                return child.parentIdentity == spec.identity && child.parentRequestIndex == request;
            }) == 5, "a later compact asset produced no actual births or borrowed the retired/unused source");
        for (const auto& child : current.emission.children) if (child.parentIdentity == spec.identity) {
            const float localX = 24.0f + 40.0f * static_cast<float>(child.birthElapsedMs) / 1000.0f;
            Check(child.emitter && child.emitter->Binding().parentAsset == capability.parentAsset &&
                child.emitter->Binding().parentSource == capability.parentSource &&
                child.emitter->Binding().childSource == capability.childSource &&
                Near(child.transform[3], spec.model.position[0] - 15) &&
                Near(child.transform[7], spec.model.position[1] + 2 * localX) &&
                Near(child.transform[11], spec.model.position[2] + 24),
                "multi-parent birth matrix lost actual node motion, pivot, source or concrete owner transform");
        }
    }
    Check(CapabilityFor(current, emitting[0].model.modelPath).parentAsset !=
        CapabilityFor(current, emitting[1].model.modelPath).parentAsset && originalPrem.sampleErrors.empty() &&
        originalPrem.emission.children.size() == 5, "multi-source rebound mutated the accepted prior parent frame");
    fixture.Unchanged();
    std::cout << "birth_compact_assets2=1 active_unused_prefix1=1 retired_prefix1=1 actual_births_each5=1"
        << " sample_errors0=1 source_owner_matrix_exact=1 independent_child_slots=1\n";
}
void Resident() {
    MultipleParents();
    Fixture fixture; std::array<AnimationInstanceSpec, 8> additions;
    for (std::uint32_t i = 1; i <= 8; ++i) {
        const auto path = "ResidentParent" + std::to_string(i) + ".mdx";
        fixture.Add(path, ParentModel(1, 20, false, i), "original-resident-parent");
        fixture.Add(ChildPath(i), ChildModel(), "original-resident-child");
        auto spec = fixture.spec; spec.identity = {100u + i, 9}; spec.hostKey = "original-resident:" + std::to_string(i);
        spec.model.modelPath = path; additions[i - 1] = std::move(spec);
    }
    auto limits = Limits(); limits.enableParticles = true; // 保证本组测试真正新的 PREM reason，而非旧禁用 PRE2 reason。
    const auto originalService = Good(fixture.Create(limits)); const auto originalScene = originalService->Scene();
    const auto original = Good(originalService->Prepare(nullptr)); const auto& originalPrem = Prem(*originalScene, *original);
    const auto oldAsset = CapabilityFor(originalPrem, kParentPath).parentAsset;
    Check(originalPrem.supportedEmitterInstances == 1 && originalScene->particleCapabilities[0].rejectionCode.empty(),
        "initial prepared parent had stale failure reason");
    const auto read = [&fixture](std::string_view path, std::uint64_t maximum) { return fixture.Read(path, maximum); };
    AnimationInstanceDelta replace; replace.session = {9, 28, "original-prem-host-sources"};
    replace.removals.push_back(fixture.spec.identity); replace.additions.assign(additions.begin(), additions.end());
    const auto plan = Good(originalService->PrepareInstanceCatalog(replace, read));
    const auto replaced = Good(originalService->PrepareInstanceDelta(*original, *plan)); const auto scene = replaced.service->Scene();
    const auto& current = Prem(*scene, *replaced.frame);
    Check(replaced.oldToNewRequests == std::vector<std::uint32_t>{UINT32_MAX} && scene->specs.size() == 8 &&
        current.supportedEmitterInstances == 8 && !current.unsupportedEmitterInstances && current.childScene &&
        current.childScene->specs.size() == 8 * 32 && current.childSlots.size() == 40 && current.emission.children.size() == 40 &&
        current.sampleErrors.empty(),
        "retired resident parent consumed the finite active child-asset pool");
    for (const auto& spec : additions) {
        const auto request = RequestFor(*scene, spec.identity); const auto& cap = CapabilityFor(current, spec.model.modelPath);
        Check(cap.prepared && scene->particleCapabilities[request].premPreparedCount == 1 &&
            scene->particleCapabilities[request].rejectionCode.empty() &&
            std::count_if(current.emission.children.begin(), current.emission.children.end(), [&](const auto& child) {
                return child.parentRequestIndex == request && child.parentIdentity == spec.identity && child.emitter &&
                    child.emitter->Binding().childSource == cap.childSource;
            }) == 5, "current resident source retained a stale rejection or later asset had no actual births");
    }
    CheckSlots(*scene, *replaced.frame);
    for (const auto& child : current.emission.children) {
        Check(child.parentRequestIndex < scene->specs.size() && child.emitter &&
            child.parentIdentity == scene->specs[child.parentRequestIndex].identity && child.parentIdentity != fixture.spec.identity,
            "resident child borrowed retired or unrelated parent owner");
        const auto& cap = CapabilityFor(current, scene->specs[child.parentRequestIndex].model.modelPath);
        const auto owner = std::find_if(replaced.frame->poses.begin(), replaced.frame->poses.end(), [&](const auto& link) {
            return link.requestIndex == child.parentRequestIndex && link.identity == child.parentIdentity;
        });
        Check(owner != replaced.frame->poses.end() && owner->asset == cap.parentAsset &&
            child.emitter->Binding().parentAsset == cap.parentAsset && child.emitter->Binding().parentSource == cap.parentSource &&
            child.emitter->Binding().childSource == cap.childSource, "multi-parent emission lost actual owner/source binding");
    }
    Check(Good(QueryAnimationParticleCoverage(*scene, *replaced.frame)).preparedPrem == 8 &&
        !Good(QueryAnimationParticleCoverage(*scene, *replaced.frame)).notShown &&
        original->states.size() == 1 && original->states[0].identity == fixture.spec.identity &&
        originalPrem.supportedEmitterInstances == 1 && originalScene->particleCapabilities[0].rejectionCode.empty(),
        "new resident acceptance mutated old immutable frame or coverage");
    const auto formerlyPreparedAsset = CapabilityFor(current, additions[7].model.modelPath).parentAsset;
    AnimationInstanceDelta restore; restore.session = {9, 29, "original-prem-host-sources"};
    auto restored = fixture.spec; restored.identity = {200, 9}; restored.hostKey = "original-resident:restored";
    restore.additions.push_back(restored);
    const auto restorePlan = Good(replaced.service->PrepareInstanceCatalog(restore, read));
    const auto over = Good(replaced.service->PrepareInstanceDelta(*replaced.frame, *restorePlan)); const auto overScene = over.service->Scene();
    const auto& crowded = Prem(*overScene, *over.frame); const auto request = RequestFor(*overScene, additions[7].identity);
    const auto& rejected = CapabilityFor(crowded, additions[7].model.modelPath);
    Check(crowded.supportedEmitterInstances == 8 && crowded.unsupportedEmitterInstances == 1 &&
        rejected.parentAsset == formerlyPreparedAsset && !rejected.prepared && rejected.rejectionCode == "PREM_CHILD_ASSET_LIMIT" &&
        CapabilityFor(crowded, kParentPath).parentAsset == oldAsset && CapabilityFor(crowded, kParentPath).prepared &&
        !overScene->particleCapabilities[request].premPreparedCount &&
        overScene->particleCapabilities[request].rejectionCode == "PREM_CHILD_ASSET_LIMIT",
        "formerly successful source failed re-admission but kept an empty/obsolete reason or lost stable source identity");
    const auto pose = std::find_if(over.frame->poses.begin(), over.frame->poses.end(), [&](const auto& link) { return link.requestIndex == request; });
    Check(pose != over.frame->poses.end() && pose->identity == additions[7].identity && pose->animated && !pose->hidden,
        "local ninth-child rejection hid the concrete accepted parent");
    const auto query = Good(AnimationDiagnosticQuery::Create(overScene)); Check(query->SetFrame(over.frame).Succeeded(), "resident Query frame rejected");
    const auto page = Good(query->Query(Good(query->Binding(request, additions[7].identity))));
    Check(page.premDeclaredCount == 1 && !page.premPreparedCount && page.hostKey == additions[7].hostKey &&
        std::any_of(page.issues.begin(), page.issues.end(), [](const auto& issue) { return issue.code == "PREM_CHILD_ASSET_LIMIT"; }),
        "recomputed rejection was absent or assigned to a different Inspector owner");
    const auto coverage = Good(QueryAnimationParticleCoverage(*overScene, *over.frame));
    Check(coverage.declared == 9 && coverage.preparedPrem == 8 && coverage.unsupportedPrem == 1 && coverage.notShown == 1 &&
        scene->particleCapabilities[RequestFor(*scene, additions[7].identity)].rejectionCode.empty() &&
        CapabilityFor(current, additions[7].model.modelPath).prepared, "re-admission lost exact coverage or changed previous accepted capability");
    fixture.Unchanged();
    std::cout << "resident_retired_source_not_active=1 current_assets8_slots256=1 source_identity_stable=1 rebuilt_reason_current=1 query_owner_exact=1 prior_frame_immutable=1\n";
}

void Budgets() {
    Fixture fixture(1, 200); fixture.spec.elapsedMs = 500; fixture.spec.globalMs = 1500;
    const auto service = Good(fixture.Create(Limits())); const auto scene = service->Scene(); const auto frame = Good(service->Prepare(nullptr));
    const auto& prem = Prem(*scene, *frame);
    Check(prem.emission.children.size() == 60 && prem.childSlots.size() == 32 && prem.slotSuppressedChildren == 28 &&
        prem.emission.anchorVisits <= 512 && prem.emission.evaluatedBirths <= 512 && prem.emission.visibilityKeyVisits <= 65536 &&
        prem.storageBytes + prem.workingBytes <= Limits().maxFrameBytes, "fixed live/slot/work budget was silently enlarged or truncated without count");
    CheckSlots(*scene, *frame);
    auto narrow = Limits(); narrow.maxFrameBytes = 65536; narrow.maxActivePoseBytes = 65536;
    const auto boundedService = Good(fixture.Create(narrow)); const auto bounded = Good(boundedService->Prepare(nullptr));
    const auto& refused = Prem(*boundedService->Scene(), *bounded);
    Check(bounded->poses[0].animated && !bounded->poses[0].hidden && refused.unsupportedEmitterInstances == 1 &&
        !refused.supportedEmitterInstances && refused.emission.children.empty() && !refused.childFrame &&
        !refused.capabilities->at(0).rejectionCode.empty() && refused.storageBytes + refused.workingBytes <= narrow.maxFrameBytes,
        "child frame budget dragged parent failure, lost local rejection or exceeded original limit");
    const auto checkBill = [](const AnimationPremFrame& packet, const AnimationLimits& limits, bool accepted) {
        const auto& bill = packet.budget;
        Check(bill.parentAssetBytesBefore && bill.parentAssetBytesAfter >= bill.parentAssetBytesBefore && bill.parentFixedFrameBytes &&
            bill.parentAssetBytesAfter <= limits.maxAssetBytes && packet.assetBytes <= limits.maxAssetBytes - bill.parentAssetBytesAfter &&
            bill.parentFixedFrameBytes <= limits.maxFrameBytes && packet.storageBytes <= limits.maxFrameBytes - bill.parentFixedFrameBytes &&
            packet.workingBytes <= limits.maxFrameBytes - bill.parentFixedFrameBytes - packet.storageBytes,
            "PREM bill exceeded original combined parent/child budget");
        Check(packet.assetBytes == bill.childSourceBytes + bill.childResourceBytes + bill.childSceneBytes &&
            bill.childFrameChargedBytes == std::max(bill.childStandFrameBytes, bill.childFrameLayoutBytes),
            "PREM bill double charged source/layout/workspace or missed actual child capacity");
        if (accepted) {
            // 只记录旧布局门的实际输入；不改变准入、源字节、独立公式或预算上限。
            std::cout << "prem_bill_diag childFrame=" << bool(packet.childFrame)
                << " childScene=" << bool(packet.childScene) << '\n';
            if (packet.childFrame && packet.childScene) {
                const auto& diagnosticFrame = *packet.childFrame;
                const auto& diagnosticScene = *packet.childScene;
                const auto diagnosticBasePayload = std::uint64_t(diagnosticFrame.instances.size()) *
                    (sizeof(War3RenderSkinnedModelInstanceV2) + sizeof(AnimationPoseLink) + sizeof(AnimationPickBounds)) +
                    std::uint64_t(diagnosticFrame.states.size()) * sizeof(AnimationInstanceState) +
                    std::uint64_t(diagnosticFrame.groups.size()) * sizeof(War3RenderGroupMatrixV2) +
                    std::uint64_t(diagnosticFrame.alphas.size()) * (sizeof(float) + sizeof(War3RenderMeshColorV2));
                const auto diagnosticMaterialPayload = std::uint64_t(diagnosticFrame.layerAlphas.size()) * sizeof(float) +
                    std::uint64_t(diagnosticFrame.layerSlices.size()) * sizeof(War3RenderMaterialLayerSliceV2) +
                    std::uint64_t(diagnosticFrame.materialStates.size()) * sizeof(War3RenderMaterialStateV2) +
                    std::uint64_t(diagnosticFrame.materialStateSlices.size()) * sizeof(War3RenderMaterialLayerSliceV2);
                const auto diagnosticOwnerBytes = 2ull * sizeof(decltype(diagnosticFrame.particleMaterialFlags)) +
                    sizeof(std::shared_ptr<const std::vector<std::uint32_t>>);
                const auto diagnosticExpectedLayout = diagnosticBasePayload + diagnosticMaterialPayload + diagnosticOwnerBytes;
                std::cout << "prem_bill_diag shared=" << diagnosticFrame.sharedPoseRanges
                    << " sceneAlpha=" << diagnosticScene.dynamicMaterialAlphas << " sceneState=" << diagnosticScene.dynamicMaterialStates
                    << " frameAlpha=" << diagnosticFrame.dynamicMaterialAlphas << " frameState=" << diagnosticFrame.dynamicMaterialStates
                    << " geosetIssues=" << diagnosticFrame.geosetIssues.size() << " materialIssues=" << diagnosticFrame.materialIssues.size()
                    << " collisions=" << diagnosticFrame.collisionMatrices.size() << " models=" << diagnosticScene.models.size()
                    << " particleFlags=" << diagnosticFrame.particleMaterialFlags.size() << '\n';
                std::cout << "prem_bill_diag layerAlphas=" << diagnosticFrame.layerAlphas.size() << '/' << diagnosticFrame.layerAlphas.capacity()
                    << " layerSlices=" << diagnosticFrame.layerSlices.size() << '/' << diagnosticFrame.layerSlices.capacity()
                    << " materialStates=" << diagnosticFrame.materialStates.size() << '/' << diagnosticFrame.materialStates.capacity()
                    << " materialStateSlices=" << diagnosticFrame.materialStateSlices.size() << '/' << diagnosticFrame.materialStateSlices.capacity() << '\n';
                std::cout << "prem_bill_diag layout=" << bill.childFrameLayoutBytes << " stand=" << bill.childStandFrameBytes
                    << " charged=" << bill.childFrameChargedBytes << " childEvaluator=" << bill.childEvaluatorBytes
                    << " birthEvaluator=" << bill.birthEvaluatorBytes << " basePayload=" << diagnosticBasePayload
                    << " materialPayload=" << diagnosticMaterialPayload << " owners=" << diagnosticOwnerBytes
                    << " expectedLayout=" << diagnosticExpectedLayout << " layoutEqualsExpected=" << (bill.childFrameLayoutBytes == diagnosticExpectedLayout) << '\n';
                std::cout << "prem_bill_diag childSource=" << bill.childSourceBytes << " childResource=" << bill.childResourceBytes
                    << " childScene=" << bill.childSceneBytes << " requestGeometry=" << diagnosticScene.requestGeometryRequired.size()
                    << '/' << diagnosticScene.requestGeometryRequired.capacity()
                    << " sourceTableBytes=" << std::uint64_t(diagnosticScene.requestGeometryRequired.capacity()) * sizeof(std::uint8_t)
                    << " storage=" << packet.storageBytes << " working=" << packet.workingBytes << " asset=" << packet.assetBytes << '\n';
            }
            Check(packet.childFrame && packet.childScene && !packet.childFrame->sharedPoseRanges &&
                !packet.childScene->dynamicMaterialAlphas && !packet.childScene->dynamicMaterialStates &&
                packet.childFrame->geosetIssues.empty() && packet.childFrame->materialIssues.empty() &&
                packet.childFrame->collisionMatrices.empty(), "PREM billing fixture acquired unrelated child layout charges");
            const auto& child=*packet.childFrame;
            Check(packet.childScene->models.size()==1 && child.particleMaterialFlags.empty(),
                "PREM billing fixture lost its single child asset or acquired particle material payload");
            const auto actualFramePayload=std::uint64_t(child.instances.size())*(sizeof(War3RenderSkinnedModelInstanceV2)+
                sizeof(AnimationPoseLink)+sizeof(AnimationPickBounds))+std::uint64_t(child.states.size())*sizeof(AnimationInstanceState)+
                std::uint64_t(child.groups.size())*sizeof(War3RenderGroupMatrixV2)+
                std::uint64_t(child.alphas.size())*(sizeof(float)+sizeof(War3RenderMeshColorV2));
            // 本夹具全新子服务仅push一次唯一资产且不reserve，MSVC资产容量为1。
            // 唯一子资产仍持有一个材质表shared_ptr槽，前/当前帧各保留空vector本体。
            // 独立按类型计算新增owner，不复用生产计费helper；场景来源表仍不得重复收费。
            const auto materialOwnerBytes=2ull*sizeof(decltype(child.particleMaterialFlags))+
                sizeof(std::shared_ptr<const std::vector<std::uint32_t>>);
            Check(bill.childFrameLayoutBytes>=actualFramePayload &&
                bill.childFrameLayoutBytes-actualFramePayload==materialOwnerBytes &&
                packet.childScene->requestGeometryRequired.capacity(),
                "PREM frame layout lost material owners or charged the scene provenance table again");
            Check(bill.childSourceBytes && bill.childResourceBytes && bill.childSceneBytes && bill.childStandFrameBytes &&
                bill.childFrameLayoutBytes && bill.childEvaluatorBytes && bill.birthEvaluatorBytes && bill.preparationResourceBytes &&
                bill.childSceneBytes <= packet.storageBytes && bill.childFrameChargedBytes <= packet.storageBytes - bill.childSceneBytes &&
                bill.childEvaluatorBytes <= packet.storageBytes - bill.childSceneBytes - bill.childFrameChargedBytes &&
                bill.birthEvaluatorBytes <= packet.storageBytes - bill.childSceneBytes - bill.childFrameChargedBytes - bill.childEvaluatorBytes,
                "PREM positive bill lost actual retained scene/frame or unique evaluator fees");
        }
        else Check(!packet.assetBytes && !bill.childSourceBytes && !bill.childResourceBytes && !bill.childSceneBytes &&
            !bill.childStandFrameBytes && !bill.childFrameLayoutBytes && !bill.childFrameChargedBytes &&
            !bill.childEvaluatorBytes && !bill.birthEvaluatorBytes, "refused PREM retained phantom child/evaluator charges");
    };
    checkBill(prem, Limits(), true); checkBill(refused, narrow, false);
    // 合法的大外层帧预算不能把出生采样器的预算参数抬过核心硬帽；实际工作量仍与64MiB一致。
    auto largeLimits = Limits(); largeLimits.maxFrameBytes = 256ull * 1024 * 1024;
    const auto largeService = Good(fixture.Create(largeLimits)); const auto largeScene = largeService->Scene();
    const auto largeFrame = Good(largeService->Prepare(nullptr)); const auto& largePrem = Prem(*largeScene, *largeFrame);
    checkBill(largePrem, largeLimits, true);
    Check(largeFrame->poses.size() == 1 && largeFrame->poses[0].animated && !largeFrame->poses[0].hidden &&
        largePrem.capabilities->size() == 1 && largePrem.capabilities->at(0).prepared &&
        largePrem.capabilities->at(0).rejectionCode.empty() && largePrem.supportedEmitterInstances == 1 &&
        !largePrem.unsupportedEmitterInstances && largePrem.childScene == largeScene->premGeometry && largePrem.childFrame &&
        largePrem.emission.children.size() == 60 && largePrem.childSlots.size() == 32 && largePrem.slotSuppressedChildren == 28 &&
        largePrem.emission.evaluatedBirths && largePrem.emission.evaluatedBirths == prem.emission.evaluatedBirths &&
        largePrem.sampleErrors.empty(), "legal 256MiB outer budget rejected actual child/birth sampling or enlarged the slot cap");
    CheckSlots(*largeScene, *largeFrame);
    Check(largePrem.storageBytes == prem.storageBytes && largePrem.workingBytes == prem.workingBytes &&
        largePrem.assetBytes == prem.assetBytes && largePrem.budget.parentFixedFrameBytes == prem.budget.parentFixedFrameBytes &&
        largePrem.budget.childFrameChargedBytes == prem.budget.childFrameChargedBytes &&
        largePrem.budget.childEvaluatorBytes == prem.budget.childEvaluatorBytes &&
        largePrem.budget.birthEvaluatorBytes == prem.budget.birthEvaluatorBytes,
        "larger outer budget inflated actual child/frame/evaluator StorageBytes for unchanged workload");
    std::cout << "large_outer_frame_bytes=" << largeLimits.maxFrameBytes << " prepared=1 births60_slots32_omitted28=1"
        << " storage=" << largePrem.storageBytes << " working=" << largePrem.workingBytes
        << " childEvaluator=" << largePrem.budget.childEvaluatorBytes << " birthEvaluator=" << largePrem.budget.birthEvaluatorBytes
        << " standard_storage_equal=1\n";
    // 费用来自本次正式服务；二分只选择检验边界，最终前后1字节均重新完整准入。
    std::uint32_t probes = 0;
    const auto probe = [&](std::uint64_t bytes, bool requireAccepted, bool verifyExpectation) {
        ++probes; auto limits = Limits(); limits.maxFrameBytes = bytes;
        limits.maxActivePoseBytes = std::min(limits.maxActivePoseBytes, bytes);
        const auto candidate = Good(fixture.Create(limits)); const auto result = Good(candidate->Prepare(nullptr));
        const auto& packet = Prem(*candidate->Scene(), *result);
        Check(result->poses.size() == 1 && result->poses[0].animated && !result->poses[0].hidden &&
            packet.capabilities->size() == 1 && packet.capabilities->at(0).parentSource.contentDigest ==
                fixture.resources.at(std::string(kParentPath)).digest,
            "budget probe changed source owner or failed the parent rather than rejecting only PREM");
        const bool accepted = packet.capabilities->at(0).prepared;
        checkBill(packet, limits, accepted);
        if (verifyExpectation) Check(accepted == requireAccepted, "exact one-byte PREM admission boundary was inconsistent");
        if (accepted) Check(packet.supportedEmitterInstances == 1 && !packet.unsupportedEmitterInstances &&
            packet.childScene && packet.childFrame && packet.childSlots.size() == 32 && packet.slotSuppressedChildren == 28,
            "budget admission silently omitted live child slots or changed workload");
        else Check(!packet.supportedEmitterInstances && packet.unsupportedEmitterInstances == 1 && !packet.childFrame &&
            !packet.childScene && packet.emission.children.empty() && !packet.capabilities->at(0).rejectionCode.empty(),
            "budget rejection did not stay local and truthful");
        if (verifyExpectation) std::cout << "budget_probe bytes=" << bytes << " prepared=" << accepted
            << " parentFixed=" << packet.budget.parentFixedFrameBytes << " storage=" << packet.storageBytes
            << " working=" << packet.workingBytes << " childFrameCharged=" << packet.budget.childFrameChargedBytes
            << " childEvaluator=" << packet.budget.childEvaluatorBytes << " birthEvaluator=" << packet.budget.birthEvaluatorBytes
            << " rejection=" << packet.capabilities->at(0).rejectionCode << '\n';
        return accepted;
    };
    std::uint64_t low = narrow.maxFrameBytes, high = Limits().maxFrameBytes;
    Check(!probe(low, false, true) && probe(high, true, true), "budget boundary brackets were not a real local refusal and acceptance");
    while (high - low > 1) {
        const auto middle = low + (high - low) / 2;
        if (probe(middle, false, false)) high = middle; else low = middle;
    }
    Check(probes <= 28 && high == low + 1, "budget boundary search exceeded its bounded work or lost one-byte precision");
    Check(!probe(high - 1, false, true) && probe(high, true, true) && probe(high + 1, true, true),
        "exact budget boundary or its immediate neighbours did not preserve admission");

    // 资产边界独立改变maxAssetBytes；原帧额度和同一原创parent/child/Stand来源均冻结。
    const auto parentSha = fixture.resources.at(std::string(kParentPath)).digest;
    const auto childSha = fixture.resources.at(std::string(kChildPath)).digest;
    const auto childRawBytes = static_cast<std::uint64_t>(fixture.resources.at(std::string(kChildPath)).bytes.size());
    auto assetLimits = Limits(); assetLimits.maxFrameBytes = 64ull * 1024 * 1024;
    Check(assetLimits.maxFrameBytes == Limits().maxFrameBytes && assetLimits.maxActivePoseBytes == Limits().maxActivePoseBytes,
        "asset-only search changed the original frame or active pose budget");
    const auto assetBudgetCode = [](std::string_view code) {
        return code == "FIXTURE_RESOURCE_LIMIT" || code == "PREM_CHILD_ASSET_BUDGET" || code == "PREM_HOST_BUDGET" ||
            code == "ANIMATION_ASSET_BUDGET" || code == "ANIMATION_ASSET_SOURCE" || code == "ANIMATION_RESOURCE_BUDGET" ||
            code == "ANIMATION_TEXTURE_BUDGET" || code == "ANIMATION_PICK_METADATA_BUDGET" ||
            code == "WORLD_MODEL_BUDGET" || code == "WORLD_MODEL_TEXTURE_BUDGET" || code == "WORLD_BAKED_BUDGET" ||
            code == "RENDER_CORE_CAPACITY_EXCEEDED";
    };
    const auto calibrationFailure = [&](const auto& diagnostics, std::uint64_t bytes) {
        bool hasError = false;
        for (const auto& diagnostic : diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error) {
            hasError = true; Check(assetBudgetCode(diagnostic.code), "parent floor calibration concealed a non-budget failure");
            std::cout << "asset_parent_calibration bytes=" << bytes << " error=" << diagnostic.code << '\n';
        }
        Check(hasError, "parent floor calibration had no actual budget error"); fixture.Unchanged();
    };
    std::uint32_t assetProbes = 0, calibrationProbes = 0;
    const auto assetProbe = [&](std::uint64_t bytes, std::optional<bool> expected, bool calibrateParent = false) -> std::optional<bool> {
        ++assetProbes; auto limits = assetLimits; limits.maxAssetBytes = bytes;
        auto created = fixture.Create(limits);
        if (!created.Succeeded()) {
            Check(calibrateParent, "asset probe failed the parent instead of rejecting only its child");
            calibrationFailure(created.diagnostics, bytes); return std::nullopt;
        }
        const auto candidate = Good(std::move(created)); const auto candidateScene = candidate->Scene();
        auto prepared = candidate->Prepare(nullptr);
        if (!prepared.Succeeded()) {
            Check(calibrateParent, "asset probe failed parent sampling instead of rejecting only its child");
            calibrationFailure(prepared.diagnostics, bytes); return std::nullopt;
        }
        const auto result = Good(std::move(prepared)); const auto& packet = Prem(*candidateScene, *result);
        Check(result->poses.size() == 1 && result->poses[0].animated && !result->poses[0].hidden && result->poses[0].poseVersion &&
            packet.capabilities->size() == 1 && packet.capabilities->at(0).parentSource.contentDigest == parentSha &&
            packet.budget.parentAssetBytesBefore == prem.budget.parentAssetBytesBefore &&
            packet.budget.parentFixedFrameBytes == prem.budget.parentFixedFrameBytes,
            "asset probe changed the actual parent source, pose or fixed parent charges");
        const auto& capability = packet.capabilities->at(0); const bool accepted = capability.prepared;
        checkBill(packet, limits, accepted);
        if (expected) Check(accepted == *expected, "exact one-byte asset admission boundary was inconsistent");
        if (accepted) {
            Check(packet.supportedEmitterInstances == 1 && !packet.unsupportedEmitterInstances && packet.childScene && packet.childFrame &&
                packet.emission.children.size() == 60 && packet.childSlots.size() == 32 && packet.slotSuppressedChildren == 28 &&
                packet.sampleErrors.empty() && capability.childSource.contentDigest == childSha && capability.emitter &&
                capability.emitter->Binding().parentSource == capability.parentSource &&
                capability.emitter->Binding().childSource == capability.childSource &&
                std::any_of(packet.childScene->sources.begin(), packet.childScene->sources.end(),
                    [&](const auto& source) { return source == capability.childSource; }) &&
                packet.budget.parentAssetBytesAfter > packet.budget.parentAssetBytesBefore &&
                packet.budget.parentAssetBytesAfter - packet.budget.parentAssetBytesBefore >= childRawBytes &&
                packet.storageBytes == prem.storageBytes && packet.workingBytes == prem.workingBytes,
                "asset admission lost real cache growth, immutable child Stand source or unchanged workload");
            if (expected) CheckSlots(*candidateScene, *result);
        } else Check(!packet.supportedEmitterInstances && packet.unsupportedEmitterInstances == 1 && !packet.childFrame &&
            !packet.childScene && !candidateScene->premGeometry && packet.childSlots.empty() && packet.emission.children.empty() &&
            assetBudgetCode(capability.rejectionCode), "asset refusal was non-budget, lost its owner or retained child geometry/slots");
        if (bytes == packet.budget.parentAssetBytesBefore) Check(!accepted &&
            packet.budget.parentAssetBytesAfter == packet.budget.parentAssetBytesBefore,
            "zero-remaining asset bracket read new cache bytes or admitted a child");
        fixture.Unchanged();
        if (expected) std::cout << "asset_probe bytes=" << bytes << " prepared=" << accepted
            << " parentBefore=" << packet.budget.parentAssetBytesBefore << " parentAfter=" << packet.budget.parentAssetBytesAfter
            << " cacheGrowth=" << packet.budget.parentAssetBytesAfter - packet.budget.parentAssetBytesBefore
            << " childSource=" << packet.budget.childSourceBytes << " childResource=" << packet.budget.childResourceBytes
            << " childScene=" << packet.budget.childSceneBytes << " childAsset=" << packet.assetBytes
            << " finalRemaining=" << bytes - packet.budget.parentAssetBytesAfter << " rejection=" << capability.rejectionCode << '\n';
        return accepted;
    };
    std::uint64_t assetLow = prem.budget.parentAssetBytesBefore, assetHigh = assetLimits.maxAssetBytes;
    Check(assetLow && assetLow < assetHigh, "actual parent asset bill did not provide a finite asset search range");
    const auto initialLow = assetProbe(assetLow, false, true);
    if (!initialLow) {
        // Before只含常驻源/缓存，不保证独立compile/pick门。最多三次父-only校准，不将失败当作child拒绝。
        bool calibrated = false;
        for (std::uint32_t attempt = 0; attempt < 3 && !calibrated; ++attempt) {
            Check(assetLow < assetHigh / 2, "parent floor calibration exhausted the original asset budget bracket");
            assetLow *= 2; ++calibrationProbes; auto limits = assetLimits;
            limits.enablePremPreview = false; limits.maxAssetBytes = assetLow;
            auto created = fixture.Create(limits);
            if (!created.Succeeded()) { calibrationFailure(created.diagnostics, assetLow); continue; }
            const auto parent = Good(std::move(created)); auto sampled = parent->Prepare(nullptr);
            if (!sampled.Succeeded()) { calibrationFailure(sampled.diagnostics, assetLow); continue; }
            const auto parentFrame = Good(std::move(sampled));
            Check(parentFrame->poses.size() == 1 && parentFrame->poses[0].animated && !parentFrame->poses[0].hidden &&
                parentFrame->poses[0].poseVersion && !parentFrame->prem &&
                std::any_of(parent->Scene()->sources.begin(), parent->Scene()->sources.end(),
                    [&](const auto& source) { return source.contentDigest == parentSha; }),
                "parent-only floor calibration did not retain the same actual animated source");
            const auto local = assetProbe(assetLow, false); Check(local && !*local, "calibrated parent floor had no local child refusal");
            calibrated = true; fixture.Unchanged();
        }
        Check(calibrated, "no valid local asset-refusal bracket within bounded parent floor calibration");
    }
    Check(assetProbe(assetHigh, true).value_or(false), "asset upper bracket was not a real acceptance");
    while (assetHigh - assetLow > 1) {
        const auto middle = assetLow + (assetHigh - assetLow) / 2;
        const auto accepted = assetProbe(middle, std::nullopt); Check(accepted.has_value(), "asset bisection lost its successful parent");
        if (*accepted) assetHigh = middle; else assetLow = middle;
    }
    Check(assetProbes <= 31 && assetHigh == assetLow + 1 && assetHigh < assetLimits.maxAssetBytes,
        "asset boundary search exceeded bounded work or lost exact original-budget neighbours");
    const auto below = assetProbe(assetHigh - 1, false), exact = assetProbe(assetHigh, true), above = assetProbe(assetHigh + 1, true);
    Check(below && !*below && exact && *exact && above && *above && assetProbes <= 34 && calibrationProbes <= 3,
        "asset boundary neighbours did not preserve local refusal, real cache growth or bounded work");
    fixture.Unchanged();
    std::cout << "births60_slots32_omitted28=1 key_anchor_work_caps=1 narrow_budget_parent_retained=1 unique_budget_bill=1 exact_boundary="
        << high << " probes=" << probes << " exact_asset_boundary=" << assetHigh << " asset_probes=" << assetProbes
        << " parent_floor_probes=" << calibrationProbes << " asset_cache_growth_real=1 combined_asset_bill=1 immutable_source_sha=1\n";
}

std::filesystem::path Environment(const char* narrowName, const wchar_t* wideName) {
#ifdef _WIN32
    (void)narrowName; const auto size = GetEnvironmentVariableW(wideName, nullptr, 0); if (!size) return {};
    std::wstring value(size, L'\0'); const auto count = GetEnvironmentVariableW(wideName, value.data(), size);
    Check(count && count < size, "real model environment changed during capture"); value.resize(count); return value;
#else
    (void)wideName; const auto* value = std::getenv(narrowName); return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}
std::vector<std::byte> ReadBound(const std::filesystem::path& path, std::uint64_t size, std::string_view sha) {
    Check(std::filesystem::file_size(path) == size && Good(Sha256File(path)) == sha, "real PREM model SHA/size changed");
    std::ifstream file(path, std::ios::binary); Check(bool(file), "real PREM model could not be opened readonly");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size)); file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(file.gcount() == static_cast<std::streamsize>(bytes.size()) && Good(Sha256(bytes)) == sha, "real model changed during readonly capture"); return bytes;
}
int Real() {
    const auto parentPath = Environment("FORGE_PREM_PARENT", L"FORGE_PREM_PARENT"), childPath = Environment("FORGE_PREM_CHILD", L"FORGE_PREM_CHILD");
    if (parentPath.empty() || childPath.empty()) { std::cout << "SKIP FORGE_PREM_PARENT / FORGE_PREM_CHILD not set\n"; return 77; }
    constexpr std::string_view parentSha = "95ccef98364b0efe3419945d994063388c1d77f8f4e9519d7a7cb1c8e66f9f0d";
    constexpr std::string_view childSha = "e8c96af76655604a5c8b2cb00ea84c757dbf30a1037b22954264d5c8150a9b9e";
    Fixture fixture; fixture.Add(std::string(kParentPath), ReadBound(parentPath, 69822, parentSha), "real-local-readonly-voodoo");
    fixture.resources.erase(std::string(kChildPath));
    fixture.Add("SharedModels\\BuildingParticlebeam.mdx", ReadBound(childPath, 2292, childSha), "real-local-readonly-beam");
    fixture.spec.model.scale = {1, 1, 1}; fixture.spec.model.angle = 0;
    fixture.spec.sequenceIndex = 2; fixture.spec.elapsedMs = 32; fixture.spec.globalMs = 1032; fixture.spec.playing = true;
    const auto model = Good(ParseMdx(fixture.resources.at(std::string(kParentPath)).bytes));
    Check(model.premEmitters.size() == 1 && model.premEmitters[0].nodeId == 48 && model.premEmitters[0].recordByteOffset == 66651 &&
        model.premEmitters[0].recordByteSize == 456, "real typed PREM/source identity changed");
    const auto service = Good(fixture.Create(Limits())); const auto scene = service->Scene(); auto frame = Good(service->Prepare(nullptr));
    Check(Prem(*scene, *frame).supportedEmitterInstances == 1 && frame->prem->emission.children.empty(), "real Death32 emitted before KPEV window");
    const auto& capability = frame->prem->capabilities->at(0);
    Check(capability.parentSource.contentDigest == parentSha && capability.childSource.contentDigest == childSha &&
        capability.originalPath == "SharedModels\\BuildingParticlebeam.MDL" && capability.resolvedPath == "SharedModels\\BuildingParticlebeam.mdx" &&
        capability.emitter && capability.emitter->Binding().parentSource == capability.parentSource &&
        capability.emitter->Binding().childSource == capability.childSource,
        "real host lost actual parent/child SHA/source or explicit missing-only path alias");
    AnimationAdvance seek; AnimationEdit edit; edit.identity = fixture.spec.identity; edit.elapsedMs = 80; edit.globalMs = 1080; seek.edits.push_back(edit);
    frame = Good(service->Prepare(frame.get(), seek)); Check(frame->prem->emission.children.size() == 1, "real Death80 first birth missing");
    const auto& birth = frame->prem->emission.children[0];
    Check(birth.birthElapsedMs == 80 && birth.birthGlobalMs == 1080 && birth.ageMs == 0 &&
        Near(birth.transform[3], 123 + model.pivots[48][0]) && Near(birth.transform[7], 456 + model.pivots[48][1]) && Near(birth.transform[11], 789),
        "real actual-node pivot/parent once transform failed");
    CheckSlots(*scene, *frame);
    seek.edits[0].elapsedMs = 150; seek.edits[0].globalMs = 1150;
    frame = Good(service->Prepare(frame.get(), seek)); Check(frame->prem->emission.children.size() == 2 &&
        frame->prem->emission.children[0].birthElapsedMs == 80 && frame->prem->emission.children[1].birthElapsedMs == 128 &&
        frame->prem->sampleErrors.empty(), "real Death150 lost actual core samples/source child transforms"); CheckSlots(*scene, *frame);
    AnimationAdvance tick; tick.deltaMs = 1000;
    frame = Good(service->Prepare(frame.get(), tick)); Check(frame->states[0].elapsedMs == 633 && !frame->states[0].playing &&
        Clock(*frame->prem).elapsedMs == 1150 && !frame->prem->emission.children.empty(), "real short Death froze PREM lifespan");
    frame = Good(service->Prepare(frame.get(), tick)); Check(Clock(*frame->prem).elapsedMs == 2150 &&
        frame->prem->emission.children.empty(), "real child life continued forever after parent clamp");
    Check(Good(Sha256File(parentPath)) == parentSha && Good(Sha256File(childPath)) == childSha, "real input file modified"); fixture.Unchanged();
    std::cout << "real_parent_child_sha_bound=1 actual_service_birth_node48=1 Death32_80_150=1 original_blp_only_not_gpu=1 real_tail_lifespan=1 readonly_inputs=1\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected lifecycle, sources, budgets, coverage, resident or real group"); const std::string_view group = argv[1];
        if (group == "lifecycle") Lifecycle(); else if (group == "sources") Sources(); else if (group == "budgets") Budgets();
        else if (group == "coverage") Coverage(); else if (group == "resident") Resident();
        else if (group == "real") return Real(); else throw std::runtime_error("unknown PREM host group"); return 0;
    } catch (const std::exception& error) { std::cerr << "AnimationPremHostTests: " << error.what() << '\n'; return 1; }
}
