#include "../GameUi/GameUiLibrary.h"
#include "../GameUi/GameUiModule.h"

#include "aj/Protocol.h"

#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// 替身游戏：对象首字是虚表，虚表槽和游戏一致；替身函数与游戏函数同调用约定，记录收到的参数。
namespace {
using namespace Audastra::Runtime32::GameUi;

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<std::string> g_log;
UiFault g_fault{};
GameUiDataPlane* g_nestedData = nullptr;
FrameHandle g_nestedFrame = 0;
bool g_nestedWrite = false;
unsigned g_faultCount = 0;
void ObserveFault(const UiFault& fault) noexcept { g_fault = fault; ++g_faultCount; }
void Log(const char* format, ...) {
    char text[256]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_log.emplace_back(text);
}
bool Logged(const std::string& line) {
    for (const auto& item : g_log) if (item == line) return true;
    return false;
}

struct FakeFrame {
    Address vtable = 0;
    std::array<unsigned char, 0x400> body{};
    Address At(std::uint32_t offset) const { return *reinterpret_cast<const Address*>(reinterpret_cast<const unsigned char*>(this) + offset); }
    void Set(std::uint32_t offset, Address value) { *reinterpret_cast<Address*>(reinterpret_cast<unsigned char*>(this) + offset) = value; }
    Address Self() const { return reinterpret_cast<Address>(this); }
};

using Vtable = std::array<Address, 64>;
Vtable vtClassic{}, vtText{}, vtBackdrop{}, vtEditBox{}, vtSlider{}, vtModel{}, vtSimple{}, vtStatusBar{}, vtTexture{},
    vtFontString{};

std::vector<std::unique_ptr<FakeFrame>> g_frames;
FakeFrame* g_root = nullptr;
FakeFrame* g_world = nullptr;
FakeFrame* g_named = nullptr;
Address g_lastParent = 0;
int g_tocResult = 1;

FakeFrame* NewFrame(const Vtable& vtable, Address parent, std::uint32_t parentOffset) {
    g_frames.push_back(std::make_unique<FakeFrame>());
    auto* frame = g_frames.back().get();
    frame->vtable = reinterpret_cast<Address>(vtable.data());
    if (parent) frame->Set(parentOffset, parent);
    return frame;
}

std::string Hex(Address value) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(value));
    return text;
}

// 只用来比对虚表 0x1C 槽，不会被调用。函数体各不相同，免得链接器把它们合并成同一个地址。
int __fastcall FakeLayoutHeight(Address) { return 1; }
int __fastcall FakeFontHeight(Address) { return 2; }
int __fastcall FakeClassicHeight(Address) { return 3; }

int __fastcall VShow(Address self) { Log("show %s", Hex(self).c_str()); return 1; }
int __fastcall VHide(Address self) { Log("hide %s", Hex(self).c_str()); return 1; }
int __fastcall VDelete(Address self, int, int flag) { Log("delete %s %d", Hex(self).c_str(), flag); return 1; }

Address __fastcall GetGameUi(int a, int b) { Log("gameui %d %d", a, b); return g_root ? g_root->Self() : 0; }
Address __fastcall CreateFrame(const char* name, Address parent, int zero, int priority, int context) {
    Log("create %s %s %d %d %d", name, Hex(parent).c_str(), zero, priority, context);
    g_lastParent = parent;
    const std::string text(name);
    if (text == "Text") return NewFrame(vtText, parent, Layout127::ClassicParent)->Self();
    if (text == "Backdrop") return NewFrame(vtBackdrop, parent, Layout127::ClassicParent)->Self();
    if (text == "Edit") return NewFrame(vtEditBox, parent, Layout127::ClassicParent)->Self();
    if (text == "Slider") return NewFrame(vtSlider, parent, Layout127::ClassicParent)->Self();
    if (text == "Model") return NewFrame(vtModel, parent, Layout127::ClassicParent)->Self();
    if (text == "Button") return NewFrame(vtClassic, parent, Layout127::ClassicParent)->Self();
    if (text == "Crash") *reinterpret_cast<volatile int*>(0) = 1;
    return 0;
}
Address __fastcall CreateSimpleFrame(const char* name, Address parent, int context) {
    Log("createsimple %s %s %d", name, Hex(parent).c_str(), context);
    const std::string text(name);
    if (text == "Bar") return NewFrame(vtStatusBar, parent, Layout127::SimpleParent)->Self();
    if (text == "Simple") return NewFrame(vtSimple, parent, Layout127::SimpleParent)->Self();
    return 0;
}
Address __fastcall FindNone(const char* name, int context) { Log("find0 %s %d", name, context); return 0; }
Address __fastcall FindSimple(const char* name, int context) {
    Log("find1 %s %d", name, context);
    return std::string(name) == "Named" && g_named ? g_named->Self() : 0;
}
int __fastcall LoadToc(const char* path, Address strings, Address fdf, Address status) {
    Log("toc %s %s %s %s", path, Hex(strings).c_str(), Hex(fdf).c_str(), Hex(status).c_str());
    return g_tocResult;
}
int __fastcall GrowStrings(Address table, int, int capacity) { Log("grows %s %d", Hex(table).c_str(), capacity); return 1; }
int __fastcall GrowFdf(Address table, int, int capacity) { Log("growf %s %d", Hex(table).c_str(), capacity); return 1; }

int __fastcall AbsPoint(Address layout, int, int point, float x, float y, int update) {
    Log("abs %s %d %.3f %.3f %d", Hex(layout).c_str(), point, x, y, update); return 1;
}
int __fastcall RelPoint(Address layout, int, int point, Address relative, int relativePoint, float x, float y, int update) {
    Log("rel %s %d %s %d %.3f %.3f %d", Hex(layout).c_str(), point, Hex(relative).c_str(), relativePoint, x, y, update);
    return 1;
}
int __fastcall AllPoints(Address layout, int, Address relative, int update) {
    Log("all %s %s %d", Hex(layout).c_str(), Hex(relative).c_str(), update); return 1;
}
int __fastcall ClearPoints(Address layout, int, int update) { Log("clear %s %d", Hex(layout).c_str(), update); return 1; }
int __fastcall Width(Address layout, int, float value) { Log("width %s %.3f", Hex(layout).c_str(), value); return 1; }
int __fastcall Height(Address layout, int, float value) { Log("height %s %.3f", Hex(layout).c_str(), value); return 1; }
int __fastcall Scale(Address layout, int, float value) { Log("scale %s %.3f", Hex(layout).c_str(), value); return 1; }
int __fastcall LayerAlpha(Address self, int, int alpha, int zero) { Log("lalpha %s %d %d", Hex(self).c_str(), alpha, zero); return 1; }
int __fastcall LayerParent(Address self, int, Address parent, int update, int zero) {
    Log("lparent %s %s %d %d", Hex(self).c_str(), Hex(parent).c_str(), update, zero); return 1;
}
int __fastcall LayerFont(Address self, int, const char* font, float height, int flags) {
    Log("lfont %s %s %.3f %d", Hex(self).c_str(), font, height, flags); return 1;
}
int __fastcall SimpleAlpha(Address self, int, int alpha) { Log("salpha %s %d", Hex(self).c_str(), alpha); return 1; }
int __fastcall SimpleParent(Address self, int, Address parent) { Log("sparent %s %s", Hex(self).c_str(), Hex(parent).c_str()); return 1; }
int __fastcall SimpleLevel(Address self, int, int level) { Log("slevel %s %d", Hex(self).c_str(), level); return 1; }
int __fastcall RegionShow(Address self) { Log("rshow %s", Hex(self).c_str()); return 1; }
int __fastcall RegionHide(Address self) { Log("rhide %s", Hex(self).c_str()); return 1; }
int __fastcall RegionColor(Address self, int, const std::uint32_t* argb) { Log("rcolor %s %08X", Hex(self).c_str(), *argb); return 1; }
int __fastcall BackdropTexture(Address self, int, const char* path, int zero, int tile, Address none, int update) {
    Log("btex %s %s %d %d %s %d", Hex(self).c_str(), path, zero, tile, Hex(none).c_str(), update); return 1;
}
int __fastcall TextText(Address self, int, const char* text) {
    Log("ttext %s %s", Hex(self).c_str(), text);
    if (g_nestedData) g_nestedWrite = g_nestedData->SaveBoolean(1234, g_nestedFrame, 5, 0).applied;
    return 1;
}
int __fastcall TextColor(Address self, int, const std::uint32_t* argb) { Log("tcolor %s %08X", Hex(self).c_str(), *argb); return 1; }
int __fastcall TextAlignH(Address self, int, int value) { Log("talignh %s %d", Hex(self).c_str(), value); return 1; }
int __fastcall TextAlignV(Address self, int, int value) { Log("talignv %s %d", Hex(self).c_str(), value); return 1; }
int __fastcall TextAlignUpdate(Address self, int) { Log("talignu %s", Hex(self).c_str()); return 1; }
int __fastcall FontText(Address self, int, const char* text) { Log("ftext %s %s", Hex(self).c_str(), text); return 1; }
int __fastcall FontFont(Address self, int, const char* font, float height, int flags) {
    Log("ffont %s %s %.3f %d", Hex(self).c_str(), font, height, flags); return 1;
}
int __fastcall FontAlignH(Address self, int, int value) { Log("falignh %s %d", Hex(self).c_str(), value); return 1; }
int __fastcall FontAlignV(Address self, int, int value) { Log("falignv %s %d", Hex(self).c_str(), value); return 1; }
int __fastcall TextureTexture(Address self, int, const char* path, int tile) { Log("stex %s %s %d", Hex(self).c_str(), path, tile); return 1; }
int __fastcall BarTexture(Address self, int, const char* path, int tile) { Log("bartex %s %s %d", Hex(self).c_str(), path, tile); return 1; }
int __fastcall BarValue(Address self, int, float value) { Log("barvalue %s %.3f", Hex(self).c_str(), value); return 1; }
int __fastcall BarLimits(Address self, int, float minimum, float maximum) {
    Log("barlimits %s %.3f %.3f", Hex(self).c_str(), minimum, maximum); return 1;
}
int __fastcall SliderValue(Address self, int, float value, int update) { Log("slider %s %.3f %d", Hex(self).c_str(), value, update); return 1; }
int __fastcall EditText(Address self, int, const char* text, int update) { Log("etext %s %s %d", Hex(self).c_str(), text, update); return 1; }
int __fastcall ModelModel(Address self, int, const char* path, int type) { Log("model %s %s %d", Hex(self).c_str(), path, type); return 1; }
int __fastcall Enabled(Address self, int, int value) { Log("enabled %s %d", Hex(self).c_str(), value); return 1; }

struct Tables {
    std::array<Address, 8> strings{};
    std::array<Address, 8> fdf{};
    Address status = 0;
    Address hovered = 0;
    Address pressed = 0;
    Address simpleTop = 0;
} g_tables;

void FillVtable(Vtable& vtable, Address height, bool classic) {
    vtable.fill(0);
    vtable[Layout127::SlotHeight / 4] = height;
    vtable[Layout127::SlotDelete / 4] = reinterpret_cast<Address>(&VDelete);
    if (classic) {
        vtable[Layout127::SlotClassicHide / 4] = reinterpret_cast<Address>(&VHide);
        vtable[Layout127::SlotClassicShow / 4] = reinterpret_cast<Address>(&VShow);
        vtable[Layout127::SlotClassicDelete / 4] = reinterpret_cast<Address>(&VDelete);
    } else {
        vtable[Layout127::SlotSimpleHide / 4] = reinterpret_cast<Address>(&VHide);
        vtable[Layout127::SlotSimpleShow / 4] = reinterpret_cast<Address>(&VShow);
    }
}

GameUiEntryPoints Entries() {
    const auto classic = reinterpret_cast<Address>(&FakeClassicHeight);
    const auto layout = reinterpret_cast<Address>(&FakeLayoutHeight);
    const auto font = reinterpret_cast<Address>(&FakeFontHeight);
    for (auto* vt : {&vtClassic, &vtText, &vtBackdrop, &vtEditBox, &vtSlider, &vtModel}) FillVtable(*vt, classic, true);
    for (auto* vt : {&vtSimple, &vtStatusBar, &vtTexture}) FillVtable(*vt, layout, false);
    FillVtable(vtFontString, font, false);
    GameUiEntryPoints e;
    e.getGameUi = &GetGameUi;
    e.createFrame = &CreateFrame;
    e.createSimpleFrame = &CreateSimpleFrame;
    e.findFrame = &FindNone;
    e.findSimpleFrame = &FindSimple;
    e.findSimpleTexture = &FindNone;
    e.findSimpleFontString = &FindNone;
    e.loadToc = &LoadToc;
    e.growStringTable = &GrowStrings;
    e.growFdfTable = &GrowFdf;
    e.setAbsPoint = &AbsPoint;
    e.setRelPoint = &RelPoint;
    e.setAllPoints = &AllPoints;
    e.clearAllPoints = &ClearPoints;
    e.setWidth = &Width;
    e.setHeight = &Height;
    e.setScale = &Scale;
    e.layoutHeight = layout;
    e.fontStringHeight = font;
    e.layerSetAlpha = &LayerAlpha;
    e.layerSetParent = &LayerParent;
    e.layerSetFont = &LayerFont;
    e.simpleSetAlpha = &SimpleAlpha;
    e.simpleSetParent = &SimpleParent;
    e.simpleSetLevel = &SimpleLevel;
    e.regionShow = &RegionShow;
    e.regionHide = &RegionHide;
    e.regionSetColor = &RegionColor;
    e.backdropSetTexture = &BackdropTexture;
    e.textSetText = &TextText;
    e.textSetColor = &TextColor;
    e.textSetAlignH = &TextAlignH;
    e.textSetAlignV = &TextAlignV;
    e.textUpdateAlign = &TextAlignUpdate;
    e.fontStringSetText = &FontText;
    e.fontStringSetFont = &FontFont;
    e.fontStringSetAlignH = &FontAlignH;
    e.fontStringSetAlignV = &FontAlignV;
    e.textureSetTexture = &TextureTexture;
    e.statusBarSetTexture = &BarTexture;
    e.statusBarSetValue = &BarValue;
    e.statusBarSetLimits = &BarLimits;
    e.sliderSetValue = &SliderValue;
    e.editBoxSetText = &EditText;
    e.modelSetModel = &ModelModel;
    e.controlSetEnabled = &Enabled;
    e.stringTable = reinterpret_cast<Address>(g_tables.strings.data());
    e.fdfTable = reinterpret_cast<Address>(g_tables.fdf.data());
    e.defaultStatus = reinterpret_cast<Address>(&g_tables.status);
    e.hoveredLayer = reinterpret_cast<Address>(&g_tables.hovered);
    e.pressedLayer = reinterpret_cast<Address>(&g_tables.pressed);
    e.simpleTop = reinterpret_cast<Address>(&g_tables.simpleTop);
    e.vtBackdrop = reinterpret_cast<Address>(vtBackdrop.data());
    e.vtText = reinterpret_cast<Address>(vtText.data());
    e.vtEditBox = reinterpret_cast<Address>(vtEditBox.data());
    e.vtSlider = reinterpret_cast<Address>(vtSlider.data());
    e.vtModel = reinterpret_cast<Address>(vtModel.data());
    e.vtSimpleStatusBar = reinterpret_cast<Address>(vtStatusBar.data());
    e.vtSimpleTexture = reinterpret_cast<Address>(vtTexture.data());
    e.vtSimpleFontString = reinterpret_cast<Address>(vtFontString.data());
    return e;
}

void Reset() {
    g_log.clear();
    g_frames.clear();
    g_tables = {};
    g_root = NewFrame(vtClassic, 0, 0);
    g_world = NewFrame(vtClassic, 0, 0);
    g_root->Set(Layout127::GameUiWorldFrame, g_world->Self());
    g_named = nullptr;
    g_tocResult = 1;
}

void IncompleteTable() {
    GameUiEntryPoints empty;
    GameUiLibrary ui(empty);
    Check(ui.Root().error == UiError::NotReady, "incomplete entry table reports not ready");
    Check(ui.SetVisible(1, true) == UiError::NotReady, "operations refuse without entry table");
}

void CreateAndLayout() {
    Reset();
    GameUiLibrary ui(Entries());
    const auto text = ui.Create("Text", 0, 3, 7);
    Check(text.error == UiError::None && text.handle > 0, "create text frame from template");
    Check(g_lastParent == g_root->Self(), "missing parent attaches to game UI root");
    Check(Logged("create Text " + Hex(g_root->Self()) + " 0 3 7"), "create passes template, parent, level and context");
    const auto* textObject = g_frames.back().get();
    const auto missing = ui.Create("Missing", 0, 0, 0);
    Check(missing.error == UiError::NotFound && missing.handle == 0, "unknown template is reported");

    const auto bar = ui.CreateSimple("Bar", 0, 0);
    Check(bar.error == UiError::None, "create simple status bar");
    const auto* barObject = g_frames.back().get();

    Check(ui.SetAbsPoint(text.handle, 4, 0.4f, 0.3f) == UiError::None, "absolute point");
    Check(Logged("abs " + Hex(textObject->Self() + Layout127::ClassicLayout) + " 4 0.400 0.300 1"),
        "classic frame positions through its layout object");
    Check(ui.SetAbsPoint(bar.handle, 0, 0.1f, 0.2f) == UiError::None, "absolute point on simple frame");
    Check(Logged("abs " + Hex(barObject->Self()) + " 0 0.100 0.200 1"), "simple frame is its own layout object");
    Check(ui.SetPoint(bar.handle, 6, text.handle, 0, 0.01f, -0.02f) == UiError::None, "relative point");
    Check(Logged("rel " + Hex(barObject->Self()) + " 6 " + Hex(textObject->Self() + Layout127::ClassicLayout)
        + " 0 0.010 -0.020 1"), "relative target uses its layout object");
    Check(ui.SetPoint(bar.handle, 9, text.handle, 0, 0, 0) == UiError::BadArgument, "point outside 0..8 is refused");
    Check(ui.SetAllPoints(bar.handle, text.handle) == UiError::None, "set all points");
    Check(ui.ClearPoints(text.handle) == UiError::None, "clear points");
    Check(Logged("clear " + Hex(textObject->Self() + Layout127::ClassicLayout) + " 1"), "clear passes update flag");
    Check(ui.SetWidth(text.handle, 0.2f) == UiError::None && ui.SetHeight(text.handle, 0.05f) == UiError::None,
        "size");
    Check(ui.SetWidth(text.handle, -1.0f) == UiError::BadArgument, "negative width refused");
    Check(ui.SetScale(text.handle, 0.0f) == UiError::BadArgument, "zero scale refused");
}

void VisibilityAndContent() {
    Reset();
    GameUiLibrary ui(Entries());
    const auto text = ui.Create("Text", 0, 0, 0);
    const auto* textObject = g_frames.back().get();
    const auto backdrop = ui.Create("Backdrop", 0, 0, 0);
    const auto* backdropObject = g_frames.back().get();
    const auto edit = ui.Create("Edit", 0, 0, 0);
    const auto slider = ui.Create("Slider", 0, 0, 0);
    const auto model = ui.Create("Model", 0, 0, 0);
    const auto bar = ui.CreateSimple("Bar", 0, 0);
    const auto* barObject = g_frames.back().get();
    auto* fontObject = NewFrame(vtFontString, barObject->Self(), Layout127::RegionParent);
    auto* textureObject = NewFrame(vtTexture, barObject->Self(), Layout127::RegionParent);
    g_named = fontObject;
    const auto font = ui.Find("Named", 0);
    Check(font.error == UiError::None, "find searches classic then simple frames");
    Check(Logged("find0 Named 0") && Logged("find1 Named 0"), "find tries lookups in game order");
    Check(ui.Find("Named", 0).handle == font.handle, "same object keeps one handle");
    g_named = textureObject;
    const auto texture = ui.Find("Named", 0);

    Check(ui.SetVisible(text.handle, false) == UiError::None && Logged("hide " + Hex(textObject->Self())),
        "classic hide uses layer virtual slot");
    Check(ui.SetVisible(bar.handle, true) == UiError::None && Logged("show " + Hex(barObject->Self())),
        "simple show uses layout virtual slot");
    Check(ui.SetVisible(font.handle, false) == UiError::None && Logged("rhide " + Hex(fontObject->Self())),
        "region hide uses region function");

    Check(ui.SetText(text.handle, "生命") == UiError::None && Logged("ttext " + Hex(textObject->Self()) + " 生命"),
        "text frame text");
    Check(ui.SetText(font.handle, "abc") == UiError::None, "simple font string text");
    Check(ui.SetText(edit.handle, "x") == UiError::None && Logged("etext " + Hex(g_frames[4]->Self()) + " x 1"),
        "edit box text passes update flag");
    Check(ui.SetText(backdrop.handle, "x") == UiError::Unsupported, "backdrop has no text");
    Check(ui.SetTexture(backdrop.handle, "a.blp", true) == UiError::None
        && Logged("btex " + Hex(backdropObject->Self()) + " a.blp 0 1 00000000 1"), "backdrop texture arguments");
    Check(ui.SetTexture(bar.handle, "b.blp", false) == UiError::None
        && Logged("bartex " + Hex(barObject->Self()) + " b.blp 0"), "status bar texture");
    Check(ui.SetTexture(texture.handle, "c.blp", false) == UiError::None, "simple texture");
    Check(ui.SetTexture(text.handle, "x.blp", false) == UiError::Unsupported, "text has no texture");
    Check(ui.SetValue(bar.handle, 0.5f) == UiError::None && Logged("barvalue " + Hex(barObject->Self()) + " 0.500"),
        "status bar value");
    Check(ui.SetValue(slider.handle, 2.0f) == UiError::None, "slider value");
    Check(ui.SetValue(text.handle, 1.0f) == UiError::Unsupported, "text has no value");
    Check(ui.SetLimits(bar.handle, 0.0f, 100.0f) == UiError::None
        && Logged("barlimits " + Hex(barObject->Self()) + " 0.000 100.000"), "limits are passed minimum first");
    Check(ui.SetLimits(bar.handle, 2.0f, 1.0f) == UiError::BadArgument, "inverted limits refused");
    Check(ui.SetModel(model.handle, "m.mdx", 0) == UiError::None, "model frame model");
    Check(ui.SetEnabled(edit.handle, false) == UiError::None, "edit box can be disabled");
    Check(ui.SetEnabled(backdrop.handle, false) == UiError::Unsupported, "backdrop is not a control");
    Check(ui.SetTextAlign(text.handle, 2, 1) == UiError::None && Logged("talignh " + Hex(textObject->Self()) + " 2")
        && Logged("talignv " + Hex(textObject->Self()) + " 1") && Logged("talignu " + Hex(textObject->Self())),
        "text frame alignment then refresh");
    Check(ui.SetTextAlign(font.handle, 0, 2) == UiError::None && Logged("falignh " + Hex(fontObject->Self()) + " 8")
        && Logged("falignv " + Hex(fontObject->Self()) + " 4"), "font string alignment flags");
    Check(ui.SetColor(text.handle, 0xFF102030u) == UiError::None && Logged("tcolor " + Hex(textObject->Self()) + " FF102030"),
        "text colour by pointer");
    Check(ui.SetColor(texture.handle, 0x80FFFFFFu) == UiError::None, "region colour");
    Check(ui.SetFont(font.handle, "f.ttf", 0.012f, 0) == UiError::None, "font string font");
    Check(ui.SetFont(text.handle, "f.ttf", 0.012f, 1) == UiError::None
        && Logged("lfont " + Hex(textObject->Self()) + " f.ttf 0.012 1"), "classic font through layer");
    Check(ui.SetAlpha(text.handle, 128) == UiError::None && Logged("lalpha " + Hex(textObject->Self()) + " 128 0"),
        "classic alpha");
    Check(ui.SetAlpha(bar.handle, 64) == UiError::None, "simple alpha");
    Check(ui.SetAlpha(font.handle, 64) == UiError::Unsupported, "region alpha not offered");
    Check(ui.SetAlpha(text.handle, 300) == UiError::BadArgument, "alpha range");

    g_log.clear();
    Check(ui.SetLevel(text.handle, 5) == UiError::None, "classic level");
    Check(g_log.size() == 2 && g_log[0] == "hide " + Hex(textObject->Self()) && g_log[1] == "show " + Hex(textObject->Self()),
        "classic level hides then shows");
    Check(textObject->At(Layout127::ClassicPriority) == 5, "classic level writes priority field");
    Check(ui.SetLevel(bar.handle, 2) == UiError::None, "simple level");

    const auto simple = ui.CreateSimple("Simple", 0, 0);
    Check(ui.SetParent(text.handle, simple.handle) == UiError::UnsupportedParent, "classic frame cannot use simple parent layout");
    Check(ui.SetParent(bar.handle, simple.handle) == UiError::None, "simple frame under simple parent");
    Check(ui.SetParent(text.handle, backdrop.handle) == UiError::None, "classic frame can use classic parent");
}

void ParentValidation() {
    Reset();
    GameUiLibrary ui(Entries());
    const auto root = ui.Root();
    const auto backdrop = ui.Create("Backdrop", root.handle, 0, 0);
    const auto simple = ui.CreateSimple("Simple", 0, 11);
    auto* simpleObject = g_frames.back().get();
    g_log.clear();
    Check(ui.CreateSimple("Bar", root.handle, 968002000).error == UiError::None,
        "verified GameUI token is an alias for default simple root");
    Check(Logged("createsimple Bar 00000000 968002000"), "GameUI address never reaches simple constructor");
    Check(ui.CreateSimple("Bar", simple.handle, 0).error == UiError::None
        && Logged("createsimple Bar " + Hex(simpleObject->Self()) + " 0"), "simple parent remains unchanged");
    auto* regionObject = NewFrame(vtTexture, simpleObject->Self(), Layout127::RegionParent);
    g_named = regionObject;
    const auto region = ui.Find("Named", 0);
    g_log.clear();
    Check(ui.CreateSimple("Bar", backdrop.handle, 0).error == UiError::UnsupportedParent,
        "classic backdrop cannot own a simple frame");
    Check(ui.CreateSimple("Simple", region.handle, 0).error == UiError::UnsupportedParent,
        "simple region cannot own a simple frame");
    Check(ui.Create("Text", region.handle, 0, 0).error == UiError::UnsupportedParent,
        "region cannot own a classic frame");
    Check(ui.Create("Text", simple.handle, 0, 0).error == UiError::UnsupportedParent,
        "simple family cannot own a classic frame");
    Check(ui.SetParent(backdrop.handle, simple.handle) == UiError::UnsupportedParent,
        "classic reparenting to simple rejected before engine");
    Check(ui.SetParent(simple.handle, backdrop.handle) == UiError::UnsupportedParent,
        "reparenting to classic family rejected before engine");
    Check(ui.SetParent(simple.handle, simple.handle) == UiError::ParentCycle, "self parenting rejected");
    Check(g_log.empty() && !ui.Faulted(), "invalid parents never call the engine or disable UI");
    const auto child = ui.CreateSimple("Simple", simple.handle, 0);
    g_log.clear();
    Check(ui.SetParent(simple.handle, child.handle) == UiError::ParentCycle,
        "reparenting to a descendant rejected before engine");
    Check(g_log.empty() && ui.SetWidth(backdrop.handle, 0.3f) == UiError::None,
        "valid UI calls still work after parent refusal");
    ui.ResetSession();
    Check(ui.CreateSimple("Simple", root.handle, 0).error == UiError::BadHandle,
        "root alias does not survive a session reset");
}

void LifetimeAndHandles() {
    Reset();
    GameUiLibrary ui(Entries());
    const auto root = ui.Root();
    Check(ui.Destroy(root.handle) == UiError::NotOwned, "game frames cannot be destroyed");
    const auto world = ui.World();
    Check(world.error == UiError::None && world.handle != root.handle, "world frame from game UI");

    const auto parent = ui.Create("Button", 0, 0, 0);
    auto* parentObject = g_frames.back().get();
    const auto child = ui.Create("Text", parent.handle, 0, 0);
    Check(ui.Destroy(parent.handle) == UiError::None, "destroy created frame");
    Check(Logged("delete " + Hex(parentObject->Self()) + " 1"), "classic delete uses layer slot with flag");
    Check(ui.SetVisible(parent.handle, true) == UiError::BadHandle, "destroyed handle is invalid");
    Check(ui.SetVisible(child.handle, true) == UiError::BadHandle, "children of destroyed frame are invalid");

    const auto text = ui.Create("Text", 0, 0, 0);
    auto* textObject = g_frames.back().get();
    textObject->vtable = reinterpret_cast<Address>(vtBackdrop.data());
    Check(ui.SetText(text.handle, "x") == UiError::StaleFrame, "frame replaced by game is stale");
    Check(ui.SetText(text.handle, "x") == UiError::BadHandle, "stale handle stays invalid");

    const auto before = ui.Create("Text", 0, 0, 0);
    ui.ResetSession();
    Check(ui.LiveCount() == 0, "session reset drops all frames");
    Check(ui.SetText(before.handle, "x") == UiError::BadHandle, "handles from previous session are invalid");
    const auto after = ui.Create("Text", 0, 0, 0);
    Check(after.handle != before.handle, "new session hands out new handles");
    Check(ui.SetText(0, "x") == UiError::BadHandle && ui.SetText(-5, "x") == UiError::BadHandle, "zero or negative");
}

void TocLoading() {
    Reset();
    GameUiLibrary ui(Entries());
    g_tables.strings[Layout127::TableCapacity / 4] = 16;
    g_tables.fdf[Layout127::TableCapacity / 4] = 0xFFFF;
    Check(ui.LoadToc("ui\\my.toc") == UiError::None, "load toc");
    Check(Logged("grows " + Hex(reinterpret_cast<Address>(g_tables.strings.data())) + " 65535"),
        "small string table grows first");
    for (const auto& line : g_log) Check(line.rfind("growf", 0) != 0, "large fdf table is not grown");
    g_tocResult = 0;
    Check(ui.LoadToc("missing.toc") == UiError::NotFound, "failed toc load is reported");
}

void PointerEvents() {
    Reset();
    GameUiLibrary ui(Entries());
    const auto button = ui.Create("Button", 0, 0, 0);
    auto* buttonObject = g_frames.back().get();
    auto* label = NewFrame(vtText, buttonObject->Self(), Layout127::ClassicParent);
    Check(ui.Watch(button.handle, kUiEventEnter | kUiEventLeave | kUiEventClick) == UiError::None, "watch");
    Check(ui.Watch(button.handle, 0x80) == UiError::BadArgument, "unknown event bit refused");

    ui.Poll({label->Self(), false});
    auto events = ui.TakeEvents();
    Check(events.size() == 1 && events[0] == UiFrameEvent{button.handle, kUiEventEnter}, "enter through child");
    ui.Poll({label->Self(), true});
    ui.Poll({label->Self(), false});
    events = ui.TakeEvents();
    Check(events.size() == 1 && events[0].kind == kUiEventClick, "press and release inside clicks");
    ui.Poll({0, false});
    events = ui.TakeEvents();
    Check(events.size() == 1 && events[0].kind == kUiEventLeave, "leave");
    ui.Poll({0, true});
    ui.Poll({label->Self(), true});
    ui.Poll({label->Self(), false});
    events = ui.TakeEvents();
    Check(events.size() == 1 && events[0].kind == kUiEventEnter, "press outside then release inside does not click");

    auto* top = NewFrame(vtClassic, 0, 0);
    g_tables.simpleTop = top->Self();
    g_tables.hovered = top->Self();
    top->Set(Layout127::SimpleTopHovered, label->Self());
    Check(ui.ReadPointer().hovered == label->Self(), "simple top resolves to hovered simple object");
    g_tables.hovered = buttonObject->Self();
    Check(ui.ReadPointer().hovered == buttonObject->Self(), "classic hovered frame read directly");
}

void FaultsAndThreads() {
    Reset();
    g_fault = {}; g_faultCount = 0;
    GameUiLibrary ui(Entries(), &ObserveFault);
    const auto text = ui.Create("Text", 0, 0, 0);
    bool wrong = false;
    std::thread other([&] { wrong = ui.SetText(text.handle, "x") == UiError::WrongThread; });
    other.join();
    Check(wrong, "calls from another thread are refused");
    ui.SetDiagnosticContext(9, 42, "audastraui.create");
    Check(ui.Create("Crash", 0, 0, 0).error == UiError::Faulted, "game function fault is caught");
    Check(g_faultCount == 1 && g_fault.code == EXCEPTION_ACCESS_VIOLATION && g_fault.address != 0
        && g_fault.epoch == 9 && g_fault.request == 42 && std::string_view(g_fault.entry) == "audastraui.create",
        "SEH diagnostics retain original code, instruction, request and fixed entry name");
    Check(ui.LastFault().address == g_fault.address, "last fault retained outside the SEH filter");
    Check(ui.Faulted() && ui.SetText(text.handle, "x") == UiError::Faulted, "library stays disabled after a fault");
    Check(g_faultCount == 1, "subsequent rejected calls do not duplicate the original exception");
}

// 经 JAPI v2 宿主调用内置模块：线格式进、线格式出，和地图侧走同一条路。
struct ModuleHost {
    audastra::japi::Host host;
    std::uint32_t epoch = 1;
    std::uint32_t request = 0;
    audastra::japi::Response Call(const std::string& command) {
        return host.Dispatch("ajapi:2;" + std::to_string(epoch) + ";" + std::to_string(++request) + ";" + command);
    }
};

void ModuleFaultDiagnostics() {
    Reset(); g_fault = {}; g_faultCount = 0;
    ModuleHost m;
    Check(InstallGameUiModule(m.host, Entries(), {}, &ObserveFault) == AJ_OK, "diagnostic observer installs without ABI changes");
    Check(m.Call("module.load;s:audastraui").status == AJ_OK, "load observed module");
    Check(m.Call("audastraui.create;s:Crash;d:0;i:0;i:0").status == AJ_INTERNAL, "observed fault keeps original wire status");
    Check(g_faultCount == 1 && g_fault.epoch == m.epoch && g_fault.request == m.request
        && std::string_view(g_fault.entry) == "audastraui.create", "module binds actual call identity to SEH diagnostics");
}

void ModuleParentValidation() {
    Reset();
    ModuleHost m;
    Check(InstallGameUiModule(m.host, Entries()) == AJ_OK, "install parent validation module");
    const auto root = m.Call("audastraui.root").value.integer;
    const auto backdrop = m.Call("audastraui.create;s:Backdrop;d:0;i:0;i:0").value.integer;
    g_log.clear();
    const auto refused = m.Call("audastraui.createsimple;s:Bar;d:" + std::to_string(backdrop) + ";i:0");
    const auto failed = m.request;
    Check(refused.status == AJ_UNSUPPORTED && g_log.empty(), "bad parent is a supported wire refusal, not an engine fault");
    Check(m.Call("audastraui.error;i:" + std::to_string(failed)).value.text == UiErrorText(UiError::UnsupportedParent),
        "wire diagnostic identifies unsupported parent family for the failed request");
    Check(m.Call("audastraui.ready").value.integer == 1, "UI remains available after parent refusal");
    Check(m.Call("audastraui.createsimple;s:Bar;d:" + std::to_string(root) + ";i:0").status == AJ_OK,
        "wire GameUI root alias can create a simple frame after refusal");
}

void ModuleCalls() {
    Reset();
    ModuleHost m;
    Check(InstallGameUiModule(m.host, Entries()) == AJ_OK, "built-in module installs through Host::Install");
    Check(InstallGameUiModule(m.host, Entries()) == AJ_STATE, "same module name cannot be installed twice");
    Check(m.Call("module.load;s:audastraui").status == AJ_OK, "map-side load finds the active built-in module");
    Check(m.Call("module.load;s:other").status == AJ_UNSUPPORTED, "no loader: maps cannot pull other modules");
    Check(m.Call("module.info;s:audastraui").value.text
        == "audastraui|1.3.0|1|active|" + std::to_string(GameUiModuleContract().commands.size()), "module info");

    auto r = m.Call("audastraui.ready");
    Check(r.status == AJ_OK && r.value.kind == AJ_BOOL && r.value.integer == 1, "ready with complete entries and game UI");
    Check(m.Call("audastraui.clickunit;i:1234").status == AJ_UNSUPPORTED, "drawing does not imply input capability");
    const auto inputFailure = m.request;
    Check(m.Call("audastraui.error;i:" + std::to_string(inputFailure)).value.text.find("单位目标输入") != std::string::npos,
        "input capability failure explains its own request");
    Check(m.Call("audastraui.clickunit;d:1234").status == AJ_ARGUMENT, "frame tokens cannot masquerade as unit arguments");
    Check(m.Call("audastraui.ready").value.integer == 1, "missing input capability leaves drawing available");
    r = m.Call("audastraui.create;s:Text;d:0;i:3;i:7");
    Check(r.status == AJ_OK && r.value.kind == AJ_HANDLE && r.value.integer > 0, "create returns a frame handle");
    Check(Logged("create Text " + Hex(g_root->Self()) + " 0 3 7"), "create arguments reach the game function");
    const auto text = r.value.integer;
    const auto* textObject = g_frames.back().get();
    const auto frame = "d:" + std::to_string(text);

    Check(m.Call("audastraui.settext;" + frame + ";s:生命%3B%25").status == AJ_OK
        && Logged("ttext " + Hex(textObject->Self()) + " 生命;%"), "text argument is decoded once");
    Check(m.Call("audastraui.setcolor;" + frame + ";i:255;i:204;i:0;i:255").status == AJ_OK
        && Logged("tcolor " + Hex(textObject->Self()) + " FFFFCC00"), "colour components packed as ARGB");
    Check(m.Call("audastraui.setabspoint;" + frame + ";i:4;r:4.00000000e-1;r:3e-1").status == AJ_OK
        && Logged("abs " + Hex(textObject->Self() + Layout127::ClassicLayout) + " 4 0.400 0.300 1"), "real arguments");
    Check(m.Call("audastraui.setsize;" + frame + ";r:0.2;r:0.05").status == AJ_OK
        && Logged("width " + Hex(textObject->Self() + Layout127::ClassicLayout) + " 0.200")
        && Logged("height " + Hex(textObject->Self() + Layout127::ClassicLayout) + " 0.050"), "size sets width and height");
    g_log.clear();
    Check(m.Call("audastraui.setsize;" + frame + ";r:0.2;r:-1").status == AJ_ARGUMENT && g_log.empty(),
        "invalid size changes nothing");

    r = m.Call("audastraui.setcolor;" + frame + ";i:256;i:0;i:0;i:255");
    const auto failed = m.request;
    Check(r.status == AJ_ARGUMENT && r.value.kind == AJ_VOID, "colour component range checked");
    Check(m.Call("audastraui.error;i:" + std::to_string(failed)).value.text == UiErrorText(UiError::BadArgument),
        "error text belongs to the failed request");
    Check(m.Call("audastraui.error;i:" + std::to_string(failed - 1)).value.text.empty(),
        "other requests have no module error");
    Check(m.Call("audastraui.settexture;" + frame + ";s:x.blp;b:0").status == AJ_UNSUPPORTED, "unsupported operation");
    Check(m.Call("audastraui.find;s:Missing;i:0").status == AJ_ARGUMENT, "missing frame name");
    const auto root = m.Call("audastraui.root").value.integer;
    Check(m.Call("audastraui.destroy;d:" + std::to_string(root)).status == AJ_STATE, "game frames cannot be destroyed");
    Check(m.Call("audastraui.settext;i:1;s:x").status == AJ_ARGUMENT, "host checks argument types against the contract");
    Check(m.Call("audastraui.nothing").status == AJ_UNKNOWN, "unknown command");

    // 鼠标移入、移出；点击依赖真实左键状态，由上面的 PointerEvents 覆盖。
    r = m.Call("audastraui.create;s:Button;d:0;i:0;i:0");
    const auto button = r.value.integer;
    auto* buttonObject = g_frames.back().get();
    Check(m.Call("audastraui.watch;d:" + std::to_string(button) + ";i:8").status == AJ_ARGUMENT, "watch mask range");
    Check(m.Call("audastraui.watch;d:" + std::to_string(button) + ";i:3").status == AJ_OK, "watch enter and leave");
    g_tables.hovered = buttonObject->Self();
    r = m.Call("audastraui.poll");
    Check(r.status == AJ_OK && r.value.integer == 1, "poll queues the enter event");
    Check(m.Call("audastraui.event").value.text == std::to_string(button) + "|1", "event text is handle|kind");
    Check(m.Call("audastraui.event").value.text.empty(), "queue drained");
    g_tables.hovered = 0;
    Check(m.Call("audastraui.poll").value.integer == 1 && m.Call("audastraui.event").value.text == std::to_string(button) + "|2",
        "leave event");

    g_tables.hovered = buttonObject->Self();
    Check(m.Call("audastraui.poll").value.integer == 1, "old module has a queued event when VM teardown begins");

    // 新 VM 代际：旧模块走停止、销毁，新实例的句柄从头开始，旧句柄失效。
    const auto reset = m.host.Reset(2);
    Check(reset.status == AJ_OK && reset.applied && reset.stoppingModules == 0 && m.host.ModuleCount() == 0,
        "host reset destroys the module");
    m.epoch = 2;
    m.request = 0;
    Check(InstallGameUiModule(m.host, Entries()) == AJ_OK, "next VM installs a fresh module");
    const auto nextFrame = m.Call("audastraui.create;s:Text;d:0;i:0;i:0");
    Check(nextFrame.status == AJ_OK && nextFrame.value.integer > 0, "next VM creates a live frame");
    Check(m.Call("audastraui.event").value.text.empty(), "previous module queued event cannot cross the VM boundary");
    Check("d:" + std::to_string(nextFrame.value.integer) != frame, "new module cannot reissue a previous VM token");
    Check(m.Call("audastraui.settext;" + frame + ";s:x").status == AJ_ARGUMENT, "handles from the previous VM are invalid");
}

void ModuleUnavailable() {
    ModuleHost m;
    Check(InstallGameUiModule(m.host, GameUiEntryPoints{}, "Game.dll 版本不受支持") == AJ_OK, "incomplete entries still install");
    auto r = m.Call("audastraui.ready");
    Check(r.status == AJ_OK && r.value.integer == 0, "not ready without entries");
    Check(m.Call("audastraui.error;i:" + std::to_string(m.request)).value.text == "Game.dll 版本不受支持",
        "ready reports the resolve reason");
    Check(m.Call("audastraui.create;s:Text;d:0;i:0;i:0").status == AJ_STATE, "calls refused while not ready");
    Check(m.Call("audastraui.poll").status == AJ_STATE, "poll stops while not ready");
    bool wrong = false;
    std::thread other([&] { wrong = m.Call("audastraui.ready").status == AJ_THREAD; });
    other.join();
    Check(wrong, "host thread only");
}

std::array<Address, 12> g_tableWords{};
Address g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
bool g_raiseTable = false;
unsigned g_tableResolves = 0;
Address __fastcall ResolveTable(std::uint32_t handle) {
    ++g_tableResolves;
    if (g_raiseTable) RaiseException(0xE050DA7A, 0, 0, nullptr);
    return handle == 1234 || handle == 5678 ? g_tableObject : 0;
}
struct VmText {
    std::array<Address, 10> object{}, linked{};
    explicit VmText(const char* text) {
        object[2] = reinterpret_cast<Address>(linked.data());
        linked[7] = reinterpret_cast<Address>(text);
    }
    Address Self() { return reinterpret_cast<Address>(object.data()); }
};

void DataPlane() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data()); g_tableResolves = 0;
    bool alive = true;
    std::atomic<std::int32_t> gate{0};
    GameUiLibrary ui(Entries());
    GameUiDataPlane data({&ResolveTable, [&] { return alive; }, &gate}, &ObserveFault);
    data.Attach(ui);
    const auto text = ui.Create("Text", 0, 0, 0).handle;
    auto* textObject = g_frames.back().get();
    const auto backdrop = ui.Create("Backdrop", 0, 0, 0).handle;
    const auto bar = ui.CreateSimple("Bar", 0, 0).handle;
    const float width = 0.25f;
    Check(!data.SaveReal(1234, text, 2, reinterpret_cast<const float*>(1)).intercepted
        && g_tableResolves == 0 && data.Errors() == "0|", "unbound tables do not read values or emit diagnostics");
    Check(data.Bind(0, 1, 1) == AJ_ARGUMENT && data.Bind(9999, 1, 2) == AJ_ARGUMENT, "null and non-table handles rejected");
    Check(data.Bind(1234, 1, 3) == AJ_OK && data.Bind(1234, 1, 4) == AJ_OK, "same valid private table binds idempotently");
    Check(gate.load() == 1234, "successful bind publishes only the table hint for the native fast path");
    Check(data.Bind(5678, 1, 5) == AJ_STATE && data.Bind(1234, 2, 6) == AJ_STALE, "binding cannot replace table or epoch");
    const auto resolves = g_tableResolves;
    Check(!data.SaveReal(5678, text, 2, reinterpret_cast<const float*>(1)).intercepted
        && g_tableResolves == resolves && data.Errors() == "0|", "another table remains original-native work");
    g_log.clear();
    Check(data.SaveReal(1234, text, 2, &width).applied && ui.SetWidth(text, 0.5f) == UiError::None
        && data.SaveBoolean(1234, text, 5, 0).applied, "typed and string paths share immediate UI operations");
    Check(g_log.size() == 3 && g_log[0].starts_with("width ") && g_log[0].ends_with("0.250")
        && g_log[1].ends_with("0.500") && g_log[2].starts_with("hide "), "cross-path call order preserved");
    VmText message("生命;%"), texture("x.blp"), empty("");
    Check(data.SaveStr(1234, text, 3, message.Self()).applied
        && Logged("ttext " + Hex(textObject->Self()) + " 生命;%"), "VM string copied once without wire escaping");
    Check(data.SaveStr(1234, text, 3, empty.Self()).applied, "empty text retains string-command semantics");
    Check(data.SaveStr(1234, text, 3, 0).applied, "null VM string clears text like the old AjArgText path");
    Check(data.SaveStr(1234, backdrop, 4, texture.Self()).applied, "texture uses existing API with tile false");
    Check(data.SaveReal(1234, bar, 1, &width).applied && data.Errors() == "0|", "status bar value takes finite float pointer");
    const auto bad = [&](UiDataWrite write) { Check(write.intercepted && !write.applied, "bad owned write consumed without applying"); };
    g_log.clear();
    bad(data.SaveReal(1234, text, 3, &width));
    bad(data.SaveStr(1234, text, 2, message.Self()));
    bad(data.SaveBoolean(1234, text, 1, 1));
    bad(data.SaveBoolean(1234, text, 5, 2));
    bad(data.SaveReal(1234, text, 2, nullptr));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bad(data.SaveReal(1234, bar, 1, &nan));
    bad(data.SaveStr(1234, text, 3, 1));
    VmText control("a\nb"), invalid("\xff");
    // 长字符串对象另持有稳定存储；不使用临时 std::string 的指针。
    const std::string longStorage(385, 'x'); VmText tooLong(longStorage.c_str());
    bad(data.SaveStr(1234, text, 3, control.Self()));
    bad(data.SaveStr(1234, text, 3, invalid.Self()));
    bad(data.SaveStr(1234, text, 3, tooLong.Self()));
    const std::string escapedStorage(129, ';'); VmText overEncoded(escapedStorage.c_str());
    bad(data.SaveStr(1234, text, 3, overEncoded.Self()));
    Check(data.Errors().starts_with("11|") && g_log.empty(), "malformed writes have bounded counter and no game mutation");
    bad(data.SaveStr(1234, backdrop, 3, message.Self()));
    Check(data.Errors() == "12|" + std::string(UiErrorText(UiError::Unsupported)), "UI failure keeps same operation reason");
    UiDataWrite threaded;
    std::thread other([&] { threaded = data.SaveBoolean(1234, text, 5, 1); }); other.join();
    bad(threaded);
    Check(data.Errors().starts_with("13|"), "wrong thread write consumed and counted");
    g_nestedData = &data; g_nestedFrame = text; g_nestedWrite = true;
    Check(data.SaveStr(1234, text, 3, message.Self()).applied && !g_nestedWrite, "synchronous nested write rejected without replay");
    g_nestedData = nullptr;
    Check(data.Errors().starts_with("14|"), "nested failure counted once");
    ++g_tableWords[3]; bad(data.SaveBoolean(1234, text, 5, 1)); --g_tableWords[3];
    Check(data.Errors().starts_with("15|"), "reused table address with changed weak identity cannot touch UI");
    GameUiDataPlane stale({&ResolveTable, [&] { return alive; }}); stale.Attach(ui);
    Check(stale.Bind(1234, 1, 1) == AJ_OK, "bind stale-session control");
    alive = false; bad(stale.SaveBoolean(1234, text, 5, 1)); alive = true;
    Check(!stale.Owns(1234) && stale.Bind(1234, 1, 2) == AJ_STALE && stale.Errors().starts_with("1|"),
        "VM identity loss clears table permanently even if an address later reappears");
    Check(ui.Destroy(text) == UiError::None, "remove frame");
    bad(data.SaveBoolean(1234, text, 5, 1));
    Check(data.Errors().starts_with("16|"), "destroyed frame token refused by shared resolver");
    g_faultCount = 0; g_raiseTable = true;
    bad(data.SaveBoolean(1234, backdrop, 5, 1)); g_raiseTable = false;
    Check(g_faultCount == 1 && g_fault.code == 0xE050DA7A && g_fault.epoch == 1 && g_fault.request == 0,
        "native table SEH keeps code and session, no synthetic request");
    bad(data.SaveBoolean(1234, backdrop, 5, 1));
    Check(g_faultCount == 1 && !ui.Faulted() && ui.SetWidth(backdrop, width) == UiError::None,
        "data fault is not retried and drawing stays available");
    data.Stop();
    Check(gate.load() == 0, "Stop clears the native fast-path hint before destroying the consumer");
    Check(!data.Owns(1234) && !data.SaveBoolean(1234, backdrop, 5, 1).intercepted
        && data.Bind(1234, 1, 99) == AJ_STALE, "stopped table unbound before UI destruction");
    GameUiDataPlane next({&ResolveTable, [&] { return alive; }}); next.Attach(ui);
    Check(next.Errors() == "0|" && next.Bind(1234, 2, 1) == AJ_OK, "new VM binds from fresh state");
    bad(next.SaveBoolean(1234, text, 5, 1));
    Check(next.Errors().starts_with("1|"), "old frame token cannot be reused after rebind");
    GameUiDataPlane nullTexture({&ResolveTable, [&] { return alive; }}); nullTexture.Attach(ui);
    Check(nullTexture.Bind(1234, 2, 2) == AJ_OK && !nullTexture.SaveStr(1234, backdrop, 4, 0).applied
        && nullTexture.Errors().starts_with("1|"), "null texture retains empty-path refusal");
    gate.store(5678); data.Stop();
    Check(gate.load() == 5678, "repeated stop on an old consumer cannot clear a newer binding");
}

void DataPlaneModule() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
    ModuleHost unavailable;
    Check(InstallGameUiModule(unavailable.host, Entries()) == AJ_OK, "drawing-only module installs");
    Check(unavailable.Call("audastraui.bindtable;i:1234").status == AJ_UNSUPPORTED
        && unavailable.Call("audastraui.ready").value.integer == 1, "missing data hooks preserves drawing capability");
    ModuleHost m;
    auto data = std::make_shared<GameUiDataPlane>(GameUiDataContext{&ResolveTable, [] { return true; }});
    Check(InstallGameUiModule(m.host, Entries(), {}, nullptr, {}, data) == AJ_OK, "install module with native data capability");
    Check(m.Call("audastraui.bindtable;i:1234").status == AJ_OK && m.Call("audastraui.dataerrors").value.text == "0|", "wire bind and diagnostics contracts");
    const auto frame = m.Call("audastraui.create;s:Text;d:0;i:0;i:0").value.integer;
    Check(data->SaveBoolean(1234, frame, 5, 1).applied, "module attaches data consumer to same UI library");
    Check(m.Call("audastraui.textformat;d:" + std::to_string(frame) + ";s:{0}").status == AJ_OK
        && data->SaveInteger(1234, frame, 24, 16777217).applied && data->SaveBoolean(1234, frame, 6, 1).applied,
        "format registration and int arguments use real module contract");
    Check(m.Call("audastraui.eventtable").status == AJ_UNSUPPORTED && m.Call("audastraui.ready").value.integer == 1,
        "optional event capability refusal does not disable drawing");
    Check(m.Call("audastraui.bindtable;i:5678").status == AJ_STATE, "conflicting bind reports normal module status");
    const auto failed = m.request;
    Check(!m.Call("audastraui.error;i:" + std::to_string(failed)).value.text.empty(), "binding reason uses failed-request error channel");
    Check(m.host.Reset(2).applied && !data->Owns(1234), "host stop clears table before module destruction");
}

#include "GameUiTextFormatTests.h"
#include "GameUiDataFormatTests.h"

// 校验已有包装调用及新增可选能力的明确契约；生成器侧接线另作集成验收。
void ScriptMatchesModule() {
    std::ifstream stream(AUDASTRA_UI_SCRIPT, std::ios::binary);
    const std::string script((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    Check(!script.empty(), "read AudastraUI.j");
    const auto contract = GameUiModuleContract();
    std::map<std::string, const audastra::japi::CommandContract*> commands;
    for (const auto& command : contract.commands) commands[command.name] = &command;
    const std::pair<std::string_view, char> callers[] = {{"Run(\"", 'v'}, {"Frame(\"", 'd'},
        {"AjCallBool(PREFIX + \"", 'b'}, {"AjCallText(PREFIX + \"", 's'}, {"AjCallInt(PREFIX + \"", 'i'},
        {"AjCallHandle(PREFIX + \"", 'd'}, {"AjCallVoid(PREFIX + \"", 'v'}};
    const std::pair<std::string_view, char> arguments[] = {{"AjArgHandle(", 'd'}, {"AjArgInt(", 'i'},
        {"AjArgRealText(", 'r'}, {"AjArgReal(", 'r'}, {"AjArgText(", 's'}, {"AjArgBool(", 'b'}};
    std::map<std::string, int> used;
    for (const auto& [caller, result] : callers) {
        for (auto at = script.find(caller); at != std::string::npos; at = script.find(caller, at + 1)) {
            const auto nameStart = at + caller.size();
            const auto nameEnd = script.find('"', nameStart);
            const auto name = "audastraui." + script.substr(nameStart, nameEnd - nameStart);
            std::string types;
            int depth = 1;
            for (auto i = nameEnd + 1; i < script.size() && depth > 0; ++i) {
                if (script[i] == '(') ++depth;
                else if (script[i] == ')') --depth;
                for (const auto& [argument, type] : arguments) {
                    if (script.compare(i, argument.size(), argument) == 0) {
                        types += type;
                        break;
                    }
                }
            }
            const auto found = commands.find(name);
            // 后加的命令：旧运行库回“未知命令”时脚本有退路（点格照常选中、刷新退回字符串请求），模块里有时照样核类型。
            if (found == commands.end() && (name == "audastraui.clickunit" || name == "audastraui.bindtable" ||
                                               name == "audastraui.dataerrors")) continue;
            Check(found != commands.end(), ("script calls a command the module lacks: " + name).c_str());
            Check(found->second->arguments == types, ("argument types differ: " + name + " " + types).c_str());
            Check(audastra::japi::TypeChar(found->second->resultKind) == result, ("result type differs: " + name).c_str());
            ++used[name];
        }
    }
    for (const auto& command : contract.commands) {
        if (command.name == "audastraui.textformat" && !used.contains(command.name)) {
            Check(command.arguments == "ds" && command.resultKind == AJ_VOID && command.domain == AJ_LOCAL_COMMAND,
                "optional format control has a separately exercised contract");
            continue;
        }
        if (command.name == "audastraui.eventtable" && !used.contains(command.name)) {
            Check(command.arguments.empty() && command.resultKind == AJ_VOID && command.domain == AJ_LOCAL_COMMAND,
                "optional event control has a separately exercised contract");
            continue;
        }
        // 输入是1.1的可选能力，旧版绘制包装仍可完整使用1.0命令。
        if (command.name == "audastraui.dataerrors" && !used.contains(command.name)) {
            Check(command.arguments.empty() && command.resultKind == AJ_TEXT && command.domain == AJ_LOCAL_COMMAND,
                "optional diagnostics command keeps its contract with an older wrapper");
            continue;
        }
        Check(used.contains(command.name), ("module command unused by the script: " + command.name).c_str());
    }
}

} // namespace

int main() {
    try {
        IncompleteTable();
        CreateAndLayout();
        VisibilityAndContent();
        ParentValidation();
        LifetimeAndHandles();
        TocLoading();
        PointerEvents();
        FaultsAndThreads();
        ModuleFaultDiagnostics();
        ModuleParentValidation();
        ModuleCalls();
        ModuleUnavailable();
        DataPlane();
        DataPlaneModule();
        TextFormatContracts();
        FormattedDataPlane();
        EventTableDataPlane();
        EventTableModule();
        EventTableFaultIsolation();
        FormattedDataCapacity();
        ScriptMatchesModule();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        for (const auto& line : g_log) std::cerr << "  " << line << "\n";
        return 1;
    }
    std::cout << "game ui library tests passed\n";
    return 0;
}
