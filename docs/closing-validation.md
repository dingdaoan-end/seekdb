# 收官修复、默认参数性能与真实交互证据

代码位于D:/clone/seekdb-2026的2026-competition-round2分支，未提交或推送。编译环境为WSL Ubuntu/GCC15.2；容器Ubuntu24.04/GCC13.3。

持久化mutations_since_calibration与rows_at_calibration，成功DML按行计数，严格超过max(64, rows/5)才自动回退；语句失败和事务回滚同时恢复计数。保留旧MV3的calibration_stale字段兼容含旧失效标志的数据。

未传预算时取memory.max/memory.high最小有限值的55%，夹限4..24MiB；无限额/不可读回退20971520B。真实16/32/64MiB容器分别得到9227468/18454937/25165824B；显式65536B覆盖通过。

删除committed.<epoch>同步。带完整长度、帧CRC及事务CRC的WAL COMMIT作为提交权威；普通100行批次一次WAL fdatasync。另修复绝对64KiB偏移跨界造成的额外同步，大事务保留有界窗口排空。故障模型为SIGKILL。

## 实际命令

```sh
cd /mnt/d/clone/seekdb-2026
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/tmp/mvdb-benchmark-venv/bin/python
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
/tmp/mvdb-benchmark-venv/bin/python tests/benchmark_local.py --phase before --automatic-ann --default-budget --binary work/phase3-baseline/mvdb_local --output work/benchmark-closing-before.json
/tmp/mvdb-benchmark-venv/bin/python tests/benchmark_local.py --phase after --automatic-ann --default-budget --binary work/build/mvdb_local --output work/benchmark-closing-after.json
docker build --target test -t minivectordb-tests:final .
docker build -t minivectordb:final .
python3 tests/docker_smoke.py --image minivectordb:final
python3 tests/docker_budget.py --image minivectordb:final
python3 tests/benchmark_commit.py --before work/phase3-baseline/mvdb_local --after work/build/mvdb_local --output work/closing-commit-paired.json
```

最终原生CTest 7/7通过，38.68s；最终容器CTest 7/7通过，44.64s。public为32/32，extra为11组（合计12个Python测试方法）；另含存储/索引/SIMD/cgroup与10k动态DML目标。四阶段Checkpoint强杀、恢复中连续二次强杀、短页、WAL续写、语句/事务回滚均通过。

## 完整默认参数对照

两版均省略预算参数且没有外部NPROBE命令。旧版默认8MiB由冻结源代码核实，旧版没有输出预算诊断字段；新版实际输出20MiB/fallback。数据、Schema及input_sha256相同：10k×128D、16类别VARCHAR(32)、content VARCHAR(128)，100×100行写入，exact与ann各1000条查询，各半有/无过滤。墙钟单位秒，I/O和RSS单位MiB。

|数据|版本/预算|写入|Exact 1000|ANN 1000|建索引|关闭|物理读|物理写|VmHWM|
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
|clustered|before/8MiB|1.914355|32.640514|18.041515|2.605528|0.014402|8191.812500|14.488281|9.777344|
|clustered|after/20MiB|3.074551|1.382594|0.748071|0.706826|0.142975|0.777344|13.199219|13.195312|
|uniform|before/8MiB|1.950861|33.376175|33.202045|2.938930|0.015258|10556.046875|14.484375|9.804688|
|uniform|after/20MiB|3.046121|1.412356|1.428978|0.711404|0.153481|0.777344|13.195312|13.207031|

|数据/版本|WAL写入B|关闭后WAL B|推荐nprobe/路由|召回无过滤/有过滤|查询写B Exact/ANN|Undo溢写B|快照B|数据页应用写B|目录占用B|
|---|---:|---:|---|---|---|---:|---:|---:|---:|
|clustered/before|5608494|0|5/{'ivf': 1000}|0.9908/0.9886|4063232/16384|0|0|7634944|7651328|
|clustered/after|5608494|0|5/{'ivf': 1000}|0.9908/0.9886|0/0|0|0|7618560|7651328|
|uniform/before|5601896|0|59/{'exact_fallback': 1000}|1.0000/1.0000|4063232/16384|0|0|7634944|7647232|
|uniform/after|5601896|0|59/{'exact_fallback': 1000}|1.0000/1.0000|0/0|0|0|7618560|7651328|

新版两组写入WAL均低于600B/行；100批各恰好100次WAL同步。查询的payload页物理读均为0；聚类1000次IVF/5，均匀1000次exact_fallback（推荐59/64）。Exact全部排名与NumPy float64 oracle一致。

这些是最终封板版本的实测值。初次新版测得1.673/1.693s写入，封板复测为3.075/3.046s，墙钟存在明显波动；完整原始运行均保留，不能从跨时间、不同预算的单次值推断单一实现改动的耗时贡献。最终表保留复测值，不用较快的早期值替换。

物理I/O是/proc/PID/io或引擎退出前/proc/self/io，覆盖进程所有文件，包含WAL/Undo/Shadow/Checkpoint；不是仅/data目录累计写。data_page_write_bytes是应用级数据页写入字节，不能与物理写相加。st_blocks仅为最终空间占用；VmHWM不等于cgroup memory.peak。引擎wait4 RSS可能继承Python fork高水位，不用它替代引擎自身VmHWM。

## 提交路径同预算交替对照

为分离默认预算变化，用两版相同20MiB、相同预编码10k行/100批，按before/after、after/before、before/after顺序做3轮。该补充实验显式指定预算，独立于上面的默认参数基准；时间只含100次SQL往返。

|轮次|版本|100批耗时s|总物理写B（含关闭）|WAL同步增量|
|---:|---|---:|---:|---:|
|1|before|2.959865|13279232|旧版未输出|
|1|after|2.586969|12517376|100|
|2|after|2.586551|12517376|100|
|2|before|3.016378|13279232|旧版未输出|
|3|before|3.051131|13279232|旧版未输出|
|3|after|2.599809|12517376|100|

中位数3.016378→2.586969s，下降14.24%；总物理写13279232→12517376B。所有新版轮次100批=100次WAL同步；负向对照恢复旧64KiB边界规则时单次同步断言失败。

## 动态索引与证据校验

10,000条聚类数据建索引后各50次INSERT/UPDATE/DELETE，共150次修改，150次标量可见性断言通过；1000个独立查询与200个新增/更新向量查询均保持IVF/5，Exact均匹配独立oracle。

独立查询召回无过滤/有过滤：0.9910/0.9884；受影响向量：0.9960/0.9890。4200行专用索引回归另验证840/841阈值两侧、失败语句与外层ROLLBACK、重启计数恢复。超过阈值当前保守Exact，不承诺在线重校准。

agent/conversation.json与根目录conversation.json从实际chat rollout抽取5轮完整student_prompt；历史agent最终回复与中途未完成节点保留，摘要明确标为事后整理。14个单元覆盖5维度，以2.0为证据目标，最终得分由正式评审决定。历史13.19MiB来自显式32MiB基准，新默认VmHWM使用本轮值。

```json
{
  "status": "PASS",
  "bytes": 91876,
  "kib": 89.72265625,
  "sha256": "e910cd300b32454563b9c6be081b945b61f0781b4dd9ffacaa228b91ea8eee97",
  "identical_copies": true,
  "conversations": 5,
  "learning_units": 14,
  "dimensions": 5,
  "distinct_resolved_evidence_pointers": 28,
  "max_evidence_characters": 175,
  "strict_utf8_json": true,
  "duplicate_keys": 0,
  "nonfinite_constants": 0,
  "synthetic_true": false
}
```

28个RFC6901指针定位不同的精确原文；学生引用还逐一验证是对应完整student_prompt的真实子串。每个student_analysis/experiment_result/transfer_and_limitation均<=200字符；最长证据175字符。UTF-8严格解析、无重复键/NaN/Infinity、无synthetic:true，两份字节完全一致。

## 原始最终CTest输出

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/7 Test #1: budget_regression ................   Passed    0.02 sec
    Start 2: storage_regression
2/7 Test #2: storage_regression ...............   Passed    4.52 sec
    Start 3: core_regression
3/7 Test #3: core_regression ..................   Passed    0.06 sec
    Start 4: simd_regression
4/7 Test #4: simd_regression ..................   Passed    0.03 sec
    Start 5: index_regression
5/7 Test #5: index_regression .................   Passed    6.00 sec
    Start 6: all
6/7 Test #6: all ..............................   Passed   16.25 sec
    Start 7: dynamic_dml
7/7 Test #7: dynamic_dml ......................   Passed   11.64 sec

100% tests passed, 0 tests failed out of 7

Total Test time (real) =  38.68 sec
```

完整raw Benchmark、动态DML记录、Docker日志、证据JSON与源码归档均随本报告交付。
