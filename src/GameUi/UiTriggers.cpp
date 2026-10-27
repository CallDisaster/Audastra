#include "War3MapCore/GameUi/UiTriggers.h"

#include "War3MapCore/GameUi/FrameTree.h"
#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "FeatureTextWriter.h"
#include "UiDescriptionText.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace War3Map::GameUi {
namespace {

constexpr double kTemplateFontHeight = 0.011;
constexpr std::string_view kLoopIndex = "GetForLoopIndexA()";
// 一个线程里最多放的界面库调用数。实测一次执行发到一百五十个左右就被指令上限掐断，约留一半余量。
constexpr std::size_t kThreadRequests = 80;

constexpr std::array<std::string_view, 9> kAnchors{"AudastraAnchorTopLeft", "AudastraAnchorTop",
    "AudastraAnchorTopRight", "AudastraAnchorLeft", "AudastraAnchorCenter", "AudastraAnchorRight",
    "AudastraAnchorBottomLeft", "AudastraAnchorBottom", "AudastraAnchorBottomRight"};
// 屏幕 0.8×0.6 上九个锚点的位置，顺序同 FramePoint。
constexpr std::array<std::array<double, 2>, 9> kScreenPoints{{{0.0, 0.6}, {0.4, 0.6}, {0.8, 0.6}, {0.0, 0.3},
    {0.4, 0.3}, {0.8, 0.3}, {0.0, 0.0}, {0.4, 0.0}, {0.8, 0.0}}};
constexpr std::array<std::string_view, 3> kJustifyH{"AudastraJustifyLeft", "AudastraJustifyCenter", "AudastraJustifyRight"};
constexpr std::array<std::string_view, 3> kJustifyV{"AudastraJustifyTop", "AudastraJustifyMiddle", "AudastraJustifyBottom"};

using namespace detail;  // 自然 T 写法

Lines Loop(std::uint32_t count, const Lines& body) {
    return Block(Call("ForLoopAMultiple", {"0", std::to_string(count - 1)}), "actions", body);
}

std::uint32_t Hash(std::string_view text) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : text) hash = (hash ^ c) * 16777619u;
    return hash;
}

enum class Property : std::uint8_t { Texture, Text, Visible, Value, Model };

struct FrameInfo {
    const UiFrame* frame = nullptr;
    std::optional<std::size_t> parent;
    std::size_t ordinal = 0;              // 树序，从 1 起
    std::optional<std::size_t> group;     // 所属重复框架（重复的根自己也算）
    bool simple = false;                  // 按 Simple 框架创建（目录里是 SIMPLE* 类型的模板）
};

class Generator {
public:
    Generator(const UiDescription& description, const UiTriggerOptions& options)
        : d_(description), options_(options), name_(UsableLabel(description.label) ? description.label : description.id) {
        contextBase_ = 1000000 + (Hash(description.id) % 1000) * 1000000;
    }

    Result<UiTriggerPlan> Run() {
        for (const auto& frame : d_.frames) Collect(frame, std::nullopt, std::nullopt);
        for (const auto& state : d_.states) {
            for (const auto& entry : state.overrides) {
                if (entry.texture) overrides_[entry.frame][Property::Texture].push_back(state.id);
                if (entry.text) overrides_[entry.frame][Property::Text].push_back(state.id);
                if (entry.visible) overrides_[entry.frame][Property::Visible].push_back(state.id);
                if (entry.value) overrides_[entry.frame][Property::Value].push_back(state.id);
                if (entry.model) overrides_[entry.frame][Property::Model].push_back(state.id);
            }
        }

        // 运行库接通前反复试；界面库可用后关掉自己再建，试满次数也关掉。
        const std::string attempts = "udg_" + UiAttemptVariableName(d_.id);
        const std::string disable = Call("DisableTrigger", {Call("GetTriggeringTrigger")});
        Lines create = Header();
        create.push_back(Set(attempts, Int(attempts, "OperatorAdd", "1")));
        Append(create, If({CompareInt(attempts, "OperatorGreater", std::to_string(kUiConnectAttempts))},
            {disable, Call("ReturnAction")}));
        Append(create, If({Call("OperatorCompareBoolean", {Call("AudastraUIIsReady"), "OperatorEqualENE", "false"})},
            {Call("ReturnAction")}));
        create.push_back(disable);
        create.push_back(Call("AudastraUILoadToc", {Quote(kUiTemplateTocMember)}));
        for (const auto& toc : options_.tocFiles) create.push_back(Call("AudastraUILoadToc", {Quote(toc)}));
        const auto creation = Segments([&](const FrameInfo& info) { return Creation(info); });
        const auto properties = Segments([&](const FrameInfo& info) { return Properties(info); });
        const auto updates = Segments([&](const FrameInfo& info) { return Updates(info); });
        // 创建连同首次刷新放得进一次执行就照旧写在一个触发器里；否则建框架、布局、刷新都分块，各开线程运行。
        const bool split = Requests(creation) + Requests(properties) + Requests(updates) > kThreadRequests;
        const auto phase = [&](const std::vector<Segment>& segments, std::string_view role, std::string_view key) {
            return split ? Threaded(segments, role, key) : Inline(segments);
        };
        Append(create, phase(creation, "建框架", "Build"));
        Append(create, phase(properties, "布局", "Layout"));
        create.push_back(Set("udg_" + UiCreatedVariableName(d_.id), "true"));
        const Lines update = phase(updates, "刷新", "Refresh");
        Append(create, update);

        Lines refresh = Header();
        Append(refresh, UntilCreated());
        Append(refresh, update);

        if (!errors_.empty()) {
            Result<UiTriggerPlan> failure;
            failure.diagnostics = std::move(errors_);
            return failure;
        }

        UiTriggerPlan plan;
        FeatureTextBuilder text;
        auto variable = [&](const std::string& name, std::string_view type, std::uint32_t size, const std::string& initial) {
            text.Variable(name, type, size ? std::optional<std::uint32_t>(size) : std::nullopt, initial);
        };
        for (const auto& info : frames_) {
            variable(UiFrameVariableName(d_.id, info.frame->id), "AudastraFrame", GroupSize(info), "AudastraFrameNone");
            for (const auto& child : Children(info)) {
                variable(UiChildVariableName(d_.id, info.frame->id, child.key), "AudastraFrame", GroupSize(info), "AudastraFrameNone");
            }
        }
        for (const auto& value : d_.values) variable(UiValueVariableName(d_.id, value.id), ValueType(value.type), value.size, Initial(value));
        if (!d_.states.empty()) variable(UiStateVariableName(d_.id), "string", 0, "\"\"");
        variable(UiCreatedVariableName(d_.id), "boolean", 0, "false");
        variable(UiAttemptVariableName(d_.id), "integer", 0, "0");
        for (const auto& sent : sent_) variable(sent.variable, sent.type, sent.size, sent.initial);

        auto trigger = [&](const std::string& name, const Lines& events, const Lines& actions) { text.Trigger(name, events, actions); };
        // 触发器的脚本名只保留 ASCII 字母数字，中文全变成下划线；名字末尾的 ASCII 编号保证脚本名不重。
        auto named = [&](std::string_view role, const std::string& key) { return UiTriggerName(d_, role, key); };
        trigger(named("创建", "Create"), {Call("TriggerRegisterTimerEventPeriodic", {Real(kUiConnectSeconds)})}, create);
        trigger(named("更新", "Update"), {}, refresh);
        for (auto& [chunkName, body] : chunks_) {
            Lines actions = Header();
            Append(actions, body);
            trigger(chunkName, {}, actions);
        }
        for (const auto& state : d_.states) trigger(UiStateTriggerName(d_, state), {}, State(state));
        if (!d_.states.empty()) trigger(named("基础状态", "BaseState"), {}, BaseState(update));
        plan.featureText = text.Text();
        plan.variables = std::move(text.variables);
        plan.triggers = std::move(text.triggers);

        Result<UiTriggerPlan> result = Result<UiTriggerPlan>::Success(std::move(plan));
        result.diagnostics = std::move(warnings_);
        return result;
    }

private:
    void Error(const std::string& message) {
        errors_.push_back(Note(DiagnosticSeverity::Error, "AUI_TRIGGER_UNSUPPORTED", message));
    }

    void Collect(const UiFrame& frame, std::optional<std::size_t> parent, std::optional<std::size_t> group) {
        const std::size_t index = frames_.size();
        // 描述检查已拒绝嵌套重复。
        if (frame.repeat) group = index;
        frames_.push_back(FrameInfo{&frame, parent, index + 1, group, frame.type == UiFrameType::Template && IsSimpleTemplate(frame)});
        byId_[frame.id] = index;
        for (const auto& child : frame.children) Collect(child, index, group);
    }

    [[nodiscard]] std::uint32_t GroupSize(const FrameInfo& info) const {
        return info.group ? frames_[*info.group].frame->repeat->count : 0;
    }

    // 按重复分组：同一重复框架及其子框架是一段，放进同一个循环（count 为实例数）；其余框架各自一段。
    struct Segment {
        std::vector<Lines> frames;  // 每个框架的语句，切块时不拆开
        std::uint32_t count = 0;
    };

    template <class Build>
    std::vector<Segment> Segments(Build&& build) {
        std::vector<Segment> out;
        for (std::size_t i = 0; i < frames_.size();) {
            const auto& info = frames_[i];
            Segment segment;
            if (!info.group) {
                segment.frames.push_back(build(info));
                ++i;
            } else {
                const auto group = *info.group;
                segment.count = GroupSize(frames_[group]);
                for (; i < frames_.size() && frames_[i].group == group; ++i) segment.frames.push_back(build(frames_[i]));
            }
            out.push_back(std::move(segment));
        }
        return out;
    }

    static Lines Inline(const std::vector<Segment>& segments) {
        Lines out;
        for (const auto& segment : segments) {
            Lines body;
            for (const auto& lines : segment.frames) Append(body, lines);
            if (body.empty()) continue;
            Append(out, segment.count ? Loop(segment.count, body) : body);
        }
        return out;
    }

    // 每个界面库调用在 JASS 里都要拼请求串、逐字符解析回应，一次执行的指令上限只够一百多个调用。
    static std::size_t Requests(const Lines& lines) {
        std::size_t count = 0;
        for (const auto& line : lines)
            for (auto at = line.find("AudastraUI"); at != std::string::npos; at = line.find("AudastraUI", at + 1)) ++count;
        return count;
    }
    static std::size_t Requests(const std::vector<Segment>& segments) {
        std::size_t total = 0;
        for (const auto& segment : segments) {
            std::size_t body = 0;
            for (const auto& lines : segment.frames) body += Requests(lines);
            total += body * std::max<std::uint32_t>(segment.count, 1);
        }
        return total;
    }

    // 切成若干个各不超过预算的触发器，用“运行触发器”各开一个线程；重复框架每个实例各运行一次，
    // 被运行的触发器里照常用循环整数 A 取当前实例。
    Lines Threaded(const std::vector<Segment>& segments, std::string_view role, std::string_view key) {
        Lines out, pending;
        std::size_t pendingRequests = 0;
        std::size_t number = 0;
        const auto chunk = [&](Lines body) {
            ++number;
            const auto name = UiTriggerName(d_, std::string(role) + " " + std::to_string(number), std::string(key) + std::to_string(number));
            chunks_.emplace_back(name, std::move(body));
            return Call("ConditionalTriggerExecute", {TriggerRef(name)});
        };
        const auto flush = [&] {
            if (pending.empty()) return;
            out.push_back(chunk(std::exchange(pending, {})));
            pendingRequests = 0;
        };
        for (const auto& segment : segments) {
            if (!segment.count) {
                for (const auto& lines : segment.frames) {
                    const auto requests = Requests(lines);
                    if (pendingRequests + requests > kThreadRequests) flush();
                    Append(pending, lines);
                    pendingRequests += requests;
                }
                continue;
            }
            flush();
            Lines calls, body;
            std::size_t bodyRequests = 0;
            for (const auto& lines : segment.frames) {
                const auto requests = Requests(lines);
                if (!body.empty() && bodyRequests + requests > kThreadRequests) {
                    calls.push_back(chunk(std::exchange(body, {})));
                    bodyRequests = 0;
                }
                Append(body, lines);
                bodyRequests += requests;
            }
            if (!body.empty()) calls.push_back(chunk(std::move(body)));
            if (!calls.empty()) Append(out, Loop(segment.count, calls));
        }
        flush();
        return out;
    }

    Lines Header() const {
        return {Call("CommentString", {Quote("由界面 " + name_ + "（" + d_.id + "）生成，重新生成会覆盖本触发器")})};
    }

    // 界面建好前不调用界面库：框架句柄还是空的，运行库也可能还没接通。
    Lines UntilCreated() const {
        return If({Call("OperatorCompareBoolean", {"udg_" + UiCreatedVariableName(d_.id), "OperatorEqualENE", "false"})},
            {Call("ReturnAction")});
    }

    // 框架变量在 from 所在上下文里的写法：同一重复组取当前下标，别的重复组取第一个实例。
    [[nodiscard]] std::string Ref(const FrameInfo& target, const FrameInfo* from) const {
        std::string name = "udg_" + UiFrameVariableName(d_.id, target.frame->id);
        if (!target.group) return name;
        if (from && from->group == target.group) return name + "[" + std::string(kLoopIndex) + "]";
        return name + "[0]";
    }
    [[nodiscard]] std::string Self(const FrameInfo& info) const { return Ref(info, &info); }

    [[nodiscard]] std::string Context(const FrameInfo& info) const {
        const std::string base = std::to_string(contextBase_ + static_cast<std::uint32_t>(info.ordinal) * 1000);
        return info.group ? Call("OperatorInt", {base, "OperatorAdd", std::string(kLoopIndex)}) : base;
    }

    // 模板里的子框架（进度条填充、按钮文字与底图）创建时按名字找一次记进变量，刷新时不再每次去找。
    struct ChildFrame {
        std::string_view name, key;
    };
    [[nodiscard]] std::vector<ChildFrame> Children(const FrameInfo& info) const {
        switch (info.frame->type) {
        case UiFrameType::Bar:
            return {{ClearBar(info) ? "AudastraBarClearFill" : "AudastraBarFill", "fill"}};
        case UiFrameType::Button:
            if (ClickButton(info)) return {{"AudastraClickButtonText", "text"}};
            return {{"AudastraButtonText", "text"}, {"AudastraButtonBackdrop", "backdrop"}, {"AudastraButtonPushed", "pushed"}};
        default:
            return {};
        }
    }
    [[nodiscard]] std::string Child(const FrameInfo& info, std::string_view name) const {
        for (const auto& child : Children(info)) {
            if (child.name != name) continue;
            const std::string variable = "udg_" + UiChildVariableName(d_.id, info.frame->id, child.key);
            return info.group ? variable + "[" + std::string(kLoopIndex) + "]" : variable;
        }
        return Call("AudastraUIFindFrame", {Quote(name), Context(info)});
    }

    // 文字所在的框架：按钮的文字在模板里的子框架上。
    [[nodiscard]] std::string TextTarget(const FrameInfo& info) const {
        if (info.frame->type != UiFrameType::Button) return Self(info);
        return Child(info, ClickButton(info) ? "AudastraClickButtonText" : "AudastraButtonText");
    }

    // 编辑器预览里没写贴图的部分是透明的；游戏里没设贴图的 BACKDROP 画成绿块。
    // 所以从头到尾都不会有贴图的框架改用不带底图的模板：底板建成普通框架，按钮建成透明点击层，进度条底层不画底图。
    [[nodiscard]] bool HasTexture(const FrameInfo& info) const {
        if (!info.frame->texture.empty() || !info.frame->pushed.empty()) return true;
        const auto found = overrides_.find(info.frame->id);
        return found != overrides_.end() && found->second.contains(Property::Texture);
    }
    [[nodiscard]] bool ClickButton(const FrameInfo& info) const {
        return info.frame->type == UiFrameType::Button && !HasTexture(info);
    }
    [[nodiscard]] bool ClearBar(const FrameInfo& info) const {
        return info.frame->type == UiFrameType::Bar && info.frame->background.empty();
    }

    [[nodiscard]] bool IsSimpleTemplate(const UiFrame& frame) {
        if (!options_.library) {
            warnings_.push_back(Note(DiagnosticSeverity::Warning, "AUI_TRIGGER_TEMPLATE_UNKNOWN",
                "没有界面目录，框架 " + frame.id + " 的模板 " + frame.templateName + " 按普通框架创建"));
            return false;
        }
        const auto found = options_.library->FindTemplate(frame.templateName);
        if (!found || !found->definition) {
            warnings_.push_back(Note(DiagnosticSeverity::Warning, "AUI_TRIGGER_TEMPLATE_UNKNOWN",
                "框架 " + frame.id + " 的模板 " + frame.templateName + " 不在已读入的界面目录里，按普通框架创建"));
            return false;
        }
        std::string type = found->definition->frameType;
        for (auto& c : type) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return type.starts_with("SIMPLE");
    }

    Lines Creation(const FrameInfo& info) {
        const UiFrame& frame = *info.frame;
        std::string parent = info.parent ? Ref(frames_[*info.parent], &info) : Call("AudastraUIGameUI");
        // 游戏只允许 Simple 框架挂在 Simple 框架下，挂到普通框架下会在游戏里出错。
        if (info.simple && info.parent && !frames_[*info.parent].simple) {
            warnings_.push_back(Note(DiagnosticSeverity::Warning, "AUI_TRIGGER_SIMPLE_PARENT",
                "框架 " + frame.id + " 的模板 " + frame.templateName + " 是 Simple 框架，父框架 " + frames_[*info.parent].frame->id
                    + " 是普通框架，游戏不允许这样挂，改挂在游戏界面根上：位置仍按锚点跟着父框架，但画在所有普通框架下面，父框架隐藏时也不跟着隐藏"));
            parent = Call("AudastraUIGameUI");
        }
        // 反过来普通框架挂在 Simple 框架下也会让游戏出错：改挂到最近的普通祖先，没有时挂游戏界面根。
        if (!info.simple && info.parent && frames_[*info.parent].simple) {
            auto ancestor = frames_[*info.parent].parent;
            while (ancestor && frames_[*ancestor].simple) ancestor = frames_[*ancestor].parent;
            const std::string target = ancestor ? "普通框架 " + frames_[*ancestor].frame->id : std::string("游戏界面根");
            warnings_.push_back(Note(DiagnosticSeverity::Warning, "AUI_TRIGGER_CLASSIC_UNDER_SIMPLE",
                "框架 " + frame.id + " 是普通框架，父框架 " + frames_[*info.parent].frame->id + " 的模板 "
                    + frames_[*info.parent].frame->templateName + " 是 Simple 框架，游戏不允许这样挂，改挂在" + target
                    + "上：位置仍按锚点跟着父框架，父框架隐藏时也不跟着隐藏"));
            parent = ancestor ? Ref(frames_[*ancestor], &info) : Call("AudastraUIGameUI");
        }
        std::string created;
        auto classic = [&](std::string_view name) {
            return Call("AudastraUICreateFrame", {Quote(name), parent, "0", Context(info)});
        };
        switch (frame.type) {
        case UiFrameType::Panel: created = classic("AudastraPanel"); break;
        case UiFrameType::Backdrop: created = classic(HasTexture(info) ? "AudastraBackdrop" : "AudastraPanel"); break;
        case UiFrameType::Text: created = classic("AudastraText"); break;
        case UiFrameType::Button: created = classic(ClickButton(info) ? "AudastraClickButton" : "AudastraButton"); break;
        case UiFrameType::Bar: created = classic(ClearBar(info) ? "AudastraBarClear" : "AudastraBar"); break;
        case UiFrameType::Model: created = classic("AudastraModel"); break;
        case UiFrameType::Template:
            created = info.simple ? Call("AudastraUICreateSimpleFrame", {Quote(frame.templateName), parent, Context(info)})
                                  : classic(frame.templateName);
            break;
        }
        Lines out{Call("SetVariable", {Self(info), created})};
        for (const auto& child : Children(info)) {
            out.push_back(Call("SetVariable", {Child(info, child.name), Call("AudastraUIFindFrame", {Quote(child.name), Context(info)})}));
        }
        return out;
    }

    // 进度条用两层普通框架：框架本身是底图，子框架 AudastraBarFill 是填充，靠左对齐，按比例改宽度。
    // 不用 SIMPLESTATUSBAR：Simple 框架画在所有普通框架下面，放进普通面板会被盖住。
    [[nodiscard]] std::string BarFill(const FrameInfo& info) const {
        return Child(info, ClearBar(info) ? "AudastraBarClearFill" : "AudastraBarFill");
    }

    [[nodiscard]] std::optional<double> BarWidth(const FrameInfo& info) {
        if (info.frame->width) return *info.frame->width;
        if (widthless_.insert(info.frame->id).second) Error("进度条 " + info.frame->id + " 没写宽度：填充按宽度显示进度，需要固定宽度");
        return std::nullopt;
    }

    std::optional<std::string> BarValue(const FrameInfo& info, double ratio) {
        const auto width = BarWidth(info);
        if (!width) return std::nullopt;
        return Call("AudastraUISetWidth", {BarFill(info), Real(std::clamp(ratio, 0.0, 1.0) * *width)});
    }

    std::optional<std::string> BarValue(const FrameInfo& info, const std::string& ratio) {
        const auto width = BarWidth(info);
        if (!width) return std::nullopt;
        const std::string clamped = Call("RMaxBJ", {"0", Call("RMinBJ", {"1", ratio})});
        return Call("AudastraUIPushWidth", {BarFill(info), Call("OperatorReal", {clamped, "OperatorMultiply", Real(*width)})});
    }

    // 锚点偏移；重复的根按实例下标整体平移：不换行时下标乘间距，换行时列号、行号分别乘各自的间距。
    [[nodiscard]] static std::string Offset(double base, double step, bool shifted, std::uint32_t columns = 0, double rowStep = 0.0) {
        if (!shifted) return Real(base);
        const std::string index(kLoopIndex);
        const std::string count = std::to_string(columns);
        const auto moved = [](const std::string& times, double by) {
            return Call("OperatorReal", {Call("I2R", {times}), "OperatorMultiply", Real(by)});
        };
        std::string sum = base == 0.0 ? std::string() : Real(base);
        const auto add = [&sum](const std::string& term) { sum = sum.empty() ? term : Call("OperatorReal", {sum, "OperatorAdd", term}); };
        if (step != 0.0) add(moved(columns ? Call("ModuloInteger", {index, count}) : index, step));
        if (columns && rowStep != 0.0) add(moved(Call("OperatorInt", {index, "OperatorDivide", count}), rowStep));
        return sum.empty() ? Real(0.0) : sum;
    }

    Lines Anchors(const FrameInfo& info) {
        const UiFrame& frame = *info.frame;
        Lines out;
        if (frame.anchors.empty()) return out;
        if (frame.type == UiFrameType::Template) out.push_back(Call("AudastraUIClearPoints", {Self(info)}));
        const bool root = info.group && frames_[*info.group].frame == &frame;
        const double dx = root ? frame.repeat->dx : 0.0;
        const double dy = root ? frame.repeat->dy : 0.0;
        const std::uint32_t columns = root ? frame.repeat->columns : 0;
        const double rowDx = root ? frame.repeat->rowDx : 0.0;
        const double rowDy = root ? frame.repeat->rowDy : 0.0;
        for (const auto& anchor : frame.anchors) {
            const auto point = static_cast<std::size_t>(anchor.point);
            const auto targetPoint = static_cast<std::size_t>(anchor.targetPoint);
            std::string target;
            switch (anchor.target) {
            case UiAnchorTarget::Parent:
                target = info.parent ? Ref(frames_[*info.parent], &info) : Call("AudastraUIGameUI");
                break;
            case UiAnchorTarget::Screen:
                out.push_back(Call("AudastraUISetAbsPoint", {Self(info), std::string(kAnchors[point]),
                    Offset(kScreenPoints[targetPoint][0] + anchor.x, dx, root, columns, rowDx),
                    Offset(kScreenPoints[targetPoint][1] + anchor.y, dy, root, columns, rowDy)}));
                continue;
            case UiAnchorTarget::Frame: {
                const auto found = byId_.find(anchor.targetId);
                if (found == byId_.end()) {
                    Error("框架 " + frame.id + " 锚在不存在的框架 " + anchor.targetId + " 上");
                    continue;
                }
                target = Ref(frames_[found->second], &info);
                break;
            }
            case UiAnchorTarget::GameArea: {
                if (anchor.targetId == kGameCodeRoot) {
                    target = Call("AudastraUIGameUI");
                    break;
                }
                const auto area = CodeArea(anchor.targetId);
                if (!area) {
                    Error("框架 " + frame.id + " 锚在不存在的游戏界面区域 " + anchor.targetId + " 上");
                    continue;
                }
                // 游戏代码区域没有可取的框架句柄，按固定位置换成屏幕坐标。
                if (fixedAreas_.insert(frame.id + "\n" + anchor.targetId).second) {
                    warnings_.push_back(Note(DiagnosticSeverity::Warning, "AUI_TRIGGER_AREA_FIXED",
                        "框架 " + frame.id + " 锚在游戏界面区域" + std::string(area->label) + "（" + anchor.targetId
                            + "），按 1.27a 经典版里这块区域的固定位置换算成屏幕坐标；地图移动或隐藏这块区域时不跟随"));
                }
                out.push_back(Call("AudastraUISetAbsPoint", {Self(info), std::string(kAnchors[point]),
                    Offset(FramePointX(area->rect, anchor.targetPoint) + anchor.x, dx, root, columns, rowDx),
                    Offset(FramePointY(area->rect, anchor.targetPoint) + anchor.y, dy, root, columns, rowDy)}));
                continue;
            }
            }
            out.push_back(Call("AudastraUISetPoint", {Self(info), std::string(kAnchors[point]), target,
                std::string(kAnchors[targetPoint]), Offset(anchor.x, dx, root, columns, rowDx), Offset(anchor.y, dy, root, columns, rowDy)}));
        }
        return out;
    }

    Lines TextStyle(const FrameInfo& info) {
        const UiFrame& frame = *info.frame;
        Lines out;
        const std::string target = TextTarget(info);
        if (!frame.font.empty() || std::abs(frame.fontHeight - kTemplateFontHeight) > 1e-9) {
            const std::string& font = frame.font.empty() ? options_.defaultFont : frame.font;
            if (font.empty()) Error("框架 " + frame.id + " 改了字号，但没有游戏默认字体的路径");
            else out.push_back(Call("AudastraUISetFont", {target, Quote(font), Real(frame.fontHeight), "0"}));
        }
        const auto& c = frame.color;
        if (c[0] != 1.0 || c[1] != 1.0 || c[2] != 1.0 || c[3] != 1.0) {
            auto byte = [](double v) { return std::to_string(static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0))); };
            out.push_back(Call("AudastraUISetColor", {target, byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3])}));
        }
        if (frame.justifyH != JustifyH::Center || frame.justifyV != JustifyV::Middle) {
            out.push_back(Call("AudastraUISetTextAlign", {target, std::string(kJustifyH[static_cast<std::size_t>(frame.justifyH)]),
                std::string(kJustifyV[static_cast<std::size_t>(frame.justifyV)])}));
        }
        return out;
    }

    [[nodiscard]] bool HasReferences(const std::string& text) const {
        const auto pieces = detail::ParseUiTextTemplate(text, nullptr);
        if (!pieces) return false;
        for (const auto& piece : *pieces) {
            if (piece.reference) return true;
        }
        return false;
    }

    // 改贴图：按钮常态贴图在子框架上；没写按下贴图时按下也用同一张，否则按住时露出没有贴图的按下底图。
    // expression 为贴图路径的表达式；push 为刷新块用的“刷新贴图”。
    std::optional<Lines> TextureCall(const FrameInfo& info, const std::string& expression, bool push = false) {
        const auto set = [&](const std::string& target) {
            return push ? Call("AudastraUIPushTexture", {target, expression}) : Call("AudastraUISetTexture", {target, expression, "false"});
        };
        switch (info.frame->type) {
        case UiFrameType::Backdrop:
            return Lines{set(Self(info))};
        case UiFrameType::Bar:
            return Lines{set(BarFill(info))};
        case UiFrameType::Button: {
            Lines out{set(Child(info, "AudastraButtonBackdrop"))};
            if (info.frame->pushed.empty()) out.push_back(set(Child(info, "AudastraButtonPushed")));
            return out;
        }
        default:
            Error("框架 " + info.frame->id + " 是 " + std::string(UiFrameTypeName(info.frame->type)) + "，没有贴图");
            return std::nullopt;
        }
    }
    // 游戏按完整文件名找贴图，没写扩展名时与预览一样补 .blp，否则游戏里显示成绿块。
    std::optional<Lines> Texture(const FrameInfo& info, const std::string& path) {
        return TextureCall(info, Quote(EnsureTextureExtension(path)));
    }
    // 取值模板拼出的贴图：以固定文字结尾且没有扩展名时补 .blp；以取值结尾时由取值给出完整路径。
    static std::string TextureTemplate(const std::string& path) {
        return !path.empty() && path.back() == '}' ? path : EnsureTextureExtension(path);
    }

    // 改模型（模型类型参数为 0）。游戏按完整文件名找模型，没写扩展名时补 .mdx。
    [[nodiscard]] Lines ModelCall(const FrameInfo& info, const std::string& expression) const {
        return Lines{Call("AudastraUISetModel", {Self(info), expression, "0"})};
    }
    [[nodiscard]] Lines Model(const FrameInfo& info, const std::string& path) const {
        return ModelCall(info, Quote(EnsureModelExtension(path)));
    }
    static std::string ModelTemplate(const std::string& path) {
        return !path.empty() && path.back() == '}' ? path : EnsureModelExtension(path);
    }

    // 创建时设的固定贴图；写了取值模板的贴图由更新块设。
    void FixedTexture(const FrameInfo& info, Lines& out) {
        const std::string& texture = info.frame->texture;
        if (texture.empty() || HasReferences(texture)) return;
        if (auto lines = Texture(info, texture)) Append(out, *lines);
    }

    Lines Properties(const FrameInfo& info) {
        const UiFrame& frame = *info.frame;
        Lines out = Anchors(info);
        if (frame.width && frame.height) {
            out.push_back(Call("AudastraUISetSize", {Self(info), Real(*frame.width), Real(*frame.height)}));
        } else if (frame.width) {
            out.push_back(Call("AudastraUISetWidth", {Self(info), Real(*frame.width)}));
        } else if (frame.height) {
            out.push_back(Call("AudastraUISetHeight", {Self(info), Real(*frame.height)}));
        }
        switch (frame.type) {
        case UiFrameType::Backdrop:
            FixedTexture(info, out);
            break;
        case UiFrameType::Bar: {
            const std::string fill = BarFill(info);
            out.push_back(Call("AudastraUIClearPoints", {fill}));
            for (const auto point : {FramePoint::TopLeft, FramePoint::BottomLeft}) {
                const std::string anchor(kAnchors[static_cast<std::size_t>(point)]);
                out.push_back(Call("AudastraUISetPoint", {fill, anchor, Self(info), anchor, "0", "0"}));
            }
            FixedTexture(info, out);
            // 填充从头到尾都没有贴图时预览里看不见，游戏里也藏起来，不画成绿条。
            if (!HasTexture(info)) out.push_back(Call("AudastraUIShow", {fill, "false"}));
            if (!frame.background.empty()) {
                out.push_back(Call("AudastraUISetTexture", {Self(info), Quote(EnsureTextureExtension(frame.background)), "false"}));
            }
            if (!frame.value.binding) {
                if (auto line = BarValue(info, frame.value.literal)) out.push_back(*line);
            }
            break;
        }
        case UiFrameType::Button:
            FixedTexture(info, out);
            if (!frame.pushed.empty()) {
                out.push_back(Call("AudastraUISetTexture", {Child(info, "AudastraButtonPushed"), Quote(EnsureTextureExtension(frame.pushed)), "false"}));
            }
            [[fallthrough]];
        case UiFrameType::Text:
            Append(out, TextStyle(info));
            if (!frame.text.empty() && !HasReferences(frame.text)) {
                if (auto text = TextExpression(info, frame.text)) out.push_back(Call("AudastraUISetText", {TextTarget(info), *text}));
            }
            break;
        case UiFrameType::Model:
            if (!frame.model.empty() && !HasReferences(frame.model)) Append(out, Model(info, frame.model));
            break;
        case UiFrameType::Panel:
        case UiFrameType::Template:
            break;
        }
        if (!frame.visible.binding && !frame.visible.literal) out.push_back(Call("AudastraUIShow", {Self(info), "false"}));
        for (const auto& event : d_.events) {
            if (event.frame != frame.id) continue;
            const std::string_view kind = event.kind == UiEventKind::Click ? "AudastraFrameEventClick"
                : event.kind == UiEventKind::Enter ? "AudastraFrameEventEnter" : "AudastraFrameEventLeave";
            out.push_back(Call("AudastraUIWatchFrame", {Self(info), std::string(kind), Quote(UiEventKey(d_.id, event.id)),
                info.group ? std::string(kLoopIndex) : std::string("0")}));
        }
        return out;
    }

    const UiValue* FindValue(const std::string& id) const {
        for (const auto& value : d_.values) {
            if (value.id == id) return &value;
        }
        return nullptr;
    }

    // 数值变量的写法；取不到时记错误并返回空。perInstance 在重复框架外没有下标，按预览规则不取值。
    std::optional<std::string> ValueRef(const FrameInfo& info, const std::string& id, bool perInstance,
        std::optional<std::uint32_t> index, const UiValue** spec) {
        const UiValue* value = FindValue(id);
        if (!value) {
            Error("框架 " + info.frame->id + " 用了不存在的数值 " + id);
            return std::nullopt;
        }
        *spec = value;
        std::string name = "udg_" + UiValueVariableName(d_.id, id);
        if (perInstance) {
            if (!info.group) return std::nullopt;
            return name + "[" + std::string(kLoopIndex) + "]";
        }
        if (index) return name + "[" + std::to_string(*index) + "]";
        return name;
    }

    std::optional<std::string> TextExpression(const FrameInfo& info, const std::string& text) {
        std::string error;
        const auto pieces = detail::ParseUiTextTemplate(text, &error);
        if (!pieces) {
            Error("框架 " + info.frame->id + " 的文字模板有误：" + error);
            return std::nullopt;
        }
        std::optional<std::string> result;
        for (const auto& piece : *pieces) {
            std::string part;
            if (!piece.reference) {
                part = Quote(piece.literal);
            } else {
                const UiValue* spec = nullptr;
                const auto& reference = *piece.reference;
                const auto ref = ValueRef(info, reference.id, reference.perInstance, reference.index, &spec);
                if (!ref) continue;
                switch (spec->type) {
                case UiValueType::Real:
                    part = Call("AudastraUIFormatReal", {*ref, piece.decimals ? std::to_string(*piece.decimals) : "-1"});
                    break;
                case UiValueType::Integer: part = Call("I2S", {*ref}); break;
                case UiValueType::Boolean: part = Call("AudastraUIBooleanText", {*ref}); break;
                case UiValueType::String: part = *ref; break;
                }
            }
            result = result ? Call("OperatorString", {*result, part}) : part;
        }
        return result ? *result : Quote("");
    }

    std::optional<std::string> NumberExpression(const FrameInfo& info, const UiBinding& binding) {
        const UiValue* spec = nullptr;
        auto ref = ValueRef(info, binding.value, binding.perInstance, binding.index, &spec);
        if (!ref) return std::nullopt;
        std::string number;
        switch (spec->type) {
        case UiValueType::Real: number = *ref; break;
        case UiValueType::Integer: number = Call("I2R", {*ref}); break;
        default:
            Error("框架 " + info.frame->id + " 的数值绑定到 " + spec->id + "，它不是实数或整数");
            return std::nullopt;
        }
        if (binding.scale != 1.0) number = Call("OperatorReal", {number, "OperatorMultiply", Real(binding.scale)});
        if (binding.offset != 0.0) number = Call("OperatorReal", {number, "OperatorAdd", Real(binding.offset)});
        return number;
    }

    Lines FlagStatements(const FrameInfo& info, const UiBinding& binding) {
        const UiValue* spec = nullptr;
        const auto ref = ValueRef(info, binding.value, binding.perInstance, binding.index, &spec);
        if (!ref) return {};
        const std::string self = Self(info);
        std::string condition;
        switch (spec->type) {
        case UiValueType::Boolean:
            condition = Call("OperatorCompareBoolean", {*ref, "OperatorEqualENE", "true"});
            break;
        case UiValueType::Integer:
            condition = Call("OperatorCompareInteger", {*ref, "OperatorNotEqual", "0"});
            break;
        case UiValueType::Real:
            condition = Call("OperatorCompareReal", {*ref, "OperatorNotEqual", "0"});
            break;
        case UiValueType::String:
            condition = Call("OperatorCompareString", {*ref, "OperatorNotEqualENE", Quote("")});
            break;
        }
        // 上次发的显示状态记成整数：-1 没发过，0 隐藏，1 显示。
        const auto name = UiSentVariableName(d_.id, info.frame->id, PropertyKey(Property::Visible));
        if (std::none_of(sent_.begin(), sent_.end(), [&](const Sent& sent) { return sent.variable == name; })) {
            sent_.push_back({name, "integer", "-1", GroupSize(info)});
        }
        const std::string sent = SentRef(info, Property::Visible);
        auto show = [&](bool shown) {
            const std::string flag = shown ? "1" : "0";
            return If({Call("OperatorCompareInteger", {sent, "OperatorNotEqual", flag})},
                {Set(sent, flag), Call("AudastraUIPushShown", {self, shown ? "true" : "false"})});
        };
        return If({condition}, show(!binding.invert), show(binding.invert));
    }

    // 有状态覆盖这一属性时，只在当前状态不覆盖它时刷新。
    Lines Guarded(const FrameInfo& info, Property property, Lines statements) const {
        if (statements.empty()) return statements;
        const auto frame = overrides_.find(info.frame->id);
        if (frame == overrides_.end()) return statements;
        const auto states = frame->second.find(property);
        if (states == frame->second.end()) return statements;
        Lines conditions;
        for (const auto& state : states->second) {
            conditions.push_back(Call("OperatorCompareString", {"udg_" + UiStateVariableName(d_.id), "OperatorNotEqualENE", Quote(state)}));
        }
        return If(conditions, statements);
    }

    // 上次发给游戏的绑定值：取值没变就不再发。界面库每次请求都很贵，团队框架每 0.1 秒把所有框架重发一遍
    // 会占满游戏主线程。显示/隐藏也记。
    struct Sent {
        std::string variable, type, initial;
        std::uint32_t size = 0;
    };
    static constexpr std::string_view kUnsent = "AUI:unsent";
    static constexpr double kUnsentNumber = -1.0e9;

    [[nodiscard]] static std::string_view PropertyKey(Property property) {
        switch (property) {
        case Property::Texture: return "texture";
        case Property::Text: return "text";
        case Property::Visible: return "visible";
        case Property::Value: return "value";
        case Property::Model: return "model";
        }
        return "value";
    }
    [[nodiscard]] std::string SentRef(const FrameInfo& info, Property property) const {
        const std::string name = "udg_" + UiSentVariableName(d_.id, info.frame->id, PropertyKey(property));
        return info.group ? name + "[" + std::string(kLoopIndex) + "]" : name;
    }
    // statements 用 SentRef 取刚记下的值，取值表达式只在变化时多算一次。贴图取值为空时不发：游戏找不到空路径会报错，
    // 空贴图的框架由显示绑定隐藏，之后取回原值也不用重发。
    Lines Changed(const FrameInfo& info, Property property, std::string_view type, const std::string& key, const Lines& statements) {
        if (statements.empty()) return statements;
        const auto name = UiSentVariableName(d_.id, info.frame->id, PropertyKey(property));
        if (std::none_of(sent_.begin(), sent_.end(), [&](const Sent& sent) { return sent.variable == name; })) {
            sent_.push_back({name, std::string(type), type == "real" ? Real(kUnsentNumber) : Quote(kUnsent), GroupSize(info)});
        }
        const std::string sent = SentRef(info, property);
        const std::string differs = type == "real" ? Call("OperatorCompareReal", {key, "OperatorNotEqual", sent})
                                                   : Call("OperatorCompareString", {key, "OperatorNotEqualENE", sent});
        Lines body{Set(sent, key)};
        Append(body, statements);
        if (property == Property::Texture || property == Property::Model) {
            return If({differs, Call("OperatorCompareString", {key, "OperatorNotEqualENE", Quote("")})}, body);
        }
        return If({differs}, body);
    }
    // 状态改过的属性回到基础状态时要重发，即使绑定的取值没变。
    void Unsend(const FrameInfo& info, Property property, Lines& statements) const {
        const auto name = UiSentVariableName(d_.id, info.frame->id, PropertyKey(property));
        const auto found = std::find_if(sent_.begin(), sent_.end(), [&](const Sent& sent) { return sent.variable == name; });
        if (found != sent_.end()) statements.push_back(Set(SentRef(info, property), found->initial));
    }

    Lines Updates(const FrameInfo& info) {
        const UiFrame& frame = *info.frame;
        Lines out;
        if ((frame.type == UiFrameType::Text || frame.type == UiFrameType::Button) && HasReferences(frame.text)) {
            if (auto text = TextExpression(info, frame.text)) {
                Append(out, Guarded(info, Property::Text, Changed(info, Property::Text, "string", *text,
                    {Call("AudastraUIPushText", {TextTarget(info), SentRef(info, Property::Text)})})));
            }
        }
        if (!frame.texture.empty() && HasReferences(frame.texture)) {
            if (auto expression = TextExpression(info, TextureTemplate(frame.texture))) {
                if (auto lines = TextureCall(info, SentRef(info, Property::Texture), true)) {
                    Append(out, Guarded(info, Property::Texture, Changed(info, Property::Texture, "string", *expression, *lines)));
                }
            }
        }
        if (frame.type == UiFrameType::Model && HasReferences(frame.model)) {
            if (auto expression = TextExpression(info, ModelTemplate(frame.model))) {
                Append(out, Guarded(info, Property::Model, Changed(info, Property::Model, "string", *expression,
                    ModelCall(info, SentRef(info, Property::Model)))));
            }
        }
        if (frame.type == UiFrameType::Bar && frame.value.binding) {
            if (auto number = NumberExpression(info, *frame.value.binding)) {
                if (auto line = BarValue(info, SentRef(info, Property::Value))) {
                    Append(out, Guarded(info, Property::Value, Changed(info, Property::Value, "real", *number, {*line})));
                }
            }
        }
        if (frame.visible.binding) Append(out, Guarded(info, Property::Visible, FlagStatements(info, *frame.visible.binding)));
        return out;
    }

    // 重复框架上的语句对所有实例执行。
    Lines ForAll(const FrameInfo& info, const Lines& statements) const {
        if (!info.group || statements.empty()) return statements;
        return Loop(GroupSize(frames_[*info.group]), statements);
    }

    const FrameInfo* Find(const std::string& id) {
        const auto found = byId_.find(id);
        if (found == byId_.end()) {
            Error("状态覆盖了不存在的框架 " + id);
            return nullptr;
        }
        return &frames_[found->second];
    }

    Lines State(const UiState& state) {
        Lines out = Header();
        Append(out, UntilCreated());
        out.push_back(Call("SetVariable", {"udg_" + UiStateVariableName(d_.id), Quote(state.id)}));
        for (const auto& entry : state.overrides) {
            const FrameInfo* info = Find(entry.frame);
            if (!info) continue;
            Lines statements;
            if (entry.texture) {
                if (auto lines = Texture(*info, *entry.texture)) Append(statements, *lines);
            }
            if (entry.text) {
                if (info->frame->type != UiFrameType::Text && info->frame->type != UiFrameType::Button) {
                    Error("状态 " + state.id + " 给框架 " + entry.frame + " 改文字，它没有文字");
                } else if (auto text = TextExpression(*info, *entry.text)) {
                    statements.push_back(Call("AudastraUISetText", {TextTarget(*info), *text}));
                }
            }
            if (entry.visible) statements.push_back(Call("AudastraUIShow", {Self(*info), *entry.visible ? "true" : "false"}));
            if (entry.value) {
                if (info->frame->type != UiFrameType::Bar) Error("状态 " + state.id + " 给框架 " + entry.frame + " 改数值，它不是进度条");
                else if (auto line = BarValue(*info, *entry.value)) statements.push_back(*line);
            }
            if (entry.model) {
                if (info->frame->type != UiFrameType::Model) Error("状态 " + state.id + " 给框架 " + entry.frame + " 改模型，它不是模型框架");
                else Append(statements, Model(*info, *entry.model));
            }
            Append(out, ForAll(*info, statements));
        }
        return out;
    }

    // 回到基础状态：被覆盖过的属性恢复成描述里的固定值；绑定数值的属性由随后的更新块刷新。
    Lines BaseState(const Lines& update) {
        Lines out = Header();
        Append(out, UntilCreated());
        out.push_back(Call("SetVariable", {"udg_" + UiStateVariableName(d_.id), Quote("")}));
        for (const auto& info : frames_) {
            const auto found = overrides_.find(info.frame->id);
            if (found == overrides_.end()) continue;
            const UiFrame& frame = *info.frame;
            Lines statements;
            for (const auto& [property, states] : found->second) {
                switch (property) {
                case Property::Texture:
                    Unsend(info, property, statements);
                    if (frame.texture.empty()) {
                        warnings_.push_back(Note(DiagnosticSeverity::Warning, "AUI_TRIGGER_TEXTURE_RESTORE",
                            "框架 " + frame.id + " 原本没有贴图，回到基础状态时保留状态设的贴图"));
                    } else if (!HasReferences(frame.texture)) {
                        if (auto lines = Texture(info, frame.texture)) Append(statements, *lines);
                    }
                    break;
                case Property::Text:
                    Unsend(info, property, statements);
                    if (!HasReferences(frame.text)) {
                        if (auto text = TextExpression(info, frame.text)) statements.push_back(Call("AudastraUISetText", {TextTarget(info), *text}));
                    }
                    break;
                case Property::Visible:
                    Unsend(info, property, statements);
                    if (!frame.visible.binding) statements.push_back(Call("AudastraUIShow", {Self(info), frame.visible.literal ? "true" : "false"}));
                    break;
                case Property::Value:
                    Unsend(info, property, statements);
                    if (!frame.value.binding) {
                        if (auto line = BarValue(info, frame.value.literal)) statements.push_back(*line);
                    }
                    break;
                case Property::Model:
                    Unsend(info, property, statements);
                    if (frame.model.empty()) {
                        warnings_.push_back(Note(DiagnosticSeverity::Warning, "AUI_TRIGGER_MODEL_RESTORE",
                            "框架 " + frame.id + " 原本没有模型，回到基础状态时保留状态设的模型"));
                    } else if (!HasReferences(frame.model)) {
                        Append(statements, Model(info, frame.model));
                    }
                    break;
                }
            }
            Append(out, ForAll(info, statements));
        }
        Append(out, update);
        return out;
    }

    static std::string_view ValueType(UiValueType type) {
        switch (type) {
        case UiValueType::Real: return "real";
        case UiValueType::Integer: return "integer";
        case UiValueType::Boolean: return "boolean";
        case UiValueType::String: return "string";
        }
        return "real";
    }

    static std::string Initial(const UiValue& value) {
        switch (value.type) {
        case UiValueType::String: return Quote(value.defaultValue);
        case UiValueType::Boolean: return value.defaultValue == "true" ? "true" : "false";
        case UiValueType::Integer: return value.defaultValue.empty() ? "0" : value.defaultValue;
        case UiValueType::Real: {
            double number = 0.0;
            const auto& text = value.defaultValue;
            std::from_chars(text.data(), text.data() + text.size(), number);
            return Real(number);
        }
        }
        return "0";
    }

    struct CodeAreaPlace {
        std::string_view label;
        FrameRect rect;
    };

    // 游戏代码区域在 1.27a 经典版里的固定矩形（GameCodeLayout 的数据，首次用到时排一次版）。
    std::optional<CodeAreaPlace> CodeArea(const std::string& id) {
        if (!areaTree_) {
            areaTree_.emplace();
            (void)areaTree_->AddGameCodeAreas();
            (void)areaTree_->Layout();
        }
        const auto node = areaTree_->FindCodeArea(id);
        const auto& areas = GameCodeAreas();
        const auto found = std::find_if(areas.begin(), areas.end(), [&](const GameCodeArea& area) { return area.id == id; });
        if (!node || found == areas.end()) return std::nullopt;
        return CodeAreaPlace{found->label, areaTree_->Nodes()[*node].rect};
    }

    const UiDescription& d_;
    const UiTriggerOptions& options_;
    std::string name_;
    std::uint32_t contextBase_ = 0;
    std::vector<FrameInfo> frames_;
    std::unordered_map<std::string, std::size_t> byId_;
    std::unordered_map<std::string, std::map<Property, std::vector<std::string>>> overrides_;
    std::vector<Diagnostic> errors_;
    std::vector<Diagnostic> warnings_;
    std::optional<FrameTree> areaTree_;
    std::set<std::string> fixedAreas_;  // 已提示过的“框架\n区域”
    std::set<std::string> widthless_;   // 已报过没写宽度的进度条
    std::vector<std::pair<std::string, Lines>> chunks_;  // 分块运行的触发器：名字和动作
    std::vector<Sent> sent_;
};

} // namespace

Result<UiTriggerPlan> GenerateUiTriggers(const UiDescription& description, const UiTriggerOptions& options) {
    if (const auto problems = ValidateUiDescription(description); !problems.empty()) {
        for (const auto& problem : problems) {
            if (problem.severity == DiagnosticSeverity::Error) {
                Result<UiTriggerPlan> failure;
                failure.diagnostics = problems;
                return failure;
            }
        }
    }
    return Generator(description, options).Run();
}

std::string UiTemplateTocText() {
    return std::string(kUiTemplateFdfMember) + "\r\n\r\n";
}

std::string UiTemplateFdfText() {
    // 子框架名固定，生成的 T 用“按名字取框架 + 编号”改按钮文字、按下贴图和进度条底图。
    static constexpr std::string_view text =
        "// Audastra 界面描述生成的触发器使用的基础模板。由编辑器生成，重新生成会覆盖。\r\n"
        "\r\n"
        "Frame \"FRAME\" \"AudastraPanel\" {\r\n"
        "}\r\n"
        "\r\n"
        "Frame \"BACKDROP\" \"AudastraBackdrop\" {\r\n"
        "}\r\n"
        "\r\n"
        "Frame \"MODEL\" \"AudastraModel\" {\r\n"
        "}\r\n"
        "\r\n"
        "Frame \"TEXT\" \"AudastraText\" {\r\n"
        "    DecorateFileNames,\r\n"
        "    FrameFont \"MasterFont\", 0.011, \"\",\r\n"
        "    FontJustificationH JUSTIFYCENTER,\r\n"
        "    FontJustificationV JUSTIFYMIDDLE,\r\n"
        "    FontShadowColor 0.0 0.0 0.0 0.9,\r\n"
        "    FontShadowOffset 0.001 -0.001,\r\n"
        "}\r\n"
        "\r\n"
        "Frame \"BACKDROP\" \"AudastraBar\" {\r\n"
        "    Width 0.1,\r\n"
        "    Height 0.012,\r\n"
        "    Frame \"BACKDROP\" \"AudastraBarFill\" {\r\n"
        "        SetAllPoints,\r\n"
        "    }\r\n"
        "}\r\n"
        "\r\n"
        "// 没有底图的进度条：底层是不画东西的普通框架。\r\n"
        "Frame \"FRAME\" \"AudastraBarClear\" {\r\n"
        "    Width 0.1,\r\n"
        "    Height 0.012,\r\n"
        "    Frame \"BACKDROP\" \"AudastraBarClearFill\" {\r\n"
        "        SetAllPoints,\r\n"
        "    }\r\n"
        "}\r\n"
        "\r\n"
        "Frame \"GLUETEXTBUTTON\" \"AudastraButton\" {\r\n"
        "    Width 0.1,\r\n"
        "    Height 0.03,\r\n"
        "    ControlStyle \"AUTOTRACK|HIGHLIGHTONMOUSEOVER\",\r\n"
        "    ControlBackdrop \"AudastraButtonBackdrop\",\r\n"
        "    Frame \"BACKDROP\" \"AudastraButtonBackdrop\" {\r\n"
        "        SetAllPoints,\r\n"
        "    }\r\n"
        "    ControlPushedBackdrop \"AudastraButtonPushed\",\r\n"
        "    Frame \"BACKDROP\" \"AudastraButtonPushed\" {\r\n"
        "        SetAllPoints,\r\n"
        "    }\r\n"
        "    ControlMouseOverHighlight \"AudastraButtonHighlight\",\r\n"
        "    Frame \"HIGHLIGHT\" \"AudastraButtonHighlight\" {\r\n"
        "        DecorateFileNames,\r\n"
        "        HighlightType \"FILETEXTURE\",\r\n"
        "        HighlightAlphaFile \"EscMenuButtonMouseOverHighlight\",\r\n"
        "        HighlightAlphaMode \"ADD\",\r\n"
        "    }\r\n"
        "    ButtonText \"AudastraButtonText\",\r\n"
        "    Frame \"TEXT\" \"AudastraButtonText\" {\r\n"
        "        DecorateFileNames,\r\n"
        "        FrameFont \"MasterFont\", 0.011, \"\",\r\n"
        "        FontJustificationH JUSTIFYCENTER,\r\n"
        "        FontJustificationV JUSTIFYMIDDLE,\r\n"
        "        FontShadowColor 0.0 0.0 0.0 0.9,\r\n"
        "        FontShadowOffset 0.001 -0.001,\r\n"
        "    }\r\n"
        "}\r\n"
        "\r\n"
        "// 没有贴图的按钮：透明点击层，只留鼠标悬停高亮和文字。\r\n"
        "Frame \"GLUETEXTBUTTON\" \"AudastraClickButton\" {\r\n"
        "    Width 0.1,\r\n"
        "    Height 0.03,\r\n"
        "    ControlStyle \"AUTOTRACK|HIGHLIGHTONMOUSEOVER\",\r\n"
        "    ControlMouseOverHighlight \"AudastraClickButtonHighlight\",\r\n"
        "    Frame \"HIGHLIGHT\" \"AudastraClickButtonHighlight\" {\r\n"
        "        DecorateFileNames,\r\n"
        "        HighlightType \"FILETEXTURE\",\r\n"
        "        HighlightAlphaFile \"EscMenuButtonMouseOverHighlight\",\r\n"
        "        HighlightAlphaMode \"ADD\",\r\n"
        "    }\r\n"
        "    ButtonText \"AudastraClickButtonText\",\r\n"
        "    Frame \"TEXT\" \"AudastraClickButtonText\" {\r\n"
        "        DecorateFileNames,\r\n"
        "        FrameFont \"MasterFont\", 0.011, \"\",\r\n"
        "        FontJustificationH JUSTIFYCENTER,\r\n"
        "        FontJustificationV JUSTIFYMIDDLE,\r\n"
        "        FontShadowColor 0.0 0.0 0.0 0.9,\r\n"
        "        FontShadowOffset 0.001 -0.001,\r\n"
        "    }\r\n"
        "}\r\n";
    return std::string(text);
}

std::string UiFrameVariableName(std::string_view description, std::string_view frame) {
    return "AUI_" + std::string(description) + "_" + std::string(frame);
}

std::string UiValueVariableName(std::string_view description, std::string_view value) {
    return "AUIV_" + std::string(description) + "_" + std::string(value);
}

std::string UiStateVariableName(std::string_view description) {
    return "AUIS_" + std::string(description);
}

std::string UiCreatedVariableName(std::string_view description) {
    return "AUIC_" + std::string(description);
}

std::string UiAttemptVariableName(std::string_view description) {
    return "AUIA_" + std::string(description);
}

std::string UiSentVariableName(std::string_view description, std::string_view frame, std::string_view property) {
    return "AUIL_" + std::string(description) + "_" + std::string(frame) + "_" + std::string(property);
}

std::string UiChildVariableName(std::string_view description, std::string_view frame, std::string_view child) {
    return "AUIK_" + std::string(description) + "_" + std::string(frame) + "_" + std::string(child);
}

std::string UiTriggerName(const UiDescription& description, std::string_view role, std::string_view key) {
    const std::string& name = UsableLabel(description.label) ? description.label : description.id;
    return name + " " + std::string(role) + " " + description.id + "_" + std::string(key);
}

std::string UiStateTriggerName(const UiDescription& description, const UiState& state) {
    return UiTriggerName(description, "状态 " + (UsableLabel(state.label) ? state.label : state.id), "State_" + state.id);
}

std::string UiEventKey(std::string_view description, std::string_view event) {
    return std::string(description) + "." + std::string(event);
}

} // namespace War3Map::GameUi
