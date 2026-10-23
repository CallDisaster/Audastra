#include "War3MapCore/GameUi/FrameDraw.h"
#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "GameUiTestSupport.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::GameUi::Testing;

bool HasMessage(const std::vector<Diagnostic>& diagnostics, std::string_view text) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) return true;
    }
    return false;
}

// 原创读条描述：底框、按进度裁剪的填充、技能名与剩余时间，以及“打断”状态。
const char* const kCastBar = R"({
  "format": "audastra.ui",
  "version": 1,
  "id": "CastBar",
  "label": "读条",
  "values": [
    {"id": "progress", "label": "进度", "type": "real", "min": 0, "max": 1},
    {"id": "spell", "label": "技能名", "type": "string", "default": "火球术"},
    {"id": "remain", "type": "real", "default": 1.5},
    {"id": "casting", "type": "boolean", "default": true}
  ],
  "frames": [
    {
      "id": "back",
      "type": "backdrop",
      "anchors": [{"point": "BOTTOM", "to": "screen", "y": 0.18}],
      "width": 0.2,
      "height": 0.02,
      "texture": "UI\\Widgets\\BarBack",
      "visible": "casting",
      "children": [
        {"id": "fill", "type": "bar", "anchors": [{"point": "LEFT", "x": 0.002}], "width": 0.196, "height": 0.016,
         "texture": "ReplaceableTextures\\TeamColor\\TeamColor04", "value": "progress"},
        {"id": "label", "type": "text", "anchors": [{"point": "CENTER"}], "text": "{spell}  {remain:1}",
         "fontHeight": 0.01, "color": [1, 0.9, 0.5, 1]}
      ]
    }
  ],
  "states": [
    {"id": "interrupted", "label": "打断", "set": [
      {"frame": "fill", "texture": "ReplaceableTextures\\TeamColor\\TeamColor00", "value": 1},
      {"frame": "label", "text": "{spell}  打断"}
    ]}
  ]
})";

UiDescription Parse(std::string_view text) {
    auto parsed = ParseUiDescription(text, "test.json");
    if (!parsed.value) {
        for (const auto& diagnostic : parsed.diagnostics) std::cerr << diagnostic.code << " " << diagnostic.message << "\n";
        throw std::runtime_error("description should parse");
    }
    return std::move(*parsed.value);
}

std::vector<Diagnostic> Errors(std::string_view text) {
    auto parsed = ParseUiDescription(text, "test.json");
    Require(!parsed.value, "invalid description must not parse");
    return parsed.diagnostics;
}

void TestRoundTrip() {
    const UiDescription description = Parse(kCastBar);
    Require(description.id == "CastBar" && description.values.size() == 4 && description.frames.size() == 1, "top level");
    Require(description.values[0].defaultValue == "0" && description.values[2].defaultValue == "1.5" &&
                description.values[3].defaultValue == "true",
        "defaults normalized");
    const UiFrame& back = description.frames[0];
    Require(back.anchors[0].target == UiAnchorTarget::Screen && back.anchors[0].targetPoint == FramePoint::Bottom,
        "toPoint defaults to point");
    Require(back.visible.binding && back.visible.binding->value == "casting", "visible bound");
    Require(back.children[0].value.binding && back.children[0].value.binding->value == "progress", "bar bound");
    Require(back.children[1].color[1] == 0.9 && back.children[1].fontHeight == 0.01, "text style");

    const std::string text = SerializeUiDescription(description);
    const UiDescription again = Parse(text);
    Require(again == description, "serialize then parse keeps the model");
    Require(SerializeUiDescription(again) == text, "canonical output is stable");
    Require(text.find("\"value\": \"progress\"") != std::string::npos && text.find("\"y\": 0.18") != std::string::npos,
        "binding shorthand and shortest numbers");
    Require(text.find("toPoint") == std::string::npos && text.find("\"x\": 0,") == std::string::npos, "defaults omitted");
    Require(text.ends_with("}\n") && text.find('\r') == std::string::npos, "LF output");
}

void TestErrors() {
    const std::string base = R"({"format": "audastra.ui", "version": 1, "id": "T", "values": [{"id": "v", "type": "real"}],)";
    Require(HasCode(Errors("{"), "AUI_JSON"), "invalid json");
    Require(HasCode(Errors(R"({"format": "other", "version": 1, "id": "T"})"), "AUI_FORMAT"), "wrong format");
    Require(HasCode(Errors(R"({"format": "audastra.ui", "version": 2, "id": "T"})"), "AUI_FORMAT"), "future version");
    auto unknown = Errors(base + R"( "frames": [{"id": "a", "type": "panel", "colour": 1}]})");
    Require(HasCode(unknown, "AUI_FIELD_UNKNOWN") && HasMessage(unknown, "/frames/0/colour"), "unknown field with path");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "panel"}, {"id": "a", "type": "panel"}]})"),
                "AUI_ID_DUPLICATE"),
        "duplicate frame id");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "parent", "type": "panel"}]})"), "AUI_ID_INVALID"), "reserved id");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "panel", "anchors": [{"point": "TOP", "to": "b"}]}]})"),
                "AUI_ANCHOR_TARGET"),
        "anchor to missing frame");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "panel", "anchors": [{"point": "TOP", "to": "game:Nowhere"}]}]})"),
                "AUI_ANCHOR_TARGET"),
        "unknown game area");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "bar", "value": "w"}]})"), "AUI_VALUE_UNKNOWN"),
        "unknown value");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "bar", "value": "v[]"}]})"), "AUI_BINDING"),
        "per-instance on scalar");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "text", "text": "{v"}]})"), "AUI_TEXT_TEMPLATE"),
        "unterminated placeholder");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "panel", "text": "x"}]})"), "AUI_FIELD_UNKNOWN"),
        "text on panel");
    Require(HasCode(Errors(base + R"( "frames": [{"id": "a", "type": "panel"}], "events": [{"id": "e", "frame": "a", "on": "click"}]})"),
                "AUI_EVENT_FRAME"),
        "click needs a button");
    Require(HasCode(Errors(R"({"format": "audastra.ui", "version": 1, "id": "T", "values": [{"id": "v", "type": "integer", "default": 1.5}]})"),
                "AUI_TYPE"),
        "default must match type");
    const std::string arrays = R"({"format": "audastra.ui", "version": 1, "id": "T", "values": [{"id": "hp", "type": "real", "size": 2}],)";
    Require(HasCode(Errors(arrays + R"( "frames": [{"id": "s", "type": "panel", "repeat": {"count": 3}, "children": [{"id": "b", "type": "bar", "value": "hp[]"}]}]})"),
                "AUI_BINDING"),
        "array smaller than repeat");
    Require(HasCode(Errors(arrays + R"( "frames": [{"id": "b", "type": "bar", "value": "hp"}]})"), "AUI_BINDING"),
        "array needs an index");
}

void TestValues() {
    Require(NormalizeUiValue(UiValueType::Real, " 0.50 ") == std::optional<std::string>("0.5"), "real normalized");
    Require(NormalizeUiValue(UiValueType::Real, "nan") == std::nullopt, "real rejects nan");
    Require(NormalizeUiValue(UiValueType::Integer, "3.0") == std::optional<std::string>("3"), "integral real accepted");
    Require(NormalizeUiValue(UiValueType::Integer, "3.5") == std::nullopt, "fraction rejected for integer");
    Require(NormalizeUiValue(UiValueType::Boolean, "1") == std::nullopt, "boolean only true/false");
    const UiDescription description = Parse(kCastBar);
    UiInstanceState state;
    Require(FormatUiText(description, state, "{spell}  {remain:1}") == "火球术  1.5", "defaults formatted");
    state.values["remain"] = "0.25";
    state.values["spell"] = "暗影箭";
    Require(FormatUiText(description, state, "{spell} {remain} {remain:2} {{x}}") == "暗影箭 0.25 0.25 {x}", "inputs formatted");
    state.values["remain"] = "bad";
    Require(FormatUiText(description, state, "{remain}") == "1.5", "invalid input falls back to default");
    Require(UiValueText(description, state, "spell", 0u) == std::nullopt, "scalar has no index");
    Require(IsUiDescriptionMember("AudastraUI\\CastBar.json") && IsUiDescriptionMember("audastraui/castbar.JSON"), "member path");
    Require(!IsUiDescriptionMember("AudastraUI\\a b.json") && !IsUiDescriptionMember("UI\\CastBar.json"), "not a member");
}

void TestTree() {
    const UiDescription description = Parse(kCastBar);
    const FrameLibrary library;
    UiInstanceState state;
    state.values["progress"] = "0.5";
    FrameTree tree;
    std::vector<Diagnostic> diagnostics;
    const std::uint32_t root = AddUiDescription(tree, library, nullptr, description, state, diagnostics);
    (void)tree.Layout();
    Require(diagnostics.empty(), "no build diagnostics");
    Require(tree.Nodes()[root].frameType == kUiDescriptionRootType && tree.Nodes()[root].name == "CastBar", "root");
    const FrameNode& back = NodeNamed(tree, "back");
    Require(Near(back.rect.left, 0.3f) && Near(back.rect.bottom, 0.18f) && Near(back.rect.right, 0.5f) && Near(back.rect.top, 0.2f),
        "back placed above screen bottom");
    Require(back.uiFrame && back.uiFrame->description == "CastBar" && back.uiFrame->frame == "back", "node linked");
    const FrameNode& fill = NodeNamed(tree, "fill");
    Require(fill.frameType == "SIMPLESTATUSBAR" && fill.fill && Near(*fill.fill, 0.5f), "fill follows value");
    Require(Near(fill.rect.left, 0.302f) && Near(fill.rect.right, 0.498f) && Near(fill.rect.bottom, 0.182f), "fill rect");
    const FrameNode& label = NodeNamed(tree, "label");
    Require(label.text && label.text->text == "火球术  1.5" && label.layout == LayoutState::SizeFromContent, "label text");

    const FrameDrawList list = BuildDrawList(tree);
    const FrameDrawCommand* fillDraw = nullptr;
    for (const auto& command : list.commands) {
        if (command.node == fill.id) fillDraw = &command;
    }
    Require(fillDraw && Near(fillDraw->image.rect.right, 0.4f) && Near(fillDraw->image.uv[1].u, 0.5f),
        "fill cropped with texture coordinates");
    Require(fillDraw->image.path == "ReplaceableTextures\\TeamColor\\TeamColor04.blp", "fill texture");

    // 打断状态：贴图、进度与文字被覆盖。
    state.state = "interrupted";
    FrameTree interrupted;
    (void)AddUiDescription(interrupted, library, nullptr, description, state, diagnostics);
    (void)interrupted.Layout();
    Require(NodeNamed(interrupted, "fill").texture->path == "ReplaceableTextures\\TeamColor\\TeamColor00.blp" &&
                Near(*NodeNamed(interrupted, "fill").fill, 1.0f),
        "state overrides bar");
    Require(NodeNamed(interrupted, "label").text->text == "火球术  打断", "state overrides text");

    // 不在施法：底框及其子框架隐藏；编辑时可要求照画。
    state.state.clear();
    state.values["casting"] = "false";
    FrameTree idle;
    (void)AddUiDescription(idle, library, nullptr, description, state, diagnostics);
    (void)idle.Layout();
    Require(NodeNamed(idle, "back").hidden, "hidden by binding");
    Require(BuildDrawList(idle).commands.empty(), "hidden subtree not drawn");
    FrameDrawOptions options;
    options.drawScriptHidden = true;
    Require(!BuildDrawList(idle, options).commands.empty(), "hidden drawn on request");

    state.state = "missing";
    std::vector<Diagnostic> unknownState;
    FrameTree other;
    (void)AddUiDescription(other, library, nullptr, description, state, unknownState);
    Require(HasCode(unknownState, "AUI_STATE_UNKNOWN"), "unknown state reported");
}

void TestRepeat() {
    const UiDescription description = Parse(R"({
  "format": "audastra.ui", "version": 1, "id": "Raid",
  "values": [
    {"id": "hp", "type": "real", "size": 4},
    {"id": "name", "type": "string", "size": 4},
    {"id": "dead", "type": "boolean", "size": 4}
  ],
  "frames": [
    {"id": "slot", "type": "backdrop", "repeat": {"count": 3, "dy": -0.03},
     "anchors": [{"point": "TOPLEFT", "to": "screen", "toPoint": "BOTTOMLEFT", "x": 0.01, "y": 0.5}],
     "width": 0.1, "height": 0.025, "texture": "Slot",
     "children": [
       {"id": "hpbar", "type": "bar", "anchors": [{"point": "TOPLEFT"}, {"point": "BOTTOMRIGHT"}], "texture": "Fill",
        "value": "hp[]"},
       {"id": "label", "type": "text", "anchors": [{"point": "CENTER", "to": "hpbar"}], "text": "{name[]}",
        "visible": {"value": "dead[]", "invert": true}}
     ]},
    {"id": "title", "type": "text", "anchors": [{"point": "BOTTOMLEFT", "to": "slot", "toPoint": "TOPLEFT"}], "text": "团队"}
  ]
})");
    UiInstanceState state;
    state.values["hp[1]"] = "0.25";
    state.values["name[2]"] = "C";
    state.values["dead[0]"] = "true";
    const FrameLibrary library;
    FrameTree tree;
    std::vector<Diagnostic> diagnostics;
    (void)AddUiDescription(tree, library, nullptr, description, state, diagnostics);
    (void)tree.Layout();
    const FrameNode& slot1 = NodeNamed(tree, "slot#1");
    Require(Near(slot1.rect.left, 0.01f) && Near(slot1.rect.top, 0.47f) && Near(slot1.rect.bottom, 0.445f), "instance offset");
    Require(slot1.uiFrame && slot1.uiFrame->instance == std::optional<std::uint32_t>(1u), "instance linked");
    Require(Near(*NodeNamed(tree, "hpbar#1").fill, 0.25f) && Near(*NodeNamed(tree, "hpbar#0").fill, 0.0f), "per-instance value");
    Require(Near(NodeNamed(tree, "hpbar#2").rect.top, 0.44f), "child follows its instance");
    Require(NodeNamed(tree, "label#2").text->text == "C", "per-instance text");
    Require(tree.Nodes()[NodeNamed(tree, "label#2").anchors[0].targetNode].name == "hpbar#2",
        "sibling anchor stays in the instance");
    Require(NodeNamed(tree, "label#0").hidden && !NodeNamed(tree, "label#1").hidden, "per-instance visibility");
    Require(tree.Nodes()[NodeNamed(tree, "title").anchors[0].targetNode].name == "slot#0", "outside reference uses first instance");
}

void TestGameArea() {
    const UiDescription description = Parse(R"({
  "format": "audastra.ui", "version": 1, "id": "Hint",
  "frames": [{"id": "tip", "type": "text", "text": "x", "anchors": [{"point": "BOTTOM", "to": "game:CommandBar", "toPoint": "TOP"}]}]
})");
    const FrameLibrary library;
    FrameTree withAreas;
    (void)withAreas.AddGameCodeAreas();
    std::vector<Diagnostic> diagnostics;
    (void)AddUiDescription(withAreas, library, nullptr, description, {}, diagnostics);
    (void)withAreas.Layout();
    Require(NodeNamed(withAreas, "tip").anchors.size() == 1 &&
                NodeNamed(withAreas, "tip").anchors[0].targetNode == withAreas.FindCodeArea("CommandBar"),
        "anchor resolved to game code area");
    FrameTree bare;
    std::vector<Diagnostic> missing;
    (void)AddUiDescription(bare, library, nullptr, description, {}, missing);
    Require(HasCode(missing, "AUI_ANCHOR_AREA") && NodeNamed(bare, "tip").anchors.empty(), "area missing reported");
}

void TestEdits() {
    const UiDescription base = Parse(kCastBar);
    auto edited = ApplyUiEdits(base, R"([
  {"op": "add_frame", "parent": "back", "frame": {"id": "hint", "type": "text", "text": "按 Esc 打断", "anchors": [{"point": "TOP", "to": "back", "toPoint": "BOTTOM"}]}},
  {"op": "set_frame", "frame": "fill", "fields": {"width": 0.15, "texture": "Other"}},
  {"op": "set_frame", "frame": "label", "fields": {"id": "caption", "label": null}},
  {"op": "set_frame", "frame": "back", "fields": {"visible": null}},
  {"op": "move_frame", "frame": "hint", "parent": null, "index": 0},
  {"op": "set_description", "label": "施法条"}
])");
    Require(edited.value.has_value(), "edits apply");
    const UiDescription& after = *edited.value;
    Require(after.label == "施法条" && after.frames.size() == 2 && after.frames[0].id == "hint", "moved to top level first");
    Require(FindUiFrame(after, "fill")->width == std::optional<double>(0.15) && FindUiFrame(after, "fill")->texture == "Other",
        "fields replaced");
    Require(FindUiFrame(after, "caption") && !FindUiFrame(after, "label"), "renamed");
    Require(after.states[0].overrides[1].frame == "caption", "state reference follows rename");
    Require(!FindUiFrame(after, "back")->visible.binding && FindUiFrame(after, "back")->visible.literal, "null removes field");
    Require(base.frames[0].children.size() == 2 && FindUiFrame(base, "label"), "base untouched");

    auto removed = ApplyUiEdits(base, R"([{"op": "remove_frame", "frame": "fill"}])");
    Require(removed.value && !FindUiFrame(*removed.value, "fill") && removed.value->states[0].overrides.size() == 1,
        "removal drops the frame's state overrides");

    // 整组不生效：任一步出错、或结果不合法。
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "add_frame", "frame": {"id": "fill", "type": "panel"}}])").diagnostics, "AUI_EDIT"),
        "duplicate id rejected");
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "set_frame", "frame": "nope", "fields": {}}])").diagnostics, "AUI_EDIT"),
        "unknown frame rejected");
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "set_frame", "frame": "back", "fields": {"children": []}}])").diagnostics, "AUI_EDIT"),
        "children not settable");
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "move_frame", "frame": "back", "parent": "fill"}])").diagnostics, "AUI_EDIT"),
        "cannot move into own subtree");
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "explode"}])").diagnostics, "AUI_EDIT"), "unknown op");
    Require(HasCode(ApplyUiEdits(base, "[").diagnostics, "AUI_EDIT"), "invalid json");
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "set_frame", "frame": "fill", "fields": {"width": "wide"}}])").diagnostics, "AUI_TYPE"),
        "result checked by the strict reader");
    auto anchored = ApplyUiEdits(base, R"([
  {"op": "add_frame", "frame": {"id": "tip", "type": "text", "text": "x", "anchors": [{"point": "TOP", "to": "fill"}]}},
  {"op": "remove_frame", "frame": "fill"}
])");
    Require(!anchored.value && HasCode(anchored.diagnostics, "AUI_ANCHOR_TARGET"), "anchor references must be fixed first");

    // 改数值编号：取值与文字模板（含状态覆盖）跟着改，其余文字原样。
    auto renamed = ApplyUiEdits(base, R"([
  {"op": "set_frame", "frame": "label", "fields": {"text": "{{{spell}}} {remain:1}"}},
  {"op": "rename_value", "from": "spell", "to": "spellName"},
  {"op": "rename_value", "from": "casting", "to": "shown"}
])");
    Require(renamed.value.has_value(), "rename value applies");
    Require(FindUiFrame(*renamed.value, "label")->text == "{{{spellName}}} {remain:1}", "template renamed, braces kept");
    Require(renamed.value->states[0].overrides[1].text == std::optional<std::string>("{spellName}  打断"), "state text renamed");
    Require(FindUiFrame(*renamed.value, "back")->visible.binding->value == "shown", "binding renamed");
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "rename_value", "from": "spell", "to": "remain"}])").diagnostics, "AUI_EDIT"),
        "rename onto existing value rejected");
    Require(HasCode(ApplyUiEdits(base, R"([{"op": "rename_value", "from": "nope", "to": "x"}])").diagnostics, "AUI_EDIT"),
        "rename of unknown value rejected");
    Require(!ApplyUiEdits(base, R"([{"op": "set_values", "values": []}])").value, "removing a bound value is rejected");

    Require(NewUiDescription("Raid", "团队").value.has_value(), "new description");
    Require(!NewUiDescription("9bad", "").value.has_value(), "new description needs a valid id");
    Require(NextUiFrameId(base, "fill") == "fill1" && NextUiFrameId(base, "text") == "text1", "next free id");
}

} // namespace

int main() {
    try {
        TestRoundTrip();
        TestErrors();
        TestValues();
        TestTree();
        TestRepeat();
        TestGameArea();
        TestEdits();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "game ui description tests passed\n";
    return 0;
}
