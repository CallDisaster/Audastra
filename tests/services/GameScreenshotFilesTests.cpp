#include "../../src/ScriptBuild/GameScreenshot127.h"
#include <windows.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Need(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
void Tga(const std::filesystem::path& path, bool pixels = true) {
    std::array<char,22> data{}; data[2]=2; data[12]=1; data[14]=1; data[16]=32;
    std::ofstream file(path, std::ios::binary); file.write(data.data(), pixels ? data.size() : 18);
}
}
int main() {
    using namespace War3Map;
    auto root=std::filesystem::temp_directory_path() / (L"audastra-shot-files-" + std::to_wstring(GetCurrentProcessId()));
    try {
        Need(std::filesystem::create_directory(root), "fresh screenshot fixture");
        Tga(root / "old.tga");
        auto none=Script::detail::CollectNativeGameScreenshot(root,25,[]{return Status::Ok();});
        Need(!none.Succeeded(), "old screenshot cannot satisfy a new request");
        auto failed=Script::detail::CollectNativeGameScreenshot(root,25,[]{return Status::Failure("Fixture","input-failed","not sent");});
        Need(!failed.Succeeded() && failed.diagnostics.front().code=="input-failed", "input failure stays failure");
        auto shot=Script::detail::CollectNativeGameScreenshot(root,25,[&]{Tga(root / "new.tga");return Status::Ok();});
        Need(shot.Succeeded() && shot.value->filename()=="new.tga", "only newly completed image returned");
        auto partial=Script::detail::CollectNativeGameScreenshot(root,25,[&]{Tga(root / "partial.tga",false);return Status::Ok();});
        Need(!partial.Succeeded(), "header without pixels is not a frame");
        auto multiple=Script::detail::CollectNativeGameScreenshot(root,25,[&]{Tga(root / "one.tga");Tga(root / "two.tga");return Status::Ok();});
        Need(!multiple.Succeeded() && multiple.diagnostics.front().code=="MAP_TEST_SCREENSHOT_AMBIGUOUS", "multiple new frames cannot be attributed uniquely");
        std::filesystem::remove_all(root);
        std::cout<<"Native screenshot collection fixtures passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
