# 运行库包的安装目录与待审批流水线

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

冻结器按大小和摘要核对输入，先以内部分发用途验证完整角色、预算及固定成员，再输出 public 清单。安装脚本只在冻结成功且回执身份一致后写目录，始终写 `publicDistributionApproved: false`，`normalSaveAvailable: false`。输出目录必须全新，失败产物保留供核对，不会覆盖已有安装或登记部分包。未批准 public 包的普通保存仍返回 `RUNTIME_DISTRIBUTION_NOT_APPROVED`。

便携包脚本新增可选参数 `-MapRuntimeInstallationRoot <new-staging-installation>`。它只复制目录中明示的六个封装文件及清单，复核角色、路径、大小和摘要，保持批准位关闭，不递归携带未登记文件。此参数不修改客户端批准事务、地图设置或正常保存的用途。

此流程没有决定引导器的来源、许可或混淆方案。受限制的引导文件必须由具有授权的提供方另行准备；不能用重新命名、封装成 blob 或更换 scope 绕过读取、复制或公开分发限制。已有内部引导候选也不能因此进入公开发行物。只有原创占位字节的流水线测试通过，仍不代表真实包冻结、首次选包、正常地图保存和进游戏验收完成。
