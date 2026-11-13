# R10-Safe验证与线上回执复盘

## 当前：镜像构建防护与差量扫描优化

本节为642d6b6之后的防御性修复；下方第一版Safe记录（包括容器8/9）保留为历史，不代表本次最终结果。

### 归因核对

0.403s来自本机4MiB，不能直接与容器10MiB的0.502589s比较。旧容器相同流程的4/8/8.8/10/default最大SQL分别为0.497560/0.509002/0.501212/0.502589/0.496518s，未显示单调的预算退化；R9存储没有>8MiB的不同刷页分支，未证明脏页积压是根因。
gprof采样中delta占CPU样本79.88%，诊断包含UPDATE/ROLLBACK及数据副本操作，其wall时间不能当作正式端到端T。对大量相同页字节逐一比较是可验证的CPU热点。
仅将相等区查找改为memcpy读取uint64_t比较，再逐字节定位首差异。区间合并、记录格式、CRC、Undo顺序和页读写/提交恢复流程不变；1200组稀疏/密集/边界输入与R9编码器逐字节相同，正反向应用全部还原。
### 构建路径

同一Dockerfile中，仅移动test阶段无法阻止legacy builder按顺序执行它。默认Dockerfile彻底去除test阶段；独立Dockerfile.test基于显式构建的build镜像执行CTest。默认旧构建器实际构建成功，日志没有ctest调用。
```sh
DOCKER_BUILDKIT=0 docker build -t minivectordb:legacy-buildsafe .
docker build --target build -t minivectordb-build:test .
docker build -f Dockerfile.test --target test -t minivectordb-tests:final .
```
测试镜像base通过BUILD_IMAGE可覆盖；普通平台构建无需上述测试步骤。不能声称默认Dockerfile的--target test仍可用，显式测试需-f Dockerfile.test。
### 实测验证

本机CTest9/9（19.57s）；显式容器CTest连续两次9/9（19.20s、21.05s，第二次--no-cache）。0.5s/建索引0.3s门槛均保持；官方二进制和只读容器各20/20、95条SQL。

|容器预算MiB|最大SQL s|全表UPDATE s|全表ROLLBACK s|
|---|---:|---:|---:|
|4|0.250615|0.189105|0.119034|
|8|0.248182|0.183757|0.117905|
|10|0.260280|0.196950|0.120598|

CTest代表预算为4/8/10MiB，运行前删除独立临时目录并在计时外sync。完整五预算（含8.8MiB及CLI默认）另外运行全部1000 Exact/ANN查询、回滚和强杀恢复，全部保留原门槛通过；不是仅靠减少预算组数通过测试。
```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.91 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.28 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    1.01 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    0.89 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    1.98 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.51 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   10.87 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  19.57 sec
```

证据文件新校验：
```json
{
  "bytes": 122675,
  "sha256": "0919daa34b67a1ab7296640a375d89d3fa545b23efa69b523b3350332faa19cf",
  "unique_valid_pointers": 62,
  "old28_units_and56_pointer_values_context_unchanged": true,
  "old9_prompts_followups_summaries_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0
}
```

---

## 第一版Safe历史记录


2026-11-13。用户报告6f91b44线上准入失败；具体失败case/异常日志未提供，以下三项是风险归因，不能认定为已复现的唯一根因。

## 回退范围

`storage.cpp/.hpp`与R9 a4c5bf5字节一致；Engine在cache()/fast_topk()以外与R9逐字一致（另新增simd.hpp引用）。SQL、协议、事务、Undo、WAL、恢复、8页追加合并、旧索引格式全部恢复。源码差异仅engine.cpp/.hpp及simd.cpp/.hpp。
保留内存字典有序倒排、max(64,nprobe*rows/(nlist*4))阈值及八累加器batch4；缓存失效仍由R9执行控制路径处理。
单文件启发式回收不能保证多文件总工作集驻留，旧4/8MiB测试遗漏>8MiB分支；追加直写与打包索引均撤回，优先恢复已上线的持久化底座。此前R10内存/I/O收益属于被撤回版本，不能沿用为Safe版指标。
数据兼容范围：R9格式；不得直接续用.pidx格式实验目录，应从R9备份或重新装载。

## 新增与保留验证

4个各4MiB文件，总工作集16MiB，在8.8MiB和10MiB页池配置下交错循环读取3遍，逐页校验且读循环DONTNEED调用为0。该断言验证调用路径，不等于在所有硬件上承诺物理读取量。
10k×128D：各预算100批写入、建索引、Exact/ANN各1000次、大事务回滚、强杀及恢复；默认启动也不传memory-budget。

|传入预算bytes（None为省略）|最大单SQL s|建索引s|ANN无/有过滤召回|
|---|---:|---:|---|
|4194304|0.402892|0.231270|0.9908/0.9898|
|8388608|0.419130|0.239427|0.9908/0.9898|
|9227468|0.399205|0.223654|0.9908/0.9898|
|10485760|0.403932|0.219380|0.9908/0.9898|
|None|0.404774|0.219071|0.9908/0.9898|

冻结R9可执行文件与Safe版各读取同一R9格式数据副本；聚类/均匀、Exact/ANN共4000条完整响应（columns/rows/距离值）逐行一致。小类别ANN允许精确子集替代近似桶结果，因此本次对照不等于证明任意数据上的近似结果永远相同。

官方二进制20/20、只读Docker20/20，公开95条SQL；存储回归保留四阶段Checkpoint强杀、重复中断恢复及语句回滚。
Docker配置缺失时，旧构建器额外执行未选中的test阶段，容器CTest为8/9：10MiB/default最大SQL为0.511172/0.515574s，超过0.5s。恢复Buildx后正式runtime镜像构建与只读协议20/20通过；额外显式构建test阶段仍为8/9：10MiB SQL0.502589s未过0.5s，默认0.496518s通过。该容器延迟边界风险未消除，不归因于已修复的插件配置。本机CTest9/9不等于容器test阶段通过，未修改任何延迟门槛。

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.03 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.87 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.26 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    1.25 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.01 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    2.88 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.72 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   19.98 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  30.18 sec
```

## 会话证据

保留原31单元和62条引用原文，在Round10末尾追加本次回执、风险复盘和工程取舍。

```json
{
  "bytes": 122218,
  "sha256": "15a324b8fa2791d3b04e52ee89dd8048345347a90d4286e92cf211608668670b",
  "unique_valid_pointers": 62,
  "old28_units_and56_pointer_values_context_unchanged": true,
  "old9_prompts_followups_summaries_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0
}
```

## 复现

```sh
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
python3 protocol_check/run.py --command '["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]' --output work/r10-safe-protocol-native
docker build -t minivectordb:r10-safe .
python3 protocol_check/run.py --image minivectordb:r10-safe --output work/r10-safe-protocol-docker
```
