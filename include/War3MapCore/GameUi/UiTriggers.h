#pragma once

#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <string>
#include <string_view>
#include <vector>

// 界面描述生成自然 T：一段功能写法（Trigger/TriggerFeatureText.h），写进触发器编辑器的 AudastraUI 类别。
// 触发器体是普通事件/条件/动作树，调用 AudastraUI 函数库（assets/ui-library/AudastraUI）的动作与调用，作者可读可改。
// 生成的触发器：创建（地图开始 0 秒）、更新（按数值变量刷新绑定的属性）、每个状态一个、回到基础状态一个。
// 触发器名为“<界面名> <用途> <界面编号>_<Create|Update|State_状态编号|BaseState>”，例如“读条 更新 cast_Update”；
// 末尾的 ASCII 部分让脚本名 gg_trg_ 互不相同（中文在脚本名里都成下划线）。
// 框架句柄存在全局变量里；界面事件按“界面编号.事件编号”关联，作者自己的触发器用“界面事件”接收。
namespace War3Map::GameUi {

inline constexpr std::string_view kUiTriggerCategory = "AudastraUI";
// 生成的触发器依赖的基础模板；两份文件随生成一起导入地图。
inline constexpr std::string_view kUiTemplateTocMember = "AudastraUI\\AudastraUI.toc";
inline constexpr std::string_view kUiTemplateFdfMember = "AudastraUI\\AudastraUI.fdf";

struct UiTriggerOptions {
    // 判断“模板”框架是不是 Simple 类（决定用哪条创建）；为空或找不到模板时按普通框架创建并给出警告。
    const FrameLibrary* library = nullptr;
    // 地图自己的界面目录，创建框架前依次加载。
    std::vector<std::string> tocFiles;
    // 游戏默认字体的路径（皮肤 MasterFont）；只改字号、没写字体时用。
    std::string defaultFont;
};

struct UiTriggerPlan {
    std::string featureText;
    std::vector<std::string> triggers;   // 生成的触发器名，按文本顺序
    std::vector<std::string> variables;  // 生成的全局变量名（不带 udg_），按文本顺序
};

// 描述检查不通过时原样返回其诊断；生成不了的部分（例如锚在游戏代码区域、改字号却没有默认字体）一次全部列出并失败，不给部分结果。
// 成功时 diagnostics 里可能有警告（例如模板不在已读入的界面目录里）。
[[nodiscard]] Result<UiTriggerPlan> GenerateUiTriggers(const UiDescription& description, const UiTriggerOptions& options = {});

[[nodiscard]] std::string UiTemplateTocText();
[[nodiscard]] std::string UiTemplateFdfText();

// 变量名（不带 udg_）：框架 AUI_<界面>_<框架>，数值 AUIV_<界面>_<数值>，当前状态 AUIS_<界面>。
[[nodiscard]] std::string UiFrameVariableName(std::string_view description, std::string_view frame);
[[nodiscard]] std::string UiValueVariableName(std::string_view description, std::string_view value);
[[nodiscard]] std::string UiStateVariableName(std::string_view description);
// 生成的触发器名：<界面名> <用途> <界面编号>_<键>；界面名为显示名（不可用时为编号）。
// 用途与键：创建/Create、更新/Update、“状态 <状态名>”/State_<状态编号>、基础状态/BaseState。
[[nodiscard]] std::string UiTriggerName(const UiDescription& description, std::string_view role, std::string_view key);
// 界面事件名：<界面编号>.<事件编号>。
[[nodiscard]] std::string UiEventKey(std::string_view description, std::string_view event);

} // namespace War3Map::GameUi
