# Audastra

Audastra 是一个从头编写的 Warcraft III 地图编辑器，面向**经典版（1.27a）和 YDWE** 地图作者。Windows x64 原生程序，C++20 + Qt 6 Widgets。

> **预览版。** 请始终在地图副本上试用。保存时会自动备份原地图，但保存结果请自己用游戏再确认一遍。

## 本项目全面拥抱 AI 技术

Audastra 从第一行代码起就是和 AI 一起写出来的：设计、编码、审查、逆向取证和测试大量由 AI 编程助手完成，作者负责方向、决策和最终验收。编辑器本身也把 AI 当作一等公民：

- **内置助手**：在编辑器里直接让 AI 查询和修改地图。助手使用的查询和编辑命令与界面完全相同，修改进入同一套撤销历史，保存前经过同样的检查。
- **作者可读**：助手修改触发器时默认写成传统的事件/条件/动作树（T），作者能看懂、能手动改，不会把 GUI 触发器偷偷换成一段代码。
- **开放接口**：同一套能力通过 MCP 和命令行开放给外部 Agent（Claude Code、Codex 等）。
- **可选**：助手默认不开启，需要自己填写模型服务商的 API 密钥；不用 AI 也能完整使用编辑器。

我们相信 AI 能让地图创作的门槛更低、速度更快，也希望和大家一起探索它在 War3 地图制作里的更多可能。

## 功能

- **触发器**：传统事件/条件/动作树、蓝图视图、JASS / vJass / Lua 代码视图，三种视图共用同一份逻辑；变量管理、查找替换、语法检查。
- **物体编辑器**：七类物编，真实头像与中文字段名，可切换原始字段；技能等列表用可搜索的选择器，不用手输 ID。
- **物体管理**：全工程搜索和双向引用。
- **声音、资源与导入、地图设置。**
- **世界**：地形和模型预览，单位、装饰物、区域、镜头的浏览与摆放。
- **地图测试**：用独立副本启动游戏，原图不动；可选的 SLK 优化由编辑器自己完成，不需要额外工具。
- **YDWE 兼容**：读取 YDWE 1.32 的触发器 UI 定义（`ui` 目录）及其公开的编译期功能。更早的 YDWE 分支地图可以打开浏览，修改触发器后的保存还在适配，暂时会明确拒绝并说明原因。
- **原生程序**：大地图打开、大目录搜索和滚动都以不卡顿为目标。

地形保存支持高度、贴图、普通悬崖、水面和通行限制，并在需要时按实际资源重建通行，正常保存会更新小地图及图标。“计算阴影并保存”可重建受支持静态场景的阴影。支持范围和限制见[地形保存说明](docs/TERRAIN_SAVING.md)。复杂对象、新建复杂坡道和重制版（Reforged）地图仍在开发中；原版重开和游戏兼容须在副本上分别确认。

## 下载与运行

1. 在 [Releases](../../releases) 下载 `Audastra-<版本>-windows-x64.zip`，解压到任意目录。
2. 双击根目录的 `Audastra.exe`。
3. 在“资源设置”中选择本机的 Warcraft III 目录（经典版 MPQ，用于头像、模型和字段元数据）；使用 YDWE 扩展触发器时再选择 YDWE 目录。

本程序不含任何游戏数据或第三方编辑器文件。保存出来的是标准地图文件，玩家不需要为它安装任何额外组件。

新版本发布后，编辑器会提示更新；更新包经过签名校验后才会安装。

旧版本若在安装更新时出现 `UPDATE_ADS_REJECTED`，可下载 0.1.7 或更新的完整包，解压到新目录，保存并退出旧版后再启动新版；旧目录及其中的地图、工程会保留。需要继续使用原安装目录时，见[旧版升级修复说明](docs/LEGACY_UPDATE_REPAIR.md)。旧客户端的这个错误需要一次性处理，之后可以继续使用程序内更新。

## 从源码构建

需要 Visual Studio 2026（x64）、CMake 3.24+、PowerShell 7、Node.js 22.19+，以及 Qt 6.8.3 MSVC 64 位（qtbase、qtsvg、qtimageformats、qtmultimedia）。

```powershell
# 1. 界面字体（固定版本，校验 SHA-256）
pwsh tools/fetch-fonts.ps1
# 2. 共享渲染核心：按 War3RenderCore.lock.json 克隆 Audastra-RenderCore 到相邻的 War3RenderCore 目录
pwsh tools/fetch-render-core.ps1
# 3. 签名、压缩和 Lua 依赖（固定版本，校验 SHA-256；输出到一个新目录）
#    找不到 Visual Studio 时加 -MSBuildPath <VS 目录>/MSBuild/Current/Bin/amd64/MSBuild.exe
pwsh tools/dev/prepare-update-deps.ps1 -OutputDirectory <新的依赖目录>
$deps = Get-Content <新的依赖目录>/cmake-paths.local.json | ConvertFrom-Json
$defines = $deps.PSObject.Properties | ForEach-Object { "-D$($_.Name)=$($_.Value)" }
# 4. AI 助手运行时（按 package-lock.json 安装并打包）
Push-Location tools/agent-runtime; npm ci --ignore-scripts; npm run build; Pop-Location
# 5. 配置、构建、测试
cmake --preset windows-x64-vs2026 "-DCMAKE_PREFIX_PATH=<Qt 安装目录>/msvc2022_64" @defines
cmake --build build/vs2026 --config Release
ctest --test-dir build/vs2026 -C Release -j 1
```

部分测试需要本机的游戏或 YDWE 文件，找不到时会跳过。

## 许可证

- 编辑器：[PolyForm Noncommercial 1.0.0](LICENSE.txt)，源码公开，非商业使用免费。
- **用 Audastra 制作的地图归你所有**：可以自由发布和出售，包括平台上的收费地图与付费道具，不需要另外授权，详见 [NOTICE.md](NOTICE.md)。
- 出售或商业分发编辑器本身、把它做成收费服务或打包进商业产品，需要作者另行授权，见 [COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md)。
- 共享渲染核心 [Audastra-RenderCore](https://github.com/CallDisaster/Audastra-RenderCore) 为 GPLv3，附与 Audastra 组合分发的附加许可。
- 第三方组件见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

Warcraft、Warcraft III、World Editor 及相关名称归其权利人所有。本项目是独立开发的兼容工具，未获 Blizzard Entertainment 认可或背书。本编辑器不提供破解或绕过地图保护的功能，请只打开你有权编辑的地图。

## 参与

欢迎提 Issue 报告问题和建议。Pull Request 需要先签署[贡献者协议](CLA.md)，详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

Bug 反馈 QQ 群：**1127618143**。反馈时请附版本号、操作步骤及错误信息；地图和日志中的私人内容请先自行检查。

## 致谢

感谢 Warcraft III 地图社区多年来积累的工具、文档和经验，感谢 YDWE 等前辈项目为中文地图作者铺下的路，也感谢每一位愿意试用预览版、提出问题和建议的作者。Audastra 还很年轻，难免有不完善的地方，欢迎大家批评指正，我们会认真对待每一条反馈。

愿这个老游戏的地图创作，还能再热闹很多年。

---

**English summary.** Audastra is a native Warcraft III map editor for classic 1.27a and YDWE maps (Windows x64, C++20, Qt 6). It is built with AI from the ground up and ships an optional in-editor assistant plus MCP/CLI access that share the editor's own commands, undo history and save checks. Maps you make with it are yours to publish and sell; the editor itself is source-available under PolyForm Noncommercial 1.0.0. Reforged is not supported yet.
