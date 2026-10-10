# 来源登记

## 内部来源材料

触发器实现可以从作者自有的早期项目中的既有代码重构（该项目不公开）。2026-07-18 审阅的基线文件如下：

| 来源 | SHA-256 |
|---|---|
| `Tools/War3TriggerCompilerCpp/src/TriggerFormat.h` | `77B1942602CDBB35D96B8E31B3AEB8ADF546AAB86BDFD2AD90D58095529D8B84` |
| `Tools/War3TriggerCompilerCpp/src/TriggerFormat.cpp` | `2F642F605FD199521414D5E88CE11B3B9E02D7618C9C73659B2A576D22244D88` |
| `Tools/War3TriggerCompilerCpp/src/Main.cpp` | `8FB8F279F4A3261EB533FD964B115110215FDF43A02A9C87A58023974FA3F05E` |
| `Doc/War3MapEditor64_Architecture_and_Roadmap_2026-07-18.md` | `40AD392F347BB287A1DC84D8558FE3882F53E607231A704EBE4A0A8B57BED8F7` |

上述来源明确记录为 clean-room 实现，没有复制 YDWE 或 w3x2lni 代码。任何导入代码都必须适配新的 Core 边界，并在公开前完成审查。

## 外部参考

- YDWE：仅作为行为与元数据兼容性参考。
- HiveWE：仅作为架构和产品参考。
- w3x2lni：仅作为可选的外部转换 oracle。

本登记不包含上述项目的实现代码。未来若需要引入，必须先进行独立许可证审查，并同步更新 `THIRD_PARTY_NOTICES.md`。

## 2026-07-28 UI TXT 来源更正

早期私有提交曾把从用户安装环境取得的 36 个 YDWE/地图 UI TXT 文件复制到 `data/ui`。这些文件不是本项目原创源码，也不应作为编辑器的固定函数库。当前 P0 工作树已删除这些跟踪文件并忽略 `data/ui`；运行时只允许读取项目自身的 `ui/config` 或用户选择的外部只读 UI 数据根。

普通删除不会移除旧提交中的对象。旧 Git 历史目前仍包含上述 TXT；仓库公开前必须取得明确授权后执行 history rewrite/force-push，并重新审计全部对象、提交与构建产物。本轮仅清理当前树，不改写历史。
