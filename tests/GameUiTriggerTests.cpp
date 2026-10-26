#include "AudastraUiCatalog.h"
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
std::string First(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? std::string("（无诊断）") : diagnostics.front().code + " " + diagnostics.front().message;
}
bool Contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

struct Fixture {
    AudastraUiTest::Catalog files;
    UiCatalog catalog;
    TriggerDocument doc;

    Fixture() {
        const auto error = files.Load();
        Require(error.empty(), error);
        catalog = std::move(*files.catalog);
        doc = std::move(*files.document);
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
        "AUIV_cast_progress", "AUIV_cast_name", "AUIV_cast_charges", "AUIV_cast_shown", "AUIS_cast", "AUIC_cast", "AUIA_cast"},
        "变量名与顺序");
    Require(Contains(text, "    AUI_cast_slot: AudastraFrame[3] = AudastraFrameNone\n"), "重复框架句柄是数组：\n" + text);
    Require(Contains(text, "    AUIV_cast_name: string = \"火球\"\n") && Contains(text, "    AUIV_cast_charges: integer[3] = 0\n"),
        "数值变量带初值：\n" + text);

    // 创建每 0.2 秒试一次：试满 150 次关掉自己；界面库不可用时等下一次；可用后关掉自己，先加载基础模板，建好记下。
    const std::size_t create = text.find("TriggerRegisterTimerEventPeriodic(0.2)");
    Require(create != std::string::npos, "创建按周期重试：\n" + text);
    const std::string createBody = text.substr(create, text.find("\n触发器 ", create) - create);
    const std::size_t attempt = createBody.find("SetVariable(udg_AUIA_cast, OperatorInt(udg_AUIA_cast, OperatorAdd, 1))");
    const std::size_t limit = createBody.find("OperatorCompareInteger(udg_AUIA_cast, OperatorGreater, 150)\n"
                                              "            then:\n                DisableTrigger(GetTriggeringTrigger())\n                ReturnAction()\n");
    const std::size_t ready = createBody.find("OperatorCompareBoolean(AudastraUIIsReady(), OperatorEqualENE, false)\n"
                                              "            then:\n                ReturnAction()\n");
    const std::size_t once = createBody.find("        DisableTrigger(GetTriggeringTrigger())\n        AudastraUILoadToc(\"AudastraUI\\\\AudastraUI.toc\")");
    const std::size_t created = createBody.find("SetVariable(udg_AUIC_cast, true)");
    Require(attempt < limit && limit < ready && ready < once && once < created && created != std::string::npos, "创建的重试顺序：\n" + createBody);
    // 更新、状态、回到基础状态在界面建好前直接返回。
    std::size_t guards = 0;
    for (std::size_t at = 0; (at = text.find("OperatorCompareBoolean(udg_AUIC_cast, OperatorEqualENE, false)\n"
                                               "            then:\n                ReturnAction()\n", at)) != std::string::npos; ++at) {
        ++guards;
    }
    Require(guards == 3, "更新与状态触发器等界面建好：\n" + text);
    // 进度条是两层普通框架：本身画底图，填充子框架靠左、按比例改宽度；文字锚在兄弟框架；重复按钮在循环里按下标创建、平移并监听。
    Require(Contains(text, "SetVariable(udg_AUI_cast_bar, AudastraUICreateFrame(\"AudastraBar\", udg_AUI_cast_root, 0, ")
            && !Contains(text, "AudastraUICreateSimpleFrame") && !Contains(text, "AudastraUISetValue"),
        "进度条按普通框架创建：\n" + text);
    Require(Contains(text, "AudastraUIClearPoints(AudastraUIFindFrame(\"AudastraBarFill\", ")
            && Contains(text, "), AudastraAnchorTopLeft, udg_AUI_cast_bar, AudastraAnchorTopLeft, 0, 0)")
            && Contains(text, "), AudastraAnchorBottomLeft, udg_AUI_cast_bar, AudastraAnchorBottomLeft, 0, 0)"),
        "填充靠左对齐：\n" + text);
    Require(Contains(text, "AudastraUISetTexture(udg_AUI_cast_bar, \"UI\\\\Feedback\\\\XPBar\\\\human-xpbar-border.blp\", false)")
            && Contains(text, "), \"UI\\\\Feedback\\\\XPBar\\\\human-bigbar-fill-red.blp\", false)"),
        "底图在进度条上，状态换的填充贴图在填充上：\n" + text);
    Require(Contains(text, "AudastraUISetWidth(AudastraUIFindFrame(\"AudastraBarFill\", ")
            && Contains(text, "), OperatorReal(RMaxBJ(0, RMinBJ(1, udg_AUIV_cast_progress)), OperatorMultiply, 0.28))"),
        "进度按比例改填充宽度，比例夹在 0..1：\n" + text);
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

// 预览里没写贴图的部分是透明的：游戏里不能留下没设贴图的 BACKDROP（会画成绿块）。
void TestUntexturedFrames() {
    UiDescription d;
    d.id = "plain";
    d.label = "无贴图";
    UiFrame panel;
    panel.id = "panel";
    panel.type = UiFrameType::Backdrop;
    panel.anchors = {Anchor(FramePoint::Center, UiAnchorTarget::Screen, FramePoint::Center)};
    panel.width = 0.2;
    panel.height = 0.1;
    UiFrame pick;
    pick.id = "pick";
    pick.type = UiFrameType::Button;
    pick.anchors = {Anchor(FramePoint::Center, UiAnchorTarget::Parent, FramePoint::Center)};
    pick.text = "选择";
    UiFrame icon;
    icon.id = "icon";
    icon.type = UiFrameType::Button;
    icon.anchors = {Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, FramePoint::TopLeft)};
    icon.texture = "ReplaceableTextures\\CommandButtons\\BTNHeal";
    UiFrame bar;
    bar.id = "bar";
    bar.type = UiFrameType::Bar;
    bar.anchors = {Anchor(FramePoint::Bottom, UiAnchorTarget::Parent, FramePoint::Bottom)};
    bar.width = 0.18;
    bar.height = 0.01;
    UiFrame tinted = bar;
    tinted.id = "tinted";
    tinted.texture = "ReplaceableTextures\\TeamColor\\TeamColor00";
    panel.children = {pick, icon, bar, tinted};
    d.frames = {panel};

    const auto plan = GenerateUiTriggers(d, {});
    Require(plan.Succeeded(), "无贴图描述生成：" + First(plan.diagnostics));
    const std::string& text = plan.value->featureText;
    Require(Contains(text, "SetVariable(udg_AUI_plain_panel, AudastraUICreateFrame(\"AudastraPanel\", "), "没贴图的底板建成普通框架：\n" + text);
    Require(Contains(text, "SetVariable(udg_AUI_plain_pick, AudastraUICreateFrame(\"AudastraClickButton\", ")
            && Contains(text, "AudastraUISetText(AudastraUIFindFrame(\"AudastraClickButtonText\", "),
        "没贴图的按钮是透明点击层，文字在它自己的子框架上：\n" + text);
    Require(Contains(text, "SetVariable(udg_AUI_plain_icon, AudastraUICreateFrame(\"AudastraButton\", ")
            && Contains(text, "AudastraUISetTexture(AudastraUIFindFrame(\"AudastraButtonBackdrop\", ")
            && Contains(text, "AudastraUISetTexture(AudastraUIFindFrame(\"AudastraButtonPushed\", ")
            && Contains(text, "\"ReplaceableTextures\\\\CommandButtons\\\\BTNHeal.blp\""),
        "有贴图没写按下贴图时按下也用同一张：\n" + text);
    Require(Contains(text, "SetVariable(udg_AUI_plain_bar, AudastraUICreateFrame(\"AudastraBarClear\", ")
            && Contains(text, "AudastraUIShow(AudastraUIFindFrame(\"AudastraBarClearFill\", "),
        "没有底图和填充贴图的进度条不画底层，填充藏起来：\n" + text);
    Require(Contains(text, "AudastraUISetTexture(AudastraUIFindFrame(\"AudastraBarClearFill\", ")
            && !Contains(text, "AudastraUICreateFrame(\"AudastraBar\", "),
        "有填充贴图的进度条仍然不画底层：\n" + text);

    // 状态里会换贴图的按钮保留底图子框架。
    d.states = {UiState{"lit", "点亮", {UiStateOverride{"pick", "UI\\Widgets\\Glues\\GlueScreen-Button1-BackdropBackground", std::nullopt, std::nullopt, std::nullopt}}}};
    const auto lit = GenerateUiTriggers(d, {});
    Require(lit.Succeeded() && Contains(lit.value->featureText, "SetVariable(udg_AUI_plain_pick, AudastraUICreateFrame(\"AudastraButton\", "),
        "状态里有贴图的按钮用带底图的模板：\n" + (lit.Succeeded() ? lit.value->featureText : First(lit.diagnostics)));
}

// 一次执行的指令上限只够一百多个界面库调用：调用多时按块分开运行，重复框架每个实例单独运行。
void TestThreadedCreation() {
    UiDescription d;
    d.id = "many";
    d.label = "多格";
    UiFrame slot;
    slot.id = "slot";
    slot.type = UiFrameType::Backdrop;
    slot.texture = "UI\\Widgets\\ToolTips\\Human\\human-tooltip-background";
    slot.anchors = {Anchor(FramePoint::TopLeft, UiAnchorTarget::Screen, FramePoint::TopLeft)};
    slot.width = 0.12;
    slot.height = 0.04;
    slot.repeat = UiRepeat{5, 0.0, -0.044};
    for (int i = 0; i < 10; ++i) {
        UiFrame label;
        label.id = "label" + std::to_string(i);
        label.type = UiFrameType::Text;
        label.anchors = {Anchor(FramePoint::TopLeft, UiAnchorTarget::Parent, FramePoint::TopLeft)};
        label.text = "名字";
        slot.children.push_back(label);
    }
    d.frames = {slot};

    const auto plan = GenerateUiTriggers(d, {});
    Require(plan.Succeeded(), "多格描述生成：" + First(plan.diagnostics));
    const auto& triggers = plan.value->triggers;
    const auto build = UiTriggerName(d, "建框架 1", "Build1");
    const auto layout = UiTriggerName(d, "布局 1", "Layout1");
    Require(std::find(triggers.begin(), triggers.end(), build) != triggers.end()
            && std::find(triggers.begin(), triggers.end(), layout) != triggers.end(),
        "调用多时建框架和布局分块：\n" + plan.value->featureText);
    const std::string& text = plan.value->featureText;
    Require(Contains(text, "ConditionalTriggerExecute(`" + build + "`)") && Contains(text, "ConditionalTriggerExecute(`" + layout + "`)"),
        "创建触发器按格运行分块：\n" + text);

    d.frames.front().repeat.reset();
    d.frames.front().children.resize(2);
    const auto small = GenerateUiTriggers(d, {});
    Require(small.Succeeded() && !Contains(small.value->featureText, "_Build1")
            && !Contains(small.value->featureText, "ConditionalTriggerExecute"),
        "调用少时照旧写在一个创建触发器里：\n" + (small.Succeeded() ? small.value->featureText : First(small.diagnostics)));
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
    Require(final.Succeeded() && *final.value == *exported.value, "编码往返后导出不变：\n" + (final.Succeeded() ? *final.value : First(final.diagnostics)));
}

void TestRefusalsAndTemplates() {
    // 锚在游戏代码区域：按 1.27a 的固定位置换成屏幕坐标，重复框架照样按下标平移；每处一条警告。
    UiDescription d = CastBar();
    d.frames[0].anchors = {Anchor(FramePoint::Bottom, UiAnchorTarget::GameArea, FramePoint::Top, 0.0, 0.01, "CommandBar")};
    d.frames[1].anchors = {Anchor(FramePoint::BottomLeft, UiAnchorTarget::GameArea, FramePoint::TopLeft, 0.0, 0.0, "Minimap")};
    UiTriggerOptions withFont;
    withFont.defaultFont = "Fonts\\dfst-m3u.ttf";
    const auto fixed = GenerateUiTriggers(d, withFont);
    Require(fixed.Succeeded(), "锚在游戏代码区域：" + First(fixed.diagnostics));
    Require(std::count_if(fixed.diagnostics.begin(), fixed.diagnostics.end(), [](const auto& item) {
                return item.code == "AUI_TRIGGER_AREA_FIXED" && item.severity == DiagnosticSeverity::Warning
                    && Contains(item.message, "1.27a");
            }) == 2,
        "锚在游戏代码区域的框架各提示一次：" + First(fixed.diagnostics));
    // 命令面板右下角离屏幕右边 0.007375、离底 0.002，宽 0.1745、高 0.129：上边中点 (0.705375, 0.131)。
    Require(Contains(fixed.value->featureText, "AudastraUISetAbsPoint(udg_AUI_cast_root, AudastraAnchorBottom, 0.705375, 0.141)"),
        "命令面板上边中点再往上 0.01");
    // 小地图左上角 (0.009375, 0.145)。
    Require(Contains(fixed.value->featureText, "AudastraUISetAbsPoint(udg_AUI_cast_slot[GetForLoopIndexA()], AudastraAnchorBottomLeft, "
                                               "OperatorReal(0.009375, OperatorAdd, OperatorReal(I2R(GetForLoopIndexA()), OperatorMultiply, 0.04)), 0.145)"),
        "小地图左上角，重复按钮逐个右移");
    Require(!Contains(fixed.value->featureText, "Minimap") && !Contains(fixed.value->featureText, "CommandBar"),
        "不引用游戏代码区域的框架");

    // 生成不了的部分每处都列出并整体失败：两个框架改了字号，却没有游戏默认字体的路径，不猜字体。
    UiDescription twoFonts = CastBar();
    twoFonts.frames[1].fontHeight = 0.02;
    const auto refused = GenerateUiTriggers(twoFonts);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 2
            && std::all_of(refused.diagnostics.begin(), refused.diagnostics.end(),
                [](const auto& item) { return item.code == "AUI_TRIGGER_UNSUPPORTED" && Contains(item.message, "默认字体"); }),
        "不支持的部分全部列出：" + First(refused.diagnostics));
    // 描述本身不合法时返回描述检查的诊断。
    UiDescription nested = CastBar();
    UiFrame inner;
    inner.id = "inner";
    inner.repeat = UiRepeat{2, 0.0, 0.01};
    nested.frames[1].children = {inner};
    const auto invalid = GenerateUiTriggers(nested, withFont);
    Require(!invalid.Succeeded() && invalid.diagnostics.front().code == "AUI_REPEAT_NESTED", "嵌套重复由描述检查拒绝");

    // 基础模板文件可解析，并提供生成的触发器用到的全部模板与子框架。
    const auto fdf = ParseFdf(UiTemplateFdfText(), std::string(kUiTemplateFdfMember));
    Require(fdf.Succeeded(), "基础模板解析：" + First(fdf.diagnostics));
    FrameLibrary library;
    library.AddParsed(*fdf.value, "map");
    for (const auto* name : {"AudastraPanel", "AudastraBackdrop", "AudastraText", "AudastraBar", "AudastraBarClear", "AudastraButton",
             "AudastraClickButton"}) {
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

    // Simple 模板不能挂在普通框架下：改挂游戏界面根并警告；它的普通子框架也不能挂它，改挂最近的普通祖先。
    UiDescription nestedSimple;
    nestedSimple.id = "nest";
    UiFrame panel;
    panel.id = "panel";
    panel.type = UiFrameType::Panel;
    UiFrame simpleInner = a;
    simpleInner.id = "inner";
    UiFrame classicChild = b;
    classicChild.id = "classicChild";
    simpleInner.children = {classicChild};
    panel.children = {simpleInner};
    nestedSimple.frames = {panel};
    const auto nestedPlan = GenerateUiTriggers(nestedSimple, options);
    Require(nestedPlan.Succeeded(), "Simple 模板在普通框架下：" + First(nestedPlan.diagnostics));
    Require(Contains(nestedPlan.value->featureText, "SetVariable(udg_AUI_nest_inner, AudastraUICreateSimpleFrame(\"MapSimple\", AudastraUIGameUI(), ")
            && Contains(nestedPlan.value->featureText, "AudastraUICreateFrame(\"MapBackdrop\", udg_AUI_nest_panel, 0, ")
            && !Contains(nestedPlan.value->featureText, ", udg_AUI_nest_inner, 0, "),
        "Simple 模板挂游戏界面根，它的普通子框架挂回普通祖先：\n" + nestedPlan.value->featureText);
    Require(nestedPlan.diagnostics.size() == 2 && nestedPlan.diagnostics[0].code == "AUI_TRIGGER_SIMPLE_PARENT"
            && Contains(nestedPlan.diagnostics[0].message, "panel") && nestedPlan.diagnostics[1].code == "AUI_TRIGGER_CLASSIC_UNDER_SIMPLE"
            && Contains(nestedPlan.diagnostics[1].message, "classicChild") && Contains(nestedPlan.diagnostics[1].message, "普通框架 panel"),
        "两处改挂都要警告：" + First(nestedPlan.diagnostics));

    // 进度条按宽度显示进度，没写宽度时拒绝生成。
    UiDescription noWidth = CastBar();
    noWidth.frames[0].children[0].width.reset();
    const auto widthless = GenerateUiTriggers(noWidth, withFont);
    Require(!widthless.Succeeded() && std::count_if(widthless.diagnostics.begin(), widthless.diagnostics.end(),
                [](const auto& item) { return item.code == "AUI_TRIGGER_UNSUPPORTED" && Contains(item.message, "宽度"); }) == 1,
        "进度条没写宽度只报一次：" + First(widthless.diagnostics));
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

    // 注入文件 = JAPI 客户端库 + 界面库；这里只声明两者用到的原版类型与 native。
    constexpr std::string_view common =
        "type agent extends handle\ntype trigger extends agent\ntype timer extends agent\ntype player extends agent\n"
        "type triggeraction extends handle\n"
        "native R2SW takes real r, integer width, integer precision returns string\n"
        "native SubString takes string source, integer start, integer end returns string\n"
        "native StringLength takes string s returns integer\n"
        "native I2S takes integer i returns string\nnative S2I takes string s returns integer\n"
        "native S2R takes string s returns real\nnative GetLocalizedString takes string source returns string\n"
        "native CreateTrigger takes nothing returns trigger\nnative DestroyTrigger takes trigger whichTrigger returns nothing\n"
        "native TriggerAddAction takes trigger whichTrigger, code actionFunc returns triggeraction\n"
        "native TriggerExecute takes trigger whichTrigger returns nothing\n"
        "native TriggerEvaluate takes trigger whichTrigger returns boolean\n"
        "native IsTriggerEnabled takes trigger whichTrigger returns boolean\n"
        "native CreateTimer takes nothing returns timer\nnative PauseTimer takes timer whichTimer returns nothing\n"
        "native TimerStart takes timer whichTimer, real timeout, boolean periodic, code handlerFunc returns nothing\n"
        "native DestroyTimer takes timer whichTimer returns nothing\nnative GetLocalPlayer takes nothing returns player\n"
        "native DisplayTimedTextToPlayer takes player toPlayer, real x, real y, real duration, string message returns nothing\n";
    Require(script.starts_with("library AudastraJapi\n") && script.find("library AudastraUI requires AudastraJapi") != std::string::npos,
        "注入文件里 JAPI 客户端库在界面库前面");
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
        TestUntexturedFrames();
        TestThreadedCreation();
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
