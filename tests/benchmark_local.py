#!/usr/bin/env python3
"""Reproducible Linux/WSL local-adapter benchmark (not a competition protocol)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import resource
import select
import shutil
import subprocess
import threading
import time
import traceback

import numpy as np


SAFE_ROOT = Path('/tmp/mvdb_benchmark')
SCHEMA = 'CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(128), embedding VECTOR(128));'
WORKLOAD_SIGNATURE = 'varchar-category-cat-00..cat-15;float32-.9g;L2_DISTANCE(embedding);ORDER BY d,id;top10;autocommit-batch100'


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def files(root):
    result = {}
    aliases = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            aliases[str(path.relative_to(root))] = os.readlink(path)
        elif path.is_file():
            st = path.stat()
            result[str(path.relative_to(root))] = {
                'logical_bytes': st.st_size, 'allocated_bytes': st.st_blocks * 512}
    def total(selected):
        return {key: sum(item[key] for item in selected) for key in ('logical_bytes', 'allocated_bytes')}

    wal = root / 'wal'
    active_wal = None
    if wal.is_file():
        st = wal.stat()
        active_wal = {'logical_bytes': st.st_size, 'allocated_bytes': st.st_blocks * 512,
                      'resolved_file': str(wal.resolve().relative_to(root))}
    return {'files': result, 'symlink_aliases_excluded_from_totals': aliases,
            'working_data': total([v for k, v in result.items() if k.startswith('data/')]),
            'checkpoint_snapshots': total([v for k, v in result.items() if k.startswith('snapshot.')]),
            'wal_generations': total([v for k, v in result.items() if k == 'wal' or k.startswith('wal.')]),
            'active_wal_view_not_additive': active_wal,
            'logical_bytes': sum(x['logical_bytes'] for x in result.values()),
            'allocated_bytes': sum(x['allocated_bytes'] for x in result.values())}


def process_stats(pid):
    result = {}
    for line in Path(f'/proc/{pid}/io').read_text().splitlines():
        key, value = line.split(':')
        result[key] = int(value)
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith(('VmHWM:', 'VmRSS:')):
            key, value, _ = line.split()
            result[key.rstrip(':') + '_bytes'] = int(value) * 1024
    return result


class Client:
    def __init__(self, binary, root, budget=None, automatic_ann=False):
        self.root = root
        self.automatic_ann = automatic_ann
        self.sql_trace = []
        self.argv = [str(binary), str(root)]
        if budget is not None:
            self.argv.append(str(budget))
        self.p = subprocess.Popen(self.argv,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.metrics = queue.Queue()
        self.last_metrics = {}
        self.final_metrics = None
        self.messages = []
        self.thread = threading.Thread(target=self.drain, daemon=True)
        self.thread.start()

    def drain(self):
        try:
            for line in self.p.stderr:
                if line.startswith('METRICS '):
                    self.metrics.put(json.loads(line[8:]))
                elif line.startswith('FINAL_METRICS '):
                    self.final_metrics = json.loads(line[len('FINAL_METRICS '):])
                else:
                    self.messages.append(line.rstrip())
        finally:
            self.metrics.put(None)

    def sql(self, statement):
        normalized = ''.join(statement.upper().split())
        if self.automatic_ann and normalized.startswith('SETNPROBE'):
            raise AssertionError('automatic ANN prohibits external SET NPROBE')
        self.sql_trace.append(statement.split(' ', 1)[0].upper() +
                              (' NPROBE' if normalized.startswith('SETNPROBE') else ''))
        start = time.perf_counter()
        self.p.stdin.write(statement + '\n')
        self.p.stdin.flush()
        ready, _, _ = select.select([self.p.stdout], [], [], 1800)
        if not ready:
            raise TimeoutError('local adapter response exceeded 1800 seconds')
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError('adapter exited: ' + repr(self.messages))
        result = json.loads(line)
        metrics = self.metrics.get(timeout=30)
        elapsed = time.perf_counter() - start
        if not result.get('ok') or metrics is None:
            raise RuntimeError(f'request failed: {result}; stderr={self.messages}')
        self.last_metrics = metrics
        return result, elapsed, metrics

    def snapshot(self):
        return {'monotonic_seconds': time.perf_counter(),
                'process': process_stats(self.p.pid), 'data_directory': files(self.root),
                'engine_metrics': dict(self.last_metrics)}

    def close(self):
        before = self.snapshot()
        start = time.perf_counter()
        self.p.stdin.close()
        _, status, usage = os.wait4(self.p.pid, 0)
        self.p.returncode = os.waitstatus_to_exitcode(status)
        self.thread.join(timeout=30)
        self.p.stdout.close()
        self.p.stderr.close()
        result = {'seconds': time.perf_counter() - start,
                  'before_close': before, 'after_close_files': files(self.root),
                  'exit_code': self.p.returncode, 'stderr': self.messages,
                  'rusage': {'max_rss_bytes': usage.ru_maxrss * 1024,
                             'user_seconds': usage.ru_utime, 'system_seconds': usage.ru_stime,
                             'block_input_operations': usage.ru_inblock,
                             'block_output_operations': usage.ru_oublock}}
        result['final_engine_metrics'] = self.final_metrics
        result['close_engine_counter_deltas'] = {
            key: self.final_metrics[key] - before['engine_metrics'][key]
            for key in ('data_page_write_bytes', 'undo_spill_bytes', 'wal_bytes_written')
            if self.final_metrics is not None and key in self.final_metrics and key in before['engine_metrics']}
        if self.p.returncode:
            raise RuntimeError(f'close failed: {result}')
        return result


def vector_literal(vector):
    # Nine significant decimal digits round-trip every finite float32 value.
    return '[' + ','.join(format(float(x), '.9g') for x in vector) + ']'


def make_data(kind, n, count, seed, calibration_count):
    rng = np.random.default_rng(seed + (0 if kind == 'clustered' else 10000))
    centers = rng.normal(0, 3, (16, 128)).astype(np.float32)

    def vectors(size):
        if kind == 'clustered':
            return (centers[rng.integers(0, 16, size)] +
                    rng.normal(0, .35, (size, 128)).astype(np.float32)).astype(np.float32)
        return rng.uniform(-1, 1, (size, 128)).astype(np.float32)

    data = vectors(n)
    categories = rng.integers(0, 16, n)
    queries = vectors(count)
    calibration = vectors(calibration_count)
    query_categories = rng.integers(0, 16, count)
    calibration_categories = rng.integers(0, 16, calibration_count)
    return data, categories, queries, query_categories, calibration, calibration_categories


def oracle(data, categories, queries, query_categories):
    # Independent float64 subtraction/squared sum, chunked to bound temporary RAM.
    result = []
    ids = np.arange(1, len(data) + 1, dtype=np.int64)
    half = len(queries) // 2
    for index, query in enumerate(queries):
        selected = np.arange(len(data)) if index < half else np.flatnonzero(categories == query_categories[index])
        distances = np.empty(len(selected), dtype=np.float64)
        for start in range(0, len(selected), 1024):
            chunk = selected[start:start + 1024]
            delta = data[chunk].astype(np.float64) - query.astype(np.float64)
            distances[start:start + len(chunk)] = np.einsum('ij,ij->i', delta, delta)
        order = np.lexsort((ids[selected], distances))[:10]
        result.append(ids[selected[order]].tolist())
    return result


def statements(queries, categories):
    half = len(queries) // 2
    return ["SELECT id,L2_DISTANCE(embedding,'" + vector_literal(q) + "') AS d FROM t" +
            ('' if i < half else f" WHERE category = 'cat-{int(categories[i]):02d}'") +
            ' ORDER BY d,id LIMIT 10;' for i, q in enumerate(queries)]


def summarize(records):
    result = {}
    for group in ('unfiltered', 'filtered', 'all'):
        selected = records if group == 'all' else [x for x in records if x['group'] == group]
        if not selected:
            continue
        times = np.array([x['seconds'] for x in selected])
        result[group] = {'count': len(selected), 'recall_at_10': float(np.mean([x['recall'] for x in selected])),
                         'seconds': float(times.sum()), 'qps': float(len(times) / times.sum()),
                         'latency_ms_mean': float(times.mean() * 1000),
                         'latency_ms_p50': float(np.quantile(times, .5) * 1000),
                         'latency_ms_p95': float(np.quantile(times, .95) * 1000),
                         'vector_page_reads': sum(x['metrics'].get('vector_page_reads', 0) for x in selected),
                         'budget_peak_bytes': max(x['metrics'].get('budget_peak', 0) for x in selected)}
        routes = {}
        for record in selected:
            route = str(record['metrics'].get('ann_route', 'not_emitted'))
            routes[route] = routes.get(route, 0) + 1
        result[group]['routing_counts'] = routes
    return result


def execute_queries(client, sql, truth, label, checkpoint=None):
    records = []
    for i, statement in enumerate(sql):
        result, elapsed, metrics = client.sql(statement)
        ids = [int(row[0]) for row in result['rows']]
        expected = truth[i]
        recall = len(set(ids) & set(expected)) / len(expected) if expected else float(not ids)
        records.append({'query': i, 'group': 'unfiltered' if i < len(sql) // 2 else 'filtered',
                        'seconds': elapsed, 'recall': recall, 'ordered_ids_match': ids == expected,
                        'ids': ids, 'metrics': metrics})
        if (i + 1) % 100 == 0 or i + 1 == len(sql):
            print(f'{label}: {i + 1}/{len(sql)} queries', flush=True)
            if checkpoint:
                checkpoint(records)
    return {'summary': summarize(records), 'records': records}


def run_dataset(args, kind, report):
    root = (args.root / args.phase / kind).resolve()
    # Validate the fully resolved deletion target, including symlink resolution.
    if root != SAFE_ROOT / args.phase / kind or root.parent.parent != SAFE_ROOT:
        raise ValueError(f'unsafe benchmark root: {root}')
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    item = {'status': 'generating', 'root': str(root), 'snapshots': {}}
    report['datasets'][kind] = item
    persist = lambda: save(args.output, report)
    persist()
    data, categories, queries, qcats, calibration, ccats = make_data(kind, args.n, args.queries, args.seed, args.calibration_queries)
    item['input_sha256'] = hashlib.sha256(SCHEMA.encode() + WORKLOAD_SIGNATURE.encode() + data.tobytes() + categories.tobytes() + queries.tobytes() + qcats.tobytes()).hexdigest()
    start = time.perf_counter()
    truth = oracle(data, categories, queries, qcats)
    calibration_truth = None if args.automatic_ann else oracle(data, categories, calibration, ccats)
    item['oracle_seconds'] = time.perf_counter() - start
    sql = statements(queries, qcats)
    calibration_sql = statements(calibration, ccats)
    client = Client(args.binary, root, args.budget, args.automatic_ann)
    item['engine_argv'] = client.argv
    item['engine_pid'] = client.p.pid
    item['snapshots']['start'] = client.snapshot()
    persist()
    try:
        client.sql(SCHEMA)
        item['resolved_budget_bytes'] = client.last_metrics.get('engine_budget_bytes')
        item['resolved_budget_source'] = client.last_metrics.get('budget_source')
        item['snapshots']['before_write'] = client.snapshot()
        item['status'] = 'write'
        persist()
        start = time.perf_counter()
        batch_seconds = []
        for first in range(0, args.n, 100):
            rows = [f"({i + 1},'cat-{int(categories[i]):02d}','content-{i + 1:06d}','{vector_literal(data[i])}')"
                    for i in range(first, min(first + 100, args.n))]
            _, elapsed, _ = client.sql('INSERT INTO t VALUES ' + ','.join(rows) + ';')
            batch_seconds.append(elapsed)
            if (first // 100 + 1) % 10 == 0:
                print(f'{kind}: inserted {first + len(rows)}/{args.n}', flush=True)
        item['write'] = {'wall_seconds': time.perf_counter() - start, 'request_seconds': sum(batch_seconds),
                         'batch_seconds': batch_seconds, 'batch_size': 100, 'autocommit': True}
        item['snapshots']['after_write'] = client.snapshot()
        active_wal = item['snapshots']['after_write']['data_directory']['active_wal_view_not_additive']
        if active_wal is None:
            raise RuntimeError('missing active WAL after inserts')
        item['write']['active_wal_bytes'] = active_wal['logical_bytes']
        item['write']['active_wal_bytes_per_row'] = active_wal['logical_bytes'] / args.n
        item['status'] = 'exact'
        persist()
        client.sql('SET SEARCH_MODE = exact;')

        def checkpoint(name, records):
            item[name] = {'summary': summarize(records), 'records': records}
            persist()

        item['snapshots']['before_exact'] = client.snapshot()
        item['exact'] = execute_queries(client, sql, truth, kind + ' exact', lambda records: checkpoint('exact', records))
        item['snapshots']['after_exact'] = client.snapshot()
        item['status'] = 'index'
        persist()
        item['snapshots']['before_index'] = client.snapshot()
        _, seconds, metrics = client.sql('CREATE VECTOR INDEX vi ON t(embedding);')
        item['index'] = {'seconds': seconds, 'metrics': metrics}
        item['snapshots']['after_index'] = client.snapshot()
        item['automatic_ann'] = args.automatic_ann
        # Optimized centroids are packed: filesize/PAGE does not give nlist.
        # These policies correspond to the preserved and optimized binaries.
        item['nlist'] = metrics.get('nlist', min(args.n, 64 if args.automatic_ann or args.phase == 'after' and args.n >= 4096 else 8))
        item['nlist_source'] = 'engine metrics' if 'nlist' in metrics else ('phase2 baseline binary policy min(N,64)' if args.automatic_ann else 'historical binary policy')
        client.sql('SET SEARCH_MODE = ann;')
        item['status'] = 'calibrating'
        item['calibration'] = []
        persist()
        chosen = None
        grid = [] if args.automatic_ann else list(range(1, item['nlist'] + 1))
        item['calibration_probe_candidates'] = grid
        item['snapshots']['before_calibration'] = client.snapshot()
        for probe in grid:
            client.sql(f'SET NPROBE = {probe};')
            measured = execute_queries(client, calibration_sql, calibration_truth, f'{kind} calibrate nprobe={probe}')
            item['calibration'].append({'nprobe': probe, **measured})
            persist()
            if all(measured['summary'][g]['recall_at_10'] >= .98 for g in ('unfiltered', 'filtered')):
                chosen = probe
                break
        if chosen is None and grid:
            chosen = grid[-1]
        item['chosen_nprobe'] = chosen
        item['snapshots']['after_calibration'] = client.snapshot()
        if not args.automatic_ann:
            client.sql(f'SET NPROBE = {chosen};')
        item['status'] = 'ann'
        item['snapshots']['before_ann'] = client.snapshot()
        persist()
        item['ann'] = execute_queries(client, sql, truth, kind + ' ann', lambda records: checkpoint('ann', records))
        item['snapshots']['after_ann'] = client.snapshot()
        if args.automatic_ann:
            final = item['snapshots']['after_ann']['engine_metrics']
            item['chosen_nprobe'] = final.get('recommended_nprobe', metrics.get('recommended_nprobe'))
            item['recommended_nprobe_source'] = 'engine metrics; null means baseline does not emit recommendation'
            item['nlist'] = final.get('nlist', item['nlist'])
        item['sql_command_trace'] = client.sql_trace
        item['external_nprobe_commands'] = sum(x == 'SET NPROBE' for x in client.sql_trace)
        if args.automatic_ann:
            assert item['external_nprobe_commands'] == 0
        item['T_query_seconds'] = item['exact']['summary']['all']['seconds'] + item['ann']['summary']['all']['seconds']
        item['acceptance'] = {
            'exact_all_ordered_ids_match': all(x['ordered_ids_match'] for x in item['exact']['records']),
            'ann_recall_at_least_098_each_group': all(item['ann']['summary'][g]['recall_at_10'] >= .98 for g in ('unfiltered', 'filtered'))}
        if args.automatic_ann:
            item['heldout_recall_at_least_0985_each_group'] = all(item['ann']['summary'][g]['recall_at_10'] >= .985 for g in ('unfiltered', 'filtered'))
            item['acceptance']['ann_recall_at_least_0985_each_group'] = item['heldout_recall_at_least_0985_each_group']
            item['official_recall_at_least_095_each_group'] = all(item['ann']['summary'][g]['recall_at_10'] >= .95 for g in ('unfiltered', 'filtered'))
            item['acceptance']['no_external_nprobe_commands'] = item['external_nprobe_commands'] == 0
            item['acceptance']['normal_batch_undo_spill_zero'] = item['snapshots']['after_write']['engine_metrics'].get('undo_spill_bytes', 0) == 0
            if args.phase == 'after':
                item['acceptance']['index_undo_spill_zero'] = metrics.get('undo_spill_bytes', -1) - item['snapshots']['before_index']['engine_metrics'].get('undo_spill_bytes', 0) == 0
        if args.phase == 'after':
            item['acceptance']['insert_active_wal_at_most_600_bytes_per_row'] = item['write']['active_wal_bytes_per_row'] <= 600
        item['status'] = 'closing'
        persist()
    finally:
        item['close'] = client.close()
        persist()
    item['status'] = 'complete'
    if args.automatic_ann and args.phase == 'after':
        item['acceptance']['no_checkpoint_snapshot_directories'] = not any(p.is_dir() for p in root.glob('snapshot.*'))
    item['phase_deltas'] = {}
    cumulative_engine_metrics = ('data_page_write_bytes', 'undo_spill_bytes', 'wal_bytes_written',
                                 'wal_syncs')
    for phase in ('write', 'exact', 'index', 'calibration', 'ann'):
        before = item['snapshots']['before_' + phase]
        after = item['snapshots']['after_' + phase]
        engine_before, engine_after = before.get('engine_metrics', {}), after.get('engine_metrics', {})
        item['phase_deltas'][phase] = {
            'snapshot_wall_seconds': after['monotonic_seconds'] - before['monotonic_seconds'],
            'process_io': {k: after['process'][k] - before['process'][k]
                           for k in ('read_bytes', 'write_bytes', 'rchar', 'wchar', 'syscr', 'syscw', 'cancelled_write_bytes')},
            'engine_cumulative_counter_deltas': {k: engine_after[k] - engine_before[k]
                                                for k in cumulative_engine_metrics
                                                if k in engine_before and k in engine_after}}
    persist()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=['before', 'after'], required=True)
    parser.add_argument('--automatic-ann', action='store_true', help='phase3: never set NPROBE or run external calibration; engine chooses ANN route')
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=SAFE_ROOT)
    budget = parser.add_mutually_exclusive_group()
    budget.add_argument('--budget', '--budget-bytes', dest='budget', type=int, default=None,
                        help='explicit adapter budget in bytes (omitted by default)')
    budget.add_argument('--default-budget', action='store_true',
                        help='omit the adapter budget argument and use engine detection')
    parser.add_argument('--n', type=int, default=10000)
    parser.add_argument('--queries', type=int, default=1000)
    parser.add_argument('--calibration-queries', type=int, default=40)
    parser.add_argument('--seed', type=int, default=20261109)
    args = parser.parse_args()
    if args.budget is not None and args.budget <= 0:
        parser.error('budget must be positive')
    args.root = args.root.resolve()
    args.binary = args.binary.resolve(strict=True)
    args.output = args.output.resolve()
    if args.root != SAFE_ROOT or args.n < 16 or args.queries < 2 or args.queries % 2 or args.calibration_queries < 2 or args.calibration_queries % 2:
        parser.error('root must resolve to /tmp/mvdb_benchmark; n>=16; query counts positive even >=2')
    workload_config = {'schema': SCHEMA, 'signature': WORKLOAD_SIGNATURE, 'n': args.n,
                       'queries': args.queries, 'seed': args.seed, 'budget': args.budget,
                       'batch_size': 100, 'automatic_ann': args.automatic_ann,
                       'calibration_queries': 0 if args.automatic_ann else args.calibration_queries}
    report = {'status': 'running', 'phase': args.phase, 'binary': str(args.binary),
              'workload_configuration': workload_config,
              'workload_configuration_sha256': hashlib.sha256(json.dumps(workload_config, sort_keys=True).encode()).hexdigest(),
              'schema_sql': SCHEMA, 'workload_signature': WORKLOAD_SIGNATURE,
              'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              'configuration': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              'full_workload': args.n == 10000 and args.queries == 1000,
              'measurement_notes': {
                  'physical_io': '/proc/PID/io read_bytes/write_bytes cover all engine files, including temporary spills; not /data-only cumulative writes',
                  'data_sizes': 'logical st_size and allocated st_blocks*512 are separate from cumulative physical writes',
                  'close': 'pre-close /proc snapshot; final wait4 rusage includes close/checkpoint; Linux block counts are operation units, not directory-specific bytes',
                  'query_time': 'local round trip including JSON and metrics; excludes SQL generation, oracle, calibration, index, close',
                  'cache': 'warm process/page cache; no OS cache dropping; exact runs before ANN',
                  'budget': 'budget=null means CLI argument omitted; resolved budget/source recorded only if emitted by engine',
                  'calibration': ('automatic: no external calibration queries or SET NPROBE; engine owns recommendation; heldout recall>=.985 checked independently' if args.automatic_ann else 'disjoint queries; every integer probe count from 1 through binary-policy nlist; stops at first mean recall>=.98 in each group; heldout reported independently')},
              'datasets': {}}
    save(args.output, report)
    try:
        for kind in ('clustered', 'uniform'):
            run_dataset(args, kind, report)
        report['status'] = 'complete'
        report['acceptance_passed'] = all(all(x['acceptance'].values()) for x in report['datasets'].values())
        save(args.output, report)
        print(f"complete: {args.output}; acceptance={report['acceptance_passed']}", flush=True)
        return 0 if report['acceptance_passed'] else 2
    except BaseException:
        report['status'] = 'failed'
        report['error'] = traceback.format_exc()
        save(args.output, report)
        raise


if __name__ == '__main__':
    raise SystemExit(main())
