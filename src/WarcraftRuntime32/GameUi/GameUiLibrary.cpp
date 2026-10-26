#include "GameUiLibrary.h"
#include <atomic>
#include <limits>

#include <windows.h>

#include <string>

namespace Audastra::Runtime32::GameUi {
namespace {

constexpr std::size_t kMaxSlots = 0xFFFF;
constexpr std::size_t kMaxEvents = 256;
constexpr int kMaxDepth = 64;
constexpr int kTableCapacity = 0xFFFF;

// 读游戏内存时对象可能已被游戏释放；读失败只当作“没有”，不让异常越过界面库。
bool ReadWord(Address at, Address& value) noexcept {
    if (!at) return false;
    __try {
        value = *reinterpret_cast<const Address*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteWord(Address at, Address value) noexcept {
    __try {
        *reinterpret_cast<Address*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 调用游戏函数；出现结构化异常时返回 false，由上层停用界面库。
int CaptureFault(EXCEPTION_POINTERS* information, UiFault& fault) noexcept {
    if (information && information->ExceptionRecord) {
        fault.code = information->ExceptionRecord->ExceptionCode;
        fault.address = reinterpret_cast<Address>(information->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
template <class Call>
bool Guarded(Call& call, UiFault& fault) noexcept {
    __try {
        call();
        return true;
    } __except (CaptureFault(GetExceptionInformation(), fault)) {
        return false;
    }
}

using VirtualCall = int(__fastcall*)(Address self);
using VirtualCallInt = int(__fastcall*)(Address self, int edx, int value);

bool ValidPoint(int point) noexcept { return point >= 0 && point <= 8; }

bool Complete(const GameUiEntryPoints& e) noexcept {
    return e.getGameUi && e.createFrame && e.createSimpleFrame && e.findFrame && e.findSimpleFrame
        && e.findSimpleTexture && e.findSimpleFontString && e.loadToc && e.growStringTable && e.growFdfTable
        && e.setAbsPoint && e.setRelPoint && e.setAllPoints && e.clearAllPoints && e.setWidth && e.setHeight
        && e.setScale && e.layoutHeight && e.fontStringHeight && e.layerSetAlpha && e.layerSetParent
        && e.layerSetFont && e.simpleSetAlpha && e.simpleSetParent && e.simpleSetLevel && e.regionShow
        && e.regionHide && e.regionSetColor && e.backdropSetTexture && e.textSetText && e.textSetColor
        && e.textSetAlignH && e.textSetAlignV && e.textUpdateAlign && e.fontStringSetText && e.fontStringSetFont
        && e.fontStringSetAlignH && e.fontStringSetAlignV && e.textureSetTexture && e.statusBarSetTexture
        && e.statusBarSetValue && e.statusBarSetLimits && e.sliderSetValue && e.editBoxSetText && e.modelSetModel
        && e.controlSetEnabled && e.stringTable && e.fdfTable && e.defaultStatus && e.hoveredLayer
        && e.pressedLayer && e.simpleTop && e.vtBackdrop && e.vtText && e.vtEditBox && e.vtSlider && e.vtModel
        && e.vtSimpleStatusBar && e.vtSimpleTexture && e.vtSimpleFontString;
}

} // namespace

std::string_view UiErrorText(UiError error) noexcept {
    switch (error) {
    case UiError::None: return "成功";
    case UiError::NotReady: return "界面库不可用：游戏版本不受支持或游戏界面尚未创建";
    case UiError::WrongThread: return "界面库只能在游戏主线程调用";
    case UiError::BadHandle: return "框架句柄无效或已失效";
    case UiError::StaleFrame: return "框架已被游戏销毁";
    case UiError::Unsupported: return "这类框架不支持该操作";
    case UiError::NotFound: return "模板、框架名字或界面文件不存在";
    case UiError::BadArgument: return "参数超出范围";
    case UiError::TableFull: return "框架句柄已用完";
    case UiError::NotOwned: return "游戏自带的框架不能删除";
    case UiError::Faulted: return "游戏界面函数出错，界面库已停用";
    case UiError::UnsupportedParent: return "父框架家族不支持此操作；Simple创建只能使用默认根或Simple父，改父级只能使用Simple父";
    case UiError::ParentCycle: return "父框架关系形成循环或超过64层";
    }
    return "未知错误";
}

GameUiLibrary::GameUiLibrary(const GameUiEntryPoints& entries, UiFaultObserver observer) noexcept
    : entries_(entries), observer_(observer), complete_(Complete(entries)), thread_(GetCurrentThreadId()) {
    slots_.resize(1);
}

void GameUiLibrary::SetDiagnosticContext(std::uint32_t epoch, std::uint32_t request, const char* entry) noexcept {
    callContext_.epoch = epoch;
    callContext_.request = request;
    callContext_.entry = entry;
}

void GameUiLibrary::ResetSession() noexcept {
    for (std::size_t index = 1; index < slots_.size(); ++index) {
        if (slots_[index].live) Kill(index);
    }
    events_.clear();
    buttonDown_ = false;
    gameRoot_ = 0;
}

UiError GameUiLibrary::Ready() const noexcept {
    if (faulted_) return UiError::Faulted;
    if (!complete_) return UiError::NotReady;
    if (GetCurrentThreadId() != thread_) return UiError::WrongThread;
    return UiError::None;
}

template <class Call>
UiError GameUiLibrary::Run(Call&& call) {
    auto fault = callContext_;
    if (!Guarded(call, fault)) {
        faulted_ = true;
        lastFault_ = fault;
        if (observer_) observer_(lastFault_);
        return UiError::Faulted;
    }
    return UiError::None;
}

FrameHandle GameUiLibrary::HandleOf(std::size_t index) const noexcept {
    return slots_[index].token;
}

bool GameUiLibrary::Classify(Address object, Address& vtable, FrameFamily& family) const noexcept {
    Address height = 0;
    if (!ReadWord(object, vtable) || !vtable || !ReadWord(vtable + Layout127::SlotHeight, height)) return false;
    if (vtable == entries_.vtSimpleTexture || vtable == entries_.vtSimpleFontString) family = FrameFamily::Region;
    else if (height == entries_.layoutHeight || height == entries_.fontStringHeight) family = FrameFamily::Simple;
    else family = FrameFamily::Classic;
    return true;
}

Address GameUiLibrary::LayoutOf(const Slot& slot) const noexcept {
    return slot.family == FrameFamily::Classic ? slot.object + Layout127::ClassicLayout : slot.object;
}

Address GameUiLibrary::ParentOf(Address object) const noexcept {
    Address vtable = 0;
    FrameFamily family{};
    if (!Classify(object, vtable, family)) return 0;
    const auto offset = family == FrameFamily::Region ? Layout127::RegionParent
        : family == FrameFamily::Simple ? Layout127::SimpleParent : Layout127::ClassicParent;
    Address parent = 0;
    return ReadWord(object + offset, parent) ? parent : 0;
}

bool GameUiLibrary::IsAncestorOrSelf(Address ancestor, Address object) const noexcept {
    for (int depth = 0; object && depth < kMaxDepth; ++depth) {
        if (object == ancestor) return true;
        object = ParentOf(object);
    }
    return false;
}

void GameUiLibrary::Kill(std::size_t index) noexcept {
    auto& slot = slots_[index];
    byObject_.erase(slot.object);
    byToken_.erase(slot.token);
    slot.token = 0;
    slot.live = false;
    slot.object = slot.vtable = 0;
    slot.watch = 0;
    slot.hovered = slot.armed = false;
    free_.push_back(static_cast<std::uint16_t>(index));
}

UiCreated GameUiLibrary::Adopt(Address object, Owner owner) {
    if (!object) return {0, UiError::NotFound};
    if (const auto found = byObject_.find(object); found != byObject_.end()) {
        auto& slot = slots_[found->second];
        if (owner == Owner::Library) slot.owner = Owner::Library;
        return {HandleOf(found->second), UiError::None};
    }
    Address vtable = 0;
    FrameFamily family{};
    if (!Classify(object, vtable, family)) return {0, UiError::StaleFrame};
    // 由进程拥有者保活的代码分配单调身份；新模块实例不能重新发出上一图的整数。
    // 达到正JASS整数上限后永久拒绝，不回绕或回收token。
    static std::atomic<std::uint64_t> nextToken{1};
    auto token = nextToken.load(std::memory_order_relaxed);
    do {
        if (token > static_cast<std::uint64_t>(std::numeric_limits<FrameHandle>::max())) return {0, UiError::TableFull};
    } while (!nextToken.compare_exchange_weak(token, token + 1, std::memory_order_relaxed));
    std::size_t index = 0;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        if (slots_.size() > kMaxSlots) return {0, UiError::TableFull};
        index = slots_.size();
        slots_.emplace_back();
    }
    auto& slot = slots_[index];
    slot.token = static_cast<FrameHandle>(token);
    slot.object = object;
    slot.vtable = vtable;
    slot.family = family;
    slot.owner = owner;
    slot.live = true;
    byObject_[object] = static_cast<std::uint16_t>(index);
    byToken_[slot.token] = static_cast<std::uint16_t>(index);
    return {HandleOf(index), UiError::None};
}

GameUiLibrary::Slot* GameUiLibrary::Resolve(FrameHandle handle, UiError& error) {
    error = Ready();
    if (error != UiError::None) return nullptr;
    const auto found = byToken_.find(handle);
    if (handle <= 0 || found == byToken_.end()) {
        error = UiError::BadHandle;
        return nullptr;
    }
    const auto index = found->second;
    auto& slot = slots_[index];
    Address vtable = 0;
    if (!ReadWord(slot.object, vtable) || vtable != slot.vtable) {
        Kill(index);
        error = UiError::StaleFrame;
        return nullptr;
    }
    return &slot;
}

UiCreated GameUiLibrary::Root() {
    if (const auto ready = Ready(); ready != UiError::None) return {0, ready};
    Address root = 0;
    if (Run([&] { root = entries_.getGameUi(0, 0); }) != UiError::None) return {0, UiError::Faulted};
    if (!root) return {0, UiError::NotReady};
    const auto adopted = Adopt(root, Owner::Game);
    if (adopted.error == UiError::None) gameRoot_ = root;
    return adopted;
}

UiCreated GameUiLibrary::World() {
    const auto root = Root();
    if (root.error != UiError::None) return root;
    UiError error{};
    const auto* slot = Resolve(root.handle, error);
    Address world = 0;
    if (!slot || !ReadWord(slot->object + Layout127::GameUiWorldFrame, world) || !world) return {0, UiError::NotReady};
    return Adopt(world, Owner::Game);
}

UiCreated GameUiLibrary::Find(std::string_view name, int context) {
    if (const auto ready = Ready(); ready != UiError::None) return {0, ready};
    if (name.empty()) return {0, UiError::BadArgument};
    const std::string text(name);
    Address found = 0;
    const FnFind finders[] = {entries_.findFrame, entries_.findSimpleFrame, entries_.findSimpleTexture,
        entries_.findSimpleFontString};
    for (const auto finder : finders) {
        if (Run([&] { found = finder(text.c_str(), context); }) != UiError::None) return {0, UiError::Faulted};
        if (found) return Adopt(found, Owner::Game);
    }
    return {0, UiError::NotFound};
}

UiCreated GameUiLibrary::Create(std::string_view templateName, FrameHandle parent, int level, int context) {
    if (const auto ready = Ready(); ready != UiError::None) return {0, ready};
    if (templateName.empty()) return {0, UiError::BadArgument};
    if (!parent) {
        const auto root = Root();
        if (root.error != UiError::None) return root;
        parent = root.handle;
    }
    UiError error{};
    const auto* owner = Resolve(parent, error);
    if (!owner) return {0, error};
    if (owner->family == FrameFamily::Region) return {0, UiError::UnsupportedParent};
    const std::string text(templateName);
    Address created = 0;
    const auto parentObject = owner->object;
    if (Run([&] { created = entries_.createFrame(text.c_str(), parentObject, 0, level, context); }) != UiError::None)
        return {0, UiError::Faulted};
    return Adopt(created, Owner::Library);
}

UiCreated GameUiLibrary::CreateSimple(std::string_view templateName, FrameHandle parent, int context) {
    if (const auto ready = Ready(); ready != UiError::None) return {0, ready};
    if (templateName.empty()) return {0, UiError::BadArgument};
    Address parentObject = 0;
    if (parent) {
        UiError error{};
        const auto* owner = Resolve(parent, error);
        if (!owner) return {0, error};
        // GameUI不是CSimpleFrame。仅这个已由Root核验的token可作为默认根别名，不能把任意Classic静默改根。
        if (owner->object != gameRoot_) {
            if (owner->family != FrameFamily::Simple) return {0, UiError::UnsupportedParent};
            parentObject = owner->object;
        }
    }
    const std::string text(templateName);
    Address created = 0;
    if (Run([&] { created = entries_.createSimpleFrame(text.c_str(), parentObject, context); }) != UiError::None)
        return {0, UiError::Faulted};
    return Adopt(created, Owner::Library);
}

UiError GameUiLibrary::LoadToc(std::string_view path) {
    if (const auto ready = Ready(); ready != UiError::None) return ready;
    if (path.empty()) return UiError::BadArgument;
    const std::string text(path);
    Address strings = 0, fdf = 0;
    if (!ReadWord(entries_.stringTable + Layout127::TableCapacity, strings)
        || !ReadWord(entries_.fdfTable + Layout127::TableCapacity, fdf))
        return UiError::NotReady;
    int loaded = 0;
    const auto run = Run([&] {
        if (strings < kTableCapacity) entries_.growStringTable(entries_.stringTable, 0, kTableCapacity);
        if (fdf < kTableCapacity) entries_.growFdfTable(entries_.fdfTable, 0, kTableCapacity);
        loaded = entries_.loadToc(text.c_str(), entries_.stringTable, entries_.fdfTable, entries_.defaultStatus);
    });
    if (run != UiError::None) return run;
    return loaded ? UiError::None : UiError::NotFound;
}

UiError GameUiLibrary::SetPoint(FrameHandle frame, int point, FrameHandle relative, int relativePoint, float x, float y) {
    UiError error{};
    auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (!ValidPoint(point) || !ValidPoint(relativePoint)) return UiError::BadArgument;
    const auto layout = LayoutOf(*slot);
    const auto* target = Resolve(relative, error);
    if (!target) return error;
    const auto targetLayout = LayoutOf(*target);
    return Run([&] { entries_.setRelPoint(layout, 0, point, targetLayout, relativePoint, x, y, 1); });
}

UiError GameUiLibrary::SetAbsPoint(FrameHandle frame, int point, float x, float y) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (!ValidPoint(point)) return UiError::BadArgument;
    const auto layout = LayoutOf(*slot);
    return Run([&] { entries_.setAbsPoint(layout, 0, point, x, y, 1); });
}

UiError GameUiLibrary::SetAllPoints(FrameHandle frame, FrameHandle relative) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto layout = LayoutOf(*slot);
    const auto* target = Resolve(relative, error);
    if (!target) return error;
    const auto targetLayout = LayoutOf(*target);
    return Run([&] { entries_.setAllPoints(layout, 0, targetLayout, 1); });
}

UiError GameUiLibrary::ClearPoints(FrameHandle frame) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto layout = LayoutOf(*slot);
    return Run([&] { entries_.clearAllPoints(layout, 0, 1); });
}

UiError GameUiLibrary::SetWidth(FrameHandle frame, float width) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (!(width >= 0.0f)) return UiError::BadArgument;
    const auto layout = LayoutOf(*slot);
    return Run([&] { entries_.setWidth(layout, 0, width); });
}

UiError GameUiLibrary::SetHeight(FrameHandle frame, float height) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (!(height >= 0.0f)) return UiError::BadArgument;
    const auto layout = LayoutOf(*slot);
    return Run([&] { entries_.setHeight(layout, 0, height); });
}

UiError GameUiLibrary::SetScale(FrameHandle frame, float scale) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (!(scale > 0.0f)) return UiError::BadArgument;
    const auto layout = LayoutOf(*slot);
    return Run([&] { entries_.setScale(layout, 0, scale); });
}

UiError GameUiLibrary::SetVisible(FrameHandle frame, bool visible) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto object = slot->object;
    if (slot->family == FrameFamily::Region)
        return Run([&] { visible ? entries_.regionShow(object) : entries_.regionHide(object); });
    const auto slotOffset = slot->family == FrameFamily::Classic
        ? (visible ? Layout127::SlotClassicShow : Layout127::SlotClassicHide)
        : (visible ? Layout127::SlotSimpleShow : Layout127::SlotSimpleHide);
    Address function = 0;
    if (!ReadWord(slot->vtable + slotOffset, function) || !function) return UiError::Unsupported;
    return Run([&] { reinterpret_cast<VirtualCall>(function)(object); });
}

UiError GameUiLibrary::SetAlpha(FrameHandle frame, int alpha) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (alpha < 0 || alpha > 255) return UiError::BadArgument;
    const auto object = slot->object;
    switch (slot->family) {
    case FrameFamily::Classic: return Run([&] { entries_.layerSetAlpha(object, 0, alpha, 0); });
    case FrameFamily::Simple: return Run([&] { entries_.simpleSetAlpha(object, 0, alpha); });
    case FrameFamily::Region: break;
    }
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetLevel(FrameHandle frame, int level) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto object = slot->object;
    if (slot->family == FrameFamily::Simple) return Run([&] { entries_.simpleSetLevel(object, 0, level); });
    if (slot->family != FrameFamily::Classic) return UiError::Unsupported;
    // 经典框架没有设层级的函数：游戏做法是先隐藏，改层级字段，再显示，让它按新层级重新排进渲染。
    Address hide = 0, show = 0;
    if (!ReadWord(slot->vtable + Layout127::SlotClassicHide, hide)
        || !ReadWord(slot->vtable + Layout127::SlotClassicShow, show) || !hide || !show)
        return UiError::Unsupported;
    bool written = true;
    const auto run = Run([&] {
        reinterpret_cast<VirtualCall>(hide)(object);
        written = WriteWord(object + Layout127::ClassicPriority, static_cast<Address>(level));
        reinterpret_cast<VirtualCall>(show)(object);
    });
    if (run != UiError::None) return run;
    return written ? UiError::None : UiError::StaleFrame;
}

UiError GameUiLibrary::SetParent(FrameHandle frame, FrameHandle parent) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto object = slot->object;
    const auto family = slot->family;
    const auto* target = Resolve(parent, error);
    if (!target) return error;
    // 游戏的两个改父级函数都要求新父级是 Simple 框架。
    if (target->family != FrameFamily::Simple) return UiError::UnsupportedParent;
    const auto parentObject = target->object;
    Address ancestor = parentObject;
    for (int depth = 0; ancestor && depth < kMaxDepth; ++depth) {
        if (ancestor == object) return UiError::ParentCycle;
        ancestor = ParentOf(ancestor);
    }
    if (ancestor) return UiError::ParentCycle;
    if (family == FrameFamily::Classic) return Run([&] { entries_.layerSetParent(object, 0, parentObject, 1, 0); });
    if (family == FrameFamily::Simple) return Run([&] { entries_.simpleSetParent(object, 0, parentObject); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetText(FrameHandle frame, std::string_view text) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const std::string value(text);
    const auto object = slot->object;
    const auto vtable = slot->vtable;
    if (vtable == entries_.vtText) return Run([&] { entries_.textSetText(object, 0, value.c_str()); });
    if (vtable == entries_.vtSimpleFontString) return Run([&] { entries_.fontStringSetText(object, 0, value.c_str()); });
    if (vtable == entries_.vtEditBox) return Run([&] { entries_.editBoxSetText(object, 0, value.c_str(), 1); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetFont(FrameHandle frame, std::string_view font, float height, int flags) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (font.empty() || !(height > 0.0f)) return UiError::BadArgument;
    const std::string value(font);
    const auto object = slot->object;
    if (slot->vtable == entries_.vtSimpleFontString)
        return Run([&] { entries_.fontStringSetFont(object, 0, value.c_str(), height, flags); });
    if (slot->family == FrameFamily::Classic)
        return Run([&] { entries_.layerSetFont(object, 0, value.c_str(), height, flags); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetTextAlign(FrameHandle frame, int horizontal, int vertical) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (horizontal < 0 || horizontal > 2 || vertical < 0 || vertical > 2) return UiError::BadArgument;
    const auto object = slot->object;
    if (slot->vtable == entries_.vtText) {
        return Run([&] {
            entries_.textSetAlignH(object, 0, horizontal);
            entries_.textSetAlignV(object, 0, vertical);
            entries_.textUpdateAlign(object, 0);
        });
    }
    if (slot->vtable == entries_.vtSimpleFontString) {
        // Simple 文字的对齐是位标志：水平 8/16/32，竖直 1/2/4（待进游戏确认方向）。
        return Run([&] {
            entries_.fontStringSetAlignH(object, 0, 8 << horizontal);
            entries_.fontStringSetAlignV(object, 0, 1 << vertical);
        });
    }
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetColor(FrameHandle frame, std::uint32_t argb) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto object = slot->object;
    if (slot->vtable == entries_.vtText) return Run([&] { entries_.textSetColor(object, 0, &argb); });
    if (slot->family == FrameFamily::Region) return Run([&] { entries_.regionSetColor(object, 0, &argb); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetTexture(FrameHandle frame, std::string_view path, bool tile) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (path.empty()) return UiError::BadArgument;
    const std::string value(path);
    const auto object = slot->object;
    const auto vtable = slot->vtable;
    const int tiled = tile ? 1 : 0;
    if (vtable == entries_.vtBackdrop)
        return Run([&] { entries_.backdropSetTexture(object, 0, value.c_str(), 0, tiled, 0, 1); });
    if (vtable == entries_.vtSimpleTexture) return Run([&] { entries_.textureSetTexture(object, 0, value.c_str(), tiled); });
    if (vtable == entries_.vtSimpleStatusBar)
        return Run([&] { entries_.statusBarSetTexture(object, 0, value.c_str(), tiled); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetValue(FrameHandle frame, float value) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto object = slot->object;
    if (slot->vtable == entries_.vtSimpleStatusBar) return Run([&] { entries_.statusBarSetValue(object, 0, value); });
    if (slot->vtable == entries_.vtSlider) return Run([&] { entries_.sliderSetValue(object, 0, value, 1); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetLimits(FrameHandle frame, float minimum, float maximum) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (!(minimum <= maximum)) return UiError::BadArgument;
    const auto object = slot->object;
    if (slot->vtable == entries_.vtSimpleStatusBar)
        return Run([&] { entries_.statusBarSetLimits(object, 0, minimum, maximum); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetModel(FrameHandle frame, std::string_view path, int modelType) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (path.empty()) return UiError::BadArgument;
    const std::string value(path);
    const auto object = slot->object;
    if (slot->vtable == entries_.vtModel) return Run([&] { entries_.modelSetModel(object, 0, value.c_str(), modelType); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::SetEnabled(FrameHandle frame, bool enabled) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    const auto object = slot->object;
    // 启用/禁用是控件的函数；只给确认是控件的类调用。
    if (slot->vtable == entries_.vtEditBox || slot->vtable == entries_.vtSlider)
        return Run([&] { entries_.controlSetEnabled(object, 0, enabled ? 1 : 0); });
    return UiError::Unsupported;
}

UiError GameUiLibrary::Destroy(FrameHandle frame) {
    UiError error{};
    const auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (slot->owner != Owner::Library) return UiError::NotOwned;
    const auto object = slot->object;
    const auto offset = slot->family == FrameFamily::Classic ? Layout127::SlotClassicDelete : Layout127::SlotDelete;
    Address function = 0;
    if (!ReadWord(slot->vtable + offset, function) || !function) return UiError::Unsupported;
    // 子框架随父框架一起被游戏删除；先作废挂在它下面的句柄，删除后不再读那些内存。
    for (std::size_t index = 1; index < slots_.size(); ++index) {
        if (slots_[index].live && IsAncestorOrSelf(object, slots_[index].object)) Kill(index);
    }
    return Run([&] { reinterpret_cast<VirtualCallInt>(function)(object, 0, 1); });
}

UiError GameUiLibrary::Watch(FrameHandle frame, std::uint8_t mask) {
    UiError error{};
    auto* slot = Resolve(frame, error);
    if (!slot) return error;
    if (mask & ~(kUiEventEnter | kUiEventLeave | kUiEventClick)) return UiError::BadArgument;
    slot->watch = mask;
    if (!mask) slot->hovered = slot->armed = false;
    return UiError::None;
}

PointerSnapshot GameUiLibrary::ReadPointer() const {
    PointerSnapshot pointer;
    if (Ready() != UiError::None) return pointer;
    Address hovered = 0, top = 0;
    if (!ReadWord(entries_.hoveredLayer, hovered)) return pointer;
    // 鼠标在 Simple 系框架上时，经典层拿到的是全局 CSimpleTop，再从它取具体对象。
    if (hovered && ReadWord(entries_.simpleTop, top) && top && hovered == top) {
        Address simple = 0;
        hovered = ReadWord(top + Layout127::SimpleTopHovered, simple) ? simple : 0;
    }
    pointer.hovered = hovered;
    pointer.buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    return pointer;
}

void GameUiLibrary::Poll(const PointerSnapshot& pointer) {
    if (Ready() != UiError::None) return;
    auto push = [&](std::size_t index, std::uint8_t kind) {
        if (events_.size() < kMaxEvents) events_.push_back({HandleOf(index), kind});
    };
    const bool pressed = pointer.buttonDown && !buttonDown_;
    const bool released = !pointer.buttonDown && buttonDown_;
    buttonDown_ = pointer.buttonDown;
    for (std::size_t index = 1; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (!slot.live || !slot.watch) continue;
        Address vtable = 0;
        if (!ReadWord(slot.object, vtable) || vtable != slot.vtable) {
            Kill(index);
            continue;
        }
        const bool inside = pointer.hovered && IsAncestorOrSelf(slot.object, pointer.hovered);
        if (inside && !slot.hovered && (slot.watch & kUiEventEnter)) push(index, kUiEventEnter);
        if (!inside && slot.hovered && (slot.watch & kUiEventLeave)) push(index, kUiEventLeave);
        slot.hovered = inside;
        // 点击 = 左键在框架上按下，并在框架上松开。
        if (pressed) slot.armed = inside;
        if (released) {
            if (slot.armed && inside && (slot.watch & kUiEventClick)) push(index, kUiEventClick);
            slot.armed = false;
        }
    }
}

std::vector<UiFrameEvent> GameUiLibrary::TakeEvents() {
    std::vector<UiFrameEvent> taken;
    taken.swap(events_);
    return taken;
}

std::size_t GameUiLibrary::LiveCount() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 1; index < slots_.size(); ++index) count += slots_[index].live ? 1 : 0;
    return count;
}

} // namespace Audastra::Runtime32::GameUi
