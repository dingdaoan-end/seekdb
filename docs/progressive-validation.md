# 128D 渐进 L2 验证报告

基线为 `b84bf5b`。本轮只在既有 TopK 候选路径中加入保守的部分平方和拒绝；原始向量、SlotId、候选顺序、索引和最终 double 决胜规则保持原有实现。默认选择 prefix32，但只对符合成本条件的无过滤驻留安全 128D 查询启用。完整 SIMD、prefix32 和 prefix64 均保留可复核对照。

## 论文前提与本轮边界

[PDX / PDX-BOND](https://arxiv.org/abs/2503.04422) 已明确支持直接处理未经变换的向量。免旋转不是本轮独创；PDX 还包含按维度跨向量存储的布局，本轮没有迁移该布局。

[ADSampling](https://arxiv.org/abs/2303.09855) 的距离比较是带高概率保证的随机方法；[DADE](https://www.vldb.org/pvldb/vol18/p812-zheng.pdf) 使用数据分布相关的低维距离估计和假设检验。本轮使用原坐标部分平方和作为确定性下界，配合浮点误差余量；没有实现随机距离估计，也没有用概率置信度作为丢弃候选的依据。不同算法和变换实现的预处理要求不能混同。

## 内核、阈值和默认路由

内核将 128D 拆为 32/96 或 64/64，四个 AVX2 累加器跨检查点继续使用。检查点归约不会反馈到累加器，幸存者与原完整快速内核的累加顺序和结果逐位一致。严格 `partial > cutoff` 才拒绝；等值继续参与 double 计算及距离/主键排序。

`cutoff = nextafterf(float(1.0001 * heap_worst_squared + 1e-5), +infinity)`。乘法余量覆盖安全输入的舍入高估，加法余量覆盖极小数和 DAZ/FTZ 情形，向 float 正无穷方向上取整。非有限/负阈值或上溢阈值禁用提前拒绝；极值输入走原有 widened-double 路径。该实现不是任意维数、任意数值环境下的通用下界证明。

默认 prefix32 需要同时满足：

- 无 SQL 谓词，候选访问所需向量页已驻留，向量维数恰为 128，查询安全。
- 查询向量列与既有 IVF 索引列一致，已有列表，活动行数至少 4096；校准未过期、未跳过，已有有效推荐探针数且不大于列表数的一半。
- 既有中心到查询的最远平方距离严格大于最近平方距离的四倍；两者均有限。ANN 复用已计算的中心距离，Exact 仅在前述校准资格成立后才检查中心。
- 每个实际候选还需通过既有极值标记检查；满堆后才能提前拒绝。

过滤查询保留旧路径。校准资格或中心分离条件不满足时保留旧路径；本次 uniform 默认没有尝试部分和拒绝。`MVDB_PROGRESSIVE=off|32|64` 是诊断强制开关；`auto`/未设置使用上述默认决策。

## 正确性、接口和存储边界

内核回归完成 400450 组比较、44633 次经完整 double 真值复核的拒绝；对每个幸存者逐位比较完整快速内核。覆盖相等/邻近阈值、能量集中在前缀、非对齐地址、符号零、极小数、极值，以及 DAZ/FTZ 开关。独立内核 ASan/UBSan 二进制复跑通过，日志为 `work/progressive-sanitizer.log`（400450/44633）。这个 sanitizer 二进制编译于新增 Engine SQL 堆测试之前，只证明内核 sanitizer 检查，不能声称新增 SQL/索引/回滚 fixture 已在 sanitizer 下执行。FMA 和非 FMA 分支均编译，本机实际走 FMA，不能据此声称非 FMA CPU 分支已执行。

Engine 新增测试比较 off/32/64/auto 的完整 SQL 返回值，包含并列主键、极值、极小数、建索引、删除和回滚。最初最大 float 的最短十进制文本 `3.4028235e38` 数学值略大于 FLT_MAX，被既有严格解析器拒绝；测试改用 FLT_MAX 的精确十进制 `340282346638528859811704183484516925440`，未放宽生产转换规则。

`work/progressive-equality.json` 记录 clustered/uniform × Exact/ANN 共 4000 条冻结查询的完整响应与基线相等。`work/progressive-invariants.json` 的 16 项源码边界检查全部 unchanged：storage、SQL 解析、适配器、缓存填充、索引校准/写入、插入路径及持久结构等。没有增加旋转向量副本或 SlotId 负载；Engine 新增三个 uint64 诊断计数器，标量字段合计 24 字节，不能宣称零内存增量。

## 实际 Engine CPU：默认与禁用

证据为 `work/bounded-engine-cpu-final.json`。10 MiB Engine 预算，冻结 `/tmp/mvdb_benchmark/after` 数据库副本；三轮交错，每轮独立新副本，先一遍预热，再五遍测量。每行 500 条查询，报告进程 CPU 毫秒/遍的三轮中位数，包含查询解析、执行及结果回调，不含外部传输。它不是官方端到端 T。`eligible` 表示满堆后的实际部分和尝试数，剪枝率分母为尝试数。

| 分布 | 过滤 | 模式 | off CPU ms | 默认 CPU ms | 默认变化 | 尝试/遍 | 提前拒绝率 |
|---|---|---|---:|---:|---:|---:|---:|
| clustered | 无 | exact | 65.4926 | 48.3916 | -26.111% | 4995000 | 92.2680% |
| clustered | 无 | ann | 14.2604 | 13.4278 | -5.839% | 458855 | 34.2771% |
| clustered | 有 | exact | 21.9276 | 21.7508 | -0.806% | 0 | 0.0000% |
| clustered | 有 | ann | 7.4252 | 7.6484 | +3.006% | 0 | 0.0000% |
| uniform | 无 | exact | 74.7260 | 75.0972 | +0.497% | 0 | 0.0000% |
| uniform | 无 | ann | 72.4706 | 70.3980 | -2.860% | 0 | 0.0000% |
| uniform | 有 | exact | 21.9196 | 21.8798 | -0.182% | 0 | 0.0000% |
| uniform | 有 | ann | 22.4616 | 21.6374 | -3.669% | 0 | 0.0000% |

默认 clustered 无过滤 Exact CPU 下降 26.111%，ANN 下降 5.839%；这不等同于整个混合工作负载或容器生命周期的收益。未启用组的正负变化全部保留：filtered clustered ANN +3.006%，uniform 从 -3.669% 到 +0.497%。零尝试组不应把这些幅度归因于剪枝收益，也不能据此保证零退化。

## 实际 Engine CPU：强制 prefix32 与 prefix64

下面由同一最终证据文件的 `forced_comparison_runs` 原始样本重新取中位数。强制开关故意绕过默认成本选择，用于暴露 uniform 和过滤路径的负收益；这些样本来自较早的强制对照构建，内核相同，不能冒充最终默认构建的逐次配对性能比。

| 分布 | 过滤 | 模式 | 强制32 CPU ms | 强制64 CPU ms | 32尝试/遍 | 32拒绝率 | 64拒绝率 |
|---|---|---|---:|---:|---:|---:|---:|
| clustered | 无 | exact | 48.2254 | 54.1472 | 4995000 | 92.2680% | 92.2750% |
| clustered | 无 | ann | 13.2736 | 13.4338 | 458855 | 34.2771% | 34.3470% |
| clustered | 有 | exact | 20.5472 | 21.5994 | 307109 | 70.6713% | 70.6905% |
| clustered | 有 | ann | 7.8410 | 7.6278 | 23378 | 40.1446% | 40.1446% |
| uniform | 无 | exact | 68.7290 | 67.2758 | 4995000 | 0.0000% | 0.1276% |
| uniform | 无 | ann | 66.7834 | 67.7194 | 4995000 | 0.0000% | 0.1276% |
| uniform | 有 | exact | 23.0404 | 22.3152 | 307403 | 0.0000% | 0.0033% |
| uniform | 有 | ann | 22.9220 | 22.5532 | 307403 | 0.0000% | 0.0033% |

初版中心分类器在 uniform 上增加约 1.8–2.8% CPU，因为先做了无收益的中心检查。随后将既有校准资格检查放到中心扫描前，避免 uniform 的额外中心工作。最终默认表仍保留约几个百分点的正负波动（含过滤 ANN -3.669%）；未开展统计显著性试验，不将这些差异包装为剪枝收益或零退化保证。32/64 增加归约和分支，uniform 前缀几乎不拒绝，因此不默认启用。

## 独立种子和八组部分和诊断

独立工具 `tools/progressive_benchmark.cpp` 使用既有 `benchmark_local.make_data` 生成器：10000×128、1000 个 Top10 查询、50% 在 16 类中做单类过滤，种子 20261109 与独立 20261116。Exact 保持槽位顺序；另建 64 列表/探针5的确定性 Lloyd **候选模型**，不是实际 Engine 的索引、存储或召回测量。训练和候选构造排除在 CPU 计时之外。

每组 full/32/64 都与同一候选集合的完整 double oracle 比较排序 ID 和 double 平方距离；所有 72 次计时结果比较、24 个计数分区及倒序主键并列检查通过。计数满足 `early_pruned + full_rejected + double_calls == candidates`。

| 分布 / 种子 | 候选路径 | full CPU ms | prefix32 CPU ms | prefix64 CPU ms |
|---|---|---:|---:|---:|
| clustered / 20261109 | exact_candidates | 73.130 | 57.290 | 62.103 |
| clustered / 20261109 | modeled_ivf64_nprobe5 | 13.328 | 12.501 | 13.328 |
| clustered / 20261116 | exact_candidates | 84.391 | 70.214 | 72.986 |
| clustered / 20261116 | modeled_ivf64_nprobe5 | 11.196 | 10.776 | 10.963 |
| uniform / 20261109 | exact_candidates | 83.905 | 84.922 | 85.046 |
| uniform / 20261109 | modeled_ivf64_nprobe5 | 12.055 | 12.197 | 12.228 |
| uniform / 20261116 | exact_candidates | 83.039 | 84.224 | 86.464 |
| uniform / 20261116 | modeled_ivf64_nprobe5 | 12.090 | 12.114 | 12.309 |

八组表使用完整路径当时的堆阈值，另做不计时的 double 前缀和，统计累积到 16/32/…/128 维后潜在可拒绝比例。分母为堆已满的 eligible 候选；这是下界分布诊断，不能称作已部署八次检查内核的实测。下面为种子 20261109；独立种子的原始计数与比率保留在 `work/internal_tools/progressive/summary.json`。

| 分布/候选路径 | 16维 | 32维 | 48维 | 64维 | 80维 | 96维 | 112维 | 128维 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| clustered/exact_candidates | 91.017% | 91.017% | 91.017% | 91.025% | 91.234% | 92.997% | 96.552% | 99.008% |
| clustered/modeled_ivf64_nprobe5 | 34.765% | 34.765% | 34.765% | 34.831% | 37.024% | 52.321% | 79.108% | 94.811% |
| uniform/exact_candidates | 0.000% | 0.000% | 0.000% | 0.120% | 8.186% | 51.582% | 90.053% | 98.957% |
| uniform/modeled_ivf64_nprobe5 | 0.000% | 0.000% | 0.000% | 0.007% | 1.589% | 23.436% | 69.264% | 93.462% |

模型中 clustered exact prefix32 拒绝约91%；其 IVF 候选模型降至23–35%。uniform prefix32 为0%，64维前缀也只有约0–0.12%。实际候选筛选越集中，前缀越可能需要接近完整向量才超过阈值，不能将全扫描中的拒绝率直接迁移给 IVF。

## 稠密正交变换的实际成本

同一工具构造 float32 归一化 128×128 Hadamard **稠密矩阵**，显式执行全部稠密点积，未使用快速 Walsh–Hadamard 变换。矩阵构造/正交性检查排除在计时外，输出数组经打印的加权 checksum 及能量保持检查消费，避免结果被优化掉。12 个样本全部通过；矩阵最大正交误差为 3.4229e-8。

10000×128×128 = **163840000 个标量乘加项**，按每项乘和加各一次计为 **327680000 arithmetic FLOPs**，不是163.84M FLOPs，也不是硬件 FMA 指令数量。实测进程 CPU 中位 **45.643 ms**，范围 **44.526–46.163 ms**。

该稠密 float 矩阵恰为 **65536字节**，单独已占满64 KiB，另有输出向量和 Engine 工作区；因此它不能与 Engine 工作区一起装入同一个64 KiB预算。该字节下限只适用于这里显式存储的稠密矩阵，结构化变换未必需要存储整块矩阵。这个实验也不证明所有 ADSampling/DADE 变体都必须付出同样成本，更不能用它否定 PDX-BOND 的免变换路径。

若在 ingest 内执行，该阶段成本才属于对应 T_write；若在建索引阶段执行，按测量项目的阶段边界处理，不能一律加到每次查询 T。额外输出、持久化、索引副本等若实际采用，还会影响 B/M；本轮未引入这些结构，也不编造这类端到端代价。保留简单的原坐标拒绝可避免新格式、额外负载和预处理复杂度，但不是普适优越性结论。

## 可复现入口与证据

内核/Engine：`tests/bounded_regression.cpp`、`tests/bounded_engine_regression.cpp`；实际 CPU：`tools/benchmark_cpu.cpp`；候选与旋转诊断：`tools/progressive_benchmark.cpp`。完整命令和 fixture 格式在 `work/internal_tools/progressive/README.md`。论文链接是原始论文；本地报告中的实验值来自上述保留的原始文件。

## 从仓库源码复现独立诊断

下面的 Linux/WSL 命令只依赖仓库源码、C++20 编译器和装有 NumPy 的 Python；不依赖未提交的辅助脚本。`make_data` 的参数40是另外生成的校准查询数，保留它以保持过滤类别的随机数序列与原基准一致。

```sh
mkdir -p work/progressive-fixtures
g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Isrc \
  tools/progressive_benchmark.cpp src/simd.cpp -o work/progressive-diagnostic
/tmp/mvdb-benchmark-venv/bin/python - <<'PY'
import sys, struct
from pathlib import Path
sys.path.insert(0, 'tests')
from benchmark_local import make_data
for seed in (20261109, 20261116):
    for kind in ('clustered', 'uniform'):
        x, cat, q, qcat, _, _ = make_data(kind, 10000, 1000, seed, 40)
        path = Path(f'work/progressive-fixtures/{kind}-{seed}.bin')
        with path.open('wb') as f:
            f.write(struct.pack('<4I', 10000, 1000, 128, seed))
            for values, dtype in ((x, '<f4'), (cat, '<u4'),
                                  (q, '<f4'), (qcat, '<u4')):
                f.write(values.astype(dtype).tobytes())
PY
for seed in 20261109 20261116; do
  for kind in clustered uniform; do
    work/progressive-diagnostic "work/progressive-fixtures/$kind-$seed.bin" "$kind" \
      > "work/progressive-fixtures/$kind-$seed.jsonl"
  done
done
```

Fixture 是小端二进制：四个 uint32 头字段 N/Q/D/seed，随后 float32[N,128] 数据、uint32[N] 类别、float32[1000,128] 查询、uint32[1000] 查询类别。输出JSONL保留每次CPU样本、拒绝计数、逐组潜在拒绝计数、checksum和旋转误差；方法0表示完整快速内核，32/64表示前缀大小。每组每方法取三次 `timing.cpu_ms` 的中位数即可复算模型表；12条 `dense_rotation.cpu_ms` 的中位数得到旋转表。实际 Engine 的CPU和容器生命周期仍须分别使用 `benchmark_cpu` 与 `tests/benchmark_cgroup.py`，不能用候选模型替代。

<!-- PROGRESSIVE_FINAL_BEGIN -->

## 完整门禁与三轮容器生命周期

实际门禁：native CTest 13/13（21.79s）、容器 CTest 13/13（20.44s）；native/Docker 官方协议各20/20，95条SQL/端；冻结4000条完整响应等值通过。日志分别为 `work/progressive-ctest.log`、`work/progressive-container-ctest.log`、`work/progressive-protocol-{native,docker}/result.json` 和 `work/progressive-equality.json`。

三轮交错采集 base/new 各36个、合计72个生命周期样本，第二轮反转版本顺序。每项目独立新数据卷/目标cgroup、只读根目录、2CPU、禁swap，固定10k×128D、1000条Top10查询、500条过滤；写入项目关闭后在独立未测容器逐行检查10000条数据和float32载荷。所有查询项目过滤/无过滤组召回均达到0.985。

T_write 为100批预编码INSERT请求加关闭/checkpoint往返；查询T为1000次JSON往返，排除装载、建索引、20条固定预热和关闭。B为目标从启动至关闭写回的cgroup读写字节合计，M为完整生命周期memory.peak，均由外部collector读取。Docker Desktop设备包含镜像启动读，非正式独立/data设备；无全局drop_caches，宿主缓存和调度噪声保留。进程CPU不能替代这里的T，VmHWM不能替代这里的M。

以下12行分别对每组项目的三轮T/B/M取中位数；所有正负变化和实际字节计数均保留。

| 分布 | 预算 | 项目 | base T s | new T s | base B MiB | new B MiB | base M MiB | new M MiB |
|---|---|---|---:|---:|---:|---:|---:|---:|
| clustered | 16m | write | 0.327834 | 0.354276 | 7.191 | 7.180 | 15.012 | 14.887 |
| clustered | 16m | exact | 0.539294 | 0.512213 | 8.605 | 8.621 | 16.000 | 16.000 |
| clustered | 16m | ann | 0.431004 | 0.412090 | 8.621 | 8.625 | 16.000 | 16.000 |
| clustered | 2g | write | 0.355589 | 0.326612 | 7.094 | 7.094 | 14.852 | 14.941 |
| clustered | 2g | exact | 0.528670 | 0.509729 | 8.355 | 8.355 | 17.281 | 17.121 |
| clustered | 2g | ann | 0.437608 | 0.414285 | 8.355 | 8.355 | 17.133 | 17.172 |
| uniform | 16m | write | 0.325783 | 0.309599 | 7.160 | 7.160 | 14.801 | 14.750 |
| uniform | 16m | exact | 0.601780 | 0.541049 | 8.605 | 8.602 | 16.000 | 16.000 |
| uniform | 16m | ann | 0.558053 | 0.546686 | 8.609 | 8.605 | 16.000 | 16.000 |
| uniform | 2g | write | 0.324107 | 0.331848 | 7.094 | 7.094 | 14.816 | 14.805 |
| uniform | 2g | exact | 0.555391 | 0.527838 | 8.355 | 8.355 | 17.254 | 17.258 |
| uniform | 2g | ann | 0.565166 | 0.528042 | 8.355 | 8.355 | 17.312 | 17.258 |

按3.3.3计算本地相对效率：每个分布/预算固定使用本次base写入三轮各指标中位作为写入参考，base Exact三轮各指标中位作为Exact及ANN共同查询参考。每一轮先算 `R_T=(T_base+.001)/(T+.001)`，B/M各加1048576字节平滑；`f(R)=R/(1+R)`，`E_project=.6*f(R_T)+.2*f(R_B)+.2*f(R_M)`，`E=(E_write+2*E_query)/3`，再取三轮E的中位，最后保留六位小数。不是把T/B/M中位代入后只算一次E。固定参考向量自身得0.5，但baseline逐轮E中位不保证恰为0.5；这是本地比较，不等于线上正式得分。

| 分布 | 预算 | 模式 | base E中位 | new E中位 | new-base |
|---|---|---|---:|---:|---:|
| clustered | 16m | exact | 0.501743 | 0.503820 | +0.002077 |
| clustered | 16m | ann | 0.516001 | 0.522778 | +0.006777 |
| clustered | 2g | exact | 0.494779 | 0.503096 | +0.008317 |
| clustered | 2g | ann | 0.513895 | 0.527695 | +0.013800 |
| uniform | 16m | exact | 0.498598 | 0.513210 | +0.014613 |
| uniform | 16m | ann | 0.509830 | 0.512360 | +0.002530 |
| uniform | 2g | exact | 0.499355 | 0.504116 | +0.004761 |
| uniform | 2g | ann | 0.498102 | 0.505899 | +0.007797 |

生命周期T增加的组：clustered/16m/write +8.07%；uniform/2g/write +2.39%。这些结果保留测量范围内的代价，不能从部分内核收益推导所有项目零退化。 uniform 默认部分和尝试数为零，不能把其容器查询 T 的下降归因于本轮剪枝；缓存、调度及整个请求生命周期的波动仍在样本中。写入路径源码未变，写入 T 的正负变化也不能直接当作算法写入成本变化。

最终两份提交JSON的实际八项检查为PASS，证据 `work/progressive-validation.json`；逐项标识为：`1_b84_history_and_units_locked_cb_prompts_equal`, `2_only_authorized_historical_word`, `3_all38_ids_and76_pointer_positions`, `4_all38_verbatim_source_substrings`, `5_all_evidence_within240_characters`, `6_all_existing_utf8_sha256_valid`, `7_size112000_to115500`, `8_strict_identical_utf8_unique_finite`。校验同时核对两份UTF-8字节完全一致、严格唯一键/有限数、历史锁定、38单元/76指针、原文子串及既有SHA256；最终字节数为115373。

原始矩阵：`work/progressive-cgroup-{base,new}.json`（各36条，内含逐轮物理I/O及内存计数）；重算后的固定参考、逐轮E、12行T/B/M中位见 `work/progressive-cgroup-summary.json`。

<!-- PROGRESSIVE_FINAL_END -->
