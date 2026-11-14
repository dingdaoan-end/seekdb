# Round11回退至R9与协议输出验证

按用户线上回执撤回R10/R11计算与持久化实验，源文件恢复R9，仅保留Safe等价字节扫描和协议输出优化。核实R9原已用to_chars，未把408ms全归序列化。常规组装零C++分配；初版逐条屏蔽信号反而变慢，改为小阻塞管道原子重试并以填满管道、连续信号、故障变体验证。独立hint对照证明条件性跨阶段重读，不宣称已证实线上唯一根因或必然涨分。

用户回执：R9 3258.709224、Safe3219.571598、R11p2655.486243，Prompt13.5；本仓库没有独立抓取平台原始T/B/M，因此不声称已证实所有线上归因，也不预测新分数。

## 交付边界

src目录按R9 a4c5bf5恢复，包括删除R10新增simd.hpp；engine.cpp/.hpp、simd.cpp、sql.cpp/.hpp、预算和JSON解析器逐文件与R9相同。storage.cpp与Safe7eb530c相同，整文件差异仅uint64相等区间扫描；45176组字节/正反重放对照通过。唯一其他源文件改动是local_adapter.cpp。
R10/R11的字典倒排子集、batch4、32/64维剪枝、半径缓存、.cidx紧凑页和新增静止交接均撤回；R9固有Checkpoint fsync/fadvise、BufferPool/WAL/Undo和物理SlotId顺序保留。Dockerfile/Dockerfile.test与Safe相同：默认不执行测试，显式独立测试镜像。
当前版本不能直接接续R10/R11新格式索引目录；请使用R9备份或新卷重新装载。退休的紧凑索引/半径专项测试归档Gitcb498ad和忽略的work目录，核心测试恢复R9基线，新增协议专项合入同一个protocol_regression，CTest目标仍9项。

## 因果取证及局限

R11p同一冻结二进制，LD_PRELOAD仅屏蔽data文件DONTNEED，其他逻辑不变；Linux/tmp、10k×128D、10MiB引擎预算，受控诊断不计入正式基准。单短VARCHAR总文件6373376B、帧池7651328B、85%门槛6503628B，交接1/换页0。首次热查询没有新增向量取页；采样窗口一度出现131072B块读但只有一次stdin read，存在上一阶段预读/归属延迟，不能算作向量查询重新读盘。
后续压力表3000行后，两臂用户态取页请求相同而块读5750784→0B；重启对照见下表。这证明历史提示可能让后续失去驻留时缺少内核备份，不能证明固定工作集、零换页的热查询必然退化，更不能据此唯一解释线上成绩。
两个短VARCHAR和空VARCHAR反例：初看磁盘文件约5.9MB却未交接；刷脏后原表分别6520832B/6504448B，后者仍比门槛高820B。应比较含脏尾部的完整全局工作集，不能仅看未同步文件大小。门槛6503628B约6.20MiB，不是6.50MiB。

|场景|抑制数据hint|初始交接|查询窗口块读取B|显式read调用数|
|---|---|---:|---:|---:|
|pressure|False|1|5750784|353|
|pressure|True|1|0|353|
|restart|False|1|5836800|355|
|restart|True|1|53248|355|

## 协议边界、分配与往返

R9原已复用响应字符串、采用GENERAL17 to_chars；本次去除的是每单元格mvdb::json/quote临时对象及字符串再次解析。直接转义同时验证UTF8，保持旧数值文本精度和原始id类型/文本。16KiB预留、64KiB stdout缓冲、包含换行的一次正常fwrite+fflush；stdin保留原64KiB read/poll，避免stdio预读使poll漏看缓冲数据。
常规预热响应组装的C++operator new计数为0；请求解析、内核、Result构建不在此承诺内。超过256KiB容量按R9策略在下次组装clear时释放，超大响应仍可有界扩容。初版全部屏蔽信号使ping变慢，负结果未删。最终仅对确认FIFO/阻塞、空stdio缓冲且记录<=PIPE_BUF（最多4096B）的输出用原子EINTR整条重试；大记录/其他FD仍完整flush期间暂缓TERM/INT。
依据：[Linux pipe(7)](https://man7.org/linux/man-pages/man7/pipe.7.html)、[write(2)](https://man7.org/linux/man-pages/man2/write.2.html)。实现限Ubuntu/glibc，__fpurge用于丢弃被中断的stdio副本；不保证永不读取的对端限时退出。首次新测试因65536B管道在65280B处页尾碎片耗尽槽位而误判，已改为近满检查加/proc/wchan确认pipe_write。连续TERM/INT下所有已输出JSON为无缺失/重复的ID前缀，事务回滚正确；删除EINTR重试的故障变体被相同测试拒绝。
输出写失败/dev/full以3退出，不等stdin；保留EOF、超大id、跨16K/64K边界和UTF8分片、20k有限double逐字节格式对照。超过公开16MiB响应能力的id无法同时有限输出及完整回显，不作为无限输入承诺。

|组装案例|字节|次数|R9 C++分配|当前 C++分配|R9 ms|当前 ms|
|---|---:|---:|---:|---:|---:|---:|
|topk10|450|1000|22000|0|0.926896|0.691140|
|affected_rows|87|1000|4000|0|0.062952|0.072437|
|multirow_vectors|89780|1000|356000|0|320.668000|306.061000|
|large_warmed_response|448484|1000|1776000|6000|1614.860000|1544.780000|
|legacy_topk10|447|1000|21000|0|0.927178|0.799981|
|legacy_empty|52|1000|1000|0|0.039929|0.026033|

以上为预建Result的组装微基准，不含解析、引擎、管道；不能将408/440ms全部归因文本序列化。

|通道1000次ping|R9 s|R11p s|回退版 s|
|---|---:|---:|---:|
|native|0.035903|0.038470|0.029178|
|docker|0.342761|0.345150|0.341446|

ping三轮交错、含驱动检查、不含启动/20次预热/关闭；Docker与组装微基准差异不是纯序列化时间。初版全屏蔽负结果和全部样本在results.json。

## 三代生命周期三轮中位数

Fresh R9 write/exact median reference; epsT=.001s,epsB=epsM=1MiB;f(R)=R/(1+R),Eproject=.6fRT+.2fRB+.2fRM,E=(Ewrite+2Equery)/3;median3 E. Not online raw score.
三轮按版本交错、第二轮反序；相同负载/种子/2CPU，16m和2g独立只读无网络容器及新卷、禁swap。B/M包含启动、装载、索引、预热、关闭写回；持久化内容检查用事后独立cgroup。DockerDesktop共享缓存及可见块设备统计并非正式专用数据盘，微小差异不应全归因代码。

|分布|内存|项目|版本|T s|B MiB|M MiB|
|---|---|---|---|---:|---:|---:|
|clustered|16m|write|r9|0.419060|7.164|14.789|
|clustered|16m|write|r11p|0.340084|7.156|14.910|
|clustered|16m|write|rollback|0.334331|7.164|14.895|
|clustered|16m|exact|r9|0.576507|8.605|16.000|
|clustered|16m|exact|r11p|0.481367|7.574|15.109|
|clustered|16m|exact|rollback|0.561045|8.621|16.000|
|clustered|16m|ann|r9|0.453261|8.605|16.000|
|clustered|16m|ann|r11p|0.457410|7.578|15.328|
|clustered|16m|ann|rollback|0.439723|8.602|16.000|
|clustered|2g|write|r9|0.421584|7.094|14.910|
|clustered|2g|write|r11p|0.354893|7.094|14.980|
|clustered|2g|write|rollback|0.331052|7.094|14.812|
|clustered|2g|exact|r9|0.581392|8.355|17.301|
|clustered|2g|exact|r11p|0.488512|7.504|15.152|
|clustered|2g|exact|rollback|0.612201|8.355|17.285|
|clustered|2g|ann|r9|0.436292|8.355|17.328|
|clustered|2g|ann|r11p|0.453888|7.504|15.145|
|clustered|2g|ann|rollback|0.437147|8.355|17.512|
|uniform|16m|write|r9|0.407020|7.160|14.984|
|uniform|16m|write|r11p|0.345054|7.160|15.168|
|uniform|16m|write|rollback|0.339700|7.160|14.840|
|uniform|16m|exact|r9|0.589526|8.609|16.000|
|uniform|16m|exact|r11p|0.569376|7.566|15.508|
|uniform|16m|exact|rollback|0.565360|8.598|16.000|
|uniform|16m|ann|r9|0.571680|8.598|16.000|
|uniform|16m|ann|r11p|0.525672|7.547|15.273|
|uniform|16m|ann|rollback|0.608800|8.605|16.000|
|uniform|2g|write|r9|0.417851|7.094|14.922|
|uniform|2g|write|r11p|0.333504|7.094|14.727|
|uniform|2g|write|rollback|0.350055|7.094|15.066|
|uniform|2g|exact|r9|0.593650|8.355|17.434|
|uniform|2g|exact|r11p|0.531742|7.473|15.312|
|uniform|2g|exact|rollback|0.610988|8.355|17.352|
|uniform|2g|ann|r9|0.572021|8.355|17.340|
|uniform|2g|ann|r11p|0.539000|7.473|15.484|
|uniform|2g|ann|rollback|0.573830|8.355|17.363|

|分布|内存|模式|R9本地E|R11p本地E|回退版本地E|
|---|---|---|---:|---:|---:|
|clustered|16m|exact|0.491612|0.533891|0.514601|
|clustered|16m|ann|0.522389|0.539244|0.538138|
|clustered|2g|exact|0.499320|0.533245|0.507100|
|clustered|2g|ann|0.526895|0.539221|0.540422|
|uniform|16m|exact|0.498289|0.516820|0.513371|
|uniform|16m|ann|0.502267|0.525190|0.505928|
|uniform|2g|exact|0.499788|0.530217|0.507382|
|uniform|2g|ann|0.503861|0.528315|0.511833|

本地标准分布并未复现线上R11p跌分：撤回紧凑页也会让B/M回升，Exact局部CPU收益被主动放弃。ANN是否回到R9范围、以及写入改进，应按上表判断；不声称已经复现/恢复线上3258.71，更不保证Prompt上涨。

## 回归与证据保真

本机9/9、容器9/9，官方二进制/只读Docker各20/20（95SQL）、legacy默认构建成功，4000完整响应与冻结R9一致；R9原四阶段Checkpoint/恢复期二次中断测试随storage_regression通过。4/8/10MiB延迟门槛未降低。
```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/9 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/9 Test #2: storage_regression ...............   Passed    0.77 sec
    Start 3: core_regression
3/9 Test #3: core_regression ..................   Passed    0.20 sec
    Start 4: simd_regression
4/9 Test #4: simd_regression ..................   Passed    0.02 sec
    Start 5: index_regression
5/9 Test #5: index_regression .................   Passed    0.62 sec
    Start 6: protocol_regression
6/9 Test #6: protocol_regression ..............   Passed    1.10 sec
    Start 7: all
7/9 Test #7: all ..............................   Passed    2.07 sec
    Start 8: dynamic_dml
8/9 Test #8: dynamic_dml ......................   Passed    3.66 sec
    Start 9: low_memory_regression
9/9 Test #9: low_memory_regression ............   Passed   11.08 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =  19.70 sec
```

前10轮31单元及62引用对象和相邻上下文不变；仅重组同一Round11第32/33单元，旧未引用完整提示可从Gitcb498ad恢复。不把标题变更视作保证正交或加分，实际去重由评审决定。
```json
{
  "bytes": 115941,
  "sha256": "3242aa858a066347f7f50e750b528f6f7cc61ca06e5bea567ea29387f0051bfd",
  "units": 33,
  "unique_valid_pointers": 66,
  "old31_units_and62_values_context_unchanged": true,
  "strict_utf8": true,
  "duplicate_keys": 0,
  "nonfinite": 0
}
```

## 复现
```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
DOCKER_BUILDKIT=0 docker build -t minivectordb:r11-rollback-legacy .
docker build --target build -t minivectordb-build:r11-rollback .
docker build -f Dockerfile.test --build-arg BUILD_IMAGE=minivectordb-build:r11-rollback --target test .
python3 protocol_check/run.py --command '["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]' --output work/r11-rollback-protocol-native
python3 protocol_check/run.py --image minivectordb:r11-rollback --output work/r11-rollback-protocol-docker
work/build/adapter_serializer_regression --benchmark 1000
python3 tests/benchmark_cgroup.py --image minivectordb:r11-rollback --phase rollback --rounds 3 --min-ann-recall .985 --output work/r11-rollback-cgroup-rollback.json
```
