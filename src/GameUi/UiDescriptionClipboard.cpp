#include "War3MapCore/GameUi/UiDescriptionEdit.h"

#include "UiDescriptionText.h"
#include "../Core/Serialization/StrictJson.h"

#include <algorithm>
#include <functional>
#include <set>

namespace War3Map::GameUi {
namespace {
using Json = nlohmann::ordered_json;
using Names = std::set<std::string, std::less<>>;

void Visit(const std::vector<UiFrame>& frames, const std::function<void(const UiFrame&)>& visitor) {
    for (const auto& frame : frames) { visitor(frame); Visit(frame.children, visitor); }
}

Result<UiDescription> Failure(std::string message) {
    Result<UiDescription> result;
    result.diagnostics.push_back(detail::MakeUiError("AUI_CLIPBOARD", std::move(message)));
    return result;
}

std::string Allocate(const std::string& id, Names& used) {
    if (used.insert(id).second) return id;
    for (std::size_t n = 1;; ++n) {
        const auto suffix = "_copy" + (n == 1 ? std::string{} : std::to_string(n));
        auto next = id.substr(0, 64 - suffix.size()) + suffix;
        if (used.insert(next).second) return next;
    }
}

void References(std::string_view text, Names& values) {
    if (const auto pieces = detail::ParseUiTextTemplate(text, nullptr)) {
        for (const auto& piece : *pieces) if (piece.reference) values.insert(piece.reference->id);
    }
}
} // namespace

Result<std::string> CopyUiFrame(const UiDescription& source, std::string_view frameId) {
    Result<std::string> result;
    result.diagnostics = ValidateUiDescription(source);
    if (!result.diagnostics.empty()) return result;
    const auto* frame = FindUiFrame(source, frameId);
    if (!frame) { result.diagnostics = Failure("要复制的框架不存在").diagnostics; return result; }
    UiDescription packet;
    packet.id = "clipboard";
    packet.frames.push_back(*frame);
    Names frames, values;
    Visit(packet.frames, [&](const UiFrame& item) { frames.insert(item.id); });
    bool external = false;
    Visit(packet.frames, [&](const UiFrame& item) {
        for (const auto& anchor : item.anchors) {
            if (anchor.target == UiAnchorTarget::Frame && !frames.contains(anchor.targetId)) external = true;
        }
        if (item.value.binding) values.insert(item.value.binding->value);
        if (item.visible.binding) values.insert(item.visible.binding->value);
        for (const auto& text : {item.text, item.texture, item.model}) References(text, values);
    });
    if (external) {
        result.diagnostics = Failure("框架锚点引用了复制范围外的框架，请改用父框架或屏幕锚点后再复制").diagnostics;
        return result;
    }
    for (const auto& state : source.states) {
        UiState copied{state.id, state.label, {}};
        for (const auto& change : state.overrides) if (frames.contains(change.frame)) {
            copied.overrides.push_back(change);
            for (const auto* text : {&change.text, &change.texture, &change.model}) if (*text) References(**text, values);
        }
        if (!copied.overrides.empty()) packet.states.push_back(std::move(copied));
    }
    for (const auto& value : source.values) if (values.contains(value.id)) packet.values.push_back(value);
    for (const auto& event : source.events) if (frames.contains(event.frame)) packet.events.push_back(event);
    for (const auto& [key, value] : source.simulation.values) {
        const auto reference = detail::ParseUiValueReference(key);
        if (reference && values.contains(reference->id)) packet.simulation.values.emplace(key, value);
    }
    const auto text = SerializeUiDescription(packet);
    auto checked = ParseUiDescription(text, "剪贴板");
    result.diagnostics = std::move(checked.diagnostics);
    if (checked.value) result.value = text;
    return result;
}

Result<UiDescription> PasteUiFrame(const UiDescription& target, std::string_view clipboard,
    std::string_view parent, std::optional<std::size_t> index) {
    auto parsed = ParseUiDescription(clipboard, "剪贴板");
    if (!parsed.value) return parsed;
    auto packet = std::move(*parsed.value);
    if (packet.frames.size() != 1 || !packet.simulation.timers.empty() || !packet.origins.empty()) return Failure("剪贴板应为一个框架子树，不含原版调整");
    auto valid = ValidateUiDescription(target);
    if (!valid.empty()) { Result<UiDescription> invalid; invalid.diagnostics = std::move(valid); return invalid; }
    Names frameNames, valueNames, stateNames, eventNames;
    Visit(target.frames, [&](const UiFrame& frame) { frameNames.insert(frame.id); });
    for (const auto& value : target.values) valueNames.insert(value.id);
    for (const auto& state : target.states) stateNames.insert(state.id);
    for (const auto& event : target.events) eventNames.insert(event.id);
    // 预留剪贴板里的全部编号，避免某次新编号占用尚未改名的源编号。
    Names reservedFrames = frameNames, reservedValues = valueNames;
    Visit(packet.frames, [&](const UiFrame& frame) { reservedFrames.insert(frame.id); });
    for (const auto& value : packet.values) reservedValues.insert(value.id);
    Json edits = Json::array();
    Visit(packet.frames, [&](const UiFrame& frame) {
        if (frameNames.contains(frame.id)) edits.push_back({{"op", "set_frame"}, {"frame", frame.id},
            {"fields", {{"id", Allocate(frame.id, reservedFrames)}}}});
    });
    for (const auto& value : packet.values) if (valueNames.contains(value.id)) {
        edits.push_back({{"op", "rename_value"}, {"from", value.id}, {"to", Allocate(value.id, reservedValues)}});
    }
    // 使用原编辑器的引用重写器；分批只为遵守同一256步预算，不发布中间状态。
    for (std::size_t begin = 0; begin < edits.size(); begin += 256) {
        Json batch = Json::array();
        for (std::size_t i = begin; i < std::min(begin + 256, edits.size()); ++i) batch.push_back(edits[i]);
        auto renamed = ApplyUiEdits(packet, batch.dump());
        if (!renamed.value) return renamed;
        packet = std::move(*renamed.value);
    }
    UiDescription result = target;
    auto* children = &result.frames;
    if (!parent.empty()) {
        auto* owner = FindUiFrame(result, parent);
        if (!owner) return Failure("粘贴的父框架不存在");
        children = &owner->children;
    }
    const auto at = index.value_or(children->size());
    if (at > children->size()) return Failure("粘贴位置超出子框架数量");
    children->insert(children->begin() + static_cast<std::ptrdiff_t>(at), std::move(packet.frames.front()));
    result.values.insert(result.values.end(), packet.values.begin(), packet.values.end());
    for (auto& state : packet.states) { state.id = Allocate(state.id, stateNames); result.states.push_back(std::move(state)); }
    for (auto& event : packet.events) { event.id = Allocate(event.id, eventNames); result.events.push_back(std::move(event)); }
    result.simulation.values.insert(packet.simulation.values.begin(), packet.simulation.values.end());
    return ParseUiDescription(SerializeUiDescription(result), UiDescriptionMemberPath(target.id));
}
} // namespace War3Map::GameUi
