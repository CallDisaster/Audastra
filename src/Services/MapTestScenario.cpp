#include "War3MapCore/Services/MapTestScenario.h"

#include "TestTriggerCopy.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace War3Map::Services {
namespace {
constexpr char kSubsystem[] = "Map.TestScenario";
constexpr std::size_t kActionBytes = 64 * 1024;
// 结果文件里的标记行：读值行为“@audastra-read 序号 值”，场景自己记录的行不会以 @ 开头。
constexpr std::string_view kReadMarker = "@audastra-read ";
constexpr std::string_view kDoneMarker = "@audastra-done";
constexpr std::string_view kStartedMarker = "@audastra-started";

template<class T> Result<T> Fail(const char* code, std::string text) {
    return Result<T>::Failure(kSubsystem, code, std::move(text));
}

// 只允许游戏目录内的普通相对路径：字母数字与 _-. 组成的段，反斜杠分隔，不能回到上级。
bool SafeResultFile(std::string_view path) {
    if (path.empty() || path.size() > 160 || path.front() == '\\' || path.back() == '\\') return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('\\', start);
        const auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == ".." || !std::all_of(part.begin(), part.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            }))
            return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

std::string JassString(std::string_view text) {
    std::string output = "\"";
    for (const char c : text) {
        if (c == '\\' || c == '"') output += '\\';
        output += c;
    }
    return output + "\"";
}

// 读取值写进一行的自然 T：数值转文字后拼接，文字直接拼接，布尔分两支。
std::string ReadStatement(std::size_t index, const detail::TestRead& read) {
    const auto variable = "udg_" + read.name + (read.index.empty() ? "" : "[" + read.index + "]");
    const auto head = std::string(kReadMarker) + std::to_string(index) + " ";
    const auto& type = read.variable->typeName.text;
    const auto line = [&](const std::string& value) {
        return "    Preload(OperatorString(" + Trigger::TriggerTextQuoted(head) + ", " + value + "))\n";
    };
    if (type == "boolean")
        return "    IfThenElseMultiple() {\n        if:\n            OperatorCompareBoolean(" + variable + ", OperatorEqualENE, true)\n"
            "        then:\n        " + line(Trigger::TriggerTextQuoted("true")) + "        else:\n        "
            + line(Trigger::TriggerTextQuoted("false")) + "    }\n";
    if (type == "integer") return line("I2S(" + variable + ")");
    if (type == "real") return line("R2S(" + variable + ")");
    return line(variable);
}

// 预载文件的开始与写出是编辑器给测试副本加的固定框架（自然 T 没有对应动作）：直接插进场景触发器，
// 不经过 Agent 的编辑批次；Agent 写的动作与读值都是上面的自然 T。
Status AddRecordingLine(Trigger::TriggerDocument& copy, const Trigger::UiCatalog& catalog, StableId trigger, std::string code,
    std::optional<std::size_t> row) {
    const auto appended = Trigger::TriggerDocumentEditor::AppendRootEca(copy, catalog, trigger, Trigger::UiDefinitionKind::Action,
        "CustomScriptCode", row);
    if (!appended.Succeeded() || !appended.value) return {appended.diagnostics};
    if (appended.value->appendedArguments.empty())
        return Status::Failure(kSubsystem, "MAP_TEST_SCENARIO_RECORDING", "函数库的“自定义代码”动作没有参数，无法写出测试结果。");
    return Trigger::TriggerDocumentEditor::SetScalarArgument(copy, appended.value->appendedArguments.front().id, 3, std::move(code));
}

// 游戏写预载文件时把反斜杠写成两个，引号原样写出。
std::string PreloadText(std::string_view content) {
    std::string text;
    text.reserve(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) {
        text.push_back(content[i]);
        if (content[i] == '\\' && i + 1 < content.size() && content[i + 1] == '\\') ++i;
    }
    return text;
}

// 记录期间游戏把载入的资源路径也写成预载行：不含空白、带目录分隔、以 2–4 位扩展名结尾的 ASCII 行按资源路径归类。
bool LoadedFilePath(std::string_view text) {
    if (!std::all_of(text.begin(), text.end(), [](char c) { return c > ' ' && c < 0x7f; })) return false;
    const auto slash = text.find_last_of("\\/"), dot = text.find_last_of('.');
    if (slash == std::string_view::npos || dot == std::string_view::npos || dot < slash || dot + 3 > text.size() || dot + 5 < text.size())
        return false;
    return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(dot + 1), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

std::string Indented(std::string_view text) {
    std::string output;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        output += "    ";
        output += line;
        output += '\n';
        start = end + 1;
    }
    return output;
}
}

Result<MapTestScenarioPlan> AddMapTestScenarioTrigger(Trigger::TriggerDocument& copy, const Trigger::UiCatalog& catalog,
    const MapTestScenarioRequest& request, const Trigger::MapObjectReferenceSnapshot* mapObjects) {
    if (request.actions.empty() || request.actions.size() > kActionBytes)
        return Fail<MapTestScenarioPlan>("MAP_TEST_SCENARIO_ACTIONS", "actions 须是 1..65536 字节的紧凑 T 文本。");
    if (!std::isfinite(request.startSeconds) || request.startSeconds < 0.0 || request.startSeconds > 600.0)
        return Fail<MapTestScenarioPlan>("MAP_TEST_SCENARIO_START", "start_seconds 须在 0..600 之间。");
    if (!SafeResultFile(request.resultFile))
        return Fail<MapTestScenarioPlan>("MAP_TEST_SCENARIO_RESULT_FILE", "结果文件须是游戏目录内的普通相对路径。");
    // 先单独解析一次，让错误行列对应场景原文而不是拼好的触发器。
    Trigger::TriggerTextTarget target;
    const auto parsed = Trigger::ParseTriggerText(request.actions, copy, catalog, target);
    if (!parsed.Succeeded() || !parsed.value) return {std::nullopt, parsed.diagnostics};
    const auto reads = detail::ResolveTestReads(copy, request.reads, kSubsystem, "MAP_TEST_SCENARIO");
    if (!reads.Succeeded() || !reads.value) return {std::nullopt, reads.diagnostics};
    for (const auto& read : *reads.value) {
        const auto& type = read.variable->typeName.text;
        if (type != "integer" && type != "real" && type != "string" && type != "boolean")
            return Fail<MapTestScenarioPlan>("MAP_TEST_SCENARIO_READ_TYPE", "测试场景只能读整数、实数、文字、布尔变量；" + read.request
                + " 是 " + type + "，请在场景里用条件判断后记录“通过”或“失败”。");
    }
    if (!catalog.Find(Trigger::UiDefinitionKind::Action, "CustomScriptCode"))
        return Fail<MapTestScenarioPlan>("MAP_TEST_SCENARIO_RECORDING", "函数库没有“自定义代码”动作，无法写出测试结果。");

    MapTestScenarioPlan plan;
    plan.resultFile = request.resultFile;
    char start[32];
    std::snprintf(start, sizeof(start), "%.2f", request.startSeconds);
    std::string text = "events:\n    TriggerRegisterTimerEventSingle(" + std::string(start) + ")\nactions:\n";
    text += Indented(request.actions);
    for (std::size_t index = 0; index < reads.value->size(); ++index) {
        const auto& read = (*reads.value)[index];
        plan.reads.emplace_back(read.request, "udg_" + read.name + (read.index.empty() ? "" : "[" + read.index + "]"));
        text += ReadStatement(index, read);
    }
    text += "    Preload(" + Trigger::TriggerTextQuoted(kDoneMarker) + ")\n";
    const auto trigger = detail::AddTestTrigger(copy, catalog, kMapTestScenarioCategory, kMapTestScenarioTrigger,
        "agent-scenario", text, kSubsystem, "MAP_TEST_SCENARIO", mapObjects);
    if (!trigger.Succeeded() || !trigger.value) return {std::nullopt, trigger.diagnostics};
    plan.triggerId = *trigger.value;
    // 场景触发器根部第 0 行是计时事件，其后是动作。动作前先单独写出一次只有开始标记的结果文件，
    // 宿主据此知道游戏已载入、场景已开始；再重新开始记录（最后写出会覆盖整份文件，开始标记再记一次），写出文件追加在最后。
    const std::string writeResult = "call PreloadGenEnd(" + JassString(request.resultFile) + ")";
    const std::string started = "call Preload(" + JassString(kStartedMarker) + ")";
    for (auto [code, row] : {std::pair{std::string("call PreloadGenClear()"), std::optional<std::size_t>(1)},
             std::pair{std::string("call PreloadGenStart()"), std::optional<std::size_t>(2)},
             std::pair{started, std::optional<std::size_t>(3)}, std::pair{writeResult, std::optional<std::size_t>(4)},
             std::pair{std::string("call PreloadGenClear()"), std::optional<std::size_t>(5)},
             std::pair{std::string("call PreloadGenStart()"), std::optional<std::size_t>(6)},
             std::pair{started, std::optional<std::size_t>(7)}, std::pair{writeResult, std::optional<std::size_t>()}}) {
        if (auto added = AddRecordingLine(copy, catalog, plan.triggerId, std::move(code), row); !added.Succeeded())
            return {std::nullopt, added.diagnostics};
    }
    return Result<MapTestScenarioPlan>::Success(std::move(plan));
}

Result<MapTestScenarioReport> ParseMapTestScenarioResult(std::string_view text, const MapTestScenarioPlan& plan) {
    // 游戏把每次 Preload 写成一行 call Preload( "内容" )。
    constexpr std::string_view open = "call Preload( \"", close = "\" )";
    MapTestScenarioReport report;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (!line.starts_with(open) || !line.ends_with(close) || line.size() < open.size() + close.size()) continue;
        const auto decoded = PreloadText(line.substr(open.size(), line.size() - open.size() - close.size()));
        const std::string_view content = decoded;
        if (content == kDoneMarker) { report.finished = true; continue; }
        if (content == kStartedMarker) { report.started = true; continue; }
        if (content.starts_with(kReadMarker)) {
            const auto rest = content.substr(kReadMarker.size());
            const auto space = rest.find(' ');
            if (space == std::string_view::npos || space == 0 || space > 3) continue;
            std::size_t index = 0;
            if (!std::all_of(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(space), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            for (std::size_t i = 0; i < space; ++i) index = index * 10 + static_cast<std::size_t>(rest[i] - '0');
            if (index < plan.reads.size()) report.reads.emplace_back(plan.reads[index].first, std::string(rest.substr(space + 1)));
            continue;
        }
        if (report.lines.size() + report.files.size() >= 4096) continue;
        if (LoadedFilePath(content)) { report.files.emplace_back(content); continue; }
        if (content.starts_with("通过")) ++report.passed;
        else if (content.starts_with("失败")) ++report.failed;
        report.lines.emplace_back(content);
    }
    return Result<MapTestScenarioReport>::Success(std::move(report));
}

} // namespace War3Map::Services
