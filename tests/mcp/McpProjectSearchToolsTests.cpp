#include "McpProjectSearchTools.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Mcp;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QString Code(const QJsonObject& response) {
    const auto diagnostics = response.value("diagnostics").toArray();
    return diagnostics.isEmpty() ? QString{} : diagnostics.first().toObject().value("code").toString();
}
struct Fixture {
    Trigger::TriggerDocument document;
    std::string revision = "search-project/1/6";
    ProjectSearchToolHost host;
    int queries = 0;
    bool changeRevisionDuringQuery = false;
    Fixture() {
        for (StableId i = 1; i <= 5; ++i) {
            Trigger::TriggerRecord trigger; trigger.id = i; trigger.name.text = "ChinesePage中文"; document.triggers.push_back(trigger);
        }
        document.triggers[0].ecas = {0};
        Trigger::EcaNode eca; eca.id = 9007199254740993ULL; eca.sourceTriggerId = 1; eca.name.text = "TextAction"; eca.arguments = {0};
        Trigger::ArgumentNode argument; argument.id = 9007199254740995ULL; argument.type = 3; argument.value.text = "唯一参数中文";
        document.ecaNodes = {eca}; document.argumentNodes = {argument};
        host.identity = [&] { return QJsonObject{{"project_identity", "owned-test-project"}, {"context_revision", QString::fromStdString(revision)}}; };
        host.query = [&](std::string_view expected, std::string_view query, const ProjectSearchOptions& options,
                std::size_t pageLimit, const ProjectSearchCursor* cursor) {
            ++queries;
            ProjectSearchContext context; context.revision = revision; context.triggers = &document; context.triggerState = ProjectSearchState::Ready;
            auto result = SearchProjectPage(context, expected, query, options, pageLimit, cursor);
            if (changeRevisionDuringQuery) revision += "/changed";
            return result;
        };
    }
    QJsonObject Call(const QJsonObject& arguments = {}) { return InvokeProjectSearchTool("project_search", arguments, host); }
};
void SameServiceAndStableIds() {
    const auto definitions = ProjectSearchToolDefinitions();
    Require(definitions.size() == 1 && definitions[0].name == "project_search"
        && definitions[0].permission == ToolPermission::Query && definitions[0].maxResponseBytes == 256U * 1024U
        && definitions[0].maxRequestIdBytes == 1024, "one query tool with real transport budgets");
    Fixture fixture;
    const auto parameter = fixture.Call({{"query", "唯一参数中文"}});
    Require(parameter.value("ok").toBool() && parameter.value("read_only").toBool(), "real shared service parameter query succeeds");
    const auto row = parameter.value("rows").toArray().first().toObject();
    Require(row.value("source_id") == "1" && row.value("eca_id") == "9007199254740993"
        && row.value("argument_id") == "9007199254740995" && row.value("raw_value") == "唯一参数中文",
        "large ECA/Argument identities preserved as decimal strings");
    Require(parameter.value("coverage").toArray().size() == 11 && !parameter.value("complete").toBool(),
        "unavailable modules remain explicit partial coverage rather than false full map scan");
    ProjectSearchContext context; context.revision = fixture.revision; context.triggers = &fixture.document;
    context.triggerState = ProjectSearchState::Ready;
    const auto direct = SearchProjectPage(context, context.revision, "唯一参数中文");
    Require(direct.Succeeded() && QString::fromStdString(direct.value->windowSha256) == parameter.value("window_sha256"),
        "MCP and direct UI service produce exactly same query window binding");
    Require(fixture.document.argumentNodes[0].value.text == "唯一参数中文" && fixture.revision == "search-project/1/6",
        "read-only queries do not mutate current IR or revision");
}
void PagingAndConflicts() {
    Fixture fixture;
    QJsonObject request{{"query", "ChinesePage"}, {"page", QJsonObject{{"limit", 2}}},
        {"budget", QJsonObject{{"window_limit", 4}}}};
    const auto first = fixture.Call(request);
    Require(first.value("ok").toBool() && first.value("rows").toArray().size() == 2
        && first.value("window_has_more").toBool() && first.value("next_cursor").isObject(),
        "MCP pagination exposes only bounded window with explicit external truncation");
    request["page"] = QJsonObject{{"limit", 2}, {"cursor", first.value("next_cursor")}};
    const auto second = fixture.Call(request);
    Require(second.value("ok").toBool() && second.value("rows").toArray().first().toObject().value("source_id") == "3"
        && second.value("next_cursor").isNull(), "actual shared service returns next two stable equal-score rows");
    auto changed = request; changed["budget"] = QJsonObject{{"window_limit", 4}, {"max_trigger_nodes", 10}};
    Require(Code(fixture.Call(changed)) == "PROJECT_SEARCH_CURSOR_STALE", "changed option scope refuses old MCP cursor");
    changed = request; changed["query"] = "different";
    Require(Code(fixture.Call(changed)) == "PROJECT_SEARCH_CURSOR_STALE", "changed query refuses old MCP cursor");
    fixture.revision = "search-project/1/7";
    Require(Code(fixture.Call(request)) == "PROJECT_SEARCH_CURSOR_STALE", "changed host revision refuses old MCP cursor");
    fixture.changeRevisionDuringQuery = true;
    Require(Code(fixture.Call()) == "PROJECT_SEARCH_STALE", "identity is rechecked after synchronous host query");
}
void BadArgumentsCannotInvokeQuery() {
    Fixture fixture;
    Require(Code(fixture.Call({{"unknown", true}})) == "PROJECT_SEARCH_ARGUMENT_UNKNOWN", "unknown fields fail closed");
    Require(Code(fixture.Call({{"project_identity", "owned-test-project"}})) == "PROJECT_SEARCH_IDENTITY_REQUIRED", "half identity is rejected");
    Require(Code(fixture.Call({{"project_identity", "wrong-project"}, {"expected_context_revision", QString::fromStdString(fixture.revision)}}))
        == "PROJECT_SEARCH_STALE", "wrong current project identity rejected before query");
    Require(Code(fixture.Call({{"query", QString(257, 'x')}})) == "PROJECT_SEARCH_ARGUMENT_BUDGET", "UTF8 byte limit is enforced");
    Require(Code(fixture.Call({{"query", QString(100, QChar(0x4e2d))}})) == "PROJECT_SEARCH_ARGUMENT_BUDGET", "Chinese UTF8 budget uses bytes not UTF16 characters");
    Require(Code(fixture.Call({{"page", QJsonObject{{"limit", 2.5}}}})) == "PROJECT_SEARCH_ARGUMENT_BUDGET", "fractional page cannot be truncated to integer");
    Require(Code(fixture.Call({{"budget", QJsonObject{{"max_trigger_nodes", 65537}}}})) == "PROJECT_SEARCH_ARGUMENT_BUDGET", "hard graph ceiling cannot be raised");
    const auto cursor = QJsonObject{{"context_revision", QString::fromStdString(fixture.revision)}, {"query", ""},
        {"window_sha256", QString(64, 'a')}, {"next_index", "01"}};
    Require(Code(fixture.Call({{"page", QJsonObject{{"cursor", cursor}}}})) == "PROJECT_SEARCH_CURSOR_INVALID", "noncanonical cursor index rejected");
    Require(fixture.queries == 0, "all invalid inputs fail before borrowing any live source");
    Require(Code(InvokeProjectSearchTool("project_edit", {}, fixture.host)) == "PROJECT_SEARCH_TOOL_UNSUPPORTED", "facade offers no mutation tool");
    fixture.host.query = {};
    Require(Code(fixture.Call()) == "PROJECT_SEARCH_HOST_UNAVAILABLE", "missing live host is not a successful empty scan");
}
} // namespace
int main() {
    try { SameServiceAndStableIds(); PagingAndConflicts(); BadArgumentsCannotInvokeQuery();
        std::cout << "MCP project search shared query tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
