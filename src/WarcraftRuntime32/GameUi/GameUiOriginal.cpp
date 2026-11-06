#include "GameUiLibrary.h"
#include "GameUiMemory.h"

#include <algorithm>
#include <array>
#include <cmath>

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
    {"Minimap", 0x3C0, &GameUiEntryPoints::vtMinimap}, {"Portrait", 0x3F4, &GameUiEntryPoints::vtPortrait},
    {"CommandBar", 0x3C8, &GameUiEntryPoints::vtCommandBar}, {"InfoBar", 0x3C4, &GameUiEntryPoints::vtInfoBar},
    {"HeroBar", 0x3DC, &GameUiEntryPoints::vtHeroBar}, {"TimeOfDay", 0x3F8, &GameUiEntryPoints::vtTimeOfDay},
    {"ChatMessage", 0x3EC, &GameUiEntryPoints::vtMessage}, {"Message", 0x3E4, &GameUiEntryPoints::vtMessage},
    {"UnitMessage", 0x3E8, &GameUiEntryPoints::vtMessage}, {"TopMessage", 0x3F0, &GameUiEntryPoints::vtMessage},
    {"MinimapButton1", 0x408, &GameUiEntryPoints::vtSimpleButton}, {"MinimapButton2", 0x40C, &GameUiEntryPoints::vtSimpleButton},
    {"MinimapButton3", 0x410, &GameUiEntryPoints::vtSimpleButton}, {"MinimapButton4", 0x414, &GameUiEntryPoints::vtSimpleButton},
    {"MinimapButton5", 0x418, &GameUiEntryPoints::vtSimpleButton}};
constexpr const char* kInventory[] = {"InventoryButton1", "InventoryButton2", "InventoryButton3", "InventoryButton4", "InventoryButton5", "InventoryButton6"};
}

UiCreated GameUiLibrary::Original(std::string_view name) {
    if (const auto ready = Ready(); ready != UiError::None) return {0, ready};
    originalProblem_.clear();
    const auto fail = [&](UiError error) -> UiCreated {
        originalProblem_ = "原版界面 " + std::string(name) + "：" + std::string(UiErrorText(error));
        return {0, error};
    };
    if (name == "consoleui" || name == "resourcebarframe" || name == "upperbuttonbarframe" || name == "WorldFrame") {
        const auto found = name == "WorldFrame" ? World() : Find(name == "consoleui" ? "ConsoleUI" : name == "resourcebarframe" ? "ResourceBarFrame" : "UpperButtonBarFrame", 0);
        return found.error == UiError::None ? found : fail(found.error);
    }
    const auto field = std::find_if(std::begin(kFields), std::end(kFields), [&](const auto& candidate) { return name == candidate.name; });
    const auto inventory = std::find_if(std::begin(kInventory), std::end(kInventory), [&](const auto* candidate) { return name == candidate; });
    if (field == std::end(kFields) && inventory == std::end(kInventory)) return fail(UiError::Unsupported);
    const auto root = Root();
    if (root.error != UiError::None) return fail(root.error);
    const auto verify = [&](Address object, Address expected) {
        Address actual = 0;
        return object && expected && ReadWord(object, actual) && actual == expected;
    };
    Address object = 0;
    if (field != std::end(kFields)) {
        if (!ReadWord(gameRoot_ + field->offset, object) || !object) return fail(UiError::NotFound);
        if (!verify(object, entries_.*(field->vtable))) return fail(UiError::Unsupported);
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
    if (point < 0 || point > 8 || !std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 3 || std::abs(y) > 3) return UiError::BadArgument;
    const auto frame = Original(name); if (frame.error != UiError::None) return frame.error;
    auto status = ClearPoints(frame.handle);
    if (status == UiError::None) status = SetAbsPoint(frame.handle, point, x, y);
    if (status == UiError::None) OriginalSettings(name).position = OriginalState::Position{point, x, y};
    return status;
}
UiError GameUiLibrary::SetOriginalScale(std::string_view name, float value) {
    if (!std::isfinite(value) || value < .1f || value > 4) return UiError::BadArgument;
    const auto frame = Original(name); if (frame.error != UiError::None) return frame.error;
    const auto status = SetScale(frame.handle, value);
    if (status == UiError::None) OriginalSettings(name).scale = value;
    return status;
}
UiError GameUiLibrary::SetOriginalAlpha(std::string_view name, int value) {
    if (value < 0 || value > 255) return UiError::BadArgument;
    const auto frame = Original(name); if (frame.error != UiError::None) return frame.error;
    const auto status = SetAlpha(frame.handle, value);
    if (status == UiError::None) OriginalSettings(name).alpha = value;
    return status;
}
UiError GameUiLibrary::RefreshOriginals() {
    if (const auto ready = Ready(); ready != UiError::None) return ready;
    UiError first = UiError::None;
    std::string reason;
    for (const auto& state : originals_) {
        const auto frame = Original(state.name);
        auto status = frame.error;
        if (status == UiError::None && state.scale) status = SetScale(frame.handle, *state.scale);
        if (status == UiError::None && state.position) {
            status = ClearPoints(frame.handle);
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
