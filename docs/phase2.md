# Phase 2: admission semantics and measured performance

Historical phase2 baseline. The current design and Docker/automatic-ANN behavior
are described in the repository README.md and phase3 benchmark report.

This document supersedes the implementation details in the historical phase1.md.
The public competition process protocol/error-code mapping is still unavailable;
the local adapter returns explicit failure for unsupported SQL, and does not
invent official error codes or handshake bytes.

## Admission and transactions

Schema validation requires exactly one BIGINT primary key and at most one VECTOR
column. A rejected CREATE leaves no usable table behind. The previous three-column
test is replaced by three independent tables sharing a 64KiB BufferPool, with
interleaved inserts, queries, updates, deletes and rollback.

Single-version slots and statement savepoints remain. Undo stores changed before-
byte ranges in a reserved stack, up to256KiB at ordinary budgets, with a smaller
adaptive reservation for the 64KiB regression. Only excess bytes spill to disk.
Large spills synchronize/drop cache every64KiB; normal100-row benchmark batches
fit the memory stack. Tests separately force a spill and reverse restoration.

## Compact WAL and checkpoint authority

WAL physical deltas identify a file/page and changed byte runs, replacing 16KiB
after-images. Nearby changed runs coalesce. Bounded REDO batches use DEFLATE with
a32KiB dictionary,16KiB batches and a charged320KiB compressor workspace. Each frame and each
committed transaction carry IEEE CRC32. Commits synchronize WAL before reply;
long transactions also bound dirty log cache with64KiB sync/drop windows.

Working data may contain uncommitted evictions and is never the sole authority.
Recovery restores a durable snapshot and applies only committed, CRC-validated
transactions. It uses bounded direct-I/O copy buffers and releases consumed WAL
cache in both verification and replay passes. Repeated recovery interruption is
idempotent.

At normal explicit close or64MiB WAL threshold, checkpoint:

1. Flush working pages and copy them to a new snapshot through16KiB direct I/O.
2. Sync snapshot files/directory, new empty WAL and pending manifest.
3. Atomically publish and sync CURRENT selecting the new snapshot/WAL generation.
4. Switch the convenience `wal` symlink and retire the previous generation.

The authority is CURRENT, not the symlink. Crash tests kill at all four stages.
Explicit close propagates checkpoint failures and is idempotent. Destruction is
best effort. Phase1 nonempty WALs require migration and are rejected explicitly;
they are not silently reinterpreted as the new format.

## Query execution and SIMD

Budget-fitting ORDER BY/LIMIT K (K<=4096) retains only K rows in a max heap and
returns in-memory typed rows. It creates no sorting or result spill files. Large
K or unbounded results retain external merge sorting. Sort direction follows SQL;
ties always use ascending primary key.

Distance scans use eight-wide float32 AVX2; FMA is checked separately. Vector
metadata flags extreme components at write time, and queries are checked once.
Extremes/tiny sums use float64 fallback. SIMD safely rejects candidates clearly
worse than the current heap threshold; potential winners are recomputed in
float64 before entering the heap, preserving strict near-tie ranks. Returned
distances are Euclidean distances, including sqrt.

Ordinary non-vector columns have paged64-bit hash sketches. Equality predicates
reject nonmatching hashes while forming a bounded4096-SlotId bitmap, then verify
actual values to handle hash collisions. All sketch writes use the same WAL/Undo
path, including updates and rollback. There is no unbounded whole-table hash map.
Empty-predicate scans avoid decoding ordinary blobs; row metadata reads copy only
the small slot record rather than an entire16KiB page.

IVF retains only4-byte SlotIds per posting. N>=4096 selects64 centroid buckets;
smaller datasets use up to8. Centroids are packed, not one16KiB page each. Candidate
bucket, visibility and ordinary-filter bitmaps are intersected before consuming
payload pages in ascending SlotId order. Small-memory training streams one bucket
at a time when full centroid accumulators cannot fit.

Without workload calibration, automatic probing visits every bucket for a safe
recall reference. The benchmark calibrates every integer probe count on disjoint
queries, selects the smallest reaching mean Recall@10>=.98 in BOTH filter groups,
then sets that count as the session query default. The1,000 measured held-out
queries are checked independently; results never imply a universal recall
guarantee for arbitrary data distributions.

## Reproduction and metrics

```sh
cmake -S . -B work/build -DCMAKE_BUILD_TYPE=Release
cmake --build work/build -j2
ctest --test-dir work/build --output-on-failure
/tmp/mvdb-benchmark-venv/bin/python tests/benchmark_local.py --phase before --binary work/baseline/mvdb_local --output work/benchmark-before.json
/tmp/mvdb-benchmark-venv/bin/python tests/benchmark_local.py --phase after --binary work/build/mvdb_local --output work/benchmark-after.json
```

The measured environment is WSL Ubuntu, GCC15.2, Python3.14.4 and NumPy2.5.4.
The harness requires NumPy for an independent float64 ground truth. Default seed
is20261109. Each run uses exactly10,000x128 float32 records,100 auto-commit batches
of100, and1,000 LIMIT10 queries per exact/ANN mode, half unfiltered and half string
category equality. There are16 categories; a filtered query selects about1/16
of rows, while50% of queries have filters. The schema is BIGINT primary key,
category VARCHAR(32), content VARCHAR(128), embedding VECTOR(128).

Each distribution (clustered/uniform) starts independently. Calibration and index
build/close times are recorded separately. Only one benchmark process runs at a
time; this is a local single-run measurement, not the official hidden harness.
No OS cache dropping is used; exact runs before ANN. Full JSON includes binary
and input SHA256 fingerprints and every query's measured result/recall.

`/proc/PID/io` is sampled on the database process, not the Python driver. It
includes WAL, Undo, data and temporary-file I/O. The new adapter also reports its
own `/proc/self/io` and VmHWM after close/checkpoint. Directory st_size and
st_blocks*512 are footprint measurements, not cumulative physical writes; aliases
are excluded. `data_page_write_bytes` counts successful working-data direct-write
request bytes, excluding snapshot copies; it is not device/cgroup io.stat B.
wait4.ru_maxrss can include pre-exec inherited driver pages, so reported M uses
engine VmHWM instead. Checkpoint overhead is included in final process I/O and
reported separately in time. Calibration I/O can be excluded using phase deltas.

The repository contains historical diagnostic numeric-category runs only in
ignored work/ archives; they are excluded from final VARCHAR(32) comparisons.
MemoryBudget is not total process/cgroup memory. Runtime/allocator overhead and
kernel accounting remain distinct from reserved engine workspace.
