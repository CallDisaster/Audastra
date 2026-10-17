#include "MapEditSaveCommand.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Invocation {
    int exitCode = 0;
    std::string output, error;
};

Invocation Invoke(std::vector<std::wstring> arguments) {
    std::vector<wchar_t*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    std::ostringstream output, error;
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    auto* previousError = std::cerr.rdbuf(error.rdbuf());
    try {
        const auto exitCode = RunMapEditSave(static_cast<int>(argv.size()), argv.data());
        std::cout.rdbuf(previousOutput);
        std::cerr.rdbuf(previousError);
        return {exitCode, output.str(), error.str()};
    } catch (...) {
        std::cout.rdbuf(previousOutput);
        std::cerr.rdbuf(previousError);
        throw;
    }
}

std::vector<std::wstring> Arguments() {
    // 参数门禁必须在打开此不存在的来源之前终止；不依赖本机地图、游戏或工具。
    return {L"war3-map-cli", L"map-edit-save", L"missing-minimap-cli-source.w3x",
        std::wstring(64, L'a'), std::wstring(64, L'b'), L"unused-tool", L"unused-game",
        L"missing-minimap-cli-candidate.w3x"};
}

void Rejected(const Invocation& result, std::string_view detail) {
    Require(result.exitCode == 2 && result.output.empty(), "invalid option must fail before any receipt");
    Require(result.error.find("SAVE_ARGUMENT_INVALID") != std::string::npos
        && result.error.find(detail) != std::string::npos, "argument rejection must identify its exact scope");
}

void MissingMinimapChoiceIsExplicitAndTerrainOnly() {
    auto arguments = Arguments();
    arguments.insert(arguments.end(), {L"--generate-missing-minimap", L"--generate-missing-minimap"});
    Rejected(Invoke(arguments), "may appear only once");

    arguments = Arguments();
    arguments.push_back(L"--generate-missing-minimap");
    Rejected(Invoke(arguments), "at least one bounded edit operation");

    arguments = Arguments();
    arguments.insert(arguments.end(), {L"--generate-missing-minimap", L"--rotate", L"units", L"0",
        L"row-key", std::wstring(64, L'c'), L"90"});
    Rejected(Invoke(arguments), "requires only nonempty height strokes");

    arguments = Arguments();
    arguments.insert(arguments.end(), {L"--stroke", L"raise", L"circle", L"4", L"4", L"0", L"1",
        L"--generate-missing-minimap", L"--rotate", L"units", L"0", L"row-key", std::wstring(64, L'c'), L"90"});
    Rejected(Invoke(arguments), "requires only nonempty height strokes");
}

} // namespace

int main() {
    try {
        MissingMinimapChoiceIsExplicitAndTerrainOnly();
        std::cout << "Map edit save CLI minimap option gates passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
