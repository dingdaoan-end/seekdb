# Round11验证、性能与约束复盘

保留持久化不变量，加入32/64维证书剪枝、最近桶预热及标准WAL紧凑桶；负反馈后优化归约、缓存相同堆顶阈值，对均匀/小扫描回退。全局驻留条件不满足时不交接缓存，未宣称45%内存降幅。

用户反馈7eb530c线上全部准入通过、Prompt13.5、原始性能3219.571598；未独立验证，不将本地公式E映射到线上原始指标或预测分数。
版本：R7=fe90005，R8=9c8798a，R9=a4c5bf5，R10-Safe=7eb530c，R11为本报告随附提交；完整旧SHA在results.json，新SHA在delivery.json。

## 核心结论与安全界限

10k工作文件逻辑总量6815744B，实际页池7651328B，85%门槛6503628B；换页0次、静止交接0次。严格条件未成立，45%峰值下降目标未达到。释放当前PageCache不能擦除已发生的memory.peak。
标准BufferPool/WAL下.cidx.r*/.lists按2KiB桶块共享页，所有写入仍走普通write/write_new及Redo；不采用冷页直写、免WAL、读循环fadvise。旧.index/.ivf可读；旧二进制不能读新.cidx，回退须用R9备份。偏斜/小桶数可有逻辑稀疏孔，驻留证明会保守拒绝。
单纯1.000002f不能代替支持范围内的误差证明；实现使用更保守前缀下界、向上舍入阈值与float64潜在赢家复算。Exact只在全驻留且新鲜选择性校准时用最近桶预热，再去重顺序扫余槽；均匀宽探测成本回退禁用剪枝。
首版高剪枝率仍变慢，已保留first_negative_cpu；后续优化证书/四通道归约，并按堆顶缓存相同阈值。少于1024预期候选回原批核，默认5桶ANN通常不启用剪枝；大探测集迁移用额外NPROBE16诊断验证，不混入默认评分负载。
R9/Safe主基线采用三轮交错窗口；R11末次纯CPU优化后重新独立采三轮，旧分版本/交错窗口均归档。宿主漂移仍可能影响微小差异，不把全部波动归因代码。

## 五代生命周期T/B/M三轮中位数

R7/R8 use earlier same-machine archived3-run window, not rerun; compiler/runtime/cache windows may differ. Fresh R9/R10Safe/R11 are the primary controlled comparison.
Fresh R9 write/exact3-run medians; epsT=.001s,epsB=epsM=1MiB; f(R)=R/(1+R),Ewrite/Equery=.6fRT+.2fRB+.2fRM,E=(Ewrite+2Equery)/3;median3 thenround6.

|分布|内存|项目|版本|T s|B MiB|M MiB|
|---|---|---|---|---:|---:|---:|
|clustered|16m|write|r7|1.585505|11.953|16.000|
|clustered|16m|write|r8|1.314830|7.477|14.992|
|clustered|16m|write|r9|0.409472|7.176|14.777|
|clustered|16m|write|r10safe|0.322517|7.164|14.703|
|clustered|16m|write|r11|0.319931|7.168|14.750|
|clustered|16m|exact|r7|2.023373|13.477|16.000|
|clustered|16m|exact|r8|0.595345|8.914|16.000|
|clustered|16m|exact|r9|0.532871|8.602|16.000|
|clustered|16m|exact|r10safe|0.518637|8.605|16.000|
|clustered|16m|exact|r11|0.536031|7.574|15.496|
|clustered|16m|ann|r7|1.211122|13.492|16.000|
|clustered|16m|ann|r8|0.477410|8.922|16.000|
|clustered|16m|ann|r9|0.421056|8.605|16.000|
|clustered|16m|ann|r10safe|0.420155|8.617|16.000|
|clustered|16m|ann|r11|0.425777|7.578|15.262|
|clustered|2g|write|r7|1.465330|11.938|20.641|
|clustered|2g|write|r8|1.191303|7.426|15.066|
|clustered|2g|write|r9|0.412112|7.094|14.906|
|clustered|2g|write|r10safe|0.321274|7.094|14.809|
|clustered|2g|write|r11|0.328504|7.094|14.812|
|clustered|2g|exact|r7|1.071073|13.191|23.484|
|clustered|2g|exact|r8|0.588208|8.688|17.500|
|clustered|2g|exact|r9|0.566133|8.355|17.309|
|clustered|2g|exact|r10safe|0.531914|8.355|17.336|
|clustered|2g|exact|r11|0.532199|7.504|15.531|
|clustered|2g|ann|r7|0.639637|13.191|23.457|
|clustered|2g|ann|r8|0.468761|8.688|17.500|
|clustered|2g|ann|r9|0.429072|8.355|17.156|
|clustered|2g|ann|r10safe|0.417724|8.355|17.551|
|clustered|2g|ann|r11|0.427907|7.504|15.309|
|uniform|16m|write|r7|1.666429|11.938|16.000|
|uniform|16m|write|r8|1.377108|7.473|15.133|
|uniform|16m|write|r9|0.398909|7.160|14.980|
|uniform|16m|write|r10safe|0.324158|7.168|14.961|
|uniform|16m|write|r11|0.334094|7.156|15.000|
|uniform|16m|exact|r7|1.887108|13.480|16.000|
|uniform|16m|exact|r8|0.608431|8.930|16.000|
|uniform|16m|exact|r9|0.551806|8.605|16.000|
|uniform|16m|exact|r10safe|0.531323|8.633|16.000|
|uniform|16m|exact|r11|0.520472|7.547|15.273|
|uniform|16m|ann|r7|1.915334|13.465|16.000|
|uniform|16m|ann|r8|0.595829|8.914|16.000|
|uniform|16m|ann|r9|0.551912|8.609|16.000|
|uniform|16m|ann|r10safe|0.532429|8.602|16.000|
|uniform|16m|ann|r11|0.513174|7.566|15.422|
|uniform|2g|write|r7|1.467417|11.930|20.418|
|uniform|2g|write|r8|1.183758|7.426|15.094|
|uniform|2g|write|r9|0.404111|7.094|14.941|
|uniform|2g|write|r10safe|0.333196|7.094|14.754|
|uniform|2g|write|r11|0.327977|7.094|14.793|
|uniform|2g|exact|r7|1.068422|13.188|22.961|
|uniform|2g|exact|r8|0.590035|8.688|17.594|
|uniform|2g|exact|r9|0.544182|8.355|17.473|
|uniform|2g|exact|r10safe|0.535995|8.355|17.340|
|uniform|2g|exact|r11|0.519671|7.473|15.289|
|uniform|2g|ann|r7|1.075595|13.188|23.008|
|uniform|2g|ann|r8|0.586605|8.688|17.379|
|uniform|2g|ann|r9|0.547669|8.355|17.438|
|uniform|2g|ann|r10safe|0.544974|8.355|17.391|
|uniform|2g|ann|r11|0.509639|7.473|15.344|

## 本地效率比E

R9三轮中位基线点E=0.5；逐轮E中位可因多指标相关性略偏离。完整RT/RB/RM、Ewrite/Equery每轮在results.json。

|分布|内存|模式|R7档案|R8档案|R9|R10Safe|R11|相对Safe|
|---|---|---|---:|---:|---:|---:|---:|---:|
|clustered|16m|exact|0.301642|0.433478|0.496980|0.515943|0.516610|+0.13%|
|clustered|16m|ann|0.342749|0.457899|0.523454|0.533585|0.539832|+1.17%|
|clustered|2g|exact|0.346406|0.445539|0.499858|0.517687|0.523578|+1.14%|
|clustered|2g|ann|0.395480|0.468235|0.523569|0.538940|0.545748|+1.26%|
|uniform|16m|exact|0.306789|0.432658|0.499796|0.514891|0.519550|+0.90%|
|uniform|16m|ann|0.306408|0.436609|0.499804|0.513669|0.518158|+0.87%|
|uniform|2g|exact|0.343506|0.441238|0.500380|0.510862|0.522867|+2.35%|
|uniform|2g|ann|0.342292|0.442031|0.499426|0.509591|0.524137|+2.85%|

## 本地端到端补充（独立单轮）

benchmark_local.py在CREATE VECTOR INDEX之前测Exact，因此不具备最近桶预热/宽探测校准回退条件，与上方建索引后容器/CPU负载分开解释。单轮不能替代三轮中位，保留负向结果。

|分布|版本|100批写入请求 s|1000 Exact s|1000 ANN s|
|---|---|---:|---:|---:|
|clustered|before|0.291866|0.206079|0.138420|
|clustered|after|0.317708|0.207678|0.132735|
|uniform|before|0.313957|0.207490|0.208869|
|uniform|after|0.296106|0.222223|0.200602|

均匀未建索引Exact由0.207490s增至0.222223s（约7.1%）；没有校准信息时仍可能启用收益不足的剪枝，属于后续成本模型改进空间。本轮保留已验证实现并明确记录，未宣称所有查询均加速。

## 核心耗时与实际剪枝率

每次诊断预热1遍、计时5遍平均。剪枝率分母为阈值有限并实际进入剪枝核的候选，32/64为分别在该阶段新增拒绝；均匀策略禁用时0试次不表示算法测得0%。

|分布|组|模式|Safe ms|R11 ms|32新增%|64新增%|末条策略|
|---|---|---|---:|---:|---:|---:|---|
|clustered|all|exact|86.467|86.218|93.8487|0.0061|small_scan|
|clustered|all|ann|21.658|22.237|54.9332|0.0496|small_scan|
|clustered|unfiltered|exact|74.299|68.596|93.8487|0.0061|enabled|
|clustered|unfiltered|ann|13.711|14.502|54.9332|0.0496|small_scan|
|clustered|filtered|exact|11.985|13.079|0.0000|0.0000|small_scan|
|clustered|filtered|ann|8.200|8.389|0.0000|0.0000|small_scan|
|uniform|all|exact|86.187|79.986|0.0000|0.0000|unselective_index|
|uniform|all|ann|87.167|80.530|0.0000|0.0000|unselective_index|
|uniform|unfiltered|exact|72.415|63.131|0.0000|0.0000|unselective_index|
|uniform|unfiltered|ann|73.451|67.286|0.0000|0.0000|unselective_index|
|uniform|filtered|exact|11.723|11.743|0.0000|0.0000|unselective_index|
|uniform|filtered|ann|12.804|12.623|0.0000|0.0000|unselective_index|

大探测集诊断（500条聚类无过滤查询+1条SET NPROBE16，每遍501语句，排除正式负载）：
```json
[
  {
    "queries": 501,
    "mode": "ann",
    "parse_seconds": 0.000801417,
    "engine_seconds": 0.0283497,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "checksum": 149955902,
    "phase": "before",
    "diagnostic_only": "500clustered unfiltered queries +1SET NPROBE16 per pass; excluded from default benchmark scores",
    "full_query_responses_equal": 500,
    "heldout_recall": 1.0
  },
  {
    "queries": 501,
    "mode": "ann",
    "parse_seconds": 0.000743798,
    "engine_seconds": 0.0226883,
    "vector_page_loads_measured": 0,
    "budget_bytes": 10485760,
    "last_query_pruning_policy": "enabled",
    "pruning_trials": 6833005,
    "pruned32": 5299435,
    "pruned64": 1665,
    "pruned32_rate": 0.775564,
    "pruned64_rate": 0.00024367,
    "checksum": 149955902,
    "phase": "after",
    "diagnostic_only": "500clustered unfiltered queries +1SET NPROBE16 per pass; excluded from default benchmark scores",
    "full_query_responses_equal": 500,
    "heldout_recall": 1.0
  }
]
```
最后一条策略只描述末条语句，混合1000查询的累计剪枝次数可能包含早先启用的无过滤查询。

## 回归与崩溃验证

本机CTest9/9、容器CTest9/9；官方二进制/只读Docker各20/20、公开95条SQL；冻结R9与R11在聚类/均匀、Exact/ANN共4000完整响应逐行一致。
紧凑索引专项在64KiB/1MiB预算下，四阶段Checkpoint各配恢复中途连续两次SIGKILL，共24次预定强杀，全部键/向量/ANN64恢复校验通过；还覆盖跨508/512桶槽增长、删除、回滚和旧格式读取。
全局交接测试覆盖小表全驻留正例、缺失页/未打开元数据/未对齐/脏页/已换页/小预算反例；4个4MiB文件在8.8/10MiB交错读取及Checkpoint数据DONTNEED严格0。
```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.95 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.36 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.05 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    3.38 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.10 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    1.77 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.51 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   10.31 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  21.67 sec
```

容器测试日志保存在r11-container-ctest.log；这些本地检查不能替代后续隐藏种子正式评测。

## 复现与统计口径

```sh
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
DOCKER_BUILDKIT=0 docker build -t minivectordb:r11-legacy .
docker build -t minivectordb:r11 .
docker build --target build -t minivectordb-build:r11 .
docker build -f Dockerfile.test --build-arg BUILD_IMAGE=minivectordb-build:r11 --target test .
python3 protocol_check/run.py --command '["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]' --output work/r11-protocol-native
python3 protocol_check/run.py --image minivectordb:r11 --output work/r11-protocol-docker
python3 tests/benchmark_cgroup.py --image minivectordb:r11 --phase r11 --output work/r11-cgroup-r11.json --rounds 3 --min-ann-recall .985
```
三代新测使用同一LinuxDockerCLI/socket、2CPU、16m/2g独立只读容器及新卷、禁swap；B/M覆盖启动/装载/建索引/预热/关闭写回，T查询排除准备；写入重启全量核验在独立事后容器。未清全局缓存，Desktop共享镜像缓存和宿主负载仍是局限。

## 证据保真

旧31单元完整对象、62条引用目标及锚点相邻整段上下文均不变。仅删历史未引用提示段落和验证冗余，摘录已标注、原文可从Git7eb530c恢复；Round11完整提示保留，未伪造结果或声称已获2分。

```json
{
  "bytes": 123803,
  "sha256": "1900425c5b98d2639254e97621fb1a504b80614444739694e5757c55e5a765e7",
  "units": 33,
  "unique_valid_pointers": 66,
  "old31_units_and62_values_context_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0
}
```
