# C：扫描的数值覆盖与拒绝原因分账

TSV 为29列，保留原字段顺序，在末尾追加：
- sample_rejection_codes：该模型各核心样本首个拒绝码和次数，格式 CODE=count;。
- primary_rejection_code：次数最多的拒绝码；并列按代码字典序取首项；无拒绝为空。

汇总另列 sample_rejection_codes={CODE:样本数}、all_rejected_model_codes={CODE:模型数}。
前者只统计核心的失败；参考和核心拒绝不同仍记 diagnostic_mismatch，不会被此账掩盖。
后者只纳入编译成功、全部计划样本确实运行、每个样本均新旧同码拒绝、无数值样本且无差分的模型。
超时、复杂度/样本上限、部分进度和没有动作的模型不计作“全拒”。
主因只描述这份采样计划，不是“已证明该模型在所有时间都不可用”。

numeric_samples 仅代表两边成功并实际进入数值对照；即使数值不一致也在此分母内，
差分继续由 mismatch_samples/color_mismatch 等报告。matched_rejections 不是数值成功。
models_with_numeric、all_samples_rejected_models、particle_only 分开列；纯粒子类别本身
也不代表已经渲染粒子。未知 GEOA 0x1 仍拒绝，不由扫描器补成白色。

子进程回执校验拒绝账：总数不得超过非数值样本数，不得少于 matched_rejections；
代码限定大写ASCII/数字/下划线、长度128、至多128种，拒绝重复码、零计数、溢出和主因不符。
旧27列回执不能和本轮worker混用；始终使用同一构建的父子进程。

合成验证（war3-mdx-scan-fixtures 生成 reason-models.txt）：
reason-numeric：KGAO/KGAC正常插值，12个数值样本。
reason-flags：未知GEOA标志，12个MDX_GEOA_UNINTERPRETED。
reason-global：不存在的颜色全局序列，12个MDX_GEOA_GLOBAL_SEQUENCE_UNSUPPORTED。
总计3模型/36样本/12数值/0差分/2个采样全拒；每类主因1个模型，旧局部阳性0。
原5个样例和9个覆盖样例没有删除；故意制造的局部回退样例仍必须检出。

本机先重建 war3-mdx-scan、war3-mdx-scan-fixtures 和 war3-render-coverage-tests；
跑 render-coverage-sequences（内含回执往返/主因/预算边界断言），再运行：
war3-mdx-scan-fixtures <新样例目录>
war3-mdx-scan --list <新样例目录>/reason-models.txt --tsv <新输出.tsv>
再用之前同一2452模型清单重扫，带回summary、TSV、fallback.txt和scope.txt。
要求差分0；采样全拒仍有多少、按哪些代码分别列出，不能拿编译成功总数替代数值覆盖。
