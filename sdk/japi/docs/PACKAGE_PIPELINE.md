# 运行库包的安装目录与发布批准

运行库源码有两个 Win32 构建目标：`audastra-map-runtime` 生成地图引导入口 DLL，`audastra-module-runtime` 生成模块入口 DLL。安装包应同时包含这两个产物，并绑定本次合成的 JASS 客户端、引导器、许可和来源说明。构建成功不代表游戏验收通过，也不授予公开分发许可。

安装布局由现有 `LoadInstalledMapRuntimeSources` 服务读取：

```text
<InstallationRoot>/runtime/catalog.json
<InstallationRoot>/runtime/packages/<id>-<version>/manifest.json
<InstallationRoot>/runtime/packages/<id>-<version>/bootstrap.blob
<InstallationRoot>/runtime/packages/<id>-<version>/payload.blob
<InstallationRoot>/runtime/packages/<id>-<version>/module.blob
<InstallationRoot>/runtime/packages/<id>-<version>/client.j
<InstallationRoot>/runtime/packages/<id>-<version>/license.txt
<InstallationRoot>/runtime/packages/<id>-<version>/origin.txt
```

冻结计划使用 schema 2；`files` 的六个角色为 bootstrap、payload、module、client、license、origin，每项明确 source、size、sha256。id、version 和 `scope: "public"` 表示待公开候选的身份与用途；它们不能提高目录的批准位。客户端必须是保存链实际注入的合成文件，其摘要仍由原有服务核对。

在授权处理全部输入产物的构建环境中，先构建上述两个 DLL 与 `audastra-runtime-package-freezer`，再调用：

```powershell
pwsh tools/dev/install-map-runtime.ps1 `
  -FreezerExecutable <freezer.exe> `
  -Plan <plan.json> -PlanSha256 <plan-sha256> `
  -OutputDirectory <new-staging-installation>
```

冻结器按大小和摘要核对输入，先以内部分发用途验证完整角色、预算及固定成员，再输出 public 清单。没有提供发布批准记录时，安装脚本保持 `publicDistributionApproved: false`、`normalSaveAvailable: false`，普通保存返回 `RUNTIME_DISTRIBUTION_NOT_APPROVED`。输出目录必须全新，失败产物保留供核对，不会覆盖已有安装或登记部分包。

便携包脚本参数 `-MapRuntimeInstallationRoot <new-staging-installation>` 只复制目录中明示的六个封装文件及清单，复核角色、路径、大小和摘要，不递归携带未登记文件。已批准包还须重新核对 `approval.json`，并整体携带该记录；缺失或身份不符即拒绝，不能只改 catalog 的布尔值。未批准目录仍保持关闭。此参数不修改作者首次确认事务、地图设置或保存用途。

便携包根目录的 `manifest.json` 使 `bin/` 下程序识别父目录为安装根。因此运行库放在包根 `runtime/`，不是 `bin/runtime/`；干净安装须用包内程序实际查询确认目录可见。

作者已于 2026-10-27 批准运行库包连同现有引导文件公开，并于 2026-10-30 确认不混淆、只允许打包脚本整体复制并核对大小和 SHA-256。打包不打开、打印、比较、解析、修改引导源码，也不运行它。安装脚本把该文件整体复制为不透明封装输入交给冻结服务；既有来源说明和许可证原样保留。把文件改名为 blob 不授予任何额外的源码访问权限。

发布时显式传入 `-ApprovalRecord <approval.json> -ApprovalSha256 <record-sha256>`。记录不得含本机路径，结构如下（示意摘要须全部替换为本次实际身份）：

```json
{
  "schema": 1,
  "id": "audastra-runtime",
  "version": "0.3.0",
  "packageSha256": "<manifest SHA-256>",
  "decisions": {
    "publicDistribution": "author-2026-10-27",
    "unobfuscatedBootstrap": "author-2026-10-30",
    "wholeFileCopyOnly": true
  },
  "files": {
    "bootstrap": "<bootstrap SHA-256>",
    "payload": "<payload SHA-256>",
    "module": "<module SHA-256>",
    "client": "<injected client SHA-256>"
  }
}
```

记录只用于可信发布打包环境，不是地图内自授权或任意包批准开关。id/version、清单摘要和四角色摘要须与冻结结果完全一致，清单同时绑定许可与来源文件。记录不匹配、未知字段、重复字段、缺角色均拒绝。脚本首先核对记录自身的传入 SHA；冻结后再次核对并复制为包内 `approval.json`，只有此时才写两个批准位为 true。没有记录和没有同时传摘要时不隐式批准。开发/测试默认保持关闭。该记录不替代代码签名、更新签名或作者每张地图的首次启用确认。

可先不带记录冻结待审批目录，再按其清单及各角色身份制作记录，使用同一计划和记录安装到另一全新目录；封装来源路径不参与清单身份，文件内容必须相同。安装回执、便携包、正常保存、干净安装和游戏验收分别保留证据，静态夹具通过不代表这些出口已通过。
