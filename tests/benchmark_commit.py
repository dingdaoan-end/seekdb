"""Paired commit microbenchmark: fixed 20MiB isolates sync from default-budget change."""
import argparse
import json
from pathlib import Path
import shutil
import statistics
import time
from benchmark_local import Client, SCHEMA, make_data, vector_literal

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--before', type=Path, required=True)
p.add_argument('--after', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
data, cats, *_ = make_data('clustered', 10000, 2, 20261109, 2)
sql = []
for first in range(0, 10000, 100):
    rows = [f"({i+1},'cat-{int(cats[i]):02d}','content-{i+1:06d}','{vector_literal(data[i])}')"
            for i in range(first, first+100)]
    sql.append('INSERT INTO t VALUES ' + ','.join(rows) + ';')
report = {'kind': 'supplementary paired write-only microbenchmark', 'budget_bytes': 20971520,
          'records': 10000, 'batches': 100, 'seed': 20261109,
          'timing': '100 pre-encoded SQL round trips; excludes SQL encoding/index/query/close',
          'not_default_budget_benchmark': True, 'runs': []}
for repeat in range(3):
    for name in (('before', 'after') if repeat % 2 == 0 else ('after', 'before')):
        root = Path('/tmp/mvdb_benchmark') / ('commit_' + name)
        assert root.resolve().parent == Path('/tmp/mvdb_benchmark')
        shutil.rmtree(root, ignore_errors=True)
        client = Client(getattr(a, name).resolve(), root, budget=20971520)
        try:
            _, _, start = client.sql(SCHEMA)
            t = time.perf_counter()
            for statement in sql:
                _, _, end = client.sql(statement)
            elapsed = time.perf_counter() - t
        finally:
            closed = client.close()
        row = {'version': name, 'repeat': repeat + 1, 'seconds': elapsed,
               'process_write_bytes_after_close': closed['final_engine_metrics']['process_write_bytes'],
               'wal_sync_delta': end['wal_syncs']-start['wal_syncs'] if 'wal_syncs' in end else None}
        report['runs'].append(row)
        print(json.dumps(row), flush=True)
report['median_seconds'] = {name: statistics.median(r['seconds'] for r in report['runs'] if r['version'] == name)
                            for name in ('before', 'after')}
a.output.write_text(json.dumps(report, indent=2), encoding='utf8')
print(json.dumps(report['median_seconds']), flush=True)
