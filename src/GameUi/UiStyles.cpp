#include "War3MapCore/GameUi/UiStyles.h"

#include <array>
#include <cctype>

namespace War3Map::GameUi {
namespace {

// 模板名、大小取自游戏界面文件（EscMenuTemplates.fdf、ScriptDialog.fdf、QuestDialog.fdf、EscMenuMainPanel.fdf），
// 这些文件经 FrameDef.toc 在进图时读入。
constexpr std::array<UiStyle, 14> kStyles{{
    {"menu-panel", "面板", "菜单面板", "Esc 菜单的底板：花纹边框、深色底，做主面板或弹窗。", "EscMenuBackdrop", "panel", 0.24, 0.18, {}, false},
    {"quest-panel", "面板", "任务面板", "任务窗口里的底板：细金边、半透明底，做提示框或小面板。", "QuestButtonBaseTemplate", "panel", 0.18, 0.08, {}, false},
    {"control-frame", "面板", "控件边框", "Esc 菜单里列表、文本区外面的细边框。", "EscMenuControlBackdropTemplate", "panel", 0.16, 0.1, {}, false},
    {"dialog-button", "按钮", "对话框按钮", "对话框与 Esc 菜单的按钮：带文字，按下、悬停有变化。", "ScriptDialogButton", "button", 0.16, 0.035, "按钮", false},
    {"quest-button", "按钮", "任务按钮", "任务窗口里的大按钮，没有文字。", "QuestButtonTemplate", "button", 0.0, 0.0, {}, false},
    {"menu-checkbox", "复选框", "复选框", "Esc 菜单选项页的勾选框。", "EscMenuCheckBoxTemplate", "checkbox", 0.0, 0.0, {}, false},
    {"menu-radio", "单选按钮", "单选按钮", "Esc 菜单选项页的圆形单选按钮。", "EscMenuRadioButtonTemplate", "radio", 0.0, 0.0, {}, false},
    {"menu-slider", "滑块", "滑块", "Esc 菜单选项页的音量滑块。", "EscMenuSliderTemplate", "slider", 0.0, 0.0, {}, false},
    {"menu-editbox", "输入框", "输入框", "Esc 菜单存档页的文字输入框。", "EscMenuEditBoxTemplate", "edit", 0.16, 0.04, {}, false},
    {"menu-popup", "下拉框", "下拉框", "Esc 菜单选项页的下拉选择框。", "EscMenuPopupMenuTemplate", "popup", 0.0, 0.0, {}, false},
    {"menu-scrollbar", "滚动条", "滚动条", "Esc 菜单列表旁的竖直滚动条。", "EscMenuScrollBarTemplate", "scroll", 0.0, 0.12, {}, false},
    {"menu-textarea", "文本区", "文本区", "带边框和滚动条的多行文字区域。", "EscMenuTextAreaTemplate", "textarea", 0.2, 0.1, {}, false},
    {"title-text", "文字样式", "标题文字", "Esc 菜单的金色标题字。", "EscMenuTitleTextTemplate", "text", 0.0, 0.0, "标题", false},
    {"label-text", "文字样式", "标签文字", "Esc 菜单选项名的白色字。", "EscMenuLabelTextTemplate", "text", 0.0, 0.0, "标签", false},
}};

std::string Upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

std::span<const UiStyle> UiStyles() { return kStyles; }

const UiStyle* FindUiStyle(std::string_view id) {
    for (const auto& style : kStyles) {
        if (style.id == id) return &style;
    }
    return nullptr;
}

UiFrame StyleUiFrame(const UiStyle& style, std::string id) {
    UiFrame frame;
    frame.id = std::move(id);
    frame.type = UiFrameType::Template;
    frame.templateName = std::string(style.templateName);
    frame.anchors.push_back(UiAnchor{});
    if (style.width > 0.0) frame.width = style.width;
    if (style.height > 0.0) frame.height = style.height;
    frame.text = std::string(style.text);
    return frame;
}

std::optional<std::string> UiTemplateTextTarget(const FrameLibrary& library, std::string_view templateName) {
    std::string name(templateName);
    for (int depth = 0; depth < 8 && !name.empty(); ++depth) {
        const auto found = library.FindTemplate(name);
        if (!found || !found->definition || found->definition->kind != DefinitionKind::Frame) return std::nullopt;
        const FdfDefinition& definition = *found->definition;
        if (depth == 0 && Upper(definition.frameType) == "TEXT") return std::string();
        if (const FdfProperty* text = FindProperty(definition.properties, "buttontext"); text && !text->values.empty() && !text->values.front().text.empty())
            return text->values.front().text;
        name = definition.inherits;
    }
    return std::nullopt;
}

} // namespace War3Map::GameUi
