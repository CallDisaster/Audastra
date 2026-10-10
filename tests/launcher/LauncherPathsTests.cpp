#include "Launcher/LauncherPaths.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace Audastra::Launcher;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void Arguments() {
    Check(ArgumentsAfterProgram(L"\"C:\\Program Files\\Audastra\\Audastra.exe\" --open-map \"D:\\地图 副本.w3x\"")
        == L"--open-map \"D:\\地图 副本.w3x\"", "quoted program name or spaced map path changed");
    Check(ArgumentsAfterProgram(L"Audastra.exe   --flag\t x") == L"--flag\t x", "unquoted program name or inner whitespace changed");
    Check(ArgumentsAfterProgram(L"\"C:\\A B\\Audastra.exe\"") .empty(), "program without arguments produced arguments");
    Check(ArgumentsAfterProgram(L"Audastra.exe").empty() && ArgumentsAfterProgram(L"").empty(), "empty command line mishandled");
    // argv[0] 不处理反斜杠转义：引号前的反斜杠属于程序名。
    Check(ArgumentsAfterProgram(L"\"C:\\dir\\\" a \"b c\"") == L"a \"b c\"", "argv0 escape rule differs from Windows");
    Check(ArgumentsAfterProgram(L"\"C:\\unterminated") .empty(), "unterminated program name leaked text as arguments");
}

void Latest() {
    Check(LatestVersionDirectory("version_dir=20261010-dafa6c0\nproduct_version=0.1.0\n") == "20261010-dafa6c0", "plain value");
    Check(LatestVersionDirectory("\xEF\xBB\xBFproduct_version=0.1.0\r\nversion_dir=20261010-dafa6c0\r\n") == "20261010-dafa6c0",
        "BOM or CRLF not accepted");
    for (const auto bad : {"version_dir=..\n", "version_dir=.\n", "version_dir=a/b\n", "version_dir=a\\b\n",
             "version_dir=C:x\n", "version_dir=\n", "version_dir= 2026\n", "other=1\n", ""})
        Check(LatestVersionDirectory(bad).empty(), "unsafe or missing version directory accepted");
}
}

int main() {
    try {
        Arguments(); Latest();
        std::cout << "launcher_arguments_verbatim=1 latest_directory_single_level=1\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
