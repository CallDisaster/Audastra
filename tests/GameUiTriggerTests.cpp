#include "War3MapCore/GameUi/FrameDefinition.h"
#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/VJass.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerFeatureText.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCallbackActions.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

// 界面描述生成的自然 T：用合成的原版签名基础层加仓库里真实的 AudastraUI 函数库，
// 生成文本须整批写进 AudastraUI 类别、导出往返一致，并能写成 WTG/WCT 再读回。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void Write(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary);
    stream << text;
    Require(stream.good(), "write " + file.string());
}
std::string First(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? std::string("（无诊断）") : diagnostics.front().code + " " + diagnostics.front().message;
}
bool Contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

// 生成文本用到的原版函数，参数类型与 TriggerData 一致。
constexpr std::string_view kBaseDefine =
    "[TriggerTypes]\n"
    "boolean=0,1,1,Boolean,boolean\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\nstring=0,1,1,String,string\n"
    "ComparisonOperator=0,0,0,Comparison\nEqualNotEqualOperator=0,0,0,EqualNotEqual\nArithmeticOperator=0,0,0,Arithmetic\n"
    "scriptcode=0,0,0,Script,string\n"
    "[TriggerParams]\n"
    "OperatorEqual=0,ComparisonOperator,==,Equal\nOperatorNotEqual=0,ComparisonOperator,!=,NotEqual\n"
    "OperatorEqualENE=0,EqualNotEqualOperator,==,Equal\nOperatorNotEqualENE=0,EqualNotEqualOperator,!=,NotEqual\n"
    "OperatorAdd=0,ArithmeticOperator,+,Add\nOperatorMultiply=0,ArithmeticOperator,*,Multiply\n";
constexpr std::string_view kBaseEvent = "[TriggerRegisterTimerEventSingle]\n[[.args]]\ntype=real\n";
constexpr std::string_view kBaseCondition =
    "[OperatorCompareBoolean]\nreturns=boolean\n[[.args]]\ntype=boolean\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=boolean\n"
    "[OperatorCompareInteger]\nreturns=boolean\n[[.args]]\ntype=integer\n[[.args]]\ntype=ComparisonOperator\n[[.args]]\ntype=integer\n"
    "[OperatorCompareReal]\nreturns=boolean\n[[.args]]\ntype=real\n[[.args]]\ntype=ComparisonOperator\n[[.args]]\ntype=real\n"
    "[OperatorCompareString]\nreturns=boolean\n[[.args]]\ntype=string\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=string\n";
constexpr std::string_view kBaseAction =
    "[IfThenElseMultiple]\n[ForLoopAMultiple]\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
    "[SetVariable]\n[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n[ReturnAction]\n[CommentString]\n[[.args]]\ntype=scriptcode\n";
constexpr std::string_view kBaseCall =
    "[GetForLoopIndexA]\nreturns=integer\n"
    "[I2R]\nreturns=real\n[[.args]]\ntype=integer\n[I2S]\nreturns=string\n[[.args]]\ntype=integer\n"
    "[OperatorInt]\nreturns=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=ArithmeticOperator\n[[.args]]\ntype=integer\n"
    "[OperatorReal]\nreturns=real\n[[.args]]\ntype=real\n[[.args]]\ntype=ArithmeticOperator\n[[.args]]\ntype=real\n"
    "[OperatorString]\nreturns=string\n[[.args]]\ntype=string\n[[.args]]\ntype=string\n";

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("Audastra-UiTriggers-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog;
    TriggerDocument doc;

    Fixture() {
        Write(root / "ui/config", "base\nAudastraUI\n");
        Write(root / "ui/base/define.txt", kBaseDefine);
        Write(root / "ui/base/event.txt", kBaseEvent);
        Write(root / "ui/base/condition.txt", kBaseCondition);
        Write(root / "ui/base/action.txt", kBaseAction);
        Write(root / "ui/base/call.txt", kBaseCall);
        const std::filesystem::path library = AUDASTRA_UI_LIBRARY_DIR "/AudastraUI";
        std::filesystem::create_directories(root / "ui/AudastraUI");
        for (const auto* name : {"define.txt", "event.txt", "action.txt", "call.txt"}) {
            std::filesystem::copy_file(library / name, root / "ui/AudastraUI" / name);
        }
        auto loaded = UiCatalog::Load(root / "ui");
        Require(loaded.Succeeded(), "AudastraUI 函数库读入：" + First(loaded.diagnostics));
        catalog = std::move(*loaded.value);
        auto empty = WtgWctCodec{}.CreateEmpty(catalog);
        Require(empty.Succeeded(), "空触发器文档：" + First(empty.diagnostics));
        doc = std::move(*empty.value);
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    Result<TriggerGraphEditReceipt> Apply(const std::string& text) {
        const auto binding = CaptureTriggerGraphBinding(doc, catalog);
        Require(binding.Succeeded(), "capture binding");
        TriggerGraphEditBatch batch{*binding.value, {}};
        batch.operations.emplace_back(GraphApplyFeatureText{std::string(kUiTriggerCategory), text, {}, std::nullopt});
        return ApplyTriggerGraphEditBatch(doc, catalog, batch);
    }
};

UiAnchor Anchor(FramePoint point, UiAnchorTarget target, FramePoint targetPoint, double x = 0.0, double y = 0.0, std::string id = {}) {
    UiAnchor anchor;
    anchor.point = point;
    anchor.target = target;
    anchor.targetId = std::move(id);
    anchor.targetPoint = targetPoint;
    anchor.x = x;
    anchor.y = y;
    return anchor;
}
UiBinding Bind(std::string value, bool perInstance = false) {
    UiBinding binding;
    binding.value = std::move(value);
    binding.perInstance = perInstance;
    return binding;
}

// 读条：进度条 + 名字，一排三个按钮（重复框架），一个“打断”状态和按钮点击事件。
UiDescription CastBar() {
    UiDescription d;
    d.id = "cast";
    d.label = "读条";
    d.values = {UiValue{"progress", "进度", UiValueType::Real, 0, "0", 0.0, 1.0},
        UiValue{"name", "技能名", UiValueType::String, 0, "火球", std::nullopt, std::nullopt},
        UiValue{"charges", "次数", UiValueType::Integer, 3, "0", std::nullopt, std::nullopt},
        UiValue{"shown", "显示", UiValueType::Boolean, 0, "true", std::nullopt, std::nullopt}};

    UiFrame root;
    root.id = "root";
    root.type = UiFrameType::Panel;
    root.anchors = {Anchor(FramePoint::Bottom, UiAnchorTarget::Screen, FramePoint::Bottom, 0.0, 0.12)};
    root.width = 0.3;
    root.height = 0.05;
    root.visible.binding = Bind("shown");

    UiFrame bar;
    bar.id = "bar";
    bar.type = UiFrameType::Bar;
    bar.anchors = {Anchor(FramePoint::Center, UiAnchorTarget::Parent, FramePoint::Center)};
    bar.width = 0.28;
    bar.height = 0.012;
    bar.texture = "UI\\Feedback\\XPBar\\human-bigbar-fill.blp";
    bar.background = "UI\\Feedback\\XPBar\\human-xpbar-border.blp";
    bar.value.binding = Bind("progress");

    UiFrame label;
    label.id = "label";
    label.type = UiFrameType::Text;
    label.anchors = {Anchor(FramePoint::Bottom, UiAnchorTarget::Frame, FramePoint::Top, 0.0, 0.004, "bar")};
    label.text = "{name} {progress:1}";
    label.fontHeight = 0.013;
    label.color = {1.0, 0.8, 0.0, 1.0};
    label.justifyH = JustifyH::Left;

    root.children = {bar, label};

    UiFrame slot;
    slot.id = "slot";
    slot.type = UiFrameType::Button;
    slot.anchors = {Anchor(FramePoint::BottomLeft, UiAnchorTarget::Screen, FramePoint::BottomLeft, 0.02, 0.02)};
    slot.width = 0.035;
    slot.height = 0.035;
    slot.texture = "ReplaceableTextures\\CommandButtons\\BTNFireBolt.blp";
    slot.text = "{charges[]}";
    slot.repeat = UiRepeat{3, 0.04, 0.0};

    d.frames = {root, slot};
    d.states = {UiState{"interrupt", "打断", {UiStateOverride{"bar", "UI\\Feedback\\XPBar\\human-bigbar-fill-red.blp", std::nullopt, std::nullopt, std::nullopt},
        UiStateOverride{"label", std::nullopt, std::string("打断"), std::nullopt, std::nullopt}}}};
    d.events = {UiEvent{"slotClick", "slot", UiEventKind::Click}};
    return d;
}

void TestGeneratedText() {
    UiTriggerOptions options;
    options.defaultFont = "Fonts\\dfst-m3u.ttf";
    const auto plan = GenerateUiTriggers(CastBar(), options);
    Require(plan.Succeeded(), "读条生成：" + First(plan.diagnostics));
    const std::string& text = plan.value->featureText;

    Require(plan.value->triggers == std::vector<std::string>{"读条 创建 cast_Create", "读条 更新 cast_Update",
        "读条 状态 打断 cast_State_interrupt", "读条 基础状态 cast_BaseState"}, "触发器名与顺序");
    Require(plan.value->variables == std::vector<std::string>{"AUI_cast_root", "AUI_cast_bar", "AUI_cast_label", "AUI_cast_slot",
        "AUIV_cast_progress", "AUIV_cast_name", "AUIV_cast_charges", "AUIV_cast_shown", "AUIS_cast"}, "变量名与顺序");
    Require(Contains(text, "    AUI_cast_slot: AudastraFrame[3] = AudastraFrameNone\n"), "重复框架句柄是数组：\n" + text);
    Require(Contains(text, "    AUIV_cast_name: string = \"火球\"\n") && Contains(text, "    AUIV_cast_charges: integer[3] = 0\n"),
        "数值变量带初值：\n" + text);

    // 运行库缺失时直接返回；先加载基础模板。
    Require(Contains(text, "IfThenElseMultiple() {\n            if:\n                OperatorCompareBoolean(AudastraUIIsReady(), OperatorEqualENE, false)\n"
                           "            then:\n                ReturnAction()\n"), "运行库检查：\n" + text);
    Require(Contains(text, "AudastraUILoadToc(\"AudastraUI\\\\AudastraUI.toc\")"), "加载基础模板");
    // 进度条走 Simple 创建；文字锚在兄弟框架；重复按钮在循环里按下标创建、平移并监听。
    Require(Contains(text, "SetVariable(udg_AUI_cast_bar, AudastraUICreateSimpleFrame(\"AudastraBar\", udg_AUI_cast_root, "), "进度条创建");
    Require(Contains(text, "AudastraUISetPoint(udg_AUI_cast_label, AudastraAnchorBottom, udg_AUI_cast_bar, AudastraAnchorTop, 0, 0.004)"),
        "相对锚点：\n" + text);
    Require(Contains(text, "AudastraUISetAbsPoint(udg_AUI_cast_root, AudastraAnchorBottom, 0.4, 0.12)"), "屏幕锚点换算成屏幕坐标");
    Require(Contains(text, "ForLoopAMultiple(0, 2) {"), "重复框架展开为循环");
    Require(Contains(text, "AudastraUISetAbsPoint(udg_AUI_cast_slot[GetForLoopIndexA()], AudastraAnchorBottomLeft, "
                           "OperatorReal(0.02, OperatorAdd, OperatorReal(I2R(GetForLoopIndexA()), OperatorMultiply, 0.04)), 0.02)"),
        "重复实例按下标平移：\n" + text);
    Require(Contains(text, "AudastraUIWatchFrame(udg_AUI_cast_slot[GetForLoopIndexA()], AudastraFrameEventClick, \"cast.slotClick\", GetForLoopIndexA())"),
        "按钮点击带实例下标");
    Require(Contains(text, "AudastraUISetFont(udg_AUI_cast_label, \"Fonts\\\\dfst-m3u.ttf\", 0.013, 0)")
        && Contains(text, "AudastraUISetColor(udg_AUI_cast_label, 255, 204, 0, 255)")
        && Contains(text, "AudastraUISetTextAlign(udg_AUI_cast_label, AudastraJustifyLeft, AudastraJustifyMiddle)"), "文字样式：\n" + text);
    // 按钮文字在模板子框架上，按实例取自己的格。
    Require(Contains(text, "AudastraUISetText(AudastraUIFindFrame(\"AudastraButtonText\", OperatorInt(")
        && Contains(text, "I2S(udg_AUIV_cast_charges[GetForLoopIndexA()])"), "按钮文字绑定：\n" + text);
    // 有状态覆盖的属性只在当前状态不覆盖时刷新。
    Require(Contains(text, "OperatorCompareString(udg_AUIS_cast, OperatorNotEqualENE, \"interrupt\")"), "状态守卫：\n" + text);
    Require(Contains(text, "OperatorString(OperatorString(udg_AUIV_cast_name, \" \"), AudastraUIFormatReal(udg_AUIV_cast_progress, 1))"),
        "文字模板拼接：\n" + text);
    Require(Contains(text, "AudastraUIShow(udg_AUI_cast_root, udg_AUIV_cast_shown)"), "显示绑定到布尔数值");
}

void TestAppliedThroughSharedChain() {
    Fixture f;
    UiTriggerOptions options;
    options.defaultFont = "Fonts\\dfst-m3u.ttf";
    const auto plan = GenerateUiTriggers(CastBar(), options);
    Require(plan.Succeeded(), "生成：" + First(plan.diagnostics));
    const auto applied = f.Apply(plan.value->featureText);
    Require(applied.Succeeded(), "生成的自然 T 写进触发器文档：" + First(applied.diagnostics) + "\n" + plan.value->featureText);

    const auto category = std::find_if(f.doc.categories.begin(), f.doc.categories.end(),
        [](const auto& row) { return row.name.text == kUiTriggerCategory; });
    Require(category != f.doc.categories.end(), "AudastraUI 类别已建立");
    // 写入会换掉整份文档，类别按编号记住。
    const auto categoryId = category->categoryId;
    std::vector<std::string> names;
    for (const auto& trigger : f.doc.triggers) {
        if (trigger.categoryId == categoryId) names.push_back(trigger.name.text);
    }
    Require(names == plan.value->triggers, "四个触发器都在 AudastraUI 类别里");
    const auto slot = std::find_if(f.doc.variables.begin(), f.doc.variables.end(), [](const auto& row) { return row.name.text == "AUI_cast_slot"; });
    Require(slot != f.doc.variables.end() && slot->typeName.text == "AudastraFrame" && slot->isArray != 0 && slot->size == 3, "框架数组变量");

    const auto exported = RenderFeatureText(f.doc, f.catalog, categoryId);
    Require(exported.Succeeded(), "导出功能写法：" + First(exported.diagnostics));
    const auto again = f.Apply(*exported.value);
    Require(again.Succeeded(), "导出文本写回：" + First(again.diagnostics));
    const auto reexported = RenderFeatureText(f.doc, f.catalog, categoryId);
    Require(reexported.Succeeded() && *reexported.value == *exported.value, "导出往返一致");

    // 重新生成整批替换同名触发器，不重复建。
    const auto regenerated = f.Apply(plan.value->featureText);
    Require(regenerated.Succeeded(), "重新生成：" + First(regenerated.diagnostics));
    Require(std::count_if(f.doc.triggers.begin(), f.doc.triggers.end(), [&](const auto& row) { return row.categoryId == categoryId; }) == 4,
        "重新生成覆盖原触发器");

    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog);
    Require(encoded.Succeeded(), "写成 WTG/WCT：" + First(encoded.diagnostics));
    const auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(parsed.Succeeded(), "WTG/WCT 读回：" + First(parsed.diagnostics));
    const auto readback = std::find_if(parsed.value->categories.begin(), parsed.value->categories.end(),
        [](const auto& row) { return row.name.text == kUiTriggerCategory; });
    Require(readback != parsed.value->categories.end(), "读回的类别");
    const auto final = RenderFeatureText(*parsed.value, f.catalog, readback->categoryId);
    Require(final.Succeeded() && *final.value == *exported.value, "编码往返后导出不变");
}

void TestRefusalsAndTemplates() {
    // 锚在游戏代码区域：每处都列出并整体失败。
    UiDescription d = CastBar();
    d.frames[0].anchors = {Anchor(FramePoint::Bottom, UiAnchorTarget::GameArea, FramePoint::Top, 0.0, 0.0, "Minimap")};
    d.frames[1].anchors = {Anchor(FramePoint::Left, UiAnchorTarget::GameArea, FramePoint::Right, 0.0, 0.0, "CommandBar")};
    UiTriggerOptions withFont;
    withFont.defaultFont = "Fonts\\dfst-m3u.ttf";
    const auto refused = GenerateUiTriggers(d, withFont);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 2
            && std::all_of(refused.diagnostics.begin(), refused.diagnostics.end(),
                [](const auto& item) { return item.code == "AUI_TRIGGER_UNSUPPORTED"; }),
        "不支持的部分全部列出：" + First(refused.diagnostics));
    // 描述本身不合法时返回描述检查的诊断。
    UiDescription nested = CastBar();
    UiFrame inner;
    inner.id = "inner";
    inner.repeat = UiRepeat{2, 0.0, 0.01};
    nested.frames[1].children = {inner};
    const auto invalid = GenerateUiTriggers(nested, withFont);
    Require(!invalid.Succeeded() && invalid.diagnostics.front().code == "AUI_REPEAT_NESTED", "嵌套重复由描述检查拒绝");

    // 改了字号但没有默认字体路径时拒绝，不猜字体。
    const auto noFont = GenerateUiTriggers(CastBar());
    Require(!noFont.Succeeded() && Contains(First(noFont.diagnostics), "默认字体"), "缺默认字体时拒绝");

    // 基础模板文件可解析，并提供生成的触发器用到的全部模板与子框架。
    const auto fdf = ParseFdf(UiTemplateFdfText(), std::string(kUiTemplateFdfMember));
    Require(fdf.Succeeded(), "基础模板解析：" + First(fdf.diagnostics));
    FrameLibrary library;
    library.AddParsed(*fdf.value, "map");
    for (const auto* name : {"AudastraPanel", "AudastraBackdrop", "AudastraText", "AudastraBar", "AudastraButton"}) {
        Require(library.FindTemplate(name).has_value(), std::string("基础模板 ") + name);
    }
    Require(ParseToc(UiTemplateTocText()) == std::vector<std::string>{std::string(kUiTemplateFdfMember)}, "基础 toc 列出基础 fdf");

    // 模板框架按目录里的框架类型决定创建方式；目录里没有时按普通框架创建并警告。
    const auto simple = ParseFdf("Frame \"SIMPLEFRAME\" \"MapSimple\" {\r\n}\r\nFrame \"BACKDROP\" \"MapBackdrop\" {\r\n}\r\n", "map.fdf");
    Require(simple.Succeeded(), "地图模板解析");
    library.AddParsed(*simple.value, "map");
    UiDescription templated;
    templated.id = "tpl";
    UiFrame a;
    a.id = "a";
    a.type = UiFrameType::Template;
    a.templateName = "MapSimple";
    UiFrame b = a;
    b.id = "b";
    b.templateName = "MapBackdrop";
    UiFrame c = a;
    c.id = "c";
    c.templateName = "Missing";
    templated.frames = {a, b, c};
    UiTriggerOptions options;
    options.library = &library;
    options.tocFiles = {"UI\\MapUi.toc"};
    const auto plan = GenerateUiTriggers(templated, options);
    Require(plan.Succeeded(), "模板框架生成：" + First(plan.diagnostics));
    const std::string& text = plan.value->featureText;
    Require(Contains(text, "AudastraUICreateSimpleFrame(\"MapSimple\", AudastraUIGameUI(), ")
            && Contains(text, "AudastraUICreateFrame(\"MapBackdrop\", AudastraUIGameUI(), 0, ")
            && Contains(text, "AudastraUICreateFrame(\"Missing\", AudastraUIGameUI(), 0, ")
            && Contains(text, "AudastraUILoadToc(\"UI\\\\MapUi.toc\")"),
        "模板创建方式：\n" + text);
    Require(plan.diagnostics.size() == 1 && plan.diagnostics.front().code == "AUI_TRIGGER_TEMPLATE_UNKNOWN", "缺模板只警告一次");
    Require(plan.value->triggers.size() == 2, "无状态时只有创建与更新");
}

// 脚本部分与函数库定义一一对应：cfg 列出全部函数，.j 的参数个数与类型同定义（事件前补触发器），
// 并能经 vJass 翻译与 JASS 检查。
void TestScriptLibrary() {
    Fixture f;
    const std::filesystem::path jass = AUDASTRA_UI_LIBRARY_DIR "/AudastraUI/jass";
    std::ifstream cfgStream(jass / "AudastraUI.cfg", std::ios::binary);
    std::vector<std::string> listed;
    for (std::string line; std::getline(cfgStream, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) listed.push_back(line);
    }
    std::ifstream scriptStream(jass / "AudastraUI.j", std::ios::binary);
    const std::string script((std::istreambuf_iterator<char>(scriptStream)), std::istreambuf_iterator<char>());
    Require(!listed.empty() && !script.empty(), "读入 AudastraUI.cfg 与 AudastraUI.j");

    std::map<std::string, std::vector<std::string>> declared;
    for (const auto& declaration : ScanUiJassDeclarations(script, jass / "AudastraUI.j")) declared[declaration.name] = declaration.parameterTypes;
    const auto jassType = [](const std::string& type) { return type.starts_with("Audastra") ? std::string("integer") : type; };
    std::vector<std::string> defined;
    for (const auto kind : {UiDefinitionKind::Event, UiDefinitionKind::Action, UiDefinitionKind::Call}) {
        for (const auto* definition : f.catalog.Definitions(kind)) {
            if (definition->Active().layer != "AudastraUI") continue;
            defined.push_back(definition->name);
            std::vector<std::string> expected;
            if (kind == UiDefinitionKind::Event) expected.push_back("trigger");
            for (const auto& argument : definition->Active().arguments) expected.push_back(jassType(argument.type));
            const auto found = declared.find(definition->name);
            Require(found != declared.end(), "脚本里缺少 " + definition->name);
            Require(found->second == expected, "参数与定义不符：" + definition->name);
        }
    }
    std::sort(defined.begin(), defined.end());
    std::sort(listed.begin(), listed.end());
    Require(defined == listed, "cfg 与函数库定义列出同一组函数");

    constexpr std::string_view common =
        "type agent extends handle\ntype trigger extends agent\n"
        "native R2SW takes real r, integer width, integer precision returns string\n"
        "native SubString takes string source, integer start, integer end returns string\n"
        "native StringLength takes string s returns integer\n";
    Jass::VJassOptions options;
    options.baseScripts = {Jass::JassSource{"common.j", common}};
    const auto translated = Jass::TranslateVJass(script, "AudastraUI.j", options);
    Require(translated.Succeeded() && translated.unsupported.empty(), "vJass 翻译：" + First(translated.diagnostics) + translated.unsupported);
    const std::array<Jass::JassSource, 2> sources{Jass::JassSource{"common.j", common}, Jass::JassSource{"AudastraUI.j", translated.text}};
    const auto checked = Jass::CheckJass(sources);
    Require(checked.Succeeded() && checked.completed && checked.semanticChecked, "JASS 检查：" + First(checked.diagnostics));
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        TestGeneratedText();
        TestAppliedThroughSharedChain();
        TestRefusalsAndTemplates();
        TestScriptLibrary();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "game ui trigger tests passed\n";
    return 0;
}
