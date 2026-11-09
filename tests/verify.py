"""Real-process assertions; local framing is NOT the competition protocol."""
import argparse
import json
import math
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import time
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--repo', required=True)
parser.add_argument('--group', choices=['all', 'public', 'extra'], default='all')
parser.add_argument('--test', help='optional unittest method name')
args = parser.parse_args()
BASE = Path('/tmp/test_data')


def clean(name):
    target = BASE / name
    assert target.parent == BASE and target != BASE
    shutil.rmtree(target, ignore_errors=True)
    target.mkdir(parents=True)
    return target


class Client:
    def __init__(self, root, **env):
        self.p = subprocess.Popen([args.binary, str(root), '65536'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True,
                                  env={**os.environ, **env})

    def sql(self, sql, ok=True):
        self.p.stdin.write(sql + '\n')
        self.p.stdin.flush()
        ready, _, _ = select.select([self.p.stdout], [], [], 20)
        if not ready:
            raise AssertionError('response timeout: ' + sql)
        line = self.p.stdout.readline()
        if not line:
            raise AssertionError('process exited: ' + self.p.stderr.read())
        result = json.loads(line)
        assert 'diagnostics' not in result, 'debug metrics must stay on stderr'
        while True:
            diagnostic = self.p.stderr.readline()
            if diagnostic.startswith('METRICS '):
                result['diagnostics'] = json.loads(diagnostic[len('METRICS '):])
                break
            if not diagnostic:
                raise AssertionError('missing stderr metrics')
        if result.get('ok') is not ok:
            raise AssertionError(f'{sql}: {result}, expected ok={ok}')
        return result

    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        stderr = self.p.stderr.read()
        if self.p.returncode:
            raise AssertionError(f'exit={self.p.returncode}: {stderr}')
        self.p.stdout.close()
        self.p.stderr.close()

    def kill(self):
        self.p.kill()
        self.p.wait(timeout=10)
        self.p.stdin.close()
        self.p.stdout.close()
        self.p.stderr.close()


def equal_cell(a, b):
    if type(b) is int:
        return type(a) is int and a == b
    if type(b) is float:
        return type(a) in (int, float) and math.isfinite(a) and math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-5)
    return type(a) is type(b) and a == b


def equal_row(a, b):
    return len(a) == len(b) and all(equal_cell(x, y) for x, y in zip(a, b))


def check(result, expected):
    assert result['ok'] is True
    for field in ('affected_rows', 'columns'):
        if field in expected:
            assert result.get(field) == expected[field], (field, result, expected)
            if field == 'affected_rows':
                assert type(result[field]) is int
    if 'rows' in expected:
        actual, wanted = result.get('rows'), expected['rows']
        assert isinstance(actual, list) and len(actual) == len(wanted), (actual, wanted)
        if expected.get('unordered'):
            remaining = list(actual)
            for row in wanted:
                found = next((i for i, value in enumerate(remaining) if equal_row(value, row)), None)
                assert found is not None, (remaining, row)
                remaining.pop(found)
        else:
            assert all(equal_row(a, b) for a, b in zip(actual, wanted)), (actual, wanted)


class Public(unittest.TestCase):
    def test_32_statements(self):
        total = 0
        manifest = json.loads((Path(args.repo) / 'mysqltest/manifest.json').read_text())
        for case in manifest['cases']:
            name = case['id']
            directory = Path(args.repo) / 'mysqltest/cases'
            sql = [s for s in (directory / (name + '.test')).read_text().splitlines() if s.strip()]
            expected = json.loads((directory / (name + '.result')).read_text())
            self.assertEqual(len(sql), case['statements'])
            self.assertEqual(len(sql), len(expected))
            client = Client(clean(name))
            try:
                for number, (statement, assertion) in enumerate(zip(sql, expected), 1):
                    check(client.sql(statement), assertion)
                    total += 1
            finally:
                client.close()
            print(f'PUBLIC {name}: {len(sql)}/{len(sql)} PASS', flush=True)
        self.assertEqual(total, 32)
        print('PUBLIC TOTAL: 32/32 PASS', flush=True)


SCHEMA = 'CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(3), v VECTOR(2));'


class Extra(unittest.TestCase):
    def test_cli_named_data_directory(self):
        root=clean('cli_named')
        run=subprocess.run([str(Path(args.binary).resolve()),'--data-dir',str(root/'db'),'--memory-budget','65536'],
                           input='CREATE TABLE t (id BIGINT PRIMARY KEY);\nINSERT INTO t VALUES (9);\nSELECT id FROM t;\n',
                           text=True,capture_output=True,cwd=root,timeout=20)
        self.assertEqual(run.returncode,0,run.stderr)
        results=[json.loads(line) for line in run.stdout.splitlines()]
        self.assertEqual(results[-1]['rows'],[[9]])
        run=subprocess.run([str(Path(args.binary).resolve()),'--data-dir',str(root/'db'),'--memory-budget','65536'],
                           input='SELECT id FROM t;\n',text=True,capture_output=True,cwd=root,timeout=20)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertEqual(json.loads(run.stdout.splitlines()[0])['rows'],[[9]])
        self.assertEqual(subprocess.run([str(Path(args.binary).resolve()),'--help'],cwd=root,capture_output=True,timeout=10).returncode,0)
        self.assertNotEqual(subprocess.run([str(Path(args.binary).resolve()),'--unsupported'],cwd=root,capture_output=True,timeout=10).returncode,0)

    def test_near_tie_exact_rank_and_extremes(self):
        c=Client(clean('near_tie'))
        try:
            c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(128));')
            a='[1,0.0001,'+','.join(['0']*126)+']'
            b='[1,'+','.join(['0']*127)+']'
            zero='['+','.join(['0']*128)+']'
            c.sql(f"INSERT INTO t VALUES (1,'{a}'),(2,'{b}');")
            r=c.sql(f"SELECT id,L2_DISTANCE(v,'{zero}') AS d FROM t ORDER BY d LIMIT 1;")
            self.assertEqual(r['rows'],[[2,1]])
            c.sql("UPDATE t SET v='["+','.join(['1e30']*128)+"]' WHERE id=1;")
            r=c.sql(f"SELECT id,L2_DISTANCE(v,'{zero}') AS d FROM t ORDER BY d LIMIT 2;")
            self.assertEqual(r['rows'][0],[2,1])
            self.assertTrue(math.isfinite(r['rows'][1][1]))
        finally:
            c.close()

    def test_statement_savepoint_and_undo(self):
        root = clean('atomic')
        c = Client(root)
        try:
            c.sql(SCHEMA)
            c.sql('BEGIN;')
            c.sql("INSERT INTO t VALUES (1,'甲','[0,0]');")
            failures = ["(1,'乙','[1,1]')", "(3,'四个汉字','[1,1]')",
                        "(3,'乙','[NaN,0]')", "(3,'乙','[Inf,0]')",
                        "(3,'乙','[1e100,0]')", "(3,'乙','[1]')"]
            for bad in failures:
                c.sql("INSERT INTO t VALUES (2,'乙','[1,1]')," + bad + ';', ok=False)
                self.assertEqual(c.sql('SELECT id FROM t ORDER BY id;')['rows'], [[1]])
            c.sql("INSERT INTO t VALUES (2,'乙','[1,1]');")
            c.sql("UPDATE t SET id = 1 WHERE id = 2;", ok=False)
            c.sql('COMMIT;')
            c.sql('BEGIN;')
            c.sql("UPDATE t SET id = -9, s = '丙', v = '[3,4]' WHERE id = 1;")
            c.sql('DELETE FROM t WHERE id = 2;')
            self.assertEqual(c.sql('SELECT id FROM t ORDER BY id;')['rows'], [[-9]])
            c.sql('ROLLBACK;')
            self.assertEqual(c.sql('SELECT id,s FROM t ORDER BY id;')['rows'], [[1,'甲'],[2,'乙']])
            self.assertEqual(c.sql("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d;")['rows'], [[1,0],[2,math.sqrt(2)]])
            # Small transactions now remain in the bounded memory undo stack.
            self.assertEqual((root / 'undo').stat().st_size, 0)
        finally:
            c.close()

    def test_lexical_boundaries(self):
        c = Client(clean('lexical'))
        try:
            c.sql('cReAtE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(3), v VECTOR(2));')
            c.sql("  insert\tinto t values (-9223372036854775808,'a''b','[-1e0, +2.5E+0]'), (9223372036854775807,'😀甲乙','[0,0]');  ")
            self.assertEqual(c.sql('SELECT id,s FROM t ORDER BY id;')['rows'], [[-9223372036854775808,"a'b"],[9223372036854775807,'😀甲乙']])
            c.sql("INSERT INTO t VALUES (0,'😀甲乙丙','[0,0]');", ok=False)
            c.sql("INSERT INTO t VALUES (9223372036854775808,'甲','[0,0]');", ok=False)
            c.sql("INSERT INTO t VALUES (0,'甲','[0,0]'); trailing", ok=False)
            c.sql('CREATE TABLE ' + 't' * 121 + ' (id BIGINT PRIMARY KEY);', ok=False)
            self.assertEqual(c.sql("SELECT id FROM t WHERE s = '四个汉字';")['rows'], [])
        finally:
            c.close()

    def test_restart_after_rejected_long_identifier(self):
        root = clean('identifier')
        c = Client(root)
        try:
            c.sql('CREATE TABLE ' + 't' * 121 + ' (id BIGINT PRIMARY KEY);', ok=False)
            c.sql(SCHEMA)
            c.sql("INSERT INTO t VALUES (1,'甲','[0,0]');")
        finally:
            c.close()
        c = Client(root)
        try:
            self.assertEqual(c.sql('SELECT id FROM t;')['rows'], [[1]])
        finally:
            c.close()

    def test_multiple_vector_columns_rejected(self):
        root=clean('schema_limit')
        c=Client(root)
        try:
            c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, a VECTOR(2), b VECTOR(2));',ok=False)
            c.sql('SELECT * FROM t;',ok=False)
            c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, a VECTOR(2));')
            c.sql("INSERT INTO t VALUES (1,'[3,4]');")
            c.sql("CREATE TABLE unsupported (id BIGINT PRIMARY KEY) AS SELECT id FROM t;",ok=False)
            c.sql("SELECT id FROM t OR id = 1;",ok=False)
            self.assertEqual(c.sql('SELECT id FROM t;')['rows'],[[1]])
        finally:
            c.close()

    def test_three_tables_with_tiny_pool(self):
        c = Client(clean('three_tables'))
        try:
            for table in ('a','b','c'):
                c.sql(f'CREATE TABLE {table} (id BIGINT PRIMARY KEY, s VARCHAR(3), v VECTOR(2));')
            c.sql('BEGIN;')
            for i in range(12):
                for j,table in enumerate(('a','b','c')):
                    c.sql(f"INSERT INTO {table} VALUES ({i},'甲','[{i},{j}]');")
            c.sql('COMMIT;')
            for j,table in enumerate(('a','b','c')):
                self.assertEqual(c.sql(f"SELECT id,L2_DISTANCE(v,'[5,{j}]') AS d FROM {table} ORDER BY d LIMIT 3;")['rows'],[[5,0],[4,1],[6,1]])
            c.sql('BEGIN;')
            c.sql("UPDATE a SET v='[100,100]' WHERE id=5;")
            c.sql('DELETE FROM b WHERE id=5;')
            c.sql('ROLLBACK;')
            for table in ('a','b','c'):
                self.assertEqual(c.sql(f'SELECT id FROM {table} WHERE id=5;')['rows'],[[5]])
        finally:
            c.close()

    def test_topk_no_spill_and_checkpoint_close(self):
        root=clean('topk_checkpoint')
        c=Client(root)
        try:
            c.sql(SCHEMA)
            c.sql("INSERT INTO t VALUES (3,'甲','[1,0]'),(1,'甲','[-1,0]'),(2,'乙','[2,0]');")
            r=c.sql("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 2;")
            self.assertEqual(r['rows'],[[1,1],[3,1]])
            self.assertEqual(r['diagnostics'].get('spill_bytes'),0)
        finally:
            c.close()
        self.assertEqual((root/'wal').stat().st_size,0)
        c=Client(root)
        try:
            self.assertEqual(c.sql('SELECT id FROM t ORDER BY id;')['rows'],[[1],[2],[3]])
        finally:
            c.close()

    def test_paged_index_eviction_and_ivf(self):
        root = clean('pages')
        c = Client(root)
        try:
            c.sql(SCHEMA)
            c.sql('BEGIN;')
            for start in range(0, 900, 100):
                values = ','.join(f"({i},'甲','[{i},0]')" for i in range(start,start+100))
                inserted=c.sql('INSERT INTO t VALUES ' + values + ';')
            self.assertGreater(inserted['diagnostics']['undo_spill_bytes'],0)
            c.sql('COMMIT;')
            for i in (0,63,127,511,899):
                self.assertEqual(c.sql(f'SELECT id FROM t WHERE id = {i};')['rows'], [[i]])
            c.sql('CREATE VECTOR INDEX vi ON t(v);')
            c.sql('SET SEARCH_MODE = ann;')
            c.sql('SET NPROBE = 8;')
            q = "SELECT id,L2_DISTANCE(v,'[444.2,0]') AS d FROM t WHERE s = '甲' ORDER BY d LIMIT 10;"
            ann = c.sql(q)['rows']
            physical = c.sql(q)
            self.assertLessEqual(physical['diagnostics']['vector_page_reads'], 2)
            self.assertLessEqual(physical['diagnostics']['budget_peak'], 65536)
            c.sql('SET SEARCH_MODE = exact;')
            self.assertEqual(ann, c.sql(q)['rows'])
            c.sql('BEGIN;')
            c.sql("UPDATE t SET v = '[444.2,0]' WHERE id = 0;")
            c.sql('DELETE FROM t WHERE id = 444;')
            c.sql('SET SEARCH_MODE = ann;')
            self.assertEqual(c.sql(q)['rows'][0][0], 0)
            c.sql("INSERT INTO t VALUES (901,'甲','[444.2,0]'),(899,'甲','[0,0]');", ok=False)
            self.assertEqual(c.sql('SELECT id FROM t WHERE id = 901;')['rows'], [])
            c.sql('ROLLBACK;')
            c.sql('SET SEARCH_MODE = ann;')
            self.assertEqual(ann, c.sql(q)['rows'])
            c.sql('SET NPROBE = 1;')
            self.assertEqual(len(c.sql("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY id;")['rows']), 900)
        finally:
            c.close()
        c = Client(root)
        try:
            self.assertEqual(len(c.sql('SELECT id FROM t;')['rows']), 900)
        finally:
            c.close()

    def test_sigkill_and_interrupted_recovery(self):
        root = clean('crash')
        c = Client(root)
        c.sql(SCHEMA)
        c.sql("INSERT INTO t VALUES (1,'甲','[3,4]');")
        c.sql('BEGIN;')
        c.sql("UPDATE t SET s = '乙' WHERE id = 1;")
        c.sql("INSERT INTO t VALUES (2,'乙','[1,1]');")
        c.sql('COMMIT;')
        c.sql('BEGIN;')
        c.sql('DELETE FROM t WHERE id = 1;')
        c.sql("UPDATE t SET v = '[9,9]' WHERE id = 2;")
        c.kill()  # SIGKILL: no destructor, no orderly rollback/checkpoint
        for _ in range(2):
            interrupted = Client(root, MVDB_RECOVERY_PAUSE_AFTER='2')
            ready, _, _ = select.select([interrupted.p.stderr], [], [], 20)
            self.assertTrue(ready, 'recovery progress timeout')
            self.assertIn('RECOVERY_PAUSED', interrupted.p.stderr.readline())
            interrupted.kill()
        for _ in range(2):
            c = Client(root)
            try:
                self.assertEqual(c.sql('SELECT id,s FROM t ORDER BY id;')['rows'], [[1,'乙'],[2,'乙']])
                rows = c.sql("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY id;")['rows']
                self.assertEqual(rows[0], [1,5])
                self.assertAlmostEqual(rows[1][1], math.sqrt(2))
            finally:
                c.close()
        # Incomplete WAL tail is discarded before subsequent appends.
        with (root / 'wal').open('ab') as f:
            f.write(b'\x12\x34\x56')
        c = Client(root)
        c.sql("INSERT INTO t VALUES (3,'丙','[0,0]');")
        c.close()
        c = Client(root)
        self.assertEqual(c.sql('SELECT id FROM t ORDER BY id;')['rows'], [[1],[2],[3]])
        c.close()

    def test_128d_payload_scans_once(self):
        c = Client(clean('128d'))
        try:
            c.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(3), v VECTOR(128));')
            c.sql('BEGIN;')
            vec = '[' + ','.join(['1'] * 128) + ']'
            for start in range(0,256,4):
                c.sql('INSERT INTO t VALUES ' + ','.join(f"({i},'甲','{vec}')" for i in range(start,start+4)) + ';')
            c.sql('COMMIT;')
            c.sql('CREATE VECTOR INDEX vi ON t(v);')
            c.sql('SET SEARCH_MODE = ann;')
            c.sql('SET NPROBE = 8;')
            result = c.sql(f"SELECT id,L2_DISTANCE(v,'{vec}') AS d FROM t WHERE s = '甲' ORDER BY d LIMIT 10;")
            self.assertEqual(result['rows'], [[i,0] for i in range(10)])
            self.assertLessEqual(result['diagnostics']['vector_page_reads'], 8)
            self.assertLessEqual(result['diagnostics']['budget_peak'], 65536)
        finally:
            c.close()


if __name__ == '__main__':
    suite = unittest.TestSuite()
    loader = unittest.defaultTestLoader
    if args.group in ('all','public'):
        suite.addTests(loader.loadTestsFromTestCase(Public))
    if args.group in ('all','extra'):
        suite.addTests(loader.loadTestsFromTestCase(Extra))
    if args.test:
        suite=unittest.TestSuite(test for test in suite if test._testMethodName==args.test)
    started = time.perf_counter()
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(f'ELAPSED {time.perf_counter()-started:.3f}s', flush=True)
    raise SystemExit(0 if result.wasSuccessful() else 1)
