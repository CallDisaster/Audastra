#include "MinimapPreviewCommand.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>

int RunMinimapPreview(int argc, wchar_t** argv) {
    using namespace War3Map;
    if (argc < 4) {
        std::cerr << "usage: minimap-preview <map> <expected-source-sha256> [<new-preview.ppm>] [--source "
                     "current|saved] [--game-root <path>] [--layers <bits>] [--units] [--starts] [--icons]\n";
        return 2;
    }
    const std::wstring wide(argv[3]);
    if (wide.size() != 64 || !std::all_of(wide.begin(), wide.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    })) { std::cerr << "MINIMAP_PREVIEW_IDENTITY: 需要小写完整SHA-256。\n"; return 2; }
    const std::string expected(wide.begin(), wide.end());
    std::wstring source = L"saved";
    std::filesystem::path gameRoot, exportPath;
    std::uint32_t layers = 0x8b;
    bool units = false, starts = false, icons = false, layerSet = false;
    try {
        for (int i = 4; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if ((arg == L"--source" || arg == L"--game-root" || arg == L"--layers") && i + 1 >= argc)
                throw std::invalid_argument("missing value");
            if (arg == L"--source")
                source = argv[++i];
            else if (arg == L"--game-root")
                gameRoot = argv[++i];
            else if (arg == L"--layers") {
                const std::wstring value = argv[++i];
                std::size_t read = 0;
                auto number = std::stoul(value, &read, 0);
                if (read != value.size() || number > 0x4af || (number & ~0x4af))
                    throw std::invalid_argument("invalid layers");
                layers = std::uint32_t(number);
                layerSet = true;
            } else if (arg == L"--units")
                units = true;
            else if (arg == L"--starts")
                starts = true;
            else if (arg == L"--icons")
                icons = true;
            else if (!arg.starts_with(L"--") && exportPath.empty())
                exportPath = arg;
            else
                throw std::invalid_argument("unknown argument");
        }
        if (source != L"saved" && source != L"current")
            throw std::invalid_argument("invalid source");
        if (source == L"saved" && (!gameRoot.empty() || layerSet || units || starts || icons))
            throw std::invalid_argument("saved source has no display layers");
    } catch (const std::exception& e) {
        std::cerr << "MINIMAP_PREVIEW_ARGUMENT: " << e.what() << '\n';
        return 2;
    }
    auto preview = source == L"current"
                       ? Services::OpenCurrentMinimapPreview(argv[2], expected, gameRoot, layers, units, starts, icons)
                       : Services::OpenMinimapPreview(argv[2], expected);
    const auto fail = [](const auto& diagnostics) {
        for (const auto& d : diagnostics) std::cerr << d.code << ": " << d.message << '\n';
        return 1;
    };
    if (!preview.Succeeded()) return fail(preview.diagnostics);
    const auto& value = *preview.value;
    if (!exportPath.empty()) {
        const auto written = Services::ExportMinimapPreviewPpm(value, exportPath);
        if (!written.Succeeded()) return fail(written.diagnostics);
    }
    std::cout << nlohmann::json{{"ok", true},
                                {"read_only", true},
                                {"saved_map_modified", false},
                                {"content", value.current ? "current_minimap" : "saved_minimap_base"},
                                {"source_sha256", value.sourceSha256},
                                {"present", value.present},
                                {"document_revision", std::to_string(value.documentRevision)},
                                {"layers", value.layers},
                                {"generation_ms", value.generationMilliseconds},
                                {"member_sha256", value.memberSha256},
                                {"rgba_sha256", value.rgbaSha256},
                                {"width", value.image.width},
                                {"height", value.image.height},
                                {"preview_exported", !exportPath.empty()}}
                     .dump()
              << '\n';
    return 0;
}
