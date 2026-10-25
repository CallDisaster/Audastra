#include "WindowFrameCapture.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace War3Map::QtApp {
namespace {
namespace Capture = winrt::Windows::Graphics::Capture;
namespace DirectX = winrt::Windows::Graphics::DirectX;

struct Apartment {
    bool owned = SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED));
    ~Apartment() { if (owned) RoUninitialize(); }
};

winrt::com_ptr<ID3D11Device> CreateDevice(winrt::com_ptr<ID3D11DeviceContext>& context) {
    winrt::com_ptr<ID3D11Device> device;
    for (const auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (SUCCEEDED(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, device.put(), nullptr, context.put())))
            return device;
        device = nullptr; context = nullptr;
    }
    return device;
}

// 不带系统黄色边框；系统不认这项权限时照常带边框捕获。
void RequestBorderless(const Capture::GraphicsCaptureSession& session) {
    try {
        Capture::GraphicsCaptureAccess::RequestAccessAsync(Capture::GraphicsCaptureAccessKind::Borderless).get();
        session.IsBorderRequired(false);
    } catch (...) {
    }
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
}
} // namespace

WindowFrame CaptureWindowFrame(HWND window, int timeoutMilliseconds, std::string& error) {
    const Apartment apartment;
    try {
        if (!Capture::GraphicsCaptureSession::IsSupported()) { error = "系统不支持窗口图形捕获。"; return {}; }
        winrt::com_ptr<ID3D11DeviceContext> context;
        const auto device = CreateDevice(context);
        if (!device) { error = "无法创建 D3D11 设备。"; return {}; }
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), inspectable.put()));
        const auto direct3d = inspectable.as<DirectX::Direct3D11::IDirect3DDevice>();

        const auto interop = winrt::get_activation_factory<Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        Capture::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        const auto pool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(direct3d,
            DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
        const auto session = pool.CreateCaptureSession(item);
        RequestBorderless(session);
        session.StartCapture();
        const auto closeAll = [&] { session.Close(); pool.Close(); };

        // 只认开始捕获之后系统交来的帧。
        Capture::Direct3D11CaptureFrame frame{nullptr};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
        while (!(frame = pool.TryGetNextFrame()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!frame) { closeAll(); error = "等待窗口画面超时。"; return {}; }

        winrt::com_ptr<ID3D11Texture2D> texture;
        const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ; description.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
        context->CopyResource(staging.get(), texture.get());

        // 捕获帧是整个窗口的可见范围，按客户区裁出游戏画面。
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof bounds))) GetWindowRect(window, &bounds);
        RECT client{};
        POINT origin{0, 0};
        GetClientRect(window, &client); ClientToScreen(window, &origin);
        const auto content = frame.ContentSize();
        const int available = std::min<int>(content.Width, static_cast<int>(description.Width));
        const int rows = std::min<int>(content.Height, static_cast<int>(description.Height));
        const int left = std::clamp<int>(origin.x - bounds.left, 0, available);
        const int top = std::clamp<int>(origin.y - bounds.top, 0, rows);
        WindowFrame result;
        result.width = std::min<int>(client.right, available - left);
        result.height = std::min<int>(client.bottom, rows - top);
        if (result.width <= 0 || result.height <= 0) { frame.Close(); closeAll(); error = "窗口客户区为空。"; return {}; }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        result.bgra.resize(static_cast<std::size_t>(result.width) * result.height * 4);
        for (int y = 0; y < result.height; ++y)
            std::memcpy(result.bgra.data() + static_cast<std::size_t>(y) * result.width * 4,
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(top + y) * mapped.RowPitch + static_cast<std::size_t>(left) * 4,
                static_cast<std::size_t>(result.width) * 4);
        context->Unmap(staging.get(), 0);
        frame.Close(); closeAll();
        return result;
    } catch (const winrt::hresult_error& failure) {
        error = winrt::to_string(failure.message());
        if (error.empty()) error = "窗口图形捕获失败。";
        return {};
    }
}

} // namespace War3Map::QtApp
