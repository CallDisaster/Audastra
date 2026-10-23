#include "War3MapCore/GameUi/UiDescriptionEdit.h"

#include "UiDescriptionText.h"
#include "../Core/Serialization/StrictJson.h"

#include <algorithm>
#include <functional>
#include <span>

// 编辑在描述的规范 JSON 上进行，最后整份交给严格读取器：编辑与读取文件用同一套规则。
namespace War3Map::GameUi {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::size_t kMaxEdits = 256;

bool Is(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

struct Located {
    Json* array = nullptr;
    std::size_t index = 0;
    [[nodiscard]] Json& Frame() const { return (*array)[index]; }
};

std::optional<Located> FindFrame(Json& frames, std::string_view id) {
    if (!frames.is_array()) return std::nullopt;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        Json& frame = frames[i];
        if (frame.is_object() && frame.contains("id") && Is(frame["id"], id)) return Located{&frames, i};
        if (frame.is_object() && frame.contains("children")) {
            if (auto found = FindFrame(frame["children"], id)) return found;
        }
    }
    return std::nullopt;
}

void CollectIds(const Json& frame, std::vector<std::string>& ids) {
    if (frame.contains("id") && frame["id"].is_string()) ids.push_back(frame["id"].get<std::string>());
    if (frame.contains("children") && frame["children"].is_array()) {
        for (const auto& child : frame["children"]) CollectIds(child, ids);
    }
}

void ForEachFrame(Json& frames, const std::function<void(Json&)>& visit) {
    if (!frames.is_array()) return;
    for (auto& frame : frames) {
        visit(frame);
        if (frame.contains("children")) ForEachFrame(frame["children"], visit);
    }
}

class Editor {
public:
    explicit Editor(const UiDescription& base) : document_(Json::parse(SerializeUiDescription(base))) {}

    std::vector<Diagnostic> diagnostics;

    bool Apply(const Json& edit, std::size_t step) {
        step_ = step;
        if (!edit.is_object() || !edit.contains("op") || !edit["op"].is_string()) return Fail("每一步须是带 op 的对象");
        const std::string op = edit["op"].get<std::string>();
        if (op == "set_description") return Keys(edit, {"op", "label"}) && SetDescription(edit);
        if (op == "add_frame") return Keys(edit, {"op", "parent", "index", "frame"}) && AddFrame(edit);
        if (op == "set_frame") return Keys(edit, {"op", "frame", "fields"}) && SetFrame(edit);
        if (op == "move_frame") return Keys(edit, {"op", "frame", "parent", "index"}) && MoveFrame(edit);
        if (op == "remove_frame") return Keys(edit, {"op", "frame"}) && RemoveFrame(edit);
        if (op == "rename_value") return Keys(edit, {"op", "from", "to"}) && RenameValue(edit);
        if (op == "set_values") return Keys(edit, {"op", "values"}) && Replace(edit, "values");
        if (op == "set_states") return Keys(edit, {"op", "states"}) && Replace(edit, "states");
        if (op == "set_events") return Keys(edit, {"op", "events"}) && Replace(edit, "events");
        return Fail("不认识的 op：" + op);
    }

    [[nodiscard]] std::string Text() const { return document_.dump(2, ' ', false, Json::error_handler_t::replace); }

private:
    bool Fail(const std::string& message) {
        diagnostics.push_back(detail::MakeUiError("AUI_EDIT", "第 " + std::to_string(step_ + 1) + " 步：" + message));
        return false;
    }

    bool Keys(const Json& edit, std::initializer_list<std::string_view> keys) {
        for (const auto& item : edit.items()) {
            if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) return Fail("不认识的字段 " + item.key());
        }
        return true;
    }

    Json& Frames() {
        if (!document_.contains("frames")) document_["frames"] = Json::array();
        return document_["frames"];
    }

    std::optional<Located> Frame(const Json& edit) {
        if (!edit.contains("frame") || !edit["frame"].is_string()) {
            Fail("缺少框架编号 frame");
            return std::nullopt;
        }
        auto found = FindFrame(Frames(), edit["frame"].get<std::string>());
        if (!found) Fail("框架 " + edit["frame"].get<std::string>() + " 不存在");
        return found;
    }

    // parent 省略或为 null 时是顶层框架列表。
    Json* Children(const Json& edit) {
        if (!edit.contains("parent") || edit["parent"].is_null()) return &Frames();
        if (!edit["parent"].is_string()) {
            Fail("parent 应为框架编号或 null");
            return nullptr;
        }
        const auto parent = FindFrame(Frames(), edit["parent"].get<std::string>());
        if (!parent) {
            Fail("父框架 " + edit["parent"].get<std::string>() + " 不存在");
            return nullptr;
        }
        Json& frame = parent->Frame();
        if (!frame.contains("children")) frame["children"] = Json::array();
        return &frame["children"];
    }

    std::optional<std::size_t> Index(const Json& edit, std::size_t size) {
        if (!edit.contains("index")) return size;
        const Json& index = edit["index"];
        if (!index.is_number_integer() || index.get<std::int64_t>() < 0 || index.get<std::int64_t>() > static_cast<std::int64_t>(size)) {
            Fail("index 应为 0 到 " + std::to_string(size) + " 的整数");
            return std::nullopt;
        }
        return static_cast<std::size_t>(index.get<std::int64_t>());
    }

    bool SetDescription(const Json& edit) {
        if (!edit.contains("label") || !edit["label"].is_string()) return Fail("label 应为文字");
        if (edit["label"].get<std::string>().empty()) document_.erase("label");
        else document_["label"] = edit["label"];
        return true;
    }

    bool AddFrame(const Json& edit) {
        if (!edit.contains("frame") || !edit["frame"].is_object()) return Fail("frame 应为框架对象");
        const Json& frame = edit["frame"];
        std::vector<std::string> ids;
        CollectIds(frame, ids);
        for (const auto& id : ids) {
            if (FindFrame(Frames(), id)) return Fail("框架编号 " + id + " 已存在");
        }
        Json* children = Children(edit);
        if (!children) return false;
        const auto index = Index(edit, children->size());
        if (!index) return false;
        children->insert(children->begin() + static_cast<std::ptrdiff_t>(*index), frame);
        return true;
    }

    bool SetFrame(const Json& edit) {
        const auto located = Frame(edit);
        if (!located) return false;
        if (!edit.contains("fields") || !edit["fields"].is_object()) return Fail("fields 应为对象");
        Json& frame = located->Frame();
        for (const auto& item : edit["fields"].items()) {
            const std::string& key = item.key();
            if (key == "children") return Fail("子框架用 add_frame、move_frame、remove_frame 修改");
            if (item.value().is_null()) {
                if (key == "id" || key == "type") return Fail(key + " 不能删除");
                frame.erase(key);
                continue;
            }
            if (key == "id") {
                if (!item.value().is_string()) return Fail("id 应为文字");
                const std::string from = frame["id"].get<std::string>();
                const std::string to = item.value().get<std::string>();
                if (to != from && FindFrame(Frames(), to)) return Fail("框架编号 " + to + " 已存在");
                Rename(from, to);
            }
            frame[key] = item.value();
        }
        return true;
    }

    // 改编号时同步锚点、状态覆盖与事件里的引用。
    void Rename(const std::string& from, const std::string& to) {
        ForEachFrame(Frames(), [&](Json& frame) {
            if (!frame.contains("anchors") || !frame["anchors"].is_array()) return;
            for (auto& anchor : frame["anchors"]) {
                if (anchor.is_object() && anchor.contains("to") && Is(anchor["to"], from)) anchor["to"] = to;
            }
        });
        if (document_.contains("states") && document_["states"].is_array()) {
            for (auto& state : document_["states"]) {
                if (!state.contains("set") || !state["set"].is_array()) continue;
                for (auto& entry : state["set"]) {
                    if (entry.contains("frame") && Is(entry["frame"], from)) entry["frame"] = to;
                }
            }
        }
        if (document_.contains("events") && document_["events"].is_array()) {
            for (auto& event : document_["events"]) {
                if (event.contains("frame") && Is(event["frame"], from)) event["frame"] = to;
            }
        }
    }

    bool MoveFrame(const Json& edit) {
        const auto located = Frame(edit);
        if (!located) return false;
        std::vector<std::string> subtree;
        CollectIds(located->Frame(), subtree);
        if (edit.contains("parent") && edit["parent"].is_string() &&
            std::find(subtree.begin(), subtree.end(), edit["parent"].get<std::string>()) != subtree.end()) {
            return Fail("不能移到自己或自己的子框架下");
        }
        Json frame = located->Frame();
        located->array->erase(located->array->begin() + static_cast<std::ptrdiff_t>(located->index));
        Json* children = Children(edit);
        if (!children) return false;
        const auto index = Index(edit, children->size());
        if (!index) return false;
        children->insert(children->begin() + static_cast<std::ptrdiff_t>(*index), std::move(frame));
        return true;
    }

    bool RemoveFrame(const Json& edit) {
        const auto located = Frame(edit);
        if (!located) return false;
        std::vector<std::string> removed;
        CollectIds(located->Frame(), removed);
        located->array->erase(located->array->begin() + static_cast<std::ptrdiff_t>(located->index));
        const auto gone = [&](const Json& item) {
            return item.contains("frame") && item["frame"].is_string() &&
                   std::find(removed.begin(), removed.end(), item["frame"].get<std::string>()) != removed.end();
        };
        if (document_.contains("states") && document_["states"].is_array()) {
            for (auto& state : document_["states"]) {
                if (!state.contains("set") || !state["set"].is_array()) continue;
                auto& set = state["set"];
                set.erase(std::remove_if(set.begin(), set.end(), gone), set.end());
                if (set.empty()) state.erase("set");
            }
        }
        if (document_.contains("events") && document_["events"].is_array()) {
            auto& events = document_["events"];
            events.erase(std::remove_if(events.begin(), events.end(), gone), events.end());
            if (events.empty()) document_.erase("events");
        }
        return true;
    }

    // 改数值编号：同步框架的取值（进度、显示）与文字模板（框架文字、状态覆盖文字）。
    bool RenameValue(const Json& edit) {
        if (!edit.contains("from") || !edit["from"].is_string() || !edit.contains("to") || !edit["to"].is_string()) {
            return Fail("from、to 应为数值编号");
        }
        const std::string from = edit["from"].get<std::string>();
        const std::string to = edit["to"].get<std::string>();
        if (!IsValidUiId(to)) return Fail(to + " 不是合法编号");
        Json* found = nullptr;
        if (document_.contains("values") && document_["values"].is_array()) {
            for (auto& value : document_["values"]) {
                if (!value.is_object() || !value.contains("id")) continue;
                if (Is(value["id"], from)) found = &value;
                else if (Is(value["id"], to)) return Fail("数值编号 " + to + " 已存在");
            }
        }
        if (!found) return Fail("数值 " + from + " 不存在");
        (*found)["id"] = to;
        const auto rebind = [&](Json& binding) {
            Json& reference = binding.is_object() && binding.contains("value") ? binding["value"] : binding;
            if (!reference.is_string()) return;
            const std::string text = reference.get<std::string>();
            const auto parsed = detail::ParseUiValueReference(text);
            if (parsed && parsed->id == from) reference = to + text.substr(from.size());
        };
        const auto retext = [&](Json& holder) {
            if (!holder.contains("text") || !holder["text"].is_string()) return;
            if (auto text = RenameInTemplate(holder["text"].get_ref<const std::string&>(), from, to)) holder["text"] = std::move(*text);
        };
        ForEachFrame(Frames(), [&](Json& frame) {
            if (frame.contains("value")) rebind(frame["value"]);
            if (frame.contains("visible")) rebind(frame["visible"]);
            retext(frame);
        });
        if (document_.contains("states") && document_["states"].is_array()) {
            for (auto& state : document_["states"]) {
                if (!state.contains("set") || !state["set"].is_array()) continue;
                for (auto& entry : state["set"]) retext(entry);
            }
        }
        return true;
    }

    // 模板里取到 from 的地方换成 to；没有可换的或模板本身不合法（由读取器报错）时为空。
    static std::optional<std::string> RenameInTemplate(std::string_view text, const std::string& from, const std::string& to) {
        const auto pieces = detail::ParseUiTextTemplate(text, nullptr);
        if (!pieces) return std::nullopt;
        bool changed = false;
        std::string result;
        for (const auto& piece : *pieces) {
            if (!piece.reference) {
                for (const char c : piece.literal) {
                    if (c == '{' || c == '}') result.push_back(c);
                    result.push_back(c);
                }
                continue;
            }
            const bool hit = piece.reference->id == from;
            changed = changed || hit;
            result += '{';
            result += hit ? to : piece.reference->id;
            if (piece.reference->perInstance) result += "[]";
            else if (piece.reference->index) result += "[" + std::to_string(*piece.reference->index) + "]";
            if (piece.decimals) result += ":" + std::to_string(*piece.decimals);
            result += '}';
        }
        if (!changed) return std::nullopt;
        return result;
    }

    bool Replace(const Json& edit, const char* key) {
        if (!edit.contains(key) || !edit[key].is_array()) return Fail(std::string(key) + " 应为数组");
        if (edit[key].empty()) document_.erase(key);
        else document_[key] = edit[key];
        return true;
    }

    Json document_;
    std::size_t step_ = 0;
};

} // namespace

Result<UiDescription> ApplyUiEdits(const UiDescription& base, std::string_view edits) {
    Result<UiDescription> result;
    const auto bytes = std::as_bytes(std::span(edits.data(), edits.size()));
    Serialization::StrictJsonLimits limits;
    limits.maxBytes = 2 * 1024 * 1024;
    limits.maxDepth = 2 * detail::kMaxDepth + 8;
    if (!Serialization::ParseStrictJson(bytes, limits).value) {
        result.diagnostics.push_back(detail::MakeUiError("AUI_EDIT", "编辑不是有效的 JSON（语法、重复字段或过大）"));
        return result;
    }
    const Json list = Json::parse(edits.begin(), edits.end());
    if (!list.is_array() || list.empty() || list.size() > kMaxEdits) {
        result.diagnostics.push_back(detail::MakeUiError("AUI_EDIT", "编辑应为 1 到 " + std::to_string(kMaxEdits) + " 步的数组"));
        return result;
    }
    Editor editor(base);
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!editor.Apply(list[i], i)) {
            result.diagnostics = std::move(editor.diagnostics);
            return result;
        }
    }
    return ParseUiDescription(editor.Text(), UiDescriptionMemberPath(base.id));
}

Result<UiDescription> NewUiDescription(std::string_view id, std::string_view label) {
    UiDescription description;
    description.id = std::string(id);
    description.label = std::string(label);
    Result<UiDescription> result;
    result.diagnostics = ValidateUiDescription(description);
    if (result.diagnostics.empty()) result.value = std::move(description);
    return result;
}

std::string NextUiFrameId(const UiDescription& description, std::string_view prefix) {
    for (std::uint32_t n = 1;; ++n) {
        std::string id = std::string(prefix) + std::to_string(n);
        if (!FindUiFrame(description, id)) return id;
    }
}

} // namespace War3Map::GameUi
