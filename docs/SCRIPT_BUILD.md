# 脚本编译与后端选项

普通保存、另存为，以及编辑器和独立 MCP 的保存入口，按实际工具链配置生成并检查脚本。工具链可以使用内置编译后端；需要且已经配置时，也可以使用外部编译器回退。未知语法、缺少定义或编译失败会明确拒绝本次保存，保留当前编辑。

命令行需要只用内置后端时，在正常保存参数后追加 `--in-process-only`：

```text
war3-map-cli map-edit-save <source> <source-sha256> <w3e-sha256> <tool-root> <game-root> <new-candidate> --in-process-only <编辑参数>
```

地图自定义设置等不需要地形身份的操作，仍按该命令的要求填写 `-`；此选项不改变领域参数和来源核对。它只限制本次调用，不修改地图、工程或工具目录的默认配置。若该次脚本需要外部预处理、vJass 后端或外部回退，会返回 `SCRIPT_IN_PROCESS_BACKEND_REQUIRED` 等具体诊断，不会静默执行外部程序。

保存回执中的 `script_backend_policy=configured` 表示按配置选择后端，`in_process_only` 表示本次显式限制；前者不等于实际一定启动了外部程序。脚本检查本身也可以使用此选项：

```text
war3-map-cli script-build <ydwe-root> <war3-root> <map> <out.j> --compile <work-dir> --in-process-only
```

内置编译链不把 `.ai` 文件当作脚本文本来源。直接、嵌套和强制包含、vJass import，以及编译模板读取这类文件时，会在打开前返回 `SCRIPT_INCLUDE_AI_UNSUPPORTED`，说明“AI 文件按不透明成员保留，不作为文本包含”。这条规则默认生效，与是否填写 `--in-process-only` 无关；合法的 `.j` 包含不受影响。

地图中的 `.ai` 导入仍由正常保存链按不透明成员保留；拒绝文本包含不表示删除它，也不改变地图调用 AI 的方式。单独选择的外部编译器有自己的文件读取行为；需要保证当前编译只经过内置链时，应显式使用上述选项。
