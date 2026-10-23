#include "GameUiCommand.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/GameUi/FrameTree.h"
#include "War3MapCore/Mpq/MpqArchive.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::GameUi;

// 单个界面文件的读取上限；游戏最大的 fdf 不到 200 KiB。
constexpr std::size_t kMaxUiFileBytes = 8u * 1024u * 1024u;
constexpr const char* kTocPath = "UI\\FrameDef\\FrameDef.toc";
constexpr const char* kSkinPath = "UI\\war3skins.txt";
constexpr const char* kMapSkinPath = "war3mapSkin.txt";

std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

int InvalidArguments(std::string_view reason) {
    std::cerr << "GameUi: ARGUMENTS_INVALID: " << reason << '\n'
              << "usage: game-ui-inspect <war3-root> [--race Human|Orc|NightElf|Undead] [--classic] [--map <map>]"
                 " [--frame <name>]...\n"
              << "  未指定 --frame 时展开 ConsoleUI、ResourceBarFrame、UpperButtonBarFrame。\n";
    return 2;
}

const char* SeverityName(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Error: return "error";
    case DiagnosticSeverity::Warning: return "warning";
    default: return "info";
    }
}

const char* KindName(DefinitionKind kind) {
    switch (kind) {
    case DefinitionKind::Texture: return "texture";
    case DefinitionKind::String: return "string";
    case DefinitionKind::Layer: return "layer";
    default: return "frame";
    }
}

const char* LayoutName(LayoutState state) {
    switch (state) {
    case LayoutState::Placed: return "placed";
    case LayoutState::SizeFromContent: return "size-from-content";
    case LayoutState::Cycle: return "cycle";
    default: return "unplaced";
    }
}

nlohmann::json TextureJson(const TextureInfo& texture) {
    return {{"key", texture.key}, {"path", texture.path}, {"source", texture.source},
        {"tex_coord", texture.texCoord}, {"alpha_mode", texture.alphaMode}};
}

nlohmann::json NodeJson(const FrameNode& node) {
    nlohmann::json json{{"id", node.id}, {"parent", node.parent == kNoNode ? nlohmann::json() : nlohmann::json(node.parent)},
        {"kind", KindName(node.kind)}, {"type", node.frameType}, {"name", node.name}, {"layout", LayoutName(node.layout)},
        {"rect", {node.rect.left, node.rect.bottom, node.rect.right, node.rect.top}}};
    if (!node.templateName.empty()) json["template"] = node.templateName;
    if (!node.layer.empty()) json["layer"] = node.layer;
    if (!node.sourceFile.empty()) json["source"] = node.sourceFile + ":" + std::to_string(node.sourceLine);
    if (node.width) json["width"] = *node.width;
    if (node.height) json["height"] = *node.height;
    if (node.layout == LayoutState::SizeFromContent) json["content_anchor"] = FramePointName(node.contentAnchor);
    if (!node.autoFillBy.empty()) json["auto_fill_by"] = node.autoFillBy;
    if (!node.anchors.empty()) {
        auto& anchors = json["anchors"] = nlohmann::json::array();
        for (const auto& anchor : node.anchors) {
            anchors.push_back({{"point", FramePointName(anchor.point)}, {"relative", anchor.relativeName},
                {"relative_node", anchor.relativeNode == kNoNode ? nlohmann::json() : nlohmann::json(anchor.relativeNode)},
                {"relative_point", FramePointName(anchor.relativePoint)}, {"x", anchor.x}, {"y", anchor.y}});
        }
    }
    if (node.texture) json["texture"] = TextureJson(*node.texture);
    if (node.edge) json["edge"] = TextureJson(*node.edge);
    if (node.text) {
        json["text"] = {{"key", node.text->key}, {"text", node.text->text}, {"source", node.text->source},
            {"font", node.text->fontKey}, {"font_path", node.text->fontPath}, {"font_height", node.text->fontHeight},
            {"color", node.text->color}, {"justify_h", node.text->justifyH}, {"justify_v", node.text->justifyV}};
    }
    return json;
}

void AppendDiagnostics(nlohmann::json& target, const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        target.push_back({{"severity", SeverityName(diagnostic.severity)}, {"subsystem", diagnostic.subsystem},
            {"code", diagnostic.code}, {"message", diagnostic.message}});
    }
}

} // namespace

int RunGameUiInspect(int argc, wchar_t* argv[]) {
    if (argc < 3) return InvalidArguments("缺少游戏目录");
    const std::filesystem::path root = argv[2];
    FrameTreeOptions options;
    std::optional<std::filesystem::path> mapPath;
    std::vector<std::string> frames;
    for (int i = 3; i < argc; ++i) {
        const std::wstring_view argument = argv[i];
        if (argument == L"--classic") {
            options.expansion = false;
        } else if ((argument == L"--race" || argument == L"--map" || argument == L"--frame") && i + 1 < argc) {
            const std::filesystem::path value = argv[++i];
            if (argument == L"--race") options.race = Utf8(value);
            else if (argument == L"--map") mapPath = value;
            else frames.push_back(Utf8(value));
        } else {
            return InvalidArguments("无法识别的参数");
        }
    }
    if (frames.empty()) frames = {"ConsoleUI", "ResourceBarFrame", "UpperButtonBarFrame"};

    nlohmann::json diagnostics = nlohmann::json::array();
    auto game = Game::GameDataSource::Mount(root);
    AppendDiagnostics(diagnostics, game.diagnostics);
    if (!game.Succeeded() || !game.value) {
        std::cout << nlohmann::json{{"ok", false}, {"diagnostics", diagnostics}}.dump(2, ' ', false,
                         nlohmann::json::error_handler_t::replace)
                  << '\n';
        return 1;
    }
    std::optional<Mpq::MpqArchive> map;
    if (mapPath) {
        auto opened = Mpq::MpqArchive::OpenReadOnly(*mapPath);
        AppendDiagnostics(diagnostics, opened.diagnostics);
        if (!opened.Succeeded() || !opened.value) {
            std::cout << nlohmann::json{{"ok", false}, {"diagnostics", diagnostics}}.dump(2, ' ', false,
                             nlohmann::json::error_handler_t::replace)
                      << '\n';
            return 1;
        }
        map.emplace(std::move(*opened.value));
    }
    // 地图导入的同名界面文件覆盖游戏文件，与游戏读取顺序一致；地图里存在但读不出时不回退到游戏层。
    const FrameFileReader reader = [&](const std::string& path) -> Result<std::vector<std::byte>> {
        if (map && map->HasFile(path)) return map->ReadFileBounded(path, kMaxUiFileBytes);
        return game.value->ReadFileBounded(path, kMaxUiFileBytes);
    };

    FrameLibrary library;
    library.LoadToc(reader, kTocPath, "game");
    UiSkin skin;
    if (auto bytes = reader(kSkinPath); bytes.Succeeded() && bytes.value) {
        skin.SetGameSkins(BytesToText(*bytes.value));
    } else {
        AppendDiagnostics(diagnostics, bytes.diagnostics);
    }
    if (map && map->HasFile(kMapSkinPath)) {
        auto bytes = map->ReadFileBounded(kMapSkinPath, kMaxUiFileBytes);
        AppendDiagnostics(diagnostics, bytes.diagnostics);
        if (bytes.Succeeded() && bytes.value) skin.SetMapSkin(BytesToText(*bytes.value));
    }
    AppendDiagnostics(diagnostics, library.Diagnostics());

    FrameTree tree(options);
    nlohmann::json roots = nlohmann::json::array();
    for (const auto& frame : frames) {
        auto instantiated = tree.Instantiate(library, &skin, frame);
        AppendDiagnostics(diagnostics, instantiated.diagnostics);
        if (instantiated.value) roots.push_back({{"name", frame}, {"id", *instantiated.value}});
    }
    AppendDiagnostics(diagnostics, tree.Layout());

    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : tree.Nodes()) nodes.push_back(NodeJson(node));
    const nlohmann::json response{{"ok", true}, {"game_root", Utf8(root)}, {"map", mapPath ? Utf8(*mapPath) : ""},
        {"race", options.race}, {"expansion", options.expansion},
        {"screen", {options.screenWidth, options.screenHeight}}, {"files", library.Files().size()},
        {"templates", library.TemplateCount()}, {"strings", library.StringCount()}, {"roots", roots},
        {"nodes", nodes}, {"diagnostics", diagnostics}};
    // 界面文字按 UTF-8 原样输出；无效字节替换显示，不影响节点结构。
    std::cout << response.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    return 0;
}
