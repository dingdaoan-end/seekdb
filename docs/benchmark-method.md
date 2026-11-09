# Local benchmark method

This harness measures the **local raw-SQL-line / JSON-line adapter**, not the
competition handshake. The workload below is the supplied workload; no unseen
section 3.3 or external benchmark specification has been independently verified.

## Reproduction

Phase 3 compares the preserved phase-2 binary and the current binary with the
same automatic ANN workload. Neither run issues `SET NPROBE`; external probe
calibration is completely bypassed. Run sequentially in WSL using the existing
NumPy environment:

```bash
cd /mnt/d/clone/seekdb-2026
/tmp/mvdb-benchmark-venv/bin/python -u tests/benchmark_local.py \
  --phase before --automatic-ann --binary work/phase2-baseline/mvdb_local \
  --output work/benchmark-phase3-before.json > work/benchmark-phase3-before.log 2>&1
/tmp/mvdb-benchmark-venv/bin/python -u tests/benchmark_local.py \
  --phase after --automatic-ann --binary work/build/mvdb_local \
  --output work/benchmark-phase3-after.json > work/benchmark-phase3-after.log 2>&1
```

The baseline's unset probe setting visits all 64 lists at N=10,000. The current
engine owns probe selection and route decisions. Recommendations and list counts
are read from engine metrics, never set by the harness. Each summary includes
`routing_counts`; older binaries without route metrics report `not_emitted`.
The command trace records command kinds and the harness rejects any external
`SET NPROBE` before transmission in automatic mode. Both dataset input hashes
and the phase-independent workload configuration hash must match for comparison.
Phase-3 files do not replace the historical phase-2 JSON/log reports.

The commands below reproduce the historical, externally calibrated protocol;
omitting `--automatic-ann` deliberately preserves its behavior.

Run under Linux/WSL with Python 3 and NumPy. The benchmark's Python/NumPy memory
is outside the engine's memory budget and engine RSS measurement.

```bash
python3 -m venv /tmp/mvdb-benchmark-venv
/tmp/mvdb-benchmark-venv/bin/pip install numpy
cd /mnt/d/clone/seekdb-2026
/tmp/mvdb-benchmark-venv/bin/python -u tests/benchmark_local.py \
  --phase before --binary work/baseline/mvdb_local \
  --output work/benchmark-before.json
/tmp/mvdb-benchmark-venv/bin/python -u tests/benchmark_local.py \
  --phase after --binary work/build/mvdb_local \
  --output work/benchmark-after.json
```

Run the two full measurements sequentially to avoid CPU and I/O interference.
The before binary is preserved; its SHA-256 is included in the report. The
default engine budget is 32 MiB. A 100-row insert contains roughly 160 KiB of
SQL text; the parser's transient representations make the 64 KiB regression-test
budget unsuitable for this workload. Use the same budget for both binaries.

A quick protocol and correctness smoke run:

```bash
/tmp/mvdb-benchmark-venv/bin/python tests/benchmark_local.py \
  --phase before --binary work/baseline/mvdb_local \
  --output work/benchmark-smoke.json --n 100 --queries 4 \
  --calibration-queries 4
```

Smoke runs are explicitly labeled `full_workload: false`; they are not evidence
for the full workload. Defaults are N=10,000, D=128, 1,000 heldout queries, and
40 additional calibration queries. The only writable database roots are
`/tmp/mvdb_benchmark/{before,after}/{clustered,uniform}`. The harness resolves
and verifies the complete target path before removing a prior dataset. It does
not use the regression directory `/tmp/test_data`.

## Data and correctness

The fixed seed is 20261109. Each dataset is generated independently from a
deterministic NumPy generator; uniform uses seed+10000. Both stored vectors and
query vectors are float32. Nine significant decimal digits round-trip float32
through SQL text. A SHA-256 covers stored vectors, categories, heldout vectors,
and heldout filter categories, plus the schema and SQL workload signature,
allowing before/after input and schema identity checks.

* Clustered: 16 random 128-dimensional centers drawn from Normal(0,3), plus
  independent Normal(0,0.35) noise, stored as float32.
* Uniform: independent Uniform(-1,1) components, stored as float32.
* Schema: `id BIGINT PRIMARY KEY`, `category VARCHAR(32)` with 16 possible values
  (`cat-00` through `cat-15`, tested with quoted equality predicates),
  `content VARCHAR(128)`, and `embedding VECTOR(128)`. Content is a deterministic short
  string (`content-000001` etc.); VARCHAR(128) is the declared capacity, not
  padding to 128 characters.
* Exactly 100 inserts of 100 rows for each default-size dataset, each in
  autocommit mode. No explicit transaction surrounds the load.
* Exactly 1,000 measured queries per search mode: 500 without a predicate,
  followed by 500 with `category = value`. Both select the nearest 10 rows by
  L2 distance, breaking ties by ascending primary key.

An independent oracle converts the generated float32 values to float64,
subtracts query components, and sums squared differences with NumPy in chunks
of 1,024 rows. It does not call an engine distance function, engine exact
search, ANN code, or engine-provided expected answers. Squared L2 has the same
ordering as L2; lexicographic sorting by (distance, primary key) defines ties.
The filter is applied before finding neighbors. Every measured exact and ANN
query is compared to its oracle result. Recall@10 is set overlap divided by
the number of available oracle neighbors (10 in the full workload); exact
mode additionally requires the complete ordered ID list to match. Per-query
IDs, recall, ordering checks, latency, and engine diagnostics are retained.

## Probe selection

For phase 3, `--automatic-ann` skips the entire calibration loop, calibration
oracle, and manual probe command. Calibration query timings are absent (the
empty calibration phase has only snapshot overhead). The engine's internal
calibration is the sole recommendation source. Heldout queries and their
independent float64 oracle are unchanged. Automatic runs require mean heldout
recall >=0.985 in each of the 500-query groups; this is reported separately
from engine calibration and exceeds the supplied 0.95 official threshold.
Normal 100-row inserts must spill zero undo bytes; current-engine index builds
must also spill zero undo bytes. Current-engine post-close storage must contain
no `snapshot.*` directories, while real `checkpoint.A`/`checkpoint.B` files
remain included in the footprint. The <=600-byte/row insert WAL requirement
continues to apply to the current engine on the full 10,000-row workload.

The remainder of this section describes historical phase-2 external calibration.

Calibration vectors are generated separately after heldout vectors and are
never included in reported query throughput. The default 40 calibration
queries have 20 unfiltered and 20 category-equality queries. Current runs test
every integer probe count in ascending order from 1 through the binary's IVF
list count. The preserved binary uses min(N,8); the optimized binary uses 64
for N>=4096 and min(N,8) otherwise. Centroids are packed in the optimized format,
so file size divided by page size cannot be used to infer the number of lists.
Testing stops at the first probe count with mean recall >=0.98 in **each**
calibration group. If no setting passes, the largest probe setting is used and
all calibration outcomes remain visible. The selected setting is then fixed
for the 1,000 heldout queries. Selection does not inspect heldout recall.

Heldout mean recall must separately reach 0.98 for filtered and unfiltered
queries. The process returns code 2 if either heldout ANN group fails or any
exact ordered ID result differs. Calibration success does not guarantee
heldout success; the report preserves an honest failure rather than changing
the setting against heldout answers. With small datasets, engine list counts
can be smaller than the probe request; the engine clamps actual list use.

Exploratory runs with numeric categories are archived under
`work/benchmark-numeric-*` and do not qualify as the supplied workload. The
final `benchmark-before.json` and `benchmark-after.json` use VARCHAR(32)
categories and sequential integer calibration probes.
The compression-only smoke report uses N=10,000 but just two heldout and two
calibration queries. Its clustered recall check failed from that tiny calibration
sample; it is not recall evidence. Both final full reports independently pass
the 1,000-query correctness and recall checks with 40 calibration queries.

## Timing, I/O, and memory

Each dataset runs: create, write, exact queries, build IVF index, calibration,
ANN heldout queries, close. Query latency is the local process round trip from
writing pre-generated SQL to receiving JSON and its stderr metrics. A dedicated
thread drains stderr continuously, so verbose diagnostics cannot fill a pipe
and deadlock the child. Progress is printed every 100 queries; the full JSON
report is atomically replaced after every phase and each progress checkpoint.

`write.wall_seconds` includes constructing and sending inserts;
`write.request_seconds` sums only round trips. Exact and ANN summaries report
request seconds, QPS, mean/p50/p95 latency, and vector-page reads. `T_query_seconds`
is the sum of exact and ANN request seconds; it excludes oracle generation,
calibration, index creation, and close. Index-build and graceful-close times
are separate. The close time includes deferred checkpoint/flush work; it must
be included when comparing end-to-end write costs.

Optimized runs also require active WAL logical bytes immediately after the
insert phase, divided by N, to be at most 600 bytes per row. This snapshot is
taken before index construction and includes the small table-creation record.
The original baseline is exempt from this optimization acceptance target.

The benchmark does not drop the OS page cache. Exact queries precede index
creation and ANN, so these are warm-process/cache results rather than cold-disk
latency. Report this ordering when interpreting speedups.

Snapshots before and after write, exact, index, calibration, and ANN include:

* `/proc/PID/io`: actual process `read_bytes`, `write_bytes`, `rchar`, `wchar`,
  operation counts, and cancelled writes. Differences give phase totals.
  `read_bytes`/`write_bytes` describe physical I/O attributed to the process;
  `rchar`/`wchar` are logical syscall bytes, including cached accesses.
* `/proc/PID/status`: engine VmRSS and VmHWM, in bytes. Per-request diagnostics
  separately report the engine's budget peak and vector-page reads. RSS and
  budget accounting are different measurements.
* Every data-directory file's logical `st_size` and allocated `st_blocks*512`,
  including WAL before close and after close. These are footprint snapshots,
  never estimates of cumulative physical writes.

For generation-based checkpoints, symlink aliases (including `root/wal`) are
listed separately and excluded from total bytes, preventing duplicate counting
of a WAL generation. Working files under `data/`, checkpoint snapshots, and WAL
generations have separate subtotals; the active-WAL view is explicitly
non-additive. Baseline storage has no WAL alias. Both final reports directly
record these subtotals and phase I/O deltas.

When present, optimized engine cumulative counters are captured in snapshots
and phase deltas: `data_page_write_bytes` is successful PAGE-sized O_DIRECT
pwrite request bytes for working data, **not device physical-write bytes**;
`undo_spill_bytes` and `wal_bytes_written` are retained separately. Raw
per-request metrics (including `spill_bytes`) remain in query records. They
complement `/proc` process I/O rather than replacing its physical-byte fields.

The last `/proc` snapshot is immediately before closing stdin. After process
exit `/proc/PID` no longer exists. `wait4` captures final child resource usage,
including close/checkpoint: maximum RSS, user/system CPU time, and block input
and output operation units. These block units are not file-specific byte counts.
The raw `wait4` maximum RSS may reflect pre-exec fork memory inherited from the
NumPy parent and is not the engine-memory comparison metric. Use the engine's
own `/proc` VmHWM instead; the baseline has the pre-close value, while the
optimized adapter can emit its own post-close VmHWM in final metrics.
The process I/O values cover **all** files touched by the engine, including
temporary spills; they do not identify cumulative physical writes to `/data`
alone. Directory growth, allocated bytes, and logical syscall bytes cannot
substitute for that missing file-specific physical-write measurement.

When the adapter emits `FINAL_METRICS` after a successful graceful close, the
report captures those final engine counters and their deltas from immediately
before close. This includes deferred working-data pwrite and checkpoint work.
Final metrics also retain the adapter's own post-close `/proc/self/io` physical
read/write byte counts and VmHWM when emitted, covering shutdown without the
fork-RSS ambiguity or loss of `/proc` access after exit.
The preserved baseline lacks final engine counters; final `wait4` resource
usage and post-close real-file sizes remain available for both binaries.
