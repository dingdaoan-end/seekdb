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
数据文件使用16KiB对齐直接 I/O，数据目录所在文件系统需要支持 O_DIRECT。

CLI 默认数据根目录 `/data`。未显式指定预算时读取 cgroup v2 的
`memory.max` 与 `memory.high`，取可解析有限额的较小值，预算为其55%，
下限4MiB、上限24MiB；均为`max`或不可读时默认20MiB（20971520字节）。
这是引擎分配预算，尚需为程序、内核和Page Cache留出余量。支持
`--data-dir PATH`、`--memory-budget BYTES`、`--help`，以及原有的
`DATA_DIR [MEMORY_BUDGET]` 位置参数。进程以前台方式读取 stdin、写入 stdout，
每条响应显式 flush，日志与诊断只写 stderr。

目前交互层是开发用的 SQL行输入/JSON行输出适配器，与 Engine 解耦。
公开题包没有完整正式驱动，因此不声称这些封包、错误码或就绪标识是正式协议。

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

## 索引与内建校准

CREATE VECTOR INDEX 在新分配的索引区内构建，N>=4096使用64桶，小表最多8桶。
构建完成后，C++引擎确定性抽样无过滤查询和高频字符串等值过滤查询，直接比较
Exact Top-10与各nprobe召回，选择两组均达到平均Recall@10>=0.985的最小值，
将 recommended_nprobe 写入索引元数据。查询不依赖外部Python校准。

ann且未手动覆盖NPROBE时使用持久化推荐值。建索引时记录
`rows_at_calibration`，每条成功DML按受影响行数增加持久化的
`mutations_since_calibration`；计数不超过`max(64, rows_at_calibration/5)`
时保留推荐值并同步维护倒排桶、有效位图和槽位映射。仅在严格超过阈值、
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
不得不逐出未提交脏页时，先同步该页原始前镜像或文件旧长度，再覆盖data页。
恢复先回放当前epoch的Shadow，再重放完整提交WAL；中途再次强杀可重复恢复。
完整长度与IEEE CRC32校验的COMMIT帧是提交权威；常规100行批次仅执行
一次WAL fdatasync，不再写入独立提交水位文件。大事务按事务内未同步字节窗口
有界排空，可能额外同步。崩溃模型为SIGKILL，不包含断电或外部篡改。
完整WAL可重建新追加页的短尾；CRC仍检查帧完整性，不承诺恢复人为破坏的已确认日志。

正常Checkpoint只原地刷脏页、同步数据文件和目录，再原子发布新的空WAL/Shadow
epoch，最后退役旧epoch。不复制全表快照。提交页刷写与Shadow退役顺序由四阶段
崩溃回归验证；旧epoch Shadow不会在新Checkpoint发布后被错误应用。

第二阶段格式的数据目录需要导出重建；遇到旧格式会明确拒绝，不静默清空。
MemoryBudget不是整个进程RSS或cgroup memory.peak；基准分别记录这些指标。

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
