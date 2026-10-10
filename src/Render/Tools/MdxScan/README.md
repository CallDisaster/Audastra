# MDX 编译兼容扫描器（第 2 步，非 CTest）

`war3-mdx-scan` 只读输入，不画图、不连游戏进程、不修改原始模型。它同时使用冻结的
`ParseMdx` / `SampleMdxNodePose` / `SampleMdxRigidSkinnedGeoset` / `SampleMdxGeosetAlpha`
和 RenderCore 编译/求值入口；不改变第 1 步编译检查的严格程度。

## 两种来源

Windows 从 FORGE 根工程构建目标 `war3-mdx-scan`，链接现有 RenderSupport / Core。
游戏模式使用 `GameDataSource::MountStrict`、`ListFilesWithSource` 和
`ReadFileBoundedWithSource`。只枚举 MPQ 可读 listfile 的名字，不声称枚举了全部模型；
高优先级读取失败不改读低层。TSV 的 source 是实际读取归档。

```powershell
& $scan --game-root $gameRoot --tsv "$output/models.tsv"
& $scan --game-root $gameRoot --list "$output/virtual-models.txt" --tsv "$output/selected.tsv"
& $scan --list "$output/disk-models.txt" --tsv "$output/disk.tsv"
```

UTF-8 清单每行一个 MDX 路径，允许第一行 BOM、空行和以 `#` 开头的注释。
有 game-root 时是 MPQ 虚拟路径；否则是磁盘路径，相对路径相对清单目录，不相对工作目录。
不会裁掉文件名里的空格。已有报告和伴随文件一律拒绝覆盖；输出目录先由调用方建立。

Linux 可单独配置本目录，仅支持磁盘清单，用于原创样例和工具自检：

```sh
cmake -S src/Render/Tools/MdxScan -B build/mdx-scan-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/mdx-scan-linux --target war3-mdx-scan -j 2
build/mdx-scan-linux/war3-mdx-scan --list models.txt --tsv new-report.tsv
```

## 样本与回退指标

每个动作取 0、中点，以及最多尝试前 1024 个整数毫秒找到的非按键点；每个时刻分别无相机、
带显式模型局部相机采样。全局时钟是 `elapsed+17`，不是游戏时钟。找不到非按键点记
`nonkey_unavailable`，不把按键帧冒充插值帧。样本上限截断会返回未覆盖状态。

编译成功时比较完整节点、组矩阵、透明度及诊断。节点来自旧单节点参考；组矩阵使用旧节点
结果，按原 MTGC/MATS 顺序独立 double 累加再转 float，不调用新核心的私有数学路径。
逐位对照，容差 0；同样拒绝算 matched_rejections，而不是差分。
成功路径警告按 `(诊断码, 展开文本)` 集合比较，不比较重复警告次数或排列顺序。
多个错误同时存在时，第一报错不同会诚实报告，不自动放宽检查。

编译拒绝时逐样本/逐 geoset 调旧蒙皮参考：找到一个成功且位置非空的结果就记
`legacy_positive_whole_rejected`，路径写到 `.fallback.txt`。这是“旧入口能生成局部几何姿势”，
不证明该 geoset 的材质、透明度或原版画面也正确。未找到只代表已扫描样本中没找到；
若预算耗尽则是 unknown，不能归入负例。

## 输出与退出码

一行 `SCAN_SUMMARY`，主 TSV 每模型一行，另有 `.fallback.txt` 和 `.scope.txt`。
TSV 包含解析/编译诊断、状态、样本数、逐类比较/差分计数、首个差分请求、实际来源等。
`content_digest` 是明示的非密码 `fnv1a64`，只给本次隔离编译提供来源标签，不能当 SHA-256
或持久缓存命中证明。两个比较入口使用同一份已读取字节。

- 0：选中且声明的样本范围已扫完，没有新旧差分；仍可能有真实解析/编译拒绝。
- 2：枚举截断、样本/资源/运行时间预算、子进程异常等导致未覆盖。
- 3：发现差分；即使同时有未覆盖也返回 3。首例在 summary 和 TSV 中。
- 1：参数、输出文件、枚举或控制进程错误。

`compile_codes` 统计第一条编译拒绝码；资源不足同时单列状态，不能直接解释为模型不兼容。
`.scope.txt` 保存 listfile 范围警告、枚举截断和逐状态数量。返回 0 不等于“游戏模型全部兼容”。

## 有界运行

默认：单模型 5000 ms、全部扫描 300 s、每子进程 512 MiB、最多 10000 模型、文件 32 MiB、
每模型最多 4096 样本 / 4096 节点 / 262144 顶点。对应参数见 `--help`。
清单最多 16 MiB / 100000 行，每行至多 65536 字节，模型路径至多 8192 字节。
编译资产与求值器仍受 RenderCore 本来的预算约束。

每个模型一个顺序子进程，连枚举也在受限子进程内完成。Windows 挂起创建后加入 Job，
限制提交内存、单活动进程、关闭即结束、自身未处理异常无对话框；加 Job 失败就拒绝运行。
Linux 是 RLIMIT_AS 地址空间上限和父进程 wait/kill；这两种内存口径都不是 RSS。
worker 定期写进度；父进程墙钟兜底（模型预算另留 250 ms 写报告余量）。单模型停止后继续下一个。
总时间到后不再启动新模型，仍把余下已选名字写成未扫描行；收尾文件 I/O 不作实时性承诺。

父进程内存由清单/行数/字段长度上限约束，且只同时持有一个模型回执；不受每子进程 Job 配额。
被 OS 强制结束而来不及写回的模型标成子进程/报告不完整，不猜测具体拒绝原因。
这是有界批处理工具，不是运行任意恶意程序的安全沙箱。

## 原创自检（手工执行，不进 CTest）

显式构建 `war3-mdx-scan-fixtures`，传一个不存在的新目录，再扫描其中 `models.txt`。
它生成正常、局部可用/整模型拒绝、边界拒绝、全局序列和截断共 5 个原创模型；另实际启动
超时和内存探针子进程。Linux 本轮：5 模型 / 4 解析成功 / 3 编译成功 / 1 局部可用整模型拒绝，
37 样本（含 1 次回退证明），差分 0。这些数字不是任何真实游戏资源的拒绝率。
