# Round10 四代实测与资源归因

本文记录提交`633935417dd0f536e60203fe851f0f8032fd1f05`的初版实测；其后ANN阈值和过滤召回约束已调整，见[路由调优报告](round10-routing-tuning.md)。复现本文的精确过滤ANN断言需使用该历史提交。

2026-11-13 接通电源后重新构建并复测；保留原有绝对延迟门槛，四代版本均重新采集。

基于已上线R9的本轮实测，新增字典有序倒排与1对4批核；首版热循环负向结果后改为八累加器、移位寻址和强制内联。追加不填充冷页池，先同步后逐页交接缓存，<=8MiB保留内核缓存。新索引直接写出、同步后发布元数据，并用2KiB逻辑块聚合SlotId列表。全部回归通过，按实际记录性能得失。

线上3258.709224及Prompt原始11.0为用户报告，未独立验证；本报告不预测3800+或13–15分。

旧Exact已在距离计算前过滤；倒排优化槽位检查而非16倍距离计算。ANN小子集改走精确扫描可增加距离计算并换取过滤召回。

## 复现命令

```sh
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
python3 protocol_check/run.py --command '["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]' --output work/r10b-protocol-native
docker build -t minivectordb:r10b .
python3 protocol_check/run.py --image minivectordb:r10b --output work/r10b-protocol-docker
/tmp/mvdb-benchmark-venv/bin/python tests/benchmark_local.py --phase after --automatic-ann --default-budget --binary work/build/mvdb_local --output work/r10b-native-r10.json
DOCKER_HOST=unix:///mnt/wsl/docker-desktop/shared-sockets/guest-services/docker.proxy.sock /tmp/mvdb-benchmark-venv/bin/python tests/benchmark_cgroup.py --docker /mnt/wsl/docker-desktop/cli-tools/usr/bin/docker --image minivectordb:r10b --phase r10 --output work/r10b-cgroup-r10.json --rounds 3 --require-filter-exact
```

协议结果目录需不存在；所有命令从仓库根目录执行。旧版本使用同一驱动与相同配置，替换冻结的可执行文件及镜像；CPU 诊断使用 tools/benchmark_cpu.cpp。

## 原生同工作负载单次端到端

|分布|代际|写入请求s|Exact s|ANN s|索引s|读MiB|写MiB|VmHWM MiB|召回无/有过滤|
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
|clustered|r7|1.184817|0.671822|0.357801|0.216170|0.434|13.191|13.652|0.9908/0.9886|
|uniform|r7|1.180229|0.765735|0.771971|0.217150|0.434|13.188|13.570|1.0000/1.0000|
|clustered|r8|0.983140|0.251437|0.168639|0.231261|0.848|8.293|13.465|0.9908/0.9886|
|uniform|r8|0.989389|0.252797|0.262967|0.238200|0.848|8.293|13.504|1.0000/1.0000|
|clustered|r9|0.460681|0.254051|0.157706|0.229353|0.875|7.961|13.398|0.9908/0.9886|
|uniform|r9|0.453823|0.275205|0.247905|0.235993|0.875|7.961|13.293|1.0000/1.0000|
|clustered|r10|0.455326|0.248353|0.167456|0.064925|1.883|6.926|12.059|0.9908/1.0000|
|uniform|r10|0.448601|0.254076|0.245156|0.069656|0.906|6.918|12.121|1.0000/1.0000|

## 三轮独立容器完整窗口 T/B/M 中位数

Same Linux Docker CLI/socket; full lifecycle B/M including load,index,warmup,close; Desktop startup image reads and holder shell included. Fresh volumes without global cache clearing. CPU microbenchmarks are diagnostic; online scores not extrapolated.

|分布|内存|项目|代际|T s|B MiB|M MiB|
|---|---|---|---|---:|---:|---:|
|clustered|16m|write|r7|1.585505|11.953|16.000|
|clustered|16m|write|r8|1.314830|7.477|14.992|
|clustered|16m|write|r9|0.484115|7.176|15.172|
|clustered|16m|write|r10|0.477904|7.145|9.031|
|clustered|16m|exact|r7|2.023373|13.477|16.000|
|clustered|16m|exact|r8|0.595345|8.914|16.000|
|clustered|16m|exact|r9|0.575673|8.605|16.000|
|clustered|16m|exact|r10|0.568977|7.367|8.797|
|clustered|16m|ann|r7|1.211122|13.492|16.000|
|clustered|16m|ann|r8|0.477410|8.922|16.000|
|clustered|16m|ann|r9|0.441078|8.633|16.000|
|clustered|16m|ann|r10|0.460272|7.363|8.871|
|clustered|2g|write|r7|1.465330|11.938|20.641|
|clustered|2g|write|r8|1.191303|7.426|15.066|
|clustered|2g|write|r9|0.484648|7.094|14.805|
|clustered|2g|write|r10|0.486820|7.094|8.867|
|clustered|2g|exact|r7|1.071073|13.191|23.484|
|clustered|2g|exact|r8|0.588208|8.688|17.500|
|clustered|2g|exact|r9|0.579249|8.355|17.309|
|clustered|2g|exact|r10|0.576944|7.320|8.953|
|clustered|2g|ann|r7|0.639637|13.191|23.457|
|clustered|2g|ann|r8|0.468761|8.688|17.500|
|clustered|2g|ann|r9|0.440581|8.355|17.289|
|clustered|2g|ann|r10|0.454764|7.320|9.035|
|uniform|16m|write|r7|1.666429|11.938|16.000|
|uniform|16m|write|r8|1.377108|7.473|15.133|
|uniform|16m|write|r9|0.490259|7.156|14.820|
|uniform|16m|write|r10|0.484203|7.148|8.996|
|uniform|16m|exact|r7|1.887108|13.480|16.000|
|uniform|16m|exact|r8|0.608431|8.930|16.000|
|uniform|16m|exact|r9|0.577738|8.605|16.000|
|uniform|16m|exact|r10|0.560572|7.359|8.957|
|uniform|16m|ann|r7|1.915334|13.465|16.000|
|uniform|16m|ann|r8|0.595829|8.914|16.000|
|uniform|16m|ann|r9|0.593135|8.605|16.000|
|uniform|16m|ann|r10|0.572784|7.359|8.879|
|uniform|2g|write|r7|1.467417|11.930|20.418|
|uniform|2g|write|r8|1.183758|7.426|15.094|
|uniform|2g|write|r9|0.500416|7.094|14.695|
|uniform|2g|write|r10|0.487409|7.094|8.934|
|uniform|2g|exact|r7|1.068422|13.188|22.961|
|uniform|2g|exact|r8|0.590035|8.688|17.594|
|uniform|2g|exact|r9|0.597962|8.355|17.348|
|uniform|2g|exact|r10|0.564966|7.312|8.926|
|uniform|2g|ann|r7|1.075595|13.188|23.008|
|uniform|2g|ann|r8|0.586605|8.688|17.379|
|uniform|2g|ann|r9|0.576437|8.355|17.340|
|uniform|2g|ann|r10|0.561641|7.312|8.977|

## 平滑效率 E

Fixed R7 three-run median write/exact reference; epsT=.001s,epsB=epsM=1048576B; .6f(RT)+.2f(RB)+.2f(RM);f(R)=R/(1+R); E=(Ewrite+2Equery)/3; median3 E then round6.
逐轮R/Ewrite/Equery完整值见results.json；R7参考中位指标点为0.5，R7逐轮E中位可因多指标联合而略偏离。

|分布|内存|模式|R7 E|R8 E|R9 E|R10 E|R10相对R9|相对参考0.5|
|---|---|---|---:|---:|---:|---:|---:|---:|
|clustered|16m|exact|0.499296|0.637414|0.685641|0.717325|+4.62%|+43.46%|
|clustered|16m|ann|0.552827|0.654118|0.702504|0.730252|+3.95%|+46.05%|
|clustered|2g|exact|0.500157|0.602483|0.645815|0.675758|+4.64%|+35.15%|
|clustered|2g|ann|0.550147|0.622496|0.669973|0.696366|+3.94%|+39.27%|
|uniform|16m|exact|0.499328|0.631636|0.682814|0.714244|+4.60%|+42.85%|
|uniform|16m|ann|0.498876|0.634549|0.681163|0.713624|+4.77%|+42.72%|
|uniform|2g|exact|0.500120|0.601768|0.640580|0.675549|+5.46%|+35.11%|
|uniform|2g|ann|0.498479|0.602369|0.643879|0.676262|+5.03%|+35.25%|

## 源码核查与迁移局限

```json
{
  "root_green": true,
  "low_memory_passed": true,
  "summary": "基于已上线R9的本轮实测，新增字典有序倒排与1对4批核；首版热循环负向结果后改为八累加器、移位寻址和强制内联。追加不填充冷页池，先同步后逐页交接缓存，<=8MiB保留内核缓存。新索引直接写出、同步后发布元数据，并用2KiB逻辑块聚合SlotId列表。全部回归通过，按实际记录性能得失。",
  "evidence_summary": "旧Exact原已在L2前过滤，不宣称减少93.75%距离计算；批核共享q但仍需四向量加载。去除新索引payload WAL和64文件填充均有作用，索引有效写113024B、物理约208KiB。SIGKILL四阶段Checkpoint、二次中断恢复、64KiB旧R9索引兼容和小预算回归均通过。保留首版CPU负向实测。",
  "index_transfer": "新命名区先完成payload同步再发布header，回滚保留区域Undo；四阶段与二次恢复中断均通过。后续DML仍走WAL/Shadow，旧索引可读；未提交孤儿区暂不清理，不宣称断电恢复。",
  "legacy_fixture": {
    "legacy_R9_index_read_R10_tiny_pool": true,
    "rows": 10,
    "route": "ivf"
  },
  "limits": "在线3258.709224及Prompt11.0为用户反馈；不据本地E预测3800或13~15分。31单元目标档位不等于已获分。缓存预热和端到端/核心计时分开；fadvise为提示，脏页不保证即时释放。",
  "cache_guard": "读后/边界回收仅预算>8MiB且单文件<=页池容量；fstat失败保留缓存。4/8/10MiB预算与4/16MiB文件迁移测试；这是单文件启发式，不是多文件工作集驻留证明。"
}
```

## CPU诊断与回归

进程内耗时排除协议与驱动，不能代替正式端到端T；不同ANN路由的校验和不要求跨版本相同，精确结果与实际召回另验。

```json
[
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.00150355,
    "engine_seconds": 0.101697,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298579606,
    "phase": "r9",
    "distribution": "clustered",
    "group": "all"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00152534,
    "engine_seconds": 0.0262167,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298517950,
    "phase": "r9",
    "distribution": "clustered",
    "group": "all"
  },
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.00192645,
    "engine_seconds": 0.100242,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298579606,
    "phase": "r10",
    "distribution": "clustered",
    "group": "all"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00148258,
    "engine_seconds": 0.0281705,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 298440142,
    "phase": "r10",
    "distribution": "clustered",
    "group": "all"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.00130393,
    "engine_seconds": 0.0720557,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 149955902,
    "phase": "r9",
    "distribution": "clustered",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.000745355,
    "engine_seconds": 0.0158645,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 149816438,
    "phase": "r9",
    "distribution": "clustered",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.000727247,
    "engine_seconds": 0.0814715,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 149955902,
    "phase": "r10",
    "distribution": "clustered",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.0011081,
    "engine_seconds": 0.0152139,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 149816438,
    "phase": "r10",
    "distribution": "clustered",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.00100355,
    "engine_seconds": 0.0250151,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 148623704,
    "phase": "r9",
    "distribution": "clustered",
    "group": "filtered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.000897468,
    "engine_seconds": 0.00838348,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 148701512,
    "phase": "r9",
    "distribution": "clustered",
    "group": "filtered"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.00086964,
    "engine_seconds": 0.0140702,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 148623704,
    "phase": "r10",
    "distribution": "clustered",
    "group": "filtered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.000803809,
    "engine_seconds": 0.0150729,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 148623704,
    "phase": "r10",
    "distribution": "clustered",
    "group": "filtered"
  },
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.00215635,
    "engine_seconds": 0.10334,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 301298566,
    "phase": "r9",
    "distribution": "uniform",
    "group": "all"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00173751,
    "engine_seconds": 0.101155,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 301298566,
    "phase": "r9",
    "distribution": "uniform",
    "group": "all"
  },
  {
    "queries": 1000,
    "mode": "exact",
    "parse_seconds": 0.00159946,
    "engine_seconds": 0.0940629,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 301298566,
    "phase": "r10",
    "distribution": "uniform",
    "group": "all"
  },
  {
    "queries": 1000,
    "mode": "ann",
    "parse_seconds": 0.00149329,
    "engine_seconds": 0.0933621,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 301298566,
    "phase": "r10",
    "distribution": "uniform",
    "group": "all"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.000793778,
    "engine_seconds": 0.0750718,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 151211168,
    "phase": "r9",
    "distribution": "uniform",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.000798672,
    "engine_seconds": 0.0770825,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 151211168,
    "phase": "r9",
    "distribution": "uniform",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.000779171,
    "engine_seconds": 0.0776747,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 151211168,
    "phase": "r10",
    "distribution": "uniform",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.00126273,
    "engine_seconds": 0.0790673,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 151211168,
    "phase": "r10",
    "distribution": "uniform",
    "group": "unfiltered"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.00116794,
    "engine_seconds": 0.0250602,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 150087398,
    "phase": "r9",
    "distribution": "uniform",
    "group": "filtered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.000846371,
    "engine_seconds": 0.0257069,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 150087398,
    "phase": "r9",
    "distribution": "uniform",
    "group": "filtered"
  },
  {
    "queries": 500,
    "mode": "exact",
    "parse_seconds": 0.000888305,
    "engine_seconds": 0.0130691,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 150087398,
    "phase": "r10",
    "distribution": "uniform",
    "group": "filtered"
  },
  {
    "queries": 500,
    "mode": "ann",
    "parse_seconds": 0.000855813,
    "engine_seconds": 0.0126535,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 150087398,
    "phase": "r10",
    "distribution": "uniform",
    "group": "filtered"
  }
]
```

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    1.04 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.30 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    1.11 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.03 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    2.10 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    4.30 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   10.89 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  20.91 sec
```

官方native20/20、readonly Docker20/20；完整协议结果和low-memory输入一并归档。

## 会话证据保真与体积

```json
{
  "bytes": 120922,
  "sha256": "5576006151be06682490169c257fb95cacacb096b9d03d45de627a336eacf62d",
  "unique_valid_pointers": 62,
  "old28_units_and56_pointer_values_context_unchanged": true,
  "old9_prompts_followups_summaries_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "size_feasibility": {
    "original_bytes": 121049,
    "all_verification_removed_bytes": 104077,
    "minimum_preserving_all_other_content_exceeds_95000": true,
    "compacted_base": {
      "bytes": 106311,
      "sha256": "329baf614019cf3903f54a9f3f04a32da158486a0d3a0df567a8f6cdd9c31ebe",
      "unique_valid_pointers": 56,
      "old28_units_and56_pointer_values_context_unchanged": true,
      "old9_prompts_followups_summaries_unchanged": true,
      "strict_utf8": true,
      "duplicate_keys": 0,
      "nonfinite": 0
    },
    "policy": "Preserve every original prompt/followup/summary and28 complete units; condense only uncited verification, retain archive+hash."
  }
}
```

95,000字节历史基础目标与保留全部提示、上下文、28单元要求不能同时满足：即移除全部历史verification仍超过95,000。本轮保留全部原文，只压缩未引用历史验证；完整历史随Git提交可恢复。
