# Round 7 防超时修复与实测报告

本轮用户后续答复：先不推送。本地提交SHA由提交后清单记录。
正式平台反馈28/29、JSON response timeout×1；没有失败用例或trace，因此I/O归因是经本地复现支持的假设，不宣称已确认线上唯一根因或已达29/29。

## 修改与验证边界

默认buffered pread/pwrite，仅MVDB_USE_ODIRECT=1使用O_DIRECT。BufferPool为.pk/.header保留有上限的优先份额，三帧池仍留payload进展。去除Shadow逐页和WAL/Undo/Spill64KiB同步；保留COMMIT一次WAL同步和Checkpoint发布顺序。SIGKILL下已完成write在内核中可被新进程读取，不扩大承诺到断电或磁盘破坏。
同主键/向量/摘要未变时跳过无用写，普通列blob能放回原槽时原地写且仍记录Undo。Resident校准16样本、单次向量真值遍历；payload超过预算一半或工作区不足时跳过、持久化经验值并自动Exact保召回，手动NPROBE仍可启用IVF。
sigaction不设SA_RESTART，handler仅置sig_atomic_t，poll/read检查停止标志，在主循环外关闭回滚。输出统一无缓冲、处理短写/EINTR并flush+fflush；SQL支持注释和多余尾分号，拒绝多语句，不执行半条语句。stdout永不被读取或正在执行SQL时无法承诺立即退出，实际测试覆盖安全边界。

## 同负载前后对照

固定seed20261109、10,000×128D聚类float32、16类VARCHAR(32)、content VARCHAR(128)；100×100自动提交写入，Exact和ANN各1000条查询，各半等值过滤。再执行全表10k UPDATE、ROLLBACK、关闭重启、提交一次修改、全表未提交UPDATE、SIGKILL、恢复并检查可见性。单条计时含JSONL往返，不含SQL生成/oracle。单位为秒。

|预算|版本|建索引|全表UPDATE|ROLLBACK|1000 Exact|1000 ANN|最慢SQL|最长启动/恢复|
|---|---|---:|---:|---:|---:|---:|---:|---:|
|4MiB|before|6.451718|4.460316|17.251507|81.994236|27.250073|17.251507|0.140569|
|8MiB|before|2.754127|4.416828|16.463328|32.924314|17.945861|16.463328|0.181756|
|4MiB|after|0.204633|0.407173|0.238679|1.177844|1.156462|0.407173|0.044839|
|8MiB|after|0.205220|0.395083|0.240015|1.244343|1.246388|0.400897|0.026942|

修改后两预算所有单次SQL均<0.5s；ANN两组召回均为1.0（小预算跳过校准，自动exact_fallback，经验推荐16）；Exact排名全部匹配NumPy float64 oracle。CTest再独立重复完整负载，最慢SQL为4MiB 0.397516s、8MiB 0.405773s，均通过。内存充足的16MiB动态回归仍校验校准IVF与150次DML。

|预算/版本|首进程物理读MiB|物理写MiB|VmHWM MiB|Undo溢写B|WAL同步次数|
|---|---:|---:|---:|---:|---:|
|4MiB/before|9590.183594|108.394531|8.492188|69059005|399|
|8MiB/before|9122.960938|106.964844|10.453125|69059005|399|
|4MiB/after|0.433594|15.761719|8.601562|1340309|104|
|8MiB/after|0.433594|15.605469|10.605469|1340309|104|

以上I/O/RSS覆盖首进程从启动、写入、建索引、2000查询、全表回滚到关闭，不是全部重启/被强杀进程之和。没有drop_caches；buffered版复用正常写入留下的内核缓存。4/8MiB是引擎预算，不是cgroup限制；干净Page Cache可回收不代表不会OOM，VmHWM不含Page Cache，本轮没有伪造memory.peak。

## 回归与协议结果

CTest：9/9 PASS，25.35s。官方二进制20/20 PASS（用例耗时之和4.794s）、只读Docker20/20 PASS（26.151s），均包含全部95条公开SQL。官方文件保持f4d4e26653af823a7601668802f80e374ad6643c原样。
Docker实际PID1的SIGTERM、SIGINT、docker stop均exit0，分别0.259095/0.270607/0.270731s，保持stdin打开并有未提交修改，重启确认完整回滚。存储回归覆盖4个Checkpoint阶段、恢复再强杀、同步/Shadow I/O的EINTR，以及显式直接I/O回退。
首轮集成发现16样本校准状态在64KiB极小预算下不够，core/all失败；加入工作区预估后修正，最终完整9/9通过。新测试不是通过放宽阈值来掩盖失败。

## 实际复验命令

```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/tmp/mvdb-benchmark-venv/bin/python
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
/tmp/mvdb-benchmark-venv/bin/python tests/low_memory.py --binary work/round7-before/mvdb_local --phase before --output work/round7-low-memory-before.json
/tmp/mvdb-benchmark-venv/bin/python tests/low_memory.py --binary work/build/mvdb_local --phase after --output work/round7-low-memory-after.json --enforce-latency
export VECTORDB_COMMAND='["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]'
python3 protocol_check/run.py --command "$VECTORDB_COMMAND" --output work/round7-protocol-native
docker build -t minivectordb:round7 .
export VECTORDB_IMAGE=minivectordb:round7
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output work/round7-protocol-docker
python3 tests/docker_shutdown.py --image minivectordb:round7
```

## 最终CTest原始输出

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.55 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.05 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    1.34 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.01 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    3.01 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    5.31 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   13.84 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  25.35 sec
```

## 证据JSON校验

保留七轮完整学生提示；20个学习单元、40条不同证据原文均可通过RFC6901定位，学生摘录逐一验证是实际提示的子串。两份文件逐字节一致，严格UTF-8 JSON，无重复键、NaN/Infinity或synthetic:true。

```json
{
  "bytes": 128066,
  "sha256": "ab707418df6f229ee2f69df35bd414b7b072aa3485c34c1fb742ea38f518cc92",
  "copies_identical": true,
  "conversations": 7,
  "learning_units": 20,
  "valid_distinct_evidence_pointers": 40,
  "max_evidence_characters": 175,
  "strict_utf8_json": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "synthetic_true": false
}
```

本地测得全部请求达标不等于所有硬件、任意规模SQL或正式隐藏用例也必然达标。完整原始逐请求计时与协议日志随报告交付；生成脚本仅在已忽略work/internal_tools/中。
