# 证据体积与小预算IVF边界微调报告

日期：2026-11-11。基线提交954cf82ec054cb351c591194404b25ea823030c9。

## JSON严格保持项

删除未被指针引用、与回复摘要/验证结果重复的agent_original_final_messages全文副本；完整原始版本仍保存在954cf82及本地work/internal_tools/json-pre-refinement.json。七轮student_prompt和agent_response_summary逐字不变，20个learning_units对象深度比较完全一致，40个RFC6901路径及其解析值完全不变。所有student_analysis仍是对应原始student_prompt的精确子串。没有增加第8轮或改写历史实验结果。

```json
{
  "old_bytes": 128066,
  "bytes": 104709,
  "sha256": "b252638155ac9e24303379beafe9cffcdba2ad86abc2763caf9d03427d3fadf6",
  "identical_copies": true,
  "conversations": 7,
  "learning_units": 20,
  "unchanged_learning_units": true,
  "unchanged_student_prompts": true,
  "unchanged_agent_response_summaries": true,
  "unchanged_distinct_rfc6901_evidence_values": 40,
  "utf8_strict": true,
  "duplicate_keys": 0,
  "nonfinite_constants": 0
}
```

## 校准策略

active_rows<=25000时只检查批量校准工作区是否可分配，不再以向量payload>budget/2拒绝校准。active_rows>25000且payload>budget/2，或工作区无法分配时仍跳过。保留推荐探测超过半数桶、显式手动探测、既有小表及漂移保护。16样本共享一次顺序向量扫描并保留Top10，不在桶中复制向量。
新增25000/25001、payload半预算/半预算+1的精确边界检查；真实4200×128D且仅1MiB预算下测量校准、自动IVF和重启持久化通过；真实64KiB工作区不足时保持安全回退。修改后的4200×128断言在旧引擎失败，在新引擎通过。

## 原生4/8MiB固定负载实测

每预算10k×128D、100批写入、Exact/ANN各1000条查询、全表事务修改/回滚、SIGKILL恢复。每条ANN响应均断言ivf、recommended_nprobe=5、effective_nprobe=5；没有发送手工NPROBE。计时包含本地JSONL往返。

|预算|建索引秒|最慢SQL秒|1000次ANN秒|路由|召回无过滤/有过滤|
|---|---:|---:|---:|---|---|
|4MiB|0.222862|0.405930|0.611564|IVF/5×1000|0.9908/0.9898|
|8MiB|0.228491|0.406803|0.575387|IVF/5×1000|0.9908/0.9898|

建索引<0.3秒、全部单条SQL<0.5秒，两组均通过。ANN实测为0.612/0.575秒，较此前约1.15/1.25秒改善；没有达到用户预估的约0.35秒，未以预估值替换测量。
完整CTest为9/9 PASS，23.96秒，包括95条公开SQL、精确距离、事务/恢复、协议/信号以及低内存和动态DML回归。

## 额外16MiB容器检查

使用官方Session和只读Docker边界，16MiB真实cgroup限额，未手工指定预算。自动预算9227468字节，10k建索引后1000次ANN全部IVF/5，召回符合>=0.985。此测试计时含Docker桥接，和原生小预算计时分列，不能混用。

```json
{
  "status": "PASS",
  "memory_limit_bytes": 16777216,
  "resolved_budget_bytes": 9227468,
  "rows": 10000,
  "ann_queries": 1000,
  "nprobe": 5,
  "all_ann_routes_ivf": true,
  "index_seconds": 1.2121627110027475,
  "ann_seconds": 1.9367290918016806,
  "max_query_seconds": 0.020845972001552582,
  "recall_unfiltered_filtered": [
    0.9907999999999999,
    0.9897999999999999
  ],
  "cgroup_memory_peak_bytes": 16777216,
  "timing_note": "Docker bridge/JSON round trip; separate from native latency benchmark"
}
```

该16MiB容器的建索引实测1.212秒，不满足原生测试的0.3/0.5秒门槛；原生门槛结果不是此容器或任意机器的保证。容器memory.peak达到16MiB限额但未OOM；本次没有官方平台隐藏用例的复测回执。

## 实际命令

```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/tmp/mvdb-benchmark-venv/bin/python
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
# CTest low_memory_regression调用完整4/8MiB负载并强制IVF/5及延迟门槛
/tmp/mvdb-benchmark-venv/bin/python tests/low_memory.py --binary work/build/mvdb_local --phase ctest --output work/build/low-memory.json --enforce-latency --require-ivf
```

## CTest原始输出

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.53 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.05 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    1.43 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.05 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    2.01 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    6.19 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   12.55 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  23.96 sec
```

所有内部压缩/报告脚本只在已忽略的work/internal_tools中；官方题包保持原样。最终提交SHA与备用仓库推送状态见提交后的结果清单。
