# R03 · 蓝图节点内编辑

用户已指定方向：参考虚幻蓝图，把频繁输入放回节点，不再使用居中的参数弹窗。本轮继续沿用 R02 的石墨灰、薄边线和绿色选中状态。

- 视觉：节点内低对比输入框；引用显示小头像和名称；选取器贴在节点右侧，覆盖其他节点，保留画布上下文。
- 内容：只显示字段名、当前值、搜索、对象名称与 rawcode。变量和自定义表达式放在同一个小选择器内，不增加介绍文字。
- 交互：数值和文本在原位输入，Enter/失焦提交，Escape 取消；布尔直接切换。引用选择即时应用。面板短时淡入，打开节点提高层级；边缘处调整位置，滚轮在面板内部滚动。
- 范围：蓝图与属性区使用同一参数控件；传统 ECA 保留原参数对话框。本轮不修改 C++ 或地图文件。

交互参考：[Epic Blueprint Basic User Guide](https://dev.epicgames.com/documentation/unreal-engine/blueprint-basic-user-guide-in-unreal-engine)、[Epic Blueprint Nodes](https://dev.epicgames.com/documentation/unreal-engine/nodes-in-unreal-engine)。具体的节点右侧头像选择器遵循用户本次要求。
