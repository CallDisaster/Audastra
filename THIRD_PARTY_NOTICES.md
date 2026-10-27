# 第三方声明

本项目没有复制 YDWE、HiveWE 或 w3x2lni 的实现代码。这些项目只能在遵守其各自许可证的前提下，作为外部行为参考或兼容性 oracle 使用。

应用只从项目 `ui/config` 或用户配置的外部只读数据根加载 YDWE/地图 UI 定义，仓库不包含这些文件。内部开发历史中曾短暂放入的 UI TXT 文件不随公开仓库发布：公开仓库由不含旧历史的代码快照生成。

桌面应用以动态链接方式使用 Qt 6.8.3 的 `qtbase` 与 `qttranslations` 模块。Qt 的官方来源为 <https://download.qt.io/archive/qt/6.8/6.8.3/>，其版权和许可证继续由 Qt 项目及各版权主体持有。除非使用商业 Qt 许可证，发布安装包时必须保留 Qt 声明并满足 LGPL v3 等适用条款及重新链接要求。

开发环境使用 aqtinstall 3.3.0 下载 Qt；它只是构建期安装工具，不会随 Audastra 分发。其来源为 <https://github.com/miurahr/aqtinstall>，许可证为 MIT。

未来引入任何依赖，都必须在合并前于本文登记精确版本、来源 URL、许可证和集成方式。

## 签名、更新归档与反馈报告

- libsodium **1.0.22**，官方源码 <https://github.com/jedisct1/libsodium/releases/tag/1.0.22-RELEASE>，ISC 许可证。Ed25519 校验使用原始清单字节和 detached signature。通过上游 Visual Studio 工程构建 x64 静态库，只把 ReleaseLIB 的运行库配置从 `/MT` 改为与应用一致的 `/MD`，不改加密实现。源码包 SHA256：`adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349`。
- miniz **3.1.2**，官方 amalgamation <https://github.com/richgel999/miniz/releases/tag/3.1.2>，MIT 许可证，静态编译，用于 ZIP 完整性检查和报告导出。源码包 SHA256：`f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`。
- nlohmann/json **3.12.0**，官方发布 <https://github.com/nlohmann/json/releases/tag/v3.12.0>，MIT 许可证，仅使用头文件。json.hpp SHA256：`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`。共用入口增加重复字段、字节、深度和节点数的严格门禁。

这三份许可全文位于 `third_party/audastra-update/`，随便携包保留。下载内容和编译二进制在开发缓存，不纳入仓库；固定下载和构建脚本为 `tools/dev/prepare-update-deps.ps1`。程序只接收公开的发布公钥和可选 HTTPS 端点，不带发布私钥、GitHub 令牌或其它凭据。

## Lua 5.3 语法编译核心

Lua **5.3.6**，官方源码 <https://www.lua.org/ftp/lua-5.3.6.tar.gz>，MIT 许可证；归档 SHA256 为 `fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60`。固定原始核心 C 源码在独立开发缓存，CMake 对每个使用的文件核 SHA，源码不进入项目历史。静态链接 20 个上游核心编译/内存文件，不编译标准库、lauxlib、linit、lua/luac CLI。原创有界包装只调用文本模式 `lua_load`，不执行函数、不打开标准库，不链接或复制本机 YDWE 的 lua53.dll。

完整 MIT 许可、版本说明与逐文件清单在 `third_party/lua53/`，许可随包保留为 `licenses/Lua53-LICENSE.txt`。新环境的 `prepare-update-deps.ps1` 同时准备该依赖，单独准备可用 `prepare-lua-check-deps.ps1`；CMake 显式使用 `AUD_LUA53_SOURCE_DIR`。

## StormLib 音频格式派生代码

`src/Core/Mpq/MpqAudioDecode.cpp` 与 `MpqAudioTables.h` 使用 StormLib 的 MPQ 自适应 Huffman 分布表、FGK 重平衡与 Blizzard ADPCM 格式算法，改写为有界的 C++20 解码实现。固定来源为官方仓库 <https://github.com/ladislav-zezula/StormLib/tree/44ebfbfc109d76e2a85bbd5d8b0c949df7e65c6f>，MIT 许可证，完整许可与改动范围见 `third_party/stormlib-audio/`。不链接或分发 StormLib DLL；本机 DLL 仅用作离线兼容性 oracle，游戏音频不进入仓库。

## 共享渲染核心

War3RenderCore 是同一作者的独立仓库，版本与逐文件哈希由 `War3RenderCore.lock.json` 固定，构建时作为源码编入编辑器。许可为 GNU GPL v3，另附依 GPLv3 第 7 条给出的“与 Audastra 组合”附加许可（见 `NOTICE.md` 与核心仓库 `COPYRIGHT.md`）。作者以版权人身份在 Audastra 中使用该核心。

## WarVK

运行库中注明来自 WarVK 的代码取自同一作者的 WarVK 项目（<https://github.com/CallDisaster/War3VK>，GNU GPL v3）。作者以版权人身份另行授权 Audastra 在本仓库协议下使用其持有版权的部分，见 WarVK 仓库 `LICENSE-AUDASTRA.md`。WarVK 所含的 DXVK、Dear ImGui、MinHook 等第三方组件不在该授权内，Audastra 不从中取用代码。

## AI 助手运行时

内置助手使用 Node.js 运行时（MIT，许可原文随包为 `licenses/Node-LICENSE.txt`）和 `@earendil-works/pi-agent-core`、`@earendil-works/pi-ai` 1.0.0（MIT），版本见 `tools/agent-runtime/package.json` 与锁文件。发布时由 esbuild 打包为单个 `worker.bundle.mjs`，其中包含的全部传递依赖（含 Apache-2.0 的 `openai`、`@google/genai` 等）的许可原文随包放在 `licenses/Agent-runtime/`。`node_modules` 与 Node 可执行文件不进入仓库。

## 字体与图标

思源黑体 SC（Source Han Sans）与 JetBrains Mono，均为 SIL Open Font License 1.1；由 `tools/fetch-fonts.ps1` 按 `third_party/fonts/fonts.json` 固定的版本与 SHA-256 下载，字体文件不入库，许可原文在 `third_party/fonts/LICENSES/`。通用图标来自 Lucide（ISC，部分图标源自 Feather，MIT），许可见 `assets/icons/lucide/LICENSE`；魔兽语境图标为本项目原创绘制。

## 独立游戏测试启动桥

Microsoft Detours **4.0.1**，官方源码 <https://github.com/microsoft/Detours/tree/v4.0.1>，MIT 许可证。必需源码及许可全文位于 `third_party/detours/`，来源说明与原始文件摘要见该目录的 `README.audastra.md`。只静态链接进原创的 x86 `audastra-warcraft-launcher.exe` 与 `audastra-warcraft-bootstrap.dll`，用于在游戏入口前装载扩展运行时。不复制 YDWE 启动器实现，不分发用户游戏文件或外部 JAPI 运行时；外部运行时由用户单独配置，其许可和游戏数据权利由各自来源负责。Detours 许可原文随包保留。
