#pragma once
#include "War3MapCore/RenderPluginEditorVisibility.h"
#include "War3MapCore/RenderPluginFrameMetrics.h"
#include <algorithm>
#include <deque>

// UI 接线夹具：只保存已提交/已 Draw 的身份，不实现绘制或冒充真实 GPU 像素。
struct EditorPresentationState {
    const War3RenderPluginApiV2* api = nullptr;
    War3RenderEditorPresentationTargetV2 target{sizeof(target), WAR3_EDITOR_PRESENTATION_SKINNED, 0, 0, 1, 0, 0};
    War3RenderEditorPresentationStatusV2 presentation{sizeof(presentation)};
    War3RenderEditorCompletedFrameV2 completed{sizeof(completed)};
    std::deque<War3RenderFrameMetricsV2> metrics; // 仅保留夹具真正Draw的记录，提交或pending不会伪造完成帧。
    bool pending = true;
};
template<class State> struct EditorPresentationFixture {
    static EditorPresentationState& Get(War3RendererHandle h) { return static_cast<State*>(h)->editor; }
    static bool Matches(const War3RenderEditorPresentationTargetV2& a, const War3RenderEditorPresentationTargetV2& b) {
        return a.modelEpoch == b.modelEpoch && a.mode == b.mode && a.instanceCount == b.instanceCount
            && a.particleDrawCount == b.particleDrawCount && a.sceneVersion == b.sceneVersion && a.contentVersion == b.contentVersion;
    }
    static void Publish(EditorPresentationState& s, const War3RenderEditorVisibilityUpdateV2& mask) {
        const auto count = [](const std::uint8_t* p, std::uint32_t n) {
            return p ? static_cast<std::uint32_t>(std::count(p, p + n, std::uint8_t{1})) : 0u;
        };
        s.presentation = {sizeof(s.presentation), s.target.mode, mask.instanceCount, mask.particleDrawCount,
            count(mask.instanceVisible, mask.instanceCount), count(mask.particleDrawVisible, mask.particleDrawCount), 0, 0,
            s.target.modelEpoch, s.target.sceneVersion, s.target.contentVersion, mask.presentationRevision};
        s.pending = true;
    }
    static void Draw(War3RendererHandle h) {
        auto& s = Get(h);
        if (!s.api || s.api->getViewInfo(h, &s.completed.view) || s.api->getSkinStatus(h, &s.completed.skin)) return;
        s.completed.camera = static_cast<State*>(h)->camera;
        s.completed.presentation = s.presentation; s.pending = false;
        War3RenderFrameMetricsV2 metric{}; metric.structSize = sizeof(metric);
        metric.flags = WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID;
        metric.completed = s.completed;
        metric.modelInstanceCount = s.presentation.instanceCount;
        metric.visibleModelInstanceCount = s.presentation.visibleInstanceCount;
        metric.particleDrawCount = s.presentation.particleDrawCount;
        metric.visibleParticleDrawCount = s.presentation.visibleParticleDrawCount;
        if (!s.metrics.empty() && s.metrics.back().completed.view.frameCount == s.completed.view.frameCount)
            s.metrics.back() = metric;
        else s.metrics.push_back(metric);
        if (s.metrics.size() > 4) s.metrics.pop_front();
    }
    static std::int32_t WAR3_RENDER_CALL Target(War3RendererHandle h, War3RenderEditorPresentationTargetV2* out) {
        *out = Get(h).target; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Submit(War3RendererHandle h, const War3RenderEditorModelSceneV2* in) {
        auto& s = Get(h);
        if (!in || !in->visibility || !Matches(in->visibility->expected, s.target)) return 1;
        const auto* pose = in->models ? in->models->initialPose : in->particles ? in->particles->models->initialPose : nullptr;
        if (in->staticModels ? s.api->submitModelScene(h, in->staticModels)
            : !pose || (in->particles ? s.api->submitParticleModelScene(h, in->particles)
                : s.api->submitSkinnedModelScene(h, in->models))) return 1;
        ++s.target.modelEpoch; s.target.mode = in->staticModels ? WAR3_EDITOR_PRESENTATION_STATIC
            : in->particles ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED;
        s.target.sceneVersion = pose ? pose->sceneVersion : 0; s.target.contentVersion = pose ? pose->contentVersion : 0;
        s.target.instanceCount = in->visibility->instanceCount; s.target.particleDrawCount = in->visibility->particleDrawCount;
        Publish(s, *in->visibility); return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Update(War3RendererHandle h, const War3RenderEditorVisibilityUpdateV2* mask) {
        auto& s = Get(h);
        if (!mask || !Matches(mask->expected, s.target) || mask->presentationRevision <= s.presentation.presentationRevision) return 1;
        if (mask->particleFrame) {
            if (s.api->updateParticleModelFrame(h, mask->particleFrame)) return 1;
            s.target.contentVersion = mask->particleFrame->contentVersion;
        } else if (mask->modelPose) {
            if (s.api->updateSkinMatrices(h, mask->modelPose)) return 1;
            s.target.contentVersion = mask->modelPose->contentVersion;
        }
        s.target.particleDrawCount = mask->particleDrawCount; Publish(s, *mask); return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Completed(War3RendererHandle h, War3RenderEditorCompletedFrameV2* out) {
        auto& s = Get(h); War3RenderViewInfoV2 view{sizeof(view)};
        if (s.pending || s.api->getViewInfo(h, &view) || view.frameCount != s.completed.view.frameCount
            || view.surfaceEpoch != s.completed.view.surfaceEpoch || view.sceneEpoch != s.completed.view.sceneEpoch
            || view.viewEpoch != s.completed.view.viewEpoch) return 2;
        *out = s.completed; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Metrics(War3RendererHandle h, std::uint64_t after,
        War3RenderFrameMetricsV2* records, std::uint32_t capacity, War3RenderFrameMetricsPageV2* out) {
        if (!out || out->structSize < sizeof(*out) || capacity > WAR3_RENDER_FRAME_METRICS_PAGE_LIMIT || (capacity && !records)) return 1;
        const auto& s = Get(h); War3RenderFrameMetricsPageV2 page{};
        page.structSize = sizeof(page); page.capacity = 4;
        if (!s.metrics.empty()) {
            page.oldestFrame = s.metrics.front().completed.view.frameCount; page.latestFrame = s.metrics.back().completed.view.frameCount;
            if (page.oldestFrame > after) page.lostBeforeCount = page.oldestFrame - after - 1;
            for (const auto& metric : s.metrics)
                if (metric.completed.view.frameCount > after && page.count < capacity) records[page.count++] = metric;
        }
        *out = page; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Capture(War3RendererHandle h, std::uint64_t epoch, std::uint64_t revision,
        void* rgba, std::uint64_t bytes, War3RenderEditorCapturedFrameV2* out) {
        auto& s = Get(h);
        if (epoch != s.target.modelEpoch || revision != s.presentation.presentationRevision) return 2;
        if (s.target.mode == WAR3_EDITOR_PRESENTATION_PARTICLES) {
            War3RenderParticleCapturedFrameV2 captured{sizeof(captured)};
            if (const auto code = s.api->captureParticleFrameRgba(h, rgba, bytes, &captured)) return code;
            out->frame = captured.models.frame; out->skin = captured.models.skin; out->particles = captured.particles;
        } else {
            War3RenderSkinnedCapturedFrameV2 captured{sizeof(captured)};
            if (const auto code = s.api->captureSkinnedFrameRgba(h, rgba, bytes, &captured)) return code;
            out->frame = captured.frame; out->skin = captured.skin;
        }
        out->structSize = sizeof(*out); out->presentation = s.presentation; Draw(h); return 0;
    }
    static void Enable(War3RenderPluginApiV2& api, EditorPresentationState& state) {
        state.api = &api; api.getEditorPresentationTarget = Target; api.submitEditorVisibility = Update;
        api.submitEditorModelScene = Submit; api.captureEditorPresentationFrameRgba = Capture;
        api.getEditorCompletedFrame = Completed;
        api.getFrameMetrics = Metrics;
    }
};
