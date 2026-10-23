#include "War3MapCore/GameUi/FrameDraw.h"
#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/GameUi/MapUiFiles.h"
#include "War3MapCore/GameUi/ScriptFrames.h"
#include "War3MapCore/GameUi/ScriptSimulation.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

using namespace War3Map;
using namespace War3Map::GameUi;

void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

const FrameNode& NodeNamed(const FrameTree& tree, std::string_view name) {
    const auto id = tree.FindByName(name);
    if (!id) throw std::runtime_error("node missing: " + std::string(name));
    return tree.Nodes()[*id];
}

// 内存中的“归档”：路径不区分大小写，供 FrameLibrary 读取。
FrameFileReader MemoryReader(const std::map<std::string, std::string>& files) {
    return [files](const std::string& path) -> Result<std::vector<std::byte>> {
        std::string key = Text::LowerAscii(path);
        for (char& c : key) {
            if (c == '/') c = '\\';
        }
        const auto found = files.find(key);
        if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Test", "MISSING", path);
        std::vector<std::byte> bytes(found->second.size());
        std::memcpy(bytes.data(), found->second.data(), bytes.size());
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    };
}

void TestParser() {
    const std::string text =
        "\xEF\xBB\xBF// header comment\n"
        "IncludeFile \"UI\\FrameDef\\Base.fdf\",\n"
        "StringList {\n"
        "    KEY_OK \"|cffffcc00O|rK\",\n"
        "    KEY_CANCEL \"Cancel\", // trailing\n"
        "}\n"
        "/* block\n comment */\n"
        "Frame \"SIMPLEFRAME\" \"Panel\" INHERITS WITHCHILDREN \"PanelTemplate\"\n"
        "{\n"
        "    Width 0.2,\n"
        "    SetPoint TOPLEFT, \"Other\", BOTTOMLEFT, 0.01, -0.02,\n"
        "    Layer \"BACKGROUND\" {\n"
        "        Texture \"PanelBg\" { File \"Bg\", TexCoord 0, 0.5, 0, 1, }\n"
        "    }\n"
        "    String \"PanelText\" INHERITS \"TextTemplate\" { Text \"KEY_OK\", }\n"
        "}\n"
        "Bogus line here\n"
        "}\n";
    const auto parsed = ParseFdf(text, "UI\\Test.fdf");
    Require(parsed.value.has_value(), "parser returns a file");
    const FdfFile& file = *parsed.value;
    Require(file.includes.size() == 1 && file.includes[0] == "UI\\FrameDef\\Base.fdf", "include path keeps backslashes");
    Require(file.strings.size() == 2 && file.strings[0].first == "KEY_OK" && file.strings[0].second == "|cffffcc00O|rK",
        "string list entries");
    Require(file.definitions.size() == 1, "one top-level definition");
    const FdfDefinition& panel = file.definitions[0];
    Require(panel.kind == DefinitionKind::Frame && panel.frameType == "SIMPLEFRAME" && panel.name == "Panel",
        "frame header type and name");
    Require(panel.inherits == "PanelTemplate" && panel.inheritsWithChildren, "inherits with children");
    Require(panel.line == 9, "definition line counts block comment lines");
    Require(panel.properties.size() == 2, "frame properties");
    const FdfProperty& setPoint = panel.properties[1];
    Require(setPoint.values.size() == 5 && setPoint.values[1].text == "Other" && setPoint.values[1].quoted &&
            setPoint.values[4].text == "-0.02" && !setPoint.values[4].quoted,
        "SetPoint values split on commas");
    Require(panel.children.size() == 2, "layer and string children");
    const FdfDefinition& layer = panel.children[0];
    Require(layer.kind == DefinitionKind::Layer && layer.frameType == "BACKGROUND" && layer.children.size() == 1,
        "layer keeps its name and texture");
    Require(layer.children[0].properties.size() == 2 && layer.children[0].properties[1].values.size() == 4,
        "single-line texture block properties");
    Require(panel.children[1].kind == DefinitionKind::String && panel.children[1].inherits == "TextTemplate",
        "string inherits");
    Require(HasCode(parsed.diagnostics, "GAMEUI_FDF_TOPLEVEL_UNKNOWN"), "unknown top-level statement reported");
    Require(HasCode(parsed.diagnostics, "GAMEUI_FDF_UNBALANCED_BRACE"), "stray brace reported");

    const auto unterminated = ParseFdf("Frame \"FRAME\" \"Open\" {\n Width 0.1,\n", "Open.fdf");
    Require(!unterminated.Succeeded() && HasCode(unterminated.diagnostics, "GAMEUI_FDF_UNBALANCED_BRACE"),
        "missing closing brace is an error");
    Require(unterminated.value && unterminated.value->definitions.size() == 1, "partial definition kept for display");

    const auto toc = ParseToc("\xEF\xBB\xBFUI\\A.fdf\r\n\r\n// skip\r\n  UI\\B.fdf  \r\n");
    Require(toc.size() == 2 && toc[0] == "UI\\A.fdf" && toc[1] == "UI\\B.fdf", "toc lines trimmed");
}

void TestSkin() {
    UiSkin skin;
    Require(skin.Races().size() == 4, "default races without skins file");
    skin.SetGameSkins(
        "[Main]\nSkins=Orc,Human\n"
        "[Default]\nMasterFont=Fonts\\Default.ttf\nPanelBg=UI\\Default\\Bg\n"
        "[Human]\nPanelBg=UI\\Human\\Bg\nPanelBg_V1=UI\\Human\\BgX\n"
        "[Orc]\nPanelBg=UI\\Orc\\Bg\n");
    Require(skin.Races().size() == 2 && skin.Races()[0] == "Orc", "races from [Main]");
    auto human = skin.Lookup("PanelBg", "Human");
    Require(human && human->value == "UI\\Human\\BgX" && human->source == "game:human" && human->matchedKey == "panelbg_v1",
        "expansion prefers _V1");
    human = skin.Lookup("panelbg", "HUMAN", false);
    Require(human && human->value == "UI\\Human\\Bg", "classic ignores _V1, case-insensitive");
    const auto orc = skin.Lookup("PanelBg", "Orc");
    Require(orc && orc->value == "UI\\Orc\\Bg", "race without _V1 falls back to base key");
    const auto font = skin.Lookup("MasterFont", "Orc");
    Require(font && font->source == "game:default", "default section fallback");
    Require(!skin.Lookup("Missing", "Orc"), "missing key");

    skin.SetMapSkin("[CustomSkin]\nPanelBg=war3mapImported\\Bg.blp\n[FrameDef]\nKEY_OK=好\n");
    const auto custom = skin.Lookup("PanelBg", "Orc");
    Require(custom && custom->value == "war3mapImported\\Bg.blp" && custom->source == "map:customskin",
        "map CustomSkin overrides every race");
    Require(skin.MapFrameString("key_ok") == std::optional<std::string>("好"), "map FrameDef string");
}

void TestLibrary() {
    const auto reader = MemoryReader({
        {"ui\\framedef\\test.toc", "UI\\Main.fdf\nUI\\Missing.fdf\n"},
        {"ui\\main.fdf", "IncludeFile \"UI\\Base.fdf\",\nFrame \"FRAME\" \"Shared\" { Width 0.2, }\n"
                         "StringList { KEY_A \"Main\", }\n"},
        {"ui\\base.fdf", "Frame \"FRAME\" \"Shared\" { Width 0.1, }\nFrame \"FRAME\" \"BaseOnly\" { }\n"
                         "StringList { KEY_A \"Base\", KEY_B \"B\", }\nIncludeFile \"UI\\Main.fdf\",\n"},
    });
    FrameLibrary library;
    library.LoadToc(reader, "UI\\FrameDef\\Test.toc", "game");
    Require(library.Files().size() == 2, "each file loaded once despite mutual include");
    Require(library.Files()[0]->file.path == "UI\\Base.fdf", "included file registered first");
    const auto shared = library.FindTemplate("shared");
    Require(shared && shared->definition->properties[0].values[0].text == "0.2", "includer overrides included template");
    Require(library.FindTemplate("BaseOnly").has_value(), "included template visible");
    Require(library.FindString("key_a") == std::optional<std::string>("Main"), "later string wins");
    Require(library.FindString("KEY_B") == std::optional<std::string>("B"), "included string visible");
    Require(HasCode(library.Diagnostics(), "GAMEUI_FDF_UNREADABLE"), "missing toc entry reported");
    Require(HasCode(library.Diagnostics(), "GAMEUI_TEMPLATE_REDEFINED"), "redefinition reported");

    FrameLibrary missingToc;
    missingToc.LoadToc(reader, "UI\\None.toc", "game");
    Require(HasCode(missingToc.Diagnostics(), "GAMEUI_TOC_UNREADABLE"), "missing toc is reported");
}

FrameLibrary LayoutLibrary() {
    const std::string text =
        "StringList { KEY_OK \"OK\", }\n"
        "Texture \"ButtonBg\" { File \"ButtonSkin\", TexCoord 0, 0.5, 0.25, 0.75, AlphaMode \"BLEND\", }\n"
        "String \"ButtonFont\" { Font \"MasterFont\", 0.012, FontColor 1.0 0.8 0.0, }\n"
        "Frame \"SIMPLEBUTTON\" \"ButtonTemplate\" { Width 0.1, Height 0.02, NormalTexture \"ButtonBg\", NormalText \"ButtonFont\" \"KEY_OK\", }\n"
        "Frame \"SIMPLEFRAME\" \"BarTemplate\" {\n"
        "    DecorateFileNames,\n"
        "    Layer \"BACKGROUND\" { Texture \"BarBg\" { File \"PanelBg\", } }\n"
        "}\n"
        "Frame \"SIMPLEFRAME\" \"Bar\" INHERITS WITHCHILDREN \"BarTemplate\" {\n"
        "    Width 0.3,\n"
        "    Height 0.05,\n"
        "    Frame \"SIMPLEBUTTON\" \"BarButton\" INHERITS \"ButtonTemplate\" {\n"
        "        SetPoint TOPLEFT, \"Bar\", TOPLEFT, 0.01, -0.01,\n"
        "    }\n"
        "    Frame \"SIMPLEBUTTON\" \"BarButton2\" INHERITS \"ButtonTemplate\" {\n"
        "        SetPoint LEFT, \"BarButton\", RIGHT, 0.0, 0.0,\n"
        "    }\n"
        "    Layer \"ARTWORK\" {\n"
        "        String \"BarLabel\" INHERITS \"ButtonFont\" { Anchor RIGHT, -0.01, 0.0, Text \"KEY_MISSING\", }\n"
        "    }\n"
        "}\n"
        "Frame \"BACKDROP\" \"Box\" {\n"
        "    DecorateFileNames,\n"
        "    BackdropBackground \"PanelBg\",\n"
        "    BackdropEdgeFile \"UI\\Literal\\Edge.tga\",\n"
        "    SetPoint CENTER, \"Bar\", CENTER, 0.0, 0.1,\n"
        "    Width 0.1, Height 0.1,\n"
        "}\n"
        "Frame \"FRAME\" \"Loose\" { Width 0.1, Height 0.1, }\n"
        "Frame \"FRAME\" \"CycleA\" { Width 0.1, Height 0.1, SetPoint TOPLEFT, \"CycleB\", TOPLEFT, 0, 0, }\n"
        "Frame \"FRAME\" \"CycleB\" { Width 0.1, Height 0.1, SetPoint TOPLEFT, \"CycleA\", TOPLEFT, 0, 0, }\n"
        "Frame \"FRAME\" \"Orphan\" { Width 0.1, Height 0.1, SetPoint TOPLEFT, \"Nowhere\", TOPLEFT, 0, 0, }\n"
        "Frame \"FRAME\" \"SelfLoop\" INHERITS WITHCHILDREN \"SelfLoop\" { }\n";
    auto parsed = ParseFdf(text, "UI\\Layout.fdf");
    Require(parsed.Succeeded(), "layout fixture parses cleanly");
    FrameLibrary library;
    library.AddParsed(std::move(*parsed.value), "game");
    return library;
}

void TestTree() {
    const FrameLibrary library = LayoutLibrary();
    UiSkin skin;
    skin.SetGameSkins("[Default]\nMasterFont=Fonts\\Main.ttf\n[Human]\nPanelBg=UI\\Human\\Panel\nButtonSkin=UI\\Human\\Button.tga\n");

    FrameTree tree;
    Require(tree.Nodes().size() == 1 && Near(tree.Nodes()[0].rect.right, 0.8f), "screen node");
    RootPlacement bottomCenter;
    bottomCenter.anchors.push_back(FrameAnchor{FramePoint::Bottom, {}, FramePoint::Bottom, 0.0f, 0.1f});
    const auto bar = tree.Instantiate(library, &skin, "bar", bottomCenter);
    Require(bar.Succeeded() && bar.value, "instantiate bar");
    const auto box = tree.Instantiate(library, &skin, "Box");
    Require(box.Succeeded(), "instantiate box");
    const auto loose = tree.Instantiate(library, &skin, "Loose");
    Require(loose.value && HasCode(loose.diagnostics, "GAMEUI_ROOT_PLACEMENT_UNKNOWN"), "unknown root placement reported");
    (void)tree.Instantiate(library, &skin, "CycleA");
    (void)tree.Instantiate(library, &skin, "CycleB");
    (void)tree.Instantiate(library, &skin, "Orphan");
    const auto missing = tree.Instantiate(library, &skin, "NotThere");
    Require(!missing.Succeeded() && HasCode(missing.diagnostics, "GAMEUI_TEMPLATE_MISSING"), "missing template");
    const auto selfLoop = tree.Instantiate(library, &skin, "SelfLoop");
    Require(selfLoop.value && HasCode(selfLoop.diagnostics, "GAMEUI_TEMPLATE_CYCLE"), "self inheritance stopped");
    const auto diagnostics = tree.Layout();

    const FrameNode& barNode = NodeNamed(tree, "Bar");
    Require(barNode.layout == LayoutState::Placed && Near(barNode.rect.left, 0.25f) && Near(barNode.rect.right, 0.55f) &&
            Near(barNode.rect.bottom, 0.1f) && Near(barNode.rect.top, 0.15f),
        "root placement with size");
    Require(barNode.sourceFile == "UI\\Layout.fdf" && barNode.sourceLine == 9, "node source location");

    const FrameNode& barBg = NodeNamed(tree, "BarBg");
    Require(barBg.layer == "BACKGROUND" && barBg.decorate, "template child keeps layer and decoration");
    Require(barBg.texture && barBg.texture->path == "UI\\Human\\Panel.blp" && barBg.texture->source == "game:human",
        "decorated texture resolved through race skin with .blp added");
    Require(barBg.layout == LayoutState::Placed && Near(barBg.rect.left, barNode.rect.left) &&
            Near(barBg.rect.top, barNode.rect.top),
        "anchorless texture fills frame through layer");
    Require(tree.Nodes()[barBg.parent].kind == DefinitionKind::Layer, "texture parent is the layer node");

    const FrameNode& button = NodeNamed(tree, "BarButton");
    Require(button.layout == LayoutState::Placed && Near(button.rect.left, 0.26f) && Near(button.rect.top, 0.14f) &&
            Near(button.rect.Width(), 0.1f),
        "SetPoint relative to named parent");
    Require(button.texture && button.texture->path == "UI\\Human\\Button.tga" && Near(button.texture->texCoord[1], 0.5f) &&
            button.texture->alphaMode == "BLEND",
        "button normal texture from texture template, existing extension kept");
    Require(button.text && button.text->text == "OK" && button.text->source == "fdf" &&
            button.text->fontPath == "Fonts\\Main.ttf" && Near(button.text->fontHeight, 0.012f) &&
            Near(button.text->color[1], 0.8f),
        "button normal text and font");

    const FrameNode& button2 = NodeNamed(tree, "BarButton2");
    Require(button2.layout == LayoutState::Placed && Near(button2.rect.left, button.rect.right) &&
            Near(button2.rect.top, button.rect.top),
        "chained sibling anchor with inherited size");
    Require(button2.templateName == "ButtonTemplate", "template name recorded");

    const FrameNode& label = NodeNamed(tree, "BarLabel");
    Require(label.layout == LayoutState::SizeFromContent && label.contentAnchor == FramePoint::Right &&
            Near(label.rect.right, barNode.rect.right - 0.01f),
        "single-point string sized by content");
    Require(label.text && label.text->text == "KEY_MISSING" && label.text->source == "literal" &&
            label.text->fontKey == "MasterFont",
        "missing string key falls back to literal, font inherited");

    const FrameNode& boxNode = NodeNamed(tree, "Box");
    Require(boxNode.layout == LayoutState::Placed && Near(boxNode.rect.left, 0.35f) && Near(boxNode.rect.bottom, 0.175f),
        "center anchor relative to another root");
    Require(boxNode.texture && boxNode.texture->source == "game:human", "backdrop background decorated");
    Require(boxNode.edge && boxNode.edge->path == "UI\\Literal\\Edge.tga" && boxNode.edge->source == "literal",
        "literal path kept when skin has no key");

    Require(NodeNamed(tree, "Loose").layout == LayoutState::Unplaced, "root without placement stays unplaced");
    Require(NodeNamed(tree, "Orphan").layout == LayoutState::Unplaced, "missing target leaves frame unplaced");
    Require(HasCode(diagnostics, "GAMEUI_ANCHOR_TARGET_MISSING"), "missing anchor target reported");
    Require(HasCode(diagnostics, "GAMEUI_LAYOUT_CYCLE"), "anchor cycle reported");
    Require(NodeNamed(tree, "CycleA").layout != LayoutState::Placed, "cycle not placed");

    // 地图皮肤覆盖与 ConsoleUI 铺满屏幕。
    skin.SetMapSkin("[CustomSkin]\nPanelBg=war3mapImported\\Panel.blp\n[FrameDef]\nKEY_OK=确定\n");
    const std::string console = "Frame \"SIMPLEFRAME\" \"ConsoleUI\" { DecorateFileNames, Texture \"ConsoleBg\" { File \"PanelBg\", } }\n";
    auto parsed = ParseFdf(console, "UI\\Console.fdf");
    FrameLibrary consoleLibrary;
    consoleLibrary.AddParsed(std::move(*parsed.value), "game");
    FrameTree consoleTree;
    const auto consoleRoot = consoleTree.Instantiate(consoleLibrary, &skin, "ConsoleUI");
    Require(consoleRoot.Succeeded() && consoleRoot.diagnostics.empty(), "ConsoleUI has a known placement");
    (void)consoleTree.Layout();
    const FrameNode& consoleBg = NodeNamed(consoleTree, "ConsoleBg");
    Require(consoleBg.layout == LayoutState::Placed && Near(consoleBg.rect.Width(), 0.8f) && Near(consoleBg.rect.Height(), 0.6f),
        "ConsoleUI fills screen");
    Require(consoleBg.texture && consoleBg.texture->path == "war3mapImported\\Panel.blp" &&
            consoleBg.texture->source == "map:customskin",
        "map skin override applied");

    // 按钮属性引用的子框架由游戏铺满按钮；未被引用的子框架保持自身锚点。
    const std::string controls =
        "Frame \"GLUETEXTBUTTON\" \"MenuButtonTemplate\" {\n"
        "    Width 0.2, Height 0.03,\n"
        "    ControlBackdrop \"MenuBg\",\n"
        "    Frame \"BACKDROP\" \"MenuBg\" { DecorateFileNames, BackdropBackground \"PanelBg\", }\n"
        "    ControlMouseOverHighlight \"MenuGlow\",\n"
        "    Frame \"HIGHLIGHT\" \"MenuGlow\" { HighlightAlphaFile \"UI\\Glow.blp\", HighlightAlphaMode \"ADD\", }\n"
        "    Frame \"BACKDROP\" \"Unreferenced\" { }\n"
        "}\n"
        "Frame \"GLUETEXTBUTTON\" \"MenuButton\" INHERITS WITHCHILDREN \"MenuButtonTemplate\" {\n"
        "    SetPoint CENTER, \"MenuButton\", CENTER, 0, 0,\n"
        "    ButtonText \"MenuButtonText\",\n"
        "    Frame \"TEXT\" \"MenuButtonText\" { Text \"KEY_OK\", }\n"
        "}\n";
    auto controlParsed = ParseFdf(controls, "UI\\Controls.fdf");
    Require(controlParsed.Succeeded(), "control fixture parses");
    FrameLibrary controlLibrary;
    controlLibrary.AddParsed(std::move(*controlParsed.value), "game");
    FrameTree controlTree;
    RootPlacement center;
    center.anchors.push_back(FrameAnchor{FramePoint::Center, {}, FramePoint::Center, 0.0f, 0.0f});
    (void)controlTree.Instantiate(controlLibrary, &skin, "MenuButton", center);
    (void)controlTree.Layout();
    const FrameNode& menuButton = NodeNamed(controlTree, "MenuButton");
    Require(menuButton.layout == LayoutState::Placed && Near(menuButton.rect.left, 0.3f), "placement overrides own anchors");
    const FrameNode& menuBg = NodeNamed(controlTree, "MenuBg");
    Require(menuBg.autoFillBy == "ControlBackdrop" && menuBg.layout == LayoutState::Placed &&
            Near(menuBg.rect.left, menuButton.rect.left) && Near(menuBg.rect.top, menuButton.rect.top),
        "control backdrop fills the button");
    const FrameNode& menuGlow = NodeNamed(controlTree, "MenuGlow");
    Require(menuGlow.autoFillBy == "ControlMouseOverHighlight" && menuGlow.texture &&
            menuGlow.texture->path == "UI\\Glow.blp" && menuGlow.texture->alphaMode == "ADD",
        "highlight texture and fill");
    const FrameNode& menuText = NodeNamed(controlTree, "MenuButtonText");
    Require(menuText.autoFillBy == "ButtonText" && menuText.layout == LayoutState::Placed &&
            Near(menuText.rect.Width(), 0.2f),
        "button text fills the button");
    Require(NodeNamed(controlTree, "Unreferenced").layout == LayoutState::Unplaced, "unreferenced child not auto-placed");

    FrameTree mapText;
    (void)mapText.Instantiate(library, &skin, "Bar");
    Require(NodeNamed(mapText, "BarButton").text->text == "确定" &&
            NodeNamed(mapText, "BarButton").text->source == "map:framedef",
        "map FrameDef string overrides fdf string");
}

void TestDrawList() {
    const std::string text =
        "Frame \"SIMPLEFRAME\" \"Panel\" {\n"
        "    Width 0.4, Height 0.2,\n"
        "    String \"PanelLabel\" { Text \"Hi\", Font \"F.ttf\", 0.01, FontJustificationH JUSTIFYLEFT, }\n"
        "    Layer \"OVERLAY\" { Texture \"PanelTop\" { File \"Top.blp\", AlphaMode \"ADD\", } }\n"
        "    Layer \"BACKGROUND\" { Texture \"PanelBack\" { File \"Back.blp\", TexCoord 0.1, 0.9, 0.2, 0.8, } }\n"
        "    Frame \"BACKDROP\" \"PanelBox\" {\n"
        "        SetAllPoints,\n"
        "        BackdropBackground \"Bg.blp\",\n"
        "        BackdropTileBackground,\n"
        "        BackdropBackgroundSize 0.1,\n"
        "        BackdropBackgroundInsets 0.01 0.01 0.01 0.01,\n"
        "        BackdropEdgeFile \"Edge.blp\",\n"
        "        BackdropCornerSize 0.02,\n"
        "        BackdropCornerFlags \"UL|UR|BL|BR|T|L|B|R\",\n"
        "    }\n"
        "    Frame \"GLUETEXTBUTTON\" \"PanelButton\" {\n"
        "        SetAllPoints,\n"
        "        ControlMouseOverHighlight \"PanelGlow\",\n"
        "        Frame \"HIGHLIGHT\" \"PanelGlow\" { HighlightAlphaFile \"Glow.blp\", }\n"
        "    }\n"
        "}\n";
    auto parsed = ParseFdf(text, "UI\\Draw.fdf");
    Require(parsed.Succeeded(), "draw fixture parses");
    FrameLibrary library;
    library.AddParsed(std::move(*parsed.value), "game");
    FrameTree tree;
    RootPlacement bottomLeft;
    bottomLeft.anchors.push_back(FrameAnchor{FramePoint::BottomLeft, {}, FramePoint::BottomLeft, 0.0f, 0.0f});
    const auto root = tree.Instantiate(library, nullptr, "Panel", bottomLeft);
    Require(root.Succeeded(), "instantiate draw fixture");
    (void)tree.Layout();

    const FrameDrawList list = BuildDrawList(tree);
    Require(list.commands.size() == 1 + 1 + 1 + 1 + 8, "layered images, text, backdrop background and eight edge pieces");
    const auto nodeOf = [&](std::size_t index) { return tree.Nodes()[list.commands[index].node].name; };
    Require(nodeOf(0) == "PanelBack" && nodeOf(1) == "PanelLabel" && nodeOf(2) == "PanelTop",
        "layers ordered BACKGROUND, ARTWORK (no layer), OVERLAY");
    Require(list.commands[0].image.uv[0].u == 0.1f && list.commands[0].image.uv[3].v == 0.8f, "texcoord corners");
    Require(list.commands[2].image.blend == DrawBlend::Add, "alpha mode");
    const TextDraw& label = list.commands[1].text;
    Require(list.commands[1].kind == DrawKind::Text && label.text == "Hi" && label.justifyH == JustifyH::Left &&
            label.justifyV == JustifyV::Middle && !label.sizeFromContent,
        "text command with justification");

    const ImageDraw& background = list.commands[3].image;
    Require(nodeOf(3) == "PanelBox" && background.repeat && Near(background.rect.left, 0.01f) &&
            Near(background.uv[3].u, 3.8f) && Near(background.uv[3].v, 1.8f),
        "tiled backdrop background inset and repeated by tile size");
    // 第 4 个起依次为 L、R、T、B、UL、UR、BL、BR。
    const ImageDraw& left = list.commands[4].image;
    Require(left.path == "Edge.blp" && Near(left.rect.Width(), 0.02f) && Near(left.uv[0].u, 0.0f) &&
            Near(left.uv[1].u, 0.125f) && Near(left.uv[2].v, 8.0f),
        "left edge uses cell 0 repeated along height");
    const ImageDraw& top = list.commands[6].image;
    Require(Near(top.rect.Height(), 0.02f) && Near(top.rect.top, 0.2f) && Near(top.uv[0].u, 0.25f) &&
            Near(top.uv[0].v, 18.0f) && Near(top.uv[1].v, 0.0f) && Near(top.uv[2].u, 0.375f),
        "top edge uses cell 2 rotated clockwise");
    const ImageDraw& lowerRight = list.commands[11].image;
    Require(!lowerRight.repeat && Near(lowerRight.rect.right, 0.4f) && Near(lowerRight.rect.bottom, 0.0f) &&
            Near(lowerRight.uv[0].u, 0.875f),
        "lower right corner uses cell 7");
    Require(list.stateOnlyNodes.size() == 1 && tree.Nodes()[list.stateOnlyNodes[0]].name == "PanelGlow",
        "mouse-over highlight not drawn in normal state");

    FrameDrawOptions options;
    options.hidden.insert(*tree.FindByName("PanelBox"));
    options.hidden.insert(*tree.FindByName("PanelBack"));
    const FrameDrawList hidden = BuildDrawList(tree, options);
    Require(hidden.commands.size() == 2, "hidden subtree and texture skipped");
    options.hidden = {*root.value};
    Require(BuildDrawList(tree, options).commands.empty(), "hidden root hides everything");
}

void RequireRect(const FrameNode& node, float left, float bottom, float right, float top, const char* reason) {
    Require(node.layout == LayoutState::Placed && Near(node.rect.left, left) && Near(node.rect.bottom, bottom) &&
            Near(node.rect.right, right) && Near(node.rect.top, top),
        reason);
}

void TestGameCodeLayout() {
    const std::string text =
        "Frame \"SIMPLEFRAME\" \"ResourceBarFrame\" { Width 0.3, Height 0.03, }\n"
        "Frame \"FRAME\" \"EscMenuMainPanel\" {\n"
        "    SetAllPoints,\n"
        "    Frame \"FRAME\" \"ReturnButton\" {\n"
        "        Width 0.2, Height 0.03,\n"
        "        SetPoint BOTTOM, \"EscMenuMainPanel\", BOTTOM, 0.0, 0.03,\n"
        "    }\n"
        "}\n"
        "Frame \"FRAME\" \"QuestDialog\" { Width 0.48, Height 0.4, }\n"
        "Frame \"FRAME\" \"TimerDialog\" { Width 0.17, Height 0.03, }\n"
        "Frame \"FRAME\" \"Multiboard\" { Width 0.024, Height 0.024, }\n"
        "Frame \"FRAME\" \"Leaderboard\" { Width 0.17, Height 0.2, }\n"
        "Frame \"DIALOG\" \"ScriptDialog\" { Width 0.288, Height 0.112, }\n"
        "Frame \"FRAME\" \"MapPanel\" { Width 0.1, Height 0.1, SetPoint BOTTOM, \"Minimap\", TOP, 0.0, 0.0, }\n";
    auto parsed = ParseFdf(text, "UI\\Code.fdf");
    Require(parsed.Succeeded(), "code layout fixture parses");
    FrameLibrary library;
    library.AddParsed(std::move(*parsed.value), "game");
    FrameTree tree;
    for (const char* name : {"ResourceBarFrame", "EscMenuMainPanel", "QuestDialog", "TimerDialog", "Multiboard", "Leaderboard",
             "ScriptDialog"}) {
        const auto created = tree.Instantiate(library, nullptr, name);
        Require(created.Succeeded() && !HasCode(created.diagnostics, "GAMEUI_ROOT_PLACEMENT_UNKNOWN"),
            "game-code placement known");
    }
    const auto codeRoot = tree.AddGameCodeAreas();
    Require(tree.AddGameCodeAreas() == codeRoot, "code areas added once");
    Require(tree.Instantiate(library, nullptr, "MapPanel").Succeeded(), "instantiate map panel");
    const auto layout = tree.Layout();

    RequireRect(NodeNamed(tree, "ResourceBarFrame"), 0.5f, 0.57f, 0.8f, 0.6f, "resource bar at top right");
    Require(NodeNamed(tree, "ResourceBarFrame").codePlaced, "placement marked as game code");
    // Esc 菜单面板随背景框：0.288×0.384，中心在屏幕中心上方 0.06。
    RequireRect(NodeNamed(tree, "EscMenuMainPanel"), 0.256f, 0.168f, 0.544f, 0.552f, "esc menu follows backdrop");
    RequireRect(NodeNamed(tree, "ReturnButton"), 0.3f, 0.198f, 0.5f, 0.228f, "return button inside the box");
    RequireRect(NodeNamed(tree, "QuestDialog"), 0.16f, 0.164f, 0.64f, 0.564f, "quest dialog centered and raised");
    // 右上角一组与脚本对话框相对世界画面（上沿在屏幕顶下 0.02，下沿在屏幕底上 0.13）。
    RequireRect(NodeNamed(tree, "TimerDialog"), 0.625f, 0.535f, 0.795f, 0.565f, "timer dialog under the resource bar");
    RequireRect(NodeNamed(tree, "Multiboard"), 0.7761f, 0.5528f, 0.8001f, 0.5768f, "multiboard at top right");
    RequireRect(NodeNamed(tree, "Leaderboard"), 0.625f, 0.365f, 0.795f, 0.565f, "leaderboard at top right");
    RequireRect(NodeNamed(tree, "ScriptDialog"), 0.256f, 0.299f, 0.544f, 0.411f, "script dialog centered on the world view");

    RequireRect(NodeNamed(tree, "Minimap"), 0.009375f, 0.006875f, 0.14875f, 0.145f, "minimap");
    RequireRect(NodeNamed(tree, "CommandBar"), 0.618125f, 0.002f, 0.792625f, 0.131f, "command card");
    RequireRect(NodeNamed(tree, "InventoryButton1"), 0.51625f, 0.08062500f, 0.5475f, 0.111875f, "first item slot");
    RequireRect(NodeNamed(tree, "InventoryButton6"), 0.55625f, 0.0043750f, 0.5875f, 0.035625f, "last item slot");
    RequireRect(NodeNamed(tree, "UnitMessage"), 0.051875f, 0.2305f, 0.551875f, 0.4805f, "unit message above message");
    RequireRect(NodeNamed(tree, "ChatEditBar"), 0.2f, 0.13f, 0.6f, 0.16f, "chat edit bar");
    RequireRect(NodeNamed(tree, "ChatMessage"), 0.00625f, 0.2f, 0.78f, 0.6f, "chat messages");
    Require(!NodeNamed(tree, "Minimap").codeLabel.empty() && NodeNamed(tree, "Minimap").frameType == "CMinimap",
        "code area label and class");
    // 代码区域在游戏里没有名字，fdf 按名字引用不到。
    Require(NodeNamed(tree, "MapPanel").layout == LayoutState::Unplaced && HasCode(layout, "GAMEUI_ANCHOR_TARGET_MISSING"),
        "fdf cannot anchor to code areas by name");

    const FrameDrawList list = BuildDrawList(tree);
    std::size_t areas = 0;
    for (const auto& command : list.commands) {
        if (command.kind != DrawKind::CodeArea) continue;
        ++areas;
        Require(command.node != codeRoot, "code root itself not drawn");
    }
    Require(areas == GameCodeAreas().size(), "each code area drawn as placeholder");
    FrameDrawOptions options;
    options.hidden.insert(codeRoot);
    for (const auto& command : BuildDrawList(tree, options).commands) {
        Require(command.kind != DrawKind::CodeArea, "hidden code root hides its areas");
    }
}

void TestMapTocs() {
    const MapScriptText jass{"war3map.j", ScriptLanguage::Jass,
        "function InitUi takes nothing returns nothing\n"
        "    // call DzLoadToc(\"ui\\\\commented.toc\")\n"
        "    call DzLoadToc(\"ui\\\\my.toc\")\n"
        "    call BJDebugMsg(\"DzLoadToc(x)\")\n"
        "    call DzLoadToc(\"ui\\\\\" + s + \".toc\")\n"
        "    call BlzLoadTOCFile(\"UI\\\\Extra.toc\")\n"
        "endfunction\n"};
    const auto calls = FindTocLoadCalls(jass);
    Require(calls.size() == 3, "comment and string text are not calls");
    Require(calls[0].function == "DzLoadToc" && calls[0].path == "ui\\my.toc" && calls[0].line == 3,
        "literal argument decoded with its line");
    Require(calls[1].path.empty(), "concatenated argument is dynamic");
    Require(calls[2].function == "BlzLoadTOCFile" && calls[2].path == "UI\\Extra.toc", "native toc loader");

    const MapScriptText lua{"war3map.lua", ScriptLanguage::Lua,
        "-- DzLoadToc('commented.toc')\n"
        "DzLoadToc('ui\\\\lua.toc')\n"
        "DzLoadToc \"ui\\\\bare.toc\"\n"
        "lib.DzLoadToc('field.toc')\n"
        "local f = DzLoadToc\n"};
    const auto luaCalls = FindTocLoadCalls(lua);
    Require(luaCalls.size() == 2 && luaCalls[0].path == "ui\\lua.toc" && luaCalls[1].path == "ui\\bare.toc",
        "lua call forms; field access and function reference skipped");

    const auto plan = PlanMapTocs({"UI\\MY.toc", "ui\\spare.toc", "war3map.j", "ui\\my.fdf"}, {jass});
    Require(plan.tocs.size() == 3 && plan.tocs[0] == "ui\\my.toc" && plan.tocs[1] == "UI\\Extra.toc" &&
            plan.tocs[2] == "ui\\spare.toc",
        "script order first, unreferenced map toc last, no duplicate by case");
    Require(HasCode(plan.diagnostics, "GAMEUI_MAP_TOC_DYNAMIC") && HasCode(plan.diagnostics, "GAMEUI_MAP_TOC_UNREFERENCED"),
        "dynamic call and unreferenced toc reported");
    const MapScriptText memHack{"war3map.j", ScriptLanguage::Jass,
        "function A takes nothing returns nothing\n    call MHFrame_LoadTOC(\"ui\\\\mh.toc\")\nendfunction\n"};
    const auto memHackCalls = FindTocLoadCalls(memHack);
    Require(memHackCalls.size() == 1 && memHackCalls[0].path == "ui\\mh.toc", "MemHack toc loader");
}

const ScriptFrame& FrameCreatedAt(const ScriptFramePlan& plan, std::uint32_t line) {
    for (const auto& frame : plan.frames) {
        if (frame.created.line == line) return frame;
    }
    throw std::runtime_error("script frame missing at line " + std::to_string(line));
}

const ScriptUiCallGroup* CallGroup(const ScriptFramePlan& plan, std::string_view function) {
    for (const auto& group : plan.calls) {
        if (group.function == function) return &group;
    }
    return nullptr;
}

void TestScriptFrames() {
    // 原创小脚本：三类接口各用一种写法；包装函数本身不算使用方。
    const MapScriptText script{"war3map.j", ScriptLanguage::Jass, R"(globals
    integer udg_Back = 0
    integer udg_Label = 0
    integer array udg_Bars
    constant real BAR_Y = 0.19
endglobals
function MHFrame_CreateEx takes string type_name, string name, string base_frame, integer parent, integer priority, integer id returns integer
    call MHFrame_SetText(parent, "wrapper body is not a use")
    return 0
endfunction
function InitUi takes nothing returns nothing
    local integer i = 0
    set udg_Back = MHFrame_CreateEx("BACKDROP", "Back", "", MHUI_GetGameUI(), 1000, 0)
    call MHFrame_SetTexture(udg_Back, "UI\\Back.blp", false)
    call MHFrame_SetSize(udg_Back, 0.2, 0.02)
    call MHFrame_SetAbsolutePoint(udg_Back, 0x4, 0.4, BAR_Y)
    call MHFrame_Hide(udg_Back, true)
    set udg_Label = DzCreateFrameByTagName("TEXT", "Label", DzGetGameUI(), "", 0)
    call DzFrameSetPoint(udg_Label, 3, udg_Back, 3, 0.01, 0.0)
    call DzFrameSetSize(udg_Label, 0.1, 0.012)
    call DzFrameSetText(udg_Label, "Cast")
    loop
        exitwhen i >= 2
        set udg_Bars[i] = BlzCreateFrameByType("BACKDROP", "Bar" + I2S(i), BlzGetOriginFrame(ORIGIN_FRAME_GAME_UI, 0), "", 0)
        call BlzFrameSetAbsPoint(udg_Bars[i], FRAMEPOINT_BOTTOMLEFT, 0.1, 0.1 * i)
        call BlzFrameSetSize(udg_Bars[i], 0.05, -(-0.01))
        set i = i + 1
    endloop
    if udg_Back != 0 then
        call MHFrame_SetWidth(udg_Back, 0.5)
    endif
    call DzFrameHideInterface()
    call DzFrameShow(DzFrameFindByName("ConsoleUI", 0), false)
    call MHUI_DrawMoveSpeed(true)
    call DzFrameSetAlpha(udg_Label, 128)
endfunction
function Later takes nothing returns nothing
    call MHFrame_Hide(udg_Back, false)
    call DzFrameSetText(udg_Label, I2S(3))
endfunction
)"};
    const auto plan = FindScriptFrames({script});
    Require(plan.frames.size() == 3, "three frames created by script; wrapper body skipped");

    const ScriptFrame& back = FrameCreatedAt(plan, 13);
    Require(back.family == UiApiFamily::MemHack && back.variable == "udg_Back" && back.name == "Back" &&
            back.frameType == "BACKDROP" && back.templateName.empty(),
        "MemHack frame by type with literal name");
    Require(back.parent.target == ScriptFrameTarget::CodeArea && back.parent.name == "GameUI", "parent is the game UI");
    Require(back.texture && *back.texture == "UI\\Back.blp", "texture literal decoded");
    Require(back.width && Near(*back.width, 0.2f) && back.height && Near(*back.height, 0.02f),
        "size from the same block; width set inside if is not applied");
    Require(back.anchors.size() == 1 && back.anchors[0].point == FramePoint::Center &&
            back.anchors[0].relative.target == ScriptFrameTarget::Screen && Near(back.anchors[0].y, 0.19f),
        "absolute point with hex point index and global constant");
    Require(back.visible && !*back.visible, "hidden at creation");
    Require(back.runtimeCalls == 2, "conditional width and later show are runtime calls");
    Require(back.runtimeFunctions == std::vector<std::string>{"InitUi", "Later"}, "functions with runtime calls listed once");

    const ScriptFrame& label = FrameCreatedAt(plan, 18);
    Require(label.family == UiApiFamily::Dz && label.text && *label.text == "Cast", "DzAPI text frame");
    Require(label.anchors.size() == 1 && label.anchors[0].point == FramePoint::Left &&
            label.anchors[0].relative.target == ScriptFrameTarget::Script && label.anchors[0].relative.frame == 0 &&
            Near(label.anchors[0].x, 0.01f),
        "relative point to another script frame");
    Require(label.runtimeCalls == 1, "text set from another function is a runtime call");

    const ScriptFrame& bar = FrameCreatedAt(plan, 24);
    Require(bar.nameDynamic && bar.variable == "udg_Bars[i]", "loop-created frame keeps its variable");
    Require(bar.anchors.empty() && bar.dynamic.size() == 1, "non-constant y is reported, not guessed");
    Require(bar.width && Near(*bar.width, 0.05f) && bar.height && Near(*bar.height, 0.01f), "constant folding of unary minus");
    Require(bar.parent.target == ScriptFrameTarget::CodeArea && bar.parent.name == "GameUI", "origin frame resolved");

    const auto* hide = CallGroup(plan, "DzFrameHideInterface");
    Require(hide && hide->reason == ScriptUiCallReason::NotPreviewed && hide->count == 1, "known call not previewed");
    const auto* show = CallGroup(plan, "DzFrameShow");
    Require(show && show->reason == ScriptUiCallReason::UntracedTarget, "stock frame by name is untraced");
    const auto* speed = CallGroup(plan, "MHUI_DrawMoveSpeed");
    Require(speed && speed->reason == ScriptUiCallReason::Unknown && speed->samples[0].line == 34, "unknown call listed");
    const auto* alpha = CallGroup(plan, "DzFrameSetAlpha");
    Require(alpha && alpha->reason == ScriptUiCallReason::Unknown, "unlisted family call listed as unknown");
    Require(!CallGroup(plan, "MHFrame_SetText"), "wrapper bodies are not analysed");
    Require(ScriptCallDiagnostics(plan).size() == plan.calls.size(), "one problem row per call group");

    FrameLibrary library;
    FrameTree tree;
    tree.AddGameCodeAreas();
    std::vector<Diagnostic> diagnostics;
    const auto root = AddScriptFrames(tree, library, nullptr, plan, diagnostics);
    Require(root != kNoNode && tree.Nodes()[root].name == kScriptRoot, "script root added");
    (void)tree.Layout();
    const FrameNode& backNode = NodeNamed(tree, "Back");
    Require(backNode.scriptFrame && *backNode.scriptFrame == 0, "node keeps its script frame index");
    RequireRect(backNode, 0.3f, 0.18f, 0.5f, 0.2f, "absolute center with script size");
    Require(backNode.texture && backNode.texture->source == "script", "texture comes from the script");
    const FrameNode& labelNode = NodeNamed(tree, "Label");
    RequireRect(labelNode, 0.31f, 0.184f, 0.41f, 0.196f, "anchored to the other script frame");
    Require(labelNode.text && labelNode.text->text == "Cast", "text node shows script text");
    Require(tree.Nodes()[tree.FindByName("udg_Bars[i]").value()].layout == LayoutState::Unplaced,
        "frame without constant position stays unplaced");
    Require(FindScriptFrames({}).frames.empty(), "no scripts, no frames");
}

const SimulationInput* InputNamed(const SimulationResult& result, std::string_view key) {
    for (const auto& input : result.inputs) {
        if (input.key == key) return &input;
    }
    return nullptr;
}

void TestScriptSimulation() {
    // 原创读条脚本：创建在一个函数里，更新在另一个函数里，依赖计时器、数组与本地玩家判断。
    const MapScriptText script{"war3map.j", ScriptLanguage::Jass, R"(globals
    integer udg_Fill = 0
    integer udg_Text = 0
    real udg_Ratio = 0.0
    boolean array udg_Channel
    string array udg_Name
    integer udg_Local = 0
    timer udg_Clock = null
endglobals
function InitBar takes nothing returns nothing
    set udg_Fill = MHFrame_CreateEx("BACKDROP", "Fill", "", MHUI_GetGameUI(), 1000, 0)
    call MHFrame_SetSize(udg_Fill, 0.2, 0.012)
    call MHFrame_SetAbsolutePoint(udg_Fill, 3, 0.3, 0.19)
    call MHFrame_Hide(udg_Fill, true)
    set udg_Text = MHFrame_CreateEx("TEXT", "Label", "", MHUI_GetGameUI(), 1001, 0)
    call MHFrame_SetAbsolutePoint(udg_Text, 4, 0.4, 0.19)
endfunction
function UpdateBar takes nothing returns nothing
    local real left = 3.0 - TimerGetElapsed(udg_Clock)
    if udg_Channel[udg_Local] then
        call MHFrame_SetTexture(udg_Fill, "TeamColor09.blp", false)
    else
        call MHFrame_SetTexture(udg_Fill, "TeamColor04.blp", false)
    endif
    call MHFrame_Hide(udg_Fill, false)
    set udg_Ratio = left / 3.0
    call MHFrame_SetWidth(udg_Fill, 0.2 * udg_Ratio)
    call MHFrame_SetText(udg_Text, udg_Name[udg_Local] + "  " + R2SW(left, 1, 1))
    if GetLocalPlayer() == Player(0) then
        call MHFrame_SetHeight(udg_Fill, 0.02)
    endif
endfunction
)"};
    const auto plan = FindScriptFrames({script});
    Require(plan.frames.size() == 2, "two frames for simulation");
    Require(SimulationFunctions(plan) == std::vector<std::string>{"UpdateBar"}, "update function offered for simulation");

    auto prepared = ScriptSimulator::Prepare({script}, plan, "UpdateBar");
    Require(prepared.value.has_value(), "simulator prepared");
    const ScriptSimulator& simulator = *prepared.value;

    const auto defaults = simulator.Run({});
    Require(defaults.inputs.size() == 4, "timer, channel flag, name and local-player condition are inputs");
    const auto* elapsed = InputNamed(defaults, "TimerGetElapsed(udg_Clock)");
    Require(elapsed && elapsed->type == SimulationValueType::Number && !elapsed->integer, "unknown call result is a number input");
    const auto* channel = InputNamed(defaults, "udg_Channel[udg_Local]");
    Require(channel && channel->type == SimulationValueType::Boolean, "declared boolean array is a boolean input");
    const auto* name = InputNamed(defaults, "udg_Name[udg_Local]");
    Require(name && name->type == SimulationValueType::Text, "declared string array is a text input");
    const auto* local = InputNamed(defaults, "if:29");
    Require(local && local->condition && local->type == SimulationValueType::Boolean,
        "handle comparison becomes a branch choice");
    const ScriptFrame& fill = defaults.frames[0];
    Require(fill.texture && *fill.texture == "TeamColor04.blp", "else branch by default");
    Require(fill.visible && *fill.visible, "shown by the update");
    Require(fill.width && Near(*fill.width, 0.2f) && fill.height && Near(*fill.height, 0.012f), "full bar at zero elapsed");
    Require(defaults.frames[1].text && *defaults.frames[1].text == "  3.0", "R2SW formats like the game");
    Require(defaults.touched == std::vector<std::uint32_t>{0, 1}, "both frames touched");

    const auto changed = simulator.Run({{"TimerGetElapsed(udg_Clock)", "1.5"}, {"udg_Channel[udg_Local]", "true"},
        {"udg_Name[udg_Local]", "Hex"}, {"if:29", "true"}});
    Require(changed.frames[0].texture && *changed.frames[0].texture == "TeamColor09.blp", "channel branch chosen");
    Require(changed.frames[0].width && Near(*changed.frames[0].width, 0.1f), "width follows the elapsed input");
    Require(changed.frames[0].height && Near(*changed.frames[0].height, 0.02f), "branch input selects the then branch");
    Require(changed.frames[1].text && *changed.frames[1].text == "Hex  1.5", "text built from inputs");
    Require(plan.frames[0].texture == std::nullopt, "plan itself is not modified");

    auto missing = ScriptSimulator::Prepare({script}, plan, "NoSuchFunction");
    Require(!missing.value && HasCode(missing.diagnostics, "GAMEUI_SIMULATION_FUNCTION_MISSING"), "missing function reported");
}

} // namespace

int main() {
    try {
        TestParser();
        TestSkin();
        TestLibrary();
        TestTree();
        TestDrawList();
        TestGameCodeLayout();
        TestMapTocs();
        TestScriptFrames();
        TestScriptSimulation();
        std::cout << "Game UI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
