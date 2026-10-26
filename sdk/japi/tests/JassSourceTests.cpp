#include "War3MapCore/Jass/JassChecker.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    // Original minimal declarations for this library's external interface. Not a copy of common.j.
    const std::string declarations = R"(
type agent extends handle
type trigger extends agent
type triggeraction extends handle
type event extends agent
type player extends agent
type timer extends agent
native StringLength takes string s returns integer
native SubString takes string s, integer a, integer b returns string
native I2S takes integer n returns string
native S2I takes string s returns integer
native S2R takes string s returns real
native R2SW takes real r, integer width, integer precision returns string
native GetLocalizedString takes string s returns string
native CreateTrigger takes nothing returns trigger
native DestroyTrigger takes trigger t returns nothing
native TriggerAddAction takes trigger t, code c returns triggeraction
native TriggerExecute takes trigger t returns nothing
native CreateTimer takes nothing returns timer
native TimerStart takes timer t, real period, boolean repeating, code callback returns nothing
native PauseTimer takes timer t returns nothing
native DestroyTimer takes timer t returns nothing
native GetLocalPlayer takes nothing returns player
native DisplayTimedTextToPlayer takes player p, real x, real y, real duration, string text returns nothing
)";
    std::vector<std::string> contents;
    for (int i = 1; i < argc; ++i) {
        std::ifstream stream(argv[i], std::ios::binary);
        if (!stream) return 2;
        contents.emplace_back(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    std::vector<War3Map::Jass::JassSource> sources{{"minimal-interface.j", declarations}};
    for (int i = 1; i < argc; ++i) sources.push_back({argv[i], contents[i - 1]});
    War3Map::Jass::JassCheckOptions options;
    options.threads = 1;
    const auto result = War3Map::Jass::CheckJass(sources, options);
    for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.message << '\n';
    std::cout << "JAPI JASS semantic check: " << result.functions << " functions; completed=" << result.completed
        << "; semantic=" << result.semanticChecked << '\n';
    return result.Succeeded() && result.completed && result.semanticChecked ? 0 : 1;
}
