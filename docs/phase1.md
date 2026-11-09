# Phase 1: local engine and verification

Historical baseline only. See phase2.md for the current single-vector schema,
compact WAL/Undo/checkpoint, Top-K and SIMD behavior.

This is a Linux x86-64 C++20 engine, not an implementation of the unpublished
competition process protocol. No official handshake, error codes, or settings
are claimed. The local executable reads SQL lines and writes JSON, flushing
success and error replies. All diagnostics go to stderr. `mvdb_core` itself
has no stdin/stdout dependency; `Engine::execute` returns typed results with
a spill-backed row cursor.

## Build and run

From WSL Ubuntu at `/mnt/d/clone/seekdb-2026`:

```sh
bash tools/validate.sh
# Or run the registered complete suite:
ctest --test-dir work/build --output-on-failure
```

Requires CMake >=3.20, GCC with C++20 and target attributes, Python 3 standard
library, Linux O_DIRECT-capable data filesystem. Tests use a separate
`/tmp/test_data/<case>` for each scenario and verify that each deletion target
is its immediate child. The checked-in public fixtures are never rewritten.

## Storage and transaction boundaries

- Fixed 16KiB direct-I/O pages; one bounded LRU BufferPool for row metadata,
  blob pages, vector pages, primary index, bitmap, and IVF posting pages.
- BIGINT -> uint32 SlotId primary B+ tree, 127 keys per node; splitting and
  lookup are paged. Deletion removes leaf entries without merging sparse nodes.
  There is no all-table in-memory key map.
- Single-version row slots, separate variable-length blobs, and aligned vector
  payload. 128D float32 occupies 512 bytes, exactly 32 vectors per payload page.
  Row metadata and validity bits are separate; vector query kernels read a
  borrowed pinned payload span without making a second vector matrix.
- Undo is disk-backed from its first record, with reverse length trailers,
  so rollback needs no growing in-memory offset stack. Before every mutating
  statement Engine saves the undo file position. A failure undoes only that
  statement. Explicit ROLLBACK synchronously restores data, B+ nodes,
  vector pages, valid bits, bucket membership, metadata and allocation counters.
- No MVCC, versions, background workers or asynchronous compaction.
- WAL uses length + checksum frames, begin/page-after-image/commit/abort records.
  Each page image is durable before a dirty page may be evicted. Statement undo
  appends compensating redo images; aborted transactions are ignored on recovery.
- Data files are derived from the authoritative WAL. Startup streams committed
  transactions into a freshly rebuilt data directory. A killed recovery can
  restart from the unchanged WAL, including multiple successive interruptions.
  Incomplete/uncommitted tails are truncated before subsequent appends.
- Buffered WAL/undo and spill files use bounded sync/drop-cache windows. This
  reduces cache accumulation; fadvise is advisory and is not a cgroup guarantee.

## Vector and filtering behavior

`CREATE VECTOR INDEX vi ON t(v)` builds up to eight IVF centroids using three
streaming Lloyd iterations with deterministic initial samples. Each bucket stores
only 4-byte SlotIds, plus a count/header and ordinary page padding. Payload is
stored exclusively in the vector column file (durability logs naturally contain
before/after images). Insert/delete/update synchronously update membership;
statement/transaction undo restores it through the same page mechanism.

Local development settings (not official protocol):

```sql
SET SEARCH_MODE = exact;
SET SEARCH_MODE = ann;
SET NPROBE = 8;
```

ANN is used only for an indexed ascending distance ORDER BY with LIMIT. For each
4096-slot window, selected bucket IDs form a bitmap, which is intersected with
validity and ordinary WHERE conditions before reading payload. Payload is then
consumed in SlotId order. One preferred vector page stays pinned so ordinary
metadata reads cannot cause repeated reads of that page. Other vector columns
are consumed immediately to avoid exhausting a tiny pool.

Ordinary predicates currently scan scalar pages into this bounded bitmap; there
is no secondary hash/B+ index yet and no unbounded in-memory scalar index.
Primary-key equalities do use the paged primary index to find the SlotId.

Runtime AVX2 detection chooses the AVX2 or scalar L2 kernel. The SIMD kernel widens
float32 to float64 before subtraction/accumulation, so finite float32 extremes do
not overflow intermediate float32 squares. L2 output includes sqrt.

## SQL and limits

Implements CREATE TABLE, INSERT (including reordered full column lists), SELECT,
UPDATE, DELETE, comparison operators, AND, ORDER BY, LIMIT, L2_DISTANCE and
BEGIN/COMMIT/ROLLBACK. Exactly one BIGINT PRIMARY KEY is required. ASCII
identifiers are case-insensitive, at most 63 bytes; at most 64 columns, schema
definition below 4096 bytes, vector dimension 1..4096. INSERT requires all columns;
NULL/defaults, joins, aggregates, expressions and other general SQL are outside
this phase. Strings use standard doubled-quote escaping, not MySQL backslash
escape extensions. UTF-8 is validated; VARCHAR lengths count Unicode scalar
values, and validated UTF-8 byte ordering equals code-point ordering. Signs,
scientific vector values, whitespace and mixed keyword case are accepted.
Nonfinite, overflowing or wrong-dimension vector values are rejected.

External merge sorting and result spilling avoid an all-table result heap;
sort ties resolve by ascending primary key. Individual statements/rows must
fit their workspace reservation. MemoryBudget controls owned cache frames and
reserved parsing/training/sort workspaces; its diagnostics are not total process
RSS or cgroup memory. Stack, runtime/allocator overhead, and kernel accounting
still require separate Linux cgroup measurement.

## Tests and remaining performance work

`tests/verify.py` reads manifest and fixtures directly and compares affected_rows,
columns, ordered rows or duplicate-preserving unordered rows. Integers are checked
as integers, floats use abs_tol=rel_tol=1e-5, and `{}` still requires success.

Extra regressions cover: partly applied multi-row INSERT errors; explicit
transaction read-your-writes and rollback; overflowing BIGINT, Unicode, quote and
numeric syntax; B+ splits/eviction with 900 rows and a 64KiB cache budget; synchronous
IVF updates and undo; 128D payload scan with 256 rows and at most eight payload
page reads; three vector columns under a tiny pool; overlong identifiers and
reopen; real SIGKILL with committed and losing transactions; two SIGKILLs during
recovery; repeated successful replay; incomplete log tail followed by new writes.

This is a correctness baseline. Full-page WAL plus disk undo amplify writes:
the 900-row regression produces approximately 129MiB WAL. No checkpoint, WAL
reclamation, blob space reclamation or B+ node merge is implemented. Reopening
replays complete history. Sorting currently spills even small Top-K queries.
These are material T/B optimization tasks, not hidden background work.

No claim is made about the official over-memory admission test, cgroup
memory.peak/io.stat score, real power-loss/storage-error handling, or Recall@10
at a reduced nprobe. nprobe=8 visits all current buckets and is the tested exact
reference; lower probes require representative recall calibration. Two
simultaneous processes on one directory are unsupported, as is concurrency
inside Engine.
