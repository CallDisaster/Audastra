# 外部依赖目录

在“应用设置 → 地图测试”填写“外部依赖目录”。留空时使用安装根的 `外部依赖` 文件夹。点击“打开文件夹”时按需创建。在线更新按用户文件保留默认目录和已有内容，安装包由用户自行补充依赖。

外部依赖目录可直接存放已知布局，或为每个提供者建一个一级子目录。使用其他布局时，把提供者发放的 `provider.json` 和同一版本的 x86 DLL 放在一起。不要把安装根、盘符根或其他包含 `test-maps` 的目录选为外部依赖目录。默认位置若经 OneDrive 云占位或目录联接，请在设置里改用普通本机文件夹。

**信任边界：** 选中并加载的 DLL 是完全受信的本机代码。编辑器会核对加载来源和 SHA-256，并核对声明式提供者的登记，但不沙箱化 DLL。只安装你信任、且适用于本机游戏的文件。

```text
war3-map-cli external-dependencies-status
war3-map-cli external-dependencies-status "D:/Local/MapTestDependencies"
war3-map-cli map-test demo.w3x "D:/Games/Warcraft" "D:/Audastra/bin" --external-dependencies "D:/Local/MapTestDependencies"
```

CLI 默认读取编辑器的目录设置，显式目录优先。状态回执包含路径、SHA-256、PE 检查、导入关系、入口、声明或静态候选的 native 和问题。未识别的 DLL 和子目录分列。MCP 的 `external_dependencies_status` 支持 `offset` 和 `limit`，按 `next_offset` 继续读取。状态查询只读取文件。

## 已知目录布局

- **YDWE EX** 支持六个 DLL 平铺，或保留 `bin/{ydbase,lua53,ydwar3}.dll` 与 `plugin/warcraft3/{yd_loader,yd_jass_api,yd_lua_engine}.dll`。目录状态检查 x86 PE、导入关系、两个 `Initialize` 导出与各文件 SHA，并列出 EX 名称候选。识别到的布局等同于配置了 YDWE 目录：JASS 测试用旧初始化链；手写 Lua 需要这些扩展时退回 YDWE Lua 引擎。这六个 DLL 不加载到自带 Lua 会话。
- **KKWE Dz** 可识别 `dz_w3_plugin.dll` 与相邻的 `version.dll`，包括其原发行版的 `插件` 子目录。当前只显示静态状态，不加载这套文件。独立离线登记和兼容性有待验证。

## 提供者声明

```json
{
  "schema": 1,
  "id": "example_provider",
  "abi": "audastra-native-provider-v1",
  "modules": [
    {"file": "example-support.dll", "entry": "", "natives": {}},
    {
      "file": "example-native.dll",
      "entry": "RegisterExample",
      "natives": {"ExampleAdd": "(II)I", "ExamplePlayer": "(Hplayer;)Hplayer;"}
    }
  ]
}
```

- 模块按数组顺序载入。文件名为同目录下的 ASCII DLL 名；旧 YDWE 六个文件名保留给旧链。静态导入须指向之前声明的模块、系统库或当前游戏的 `Game.dll` / `Storm.dll`，延迟导入会被拒绝。每次测试最多选择 8 个模块、256 个 native。
- 有 native 的模块须导出 x86 ABI v1 登记入口，见 `War3MapCore/Runtime/TestNativeProviderAbi.h`。入口是 `BOOL WINAPI Register(AddTestNative add, void* context)`，在游戏的登记窗口同步调用 `add`，传入本模块内的可执行函数地址、名称与 1.27a 登记签名。模块加载阶段只完成普通 DLL 初始化。文件名或任意 `Initialize` 导出本身无法证明该 ABI。
- `I`、`R`、`B`、`S`、`C` 分别表示 integer、real、boolean、string、code，`V` 表示无返回值，`H类型;` 表示游戏句柄。括号内为参数，括号后为返回值。
- 地图声明、提供者声明与游戏实际登记互相核对。缺名、签名不符、同名冲突、文件身份变化、来源不明和超出登记窗口都会报告。地图归档里的 DLL 不会被提取为提供者。

## 测试环境选择

地图的 JASS 脚本声明、且原版 `common.j` 没有的 native 是必需项。普通 JASS 与 UTF-8 手写 Lua 测试按以下顺序选择：

1. 声明式提供者能覆盖本图全部必需接口：用选中的提供者，游戏需要可核验的 1.27a。
2. 覆盖不全或没有匹配：先看显式“扩展运行时目录”（`mapTest/runtimeRoot`），再看外部依赖目录中识别且候选接口匹配的 YDWE，最后看从 `ui/dataRoot` 推导的 YDWE。外部目录不可用会给出原因；不相关的提供者不改变旧路径。旧初始化链不代表逐接口登记已验证，JASS 走旧链不额外要求声明式提供者的 1.27a 身份门禁。
3. 两条路都不可用：游戏启动前报告缺口和设置办法。明确选用 YDWE Lua 或源文件不是 UTF-8 时，走旧引擎路径。

Lua 的 `jass.japi` 取值是可选项，不会选中额外提供者或切换引擎。找不到接口时返回 `nil` 并记诊断；静态扫描未完成时预检给警告，正常语法检查仍然生效。外部调用沿用现有参数、返回值、句柄和行为限制，外部 `code` 回调在调用前拒绝。由触发器生成的经典 Lua 使用原版全局绑定，含外部 JASS 声明的生成图会在启动前拒绝。

启动诊断与回执列出所选提供者、文件路径、预期与实际 SHA-256、加载与登记状态。地图内的 DLL 只出现在警告中，不参与提供者选择。
