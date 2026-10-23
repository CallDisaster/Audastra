#include "GameUiCommand.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/GameUi/FrameTree.h"
#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/GameUi/MapUiFiles.h"
#include "War3MapCore/GameUi/ScriptFrames.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Text/TextUtil.h"

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
// 地图脚本只用于查找加载界面目录的调用。
constexpr std::size_t kMaxScriptBytes = 64u * 1024u * 1024u;
constexpr const char* kTocPath = "UI\\FrameDef\\FrameDef.toc";
constexpr const char* kSkinPath = "UI\\war3skins.txt";
constexpr const char* kMiscUiPath = "UI\\MiscUI.txt";
constexpr const char* kMapSkinPath = "war3mapSkin.txt";

std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

int InvalidArguments(std::string_view reason) {
    std::cerr << "GameUi: ARGUMENTS_INVALID: " << reason << '\n'
              << "usage: game-ui-inspect <war3-root> [--race Human|Orc|NightElf|Undead] [--classic] [--map <map>]"
                 " [--frame <name>]...\n"
              << "  未指定 --frame 时展开 ConsoleUI、GameUI、ResourceBarFrame、UpperButtonBarFrame、MapScriptUI。\n";
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
    if (!node.codeLabel.empty()) json["code_area"] = node.codeLabel;
    if (node.codePlaced) json["placed_by"] = "game-code";
    if (node.scriptFrame) json["script_frame"] = *node.scriptFrame;
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

const char* TargetName(ScriptFrameTarget target) {
    switch (target) {
    case ScriptFrameTarget::Screen: return "screen";
    case ScriptFrameTarget::CodeArea: return "code-area";
    case ScriptFrameTarget::Script: return "script";
    case ScriptFrameTarget::Named: return "named";
    default: return "unknown";
    }
}

nlohmann::json RefJson(const ScriptFrameRef& ref) {
    nlohmann::json json{{"target", TargetName(ref.target)}};
    if (!ref.name.empty()) json["name"] = ref.name;
    if (ref.target == ScriptFrameTarget::Script) json["frame"] = ref.frame;
    return json;
}

nlohmann::json SiteJson(const ScriptCallSite& site) {
    return {{"script", site.scriptFile}, {"line", site.line}, {"function", site.function}, {"call", site.call}};
}

nlohmann::json ScriptFrameJson(std::size_t index, const ScriptFrame& frame) {
    nlohmann::json json{{"index", index}, {"family", UiApiFamilyName(frame.family)}, {"created", SiteJson(frame.created)},
        {"name", frame.name}, {"name_dynamic", frame.nameDynamic}, {"parent", RefJson(frame.parent)},
        {"runtime_calls", frame.runtimeCalls}};
    if (frame.object != ScriptObjectKind::Frame) json["object"] = ScriptObjectKindName(frame.object);
    if (!frame.variable.empty()) json["variable"] = frame.variable;
    if (!frame.frameType.empty()) json["type"] = frame.frameType;
    if (!frame.templateName.empty()) json["template"] = frame.templateName;
    if (frame.clearPoints) json["clear_points"] = true;
    auto& anchors = json["anchors"] = nlohmann::json::array();
    for (const auto& anchor : frame.anchors) {
        anchors.push_back({{"point", FramePointName(anchor.point)}, {"relative", RefJson(anchor.relative)},
            {"relative_point", FramePointName(anchor.relativePoint)}, {"x", anchor.x}, {"y", anchor.y}});
    }
    if (frame.width) json["width"] = *frame.width;
    if (frame.height) json["height"] = *frame.height;
    if (frame.texture) json["texture"] = *frame.texture;
    if (frame.text) json["text"] = *frame.text;
    if (frame.visible) json["visible"] = *frame.visible;
    if (!frame.buttons.empty()) json["buttons"] = frame.buttons;
    if (frame.board) {
        const ScriptBoard& board = *frame.board;
        auto& out = json["board"] = {{"rows", board.rows}, {"columns", board.columns}, {"minimized", board.minimized}};
        auto& items = out["items"] = nlohmann::json::array();
        for (std::uint32_t r = 0; r < board.rows; ++r) {
            auto row = nlohmann::json::array();
            for (std::uint32_t c = 0; c < board.columns; ++c) {
                const ScriptBoardItem& item = *board.At(r, c);
                nlohmann::json cell{{"value", item.value}, {"width", item.width}, {"show_value", item.showValue},
                    {"show_icon", item.showIcon}, {"color", {item.color[0], item.color[1], item.color[2], item.color[3]}}};
                if (item.icon) cell["icon"] = *item.icon;
                row.push_back(std::move(cell));
            }
            items.push_back(std::move(row));
        }
    }
    if (frame.leaderboard) {
        const ScriptLeaderboard& board = *frame.leaderboard;
        auto& out = json["leaderboard"] = {{"size", board.size}, {"show_title", board.showTitle}};
        if (board.clipped) out["clipped"] = true;
        if (board.sortTies) out["sort_ties"] = true;
        const auto color = [](const std::array<std::uint8_t, 4>& c) { return nlohmann::json{c[0], c[1], c[2], c[3]}; };
        auto& items = out["items"] = nlohmann::json::array();
        for (const auto& item : board.items) {
            nlohmann::json entry{{"label", item.label}, {"value", item.value}, {"show_label", item.showLabel},
                {"show_value", item.showValue}, {"show_icon", item.showIcon}};
            entry["player"] = item.player ? nlohmann::json(*item.player) : nlohmann::json(nullptr);
            if (item.labelColor) entry["label_color"] = color(*item.labelColor);
            if (item.valueColor) entry["value_color"] = color(*item.valueColor);
            items.push_back(std::move(entry));
        }
    }
    if (!frame.dynamic.empty()) json["dynamic"] = frame.dynamic;
    if (!frame.runtimeSamples.empty()) {
        auto& samples = json["runtime_samples"] = nlohmann::json::array();
        for (const auto& sample : frame.runtimeSamples) samples.push_back(SiteJson(sample));
    }
    return json;
}

const char* ReasonName(ScriptUiCallReason reason) {
    switch (reason) {
    case ScriptUiCallReason::UntracedTarget: return "untraced-target";
    case ScriptUiCallReason::Unknown: return "unknown";
    default: return "not-previewed";
    }
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
    if (frames.empty()) {
        frames = {"ConsoleUI", std::string(kGameCodeRoot), "ResourceBarFrame", "UpperButtonBarFrame", std::string(kScriptRoot)};
    }

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
    nlohmann::json tocCalls = nlohmann::json::array();
    ScriptFramePlan scriptPlan;
    if (map) {
        // 游戏只执行一份 JASS 脚本（先根目录再 scripts 目录）；Lua 地图另有 war3map.lua。
        std::vector<MapScriptText> scripts;
        bool jass = false;
        for (const auto& candidate : MapScriptPaths()) {
            const std::string path(candidate.path);
            if ((candidate.language == ScriptLanguage::Jass && jass) || !map->HasFile(path)) continue;
            auto bytes = map->ReadFileBounded(path, kMaxScriptBytes);
            AppendDiagnostics(diagnostics, bytes.diagnostics);
            if (!bytes.Succeeded() || !bytes.value) continue;
            scripts.push_back({path, candidate.language, BytesToText(*bytes.value)});
            if (candidate.language == ScriptLanguage::Jass) jass = true;
        }
        const auto plan = PlanMapTocs(map->ListFiles(), scripts);
        AppendDiagnostics(diagnostics, plan.diagnostics);
        for (const auto& toc : plan.tocs) library.LoadToc(reader, toc, "map");
        for (const auto& call : plan.calls) {
            tocCalls.push_back({{"function", call.function}, {"path", call.path}, {"script", call.scriptFile},
                {"line", call.line}});
        }
        TriggerStrings strings;
        if (map->HasFile("war3map.wts")) {
            auto bytes = map->ReadFileBounded("war3map.wts", kMaxScriptBytes);
            AppendDiagnostics(diagnostics, bytes.diagnostics);
            if (bytes.Succeeded() && bytes.value) {
                if (auto table = MapData::ParseStringTable(*bytes.value); table.value) {
                    for (auto& entry : table.value->entries) strings[entry.id] = std::move(entry.value);
                }
            }
        }
        scriptPlan = FindScriptFrames(scripts, std::move(strings));
        AppendDiagnostics(diagnostics, scriptPlan.diagnostics);
    }
    UiSkin skin;
    if (auto bytes = reader(kSkinPath); bytes.Succeeded() && bytes.value) {
        skin.SetGameSkins(BytesToText(*bytes.value));
    } else {
        AppendDiagnostics(diagnostics, bytes.diagnostics);
    }
    if (auto bytes = reader(kMiscUiPath); bytes.Succeeded() && bytes.value) {
        skin.SetMiscUi(BytesToText(*bytes.value));
    } else {
        AppendDiagnostics(diagnostics, bytes.diagnostics);
    }
    if (map && map->HasFile(kMapSkinPath)) {
        auto bytes = map->ReadFileBounded(kMapSkinPath, kMaxUiFileBytes);
        AppendDiagnostics(diagnostics, bytes.diagnostics);
        if (bytes.Succeeded() && bytes.value) skin.SetMapSkin(BytesToText(*bytes.value));
    }
    if (skin.HasGameSkins()) {
        std::vector<Diagnostic> colorDiagnostics;
        skin.SetTeamColors(ReadTeamColors(skin, [&reader](const std::string& path) -> std::optional<std::vector<std::byte>> {
            auto bytes = reader(path);
            if (!bytes.Succeeded() || !bytes.value) return std::nullopt;
            return std::move(*bytes.value);
        }, colorDiagnostics));
        AppendDiagnostics(diagnostics, colorDiagnostics);
    }
    AppendDiagnostics(diagnostics, library.Diagnostics());

    FrameTree tree(options);
    nlohmann::json roots = nlohmann::json::array();
    bool scriptRoot = false;
    for (const auto& frame : frames) {
        if (Text::LowerAscii(frame) == Text::LowerAscii(kGameCodeRoot)) {
            roots.push_back({{"name", frame}, {"id", tree.AddGameCodeAreas()}});
            continue;
        }
        if (Text::LowerAscii(frame) == Text::LowerAscii(kScriptRoot)) {
            scriptRoot = true;
            continue;
        }
        auto instantiated = tree.Instantiate(library, &skin, frame);
        AppendDiagnostics(diagnostics, instantiated.diagnostics);
        if (instantiated.value) roots.push_back({{"name", frame}, {"id", *instantiated.value}});
    }
    // 脚本框架可以锚在游戏代码区域上，放在其他根之后展开。
    if (scriptRoot) {
        std::vector<Diagnostic> added;
        if (const auto id = AddScriptFrames(tree, library, &skin, scriptPlan, added); id != kNoNode) {
            roots.push_back({{"name", std::string(kScriptRoot)}, {"id", id}});
        }
        AppendDiagnostics(diagnostics, added);
    }
    AppendDiagnostics(diagnostics, tree.Layout());
    nlohmann::json scriptFrames = nlohmann::json::array();
    for (std::size_t i = 0; i < scriptPlan.frames.size(); ++i) scriptFrames.push_back(ScriptFrameJson(i, scriptPlan.frames[i]));
    nlohmann::json scriptCalls = nlohmann::json::array();
    for (const auto& group : scriptPlan.calls) {
        nlohmann::json samples = nlohmann::json::array();
        for (const auto& sample : group.samples) samples.push_back(SiteJson(sample));
        scriptCalls.push_back({{"function", group.function}, {"family", UiApiFamilyName(group.family)},
            {"reason", ReasonName(group.reason)}, {"count", group.count}, {"samples", samples}});
    }

    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : tree.Nodes()) nodes.push_back(NodeJson(node));
    const nlohmann::json response{{"ok", true}, {"game_root", Utf8(root)}, {"map", mapPath ? Utf8(*mapPath) : ""},
        {"race", options.race}, {"expansion", options.expansion},
        {"screen", {options.screenWidth, options.screenHeight}}, {"files", library.Files().size()},
        {"templates", library.TemplateCount()}, {"strings", library.StringCount()}, {"toc_calls", tocCalls},
        {"script_frames", scriptFrames}, {"script_calls", scriptCalls}, {"roots", roots},
        {"nodes", nodes}, {"diagnostics", diagnostics}};
    // 界面文字按 UTF-8 原样输出；无效字节替换显示，不影响节点结构。
    std::cout << response.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    return 0;
}
