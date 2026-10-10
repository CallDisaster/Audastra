#include "WorldAnimationDiagnostics.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include <QCoreApplication>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace War3Map;
using namespace War3Map::Modules;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool SameWarning(const Render::AnimationPoseWarnings& left, const Render::AnimationPoseWarnings& right) {
    if (left.count != right.count || left.omitted != right.omitted || left.requestIndex != right.requestIndex
        || left.sequenceIndex != right.sequenceIndex || left.identity != right.identity || left.asset != right.asset
        || left.evaluationEpoch != right.evaluationEpoch || left.poseVersion != right.poseVersion
        || left.elapsedMs != right.elapsedMs || left.globalMs != right.globalMs || left.cachedStand != right.cachedStand) return false;
    for (std::size_t i = 0; i < left.records.size(); ++i) {
        const auto& a = left.records[i]; const auto& b = right.records[i];
        if (a.code != b.code || a.severity != b.severity || a.objectId != b.objectId
            || a.track != b.track || a.leftFrame != b.leftFrame || a.rightFrame != b.rightFrame) return false;
    }
    return true;
}
struct Fixture {
    std::shared_ptr<Render::AnimatedModelScene> scene = std::make_shared<Render::AnimatedModelScene>();
    std::shared_ptr<Render::AnimationFrame> frame = std::make_shared<Render::AnimationFrame>();
    explicit Fixture(std::uint32_t count) {
        scene->sceneVersion = 11; scene->generation = 2; scene->documentRevision = 7; scene->sourceIdentity = "frozen-map";
        scene->particleCapabilities.resize(count); scene->sequences.resize(count);
        scene->sources.push_back({"units\\fixture.mdx", "frozen-map|game:base.mpq", std::string(64, 'a')});
        frame->sceneVersion = 11; frame->documentRevision = 7; frame->contentVersion = 19;
        for (std::uint32_t request = 0; request < count; ++request) {
            Render::AnimationInstanceSpec spec; spec.identity = {request + 1, 2};
            spec.hostKey = "units:" + std::to_string(request); spec.model.modelPath = "Units/Fixture.mdx";
            scene->specs.push_back(spec); scene->geometry.instances.push_back({});
            scene->geometry.instanceRequests.push_back(request);
            Render::AnimationInstanceState state; state.identity = spec.identity; frame->states.push_back(state);
            Render::AnimationPoseLink pose; pose.identity = spec.identity; pose.requestIndex = request;
            pose.animated = true; pose.poseVersion = 8; frame->poses.push_back(pose);
        }
    }
    void SkipEmitters() {
        auto issues = std::make_shared<std::vector<Render::AnimationParticleIssue>>();
        for (const auto index : {2u, 3u, 6u, 7u}) issues->push_back({"PRE2", index, index + 200,
            "PARTICLE_KEYS_INVALID", "粒子按键必须严格递增。"});
        for (auto& capability : scene->particleCapabilities) capability.skippedEmitters = issues;
    }
    void Warning(std::uint32_t request, std::uint64_t elapsedMs, std::uint64_t globalMs,
        bool cachedStand = false) {
        scene->sequences[request].resize(2);
        auto& state = frame->states[request];
        state.sequenceIndex = 1; state.elapsedMs = elapsedMs; state.globalMs = globalMs;
        auto& pose = frame->poses[request];
        pose.renderedInstance = request; pose.asset = {41, scene->generation};
        pose.evaluationEpoch = 5; pose.poseVersion = 8;
        pose.staticForBudget = cachedStand; pose.animated = !cachedStand;
        pose.staticSequenceIndex = 0; pose.staticElapsedMs = 0; pose.staticGlobalMs = 77;
        pose.standPoseIndex = cachedStand ? 3 : UINT32_MAX;
        auto& warnings = pose.warnings;
        warnings.count = 1; warnings.requestIndex = request;
        warnings.identity = pose.identity; warnings.asset = pose.asset;
        warnings.evaluationEpoch = pose.evaluationEpoch; warnings.poseVersion = pose.poseVersion;
        warnings.cachedStand = cachedStand;
        warnings.sequenceIndex = cachedStand ? pose.staticSequenceIndex : state.sequenceIndex;
        warnings.elapsedMs = cachedStand ? pose.staticElapsedMs : state.elapsedMs;
        warnings.globalMs = cachedStand ? pose.staticGlobalMs : state.globalMs;
        auto& record = warnings.records[0];
        record.code = War3RenderCore::Code::NodeSourceOrderPreview;
        record.severity = War3RenderCore::Severity::Warning;
        record.objectId = 19; record.track = War3RenderCore::TrackKind::Translation;
        record.leftFrame = 600; record.rightFrame = 400;
    }
};
void RepeatedPlacementsCollapseWithoutLosingRawRecords() {
    Fixture fixture(250); fixture.SkipEmitters();
    auto duplicated = std::make_shared<std::vector<Render::AnimationParticleIssue>>(
        *fixture.scene->particleCapabilities[0].skippedEmitters);
    duplicated->push_back(duplicated->front()); fixture.scene->particleCapabilities[0].skippedEmitters = duplicated;
    WorldAnimationDiagnostics diagnostics;
    const auto grouped = diagnostics.Summarize(fixture.scene, *fixture.frame, 2);
    Require(grouped.modelGroups.size() == 4 && grouped.modelGroups[2].requests.size() == 250
        && grouped.modelGroups[3].records == 250 && grouped.modelGroups[2].sourceKnown,
        "full typed model list retains groups and placement identities beyond compressed two-line summary");
    Require(grouped.groups == 4 && grouped.records == 1001 && grouped.representedRecords == 501,
        "four emitter reasons aggregate records and distinct placements independently");
    const auto text = grouped.rows.join('\n');
    Require(text.contains("250 个摆放") && text.contains("2 组未展开") && text.contains("500 条原始记录")
        && text.contains("字节偏移未知") && text.contains("实例 1/2") && text.contains(QString(64, 'a')),
        "rows expose actual source hash, sample identity, unknown offset and bounded omitted-record count");
    const auto query = Render::AnimationDiagnosticQuery::Create(fixture.scene);
    Require(query.Succeeded() && (*query.value)->SetFrame(fixture.frame).Succeeded(), "original query remains available");
    const auto binding = (*query.value)->Binding(0, fixture.scene->specs[0].identity);
    const auto page = (*query.value)->Query(*binding.value, 0, 3);
    const auto tail = (*query.value)->Query(*binding.value, 3, 3);
    Require(page.Succeeded() && tail.Succeeded() && page.value->total == 5 && page.value->issues.size() == 3
        && tail.value->issues.size() == 2 && duplicated->size() == 5,
        "aggregation does not discard original emitter records or change raw pagination");
}
void AmbiguousSourcesStaySeparateAndSceneChangesInvalidate() {
    Fixture fixture(2); fixture.SkipEmitters();
    fixture.scene->sources.push_back({"UNITS/Fixture.mdx", "frozen-map|map:import", std::string(64, 'b')});
    WorldAnimationDiagnostics diagnostics;
    const auto ambiguous = diagnostics.Summarize(fixture.scene, *fixture.frame);
    Require(ambiguous.groups == 8 && ambiguous.rows.join('\n').contains("来源/SHA未知，未跨实例合并"),
        "same virtual path with contradictory actual sources cannot merge instances");
    Require(ambiguous.modelGroups.size() == 8 && !ambiguous.modelGroups.front().sourceKnown
        && ambiguous.modelGroups.front().requests.size() == 1,
        "typed list keeps unknown sources separate and exposes original requested model path");
    Fixture missingDigest(2); missingDigest.SkipEmitters(); missingDigest.scene->sources[0].contentDigest.clear();
    Require(diagnostics.Summarize(missingDigest.scene, *missingDigest.frame).groups == 8,
        "path and layer without a content digest do not prove shared content");
    Fixture next(2); next.SkipEmitters(); next.scene->sceneVersion = 12;
    next.scene->sources[0].contentDigest.assign(64, 'c');
    Require(diagnostics.Summarize(next.scene, *next.frame).records == 0, "old frame is not shown for replacement scene");
    next.frame->sceneVersion = 12;
    const auto rebound = diagnostics.Summarize(next.scene, *next.frame);
    Require(rebound.groups == 4 && rebound.rows.join('\n').contains(QString(64, 'c'))
        && !rebound.rows.join('\n').contains(QString(64, 'a')), "new scene replaces cached source identity");
    std::weak_ptr<const Render::AnimatedModelScene> held = next.scene;
    next.scene.reset(); diagnostics.Clear(); Require(held.expired(), "closing scene releases diagnostic asset ownership");
}
void ComponentsOffsetsAndBudgetStatesDoNotCollapse() {
    Fixture fixture(2);
    for (std::uint32_t i = 0; i < 2; ++i) {
        Render::AnimationGeosetIssue geoset; geoset.geosetIndex = 8;
        geoset.diagnostic.code = War3RenderCore::Code::GeoaKeysInvalid;
        fixture.frame->geosetIssues.push_back(geoset); fixture.frame->poses[i].geosetIssues = {i, 1};
        Render::AnimationMaterialIssue material; material.source.materialIndex = 3; material.source.layerIndex = 1;
        material.source.sourceByteOffset = 128 + i * 16; material.source.error = War3RenderCore::Code::MaterialAlphaKeysInvalid;
        fixture.frame->materialIssues.push_back(material); fixture.frame->poses[i].materialIssues = {i, 1};
        Render::AnimationParticleLink particle; particle.requestIndex = i; particle.identity = fixture.scene->specs[i].identity;
        particle.rejectionCode = "SAME_DIAGNOSTIC"; particle.rejectionMessage = "same reason"; particle.frozen = true;
        particle.outsideView = i == 0; particle.budgetLimited = i == 1; fixture.frame->particleLinks.push_back(particle);
    }
    WorldAnimationDiagnostics diagnostics;
    const auto result = diagnostics.Summarize(fixture.scene, *fixture.frame);
    const auto text = result.rows.join('\n');
    Require(result.groups == 5 && result.records == 6 && text.contains("2 个摆放")
        && text.contains("字节偏移 128") && text.contains("字节偏移 144")
        && text.contains("视野外") && text.contains("预算省略"), "GEOA groups by source and component; offsets and states remain separate");
    fixture.frame->poses[0].geosetIssues = {UINT32_MAX, 3};
    Require(diagnostics.Summarize(fixture.scene, *fixture.frame).rows.join('\n').contains("诊断范围无效"),
        "malformed range stays explicit instead of reading outside frame");
}

void SuccessfulWarningsKeepSampledOwnerAndClock() {
    Fixture fixture(3);
    fixture.Warning(0, 400, 1200); fixture.Warning(1, 600, 1400);
    // 第三个同模型实例未捕获警告；全局摘要同样不能承担实例归属。
    fixture.frame->poses[2].asset = fixture.frame->poses[0].asset;
    auto unbound = fixture.frame->poses[0].warnings.records[0];
    unbound.objectId = 99; fixture.frame->diagnostics.push_back(unbound);
    const auto original0 = fixture.frame->poses[0].warnings;
    const auto original1 = fixture.frame->poses[1].warnings;
    WorldAnimationDiagnostics diagnostics;
    const auto summary = diagnostics.Summarize(fixture.scene, *fixture.frame);
    Require(summary.groups == 2 && summary.records == 2 && summary.modelGroups.size() == 2,
        "successful warnings retain the two actual samples without attributing global diagnostics or unsampled neighbor");
    for (std::uint32_t request = 0; request < 2; ++request) {
        const auto found = std::find_if(summary.modelGroups.begin(), summary.modelGroups.end(), [request](const auto& group) {
            return group.requests == std::vector<std::uint32_t>{request};
        });
        Require(found != summary.modelGroups.end() && found->sourceKnown && found->records == 1
            && found->stage == QStringLiteral("兼容预览警告"), "same asset warnings retain exact separate sampled owners");
        const auto clock = request == 0 ? QStringLiteral("400/1200 ms") : QStringLiteral("600/1400 ms");
        Require(found->reason.contains(clock) && found->reason.contains("动作索引 1")
            && found->reason.contains("节点 19") && found->reason.contains("TRS/Translation")
            && found->reason.contains("原帧 600 → 400") && found->reason.contains("兼容近似预览")
            && found->reason.contains("未确认原版等价"), "warning list exposes node/TRS/source frames and actual sample clock");
    }
    Require(!summary.rows.join('\n').contains("节点 99") && !summary.rows.join('\n').contains("本帧隐藏"),
        "unbound global warning is ignored and successful preview is not displayed as a hidden failure");
    Require(SameWarning(fixture.frame->poses[0].warnings, original0)
        && SameWarning(fixture.frame->poses[1].warnings, original1)
        && !fixture.frame->poses[0].hidden && !fixture.frame->poses[0].partial
        && !fixture.frame->poses[1].hidden && !fixture.frame->poses[1].partial,
        "aggregation does not rewrite raw warnings or alter successful pose flags");

    Fixture unknown(2); unknown.Warning(0, 400, 1200); unknown.Warning(1, 400, 1200);
    unknown.scene->sources[0].contentDigest.clear();
    const auto separated = diagnostics.Summarize(unknown.scene, *unknown.frame);
    Require(separated.groups == 2 && std::all_of(separated.modelGroups.begin(), separated.modelGroups.end(), [](const auto& group) {
        return !group.sourceKnown && group.requests.size() == 1;
    }), "same warning and clock cannot merge instances whose actual source digest is unknown");
}

void InvalidWarningOwnersAreNeverAssignedToPlacements() {
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        Fixture fixture(2); fixture.Warning(0, 400, 1200);
        auto& warnings = fixture.frame->poses[0].warnings;
        switch (invalid) {
        case 0: warnings.requestIndex = 1; break;
        case 1: warnings.identity = fixture.scene->specs[1].identity; break;
        case 2: ++warnings.asset.generation; break;
        case 3: ++warnings.evaluationEpoch; break;
        case 4: ++warnings.elapsedMs; break;
        case 5: warnings.count = static_cast<std::uint32_t>(Render::AnimationPoseWarnings::Capacity + 1); break;
        case 6: fixture.scene->geometry.instanceRequests[0] = 1; break;
        }
        WorldAnimationDiagnostics diagnostics;
        const auto summary = diagnostics.Summarize(fixture.scene, *fixture.frame);
        const auto failure = std::find_if(summary.modelGroups.begin(), summary.modelGroups.end(), [](const auto& group) {
            return group.code == QStringLiteral("ANIMATION_POSE_WARNING_BINDING_INVALID");
        });
        Require(failure != summary.modelGroups.end() && failure->requests.empty() && !failure->sourceKnown
            && failure->reason.contains("实例警告绑定无效，未使用记录"),
            "invalid owner/clock/range becomes an unbound explicit failure without guessing placement provenance");
        Require(std::none_of(summary.modelGroups.begin(), summary.modelGroups.end(), [](const auto& group) {
            return group.stage == QStringLiteral("兼容预览警告");
        }), "invalid warning cannot reappear through the model-path grouping fallback");
    }
    Fixture replacement(1); replacement.Warning(0, 400, 1200); ++replacement.scene->sceneVersion;
    WorldAnimationDiagnostics diagnostics;
    Require(diagnostics.Summarize(replacement.scene, *replacement.frame).modelGroups.empty(),
        "replacement scene never displays old successful-warning sample");
}

void CachedClockAndOmittedWarningCountStayExplicit() {
    Fixture fixture(2);
    fixture.Warning(0, 9000, 24000, true); fixture.Warning(1, 0, 77);
    const auto original = fixture.frame->poses[0].warnings;
    auto& warning = fixture.frame->poses[0].warnings;
    warning.count = static_cast<std::uint32_t>(Render::AnimationPoseWarnings::Capacity);
    std::fill(warning.records.begin(), warning.records.end(), warning.records[0]); warning.omitted = 13;
    const auto frozen = warning;
    WorldAnimationDiagnostics diagnostics;
    const auto summary = diagnostics.Summarize(fixture.scene, *fixture.frame);
    Require(summary.groups == 3 && summary.records == 9,
        "omitted warning notice is separate and does not invent retained records or merge cached and dynamic samples");
    const auto omitted = std::find_if(summary.modelGroups.begin(), summary.modelGroups.end(), [](const auto& group) {
        return group.code == QStringLiteral("ANIMATION_POSE_WARNINGS_OMITTED");
    });
    Require(omitted != summary.modelGroups.end() && omitted->requests == std::vector<std::uint32_t>{0}
        && omitted->records == 0 && omitted->reason.contains("保留 8 条警告，另有 13 条")
        && omitted->reason.contains("具体节点、轨道和原帧未知"),
        "exact per-instance omitted count remains visible with no fabricated lost record metadata");
    const auto cached = std::find_if(summary.modelGroups.begin(), summary.modelGroups.end(), [](const auto& group) {
        return group.code != QStringLiteral("ANIMATION_POSE_WARNINGS_OMITTED")
            && group.requests == std::vector<std::uint32_t>{0};
    });
    Require(cached != summary.modelGroups.end() && cached->records == 8
        && cached->reason.contains("缓存 Stand 采样") && cached->reason.contains("动作索引 0")
        && cached->reason.contains("0/77 ms") && !cached->reason.contains("9000/24000"),
        "cached preview shows actual stored clock rather than currently requested instance time");
    Require(summary.rows.join('\n').contains("最多保留 8 条")
        && !summary.rows.join('\n').contains("完整原始诊断仍保留")
        && SameWarning(fixture.frame->poses[0].warnings, frozen)
        && fixture.frame->poses[0].warnings.records[0].objectId == original.records[0].objectId,
        "bounded warning capture is disclosed and raw original record stays untouched");
    ++fixture.frame->poses[0].staticGlobalMs;
    const auto stale = diagnostics.Summarize(fixture.scene, *fixture.frame);
    Require(std::none_of(stale.modelGroups.begin(), stale.modelGroups.end(), [](const auto& group) {
        return group.stage == QStringLiteral("兼容预览警告") && group.requests == std::vector<std::uint32_t>{0};
    }), "stale cached clock is rejected without borrowing requested dynamic clock");
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        RepeatedPlacementsCollapseWithoutLosingRawRecords();
        AmbiguousSourcesStaySeparateAndSceneChangesInvalidate();
        ComponentsOffsetsAndBudgetStatesDoNotCollapse();
        SuccessfulWarningsKeepSampledOwnerAndClock();
        InvalidWarningOwnersAreNeverAssignedToPlacements();
        CachedClockAndOmittedWarningCountStayExplicit();
        std::cout << "animation diagnostic aggregation checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
