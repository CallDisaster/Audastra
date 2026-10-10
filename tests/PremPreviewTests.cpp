#include "War3MapCore/Render/PremPreview.h"
#include "War3MapCore/Hash.h"
#include "War3RenderCore/CompiledModel.h"
#include "War3RenderCore/PoseBatch.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
namespace P = War3Map::Render::Prem;
using War3RenderCore::NodePose;

void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
template<class T> T Good(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "missing result" :
        result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    return std::move(*result.value);
}
bool Near(float actual, float expected) { return std::isfinite(actual) && std::abs(actual - expected) <= 0.001f; }
bool Issue(const P::Frame& frame, std::string_view code) {
    return std::any_of(frame.issues.begin(), frame.issues.end(),
        [code](const auto& value) { return std::string_view(value.code) == code; });
}

P::SourceBinding Binding() {
    return {{31, 2}, {"OriginalParent.mdx", "test-parent", std::string(64, 'a')},
        {"OriginalChild.mdx", "test-child", std::string(64, 'b')}};
}

std::shared_ptr<MdxModel> Model() {
    // 原创typed输入，用于服务数学合同；不把它描述为原MDX解析或实际GPU证据。
    auto model = std::make_shared<MdxModel>();
    model->version = 800;
    model->sequences.push_back({"Death", 1000, 3000, 1});
    MdxNode node;
    node.objectId = 0;
    node.parentId = -1;
    node.flags = 0x9000;
    node.kind = MdxNodeKind::ParticleEmitter;
    model->otherNodes.push_back(node);
    model->pivots.push_back({2, 3, 5});
    MdxPremEmitter emitter;
    emitter.flags = 0x9000;
    emitter.path = "OriginalChild.MDL";
    emitter.pathTerminated = true;
    for (std::size_t i = 0; i < emitter.path.size(); ++i) emitter.rawPath[i] = static_cast<std::byte>(emitter.path[i]);
    emitter.recordByteOffset = 128;
    emitter.recordByteSize = 456;
    emitter.payloadByteOffset = 228;
    emitter.pathByteOffset = 244;
    emitter.emissionRate = 20;
    emitter.lifeSpan = 0.5f;
    MdxPremScalarTrack visibility;
    for (const auto& [frame, value] : std::array<std::pair<std::int32_t, float>, 6>{
        {{1000, 0.0f}, {1100, 1.0f}, {1300, 0.0f}, {1500, 1.0f}, {1650, 0.0f}, {2000, 0.0f}}}) {
        MdxPremScalarKey key;
        key.frame = frame;
        key.value = value;
        visibility.keys.push_back(key);
    }
    visibility.sourceKeyCount = static_cast<std::uint32_t>(visibility.keys.size());
    emitter.tracks[static_cast<std::size_t>(MdxPremParameter::Visibility)] = std::move(visibility);
    model->premEmitters.push_back(std::move(emitter));
    return model;
}

std::shared_ptr<const P::Emitter> Emitter(std::shared_ptr<const MdxModel> model) {
    return Good(P::CreateEmitter(std::move(model), 0, 0, 7, Binding()));
}

P::Request Request(std::shared_ptr<const P::Emitter> emitter, std::uint64_t elapsed) {
    P::Request request;
    request.emitter = std::move(emitter);
    request.identity = {17, 3};
    request.requestIndex = 42;
    request.elapsedMs = elapsed;
    request.globalMs = elapsed + 1000;
    return request;
}

NodePose Node(const P::Request& request, std::uint64_t elapsed = 0) {
    NodePose node;
    node.objectId = request.emitter->Source().nodeId;
    node.pivot = request.emitter->Model().pivots[node.objectId];
    node.worldMatrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
        static_cast<float>(elapsed) * 0.01f, 0, 0, 1};
    return node;
}

P::NodeSampler Sampler(std::vector<std::pair<std::uint64_t, std::uint64_t>>* clocks = nullptr) {
    return [clocks](const P::Request& request, std::uint64_t elapsed, std::uint64_t global) {
        if (clocks) clocks->push_back({elapsed, global});
        return Result<NodePose>::Success(Node(request, elapsed));
    };
}

P::Frame Build(const P::Request& request, const P::NodeSampler& sampler = Sampler(), const P::Limits& limits = {}) {
    return Good(P::Build(std::span(&request, 1), sampler, limits));
}

void Visibility() {
    const auto emitter = Emitter(Model());
    const std::array<std::pair<std::uint64_t, std::size_t>, 11> cases{{
        {0, 0}, {99, 0}, {100, 0}, {148, 0}, {149, 1}, {299, 4}, {300, 4}, {500, 4}, {549, 5}, {600, 6}, {650, 6}}};
    for (const auto& [time, count] : cases) {
        const auto frame = Build(Request(emitter, time));
        Check(frame.children.size() == count && frame.unsupportedRequests == 0 && frame.issues.empty(),
            "None visibility/lifetime produced the wrong number of children");
        for (const auto& child : frame.children) {
            Check((child.birthElapsedMs >= 100 && child.birthElapsedMs < 300) ||
                (child.birthElapsedMs >= 500 && child.birthElapsedMs < 650),
                "child born outside the actual None visibility windows");
            Check(child.ageMs < 500 && child.birthGlobalMs == child.birthElapsedMs + 1000 &&
                child.parentIdentity == War3RenderCore::InstanceIdentity{17, 3} && child.parentRequestIndex == 42 &&
                child.emitter == emitter && child.emitter->Source().recordByteOffset == 128 && child.emitter->ChildModelIndex() == 7,
                "child lifetime/source/owner/birth clock metadata was not preserved");
        }
    }
    Check(Build(Request(emitter, 1150)).children.empty(), "expired non-looping children were replayed");
    std::cout << "visibility_half_open_windows=2 exact_birth_grid=1 expired_cleared=1 original_owner_source_preserved=1\n";
}

void Transform() {
    auto model = Model();
    auto& emitter = model->premEmitters[0];
    emitter.tracks = {};
    emitter.emissionRate = 1000;
    auto request = Request(Emitter(model), 0);
    request.parentTransform = {0, 0, 4, 123, 0, 3, 0, 456, -2, 0, 0, 789};
    const auto sampler = [](const P::Request& input, std::uint64_t, std::uint64_t) {
        auto node = Node(input);
        node.worldMatrix = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 2, 0, 7, 11, 13, 1};
        return Result<NodePose>::Success(node);
    };
    const auto first = Build(request, sampler);
    Check(first.children.size() == 1, "first visible millisecond did not emit at rate1000");
    constexpr std::array<float, 12> expected{0, 0, 8, 215, 3, 0, 0, 495, 0, 2, 0, 781};
    for (std::size_t i = 0; i < expected.size(); ++i)
        Check(Near(first.children[0].transform[i], expected[i]), "node*pivot*nonuniform parent transform was repeated or transposed");
    model = Model();
    model->sequences[0].end = 11000;
    model->premEmitters[0].tracks = {};
    model->premEmitters[0].emissionRate = 1;
    model->premEmitters[0].lifeSpan = 5;
    model->premEmitters[0].gravity = 2;
    model->premEmitters[0].speed = 10;
    request.emitter = Emitter(model);
    request.elapsedMs = 1999;
    request.globalMs = 2999;
    const auto moving = Build(request, sampler);
    Check(moving.children.size() == 2 && moving.children[0].ageMs == 1000 &&
        Near(moving.children[0].transform[3], 291) && Near(moving.children[0].transform[7], 495) &&
        Near(moving.children[0].transform[11], 781), "speed/localZ gravity escaped model space or parent was applied twice");
    Check(moving.children[0].birthElapsedMs == 999 && moving.children[0].birthGlobalMs == 1999,
        "historical birth sample did not bind actual local/global clocks");
    const auto invalid = [](const P::Request& input, std::uint64_t, std::uint64_t) {
        auto node = Node(input);
        ++node.objectId;
        return Result<NodePose>::Success(node);
    };
    const auto bad = Build(request, invalid);
    Check(bad.children.empty() && bad.unsupportedRequests == 1 && Issue(bad, "PREM_NODE_TRANSFORM_INVALID"),
        "wrong compiled node objectId was used as an emission anchor");
    const auto badPivot = [](const P::Request& input, std::uint64_t, std::uint64_t) {
        auto node = Node(input);
        node.pivot[0] += 1;
        return Result<NodePose>::Success(node);
    };
    Check(Build(request, badPivot).unsupportedRequests == 1, "different source pivot was accepted as actual compiled anchor");
    std::cout << "nonzero_pivot=1 node_columnmajor=1 parent_rowmajor_once=1 rotated_model_gravity=1\n";
}

void Deterministic() {
    auto model = Model();
    model->premEmitters[0].speed = 100;
    model->premEmitters[0].latitude = 0.7f;
    model->premEmitters[0].longitude = 3.1f;
    const auto emitter = Emitter(model);
    const auto request = Request(emitter, 600);
    const auto first = Build(request), paused = Build(request);
    Check(first.children.size() == paused.children.size(), "pause changed population");
    for (std::size_t i = 0; i < first.children.size(); ++i)
        Check(first.children[i].transform == paused.children[i].transform &&
            first.children[i].birthOrdinal == paused.children[i].birthOrdinal, "same clock did not repeat bitwise");
    (void)Build(Request(emitter, 1000));
    const auto reversed = Build(Request(emitter, 600));
    for (std::size_t i = 0; i < first.children.size(); ++i)
        Check(first.children[i].transform == reversed.children[i].transform, "rewind reused future particle state");
    auto other = request;
    ++other.identity.generation;
    const auto changed = Build(other);
    Check(changed.children[0].transform != first.children[0].transform, "random seed ignored owner generation");
    std::vector<std::pair<std::uint64_t, std::uint64_t>> clocks;
    (void)Build(request, Sampler(&clocks));
    Check(clocks.size() == first.children.size(), "birth did not require exactly one actual node sample");
    for (const auto& [elapsed, global] : clocks) Check(global == elapsed + 1000, "birthGlobal was rebased to current global");
    model = Model();
    model->sequences[0] = {"Stand", 1000, 2000, 0};
    model->premEmitters[0].tracks = {};
    model->premEmitters[0].lifeSpan = 0.2f;
    const auto late = Build(Request(Emitter(model), 300001));
    Check(late.children.size() == 4 && late.anchorVisits == 4 && late.evaluatedBirths == 4,
        "long looping timeline replayed history outside the lifespan window");
    Check(late.children.back().birthElapsedMs == 299999 && late.children.back().birthOrdinal == 6000,
        "loop boundary changed deterministic emission ordinal or birth time");
    std::cout << "pause_bitwise_equal=1 rewind_rebuild=1 generation_seed_bound=1 long_timeline_anchors4=1\n";
}

void Bounds() {
    const auto emitter = Emitter(Model());
    const auto request = Request(emitter, 299);
    P::Limits limits;
    limits.maxBirths = 2;
    auto bounded = Build(request, Sampler(), limits);
    Check(bounded.children.size() == 2 && bounded.suppressedChildren == 2 && bounded.evaluatedBirths == 2 &&
        bounded.anchorVisits == 2 && Issue(bounded, "PREM_CHILD_BUDGET"), "birth budget did not suppress exact remainder");
    limits = {};
    limits.maxLiveChildren = 1;
    bounded = Build(request, Sampler(), limits);
    Check(bounded.children.size() == 1 && bounded.suppressedChildren == 3 && bounded.anchorVisits == 1,
        "pooled child cap was exceeded or counted as source failure");
    limits = {};
    limits.maxAnchorVisits = 1;
    bounded = Build(request, Sampler(), limits);
    Check(bounded.children.size() == 1 && bounded.suppressedChildren == 3 && bounded.anchorVisits == 1,
        "strict anchor visit cap did not stop births");
    limits = {};
    limits.maxVisibilityKeyVisits = 1;
    bounded = Build(request, Sampler(), limits);
    Check(bounded.children.empty() && bounded.visibilityKeyVisits == 0 && bounded.unsupportedRequests == 1 &&
        Issue(bounded, "PREM_VISIBILITY_VISIT_LIMIT"), "visibility key scan ignored its frame limit");
    limits = {};
    limits.maxFrameBytes = 1;
    Check(!P::Build(std::span(&request, 1), Sampler(), limits).Succeeded(), "frame-byte budget ignored");
    limits = {};
    limits.maxLiveChildren = 4097;
    Check(!P::Build(std::span(&request, 1), Sampler(), limits).Succeeded(), "hard live pool limit relaxed");
    limits = {};
    limits.maxRequests = 1;
    const std::array<P::Request, 2> many{request, request};
    Check(!P::Build(many, Sampler(), limits).Succeeded(), "request-count bound ignored");
    std::stop_source cancelled;
    cancelled.request_stop();
    Check(!P::Build(std::span(&request, 1), Sampler(), {}, cancelled.get_token()).Succeeded(), "cancelled frame published a result");
    std::stop_source during;
    const auto stopSampler = [&during](const P::Request& input, std::uint64_t elapsed, std::uint64_t) {
        during.request_stop();
        return Result<NodePose>::Success(Node(input, elapsed));
    };
    auto oneBirth = Request(emitter, 149);
    Check(!P::Build(std::span(&oneBirth, 1), stopSampler, {}, during.get_token()).Succeeded(),
        "last-anchor cancellation still published a frame");
    auto clock = request;
    clock.globalMs = 0;
    bounded = Build(clock);
    Check(bounded.children.size() == 1 && bounded.children[0].birthElapsedMs == 299 && bounded.anchorVisits == 1 &&
        bounded.clockRejectedChildren == 3 && bounded.suppressedChildren == 3 && Issue(bounded, "PREM_BIRTH_GLOBAL_UNDERFLOW"),
        "negative actual birthGlobal was silently replaced with zero or sampled");
    unsigned calls = 0;
    const auto broken = [&calls](const P::Request& input, std::uint64_t elapsed, std::uint64_t) {
        if (++calls == 2) return Result<NodePose>::Failure("test", "REAL_NODE_ERROR", "original node failure");
        return Result<NodePose>::Success(Node(input, elapsed));
    };
    bounded = Build(request, broken);
    Check(bounded.children.empty() && bounded.unsupportedRequests == 1 && bounded.suppressedChildren == 4 &&
        bounded.issues.size() == 1 && Issue(bounded, "PREM_NODE_SAMPLE_FAILED"), "failed anchor left a partial untrusted emitter pool");
    auto huge = request;
    huge.parentTransform[0] = std::numeric_limits<float>::max();
    bounded = Build(huge);
    Check(bounded.children.empty() && bounded.unsupportedRequests == 1 && Issue(bounded, "PREM_NODE_TRANSFORM_INVALID"),
        "finite input multiplication overflow published nonfinite geometry");
    std::cout << "live_birth_anchor_keys_requests_bytes_hardcaps=1 exact_suppression=1 global_underflow3=1 failed_anchor_pool_clear=1\n";
}

void Unsupported() {
    const auto rejects = [](const auto& mutate) {
        auto model = Model();
        mutate(*model);
        Check(!P::CreateEmitter(model, 0, 0, 7, Binding()).Succeeded(), "unsupported emitter accepted by basic preview");
    };
    rejects([](MdxModel& model) { model.premEmitters[0].flags = 0x11000; });
    rejects([](MdxModel& model) { model.premEmitters[0].emissionRate = std::numeric_limits<float>::quiet_NaN(); });
    rejects([](MdxModel& model) { model.premEmitters[0].lifeSpan = 31; });
    rejects([](MdxModel& model) { model.premEmitters[0].pathTerminated = false; });
    rejects([](MdxModel& model) { model.premEmitters[0].tracks[0] = MdxPremScalarTrack{}; });
    rejects([](MdxModel& model) { model.premEmitters[0].tracks[6]->globalSequence = 0; });
    rejects([](MdxModel& model) { model.premEmitters[0].tracks[6]->interpolation = 1; });
    rejects([](MdxModel& model) { model.premEmitters[0].tracks[6]->keys[1].frame = 1000; });
    rejects([](MdxModel& model) { model.premEmitters[0].tracks[6]->keys[1].value = std::numeric_limits<float>::infinity(); });
    rejects([](MdxModel& model) { model.premEmitters[0].support = MdxPremSupport::UnknownTrack; });
    auto model = Model();
    auto wrongSource = Binding();
    wrongSource.parentSource.contentDigest = "not-sha256";
    Check(!P::CreateEmitter(model, 0, 0, 7, wrongSource).Succeeded(), "source metadata identity absent");
    const auto emitter = Emitter(model);
    auto request = Request(emitter, 600);
    request.elapsedMs = UINT64_MAX;
    Check(Build(request).unsupportedRequests == 1, "unbounded timeline overflow accepted");
    request = Request(emitter, 600);
    request.sequenceIndex = 99;
    Check(Build(request).unsupportedRequests == 1, "unknown sequence used another action's visibility");
    const std::array<P::Request, 2> inputs{request, Request(emitter, 600)};
    const auto isolated = Good(P::Build(inputs, Sampler()));
    Check(isolated.unsupportedRequests == 1 && isolated.children.size() == 6 && isolated.issues[0].parentRequestIndex == 42,
        "unsupported request hid another emitter's valid child transforms");
    std::cout << "dynamic_global_unknown_flags_badkeys_nonfinite_localreject=1 source_identity_required=1 unrelated_request_survives=1\n";
}

std::filesystem::path ParentEnvironment() {
#ifdef _WIN32
    const auto size = GetEnvironmentVariableW(L"FORGE_PREM_PARENT", nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const auto count = GetEnvironmentVariableW(L"FORGE_PREM_PARENT", value.data(), size);
    Check(count && count < size, "FORGE_PREM_PARENT changed during capture");
    value.resize(count);
    return value;
#else
    const auto* value = std::getenv("FORGE_PREM_PARENT");
    return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

int Real() {
    const auto parent = ParentEnvironment();
    if (parent.empty()) { std::cout << "SKIP FORGE_PREM_PARENT not set\n"; return 77; }
    constexpr std::string_view expectedSha = "95ccef98364b0efe3419945d994063388c1d77f8f4e9519d7a7cb1c8e66f9f0d";
    Check(Good(Sha256File(parent)) == expectedSha && std::filesystem::file_size(parent) == 69822,
        "real Voodoo parent must bind the exact approved readonly sample SHA/bytes");
    std::ifstream file(parent, std::ios::binary);
    Check(bool(file), "real parent could not be opened");
    std::vector<std::byte> bytes(69822);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(file.gcount() == static_cast<std::streamsize>(bytes.size()) && Good(Sha256(bytes)) == expectedSha,
        "real parent changed during readonly capture");
    auto model = std::make_shared<const MdxModel>(Good(ParseMdx(bytes)));
    Check(model->premEmitters.size() == 1 && model->premEmitters[0].nodeId == 48 &&
        model->premEmitters[0].recordByteOffset == 66651 && model->premEmitters[0].recordByteSize == 456,
        "actual typed PREM/source offsets differ from minimum sample");
    auto binding = Binding();
    binding.parentSource = {"buildings\\orc\\VoodooLounge\\VoodooLounge.mdx", "readonly-local-env", std::string(expectedSha)};
    binding.childSource = {"SharedModels\\BuildingParticlebeam.mdx", "source-binding-only-no-child-geometry-test",
        "e8c96af76655604a5c8b2cb00ea84c757dbf30a1037b22954264d5c8150a9b9e"};
    const auto compiled = Good(War3RenderCore::CompileMdxModel(*model, binding.parentAsset, binding.parentSource));
    std::uint32_t nodeIndex = UINT32_MAX;
    for (std::uint32_t i = 0; i < compiled.NodeCount(); ++i) if (compiled.Node(i).objectId == 48) nodeIndex = i;
    Check(nodeIndex != UINT32_MAX, "real compiled node48 was not available");
    auto emitter = Good(P::CreateEmitter(model, 0, nodeIndex, 7, binding));
    War3RenderCore::BatchLimits limits;
    limits.maxInstances = 1;
    limits.maxNodes = compiled.NodeCount();
    limits.maxGroups = compiled.GroupCount();
    limits.maxGeosets = compiled.GeosetCount();
    auto evaluator = Good(War3RenderCore::PoseBatchEvaluator::Create(std::span(&compiled, 1), 5, limits));
    const auto actual = [&evaluator, &compiled](const P::Request& input, std::uint64_t elapsed, std::uint64_t global) -> Result<NodePose> {
        War3RenderCore::PoseRequest pose;
        pose.instance = input.identity;
        pose.asset = compiled.Identity();
        pose.sequenceIndex = input.sequenceIndex;
        pose.sequenceElapsedMs = elapsed;
        pose.globalElapsedMs = global;
        const auto outcome = evaluator.Evaluate(std::span(&pose, 1));
        if (outcome.error != War3RenderCore::Code::Ok || evaluator.Receipts().size() != 1 ||
            evaluator.Receipts()[0].error != War3RenderCore::Code::Ok)
            return Result<NodePose>::Failure("real", "ACTUAL_CORE_POSE_FAILED", "strict actual parent pose failed");
        const auto nodes = evaluator.Nodes(0);
        if (input.emitter->CompiledNodeIndex() >= nodes.size())
            return Result<NodePose>::Failure("real", "ACTUAL_CORE_NODE_MISSING", "actual node index missing");
        return Result<NodePose>::Success(nodes[input.emitter->CompiledNodeIndex()]);
    };
    auto request = Request(emitter, 150);
    request.sequenceIndex = 2;
    request.parentTransform[3] = 123;
    request.parentTransform[7] = 456;
    request.parentTransform[11] = 789;
    const auto frame = Build(request, actual);
    Check(frame.children.size() == 2 && frame.anchorVisits == 2 && frame.issues.empty() &&
        frame.children[0].birthElapsedMs == 80 && frame.children[1].birthElapsedMs == 128 &&
        frame.children[0].birthGlobalMs == 1080 && frame.children[1].birthGlobalMs == 1128,
        "real Death150ms did not produce the two bounded actual-node child transforms");
    Check(model->pivots[48] == std::array<float, 3>{-6.822879791259766f, -4.979599952697754f, 0},
        "real nonzero emitter pivot changed");
    request.elapsedMs = 80;
    request.globalMs = 1080;
    const auto birth = Build(request, actual);
    Check(birth.children.size() == 1 && birth.children[0].ageMs == 0 &&
        Near(birth.children[0].transform[3], 123 + model->pivots[48][0]) &&
        Near(birth.children[0].transform[7], 456 + model->pivots[48][1]) &&
        Near(birth.children[0].transform[11], 789), "real birth anchor lost its source pivot or repeated parent translation");
    request.elapsedMs = 32;
    request.globalMs = 1032;
    Check(Build(request, actual).children.empty(), "real Death before KPEV visibility emitted children");
    Check(Good(Sha256File(parent)) == expectedSha && Good(Sha256(bytes)) == expectedSha,
        "real parent bytes were modified");
    std::cout << "real_parent_sha=" << expectedSha << " typed_node48=1 Death150_live2=1 actual_strict_birth_node_samples2=1"
        " parent_position123_456_789=1 readonly_bytes_unchanged=1 child_geometry_gpu_not_tested=1\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        if (group == "real") return Real();
        Check(group == "all" || group == "visibility" || group == "transform" || group == "deterministic" ||
            group == "bounds" || group == "unsupported", "unknown PREM preview test group");
        if (group == "all" || group == "visibility") Visibility();
        if (group == "all" || group == "transform") Transform();
        if (group == "all" || group == "deterministic") Deterministic();
        if (group == "all" || group == "bounds") Bounds();
        if (group == "all" || group == "unsupported") Unsupported();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
