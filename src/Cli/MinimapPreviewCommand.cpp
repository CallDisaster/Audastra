#include "MinimapPreviewCommand.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>

int RunMinimapPreview(int argc, wchar_t** argv) {
    using namespace War3Map;
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: minimap-preview <map> <expected-source-sha256> [<new-preview.ppm>]\n";
        return 2;
    }
    const std::wstring wide(argv[3]);
    if (wide.size() != 64 || !std::all_of(wide.begin(), wide.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    })) { std::cerr << "MINIMAP_PREVIEW_IDENTITY: 需要小写完整SHA-256。\n"; return 2; }
    const std::string expected(wide.begin(), wide.end());
    auto preview = Services::OpenMinimapPreview(argv[2], expected);
    const auto fail = [](const auto& diagnostics) {
        for (const auto& d : diagnostics) std::cerr << d.code << ": " << d.message << '\n';
        return 1;
    };
    if (!preview.Succeeded()) return fail(preview.diagnostics);
    const auto& value = *preview.value;
    if (argc == 5) {
        const auto written = Services::ExportMinimapPreviewPpm(value, argv[4]);
        if (!written.Succeeded()) return fail(written.diagnostics);
    }
    std::cout << nlohmann::json{{"ok", true}, {"read_only", true}, {"saved_map_modified", false},
        {"content", "saved_minimap_base"}, {"source_sha256", value.sourceSha256}, {"present", value.present},
        {"member_sha256", value.memberSha256}, {"rgba_sha256", value.rgbaSha256},
        {"width", value.image.width}, {"height", value.image.height}, {"preview_exported", argc == 5}}.dump() << '\n';
    return 0;
}
