# Round 11补充：论文机制裁切与确定性查询剪枝

在78c0d67基础上追加同一Round11：内存半径排序、保守三角界、中心扩展、缺页前重检与预算内工作区复用。标量首版变慢后补齐整侧停止/四路批核/驻留句柄，再按小扫描与探测比例设成本门槛；存储/WAL/Undo/SIMD/协议文件不变。

本轮在已提交78c0d67上追加，不冒充新的第12轮；仅推送备用仓库。前10轮原31单元对象和62引用上下文保留，原Round11完整提示已归档Git78c0d67，本次完整提示作为同轮followup。

## 论文核查与实现边界

- [Starling](https://arxiv.org/html/2401.02116v3)：disk graph block shuffling/navigation and block search；Use only locality/candidate-first principle; do not rewrite stable SlotId payload. 5.12MB is hypothetical vector rewrite payload, not measured added physical bytes.
- [RaBitQ-SSD](https://arxiv.org/html/2610.02652v1)：probabilistic split-code lower bound and async SSD pipeline；No code files or probability-based pruning; deterministic reverse-triangle bound instead. No claim that this standard inequality originates in paper.
- [Gorgeous](https://arxiv.org/html/2508.15290v1)：graph-prioritized cache, decoupled layout, approximate search/refinement；Our IVF engine has no graph; do not transplant graph replication or its large-scale results.
- [PipeANN](https://www.usenix.org/conference/osdi25/presentation/guo)：graph best-first traversal aligned to SSD, relaxed compute/I/O ordering；Inspired pre-I/O recheck; this implementation is synchronous, no pipeline or io_uring claims.
- [ADSampling](https://arxiv.org/abs/2303.09855)：randomized distance comparison with high-probability success；Use deterministic nonnegative prefix bounds, not random extrapolation. Quantized auxiliary bounds can in principle retain Exact with certified rerank; rejected here for cost, not intrinsically forbidden.

这些论文没有证明本负载必然加速；三角半径排序来自通用反三角不等式，不把它虚构为上述论文的直接算法。物理重排可以额外映射维持SlotId，量化摘要也可配严格误差界保留Exact；这里按小负载的写入与内存成本拒绝，非数学上不可行。ADSampling随机外推与本实现确定性部分和界有本质区别。+5.12MB仅为重写原始payload的分析量，不是执行重排后的物理I/O实测。

## 安全不变量与未达目标

工作文件6815744B，实际帧池7651328B，85%门槛6503628B；交接0次。门槛未成立，未降低它；未实现9–10MiB或45%内存目标。边界交接也不能删除已发生的memory.peak。
与78c0d67相比，storage.cpp/.hpp、sql.cpp、simd.cpp/.hpp、local_adapter.cpp、Dockerfile和持久化布局均零修改。半径、排列和工作区只在内存中，有预算保留；DML/语句回滚/事务变更仍立即废弃TableReadCache。
float半径有量化误差：下界从|qd-r|减去(32eps_float+64dim eps_double)(|qd|+|r|)+1e-30并向零舍入；阈值向上留裕度。浮点半径溢出尾部不参与整侧停止。潜在赢家仍float64精算，保留sqrt舍入与主键决胜。
保留原SlotId顺序数组，另建radial SlotId排列及float半径（10k共约80KiB，非40KiB总开销）；大工作区另计预算。首次构建顺序读向量并登记版本句柄；低内存下旧句柄仍失效。缺页候选按槽位排序，最新阈值检查位于handle/pin之前，驻留裸指针不跨换页操作。
成本门槛：预计候选至少256；子集或IVF至少1024，驻留IVF还须探测至少四分之一桶。小过滤/默认5桶通常保留旧批核，均匀宽探测保留既有回退。门槛是经验成本选择，不是召回证书。

## 生命周期三轮中位数

Fresh R9 median write/exact; epsT=.001s,epsB=epsM=1MiB;Eproject=.6fRT+.2fRB+.2fRM;f(R)=R/(1+R);E=(write+2query)/3;median3 E. Not online raw score.

三版本按轮交错，第二轮反序；独立只读/no-network/no-capabilities容器、新卷、2CPU、16m/2g且禁swap。B/M计入装载、建索引、预热、关闭写回；写入重启全量核验在独立事后cgroup。宿主负载与Desktop共享缓存仍可能带来方差。B合计目标cgroup可见块设备的io.stat；Docker Desktop没有复刻正式专用/data数据盘隔离，可能含镜像读取，不冒充线上同环境统计。

|分布|内存|项目|版本|T s|B MiB|M MiB|
|---|---|---|---|---:|---:|---:|
|clustered|16m|write|r9|0.491347|7.199|15.027|
|clustered|16m|write|r10safe|0.403338|7.176|15.199|
|clustered|16m|write|r11p|0.404420|7.156|14.820|
|clustered|16m|exact|r9|0.593992|8.602|16.000|
|clustered|16m|exact|r10safe|0.576667|8.605|16.000|
|clustered|16m|exact|r11p|0.468407|7.602|15.387|
|clustered|16m|ann|r9|0.451842|8.605|16.000|
|clustered|16m|ann|r10safe|0.444798|8.609|16.000|
|clustered|16m|ann|r11p|0.451547|7.574|15.145|
|clustered|2g|write|r9|0.490828|7.094|14.770|
|clustered|2g|write|r10safe|0.416746|7.094|14.828|
|clustered|2g|write|r11p|0.415094|7.094|14.789|
|clustered|2g|exact|r9|0.578954|8.355|17.328|
|clustered|2g|exact|r10safe|0.572115|8.355|17.395|
|clustered|2g|exact|r11p|0.475736|7.504|15.145|
|clustered|2g|ann|r9|0.441016|8.355|17.426|
|clustered|2g|ann|r10safe|0.441770|8.355|17.309|
|clustered|2g|ann|r11p|0.445155|7.504|15.172|
|uniform|16m|write|r9|0.507842|7.168|14.918|
|uniform|16m|write|r10safe|0.411644|7.184|15.398|
|uniform|16m|write|r11p|0.404085|7.180|14.922|
|uniform|16m|exact|r9|0.600765|8.605|16.000|
|uniform|16m|exact|r10safe|0.604304|8.594|16.000|
|uniform|16m|exact|r11p|0.564606|7.551|15.480|
|uniform|16m|ann|r9|0.602699|8.605|16.000|
|uniform|16m|ann|r10safe|0.611425|8.605|16.000|
|uniform|16m|ann|r11p|0.585247|7.543|15.320|
|uniform|2g|write|r9|0.492452|7.094|14.902|
|uniform|2g|write|r10safe|0.421918|7.094|14.863|
|uniform|2g|write|r11p|0.417133|7.094|14.879|
|uniform|2g|exact|r9|0.598327|8.355|17.328|
|uniform|2g|exact|r10safe|0.580706|8.355|17.574|
|uniform|2g|exact|r11p|0.550087|7.473|15.297|
|uniform|2g|ann|r9|0.591168|8.355|17.352|
|uniform|2g|ann|r10safe|0.579321|8.355|17.227|
|uniform|2g|ann|r11p|0.558167|7.473|15.523|

|分布|内存|查询模式|R9本地E|Safe本地E|本轮E|相对Safe|
|---|---|---|---:|---:|---:|---:|
|clustered|16m|exact|0.500459|0.512302|0.538154|+5.05%|
|clustered|16m|ann|0.526953|0.538135|0.542664|+0.84%|
|clustered|2g|exact|0.499836|0.509319|0.534759|+4.99%|
|clustered|2g|ann|0.525970|0.535117|0.541129|+1.12%|
|uniform|16m|exact|0.498829|0.509122|0.522926|+2.71%|
|uniform|16m|ann|0.499792|0.508249|0.518439|+2.00%|
|uniform|2g|exact|0.498908|0.510888|0.522216|+2.22%|
|uniform|2g|ann|0.501475|0.510543|0.525111|+2.85%|

## CPU诊断与反例反馈

基线为冻结78c0d67，新旧共用R9格式夹具，预热一遍、重复5遍平均。不是官方端到端T。标量首版和成本门槛前完整数据在results.json，失败的旧二进制驱动预检未混入性能证据。

|分布|组|模式|78 ms|本轮 ms|Level0拒绝/测试|32拒绝/试次|64拒绝/试次|
|---|---|---|---:|---:|---|---|---|
|clustered|all|exact|87.932|35.983|23436585/25000000 (93.75%)|0/1533405 (0.00%)|1825/1533405 (0.12%)|
|clustered|all|ann|23.371|23.664|未启用（0试次）|553680/1007915 (54.93%)|500/1007915 (0.05%)|
|clustered|unfiltered|exact|71.713|22.810|23436585/25000000 (93.75%)|0/1533405 (0.00%)|1825/1533405 (0.12%)|
|clustered|unfiltered|ann|15.470|15.558|未启用（0试次）|553680/1007915 (54.93%)|500/1007915 (0.05%)|
|clustered|filtered|exact|13.736|13.912|未启用（0试次）|未启用（0试次）|未启用（0试次）|
|clustered|filtered|ann|8.500|8.320|未启用（0试次）|未启用（0试次）|未启用（0试次）|
|uniform|all|exact|81.389|82.908|未启用（0试次）|未启用（0试次）|未启用（0试次）|
|uniform|all|ann|82.402|80.258|未启用（0试次）|未启用（0试次）|未启用（0试次）|
|uniform|unfiltered|exact|69.424|68.437|未启用（0试次）|未启用（0试次）|未启用（0试次）|
|uniform|unfiltered|ann|69.084|69.078|未启用（0试次）|未启用（0试次）|未启用（0试次）|
|uniform|filtered|exact|13.609|12.598|未启用（0试次）|未启用（0试次）|未启用（0试次）|
|uniform|filtered|ann|13.856|13.298|未启用（0试次）|未启用（0试次）|未启用（0试次）|

计数均跨5遍；Level0 tested是当前机制实际处理的候选，32/64分母是实际进入有限阈值SIMD核的候选；不能相加当成全表剪枝率。均匀回退0试次不是算法测得0%有效率。低内存mixed50及NPROBE16诊断不混入默认1000查询排名。

## 原生端到端单轮（建索引前Exact）

|分布|版本|写入请求 s|Exact1000 s|ANN1000 s|
|---|---|---:|---:|---:|
|clustered|before|0.304079|0.216257|0.144203|
|clustered|after|0.298525|0.214657|0.137961|
|uniform|before|0.319481|0.256822|0.221035|
|uniform|after|0.328389|0.241074|0.220923|

## 回归验证

本机CTest9/9、Dockerfile.test CTest9/9，官方二进制及只读Docker各20/20（95条SQL），legacy默认构建成功；4000完整响应与冻结R9一致。既有24次SIGKILL紧凑恢复/四阶段Checkpoint/二次恢复中断随index_regression通过。
新增4MiB/5000×128D缺页夹具：{'rows': 5000, 'dim': 128, 'budget': 4194304, 'tested': 5000, 'rejected': 4959, 'late_rejected': 4959, 'pages_skipped': 155}。这些是BufferPool页调用避免计数，不伪称实际磁盘rbytes。原4×4MiB多文件全局交接反例/小表正例随storage_regression通过。
新增数值/DML夹具覆盖32/64/127/128/129维、2/10MiB、极大/次正规/微小值、负主键、尾页删除、混合有限与溢出半径、语句部分失败及显式ROLLBACK，完整id/double距离与通用执行路径一致。

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    1.11 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    1.16 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.02 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    3.57 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.00 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    2.08 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.82 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   11.80 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  24.68 sec
```

## 复现

```sh
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
DOCKER_BUILDKIT=0 docker build -t minivectordb:r11p-legacy .
docker build -t minivectordb:r11p .
docker build --target build -t minivectordb-build:r11p .
docker build -f Dockerfile.test --build-arg BUILD_IMAGE=minivectordb-build:r11p --target test .
python3 protocol_check/run.py --command '["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]' --output work/r11p-protocol-native
python3 protocol_check/run.py --image minivectordb:r11p --output work/r11p-protocol-docker
python3 tests/benchmark_local.py --phase after --automatic-ann --default-budget --binary work/build/mvdb_local --output work/r11p-native-after.json
python3 tests/benchmark_cgroup.py --image minivectordb:r11p --phase r11p --rounds 3 --min-ann-recall .985 --output work/r11p-cgroup-r11p.json
```

证据校验：
```json
{
  "bytes": 116590,
  "sha256": "0846c0a56b7d7d8390d0e2423e86cb3750e6f93c0c92a7b378f7d5a61f7e4386",
  "units": 33,
  "unique_valid_pointers": 66,
  "old31_units_and62_values_context_preserved": true,
  "duplicate_keys": 0,
  "nonfinite": 0
}
```
