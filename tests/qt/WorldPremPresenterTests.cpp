#include "WorldPremPresenter.h"
#include "War3MapCore/Hash.h"
#include "../AnimationTestModel.h"

#include <QJsonArray>
#include <QTest>

#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace War3Map;
using namespace War3Map::Modules;
using namespace War3Map::Render;

namespace {
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty()
        ? "original PREM fixture failed without a diagnostic" : result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    return std::move(*result.value);
}
QString Code(const Result<QJsonObject>& result) {
    return result.diagnostics.empty() ? QString{} : QString::fromStdString(result.diagnostics.front().code);
}

struct Fixture {
    static constexpr std::uint64_t ViewportRevision = 91;
    std::shared_ptr<ModelAnimationService> service;
    std::shared_ptr<const AnimatedModelScene> scene;
    std::shared_ptr<const AnimationFrame> frame;

    explicit Fixture(bool baked = false, bool enablePrem = true) {
        const auto bytes = AnimationTest::Bytes();
        const auto digest = Take(Sha256(bytes));
        const AnimationResourceReader read = [bytes, digest](std::string_view path, std::uint64_t maximum) {
            if (path == "OriginalEmptyPrem.mdx" && bytes.size() <= maximum)
                return Result<AnimationResource>::Success({bytes, "original-test-model", digest});
            return Result<AnimationResource>::Failure("Test", "RESOURCE_NOT_FOUND", "原创夹具没有该资源。");
        };
        std::vector<AnimationInstanceSpec> specs;
        std::vector<BakedModelMesh> geometry;
        if (baked) {
            BakedModelMesh triangle;
            triangle.vertices = {{-10, 0, 0, 0, 0, 1, 0, 0}, {10, 0, 0, 0, 0, 1, 1, 0}, {0, 10, 0, 0, 0, 1, 0, 1}};
            triangle.indices = {0, 1, 2};
            geometry.push_back(std::move(triangle));
        } else {
            AnimationInstanceSpec spec;
            spec.identity = {1, 7}; spec.hostKey = "original-readonly-instance";
            spec.model.modelPath = "OriginalEmptyPrem.mdx"; spec.sequenceIndex = 0; spec.elapsedMs = 250;
            specs.push_back(std::move(spec));
        }
        AnimationLimits limits; limits.enablePremPreview = enablePrem;
        service = Take(ModelAnimationService::Create({7, 0, "original-empty-prem-source"}, specs, read, limits, geometry));
        scene = service->Scene(); frame = Take(service->Prepare(nullptr));
    }

    // 原创 GPU 合同回执夹具，不调用渲染器、不生成像素、不宣称真实游戏或 GPU 验收。
    // CPU 场景、姿势与 PREM 计划由正式字节解析和 ModelAnimationService 生成。
    CapturedFrameRgba Receipt() const {
        CapturedFrameRgba capture;
        capture.documentRevision = ViewportRevision; capture.animation = frame;
        capture.view.identity.surfaceEpoch = 11; capture.view.identity.sceneEpoch = 12;
        capture.view.identity.viewEpoch = 13; capture.view.identity.frameCount = 14;
        War3RenderSkinStatusV2 skin{};
        skin.structSize = sizeof(skin); skin.active = 1; skin.frameCount = capture.view.identity.frameCount;
        skin.sceneVersion = frame->sceneVersion; skin.contentVersion = frame->contentVersion;
        skin.staticUploadCount = 1; skin.matrixUploadCount = 1; capture.skin = skin;
        War3RenderEditorPresentationStatusV2 presentation{};
        presentation.structSize = sizeof(presentation); presentation.mode = WAR3_EDITOR_PRESENTATION_SKINNED;
        presentation.instanceCount = static_cast<std::uint32_t>(frame->instances.size());
        presentation.modelEpoch = 15; presentation.presentationRevision = 16;
        presentation.sceneVersion = frame->sceneVersion; presentation.contentVersion = frame->contentVersion;
        capture.presentation = presentation; capture.modelLayersRevision = 17;
        return capture;
    }
    Result<QJsonObject> Capture(const CapturedFrameRgba& capture) const {
        return WorldPremPresenter::CaptureEmptySnapshot(capture, scene, frame, ViewportRevision);
    }
};
}

class WorldPremPresenterTests final : public QObject {
    Q_OBJECT
private slots:
    void PreparedEmptyPlan_data() {
        QTest::addColumn<bool>("baked");
        QTest::newRow("animated-parent-without-prem") << false;
        QTest::newRow("baked-static-geometry-only") << true;
    }
    void PreparedEmptyPlan() {
        QFETCH(bool, baked);
        Fixture fixture(baked);
        const auto& frame = *fixture.frame;
        QVERIFY(frame.prem); QVERIFY(frame.prem->capabilities); QVERIFY(frame.prem->capabilities->empty());
        QVERIFY(fixture.scene->premEnabled); QVERIFY(frame.prem->parentEvaluationEpoch);
        QVERIFY(!frame.prem->childScene); QVERIFY(!frame.prem->childFrame); QVERIFY(!fixture.scene->premGeometry);
        QVERIFY(frame.prem->childSlots.empty()); QVERIFY(frame.prem->emission.children.empty());
        QVERIFY(frame.prem->storageBytes); QVERIFY(frame.prem->workingBytes);
        QCOMPARE(frame.prem->budget.birthEvaluatorBytes, std::uint64_t(0));
        QCOMPARE(frame.prem->budget.childEvaluatorBytes, std::uint64_t(0));
        QCOMPARE(frame.prem->budget.childSceneBytes, std::uint64_t(0));
        if (baked) {
            QVERIFY(frame.states.empty()); QCOMPARE(frame.instances.size(), std::size_t(1));
            QCOMPARE(fixture.scene->retainedStaticInstances, std::size_t(1));
        }
        const auto captured = fixture.Capture(fixture.Receipt());
        QVERIFY2(captured.Succeeded(), qPrintable(Code(captured)));
        const auto& output = *captured.value;
        QVERIFY(output["cpuPlanProvesEmptySlots"].toBool()); QVERIFY(output["gpuLayerAbsenceProven"].toBool());
        QVERIFY(!output["gpuLayerCompletionProven"].toBool()); QVERIFY(!output["visibleAcceptance"].toBool());
        QVERIFY(!output["selectionAcceptance"].toBool()); QVERIFY(!output["gameplayVerified"].toBool());
        QCOMPARE(output["absenceReason"].toString(), QStringLiteral("no-filled-slot-for-captured-parent"));
        QCOMPARE(output["parentDocumentRevision"].toString(), QStringLiteral("0"));
        QCOMPARE(output["parentContentVersion"].toString(), QString::number(frame.contentVersion));
        QCOMPARE(output["parentSourceIdentity"].toString(), QString::fromStdString(fixture.scene->sourceIdentity));
        QCOMPARE(output["capabilityRecordCount"].toString(), QStringLiteral("0"));
        QVERIFY(output["capabilities"].isArray()); QVERIFY(output["sampleErrors"].isArray()); QVERIFY(output["children"].isArray());
        QVERIFY(output["capabilities"].toArray().isEmpty()); QVERIFY(output["sampleErrors"].toArray().isEmpty());

        const auto next = Take(fixture.service->Prepare(fixture.frame.get(), AnimationAdvance{}));
        QVERIFY(next->prem); QCOMPARE(next->prem->parentContentVersion, next->contentVersion);
        QCOMPARE(next->prem->parentEvaluationEpoch, frame.prem->parentEvaluationEpoch);
        QVERIFY(next->prem->capabilities == frame.prem->capabilities);
    }
    void DisabledIsNotEmpty() {
        Fixture fixture(false, false);
        QVERIFY(!fixture.scene->premEnabled); QVERIFY(!fixture.frame->prem);
        const auto result = fixture.Capture(fixture.Receipt());
        QVERIFY(!result.Succeeded()); QCOMPARE(Code(result), QStringLiteral("WORLD_PREM_CAPTURE_PLAN_MISSING"));
    }
    void ParentAndPlanIdentityCannotBeBorrowed() {
        Fixture fixture;
        QVERIFY(fixture.frame->prem); QVERIFY(!fixture.frame->poses.empty());
        QVERIFY(fixture.frame->poses.front().evaluationEpoch);
        using Edit = std::function<void(AnimatedModelScene&, AnimationFrame&, AnimationPremFrame&)>;
        struct Row { const char* name; const char* expected; Edit edit; };
        const Row rows[] = {
            {"scene-version-missing", "WORLD_PREM_CAPTURE_CPU_BINDING_INVALID", [](auto& s, auto&, auto&) { s.sceneVersion = 0; }},
            {"scene-generation-missing", "WORLD_PREM_CAPTURE_CPU_BINDING_INVALID", [](auto& s, auto&, auto&) { s.generation = 0; }},
            {"scene-source-missing", "WORLD_PREM_CAPTURE_CPU_BINDING_INVALID", [](auto& s, auto&, auto&) { s.sourceIdentity.clear(); }},
            {"frame-scene-stale", "WORLD_PREM_CAPTURE_CPU_BINDING_INVALID", [](auto&, auto& f, auto&) { ++f.sceneVersion; }},
            {"frame-document-stale", "WORLD_PREM_CAPTURE_CPU_BINDING_INVALID", [](auto&, auto& f, auto&) { ++f.documentRevision; }},
            {"frame-content-missing", "WORLD_PREM_CAPTURE_CPU_BINDING_INVALID", [](auto&, auto& f, auto&) { f.contentVersion = 0; }},
            {"frame-states-incomplete", "WORLD_PREM_CAPTURE_CPU_BINDING_INVALID", [](auto&, auto& f, auto&) { f.states.clear(); }},
            {"prem-disabled", "WORLD_PREM_CAPTURE_PLAN_MISSING", [](auto& s, auto&, auto&) { s.premEnabled = false; }},
            {"no-packet", "WORLD_PREM_CAPTURE_PLAN_MISSING", [](auto&, auto& f, auto&) { f.prem.reset(); }},
            {"no-capability-inventory", "WORLD_PREM_CAPTURE_PLAN_MISSING", [](auto&, auto&, auto& p) { p.capabilities.reset(); }},
            {"no-evaluation-epoch", "WORLD_PREM_CAPTURE_PLAN_MISSING", [](auto&, auto&, auto& p) { p.parentEvaluationEpoch = 0; }},
            {"packet-scene-stale", "WORLD_PREM_PARENT_BINDING_INVALID", [](auto&, auto&, auto& p) { ++p.parentSceneVersion; }},
            {"packet-content-stale", "WORLD_PREM_PARENT_BINDING_INVALID", [](auto&, auto&, auto& p) { ++p.parentContentVersion; }},
            {"packet-document-stale", "WORLD_PREM_PARENT_BINDING_INVALID", [](auto&, auto&, auto& p) { ++p.parentDocumentRevision; }},
            {"packet-generation-stale", "WORLD_PREM_PARENT_BINDING_INVALID", [](auto&, auto&, auto& p) { ++p.parentGeneration; }},
            {"packet-source-stale", "WORLD_PREM_PARENT_BINDING_INVALID", [](auto&, auto&, auto& p) { p.parentSourceIdentity += "-old"; }},
            {"packet-epoch-stale", "WORLD_PREM_PARENT_EPOCH_INVALID", [](auto&, auto&, auto& p) { ++p.parentEvaluationEpoch; }},
            {"slot-needs-full-presenter", "WORLD_PREM_CAPTURE_SLOTS_PRESENT", [](auto&, auto&, auto& p) { p.childSlots.push_back({}); }},
            {"child-plan-stale", "WORLD_PREM_CAPTURE_CHILD_PLAN_STALE", [](auto& s, auto&, auto& p) {
                p.childScene = std::make_shared<AnimatedModelScene>(s);
            }}
        };
        for (const auto& row : rows) {
            auto scene = std::make_shared<AnimatedModelScene>(*fixture.scene);
            auto frame = std::make_shared<AnimationFrame>(*fixture.frame);
            auto plan = std::make_shared<AnimationPremFrame>(*frame->prem); frame->prem = plan;
            row.edit(*scene, *frame, *plan);
            auto capture = fixture.Receipt(); capture.animation = frame;
            const auto result = WorldPremPresenter::CaptureEmptySnapshot(capture, scene, frame, Fixture::ViewportRevision);
            QVERIFY2(!result.Succeeded(), row.name);
            const auto context = QByteArray(row.name) + ": " + Code(result).toUtf8();
            QVERIFY2(Code(result) == QString::fromUtf8(row.expected), context.constData());
        }
    }
    void CompletedFrameChecksStayStrict() {
        Fixture fixture;
        using Edit = std::function<void(CapturedFrameRgba&)>;
        struct Row { const char* name; const char* expected; Edit edit; };
        const Row rows[] = {
            {"different-parent-object", "WORLD_PREM_CAPTURE_PARENT_STALE", [](auto& c) { c.animation = std::make_shared<AnimationFrame>(*c.animation); }},
            {"viewport-revision-stale", "WORLD_PREM_CAPTURE_PARENT_STALE", [](auto& c) { c.documentRevision = 0; }},
            {"missing-layer-revision", "WORLD_PREM_CAPTURE_LAYER_LIST_INVALID", [](auto& c) { c.modelLayersRevision = 0; }},
            {"incomplete-layer-frame-list", "WORLD_PREM_CAPTURE_LAYER_LIST_INVALID", [](auto& c) { c.modelLayerFrames.push_back(c.animation); }},
            {"missing-completed-frame", "WORLD_PREM_CAPTURE_RECEIPT_MISSING", [](auto& c) { c.view.identity.frameCount = 0; }},
            {"missing-surface", "WORLD_PREM_CAPTURE_RECEIPT_MISSING", [](auto& c) { c.view.identity.surfaceEpoch = 0; }},
            {"missing-view", "WORLD_PREM_CAPTURE_RECEIPT_MISSING", [](auto& c) { c.view.identity.viewEpoch = 0; }},
            {"missing-scene", "WORLD_PREM_CAPTURE_RECEIPT_MISSING", [](auto& c) { c.view.identity.sceneEpoch = 0; }},
            {"missing-skin", "WORLD_PREM_CAPTURE_RECEIPT_MISSING", [](auto& c) { c.skin.reset(); }},
            {"missing-presentation", "WORLD_PREM_CAPTURE_RECEIPT_MISSING", [](auto& c) { c.presentation.reset(); }},
            {"skin-struct-size", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { c.skin->structSize = 0; }},
            {"skin-error", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { c.skin->result = 2; }},
            {"skin-inactive", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { c.skin->active = 0; }},
            {"skin-reserved", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { c.skin->reserved = 1; }},
            {"skin-frame-stale", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { ++c.skin->frameCount; }},
            {"skin-scene-stale", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { ++c.skin->sceneVersion; }},
            {"skin-content-stale", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { ++c.skin->contentVersion; }},
            {"no-static-upload", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { c.skin->staticUploadCount = 0; }},
            {"no-matrix-upload", "WORLD_PREM_CAPTURE_SKIN_INVALID", [](auto& c) { c.skin->matrixUploadCount = 0; }},
            {"presentation-struct-size", "WORLD_PREM_CAPTURE_PRESENTATION_INVALID", [](auto& c) { c.presentation->structSize = 0; }},
            {"presentation-reserved", "WORLD_PREM_CAPTURE_PRESENTATION_INVALID", [](auto& c) { c.presentation->reserved = 1; }},
            {"presentation-no-model", "WORLD_PREM_CAPTURE_PRESENTATION_INVALID", [](auto& c) { c.presentation->modelEpoch = 0; }},
            {"presentation-no-revision", "WORLD_PREM_CAPTURE_PRESENTATION_INVALID", [](auto& c) { c.presentation->presentationRevision = 0; }},
            {"presentation-scene-stale", "WORLD_PREM_CAPTURE_PRESENTATION_INVALID", [](auto& c) { ++c.presentation->sceneVersion; }},
            {"presentation-content-stale", "WORLD_PREM_CAPTURE_PRESENTATION_INVALID", [](auto& c) { ++c.presentation->contentVersion; }},
            {"presentation-instance-count", "WORLD_PREM_CAPTURE_PRESENTATION_INVALID", [](auto& c) { ++c.presentation->instanceCount; }},
            {"residual-prem-layer", "WORLD_PREM_CAPTURE_RESIDUAL_LAYER", [](auto& c) {
                War3RenderModelLayerStatusV2 layer{}; layer.layerId = WorldPremPresenter::LayerId;
                c.modelLayers.push_back(layer); c.modelLayerFrames.push_back(c.animation);
            }}
        };
        for (const auto& row : rows) {
            auto capture = fixture.Receipt(); row.edit(capture);
            const auto result = fixture.Capture(capture);
            QVERIFY2(!result.Succeeded(), row.name);
            const auto context = QByteArray(row.name) + ": " + Code(result).toUtf8();
            QVERIFY2(Code(result) == QString::fromUtf8(row.expected), context.constData());
        }
    }
    void EmptyPlanDoesNotEraseFailureDiagnostics() {
        Fixture fixture;
        auto frame = std::make_shared<AnimationFrame>(*fixture.frame);
        auto plan = std::make_shared<AnimationPremFrame>(*frame->prem); frame->prem = plan;
        auto capabilities = std::make_shared<std::vector<AnimationPremCapability>>();
        AnimationPremCapability rejected;
        rejected.sourceIndex = 0; rejected.originalPath = "OriginalMissingChild.mdx";
        rejected.rejectionCode = "PREM_CHILD_READ_FAILED"; rejected.rejectionMessage = "原创缺失资源诊断。";
        capabilities->push_back(std::move(rejected)); plan->capabilities = capabilities;
        plan->unsupportedEmitterInstances = 1; plan->requestBudgetOmissions = 2; plan->slotSuppressedChildren = 3;
        plan->emission.suppressedChildren = 4; plan->emission.clockRejectedChildren = 5;
        AnimationPremSampleError error; error.code = "PREM_BIRTH_NODE_STALE"; plan->sampleErrors.push_back(std::move(error));
        auto capture = fixture.Receipt(); capture.animation = frame;
        const auto result = WorldPremPresenter::CaptureEmptySnapshot(capture, fixture.scene, frame, Fixture::ViewportRevision);
        QVERIFY2(result.Succeeded(), qPrintable(Code(result)));
        const auto& output = *result.value;
        QCOMPARE(output["unsupportedEmitterInstances"].toString(), QStringLiteral("1"));
        QCOMPARE(output["requestBudgetOmissions"].toString(), QStringLiteral("2"));
        QCOMPARE(output["slotSuppressedChildren"].toString(), QStringLiteral("3"));
        QCOMPARE(output["suppressedChildren"].toString(), QStringLiteral("7"));
        QCOMPARE(output["clockRejectedChildren"].toString(), QStringLiteral("5"));
        QCOMPARE(output["sampleErrorCount"].toString(), QStringLiteral("1"));
        QCOMPARE(output["capabilities"].toArray().at(0).toObject()["rejectionCode"].toString(), QStringLiteral("PREM_CHILD_READ_FAILED"));
        QCOMPARE(output["sampleErrors"].toArray().at(0).toObject()["code"].toString(), QStringLiteral("PREM_BIRTH_NODE_STALE"));
        QVERIFY(!output["gpuLayerCompletionProven"].toBool()); QVERIFY(!output["gameplayVerified"].toBool());
    }
};

QTEST_GUILESS_MAIN(WorldPremPresenterTests)
#include "WorldPremPresenterTests.moc"
