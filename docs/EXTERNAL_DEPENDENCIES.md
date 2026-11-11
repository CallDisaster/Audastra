# 外部依赖目录

在“应用设置 → 地图测试”填写“外部依赖目录”。留空时使用安装根的 `外部依赖` 文件夹。点击“打开文件夹”时按需创建。在线更新按用户文件保留默认目录和已有内容，安装包由用户自行补充依赖。

外部依赖目录可直接存放已知布局，或为每个提供者建一个一级子目录。使用其他布局时，把提供者发放的 `provider.json` 和同一版本的 x86 DLL 放在一起。只有测试启动时选中的提供者会在游戏进程内执行，请使用可信来源。

```text
war3-map-cli external-dependencies-status
war3-map-cli external-dependencies-status "D:/Local/MapTestDependencies"
war3-map-cli map-test demo.w3x "D:/Games/Warcraft" "D:/Audastra/bin" --external-dependencies "D:/Local/MapTestDependencies"
```

CLI 默认读取编辑器的目录设置，显式目录优先。状态回执包含路径、SHA-256、PE 检查、导入关系、入口、声明或静态候选的 native 和问题。未识别的 DLL 和子目录分列。MCP 的 `external_dependencies_status` 支持 `offset` 和 `limit`，按 `next_offset` 继续读取。状态查询只读取文件。

## 已知目录布局

- **YDWE EX** 支持六个 DLL 平铺，或保留 `bin/{ydbase,lua53,ydwar3}.dll` 与 `plugin/warcraft3/{yd_loader,yd_jass_api,yd_lua_engine}.dll`。目录状态会检查 x86 PE、导入关系、两个 `Initialize` 导出与各文件 SHA，并列出从插件字节找到的 EX 名称候选。JASS 测试保留旧初始化链。自带 Lua 只接受两个入口同步执行期间、地址属于这六个已核验模块的登记，所需名字和签名要与地图声明相符。测试回执列出实际观察到的名字。
- **KKWE Dz** 可识别 `dz_w3_plugin.dll` 与相邻的 `version.dll`，包括其原发行版的 `插件` 子目录。该布局目前只报告静态状态。独立离线加载、登记时机和与其他运行时的兼容性有待验证。

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

- 模块按数组顺序载入。文件名为同目录下的 ASCII DLL 名。静态导入须指向之前声明的模块、系统库或当前游戏的 `Game.dll` / `Storm.dll`，延迟导入会被拒绝。每次测试最多选择 8 个模块、256 个 native。
- 有 native 的模块须导出 x86 ABI v1 登记入口，见 `War3MapCore/Runtime/TestNativeProviderAbi.h`。入口是 `BOOL WINAPI Register(AddTestNative add, void* context)`，在游戏的登记窗口同步调用 `add`，传入本模块内的可执行函数地址、名称与 1.27a 登记签名。模块加载阶段只完成普通 DLL 初始化。文件名或任意 `Initialize` 导出本身无法证明该 ABI。
- `I`、`R`、`B`、`S`、`C` 分别表示 integer、real、boolean、string、code，`V` 表示无返回值，`H类型;` 表示游戏句柄。括号内为参数，括号后为返回值。
- 地图声明、提供者声明与游戏实际登记互相核对。缺名、签名不符、同名冲突、文件身份变化、来源不明和超出登记窗口都会报告。地图归档里的 DLL 不会被提取为提供者。

## 测试环境选择

JASS 地图声明、且原版 `common.j` 没有的 native 是必需项。普通 JASS 与 UTF-8 手写 Lua 测试优先选能覆盖必需项的声明式提供者，再考虑外部依赖目录中识别的 YDWE EX 布局。Lua 的字面量 `jass.japi` 查询是可选项。查不到时仍可启动，预检会提示，运行时查询返回 `nil` 并留下诊断。动态查询由会话处理。已核验的外部调用沿用原有的参数、返回值、句柄和行为限制，外部 `code` 回调在调用前拒绝。由触发器生成的经典 Lua 使用原版全局绑定，含外部 JASS 声明的生成图会在启动前拒绝。

外部目录不可用时会报告原因，并保留旧的扩展运行时选择。旧设置中显式“扩展运行时目录”优先于从 YDWE 数据根推导的位置。明确选择 YDWE Lua 或旧编码回退时沿用旧引擎；普通可选 Lua 查询不会触发回退。旧初始化链不代表逐接口登记已验证。

启动诊断与回执列出所选提供者、文件路径、预期与实际 SHA-256、加载与登记状态。地图内的 DLL 只出现在警告中，不参与提供者选择。
