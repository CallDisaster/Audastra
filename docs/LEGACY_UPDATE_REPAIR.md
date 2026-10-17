# 旧版本更新修复入口

此入口面向 Audastra **0.1.1、0.1.5、0.1.6 → 0.1.7** 的官方修复包。它调用现有 CLI 验签和只读计划，再启动包内的正式 updater。安装备份、同卷原子切换、运行实例检查、健康启动和回滚都由正式 updater 完成。

这是开发中的入口源码。只有发布方已完成 0.1.7 构建、签名、完整修复包及隔离安装验收后，才能作为用户交付；单独下载这个脚本不能升级。

旧版本可能因 Windows 下载来源标记 `Zone.Identifier` 在本地安装树检查时拒绝更新。目标下载包验签成功仍可能发生此问题。修复包携带已修复的外置 helper，直接使用正式更新事务；它不删除下载标记、不结束编辑器进程，也不修改签名或公钥。

## 使用

1. 从官方发布渠道取得完整修复包，并核对发布说明给出的整包 SHA256。解压到安装目录之外的独立、可写目录；修复包与安装目录不能相互包含。保留原安装路径及全部文件。
2. 在旧编辑器中保存工作并正常关闭所有窗口。脚本不会替你关闭窗口；正式 updater 仍会重新检查实际运行实例。
3. 在修复包根目录打开 PowerShell，明确传入安装目录。路径含空格时保留引号。可以先执行只读计划：

```powershell
.\RepairLegacyInstall.ps1 -InstallDirectory 'C:\Apps\Audastra' -PlanOnly
```

只读计划通过后，以同一路径执行升级：

```powershell
.\RepairLegacyInstall.ps1 -InstallDirectory 'C:\Apps\Audastra'
```

没有参数时会显示用法。若系统脚本策略阻止运行，请遵循组织或发布方提供的受信脚本运行方式；不要修改机器级策略或删除文件的来源标记。

脚本最多尝试三个固定的旧签名 baseline，并将各自全部文件逐项与正式只读计划中的现存文件长度和 SHA256 比较；仅在恰好一个版本完整匹配时继续。它不会仅凭旧旁置 manifest 的版本字符串选版，因为旧升级可能保留该旁置文件。源树在最终复查时发生变化也会停止。

签名错误、文件缺失或修改、未知 ADS、用户文件碰撞、受保护路径、来源元数据超限及仍有编辑器运行等情况，都应保留原目录和回执再排查。不要通过清除 ADS、替换单个 DLL 或重复运行来绕过拒绝。

## 修复包固定布局

发布方把仓库中的 `tools/update/RepairLegacyInstall.ps1` 放到修复包根目录，并提供以下完整布局。脚本不下载文件，也不接受用户指定清单、公钥或载荷路径。

```text
RepairLegacyInstall.ps1
LEGACY_UPDATE_REPAIR.md
helper/
  bin/
    war3-map-cli.exe
    audastra-updater.exe
    ...必要的 app-local 运行依赖
target/
  update-manifest.json
  update-manifest.json.sig
  Audastra-0.1.7-windows-x64-update.zip
baseline/
  0.1.1/update-manifest.json
  0.1.1/update-manifest.json.sig
  0.1.5/update-manifest.json
  0.1.5/update-manifest.json.sig
  0.1.6/update-manifest.json
  0.1.6/update-manifest.json.sig
receipts/                     每次调用建立独立子目录
```

旧 baseline 必须是相应正式版本的原始签名清单和签名，不能编辑版本或来源 commit。完整 portable ZIP 不能替代此处的 **update ZIP**。helper 必须与签名 0.1.7 目标同源，编译期发布公钥必须接受三份旧签名及新目标签名。脚本核对 CLI 的版本/commit，并将 helper 子树中的文件逐项对应到新目标签名文件清单；发布方仍须验收其实际加载模块、完整依赖和整包可信来源。

所有输入和 helper 都位于原安装树之外；正式事务仍会在安装目录的父目录建立同卷 next、backup 和 transaction 并切换原安装路径。脚本固定传入 `--exe bin/war3-map-editor.exe`，使用正常桌面健康启动，不传测试、自退出或伪健康参数；若当前终端包含离屏 Qt 或已知测试注入环境，会停止并要求从正常环境运行。

## 查看结果

终端仅显示版本、计划项数和本地回执位置。各 CLI 调用及 updater 的完整 stdout JSON 与 stderr 都保存在修复包的 `receipts/<本次唯一标识>/`；文件仅保存在本机。

- `-PlanOnly` 通过表示只读验证通过，尚未升级。
- 正式升级成功要求 updater exit 0、`committed`、健康验证和用户文件保留均成立，并且最终事务 journal 的目标版本、来源 commit、新旧签名清单身份和健康进程身份一致。新编辑器继续正常运行。
- 非零结果不能视为成功。`rolled-back` 表示旧版已恢复；exit 2 / `rollback-pending` 表示恢复条件仍不齐。保留全部原目录、backup、failed、next、transaction 和回执，不自动重试，也不要手工清理。

若回执提示需恢复，先保存并正常关闭仍运行的新编辑器，再由支持人员根据原结果中精确的 `transaction_directory` 使用同一外置 updater 的既有 `--recover --transaction-dir` 入口。脚本不自动恢复、清理备份或结束进程。若 updater 已报告提交，但脚本最终 journal 对照失败，也应先确认现场和回执，不能假设未安装后重新运行。
