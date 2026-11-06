#include "GameUiLibrary.h"
#include "GameUiMemory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

namespace Audastra::Runtime32::GameUi {
namespace {
using Detail::ReadWord;
struct Field {
    const char* name;
    std::uint32_t offset;
    Address GameUiEntryPoints::* vtable;
};
// CGameUI构造349FA0、350110、CInfoBar构造380F40及物品面板381BF0的写入。
constexpr Field kFields[] = {
    {"consoleui", 0x428, &GameUiEntryPoints::vtConsole},
    {"resourcebarframe", 0x3CC, &GameUiEntryPoints::vtResourceBar},
    {"upperbuttonbarframe", 0x3D0, &GameUiEntryPoints::vtUpperButtonBar},
    {"Minimap", 0x3C0, &GameUiEntryPoints::vtMinimap}, {"Portrait", 0x3F4, &GameUiEntryPoints::vtPortrait},
    {"CommandBar", 0x3C8, &GameUiEntryPoints::vtCommandBar}, {"InfoBar", 0x3C4, &GameUiEntryPoints::vtInfoBar},
    {"HeroBar", 0x3DC, &GameUiEntryPoints::vtHeroBar}, {"TimeOfDay", 0x3F8, &GameUiEntryPoints::vtTimeOfDay},
    {"ChatMessage", 0x3EC, &GameUiEntryPoints::vtMessage}, {"Message", 0x3E4, &GameUiEntryPoints::vtMessage},
    {"UnitMessage", 0x3E8, &GameUiEntryPoints::vtMessage}, {"TopMessage", 0x3F0, &GameUiEntryPoints::vtMessage},
    {"MinimapButton1", 0x408, &GameUiEntryPoints::vtSimpleButton}, {"MinimapButton2", 0x40C, &GameUiEntryPoints::vtSimpleButton},
    {"MinimapButton3", 0x410, &GameUiEntryPoints::vtSimpleButton}, {"MinimapButton4", 0x414, &GameUiEntryPoints::vtSimpleButton},
    {"MinimapButton5", 0x418, &GameUiEntryPoints::vtSimpleButton}};
constexpr const char* kInventory[] = {"InventoryButton1", "InventoryButton2", "InventoryButton3", "InventoryButton4", "InventoryButton5", "InventoryButton6"};

bool ReadFloat(Address at, float& value) {
    Address bits = 0;
    if (!ReadWord(at, bits)) return false;
    value = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
    return std::isfinite(value);
}
// CLayout复制矩形0BCE10、差值0BD343..0BD34D及缩放尺寸0BCDF0/0BCE40。
// 两角锚定的框架可能没有显式宽高；清锚前保留已解出的未缩放尺寸。
bool OriginalDimensions(Address layout, float& width, float& height) {
    float scale = 0;
    if (!ReadFloat(layout + 0x60, scale) || scale <= 0 || scale > 16 ||
        !ReadFloat(layout + 0x58, width) || !ReadFloat(layout + 0x5C, height)) return false;
    if (width <= 0 || height <= 0) {
        Address resolved = 0;
        std::array<float, 4> rect{};
        if (!ReadWord(layout + 0x54, resolved) || !resolved) return false;
        for (unsigned i = 0; i != rect.size(); ++i) if (!ReadFloat(layout + 0x44 + 4 * i, rect[i])) return false;
        if (width <= 0) width = (rect[2] - rect[0]) / scale;
        if (height <= 0) height = (rect[3] - rect[1]) / scale;
    }
    return std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0 && width <= 4 && height <= 4;
}
}

UiCreated GameUiLibrary::Original(std::string_view name) {
    originalProblem_.clear();
    const auto fail = [&](UiError error) -> UiCreated {
        originalProblem_ = "原版界面 " + std::string(name) + "：" + std::string(UiErrorText(error));
        return {0, error};
    };
    if (const auto ready = Ready(); ready != UiError::None) return fail(ready);
    // 消息框有独立的生命周期；通用布局调整尚未通过实机验收，不向其分派布局调用。
    if (name == "ChatMessage" || name == "Message" || name == "UnitMessage" || name == "TopMessage") {
        const auto result = fail(UiError::Unsupported);
        originalProblem_ += "（消息框布局未通过验收）";
        return result;
    }
    if (name == "WorldFrame") {
        const auto found = World();
        return found.error == UiError::None ? found : fail(found.error);
    }
    const auto field = std::find_if(std::begin(kFields), std::end(kFields), [&](const auto& candidate) { return name == candidate.name; });
    const auto inventory = std::find_if(std::begin(kInventory), std::end(kInventory), [&](const auto* candidate) { return name == candidate; });
    if (field == std::end(kFields) && inventory == std::end(kInventory)) return fail(UiError::Unsupported);
    const auto root = Root();
    if (root.error != UiError::None) return fail(root.error);
    std::string mismatch;
    const auto verify = [&](Address object, Address expected) {
        Address actual = 0;
        if (!object || !expected || !ReadWord(object, actual)) return false;
        if (actual == expected) return true;
        const auto base = entries_.vtSimpleFrame >= 0x95C8A4 ? entries_.vtSimpleFrame - 0x95C8A4 : 0;
        if (base && actual >= base && actual - base < 0x1000000 && expected >= base) {
            char text[96]{};
            std::snprintf(text, sizeof(text), " (type RVA=%06X, expected=%06X)",
                static_cast<unsigned>(actual - base), static_cast<unsigned>(expected - base));
            mismatch = text;
        }
        return false;
    };
    Address object = 0;
    if (field != std::end(kFields)) {
        if (!ReadWord(gameRoot_ + field->offset, object) || !object) return fail(UiError::NotFound);
        if (!verify(object, entries_.*(field->vtable))) {
            const auto result = fail(UiError::Unsupported); originalProblem_ += mismatch; return result;
        }
    } else {
        Address info = 0, panel = 0, count = 0, data = 0;
        if (!ReadWord(gameRoot_ + 0x3C4, info) || !verify(info, entries_.vtInfoBar) ||
            !ReadWord(info + 0x148, panel) || !verify(panel, entries_.vtInventory) ||
            !ReadWord(panel + 0x12C, count) || count != 6 || !ReadWord(panel + 0x130, data) || !data) return fail(UiError::NotFound);
        const auto index = static_cast<Address>(inventory - std::begin(kInventory));
        if (!ReadWord(data + 8 * index + 4, object) || !verify(object, entries_.vtCommandButton)) return fail(UiError::Unsupported);
    }
    const auto adopted = Adopt(object, Owner::Game);
    return adopted.error == UiError::None ? adopted : fail(adopted.error);
}

GameUiLibrary::OriginalState& GameUiLibrary::OriginalSettings(std::string_view name) {
    const auto found = std::find_if(originals_.begin(), originals_.end(), [&](const auto& state) { return state.name == name; });
    if (found != originals_.end()) return *found;
    originals_.push_back({std::string(name)});
    return originals_.back();
}

UiError GameUiLibrary::SetOriginalVisible(std::string_view name, bool value) {
    const auto frame = Original(name); if (frame.error != UiError::None) return frame.error;
    const auto status = SetVisible(frame.handle, value);
    if (status == UiError::None) OriginalSettings(name).visible = value;
    return status;
}
UiError GameUiLibrary::SetOriginalPosition(std::string_view name, int point, float x, float y) {
    originalProblem_.clear();
    if (point < 0 || point > 8 || !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 3 || std::abs(y) > 3) return UiError::BadArgument;
    const auto frame = Original(name); if (frame.error != UiError::None) return frame.error;
    UiError error{};
    const auto* slot = Resolve(frame.handle, error);
    if (!slot) return error;
    const auto previous = std::find_if(originals_.begin(), originals_.end(), [&](const auto& state) { return state.name == name; });
    float width = 0, height = 0;
    if (previous != originals_.end() && previous->position) {
        width = previous->position->width; height = previous->position->height;
    } else if (previous != originals_.end() && previous->baselineSize) {
        width = previous->baselineSize->width; height = previous->baselineSize->height;
    } else if (!OriginalDimensions(LayoutOf(*slot), width, height)) {
        originalProblem_ = "原版界面 " + std::string(name) + "：尚无有效布局尺寸，未修改锚点";
        return UiError::NotFound;
    }
    auto status = SetWidth(frame.handle, width);
    if (status == UiError::None) status = SetHeight(frame.handle, height);
    if (status == UiError::None) status = ClearPoints(frame.handle);
    if (status == UiError::None) status = SetAbsPoint(frame.handle, point, x, y);
    if (status == UiError::None) OriginalSettings(name).position = OriginalState::Position{point, x, y, width, height};
    return status;
}
UiError GameUiLibrary::SetOriginalScale(std::string_view name, float value) {
    originalProblem_.clear();
    if (!std::isfinite(value) || value < .1f || value > 4) return UiError::BadArgument;
    const auto frame = Original(name); if (frame.error != UiError::None) return frame.error;
    // 初次缩放前的矩形可能依赖两角锚定；布局尚未重算时，不再用新scale除旧缓存矩形。
    std::optional<OriginalState::Size> baseline;
    UiError error{};
    if (const auto* slot = Resolve(frame.handle, error)) {
        float width = 0, height = 0;
        if (OriginalDimensions(LayoutOf(*slot), width, height)) baseline = OriginalState::Size{width, height};
    }
    const auto status = SetScale(frame.handle, value);
    if (status == UiError::None) {
        auto& settings = OriginalSettings(name);
        settings.scale = value;
        if (!settings.baselineSize && baseline) settings.baselineSize = baseline;
    }
    return status;
}
UiError GameUiLibrary::SetOriginalAlpha(std::string_view name, int value) {
    originalProblem_.clear();
    if (value < 0 || value > 255) return UiError::BadArgument;
    const auto frame = Original(name); if (frame.error != UiError::None) return frame.error;
    const auto status = SetAlpha(frame.handle, value);
    if (status == UiError::None) OriginalSettings(name).alpha = value;
    return status;
}
UiError GameUiLibrary::RefreshOriginals() {
    originalProblem_.clear();
    if (const auto ready = Ready(); ready != UiError::None) return ready;
    UiError first = UiError::None;
    std::string reason;
    for (const auto& state : originals_) {
        const auto frame = Original(state.name);
        auto status = frame.error;
        if (status == UiError::None && state.scale) status = SetScale(frame.handle, *state.scale);
        if (status == UiError::None && state.position) {
            status = SetWidth(frame.handle, state.position->width);
            if (status == UiError::None) status = SetHeight(frame.handle, state.position->height);
            if (status == UiError::None) status = ClearPoints(frame.handle);
            if (status == UiError::None) status = SetAbsPoint(frame.handle, state.position->point, state.position->x, state.position->y);
        }
        if (status == UiError::None && state.alpha) status = SetAlpha(frame.handle, *state.alpha);
        if (status == UiError::None && state.visible) status = SetVisible(frame.handle, *state.visible);
        if (status != UiError::None && first == UiError::None) { first = status; reason = "原版界面 " + state.name + "：" + std::string(UiErrorText(status)); }
        if (status == UiError::Faulted) break;
    }
    originalProblem_ = std::move(reason);
    return first;
}
} // namespace Audastra::Runtime32::GameUi
