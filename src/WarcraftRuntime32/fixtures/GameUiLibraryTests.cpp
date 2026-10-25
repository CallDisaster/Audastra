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
int __fastcall TextText(Address self, int, const char* text) { Log("ttext %s %s", Hex(self).c_str(), text); return 1; }
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
    Check(ui.SetParent(text.handle, simple.handle) == UiError::None, "classic frame under simple parent");
    Check(ui.SetParent(bar.handle, simple.handle) == UiError::None, "simple frame under simple parent");
    Check(ui.SetParent(text.handle, backdrop.handle) == UiError::Unsupported, "classic parent not offered");
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
    GameUiLibrary ui(Entries());
    const auto text = ui.Create("Text", 0, 0, 0);
    bool wrong = false;
    std::thread other([&] { wrong = ui.SetText(text.handle, "x") == UiError::WrongThread; });
    other.join();
    Check(wrong, "calls from another thread are refused");
    Check(ui.Create("Crash", 0, 0, 0).error == UiError::Faulted, "game function fault is caught");
    Check(ui.Faulted() && ui.SetText(text.handle, "x") == UiError::Faulted, "library stays disabled after a fault");
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

void ModuleCalls() {
    Reset();
    ModuleHost m;
    Check(InstallGameUiModule(m.host, Entries()) == AJ_OK, "built-in module installs through Host::Install");
    Check(InstallGameUiModule(m.host, Entries()) == AJ_STATE, "same module name cannot be installed twice");
    Check(m.Call("module.load;s:audastraui").status == AJ_OK, "map-side load finds the active built-in module");
    Check(m.Call("module.load;s:other").status == AJ_UNSUPPORTED, "no loader: maps cannot pull other modules");
    Check(m.Call("module.info;s:audastraui").value.text
        == "audastraui|1.0.0|1|active|" + std::to_string(GameUiModuleContract().commands.size()), "module info");

    auto r = m.Call("audastraui.ready");
    Check(r.status == AJ_OK && r.value.kind == AJ_BOOL && r.value.integer == 1, "ready with complete entries and game UI");
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

    // 新 VM 代际：旧模块走停止、销毁，新实例的句柄从头开始，旧句柄失效。
    const auto reset = m.host.Reset(2);
    Check(reset.status == AJ_OK && reset.applied && reset.stoppingModules == 0 && m.host.ModuleCount() == 0,
        "host reset destroys the module");
    m.epoch = 2;
    m.request = 0;
    Check(InstallGameUiModule(m.host, Entries()) == AJ_OK, "next VM installs a fresh module");
    const auto nextFrame = m.Call("audastraui.create;s:Text;d:0;i:0;i:0");
    Check(nextFrame.status == AJ_OK && nextFrame.value.integer > 0, "next VM creates a live frame");
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

// AudastraUI.j 里每个模块调用与契约一致：命令名、参数类型顺序、返回类型；契约里每条命令都被用到。
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
            Check(found != commands.end(), ("script calls a command the module lacks: " + name).c_str());
            Check(found->second->arguments == types, ("argument types differ: " + name + " " + types).c_str());
            Check(audastra::japi::TypeChar(found->second->resultKind) == result, ("result type differs: " + name).c_str());
            ++used[name];
        }
    }
    for (const auto& command : contract.commands)
        Check(used.contains(command.name), ("module command unused by the script: " + command.name).c_str());
}

} // namespace

int main() {
    try {
        IncompleteTable();
        CreateAndLayout();
        VisibilityAndContent();
        LifetimeAndHandles();
        TocLoading();
        PointerEvents();
        FaultsAndThreads();
        ModuleCalls();
        ModuleUnavailable();
        ScriptMatchesModule();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        for (const auto& line : g_log) std::cerr << "  " << line << "\n";
        return 1;
    }
    std::cout << "game ui library tests passed\n";
    return 0;
}
