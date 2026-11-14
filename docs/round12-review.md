# Round12 审查修正与完整实测

本轮以c3d5157为审查起点：用纯读主键单调预检选择原始顺序流或已有乱序排序；以有界引号搜索减少额外SQL扫描；主键等值SELECT/UPDATE/DELETE按B+树定位并保持完整谓词、有效位、Meta与Undo语义。按要求撤回自动预热和fast_topk满页分支，fast_topk逐字恢复025。仅推备用仓库，不推比赛仓库。

页差量uint64扫描与双Dockerfile隔离来自7eb/025；短十进制/64B scratch、生产each_set_bit、签名批量筛选、可选预算空簇重播种来自c3。继承与再验证均明确标注，不将八个单元或82个指针当作已获评分。

## 先核对因果与负反馈

自动prewarm发生在create_index返回之后，训练Reservation已经释放；“训练内存直接叠加导致峰值”尚未证明。满32-slot软预取分支只服务Exact/Exact回退，并不位于聚类IVF ANN主路径；不能以其解释所有ANN波动。本轮仍按审查要求撤回两项改动，具体效果以完整对照表为准。

试探式升序前缀写入、遇乱序再Undo的方案已否决：曾出现升序CPU约+11%及晚乱序宽行约2倍写量。最终方案在任何写入前完成纯读单调预检；已升序走原025流，乱序/相等才沿用c3有界排序，SlotId保持输入序。最终晚乱序CPU负结果仍保留，绝不声称“零额外CPU”。

历史100行逻辑叶切换99→4仅是局部性代理。历史gprof79.88%及short_decimal21.55→14.28ms未重采样；本次新增词法微基准与旧数值转换性能分别报告。签名碰撞只通过注入相同存储签名模拟候选假阳性，再完整比较原字符串；不能对损坏签名的不同值查询宣称零漏报。

## 相同主键SQL的三轮A/B

10,000行、每任务每轮100次、3轮版本交错。SELECT/UPDATE/DELETE两版本执行相同pk=7777 SQL；UPDATE/DELETE的BEGIN、ROLLBACK、验证与夹具构造在计时外。pin为逻辑BufferPool请求，pool loads为缓存装载，均非物理磁盘页数。旧c3 SELECT已做tree.find但仍走位图；旧“点查8 vs范围20017 pin”是不同谓词的功能对照，不能冒充旧主键等值性能基线。

|任务|版本|CPU µs/次|wall µs/次|pin/次|pool loads/次|
|---|---|---:|---:|---:|---:|
|select|c3|18.520|18.720|11.000|0.000|
|select|current|3.140|3.352|8.000|0.000|
|update|c3|2037.510|13642.700|20026.000|33.000|
|update|current|114.420|3507.010|19.000|0.000|
|delete|c3|2101.890|15695.800|20030.000|32.000|
|delete|current|168.470|4348.380|23.000|0.000|

负主键、不存在主键、其余AND不匹配、有效位、Meta统计、语句失败和显式事务回滚由生产引擎永久回归覆盖。点查为单元39唯一新工程链；撤回旧缓存/预取仅是负反馈，不另计重复优化链。

## 词法扫描与主键插入反馈

本轮1.28M引号分量PK-only词法CPU中位6.1675→1.0390ms（-83.15%），有界find保留语义；short_decimal数值转换未改，21.55→14.28ms/-33.71%为c3历史。

|插入模式|版本|行数|CPU ms|wall ms|数据页写B|WAL B|Undo spill B|
|---|---|---:|---:|---:|---:|---:|---:|
|ascending|025|4000|30.785|106.443|410872|196562|0|
|ascending|c3|4000|30.231|102.373|410872|196562|0|
|ascending|readonly-current|4000|30.225|113.642|410872|196562|0|
|random|025|4000|33.974|100.939|410872|1.37164e+06|0|
|random|c3|4000|28.579|100.324|410872|196562|0|
|random|readonly-current|4000|28.912|92.795|410872|196562|0|
|late_disorder|025|4000|28.908|86.059|410872|197042|0|
|late_disorder|c3|4000|27.984|86.022|410872|196562|0|
|late_disorder|readonly-current|4000|29.957|101.202|410872|196562|0|
|late_wide|025|800|26.721|36.312|2.4568e+06|55368|0|
|late_wide|c3|800|24.893|33.783|2.4568e+06|55272|0|
|late_wide|readonly-current|800|24.755|39.068|2.4568e+06|55272|0|

最终纯读旁路所有模式数据页写量和WAL与c3相同；CPU、wall有正有负，保留全部模式。被否决前缀方案原始汇总单独保存在results.json的rejected_speculative_prefix字段，不与最终方案混合。

## 全量正确性与预算门槛

本机/容器CTest各9/9；官方二进制/只读Docker各20/20（95 SQL）；冻结R9完整响应4000条相同；4/8/10MiB阈值通过。
本轮legacy默认构建成功且无RUN ctest；显式Dockerfile.test容器CTest9/9通过。

|预算MiB|最大SQL s|全表UPDATE s|ROLLBACK s|
|---|---:|---:|---:|
|4|0.149265|0.114936|0.078837|
|8|0.148117|0.113637|0.077191|
|10|0.153024|0.116996|0.083767|

### ctest

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    1.50 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.81 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    0.72 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.36 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    1.62 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.62 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   11.12 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  20.87 sec
```

### container-ctest

```text
Test project /build
#5 9.950     Start 1: budget_regression
#5 9.953 1/9 Test #1: budget_regression ................   Passed    0.00 sec
#5 9.953     Start 2: storage_regression
#5 11.78 2/9 Test #2: storage_regression ...............   Passed    1.83 sec
#5 11.78     Start 3: core_regression
#5 12.68 3/9 Test #3: core_regression ..................   Passed    0.90 sec
#5 12.68     Start 4: simd_regression
#5 12.69 4/9 Test #4: simd_regression ..................   Passed    0.01 sec
#5 12.69     Start 5: index_regression
#5 13.58 5/9 Test #5: index_regression .................   Passed    0.89 sec
#5 13.58     Start 6: protocol_regression
#5 14.51 6/9 Test #6: protocol_regression ..............   Passed    0.94 sec
#5 14.51     Start 7: all
#5 15.62 7/9 Test #7: all ..............................   Passed    1.11 sec
#5 15.62     Start 8: dynamic_dml
#5 19.00 8/9 Test #8: dynamic_dml ......................   Passed    3.37 sec
#5 19.00     Start 9: low_memory_regression
#5 30.46 9/9 Test #9: low_memory_regression ............   Passed   11.47 sec
#5 30.46
#5 30.46 100% tests passed, 0 tests failed out of 9
#5 30.46
#5 30.46 Total Test time (real) =  20.51 sec
#5 DONE 30.5s

#6 exporting to image
#6 exporting layers
#6 exporting layers 1.2s done
#6 exporting manifest sha256:f69657d003622773df2dbbc7deced14adf356d33fc87718283b058fa2ecf80be done
#6 exporting config sha256:4dbf2addc664d8cd8acb45f59923e8a3e799d5faeb0b3001c14847b13f15abbf done
#6 exporting attestation manifest sha256:ae06508cf2b0d5f74d13598faf630dc640aa6d6699793818bd7b361398ed4951 done
#6 exporting manifest list sha256:10d64c778627dd85a6e0fb4f328e2d5b04c3857120a1b42c202f829568df9039 done
#6 naming to docker.io/library/minivectordb-tests:r12-review done
#6 unpacking to docker.io/library/minivectordb-tests:r12-review
#6 unpacking to docker.io/library/minivectordb-tests:r12-review 0.5s done
#6 DONE 1.8s
```

## 四版本三轮容器生命周期矩阵

本次新采144个样本：R9、025 rollback、c3、review各36。固定10k×128D、1000 Top10及500过滤、相同seed；3轮交错、第二轮逆序；16m/2g、新数据卷、只读根目录、2CPU、禁swap。B为从启动到关闭写回的cgroup io.stat，M为完整生命周期memory.peak，T_write含预编码写请求与关闭，查询T排除装载/建索引/固定预热/关闭。Docker Desktop设备包含镜像启动读，非正式独立/data设备；宿主缓存和调度噪声保留。

|分布|预算|项目|版本|T s|B MiB|M MiB|
|---|---|---|---|---:|---:|---:|
|clustered|16m|write|r9|0.412078|7.180|14.988|
|clustered|16m|write|rollback|0.333932|7.164|14.727|
|clustered|16m|write|c3|0.322708|7.176|15.254|
|clustered|16m|write|review|0.332135|7.164|14.926|
|clustered|16m|exact|r9|0.586782|8.605|16.000|
|clustered|16m|exact|rollback|0.556735|8.594|16.000|
|clustered|16m|exact|c3|0.576357|8.602|16.000|
|clustered|16m|exact|review|0.561847|8.609|16.000|
|clustered|16m|ann|r9|0.426853|8.617|16.000|
|clustered|16m|ann|rollback|0.459352|8.605|16.000|
|clustered|16m|ann|c3|0.440847|8.594|16.000|
|clustered|16m|ann|review|0.430898|8.605|16.000|
|clustered|2g|write|r9|0.449794|7.094|14.727|
|clustered|2g|write|rollback|0.334606|7.094|14.879|
|clustered|2g|write|c3|0.333920|7.094|14.875|
|clustered|2g|write|review|0.325480|7.094|14.941|
|clustered|2g|exact|r9|0.570748|8.355|17.312|
|clustered|2g|exact|rollback|0.575577|8.355|17.320|
|clustered|2g|exact|c3|0.571616|8.355|17.379|
|clustered|2g|exact|review|0.554233|8.355|17.281|
|clustered|2g|ann|r9|0.431290|8.355|17.453|
|clustered|2g|ann|rollback|0.440731|8.355|17.418|
|clustered|2g|ann|c3|0.442949|8.355|17.410|
|clustered|2g|ann|review|0.437739|8.355|17.328|
|uniform|16m|write|r9|0.408119|7.164|14.707|
|uniform|16m|write|rollback|0.372975|7.164|14.750|
|uniform|16m|write|c3|0.336242|7.164|15.043|
|uniform|16m|write|review|0.332668|7.164|14.656|
|uniform|16m|exact|r9|0.548516|8.602|16.000|
|uniform|16m|exact|rollback|0.559773|8.621|16.000|
|uniform|16m|exact|c3|0.550981|8.602|16.000|
|uniform|16m|exact|review|0.565685|8.605|16.000|
|uniform|16m|ann|r9|0.594881|8.617|16.000|
|uniform|16m|ann|rollback|0.563216|8.613|16.000|
|uniform|16m|ann|c3|0.558147|8.605|16.000|
|uniform|16m|ann|review|0.574447|8.602|16.000|
|uniform|2g|write|r9|0.419227|7.094|14.777|
|uniform|2g|write|rollback|0.358152|7.094|14.922|
|uniform|2g|write|c3|0.334580|7.094|14.883|
|uniform|2g|write|review|0.340939|7.094|14.758|
|uniform|2g|exact|r9|0.557557|8.355|17.336|
|uniform|2g|exact|rollback|0.541080|8.355|17.395|
|uniform|2g|exact|c3|0.550194|8.355|17.355|
|uniform|2g|exact|review|0.564226|8.355|17.352|
|uniform|2g|ann|r9|0.557437|8.355|17.340|
|uniform|2g|ann|rollback|0.555112|8.355|17.344|
|uniform|2g|ann|c3|0.557144|8.355|17.367|
|uniform|2g|ann|review|0.563259|8.355|17.340|

每组每项目取3轮T/B/M中位。以下本地E使用本次R9写入/Exact中位作为共同基准：R_T=(Tbase+.001)/(T+.001)，R_B/R_M各平滑1MiB；f(R)=R/(1+R)，E项目=.6f(R_T)+.2f(R_B)+.2f(R_M)，E=(E写+2E查)/3。逐轮先算E，再取3轮中位；ANN亦相对R9 Exact基准。全部样本及比值在results.json，不等于线上正式分数。

|分布|预算|模式|R9 E|025 E|c3 E|review E|
|---|---|---|---:|---:|---:|---:|
|clustered|16m|exact|0.498380|0.510907|0.513878|0.515518|
|clustered|16m|ann|0.531423|0.533326|0.539458|0.541025|
|clustered|2g|exact|0.500765|0.513329|0.514383|0.518819|
|clustered|2g|ann|0.526828|0.539496|0.536653|0.542275|
|uniform|16m|exact|0.500079|0.502663|0.508262|0.507098|
|uniform|16m|ann|0.492137|0.501869|0.506972|0.505112|
|uniform|2g|exact|0.499162|0.507529|0.512194|0.508859|
|uniform|2g|ann|0.500099|0.505386|0.510806|0.508514|

完整中位对照中，review较R9/025/c3的T/B/M存在27个更高单项；该计数包括重复参考对照，详细数值见results.json的negative_comparisons。不能承诺所有T/B/M全面更优，也不承诺2g峰值固定降至17.31MiB或最终评分。

## 进程内CPU诊断（独立口径）

|分布|组别|版本|模式|核心 ms|向量页装载|
|---|---|---|---|---:|---:|
|clustered|all|r9|exact|103.207|0|
|clustered|all|r9|ann|23.054|0|
|clustered|all|rollback|exact|96.453|0|
|clustered|all|rollback|ann|23.950|0|
|clustered|all|c3|exact|93.030|0|
|clustered|all|c3|ann|23.502|0|
|clustered|all|review|exact|103.305|0|
|clustered|all|review|ann|25.920|0|
|clustered|unfiltered|r9|exact|71.061|0|
|clustered|unfiltered|r9|ann|15.170|0|
|clustered|unfiltered|rollback|exact|82.042|0|
|clustered|unfiltered|rollback|ann|15.102|0|
|clustered|unfiltered|c3|exact|67.080|0|
|clustered|unfiltered|c3|ann|15.230|0|
|clustered|unfiltered|review|exact|78.448|0|
|clustered|unfiltered|review|ann|14.773|0|
|clustered|filtered|r9|exact|23.665|0|
|clustered|filtered|r9|ann|7.733|0|
|clustered|filtered|rollback|exact|24.120|0|
|clustered|filtered|rollback|ann|8.350|0|
|clustered|filtered|c3|exact|22.837|0|
|clustered|filtered|c3|ann|7.845|0|
|clustered|filtered|review|exact|23.160|0|
|clustered|filtered|review|ann|7.849|0|
|uniform|all|r9|exact|95.830|0|
|uniform|all|r9|ann|104.295|0|
|uniform|all|rollback|exact|106.019|0|
|uniform|all|rollback|ann|100.315|0|
|uniform|all|c3|exact|96.185|0|
|uniform|all|c3|ann|89.721|0|
|uniform|all|review|exact|99.571|0|
|uniform|all|review|ann|97.934|0|
|uniform|unfiltered|r9|exact|82.729|0|
|uniform|unfiltered|r9|ann|81.644|0|
|uniform|unfiltered|rollback|exact|81.716|0|
|uniform|unfiltered|rollback|ann|71.900|0|
|uniform|unfiltered|c3|exact|70.192|0|
|uniform|unfiltered|c3|ann|66.768|0|
|uniform|unfiltered|review|exact|75.550|0|
|uniform|unfiltered|review|ann|81.624|0|
|uniform|filtered|r9|exact|24.802|0|
|uniform|filtered|r9|ann|23.570|0|
|uniform|filtered|rollback|exact|23.318|0|
|uniform|filtered|rollback|ann|23.256|0|
|uniform|filtered|c3|exact|27.287|0|
|uniform|filtered|c3|ann|24.749|0|
|uniform|filtered|review|exact|23.264|0|
|uniform|filtered|review|ann|23.672|0|

原生before(c3)/after(review)全部原始记录也保留在results.json；原生wall包含驱动交互，不能与容器预编码请求T混为同一口径；进程VmHWM不等于cgroup memory.peak。

## 生产落点与证据保真

永久回归薄接口调用实际内部delta；Storage.write、redo批次与逆向Undo共用同一未改生产编码器。fast_topk与025函数文本相同，internal delta与c3函数文本相同。R9持久化布局、WAL/Undo/恢复、SlotId顺序及缓存策略契约保持；不启用墓碑槽位复用，无新页格式或绕过BufferPool。

each_set_bit生产落点（包含声明；Engine中无bitmap_next调用）：

- `src/engine.cpp:29`：`template<class F>void each_set_bit(const std::array<uint64_t,64>& bits,size_t begin,size_t end,F&& visit){`
- `src/engine.cpp:624`：`each_set_bit(candidates,part,limit,[&](size_t at){`
- `src/engine.cpp:647`：`each_set_bit(candidates,0,end,[&](size_t at){`
- `src/engine.cpp:725`：`each_set_bit(candidates,0,end,[&](size_t at){auto slot=static_cast<SlotId>(start+at);auto mask=uint64_t(1)<<(at%64);auto& word=candidates[at/64];`
- `src/engine.cpp:729`：`each_set_bit(candidates,0,end,[&](size_t at){auto slot=static_cast<SlotId>(start+at);`

稀疏next历史组件倍率不外推到端到端；生产稠密扫描每word加载一次ctz枚举。空簇工作区最多64个最远样本序号且预算可选，64KiB守卫保留；完全重合点不可能制造不同非空欧氏簇，COSINE/IP仍保持明确拒绝。

前33对象、66引用值以及证据句前后相邻完整句同时与c3/025核对；最新完整用户审查为conversation索引11的student_followup，另记最终“只push到备用仓库”修正。历史仅裁剪未引用内容，双JSON使用同一UTF8字节串。

```json
{
  "bytes": 122569,
  "sha256": "0dda6f48015eb99e510a946a364516e13f58069155b1eb32f0e249a2fd87e53a",
  "learning_units": 41,
  "unique_valid_pointers": 82,
  "old33_objects_identical": true,
  "old66_values_and_adjacent_sentences_unchanged": true,
  "latest_review_verbatim": true,
  "round12_conversation_index": 11,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "push_scope": "backup only",
  "old33_and_66_also_identical_to_025": true
}
```
