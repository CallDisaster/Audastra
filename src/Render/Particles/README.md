# PRE2 / RIBB 通用预览（Step5 WIP）

这是CPU检查点，还没有接GPU、世界页或截图，不是第5步最终交付。
只读新增目录；没有修改Step4的Assets或RenderCore。
格式字段参考 GhostWolf 的 MDX Specifications：
https://www.hiveworkshop.com/threads/mdx-specifications.240487/
只参考字段布局，没有复制第三方或WarVK实现。继承语义继续用已核实的现有核心，
不采用该格式帖里与IDA不符的低三位解释。本次没有新的IDA公式核实。

解析器读取PRE2/RIBB负载与属性轨迹，Node TRS仍由既有模型/核心解释；
未知轨迹、图集范围、非有限数、坏长度和全局序列明确拒绝。PREM只计数警告。
全部模拟/几何为PreviewApproximate，不等于原版。预览选择如下：
- 固定10ms积分，尾余毫秒只用于解析外推；分多次33ms与一次跳到同一终点一致。
- 发射累计保留小数，重力沿模拟空间-Z；速度在纬度锥内均匀取角，variation对称扰动。
- squirt按发射率轨迹按键跨越触发（包含首次0点）；无轨迹时只在初始步发射一次。
  触发键的原始数量须为有限的0..1000000，超限明确拒绝；允许范围内向下取整，
  配额不足时按未截小的发射总数累计suppressed，不先夹小键值隐瞒抑制量。
- Node轴心为发射中心；world-space在发射时应用实例变换，model-space在绘制时应用。
- 头为相机平面/XY平面方片，尾用速度乘tailLength；颜色/透明度/缩放分两段线性过渡。
- RIBB沿节点局部Z取上下边，记录历史位置/颜色/纹理槽，重力下落后连接相邻边。
- 属性轨迹取键（2026-10-09 起）：非全局轨迹按 1.27a 原版逐动作区间取键（IDA：各轨迹编译都调用
  sub_6F78A090，标量取值 sub_6F789EB0 走 sub_6F789830 顺序查找），与节点轨迹共用核心
  `War3RenderCore/GameKeyRanges.h`。SEQS 按文件顺序共享只进游标切区间，区间内顺序查找，首键前和
  末键后接回另一端；空区间取默认值。预览模式接受非全局逆序键并报告 `PARTICLE_KEYS_GAME_ORDER_PREVIEW`
  （记录第一条逆序边与轨迹位置），严格模式与全局轨迹仍要求递增；没有 SEQS 的逆序发射器隔离。
  squirt 只数本动作区间内的键。
- 单次步最多重放6000次（60秒）；超过明确失败，不截断时间冒充目标帧。
- 场景总65536粒子/32768条带边；配额必须由宿主按全场景发射器分配，
  不是每实例独占该上限。每发射器最多4096，超出抑制新发射并累计suppressed。

## 显式双时钟与事务

`Timeline.elapsedReferenceMs/globalReferenceMs` 是同一显式时刻的动作/全局时间；
`GlobalTimeAt(t, elapsed)` 是宿主节点AnchorSampler与属性轨迹共用的受检映射。
两时钟和映射结果上限均为2^53，不读取墙钟，不把动作时间隐式写入原有模型全局时钟。
例如参考对500/1000，回放到0时global为500，到550时为1050。

当前从0回放不定义负的全局历史。参考对500/0本身可读取500时global=0，但Advance
会以`PARTICLE_GLOBAL_REPLAY_UNSUPPORTED`拒绝该组合的从0回放，即使粒子属性没有
全局轨迹也采用同一合同，因为节点仍可能有全局轨迹。不得只给属性取模而伪造节点历史。
既有模型时钟仍允许该组合；未来共用服务应如实报告粒子不支持，不修改模型请求。

首次Advance绑定映射；只有等价参考对可以继续。相位变化、回退或切动作须重新Create
并显式回放；非等价映射返回`PARTICLE_TIMELINE_CHANGED`。宿主始终推进候选State，
失败/取消不发布，因为Advance不保证对传入工作副本回滚。

## 配对输入边界

`CopyParticleInput`接受基础模型姿势或B的完整动态RGB姿势扩展；颜色格式、数量、
指针和有限0..1数值须满足既有CopySkinUpdate合同，未知结构尺寸拒绝，不截断尾部。
它只深拷贝粒子数组并核对配对头；完整模型姿势仍须由既有CopySkinUpdate校验/复制，
随后同锁发布。CPU版本号校验通过不等于GPU同帧发布或截图已经完成。

CPU自检：cmake -S src/Render/Particles -B build/particles -G Ninja；
cmake --build build/particles；ctest --test-dir build/particles -V。
原WIP只报告GCC合成状态/几何可重复、发射寿命和预算、格式边界；本机接手后以
MSVC 19.50 Release 实际构建并运行 x64 与 `-A Win32`，各3/3通过且0编译诊断。
Win32 EXE 已核对 PE 0x014c，证据见 `artifacts/astra-rounds/takeover-20260929/`。
这些仅为CPU回归；粒子的FXC、WARP、真实模型显示和Claude审查均未完成。
后续提交接同一动画时钟、GPU组合事务和同帧回执。
