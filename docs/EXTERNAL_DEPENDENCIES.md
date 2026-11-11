# 外部依赖目录

在“应用设置 → 地图测试”填写“外部依赖目录”。留空时使用安装根的 `外部依赖` 文件夹。点击“打开文件夹”时按需创建。在线更新按用户文件保留默认目录和已有内容，安装包由用户自行补充依赖。

在该目录下为每个提供者建一个子目录。把来源明确的 x86 DLL 和同一提供者发放的 `provider.json` 放在一起。也可把一份声明及 DLL 直接放在根目录。请使用提供者发布方核对过的声明。DLL 会在测试游戏进程中执行，来源应与安装的其他本机软件一样可信。

```text
war3-map-cli external-dependencies-status
war3-map-cli external-dependencies-status "D:/Local/MapTestDependencies"
war3-map-cli map-test demo.w3x "D:/Games/Warcraft" "D:/Audastra/bin" --external-dependencies "D:/Local/MapTestDependencies"
```

状态回执包含路径、SHA-256、PE 检查、导入关系、入口、声明的 native 和问题。未关联声明的 DLL 列在 `undescribed_dlls`。编辑器的 MCP 可用 `external_dependencies_status` 查询本机目录。状态查询只读取文件。

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

- 模块按数组顺序载入。文件名为同目录下的 ASCII DLL 名。静态导入须指向之前声明的模块或系统库，延迟导入会被拒绝。每次测试最多选择 8 个模块、256 个 native。
- 有 native 的模块须导出 x86 ABI v1 登记入口，见 `War3MapCore/Runtime/TestNativeProviderAbi.h`。入口是 `BOOL WINAPI Register(AddTestNative add, void* context)`，在游戏的登记窗口同步调用 `add`，传入本模块内的可执行函数地址、名称与 1.27a 登记签名。模块加载阶段只完成普通 DLL 初始化。文件名或任意 `Initialize` 导出本身无法证明该 ABI。
- `I`、`R`、`B`、`S`、`C` 分别表示 integer、real、boolean、string、code，`V` 表示无返回值，`H类型;` 表示游戏句柄。括号内为参数，括号后为返回值。
- 地图声明、提供者声明与游戏实际登记互相核对。缺名、签名不符、同名冲突、文件身份变化、来源不明和超出登记窗口都会报告。地图归档里的 DLL 不会被提取为提供者。

## 测试环境选择

JASS 或 UTF-8 的自带 Lua 测试先尝试由本机声明完整覆盖已知扩展名称。相应提供者加载并通过实际登记后，Lua 通过 `require("jass.japi")` 取得函数，继续使用原有的类型、句柄和行为限制。外部 `code` 回调在调用前拒绝。字面量和简单 require 别名可作启动前检查，动态键由会话查找。

旧版六 DLL 布局仍由“扩展运行时目录”或已配置的 YDWE 数据目录供旧引擎使用，显式旧目录优先。旧路径保留原 PE 和初始化检查，无法给出本页声明的逐接口登记证明。明确选择 YDWE Lua 或已有旧编码回退时沿用旧引擎。通用提供者与旧六 DLL 初始化链各自运行。

游戏测试的独立运行回执记录实际模块路径、预期与实际 SHA-256、加载状态和 `providerRegistrationsVerified`。缺少来源明确且兼容的 DzAPI / EX 实现时，仍需由发布该实现的一方提供相应文件和声明。
