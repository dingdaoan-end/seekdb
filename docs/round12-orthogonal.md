# Round 12：R9格式兼容的子系统优化与验证

基于025c429落实短十进制/64B完整回退、128项主键排序及512项借用视图单遍复用、输入SlotId保序、满页连续扫描和提交后元数据预热、签名字级初筛与碰撞复核、预算可选的空簇重播种、64位有效行枚举。修正64KiB/宽行预算回归；首轮写入回退后移除第二遍SQL扫描再复测。差量扫描和Docker隔离注明继承，非本轮首次实现。

本轮最终仅推备用仓库；比赛仓库保持不动。未承诺所有链都首次实现或评分达到2.0；页差量uint64和双Dockerfile来自Safe，新增永久回归并再次验证。

## 实现、修正与边界

- SQL：<=9位普通十进制走精确整数/双精度十进制除法，保留R9 double→float舍入；e/E、hex、极值完整回退。64字节scratch绝不截断长token。
- PK：128项有界键/原序号排序，先查重再有序访问B+树，数据槽位保持输入顺序。首轮双扫描写入回退后改为最多512个借用字面量视图复用，128行/512项边界和超限回退验证通过；不复制向量内容。迟到错误由同一Savepoint撤销索引和已写行。审查发现可选2KiB保留到行转换导致宽行准入退化，现已提前释放；262144B/14500字符回归通过。
- 查询：CREATE INDEX自动提交后预热；显式事务推迟至COMMIT，失败可选缓存不反转提交。构建原R9字典需要.rows/.blob和质心/桶元数据；不虚称只读150KiB。.v0预热读取严格0。满32槽连续读取无新位图/向量副本，空洞/尾页/缺页保留原路径。
- 签名：完整64位FNV，实际字典上限128值；UPDATE/DELETE扩展字级初筛并始终全值复核。碰撞通过故障注入.s制造同签名，未声称找到真实FNV碰撞。
- 训练：既有遍历内保存最多64个最远样本序号，空簇才回读样本；0残差不可产生新的不同簇。新增1280B工作区可选；首轮挤掉64KiB建索引的回归已修复，预算不足保留原训练。L2支持，COSINE/IP继续明确拒绝；没有虚构三度量召回率。
- 位图：有限范围next0/next1及guard-page测试；密集扫描采用每字一次加载+ctz，而不重复next。默认不复用历史墓碑SlotId，避免改变R9追加/WAL行为。Header/Meta/PK Node布局、delta记录、WAL/Undo/Shadow/恢复流程未改，无数据fadvise、新索引格式或绕过BufferPool。

## 专项证据

128万分量CPU中位 21.5465→14.2835ms（-33.71%）；短hex128分量C++分配129→1，常规十进制仍只有输出vector分配。2400+差分及ASAN/UBSAN通过。
最终持久树的100个主键访问轨迹，叶切换99→4，100行SlotId映射不变。该指标是最终树的逻辑局部性代理，不是pin次数或物理B。
同引擎5120次驻留Exact：control0.631862s→fullpage0.607002s；首次负结果0.624221→0.638587s保留，不能选择性忽略波动。45,176组实际delta与R9独立逐字节oracle正反相同；4,375,040组位图边界对照通过。
空/稀疏next组件约19.4x/16.8x，但稠密重复next为原字节扫描2.46倍耗时，故执行器选择word-at-once；未将组件倍率外推到端到端。

## 全套回归

本机9/9，容器9/9；官方二进制和只读Docker各20/20（95 SQL）；legacy默认镜像构建成功，未执行CTest；冻结R9对拍4000完整响应相同。旧四阶段Checkpoint/恢复中二次强杀和追加窗口测试未削减。
4/8/10MiB均保留单SQL<0.5s、索引<0.3s门槛。

|预算MiB|最大SQL s|全表UPDATE s|ROLLBACK s|
|---|---:|---:|---:|
|4|0.151520|0.115311|0.073669|
|8|0.150085|0.115663|0.076331|
|10|0.152291|0.117453|0.075906|

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    1.53 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.50 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    0.71 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.22 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    1.95 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.75 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   11.65 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  21.45 sec
```

## 三代三轮容器生命周期实测

固定10k×128D/1000 Top10/500过滤、相同随机种子，三轮版本交错且第二轮反序，16m/2g独立只读无网络容器与新卷、2CPU禁swap。B/M从启动、装载、索引准备、预热到关闭写回；内容/重启检查在独立事后容器。Docker Desktop可见设备io.stat并非正式专用/data盘，宿主共享缓存与调度造成噪声。

|分布|内存|项目|版本|T s|B MiB|M MiB|
|---|---|---|---|---:|---:|---:|
|clustered|16m|write|r9|0.417453|7.164|14.863|
|clustered|16m|write|rollback|0.347810|7.164|14.738|
|clustered|16m|write|r12|0.342961|7.191|14.984|
|clustered|16m|exact|r9|0.580752|8.602|16.000|
|clustered|16m|exact|rollback|0.583130|8.625|16.000|
|clustered|16m|exact|r12|0.597440|8.609|16.000|
|clustered|16m|ann|r9|0.445732|8.605|16.000|
|clustered|16m|ann|rollback|0.443863|8.613|16.000|
|clustered|16m|ann|r12|0.444141|8.617|16.000|
|clustered|2g|write|r9|0.422328|7.094|14.879|
|clustered|2g|write|rollback|0.345023|7.094|14.766|
|clustered|2g|write|r12|0.338618|7.094|14.801|
|clustered|2g|exact|r9|0.591962|8.355|17.316|
|clustered|2g|exact|rollback|0.581892|8.355|17.320|
|clustered|2g|exact|r12|0.580608|8.355|17.340|
|clustered|2g|ann|r9|0.446839|8.355|17.414|
|clustered|2g|ann|rollback|0.445721|8.355|17.312|
|clustered|2g|ann|r12|0.447919|8.355|17.426|
|uniform|16m|write|r9|0.422246|7.160|15.047|
|uniform|16m|write|rollback|0.348402|7.164|15.043|
|uniform|16m|write|r12|0.362863|7.164|14.867|
|uniform|16m|exact|r9|0.574420|8.605|16.000|
|uniform|16m|exact|rollback|0.590491|8.637|16.000|
|uniform|16m|exact|r12|0.598327|8.602|16.000|
|uniform|16m|ann|r9|0.593206|8.609|16.000|
|uniform|16m|ann|rollback|0.603611|8.602|16.000|
|uniform|16m|ann|r12|0.592422|8.605|16.000|
|uniform|2g|write|r9|0.430651|7.094|14.871|
|uniform|2g|write|rollback|0.358147|7.094|14.914|
|uniform|2g|write|r12|0.363412|7.094|14.855|
|uniform|2g|exact|r9|0.580210|8.355|17.266|
|uniform|2g|exact|rollback|0.582221|8.355|17.305|
|uniform|2g|exact|r12|0.579637|8.355|17.469|
|uniform|2g|ann|r9|0.590614|8.355|17.348|
|uniform|2g|ann|rollback|0.584939|8.355|17.379|
|uniform|2g|ann|r12|0.587962|8.355|17.570|

每项目以本轮R9三轮中位为本地基线，R_T=(Tbase+.001)/(T+.001)，R_B/R_M平滑1MiB；f(R)=R/(1+R)，E项目=.6f(R_T)+.2f(R_B)+.2f(R_M)，E=(E写+2E查)/3。下面对三轮E取中位，原始各R/E全在results.json，不等同线上原始指标。

|分布|内存|模式|R9 E|025 Rollback E|R12 E|
|---|---|---|---:|---:|---:|
|clustered|16m|exact|0.499986|0.506527|0.507247|
|clustered|16m|ann|0.525382|0.532484|0.535444|
|clustered|2g|exact|0.498158|0.510958|0.512973|
|clustered|2g|ann|0.528505|0.537266|0.538847|
|uniform|16m|exact|0.499512|0.507626|0.505654|
|uniform|16m|ann|0.496201|0.504116|0.504526|
|uniform|2g|exact|0.500000|0.508398|0.507553|
|uniform|2g|ann|0.498084|0.508223|0.506154|

原生单轮负结果保留：写入wall包含原本驱动生成/交互，不能等同容器预编码T；原生部分Exact/写入退化，不能只挑CPU指标宣称全链加速。B/M受测量噪声影响不承诺逐字相等，是否达到用户期望须按完整表判断；不预测线上分数。

## 进程内诊断

|分布|组别|版本|模式|核心ms|测量期向量缺页|
|---|---|---|---|---:|---:|
|clustered|all|r9|exact|107.481|0|
|clustered|all|r9|ann|24.955|0|
|clustered|all|rollback|exact|107.842|0|
|clustered|all|rollback|ann|24.380|0|
|clustered|all|r12|exact|95.896|0|
|clustered|all|r12|ann|27.335|0|
|clustered|unfiltered|r9|exact|71.537|0|
|clustered|unfiltered|r9|ann|17.959|0|
|clustered|unfiltered|rollback|exact|78.934|0|
|clustered|unfiltered|rollback|ann|16.454|0|
|clustered|unfiltered|r12|exact|72.072|0|
|clustered|unfiltered|r12|ann|15.756|0|
|clustered|filtered|r9|exact|32.036|0|
|clustered|filtered|r9|ann|8.607|0|
|clustered|filtered|rollback|exact|26.070|0|
|clustered|filtered|rollback|ann|9.058|0|
|clustered|filtered|r12|exact|24.452|0|
|clustered|filtered|r12|ann|9.414|0|
|uniform|all|r9|exact|98.453|0|
|uniform|all|r9|ann|109.277|0|
|uniform|all|rollback|exact|110.388|0|
|uniform|all|rollback|ann|112.690|0|
|uniform|all|r12|exact|103.309|0|
|uniform|all|r12|ann|97.106|0|
|uniform|unfiltered|r9|exact|75.641|0|
|uniform|unfiltered|r9|ann|79.545|0|
|uniform|unfiltered|rollback|exact|74.847|0|
|uniform|unfiltered|rollback|ann|79.469|0|
|uniform|unfiltered|r12|exact|71.365|0|
|uniform|unfiltered|r12|ann|75.232|0|
|uniform|filtered|r9|exact|27.324|0|
|uniform|filtered|r9|ann|26.464|0|
|uniform|filtered|rollback|exact|25.484|0|
|uniform|filtered|rollback|ann|24.727|0|
|uniform|filtered|r12|exact|29.405|0|
|uniform|filtered|r12|ann|25.167|0|

## 证据文件保真

{
  "bytes": 117884,
  "sha256": "d8f891e82e406af6ad05a21432b0a4328af9ada3f1fe54548c3a7967a5f87397",
  "learning_units": 41,
  "unique_valid_pointers": 82,
  "old33_objects_identical": true,
  "old66_values_and_adjacent_sentences_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "push_scope": "backup only"
}

前33单元对象整体未改；66个引用值和紧邻前后完整句保持原文。历史未引用长段落节录并注明git:025c429完整来源，新增Round12使用本轮真实完整用户提示。34/35明确继承，40明确未扩展COSINE/IP，41明确不启用墓碑复用；不能把目标2.0或82指针当作已获评分。

## 复现

```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
DOCKER_BUILDKIT=0 docker build -t minivectordb:r12-legacy .
docker build --target build -t minivectordb-build:r12 .
docker build -f Dockerfile.test --build-arg BUILD_IMAGE=minivectordb-build:r12 --target test .
python3 protocol_check/run.py --command '["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]' --output work/r12-protocol-native
python3 protocol_check/run.py --image minivectordb:r12 --output work/r12-protocol-docker
python3 tests/benchmark_cgroup.py --image minivectordb:r12 --phase r12 --rounds 3 --min-ann-recall .985 --output work/r12-cgroup.json
```
