# Round 6 官方协议与新公开用例修复实测

从官方f4d4e2665同步49个题包/协议文件，20组95条SQL。按官方JSONL实现ready、原样id、默认execute、ping、ann/approx、结构化error.code、向量数组、shutdown和VECTORDB_DATA_DIR；裸SQL兼容但客户端须消费ready。修复生产路径tmpfile落到只读/tmp的问题，改为数据目录内匿名文件；O_DIRECT在open或首次读写拒绝时回退并有界fadvise。INT映射BIGINT，单整数列推断主键。核实Storage.close本来已有回滚，新增显式close/EOF/索引计数回归验证而非虚构原来缺失。初次新增协议测试发现默认op未实现，修正后全套8/8通过。

官方来源：f4d4e26653af823a7601668802f80e374ad6643c。49个同步文件与该提交内容一致（仅Git换行规范化）。

|检查|实际结果|耗时|
|---|---|---|
|官方二进制协议|20/20 PASS|2.021s（用例耗时之和）|
|官方Docker协议（read-only）|20/20 PASS|24.597s（用例耗时之和）|
|公开SQL|95/95 PASS（20组）|0.867s|
|全量CTest|8/8 PASS|20.44s|

```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=/tmp/mvdb-benchmark-venv/bin/python
cmake --build work/build -j4
ctest --test-dir work/build --output-on-failure
python3 tests/verify.py --binary work/build/mvdb_local --repo . --group public
export VECTORDB_COMMAND='["/mnt/d/clone/seekdb-2026/work/build/mvdb_local"]'
python3 protocol_check/run.py --command "$VECTORDB_COMMAND" --output work/round6-protocol-native-final2
docker build -t minivectordb:round6 .
export VECTORDB_IMAGE=minivectordb:round6
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output work/round6-protocol-docker
```

## 最终CTest原始输出

```text
Test project /mnt/d/clone/seekdb-2026/work/build
    Start 1: budget_regression
1/8 Test #1: budget_regression ................   Passed    0.01 sec
    Start 2: storage_regression
2/8 Test #2: storage_regression ...............   Passed    2.35 sec
    Start 3: core_regression
3/8 Test #3: core_regression ..................   Passed    0.17 sec
    Start 4: simd_regression
4/8 Test #4: simd_regression ..................   Passed    0.01 sec
    Start 5: index_regression
5/8 Test #5: index_regression .................   Passed    2.75 sec
    Start 6: protocol_regression
6/8 Test #6: protocol_regression ..............   Passed    0.26 sec
    Start 7: all
7/8 Test #7: all ..............................   Passed    8.50 sec
    Start 8: dynamic_dml
8/8 Test #8: dynamic_dml ......................   Passed    6.28 sec

100% tests passed, 0 tests failed out of 8

Total Test time (real) =  20.44 sec
```

## JSON校验

```json
{
  "bytes": 110868,
  "sha256": "7c0bf83f81ef139f662c7bef3c98f55357628cea8639bc85d5887061e005ced5",
  "copies_identical": true,
  "conversations": 6,
  "learning_units": 17,
  "distinct_valid_evidence_pointers": 34,
  "strict_utf8_json": true,
  "duplicate_keys": 0,
  "nonfinite": 0,
  "auxiliary_generator_location": "ignored work/internal_tools/"
}
```

本轮没有提交或推送，未运行新Benchmark。官方检查器保持原样；本地docker.exe入口包装仅位于已忽略work目录。内部生成脚本未加入暂存区。
