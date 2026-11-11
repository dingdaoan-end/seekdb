# Round9：三代实测与二阶瓶颈核查

R7=fe90005e5f2047f48608dcf556d36bdb46ab87ee；R8=9c8798a406f59af709a58aeea73f32a36e6acb08。本轮全部为本地新测，无线上测试或分数承诺。

## 核查与实现

旧热缓存并无全表候选位图，句柄按页而非逐槽位校验；旧SIMD已四累加器FMA。本轮不把已有代码算作新成果。旧12800分量引号向量只产生9个SQL Token，实际优化对象是复制/分配与逐行页操作。
采用游标借用词法、逐元组INSERT及最多8页合并、最后发布header；保留文件级旧Redo/Shadow保护与逐语句Undo。修复失败追加之后水位外Meta重用；稀有hex/下溢scratch按数字分量截断并计预算，最多64字段且及时拦截维度溢出。
全驻留页面一次校验后直接遍历桶；缺页保留排序与逐页校验。128D使用移位/掩码，预取仅在ANN有效驻留地址启用。首版Exact核心0.130s反增，进一步发现动态整除及GCC热lambda未内联，依据微测修正。

## 原生单次端到端（同一legacy SQL/JSON适配路径，1000查询）

|分布|版本|预算MiB|写入请求s|Exact s|ANN s|插入WAL MiB|进程读/写MiB|VmHWM MiB|ANN召回 无/有过滤|
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
|clustered|r7|20.0|1.299686|0.686674|0.324685|5.3487|0.844/13.191|13.633|0.9908/0.9886|
|uniform|r7|20.0|1.232219|0.678209|0.719267|5.3424|0.434/13.188|13.578|1.0000/1.0000|
|clustered|r8|12.0|0.976298|0.248285|0.175544|0.4644|1.988/8.293|13.457|0.9908/0.9886|
|uniform|r8|12.0|1.038411|0.280434|0.294613|0.4643|0.848/8.293|13.488|1.0000/1.0000|
|clustered|r9|10.0|0.385048|0.251289|0.139523|0.1354|0.922/7.961|13.398|0.9908/0.9886|
|uniform|r9|10.0|0.386163|0.246756|0.244471|0.1354|0.875/7.961|13.293|1.0000/1.0000|

## 官方JSONL独立容器三轮中位数

每个写入/Exact/ANN项目独立卷及cgroup。B/M含启动、装载、建索引、固定预热和关闭写回。采样器在目标cgroup外，固定holder shell计入M；Desktop同设备还包含启动镜像读。冷启动新卷，未清宿主全局缓存。每个写入项目在统计后另容器重启核对全部10000行及128D向量（共36次）。
三代统一使用Linux Docker客户端直连同一daemon socket，减少Windows跨系统管道调度影响。前一批Windows客户端数据作为中间版本诊断保留，不与此表混用。所有被测容器仍为只读根、cap-drop ALL、无swap；驱动访问daemon的宿主权限不进入被测cgroup。

|分布|限额|项目|版本|T s|B MiB|M MiB|
|---|---|---|---|---:|---:|---:|
|clustered|16m|write|r7|1.486428|11.941|16.000|
|clustered|16m|write|r8|1.364875|7.473|15.203|
|clustered|16m|write|r9|0.428717|7.164|14.793|
|clustered|16m|exact|r7|1.997165|13.473|16.000|
|clustered|16m|exact|r8|0.622407|8.910|16.000|
|clustered|16m|exact|r9|0.591763|8.605|16.000|
|clustered|16m|ann|r7|1.191816|13.488|16.000|
|clustered|16m|ann|r8|0.496859|8.926|16.000|
|clustered|16m|ann|r9|0.455833|8.605|16.000|
|clustered|2g|write|r7|1.463158|11.938|20.738|
|clustered|2g|write|r8|1.238482|7.426|15.016|
|clustered|2g|write|r9|0.439917|7.094|14.797|
|clustered|2g|exact|r7|1.086584|13.191|23.367|
|clustered|2g|exact|r8|0.627144|8.688|17.551|
|clustered|2g|exact|r9|0.601387|8.355|17.527|
|clustered|2g|ann|r7|0.664010|13.191|23.238|
|clustered|2g|ann|r8|0.502307|8.688|17.555|
|clustered|2g|ann|r9|0.454618|8.355|17.301|
|uniform|16m|write|r7|1.611294|11.930|16.000|
|uniform|16m|write|r8|1.213318|7.473|15.008|
|uniform|16m|write|r9|0.427496|7.156|14.770|
|uniform|16m|exact|r7|1.992834|13.465|16.000|
|uniform|16m|exact|r8|0.631478|8.914|16.000|
|uniform|16m|exact|r9|0.606685|8.609|16.000|
|uniform|16m|ann|r7|1.978439|13.469|16.000|
|uniform|16m|ann|r8|0.653218|8.926|16.000|
|uniform|16m|ann|r9|0.611281|8.594|16.000|
|uniform|2g|write|r7|1.523516|11.930|20.387|
|uniform|2g|write|r8|1.150896|7.426|15.184|
|uniform|2g|write|r9|0.437056|7.094|15.066|
|uniform|2g|exact|r7|1.103683|13.188|23.094|
|uniform|2g|exact|r8|0.641598|8.688|17.508|
|uniform|2g|exact|r9|0.600008|8.355|17.348|
|uniform|2g|ann|r7|1.138267|13.188|23.074|
|uniform|2g|ann|r8|0.623403|8.688|17.516|
|uniform|2g|ann|r9|0.610467|8.355|17.520|

## 按3.3.3的本地相对效率

R7各项目三轮T/B/M中位数作固定参考，查询参考为R7 Exact；εT=.001s，εB=εM=1048576B。f(R)=R/(1+R)，E项目=.6f(RT)+.2f(RB)+.2f(RM)，E=(E写+2E查)/3。下表E逐轮计算后取中位数；参考点为0.5。全部R与E写/E查逐轮值见results.json。

|分布|限额|模式|R8 E|R9 E|相对R8提升|相对参考0.5提升|
|---|---|---|---:|---:|---:|---:|
|clustered|16m|exact|0.630407|0.684617|8.60%|36.92%|
|clustered|16m|ann|0.642883|0.703249|9.39%|40.65%|
|clustered|2g|exact|0.596004|0.646227|8.43%|29.25%|
|clustered|2g|ann|0.615294|0.671408|9.12%|34.28%|
|uniform|16m|exact|0.638138|0.686872|7.64%|37.37%|
|uniform|16m|ann|0.635871|0.686379|7.94%|37.28%|
|uniform|2g|exact|0.599767|0.649831|8.35%|29.97%|
|uniform|2g|ann|0.600186|0.648058|7.98%|29.61%|

## 进程内剖析与预算帕累托

benchmark_cpu排除协议/序列化/驱动，五次完整查询循环平均，仅作归因诊断，绝不替代端到端T。两代均读取相同数据库副本与查询，checksum保持一致。

```json
[
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.0029508,
    "engine_seconds": 0.107548,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298579606,
    "phase": "r8"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00254214,
    "engine_seconds": 0.0444303,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298517950,
    "phase": "r8"
  },
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.00260356,
    "engine_seconds": 0.104657,
    "vector_page_loads_measured": 0,
    "budget_bytes": 12582912,
    "checksum": 298579606,
    "phase": "r8"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00242942,
    "engine_seconds": 0.0448505,
    "vector_page_loads_measured": 0,
    "budget_bytes": 12582912,
    "checksum": 298517950,
    "phase": "r8"
  },
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.00151995,
    "engine_seconds": 0.103644,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298579606,
    "phase": "r9"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00143798,
    "engine_seconds": 0.0252711,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298517950,
    "phase": "r9"
  },
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.00162401,
    "engine_seconds": 0.0981932,
    "vector_page_loads_measured": 0,
    "budget_bytes": 12582912,
    "checksum": 298579606,
    "phase": "r9"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00149828,
    "engine_seconds": 0.0253728,
    "vector_page_loads_measured": 0,
    "budget_bytes": 12582912,
    "checksum": 298517950,
    "phase": "r9"
  }
]
```

空请求诊断（1020次，前20次预热；计入诊断行接收）：原生1000次ping约0.073s，Linux Docker约0.408s。该设置的固定交互成本已占ANN端到端的大部分，但空请求不是严格可相减的成本模型。

```json
[
  {
    "phase": "r8",
    "mode": "legacy_set",
    "requests": 1000,
    "roundtrip_including_metrics_seconds": 0.07217249795940006
  },
  {
    "phase": "r8",
    "mode": "json_ping",
    "requests": 1000,
    "roundtrip_including_metrics_seconds": 0.0727689660343458
  },
  {
    "phase": "r9",
    "mode": "legacy_set",
    "requests": 1000,
    "roundtrip_including_metrics_seconds": 0.07314420895272633
  },
  {
    "phase": "r9",
    "mode": "json_ping",
    "requests": 1000,
    "roundtrip_including_metrics_seconds": 0.07294541395822307
  },
  {
    "phase": "r9_docker_linux",
    "mode": "json_ping",
    "requests": 1000,
    "roundtrip_including_metrics_seconds": 0.4078157630065107
  }
]
```

10/12MiB显式预算两个数据分布预热后向量换页和查询物理read_bytes均0，故默认上限调整到10MiB；16MiB容器55%规则仍生效，4/8MiB保持独立回归。实验原始JSON分别保存。

## 回归与证据

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.83 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.26 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.02 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    0.80 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    0.94 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    1.91 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    4.20 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed    9.76 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  18.89 sec
```

官方二进制20/20、只读Docker20/20，95条SQL。扩展1500余语法差分、混合驻留、失败批次/孤儿槽位、113B部分写强杀、四阶段Checkpoint和恢复中再强杀。PID1信号回滚通过。

```json
[
  {
    "budget": 4194304,
    "max_request_seconds": 0.42264784399594646,
    "index_below_03s": true,
    "ann_effective_nprobes": [
      5
    ],
    "ann_route_counts": {
      "ivf": 1000
    }
  },
  {
    "budget": 8388608,
    "max_request_seconds": 0.4138540589992772,
    "index_below_03s": true,
    "ann_effective_nprobes": [
      5
    ],
    "ann_route_counts": {
      "ivf": 1000
    }
  }
]
```

```json
{
  "bytes": 121049,
  "sha256": "0c47c38db4cb13612a6fd5c7d12895670c8b007ec67acd2ce9fd080f090bed5c",
  "identical_copies": true,
  "rounds": 9,
  "learning_units": 28,
  "unique_valid_pointers": 56,
  "old48_pointer_values_and24_units_unchanged": true,
  "old8_prompts_followups_summaries_unchanged": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "strict_utf8": true
}
```

保留前8轮所有提示、补充、摘要及24单元/48条证据原文，仅压缩未引用历史诊断并指向Git完整记录；新Round9为真实提示全文及4个新闭环单元。内部生成脚本只留work/internal_tools。

局限：目标ANN<0.09s、Exact<0.18s以实际数值评判，不能将进程内微测当达标；宿主调度和管道固定开销仍存在。查询/写入均不得省略正确性或持久化。只保证已测负载，SIGKILL模型不扩展为断电恢复。未预测线上3000+或下一次Prompt分数。

复现：cmake Release构建；ctest --test-dir work/build --output-on-failure；protocol_check/run.py二进制及镜像；tests/benchmark_local.py --automatic-ann --default-budget；tests/benchmark_cgroup.py --rounds 3。CPU诊断需显式构建benchmark_cpu，参数为数据副本、查询SQL文件、exact/ann、预算字节。
