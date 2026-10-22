#include "War3MapCore/Trigger/TriggerFeatureText.h"

#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <charconv>
#include <set>

namespace War3Map::Trigger {
namespace {

constexpr const char* kDomain = "Trigger.Feature";

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

std::string At(std::size_t line) { return "第 " + std::to_string(line) + " 行"; }

// 顶层名字：可用双引号括起（转义同紧凑 T），否则取原文去首尾空格。
std::optional<std::string> HeaderName(std::string_view text, std::string& error) {
    text = Trim(text);
    if (text.empty()) { error = "缺少名字。"; return std::nullopt; }
    if (text.front() != '"') return std::string(text);
    auto value = ParseTriggerTextValue(text);
    if (!value.Succeeded() || !value.value) { error = "名字的引号写法无效。"; return std::nullopt; }
    return *value.value;
}

bool NeedsQuotes(std::string_view name) {
    return name.empty() || name.front() == ' ' || name.back() == ' ' || name.front() == '"'
        || name.find_first_of(":()\n\r\t") != std::string_view::npos;
}

std::string HeaderText(std::string_view name) { return NeedsQuotes(name) ? TriggerTextQuoted(name) : std::string(name); }

class Parser {
    enum class Section { None, Variables, Trigger };
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<FeatureText> Run() {
        Section section = Section::None;
        std::size_t lineNumber = 0;
        for (std::size_t start = 0; start <= text_.size();) {
            const auto end = std::min(text_.find('\n', start), text_.size());
            const auto line = text_.substr(start, end - start);
            ++lineNumber;
            const auto content = Trim(line);
            const bool indented = !line.empty() && (line.front() == ' ' || line.front() == '\t');
            if (content.empty() || content.starts_with("//")) {
                if (section == Section::Trigger) Body(line);
            } else if (!indented) {
                section = Header(content, lineNumber);
            } else if (section == Section::Variables) {
                Variable(content, lineNumber);
            } else if (section == Section::Trigger) {
                Body(line);
            } else {
                Error(lineNumber, "缩进内容前须有 变量: 或 触发器 名字: 的标题行。");
            }
            if (end == text_.size()) break;
            start = end + 1;
        }
        if (!errors_.empty()) return {std::nullopt, std::move(errors_)};
        if (result_.variables.empty() && result_.triggers.empty())
            return Result<FeatureText>::Failure(kDomain, "TRIGGER_FEATURE_EMPTY", "功能文本里没有变量或触发器。");
        return Result<FeatureText>::Success(std::move(result_));
    }

private:
    void Error(std::size_t line, std::string message) {
        if (errors_.size() < 64) errors_.push_back({DiagnosticSeverity::Error, kDomain, "TRIGGER_FEATURE_SYNTAX", At(line) + "：" + std::move(message), {}});
    }

    // 触发器体按原行保留（前面补空行），解析紧凑 T 时行列即整段文本的行列。
    void Body(std::string_view line) {
        auto& body = result_.triggers.back().body;
        body.append(line);
        body += '\n';
    }

    Section Header(std::string_view content, std::size_t line) {
        if (content == "variables:" || content == "变量:" || content == "变量：") return Section::Variables;
        std::string_view rest;
        if (content.starts_with("trigger ")) rest = content.substr(8);
        else if (content.starts_with("触发器 ")) rest = content.substr(std::string_view("触发器 ").size());
        else { Error(line, "顶层只能写 变量: 或 触发器 名字:。"); return Section::None; }
        rest = Trim(rest);
        if (rest.ends_with("：")) rest.remove_suffix(std::string_view("：").size());
        else if (rest.ends_with(':')) rest.remove_suffix(1);
        else { Error(line, "触发器标题须以冒号结尾。"); return Section::None; }
        FeatureTrigger trigger;
        trigger.line = line;
        rest = Trim(rest);
        if (rest.ends_with(')') && !(rest.front() == '"' && rest.find('"', 1) == rest.size() - 1)) {
            const auto open = rest.rfind('(');
            if (open == std::string_view::npos) { Error(line, "选项缺少左括号。"); return Section::None; }
            auto options = rest.substr(open + 1, rest.size() - open - 2);
            rest = Trim(rest.substr(0, open));
            while (!options.empty()) {
                const auto comma = options.find(',');
                const auto option = Trim(options.substr(0, comma));
                if (option == "init" || option == "初始化") trigger.runOnInit = true;
                else if (option == "off" || option == "关闭") trigger.initiallyOn = false;
                else if (option == "disabled" || option == "禁用") trigger.enabled = false;
                else if (!option.empty()) Error(line, "未知触发器选项“" + std::string(option) + "”（可用 init/off/disabled）。");
                if (comma == std::string_view::npos) break;
                options.remove_prefix(comma + 1);
            }
        }
        std::string error;
        const auto name = HeaderName(rest, error);
        if (!name) { Error(line, "触发器" + error); return Section::None; }
        trigger.name = *name;
        for (const auto& existing : result_.triggers)
            if (existing.name == trigger.name) Error(line, "触发器“" + trigger.name + "”重复出现。");
        trigger.body.assign(line, '\n');
        result_.triggers.push_back(std::move(trigger));
        return Section::Trigger;
    }

    void Variable(std::string_view content, std::size_t line) {
        if (content.find("//") != std::string_view::npos && content.find('"') == std::string_view::npos)
            content = Trim(content.substr(0, content.find("//")));
        const auto colon = content.find(':');
        if (colon == std::string_view::npos) { Error(line, "变量写成 名字: 类型 [= 初值]。"); return; }
        FeatureVariable variable;
        variable.line = line;
        std::string error;
        const auto name = HeaderName(content.substr(0, colon), error);
        if (!name) { Error(line, "变量" + error); return; }
        variable.name = name->starts_with("udg_") ? name->substr(4) : *name;
        auto rest = Trim(content.substr(colon + 1));
        const auto equals = rest.find('=');
        auto type = Trim(rest.substr(0, equals));
        if (equals != std::string_view::npos) {
            auto value = ParseTriggerTextValue(Trim(rest.substr(equals + 1)));
            if (!value.Succeeded() || !value.value) { Error(line, "变量“" + variable.name + "”的初值只能写一个值（\"文本\"、'A000'、数字或名字）。"); return; }
            variable.initial = std::move(*value.value);
        }
        if (type.ends_with(']')) {
            const auto open = type.find('[');
            if (open == std::string_view::npos) { Error(line, "数组类型写成 类型[] 或 类型[长度]。"); return; }
            const auto size = Trim(type.substr(open + 1, type.size() - open - 2));
            variable.isArray = true;
            if (!size.empty()) {
                const auto parsed = std::from_chars(size.data(), size.data() + size.size(), variable.size);
                if (parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() || variable.size <= 0) {
                    Error(line, "数组长度须为正整数。"); return;
                }
            }
            type = Trim(type.substr(0, open));
        }
        if (type.empty() || type.find_first_of(" \t") != std::string_view::npos) { Error(line, "变量类型无效。"); return; }
        variable.type = std::string(type);
        for (const auto& existing : result_.variables)
            if (existing.name == variable.name) Error(line, "变量“" + variable.name + "”重复声明。");
        result_.variables.push_back(std::move(variable));
    }

    std::string_view text_;
    FeatureText result_;
    std::vector<Diagnostic> errors_;
};

// 触发器（含参数中的调用与数组下标）引用到的全局变量名。
void CollectVariables(const TriggerDocument& document, std::uint32_t eca, std::set<std::string>& names, std::size_t depth = 0) {
    if (eca >= document.ecaNodes.size() || depth > 256) return;
    const auto& node = document.ecaNodes[eca];
    const auto argument = [&](auto&& self, std::uint32_t index) -> void {
        if (index >= document.argumentNodes.size()) return;
        const auto& value = document.argumentNodes[index];
        if (value.type == 1) names.insert(value.value.text.starts_with("udg_") ? value.value.text.substr(4) : value.value.text);
        if (value.type == 2 && value.callEca) CollectVariables(document, *value.callEca, names, depth + 1);
        if (value.indexArgument) self(self, *value.indexArgument);
    };
    for (const auto index : node.arguments) argument(argument, index);
    for (const auto child : node.children) CollectVariables(document, child, names, depth + 1);
}

} // namespace

Result<FeatureText> ParseFeatureText(std::string_view text) {
    return Parser(text).Run();
}

Result<std::string> RenderFeatureText(const TriggerDocument& document, const UiCatalog& catalog,
    std::int32_t categoryId, std::size_t maxBytes, const MapObjectReferenceSnapshot* mapObjects) {
    if (std::none_of(document.categories.begin(), document.categories.end(), [&](const auto& row) { return row.categoryId == categoryId; }))
        return Result<std::string>::Failure(kDomain, "TRIGGER_FEATURE_CATEGORY_MISSING", "找不到类别 " + std::to_string(categoryId) + "。");
    std::set<std::string> referenced;
    for (const auto& trigger : document.triggers) {
        if (trigger.categoryId != categoryId) continue;
        for (const auto eca : trigger.ecas) CollectVariables(document, eca, referenced);
    }
    TriggerTypeSystem types(catalog);
    std::string out;
    bool any = false;
    for (const auto& variable : document.variables) {
        if (!referenced.contains(variable.name.text)) continue;
        if (!any) { out += "变量:\n"; any = true; }
        out += "    " + HeaderText(variable.name.text) + ": " + variable.typeName.text;
        if (variable.isArray != 0) out += variable.size > 1 ? "[" + std::to_string(variable.size) + "]" : "[]";
        if (variable.hasDefault != 0) {
            const auto& value = variable.value.text;
            const auto kind = types.LiteralKind(variable.typeName.text);
            out += " = ";
            if (kind == TriggerTypeSystem::Literal::String) out += TriggerTextQuoted(value);
            else if (kind == TriggerTypeSystem::Literal::ObjectCode && TriggerTypeSystem::IsObjectCodeLiteral(value)) out += "'" + value + "'";
            else if (const auto plain = ParseTriggerTextValue(value); plain.Succeeded() && *plain.value == value) out += value;
            else if (!value.empty()) out += TriggerTextName(value);
            else return Result<std::string>::Failure(kDomain, "TRIGGER_FEATURE_RENDER", "变量“" + variable.name.text + "”的空初值无法写成功能文本。");
        }
        out += '\n';
    }
    for (const auto& trigger : document.triggers) {
        if (trigger.categoryId != categoryId) continue;
        auto body = RenderTriggerText(document, catalog, trigger.id, maxBytes, nullptr, mapObjects);
        if (!body.Succeeded() || !body.value) return {std::nullopt, std::move(body.diagnostics)};
        std::vector<std::string> options;
        if (trigger.runOnInit != 0) options.emplace_back("init");
        if (trigger.initiallyClosed != 0) options.emplace_back("off");
        if (trigger.enabled == 0) options.emplace_back("disabled");
        out += "触发器 " + HeaderText(trigger.name.text);
        if (!options.empty()) {
            out += " (";
            for (std::size_t i = 0; i < options.size(); ++i) out += (i ? ", " : "") + options[i];
            out += ")";
        }
        out += ":\n";
        for (std::size_t start = 0; start < body.value->size();) {
            const auto end = body.value->find('\n', start);
            out += "    " + body.value->substr(start, end - start) + "\n";
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (out.size() > maxBytes)
            return Result<std::string>::Failure(kDomain, "TRIGGER_FEATURE_TOO_LARGE", "导出文本超过 " + std::to_string(maxBytes) + " 字节上限。");
    }
    if (out.empty()) return Result<std::string>::Failure(kDomain, "TRIGGER_FEATURE_EMPTY", "该类别没有触发器。");
    return Result<std::string>::Success(std::move(out));
}

} // namespace War3Map::Trigger
