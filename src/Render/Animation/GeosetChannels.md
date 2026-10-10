# GEOA 颜色与透明度：通用预览合同

静态 A 和动态 B 均不依赖粒子接口，未修改地图、未烘焙顶点或贴图。
静态GEOA颜色沿文件RGB直接消费，KGAC动态值及切线由文件BGR转为RGB一次；
二者布局由1.27a读取与消费链分别取证，不能套用同一交换。0x2启用颜色，0x0的
不活动颜色为白。私有共享0.1.5实现该修正，证据在round4-n2-model-colors-20261002。
0x1 已经由 2026-09-29 的只读 IDA 确认为 MDL `DropShadow` 标记：token表
`0x6FA647D0` 的339对应字符串 `0x6FA64EB0`，`0x6F82C150` 将GEOA+0x34传入
`0x6F82C2A0`；其调用的 `0x6F82B9B0` 对token339返回1，随后OR入标志字段。
二进制读取器 `0x6F82BD70` 也将GEOA的flags写入+0x34。原Game.dll SHA-256为
`e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a`。
2026-10-02补充消费链证明该位会通过compiled node+B1 bit40进入0x6F77E210，
改变最终组矩阵：去旋转保列长度、组合context基并修改平移。上层packet语义尚未
闭合，当前仍拒绝0x1，不将其当作无影响元数据。透明度/颜色轨迹独立，不能填alpha1。
完整有限调用证据见round4-n2-model-colors-20261002/geoa-drop-shadow。
早期原始指令/伪代码见
`artifacts/astra-rounds/takeover-20260929/geoa-native-readonly.json`。
其它未知位、未知/重复轨迹及无法解释的尾部继续拒绝。

## 插值与边界

KGAO/KGAC 支持 0阶梯、1线性、2Hermite、3Bezier；曲线使用现有节点通用公式的
固定 double 运算顺序后落 float。参考扫描原始按键，核心加载时编译/校验后做二分；
两者不调用对方的数学结果。所有输出仍 PreviewApproximate，不是原版 SSE/量化证明。

普通序列用显式动作时间。全局轨迹先按真实首键归一；正周期使用globalMs % period，
零周期采用已取证的归一时刻0，单键直接取该键。不存在的序列引用仍拒绝。
只采样落在该序列区间内的按键；区间无键用该通道静态值。
阶梯模式保留旧规则：首键前拒绝，末键后保持最后值。
其它模式只接受确切按键或两个真实按键之间；末键后/首键前不补键、不回绕拼曲线。
动作区间只有一个有效键时直接取该值，依据1.27a的单键分支；不编造相邻键。

颜色静态值/端点要求有限且0..1，切线要求有限但允许负值；曲线求值后将有限 RGB
夹到0..1，作为明确的归一化颜色预览规则，不仿原版字节舍入。
alpha 保留有限实数输出，着色器现有 saturate/隐藏规则不改；非有限中间结果拒绝。
未采样动作内没有键时不借用其它动作颜色；坏序列隔离仍保留原 SEQS 序号。

## CPU、宿主与GPU

PoseBatchEvaluator::Colors 与 Alphas 使用同一 geoset 顺序和 PoseReceipt.alphas 片段，
两种输出模式都产出 RGB；无须改变 PoseRequest/PoseReceipt 的定宽尺寸。
每个 geoset 预热额外12字节 RGB，StorageBytes 和服务帧包预算已包含它。
CompiledModel::GeosetColors 仅代表静态属性；KGAC 渲染必须用当前成功求值的 Colors。
无动态分配仅指核心预热后求值，服务自己的帧包仍分配，不能外推零RSS成本。

宿主把原geoset RGB映射到实际mesh，每实例独立；烘焙悬崖保持白色乘数。
KGAC场景要求显式动态RGB能力，即使第一帧恰为全白。缺能力的旧插件拒绝此场景，
不能把静态色/丢失颜色当作播放成功；原静态场景仍兼容旧插件。

新描述符只扩展 SkinUpdate 尾部；原V2函数/结构前缀不变。
War3RenderDynamicColoredSkinUpdateV2.colors 与 alphaCount/firstAlpha 平行，每项12字节RGB。
初始姿势确定是否使用动态RGB，后续更新不能切换有无颜色；数量/NaN/布局/预算拒绝。
GPU t4为12字节StructuredBuffer<float3>，b4的32字节常量大小不变。
动态RGB为绝对GEOA值，不再乘静态meshColors；仍乘实例色、材质贴图与既有光照。
RGB跟矩阵/alpha一起写非活动缓冲，全部成功才交换版本，不重新创建静态几何/贴图。
截图JSON新增每实例colorHashFNV1a；它是诊断指纹，不是原版一致或密码摘要证明。

## 本机验收

新增CPU目标 war3-geoset-channels-tests、war3-animation-dynamic-color-tests。
运行 render-geoset-channels、model-animation-dynamic-color，完整旧核心/服务/颜色测试，
再完整 render-d3d11-headless（其中新增动态RGB真实WARP用例，原静态RGB用例保留）。
低优先级全量构建/CTest；独立RenderCore Win32和Animation Win32分别运行全部测试。
复用四英雄 Stand500ms 计划，不能为出图换时刻或跳geoset。
选实际KGAC模型，在同一个war3-model-animation计划里 time→capture→time→capture→capture：
比较同实例颜色变化、另实例不变、重复帧颜色/图片一致、contentVersion递增、staticUploadCount不变。
对只支持静态RGB的旧插件要求 ANIMATION_DYNAMIC_COLOR_BACKEND_UNSUPPORTED。

Linux已验证CPU数值/来源/预算/深拷贝；FXC、12字节SRV步长、真实WARP和英雄图仍待本机。
没有Claude本轮审查结论，不把未审查说成无问题。
