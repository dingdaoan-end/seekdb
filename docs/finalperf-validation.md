# Final performance validation

代码比较基线为 `4722cb866b3c8266c2b34fa2290a9be5650ad392`；历史证据基线为 R10-Safe 的 `7eb530ccfd81d1a79c3d7a611b78d974404573cf`，两者用途不同。历史 JSON 为 122,675 字节、10 轮、31 个学习单元，旧对象与旧 metadata 原值保持完整，仅允许轮数与单元数两个计数字段变更。本文最终门禁与72个项目样本见下文；局部阶段实验单独标注，不把用户提出的预期当作实测结论。

## 资源窗口与比较边界

项目权重折算后，写入时间占综合指标 20%、查询时间占 40%；B 与 M 各占 20%。进程内 CPU、进程 RSS、malloc 申请字节、cgroup `memory.peak` 是不同指标。只有独立测量容器的完整生命周期 `io.stat` 与 `memory.peak` 用于本地 B/M 比较。每轮使用全新数据卷与 cgroup，采样容器位于目标 cgroup 外；关闭、checkpoint 与写回后再冻结计数。查询 T 是 1,000 次 JSON 请求往返，排除装载、建索引、20 次固定预热和关闭；其 B/M 仍覆盖这些阶段。写入 T 包含 100 批请求和正常关闭。

禁止数据文件 `POSIX_FADV_DONTNEED`，没有全局清缓存，也没有 mmap 数据文件绕过 BufferPool。旧 `sync_all()` 在数据 fsync 后逐文件调用 DONTNEED 的行为已删除；剩余 DONTNEED 仅涉及 WAL 与临时溢写文件。移除旧数据缓存驱逐可能影响完整容器的 B/M，必须由最终 72 个项目样本体现，不能用局部 RSS 替代。

## 逐项 A/B 与保留理由

|项目|权重或测量对象|已采集结果|解释与状态|
|---|---|---|---|
|帧按需绑定|M_write/M_query|16MiB 预算：构造 RSS 4316→4316 KiB；满载 HWM 16160→16160 KiB；discard 后 RSS 16380→4428 KiB|真实收益是区域失效后释放匿名页，构造与满载峰值无显著收益；不声称下降45%。|
|每帧 malloc 对齐方案|RSS 负向实验|满载 RSS 16336→19276 KiB|增加约2.87MiB，因分配器对齐与块头碎片而弃用；采用每帧匿名映射消除该开销。|
|浮点解析守卫快路径|CPU；写/查询时间|3,200,350 组 oracle，0 个与冻结解析器不符；直接 strtof 与既有语义存在161,116个差异|以冻结解析器的逐位结果和非法输入接纳行为为兼容标准，不能误换为纯 strtof 基线。七类完整计时如下。|
|紧凑字典候选切片|50% 带过滤查询 CPU|最终 clustered Filtered Exact 0.034439→0.0159738s；uniform 0.034784→0.0172042s|相对基线快53.62%/50.54%；关闭切片后的对照分别为0.0316316/0.0331446s。标准 ANN 仍走原5桶探测。|
|64候选微探测|Exact CPU|严格前64次试探在聚类初期0次早停；最终聚类无过滤CPU0.102828→0.074463s|证伪“堆刚填满即有高拒绝率”；保留阈值改善4倍后的复探和256槽位块控制。均匀每查询只试64次，无早停即关闭。|
|FlatRequest|输入解析|本轮30,000组差分变异；千次向量请求解析分配12,000→0、CPU2.08291→1.19805ms|不包含SQL字符串物化、引擎、响应与管道I/O，不声称整个请求零分配。|
|向量临时所有权复用|分配次数|七类解析计时均为10,000次、5,120,000字节分配|未观察到所有权复用收益，因此未加入该改动。|
|索引训练工作区|M_query|未发现整表10,000×128向量复制；保留已受预算约束的质心与工作区|不能虚构削减5.12MB训练副本或独立索引内存峰值收益；最终容器 M 仍须实测。|

### 浮点解析七类 CPU 中位数

每类 1,280,000 个分量，7 轮交错计时。两端分配次数和字节完全相同，checksum 相同。秒数来自本轮已生成的 `timing-final.txt`，不是端到端请求 T。

|类别|输入格式|基线CPU(s)|新版CPU(s)|新版/基线|分配次数/字节（两端相同）|
|---|---|---:|---:|---:|---|
|0|6位小数|0.025657|0.022219|0.8660|10000 / 5120000|
|1|12位小数|0.054565|0.037581|0.6887|10000 / 5120000|
|2|14位小数|0.058526|0.039583|0.6763|10000 / 5120000|
|3|6位科学计数|0.057537|0.035128|0.6105|10000 / 5120000|
|4|17位有效数字|0.050947|0.051996|1.0206|10000 / 5120000|
|5|次正规/长科学计数|0.091869|0.094255|1.0260|10000 / 5120000|
|6|15位小数|0.054860|0.051251|0.9342|10000 / 5120000|

17位有效数字和次正规两项回退路径中的约2%回归保留为实际局限，不能将所有格式描述为加速。最终容器分布决定总体收益。IEEE-754 守卫拒绝半途点附近、溢出、次正规与不满足安全前提的输入；回退维持原有接纳行为与错误处理。

## 内存与持久化语义

旧连续 arena 构造时没有整体 memset，因此操作系统已按需提交物理页。新实现保留原 Frame 结构、固定帧数与全部 MemoryBudget 预留，只添加固定长度的页指针槽位，计入原每帧 metadata 余量。首次绑定时申请16KiB `MAP_PRIVATE|MAP_ANONYMOUS` 内存，地址满足4096对齐；普通换页复用 backing，成功区域失效后 unmap。此处映射的是匿名内存，不是数据库文件；所有追加仍经过 BufferPool，页格式、持久化结构、WAL、Undo 与数据文件格式未改变。

区域失效先检查所有匹配 frame 的 pin，再执行任何失效操作；失败不能只丢掉前半组。成功丢弃脏帧不回写，先使 handle/version 失效，再释放内存及匹配描述符。短页读取失败前也先作旧版本失效，避免 handle 指向已经被覆盖的 bytes。原基线的 pinned-preflight 测试为红，新实现为绿；新测试另覆盖同名不同布局重建、脏页丢弃、相邻名称、保存点/整事务回滚、SIGKILL及重复恢复、冷3页、249帧满载循环、直接I/O与零数据缓存驱逐。

字典切片的两个 uint32 数组在10,000槽位时额外约80KB，按容量向统一预算记账。它们属于可失效的读缓存，没有修改持久化头或索引布局。精确模式按升序槽位选择最小候选桶；正常过滤 ANN 保持5桶路线；极稀少桶直通与候选不足时扩桶已有针对性测试。这些功能测试只能说明对应场景行为，不能替代所有数据分布的召回保证。

## 最终门禁、完整资源表与 E

本节仅由完整实际输入更新。此前不预测线上性能分、四个单元的正式评分或综合大幅提升。

<!-- FINAL_GATES_START -->
|门禁|实测|要求|
|---|---|---|
|native_ctest|14|14|
|container_ctest|14|14|
|native_protocol|20|20|
|readonly_protocol|20|20|
|equal_responses|4000|4000|
|persistent_checks|25|25|
|small_budget_runs|4|4|
|json_mutations|30000|30000|
<!-- FINAL_GATES_END -->

<!-- FINAL_CPU_START -->
以下为最终源码的实际 CPU 输出；微基准不等同完整 JSON/cgroup T。

```json
[
  {
    "distribution": "clustered",
    "group": "unfiltered",
    "mode": "exact",
    "cpu_seconds": {
      "baseline": 0.102828,
      "all": 0.074463,
      "no_prefix": 0.1059,
      "no_dictionary": 0.0878212,
      "no_expansion": 0.0840156
    },
    "wall_seconds": {
      "baseline": 0.102837,
      "all": 0.0744702,
      "no_prefix": 0.105909,
      "no_dictionary": 0.0878294,
      "no_expansion": 0.0840233
    },
    "cpu_change_vs_baseline_pct": -27.58489905473217,
    "prefix_cpu_change_pct": -29.685552407932004,
    "dictionary_cpu_change_pct": -15.210678059511828,
    "checksum_equal": true,
    "auto_attempts": 24335000,
    "auto_pruned": 22663035,
    "auto_fallbacks": 2500,
    "route": "exact",
    "nprobe": 0
  },
  {
    "distribution": "clustered",
    "group": "unfiltered",
    "mode": "ann",
    "cpu_seconds": {
      "baseline": 0.0218292,
      "all": 0.022469,
      "no_prefix": 0.0220656,
      "no_dictionary": 0.0218546,
      "no_expansion": 0.0217046
    },
    "wall_seconds": {
      "baseline": 0.021834,
      "all": 0.0224735,
      "no_prefix": 0.0220685,
      "no_dictionary": 0.0218577,
      "no_expansion": 0.0217076
    },
    "cpu_change_vs_baseline_pct": 2.9309365437120816,
    "prefix_cpu_change_pct": 1.8281850482198436,
    "dictionary_cpu_change_pct": 2.811307459299184,
    "checksum_equal": true,
    "auto_attempts": 160000,
    "auto_pruned": 0,
    "auto_fallbacks": 2500,
    "route": "ivf",
    "nprobe": 5
  },
  {
    "distribution": "clustered",
    "group": "filtered",
    "mode": "exact",
    "cpu_seconds": {
      "baseline": 0.034439,
      "all": 0.0159738,
      "no_prefix": 0.0173928,
      "no_dictionary": 0.0316316,
      "no_expansion": 0.0179058
    },
    "wall_seconds": {
      "baseline": 0.0344426,
      "all": 0.0159762,
      "no_prefix": 0.0173951,
      "no_dictionary": 0.0316352,
      "no_expansion": 0.0179093
    },
    "cpu_change_vs_baseline_pct": -53.61712012543918,
    "prefix_cpu_change_pct": -8.158548364840623,
    "dictionary_cpu_change_pct": -49.50049950049951,
    "checksum_equal": true,
    "auto_attempts": 889290,
    "auto_pruned": 685155,
    "auto_fallbacks": 2485,
    "route": "exact",
    "nprobe": 0
  },
  {
    "distribution": "clustered",
    "group": "filtered",
    "mode": "ann",
    "cpu_seconds": {
      "baseline": 0.0112918,
      "all": 0.0114254,
      "no_prefix": 0.0110806,
      "no_dictionary": 0.0110072,
      "no_expansion": 0.0108014
    },
    "wall_seconds": {
      "baseline": 0.0112936,
      "all": 0.011427,
      "no_prefix": 0.0110822,
      "no_dictionary": 0.0110092,
      "no_expansion": 0.0108031
    },
    "cpu_change_vs_baseline_pct": 1.1831594608477047,
    "prefix_cpu_change_pct": 3.111744851361853,
    "dictionary_cpu_change_pct": 3.799331346754853,
    "checksum_equal": true,
    "auto_attempts": 0,
    "auto_pruned": 0,
    "auto_fallbacks": 0,
    "route": "ivf",
    "nprobe": 5
  },
  {
    "distribution": "uniform",
    "group": "unfiltered",
    "mode": "exact",
    "cpu_seconds": {
      "baseline": 0.11414,
      "all": 0.108065,
      "no_prefix": 0.110274,
      "no_dictionary": 0.107821,
      "no_expansion": 0.106385
    },
    "wall_seconds": {
      "baseline": 0.11415,
      "all": 0.108074,
      "no_prefix": 0.110284,
      "no_dictionary": 0.10783,
      "no_expansion": 0.106394
    },
    "cpu_change_vs_baseline_pct": -5.322411074119515,
    "prefix_cpu_change_pct": -2.003192048896385,
    "dictionary_cpu_change_pct": 0.2263009988777709,
    "checksum_equal": true,
    "auto_attempts": 160000,
    "auto_pruned": 0,
    "auto_fallbacks": 2500,
    "route": "exact",
    "nprobe": 0
  },
  {
    "distribution": "uniform",
    "group": "unfiltered",
    "mode": "ann",
    "cpu_seconds": {
      "baseline": 0.10762,
      "all": 0.105043,
      "no_prefix": 0.109267,
      "no_dictionary": 0.104415,
      "no_expansion": 0.114297
    },
    "wall_seconds": {
      "baseline": 0.107626,
      "all": 0.105049,
      "no_prefix": 0.109274,
      "no_dictionary": 0.104422,
      "no_expansion": 0.114307
    },
    "cpu_change_vs_baseline_pct": -2.3945363315368895,
    "prefix_cpu_change_pct": -3.8657600190359465,
    "dictionary_cpu_change_pct": 0.6014461523727421,
    "checksum_equal": true,
    "auto_attempts": 160000,
    "auto_pruned": 0,
    "auto_fallbacks": 2500,
    "route": "exact_fallback",
    "nprobe": 0
  },
  {
    "distribution": "uniform",
    "group": "filtered",
    "mode": "exact",
    "cpu_seconds": {
      "baseline": 0.034784,
      "all": 0.0172042,
      "no_prefix": 0.017673,
      "no_dictionary": 0.0331446,
      "no_expansion": 0.0176694
    },
    "wall_seconds": {
      "baseline": 0.0347877,
      "all": 0.0172061,
      "no_prefix": 0.0176748,
      "no_dictionary": 0.033148,
      "no_expansion": 0.0176716
    },
    "cpu_change_vs_baseline_pct": -50.53990340386385,
    "prefix_cpu_change_pct": -2.652633961410067,
    "dictionary_cpu_change_pct": -48.09350542773182,
    "checksum_equal": true,
    "auto_attempts": 160000,
    "auto_pruned": 0,
    "auto_fallbacks": 2500,
    "route": "exact",
    "nprobe": 0
  },
  {
    "distribution": "uniform",
    "group": "filtered",
    "mode": "ann",
    "cpu_seconds": {
      "baseline": 0.0346014,
      "all": 0.017761,
      "no_prefix": 0.0175082,
      "no_dictionary": 0.0335028,
      "no_expansion": 0.0185674
    },
    "wall_seconds": {
      "baseline": 0.0346086,
      "all": 0.0177633,
      "no_prefix": 0.0175105,
      "no_dictionary": 0.0335062,
      "no_expansion": 0.0185698
    },
    "cpu_change_vs_baseline_pct": -48.6697070060749,
    "prefix_cpu_change_pct": 1.4438948606938284,
    "dictionary_cpu_change_pct": -46.98652052962737,
    "checksum_equal": true,
    "auto_attempts": 160000,
    "auto_pruned": 0,
    "auto_fallbacks": 2500,
    "route": "exact_fallback",
    "nprobe": 0
  }
]
```
<!-- FINAL_CPU_END -->

CPU每组为500条查询，先整组预热，再执行5遍，时间除以5；尝试、剪枝、回退计数为全部2500次查询的合计。最终矩阵包含8组、5种版本/开关、3次交错对照，共120次进程测量。开关对照存在短运行噪声；例如无过滤查询不应因字典切片开关产生算法收益，观察到的这部分差异不能归因于字典过滤。

<!-- FINAL_RESOURCE_START -->
|分布|限额|阶段|基线T(s)|新版T(s)|基线B(MiB)|新版B(MiB)|基线M(MiB)|新版M(MiB)|
|---|---|---|---|---|---|---|---|---|
|clustered|16m|write|0.510755|0.507243|7.168|7.160|14.801|14.938|
|clustered|16m|exact|0.800707|0.715169|8.602|8.609|16.000|16.000|
|clustered|16m|ann|0.599167|0.596572|8.605|8.605|16.000|16.000|
|clustered|2g|write|0.535027|0.507564|7.094|7.094|14.980|14.848|
|clustered|2g|exact|0.792597|0.704653|8.355|8.355|17.316|17.305|
|clustered|2g|ann|0.604998|0.596258|8.355|8.355|17.324|17.297|
|uniform|16m|write|0.556377|0.502636|7.168|7.160|14.902|15.012|
|uniform|16m|exact|0.801021|0.779623|8.605|8.605|16.000|16.000|
|uniform|16m|ann|0.800125|0.768163|8.609|8.602|16.000|16.000|
|uniform|2g|write|0.549635|0.519190|7.094|7.094|14.797|14.746|
|uniform|2g|exact|0.794651|0.742110|8.355|8.355|17.344|17.254|
|uniform|2g|ann|0.804691|0.758320|8.355|8.355|17.340|17.250|

完整72个实测项目样本（每项均为独立容器；B为读写总和）：

|版本|分布|限额|轮|阶段|T(s)|读B|写B|总B|M字节|建索引(s)|Recall(无/有过滤)|
|---|---|---|---|---|---|---|---|---|---|---|---|
|base|clustered|16m|1|ann|0.592924926|413696|8609792|9023488|16777216|0.242138|[0.9907999999999999, 0.9886]|
|base|clustered|16m|1|exact|0.783770982|413696|8597504|9011200|16777216|0.248309|[1.0, 1.0]|
|base|clustered|16m|1|write|0.518723352|1175552|7102464|8278016|16449536|0.000000|None|
|base|clustered|16m|2|ann|0.599235738|413696|8593408|9007104|16777216|0.238974|[0.9907999999999999, 0.9886]|
|base|clustered|16m|2|exact|0.830627702|413696|8605696|9019392|16777216|0.242008|[1.0, 1.0]|
|base|clustered|16m|2|write|0.505050269|409600|7106560|7516160|15519744|0.000000|None|
|base|clustered|16m|3|ann|0.599166625|413696|8613888|9027584|16777216|0.243338|[0.9907999999999999, 0.9886]|
|base|clustered|16m|3|exact|0.800706517|413696|8609792|9023488|16777216|0.255486|[1.0, 1.0]|
|base|clustered|16m|3|write|0.510754556|409600|7094272|7503872|15425536|0.000000|None|
|base|clustered|2g|1|ann|0.604998378|413696|8347648|8761344|18157568|0.235249|[0.9907999999999999, 0.9886]|
|base|clustered|2g|1|exact|0.792597281|413696|8347648|8761344|18157568|0.231538|[1.0, 1.0]|
|base|clustered|2g|1|write|0.535027166|409600|7028736|7438336|15794176|0.000000|None|
|base|clustered|2g|2|ann|0.609631394|413696|8347648|8761344|18165760|0.234257|[0.9907999999999999, 0.9886]|
|base|clustered|2g|2|exact|0.777591317|413696|8347648|8761344|18153472|0.242758|[1.0, 1.0]|
|base|clustered|2g|2|write|0.543948046|409600|7028736|7438336|15601664|0.000000|None|
|base|clustered|2g|3|ann|0.599062849|413696|8347648|8761344|18386944|0.254670|[0.9907999999999999, 0.9886]|
|base|clustered|2g|3|exact|0.797190102|413696|8347648|8761344|18161664|0.234815|[1.0, 1.0]|
|base|clustered|2g|3|write|0.522923689|409600|7028736|7438336|15708160|0.000000|None|
|base|uniform|16m|1|ann|0.800124625|413696|8626176|9039872|16777216|0.249817|[1.0, 1.0]|
|base|uniform|16m|1|exact|0.802827279|413696|8613888|9027584|16777216|0.238320|[1.0, 1.0]|
|base|uniform|16m|1|write|0.556376520|409600|7118848|7528448|15925248|0.000000|None|
|base|uniform|16m|2|ann|0.799681107|413696|8613888|9027584|16777216|0.241832|[1.0, 1.0]|
|base|uniform|16m|2|exact|0.800990702|413696|8597504|9011200|16777216|0.249503|[1.0, 1.0]|
|base|uniform|16m|2|write|0.579305430|409600|7090176|7499776|15413248|0.000000|None|
|base|uniform|16m|3|ann|0.825629275|413696|8613888|9027584|16777216|0.242890|[1.0, 1.0]|
|base|uniform|16m|3|exact|0.801020961|413696|8609792|9023488|16777216|0.239052|[1.0, 1.0]|
|base|uniform|16m|3|write|0.549493179|409600|7106560|7516160|15626240|0.000000|None|
|base|uniform|2g|1|ann|0.804691090|413696|8347648|8761344|18132992|0.236662|[1.0, 1.0]|
|base|uniform|2g|1|exact|0.794650904|413696|8347648|8761344|18186240|0.232800|[1.0, 1.0]|
|base|uniform|2g|1|write|0.516514193|409600|7028736|7438336|15482880|0.000000|None|
|base|uniform|2g|2|ann|0.793937383|413696|8347648|8761344|18182144|0.244960|[1.0, 1.0]|
|base|uniform|2g|2|exact|0.813097320|413696|8347648|8761344|18169856|0.231800|[1.0, 1.0]|
|base|uniform|2g|2|write|0.549634524|409600|7028736|7438336|15515648|0.000000|None|
|base|uniform|2g|3|ann|0.805726085|413696|8347648|8761344|18202624|0.237739|[1.0, 1.0]|
|base|uniform|2g|3|exact|0.781743059|413696|8347648|8761344|18198528|0.234683|[1.0, 1.0]|
|base|uniform|2g|3|write|0.574687424|409600|7028736|7438336|15859712|0.000000|None|
|new|clustered|16m|1|ann|0.599727162|413696|8634368|9048064|16777216|0.245082|[0.9907999999999999, 0.9886]|
|new|clustered|16m|1|exact|0.715168760|413696|8626176|9039872|16777216|0.254404|[1.0, 1.0]|
|new|clustered|16m|1|write|0.507243485|409600|7102464|7512064|15855616|0.000000|None|
|new|clustered|16m|2|ann|0.594712205|413696|8597504|9011200|16777216|0.238161|[0.9907999999999999, 0.9886]|
|new|clustered|16m|2|exact|0.698660462|413696|8609792|9023488|16777216|0.239123|[1.0, 1.0]|
|new|clustered|16m|2|write|0.495972507|409600|7098368|7507968|15663104|0.000000|None|
|new|clustered|16m|3|ann|0.596571544|413696|8609792|9023488|16777216|0.240216|[0.9907999999999999, 0.9886]|
|new|clustered|16m|3|exact|0.720053056|413696|8613888|9027584|16777216|0.240030|[1.0, 1.0]|
|new|clustered|16m|3|write|0.509271592|409600|7090176|7499776|15388672|0.000000|None|
|new|clustered|2g|1|ann|0.596535020|413696|8347648|8761344|18153472|0.228974|[0.9907999999999999, 0.9886]|
|new|clustered|2g|1|exact|0.704653291|413696|8347648|8761344|18350080|0.233043|[1.0, 1.0]|
|new|clustered|2g|1|write|0.507563631|409600|7028736|7438336|15622144|0.000000|None|
|new|clustered|2g|2|ann|0.596258426|413696|8347648|8761344|18042880|0.238268|[0.9907999999999999, 0.9886]|
|new|clustered|2g|2|exact|0.709978771|413696|8347648|8761344|18116608|0.238019|[1.0, 1.0]|
|new|clustered|2g|2|write|0.506619567|409600|7028736|7438336|15568896|0.000000|None|
|new|clustered|2g|3|ann|0.591258568|413696|8347648|8761344|18137088|0.234082|[0.9907999999999999, 0.9886]|
|new|clustered|2g|3|exact|0.689807913|413696|8347648|8761344|18145280|0.232772|[1.0, 1.0]|
|new|clustered|2g|3|write|0.517057115|409600|7028736|7438336|15560704|0.000000|None|
|new|uniform|16m|1|ann|0.750892244|413696|8601600|9015296|16777216|0.239959|[1.0, 1.0]|
|new|uniform|16m|1|exact|0.783361398|413696|8626176|9039872|16777216|0.241759|[1.0, 1.0]|
|new|uniform|16m|1|write|0.502636482|409600|7094272|7503872|15773696|0.000000|None|
|new|uniform|16m|2|ann|0.768163292|413696|8605696|9019392|16777216|0.238195|[1.0, 1.0]|
|new|uniform|16m|2|exact|0.779622700|413696|8601600|9015296|16777216|0.245672|[1.0, 1.0]|
|new|uniform|16m|2|write|0.510458893|409600|7098368|7507968|15740928|0.000000|None|
|new|uniform|16m|3|ann|0.787175681|413696|8613888|9027584|16777216|0.244735|[1.0, 1.0]|
|new|uniform|16m|3|exact|0.779348466|413696|8609792|9023488|16777216|0.244455|[1.0, 1.0]|
|new|uniform|16m|3|write|0.501024339|409600|7098368|7507968|15446016|0.000000|None|
|new|uniform|2g|1|ann|0.779274262|413696|8347648|8761344|18087936|0.233512|[1.0, 1.0]|
|new|uniform|2g|1|exact|0.735112999|413696|8347648|8761344|18161664|0.232695|[1.0, 1.0]|
|new|uniform|2g|1|write|0.519189601|409600|7028736|7438336|15675392|0.000000|None|
|new|uniform|2g|2|ann|0.758320053|413696|8347648|8761344|18366464|0.235339|[1.0, 1.0]|
|new|uniform|2g|2|exact|0.761797571|413696|8347648|8761344|18026496|0.236121|[1.0, 1.0]|
|new|uniform|2g|2|write|0.519228663|409600|7028736|7438336|15380480|0.000000|None|
|new|uniform|2g|3|ann|0.745529960|413696|8347648|8761344|17993728|0.234308|[1.0, 1.0]|
|new|uniform|2g|3|exact|0.742110119|413696|8347648|8761344|18092032|0.236848|[1.0, 1.0]|
|new|uniform|2g|3|write|0.510445660|409600|7028736|7438336|15462400|0.000000|None|

输入镜像标识：{"base": ["sha256:265ce5d83e60c9c74db7b452d7be7e2296b6c4444516f3e532c95f2819bb9f86"], "new": ["sha256:997f8b96329f4470f7f63c0cbb2f061cf5c1170f5df3c277dd00fff1073f6a6c"]}
<!-- FINAL_RESOURCE_END -->

<!-- FINAL_E_START -->
固定本次代码基线三轮中位 write/exact 参考；epsilon_T=.001s、epsilon_B=epsilon_M=1048576字节；R=(ref+epsilon)/(value+epsilon)，f(R)=R/(1+R)，E_project=.6f(R_T)+.2f(R_B)+.2f(R_M)，E=(E_write+2E_query)/3。逐轮计算后取中位、显示6位；它是本地相对指标，不能转换成线上评分。

|分布|限额|查询模式|基线E三轮|新版E三轮|基线中位|新版中位|相对变化|
|---|---|---|---|---|---|---|---|
|clustered|16m|exact|0.499060,0.496897,0.500105|0.511221,0.514918,0.510875|0.499060|0.511221|+2.437%|
|clustered|16m|ann|0.526661,0.529343,0.528845|0.528584,0.530847,0.529473|0.528845|0.529473|+0.119%|
|clustered|2g|exact|0.499915,0.501197,0.500558|0.514114,0.513915,0.515723|0.500558|0.514114|+2.708%|
|clustered|2g|ann|0.526722,0.525326,0.528520|0.530909,0.531294,0.530944|0.526722|0.530944|+0.801%|
|uniform|16m|exact|0.499441,0.498275,0.500621|0.507114,0.506929,0.508161|0.499441|0.507114|+1.536%|
|uniform|16m|ann|0.499738,0.498384,0.497586|0.511421,0.508394,0.507150|0.498384|0.508394|+2.008%|
|uniform|2g|exact|0.503134,0.497737,0.499047|0.510500,0.507470,0.510736|0.499047|0.510500|+2.295%|
|uniform|2g|ann|0.501972,0.500097,0.496022|0.504805,0.507338,0.510449|0.500097|0.507338|+1.448%|
<!-- FINAL_E_END -->

## 复现入口

以下命令在 Linux/WSL 仓库根目录执行，使用项目内受版本管理的构建入口；CTest目标包含新回归，无需忽略目录中的实现脚本。

```sh
cmake -S . -B work/final-review -DCMAKE_BUILD_TYPE=Release
cmake --build work/final-review -j2
ctest --test-dir work/final-review --output-on-failure
cmake --build work/final-review --target benchmark_cpu -j2
```

可单独复现新的C++边界测试。两个 companion 测试文件只在显式宏下提供 main，存储测试本身含 main：

```sh
g++ -std=c++20 -O2 -UNDEBUG -DMVDB_FINAL_FLOAT_STANDALONE tests/final_float_regression.cpp src/sql.cpp -o /tmp/final-float
/tmp/final-float
g++ -std=c++20 -O2 -UNDEBUG -DMVDB_FINAL_QUERY_STANDALONE tests/final_query_regression.cpp src/engine.cpp src/sql.cpp src/storage.cpp src/simd.cpp -lz -o /tmp/final-query
/tmp/final-query
g++ -std=c++20 -O2 -UNDEBUG tests/final_storage_regression.cpp src/storage.cpp -lz -o /tmp/final-storage
/tmp/final-storage
MVDB_USE_ODIRECT=1 /tmp/final-storage
```

`benchmark_cpu` 对已装载数据库的可丢弃副本与每行一条查询的SQL文件运行：`work/final-review/benchmark_cpu DATA_COPY QUERIES_SQL exact 12582912`。它固定完整预热并计算5遍平均，每个版本用同一批SQL和独立数据库副本；最终对照另做交错重复，不能在同一时段运行其他CPU基准。

完整容器测量使用 `tests/benchmark_cgroup.py`，固定种子20261109，10,000行128D、1,000查询、16类过滤，分别构建代码基线与新版镜像，交错运行三次单轮采集：

```sh
python tests/benchmark_cgroup.py --image IMAGE --phase LABEL --rounds 1 --memories 16m 2g --distributions clustered uniform --projects write exact ann --output RUN_JSON
```

每个版本三份单轮文件合并后必须正好36行（repeat需规范为1..3且保留原文件）；也可以用 `--rounds 3` 得到36行，但执行顺序应如实声明，不能声称交错。Docker Desktop宿主设备含镜像启动读、容器shell与系统调度差异，不等同官方独立数据盘环境。原始B=read_bytes+write_bytes，不能写成只统计数据文件或累计逻辑页写入。

## 补充逐项对照

72项主矩阵后，在另一连续窗口对每个模块执行3轮关闭对照，共24个独立容器项目。下表与主矩阵新版三轮中位比较；窗口不同，不能把小幅差异或无关查询的变化当作因果收益。每个样本正确性均通过。

no-storage保留旧帧与旧sync_all，no-parse同时关闭新浮点和输入解析；no-dictionary只关闭切片路由，仍构建并预算记账读缓存；no-prefix关闭早停。后两者隔离执行策略，不隔离缓存构建M。

|关闭模块|分布|阶段|T旧开关/全开(s)|B旧开关/全开(MiB)|M旧开关/全开(MiB)|
|---|---|---|---|---|---|
|no-storage|clustered|exact|0.697675 / 0.704653|8.355469 / 8.355469|17.425781 / 17.304688|
|no-storage|clustered|write|0.529961 / 0.507564|7.093750 / 7.093750|14.992188 / 14.847656|
|no-parse|clustered|exact|0.706554 / 0.704653|8.355469 / 8.355469|17.316406 / 17.304688|
|no-parse|clustered|write|0.534002 / 0.507564|7.093750 / 7.093750|14.742188 / 14.847656|
|no-dictionary|clustered|exact|0.744428 / 0.704653|8.355469 / 8.355469|17.453125 / 17.304688|
|no-dictionary|uniform|exact|0.792817 / 0.742110|8.355469 / 8.355469|17.261719 / 17.253906|
|no-prefix|clustered|exact|0.765025 / 0.704653|8.355469 / 8.355469|17.312500 / 17.304688|
|no-prefix|uniform|exact|0.816217 / 0.742110|8.355469 / 8.355469|17.378906 / 17.253906|

|关闭模块|分布|轮|阶段|T秒|B字节|M字节|
|---|---|---|---|---|---|---|
|no-storage|clustered|1|write|0.5435142820001602|7438336|15720448|
|no-storage|clustered|1|exact|0.6995305680038655|8761344|18272256|
|no-storage|clustered|2|write|0.5299611860000368|7438336|15859712|
|no-storage|clustered|2|exact|0.6796607010001026|8761344|18120704|
|no-storage|clustered|3|write|0.5127395530003014|7438336|15630336|
|no-storage|clustered|3|exact|0.6976750440021533|8761344|18583552|
|no-parse|clustered|1|write|0.5604281619998801|7438336|15458304|
|no-parse|clustered|1|exact|0.7116076839952257|8761344|18153472|
|no-parse|clustered|2|write|0.5303526669995335|7438336|15400960|
|no-parse|clustered|2|exact|0.6946953019796638|8761344|18157568|
|no-parse|clustered|3|write|0.5340024659999472|7438336|15618048|
|no-parse|clustered|3|exact|0.7065542419964004|8761344|18407424|
|no-dictionary|clustered|1|exact|0.7444283829986489|8761344|18571264|
|no-dictionary|uniform|1|exact|0.7985939729937854|8761344|18235392|
|no-dictionary|clustered|2|exact|0.7390010369986157|8761344|18300928|
|no-dictionary|uniform|2|exact|0.7816854499992587|8761344|18100224|
|no-dictionary|clustered|3|exact|0.7499337730100706|8761344|18194432|
|no-dictionary|uniform|3|exact|0.7928174219987341|8761344|18100224|
|no-prefix|clustered|1|exact|0.7891560450111683|8761344|18522112|
|no-prefix|uniform|1|exact|0.8162170640057411|8761344|18354176|
|no-prefix|clustered|2|exact|0.7508377730018765|8761344|18108416|
|no-prefix|uniform|2|exact|0.7640448570068656|8761344|18223104|
|no-prefix|clustered|3|exact|0.76502520700069|8761344|18153472|
|no-prefix|uniform|3|exact|0.8283869030019559|8761344|18104320|

### 10k谓词饥饿专项

固定128D，16组、每组625行；向量仅第0维为100×组号+组内序号/1000，其余0。末组前24行是tiny，倒数第二组前150行是far，其余common。初始手动NPROBE=5，仅用于这个诊断，正式主基准仍禁止外部校准；各标签32条近0查询，以Exact结果为真值。

|版本|匹配总行数|Recall@10|足额K比例|实际探测桶均值|32条ANN往返总ms|
|---|---|---|---|---|---|
|fixed|24|0.0|0|5|2.412541|
|fixed|150|0.0|0|5|2.568434|
|adaptive|24|1.0|1|0|2.840911|
|adaptive|150|1.0|1|57|5.757498|

固定探测返回空集虽更快，但不符合足额返回要求；扩探57桶修复该样本，需要额外时间。它不是所有偏斜分布Recall的证明。新增回归另覆盖冷页、AND交集、DML/回滚失效、通用堆与外部排序路径。

### 原生完整负载与低内存

`tests/benchmark_local.py --phase after --binary work/build/mvdb_local --automatic-ann --default-budget`完整运行通过；clustered/uniform各10k×128、Exact/ANN各1000查询；无外部NPROBE命令。CPU与容器主矩阵完成后运行，不干扰其计时。

clustered: {"resolved_budget_bytes": 10485760, "chosen_nprobe": 5, "T_query_seconds": 0.5411315870060207, "acceptance": {"exact_all_ordered_ids_match": true, "ann_recall_at_least_098_each_group": true, "ann_recall_at_least_0985_each_group": true, "no_external_nprobe_commands": true, "normal_batch_undo_spill_zero": true, "index_undo_spill_zero": true, "insert_active_wal_at_most_600_bytes_per_row": true, "no_checkpoint_snapshot_directories": true}}

uniform: {"resolved_budget_bytes": 10485760, "chosen_nprobe": 57, "T_query_seconds": 0.9213690469869107, "acceptance": {"exact_all_ordered_ids_match": true, "ann_recall_at_least_098_each_group": true, "ann_recall_at_least_0985_each_group": true, "no_external_nprobe_commands": true, "normal_batch_undo_spill_zero": true, "index_undo_spill_zero": true, "insert_active_wal_at_most_600_bytes_per_row": true, "no_checkpoint_snapshot_directories": true}}

|预算MiB|最大单SQL秒|建索引<0.3s|ANN路径/桶数|
|---|---|---|---|
|4.0|0.2116960889998154|True|{'ivf': 1000}/[5]|
|8.0|0.20774144500001057|True|{'ivf': 1000}/[5]|
|10.0|0.20922062199997526|True|{'ivf': 1000}/[5]|
|16.0|0.21253069099998356|True|{'ivf': 1000}/[5]|

### 证据归档硬校验

历史10轮/31单元逐对象及紧凑字节串锁定；仅增加1轮和4单元，计11轮/35单元、70条独立证据路径。两份文件130165字节，SHA256 `6f4dd6bb3d28c789cd7f950290136a866aeb5407a377dcaa80fe7e37a3609f50`。新提示由用户批准的同一请求完整原段分组构成，没有虚构新的用户回复。

|硬校验|结果|
|---|---|
|r10_conversations_locked|True|
|r10_units_locked|True|
|no_external_or_forbidden_words|True|
|all_ids_and_pointers_valid|True|
|all_verbatim_substrings|True|
|all_within_240_chars|True|
|all_sha256_valid|True|
|size_within_limit|True|
|both_files_identical|True|

历史第7轮followup原本没有SHA字段；依照历史对象不变要求保留，全部存在的13个文本SHA均验证通过。新增字段不存在外部证据路径或历史提交引用，全部35个原文来源为可定位的精确子串，长度均<=240字符。辅助组装和测量中间文件未纳入Git。
