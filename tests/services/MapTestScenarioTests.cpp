#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Services/MapTestScenario.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// Agent 测试场景：紧凑 T 动作编进测试副本的临时触发器，生成的 JASS 用游戏预载文件写出记录与读取；
// 游戏写回的文件按标记分出断言、读取与结束。
namespace {
using namespace War3Map;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << text;
    Require(static_cast<bool>(stream), "fixture write failed");
}
bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

struct Fixture {
    std::filesystem::path root;
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument document;
    Fixture() {
        root = std::filesystem::temp_directory_path() / ("audastra-scenario-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(root);
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer\nreal=0,1,1,Real\nboolean=0,1,1,Boolean\n"
            "string=0,1,1,String\nunit=0,1,1,Unit\nanyfile=0,0,0,File,string\nscriptcode=0,0,0,Script,string\n"
            "EqualNotEqualOperator=0,0,0,EqualNotEqual\n[TriggerParams]\nOperatorEqualENE=0,EqualNotEqualOperator,==,Equal\n");
        Write(root / "ui/base/event.txt", "[TriggerRegisterTimerEventSingle]\n[[.args]]\ntype=real\n");
        Write(root / "ui/base/condition.txt", "[OperatorCompareBoolean]\nreturns=boolean\n[[.args]]\ntype=boolean\n"
            "[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=boolean\n");
        Write(root / "ui/base/action.txt", "[Record]\n[[.args]]\ntype=integer\n[Preload]\n[[.args]]\ntype=anyfile\n"
            "[CustomScriptCode]\n[[.args]]\ntype=scriptcode\n[IfThenElseMultiple]\n");
        Write(root / "ui/base/call.txt", "[I2S]\nreturns=string\n[[.args]]\ntype=integer\n[R2S]\nreturns=string\n[[.args]]\ntype=real\n"
            "[OperatorString]\nreturns=string\n[[.args]]\ntype=string\n[[.args]]\ntype=string\n");
        auto loaded = Trigger::UiCatalog::Load(root);
        Require(loaded.Succeeded() && loaded.value, "fixture catalog");
        catalog = std::move(*loaded.value);
        // 图编辑只接受真实 codec 读入的文档：按作者文档的样子建一份再经编码往返。
        auto empty = Trigger::WtgWctCodec{}.CreateEmpty(catalog);
        Require(empty.Succeeded() && empty.value, "sealed empty document");
        auto sealed = std::move(*empty.value);
        const auto category = Trigger::TriggerDirectoryEditor::AddCategory(sealed, "Author");
        Require(category.Succeeded() && Trigger::TriggerDocumentEditor::AddTrigger(sealed, *category.value, "Original").Succeeded(), "author trigger");
        for (const auto& [name, type] : {std::pair{"Count", "integer"}, {"Flag", "boolean"}, {"Name", "string"}, {"Rate", "real"}, {"Hero", "unit"}})
            Require(Trigger::TriggerDocumentEditor::AddVariable(sealed, name, type).Succeeded(), "author variable");
        Require(Trigger::TriggerDocumentEditor::AddVariable(sealed, "Picks", "integer", true).Succeeded(), "author array");
        const auto encoded = Trigger::WtgWctCodec{}.Serialize(sealed, catalog);
        Require(encoded.Succeeded() && encoded.value, "author fixture encoded");
        const auto parsed = Trigger::WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog);
        Require(parsed.Succeeded() && parsed.value, "author fixture sealed");
        document = *parsed.value;
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

Services::MapTestScenarioRequest Request(std::string actions, std::vector<std::string> reads = {}) {
    Services::MapTestScenarioRequest request;
    request.actions = std::move(actions); request.reads = std::move(reads);
    request.startSeconds = 2.5; request.resultFile = "Maps\\Audastra\\scenario-test.txt";
    return request;
}

void GeneratesPreloadScenario() {
    Fixture fixture;
    auto copy = fixture.document;
    const auto plan = Services::AddMapTestScenarioTrigger(copy, fixture.catalog,
        Request("Record(3)\nPreload(\"通过: 记录\")", {"Count", "udg_Flag", "Name", "Rate"}));
    Require(plan.Succeeded() && plan.value, plan.Succeeded() ? "scenario plan missing" : plan.diagnostics.front().message.c_str());
    Require(fixture.document.triggers.size() == 1 && fixture.document.categories.size() == 1, "scenario changed the author document");
    Require(plan.value->reads.size() == 4 && plan.value->reads[1].first == "udg_Flag" && plan.value->reads[1].second == "udg_Flag", "read plan");
    Script::MapScriptInputs inputs; inputs.triggers = &copy; inputs.catalog = &fixture.catalog;
    const auto source = Script::GenerateGuiTriggerSource(inputs, plan.value->triggerId);
    Require(source.Succeeded() && source.value, source.Succeeded() ? "scenario source missing" : source.diagnostics.front().message.c_str());
    const auto& text = source.value->text;
    Require(Contains(text, "TriggerRegisterTimerEventSingle(") && Contains(text, "2.50"), "start time event");
    const std::string_view write = "call PreloadGenEnd(\"Maps\\\\Audastra\\\\scenario-test.txt\")";
    const auto start = text.find("call PreloadGenStart()"), started = text.find("call Preload(\"@audastra-started\")"),
        early = text.find(write), restart = text.find("call PreloadGenStart()", early), record = text.find("call Record(3)"),
        end = text.find(write, record), again = text.find("call Preload(\"@audastra-started\")", restart);
    Require(text.find("call PreloadGenClear()") < start && start < started && started < early && early < restart
            && restart < again && again < record && record < end && end != std::string::npos,
        ("started marker is written first, then recording wraps the author actions:\n" + text).c_str());
    Require(Contains(text, "call Preload(\"通过: 记录\")"), "author record line");
    Require(Contains(text, "\"@audastra-read 0 \" + I2S(udg_Count)") && Contains(text, "\"@audastra-read 1 \" + \"true\"")
        && Contains(text, "\"@audastra-read 1 \" + \"false\"") && Contains(text, "\"@audastra-read 2 \" + udg_Name")
        && Contains(text, "\"@audastra-read 3 \" + R2S(udg_Rate)"), ("reads are written by type:\n" + text).c_str());
    Require(Contains(text, "call Preload(\"@audastra-done\")") && text.find("@audastra-done") < end, "done marker before writing");
}

void RejectsUnsafeInput() {
    Fixture fixture;
    const auto code = [&](Services::MapTestScenarioRequest request) {
        auto copy = fixture.document;
        const auto result = Services::AddMapTestScenarioTrigger(copy, fixture.catalog, request);
        Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "unsafe scenario accepted");
        return result.diagnostics.front().code;
    };
    auto request = Request("Record(1)");
    for (const auto* path : {"..\\x.txt", "C:\\x.txt", "Maps\\..\\x.txt", "Maps/x.txt", "", "\\x.txt"}) {
        request.resultFile = path;
        Require(code(request) == "MAP_TEST_SCENARIO_RESULT_FILE", "unsafe result path accepted");
    }
    request = Request("Record(1)"); request.startSeconds = 700;
    Require(code(request) == "MAP_TEST_SCENARIO_START", "start time out of range accepted");
    Require(code(Request("")) == "MAP_TEST_SCENARIO_ACTIONS", "empty actions accepted");
    Require(code(Request("Record(1)", {"Nope"})) == "MAP_TEST_SCENARIO_READ_UNKNOWN", "unknown read accepted");
    Require(code(Request("Record(1)", {"Count[1]"})) == "MAP_TEST_SCENARIO_READS", "index on scalar accepted");
    Require(code(Request("Record(1)\nMissing()")) == "TRIGGER_TEXT_UNKNOWN_FUNCTION", "unknown function accepted");
    Require(code(Request("Record(1)", {"Hero"})) == "MAP_TEST_SCENARIO_READ_TYPE", "handle read accepted");
    Require(code(Request("Record(1)", {"Picks[8192]"})) == "MAP_TEST_SCENARIO_READS", "index past the JASS array accepted");
    auto arrays = fixture.document;
    // 数组变量的大小只是初始化个数，下标可到 JASS 数组上限。
    Require(Services::AddMapTestScenarioTrigger(arrays, fixture.catalog, Request("Record(1)", {"Picks[9]"})).Succeeded(),
        "index past the initialized size rejected");
    Require(code(Request("CustomScriptCode(\"call Record(1)\")")) == "TRIGGER_GRAPH_CODE_UNSUPPORTED", "agent code accepted");
    auto copy = fixture.document;
    Require(Services::AddMapTestScenarioTrigger(copy, fixture.catalog, Request("Record(1)")).Succeeded(), "first scenario");
    Require(!Services::AddMapTestScenarioTrigger(copy, fixture.catalog, Request("Record(1)")).Succeeded(), "second scenario in one copy");
}

void ParsesGameOutput() {
    Services::MapTestScenarioPlan plan;
    plan.reads = {{"Count", "udg_Count"}, {"Flag", "udg_Flag"}};
    const std::string text = "function PreloadFiles takes nothing returns nothing\r\n\r\n\tcall PreloadStart()\r\n"
        "\tcall Preload( \"通过: 记录\" )\r\n\tcall Preload( \"war3mapImported\\\\x.mdx \"q\"\" )\r\n"
        "\tcall Preload( \"Units\\\\Orc\\\\KotoBeast\\\\KodoDrum1.wav\" )\r\n"
        "\tcall Preload( \"失败: 数值 3\" )\r\n\tcall Preload( \"@audastra-read 0 7\" )\r\n"
        "\tcall Preload( \"@audastra-read 1 false\" )\r\n\tcall Preload( \"@audastra-read 9 x\" )\r\n"
        "\tcall Preload( \"@audastra-started\" )\r\n\tcall Preload( \"@audastra-done\" )\r\n\tcall PreloadEnd( 0.0 )\r\n\r\nendfunction\r\n";
    const auto parsed = Services::ParseMapTestScenarioResult(text, plan);
    Require(parsed.Succeeded() && parsed.value, "parse result");
    const auto& report = *parsed.value;
    Require(report.started && report.finished && report.passed == 1 && report.failed == 1, "assert counts, started and done markers");
    Require(report.lines.size() == 3 && report.lines[1] == "war3mapImported\\x.mdx \"q\"", "record lines kept in order, backslashes unescaped");
    Require(report.files.size() == 1 && report.files[0] == "Units\\Orc\\KotoBeast\\KodoDrum1.wav", "loaded resource paths kept apart");
    Require(report.reads.size() == 2 && report.reads[0].first == "Count" && report.reads[0].second == "7"
        && report.reads[1].second == "false", "reads by plan index; unknown index ignored");
    const auto partial = Services::ParseMapTestScenarioResult("\tcall Preload( \"通过: 一半\" )\n", plan);
    Require(partial.Succeeded() && partial.value && !partial.value->finished && !partial.value->started && partial.value->passed == 1,
        "missing done marker");
}
}

int main() {
    try {
        GeneratesPreloadScenario();
        RejectsUnsafeInput();
        ParsesGameOutput();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
