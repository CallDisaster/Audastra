#pragma once

#include <cstdint>
#include <functional>
#include <string>

// 经典 1.27a Game.dll（与 GameImage127 校验的同一文件）的界面函数入口。
// 偏移相对 Game.dll 基址，逐个在该版本上核对过函数入口和虚表类名；参数顺序见下面的函数指针类型。
// 游戏的 this 调用一律按 __fastcall 加占位 EDX 参数声明：对象在 ECX、其余压栈、被调用方清栈，二进制等价。
namespace Audastra::Runtime32::GameUi {

namespace Rva127 {
inline constexpr std::uint32_t GetGameUi = 0x34F3A0;
inline constexpr std::uint32_t CreateFrame = 0x0909C0;
inline constexpr std::uint32_t CreateSimpleFrame = 0x090A60;
inline constexpr std::uint32_t FindFrame = 0x09EF40;
inline constexpr std::uint32_t FindSimpleFrame = 0x0C8FD0;
inline constexpr std::uint32_t FindSimpleTexture = 0x0C90B0;
inline constexpr std::uint32_t FindSimpleFontString = 0x0C8EF0;
inline constexpr std::uint32_t LoadToc = 0x066590;
inline constexpr std::uint32_t GrowStringTable = 0x067560;
inline constexpr std::uint32_t GrowFdfTable = 0x066ED0;

inline constexpr std::uint32_t SetAbsPoint = 0x0BD830;
inline constexpr std::uint32_t SetRelPoint = 0x0BD8A0;
inline constexpr std::uint32_t SetAllPoints = 0x0BD750;
inline constexpr std::uint32_t ClearAllPoints = 0x0BCB50;
inline constexpr std::uint32_t SetWidth = 0x0BD960;
inline constexpr std::uint32_t SetHeight = 0x0BD7C0;
inline constexpr std::uint32_t SetScale = 0x0BD7E0;
inline constexpr std::uint32_t LayoutHeight = 0x0BCDF0;
inline constexpr std::uint32_t FontStringHeight = 0x0C0490;

inline constexpr std::uint32_t LayerSetAlpha = 0x09CA50;
inline constexpr std::uint32_t LayerSetParent = 0x09D2B0;
inline constexpr std::uint32_t LayerSetFont = 0x09CE60;
inline constexpr std::uint32_t SimpleSetAlpha = 0x0B9DB0;
inline constexpr std::uint32_t SimpleSetParent = 0x0BA040;
inline constexpr std::uint32_t SimpleSetLevel = 0x356430;
inline constexpr std::uint32_t RegionShow = 0x0BFA70;
inline constexpr std::uint32_t RegionHide = 0x0BF8D0;
inline constexpr std::uint32_t RegionSetColor = 0x0BFA30;

inline constexpr std::uint32_t BackdropSetTexture = 0x0A62A0;
inline constexpr std::uint32_t TextSetText = 0x0AA130;
inline constexpr std::uint32_t TextSetColor = 0x0AA210;
inline constexpr std::uint32_t TextSetAlignH = 0x0A9FC0;
inline constexpr std::uint32_t TextSetAlignV = 0x0AA2B0;
inline constexpr std::uint32_t TextUpdateAlign = 0x0AA2D0;
inline constexpr std::uint32_t FontStringSetText = 0x0C1020;
inline constexpr std::uint32_t FontStringSetFont = 0x0C0B40;
inline constexpr std::uint32_t FontStringSetAlignH = 0x0C1400;
inline constexpr std::uint32_t FontStringSetAlignV = 0x0C0C20;
inline constexpr std::uint32_t TextureSetTexture = 0x0C11E0;
inline constexpr std::uint32_t StatusBarSetTexture = 0x0BC130;
inline constexpr std::uint32_t StatusBarSetValue = 0x0BC2E0;
inline constexpr std::uint32_t StatusBarSetLimits = 0x0BC270;
inline constexpr std::uint32_t SliderSetValue = 0x0AB3B0;
inline constexpr std::uint32_t EditBoxSetText = 0x0B0450;
inline constexpr std::uint32_t ModelSetModel = 0x0A3FA0;
inline constexpr std::uint32_t ControlSetEnabled = 0x0A95F0;

// 数据
inline constexpr std::uint32_t StringTable = 0xBB9CAC;
inline constexpr std::uint32_t FdfTable = 0xBB9CFC;
inline constexpr std::uint32_t DefaultStatus = 0xB662CC;
inline constexpr std::uint32_t ActiveLayer = 0xBB9D88;
inline constexpr std::uint32_t MouseEvent = 0xB66308;
inline constexpr std::uint32_t HitTestLayer = 0x0996E0;
inline constexpr std::uint32_t SimpleTop = 0xBBA0D0;

// 虚表
inline constexpr std::uint32_t VtBackdrop = 0x95AC3C;
inline constexpr std::uint32_t VtText = 0x95B050;
inline constexpr std::uint32_t VtEditBox = 0x95BCBC;
inline constexpr std::uint32_t VtSlider = 0x95B468;
inline constexpr std::uint32_t VtModel = 0x95AAE4;
inline constexpr std::uint32_t VtSimpleStatusBar = 0x95CABC;
inline constexpr std::uint32_t VtSimpleTexture = 0x95CDC4;
inline constexpr std::uint32_t VtSimpleFontString = 0x95CE00;
} // namespace Rva127

// 对象内偏移与虚函数槽（字节）。
namespace Layout127 {
inline constexpr std::uint32_t ClassicLayout = 0xB4;     // 经典框架的布局对象
inline constexpr std::uint32_t ClassicParent = 0x20;
inline constexpr std::uint32_t SimpleParent = 0x6C;
inline constexpr std::uint32_t RegionParent = 0x74;
inline constexpr std::uint32_t ClassicPriority = 0xA8;
inline constexpr std::uint32_t TableCapacity = 0x14;     // 字符串表、fdf 表的容量
inline constexpr std::uint32_t GameUiWorldFrame = 0x3BC;
inline constexpr std::uint32_t SimpleTopHovered = 0x16C;
inline constexpr std::uint32_t SimpleTopPressed = 0x170;
inline constexpr std::uint32_t SlotDelete = 0x8;
inline constexpr std::uint32_t SlotHeight = 0x1C;
inline constexpr std::uint32_t SlotSimpleHide = 0x64;
inline constexpr std::uint32_t SlotSimpleShow = 0x68;
inline constexpr std::uint32_t SlotClassicHide = 0xD0;
inline constexpr std::uint32_t SlotClassicShow = 0xD4;
inline constexpr std::uint32_t SlotClassicDelete = 0xD8;
} // namespace Layout127

using Address = std::uintptr_t;

// 函数指针类型。名字里的 This 表示游戏是 this 调用，第二个参数是占位 EDX。
using FnGetGameUi = Address(__fastcall*)(int, int);
using FnCreateFrame = Address(__fastcall*)(const char* name, Address parent, int zero, int priority, int context);
using FnCreateSimpleFrame = Address(__fastcall*)(const char* name, Address parent, int context);
using FnFind = Address(__fastcall*)(const char* name, int context);
using FnLoadToc = int(__fastcall*)(const char* path, Address strings, Address fdf, Address status);
using FnThisGrow = int(__fastcall*)(Address table, int edx, int capacity);
using FnThisAbsPoint = int(__fastcall*)(Address layout, int edx, int point, float x, float y, int update);
using FnThisRelPoint = int(__fastcall*)(Address layout, int edx, int point, Address relative, int relativePoint,
    float x, float y, int update);
using FnThisAllPoints = int(__fastcall*)(Address layout, int edx, Address relative, int update);
using FnThisInt = int(__fastcall*)(Address self, int edx, int value);
using FnThisFloat = int(__fastcall*)(Address self, int edx, float value);
using FnThisFloat2 = int(__fastcall*)(Address self, int edx, float first, float second);
using FnThisFloatInt = int(__fastcall*)(Address self, int edx, float value, int update);
using FnThisAddress = int(__fastcall*)(Address self, int edx, Address value);
using FnThisHitTest = Address(__fastcall*)(Address self, int edx, Address mouseEvent);
using FnThisVoid = int(__fastcall*)(Address self, int edx);
using FnThisText = int(__fastcall*)(Address self, int edx, const char* text);
using FnThisTextInt = int(__fastcall*)(Address self, int edx, const char* text, int value);
using FnThisColor = int(__fastcall*)(Address self, int edx, const std::uint32_t* argb);
using FnThisFont = int(__fastcall*)(Address self, int edx, const char* font, float height, int flags);
using FnThisLayerParent = int(__fastcall*)(Address self, int edx, Address parent, int update, int zero);
using FnThisBackdropTexture = int(__fastcall*)(Address self, int edx, const char* path, int zero, int tile,
    Address none, int update);
using FnThisAlphaZero = int(__fastcall*)(Address self, int edx, int alpha, int zero);
using FnRegion = int(__fastcall*)(Address self);

// 运行时解析出的绝对地址。测试用同样调用约定的替身函数和替身全局填写。
struct GameUiEntryPoints {
    FnGetGameUi getGameUi = nullptr;
    FnCreateFrame createFrame = nullptr;
    FnCreateSimpleFrame createSimpleFrame = nullptr;
    FnFind findFrame = nullptr;
    FnFind findSimpleFrame = nullptr;
    FnFind findSimpleTexture = nullptr;
    FnFind findSimpleFontString = nullptr;
    FnLoadToc loadToc = nullptr;
    FnThisGrow growStringTable = nullptr;
    FnThisGrow growFdfTable = nullptr;

    FnThisAbsPoint setAbsPoint = nullptr;
    FnThisRelPoint setRelPoint = nullptr;
    FnThisAllPoints setAllPoints = nullptr;
    FnThisInt clearAllPoints = nullptr;
    FnThisFloat setWidth = nullptr;
    FnThisFloat setHeight = nullptr;
    FnThisFloat setScale = nullptr;
    Address layoutHeight = 0;       // 只用来判断类别（虚表 0x1C 槽）
    Address fontStringHeight = 0;

    FnThisAlphaZero layerSetAlpha = nullptr;
    FnThisLayerParent layerSetParent = nullptr;
    FnThisFont layerSetFont = nullptr;
    FnThisInt simpleSetAlpha = nullptr;
    FnThisAddress simpleSetParent = nullptr;
    FnThisInt simpleSetLevel = nullptr;
    FnRegion regionShow = nullptr;
    FnRegion regionHide = nullptr;
    FnThisColor regionSetColor = nullptr;

    FnThisBackdropTexture backdropSetTexture = nullptr;
    FnThisText textSetText = nullptr;
    FnThisColor textSetColor = nullptr;
    FnThisInt textSetAlignH = nullptr;
    FnThisInt textSetAlignV = nullptr;
    FnThisVoid textUpdateAlign = nullptr;
    FnThisText fontStringSetText = nullptr;
    FnThisFont fontStringSetFont = nullptr;
    FnThisInt fontStringSetAlignH = nullptr;
    FnThisInt fontStringSetAlignV = nullptr;
    FnThisTextInt textureSetTexture = nullptr;
    FnThisTextInt statusBarSetTexture = nullptr;
    FnThisFloat statusBarSetValue = nullptr;
    FnThisFloat2 statusBarSetLimits = nullptr;   // (最小, 最大)
    FnThisFloatInt sliderSetValue = nullptr;
    FnThisTextInt editBoxSetText = nullptr;
    FnThisTextInt modelSetModel = nullptr;
    FnThisInt controlSetEnabled = nullptr;

    Address stringTable = 0;
    Address fdfTable = 0;
    Address defaultStatus = 0;
    Address activeLayer = 0;
    Address mouseEvent = 0;
    FnThisHitTest hitTestLayer = nullptr;
    std::function<void(Address, Address, bool)> observePointer;
    Address simpleTop = 0;

    Address vtBackdrop = 0;
    Address vtText = 0;
    Address vtEditBox = 0;
    Address vtSlider = 0;
    Address vtModel = 0;
    Address vtSimpleStatusBar = 0;
    Address vtSimpleTexture = 0;
    Address vtSimpleFontString = 0;
};

// 按 Game.dll 基址解析。executable 核对每个函数入口确实落在已映射的代码段（由 GameImage127 提供）。
// 任何一项不通过就整体失败，不返回部分可用的表。
[[nodiscard]] bool ResolveGameUiEntryPoints127(Address base,
    const std::function<bool(Address, std::size_t)>& executable, GameUiEntryPoints& out, std::string& error);

} // namespace Audastra::Runtime32::GameUi
