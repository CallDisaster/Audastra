#include "War3MapCore/GameUi/UiDescription.h"

#include "War3MapCore/Text/TextUtil.h"
#include "UiDescriptionText.h"
#include "../Core/Serialization/StrictJson.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <span>

// audastra.ui 的读取与规范输出（检查在 UiDescriptionCheck.cpp）。读取严格：未知字段、类型不符与引用错误
// 都报到 JSON 路径，不返回部分结果。
namespace War3Map::GameUi {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

using namespace detail;  // 上限常量


constexpr std::array<std::string_view, 6> kFrameTypes{"panel", "backdrop", "text", "bar", "button", "template"};
constexpr std::array<std::string_view, 4> kValueTypes{"real", "integer", "boolean", "string"};
constexpr std::array<std::string_view, 3> kEventKinds{"click", "enter", "leave"};
constexpr std::array<std::string_view, 3> kJustifyH{"left", "center", "right"};
constexpr std::array<std::string_view, 3> kJustifyV{"top", "middle", "bottom"};

Diagnostic MakeError(const char* code, std::string message) {
    return MakeUiError(code, std::move(message));
}

template <std::size_t N>
std::optional<std::size_t> IndexOf(const std::array<std::string_view, N>& names, std::string_view text) {
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == text) return i;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- 读取

class Reader {
public:
    explicit Reader(std::string_view source) : source_(source.empty() ? std::string("audastra.ui") : std::string(source)) {}

    std::vector<Diagnostic> diagnostics;

    void Error(const std::string& path, const char* code, const std::string& message) {
        diagnostics.push_back(MakeError(code, source_ + " " + (path.empty() ? "/" : path) + "：" + message));
    }

    // 对象且只含给定字段。
    bool Object(const Json& value, const std::string& path, std::initializer_list<std::string_view> keys) {
        if (!value.is_object()) {
            Error(path, "AUI_TYPE", "应为对象");
            return false;
        }
        for (const auto& item : value.items()) {
            if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) {
                Error(path + "/" + item.key(), "AUI_FIELD_UNKNOWN", "不认识的字段");
            }
        }
        return true;
    }

    const Json* Field(const Json& object, std::string_view key, const std::string& path, bool required) {
        const auto it = object.find(std::string(key));
        if (it == object.end()) {
            if (required) Error(path + "/" + std::string(key), "AUI_FIELD_MISSING", "缺少必需字段");
            return nullptr;
        }
        return &*it;
    }

    std::optional<std::string> String(const Json& object, std::string_view key, const std::string& path, bool required,
        std::size_t maxBytes = kMaxTextBytes) {
        const Json* value = Field(object, key, path, required);
        if (!value) return std::nullopt;
        if (!value->is_string()) {
            Error(path + "/" + std::string(key), "AUI_TYPE", "应为文字");
            return std::nullopt;
        }
        const auto& text = value->get_ref<const std::string&>();
        if (text.size() > maxBytes || text.find('\0') != std::string::npos) {
            Error(path + "/" + std::string(key), "AUI_RANGE", "文字过长或含空字符");
            return std::nullopt;
        }
        return text;
    }

    std::optional<double> Number(const Json& object, std::string_view key, const std::string& path, bool required,
        double low, double high) {
        const Json* value = Field(object, key, path, required);
        if (!value) return std::nullopt;
        return NumberValue(*value, path + "/" + std::string(key), low, high);
    }

    std::optional<double> NumberValue(const Json& value, const std::string& path, double low, double high) {
        if (!value.is_number()) {
            Error(path, "AUI_TYPE", "应为数值");
            return std::nullopt;
        }
        const double number = value.get<double>();
        if (!std::isfinite(number) || number < low || number > high) {
            Error(path, "AUI_RANGE", "数值超出范围");
            return std::nullopt;
        }
        return number;
    }

    std::optional<std::uint32_t> Count(const Json& object, std::string_view key, const std::string& path, bool required,
        std::uint32_t low, std::uint32_t high) {
        const Json* value = Field(object, key, path, required);
        if (!value) return std::nullopt;
        if (!value->is_number_integer() || value->get<std::int64_t>() < low || value->get<std::int64_t>() > high) {
            Error(path + "/" + std::string(key), "AUI_RANGE",
                "应为 " + std::to_string(low) + " 到 " + std::to_string(high) + " 的整数");
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(value->get<std::int64_t>());
    }

    std::optional<bool> Bool(const Json& object, std::string_view key, const std::string& path, bool required) {
        const Json* value = Field(object, key, path, required);
        if (!value) return std::nullopt;
        if (!value->is_boolean()) {
            Error(path + "/" + std::string(key), "AUI_TYPE", "应为 true 或 false");
            return std::nullopt;
        }
        return value->get<bool>();
    }

    template <std::size_t N>
    std::optional<std::size_t> Choice(const Json& object, std::string_view key, const std::string& path, bool required,
        const std::array<std::string_view, N>& names) {
        const auto text = String(object, key, path, required);
        if (!text) return std::nullopt;
        const auto index = IndexOf(names, *text);
        if (!index) {
            std::string allowed;
            for (const auto name : names) allowed += (allowed.empty() ? "" : "、") + std::string(name);
            Error(path + "/" + std::string(key), "AUI_RANGE", "可选值为 " + allowed);
        }
        return index;
    }

    std::optional<FramePoint> Point(const Json& object, std::string_view key, const std::string& path, bool required) {
        const auto text = String(object, key, path, required);
        if (!text) return std::nullopt;
        const auto point = ParseFramePoint(*text);
        if (!point) Error(path + "/" + std::string(key), "AUI_RANGE", "不是锚点名（TOPLEFT … BOTTOMRIGHT）");
        return point;
    }

    const Json* Array(const Json& object, std::string_view key, const std::string& path, std::size_t limit) {
        const Json* value = Field(object, key, path, false);
        if (!value) return nullptr;
        if (!value->is_array()) {
            Error(path + "/" + std::string(key), "AUI_TYPE", "应为数组");
            return nullptr;
        }
        if (value->size() > limit) {
            Error(path + "/" + std::string(key), "AUI_RANGE", "最多 " + std::to_string(limit) + " 项");
            return nullptr;
        }
        return value;
    }

    // 绑定：数值或布尔字面量、"编号" 简写，或 {value, scale, offset, invert}。
    std::optional<UiBinding> Binding(const Json& value, const std::string& path) {
        if (value.is_string()) return BindingText(value.get_ref<const std::string&>(), path);
        if (!Object(value, path, {"value", "scale", "offset", "invert"})) return std::nullopt;
        const auto text = String(value, "value", path, true);
        if (!text) return std::nullopt;
        auto binding = BindingText(*text, path + "/value");
        if (!binding) return std::nullopt;
        binding->scale = Number(value, "scale", path, false, -1.0e6, 1.0e6).value_or(1.0);
        binding->offset = Number(value, "offset", path, false, -1.0e6, 1.0e6).value_or(0.0);
        binding->invert = Bool(value, "invert", path, false).value_or(false);
        return binding;
    }

    std::optional<UiBinding> BindingText(std::string_view text, const std::string& path) {
        auto reference = detail::ParseUiValueReference(text);
        if (!reference) {
            Error(path, "AUI_BINDING", "取值应写作 编号、编号[] 或 编号[下标]");
            return std::nullopt;
        }
        UiBinding binding;
        binding.value = std::move(reference->id);
        binding.perInstance = reference->perInstance;
        binding.index = reference->index;
        return binding;
    }

private:
    std::string source_;
};

void ReadValue(Reader& reader, const Json& json, const std::string& path, std::vector<UiValue>& values) {
    if (!reader.Object(json, path, {"id", "label", "type", "size", "default", "min", "max"})) return;
    UiValue value;
    value.id = reader.String(json, "id", path, true).value_or("");
    value.label = reader.String(json, "label", path, false).value_or("");
    const auto type = reader.Choice(json, "type", path, true, kValueTypes);
    if (!type) return;
    value.type = static_cast<UiValueType>(*type);
    value.size = reader.Count(json, "size", path, false, 0, kMaxArraySize).value_or(0);
    const bool numeric = value.type == UiValueType::Real || value.type == UiValueType::Integer;
    if (numeric) {
        value.min = reader.Number(json, "min", path, false, -1.0e9, 1.0e9);
        value.max = reader.Number(json, "max", path, false, -1.0e9, 1.0e9);
    } else if (json.contains("min") || json.contains("max")) {
        reader.Error(path, "AUI_FIELD_UNKNOWN", "只有数值类型可写 min / max");
    }
    value.defaultValue = *NormalizeUiValue(value.type, value.type == UiValueType::Boolean ? "false"
                                                     : value.type == UiValueType::String ? ""
                                                                                          : "0");
    if (const Json* given = reader.Field(json, "default", path, false)) {
        std::optional<std::string> text;
        if (numeric && given->is_number()) {
            text = NormalizeUiValue(value.type, detail::FormatUiNumber(given->get<double>()));
        } else if (value.type == UiValueType::Boolean && given->is_boolean()) {
            text = given->get<bool>() ? "true" : "false";
        } else if (value.type == UiValueType::String && given->is_string() &&
                   given->get_ref<const std::string&>().size() <= kMaxTextBytes) {
            text = given->get<std::string>();
        }
        if (text) value.defaultValue = *text;
        else reader.Error(path + "/default", "AUI_TYPE", "默认值与类型不符");
    }
    values.push_back(std::move(value));
}

void ReadFrame(Reader& reader, const Json& json, const std::string& path, std::size_t depth, std::size_t& count,
    std::vector<UiFrame>& frames) {
    if (depth > kMaxDepth) {
        reader.Error(path, "AUI_RANGE", "框架嵌套超过 " + std::to_string(kMaxDepth) + " 层");
        return;
    }
    if (++count > kMaxFrames) {
        if (count == kMaxFrames + 1) reader.Error(path, "AUI_RANGE", "框架超过 " + std::to_string(kMaxFrames) + " 个");
        return;
    }
    if (!reader.Object(json, path,
            {"id", "label", "type", "template", "anchors", "width", "height", "texture", "background", "pushed", "text",
                "font", "fontHeight", "color", "justifyH", "justifyV", "value", "visible", "repeat", "children"})) {
        return;
    }
    UiFrame frame;
    frame.id = reader.String(json, "id", path, true).value_or("");
    frame.label = reader.String(json, "label", path, false).value_or("");
    if (const auto type = reader.Choice(json, "type", path, true, kFrameTypes)) frame.type = static_cast<UiFrameType>(*type);
    frame.templateName = reader.String(json, "template", path, false, kMaxPathBytes).value_or("");
    if (const Json* anchors = reader.Array(json, "anchors", path, 9)) {
        for (std::size_t i = 0; i < anchors->size(); ++i) {
            const Json& item = (*anchors)[i];
            const std::string at = path + "/anchors/" + std::to_string(i);
            if (!reader.Object(item, at, {"point", "to", "toPoint", "x", "y"})) continue;
            UiAnchor anchor;
            anchor.point = reader.Point(item, "point", at, true).value_or(FramePoint::Center);
            anchor.targetPoint = reader.Point(item, "toPoint", at, false).value_or(anchor.point);
            const std::string to = reader.String(item, "to", at, false, kMaxPathBytes).value_or("parent");
            if (to == "parent") {
                anchor.target = UiAnchorTarget::Parent;
            } else if (to == "screen") {
                anchor.target = UiAnchorTarget::Screen;
            } else if (to.starts_with("game:")) {
                anchor.target = UiAnchorTarget::GameArea;
                anchor.targetId = to.substr(5);
            } else {
                anchor.target = UiAnchorTarget::Frame;
                anchor.targetId = to;
            }
            anchor.x = reader.Number(item, "x", at, false, -2.0, 2.0).value_or(0.0);
            anchor.y = reader.Number(item, "y", at, false, -2.0, 2.0).value_or(0.0);
            frame.anchors.push_back(std::move(anchor));
        }
    }
    frame.width = reader.Number(json, "width", path, false, 0.0, 2.0);
    frame.height = reader.Number(json, "height", path, false, 0.0, 2.0);
    frame.texture = reader.String(json, "texture", path, false, kMaxPathBytes).value_or("");
    frame.background = reader.String(json, "background", path, false, kMaxPathBytes).value_or("");
    frame.pushed = reader.String(json, "pushed", path, false, kMaxPathBytes).value_or("");
    frame.text = reader.String(json, "text", path, false).value_or("");
    frame.font = reader.String(json, "font", path, false, kMaxPathBytes).value_or("");
    frame.fontHeight = reader.Number(json, "fontHeight", path, false, 0.001, 0.1).value_or(frame.fontHeight);
    if (const Json* color = reader.Field(json, "color", path, false)) {
        if (!color->is_array() || color->size() != 4) {
            reader.Error(path + "/color", "AUI_TYPE", "应为 4 个 0 到 1 的数值（红、绿、蓝、不透明度）");
        } else {
            for (std::size_t i = 0; i < 4; ++i) {
                frame.color[i] = reader.NumberValue((*color)[i], path + "/color/" + std::to_string(i), 0.0, 1.0).value_or(1.0);
            }
        }
    }
    if (const auto h = reader.Choice(json, "justifyH", path, false, kJustifyH)) frame.justifyH = static_cast<JustifyH>(*h);
    if (const auto v = reader.Choice(json, "justifyV", path, false, kJustifyV)) frame.justifyV = static_cast<JustifyV>(*v);
    if (const Json* value = reader.Field(json, "value", path, false)) {
        if (value->is_number()) frame.value.literal = reader.NumberValue(*value, path + "/value", -1.0e6, 1.0e6).value_or(1.0);
        else frame.value.binding = reader.Binding(*value, path + "/value");
    }
    if (const Json* visible = reader.Field(json, "visible", path, false)) {
        if (visible->is_boolean()) frame.visible.literal = visible->get<bool>();
        else frame.visible.binding = reader.Binding(*visible, path + "/visible");
    }
    if (const Json* repeat = reader.Field(json, "repeat", path, false)) {
        const std::string at = path + "/repeat";
        if (reader.Object(*repeat, at, {"count", "dx", "dy", "columns", "rowDx", "rowDy"})) {
            UiRepeat item;
            item.count = reader.Count(*repeat, "count", at, true, 1, kMaxRepeat).value_or(1);
            item.dx = reader.Number(*repeat, "dx", at, false, -2.0, 2.0).value_or(0.0);
            item.dy = reader.Number(*repeat, "dy", at, false, -2.0, 2.0).value_or(0.0);
            item.columns = reader.Count(*repeat, "columns", at, false, 1, kMaxRepeat).value_or(0);
            item.rowDx = reader.Number(*repeat, "rowDx", at, false, -2.0, 2.0).value_or(0.0);
            item.rowDy = reader.Number(*repeat, "rowDy", at, false, -2.0, 2.0).value_or(0.0);
            frame.repeat = item;
        }
    }
    if (const Json* children = reader.Array(json, "children", path, kMaxFrames)) {
        for (std::size_t i = 0; i < children->size(); ++i) {
            ReadFrame(reader, (*children)[i], path + "/children/" + std::to_string(i), depth + 1, count, frame.children);
        }
    }
    frames.push_back(std::move(frame));
}

void ReadState(Reader& reader, const Json& json, const std::string& path, std::vector<UiState>& states) {
    if (!reader.Object(json, path, {"id", "label", "set"})) return;
    UiState state;
    state.id = reader.String(json, "id", path, true).value_or("");
    state.label = reader.String(json, "label", path, false).value_or("");
    if (const Json* set = reader.Array(json, "set", path, kMaxFrames)) {
        for (std::size_t i = 0; i < set->size(); ++i) {
            const Json& item = (*set)[i];
            const std::string at = path + "/set/" + std::to_string(i);
            if (!reader.Object(item, at, {"frame", "texture", "text", "visible", "value"})) continue;
            UiStateOverride entry;
            entry.frame = reader.String(item, "frame", at, true).value_or("");
            entry.texture = reader.String(item, "texture", at, false, kMaxPathBytes);
            entry.text = reader.String(item, "text", at, false);
            entry.visible = reader.Bool(item, "visible", at, false);
            entry.value = reader.Number(item, "value", at, false, -1.0e6, 1.0e6);
            state.overrides.push_back(std::move(entry));
        }
    }
    states.push_back(std::move(state));
}

// 起始值按数值类型写：实数、整数写数值，布尔写 true/false，文字写字符串；存成规范文本。
// 引用的数值不存在、下标不对等留给检查报告。
void ReadSimulation(Reader& reader, const Json& json, UiDescription& description) {
    const std::string path = "/simulation";
    if (!reader.Object(json, path, {"timers", "values"})) return;
    if (const Json* timers = reader.Array(json, "timers", path, kMaxSimulationTimers)) {
        for (std::size_t i = 0; i < timers->size(); ++i) {
            const Json& item = (*timers)[i];
            const std::string at = path + "/timers/" + std::to_string(i);
            if (!reader.Object(item, at, {"id", "label", "period", "repeat"})) continue;
            UiSimulationTimer timer;
            timer.id = reader.String(item, "id", at, true).value_or("");
            timer.label = reader.String(item, "label", at, false).value_or("");
            timer.period = reader.Number(item, "period", at, true, kMinSimulationPeriod, kMaxSimulationPeriod).value_or(1.0);
            timer.repeat = reader.Bool(item, "repeat", at, false).value_or(true);
            description.simulation.timers.push_back(std::move(timer));
        }
    }
    const Json* values = reader.Field(json, "values", path, false);
    if (!values) return;
    if (!values->is_object()) {
        reader.Error(path + "/values", "AUI_TYPE", "应为对象：键为 编号 或 编号[下标]");
        return;
    }
    for (const auto& item : values->items()) {
        const std::string at = path + "/values/" + item.key();
        const Json& value = item.value();
        const auto reference = detail::ParseUiValueReference(item.key());
        const UiValue* target = reference ? FindUiValue(description, reference->id) : nullptr;
        std::string text;
        if (value.is_string()) {
            text = value.get<std::string>();
        } else if (value.is_boolean()) {
            text = value.get<bool>() ? "true" : "false";
        } else if (value.is_number()) {
            const auto number = reader.NumberValue(value, at, -1.0e9, 1.0e9);
            if (!number) continue;
            text = detail::FormatUiNumber(*number);
        } else {
            reader.Error(at, "AUI_TYPE", "应为数值、布尔或文字");
            continue;
        }
        if (target) {
            const bool number = target->type == UiValueType::Real || target->type == UiValueType::Integer;
            const bool matches = number ? value.is_number() : target->type == UiValueType::Boolean ? value.is_boolean() : value.is_string();
            if (!matches) {
                reader.Error(at, "AUI_TYPE", "起始值与数值类型不符");
                continue;
            }
            if (auto normalized = NormalizeUiValue(target->type, text)) text = std::move(*normalized);
        }
        description.simulation.values.emplace(item.key(), std::move(text));
    }
}

// ---------------------------------------------------------------- 输出

OrderedJson Num(double value) {
    if (value == 0.0) return 0;  // 含 -0
    if (std::floor(value) == value && std::fabs(value) < 1.0e15) return static_cast<std::int64_t>(value);
    return value;
}

OrderedJson BindingJson(const UiBinding& binding) {
    std::string reference = binding.value;
    if (binding.perInstance) reference += "[]";
    else if (binding.index) reference += "[" + std::to_string(*binding.index) + "]";
    if (binding.scale == 1.0 && binding.offset == 0.0 && !binding.invert) return reference;
    OrderedJson json;
    json["value"] = reference;
    if (binding.scale != 1.0) json["scale"] = Num(binding.scale);
    if (binding.offset != 0.0) json["offset"] = Num(binding.offset);
    if (binding.invert) json["invert"] = true;
    return json;
}

OrderedJson FrameJson(const UiFrame& frame) {
    OrderedJson json;
    json["id"] = frame.id;
    if (!frame.label.empty()) json["label"] = frame.label;
    json["type"] = UiFrameTypeName(frame.type);
    if (!frame.templateName.empty()) json["template"] = frame.templateName;
    if (!frame.anchors.empty()) {
        OrderedJson anchors = OrderedJson::array();
        for (const auto& anchor : frame.anchors) {
            OrderedJson item;
            item["point"] = FramePointName(anchor.point);
            switch (anchor.target) {
            case UiAnchorTarget::Parent: break;
            case UiAnchorTarget::Screen: item["to"] = "screen"; break;
            case UiAnchorTarget::Frame: item["to"] = anchor.targetId; break;
            case UiAnchorTarget::GameArea: item["to"] = "game:" + anchor.targetId; break;
            }
            if (anchor.targetPoint != anchor.point) item["toPoint"] = FramePointName(anchor.targetPoint);
            if (anchor.x != 0.0) item["x"] = Num(anchor.x);
            if (anchor.y != 0.0) item["y"] = Num(anchor.y);
            anchors.push_back(std::move(item));
        }
        json["anchors"] = std::move(anchors);
    }
    if (frame.width) json["width"] = Num(*frame.width);
    if (frame.height) json["height"] = Num(*frame.height);
    if (!frame.texture.empty()) json["texture"] = frame.texture;
    if (!frame.background.empty()) json["background"] = frame.background;
    if (!frame.pushed.empty()) json["pushed"] = frame.pushed;
    if (!frame.text.empty()) json["text"] = frame.text;
    if (!frame.font.empty()) json["font"] = frame.font;
    const bool textType = frame.type == UiFrameType::Text || frame.type == UiFrameType::Button;
    if (textType && frame.fontHeight != UiFrame{}.fontHeight) json["fontHeight"] = Num(frame.fontHeight);
    if (frame.color != UiFrame{}.color) {
        OrderedJson color = OrderedJson::array();
        for (const double component : frame.color) color.push_back(Num(component));
        json["color"] = std::move(color);
    }
    if (frame.justifyH != JustifyH::Center) json["justifyH"] = kJustifyH[static_cast<std::size_t>(frame.justifyH)];
    if (frame.justifyV != JustifyV::Middle) json["justifyV"] = kJustifyV[static_cast<std::size_t>(frame.justifyV)];
    if (frame.value.binding) json["value"] = BindingJson(*frame.value.binding);
    else if (frame.value.literal != 1.0) json["value"] = Num(frame.value.literal);
    if (frame.visible.binding) json["visible"] = BindingJson(*frame.visible.binding);
    else if (!frame.visible.literal) json["visible"] = false;
    if (frame.repeat) {
        OrderedJson repeat;
        repeat["count"] = frame.repeat->count;
        if (frame.repeat->dx != 0.0) repeat["dx"] = Num(frame.repeat->dx);
        if (frame.repeat->dy != 0.0) repeat["dy"] = Num(frame.repeat->dy);
        if (frame.repeat->columns != 0) repeat["columns"] = frame.repeat->columns;
        if (frame.repeat->rowDx != 0.0) repeat["rowDx"] = Num(frame.repeat->rowDx);
        if (frame.repeat->rowDy != 0.0) repeat["rowDy"] = Num(frame.repeat->rowDy);
        json["repeat"] = std::move(repeat);
    }
    if (!frame.children.empty()) {
        OrderedJson children = OrderedJson::array();
        for (const auto& child : frame.children) children.push_back(FrameJson(child));
        json["children"] = std::move(children);
    }
    return json;
}

} // namespace

bool IsValidUiId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!letter(id.front())) return false;
    return std::all_of(id.begin(), id.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

std::string_view UiFrameTypeName(UiFrameType type) {
    return kFrameTypes[static_cast<std::size_t>(type)];
}

std::string_view UiValueTypeName(UiValueType type) {
    return kValueTypes[static_cast<std::size_t>(type)];
}

Result<UiDescription> ParseUiDescription(std::string_view text, std::string_view source) {
    using ParseResult = Result<UiDescription>;
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    Serialization::StrictJsonLimits limits;
    limits.maxBytes = 2 * 1024 * 1024;
    limits.maxDepth = 2 * kMaxDepth + 8;
    auto parsed = Serialization::ParseStrictJson(bytes, limits);
    if (!parsed.value) {
        ParseResult result;
        result.diagnostics.push_back(MakeError("AUI_JSON",
            (source.empty() ? std::string("audastra.ui") : std::string(source)) + "：不是有效的 JSON（语法、重复字段或过大）"));
        return result;
    }
    const Json& json = *parsed.value;
    Reader reader(source);
    UiDescription description;
    if (reader.Object(json, "", {"format", "version", "id", "label", "values", "frames", "states", "events", "simulation"})) {
        const auto format = reader.String(json, "format", "", true);
        if (format && *format != kUiDescriptionFormat) reader.Error("/format", "AUI_FORMAT", "不是 audastra.ui 界面描述");
        const auto version = reader.Count(json, "version", "", true, 1, 1000);
        if (version && *version != kUiDescriptionVersion) {
            reader.Error("/version", "AUI_FORMAT", "版本 " + std::to_string(*version) + " 不受支持（支持 1）");
        }
        description.id = reader.String(json, "id", "", true).value_or("");
        description.label = reader.String(json, "label", "", false).value_or("");
        if (const Json* values = reader.Array(json, "values", "", kMaxValues)) {
            for (std::size_t i = 0; i < values->size(); ++i) ReadValue(reader, (*values)[i], "/values/" + std::to_string(i), description.values);
        }
        if (const Json* frames = reader.Array(json, "frames", "", kMaxFrames)) {
            std::size_t count = 0;
            for (std::size_t i = 0; i < frames->size(); ++i) {
                ReadFrame(reader, (*frames)[i], "/frames/" + std::to_string(i), 0, count, description.frames);
            }
        }
        if (const Json* states = reader.Array(json, "states", "", kMaxStates)) {
            for (std::size_t i = 0; i < states->size(); ++i) ReadState(reader, (*states)[i], "/states/" + std::to_string(i), description.states);
        }
        if (const Json* events = reader.Array(json, "events", "", kMaxEvents)) {
            for (std::size_t i = 0; i < events->size(); ++i) {
                const Json& item = (*events)[i];
                const std::string path = "/events/" + std::to_string(i);
                if (!reader.Object(item, path, {"id", "frame", "on"})) continue;
                UiEvent event;
                event.id = reader.String(item, "id", path, true).value_or("");
                event.frame = reader.String(item, "frame", path, true).value_or("");
                if (const auto kind = reader.Choice(item, "on", path, true, kEventKinds)) event.kind = static_cast<UiEventKind>(*kind);
                description.events.push_back(std::move(event));
            }
        }
        if (const Json* simulation = reader.Field(json, "simulation", "", false)) ReadSimulation(reader, *simulation, description);
    }
    ParseResult result;
    result.diagnostics = std::move(reader.diagnostics);
    if (!result.diagnostics.empty()) return result;
    result.diagnostics = ValidateUiDescription(description);
    if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const Diagnostic& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; })) {
        result.value = std::move(description);
    }
    return result;
}

std::string SerializeUiDescription(const UiDescription& description) {
    OrderedJson json;
    json["format"] = kUiDescriptionFormat;
    json["version"] = kUiDescriptionVersion;
    json["id"] = description.id;
    if (!description.label.empty()) json["label"] = description.label;
    if (!description.values.empty()) {
        OrderedJson values = OrderedJson::array();
        for (const auto& value : description.values) {
            OrderedJson item;
            item["id"] = value.id;
            if (!value.label.empty()) item["label"] = value.label;
            item["type"] = UiValueTypeName(value.type);
            if (value.size > 0) item["size"] = value.size;
            switch (value.type) {
            case UiValueType::Real:
            case UiValueType::Integer: {
                double number = 0.0;
                const auto& text = value.defaultValue;
                std::from_chars(text.data(), text.data() + text.size(), number);
                if (number != 0.0) item["default"] = Num(number);
                break;
            }
            case UiValueType::Boolean:
                if (value.defaultValue == "true") item["default"] = true;
                break;
            case UiValueType::String:
                if (!value.defaultValue.empty()) item["default"] = value.defaultValue;
                break;
            }
            if (value.min) item["min"] = Num(*value.min);
            if (value.max) item["max"] = Num(*value.max);
            values.push_back(std::move(item));
        }
        json["values"] = std::move(values);
    }
    if (!description.frames.empty()) {
        OrderedJson frames = OrderedJson::array();
        for (const auto& frame : description.frames) frames.push_back(FrameJson(frame));
        json["frames"] = std::move(frames);
    }
    if (!description.states.empty()) {
        OrderedJson states = OrderedJson::array();
        for (const auto& state : description.states) {
            OrderedJson item;
            item["id"] = state.id;
            if (!state.label.empty()) item["label"] = state.label;
            OrderedJson set = OrderedJson::array();
            for (const auto& entry : state.overrides) {
                OrderedJson change;
                change["frame"] = entry.frame;
                if (entry.texture) change["texture"] = *entry.texture;
                if (entry.text) change["text"] = *entry.text;
                if (entry.visible) change["visible"] = *entry.visible;
                if (entry.value) change["value"] = Num(*entry.value);
                set.push_back(std::move(change));
            }
            if (!set.empty()) item["set"] = std::move(set);
            states.push_back(std::move(item));
        }
        json["states"] = std::move(states);
    }
    if (!description.events.empty()) {
        OrderedJson events = OrderedJson::array();
        for (const auto& event : description.events) {
            OrderedJson item;
            item["id"] = event.id;
            item["frame"] = event.frame;
            item["on"] = kEventKinds[static_cast<std::size_t>(event.kind)];
            events.push_back(std::move(item));
        }
        json["events"] = std::move(events);
    }
    const UiSimulation& simulation = description.simulation;
    if (!simulation.timers.empty() || !simulation.values.empty()) {
        OrderedJson section;
        if (!simulation.timers.empty()) {
            OrderedJson timers = OrderedJson::array();
            for (const auto& timer : simulation.timers) {
                OrderedJson item;
                item["id"] = timer.id;
                if (!timer.label.empty()) item["label"] = timer.label;
                item["period"] = Num(timer.period);
                if (!timer.repeat) item["repeat"] = false;
                timers.push_back(std::move(item));
            }
            section["timers"] = std::move(timers);
        }
        if (!simulation.values.empty()) {
            OrderedJson values = OrderedJson::object();
            for (const auto& [key, text] : simulation.values) {
                const auto reference = detail::ParseUiValueReference(key);
                const UiValue* target = reference ? FindUiValue(description, reference->id) : nullptr;
                double number = 0.0;
                if (target && (target->type == UiValueType::Real || target->type == UiValueType::Integer) &&
                    std::from_chars(text.data(), text.data() + text.size(), number).ec == std::errc{}) {
                    values[key] = Num(number);
                } else if (target && target->type == UiValueType::Boolean && (text == "true" || text == "false")) {
                    values[key] = text == "true";
                } else {
                    values[key] = text;
                }
            }
            section["values"] = std::move(values);
        }
        json["simulation"] = std::move(section);
    }
    return json.dump(2, ' ', false, OrderedJson::error_handler_t::replace) + "\n";
}

namespace {

template <typename Frames>
auto FindIn(Frames& frames, std::string_view id) -> decltype(&frames.front()) {
    for (auto& frame : frames) {
        if (frame.id == id) return &frame;
        if (auto* found = FindIn(frame.children, id)) return found;
    }
    return nullptr;
}

} // namespace

const UiFrame* FindUiFrame(const UiDescription& description, std::string_view id) {
    return FindIn(description.frames, id);
}

UiFrame* FindUiFrame(UiDescription& description, std::string_view id) {
    return FindIn(description.frames, id);
}

const UiValue* FindUiValue(const UiDescription& description, std::string_view id) {
    const auto found = std::find_if(description.values.begin(), description.values.end(), [&](const UiValue& value) { return value.id == id; });
    return found == description.values.end() ? nullptr : &*found;
}

std::string SerializeUiFrame(const UiFrame& frame) {
    OrderedJson json = FrameJson(frame);
    json.erase("children");
    return json.dump(-1, ' ', false, OrderedJson::error_handler_t::replace);
}

std::string UiDescriptionMemberPath(std::string_view id) {
    return std::string(kUiDescriptionFolder) + std::string(id) + ".json";
}

bool IsUiDescriptionMember(std::string_view path) {
    std::string lower = Text::LowerAscii(path);
    std::replace(lower.begin(), lower.end(), '/', '\\');
    const std::string folder = Text::LowerAscii(kUiDescriptionFolder);
    if (!lower.starts_with(folder) || !lower.ends_with(".json")) return false;
    const std::string_view id = std::string_view(path).substr(folder.size(), path.size() - folder.size() - 5);
    return IsValidUiId(id);
}

std::optional<std::string> NormalizeUiValue(UiValueType type, std::string_view text) {
    switch (type) {
    case UiValueType::Real: {
        const auto trimmed = Text::TrimCWhitespaceCopy(text);
        double number = 0.0;
        const auto [end, error] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
        if (error != std::errc() || end != trimmed.data() + trimmed.size() || !std::isfinite(number)) return std::nullopt;
        return detail::FormatUiNumber(number);
    }
    case UiValueType::Integer: {
        const auto trimmed = Text::TrimCWhitespaceCopy(text);
        std::int32_t number = 0;
        const auto [end, error] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
        if (error == std::errc() && end == trimmed.data() + trimmed.size()) return std::to_string(number);
        // 整数输入也接受整值实数写法（如滑块给出的 3.0）。
        double real = 0.0;
        const auto [realEnd, realError] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), real);
        if (realError != std::errc() || realEnd != trimmed.data() + trimmed.size() || std::floor(real) != real ||
            std::fabs(real) > 2147483647.0) {
            return std::nullopt;
        }
        return std::to_string(static_cast<std::int32_t>(real));
    }
    case UiValueType::Boolean:
        if (text == "true" || text == "false") return std::string(text);
        return std::nullopt;
    case UiValueType::String:
        if (text.size() > kMaxTextBytes) return std::nullopt;
        return std::string(text);
    }
    return std::nullopt;
}

} // namespace War3Map::GameUi
