#include "War3MapCore/GameUi/UiDescription.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "UiDescriptionText.h"

#include <algorithm>
#include <set>
#include <unordered_map>

// audastra.ui 的检查：编号、引用、取值与各类框架适用的字段。读取时调用；编辑后保存前再调用。
namespace War3Map::GameUi {
namespace {

using namespace detail;

Diagnostic MakeError(const char* code, std::string message) {
    return MakeUiError(code, std::move(message));
}

struct FrameInfo {
    const UiFrame* frame = nullptr;
    std::string path;
    std::optional<std::uint32_t> repeatCount;  // 所在重复范围的实例数
};

class Validator {
public:
    explicit Validator(const UiDescription& description) : description_(description) {}

    std::vector<Diagnostic> Run() {
        if (!IsValidUiId(description_.id)) Error("/id", "AUI_ID_INVALID", "界面编号无效");
        if (description_.values.size() > kMaxValues) Error("/values", "AUI_RANGE", "数值超过 " + std::to_string(kMaxValues) + " 个");
        if (description_.states.size() > kMaxStates) Error("/states", "AUI_RANGE", "状态超过 " + std::to_string(kMaxStates) + " 个");
        if (description_.events.size() > kMaxEvents) Error("/events", "AUI_RANGE", "事件超过 " + std::to_string(kMaxEvents) + " 个");
        CheckValues();
        for (std::size_t i = 0; i < description_.frames.size(); ++i) {
            Collect(description_.frames[i], "/frames/" + std::to_string(i), std::nullopt, 0);
        }
        for (const auto& info : order_) CheckFrame(info);
        CheckStates();
        CheckEvents();
        CheckSimulation();
        return std::move(diagnostics_);
    }

private:
    void Error(const std::string& path, const char* code, const std::string& message) {
        diagnostics_.push_back(MakeError(code, UiDescriptionMemberPath(description_.id) + " " + path + "：" + message));
    }

    void CheckSimulation() {
        const UiSimulation& simulation = description_.simulation;
        if (simulation.timers.size() > kMaxSimulationTimers) {
            Error("/simulation/timers", "AUI_RANGE", "模拟计时器超过 " + std::to_string(kMaxSimulationTimers) + " 个");
        }
        std::set<std::string> seen;
        for (std::size_t i = 0; i < simulation.timers.size(); ++i) {
            const UiSimulationTimer& timer = simulation.timers[i];
            const std::string path = "/simulation/timers/" + std::to_string(i);
            if (!IsValidUiId(timer.id)) Error(path + "/id", "AUI_ID_INVALID", "模拟计时器编号无效");
            else if (!seen.insert(timer.id).second) Error(path + "/id", "AUI_ID_DUPLICATE", "模拟计时器编号 " + timer.id + " 重复");
            if (!(timer.period >= kMinSimulationPeriod && timer.period <= kMaxSimulationPeriod)) {
                Error(path + "/period", "AUI_RANGE", "周期应在 0.01 到 3600 秒之间");
            }
        }
        for (const auto& [key, text] : simulation.values) {
            const std::string path = "/simulation/values/" + key;
            const auto reference = detail::ParseUiValueReference(key);
            const UiValue* value = reference && !reference->perInstance ? FindUiValue(description_, reference->id) : nullptr;
            if (!value) {
                Error(path, "AUI_VALUE_UNKNOWN", "起始值对应的数值不存在");
                continue;
            }
            if (value->size > 0 && (!reference->index || *reference->index >= value->size)) {
                Error(path, "AUI_BINDING", "数组数值的起始值要写下标 0 到 " + std::to_string(value->size - 1));
            } else if (value->size == 0 && reference->index) {
                Error(path, "AUI_BINDING", "单值不能写下标");
            } else if (NormalizeUiValue(value->type, text) != std::optional<std::string>(text)) {
                Error(path, "AUI_TYPE", "起始值与数值类型不符");
            }
        }
    }

    void CheckValues() {
        std::set<std::string> seen;
        for (std::size_t i = 0; i < description_.values.size(); ++i) {
            const UiValue& value = description_.values[i];
            const std::string path = "/values/" + std::to_string(i);
            if (!IsValidUiId(value.id)) Error(path + "/id", "AUI_ID_INVALID", "数值编号无效");
            else if (!seen.insert(value.id).second) Error(path + "/id", "AUI_ID_DUPLICATE", "数值编号 " + value.id + " 重复");
            if (value.size > kMaxArraySize) Error(path + "/size", "AUI_RANGE", "数组最多 " + std::to_string(kMaxArraySize) + " 格");
            if (!NormalizeUiValue(value.type, value.defaultValue)) Error(path + "/default", "AUI_TYPE", "默认值与类型不符");
            if (value.min && value.max && *value.min > *value.max) Error(path, "AUI_RANGE", "min 大于 max");
        }
    }

    void Collect(const UiFrame& frame, const std::string& path, std::optional<std::uint32_t> repeatCount, std::size_t depth) {
        if (!IsValidUiId(frame.id) || frame.id == "parent" || frame.id == "screen") {
            Error(path + "/id", "AUI_ID_INVALID", "框架编号无效（parent、screen 为保留字）");
        } else if (frames_.contains(frame.id)) {
            Error(path + "/id", "AUI_ID_DUPLICATE", "框架编号 " + frame.id + " 重复");
        }
        if (frame.repeat) {
            if (repeatCount) Error(path + "/repeat", "AUI_REPEAT_NESTED", "重复范围内不能再嵌套重复");
            else repeatCount = frame.repeat->count;
            if (frame.repeat->count < 1 || frame.repeat->count > kMaxRepeat) {
                Error(path + "/repeat/count", "AUI_RANGE", "重复次数应为 1 到 " + std::to_string(kMaxRepeat));
            }
        }
        if (++frameCount_ == kMaxFrames + 1) Error(path, "AUI_RANGE", "框架超过 " + std::to_string(kMaxFrames) + " 个");
        if (depth > kMaxDepth) Error(path, "AUI_RANGE", "框架嵌套超过 " + std::to_string(kMaxDepth) + " 层");
        order_.push_back(FrameInfo{&frame, path, repeatCount});
        frames_.try_emplace(frame.id, order_.back());
        for (std::size_t i = 0; i < frame.children.size(); ++i) {
            Collect(frame.children[i], path + "/children/" + std::to_string(i), repeatCount, depth + 1);
        }
    }

    const UiValue* FindValue(std::string_view id) const {
        for (const auto& value : description_.values) {
            if (value.id == id) return &value;
        }
        return nullptr;
    }

    // 取值引用：数值存在；单值不带下标；数组带下标或 []，[] 只能在重复范围内且格数够用。
    const UiValue* CheckReference(std::string_view id, bool perInstance, std::optional<std::uint32_t> index,
        const FrameInfo& info, const std::string& path) {
        const UiValue* value = FindValue(id);
        if (!value) {
            Error(path, "AUI_VALUE_UNKNOWN", "数值 " + std::string(id) + " 未定义");
            return nullptr;
        }
        if (value->size == 0) {
            if (perInstance || index) Error(path, "AUI_BINDING", "数值 " + value->id + " 不是数组，不能带下标");
        } else if (perInstance) {
            if (!info.repeatCount) Error(path, "AUI_BINDING", "[] 只能用在重复范围内");
            else if (*info.repeatCount > value->size) {
                Error(path, "AUI_BINDING", "数组 " + value->id + " 只有 " + std::to_string(value->size) + " 格，少于重复次数");
            }
        } else if (!index) {
            Error(path, "AUI_BINDING", "数组 " + value->id + " 需要写下标或 []");
        } else if (*index >= value->size) {
            Error(path, "AUI_BINDING", "下标超出数组 " + value->id + " 的范围");
        }
        return value;
    }

    void CheckBinding(const UiBinding& binding, const FrameInfo& info, const std::string& path, bool flag) {
        const UiValue* value = CheckReference(binding.value, binding.perInstance, binding.index, info, path);
        if (!value) return;
        if (!flag && (value->type == UiValueType::String || value->type == UiValueType::Boolean)) {
            Error(path, "AUI_VALUE_TYPE", "进度只能取实数或整数数值");
        }
    }

    void CheckText(std::string_view text, const FrameInfo& info, const std::string& path) {
        std::string error;
        const auto pieces = detail::ParseUiTextTemplate(text, &error);
        if (!pieces) {
            Error(path, "AUI_TEXT_TEMPLATE", error);
            return;
        }
        for (const auto& piece : *pieces) {
            if (piece.reference) {
                CheckReference(piece.reference->id, piece.reference->perInstance, piece.reference->index, info, path);
            }
        }
    }

    void CheckFrame(const FrameInfo& info) {
        const UiFrame& frame = *info.frame;
        const std::string& path = info.path;
        const bool hasTemplate = frame.type == UiFrameType::Template;
        if (hasTemplate && frame.templateName.empty()) Error(path + "/template", "AUI_FIELD_MISSING", "模板框架需要模板名");
        if (!hasTemplate && !frame.templateName.empty()) Error(path + "/template", "AUI_FIELD_UNKNOWN", "只有模板框架可写模板名");
        const auto only = [&](bool allowed, bool present, const char* field) {
            if (present && !allowed) {
                Error(path + "/" + field, "AUI_FIELD_UNKNOWN", std::string(UiFrameTypeName(frame.type)) + " 框架不使用 " + field);
            }
        };
        const bool imageType = frame.type == UiFrameType::Backdrop || frame.type == UiFrameType::Bar ||
                               frame.type == UiFrameType::Button;
        const bool textType = frame.type == UiFrameType::Text || frame.type == UiFrameType::Button;
        only(imageType, !frame.texture.empty(), "texture");
        only(frame.type == UiFrameType::Bar, !frame.background.empty(), "background");
        only(frame.type == UiFrameType::Button, !frame.pushed.empty(), "pushed");
        only(textType, !frame.text.empty(), "text");
        only(textType, !frame.font.empty(), "font");
        only(frame.type == UiFrameType::Bar, frame.value.binding || frame.value.literal != 1.0, "value");
        std::set<FramePoint> points;
        for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
            const UiAnchor& anchor = frame.anchors[i];
            const std::string at = path + "/anchors/" + std::to_string(i);
            if (!points.insert(anchor.point).second) Error(at + "/point", "AUI_ANCHOR_DUPLICATE", "同一锚点写了两次");
            if (anchor.target == UiAnchorTarget::Frame) {
                if (anchor.targetId == frame.id) Error(at + "/to", "AUI_ANCHOR_TARGET", "不能锚在自己身上");
                else if (!frames_.contains(anchor.targetId)) {
                    Error(at + "/to", "AUI_ANCHOR_TARGET", "框架 " + anchor.targetId + " 不存在");
                }
            } else if (anchor.target == UiAnchorTarget::GameArea) {
                const auto& areas = GameCodeAreas();
                const bool known = anchor.targetId == kGameCodeRoot ||
                                   std::any_of(areas.begin(), areas.end(), [&](const GameCodeArea& area) { return area.id == anchor.targetId; });
                if (!known) Error(at + "/to", "AUI_ANCHOR_TARGET", "游戏界面区域 " + anchor.targetId + " 不存在");
            }
        }
        if (frame.value.binding) CheckBinding(*frame.value.binding, info, path + "/value", false);
        if (frame.visible.binding) CheckBinding(*frame.visible.binding, info, path + "/visible", true);
        if (!frame.text.empty()) CheckText(frame.text, info, path + "/text");
        if (!frame.texture.empty()) CheckText(frame.texture, info, path + "/texture");
    }

    void CheckStates() {
        std::set<std::string> seen;
        for (std::size_t i = 0; i < description_.states.size(); ++i) {
            const UiState& state = description_.states[i];
            const std::string path = "/states/" + std::to_string(i);
            if (!IsValidUiId(state.id)) Error(path + "/id", "AUI_ID_INVALID", "状态编号无效");
            else if (!seen.insert(state.id).second) Error(path + "/id", "AUI_ID_DUPLICATE", "状态编号 " + state.id + " 重复");
            std::set<std::string> touched;
            for (std::size_t j = 0; j < state.overrides.size(); ++j) {
                const UiStateOverride& entry = state.overrides[j];
                const std::string at = path + "/set/" + std::to_string(j);
                const auto found = frames_.find(entry.frame);
                if (found == frames_.end()) {
                    Error(at + "/frame", "AUI_STATE_FRAME", "框架 " + entry.frame + " 不存在");
                    continue;
                }
                if (!touched.insert(entry.frame).second) Error(at + "/frame", "AUI_ID_DUPLICATE", "同一框架在状态里写了两次");
                const UiFrameType type = found->second.frame->type;
                if (entry.texture && type != UiFrameType::Backdrop && type != UiFrameType::Bar && type != UiFrameType::Button) {
                    Error(at + "/texture", "AUI_FIELD_UNKNOWN", "该框架没有贴图");
                }
                if (entry.text) {
                    if (type != UiFrameType::Text && type != UiFrameType::Button) Error(at + "/text", "AUI_FIELD_UNKNOWN", "该框架没有文字");
                    else CheckText(*entry.text, found->second, at + "/text");
                }
                if (entry.value && type != UiFrameType::Bar) Error(at + "/value", "AUI_FIELD_UNKNOWN", "只有进度条有进度");
            }
        }
    }

    void CheckEvents() {
        std::set<std::string> seen;
        for (std::size_t i = 0; i < description_.events.size(); ++i) {
            const UiEvent& event = description_.events[i];
            const std::string path = "/events/" + std::to_string(i);
            if (!IsValidUiId(event.id)) Error(path + "/id", "AUI_ID_INVALID", "事件编号无效");
            else if (!seen.insert(event.id).second) Error(path + "/id", "AUI_ID_DUPLICATE", "事件编号 " + event.id + " 重复");
            const auto found = frames_.find(event.frame);
            if (found == frames_.end()) Error(path + "/frame", "AUI_EVENT_FRAME", "框架 " + event.frame + " 不存在");
            else if (event.kind == UiEventKind::Click && found->second.frame->type != UiFrameType::Button) {
                Error(path + "/on", "AUI_EVENT_FRAME", "只有按钮有点击事件");
            }
        }
    }

    const UiDescription& description_;
    std::vector<FrameInfo> order_;                       // 文档顺序，含重复编号的框架
    std::unordered_map<std::string, FrameInfo> frames_;  // 按编号查（重复时取第一个）
    std::size_t frameCount_ = 0;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

std::vector<Diagnostic> ValidateUiDescription(const UiDescription& description) {
    return Validator(description).Run();
}

} // namespace War3Map::GameUi
