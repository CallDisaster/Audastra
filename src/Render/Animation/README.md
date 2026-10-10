# 模型动画服务（W1，第 3 步）

这是 FORGE 的 Qt-free 实例动画服务，不是新引擎或 WarVK 适配器。
质量仅为 `PreviewApproximate`；不改变 RenderCore 的加载校验、参考求值器或插件 V2 布局。

## 数据链

`实际来源读取器 → 一次编译/材质几何 → 独立实例状态 → GroupsAndAlphas 分批 → 自有帧包 → V2 蒙皮槽`

`ModelAnimationService::Create` 只在来源/静态布局改变时调用。宿主给非零代次、实例身份、
宿主行键、文档修订以及来源身份；读取器返回实际命中层、字节和 SHA-256。资产拥有解析结果和
编译结果，不借外部 MDX 内存。材质、队伍色和替换贴图复用 `WorldModelScene` 的 Builder。
同模型不同队伍色沿用旧材质变体规则，可能有不同几何变体；不要把不同变体当作唯一几何。

`Prepare(previous, command)` 是单所有者调用：不读系统时间，不读资源，也不提交 GPU。
输入可以是显式 dt（至多1000 ms）、实例编辑、播放/暂停和模型世界相机。每个实例保留自己的
动作、时间、速度、分数毫秒；普通 dt 只推进 playing 实例，单步固定由宿主传33 ms并允许暂停实例推进。
动作解析使用实际 SEQS，全名优先，其次完整首词的数字变体，不硬编码 Walk 序号。
默认首帧取动作中点；命令里显式时间不被偷偷改成中点。全局序列时间也显式保存，不冒充游戏时钟。

每个批次至多64个实例，预热预算仍为64MiB；单模型过大时缩小批宽。GroupsAndAlphas 结果立即
复制到本帧拥有的 groups/alphas，下一次求值不会覆盖先前的帧包。几何按 model 排序，
`poses` 保存原请求索引、渲染索引、实例身份、poseEpoch/poseVersion、组/透明度范围；
geoset 透明度按编译资产顺序映射到实际 mesh。整个动态包另有 contentVersion。
这里的自有帧包和编辑/诊断容器会分配，**不声称服务每帧零分配**；核心内部的预热复用保持原样。

取消任务时丢弃其帧包；下一次从最后被宿主接受的 previous 再推进。版本号允许有间隙，
数值相同时版本仍递增。CPU接收不等于GPU完成，真正截图必须走 AnimationRenderer::Capture。
源代次或文档修订不符的 previous 拒绝。公开 C++ 结构是进程内契约，不是可直接传输的 IPC ABI。

## 一个模型槽，静态内容不消失

选择统一蒙皮槽：编译成功的模型使用真实组矩阵；世界坐标烘焙悬崖每个 mesh 使用一个恒等组。
整个旧材质/模型排序、地形与叠加层顺序保留，不另建静态槽与蒙皮槽之间的透明排序规则。
代价是静态 mesh 每动态包也带48字节恒等矩阵，以及少量动态元数据；不重传它的顶点和贴图。

世界浏览允许**整模型静态保留**，并报告 `ANIMATION_STATIC_RETAINED` 与原因。这不降低
CompileMdxModel 的严格程度，不将旧局部 geoset 输出拼成新动画成功。整个世界首帧求值失败时，
世界 UI 明确禁用动画并回到原静态显示；旧静态路径本来不可显示的对象仍有旧诊断。
独立单位预览/CLI 默认严格，不能靠静态保留通过 W1。

旧 `BuildWorldModelScene` / 显式刚性姿势 / `war3-model-preview` 不变。
新 `BuildAnimatedModelGeometry` 只接受已解析/编译资产提供器，复用原 Builder。
世界动画的 CPU 点选快照只有实例和模型 AABB，没有大量顶点/贴图副本；不提供骨骼精确点选。

## 预算与来源

默认模型原始读取总量256MiB，单模型/单贴图16MiB；几何与贴图沿用 WorldModelLimits。
单动态帧有效存储估计至多64MiB，groups和mesh透明度池分别至多1,048,576；没有地图实例总数8192门禁。
全量请求、实例身份、状态、点选盒按实际 owned 字节核算。显式世界预览在逐实例姿势超过预算时，
用同一个核心真实求值每模型的 Stand（没有 Stand 时为实际 SEQS 0），保留同模型共享姿势片段，
并按摄像机距离与原序确定动态子集；每批仍至多64实例。缓存记录实际动作和时间，不把未计算的
请求时钟说成已经显示的姿势。静态实例每帧仍有 O(N) 状态与变换成本，近处筛选成本为 O(N log K)。
共享片段明确要求插件矩阵模式2；旧连续模式1不放宽。基础身份或真实 Stand 自身超内存预算时
仍明确拒绝动画，宿主保留原静态模型和悬崖；不静默截断地图实例。粒子绑定按实际模拟预算收紧，
未获动态姿势或粒子配额的对应实例保留模型并有逐实例原因；纯粒子对象按现有源次序分配绑定。
这不是进程RSS硬上限：解析/编译副本、上一帧、候选帧、插件深拷贝、GPU双缓冲分别有成本。

Windows 的 AnimationSourceGuard 在读前锚定文件身份；游戏包名单复用 GameDataSource。
地图与导入文件、已有或缺失的高层游戏包在复用前检查，出现替换/新增高层包则拒绝旧缓存。
它允许保存流程 rename，拒绝继续把旧句柄当当前来源；没有修改来源文件。
完整来源租约、Windows大小写目录行为和竞态需要本机验证，不能用Linux测试替代。

## Qt 与动态提交

WorldAnimationPlayer 只做按钮、QTimer→显式dt、单worker调度和提交；求值/时钟语义都在服务内。
世界页“播放 / 暂停 / 单步”和选中单位的动作选择/播放使用同一个控制层。
默认目标约30fps；计算+GUI提交超过16ms时降频，超过500ms暂停。V2动态接收仍有深拷贝，
GUI提交成本只能测量/降频，不能承诺大地图UI零阻塞。忙时单步按钮禁用，不悄悄丢掉多次点击。
暂停不继续提交矩阵；相机在暂停时可动，但 billboard 维持冻结姿势，直到单步/播放才重算。

保守边界：已提交文档修订（含撤销/重做）或悬崖拓扑变化会重建一次新场景与来源代次；
纯pose播放/暂停/单步不重传静态资产，临时拖动走动态变换。不要将此描述为任意地图编辑都不重载。
世界页默认各实例选择自己的真实 Stand，中点起步；本轮不同 Stand/Walk 的同帧展示由无窗口
多实例命令完成。选中单位预览的动作选择只改独立预览，不改变地图中的单位或玩法。

## 命令与回执

新可执行程序 `war3-model-animation` 使用游戏根和 `W1_Footman.tsv`，同一进程连续截三帧。
默认WARP，`--hardware` 才用硬件；插件固定来自宿主exe目录，不接受外部DLL路径。
输出目录必须不存在，父目录须存在；产出三份BMP/JSON及最后的report.json。
中途失败可留下先前帧或空目录，但没有成功的最终报告；重试必须使用新目录。
所有源图、游戏资源只读。

JSON 的大整数用字符串；每实例含身份/代次、真实动作序号/名称、显式时间、poseEpoch/version、
世界变换和矩阵/透明度FNV指纹。整帧含实际scene/content版本、相机、尺寸、ticket、各epoch、
静态/矩阵上传计数、BMP SHA、实际资源来源及场景/姿势诊断。FNV是诊断指纹，不是逐字节证明或资源摘要。
CLI的 `--revision` 是调用者赋予的预览修订，不是活动Qt地图的未保存修订。

`VerifyW1.py <输出目录>/report.json` 核对这份三帧脚本的回执一致性。只有Stand不变、Walk+33、
重复帧数值指纹/图片一致、版本递增、静态上传1次才通过；仍需人查看两个Footman是否真实正确显示。

MCP `capture_model_animation` 接收 `plan_tsv`、`output_name`、可选width/height。
宿主必须设置 `FORGE_MCP_WAR3_ROOT`、`FORGE_MCP_ANIMATION_OUTPUT_ROOT`。返回报告路径，
与CLI调用同一 RunModelAnimationCapture，不读取活动Qt地图，也不在协议里塞Base64图片。
当前是同步、有界帧数的独立渲染工具，不是实时远控世界视图的流式接口。

## 独立 CPU 验证

```sh
cmake -S src/Render/Animation -B build/animation-cpu -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/animation-cpu
ctest --test-dir build/animation-cpu --output-on-failure -V
```

这条入口不链接整个Core/Qt/Windows，编译生产服务加必需的旧解析/图像支持切片。
GCC/Clang下只读BlpImage.cpp有一处已有origin省略初始化告警，保留告警、不把它提升为错误；
新服务/测试仍用-Werror。Windows、资源租约、FXC/WARP和真实窗口必须由本机单独验收。
