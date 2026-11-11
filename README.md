# MiniVectorDB

C++20 单进程、单客户端数据库。每表一个 BIGINT 主键，最多一个 VECTOR 列；
支持公开样例中的 SQL、显式事务、持久化、向量检索与崩溃恢复。

## 构建与运行

```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release
cmake --build work/build -j2
ctest --test-dir work/build --output-on-failure
work/build/mvdb_local --data-dir /tmp/minivectordb
```

依赖 Linux、C++20 编译器、CMake >=3.20、zlib；回归驱动使用 Python3 标准库。
独立性能基准额外使用 NumPy。运行时检测 AVX2/FMA，保留 float64 回退。
数据页为16KiB，默认使用buffered pread/pwrite；仅MVDB_USE_ODIRECT=1时启用直接I/O，并保留不支持时的回退。

CLI 默认数据根目录 `/data`。未显式指定预算时读取 cgroup v2 的
`memory.max` 与 `memory.high`，取可解析有限额的较小值，预算为其55%，
下限4MiB、上限10MiB；均为`max`或不可读时默认10MiB（10485760字节）。
这是引擎分配预算，尚需为程序、内核和Page Cache留出余量。支持
`--data-dir PATH`、`--memory-budget BYTES`、`--help`，以及原有的
`DATA_DIR [MEMORY_BUDGET]` 位置参数。进程以前台方式读取 stdin、写入 stdout，
每条响应显式 flush，日志与诊断只写 stderr。

交互层遵循官方 [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md)：启动及恢复完成后立即输出
`{"type":"ready"}`，逐行接收带原样id的JSON请求；execute返回查询列/行或受影响行数，
SQL错误返回error.code对象，shutdown回滚未提交事务并正常关闭。普通裸SQL行仍兼容，
旧客户端也需要先消费启动ready。向量投影为JSON数组，BIGINT和id不经double转换。
未显式提供数据目录时，优先使用VECTORDB_DATA_DIR，再默认/data。

官方交互式验证：

```sh
python3 protocol_check/run.py --command '["/absolute/path/to/mvdb_local"]' --output work/protocol-native
python3 protocol_check/run.py --image minivectordb:round8 --output work/protocol-docker
```

公开题包为官方f4d4e2665版本的20个用例、95条SQL，包含重启、模式切换及错误断言。
测试驱动逐条使用JSONL；驱动指令不消耗SQL断言。正式容器只读根文件系统下，
所有临时/排序文件均写数据目录并及时unlink。默认buffered I/O允许内核复用干净页，
BufferPool为.pk/.header保留有上限的优先份额，顺序扫描使用其余帧。直接I/O
仅由MVDB_USE_ODIRECT=1显式启用；若文件系统拒绝，则自动回退。
SIGTERM/SIGINT处理器只设置sig_atomic_t标志；空闲输入可立即中断，在安全边界
执行关闭回滚。正在执行的SQL和响应先完成；stdout消费者始终不读取时仍可能形成背压。

## Docker（Ubuntu24.04）

```sh
docker build -t minivectordb:final .
docker volume create minivectordb-data
docker run --rm -i --network none --mount type=volume,src=minivectordb-data,dst=/data minivectordb:final
```

镜像采用Ubuntu24.04多阶段构建，构建阶段执行 CMake Release；运行镜像只包含
程序和必要运行库。exec形式 ENTRYPOINT 直接运行数据库，默认使用/挂载 `/data`，
没有 EXPOSE，也不启动网络服务。自定义参数示例：

```sh
docker run --rm -i --network none --mount type=volume,src=minivectordb-data,dst=/data minivectordb:final --data-dir /data/custom --memory-budget 33554432
docker build --target test -t minivectordb-tests:final .
python3 tests/docker_smoke.py --image minivectordb:final
```

`test`镜像目标在Ubuntu24.04/GCC13中运行完整CTest。Docker烟测另外检查无网络端口、
默认目录、命名卷重启、SIGKILL撤销未提交事务，以及自定义数据目录。

## 记录布局

数据根目录布局（CLI指定的是根目录）：

```text
/data/
  FORMAT                  MV3格式标识
  CURRENT                 已发布的Checkpoint epoch
  wal -> wal.<epoch>      当前紧凑Redo日志
  shadow.<epoch>/         逐出前镜像journal与可重建的磁盘页标记
  undo                    内存Undo超预算后的溢写区
  regions/                新索引区的分配标记
  data/
    <table>.header        Schema、活跃行数、索引区及推荐nprobe
    <table>.rows          固定32字节槽位元数据
    <table>.blob          VARCHAR等普通字段变长内容
    <table>.v<column>     连续float32向量payload
    <table>.s<column>     普通列64位哈希摘要，命中后复核原值
    <table>.valid         有效槽位位图
    <table>.pk            分页B+树：BIGINT -> uint32 SlotId
    <index-region>.*      质心、SlotId倒排桶和槽位到桶的映射
```

所有页共享受 MemoryBudget 管控的有界BufferPool。128维向量占512字节，
16KiB payload页容纳32条；向量与行元数据分离，IVF桶仅存4字节SlotId。
小K查询使用内存Top-K堆，以排序键和主键升序决胜；大结果才回退外部排序。

预算大于8MiB时约四分之三分配给BufferPool，其余保留给Undo、解析和查询工作区；
4/8MiB及更小预算仍保留一半工作区。最多25,000槽位的向量TopK查询可按预算
建立单表热读缓存：主键、有效/极端值标记、质心和4B SlotId桶。普通字符串列
最多128个字典值、16KiB字符串内容，以uint16槽位编码精确过滤；高基数列回退
通用执行器。缓存不复制向量payload，所有DML、事务控制和异常均使缓存失效。
驻留向量页使用带帧版本的句柄验证后直接读取；缺页仍由BufferPool统一换入。
SIMD平方距离仅筛除确定落选者，潜在赢家用float64复算，最终开方并按主键决胜。

新槽位向量与blob逻辑尾部可以在COMMIT前直接写入数据文件，WAL只发布其
可见元数据，减少双写。仅在整个文件本epoch从未被普通Redo或Shadow触及时
启用；否则保留完整WAL，防止恢复前镜像、文件截断或旧Redo覆盖新提交尾部。
这些保护标记跨重启保留，Checkpoint后重置。追加先扩展完整页，再写不可见
字节，避免强杀产生短页。该优化依据赛题SIGKILL模型，不宣称支持断电恢复。
WAL同步后、数据Checkpoint同步后及临时文件关闭时提示内核释放可回收页缓存。

Round 8容器基准使用独立数据卷和cgroup，外部采样器读取io.stat与memory.peak：

```sh
python3 tests/benchmark_local.py --phase after --automatic-ann --default-budget --binary work/build/mvdb_local --output work/native-after.json
python3 tests/benchmark_cgroup.py --image minivectordb:round8 --phase after --output work/cgroup-after.json --rounds 3
```

本地Docker Desktop统计包含同设备启动读，且容器内固定保留一个关闭后采样用的
shell；前后口径一致，但不能替代正式平台的专用数据设备成绩。历史实测与本轮
对比见[Round 8报告](docs/round8-performance.md)。

Round 9采用借用SQL游标和逐元组INSERT，最多8个有界页缓冲合并追加，最后发布
表头水位；语句失败仍恢复Undo，水位之外的孤儿槽位不得参与后续修改。
向量页全部驻留时按探测桶直读，128D使用移位寻址；存在缺页时保留顺序安全路径。
已有四累加器AVX2/FMA代码单独放在`src/simd.cpp`，数值与极端值回退保持一致。
10MiB默认上限来自10/12MiB驻留率实验，4/8MiB回归独立验证。
三代完整窗口实测及局限见[Round 9报告](docs/round9-performance.md)。

```sh
cmake --build work/build --target benchmark_cpu -j2
work/build/benchmark_cpu DATA_COPY QUERY_SQL_LINES ann 10485760
```

`benchmark_cpu`仅用于进程内归因，须使用可丢弃的数据副本，不能代替端到端成绩。

## 索引与内建校准

CREATE VECTOR INDEX 在新分配的索引区内构建，N>=4096使用64桶，小表最多8桶。
构建完成后，C++引擎确定性抽样无过滤查询和高频字符串等值过滤查询，直接比较
16条样本（无过滤/高频等值过滤各8条），在一次向量扫描中维护各样本Top-10，
再比较各nprobe召回，选择两组均达到平均Recall@10>=0.985的最小值，
将 recommended_nprobe 写入索引元数据。查询不依赖外部Python校准。
当有效行数不超过25000时，只要校准工作区可分配就执行这次单遍校准，
不再因payload超过预算一半而跳过。仅在行数超过25000且payload也超出
预算一半，或校准工作区不可分配时，跳过抽样校准并持久化
经验推荐max(4,nlist/4)（不超过nlist）和calibration_skipped。该推荐没有召回证明，
自动ANN保守使用Exact；手动NPROBE仍可选择IVF。

ann且未手动覆盖NPROBE时使用持久化推荐值。建索引时记录
`rows_at_calibration`，每条成功DML按受影响行数增加持久化的
`mutations_since_calibration`；计数不超过`max(64, rows_at_calibration/5)`
时保留推荐值并同步维护倒排桶、有效位图和槽位映射。预算跳过校准、严格超过阈值、
推荐探测超过一半桶或活跃行数小于4096时，自动回退连续SIMD扫描。
计数与数据一起受语句/事务Undo管理；阈值外当前保守回退，不在查询中隐式重建。
显式 `SET NPROBE = n` 保留手工控制，`SET NPROBE = 0` 恢复自动模式。
这是本地适配器的设置语法，不作为正式评测封包声明。

IVF先生成候选SlotId位图，与有效位图和普通列过滤摘要求交，再按SlotId物理顺序
读取payload。正常值用float32 SIMD筛除明显落后项，潜在Top-K赢家使用float64
复算，避免极近距离舍入成错误并列；极端有限值也回退安全距离核。

## 事务与恢复

采用单版本槽位、字节增量Undo和语句Savepoint，没有MVCC或后台压实线程。
普通预算下Undo最多256KiB留在内存，超大事务才溢写；单条SQL失败只回滚到其
Savepoint，不撤销外层事务已经成功的语句。新索引区只记录小型分配撤销标记，
失败/回滚整体丢弃区域，成功构建后发布Header；后续DML恢复普通字节Undo。

Redo WAL记录带CRC32的变化范围并按有界批次压缩，COMMIT前同步WAL。
No-Steal是有界偏好：优先保留未提交脏页，但不允许冷页无限占住小缓存。
不得不逐出未提交脏页时，先完成该页原始前镜像或文件旧长度的write，再覆盖data页；不逐页fsync。
恢复先回放当前epoch的Shadow，再重放完整提交WAL；中途再次强杀可重复恢复。
完整长度与IEEE CRC32校验的COMMIT帧是提交权威；常规100行批次仅执行
一次WAL fdatasync，不再写入独立提交水位文件。Undo、Shadow逐出和临时排序
不执行64KiB窗口同步；Spill按数MiB块及运行结束时作轻量缓存提示。崩溃模型为SIGKILL，不包含断电或外部篡改。
完整WAL可重建新追加页的短尾；CRC仍检查帧完整性，不承诺恢复人为破坏的已确认日志。

正常Checkpoint只原地刷脏页、同步数据文件和目录，再原子发布新的空WAL/Shadow
epoch，最后退役旧epoch。不复制全表快照。提交页刷写与Shadow退役顺序由四阶段
崩溃回归验证；旧epoch Shadow不会在新Checkpoint发布后被错误应用。

第二阶段格式的数据目录需要导出重建；遇到旧格式会明确拒绝，不静默清空。
MemoryBudget不是整个进程RSS或cgroup memory.peak。干净Page Cache可被回收，
但这不意味着任意内存限额下都不可能OOM；内核缓存和进程内存必须一起计入容器限额。

## 自动模式基准

```sh
python3 tests/benchmark_local.py --phase before --automatic-ann --default-budget --binary work/phase3-baseline/mvdb_local --output work/benchmark-closing-before.json
python3 tests/benchmark_local.py --phase after --automatic-ann --default-budget --binary work/build/mvdb_local --output work/benchmark-closing-after.json
```

两版均不发送SET NPROBE。基准包含固定种子聚类/均匀两组10,000×128数据、
100批×100条写入，以及exact/ann各1,000条查询（各500条无过滤/字符串等值过滤）。
独立NumPy float64 oracle验证排名与召回；记录查询路由、推荐值、实际probe、
进程物理I/O、VmHWM、WAL、Undo及目录占用，正常关闭成本也纳入记录。

默认基准不会传入内存预算或NPROBE；旧版省略预算时为8MiB，新版由cgroup决定。
对比报告区分预算变化和提交路径变化，不能将全部性能差异归因于单一修改。
安装NumPy的Python被CMake选中时，CTest包含dynamic_dml：10k建索引后各50次
插入/更新/删除，校验150次可见性及1200次ANN的IVF/5路径和独立真值召回。
最终交互证据在`agent/conversation.json`和根目录`conversation.json`，两份字节一致。

## 小预算防超时复验

```sh
python3 tests/low_memory.py --binary work/build/mvdb_local --phase after --output work/low-memory.json --enforce-latency --require-ivf
```

固定种子10k×128D，在4MiB/8MiB引擎预算下各执行100批写入、建索引、
Exact/ANN各1000查询、全表事务修改/回滚、强杀恢复并校验已提交数据。
每条SQL响应必须低于0.5秒、建索引低于0.3秒，聚类10k自动ANN要求
使用校准所得IVF/nprobe=5，输出保存全部请求计时；该指标是本机实测，
不能当成任意硬件或任意SQL规模的时限保证。
NumPy可用时CTest包含此完整回归及16MiB预算下的增量IVF维护验证。
