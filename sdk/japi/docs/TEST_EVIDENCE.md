# 专用测试运行的外部证据

CLI `map-test`和编辑器测试共用启动租约。每次启动创建随机128位会话、冻结地图SHA-256和固定312字节的本方邮箱，只把会话名交给本次子进程环境。运行库不会自行创建邮箱。正常玩家没有这份邮箱，不写证据、不显示提示；原生文件诊断也只在成功附着测试邮箱时开启。

本方模块自己发布证据，宿主只映射自己的邮箱，不读取游戏地址、不查询其他实例、不依赖地图中的其他库。读取核对结构、会话、地图、原Job捕获的游戏PID和进程创建时间，一致性快照拒绝写入中间态。原进程退出或测试副本清理后，只要租约仍在，最后回执仍可查询。

`map_sha256`绑定本次测试启动时冻结的地图产物，不是从游戏中探测的当前地图摘要。手动换图后可以观察epoch变化，但不能仅凭此邮箱确认换到了哪张地图；跨图内容和可见功能仍需单独验收。

CLI输出变化的`runtime_evidence=<JSON>`，退出时再查一次。编辑器`map_test_result.runtime_evidence`使用同一服务，不带测试场景也能查。场景断言、UI可见性与握手分别验收。

| state | 含义 |
|---|---|
| `waiting_for_module` | 尚无本方入口证据，不推断加载失败 |
| `module_loaded` | 本方入口已执行，尚无客户端身份确认 |
| `handshake_success` | 当前VM成功hello，SDK比较DLL身份相同后明确确认 |
| `identity_rejected` | SDK比较到不同身份并以状态14拒绝 |
| `initialization_rejected` | 安装或进程拥有者检查失败，status保留原错误，不猜具体原因 |
| `handshake_timeout` | SDK明确超时，或当前观察起点后35秒仍无确认；后者`host_deadline=true` |

字段包含`session_nonce`、`map_sha256`、`process_id`、`process_created`、`epoch`、`runtime_sha256`、`expected_sha256`和`status`。`process_created`是Windows UTC FILETIME的100纳秒整数；`phase_started_ms`、`installed_ms`、`hello_ms`、`report_ms`是同机GetTickCount64毫秒值，0表示未到该阶段。phase起点为首次DLL入口或本次VM reset，不能把换图后的起点当成DLL重新映射；installed是原生钩子首次完成时间。宿主期限不证明SDK实际执行了第150次轮询。VM重建清除旧确认并重新计时；旧epoch不能确认新会话。

可选命令`host.testreport(i:state;i:status;s:expectedSha256)`只在可信适配器安装测试观察器时可用；普通Host返回POLICY。SDK在身份比较后报告成功或拒绝，在桥可达时报告超时，不改变原错误槽。观察器要求当前epoch、成功hello和合法SHA；成功必须与实际DLL相同，拒绝必须不同。此接口不授予路径、函数地址、执行代码或写文件能力。本机受信测试证据不是签名：地图也能请求接口，不能据此认定可见UI或全部玩法通过。

# 游戏截图身份

CLI `--screenshot-after`与编辑器首选路径共用游戏截图服务。编辑器另有系统图形捕获、窗口自绘和满足条件时的屏幕拷贝；CLI没有这些后备路径。编辑器截到图不证明入口校验通过，须看返回的`method`。

游戏截图只允许完整SHA锁定的经典1.27a Game.dll。每次请求持有原磁盘文件只读租约，核对实际模块路径、映射PE头、截图标志所在的可写MEM_IMAGE数据页，才置位已知数据槽。PE头只允许ImageBase保持磁盘原值或等于实际映射基址，其余字段逐字节一致。入口指令可能被其他组件改写；服务不执行或跟随入口，不凭跳转猜版本。身份、映射头或数据页不符仍拒绝。成功诊断`MAP_TEST_SCREENSHOT_IDENTITY`和超时诊断说明入口字节是否改变，不归因于特定组件。限时没有新帧仍失败，并撤回未消费的请求。

内部手动目标`audastra-screenshot-identity-validation`可把明确指定且SHA正确的映像以`SEC_IMAGE_NO_EXECUTE`映射为自己的数据，验证原版、入口改写、映射头变化、只读目标页、未知SHA的门禁。它不调用DLL入口、不启动游戏，不能证明实际画帧或截图；仍须专用进图复验。
