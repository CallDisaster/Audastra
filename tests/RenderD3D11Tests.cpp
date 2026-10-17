#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/RenderPluginFrameMetrics.h"
#include "War3MapCore/Render/AnimationRenderer.h"
#include "War3MapCore/RenderPluginEditorVisibility.h"
#include "War3MapCore/RenderPluginMaterialAlpha.h"
#include "War3MapCore/Render/ModelSceneLayerRenderer.h"
#include "War3MapCore/Render/BuildingSplatPreview.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Hash.h"
#include "MdxTestModel.h"
#include "AnimationTestModel.h"
#include "RealTerrainPairAudit.h"
#include <Windows.h>
#include <DirectXMath.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct RendererOwner {
    const War3RenderPluginApiV2* api;
    War3RendererHandle handle;
    ~RendererOwner() { api->destroyRenderer(handle); }
};
std::vector<std::uint8_t> Capture(const RendererOwner& renderer, std::uint32_t width, std::uint32_t height,
    std::uint64_t revision = 42) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    War3RenderCaptureInfoV2 receipt{sizeof(War3RenderCaptureInfoV2)};
    Require(renderer.api->captureRgba(renderer.handle, pixels.data(), pixels.size(), &receipt) == 0, "D3D11 readback failed");
    Require(receipt.width == width && receipt.height == height && receipt.frameCount != 0 && receipt.ticket != 0 &&
        receipt.documentRevision == revision, "capture receipt mismatch");
    War3RenderViewInfoV2 view{sizeof(view)};
    Require(renderer.api->getViewInfo(renderer.handle, &view) == 0 && view.width == width && view.height == height &&
        view.documentRevision == revision && view.frameCount == receipt.frameCount && view.surfaceEpoch &&
        view.sceneEpoch && view.viewEpoch, "completed view receipt mismatch");
    Require(std::all_of(std::begin(view.viewProjection), std::end(view.viewProjection),
        [](float value) { return std::isfinite(value); }), "completed view matrix invalid");
    return pixels;
}
War3RenderViewInfoV2 ReadView(const RendererOwner& renderer) {
    War3RenderViewInfoV2 view{sizeof(view)};
    Require(renderer.api->getViewInfo(renderer.handle, &view) == 0, "completed view missing");
    return view;
}
void RequireLatestOrPending(const RendererOwner& renderer, const War3RenderViewInfoV2& old, int changedEpoch) {
    War3RenderViewInfoV2 output{sizeof(output)};
    output.frameCount = 987654;
    const auto result = renderer.api->getViewInfo(renderer.handle, &output);
    Require((result == 2 && output.frameCount == 987654) || (result == 0 &&
        (changedEpoch == 0 ? output.surfaceEpoch > old.surfaceEpoch :
         changedEpoch == 1 ? output.sceneEpoch > old.sceneEpoch : output.viewEpoch > old.viewEpoch)),
        "view query returned stale state or overwrote pending output");
}
void RequireExactMatrix(const War3RenderViewInfoV2& view, const War3RenderCameraV2& camera) {
    using namespace DirectX;
    const auto target = XMVectorSet(camera.targetX, camera.targetY, camera.targetZ, 1);
    const auto eye = XMVectorAdd(target, XMVectorSet(camera.distance * std::cos(camera.pitchRadians) * std::cos(camera.yawRadians),
        camera.distance * std::cos(camera.pitchRadians) * std::sin(camera.yawRadians), camera.distance * std::sin(camera.pitchRadians), 0));
    XMFLOAT4X4 expected;
    XMStoreFloat4x4(&expected, XMMatrixLookAtRH(eye, target, XMVectorSet(0, 0, 1, 0)) *
        XMMatrixPerspectiveFovRH(XM_PIDIV4, static_cast<float>(view.width) / static_cast<float>(view.height), 1, 1000000));
    Require(std::memcmp(&expected, view.viewProjection, sizeof(expected)) == 0, "receipt differs from actual float MVP");
}
std::size_t Colors(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        Require(pixels[i + 3] == 255, "RGBA alpha not opaque");
        if (pixels[i] > 40 || pixels[i + 1] > 40 || pixels[i + 2] > 40) ++count;
    }
    return count;
}
bool HasCaptureFrame(const War3RenderPluginApiV2* api) {
    return api && api->structSize >= offsetof(War3RenderPluginApiV2, captureFrameRgba) + sizeof(api->captureFrameRgba) &&
        api->apiVersion == WAR3_RENDER_PLUGIN_API_V2 &&
        api->captureFrameRgba != nullptr;
}
bool SameView(const War3RenderViewInfoV2& left, const War3RenderViewInfoV2& right) {
    return left.structSize == right.structSize && left.width == right.width && left.height == right.height
        && left.frameCount == right.frameCount && left.documentRevision == right.documentRevision
        && left.surfaceEpoch == right.surfaceEpoch && left.sceneEpoch == right.sceneEpoch && left.viewEpoch == right.viewEpoch
        && std::memcmp(left.viewProjection, right.viewProjection, sizeof(left.viewProjection)) == 0;
}
struct CapturedFrame {
    std::vector<std::uint8_t> pixels;
    War3RenderCapturedFrameV2 receipt{sizeof(War3RenderCapturedFrameV2)};
};
CapturedFrame CaptureFrame(const RendererOwner& renderer, std::uint32_t width, std::uint32_t height,
    std::uint64_t revision, const War3RenderCameraV2& camera) {
    Require(HasCaptureFrame(renderer.api), "atomic capture capability unavailable");
    CapturedFrame output;
    output.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    Require(renderer.api->captureFrameRgba(renderer.handle, output.pixels.data(), output.pixels.size(), &output.receipt) == 0,
        "atomic capture failed without concurrent state changes");
    const auto& receipt = output.receipt;
    Require(receipt.structSize == sizeof(receipt) && receipt.capture.structSize == sizeof(receipt.capture)
        && receipt.view.structSize == sizeof(receipt.view) && receipt.camera.structSize == sizeof(receipt.camera),
        "atomic capture nested struct sizes differ");
    Require(receipt.capture.ticket != 0 && receipt.capture.frameCount != 0
        && receipt.capture.width == width && receipt.capture.height == height && receipt.capture.documentRevision == revision
        && receipt.view.width == receipt.capture.width && receipt.view.height == receipt.capture.height
        && receipt.view.frameCount == receipt.capture.frameCount && receipt.view.documentRevision == receipt.capture.documentRevision
        && receipt.view.surfaceEpoch && receipt.view.sceneEpoch && receipt.view.viewEpoch,
        "atomic capture image and view do not share identity");
    Require(std::memcmp(&receipt.camera, &camera, sizeof(camera)) == 0, "atomic capture camera is not the drawn camera");
    RequireExactMatrix(receipt.view, camera);
    return output;
}

bool HasFrameMetrics(const War3RenderPluginApiV2* api) {
    return api && api->structSize >= offsetof(War3RenderPluginApiV2, getFrameMetrics) + sizeof(api->getFrameMetrics)
        && api->getFrameMetrics != nullptr;
}
void TestFrameMetrics(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    Require(HasFrameMetrics(api), "completed-frame metrics unavailable");
    auto old = *api; old.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, getFrameMetrics));
    Require(!HasFrameMetrics(&old) && !HasFrameMetrics(nullptr), "old prefix advertises missing metrics tail");
    old = *api; old.getFrameMetrics = nullptr;
    Require(!HasFrameMetrics(&old), "null metrics function advertised");
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle != nullptr, "metrics renderer unavailable");
    (void)Capture(renderer, info.width, info.height, 0);
    (void)Capture(renderer, info.width, info.height, 0);
    const auto screenshotView = ReadView(renderer);
    War3RenderFrameMetricsPageV2 page{sizeof(page)};
    std::array<War3RenderFrameMetricsV2, WAR3_RENDER_FRAME_METRICS_PAGE_LIMIT> rows{};
    Require(api->getFrameMetrics(renderer.handle, 0, rows.data(), static_cast<std::uint32_t>(rows.size()), &page) == 0
        && page.count >= 2 && page.latestFrame == screenshotView.frameCount
        && page.oldestFrame != 0 && page.capacity == WAR3_RENDER_FRAME_METRICS_RING_CAPACITY
        && page.software == info.forceWarp && page.featureLevel == D3D_FEATURE_LEVEL_11_0 && page.adapterName[0],
        "metrics metadata is not the actual initialized/completed device");
    for (std::uint32_t i = 0; i < page.count; ++i) {
        const auto& row = rows[i];
        Require(row.structSize == sizeof(row) && row.completed.view.frameCount == page.oldestFrame + i
            && row.completedSteadyNs && row.drawCpuNs && row.uploadCpuNs && !row.presentCpuNs
            && !row.gpuDrawNs && !row.result && !row.presentResult
            && (row.flags & (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID))
                == (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID)
            && !(row.flags & (WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED | WAR3_RENDER_FRAME_METRICS_GPU_TIME_AVAILABLE)),
            "headless metrics mislabeled as visible/GPU execution or lost a completed draw");
        if (i) Require(row.completedSteadyNs >= rows[i - 1].completedSteadyNs, "completion clock regressed");
    }
    const auto& last = rows[page.count - 1];
    Require(SameView(last.completed.view, screenshotView) && last.readbackCpuNs
        && (last.flags & WAR3_RENDER_FRAME_METRICS_READBACK), "metrics and screenshot do not share completed frame");
    War3RenderFrameMetricsV2 one{};
    Require(api->getFrameMetrics(renderer.handle, page.latestFrame - 1, &one, 1, &page) == 0 && page.count == 1
        && one.completed.view.frameCount == screenshotView.frameCount, "exclusive metrics cursor skipped/repeated a frame");
    Require(api->getFrameMetrics(renderer.handle, page.latestFrame, &one, 1, &page) == 0 && page.count == 0
        && page.latestFrame == screenshotView.frameCount, "read-only metrics generated a draw");
    struct FuturePage { War3RenderFrameMetricsPageV2 known; std::array<std::uint8_t, 19> tail; } future{};
    future.known.structSize = sizeof(future); future.tail.fill(0x5c);
    Require(api->getFrameMetrics(renderer.handle, 0, nullptr, 0, &future.known) == 0 && future.known.count == 0
        && future.known.latestFrame == screenshotView.frameCount
        && std::all_of(future.tail.begin(), future.tail.end(), [](auto byte) { return byte == 0x5c; }),
        "metadata-only query changed unknown page tail");
    const auto unchanged = page;
    Require(api->getFrameMetrics(renderer.handle, 0, nullptr, 1, &page) == 1
        && api->getFrameMetrics(renderer.handle, 0, rows.data(), WAR3_RENDER_FRAME_METRICS_PAGE_LIMIT + 1, &page) == 1
        && std::memcmp(&page, &unchanged, sizeof(page)) == 0, "invalid metrics query changed output");
    page.structSize = sizeof(page) - 1;
    Require(api->getFrameMetrics(renderer.handle, 0, rows.data(), 1, &page) == 1, "short metrics page accepted");
    std::cout << "completed-frame metrics: immutable timing/device identity, pagination, passive query and ABI prefix passed\n";
}

constexpr UINT CaptureMutationMessage = WM_APP + 71;
struct CaptureMutation {
    const RendererOwner* renderer;
    std::uint64_t afterFrame;
    War3RenderCameraV2 camera;
    bool observedCompleted = false, applied = false;
};
LRESULT CALLBACK CaptureMutationWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == CaptureMutationMessage) {
        auto& mutation = *reinterpret_cast<CaptureMutation*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        const auto& renderer = *mutation.renderer;
        const auto deadline = GetTickCount64() + 5000;
        // 回调由 CapturePixels 的 sent-message 泵执行，此刻 renderer.mutex 已释放。
        // 使用 headless renderer，GPU 不依赖本 HWND；等读回完成再变相机，确定制造旧截图。
        do {
            War3RenderStatusV2 status{sizeof(status)};
            if (renderer.api->getStatus(renderer.handle, &status) != 0 || status.result != 0) break;
            if (status.frameCount > mutation.afterFrame) { mutation.observedCompleted = true; break; }
            Sleep(1);
        } while (GetTickCount64() < deadline);
        if (mutation.observedCompleted) {
            mutation.applied = renderer.api->setCamera(renderer.handle, &mutation.camera) == 0;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void TestSupersededCapture(const RendererOwner& renderer, War3RenderCameraV2& camera) {
    CaptureMutation mutation{&renderer, ReadView(renderer).frameCount, camera};
    mutation.camera.yawRadians += 0.25f;
    const auto instance = GetModuleHandleW(nullptr);
    constexpr auto className = L"FORGE capture receipt mutation";
    WNDCLASSW descriptor{};
    descriptor.hInstance = instance; descriptor.lpszClassName = className; descriptor.lpfnWndProc = CaptureMutationWindowProc;
    Require(RegisterClassW(&descriptor) != 0, "capture mutation class unavailable");
    struct WindowGuard {
        HWND window; HINSTANCE instance; const wchar_t* name;
        ~WindowGuard() { if (window) DestroyWindow(window); UnregisterClassW(name, instance); }
    } guard{CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr), instance, className};
    Require(guard.window != nullptr, "capture mutation window unavailable");
    SetWindowLongPtrW(guard.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&mutation));
    std::vector<std::uint8_t> pixels(320 * 240 * 4, 0x39);
    War3RenderCapturedFrameV2 receipt{};
    std::memset(&receipt, 0x6d, sizeof(receipt)); receipt.structSize = sizeof(receipt);
    std::array<std::uint8_t, sizeof(receipt)> original;
    std::memcpy(original.data(), &receipt, sizeof(receipt));
    bool sent = false;
    std::thread sender([&] {
        DWORD_PTR ignored = 0;
        sent = SendMessageTimeoutW(guard.window, CaptureMutationMessage, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 6000, &ignored) != 0;
    });
    // MsgWait 可因激活等系统事件提前醒来，不能用其返回值证明 sent message 已排队。
    // GetQueueStatus 不派发；它标记消息为 old 后，截图的 MWMO_INPUTAVAILABLE 仍会处理。
    const auto queueDeadline = GetTickCount64() + 5000;
    bool pendingSend = false;
    do {
        pendingSend = (HIWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) != 0;
        if (pendingSend) break;
        Sleep(1);
    } while (GetTickCount64() < queueDeadline);
    const auto result = pendingSend
        ? renderer.api->captureFrameRgba(renderer.handle, pixels.data(), pixels.size(), &receipt) : -1;
    sender.join(); // 发送有超时，即使设备失败提前返回也不能永久等待消息。
    Require(pendingSend && sent && mutation.observedCompleted && mutation.applied,
        "could not create a completed capture superseded during its wait");
    Require(result == 2, "superseded capture must return pending, not old pixels as current");
    Require(std::all_of(pixels.begin(), pixels.end(), [](auto value) { return value == 0x39; })
        && std::memcmp(original.data(), &receipt, sizeof(receipt)) == 0,
        "pending capture changed caller pixels or receipt");
    camera = mutation.camera;
    (void)CaptureFrame(renderer, 320, 240, 9001, camera);
}

void TestShortReceiptGuardPage(const RendererOwner& renderer, std::vector<std::uint8_t>& pixels) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page = static_cast<std::size_t>(system.dwPageSize);
    struct MemoryGuard {
        void* memory;
        ~MemoryGuard() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
    } allocation{VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)};
    Require(allocation.memory != nullptr, "short receipt guard allocation failed");
    auto* nextPage = static_cast<std::uint8_t*>(allocation.memory) + page;
    DWORD oldProtection = 0;
    Require(VirtualProtect(nextPage, page, PAGE_NOACCESS, &oldProtection) != 0, "short receipt guard protection failed");
    // 保持完整结构所需的对齐；首字段后只有 ABI 对齐字节，首个嵌套结构就在保护页。
    auto* sizeOnly = reinterpret_cast<std::uint32_t*>(nextPage - alignof(War3RenderCapturedFrameV2));
    *sizeOnly = sizeof(std::uint32_t);
    Require(renderer.api->captureFrameRgba(renderer.handle, pixels.data(), pixels.size(),
        reinterpret_cast<War3RenderCapturedFrameV2*>(sizeOnly)) == 1 && *sizeOnly == sizeof(std::uint32_t),
        "size-only receipt should be rejected without reading or writing its absent tail");
    Require(std::all_of(pixels.begin(), pixels.end(), [](auto value) { return value == 0xa5; }),
        "guarded short receipt changed pixels");
}

void TestAtomicCapture(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle && HasCaptureFrame(api), "atomic capture renderer unavailable");
    const War3RenderVertexV2 vertices[]{{-1000,-1000,0,1,0,0},{1000,-1000,0,0,1,0},{0,1000,700,0,0,1}};
    const std::uint32_t indices[]{0,1,2};
    War3RenderMeshV2 mesh{sizeof(mesh),3,3,9001,vertices,indices};
    War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.1f,0.8f};
    Require(api->submitMesh(renderer.handle, &mesh) == 0 && api->setCamera(renderer.handle, &camera) == 0,
        "atomic capture inputs rejected");
    const auto first = CaptureFrame(renderer, 320, 240, 9001, camera);
    Require(Colors(first.pixels) > 1500, "atomic capture did not return actual rendered pixels");
    // 无并发变更时，两接口独立所得 view 可精确对照，但生产回执不靠二次查询拼接。
    const auto completed = ReadView(renderer);
    Require(SameView(completed, first.receipt.view),
        "atomic receipt is not the completed frame's actual view");
    const auto savedFirst = first;

    std::vector<std::uint8_t> untouchedPixels(320 * 240 * 4, 0xa5);
    War3RenderCapturedFrameV2 untouchedReceipt{};
    std::memset(&untouchedReceipt, 0x5a, sizeof(untouchedReceipt));
    untouchedReceipt.structSize = sizeof(untouchedReceipt);
    std::array<std::uint8_t, sizeof(untouchedReceipt)> originalReceipt;
    std::memcpy(originalReceipt.data(), &untouchedReceipt, sizeof(untouchedReceipt));
    const auto reject = [&](War3RenderCapturedFrameV2* receipt, std::uint64_t bytes) {
        Require(api->captureFrameRgba(renderer.handle, untouchedPixels.data(), bytes, receipt) == 1,
            "invalid atomic capture request accepted");
        Require(std::all_of(untouchedPixels.begin(), untouchedPixels.end(), [](auto value) { return value == 0xa5; }),
            "failed capture changed caller pixel buffer");
    };
    reject(&untouchedReceipt, untouchedPixels.size() - 1);
    Require(std::memcmp(&untouchedReceipt, originalReceipt.data(), sizeof(untouchedReceipt)) == 0,
        "small pixel buffer changed receipt");
    untouchedReceipt.structSize = sizeof(untouchedReceipt) - 1;
    std::array<std::uint8_t, sizeof(untouchedReceipt)> shortBefore;
    std::memcpy(shortBefore.data(), &untouchedReceipt, sizeof(untouchedReceipt));
    reject(&untouchedReceipt, untouchedPixels.size());
    Require(std::memcmp(&untouchedReceipt, shortBefore.data(), sizeof(untouchedReceipt)) == 0,
        "short receipt changed known or unknown fields");
    reject(nullptr, untouchedPixels.size());
    TestShortReceiptGuardPage(renderer, untouchedPixels);

    // 新调用方更大的结构可用；插件只写当前定义前缀，未知尾部必须逐字节不动。
    struct ExtendedReceipt { War3RenderCapturedFrameV2 known; std::array<std::uint8_t, 37> future; } extended{};
    static_assert(offsetof(ExtendedReceipt, future) == sizeof(War3RenderCapturedFrameV2));
    extended.known.structSize = sizeof(extended);
    extended.future.fill(0xc7);
    Require(api->captureFrameRgba(renderer.handle, untouchedPixels.data(), untouchedPixels.size(), &extended.known) == 0
        && extended.known.structSize == sizeof(War3RenderCapturedFrameV2)
        && extended.known.capture.ticket > first.receipt.capture.ticket,
        "forward-compatible receipt prefix not filled");
    Require(std::all_of(extended.future.begin(), extended.future.end(), [](auto value) { return value == 0xc7; }),
        "capture overwrote caller's unknown receipt tail");
    Require(untouchedPixels == first.pixels, "legacy-sized and extended receipt captured different static pixels");

    TestSupersededCapture(renderer, camera);
    camera.yawRadians += 0.8f;
    Require(api->setCamera(renderer.handle, &camera) == 0, "atomic capture orbit rejected");
    RequireLatestOrPending(renderer, first.receipt.view, 2);
    const auto rotated = CaptureFrame(renderer, 320, 240, 9001, camera);
    Require(rotated.receipt.view.viewEpoch > first.receipt.view.viewEpoch && rotated.pixels != first.pixels,
        "new camera returned old camera identity/pixels");
    mesh.documentRevision = 9002;
    Require(api->submitMesh(renderer.handle, &mesh) == 0, "atomic capture scene rejected");
    RequireLatestOrPending(renderer, rotated.receipt.view, 1);
    const auto replaced = CaptureFrame(renderer, 320, 240, 9002, camera);
    Require(replaced.receipt.view.sceneEpoch > rotated.receipt.view.sceneEpoch, "new scene returned old frame identity");
    Require(api->resize(renderer.handle, 160, 120) == 0, "atomic capture resize rejected");
    RequireLatestOrPending(renderer, replaced.receipt.view, 0);
    const auto resized = CaptureFrame(renderer, 160, 120, 9002, camera);
    Require(resized.receipt.view.surfaceEpoch > replaced.receipt.view.surfaceEpoch && Colors(resized.pixels) > 300,
        "resized atomic capture returned old surface");
    Require(first.pixels == savedFirst.pixels
        && first.receipt.capture.ticket == savedFirst.receipt.capture.ticket
        && SameView(first.receipt.view, savedFirst.receipt.view)
        && std::memcmp(&first.receipt.camera, &savedFirst.receipt.camera, sizeof(first.receipt.camera)) == 0,
        "new frames mutated an already returned capture snapshot");
    // 保留旧调用路径；两种接口共享递增 ticket，而旧结构不要求新增字段。
    Require(Capture(renderer, 160, 120, 9002) == resized.pixels, "legacy capture no longer preserves pixels");
}

void SaveBmp(const std::filesystem::path& path, const std::vector<std::uint8_t>& pixels, LONG width, LONG height) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
    info.biPlanes = 1; info.biBitCount = 32;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    auto bgra = pixels;
    for (std::size_t at = 0; at < bgra.size(); at += 4) std::swap(bgra[at], bgra[at + 2]);
    stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    Require(stream.good(), "screenshot write failed");
}

void RequireWorldColor(const std::vector<std::uint8_t>& pixels, const War3RenderViewInfoV2& view,
    float x, float y, const std::array<int, 3>& expected, int tolerance = 8) {
    using namespace DirectX;
    XMFLOAT4X4 matrix;
    std::memcpy(&matrix, view.viewProjection, sizeof(matrix));
    XMFLOAT4 clip;
    XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(x, y, 0, 1), XMLoadFloat4x4(&matrix)));
    Require(clip.w > 0, "texture sample lies behind view");
    const int px = static_cast<int>(std::floor((clip.x / clip.w + 1) * 0.5f * static_cast<float>(view.width)));
    const int py = static_cast<int>(std::floor((1 - clip.y / clip.w) * 0.5f * static_cast<float>(view.height)));
    Require(px >= 0 && py >= 0 && px < static_cast<int>(view.width) && py < static_cast<int>(view.height),
        "texture sample outside frame");
    const auto offset = (static_cast<std::size_t>(py) * view.width + static_cast<std::size_t>(px)) * 4;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(static_cast<int>(pixels[offset + channel]) - expected[channel]) > tolerance) {
            std::cerr << "texture sample world=" << x << ',' << y << " channel=" << channel
                << " got=" << static_cast<int>(pixels[offset + channel]) << " expected=" << expected[channel] << '\n';
            throw std::runtime_error("terrain source pixel/UV mismatch");
        }
    }
    Require(pixels[offset + 3] == 255, "terrain base layer must be opaque");
}

std::vector<std::uint8_t> TestTerrain(const RendererOwner& renderer) {
    constexpr std::uint64_t revision = 701;
    const auto before = ReadView(renderer);
    Require(renderer.api->structSize >= offsetof(War3RenderPluginApiV2, submitTerrainScene)
        + sizeof(renderer.api->submitTerrainScene) && renderer.api->submitTerrainScene, "optional terrain ABI missing");
    Require(renderer.api->resize(renderer.handle, 320, 240) == 0, "terrain initial size rejected");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(renderer.api->setCamera(renderer.handle, &camera) == 0, "terrain camera rejected");
    // 原创2x2 RGBA：上红绿、下蓝黄。不同alpha验证只显示底层RGB，不做mask混合。
    std::vector<std::uint8_t> rgba{255,0,0,0, 0,255,0,85, 0,0,255,170, 255,255,0,255};
    const auto originalRgba = rgba;
    War3RenderTextureRgbaV2 texture{sizeof(texture), 2, 2, rgba.size(), rgba.data()};
    std::array<War3RenderTerrainVertexV2, 4> vertices{{
        {-1000,-1000,0,1,1,1,0.25f,0.75f}, {1000,-1000,0,1,1,1,0.75f,0.75f},
        {-1000,1000,0,1,1,1,0.25f,0.25f}, {1000,1000,0,1,1,1,0.75f,0.25f}}};
    const auto originalVertices = vertices;
    std::array<std::uint32_t, 6> indices{0,1,2,1,3,2};
    const auto originalIndices = indices;
    War3RenderTerrainBatchV2 batch{0, 6, 0};
    War3RenderTerrainSceneV2 input{sizeof(input), 4, 6, 1, 1, revision,
        vertices.data(), indices.data(), &texture, &batch};
    Require(renderer.api->submitTerrainScene(renderer.handle, &input) == 0, "terrain base scene rejected");
    RequireLatestOrPending(renderer, before, 1);
    std::fill(rgba.begin(), rgba.end(), std::uint8_t{0}); vertices.fill({}); indices.fill(0); batch.textureIndex = 15;
    texture.width = 0; texture.rgba = nullptr;
    const auto first = Capture(renderer, 320, 240, revision);
    const auto firstView = ReadView(renderer);
    RequireWorldColor(first, firstView, -900, 900, {231,13,12});
    RequireWorldColor(first, firstView, 900, 900, {24,242,1});
    RequireWorldColor(first, firstView, -900, -900, {24,13,230});
    RequireWorldColor(first, firstView, 900, -900, {231,242,12});
    // 朝北俯视（yaw −90°）：世界东北角（绿）必须在画面右上、西北角（红）在左上，画面不能左右镜像。
    const auto pixelAt = [&](int x, int y) { return &first[(static_cast<std::size_t>(y) * 320 + static_cast<std::size_t>(x)) * 4]; };
    Require(pixelAt(234, 46)[1] > 200 && pixelAt(234, 46)[0] < 60, "world east/north must appear at screen right/top");
    Require(pixelAt(85, 46)[0] > 200 && pixelAt(85, 46)[1] < 60, "world west/north must appear at screen left/top");
    Require(Capture(renderer, 320, 240, revision) == first, "caller mutation changed GPU snapshot");

    rgba = originalRgba; vertices = originalVertices; indices = originalIndices; batch = {0,6,0};
    texture = {sizeof(texture),2,2,rgba.size(),rgba.data()}; input.documentRevision = revision + 1;
    const auto reject = [&](War3RenderTerrainSceneV2 invalid) {
        Require(renderer.api->submitTerrainScene(renderer.handle, &invalid) != 0, "invalid terrain scene accepted");
        Require(Capture(renderer, 320, 240, revision) == first, "invalid terrain submission replaced previous pixels");
    };
    auto invalid = input; invalid.vertexCount = 1048577; reject(invalid);
    invalid = input; invalid.textureCount = 17; reject(invalid);
    invalid = input; invalid.batchCount = 4097; reject(invalid);
    invalid = input; invalid.textures = nullptr; reject(invalid);
    texture.width = 2049; reject(input); texture.width = 2;
    texture.rgbaByteCount = std::numeric_limits<std::uint64_t>::max(); reject(input); texture.rgbaByteCount = rgba.size();
    vertices[0].u = std::numeric_limits<float>::quiet_NaN(); reject(input); vertices = originalVertices;
    indices[5] = 4; reject(input); indices = originalIndices;
    batch.textureIndex = 1; reject(input); batch.textureIndex = 0;
    batch.firstIndex = 3; reject(input); batch.firstIndex = 0;
    batch.indexCount = 3; reject(input); batch.indexCount = 5; reject(input); batch.indexCount = 6;
    const War3RenderTerrainBatchV2 overlapping[]{{0,3,0},{0,3,0}};
    invalid = input; invalid.batchCount = 2; invalid.batches = overlapping; reject(invalid);
    // 总纹理字节门应在任何像素拷贝前拒绝，描述符只借用小fixture指针。
    std::array<War3RenderTextureRgbaV2, 5> excessive;
    excessive.fill({sizeof(texture), 2048, 2048, 16ull * 1024 * 1024, rgba.data()});
    invalid = input; invalid.textureCount = static_cast<std::uint32_t>(excessive.size()); invalid.textures = excessive.data(); reject(invalid);

    // 原创4x4 cell atlas，每cell2x2：目标cell红色，邻格绿色，验证半texel内缩避免串格。
    std::vector<std::uint8_t> atlas(8 * 8 * 4);
    for (std::size_t pixel = 0; pixel < 64; ++pixel) { atlas[pixel * 4 + 1] = 255; atlas[pixel * 4 + 3] = 255; }
    for (std::size_t y = 2; y < 4; ++y) for (std::size_t x = 2; x < 4; ++x) {
        atlas[(y * 8 + x) * 4] = 255; atlas[(y * 8 + x) * 4 + 1] = 0;
    }
    texture = {sizeof(texture),8,8,atlas.size(),atlas.data()};
    for (auto& vertex : vertices) {
        vertex.u = vertex.x < 0 ? 2.5f/8 : 3.5f/8;
        vertex.v = vertex.y < 0 ? 3.5f/8 : 2.5f/8;
    }
    Require(renderer.api->submitTerrainScene(renderer.handle, &input) == 0, "original atlas fixture rejected");
    const auto atlasFrame = Capture(renderer, 320, 240, revision + 1);
    const auto atlasView = ReadView(renderer);
    for (const float x : {-950.f,0.f,950.f}) for (const float y : {-950.f,0.f,950.f})
        RequireWorldColor(atlasFrame, atlasView, x, y, {255,0,0}, 1);

    // 先旧高度网格，再材质，检查两条布局/Shader/SRV路径不会互相残留。
    std::array<War3RenderVertexV2,4> height;
    for (std::size_t i = 0; i < height.size(); ++i) {
        const auto& vertex = vertices[i]; height[i] = {vertex.x,vertex.y,vertex.z,0,0,1};
    }
    const War3RenderMeshV2 heightMesh{sizeof(heightMesh),4,6,revision+2,height.data(),indices.data()};
    Require(renderer.api->submitMesh(renderer.handle, &heightMesh) == 0, "terrain to height rejected");
    const auto heightFrame = Capture(renderer,320,240,revision+2);
    RequireWorldColor(heightFrame, ReadView(renderer), 0,0,{0,0,255},1);
    input.documentRevision = revision + 3;
    Require(renderer.api->submitTerrainScene(renderer.handle, &input) == 0, "height to terrain rejected");
    Require(Capture(renderer,320,240,revision+3) == atlasFrame, "height/terrain switching changed source pixels");
    Require(renderer.api->resize(renderer.handle,160,120) == 0, "textured resize rejected");
    const auto resized = Capture(renderer,160,120,revision+3);
    RequireWorldColor(resized, ReadView(renderer),0,0,{255,0,0},1);
    // 新terrain顶点布局的高度色batch也必须工作。
    for (auto& vertex : vertices) { vertex.red=0; vertex.green=0; vertex.blue=1; }
    batch.textureIndex = WAR3_RENDER_HEIGHT_COLOR_TEXTURE; input.documentRevision = revision + 4;
    Require(renderer.api->submitTerrainScene(renderer.handle, &input) == 0, "height-color terrain batch rejected");
    const auto fallback = Capture(renderer,160,120,revision+4);
    RequireWorldColor(fallback,ReadView(renderer),0,0,{0,0,255},1);
    // 贴图批次以顶点颜色（宿主光照明暗）乘贴图：品红 × 红 = 红；高度色批次直接显示品红。
    for (auto& vertex : vertices) { vertex.red=1; vertex.green=0; vertex.blue=1; }
    const War3RenderTerrainBatchV2 mixed[]{{0,3,0},{3,3,WAR3_RENDER_HEIGHT_COLOR_TEXTURE}};
    input.batchCount = 2; input.batches = mixed; input.documentRevision = revision + 5;
    Require(renderer.api->submitTerrainScene(renderer.handle,&input) == 0, "mixed terrain/height batches rejected");
    const auto mixedFrame = Capture(renderer,160,120,revision+5);
    const auto mixedView = ReadView(renderer);
    RequireWorldColor(mixedFrame,mixedView,-500,-500,{255,0,0},1);
    RequireWorldColor(mixedFrame,mixedView,500,500,{255,0,255},1);
    // 过渡层：同坐标第二个 quad。带混合标志时按贴图 alpha 叠在红色基础层上；不带标志时忽略 alpha 覆盖。
    {
        std::vector<std::uint8_t> red(16), blue(16);
        for (std::size_t i = 0; i < 4; ++i) {
            red[i * 4] = 255; red[i * 4 + 3] = 255;
            blue[i * 4 + 2] = 255; blue[i * 4 + 3] = 128;
        }
        const War3RenderTextureRgbaV2 layers[]{{sizeof(texture),2,2,red.size(),red.data()},
            {sizeof(texture),2,2,blue.size(),blue.data()}};
        std::array<War3RenderTerrainVertexV2, 8> stacked{};
        for (std::size_t i = 0; i < 8; ++i) { stacked[i] = originalVertices[i % 4]; stacked[i].red = stacked[i].green = stacked[i].blue = 1; }
        const std::array<std::uint32_t, 12> stackedIndices{0,1,2,1,3,2, 4,5,6,5,7,6};
        War3RenderTerrainBatchV2 layered[]{{0,6,0},{6,6,1u | WAR3_RENDER_TERRAIN_ALPHA_BLEND}};
        War3RenderTerrainSceneV2 blendScene{sizeof(blendScene), 8, 12, 2, 2, revision + 7,
            stacked.data(), stackedIndices.data(), layers, layered};
        Require(renderer.api->submitTerrainScene(renderer.handle, &blendScene) == 0, "alpha-blend overlay rejected");
        const auto blended = Capture(renderer,160,120,revision+7);
        RequireWorldColor(blended, ReadView(renderer), 0, 0, {127,0,128}, 3);
        Require(blended[3] == 255, "overlay must keep target alpha opaque");
        layered[1].textureIndex = 1; blendScene.documentRevision = revision + 8;
        Require(renderer.api->submitTerrainScene(renderer.handle, &blendScene) == 0, "opaque second layer rejected");
        const auto opaque = Capture(renderer,160,120,revision+8);
        RequireWorldColor(opaque, ReadView(renderer), 0, 0, {0,0,255}, 1);
        layered[1].textureIndex = 2u | WAR3_RENDER_TERRAIN_ALPHA_BLEND; blendScene.documentRevision = revision + 9;
        Require(renderer.api->submitTerrainScene(renderer.handle, &blendScene) != 0, "flagged out-of-range texture accepted");
    }
    // 显式截图参数输出原创2x2贴图结果，方便根线程核查通道与方向。
    vertices = originalVertices; batch = {0,6,0};
    texture = {sizeof(texture),2,2,rgba.size(),rgba.data()};
    input.batchCount = 1; input.batches = &batch; input.documentRevision = revision + 10;
    Require(renderer.api->submitTerrainScene(renderer.handle,&input) == 0, "final colored fixture rejected");
    auto screenshot = Capture(renderer,160,120,revision+10);
    std::cout << "terrain base mip0: original 2x2 RGBA, atlas UV, immutable input, invalid preservation, height switch and resize passed\n";
    return screenshot;
}

void TestTerrainMips(const RendererOwner& renderer) {
    Require(renderer.api->structSize >= offsetof(War3RenderPluginApiV2, submitTerrainMipScene)
        + sizeof(renderer.api->submitTerrainMipScene) && renderer.api->submitTerrainMipScene,
        "optional original-mip terrain ABI missing");
    Require(renderer.api->resize(renderer.handle, 320, 240) == 0, "mip viewport rejected");
    War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(renderer.api->setCamera(renderer.handle, &camera) == 0, "mip camera rejected");
    const auto solid = [](std::uint32_t size, std::array<std::uint8_t, 3> color) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4);
        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>(at));
            pixels[at + 3] = 255;
        }
        return pixels;
    };
    // 三个独立原创级：红64²、蓝32²、绿16²。颜色不是从前级缩小得到。
    std::array<std::vector<std::uint8_t>, 3> pixels{solid(64, {255,0,0}), solid(32, {0,0,255}), solid(16, {0,255,0})};
    const auto originals = pixels;
    std::array<War3RenderMipRgbaV2, 3> mips{};
    const auto describe = [&] {
        for (std::size_t i = 0; i < mips.size(); ++i) {
            const auto size = 64u >> i;
            mips[i] = {sizeof(War3RenderMipRgbaV2), size, size, pixels[i].size(), pixels[i].data()};
        }
    };
    describe();
    War3RenderTextureMipChainV2 texture{sizeof(texture), 3, 2, 1, 1, mips.data()};
    std::array<War3RenderTerrainVertexV2, 4> vertices{{
        {-1000,-1000,0,1,1,1,0,1}, {1000,-1000,0,1,1,1,1,1},
        {-1000,1000,0,1,1,1,0,0}, {1000,1000,0,1,1,1,1,0}}};
    const auto originalVertices = vertices;
    const std::array<std::uint32_t, 6> indices{0,1,2,1,3,2};
    const War3RenderTerrainBatchV2 batch{0,6,0};
    War3RenderTerrainMipSceneV2 input{sizeof(input),4,6,1,1,801,vertices.data(),indices.data(),&texture,&batch};
    Require(renderer.api->submitTerrainMipScene(renderer.handle,&input) == 0, "original mip scene rejected");
    auto frame = Capture(renderer,320,240,801);
    RequireWorldColor(frame,ReadView(renderer),0,0,{255,0,0},1);
    camera.distance = 56000;
    Require(renderer.api->setCamera(renderer.handle,&camera) == 0, "minification camera rejected");
    frame = Capture(renderer,320,240,801);
    RequireWorldColor(frame,ReadView(renderer),0,0,{0,255,0},1);
    texture.maxSampleMip = 1; input.documentRevision = 802;
    Require(renderer.api->submitTerrainMipScene(renderer.handle,&input) == 0, "LOD clamp scene rejected");
    // 在调用返回后覆写所有级像素和嵌套描述符，远景仍必须使用原蓝色 mip1。
    for (auto& level : pixels) std::fill(level.begin(),level.end(),std::uint8_t{0});
    mips.fill({}); texture.mips = nullptr;
    const auto blueFrame = Capture(renderer,320,240,802);
    RequireWorldColor(blueFrame,ReadView(renderer),0,0,{0,0,255},1);
    Require(Capture(renderer,320,240,802) == blueFrame, "nested caller mip mutation changed GPU scene");
    pixels = originals; describe(); texture = {sizeof(texture),3,1,1,1,mips.data()};
    input.documentRevision = 803;
    const auto reject = [&](War3RenderTerrainMipSceneV2 invalid) {
        Require(renderer.api->submitTerrainMipScene(renderer.handle,&invalid) != 0, "invalid mip scene accepted");
        Require(Capture(renderer,320,240,802) == blueFrame, "rejected mip input replaced old complete scene");
    };
    auto invalid = input; invalid.structSize = 0; reject(invalid);
    invalid = input; invalid.textures = nullptr; reject(invalid);
    texture.mipCount = 0; reject(input); texture.mipCount = 13; reject(input); texture.mipCount = 3;
    texture.maxSampleMip = 3; reject(input); texture.maxSampleMip = 1;
    texture.mips = nullptr; reject(input); texture.mips = mips.data();
    texture.atlasColumns = 0; reject(input); texture.atlasColumns = 3; reject(input); texture.atlasColumns = 1;
    mips[1].width = 16; reject(input); describe(); // 跳过32级，不能按数组顺序猜测/补齐。
    mips[1].rgba = nullptr; reject(input); describe();
    mips[1].rgbaByteCount = std::numeric_limits<std::uint64_t>::max(); reject(input); describe();
    mips[2].structSize = 0; reject(input); describe(); // 不可采的保留级也必须检查。
    texture.atlasRows = 32; texture.maxSampleMip = 2; reject(input); texture.atlasRows = 1; texture.maxSampleMip = 1;
    std::array<War3RenderMipRgbaV2,2> repeated{{
        {sizeof(War3RenderMipRgbaV2),1,1,4,pixels[0].data()},
        {sizeof(War3RenderMipRgbaV2),1,1,4,pixels[0].data()}}};
    texture.mips = repeated.data(); texture.mipCount = 2; reject(input);
    texture.mips = mips.data(); texture.mipCount = 3;
    // 总链预算在任何像素复制前检查：用小地址作描述，不实际分配80MiB测试载荷。
    const std::array<War3RenderMipRgbaV2,2> large{{
        {sizeof(War3RenderMipRgbaV2),2048,2048,16ull*1024*1024,pixels[0].data()},
        {sizeof(War3RenderMipRgbaV2),1024,1024,4ull*1024*1024,pixels[0].data()}}};
    std::array<War3RenderTextureMipChainV2,4> excessive;
    excessive.fill({sizeof(texture),2,1,1,1,large.data()});
    invalid = input; invalid.textureCount = 4; invalid.textures = excessive.data(); reject(invalid);

    // 4x4 atlas，所有相邻cell为绿；目标(1,1)在mip0蓝、mip1/2红。
    // 将一轴固定在mip0半texel边缘，另一轴导数制造LOD2：无逐级clamp时必串绿。
    std::array<std::vector<std::uint8_t>,3> atlas;
    std::array<War3RenderMipRgbaV2,3> atlasMips{};
    for (std::size_t level = 0; level < atlas.size(); ++level) {
        const auto size = 512u >> level, cell = size / 4;
        atlas[level] = solid(size,{0,255,0});
        for (std::uint32_t y = cell; y < 2 * cell; ++y) for (std::uint32_t x = cell; x < 2 * cell; ++x) {
            const auto at = (static_cast<std::size_t>(y) * size + x) * 4;
            atlas[level][at] = level == 0 ? 0 : 255;
            atlas[level][at + 1] = 0; atlas[level][at + 2] = level == 0 ? 255 : 0;
        }
        atlasMips[level] = {sizeof(War3RenderMipRgbaV2),size,size,atlas[level].size(),atlas[level].data()};
    }
    texture = {sizeof(texture),3,2,4,4,atlasMips.data()};
    camera.distance = 28000;
    Require(renderer.api->setCamera(renderer.handle,&camera) == 0, "atlas minification camera rejected");
    for (std::uint32_t edge = 0; edge < 4; ++edge) {
        vertices = originalVertices;
        for (auto& vertex : vertices) {
            vertex.u = vertex.x < 0 ? 128.5f/512 : 255.5f/512;
            vertex.v = vertex.y < 0 ? 255.5f/512 : 128.5f/512;
            if (edge < 2) vertex.u = edge == 0 ? 128.5f/512 : 255.5f/512;
            else vertex.v = edge == 2 ? 128.5f/512 : 255.5f/512;
        }
        input.documentRevision = 810 + edge;
        Require(renderer.api->submitTerrainMipScene(renderer.handle,&input) == 0, "atlas edge mip scene rejected");
        frame = Capture(renderer,320,240,input.documentRevision);
        const auto view = ReadView(renderer);
        for (const float x : {-700.f,0.f,700.f}) for (const float y : {-700.f,0.f,700.f})
            RequireWorldColor(frame,view,x,y,{255,0,0},1);
    }
    // 非二次幂链必须使用实际逐级尺寸（65x33 -> 32x16），而不是小数尺寸。
    auto npot0 = solid(65,{255,0,0}); npot0.resize(65 * 33 * 4);
    auto npot1 = solid(32,{0,0,255}); npot1.resize(32 * 16 * 4);
    const War3RenderMipRgbaV2 npotMips[]{
        {sizeof(War3RenderMipRgbaV2),65,33,npot0.size(),npot0.data()},
        {sizeof(War3RenderMipRgbaV2),32,16,npot1.size(),npot1.data()}};
    vertices = originalVertices;
    texture = {sizeof(texture),2,1,1,1,npotMips}; input.documentRevision = 819;
    // 原squareworld保留为质量观察；格式门另配平texel密度，避免把新LOD选择误当mip尺寸损坏。
    Require(renderer.api->submitTerrainMipScene(renderer.handle,&input) == 0, "NPOT square observation rejected");
    const auto npotSquare = Capture(renderer,320,240,819);
    const auto npotCenter = (std::size_t{120} * 320 + 160) * 4;
    std::cout << "NPOT squareworld observation RGB=" << int(npotSquare[npotCenter]) << ','
        << int(npotSquare[npotCenter + 1]) << ',' << int(npotSquare[npotCenter + 2]) << '\n';
    for (auto& vertex : vertices) vertex.y *= 33.0f / 65.0f;
    Require(renderer.api->submitTerrainMipScene(renderer.handle,&input) == 0, "NPOT original mip chain rejected");
    frame = Capture(renderer,320,240,819);
    RequireWorldColor(frame,ReadView(renderer),0,0,{0,0,255},1);
    // 返回旧mip0接口，不能残留此前maxLOD或atlas常量。
    vertices = originalVertices;
    const War3RenderTextureRgbaV2 oldTexture{sizeof(oldTexture),64,64,pixels[0].size(),pixels[0].data()};
    const War3RenderTerrainSceneV2 oldScene{sizeof(oldScene),4,6,1,1,820,vertices.data(),indices.data(),&oldTexture,&batch};
    Require(renderer.api->submitTerrainScene(renderer.handle,&oldScene) == 0, "mip to legacy scene switch rejected");
    frame = Capture(renderer,320,240,820);
    RequireWorldColor(frame,ReadView(renderer),0,0,{255,0,0},1);
    std::cout << "original mip chain: LOD0/1/2 colors, clamp, deep copy, rejected preservation and four atlas edges passed\n";
}

void TestTerrainZeroAlphaContribution(const RendererOwner& renderer) {
    Require(renderer.api->resize(renderer.handle, 320, 240) == 0, "zero-alpha viewport rejected");
    War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    const std::array<std::uint8_t, 4> red{255,0,0,255}, blue{0,0,255,128}, green{0,255,0,64};
    // 原创128²/4×4 atlas，目标cell(1,1)为白色alpha0，所有邻格是不透明黄。
    // 第二级不透明白是反例毒值：maxSampleMip0必须完全忽略，放开到1则不能证明零贡献。
    std::vector<std::uint8_t> atlas(128 * 128 * 4), lower(64 * 64 * 4, 255);
    for (std::size_t at = 0; at < atlas.size(); at += 4) {
        atlas[at] = atlas[at + 1] = atlas[at + 3] = 255;
    }
    const auto cellAlpha = [&](std::uint8_t alpha) {
        for (std::uint32_t y = 32; y < 64; ++y) for (std::uint32_t x = 32; x < 64; ++x) {
            const auto at = (static_cast<std::size_t>(y) * 128 + x) * 4;
            atlas[at + 2] = 255; atlas[at + 3] = alpha;
        }
    };
    cellAlpha(0);
    const auto selectedCellZeroAtlas = atlas;
    const War3RenderMipRgbaV2 redMip{sizeof(redMip),1,1,red.size(),red.data()};
    const War3RenderMipRgbaV2 blueMip{sizeof(blueMip),1,1,blue.size(),blue.data()};
    const War3RenderMipRgbaV2 greenMip{sizeof(greenMip),1,1,green.size(),green.data()};
    const std::array<War3RenderMipRgbaV2, 2> atlasMips{{
        {sizeof(War3RenderMipRgbaV2),128,128,atlas.size(),atlas.data()},
        {sizeof(War3RenderMipRgbaV2),64,64,lower.size(),lower.data()}}};
    std::array<War3RenderTextureMipChainV2, 4> textures{{
        {sizeof(War3RenderTextureMipChainV2),1,0,1,1,&redMip},
        {sizeof(War3RenderTextureMipChainV2),2,0,4,4,atlasMips.data()},
        {sizeof(War3RenderTextureMipChainV2),1,0,1,1,&blueMip},
        {sizeof(War3RenderTextureMipChainV2),1,0,1,1,&greenMip}}};
    const std::array<War3RenderTextureRgbaV2, 4> legacy{{
        {sizeof(War3RenderTextureRgbaV2),1,1,red.size(),red.data()},
        {sizeof(War3RenderTextureRgbaV2),128,128,atlas.size(),atlas.data()},
        {sizeof(War3RenderTextureRgbaV2),1,1,blue.size(),blue.data()},
        {sizeof(War3RenderTextureRgbaV2),1,1,green.size(),green.data()}}};
    std::array<War3RenderTerrainVertexV2, 16> vertices{};
    const std::array<float, 4> heights{0,100,50,25};
    for (std::uint32_t quad = 0; quad < 4; ++quad) for (std::uint32_t corner = 0; corner < 4; ++corner)
        vertices[quad * 4 + corner] = {corner % 2 == 0 ? -1000.f : 1000.f, corner < 2 ? -1000.f : 1000.f,
            heights[quad],1,1,1,corner % 2 == 0 ? 0.f : 1.f,corner < 2 ? 1.f : 0.f};
    std::uint64_t revision = 840;
    const auto draw = [&](bool includeZero, bool includeVisible, bool oldApi) {
        std::vector<std::uint32_t> indices;
        std::vector<War3RenderTerrainBatchV2> batches;
        for (std::uint32_t quad = 0; quad < 4; ++quad) {
            if ((quad == 1 && !includeZero) || (quad >= 2 && !includeVisible)) continue;
            const auto sw = quad * 4;
            batches.push_back({static_cast<std::uint32_t>(indices.size()),6,
                quad == 0 ? 0 : quad | WAR3_RENDER_TERRAIN_TRANSLUCENT | WAR3_RENDER_TERRAIN_ALPHA_BLEND});
            for (const auto index : {sw,sw+1,sw+2,sw+1,sw+3,sw+2}) indices.push_back(index);
        }
        ++revision;
        if (oldApi) {
            const War3RenderTerrainSceneV2 scene{sizeof(scene),16,static_cast<std::uint32_t>(indices.size()),4,
                static_cast<std::uint32_t>(batches.size()),revision,vertices.data(),indices.data(),legacy.data(),batches.data()};
            Require(renderer.api->submitTerrainScene(renderer.handle,&scene) == 0, "legacy zero-alpha scene rejected");
        } else {
            const War3RenderTerrainMipSceneV2 scene{sizeof(scene),16,static_cast<std::uint32_t>(indices.size()),4,
                static_cast<std::uint32_t>(batches.size()),revision,vertices.data(),indices.data(),textures.data(),batches.data()};
            Require(renderer.api->submitTerrainMipScene(renderer.handle,&scene) == 0, "mip zero-alpha scene rejected");
        }
        return Capture(renderer,320,240,revision);
    };
    for (const bool oldApi : {false,true}) for (const float distance : {3500.f,56000.f}) {
        // mip ABI 有逐 cell clamp；legacy 只有整 atlas clamp，允许删除必须证明所有邻 cell 也为零。
        if (oldApi) for (std::size_t at = 3; at < atlas.size(); at += 4) atlas[at] = 0;
        else std::copy(selectedCellZeroAtlas.begin(), selectedCellZeroAtlas.end(), atlas.begin());
        camera.distance = distance;
        Require(renderer.api->setCamera(renderer.handle,&camera) == 0, "zero-alpha camera rejected");
        for (std::uint32_t edge = 0; edge < 5; ++edge) {
            for (std::uint32_t corner = 0; corner < 4; ++corner) {
                auto& vertex = vertices[4 + corner];
                vertex.u = corner % 2 == 0 ? 32.5f/128 : 63.5f/128;
                vertex.v = corner < 2 ? 63.5f/128 : 32.5f/128;
                if (edge < 2) vertex.u = edge == 0 ? 32.5f/128 : 63.5f/128;
                else if (edge < 4) vertex.v = edge == 2 ? 32.5f/128 : 63.5f/128;
            }
            const auto withoutZero = draw(false,true,oldApi);
            const auto beforeView = ReadView(renderer);
            Require(Colors(withoutZero) != 0, "zero-alpha baseline must preserve opaque target alpha");
            RequireWorldColor(withoutZero,ReadView(renderer),0,0,{95,64,96},2);
            // 最近的零alpha水面不得写深度；其后的两层保留原序，目标RGBA须逐字节完全相同。
            const auto withZero = draw(true,true,oldApi);
            if (withZero != withoutZero) {
                const auto afterView = ReadView(renderer);
                const auto first = static_cast<std::size_t>(std::mismatch(withoutZero.begin(),withoutZero.end(),withZero.begin()).first-withoutZero.begin());
                std::size_t pixels = 0, alphaPixels = 0; unsigned channels = 0, maximumDelta = 0;
                for (std::size_t at = 0; at < withZero.size(); at += 4) {
                    bool changed = false;
                    for (unsigned c = 0; c < 4; ++c) if (withZero[at+c] != withoutZero[at+c]) {
                        changed = true; channels |= 1u << c;
                        maximumDelta = std::max(maximumDelta,static_cast<unsigned>(std::abs(int(withZero[at+c])-int(withoutZero[at+c]))));
                    }
                    pixels += changed; alphaPixels += withZero[at+3] != withoutZero[at+3];
                }
                std::cerr << "zero-alpha mismatch legacy=" << oldApi << " distance=" << distance << " edge=" << edge
                    << " first=" << first/4%320 << ',' << first/4/320 << " channel=" << first%4
                    << " pixels=" << pixels << " channelMask=" << channels << " alphaPixels=" << alphaPixels << " maxDelta=" << maximumDelta
                    << " revisions=" << beforeView.documentRevision << ',' << afterView.documentRevision
                    << " epochs=" << beforeView.surfaceEpoch << ',' << beforeView.sceneEpoch << ',' << beforeView.viewEpoch
                    << '/' << afterView.surfaceEpoch << ',' << afterView.sceneEpoch << ',' << afterView.viewEpoch
                    << " sameMvp=" << std::equal(std::begin(beforeView.viewProjection),std::end(beforeView.viewProjection),std::begin(afterView.viewProjection))
                    << " expected=";
                for (std::size_t c=0;c<4;++c) std::cerr << unsigned(withoutZero[first/4*4+c]) << ',';
                std::cerr << " actual=";
                for (std::size_t c=0;c<4;++c) std::cerr << unsigned(withZero[first/4*4+c]) << ',';
                std::cerr << '\n';
            }
            Require(withZero == withoutZero, "zero-alpha removal changed RGBA/depth/order or leaked adjacent cell");
        }
    }
    // 保留已暴露 legacy 半 texel 边缘泄漏的旧反例。等值与否只作观测，不能授予 selected-cell-only 剔除许可。
    std::copy(selectedCellZeroAtlas.begin(), selectedCellZeroAtlas.end(), atlas.begin());
    camera.distance = 3500;
    Require(renderer.api->setCamera(renderer.handle,&camera) == 0, "legacy adjacent-cell observation camera rejected");
    for (std::uint32_t corner = 0; corner < 4; ++corner) {
        vertices[4 + corner].u = 32.5f/128;
        vertices[4 + corner].v = corner < 2 ? 63.5f/128 : 32.5f/128;
    }
    const auto legacyWithoutZero = draw(false,true,true);
    const auto legacyWithZero = draw(true,true,true);
    Require(Colors(legacyWithoutZero) != 0 && Colors(legacyWithZero) != 0,
        "legacy adjacent-cell observation must retain opaque target alpha");
    std::cout << "legacy selected-cell-zero observation distance=3500 edge=0 equal="
        << (legacyWithZero == legacyWithoutZero) << "; no cull permission\n";
    for (std::uint32_t corner = 0; corner < 4; ++corner) {
        vertices[4 + corner].u = corner % 2 == 0 ? 32.5f/128 : 63.5f/128;
        vertices[4 + corner].v = corner < 2 ? 63.5f/128 : 32.5f/128;
    }
    camera.distance = 3500;
    Require(renderer.api->setCamera(renderer.handle,&camera) == 0, "one-alpha camera rejected");
    const auto withoutWater = draw(false,false,false);
    cellAlpha(1);
    const auto oneAlpha = draw(true,false,false);
    Require(oneAlpha != withoutWater, "one-byte nonzero alpha was incorrectly equivalent to omitted water");
    RequireWorldColor(oneAlpha,ReadView(renderer),0,0,{255,1,1},1);
    cellAlpha(0); textures[1].maxSampleMip = 1;
    camera.distance = 56000;
    Require(renderer.api->setCamera(renderer.handle,&camera) == 0, "accessible lower mip camera rejected");
    const auto withoutLower = draw(false,false,false);
    const auto withLower = draw(true,false,false);
    Require(withLower != withoutLower, "accessible nonzero lower mip must invalidate a mip0-only zero proof");
    RequireWorldColor(withLower,ReadView(renderer),0,0,{255,255,255},1);
    std::cout << "zero-alpha water: full RGBA equality, read-only depth, retained layer order, mip cell/legacy whole-atlas zero and nonzero controls passed\n";
}

// 上一版V2表的完整布局。新增能力只能出现在这个前缀之后。
struct OldRenderApiV2 {
    std::uint32_t structSize, apiVersion;
    decltype(War3RenderPluginApiV2::createRenderer) createRenderer;
    decltype(War3RenderPluginApiV2::destroyRenderer) destroyRenderer;
    decltype(War3RenderPluginApiV2::attachNativeWindow) attachNativeWindow;
    decltype(War3RenderPluginApiV2::resize) resize;
    decltype(War3RenderPluginApiV2::submitMesh) submitMesh;
    decltype(War3RenderPluginApiV2::setCamera) setCamera;
    decltype(War3RenderPluginApiV2::requestFrame) requestFrame;
    decltype(War3RenderPluginApiV2::getStatus) getStatus;
    decltype(War3RenderPluginApiV2::captureRgba) captureRgba;
    decltype(War3RenderPluginApiV2::getViewInfo) getViewInfo;
};
static_assert(sizeof(OldRenderApiV2) == offsetof(War3RenderPluginApiV2, submitTerrainScene));
struct PreviousTerrainApiV2 {
    OldRenderApiV2 prefix;
    decltype(War3RenderPluginApiV2::submitTerrainScene) submitTerrainScene;
};
static_assert(sizeof(PreviousTerrainApiV2) == offsetof(War3RenderPluginApiV2, submitTerrainMipScene));
struct PreviousMipApiV2 {
    PreviousTerrainApiV2 prefix;
    decltype(War3RenderPluginApiV2::submitTerrainMipScene) submitTerrainMipScene;
};
static_assert(sizeof(PreviousMipApiV2) == offsetof(War3RenderPluginApiV2, captureFrameRgba));
void TestOldPrefix(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    PreviousMipApiV2 mipPrefix{};
    std::memcpy(&mipPrefix, api, sizeof(mipPrefix));
    Require(mipPrefix.submitTerrainMipScene == api->submitTerrainMipScene, "previous mip ABI prefix moved");
    auto noCapture = *api;
    noCapture.structSize = static_cast<std::uint32_t>(sizeof(mipPrefix));
    Require(!HasCaptureFrame(&noCapture), "short provider advertises an absent capture tail");
    noCapture = *api;
    noCapture.captureFrameRgba = nullptr;
    Require(!HasCaptureFrame(&noCapture) && !HasCaptureFrame(nullptr), "missing capture function must be unavailable");
    PreviousTerrainApiV2 previous{};
    std::memcpy(&previous, api, sizeof(previous));
    Require(previous.submitTerrainScene == api->submitTerrainScene &&
        sizeof(previous) < offsetof(War3RenderPluginApiV2,submitTerrainMipScene) + sizeof(api->submitTerrainMipScene),
        "previous terrain ABI prefix moved or accidentally advertises mip tail");
    OldRenderApiV2 old{};
    std::memcpy(&old, api, sizeof(old));
    Require(old.apiVersion == 2 && old.structSize >= sizeof(old), "old API prefix identity changed");
    RendererOwner renderer{api, old.createRenderer(&info)};
    Require(renderer.handle != nullptr, "old prefix create failed");
    const War3RenderVertexV2 vertices[]{{-1000,-1000,0,1,0,0},{1000,-1000,0,1,0,0},{0,1000,0,1,0,0}};
    const std::uint32_t indices[]{0,1,2};
    const War3RenderMeshV2 mesh{sizeof(mesh),3,3,42,vertices,indices};
    Require(old.submitMesh(renderer.handle,&mesh) == 0, "old prefix mesh failed");
    Require(Colors(Capture(renderer,320,240)) > 100, "old prefix height path not drawn");
    auto legacy = *api;
    legacy.structSize = static_cast<std::uint32_t>(sizeof(old)); legacy.submitTerrainScene = nullptr;
    Require(legacy.structSize < offsetof(War3RenderPluginApiV2,submitTerrainScene) + sizeof(legacy.submitTerrainScene),
        "legacy provider must not advertise terrain tail");
}
}
// 模型场景：同一模型两个实例（实例化绘制）+ 另一模型半透明混合层叠在右侧实例上；
// 调用方随后改动数据不影响快照；无效引用保留旧模型；空场景清除模型。
void TestModels(const RendererOwner& renderer) {
    const auto* api = renderer.api;
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, submitModelScene) + sizeof(api->submitModelScene)
        && api->submitModelScene, "optional model ABI missing");
    Require(api->resize(renderer.handle, 320, 240) == 0, "model viewport rejected");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "model camera rejected");
    // 背景高度网格：z=0 的高度色为蓝色。
    War3RenderVertexV2 ground[]{{-1500,-1500,0,0,0,1},{1500,-1500,0,0,0,1},{-1500,1500,0,0,0,1},{1500,1500,0,0,0,1}};
    const std::uint32_t groundIndices[]{0,1,2,1,3,2};
    const War3RenderMeshV2 groundMesh{sizeof(groundMesh), 4, 6, 900, ground, groundIndices};
    Require(api->submitMesh(renderer.handle, &groundMesh) == 0, "model background rejected");

    std::array<War3RenderModelVertexV2, 4> vertices{{
        {-250,-250,5, 0,0,1, 0.25f,0.75f}, {250,-250,5, 0,0,1, 0.75f,0.75f},
        {-250,250,5, 0,0,1, 0.25f,0.25f}, {250,250,5, 0,0,1, 0.75f,0.25f}}};
    std::array<std::uint16_t, 6> indices{0,1,2,1,3,2};
    std::vector<std::uint8_t> red(16), blue(16);
    for (std::size_t i = 0; i < 4; ++i) { red[i * 4] = 255; red[i * 4 + 3] = 255; blue[i * 4 + 2] = 255; blue[i * 4 + 3] = 128; }
    const War3RenderMipRgbaV2 redMip{sizeof(War3RenderMipRgbaV2), 2, 2, red.size(), red.data()};
    const War3RenderMipRgbaV2 blueMip{sizeof(War3RenderMipRgbaV2), 2, 2, blue.size(), blue.data()};
    const War3RenderTextureMipChainV2 textures[]{{sizeof(War3RenderTextureMipChainV2), 1, 0, 1, 1, &redMip},
        {sizeof(War3RenderTextureMipChainV2), 1, 0, 1, 1, &blueMip}};
    std::array<War3RenderModelLayerV2, 2> layers{{{0, 0, 1, 1.0f}, {1, 2, 1, 1.0f}}};
    const std::array<War3RenderModelMeshV2, 2> meshes{{{0, 6, 0, 0, 1}, {0, 6, 0, 1, 1}}};
    const std::array<War3RenderModelV2, 2> models{{{0, 1}, {1, 1}}};
    const auto at = [](std::uint32_t model, float x, float z) {
        return War3RenderModelInstanceV2{model, {1,0,0,x, 0,1,0,0, 0,0,1,z}, {1,1,1}};
    };
    std::array<War3RenderModelInstanceV2, 3> instances{{at(0, -500, 0), at(0, 500, 0), at(1, 500, 10)}};
    War3RenderModelSceneV2 scene{sizeof(scene), 4, 6, 2, 2, 2, 3, 2, vertices.data(), indices.data(), layers.data(),
        meshes.data(), models.data(), instances.data(), textures};
    Require(api->submitModelScene(renderer.handle, &scene) == 0, "model scene rejected");
    vertices.fill({}); // 快照必须独立于调用方缓冲
    const auto frame = Capture(renderer, 320, 240, 900);
    const auto view = ReadView(renderer);
    RequireWorldColor(frame, view, -500, 0, {255, 0, 0}, 1);
    RequireWorldColor(frame, view, 500, 0, {127, 0, 128}, 3);
    RequireWorldColor(frame, view, 0, 0, {0, 0, 255}, 1);
    Require(frame[3] == 255, "model blending must keep target alpha opaque");

    // 无效输入保留上一组模型。
    const auto reject = [&](War3RenderModelSceneV2 invalid) {
        Require(api->submitModelScene(renderer.handle, &invalid) != 0, "invalid model scene accepted");
        Require(Capture(renderer, 320, 240, 900) == frame, "invalid model scene replaced previous models");
    };
    auto invalid = scene;
    std::array<War3RenderModelInstanceV2, 3> unsorted{{at(1, 500, 10), at(0, -500, 0), at(0, 500, 0)}};
    invalid.instances = unsorted.data(); reject(invalid);
    layers[0].textureIndex = 2; reject(scene); layers[0].textureIndex = 0;
    layers[1].filterMode = 7; reject(scene); layers[1].filterMode = 2;
    std::array<std::uint16_t, 6> badIndices{0,1,2,1,3,4};
    invalid = scene; invalid.indices = badIndices.data(); reject(invalid);
    // 地图实例数不再以262144为上限；这个短指针只能用于在读取前必拒的字节预算样例。
    invalid = scene; invalid.instanceCount = std::numeric_limits<std::uint32_t>::max(); reject(invalid);

    // 只更新实例变换：左侧实例移到 x=-1000；数量或模型序号不一致时拒绝并保留。
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, setOverlayLines) + sizeof(api->setOverlayLines)
        && api->updateModelInstances && api->setOverlayLines, "instance/overlay ABI missing");
    auto moved = instances;
    moved[0] = at(0, -1000, 0);
    Require(api->updateModelInstances(renderer.handle, moved.data(), 3) == 0, "instance update rejected");
    moved[0] = at(0, 0, 0); // 必须深拷贝
    auto movedFrame = Capture(renderer, 320, 240, 900);
    auto movedView = ReadView(renderer);
    RequireWorldColor(movedFrame, movedView, -1000, 0, {255, 0, 0}, 1);
    RequireWorldColor(movedFrame, movedView, -500, 0, {0, 0, 255}, 1);
    Require(api->updateModelInstances(renderer.handle, moved.data(), 2) != 0, "instance count change accepted");
    std::array<War3RenderModelInstanceV2, 3> wrongModel{{at(1, -500, 0), at(0, 500, 0), at(1, 500, 10)}};
    Require(api->updateModelInstances(renderer.handle, wrongModel.data(), 3) != 0, "instance model change accepted");
    // 叠加线：z=0 的绿色线带画在 z=5 的模型之上（不做深度测试）。
    std::vector<War3RenderOverlayLineV2> lines;
    for (int i = 0; i <= 60; ++i) {
        const float y = -120.0f + static_cast<float>(i) * 4.0f;
        lines.push_back({{300, y, 0}, {700, y, 0}, {0, 1, 0}});
    }
    Require(api->setOverlayLines(renderer.handle, lines.data(), static_cast<std::uint32_t>(lines.size())) == 0, "overlay rejected");
    lines.assign(lines.size(), {{0, 0, 0}, {0, 0, 0}, {1, 0, 0}}); // 必须深拷贝
    RequireWorldColor(Capture(renderer, 320, 240, 900), ReadView(renderer), 500, 0, {0, 255, 0}, 1);
    const War3RenderOverlayLineV2 badLine{{0, 0, 0}, {1, 1, 1}, {2, 0, 0}};
    Require(api->setOverlayLines(renderer.handle, &badLine, 1) != 0, "overlay color out of range accepted");
    Require(api->setOverlayLines(renderer.handle, nullptr, 0) == 0, "overlay clear rejected");
    Require(Capture(renderer, 320, 240, 900) == movedFrame, "overlay clear must restore the scene");
    // 重新提交模型场景后，实例覆盖失效（顶点缓冲前面已清零，先恢复）。
    vertices = {{{-250,-250,5, 0,0,1, 0.25f,0.75f}, {250,-250,5, 0,0,1, 0.75f,0.75f},
        {-250,250,5, 0,0,1, 0.25f,0.25f}, {250,250,5, 0,0,1, 0.75f,0.25f}}};
    Require(api->submitModelScene(renderer.handle, &scene) == 0, "model resubmit rejected");
    Require(Capture(renderer, 320, 240, 900) == frame, "model resubmit must drop the instance override");

    // 空场景清除模型，只剩背景。
    const War3RenderModelSceneV2 empty{sizeof(empty), 0, 0, 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    Require(api->submitModelScene(renderer.handle, &empty) == 0, "empty model scene rejected");
    const auto cleared = Capture(renderer, 320, 240, 900);
    RequireWorldColor(cleared, ReadView(renderer), -500, 0, {0, 0, 255}, 1);

    // 半透明地形（水面）在不透明模型之后绘制且不写深度：z=100 的半透明蓝色水面下，
    // z=5 的红色模型必须透出（红蓝混合），没有模型处透出绿色地面。
    {
        std::vector<std::uint8_t> green(16), water(16);
        for (std::size_t i = 0; i < 4; ++i) { green[i * 4 + 1] = 255; green[i * 4 + 3] = 255; water[i * 4 + 2] = 255; water[i * 4 + 3] = 128; }
        const War3RenderTextureRgbaV2 terrainTextures[]{{sizeof(War3RenderTextureRgbaV2), 2, 2, green.size(), green.data()},
            {sizeof(War3RenderTextureRgbaV2), 2, 2, water.size(), water.data()}};
        std::array<War3RenderTerrainVertexV2, 8> terrain{};
        for (std::size_t i = 0; i < 8; ++i) {
            terrain[i] = {i % 2 ? 1500.0f : -1500.0f, i % 4 < 2 ? -1500.0f : 1500.0f, i < 4 ? 0.0f : 100.0f, 1, 1, 1,
                i % 2 ? 0.75f : 0.25f, i % 4 < 2 ? 0.75f : 0.25f};
        }
        const std::array<std::uint32_t, 12> terrainIndices{0,1,2,1,3,2, 4,5,6,5,7,6};
        War3RenderTerrainBatchV2 terrainBatches[]{{0,6,0},{6,6,1u | WAR3_RENDER_TERRAIN_TRANSLUCENT}};
        const War3RenderTerrainSceneV2 waterScene{sizeof(waterScene), 8, 12, 2, 2, 901,
            terrain.data(), terrainIndices.data(), terrainTextures, terrainBatches};
        Require(api->submitTerrainScene(renderer.handle, &waterScene) == 0, "translucent terrain batch rejected");
        std::array<War3RenderModelInstanceV2, 1> under{{at(0, -500, 0)}};
        vertices = {{{-250,-250,5, 0,0,1, 0.25f,0.75f}, {250,-250,5, 0,0,1, 0.75f,0.75f},
            {-250,250,5, 0,0,1, 0.25f,0.25f}, {250,250,5, 0,0,1, 0.75f,0.25f}}}; // 前面已清零
        auto submerged = scene;
        submerged.instanceCount = 1; submerged.instances = under.data();
        Require(api->submitModelScene(renderer.handle, &submerged) == 0, "submerged model rejected");
        const auto waterFrame = Capture(renderer, 320, 240, 901);
        const auto waterView = ReadView(renderer);
        RequireWorldColor(waterFrame, waterView, -500, 0, {127, 0, 128}, 3);
        RequireWorldColor(waterFrame, waterView, 500, 0, {0, 127, 128}, 3);
        Require(api->submitModelScene(renderer.handle, &empty) == 0, "model clear after water rejected");
    }
    std::cout << "models: instancing, blend layer, copied snapshot, invalid preservation, clear and underwater order passed\n";
}
// 场景环境：线性雾按视深混向雾色，静态阴影压暗地表，模型光照取环境的环境光/直射光；无效环境保留旧状态。
void TestEnvironment(const RendererOwner& renderer) {
    const auto* api = renderer.api;
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, setEnvironment) + sizeof(api->setEnvironment)
        && api->setEnvironment, "optional environment ABI missing");
    Require(api->resize(renderer.handle, 320, 240) == 0, "environment viewport rejected");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "environment camera rejected");
    std::vector<std::uint8_t> white(16, 255);
    const War3RenderTextureRgbaV2 texture{sizeof(texture), 2, 2, white.size(), white.data()};
    const std::array<War3RenderTerrainVertexV2, 4> ground{{
        {-1500,-1500,0,1,1,1,0.25f,0.75f}, {1500,-1500,0,1,1,1,0.75f,0.75f},
        {-1500,1500,0,1,1,1,0.25f,0.25f}, {1500,1500,0,1,1,1,0.75f,0.25f}}};
    const std::array<std::uint32_t, 6> indices{0,1,2,1,3,2};
    const War3RenderTerrainBatchV2 batch{0,6,0};
    const War3RenderTerrainSceneV2 scene{sizeof(scene), 4, 6, 1, 1, 950, ground.data(), indices.data(), &texture, &batch};
    Require(api->submitTerrainScene(renderer.handle, &scene) == 0, "environment terrain rejected");

    War3RenderEnvironmentV2 environment{sizeof(environment), {0, 0, 1}, {1, 1, 1}, {0, 0, 0}, WAR3_RENDER_FOG_LINEAR,
        1000, 6000, 0, {1, 0, 0}, 0, 0, 0, 0, 0, 0, nullptr};
    Require(api->setEnvironment(renderer.handle, &environment) == 0, "linear fog rejected");
    auto frame = Capture(renderer, 320, 240, 950);
    // 视深 3500：保留一半场景色。
    RequireWorldColor(frame, ReadView(renderer), 500, 0, {255, 128, 128}, 3);
    Require(frame[3] == 255, "fog must keep target alpha opaque");

    // 4×4 阴影图：西半边全阴影，压暗一半；东半边无阴影。
    std::vector<std::uint8_t> shadow(16);
    for (std::size_t row = 0; row < 4; ++row) shadow[row * 4] = shadow[row * 4 + 1] = 255;
    environment.fogMode = WAR3_RENDER_FOG_NONE;
    environment.shadowWidth = environment.shadowHeight = 4;
    environment.shadowOriginX = environment.shadowOriginY = -1500;
    environment.shadowCellSize = 750;
    environment.shadowDarkness = 0.5f;
    environment.shadow = shadow.data();
    Require(api->setEnvironment(renderer.handle, &environment) == 0, "static shadow rejected");
    shadow.assign(shadow.size(), 0); // 阴影图必须深拷贝
    frame = Capture(renderer, 320, 240, 950);
    auto view = ReadView(renderer);
    RequireWorldColor(frame, view, -500, 0, {128, 128, 128}, 3);
    RequireWorldColor(frame, view, 500, 0, {255, 255, 255}, 1);

    // 无效环境拒绝并保留上一状态。
    auto invalid = environment;
    invalid.fogMode = WAR3_RENDER_FOG_LINEAR; invalid.fogStart = invalid.fogEnd = 100;
    Require(api->setEnvironment(renderer.handle, &invalid) != 0, "empty fog range accepted");
    invalid = environment; invalid.shadow = nullptr;
    Require(api->setEnvironment(renderer.handle, &invalid) != 0, "shadow without pixels accepted");
    invalid = environment; invalid.towardLight[0] = invalid.towardLight[1] = invalid.towardLight[2] = 0;
    Require(api->setEnvironment(renderer.handle, &invalid) != 0, "zero light direction accepted");
    Require(Capture(renderer, 320, 240, 950) == frame, "invalid environment replaced previous state");

    // 有光照的红色模型：直射光 (1, 0.5, 0.5) 朝上照平面 → 纯红；只剩环境光 0.2 → 暗红。
    const std::array<War3RenderModelVertexV2, 4> vertices{{
        {-250,-250,5, 0,0,1, 0.25f,0.75f}, {250,-250,5, 0,0,1, 0.75f,0.75f},
        {-250,250,5, 0,0,1, 0.25f,0.25f}, {250,250,5, 0,0,1, 0.75f,0.25f}}};
    const std::array<std::uint16_t, 6> modelIndices{0,1,2,1,3,2};
    std::vector<std::uint8_t> red(16);
    for (std::size_t i = 0; i < 4; ++i) { red[i * 4] = 255; red[i * 4 + 3] = 255; }
    const War3RenderMipRgbaV2 redMip{sizeof(War3RenderMipRgbaV2), 2, 2, red.size(), red.data()};
    const War3RenderTextureMipChainV2 modelTexture{sizeof(War3RenderTextureMipChainV2), 1, 0, 1, 1, &redMip};
    const War3RenderModelLayerV2 layer{0, 0, 0, 1.0f};
    const War3RenderModelMeshV2 mesh{0, 6, 0, 0, 1};
    const War3RenderModelV2 model{0, 1};
    const War3RenderModelInstanceV2 instance{0, {1,0,0,500, 0,1,0,0, 0,0,1,0}, {1,1,1}};
    const War3RenderModelSceneV2 models{sizeof(models), 4, 6, 1, 1, 1, 1, 1, vertices.data(), modelIndices.data(), &layer,
        &mesh, &model, &instance, &modelTexture};
    Require(api->submitModelScene(renderer.handle, &models) == 0, "lit model rejected");
    environment.shadowWidth = environment.shadowHeight = 0; environment.shadow = nullptr;
    environment.lightColor[0] = 1; environment.lightColor[1] = environment.lightColor[2] = 0.5f;
    Require(api->setEnvironment(renderer.handle, &environment) == 0, "model light rejected");
    RequireWorldColor(Capture(renderer, 320, 240, 950), ReadView(renderer), 500, 0, {255, 0, 0}, 1);
    environment.lightColor[0] = environment.lightColor[1] = environment.lightColor[2] = 0;
    environment.ambientColor[0] = environment.ambientColor[1] = environment.ambientColor[2] = 0.2f;
    Require(api->setEnvironment(renderer.handle, &environment) == 0, "ambient-only light rejected");
    RequireWorldColor(Capture(renderer, 320, 240, 950), ReadView(renderer), 500, 0, {51, 0, 0}, 2);
    const War3RenderModelSceneV2 empty{sizeof(empty), 0, 0, 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    Require(api->submitModelScene(renderer.handle, &empty) == 0, "model clear after environment rejected");
    std::cout << "environment: linear fog, static shadow deep copy, invalid preservation and model lighting passed\n";
}

#include "RenderD3D11SkinTests.h"
#include "RenderD3D11AnimationTests.h"
#include "RenderD3D11ParticleTests.h"
#include "RenderD3D11ParticleMaterialTests.h"
#include "RenderInstanceRosterTests.h"
#include "RenderD3D11AssetAppendTests.h"
#include "RenderD3D11MaterialStateTests.h"
#include "RenderD3D11StaticSamplerTests.h"
#include "RenderD3D11ObliqueSamplerTests.h"
#include "RenderD3D11TerrainObliqueTests.h"
#include "RenderD3D11ModelSourceMipTests.h"

// 原创双层平面复用已有skin夹具；验证绝对层值、GEOA独立性和同一roster事务。
void TestMaterialLayerAlphas(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info,
    InstanceRosterSetFault fault = nullptr, InstanceRosterGateRead gate = nullptr, InstanceRosterAssetRead assets = nullptr) {
    RendererOwner renderer{api,api->createRenderer(&info)}; Require(renderer.handle != nullptr,"material renderer unavailable");
    War3RenderSkinMaterialCapabilitiesV2 capability{};
    capability.base.base.base.base.structSize = sizeof(capability);
    Require(api->getSkinStatus(renderer.handle,&capability.base.base.base.base) == 0
        && capability.layerAlphaFormat == WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1,"material capability absent");
    capability.layerAlphaFormat = 0x5a5a5a5a; capability.base.base.base.base.structSize = sizeof(War3RenderSkinStatusV2);
    Require(api->getSkinStatus(renderer.handle,&capability.base.base.base.base) == 0
        && capability.layerAlphaFormat == 0x5a5a5a5a,"old status prefix wrote unknown tail");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    const War3RenderMeshV2 empty{sizeof(empty),0,0,917,nullptr,nullptr};
    Require(api->setCamera(renderer.handle,&camera) == 0 && api->submitMesh(renderer.handle,&empty) == 0,"material view failed");
    const auto background = Capture(renderer,info.width,info.height,917);
    RenderSkinFixture fixture; fixture.mesh.mesh.layerCount = 2;
    for (auto& instance : fixture.instances) for (auto& color : instance.instance.color) color = 1;
    std::array<std::array<std::uint8_t,16>,2> rgba{};
    for (std::size_t index = 0; index < 4; ++index) { rgba[0][index*4] = rgba[1][index*4+2] = 255; rgba[0][index*4+3] = rgba[1][index*4+3] = 255; }
    const std::array<War3RenderMipRgbaV2,2> mips{{{sizeof(War3RenderMipRgbaV2),2,2,16,rgba[0].data()},
        {sizeof(War3RenderMipRgbaV2),2,2,16,rgba[1].data()}}};
    const std::array<War3RenderTextureMipChainV2,2> textures{{{sizeof(War3RenderTextureMipChainV2),1,0,1,1,&mips[0]},
        {sizeof(War3RenderTextureMipChainV2),1,0,1,1,&mips[1]}}};
    const std::array<War3RenderModelLayerV2,2> layers{{{0,0,1,0.7f},{1,2,1,0.2f}}};
    std::array<float,2> layerValues{0.7f,1}; const War3RenderMaterialLayerSliceV2 slice{0,2};
    War3RenderMaterialSkinUpdateV2 pose{}; pose.base.base = fixture.Update();
    pose.base.base.structSize = sizeof(pose); pose.base.base.instanceCount = pose.base.base.groupCount = pose.base.base.alphaCount = 1;
    pose.layerAlphaFormat = WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1; pose.layerAlphaCount = 2; pose.instanceSliceCount = 1;
    pose.layerAlphas = layerValues.data(); pose.instanceSlices = &slice;
    auto scene = fixture.Scene(pose.base.base); scene.layerCount = 2; scene.layers = layers.data(); scene.textureCount = 2; scene.textures = textures.data();
    Require(api->submitSkinnedModelScene(renderer.handle,&scene) == 0,"material scene rejected");
    War3RenderSkinnedCapturedFrameV2 receipt{sizeof(receipt)};
    std::uint64_t staticUploads = 1;
    const auto capture = [&](std::uint64_t sceneVersion, std::uint64_t contentVersion) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width)*info.height*4);
        Require(api->captureSkinnedFrameRgba(renderer.handle,pixels.data(),pixels.size(),&receipt) == 0
            && receipt.skin.sceneVersion == sceneVersion && receipt.skin.contentVersion == contentVersion
            && receipt.skin.frameCount == receipt.frame.view.frameCount && receipt.skin.staticUploadCount == staticUploads
            && receipt.frame.capture.documentRevision == 917,"material completed identity mismatch"); return pixels;
    };
    RequireWorldColor(capture(1,1),receipt.frame.view,-500,0,{0,0,255},2); // dynamic1，不是static0.2的乘数。
    layerValues[1] = 0.5f; pose.base.base.contentVersion = 2;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base) == 0,"material update rejected");
    const auto half = capture(1,2); RequireWorldColor(half,receipt.frame.view,-500,0,{128,0,128},2);
    layerValues[1] = std::numeric_limits<float>::quiet_NaN(); pose.base.base.contentVersion = 3;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base) != 0 && capture(1,2) == half,"invalid layer published");
    layerValues = {0,0};
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base) == 0 && capture(1,3) == background,"dynamic opaque zero did not close layer");
    layerValues = {0.7f,0.5f}; pose.base.base.contentVersion = 4;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base) == 0 && capture(1,4) == half,"material restore changed static pixels");
    const auto uploads = receipt.skin.matrixUploadCount;
    War3RenderInstanceRosterResultV2 base{sizeof(base)};
    Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0,"material roster target absent");
    std::vector<War3RenderSkinnedModelInstanceV2> instances(20,fixture.instances[0]);
    std::vector<War3RenderGroupMatrixV2> groups(20,fixture.groups[0]); std::vector<float> alphas(20,1), values(40,0);
    std::vector<War3RenderMaterialLayerSliceV2> slices(20); std::vector<std::uint8_t> visible(20,0); visible[0] = visible[1] = 1;
    for (std::uint32_t index = 0; index < 20; ++index) {
        instances[index].firstGroup = instances[index].firstAlpha = index; slices[index] = {index*2,2}; values[index*2] = 0.7f;
        groups[index].values[9] = index == 0 ? -500.0f : 500.0f + static_cast<float>(index-1)*500;
    }
    values[1] = 0.5f; pose.base.base.sceneVersion = 2; pose.base.base.contentVersion = 1;
    pose.base.base.instanceCount = pose.base.base.groupCount = pose.base.base.alphaCount = 20;
    pose.base.base.instances = instances.data(); pose.base.base.groups = groups.data(); pose.base.base.alphas = alphas.data();
    pose.layerAlphaCount = 40; pose.instanceSliceCount = 20; pose.layerAlphas = values.data(); pose.instanceSlices = slices.data();
    War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask),20,0,0,base.target,1,visible.data(),nullptr,nullptr,nullptr,nullptr};
    War3RenderEditorInstanceRosterV2 request{}; request.structSize = sizeof(request); request.expected = base.target;
    request.expectedAssetIdentity = base.assetIdentity; request.expectedLayersRevision = base.layersRevision;
    request.expectedPresentationRevision = base.presentationRevision; request.candidatePose = &pose.base.base; request.visibility = &mask;
    std::uint64_t ticket = 0;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base) != 0,"ordinary material update changed roster");
    if (fault) {
        Require(gate && assets,"material fault controls absent");
        std::array<std::uint64_t,7> oldResources{}, after{};
        Require(assets(renderer.handle,oldResources.data(),7) == 0 && fault(renderer.handle,7) == 0,"material fault arm failed");
        Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0,"material fault not queued");
        const auto rejected = WaitInstanceRoster(renderer,ticket);
        Require(rejected.state == WAR3_RENDER_ROSTER_REJECTED && rejected.result < 0 && !rejected.completed.view.frameCount
            && capture(1,4) == half && receipt.skin.matrixUploadCount == uploads,"layer resource failure published");
        struct ReleaseGate { InstanceRosterSetFault fault; War3RendererHandle handle; ~ReleaseGate() { (void)fault(handle,0); } } release{fault,renderer.handle};
        Require(fault(renderer.handle,6) == 0 && api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0,"material cancel not queued");
        const auto deadline = GetTickCount64()+10000; while (gate(renderer.handle) != 1 && GetTickCount64() < deadline) Sleep(1);
        Require(gate(renderer.handle) == 1,"material prePublish gate absent");
        War3RenderInstanceRosterResultV2 cancelled{sizeof(cancelled)};
        Require(api->cancelEditorInstanceRoster(renderer.handle,ticket,&cancelled) == 0 && cancelled.state == WAR3_RENDER_ROSTER_SUPERSEDED
            && !cancelled.completed.view.frameCount && fault(renderer.handle,0) == 0,"material prepared cancel failed");
        Require(WaitInstanceRoster(renderer,ticket).state == WAR3_RENDER_ROSTER_SUPERSEDED && capture(1,4) == half
            && receipt.skin.matrixUploadCount == uploads && assets(renderer.handle,after.data(),7) == 0 && oldResources == after,
            "material cancel changed old pixels, owner or resources");
    }
    Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0,"twenty material roster rejected");
    values[1] = 0; // 输入期后必须owned，不能再借宿主池。
    const auto accepted = WaitInstanceRoster(renderer,ticket);
    Require(accepted.state == WAR3_RENDER_ROSTER_COMPLETED && accepted.completed.presentation.visibleInstanceCount == 2
        && accepted.completed.skin.staticUploadCount == 1 && accepted.assetIdentity == base.assetIdentity,"material roster completion wrong");
    const auto two = capture(2,1); RequireWorldColor(two,receipt.frame.view,-500,0,{128,0,128},2);
    RequireWorldColor(two,receipt.frame.view,500,0,{255,0,0},2); // 同一mesh的Glow0不能隐藏本体层。
    values[1] = 0.5f; alphas[0] = 0.5f; pose.base.base.contentVersion = 2;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base) == 0,"material/GEOA pair update rejected");
    RequireWorldColor(capture(2,2),receipt.frame.view,-500,0,{191,0,64},2); // GEOA×KMTA各一次。
    Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0,"append material target absent");
    std::array<std::uint64_t,7> oldAssets{}, appendedAssets{};
    if (assets) Require(assets(renderer.handle,oldAssets.data(),7) == 0,"material prefix identities absent");
    auto appendedMesh = fixture.mesh; appendedMesh.mesh.baseVertex = 4; appendedMesh.mesh.firstIndex = 6; appendedMesh.mesh.firstLayer = 2;
    auto appendedModel = fixture.model; appendedModel.model.firstMesh = 1;
    War3RenderAssetAppendV2 suffix{}; suffix.structSize = sizeof(suffix); suffix.prefix = {4,6,2,1,1,2,0};
    suffix.vertexCount = 4; suffix.indexCount = 6; suffix.layerCount = 2; suffix.meshCount = suffix.modelCount = 1;
    suffix.vertices = fixture.vertices.data(); suffix.indices = fixture.indices.data(); suffix.layers = layers.data();
    suffix.meshes = &appendedMesh; suffix.models = &appendedModel;
    instances.push_back(fixture.instances[0]); instances.back().instance.model = 1; instances.back().firstGroup = instances.back().firstAlpha = 20;
    groups.push_back(fixture.groups[1]); groups.back().values[10] = 600; alphas.push_back(1);
    values.push_back(0.7f); values.push_back(1); slices.push_back({40,2}); visible.push_back(1);
    pose.base.base.sceneVersion = 3; pose.base.base.contentVersion = 1;
    pose.base.base.instanceCount = pose.base.base.groupCount = pose.base.base.alphaCount = 21;
    pose.base.base.instances = instances.data(); pose.base.base.groups = groups.data(); pose.base.base.alphas = alphas.data();
    pose.layerAlphaCount = 42; pose.instanceSliceCount = 21; pose.layerAlphas = values.data(); pose.instanceSlices = slices.data();
    mask = {sizeof(mask),21,0,0,base.target,2,visible.data(),nullptr,nullptr,nullptr,nullptr};
    War3RenderEditorAssetRosterV2 append{}; append.base = request; append.base.structSize = sizeof(append);
    append.base.flags = WAR3_RENDER_ROSTER_APPEND_ASSETS; append.base.expected = base.target;
    append.base.expectedPresentationRevision = base.presentationRevision; append.base.expectedLayersRevision = base.layersRevision; append.assets = &suffix;
    Require(api->submitEditorInstanceRoster(renderer.handle,&append.base,&ticket) == 0
        && WaitInstanceRoster(renderer,ticket).state == WAR3_RENDER_ROSTER_COMPLETED,"material asset append rejected");
    staticUploads = 2; const auto appended = capture(3,1);
    RequireWorldColor(appended,receipt.frame.view,-500,0,{191,0,64},2);
    RequireWorldColor(appended,receipt.frame.view,500,600,{0,0,255},2);
    if (assets) Require(assets(renderer.handle,appendedAssets.data(),7) == 0 && appendedAssets[0] == 2
        && appendedAssets[1] == oldAssets[1] && appendedAssets[3] == oldAssets[3] && appendedAssets[4] == oldAssets[4]
        && appendedAssets[5] == oldAssets[5],"material append reuploaded prefix VB/IB/texture");
    Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0,"empty material target absent");
    pose.base.base.sceneVersion = 4; pose.base.base.contentVersion = 1;
    pose.base.base.instanceCount = pose.base.base.groupCount = pose.base.base.alphaCount = pose.layerAlphaCount = pose.instanceSliceCount = 0;
    pose.base.base.instances = nullptr; pose.base.base.groups = nullptr; pose.base.base.alphas = nullptr;
    pose.layerAlphas = nullptr; pose.instanceSlices = nullptr;
    mask = {sizeof(mask),0,0,0,base.target,3,nullptr,nullptr,nullptr,nullptr,nullptr};
    request.expected = base.target; request.expectedPresentationRevision = base.presentationRevision; request.expectedLayersRevision = base.layersRevision;
    request.expectedAssetIdentity = base.assetIdentity;
    Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0
        && WaitInstanceRoster(renderer,ticket).state == WAR3_RENDER_ROSTER_COMPLETED,"zero material roster rejected");
    (void)capture(4,1);
    auto legacy = pose.base.base; legacy.structSize = sizeof(legacy); legacy.contentVersion = 2;
    Require(api->updateSkinMatrices(renderer.handle,&legacy) != 0,"empty material roster lost explicit mode");
    std::cout << "material WARP: absolute layer alpha, two independent instances, GEOA pair, one->twenty->append->zero, immutable prefix assets"
        << (fault ? ", layer fault and prepared cancellation rollback\n" : "\n");
}
void TestMaterialLayerFaults(const std::filesystem::path& directory, const War3RenderCreateInfoV2& info) {
    const auto module = LoadLibraryExW((directory/L"war3-render-d3d11-test.dll").c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Require(module != nullptr,"material fault DLL absent");
    struct Guard { HMODULE module; ~Guard() { FreeLibrary(module); } } guard{module};
    const auto api = reinterpret_cast<War3GetRenderPluginApiV2>(GetProcAddress(module,"War3MapEditor_GetRenderPluginApiV2"));
    const auto fault = reinterpret_cast<InstanceRosterSetFault>(GetProcAddress(module,"War3RenderTest_SetFault"));
    const auto gate = reinterpret_cast<InstanceRosterGateRead>(GetProcAddress(module,"War3RenderTest_RosterGateReached"));
    const auto assets = reinterpret_cast<InstanceRosterAssetRead>(GetProcAddress(module,"War3RenderTest_GetAssetResources"));
    Require(api && fault && gate && assets,"material fault controls absent");
    TestMaterialLayerAlphas(api(WAR3_RENDER_PLUGIN_API_V2),info,fault,gate,assets);
    TestMaterialStates(api(WAR3_RENDER_PLUGIN_API_V2),info,fault,gate,assets);
}

void TestAdditionalModelLayer(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    Require(War3Map::Render::ModelSceneLayerRenderer::Supported(api), "additional model layer tail missing");
    auto old = *api; old.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, submitModelSceneLayer));
    Require(!War3Map::Render::ModelSceneLayerRenderer::Supported(&old), "old provider claims model layers");
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle != nullptr, "layer renderer unavailable");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "layer camera rejected");
    RenderSkinFixture world; auto worldPose = world.Update(); auto worldScene = world.Scene(worldPose);
    Require(api->submitSkinnedModelScene(renderer.handle, &worldScene) == 0, "layer world rejected");
    War3RenderModelLayersTargetV2 target{sizeof(target)};
    War3RenderLayeredCapturedFrameV2 receipt{sizeof(receipt)};
    const auto capture = [&] {
        Require(api->getModelLayersTarget(renderer.handle, &target) == 0, "layer target query failed");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width) * info.height * 4);
        Require(api->captureLayeredFrameRgba(renderer.handle, target.layersRevision, pixels.data(), pixels.size(), &receipt) == 0,
            "layer combined capture failed");
        Require(receipt.layersRevision == target.layersRevision && receipt.layerCount == target.layerCount,
            "layer capture receipt not bound to target");
        return pixels;
    };
    const auto original = capture(); const auto uploads = receipt.world.skin.staticUploadCount;
    RenderSkinFixture ghost;
    for (auto& instance : ghost.instances) instance.instance.transform[7] = 600;
    auto pose = ghost.Update(); pose.sceneVersion = 2; auto models = ghost.Scene(pose);
    War3RenderModelSceneLayerV2 layer{sizeof(layer), 0, 501, target.parentModelEpoch, 1, 0, &models, nullptr};
    Require(api->submitModelSceneLayer(renderer.handle, &layer) == 0, "new layer rejected");
    const auto added = capture();
    Require(added != original && receipt.layerCount == 1 && receipt.layers[0].revision == 1
        && receipt.layers[0].skin.staticUploadCount == 1 && receipt.world.skin.staticUploadCount == uploads,
        "layer missing or reuploaded world assets");
    RequireWorldColor(added, receipt.world.frame.view, -500, 0, {255,0,0}, 1);
    RequireWorldColor(added, receipt.world.frame.view, -500, 600, {255,0,0}, 1);
    const auto oldRevision = target.layersRevision;
    for (auto& instance : ghost.instances) instance.instance.transform[7] = -600;
    pose = ghost.Update(2); pose.sceneVersion = 2;
    const War3RenderModelLayerUpdateV2 move{sizeof(move), 0, 501, layer.parentModelEpoch, 2, 1, &pose, nullptr};
    Require(api->updateModelSceneLayer(renderer.handle, &move) == 0, "layer move rejected");
    std::vector<std::uint8_t> untouched(original.size(), 0x71);
    const auto before = receipt;
    Require(api->captureLayeredFrameRgba(renderer.handle, oldRevision, untouched.data(), untouched.size(), &receipt) == 2
        && std::memcmp(&before, &receipt, sizeof(receipt)) == 0 && untouched.front() == 0x71, "stale layer capture modified output");
    const auto moved = capture();
    Require(moved != added && receipt.layers[0].revision == 2 && receipt.layers[0].skin.staticUploadCount == 1
        && receipt.layers[0].skin.matrixUploadCount == 2 && receipt.world.skin.staticUploadCount == uploads,
        "layer movement reuploaded geometry or mixed revisions");
    RequireWorldColor(moved, receipt.world.frame.view, -500, -600, {255,0,0}, 1);
    Require(api->updateModelSceneLayer(renderer.handle, &move) != 0, "stale layer update accepted");
    Require(api->removeModelSceneLayer(renderer.handle, 501, layer.parentModelEpoch, 2) == 0, "layer removal rejected");
    Require(capture() == original && receipt.layerCount == 0 && receipt.world.skin.staticUploadCount == uploads,
        "removing layer changed main scene pixels or reuploaded assets");
    layer.expectedRevision = 0; layer.revision = 3;
    Require(api->submitModelSceneLayer(renderer.handle, &layer) == 0, "layer resubmit rejected");
    worldPose.sceneVersion = 3;
    Require(api->submitSkinnedModelScene(renderer.handle, &worldScene) == 0, "new parent rejected");
    Require(capture() == original && receipt.layerCount == 0, "new parent retained stale preview layer");
    Require(api->updateModelSceneLayer(renderer.handle, &move) != 0, "old parent layer update accepted");
    std::cout << "additional model layer: same depth/camera pixels, dynamic movement, cancellation, parent invalidation and unchanged world uploads passed\n";
}

void TestBuildingSplatLayer(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    using namespace War3Map;
    using namespace War3Map::Render;
    using namespace War3Map::Services;
    // 原创目录和透明RGBA，经过公共查询/构建器；不是手工伪造的GPU贴片或原版投影证据。
    const std::array<std::string_view, 20> columns{"Name","Dir","file","BlendMode","Scale","BirthTime","PauseTime","Decay",
        "StartR","StartG","StartB","StartA","MiddleR","MiddleG","MiddleB","MiddleA","EndR","EndG","EndB","EndA"};
    const std::array<std::string_view, 20> values{"TEST","Textures","TestSplat","0","256","1","0","2",
        "255","255","255","0","255","255","255","255","255","255","255","0"};
    std::string slk = "ID;PWXL;N;E\n";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        slk += "C;X" + std::to_string(i + 1) + ";Y1;K\"" + std::string(columns[i]) + "\"\n";
        slk += "C;X" + std::to_string(i + 1) + ";Y2;K\"" + std::string(values[i]) + "\"\n";
    }
    slk += "E\n";
    const auto bytes = std::as_bytes(std::span(slk.data(), slk.size()));
    const auto parsed = ParseBuildingSplatCatalog({{"Splats/UberSplatData.slk", Resources::ResourceLayer::Game,
        330, "owned-headless-fixture", "Splats/UberSplatData.slk"}, {bytes.begin(), bytes.end()}});
    Require(parsed.Succeeded(), "headless splat catalog invalid");
    const auto catalog = *parsed.value;
    auto image = std::make_shared<ModelTextureRgba>(); image->path = catalog->definitions.at("TEST").texturePath;
    image->width = image->height = 16; image->rgba.resize(16 * 16 * 4);
    for (std::size_t y = 4; y < 12; ++y) for (std::size_t x = 4; x < 12; ++x) {
        const auto at = (y * 16 + x) * 4; image->rgba[at + 1] = 255; image->rgba[at + 3] = 128;
    }
    const auto digest = Sha256(std::as_bytes(std::span(image->rgba.data(), image->rgba.size())));
    Require(digest.Succeeded(), "splat RGBA identity failed");
    const std::array<BuildingSplatPreviewTexture, 1> textures{{{image->path, "owned-headless-fixture", *digest.value, image}}};
    const auto loaded = BuildBuildingSplatPreviewAssets(catalog, textures, {13, 5, "owned-headless-resources"});
    Require(loaded.Succeeded(), "headless splat assets failed");
    auto terrain = std::make_shared<MapData::TerrainGridSnapshot>(); terrain->header.width = terrain->header.height = 17;
    terrain->header.offset = {-1024, -1024}; terrain->minimum = {-1024, -1024, 0}; terrain->maximum = {1024, 1024, 0};
    for (std::uint32_t y = 0; y < 17; ++y) for (std::uint32_t x = 0; x < 17; ++x) {
        MapData::TerrainVertex vertex; vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2;
        vertex.position = {-1024 + 128 * static_cast<float>(x), -1024 + 128 * static_cast<float>(y), 0};
        terrain->vertices.push_back(vertex);
    }
    for (std::uint32_t y = 0; y < 16; ++y) for (std::uint32_t x = 0; x < 16; ++x)
        terrain->tiles.push_back({{y * 17 + x, y * 17 + x + 1, (y + 1) * 17 + x, (y + 1) * 17 + x + 1}});
    WorldSelectionVersion version{{"owned-headless-map", 11, 7, "owned-map-source"}, 3, 4};
    BuildingSplatPreviewBinding binding{version, catalog->identitySha256, "owned-flat-terrain", 1};
    BuildingSplatUnitInput unit; unit.owner = {MapData::PlacementFile::Units, 0, "hars", 17}; unit.version = version;
    unit.position = {0, 0, 900}; unit.fields.complete = true; unit.fields.uubs = "TEST"; unit.fields.tint = {255, 255, 255};
    for (const auto field : {"uubs", "uclr", "uclg", "uclb"})
        unit.fields.origins.push_back({field, "hars", "owned-headless-fields", "Units/UnitUI.slk", {}});
    const auto inputs = [&] {
        const std::array<BuildingSplatUnitInput, 1> units{unit};
        const auto inputDigest = Sha256Text("owned-headless-input/" + std::to_string(version.document.revision)
            + "/" + std::to_string(unit.position[0]));
        Require(inputDigest.Succeeded(), "splat input identity failed");
        const auto page = QueryBuildingSplatFootprints(catalog, {version, *inputDigest.value, units},
            version, catalog->identitySha256);
        Require(page.Succeeded() && page.value->rows.size() == 1, "headless splat footprint query failed");
        return std::array<BuildingSplatPreviewInstance, 1>{{{page.value->rows.front(), 917}}};
    };
    auto requested = inputs();
    const auto built = BuildBuildingSplatPreviewFrame(*loaded.value, requested, binding, terrain, 50);
    Require(built.Succeeded(), "headless splat frame failed");
    const auto& first = *built.value;
    Require(first.previewApproximate && first.renderLayerRole == WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW
        && first.displayed == 1 && first.owners.size() == 1
        && first.owners[0].owner == unit.owner && first.owners[0].stablePlacementId == 917
        && first.owners[0].renderedInstance == 0 && first.owners[0].origins.size() == 4
        && first.binding == binding && first.terrain == terrain && first.frame->documentRevision == 5
        && first.binding.version.document.revision == 7, "splat owner or revision namespaces mixed");
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle != nullptr, "headless splat renderer unavailable");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "headless splat camera rejected");
    RenderSkinFixture world; // 保留真实主场景parent；其几何移出相机，便于测透明贴片。
    for (auto& instance : world.instances) instance.instance.transform[3] = 10000;
    auto worldPose = world.Update(); auto worldScene = world.Scene(worldPose);
    Require(api->submitSkinnedModelScene(renderer.handle, &worldScene) == 0, "headless splat parent rejected");
    War3RenderModelLayersTargetV2 target{sizeof(target)};
    War3RenderLayeredCapturedFrameV2 receipt{sizeof(receipt)};
    const auto capture = [&] {
        Require(api->getModelLayersTarget(renderer.handle, &target) == 0, "splat target query failed");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width) * info.height * 4);
        Require(api->captureLayeredFrameRgba(renderer.handle, target.layersRevision, pixels.data(), pixels.size(), &receipt) == 0
            && receipt.layersRevision == target.layersRevision && receipt.layerCount == target.layerCount
            && receipt.world.frame.capture.ticket && receipt.world.frame.capture.frameCount, "splat completed capture failed");
        return pixels;
    };
    const auto stockBackground = capture();
    const auto reportPixel = [&](const std::vector<std::uint8_t>& pixels, float x, float y, const char* stage) {
        using namespace DirectX;
        XMFLOAT4X4 matrix; std::memcpy(&matrix, receipt.world.frame.view.viewProjection, sizeof(matrix));
        XMFLOAT4 clip; XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(x, y, 0, 1), XMLoadFloat4x4(&matrix)));
        Require(clip.w > 0, "splat diagnostic pixel behind view");
        const auto px = static_cast<std::uint32_t>(std::floor((clip.x / clip.w + 1) * .5f * static_cast<float>(info.width)));
        const auto py = static_cast<std::uint32_t>(std::floor((1 - clip.y / clip.w) * .5f * static_cast<float>(info.height)));
        Require(px < info.width && py < info.height, "splat diagnostic pixel outside frame");
        const auto at = (static_cast<std::size_t>(py) * info.width + px) * 4;
        std::cout << "building splat diagnostic " << stage << " world=" << x << ',' << y << " rgba="
            << static_cast<unsigned>(pixels[at]) << ',' << static_cast<unsigned>(pixels[at + 1]) << ','
            << static_cast<unsigned>(pixels[at + 2]) << ',' << static_cast<unsigned>(pixels[at + 3]) << '\n';
    };
    reportPixel(stockBackground, -400, 0, "default-grid");
    // 无主mesh时默认Z=0网格与贴画共面；改用相机外indexed背景，独立测试辅助层而非未闭合的bias。
    const War3RenderVertexV2 background[]{{10000,10000,0,.045f,.060f,.075f},
        {10128,10000,0,.045f,.060f,.075f}, {10000,10128,0,.045f,.060f,.075f}};
    const std::uint32_t backgroundIndices[]{0, 1, 2};
    const War3RenderMeshV2 backgroundMesh{sizeof(backgroundMesh), 3, 3, 71, background, backgroundIndices};
    Require(api->submitMesh(renderer.handle, &backgroundMesh) == 0, "splat controlled background rejected");
    const auto original = capture(); const auto worldUploads = receipt.world.skin.staticUploadCount;
    const auto parentEpoch = target.parentModelEpoch;
    const auto worldIdentity = receipt.world;
    RequireWorldColor(original, receipt.world.frame.view, -400, 0, {11, 15, 19}, 1);
    ModelSceneLayerRenderer layer(api, renderer.handle, 502);
    Require(layer.Submit(first.scene, first.frame).Succeeded(), "builder splat layer rejected");
    const auto added = capture();
    const auto checkIdentity = [&](const BuildingSplatPreviewFrame& frame) {
        Require(receipt.layerCount == 1 && receipt.layers[0].layerId == 502
            && receipt.layers[0].parentModelEpoch == parentEpoch && target.parentModelEpoch == parentEpoch
            && receipt.layers[0].revision == layer.Revision() && receipt.layers[0].result == 0
            && receipt.layers[0].skin.result == 0 && receipt.layers[0].skin.sceneVersion == frame.scene->sceneVersion
            && receipt.layers[0].skin.contentVersion == frame.frame->contentVersion
            && receipt.layers[0].skin.frameCount == receipt.world.frame.capture.frameCount
            && layer.Target() == frame.frame && receipt.world.skin.staticUploadCount == worldUploads,
            "splat completed pixels not bound to accepted owner/parent/frame");
        Require(receipt.world.skin.sceneVersion == worldIdentity.skin.sceneVersion
            && receipt.world.skin.contentVersion == worldIdentity.skin.contentVersion
            && receipt.world.skin.matrixUploadCount == worldIdentity.skin.matrixUploadCount
            && receipt.world.frame.capture.documentRevision == worldIdentity.frame.capture.documentRevision
            && receipt.world.frame.view.surfaceEpoch == worldIdentity.frame.view.surfaceEpoch
            && receipt.world.frame.view.viewEpoch == worldIdentity.frame.view.viewEpoch
            && std::memcmp(receipt.world.frame.view.viewProjection, worldIdentity.frame.view.viewProjection,
                sizeof(worldIdentity.frame.view.viewProjection)) == 0,
            "splat edit changed parent model, terrain or camera identity");
    };
    checkIdentity(first);
    Require(added != original, "builder splat has no visible pixels");
    RequireWorldColor(original, receipt.world.frame.view, 0, 0, {11, 15, 19}, 1);
    RequireWorldColor(added, receipt.world.frame.view, 0, 0, {6, 135, 9}, 2);
    RequireWorldColor(added, receipt.world.frame.view, 200, 200, {11, 15, 19}, 1);
    RequireWorldColor(added, receipt.world.frame.view, -400, 0, {11, 15, 19}, 1);
    const auto staticUploads = receipt.layers[0].skin.staticUploadCount;
    Require(staticUploads == 1, "splat static assets were not installed exactly once");
    const auto poseUploads = receipt.layers[0].skin.matrixUploadCount;
    // 同一投影几何上的改色仍须复用场景；保留动态更新不重新上传的完成帧门禁。
    ++version.document.revision; unit.version = version; unit.fields.tint[1] = 128; binding.version = version;
    requested = inputs();
    const auto recolored = BuildBuildingSplatPreviewFrame(*loaded.value, requested, binding, terrain, 51, {}, &first);
    Require(recolored.Succeeded() && recolored.value->scene == first.scene && recolored.value->owners.size() == 1
        && recolored.value->owners[0].owner == first.owners[0].owner && recolored.value->owners[0].stablePlacementId == 917,
        "color-only splat failed to retain projected assets/owner");
    Require(layer.Update(recolored.value->frame).Succeeded(), "splat color-only update rejected");
    const auto recoloredPixels = capture(); checkIdentity(*recolored.value);
    Require(recoloredPixels != added && receipt.layers[0].skin.staticUploadCount == staticUploads
        && receipt.layers[0].skin.matrixUploadCount == poseUploads + 1, "splat color-only edit uploaded static assets");
    RequireWorldColor(recoloredPixels, receipt.world.frame.view, 0, 0, {6, 72, 9}, 2);
    RequireWorldColor(recoloredPixels, receipt.world.frame.view, -400, 0, {11, 15, 19}, 1);
    const auto colorRevision = layer.Revision();
    // 位置改变会改变沿地形三角面裁切的世界顶点；必须重新绑定静态场景，不能只移矩阵。
    ++version.document.revision; unit.version = version; unit.position[0] = -400; binding.version = version;
    unit.fields.tint[1] = 255;
    requested = inputs();
    const auto moved = BuildBuildingSplatPreviewFrame(*loaded.value, requested, binding, terrain, 52, {}, &*recolored.value);
    Require(moved.Succeeded() && moved.value->scene != recolored.value->scene && moved.value->owners.size() == 1
        && moved.value->scene->sceneVersion != recolored.value->scene->sceneVersion
        && moved.value->scene->geometry.textures.SharesStorageWith(first.scene->geometry.textures)
        && moved.value->owners[0].owner == first.owners[0].owner && moved.value->owners[0].stablePlacementId == 917,
        "moved projected splat did not rebuild geometry while retaining texture/owner");
    Require(moved.value->frame->instances[0].instance.transform[3] == 0
        && std::all_of(std::begin(moved.value->frame->instances[0].instance.color),
            std::end(moved.value->frame->instances[0].instance.color), [](float value) { return value == 1; }),
        "projected world geometry was translated twice or retained old tint");
    Require(!moved.value->scene->geometry.vertices.empty()
        && std::all_of(moved.value->scene->geometry.vertices.begin(), moved.value->scene->geometry.vertices.end(),
            [&](const auto& vertex) { return vertex.x >= requested[0].row.footprint->minimum[0]
                && vertex.x <= requested[0].row.footprint->maximum[0]
                && vertex.y >= requested[0].row.footprint->minimum[1]
                && vertex.y <= requested[0].row.footprint->maximum[1] && vertex.z == 0; }),
        "moved projected vertices left the actual flat terrain footprint");
    Require(!layer.Update(moved.value->frame).Succeeded() && layer.Target() == recolored.value->frame
        && layer.Revision() == colorRevision, "new static splat geometry bypassed the layer rebind guard");
    Require(capture() == recoloredPixels, "rejected matrix-only move changed accepted pixels");
    Require(layer.Submit(moved.value->scene, moved.value->frame).Succeeded() && layer.Revision() == colorRevision + 1,
        "projected splat scene rebind rejected");
    const auto movedPixels = capture(); checkIdentity(*moved.value);
    reportPixel(movedPixels, -400, 0, "moved-owned-frame");
    // 替换静态模型建立新的层 GPU 容器；这两个计数针对该容器，不是跨重建累计值。
    Require(movedPixels != added && receipt.layers[0].skin.staticUploadCount == 1
        && receipt.layers[0].skin.matrixUploadCount == 1, "rebound projected geometry was not uploaded exactly once");
    RequireWorldColor(movedPixels, receipt.world.frame.view, -400, 0, {6, 135, 9}, 2);
    RequireWorldColor(movedPixels, receipt.world.frame.view, 0, 0, {11, 15, 19}, 1);
    const auto acceptedRevision = layer.Revision();
    auto invalid = std::make_shared<AnimationFrame>(*moved.value->frame); ++invalid->contentVersion;
    invalid->groups.front().values[11] = std::numeric_limits<float>::quiet_NaN();
    Require(!layer.Update(invalid).Succeeded() && !layer.Update(first.frame).Succeeded()
        && layer.Target() == moved.value->frame && layer.Revision() == acceptedRevision, "bad/stale splat replaced accepted owner");
    auto stale = requested; stale[0].row.version.document.revision = 7;
    Require(!BuildBuildingSplatPreviewFrame(*loaded.value, stale, binding, terrain, 53).Succeeded(), "stale map splat candidate accepted");
    Require(capture() == movedPixels && receipt.layers[0].skin.staticUploadCount == 1
        && receipt.layers[0].skin.matrixUploadCount == 1, "rejected splat changed pixels/uploads");
    checkIdentity(*moved.value);
    Require(layer.Remove().Succeeded() && capture() == original && receipt.layerCount == 0,
        "removing builder splat did not restore parent pixels");
    // 真正共面：地形来自同一flat snapshot，贴画几何仍Z=0；不再移走背景掩盖深度竞争。
    const War3RenderVertexV2 plane[]{{-1024,-1024,0,.045f,.060f,.075f},{1024,-1024,0,.045f,.060f,.075f},
        {-1024,1024,0,.045f,.060f,.075f},{1024,1024,0,.045f,.060f,.075f}};
    const std::uint32_t planeIndices[]{0,1,2,1,3,2};
    const War3RenderMeshV2 planeMesh{sizeof(planeMesh),4,6,72,plane,planeIndices};
    Require(api->submitMesh(renderer.handle,&planeMesh)==0, "coplanar terrain rejected");
    auto oldApi=*api; oldApi.structSize=static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2,getModelLayerCapabilities));
    oldApi.getModelLayerCapabilities=nullptr; ModelSceneLayerRenderer oldLayer(&oldApi,renderer.handle,503);
    Require(!oldLayer.Submit(first.scene,first.frame,WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW).Succeeded()
        && oldLayer.Submit(first.scene,first.frame).Succeeded() && oldLayer.Remove().Succeeded(), "old layer prefix/capability changed");
    Require(layer.Submit(first.scene,first.frame,WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW).Succeeded(), "typed decal rejected");
    const std::array<War3RenderCameraV2,3> angles{{camera,{sizeof(camera),0,0,0,3500,-.7f,.8f},{sizeof(camera),0,0,0,4500,.2f,.5f}}};
    for (const auto& angle:angles) { Require(api->setCamera(renderer.handle,&angle)==0, "decal view rejected");
        const auto pixels=capture(); RequireWorldColor(pixels,receipt.world.frame.view,0,0,{6,135,9},2); }
    const auto stable=capture(); const auto stableRevision=target.layersRevision;
    AnimatedSceneDescriptor decalDescriptor; decalDescriptor.Refresh(*first.scene,*first.frame);
    War3RenderModelSceneLayerV2 bad{sizeof(bad),0,502,target.parentModelEpoch,layer.Revision()+1,layer.Revision(),&decalDescriptor.scene,nullptr,2,0};
    Require(api->submitModelSceneLayer(renderer.handle,&bad)!=0, "unknown layer role accepted");
    bad.role=WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW; bad.roleReserved=1;
    Require(api->submitModelSceneLayer(renderer.handle,&bad)!=0, "reserved role bytes accepted");
    bad.roleReserved=0; bad.role=WAR3_RENDER_MODEL_LAYER_MODEL;
    Require(api->submitModelSceneLayer(renderer.handle,&bad)!=0 && capture()==stable && target.layersRevision==stableRevision,
        "role replacement changed accepted frame");
    Require(api->setCamera(renderer.handle,&camera)==0, "foreground view rejected");
    world.instances[0].instance.transform[3]=0; world.groups[0].values[9]=0;
    for (auto& vertex:world.vertices) { vertex.vertex.x*=.25f; vertex.vertex.y*=.25f; }
    worldPose=world.Update(2); worldScene=world.Scene(worldPose);
    // Update(2)只增加contentVersion；静态几何替换必须拥有新sceneVersion，不能降低插件的旧场景guard。
    Require(api->submitSkinnedModelScene(renderer.handle,&worldScene)==1, "stale static scene version accepted");
    worldPose.sceneVersion=2; worldScene=world.Scene(worldPose);
    const auto foregroundResult=api->submitSkinnedModelScene(renderer.handle,&worldScene);
    std::cout << "building splat foreground parent submit=" << foregroundResult << " scene=" << worldPose.sceneVersion << '\n';
    Require(foregroundResult==0, "decal foreground parent setup failed");
    const auto foregroundLayer=layer.Submit(first.scene,first.frame,WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW);
    for (const auto& diagnostic:foregroundLayer.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(foregroundLayer.Succeeded(), "decal foreground layer setup failed");
    const auto occluded=capture(); RequireWorldColor(occluded,receipt.world.frame.view,0,0,{255,0,0},1);
    RequireWorldColor(occluded,receipt.world.frame.view,100,0,{6,135,9},2);
    // 共面和前景遮挡已在上面单独验证。0.01世界单位低于此镜头的D32深度分辨率；
    // no-write检查恢复原背景以隔离地形的不同VS误差，若贴画写深度仍会挡住普通层。
    Require(api->submitMesh(renderer.handle,&backgroundMesh)==0, "ordinary layer controlled background restore failed");
    RequireWorldColor(capture(),receipt.world.frame.view,100,0,{6,135,9},2);
    auto above=std::make_shared<AnimationFrame>(*first.frame); above->contentVersion=60;
    for (auto& group:above->groups) group.values[11]+=.01f;
    ModelSceneLayerRenderer ordinary(api,renderer.handle,503);
    Require(ordinary.Submit(first.scene,above).Succeeded(), "ordinary layer after decal rejected");
    const auto combined=capture(); RequireWorldColor(combined,receipt.world.frame.view,100,0,{3,195,4},2);
    Require(ordinary.Remove().Succeeded() && layer.Remove().Succeeded(), "typed decal cleanup failed");
    std::cout << "building splat auxiliary layer: owned builder, alpha pixels, owner/parent/completed-frame binding, static reuse and rejection preservation; PreviewApproximate only\n";
}

void TestOwnedStaticHelper(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    using namespace War3Map::Render;
    const auto asset = CreateCheckerBlockerAsset(std::array<float, 2>{256, 256});
    Require(asset.Succeeded(), "owned checker asset could not be sealed");
    AnimationInstanceSpec spec; spec.identity = {901, 44}; spec.hostKey = "owned-placement";
    spec.model.modelPath = "Objects\\InvisibleBlocker.mdx"; spec.model.position = {-300, 0, 0};
    spec.model.ownedStaticAsset = *asset.value;
    unsigned reads = 0;
    const auto created = ModelAnimationService::Create({44, 1781, "owned-headless"}, std::span(&spec, 1),
        [&](std::string_view, std::uint64_t) -> War3Map::Result<AnimationResource> {
            ++reads; return War3Map::Result<AnimationResource>::Failure("Test", "MISSING", "辅助资产不得读取假MDX。");
        });
    Require(created.Succeeded(), "owned animation scene could not be created"); const auto service = *created.value;
    auto prepared = service->Prepare(nullptr); Require(prepared.Succeeded(), "owned static skin frame failed");
    const auto first = *prepared.value;
    RendererOwner renderer{api, api->createRenderer(&info)}; Require(renderer.handle != nullptr, "owned renderer missing");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 1400, -1.5707963f, 1.5f};
    // 非空相机外mesh关闭默认grid，让移动与隐藏只比较真正的辅助模型像素。
    const War3RenderVertexV2 backgroundVertices[]{{10000, 10000, 0, .045f, .060f, .075f},
        {10128, 10000, 0, .045f, .060f, .075f}, {10000, 10128, 0, .045f, .060f, .075f}};
    const std::uint32_t backgroundIndices[]{0, 1, 2};
    const War3RenderMeshV2 backgroundMesh{sizeof(backgroundMesh), 3, 3, 1781, backgroundVertices, backgroundIndices};
    Require(api->setCamera(renderer.handle, &camera) == 0 && api->submitMesh(renderer.handle, &backgroundMesh) == 0,
        "owned controlled background failed");
    const auto background = Capture(renderer, info.width, info.height, 1781);
    AnimationRenderer bridge(api, renderer.handle); std::array<std::uint8_t, 1> visible{1};
    EditorVisibilityMasks masks{visible, {}, 1};
    Require(bridge.Submit(service->Scene(), first, &masks).Succeeded(), "owned production skin submit rejected");
    std::vector<std::byte> bytes(static_cast<std::size_t>(info.width) * info.height * 4);
    AnimationCaptureReceipt receipt;
    const auto capture = [&] {
        const auto result = bridge.Capture(bytes, 1781); Require(result.Succeeded(), "owned completed frame capture failed");
        receipt = *result.value;
        Require(receipt.presentation && receipt.presentation->presentationRevision == masks.presentationRevision
            && receipt.skin.result == 0 && receipt.skin.active && receipt.skin.sceneVersion == service->Scene()->sceneVersion
            && receipt.skin.contentVersion == bridge.Target()->contentVersion && receipt.frame.capture.ticket
            && receipt.skin.frameCount == receipt.frame.capture.frameCount
            && receipt.frame.view.frameCount == receipt.skin.frameCount, "owned completed identity mismatch");
        RequireExactMatrix(receipt.frame.view, camera);
        std::vector<std::uint8_t> pixels(bytes.size()); std::memcpy(pixels.data(), bytes.data(), bytes.size()); return pixels;
    };
    const auto requireChecker = [](const std::vector<std::uint8_t>& pixels) {
        std::size_t purple = 0, black = 0;
        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            purple += pixels[at] >= 190 && pixels[at + 1] <= 4 && pixels[at + 2] >= 224;
            black += pixels[at] == 12 && pixels[at + 1] == 10 && pixels[at + 2] == 16;
        }
        Require(purple >= 16 && black >= 16, "owned purple and black checker pixels were not actually drawn");
    };
    const auto initial = capture(); requireChecker(initial); const auto staticUploads = receipt.skin.staticUploadCount;
    Require(staticUploads == 1 && !reads && service->Scene()->geometry.instanceRequests == std::vector<std::uint32_t>{0},
        "owned helper read MDX, lost ordinary instance identity or duplicated static upload");
    AnimationEdit move; move.identity = spec.identity; move.transform = first->states[0].transform; (*move.transform)[3] = 300;
    AnimationAdvance command; command.edits.push_back(move);
    prepared = service->Prepare(first.get(), command); Require(prepared.Succeeded(), "owned movement preparation failed");
    masks.presentationRevision = 2;
    Require(bridge.Update(*prepared.value, &masks).Succeeded(), "owned movement skin update failed");
    const auto moved = capture(); requireChecker(moved);
    for (std::size_t row = 0; row < info.height; ++row) {
        const auto start = row * info.width * 4, half = static_cast<std::size_t>(info.width / 2) * 4;
        Require(std::equal(initial.begin() + start + half, initial.begin() + start + info.width * 4,
            background.begin() + start + half), "owned initial helper was not isolated in left half");
        Require(std::equal(moved.begin() + start, moved.begin() + start + half, background.begin() + start),
            "owned movement left ghost pixels at original location");
    }
    const auto matrixUploads = receipt.skin.matrixUploadCount;
    Require(moved != initial && receipt.skin.staticUploadCount == staticUploads && matrixUploads == 2,
        "owned same-layout movement reuploaded static assets or did not upload dynamic pose");
    visible[0] = 0; masks.presentationRevision = 3;
    Require(bridge.Present(masks).Succeeded() && capture() == background
        && receipt.skin.staticUploadCount == staticUploads && receipt.skin.matrixUploadCount == matrixUploads,
        "owned hide left pixels or reuploaded resident assets");
    visible[0] = 1; masks.presentationRevision = 4;
    Require(bridge.Present(masks).Succeeded() && capture() == moved && !reads
        && receipt.skin.staticUploadCount == staticUploads && receipt.skin.matrixUploadCount == matrixUploads,
        "owned show did not restore frozen texture/pose without upload");
    std::cout << "owned helper: production skin/completed checker pixels, movement/hide without ghosts, resident asset uploads; PreviewApproximate\n";
}

void TestEditorVisibility(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, getEditorCompletedFrame) + sizeof(api->getEditorCompletedFrame)
        && api->getEditorPresentationTarget && api->submitEditorVisibility && api->captureEditorPresentationFrameRgba
        && api->submitEditorModelScene && api->getEditorCompletedFrame, "editor visibility/completed tail unavailable");
    for (const auto mode : {WAR3_EDITOR_PRESENTATION_STATIC, WAR3_EDITOR_PRESENTATION_SKINNED, WAR3_EDITOR_PRESENTATION_PARTICLES}) {
        RendererOwner renderer{api, api->createRenderer(&info)}; Require(renderer.handle != nullptr, "editor renderer missing");
        const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
        const War3RenderMeshV2 empty{sizeof(empty),0,0,1711,nullptr,nullptr};
        Require(api->setCamera(renderer.handle,&camera) == 0 && api->submitMesh(renderer.handle,&empty) == 0, "editor view rejected");
        const auto background = Capture(renderer,320,240,1711);
        ParticleGpuFixture fixture; fixture.Bind(10,20);
        std::array<War3RenderModelVertexV2,4> vertices;
        std::array<War3RenderModelInstanceV2,2> instances;
        for (std::size_t i=0;i<vertices.size();++i) vertices[i]=fixture.skin.vertices[i].vertex;
        for (std::size_t i=0;i<instances.size();++i) {
            instances[i]=fixture.skin.instances[i].instance; instances[i].transform[3]=fixture.skin.groups[i].values[9];
            std::copy_n(fixture.colors[i].rgb,3,instances[i].color);
        }
        const auto mesh=fixture.skin.mesh.mesh; const auto model=fixture.skin.model.model;
        War3RenderModelSceneV2 staticScene{sizeof(staticScene),4,6,1,1,1,2,0,vertices.data(),fixture.skin.indices.data(),
            &fixture.skin.layer,&mesh,&model,instances.data(),nullptr};
        War3RenderEditorPresentationTargetV2 target{sizeof(target)};
        Require(api->getEditorPresentationTarget(renderer.handle,&target) == 0, "initial editor target unavailable");
        std::array<std::uint8_t,2> visible{1,1}; std::uint8_t particleVisible=1;
        War3RenderEditorVisibilityUpdateV2 update{sizeof(update),2,mode==WAR3_EDITOR_PRESENTATION_PARTICLES?1u:0u,0,target,1,
            visible.data(),mode==WAR3_EDITOR_PRESENTATION_PARTICLES?&particleVisible:nullptr,nullptr,nullptr};
        const War3RenderEditorModelSceneV2 scene{sizeof(scene),0,mode==WAR3_EDITOR_PRESENTATION_STATIC?&staticScene:nullptr,
            mode==WAR3_EDITOR_PRESENTATION_SKINNED?&fixture.models:nullptr,mode==WAR3_EDITOR_PRESENTATION_PARTICLES?&fixture.scene:nullptr,&update};
        Require(api->submitEditorModelScene(renderer.handle,&scene) == 0, "atomic editor scene rejected");
        Require(api->getEditorPresentationTarget(renderer.handle,&target) == 0 && target.mode==mode
            && target.modelEpoch && target.instanceCount==2 && target.particleDrawCount==update.particleDrawCount, "editor target identity wrong");
        War3RenderEditorCapturedFrameV2 receipt{sizeof(receipt)};
        const auto capture = [&](std::uint64_t revision) {
            std::vector<std::uint8_t> pixels(320*240*4);
            Require(api->captureEditorPresentationFrameRgba(renderer.handle,target.modelEpoch,revision,pixels.data(),pixels.size(),&receipt) == 0,
                "editor completed capture failed");
            Require(receipt.presentation.modelEpoch==target.modelEpoch && receipt.presentation.presentationRevision==revision
                && receipt.presentation.sceneVersion==target.sceneVersion && receipt.presentation.contentVersion==target.contentVersion
                && receipt.frame.capture.documentRevision==1711 && receipt.frame.capture.frameCount==receipt.frame.view.frameCount
                && std::memcmp(&receipt.frame.camera,&camera,sizeof(camera))==0, "editor receipt mixed completed identities");
            if (mode!=WAR3_EDITOR_PRESENTATION_STATIC) Require(receipt.skin.active && receipt.skin.result==0
                && receipt.skin.frameCount==receipt.frame.capture.frameCount && receipt.skin.contentVersion==target.contentVersion,
                "visibility fabricated pose version or frame");
            if (mode==WAR3_EDITOR_PRESENTATION_PARTICLES) Require(receipt.particles.contentVersion==target.contentVersion
                && receipt.particles.sceneVersion==target.sceneVersion, "visibility fabricated particle version");
            struct ExtendedCompleted { War3RenderEditorCompletedFrameV2 known; std::uint64_t sentinel; };
            ExtendedCompleted completed{{sizeof(ExtendedCompleted)}, 0x3141592653589793ull};
            Require(api->getEditorCompletedFrame(renderer.handle,&completed.known)==0
                && completed.sentinel==0x3141592653589793ull && completed.known.view.frameCount>=receipt.frame.view.frameCount
                && completed.known.view.surfaceEpoch==receipt.frame.view.surfaceEpoch
                && completed.known.view.sceneEpoch==receipt.frame.view.sceneEpoch && completed.known.view.viewEpoch==receipt.frame.view.viewEpoch
                && std::memcmp(&completed.known.camera,&receipt.frame.camera,sizeof(camera))==0
                && std::memcmp(&completed.known.presentation,&receipt.presentation,sizeof(receipt.presentation))==0,
                "metadata-only query mixed frames or overwrote future tail");
            if (mode!=WAR3_EDITOR_PRESENTATION_STATIC) Require(completed.known.skin.result==0
                && completed.known.skin.frameCount==completed.known.view.frameCount
                && completed.known.skin.contentVersion==target.contentVersion,"metadata pose is not the completed Draw");
            return pixels;
        };
        const auto first=capture(1); const auto original=receipt;
        RequireWorldColor(first,receipt.frame.view,500,0,{0,255,0},1);
        update.expected=target; update.presentationRevision=2; particleVisible=0;
        Require(api->submitEditorVisibility(renderer.handle,&update)==0,"particle hide rejected");
        RequireWorldColor(capture(2),receipt.frame.view,-500,0,{255,0,0},1);
        visible={0,1}; particleVisible=1; update.presentationRevision=3;
        Require(api->submitEditorVisibility(renderer.handle,&update)==0,"model hide rejected");
        const auto one=capture(3);
        if (mode==WAR3_EDITOR_PRESENTATION_PARTICLES) RequireWorldColor(one,receipt.frame.view,-500,0,{64,128,191},2);
        else Require(one!=first,"model visibility failed to alter pixels");
        visible={0,0}; particleVisible=0; update.presentationRevision=4;
        Require(api->submitEditorVisibility(renderer.handle,&update)==0,"all hide rejected"); visible={1,1}; particleVisible=1;
        Require(capture(4)==background && receipt.presentation.visibleInstanceCount==0 && receipt.presentation.visibleParticleDrawCount==0,
            "hidden models/particles remained or caller mask was borrowed");
        update.presentationRevision=5; Require(api->submitEditorVisibility(renderer.handle,&update)==0,"show restore rejected");
        Require(capture(5)==first && receipt.skin.staticUploadCount==original.skin.staticUploadCount
            && receipt.skin.matrixUploadCount==original.skin.matrixUploadCount
            && receipt.particles.textureUploadCount==original.particles.textureUploadCount
            && receipt.particles.dynamicUploadCount==original.particles.dynamicUploadCount,"mask-only changed assets/pose or failed restore");
        auto bad=update; bad.presentationRevision=6; bad.expected.contentVersion++;
        Require(api->submitEditorVisibility(renderer.handle,&bad)!=0,"stale content target accepted");
        Require(api->submitEditorVisibility(renderer.handle,&update)!=0,"stale presentation revision accepted");
        visible[0]=2; bad=update; bad.presentationRevision=6;
        Require(api->submitEditorVisibility(renderer.handle,&bad)!=0,"nonboolean visibility accepted"); visible[0]=1;
        const auto oldTarget=target; update.presentationRevision=6; fixture.Bind(11,20);
        Require(api->submitEditorModelScene(renderer.handle,&scene)==0,"same-count scene replacement rejected");
        Require(api->getEditorPresentationTarget(renderer.handle,&target)==0 && target.modelEpoch>oldTarget.modelEpoch,
            "same-count replacement reused model epoch");
        const auto replaced=capture(6); const auto replacedReceipt=receipt;
        bad=update; bad.expected=oldTarget; bad.presentationRevision=7;
        Require(api->submitEditorVisibility(renderer.handle,&bad)!=0,"old same-count mask accepted");
        std::vector<std::uint8_t> sentinel(320*240*4,0x71); auto untouched=receipt;
        Require(api->captureEditorPresentationFrameRgba(renderer.handle,oldTarget.modelEpoch,5,sentinel.data(),sentinel.size(),&receipt)==2
            && std::memcmp(&receipt,&untouched,sizeof(receipt))==0 && std::all_of(sentinel.begin(),sentinel.end(),[](auto v){return v==0x71;}),
            "stale editor capture overwrote output");
        Require(capture(6)==replaced,"rejected request changed editor pixels");
        if (mode!=WAR3_EDITOR_PRESENTATION_STATIC) {
            fixture.skin.groups[1].values[9]=700; fixture.Bind(11,21); visible={0,1}; particleVisible=0;
            update.expected=target; update.presentationRevision=7;
            if (mode==WAR3_EDITOR_PRESENTATION_PARTICLES) update.particleFrame=&fixture.frame; else update.modelPose=&fixture.pose.base;
            Require(api->submitEditorVisibility(renderer.handle,&update)==0,"atomic pose/mask update rejected");
            Require(api->getEditorPresentationTarget(renderer.handle,&target)==0 && target.contentVersion==21,"atomic content target wrong");
            const auto posed=capture(7); RequireWorldColor(posed,receipt.frame.view,700,0,{0,255,0},1);
            for (std::size_t row=0;row<240;++row) Require(std::equal(posed.begin()+row*320*4,posed.begin()+row*320*4+160*4,
                background.begin()+row*320*4),"atomic pose showed masked model/particle in left half");
            Require(receipt.skin.staticUploadCount==replacedReceipt.skin.staticUploadCount
                && receipt.skin.matrixUploadCount==replacedReceipt.skin.matrixUploadCount+1
                && receipt.particles.textureUploadCount==replacedReceipt.particles.textureUploadCount
                && (mode!=WAR3_EDITOR_PRESENTATION_PARTICLES || receipt.particles.dynamicUploadCount==replacedReceipt.particles.dynamicUploadCount+1),
                "atomic pose reuploaded assets or fabricated particle upload");
            update.modelPose=&fixture.pose.base; update.particleFrame=&fixture.frame; update.expected=target; update.presentationRevision=8;
            Require(api->submitEditorVisibility(renderer.handle,&update)!=0 && capture(7)==posed,"mutually exclusive dynamic inputs replaced frame");
        } else {
            instances[1].transform[3]=700; visible={0,1}; update.expected=target;
            update.presentationRevision=7; update.staticInstances=instances.data(); const auto epoch=target.modelEpoch;
            Require(api->submitEditorVisibility(renderer.handle,&update)==0,"atomic static instances/mask rejected");
            Require(api->getEditorPresentationTarget(renderer.handle,&target)==0 && target.modelEpoch==epoch+1,"static instance identity did not advance");
            const auto moved=capture(7); RequireWorldColor(moved,receipt.frame.view,700,0,{0,255,0},1);
            for (std::size_t row=0;row<240;++row) Require(std::equal(moved.begin()+row*320*4,moved.begin()+row*320*4+160*4,
                background.begin()+row*320*4),"static instance update showed masked model");
        }
        War3RenderEditorCompletedFrameV2 noMask{sizeof(noMask)};
        const auto unchanged=noMask;
        Require(api->submitModelScene(renderer.handle,&staticScene)==0,"legacy scene reset rejected");
        Require(api->getEditorCompletedFrame(renderer.handle,&noMask)==2
            && std::memcmp(&noMask,&unchanged,sizeof(noMask))==0,"metadata query accepted invalidated presentation or wrote pending output");
        noMask.structSize=sizeof(noMask)-1;
        Require(api->getEditorCompletedFrame(renderer.handle,&noMask)==1,"short completed metadata structure accepted");
    }
    std::cout << "editor WARP: static/skin/particle hide/show pixels, immutable uploads, atomic pose and epoch-bound receipt passed\n";
}

int wmain(int argc, wchar_t** argv) {
    HMODULE module = nullptr;
    try {
        const bool realPair = argc > 1 && std::wstring_view(argv[1]) == L"--real-terrain-pair";
        const bool realCliffGround = argc > 1 && std::wstring_view(argv[1]) == L"--real-cliff-ground";
        const bool modelSourceMips = argc > 1 && std::wstring_view(argv[1]) == L"--model-source-mips";
        for (int argument = 1; argument < argc; ++argument) {
            if (std::wstring_view(argv[argument]).starts_with(L"--real-terrain-pair"))
                Require(argument == 1 && realPair && argc == 5,
                    "real terrain pair requires the sole first option, manifest, SHA and empty output directory");
            if (realPair) Require(std::wstring_view(argv[argument]) != L"--hardware", "real terrain pair cannot use hardware mode");
            if (std::wstring_view(argv[argument]).starts_with(L"--real-cliff-ground"))
                Require(argument == 1 && realCliffGround && argc == 5,
                    "real cliff ground requires the sole first option, manifest, SHA and empty output directory");
            if (realCliffGround) Require(std::wstring_view(argv[argument]) != L"--hardware", "real cliff ground cannot use hardware mode");
            if (std::wstring_view(argv[argument]).starts_with(L"--model-source-mips"))
                Require(argument == 1 && modelSourceMips && (argc == 2 || argc == 3),
                    "model source mips requires the sole first option and optional new absolute output directory");
        }
        if (!modelSourceMips) {
            War3Map::Tests::RunRealTerrainPairValidationTests();
            War3Map::Tests::RunRealCliffGroundValidationTests();
        }
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        Require(length != 0 && length < 32768, "executable path unavailable");
        const auto dll = std::filesystem::path(executable).parent_path() / L"war3-render-d3d11.dll";
        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(module != nullptr, "renderer DLL load failed");
        const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(GetProcAddress(module, "War3MapEditor_GetRenderPluginApiV2"));
        Require(getApi != nullptr && getApi(99) == nullptr, "ABI version gate failed");
        const auto* api = getApi(WAR3_RENDER_PLUGIN_API_V2);
        Require(api && api->structSize == sizeof(*api), "V2 ABI unavailable");
        // 原创因果回归只执行CPU来源门与两张完成帧，不进入普通多帧/窗口/硬件测试。
        if (modelSourceMips) {
            ModelSourceMipTest::Run(api,dll,argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
            FreeLibrary(module); module = nullptr;
            return 0;
        }
        // 专用真实输入入口必须在普通测试、硬件和 HWND 生命周期之前返回。
        if (realPair) {
            const std::wstring wideSha(argv[3]);
            Require(wideSha.size() == 64 && std::all_of(wideSha.begin(), wideSha.end(), [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }), "real terrain pair SHA invalid");
            std::string sha; sha.reserve(wideSha.size());
            for (const auto character : wideSha) sha.push_back(static_cast<char>(character));
            War3Map::Tests::RunRealTerrainPairAudit(argv[2], sha, argv[4],
                [api](const War3RenderTerrainMipSceneV2& scene, const War3RenderCameraV2& camera,
                    std::uint32_t width, std::uint32_t height) {
                    Require(api->submitTerrainMipScene && api->setEnvironment, "real terrain pair requires original-mip/environment ABI");
                    const War3RenderCreateInfoV2 pairInfo{sizeof(pairInfo),width,height,1};
                    RendererOwner renderer{api,api->createRenderer(&pairInfo)};
                    Require(renderer.handle != nullptr, "real terrain pair WARP creation failed");
                    const War3RenderEnvironmentV2 environment{sizeof(environment),{0,0,1},{1,1,1},{0,0,0},
                        WAR3_RENDER_FOG_NONE,0,1,0,{0,0,0},0,0,0,0,1,0,nullptr};
                    Require(api->setEnvironment(renderer.handle,&environment) == 0 &&
                        api->setCamera(renderer.handle,&camera) == 0 && api->submitTerrainMipScene(renderer.handle,&scene) == 0,
                        "real terrain pair production scene rejected; no legacy fallback");
                    auto frame = CaptureFrame(renderer,width,height,scene.documentRevision,camera);
                    War3Map::Tests::RealTerrainPairFrame result;
                    result.pixels = std::move(frame.pixels); result.receipt = frame.receipt;
                    result.status.structSize = sizeof(result.status);
                    Require(api->getStatus(renderer.handle,&result.status) == 0, "real terrain pair status missing");
                    return result; // RendererOwner 在 DLL 卸载前析构。
                }, [](const std::filesystem::path& path, const std::vector<std::uint8_t>& pixels,
                    std::uint32_t width, std::uint32_t height) {
                    SaveBmp(path,pixels,static_cast<LONG>(width),static_cast<LONG>(height));
                });
            FreeLibrary(module); module = nullptr;
            std::cout << "real terrain pair source, geometry, atomic WARP capture and finite edge observations completed\n";
            return 0;
        }
        if (realCliffGround) {
            const std::wstring wideSha(argv[3]);
            Require(wideSha.size() == 64 && std::all_of(wideSha.begin(),wideSha.end(),[](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }), "real cliff ground SHA invalid");
            std::string sha;for (const auto c:wideSha) sha.push_back(static_cast<char>(c));
            const auto providerSha=War3Map::Sha256File(dll);
            Require(providerSha.Succeeded(),"real cliff ground actual provider SHA unavailable");
            War3Map::Tests::RunRealCliffGroundAudit(argv[2],sha,argv[4],
                [api,actualProvider=*providerSha.value](const War3RenderTerrainMipSceneV2& terrain,const War3RenderModelSceneV2& models,
                    const War3RenderCameraV2& camera,std::uint32_t width,std::uint32_t height) {
                    Require(api->submitTerrainMipScene && api->submitModelScene && api->setEnvironment,
                        "real cliff ground requires current terrain/model/environment ABI");
                    const War3RenderCreateInfoV2 create{sizeof(create),width,height,1};
                    RendererOwner renderer{api,api->createRenderer(&create)};
                    Require(renderer.handle != nullptr,"real cliff ground WARP creation failed");
                    Require(War3Map::Render::CheckStaticModelSamplerCapability(api,renderer.handle).Succeeded(),
                        "real cliff ground source sampler backend capability missing");
                    const War3RenderEnvironmentV2 environment{sizeof(environment),{0,0,1},{1,1,1},{0,0,0},
                        WAR3_RENDER_FOG_NONE,0,1,0,{0,0,0},0,0,0,0,1,0,nullptr};
                    Require(api->setEnvironment(renderer.handle,&environment) == 0 &&
                        api->submitTerrainMipScene(renderer.handle,&terrain) == 0 &&
                        api->submitModelScene(renderer.handle,&models) == 0 && api->setCamera(renderer.handle,&camera) == 0,
                        "real cliff ground production packet pair rejected; no partial capture");
                    auto captured=CaptureFrame(renderer,width,height,terrain.documentRevision,camera);
                    War3Map::Tests::RealTerrainPairFrame frame;
                    frame.pixels=std::move(captured.pixels);frame.receipt=captured.receipt;frame.providerSha256=actualProvider;
                    frame.status.structSize=sizeof(frame.status);
                    Require(api->getStatus(renderer.handle,&frame.status) == 0,"real cliff ground status missing");return frame;
                },[](const std::filesystem::path& path,const std::vector<std::uint8_t>& pixels,std::uint32_t width,std::uint32_t height) {
                    SaveBmp(path,pixels,static_cast<LONG>(width),static_cast<LONG>(height));
                });
            FreeLibrary(module);module=nullptr;
            std::cout << "real cliff ground source, production ramp/selection/coverage and same-provider frame completed\n";
            return 0;
        }
        const bool hardware = argc > 1 && std::wstring_view(argv[1]) == L"--hardware";
        const War3RenderCreateInfoV2 info{sizeof(info), 320, 240, hardware ? 0u : 1u};
        auto invalid = info; invalid.width = 0;
        Require(api->createRenderer(&invalid) == nullptr, "invalid dimensions accepted");
        TestOldPrefix(api, info);
        TestFrameMetrics(api, info);
        TestAtomicCapture(api, info);
        TestSkinnedModels(api, info);
        TestMaterialLayerAlphas(api, info);
        TestMaterialStates(api, info);
        TestUnfoggedMaterials(api, info);
        TestStaticLayerSamplers(api, info);
        TestModelObliqueSampler(api, info);
        TestTerrainObliqueFilter(api, info);
        TestMaterialLayerFaults(dll.parent_path(), info);
        TestAdditionalModelLayer(api, info);
        TestBuildingSplatLayer(api, info);
        TestAnimationServiceWARP(api, info);
        TestParticleModels(api, info);
        TestParticleUnfoggedMaterials(api, info);
        TestOwnedStaticHelper(api, info);
        TestEditorVisibility(api, info);
        TestParticleFaults(dll.parent_path(), module, info);
        TestInstanceRoster(api, info);
        TestInstanceRosterFaults(dll.parent_path(), info);
        TestAssetAppend(api, info);
        TestAssetAppend(api, info, nullptr, nullptr, true);
        TestAssetAppendFaults(dll.parent_path(), info);
        std::vector<std::uint8_t> screenshot;
        for (int round = 0; round < 2; ++round) {
            RendererOwner renderer{api, api->createRenderer(&info)};
            Require(renderer.handle != nullptr, "renderer creation failed");
            const auto grid = Capture(renderer, 320, 240, 0);
            const auto gridView = ReadView(renderer);
            Require(Colors(grid) > 100, "empty scene grid was not drawn");
            War3RenderVertexV2 vertices[]{{-1000,-1000,0,1,0,0},{1000,-1000,0,0,1,0},{0,1000,700,0,0,1}};
            const std::uint32_t indices[]{0,1,2};
            const War3RenderMeshV2 mesh{sizeof(mesh), 3, 3, 42, vertices, indices};
            Require(api->submitMesh(renderer.handle, &mesh) == 0, "mesh rejected");
            RequireLatestOrPending(renderer, gridView, 1);
            // 提交后立即覆盖调用方数据，帧仍必须使用其不可变副本。
            for (auto& vertex : vertices) vertex = {};
            War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.1f,0.8f};
            Require(api->setCamera(renderer.handle, &camera) == 0, "valid camera rejected");
            auto first = Capture(renderer, 320, 240);
            const auto firstView = ReadView(renderer);
            RequireExactMatrix(firstView, camera);
            Require(Colors(first) > 1500 && first != grid, "mesh snapshot not drawn");
            camera.yawRadians += 0.8f;
            Require(api->setCamera(renderer.handle, &camera) == 0, "orbit rejected");
            RequireLatestOrPending(renderer, firstView, 2);
            const auto rotated = Capture(renderer, 320, 240);
            const auto rotatedView = ReadView(renderer);
            RequireExactMatrix(rotatedView, camera);
            Require(rotated != first, "camera orbit did not change pixels");
            camera.pitchRadians = std::numeric_limits<float>::quiet_NaN();
            Require(api->setCamera(renderer.handle, &camera) != 0, "NaN camera accepted");
            auto bad = mesh; const std::uint32_t badIndices[]{0,1,3}; bad.indices = badIndices;
            Require(api->submitMesh(renderer.handle, &bad) != 0, "out of range index accepted");
            Require(Capture(renderer, 320, 240) == rotated, "invalid request corrupted previous frame");
            Require(api->resize(renderer.handle, 160, 120) == 0, "resize rejected");
            RequireLatestOrPending(renderer, rotatedView, 0);
            screenshot = Capture(renderer, 160, 120);
            Require(Colors(screenshot) > 300, "resized scene missing");
            War3RenderStatusV2 status{sizeof(status)};
            Require(api->getStatus(renderer.handle, &status) == 0 && status.deviceReady == 1 && status.result == 0 &&
                status.documentRevision == 42 && status.frameCount >= 5 && status.width == 160 && status.height == 120 &&
                status.software == (hardware ? 0u : 1u), "status identity incorrect");
            std::cout << status.message << " frames=" << status.frameCount << " revision=" << status.documentRevision << '\n';
            screenshot = TestTerrain(renderer); // 外层两轮销毁/重建，在硬件与WARP入口均执行。
            TestTerrainMips(renderer);
            TestTerrainZeroAlphaContribution(renderer);
            TestModels(renderer);
            TestEnvironment(renderer);
        }
        // 真正的 HWND 路径：创建后立即关闭及 resize/detach 请求竞争。
        // CTest 的超时门禁捕获 UI join 与 DXGI SendMessage 的互等；窗口始终隐藏。
        for (int round = 0; round < 8; ++round) {
            const HWND window = CreateWindowExW(0, L"STATIC", L"FORGE renderer lifecycle test", WS_OVERLAPPEDWINDOW,
                0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(window != nullptr, "native lifecycle HWND unavailable");
            {
                RendererOwner renderer{api, api->createRenderer(&info)};
                Require(renderer.handle && api->attachNativeWindow(renderer.handle, window) == 0, "native attach failed");
                if (round == 7) Require(Colors(Capture(renderer, 320, 240, 0)) > 100, "UI-thread native capture failed");
                Sleep(static_cast<DWORD>(round * 3));
                Require(api->resize(renderer.handle, 200, 160) == 0, "native resize failed");
                if (round % 2) Require(api->attachNativeWindow(renderer.handle, nullptr) == 0, "detach failed");
            }
            Require(DestroyWindow(window) != 0, "native lifecycle HWND not released");
        }
        if (argc > 2) SaveBmp(argv[2], screenshot, 160, 120);
        FreeLibrary(module);
        std::cout << "D3D11 grid, copied mesh, orbit, invalid-input preservation, resize, recreate and native close passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (module) FreeLibrary(module);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
