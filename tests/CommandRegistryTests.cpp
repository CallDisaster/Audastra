#include "War3MapCore/Services/CommandRegistry.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace War3Map::Services;

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

CommandDefinition Definition(std::string id, std::string title) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.title = std::move(title);
    definition.execute = [] { return CommandExecution{}; };
    return definition;
}

void Ranking() {
    struct Sample { const char* query; const char* text; std::int64_t score; };
    for (const auto& sample : {
        Sample{"", "地图", 0}, Sample{"地图", "地图", 1000}, Sample{"地图", "地图设置", 796},
        Sample{"设置", "地图设置", 588}, Sample{"图置", "地图设置", 199},
        Sample{"SAVE map", "Map Save As", 1362}, Sample{"\xE2\x80\x83MAP\xEF\xBB\xBFsave\t", "Map Save As", 1362},
        Sample{"😀", "地图😀x", 587}, Sample{"😀b", "a😀cb", 199},
        Sample{"Å Δ Я", "å δ я", 1961}}) {
        Require(FuzzyCommandScore(sample.query, sample.text) == sample.score, "prototype score mismatch");
    }
    Require(!FuzzyCommandScore("保存 地形", "地图保存"), "multiword AND not enforced");
    Require(!FuzzyCommandScore("😀", "😁"), "surrogate half matched");
    Require(!FuzzyCommandScore("\xC0\xAF", "x"), "invalid UTF8 accepted");
    Require(!FuzzyCommandScore(std::string(1025, 'x'), "x"), "unbounded query accepted");
}

void Registry() {
    CommandRegistry registry;
    bool enabled = true;
    int calls = 0;
    auto edit = Definition("map.save", "保存地图");
    edit.group = "工程";
    edit.shortcut = "Ctrl+S";
    edit.keywords = "save map";
    edit.availability = [&] { return CommandAvailability{enabled, "没有可保存的工程"}; };
    edit.execute = [&] { ++calls; return CommandExecution{}; };
    Require(registry.Register(edit), "valid command rejected");
    std::string error;
    Require(!registry.Register(edit, &error) && !error.empty(), "duplicate overwrote command");
    Require(registry.Search("save map").items.size() == 1, "keywords/group search failed");
    Require(registry.List().items[0].definition.shortcut == "Ctrl+S", "metadata lost");
    enabled = false; // 模拟面板列出后工程状态改变，必须在执行时重新检查。
    Require(registry.Execute("map.save").status == CommandExecutionStatus::Disabled && calls == 0, "stale enabled snapshot used");
    Require(registry.Search("save").items.empty(), "disabled command included by default");
    Require(!registry.List().items[0].availability.available, "disabled reason lost");
    enabled = true;
    Require(registry.Execute("map.save").Succeeded() && calls == 1, "enabled handler not invoked");
    Require(registry.Recent() == std::vector<std::string>{"map.save"}, "success not recorded");
    Require(registry.Execute("missing").status == CommandExecutionStatus::Unknown, "unknown command not structured");
    Require(registry.Execute("").status == CommandExecutionStatus::InvalidRequest, "empty ID accepted");
    auto bad = Definition("throwing", "失败");
    bad.execute = []() -> CommandExecution { throw std::runtime_error("handler"); };
    Require(registry.Register(bad) && registry.Execute("throwing").status == CommandExecutionStatus::Failed, "handler exception escaped");
    bad.id = "availability";
    bad.availability = []() -> CommandAvailability { throw std::runtime_error("availability"); };
    Require(registry.Register(bad) && registry.Execute("availability").status == CommandExecutionStatus::Disabled, "availability exception opened command");
    auto remove = Definition("remove", "自移除");
    remove.execute = [&] { registry.Remove("remove"); return CommandExecution{}; };
    Require(registry.Register(remove) && registry.Execute("remove").Succeeded(), "self removal invalidated handler");
    Require(registry.Execute("remove").status == CommandExecutionStatus::Unknown, "removed command still callable");
    Require(!registry.List(0, 0).valid && !registry.List(0, 257).valid && !registry.List(4097).valid, "unbounded page accepted");
    Require(!registry.Search(std::string(1025, 'x')).valid && !registry.Search("\xED\xA0\x80").valid, "invalid query accepted");
    bad.id = "invalid"; bad.title = std::string("a\0b", 3);
    Require(!registry.Register(bad), "embedded null metadata accepted");
    registry.Clear();
    Require(registry.Size() == 0 && registry.Recent().empty(), "clear retained commands/preferences");
}

void PagingAndRecent() {
    CommandRegistry registry;
    std::vector<std::string> ids;
    for (int index = 0; index < 14; ++index) {
        ids.push_back("command." + std::to_string(index));
        Require(registry.Register(Definition(ids.back(), "相同")), "fixture register failed");
    }
    auto page = registry.Search("相同", 0, 2);
    Require(page.totalMatches == 14 && page.hasMore && page.items.size() == 2, "page bound/count mismatch");
    Require(page.items[0].definition.id == ids[0] && page.items[1].definition.id == ids[1], "equal scores unstable");
    Require(registry.Search("相同", 13, 2).items[0].definition.id == ids[13] && !registry.Search("相同", 13, 2).hasMore, "last page incorrect");
    Require(registry.Search("相同", 14).items.empty(), "past-end page incorrect");
    for (const auto& id : ids) Require(registry.Execute(id).Succeeded(), "fixture execute failed");
    Require(registry.Recent().size() == 12 && registry.Recent().front() == ids[13] && registry.Recent().back() == ids[2], "recent12 bound/order incorrect");
    page = registry.Search("", 0, 14);
    Require(page.items[0].score == 60 && page.items[11].score == 16 && page.items[12].definition.id == ids[0], "recent boost or fallback order wrong");
    Require(registry.SetRecent({ids[1], ids[1], ids[0]}) && registry.Recent().size() == 2, "recent dedup failed");
    Require(!registry.SetRecent(ids) && registry.Recent().size() == 2, "invalid recent request changed state");
    Require(registry.Remove(ids[1]) && registry.Recent().size() == 1, "remove retained recent ID");
    auto mutation = Definition("mutation", "动态");
    mutation.availability = [&] { registry.Clear(); return CommandAvailability{}; };
    Require(registry.Register(mutation) && registry.List().items.size() == 14, "availability mutation invalidated list traversal");
}
} // namespace

int main() {
    try { Ranking(); Registry(); PagingAndRecent(); std::cout << "command registry tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
