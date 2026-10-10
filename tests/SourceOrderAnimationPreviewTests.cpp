#include "War3MapCore/Render/ModelAnimation.h"
#include "AnimationParticleContinuityTests.h"
#include "../src/Render/Animation/AnimationPoseWarnings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
using War3RenderCore::Code;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T> T Good(Result<T> result) {
    if (!result.Succeeded()) {
        throw std::runtime_error(result.diagnostics.empty() ? "service returned an empty failure" :
            result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    }
    return std::move(*result.value);
}

bool Near(float actual, float expected) {
    return std::isfinite(actual) && std::abs(actual - expected) <= 0.0002f;
}

std::uint32_t ReadWord(std::span<const std::byte> bytes, std::size_t at) {
    Check(at <= bytes.size() && bytes.size() - at >= 4, "original fixture word was truncated");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(bytes[at + i]) << (i * 8);
    }
    return value;
}

std::vector<std::byte> DescendingModel(std::uint32_t extraHelpers = 0, bool invalidRotation = false,
    bool dependentLastHelper = false) {
    // 只替换现有原创夹具的 HELP 块；骨骼、网格和 PRE2 发射器保持真实父链。
    // 不读取原图，不排序/删除原始轨迹；该测试资源不是 Warcraft 游戏资产。
    auto bytes = AnimationParticleContinuityTest::Model();
    MdxTest::Writer helper;
    helper.Tag("HELP");
    const auto chunk = helper.Begin();
    constexpr std::array<std::uint32_t, 7> frames{0, 400, 600, 400, 1000, 2000, 3000};
    constexpr std::array<float, 7> values{0, 4, 6, 40, 10, 20, 60};
    Check(extraHelpers <= 8, "original extra-helper fixture exceeded its explicit bound");
    Check((!invalidRotation && !dependentLastHelper) || extraHelpers > 0,
        "dependent helper needs an original second helper");
    for (std::uint32_t h = 0; h <= extraHelpers; ++h) {
        const auto node = helper.Begin();
        helper.Fixed("Source-order motion", 80);
        helper.U32(h == 0 ? 1 : h + 2); // 原 PRE2 的 objectId 2 保留。
        // 错误反例与成功对照都依赖节点1；内部先完成其平移，再采样最后节点。
        // 批量求值失败时只发布真正Error，不能把内部已产生的warning视为失败回执。
        // 普通和拥挤警告夹具的父链保持原样。
        helper.U32((invalidRotation || dependentLastHelper) && h == extraHelpers ? 1u : UINT32_MAX);
        helper.U32(0);
        helper.Tag("KGTR");
        helper.U32(7);
        helper.U32(1);
        helper.U32(UINT32_MAX);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            helper.U32(frames[i]);
            helper.F32(values[i]);
            helper.F32(0);
            helper.F32(0);
        }
        if (invalidRotation && h == extraHelpers) {
            // 二进制中的有限零四元数可解析，只有实际节点采样归一化时才拒绝。
            helper.Tag("KGRT"); helper.U32(2); helper.U32(1); helper.U32(UINT32_MAX);
            for (const auto frame : {0u, 1000u}) {
                helper.U32(frame);
                for (unsigned axis = 0; axis < 4; ++axis) helper.F32(0);
            }
        }
        helper.EndInclusive(node);
    }
    helper.EndChunk(chunk);
    bool replaced = false;
    for (std::size_t at = 4; at <= bytes.size() && bytes.size() - at >= 8;) {
        const auto size = ReadWord(bytes, at + 4);
        Check(size <= bytes.size() - at - 8, "original fixture chunk was truncated");
        const auto end = at + 8 + size;
        if (std::memcmp(bytes.data() + at, "HELP", 4) == 0) {
            bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                bytes.begin() + static_cast<std::ptrdiff_t>(end));
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                helper.bytes.begin(), helper.bytes.end());
            replaced = true;
            break;
        }
        at = end;
    }
    Check(replaced, "original helper fixture was missing");
    if (!extraHelpers) return bytes;
    for (std::size_t at = 4; at <= bytes.size() && bytes.size() - at >= 8;) {
        const auto size = ReadWord(bytes, at + 4);
        Check(size <= bytes.size() - at - 8, "extra-helper fixture chunk was truncated");
        if (std::memcmp(bytes.data() + at, "PIVT", 4) == 0) {
            const auto nextSize = size + extraHelpers * 12;
            for (unsigned i = 0; i < 4; ++i) bytes[at + 4 + i] = static_cast<std::byte>((nextSize >> (i * 8)) & 255u);
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + size), extraHelpers * 12, std::byte{});
            return bytes;
        }
        at += 8 + size;
    }
    throw std::runtime_error("extra-helper pivots were missing");
}

struct Fixture {
    std::vector<std::byte> model;
    const std::vector<std::byte> original;
    std::vector<std::byte> texture = AnimationParticleContinuityTest::Texture();
    AnimationInstanceSpec spec;

    explicit Fixture(std::uint32_t extraHelpers = 0, bool invalidRotation = false, bool dependentLastHelper = false)
        : model(DescendingModel(extraHelpers, invalidRotation, dependentLastHelper)), original(model) {
        spec.identity = {17, 9};
        spec.hostKey = "original-source-order:17";
        spec.model.modelPath = "original-source-order.mdx";
        spec.model.position = {123, 456, 789};
        spec.sequenceIndex = 0; // 共享夹具按时间顺序：Stand=0、Walk=1
        spec.elapsedMs = 400;
        spec.globalMs = 0;
    }

    Result<AnimationResource> Read(std::string_view path, std::uint64_t maximum) const {
        const auto& bytes = path == spec.model.modelPath ? model : texture;
        if (bytes.size() > maximum) {
            return Result<AnimationResource>::Failure("Test", "FIXTURE_RESOURCE_LIMIT",
                "原创夹具超过读取预算。");
        }
        return Result<AnimationResource>::Success({bytes, "original-service-source-order-fixture",
            std::string(64, 'd')});
    }

    auto Create(const AnimationLimits& limits) const {
        return ModelAnimationService::Create({9, 27, "original-service-source-order-fixture"},
            std::span(&spec, 1), [this](auto path, auto maximum) { return Read(path, maximum); }, limits);
    }

    auto Create(const AnimationLimits& limits, std::span<const AnimationInstanceSpec> specs) const {
        return ModelAnimationService::Create({9, 27, "original-service-source-order-fixture"},
            specs, [this](auto path, auto maximum) { return Read(path, maximum); }, limits);
    }

    void Unchanged() const {
        Check(model == original, "service altered source-order fixture bytes");
    }
};

bool HasKeysError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error && diagnostic.code == "MDX_POSE_KEYS_INVALID";
    });
}

void RequireWarning(const AnimationFrame& frame) {
    const auto found = std::find_if(frame.diagnostics.begin(), frame.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == Code::NodeSourceOrderPreview &&
            diagnostic.severity == War3RenderCore::Severity::Warning && diagnostic.objectId == 1 &&
            diagnostic.track == War3RenderCore::TrackKind::Translation &&
            diagnostic.leftFrame == 600 && diagnostic.rightFrame == 400;
    });
    Check(found != frame.diagnostics.end(), "source-order warning lost the original node/track/descending frames");
    Check(War3RenderCore::DescribeDiagnostic(*found).find("1.27a 原版区间与顺序查找") != std::string::npos,
        "preview warning lost its game-rule qualification");
    Check(std::none_of(frame.diagnostics.begin(), frame.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == War3RenderCore::Severity::Error;
    }), "successful preview retained an unhandled error");
}

void RequirePlacement(const AnimationFrame& frame) {
    Check(frame.states.size() == 1 && frame.instances.size() == 1, "original instance identity was lost");
    constexpr std::array<float, 3> position{123, 456, 789};
    for (std::size_t axis = 0; axis < position.size(); ++axis) {
        const auto index = axis * 4 + 3;
        Check(Near(frame.states[0].transform[index], position[axis]) &&
            Near(frame.instances[0].instance.transform[index], position[axis]),
            "row-major placement translation 3/7/11 was transposed or baked into the node pose");
    }
    Check(frame.states[0].identity.value == 17 && frame.states[0].identity.generation == 9,
        "source-order preview changed placement identity");
}

void RequireBody(const AnimationFrame& frame, float translation) {
    RequirePlacement(frame);
    Check(frame.poses.size() == 1 && frame.animatedInstances == 1 && frame.hiddenInstances == 0 &&
        frame.retainedStaticInstances == 0 && frame.poses[0].animated && !frame.poses[0].hidden &&
        frame.poses[0].poseVersion != 0, "source-order opt-in failed to produce a visible computed pose");
    const auto& pose = frame.poses[0];
    Check(pose.groupCount == 2 && pose.firstGroup <= frame.groups.size() &&
        pose.groupCount <= frame.groups.size() - pose.firstGroup, "original two-geoset group ranges were lost");
    for (std::uint32_t i = 0; i < pose.groupCount; ++i) {
        const auto& group = frame.groups[pose.firstGroup + i];
        // 矩阵组为 column-major 3x4；实例/state 为 row-major 3x4，不能混用。
        Check(Near(group.values[9], translation) && Near(group.values[10], 0) && Near(group.values[11], 0),
            "computed group translation disagreed with the independent source-order window");
    }
    RequireWarning(frame);
}

void StrictGate() {
    Check(!AnimationLimits{}.allowSourceOrderNodePreview, "source-order preview was enabled by default");
    for (const auto policy : {AnimationPlaybackPolicy::StrictReplay, AnimationPlaybackPolicy::InteractivePreview}) {
        for (const bool flag : {false, true}) {
            if (policy == AnimationPlaybackPolicy::InteractivePreview && flag) continue;
            for (const bool retain : {false, true}) {
                Fixture fixture;
                fixture.spec.elapsedMs = 500;
                AnimationLimits limits;
                limits.playbackPolicy = policy;
                limits.allowSourceOrderNodePreview = flag;
                limits.retainUnsupportedStatic = retain;
                // 不启用粒子，避免全局时钟回放门禁掩盖本用例的实际节点轨迹错误。
                auto created = fixture.Create(limits);
                if (!created.Succeeded()) {
                    Check(HasKeysError(created.diagnostics), "strict Create failed for a different reason");
                } else {
                    auto frame = (*created.value)->Prepare(nullptr);
                    if (!frame.Succeeded()) {
                        Check(HasKeysError(frame.diagnostics), "strict Prepare failed for a different reason");
                    } else {
                        const auto& frozen = **frame.value;
                        Check(retain && frozen.hiddenInstances == 1 &&
                            frozen.animatedInstances == 0 && frozen.poses.size() == 1,
                            "a closed source-order gate displayed an inverse-key pose");
                        const auto& pose = frozen.poses[0];
                        Check(pose.hidden && !pose.animated && pose.poseVersion == 0 &&
                            pose.rejection.code == Code::PoseKeysInvalid && pose.alphaCount != 0,
                            "retained strict rejection lost its typed node failure");
                        const auto& alphas = frozen.alphas;
                        Check(pose.firstAlpha <= alphas.size() && pose.alphaCount <= alphas.size() - pose.firstAlpha &&
                            std::all_of(alphas.begin() + pose.firstAlpha, alphas.begin() + pose.firstAlpha + pose.alphaCount,
                                [](float alpha) { return alpha == 0; }), "strict hidden model retained visible meshes");
                        RequirePlacement(frozen);
                    }
                }
                fixture.Unchanged();
            }
        }
    }
    std::cout << "default_strict=1 explicit_interactive_required=1 strict_flag_ignored=1 retention_gate=1\n";
}

void Body() {
    Fixture fixture;
    AnimationLimits limits;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    limits.allowSourceOrderNodePreview = true;
    auto service = Good(fixture.Create(limits));
    auto frame = Good(service->Prepare(nullptr));
    RequireBody(*frame, 4); // 1.27a 顺序查找在 400 停在第一个 400 键（4），不跳到逆序后的 400（40）。
    const auto captured = Good(ReadAnimationPoseWarnings(*service->Scene(), *frame, frame->poses[0]));
    Check(captured.records.size() == 1 && !captured.omitted && !captured.cachedStand &&
        captured.sequenceIndex == 0 && captured.elapsedMs == 400 && captured.globalMs == 0,
        "actual computed warning did not retain its owner and sampled clock");
    for (const auto& sample : {std::pair<std::uint64_t, float>{500, 5.0f}, {601, 29.95f}}) {
        AnimationAdvance advance;
        AnimationEdit edit;
        edit.identity = fixture.spec.identity;
        edit.elapsedMs = sample.first;
        advance.edits.push_back(edit);
        frame = Good(service->Prepare(frame.get(), advance));
        RequireBody(*frame, sample.second);
        Check(frame->states[0].elapsedMs == sample.first && frame->states[0].globalMs == 0,
            "source-order preview fabricated a different clock");
        auto repeated = Good(service->Prepare(frame.get()));
        RequireBody(*repeated, sample.second);
        Check(repeated->poses[0].matrixHash == frame->poses[0].matrixHash, "repeat changed a frozen pose");
        frame = std::move(repeated);
    }
    fixture.Unchanged();
    std::cout << "game_order_exact=4 first_source_right=5 post_inverse_segment=29.95 warning_retained=1 rowmajor_3_7_11=1\n";
}

std::vector<AnimationInstanceSpec> Specs(const Fixture& fixture, std::size_t count) {
    std::vector<AnimationInstanceSpec> specs(count, fixture.spec);
    for (std::size_t i = 0; i < count; ++i) {
        specs[i].identity.value = i + 1;
        specs[i].hostKey = "original-warning-owner:" + std::to_string(i);
        specs[i].elapsedMs = 400 + static_cast<std::uint64_t>(i % 200);
    }
    return specs;
}

bool PoseWarningIssue(const AnimationDiagnosticIssue& issue) {
    return issue.stage == "pose-warning" && issue.code == "MDX_POSE_SOURCE_ORDER_PREVIEW";
}

void PrintDiagnosticPage(std::string_view label, const AnimationDiagnosticPage& page) {
    std::cout << "PAGE " << label << " total=" << page.total << " offset=" << page.offset
        << " truncated=" << page.truncated << " issues=" << page.issues.size()
        << " source=" << page.source.has_value() << '\n';
    for (std::size_t i = 0; i < page.issues.size(); ++i) {
        const auto& issue = page.issues[i];
        auto bytes = std::min(issue.message.size(), std::size_t{192});
        while (bytes && bytes < issue.message.size() &&
            (static_cast<unsigned char>(issue.message[bytes]) & 0xC0u) == 0x80u) --bytes;
        std::cout << "PAGE_ISSUE " << label << '[' << i << "] stage=" << issue.stage << " code=" << issue.code
            << " message=" << issue.message.substr(0, bytes) << '\n';
    }
}

void PrintPoseDiagnostic(std::string_view label, const War3RenderCore::PoseDiagnostic& diagnostic) {
    std::cout << "POSE_DIAGNOSTIC " << label << " code=" << War3RenderCore::CodeName(diagnostic.code)
        << " severity=" << static_cast<std::uint32_t>(diagnostic.severity)
        << " objectId=" << diagnostic.objectId << " track=" << static_cast<std::uint32_t>(diagnostic.track)
        << " leftFrame=" << diagnostic.leftFrame << " rightFrame=" << diagnostic.rightFrame << '\n';
}

void Warnings() {
    AnimationLimits limits;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    limits.allowSourceOrderNodePreview = true;
    Fixture fixture;
    auto specs = Specs(fixture, 301);
    specs.back().sequenceIndex = 1;
    specs.back().elapsedMs = 100; // 同asset的Walk没有本动作逆序，不能借用Stand警告。
    auto service = Good(fixture.Create(limits, specs));
    const auto frame = Good(service->Prepare(nullptr));
    Check(frame->diagnostics.size() == 256 && frame->omittedDiagnostics == 44 && frame->animatedInstances == 301,
        "global diagnostic truncation fixture did not really exceed 256 successful warnings");
    const auto scene = service->Scene();
    auto query = Good(AnimationDiagnosticQuery::Create(scene));
    Check(query->SetFrame(frame).Succeeded(), "accepted warning frame could not be queried");
    const auto binding = Good(query->Binding(299, specs[299].identity));
    const auto page = Good(query->Query(binding));
    Check(page.animated && !page.hidden && !page.partial &&
        std::any_of(page.issues.begin(), page.issues.end(), PoseWarningIssue),
        "last owner's successful warning disappeared behind the global 256 summary");
    const auto& lastPose = frame->poses[299];
    const auto view = Good(ReadAnimationPoseWarnings(*scene, *frame, lastPose));
    Check(view.elapsedMs == *specs[299].elapsedMs && view.sequenceIndex == 0 &&
        lastPose.warnings.requestIndex == 299 && lastPose.warnings.identity == specs[299].identity &&
        view.records.size() == 1, "diagnostic records were rebound by shared asset instead of actual receipt");
    const auto clean = Good(query->Query(Good(query->Binding(300, specs[300].identity))));
    Check(std::none_of(clean.issues.begin(), clean.issues.end(), PoseWarningIssue) &&
        Good(ReadAnimationPoseWarnings(*scene, *frame, frame->poses[300])).records.empty(),
        "same asset's warning was copied to an independently sampled clean sequence");
    for (unsigned failure = 0; failure < 12; ++failure) {
        auto changed = std::make_shared<AnimationFrame>(*frame);
        auto& pose = changed->poses[299];
        switch (failure) {
        case 0: ++pose.warnings.requestIndex; break;
        case 1: ++pose.warnings.identity.value; break;
        case 2: ++pose.warnings.asset.value; break;
        case 3: ++pose.warnings.evaluationEpoch; break;
        case 4: ++pose.warnings.poseVersion; break;
        case 5: ++pose.warnings.elapsedMs; break;
        case 6: pose.warnings.sequenceIndex = 1; break;
        case 7: pose.warnings.count = 9; break;
        case 8: pose.warnings.records[0].severity = War3RenderCore::Severity::Error; break;
        case 9: pose.renderedInstance = 0; break;
        case 10: pose.hidden = true; break;
        case 11: pose.outsideView = true; break;
        }
        Check(!ReadAnimationPoseWarnings(*scene, *changed, pose).Succeeded(), "invalid warning binding exposed records");
        Check(query->SetFrame(changed).Succeeded() && !query->Query(binding).Succeeded(),
            "paginated Query bypassed the shared warning-owner validator");
    }
    fixture.Unchanged();

    Fixture crowded(8); // 9个真实逆序helper，进入实际Evaluate/receipt链。
    auto crowdedService = Good(crowded.Create(limits));
    const auto crowdedFrame = Good(crowdedService->Prepare(nullptr));
    const auto crowdedView = Good(ReadAnimationPoseWarnings(*crowdedService->Scene(), *crowdedFrame, crowdedFrame->poses[0]));
    Check(crowdedView.records.size() == 8 && crowdedView.omitted == 1, "per-owner fixed8 lost its exact omission count");
    auto crowdedQuery = Good(AnimationDiagnosticQuery::Create(crowdedService->Scene()));
    Check(crowdedQuery->SetFrame(crowdedFrame).Succeeded(), "crowded warning frame rejected");
    const auto crowdedBinding = Good(crowdedQuery->Binding(0, crowded.spec.identity));
    const auto first = Good(crowdedQuery->Query(crowdedBinding, 0, 8));
    PrintDiagnosticPage("crowded-first", first);
    const auto second = Good(crowdedQuery->Query(crowdedBinding, 8, 8));
    PrintDiagnosticPage("crowded-second", second);
    Check(first.total == 10 && first.offset == 0 && first.truncated && first.issues.size() == 8 && first.source &&
        first.issues.front().stage == "pose-warning" && first.issues.front().code == "ANIMATION_POSE_WARNINGS_OMITTED" &&
        first.issues.front().message.find("另有 1 条") != std::string::npos &&
        first.issues.front().message.find("超过固定容量8") != std::string::npos &&
        first.issues.front().message.find("未保存或展开") != std::string::npos && !first.issues.front().nodeId,
        "first Inspector page failed to expose the exact omitted count");
    Check(second.total == 10 && second.offset == 8 && !second.truncated && second.issues.size() == 2 && second.source &&
        PoseWarningIssue(second.issues[0]) && second.issues[0].nodeId == 9u &&
        second.issues[1].stage == "particle" && second.issues[1].code == "PARTICLE_PREVIEW_DISABLED",
        "fixed8 warnings could not be paginated without duplicated or guessed ownership");
    std::array<bool, 11> seen{};
    std::size_t warningsAcrossPages = 0;
    for (const auto* currentPage : {&first, &second}) {
        for (const auto& issue : currentPage->issues) {
            if (!PoseWarningIssue(issue)) continue;
            Check(issue.nodeId && *issue.nodeId < seen.size(), "paginated warning lost its original node binding");
            Check(!seen[*issue.nodeId], "same owned warning appeared twice across diagnostic pages");
            seen[*issue.nodeId] = true;
            ++warningsAcrossPages;
        }
    }
    Check(warningsAcrossPages == 8 && crowdedView.omitted == 1 && seen[1] && !seen[0] && !seen[2] && !seen[10],
        "complete pages changed the fixed8 warning/omission inventory");
    for (std::size_t node = 3; node <= 9; ++node)
        Check(seen[node], "a retained original warning node vanished between pages");
    // 捕获排序单独验证：一般warning先出现也不能挤掉主问题的原序预览说明。
    auto capture = frame->poses[299];
    War3RenderCore::PoseRequest request;
    request.instance = capture.identity; request.asset = capture.asset;
    request.sequenceIndex = view.sequenceIndex; request.sequenceElapsedMs = view.elapsedMs;
    request.globalElapsedMs = view.globalMs;
    War3RenderCore::PoseReceipt receipt;
    receipt.instance = capture.identity; receipt.asset = capture.asset;
    receipt.evaluationEpoch = capture.evaluationEpoch; receipt.poseVersion = capture.poseVersion;
    std::vector<War3RenderCore::PoseDiagnostic> priority(1,
        {Code::GenericRotationPreview, War3RenderCore::Severity::Warning});
    priority.insert(priority.end(), 9, view.records[0]);
    Check(CaptureAnimationPoseWarnings(capture, receipt, request, priority, false).Succeeded() &&
        capture.warnings.count == 8 && capture.warnings.omitted == 2 &&
        std::all_of(capture.warnings.records.begin(), capture.warnings.records.end(), [](const auto& record) {
            return record.code == Code::NodeSourceOrderPreview;
        }), "general warnings displaced source-order compatibility evidence");
    crowded.Unchanged();
    // 使用同一父链的成功对照，实际证明节点1/3有可捕获的原序预览警告。
    Fixture healthyDependent(1, false, true);
    auto healthyService = Good(healthyDependent.Create(limits));
    const auto healthyFrame = Good(healthyService->Prepare(nullptr));
    Check(healthyFrame->diagnostics.size() == 2 && healthyFrame->poses.size() == 1 &&
        healthyFrame->animatedInstances == 1 && healthyFrame->hiddenInstances == 0 &&
        healthyFrame->poses[0].animated && !healthyFrame->poses[0].hidden && healthyFrame->poses[0].poseVersion != 0,
        "same-parent successful control did not really publish two node-preview warnings");
    const auto healthyWarnings = Good(ReadAnimationPoseWarnings(*healthyService->Scene(), *healthyFrame,
        healthyFrame->poses[0]));
    Check(healthyWarnings.records.size() == 2 && healthyWarnings.omitted == 0,
        "same-parent successful control lost its owned preview warnings");
    for (std::size_t i = 0; i < healthyFrame->diagnostics.size(); ++i) {
        const auto& diagnostic = healthyFrame->diagnostics[i];
        const auto& owned = healthyWarnings.records[i];
        const auto expectedNode = i == 0 ? 1u : 3u;
        Check(diagnostic.code == Code::NodeSourceOrderPreview &&
            diagnostic.severity == War3RenderCore::Severity::Warning && diagnostic.objectId == expectedNode &&
            diagnostic.track == War3RenderCore::TrackKind::Translation &&
            diagnostic.leftFrame == 600 && diagnostic.rightFrame == 400 &&
            owned.code == diagnostic.code && owned.severity == diagnostic.severity &&
            owned.objectId == diagnostic.objectId && owned.track == diagnostic.track &&
            owned.leftFrame == diagnostic.leftFrame && owned.rightFrame == diagnostic.rightFrame,
            "successful control changed the parent-first preview warning identity or original frame interval");
        PrintPoseDiagnostic("healthy-dependent[" + std::to_string(i) + "]", diagnostic);
    }
    healthyDependent.Unchanged();
    // 防御性选择器的独立输入；当前核心失败receipt不同时发布此前Warning与Error。
    const std::array<War3RenderCore::PoseDiagnostic, 2> selectorInput{{
        {Code::NodeSourceOrderPreview, War3RenderCore::Severity::Warning, 1,
            War3RenderCore::TrackKind::Translation, 600, 400},
        {Code::PoseRotationInvalid, War3RenderCore::Severity::Error, 3,
            War3RenderCore::TrackKind::Rotation, 0, 0}}};
    const auto selected = FirstAnimationPoseError(selectorInput, Code::PoseNodeInvalid);
    Check(selected.code == Code::PoseRotationInvalid && selected.severity == War3RenderCore::Severity::Error &&
        selected.objectId == 3 && selected.track == War3RenderCore::TrackKind::Rotation &&
        selected.leftFrame == 0 && selected.rightFrame == 0,
        "defensive diagnostic selector replaced a real typed Error with a preceding Warning");
    const auto fallback = FirstAnimationPoseError(std::span(selectorInput).first(1), Code::PoseKeysInvalid);
    Check(fallback.code == Code::PoseKeysInvalid && fallback.severity == War3RenderCore::Severity::Error &&
        fallback.objectId == War3RenderCore::NoIndex && fallback.track == War3RenderCore::TrackKind::None &&
        fallback.leftFrame == 0 && fallback.rightFrame == 0,
        "defensive diagnostic selector lost the typed failure fallback when no Error was supplied");
    Fixture broken(1, true);
    limits.retainUnsupportedStatic = true;
    auto brokenService = Good(broken.Create(limits));
    const auto brokenFrame = Good(brokenService->Prepare(nullptr));
    const auto firstWarning = std::find_if(brokenFrame->diagnostics.begin(), brokenFrame->diagnostics.end(), [](const auto& record) {
        return record.code == Code::NodeSourceOrderPreview && record.severity == War3RenderCore::Severity::Warning;
    });
    const auto laterError = std::find_if(brokenFrame->diagnostics.begin(), brokenFrame->diagnostics.end(), [](const auto& record) {
        return record.code == Code::PoseRotationInvalid && record.severity == War3RenderCore::Severity::Error;
    });
    std::cout << "BROKEN_FRAME diagnostics=" << brokenFrame->diagnostics.size()
        << " omittedDiagnostics=" << brokenFrame->omittedDiagnostics
        << " hiddenInstances=" << brokenFrame->hiddenInstances
        << " animatedInstances=" << brokenFrame->animatedInstances
        << " poses=" << brokenFrame->poses.size()
        << " firstWarningFound=" << (firstWarning != brokenFrame->diagnostics.end())
        << " laterErrorFound=" << (laterError != brokenFrame->diagnostics.end())
        << " warningBeforeError=" << (firstWarning != brokenFrame->diagnostics.end() &&
            laterError != brokenFrame->diagnostics.end() && firstWarning < laterError) << '\n';
    const auto printedDiagnostics = std::min(brokenFrame->diagnostics.size(), std::size_t{20});
    for (std::size_t i = 0; i < printedDiagnostics; ++i)
        PrintPoseDiagnostic("broken-frame[" + std::to_string(i) + "]", brokenFrame->diagnostics[i]);
    if (printedDiagnostics != brokenFrame->diagnostics.size())
        std::cout << "BROKEN_FRAME_PRINT_OMITTED " << brokenFrame->diagnostics.size() - printedDiagnostics << '\n';
    if (!brokenFrame->poses.empty()) {
        const auto& brokenPose = brokenFrame->poses[0];
        const auto warningView = ReadAnimationPoseWarnings(*brokenService->Scene(), *brokenFrame, brokenPose);
        std::cout << "BROKEN_POSE hidden=" << brokenPose.hidden << " animated=" << brokenPose.animated
            << " staticForBudget=" << brokenPose.staticForBudget << " outsideView=" << brokenPose.outsideView
            << " poseVersion=" << brokenPose.poseVersion << " warningsCount=" << brokenPose.warnings.count
            << " warningsOmitted=" << brokenPose.warnings.omitted
            << " warningViewSucceeded=" << warningView.Succeeded()
            << " warningViewRecords=" << (warningView.value ? warningView.value->records.size() : 0)
            << " rejectionCode=" << brokenPose.rejectionCode << '\n';
        PrintPoseDiagnostic("broken-pose-rejection", brokenPose.rejection);
    }
    // PoseBatch失败分支重置诊断范围，只公开第一条Error；先前内部warning必须消失。
    Check(brokenFrame->diagnostics.size() == 1 && firstWarning == brokenFrame->diagnostics.end() &&
        laterError == brokenFrame->diagnostics.begin() && laterError->objectId == 3 &&
        laterError->track == War3RenderCore::TrackKind::Rotation &&
        laterError->leftFrame == 0 && laterError->rightFrame == 0 && brokenFrame->omittedDiagnostics == 0 &&
        brokenFrame->hiddenInstances == 1 && brokenFrame->animatedInstances == 0 && brokenFrame->poses.size() == 1 &&
        brokenFrame->poses[0].hidden && !brokenFrame->poses[0].animated &&
        brokenFrame->poses[0].rejection.code == Code::PoseRotationInvalid &&
        brokenFrame->poses[0].rejection.severity == War3RenderCore::Severity::Error &&
        brokenFrame->poses[0].rejection.objectId == 3 &&
        brokenFrame->poses[0].rejection.track == War3RenderCore::TrackKind::Rotation &&
        brokenFrame->poses[0].rejection.leftFrame == 0 && brokenFrame->poses[0].rejection.rightFrame == 0 &&
        brokenFrame->poses[0].poseVersion == 0 && brokenFrame->poses[0].warnings.count == 0 &&
        brokenFrame->poses[0].warnings.omitted == 0 &&
        Good(ReadAnimationPoseWarnings(*brokenService->Scene(), *brokenFrame, brokenFrame->poses[0])).records.empty(),
        "failed core receipt retained a prior Warning, lost the true Rotation Error, or left owned warnings on hidden pose");
    limits.retainUnsupportedStatic = false;
    auto refusing = Good(broken.Create(limits));
    const auto rejected = refusing->Prepare(nullptr);
    Check(!rejected.Succeeded() && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "MDX_POSE_ROTATION_INVALID" && diagnostic.severity == DiagnosticSeverity::Error;
    }), "strict failure retention changed for a real later-node rotation error");
    limits.playbackPolicy = AnimationPlaybackPolicy::StrictReplay;
    auto strictBroken = Good(broken.Create(limits));
    Check(HasKeysError(strictBroken->Prepare(nullptr).diagnostics),
        "default strict node policy no longer rejected the first descending track before later errors");
    broken.Unchanged();
    std::cout << "owned_warning_after_global256=1 clean_same_asset=1 invalid_bindings=12 fixed8_omitted=1 source_order_priority=1\n";
    std::cout << "actual_success_parent_warnings=2 failed_receipt_only_error=1 hidden_actual_rotation_error=1 strict_retention_reject=1\n";
    std::cout << "defensive_selector_pure_input=1 typed_fallback=1\n";
}

void Cache() {
    Fixture fixture;
    auto specs = Specs(fixture, 1024);
    AnimationLimits limits;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    limits.allowSourceOrderNodePreview = true;
    limits.retainUnsupportedStatic = true;
    const auto metadata = specs.size() * (sizeof(War3RenderSkinnedModelInstanceV2) + sizeof(AnimationPoseLink) +
        sizeof(AnimationInstanceState) + sizeof(AnimationPickBounds));
    // 完整逐实例矩阵/GEOA池超过余量；余量仍容纳批量workspace、Stand和动态槽。
    limits.maxFrameBytes = static_cast<std::uint64_t>(metadata) + 131072;
    limits.maxActivePoseBytes = 4096;
    auto service = Good(fixture.Create(limits, specs));
    const auto frame = Good(service->Prepare(nullptr));
    Check(frame->sharedPoseRanges && frame->budgetStaticInstances > 0 && frame->animatedInstances > 0,
        "bounded service fixture did not really mix cached Stand with active poses");
    std::size_t cached = frame->poses.size(), active = frame->poses.size();
    for (std::size_t i = 0; i < frame->poses.size(); ++i) {
        const auto& pose = frame->poses[i];
        const auto view = Good(ReadAnimationPoseWarnings(*service->Scene(), *frame, pose));
        Check(view.records.size() == 1 && !view.omitted && pose.warnings.identity == specs[pose.requestIndex].identity,
            "shared cache warning lost actual owner binding");
        if (pose.staticForBudget) {
            cached = i;
            Check(view.cachedStand && view.sequenceIndex == 0 && view.elapsedMs == 500 && view.globalMs == 0,
                "cached Stand warning claimed the requested elapsed time");
        } else if (pose.animated) {
            active = i;
            Check(!view.cachedStand && view.elapsedMs == frame->states[pose.requestIndex].elapsedMs,
                "active slot retained the Stand warning clock");
        }
    }
    Check(cached < frame->poses.size() && active < frame->poses.size(), "cache and active warning samples were not found");
    const auto target = frame->poses[cached].requestIndex;
    auto query = Good(AnimationDiagnosticQuery::Create(service->Scene()));
    Check(query->SetFrame(frame).Succeeded(), "cached warning frame rejected");
    const auto binding = Good(query->Binding(target, specs[target].identity));
    const auto page = Good(query->Query(binding));
    Check(page.displayedElapsedMs == 500 && page.elapsedMs == *specs[target].elapsedMs &&
        std::any_of(page.issues.begin(), page.issues.end(), [](const auto& issue) {
            return PoseWarningIssue(issue) && issue.message.find("实际缓存样本") != std::string::npos;
        }), "Query confused cached displayed time with current request time");
    auto outside = std::make_shared<AnimationFrame>(*frame);
    outside->poses[cached].outsideView = true;
    RebindCachedAnimationPoseWarnings(outside->poses[cached]);
    Check(Good(ReadAnimationPoseWarnings(*service->Scene(), *outside, outside->poses[cached])).records.empty(),
        "outside cached link retained a current warning");
    AnimationAdvance advance;
    AnimationEdit edit;
    const auto firstRequest = frame->poses[active].requestIndex;
    edit.identity = specs[firstRequest].identity;
    edit.sequenceIndex = 1; edit.elapsedMs = 100;
    advance.edits.push_back(edit);
    const auto changed = Good(service->Prepare(frame.get(), advance));
    Check(changed->poses[active].animated &&
        Good(ReadAnimationPoseWarnings(*service->Scene(), *changed, changed->poses[active])).records.empty(),
        "active slot reused an old warning after switching to the clean Walk sequence");
    fixture.Unchanged();
    std::cout << "actual_cached_stand_clock=500 active_clock_owned=1 outside_cache_cleared=1 active_clean_sequence_cleared=1\n";
}

void Anchors() {
    Fixture fixture;
    fixture.spec.playing = true;
    AnimationLimits limits;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    limits.allowSourceOrderNodePreview = true;
    limits.enableParticles = true;
    limits.retainUnsupportedStatic = true;
    auto service = Good(fixture.Create(limits));
    AnimationAdvance advance;
    advance.worldCamera = War3RenderCore::LocalCamera{{123, -1000, 1800}, {0, 0, 1}};
    auto frame = Good(service->Prepare(nullptr, advance));
    RequireBody(*frame, 4);
    Check(frame->particleLinks.size() == 1 && frame->particleLinks[0].simulated &&
        !frame->particleLinks[0].hidden && !frame->particleLinks[0].frozen &&
        frame->particleLinks[0].rejectionCode.empty() && frame->particleLinks[0].particleCount == 0 &&
        frame->timings.particleAnchorRequests == 1, "particle evaluator did not opt into the same current node pose");
    advance.deltaMs = 33;
    frame = Good(service->Prepare(frame.get(), advance));
    RequireBody(*frame, 4.33f);
    Check(frame->states[0].elapsedMs == 433 && frame->states[0].globalMs == 33 &&
        frame->particleLinks.size() == 1 && frame->particleLinks[0].simulated &&
        !frame->particleLinks[0].hidden && !frame->particleLinks[0].frozen &&
        !frame->particleLinks[0].partial && frame->particleLinks[0].resetReason.empty() &&
        frame->particleLinks[0].rejectionCode.empty() && frame->particleLinks[0].particleCount == 3 &&
        !frame->particleVertices.empty() && !frame->particleDraws.empty() &&
        frame->timings.particleAnchorRequests == 4, "bounded continuation lost real source-order particle simulation");
    Check(frame->particleCheckpoint && frame->particleCheckpoint->instances.size() == 1 &&
        frame->particleCheckpoint->instances[0], "particle proof lacks an owned accepted checkpoint");
    const auto& emitters = frame->particleCheckpoint->instances[0]->state.emitters;
    Check(emitters.size() == 1 && emitters[0].particles.size() == 3, "original PRE2 fixture did not really emit");
    for (std::size_t i = 0; i < emitters[0].particles.size(); ++i) {
        const auto& particle = emitters[0].particles[i];
        // 速度、重力、宽长都为零：410/420/430 的出生点直接证明锚点使用相同的原序窗口。
        const float expectedX = 123 + 4.1f + static_cast<float>(i) * 0.1f;
        Check(Near(particle.position[0], expectedX) && Near(particle.position[1], 456) &&
            Near(particle.position[2], 789) && Near(particle.velocity[0], 0) &&
            Near(particle.velocity[1], 0) && Near(particle.velocity[2], 0),
            "particle anchor used strict/fabricated node pose or wrong row-major placement");
    }
    fixture.Unchanged();
    std::cout << "main_and_anchor_policy=1 bounded_no_history=1 actual_particles=3 anchor_requests=4 world_position_verified=1\n";
}
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected one test group: policy, body, anchors, warnings or cache");
        const std::string_view group = argv[1];
        if (group == "policy") StrictGate();
        else if (group == "body") Body();
        else if (group == "anchors") Anchors();
        else if (group == "warnings") Warnings();
        else if (group == "cache") Cache();
        else throw std::runtime_error("unknown source-order service test group");
        std::cout << "PASS " << group << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
