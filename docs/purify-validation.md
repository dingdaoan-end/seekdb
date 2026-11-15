# 输入解析、谓词短路及区域失效验证

以223ca3ac723f316836242ece272cfd9d63a52283为基线；仅推备用仓库。没有改动fast_topk、SIMD、TableReadCache构建时机、存储追加与10MiB预算公式。19项源码不变量核对通过。

## 实现与专项对照

输入使用保守string_view扫描，仅接受id/sql/op/search_mode白名单、平坦无转义形态。mode等未知键、转义、嵌套及可疑输入仍走原严格Parser。原始id文本和异常响应保持；SQL仍需在Engine的string接口处物化，零分配指解析阶段。30,000组种子变异及UTF8/非法数字/重复键/乱序键差分通过。

|1000次输入解析|旧分配|新分配|旧耗时ms|新耗时ms|
|---|---:|---:|---:|---:|
|flat_ping|3000|0|0.105921|0.067459|
|flat_short|6000|0|0.324231|0.127538|
|flat_vector_sql|12000|0|1.816890|0.984376|

多AND标量查询先检查Meta主键和惰性读取的BIGINT字段，再按原谓词执行完整行复核。所有字面量仍预先转换，混合VECTOR比较保留原路径；0/1谓词无新增分配。未实现或宣称完整的四级字典/短字符串代价规划。非选择性数值条件可能额外读取字段头。

|10k行、每行32768B VARCHAR在数值列之前；相同SQL|逻辑pin|推导blob请求|BufferPool页装载|三次CPU中位ms|
|---|---:|---:|---:|---:|
|冻结223ca3：payload>='' AND n< -10000|40020|30016|20040|68.230|
|本轮：相同多AND|30020|20016|10040|30.934|

blob请求由此无输出扫描的总pin减header、valid、Meta请求推导，不是独立物理I/O计数。页装载也不是设备read_bytes。相同单谓词两版本均为40020 pin/30016 blob请求；本轮全套测试后的额外测量为74.32→33.022ms，不能混用不同采样的CPU值。

区域丢弃原本已会废弃dirty帧且不写回。本轮真实修复为：先检查前缀内全部pins，再使任一页失效，避免后段被pin时已丢弃前段。新回归在旧版本失败、修复后通过；脏页丢弃、同名不同布局、邻近前缀隔离、旧句柄失效、Savepoint/事务回滚及SIGKILL恢复通过，废页pwrite为0次/0B。经用户确认，不新增DROP SQL，仍返回unsupported_sql。

## 全量验证

- 本机CTest12/12，22.46s；容器Dockerfile.test CTest12/12，22.00s（包含原9项及新增3项）。
- 官方二进制与只读Docker各20/20、95SQL；冻结R9聚类/均匀、Exact/ANN完整响应4000/4000相等。
- 默认Docker构建成功；真正DOCKER_BUILDKIT=0的Windows CLI构建另见work/purify-legacy-real.log。
- JSON8项校验见work/purify-validation.json，原27历史单元只修改编号及自身索引，前10轮提示词/跟进/SHA均不变。

## 同机交错三轮容器对照

每版本36个独立cgroup/数据卷，clustered/uniform ×16m/2g ×write/exact/ann ×3；版本顺序交错。表中各指标取三轮中位数。B为完整生命周期io.stat，M为memory.peak；Docker Desktop启动读也计入，不等于官方专用数据设备。查询T含1000次JSONL往返；准备开销计入B/M。

|分布|内存|项目|旧T s|新T s|旧B MiB|新B MiB|旧M MiB|新M MiB|
|---|---|---|---:|---:|---:|---:|---:|---:|
|clustered|16m|write|0.333921|0.311231|7.164|7.188|14.898|14.984|
|clustered|16m|exact|0.590022|0.532895|8.598|8.598|16.000|16.000|
|clustered|16m|ann|0.441232|0.431794|8.605|8.602|16.000|16.000|
|clustered|2g|write|0.321703|0.322029|7.094|7.094|14.859|14.855|
|clustered|2g|exact|0.513871|0.542315|8.355|8.355|17.363|17.371|
|clustered|2g|ann|0.421996|0.424491|8.355|8.355|17.320|17.289|
|uniform|16m|write|0.344924|0.323411|7.160|7.160|15.027|14.988|
|uniform|16m|exact|0.557444|0.528535|8.609|8.605|16.000|16.000|
|uniform|16m|ann|0.563519|0.562826|8.605|8.605|16.000|16.000|
|uniform|2g|write|0.350821|0.329524|7.094|7.094|14.852|14.766|
|uniform|2g|exact|0.551508|0.558797|8.355|8.355|17.426|17.387|
|uniform|2g|ann|0.546778|0.549910|8.355|8.355|17.426|17.258|

本地效率比按3.3.3的1ms/1MiB平滑常量与0.6/0.2/0.2权重计算；查询以同机冻结基线Exact为共同参考。下表是上述三轮各指标中位数的代入结果，并非正式判题端未知基线分数，也不预测Prompt或线上指标。

|分布|内存|查询模式|旧本地E|新本地E|相对变化|
|---|---|---|---:|---:|---:|
|clustered|16m|exact|0.500000|0.513525|+2.71%|
|clustered|16m|ann|0.528773|0.534265|+1.04%|
|clustered|2g|exact|0.500000|0.494564|-1.09%|
|clustered|2g|ann|0.519670|0.519099|-0.11%|
|uniform|16m|exact|0.500000|0.508578|+1.72%|
|uniform|16m|ann|0.498932|0.502304|+0.68%|
|uniform|2g|exact|0.500000|0.501972|+0.39%|
|uniform|2g|ann|0.500860|0.503807|+0.59%|

保留全部条件的正负变化，不将单次微基准外推为所有工作负载加速。未改变持久化格式；小幅B/M差异仍可能来自容器启动及页分配噪声。历史单元独立贡献不能从总分差严格推出；按用户要求移除原21至24，不保证重组后新增单元均获2分。

## 对2g聚类Exact负向结果的专项复测

完整矩阵保留0.513871→0.542315s负向结果；在无编译负载下再次交错三轮，同镜像同配置中位数为0.549322→0.531862s（-3.18%）。该项未复现稳定退化，反映宿主调度方差；不能用复测替换初测，也不能宣称所有配置零波动。原始六个样本位于work/purify-focus.log。

## 证据文件8项硬校验

```json
{
  "checks": {
    "1_historical_prompt_followup_sha_exact": true,
    "2_only_authorized_original_word": true,
    "3_scoring_history_27_units_fields_equal": true,
    "4_ids_37_and_74_independent_nonempty_pointers": true,
    "5_all_37_verbatim_source_substrings": true,
    "6_all_evidence_within_240_characters": true,
    "7_size_100000_to_112000": true,
    "8_identical_utf8_unique_keys_finite": true
  },
  "all_passed": true,
  "bytes_each": 111806,
  "sha256": "5c9f2b79ec05f92c7efb244937606e8b65f6acefab76af906896b27c21c9ad9f",
  "rounds": 11,
  "units": 37,
  "pointers": 74,
  "all_prompt_sha256_valid": true
}
```

辅助生成器、全量原始JSON、测量脚本和日志全部留在被忽略的work/，不纳入交付Git。
