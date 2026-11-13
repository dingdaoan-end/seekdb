# Round10 ANN路由阈值微调

2026-11-13；对照版本633935417dd0f536e60203fe851f0f8032fd1f05。

Exact和ANN成本回退扫描始终使用单列字典倒排子集；实际走IVF时，阈值改为`max(64, nprobe*active_rows/(nlist*4))`，使用64位乘法后再除法。
10k/64桶/5探测时阈值195；常规约625行类别保留IVF加字典过滤。64行下限和82/83行比例边界均有回归。旧代码在83行类别路由断言失败，修改后完整通过。

## 同机进程内对照

每个诊断先预热一遍，再计算五遍平均；不含管道交互，不代替端到端T。

|分布|查询组|模式|修改前ms|修改后ms|变化|
|---|---|---|---:|---:|---:|
|clustered|all|exact|85.790|85.155|-0.74%|
|clustered|all|ann|26.507|21.374|-19.37%|
|clustered|filtered|exact|11.694|12.679|+8.42%|
|clustered|filtered|ann|12.068|8.060|-33.21%|
|uniform|all|exact|88.315|84.205|-4.65%|
|uniform|all|ann|86.574|85.172|-1.62%|
|uniform|filtered|exact|12.373|12.699|+2.63%|
|uniform|filtered|ann|12.611|12.087|-4.16%|

## 原生1000条查询端到端与召回

|分布|模式|修改前s|修改后s|召回无过滤/过滤|
|---|---|---:|---:|---|
|clustered|exact|0.237179|0.234636|1.0000/1.0000|
|clustered|ann|0.142413|0.145628|0.9908/0.9886|
|uniform|exact|0.263190|0.247709|1.0000/1.0000|
|uniform|ann|0.262927|0.240361|1.0000/1.0000|

## 三轮只读容器ANN

本次12个全生命周期容器；每组分别检查召回>=0.985，不再要求聚类过滤ANN精确等于oracle。`--require-filter-exact`仍保留为可选严格检查；本次使用`--min-ann-recall .985`。

|分布|内存|1000查询T中位s|B中位MiB|M中位MiB|无过滤/过滤召回|
|---|---|---:|---:|---:|---|
|clustered|16m|0.428024|7.379|8.863|0.9908/0.9886|
|clustered|2g|0.430063|7.320|8.953|0.9908/0.9886|
|uniform|16m|0.549798|7.359|9.105|1.0000/1.0000|
|uniform|2g|0.572616|7.312|8.902|1.0000/1.0000|

## 验证与局限

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.97 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.29 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    0.93 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    0.99 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    1.83 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.77 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed    9.22 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  18.10 sec
```

官方二进制与只读Docker各20/20、95条SQL。4/8MiB最大单SQL分别为0.436362s/0.436202s，建索引均<0.3s。
动态增删改继续验证事务可见性和独立真值召回；Exact保持有序结果断言。新路由允许近似查询，无过滤与过滤组均单独设门槛。
收益取决于桶和类别分布；代价阈值是启发式，不证明所有数据都更快或固定召回。当前数据验证不能替代隐藏种子正式评测；不预测线上分数。
此前Round10报告及单元29–31描述初版历史实测，本次追加follow-up，保留全部31单元与62条证据原文。

## 复现

```sh
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
python3 protocol_check/run.py --command '["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]' --output work/r10-route-protocol-native
docker build -t minivectordb:r10-route .
python3 protocol_check/run.py --image minivectordb:r10-route --output work/r10-route-protocol-docker
python3 tests/benchmark_cgroup.py --image minivectordb:r10-route --phase r10-route --output work/r10-route-cgroup.json --rounds 3 --projects ann --min-ann-recall .985
```

## 会话校验

```json
{
  "bytes": 121441,
  "sha256": "2d1d61a0b25de25b58e1020b8877270047a28b08db43c7bef1e039ba01475f4b",
  "unique_valid_pointers": 62,
  "old28_units_and56_pointer_values_context_unchanged": true,
  "old9_prompts_followups_summaries_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "all31_existing_units_and62_pointer_values_unchanged": true
}
```
