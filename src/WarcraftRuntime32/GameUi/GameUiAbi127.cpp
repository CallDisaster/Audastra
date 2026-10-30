#include "GameUiAbi127.h"

#include <cstdio>
#include <limits>
#include <cwchar>
#include <windows.h>

namespace Audastra::Runtime32::GameUi {
namespace {

bool ReadOwnUiCursor(float& x, float& y) {
    const auto window = GetForegroundWindow();
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(window, &process);
    wchar_t name[64]{};
    if (!window || process != GetCurrentProcessId() || thread != GetCurrentThreadId()
        || GetAncestor(window, GA_ROOT) != window || GetWindow(window, GW_OWNER) || IsIconic(window)
        || !GetClassNameW(window, name, 64) || std::wcscmp(name, L"Warcraft III") != 0) return false;
    using GetContext = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
    using SetContext = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static const auto getContext = reinterpret_cast<GetContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext"));
    static const auto setContext = reinterpret_cast<SetContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    const auto previous = getContext && setContext ? setContext(getContext(window)) : nullptr;
    if ((getContext || setContext) && !previous) return false;
    struct RestoreDpi {
        SetContext set; DPI_AWARENESS_CONTEXT previous;
        ~RestoreDpi() { if (previous) set(previous); }
    } restore{setContext, previous};
    POINT point{}; RECT client{};
    if (!GetCursorPos(&point) || !ScreenToClient(window, &point) || !GetClientRect(window, &client)
        || client.right <= 0 || client.bottom <= 0 || point.x < 0 || point.y < 0
        || point.x >= client.right || point.y >= client.bottom) return false;
    x = 0.8f * static_cast<float>(point.x) / static_cast<float>(client.right);
    y = 0.6f * (1.0f - static_cast<float>(point.y) / static_cast<float>(client.bottom));
    return true;
}

class Resolver {
public:
    Resolver(Address base, const std::function<bool(Address, std::size_t)>& executable)
        : base_(base), executable_(executable) {}

    template <class Fn>
    void Code(Fn& field, std::uint32_t rva) {
        const auto address = At(rva);
        if (!address || !executable_(address, 1)) {
            Fail(rva);
            return;
        }
        field = reinterpret_cast<Fn>(address);
    }

    void CodeAddress(Address& field, std::uint32_t rva) {
        const auto address = At(rva);
        if (!address || !executable_(address, 1)) {
            Fail(rva);
            return;
        }
        field = address;
    }

    void Data(Address& field, std::uint32_t rva) {
        field = At(rva);
        if (!field) Fail(rva);
    }

    [[nodiscard]] bool Ok() const noexcept { return failed_ == 0; }
    [[nodiscard]] std::uint32_t Failed() const noexcept { return failed_; }

private:
    [[nodiscard]] Address At(std::uint32_t rva) const noexcept {
        if (!base_ || rva > std::numeric_limits<Address>::max() - base_) return 0;
        return base_ + rva;
    }
    void Fail(std::uint32_t rva) noexcept {
        if (!failed_) failed_ = rva ? rva : 1;
    }

    Address base_;
    const std::function<bool(Address, std::size_t)>& executable_;
    std::uint32_t failed_ = 0;
};

} // namespace

bool ResolveGameUiEntryPoints127(Address base, const std::function<bool(Address, std::size_t)>& executable,
    GameUiEntryPoints& out, std::string& error) {
    if (!base || !executable) {
        error = "没有已核验的 Game.dll 映像，界面库不可用。";
        return false;
    }
    GameUiEntryPoints table;
    Resolver r(base, executable);
    using namespace Rva127;
    r.Code(table.getGameUi, GetGameUi);
    r.Code(table.createFrame, CreateFrame);
    r.Code(table.createSimpleFrame, CreateSimpleFrame);
    r.Code(table.findFrame, FindFrame);
    r.Code(table.findSimpleFrame, FindSimpleFrame);
    r.Code(table.findSimpleTexture, FindSimpleTexture);
    r.Code(table.findSimpleFontString, FindSimpleFontString);
    r.Code(table.loadToc, LoadToc);
    r.Code(table.growStringTable, GrowStringTable);
    r.Code(table.growFdfTable, GrowFdfTable);

    r.Code(table.setAbsPoint, SetAbsPoint);
    r.Code(table.setRelPoint, SetRelPoint);
    r.Code(table.setAllPoints, SetAllPoints);
    r.Code(table.clearAllPoints, ClearAllPoints);
    r.Code(table.setWidth, SetWidth);
    r.Code(table.setHeight, SetHeight);
    r.Code(table.setScale, SetScale);
    r.CodeAddress(table.layoutHeight, LayoutHeight);
    r.CodeAddress(table.fontStringHeight, FontStringHeight);

    r.Code(table.layerSetAlpha, LayerSetAlpha);
    r.Code(table.layerSetParent, LayerSetParent);
    r.Code(table.layerSetFont, LayerSetFont);
    r.Code(table.simpleSetAlpha, SimpleSetAlpha);
    r.Code(table.simpleSetParent, SimpleSetParent);
    r.Code(table.simpleSetLevel, SimpleSetLevel);
    r.Code(table.regionShow, RegionShow);
    r.Code(table.regionHide, RegionHide);
    r.Code(table.regionSetColor, RegionSetColor);

    r.Code(table.backdropSetTexture, BackdropSetTexture);
    r.Code(table.textSetText, TextSetText);
    r.Code(table.textSetColor, TextSetColor);
    r.Code(table.textSetAlignH, TextSetAlignH);
    r.Code(table.textSetAlignV, TextSetAlignV);
    r.Code(table.textUpdateAlign, TextUpdateAlign);
    r.Code(table.fontStringSetText, FontStringSetText);
    r.Code(table.fontStringSetFont, FontStringSetFont);
    r.Code(table.fontStringSetAlignH, FontStringSetAlignH);
    r.Code(table.fontStringSetAlignV, FontStringSetAlignV);
    r.Code(table.textureSetTexture, TextureSetTexture);
    r.Code(table.statusBarSetTexture, StatusBarSetTexture);
    r.Code(table.statusBarSetValue, StatusBarSetValue);
    r.Code(table.statusBarSetLimits, StatusBarSetLimits);
    r.Code(table.sliderSetValue, SliderSetValue);
    r.Code(table.editBoxSetText, EditBoxSetText);
    r.Code(table.modelSetModel, ModelSetModel);
    r.Code(table.controlSetEnabled, ControlSetEnabled);

    r.Data(table.stringTable, StringTable);
    r.Data(table.fdfTable, FdfTable);
    r.Data(table.defaultStatus, DefaultStatus);
    r.Data(table.activeLayer, ActiveLayer);
    r.Data(table.mouseEvent, MouseEvent);
    r.Data(table.mouseEventVtable, MouseEventVtable);
    table.readCursorUi = &ReadOwnUiCursor;
    r.Code(table.hitTestLayer, HitTestLayer);
    r.Data(table.simpleTop, SimpleTop);

    r.Data(table.vtBackdrop, VtBackdrop);
    r.Data(table.vtText, VtText);
    r.Data(table.vtEditBox, VtEditBox);
    r.Data(table.vtSlider, VtSlider);
    r.Data(table.vtModel, VtModel);
    r.Data(table.vtSimpleStatusBar, VtSimpleStatusBar);
    r.Data(table.vtSimpleTexture, VtSimpleTexture);
    r.Data(table.vtSimpleFontString, VtSimpleFontString);

    if (!r.Ok()) {
        char text[96]{};
        std::snprintf(text, sizeof(text), "Game.dll 偏移 0x%06X 不在可执行代码段，界面库不可用。", r.Failed());
        error = text;
        return false;
    }
    out = table;
    error.clear();
    return true;
}

} // namespace Audastra::Runtime32::GameUi
