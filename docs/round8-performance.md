# Round 8 性能优化与验证

基线：`fe90005e5f2047f48608dcf556d36bdb46ab87ee`。用户报告其线上29/29通过、Prompt原始9.0；本轮以下均为本地实测，不预测线上分数。
基线镜像另外用该提交git archive重建，二进制SHA256与实际被测旧镜像完全一致：`6f6477a1fb80afa6ec03fc3a3f2dc0ff9a2b6ea7b229495fdce9b3d819d60240`。

## 实现与反馈修正

- 有界单表热读缓存：低基数字符串精确uint16字典、主键/有效位、质心和4B SlotId；不复制向量。DML、事务控制、异常均失效，高基数回退。
- 驻留帧版本句柄按页验证；候选按物理槽位升序读取。平方距离SIMD筛选，潜在赢家float64复算，最终开方并按BIGINT主键决胜。
- 常见数值from_chars/to_chars；诊断由约50次流插入合为一次整行写。默认预算上限12MiB，与>8MiB时3/4页池配套。
- 不可见追加向量/blob先写数据、后用WAL提交元数据；文件一旦存在普通Redo或Shadow，本epoch追加恢复完整WAL。防止Shadow截断、旧逆Redo重放覆盖后来已提交尾部。追加先扩展完整页。
- WAL同步后fadvise，Checkpoint同步后提示释放数据缓存，临时文件关闭提示。没有恢复逐页同步。
- 集成纠正：64KiB工作区被固定16项保护缓存挤占，改为小预算4项；真实ORDER BY d,id起初未命中快路径，补上可选主键升序并新增断言。重启内容检查拆成100个有界响应，遵守16MiB协议上限。

## 原生进程：同机单次辅助测量

T_write为100批请求合计（SQL生成不计入）；完整E只使用下一节独立容器项目。B为进程/proc/self/io累计到关闭；RSS为VmHWM，不当作cgroup M。

|分布|版本|预算MiB|T_write s|Exact1000 s|ANN1000 s|建索引s|WAL插入MiB|进程读/写MiB|VmHWM MiB|ANN召回 无过滤/过滤|
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
|clustered|before|20.00|1.153345|0.626363|0.280700|0.219594|5.3487|0.844/13.191|13.641|0.9908/0.9886|
|clustered|after|12.00|0.984260|0.329263|0.188219|0.246970|0.4644|0.848/8.293|13.457|0.9908/0.9886|
|uniform|before|20.00|1.175630|0.665555|0.659600|0.227296|5.3424|0.434/13.188|13.562|1.0000/1.0000|
|uniform|after|12.00|1.005633|0.298816|0.286675|0.245472|0.4643|0.848/8.293|13.500|1.0000/1.0000|

## 独立容器：三轮中位数

2g代表未手工传预算的宽限额容器；16m也不传预算，自动读取cgroup。每个项目独立卷/cgroup。B/M全窗口含启动、装载、索引、预热、查询与关闭写回；T查询仅1000请求，T写入100批墙钟加关闭Checkpoint。采样器不在被测cgroup内。

|分布|容器限额|项目|T_before→after s|B_before→after MiB|M_before→after MiB|
|---|---|---|---:|---:|---:|
|clustered|16m|write|2.750621→2.356289|11.945→7.477|16.000→15.230|
|clustered|16m|exact|3.718983→1.475726|13.527→8.922|16.000→16.000|
|clustered|16m|ann|2.558708→1.283976|13.477→8.918|16.000→16.000|
|clustered|2g|write|2.490537→2.223293|11.938→7.426|20.703→15.000|
|clustered|2g|exact|2.243204→1.636347|13.191→8.688|23.258→17.508|
|clustered|2g|ann|1.730371→1.449984|13.191→8.688|23.234→17.500|
|uniform|16m|write|2.771792→2.505269|11.930→7.477|16.000→15.062|
|uniform|16m|exact|3.632254→1.468886|13.465→8.918|16.000→16.000|
|uniform|16m|ann|3.860476→1.365220|13.457→8.914|16.000→16.000|
|uniform|2g|write|2.577294→2.201793|11.930→7.426|20.488→15.207|
|uniform|2g|exact|2.253593→1.544787|13.188→8.688|23.035→17.535|
|uniform|2g|ann|2.207923→1.556831|13.188→8.688|23.086→17.496|

## 严格按3.3.3的本地效率比

固定参考：各分布/限额下旧版写入与Exact查询各自三轮T/B/M中位数；ε_T=0.001s，ε_B=ε_M=1048576B。f(R)=R/(1+R)，E项目=.6f(RT)+.2f(RB)+.2f(RM)，E=(E写+2E查)/3。以下各效率比为三次比值中位数；E为逐轮计算后中位数，完整逐轮数值见results.json。参考点E=0.5。

|分布|限额|新查询模式|写 R_T/R_B/R_M|查询 R_T/R_B/R_M|E写|E查|E 六位|相对0.5|
|---|---|---|---|---|---:|---:|---:|---:|
|clustered|16m|exact|1.167/1.527/1.047|2.519/1.464/1.000|0.547685|0.648300|0.614761|22.95%|
|clustered|16m|ann|1.167/1.527/1.047|2.895/1.465/1.000|0.547685|0.664888|0.629607|25.92%|
|clustered|2g|exact|1.120/1.535/1.356|1.371/1.465/1.311|0.553246|0.578643|0.572563|14.51%|
|clustered|2g|ann|1.120/1.535/1.356|1.547/1.465/1.311|0.553246|0.596726|0.581881|16.38%|
|uniform|16m|exact|1.106/1.525/1.058|2.472/1.458/1.000|0.538819|0.645846|0.610170|22.03%|
|uniform|16m|ann|1.106/1.525/1.058|2.659/1.459/1.000|0.538819|0.654684|0.616062|23.21%|
|uniform|2g|exact|1.170/1.535/1.326|1.459/1.465/1.297|0.558662|0.587835|0.578669|15.73%|
|uniform|2g|ann|1.170/1.535/1.326|1.447/1.465/1.299|0.558662|0.586700|0.577913|15.58%|

## 正确性与局限

CTest 9/9通过，含20组95SQL、11组extra、动态DML、4/8MiB、四阶段Checkpoint与恢复中再强杀；官方二进制20/20、只读Docker20/20。全部12个优化后写入项目均在另起容器重启后逐字段核对10000条记录及128D float32字节值。PID1 TERM/INT/stop退出0并验证回滚。

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.81 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.14 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    1.39 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    0.88 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    2.52 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    4.89 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   11.44 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  22.20 sec
```

```json
[
  {
    "budget": 4194304,
    "max_request_seconds": 0.41991633200086653,
    "ann_route_counts": {
      "ivf": 1000
    },
    "ann_effective_nprobes": [
      5
    ],
    "recalls": {
      "exact": [
        1.0,
        1.0
      ],
      "ann": [
        0.9907999999999999,
        0.9898
      ]
    },
    "index_below_03s": true,
    "single_sql_below_05s": true,
    "index_seconds": 0.24860426899977028
  },
  {
    "budget": 8388608,
    "max_request_seconds": 0.43509423900104593,
    "ann_route_counts": {
      "ivf": 1000
    },
    "ann_effective_nprobes": [
      5
    ],
    "recalls": {
      "exact": [
        1.0,
        1.0
      ],
      "ann": [
        0.9907999999999999,
        0.9898
      ]
    },
    "index_below_03s": true,
    "single_sql_below_05s": true,
    "index_seconds": 0.24071578400617
  }
]
```

ANN<0.12s、Exact<0.22s是目标，本机原生测量未全部达到；报告保留实际值。聚类ANN仍依靠自动nprobe=5，均匀分布自动Exact回退。未使用外部校准或预存答案。

Docker Desktop单设备同时计启动镜像读；固定holder shell计入M，采样器独立。新数据卷、统一预热、不清宿主全局缓存。有限额容器与宿主调度会影响端到端时间，三轮中位数降低噪声但不消除。不能由本地E换算线上1930.412174或承诺3000+。

追加优化只依据进程SIGKILL模型；已触碰旧文件会保守退回完整WAL。缓存对>25k槽位、高基数和复杂排序回退，因此不保证所有工作负载同等加速。

## 证据JSON校验

```json
{
  "bytes": 117245,
  "sha256": "b61d8ae30fc06d06b0c9744a9018f212f2e48a397f78964ee9d1cf12fed241f0",
  "identical_copies": true,
  "rounds": 8,
  "learning_units": 24,
  "distinct_valid_evidence_pointers": 48,
  "old_40_pointer_values_unchanged": true,
  "old_20_units_unchanged": true,
  "old_7_student_prompts_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "synthetic_true": false,
  "max_evidence_characters": 175
}
```

旧7轮提示、20单元与40条证据原文均保留；新增Round8完整原始提示和公式补充，以及4个独立学习单元。内部生成脚本只留在忽略的work/internal_tools。

## 复现命令

```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/tmp/mvdb-benchmark-venv/bin/python
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
python3 protocol_check/run.py --command '["/absolute/path/to/mvdb_local"]' --output work/protocol-native
docker build -t minivectordb:round8 .
python3 protocol_check/run.py --image minivectordb:round8 --output work/protocol-docker
python3 tests/benchmark_local.py --phase after --automatic-ann --default-budget --binary work/build/mvdb_local --output work/native-after.json
python3 tests/benchmark_cgroup.py --image minivectordb:round8 --phase after --output work/cgroup-after.json --rounds 3
```
