# 触发器 codec 来源与安全边界

本模块是对作者自有早期项目中触发器编译工具（`Tools/War3TriggerCompilerCpp`，该项目不公开）既有 WTG v7/WCT v1 行为的 clean-room 重构。

早期实现由本项目所有者编写。本模块没有复制 YDWE、HiveWE 或 w3x2lni 的实现源码；这些项目只能作为外部兼容性参考。

当前保证：

- 将 WTG v7 和 WCT v1 解析为不依赖 Qt、基于索引的文档模型。
- WCT custom text 由对应 `TriggerRecord` 持有，因此触发器重排时两个文件仍保持槽位对齐。
- 所有函数库（含纯数字命名的层，如 `9527/SJWE`）一视同仁：按库定义解析、编辑、删除和复制，没有按编号的只读特判（2026-09-24 移除）。解析器只保留来源 seal（原始 WTG/WCT 与 UI Catalog 的 SHA-256），serializer 据此拒绝无来源或 Catalog 已变化的文档。
- 索引 UI `config` 和已启用 layer 下的所有 `.txt`；缺失的标准文件会继续显示在 manifest 中，不会被静默跳过。
- 兼容旧版 YDWE 的 `share/mpq` 目录：根目录 `config` 仍按层序加载；层内
  `ui/TriggerData.txt` 提供动作/条件/调用的参数签名，`ui/TriggerStrings.txt`
  只补充标题、描述和帮助文本。字符串文件中没有对应 `TriggerData` 签名时不会
  凭标题虚构可编辑节点。
- 第三方 UI layer 完全来自用户数据根，不在 Core 中维护固定函数表。缺失的可选 layer 只产生警告，不会伪造定义。
- ECA 身份始终使用 WTG 中的原始函数名；面向作者的主界面由 `TriggerPresentation` 按当前 UI TXT `description` 位置模板生成自然语言，并递归展示嵌套 call、数组索引与动态预设显示值。
- 若 Catalog 缺少该函数，由于 WTG 不记录参数数量，解析会报告原名称并停止，禁止根据相邻字节猜测节点边界。不得用硬编码函数表伪造一个可编辑节点。
- `UiTextDocument` 保留 BOM、编码字节、注释、未知行和换行风格；未修改时可以逐字节一致序列化。

本模块不是 WTG→JASS compiler，也不宣称通用 MPQ/LNI 打包可以生成可运行地图。
