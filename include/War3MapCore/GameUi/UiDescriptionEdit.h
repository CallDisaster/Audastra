#pragma once

#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/Types.h"

#include <string>
#include <string_view>

// 界面描述的编辑：编辑器界面与 Agent 共用。一组编辑是 JSON 数组，每项一个 op；框架与字段用 audastra.ui 的
// 写法表示，由同一个严格读取器检查。任一步或编辑后的描述不合法时整组不生效。
//
//   {"op": "set_description", "label": "读条"}
//   {"op": "add_frame", "parent": "back", "index": 0, "frame": {"id": "fill", "type": "bar", ...}}   parent 省略为顶层
//   {"op": "set_frame", "frame": "fill", "fields": {"width": 0.2, "texture": "X", "value": null}}     null 删除该字段；
//       改 id 时同步改锚点、状态与事件里的引用；children 用 add/move/remove 改
//   {"op": "move_frame", "frame": "fill", "parent": null, "index": 2}
//   {"op": "remove_frame", "frame": "fill"}   连同子框架；指向它们的状态覆盖与事件一并删除，锚点引用须先改掉
//   {"op": "rename_value", "from": "hp", "to": "health"}   框架的取值与文字模板（含状态覆盖文字）一并改
//   {"op": "set_values" | "set_states" | "set_events", "values" | "states" | "events": [...]}   整体替换；
//       set_values 删掉的数值同时去掉它的模拟起始值，rename_value 同时改起始值的键
//   {"op": "set_simulation", "simulation": {"timers": [{"id", "label", "period", "repeat"}], "values": {"hp": 80}}}  整体替换，{} 删除
namespace War3Map::GameUi {

[[nodiscard]] Result<UiDescription> ApplyUiEdits(const UiDescription& base, std::string_view edits);

// 新建只有编号与显示名的空描述。
[[nodiscard]] Result<UiDescription> NewUiDescription(std::string_view id, std::string_view label);

// 描述里还没用过的框架编号：prefix 后接从 1 起最小的可用序号。
[[nodiscard]] std::string NextUiFrameId(const UiDescription& description, std::string_view prefix);

} // namespace War3Map::GameUi
