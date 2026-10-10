# Step4 覆盖修复后的扫描口径

`war3-mdx-scan` 仍用同一份字节、同编译器编译的参考/核心逐位对照。
未知变换、相机限制、边界补键、预算、截断等拒绝不放宽；不按 geoset 降级。

## 新列

TSV 原有 22 列的顺序保持，尾部追加 `particle_only` 和 `detail`，现在共 24 列。
控制器/worker 使用同一新格式，旧 22 列报告不作为新 worker 回执读取。
`detail` 最多 8192 字节，整条附加，不截断 UTF-8；超限写 `detail_truncated=1`。
制表、换行、回车和反斜杠按原 Cell 规则转义。它是可读定位信息，不是完整节点转储。

详情包含节点类型计数、继承或未知 flags、节点 ID、非骨骼父节点的 ID/类型、
坏动作的原序号/名称/起止/flags、零法线 geoset/顶点数、拓扑问题与编译拒绝说明。
坏动作不从采样清单删除：仍比较它的拒绝码，好动作继续求值。
`matched_rejections` 与数值成功样本必须区分，不把两边拒绝叫作画面成功。

## 纯粒子

`ClassifyMdxContent` 只描述结构，编译校验仍独立执行。只有无 GEOS 且含 PREM/PRE2
并编译成功的模型计入 `particle_only=1`；汇总 `particle_only` 是 `compiled` 的子集。
空模型、只有光源/挂点、包含 GEOS 的混合模型不计纯粒子。
纯粒子的最终状态是 `particle_only_unimplemented`，不伪造网格或截图。
有 SEQS 时仍比较公共节点姿势；无 SEQS 时明确 `no_sequence=1; pose_samples=0`，
这是分类完成而不是动画验证；预算中断仍退出 2，有差分仍退出 3。
粒子负载、条带、发射和绘制均未实现，公共节点图成功不等于特效渲染成功。

## 零法线

只对原始精确零向量降为 `MDX_SKIN_ZERO_NORMAL_PREVIEW` 警告，含 geoset 和顶点数。
CPU 参考/核心保留规范零；很小的非零法线不预先改值，非有限或非零变换退化仍拒绝。
GPU 使用安全的预览归一化，仍需本机 FXC/WARP 和受影响模型外观验收。

## 手工自检与真实复扫

工具不进 CTest。显式构建 `war3-mdx-scan-fixtures`，用一个全新的目录生成原创样例。
`models.txt` 保留 5 模型基线；旧零法线反例已转为支持项，真正的局部回退阳性改为
无关 HELP 的未知位，仍要求能查出，而不是把回退检测器删掉。
另生成 `coverage-models.txt`：9 模型含零法线、ATCH 父节点、两种继承位、坏动作、
纯粒子（有/无 SEQS）和一个故意的回退阳性。期望 9/9 解析、8 编译、纯粒子 2、
拒绝 1、旧局部阳性 1、81 样本、差分 0；这份合成清单故意不满足真实数据的零回退门。

```text
war3-mdx-scan-fixtures <新目录>
war3-mdx-scan --list <新目录>/models.txt --tsv <新报告>/baseline.tsv
war3-mdx-scan --list <新目录>/coverage-models.txt --tsv <新报告>/coverage.tsv
war3-mdx-scan --game-root <游戏根> --list <相同标准物编模型清单> --tsv <新报告>/full.tsv --total-seconds 3600
```

真实数据须把 summary、TSV、fallback.txt、scope.txt 一起回传，检查差分=0、旧局部阳性=0，
并逐项列出剩余拒绝和原因；缺失路径、未枚举模型、预算缺口不消失。原版一致性未验证。
