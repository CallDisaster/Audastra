# V2 矩阵组蒙皮（第 2 步）

只扩展模型槽，不接世界页/单位预览/动画服务。CPU 资产编译由 RenderCore 完成，几何/贴图
第一次提交，之后更新组矩阵、实例变换/RGB 和 mesh 透明度。全部语义为通用预览近似。

## ABI 和调用

旧模型结构、旧函数及其排列不动，在 ApiV2 的 setOverlayLines 后追加四个可选函数。
宿主逐项核 `structSize >= offsetof(字段)+sizeof(函数指针)` 和非空再调用；不得只核版本号 2。
旧 provider 没有能力时明确不支持，不拿旧截图再查询当前版本拼成新回执。
新输入/回执接受未来更大 structSize，但只读取/写入本版本已知前缀。

submitSkinnedModelScene 深拷贝并完整校验后排队；updateSkinMatrices 只替换动态部分。
返回 0 仅表示 CPU 接收。sceneVersion 在 renderer 生命周期中非零且严格递增，contentVersion
在同一 sceneVersion 中非零且严格递增。更新不能改实例数量、model 或组/透明度片段。
要改静态场景/实例布局就提交新的 sceneVersion。旧 submitModelScene 切回静态槽；地形不变。
清空蒙皮槽也需要全零计数和合法 initialPose 版本。

CPU 输入拒绝保持原状态。GPU 静态资源先在临时拥有者内创建，全部成功才交换；GPU 更新先写
非活动动态缓冲组，再切 active。失败不把请求指针当成实际安装版本。资源失败后尚未安装的
新静态场景需用更大的 sceneVersion 重试；同一已安装场景的后续有效内容可重新尝试更新。
设备丢失仍遵从现有后端错误路径，没有新增设备重建系统。

## 矩阵和索引

组矩阵是 48 B：四列 float3，模型局部、列向量，已组内平均，未乘实例世界矩阵：
`x'=m[0]*x+m[3]*y+m[6]*z+m[9]`，其余行类推。实例世界 transform 保持旧的行主序 3×4。
不要把这两种布局混为一谈；更不能把原生游戏某块48字节内存直接当已转换的矩阵。

vertex.group 是 mesh 内局部组号；mesh.firstGroup 相对 model 组池；instance.firstGroup 指向
本次更新全池。着色器下标为三者相加。每实例透明度池按 model 内 mesh 顺序，一项一个 mesh。
顶点步长36、实例步长72、组矩阵步长48；结构缓冲有明确 StructureByteStride，SRV格式UNKNOWN。
同模型实例共用几何/贴图，各自矩阵/颜色/透明度。按模型分组绘制，不强行合成跨模型 draw。

法线先用组3×3变换并归一化，再沿用旧实例法线公式；退化方向采用现有预览约定。
非均匀缩放未做逆转置，CPU/GPU运算不承诺逐位一致，原版未验证。alpha 输入只要求有限，
在 shader saturate；零值不输出像素，0..1 内的混合仍受旧 layer.filterMode 决定，不承诺
任意原版透明排序/GEOA 材质语义。静态路径 opacity=1，仍使用原来的材质/光照规则。

## 帧回执与计数

getSkinStatus 函数返回 0 表示查询成功，还需检查 active 和 result（0已Draw/2待Draw/负值失败）。
它不是阻塞 GPU 完成栅栏。captureSkinnedFrameRgba 则复用实际渲染读回：像素、完整旧帧回执、
实际 skin 版本和计数在同一临界区保存/返回。返回前核当前 surface/scene/view 和 skin内容，
若被新请求替代返回2，不写输出；上传错误负值，不用旧像素冒充新内容。等待上限沿用10秒。

staticUploadCount 是成功安装整组静态模型资源的次数，不是每个贴图/每次API调用数。
matrixUploadCount 是成功安装动态内容的次数，初次非空蒙皮提交也计1；多个排队更新可以
合并成一帧，所以不能要求它永远等于宿主递增次数。按实际完成截图逐次更新时可以断言计数。
旧静态提交同样计入静态安装；空清理不算上传。换图/实例管理仍由未来宿主服务负责。

动态输入≤64MiB；组/透明度各≤1,048,576；静态完整输入≤320MiB并沿用纹理预算。
更新不是零拷贝：CPU拥有副本，GPU有两组动态缓冲，D3D11还可能有驱动暂存；预算不等于
整个进程或GPU总驻留。矩阵/变换绝对值≤1e7且有限，RGB限0..1；更大输入明确拒绝。

## 当前验证边界

Linux 仅编译/运行了共享输入校验和 ABI 前缀断言。新 WARP用例复用 RenderD3D11Tests，
验证双实例、连续更新不重传静态资产、帧版本与非法输入保留旧状态。FXC/MSVC/WARP仍待本机。
未注入 GPU OOM、设备移除或驱动故障，不能把 CPU 拒绝测试说成这些故障已验证。
