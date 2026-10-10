#include "War3MapCore/RenderPlugin.h"

#include <new>

namespace {

struct StubRenderer {
    std::uint64_t revision = 0;
    std::uint64_t lastPayloadSize = 0;
    bool windowAttached = false;
};

constexpr std::int32_t kOk = 0;
constexpr std::int32_t kInvalidArgument = 1;

} // namespace

extern "C" {

War3RendererHandle WAR3_RENDER_CALL CreateStubRenderer() {
    return new (std::nothrow) StubRenderer();
}

void WAR3_RENDER_CALL DestroyStubRenderer(War3RendererHandle renderer) {
    delete static_cast<StubRenderer*>(renderer);
}

std::int32_t WAR3_RENDER_CALL SubmitStubSceneSnapshot(
    War3RendererHandle renderer,
    const War3SceneSnapshotV1* snapshot) {
    auto* state = static_cast<StubRenderer*>(renderer);
    if (state == nullptr || snapshot == nullptr || snapshot->structSize != sizeof(War3SceneSnapshotV1)) return kInvalidArgument;
    if (snapshot->payload == nullptr && snapshot->payloadSize != 0) return kInvalidArgument;
    state->revision = snapshot->documentRevision;
    state->lastPayloadSize = snapshot->payloadSize;
    return kOk;
}

std::int32_t WAR3_RENDER_CALL AttachStubNativeWindow(
    War3RendererHandle renderer,
    void* nativeWindow) {
    auto* state = static_cast<StubRenderer*>(renderer);
    if (state == nullptr || nativeWindow == nullptr) return kInvalidArgument;
    state->windowAttached = true;
    return kOk;
}

void WAR3_RENDER_CALL StubDocumentRevisionChanged(
    War3RendererHandle renderer,
    std::uint64_t revision) {
    if (auto* state = static_cast<StubRenderer*>(renderer)) state->revision = revision;
}

const War3RenderPluginApiV1 kStubApi{
    sizeof(War3RenderPluginApiV1),
    WAR3_RENDER_PLUGIN_API_VERSION,
    &CreateStubRenderer,
    &DestroyStubRenderer,
    &SubmitStubSceneSnapshot,
    &AttachStubNativeWindow,
    &StubDocumentRevisionChanged,
};

const War3RenderPluginApiV1* WAR3_RENDER_CALL
War3MapEditor_GetRenderPluginApi(std::uint32_t requestedApiVersion) {
    return requestedApiVersion == WAR3_RENDER_PLUGIN_API_VERSION ? &kStubApi : nullptr;
}

}