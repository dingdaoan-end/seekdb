#!/usr/bin/env python3
"""Clustered 10K/128D automatic-IVF regression with 150 committed mutations."""
import argparse
from pathlib import Path
import shutil
import traceback

import numpy as np

from benchmark_local import (Client, SCHEMA, execute_queries, make_data, oracle,
                             save, statements, vector_literal)


def run(args, report):
    root = Path('/tmp/mvdb_benchmark/dynamic_dml').resolve()
    if root != Path('/tmp/mvdb_benchmark/dynamic_dml'):
        raise ValueError('dynamic regression root resolves outside intended directory')
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    data, cats, queries, qcats, _, _ = make_data('clustered', 10000, 1000, 20261109, 40)
    # Keep this case on the calibrated path; the separate 4/8 MiB harness
    # verifies the empirical recommendation and conservative exact fallback.
    client = Client(args.binary, root, budget=16 * 1024 * 1024, automatic_ann=True)
    report['engine_argv'] = client.argv
    try:
        client.sql(SCHEMA)
        for first in range(0, len(data), 100):
            rows = [f"({i+1},'cat-{int(cats[i]):02d}','content-{i+1:06d}','{vector_literal(data[i])}')"
                    for i in range(first, first + 100)]
            client.sql('INSERT INTO t VALUES ' + ','.join(rows) + ';')
        _, _, metrics = client.sql('CREATE VECTOR INDEX vi ON t(embedding);')
        report['index_metrics'] = metrics
        recommendation = metrics['recommended_nprobe']
        assert 1 <= recommendation <= 32, metrics
        client.sql('SET SEARCH_MODE = ann;')
        _, _, metrics = client.sql(statements(queries[:2], qcats[:2])[0])
        assert metrics['ann_route'] == 'ivf' and metrics['effective_nprobe'] == recommendation, metrics

        # Preserve slots/IDs so the independent oracle returns the SQL IDs.
        live = np.ones(10050, dtype=bool)
        live[10000:] = False
        current = np.concatenate((data.copy(), np.zeros((50, 128), dtype=np.float32)))
        categories = np.concatenate((cats.copy(), np.zeros(50, dtype=cats.dtype)))
        targets, target_cats = [], []
        for i in range(50):
            inserted = 10001 + i
            updated = i + 1
            deleted = i + 51
            insert_vec = data[200 + i].copy()
            update_vec = data[400 + i].copy()
            insert_cat = int(cats[200 + i])
            update_cat = int(cats[400 + i])
            client.sql(f"INSERT INTO t VALUES ({inserted},'cat-{insert_cat:02d}','insert-{i}','{vector_literal(insert_vec)}');")
            current[inserted - 1], categories[inserted - 1], live[inserted - 1] = insert_vec, insert_cat, True
            client.sql(f"UPDATE t SET category='cat-{update_cat:02d}',content='update-{i}',embedding='{vector_literal(update_vec)}' WHERE id={updated};")
            current[updated - 1], categories[updated - 1] = update_vec, update_cat
            client.sql(f'DELETE FROM t WHERE id={deleted};')
            live[deleted - 1] = False
            for key, expected in ((inserted, [[str(inserted), f'cat-{insert_cat:02d}', f'insert-{i}']]),
                                  (updated, [[str(updated), f'cat-{update_cat:02d}', f'update-{i}']]),
                                  (deleted, [])):
                result, _, _ = client.sql(f'SELECT id,category,content FROM t WHERE id={key};')
                actual = [[str(cell) for cell in row] for row in result['rows']]
                assert actual == expected, (key, actual, expected)
            targets.extend((insert_vec, update_vec))
            target_cats.extend((insert_cat, update_cat))
        report['mutations'] = {'insert': 50, 'update': 50, 'delete': 50,
                               'total': 150, 'visibility_assertions': 150}

        def independent_truth(vectors, selected_categories):
            # oracle() uses contiguous row IDs: map its selected live slots back.
            slots = np.flatnonzero(live)
            answers = oracle(current[slots], categories[slots], vectors, selected_categories)
            return [[int(slots[key - 1]) + 1 for key in answer] for answer in answers]

        def check(label, vectors, selected_categories):
            truth = independent_truth(vectors, selected_categories)
            sql = statements(vectors, selected_categories)
            client.sql('SET SEARCH_MODE = exact;')
            exact = execute_queries(client, sql, truth, label + ' exact')
            assert all(row['ordered_ids_match'] for row in exact['records']), label
            client.sql('SET SEARCH_MODE = ann;')
            ann = execute_queries(client, sql, truth, label + ' ann')
            for row in ann['records']:
                filtered=row['group']=='filtered'
                assert row['metrics']['ann_route'] == ('ann_filter_exact' if filtered else 'ivf'), row
                assert row['metrics']['effective_nprobe'] == (0 if filtered else recommendation), row
                if filtered:assert row['ordered_ids_match'],row
                assert row['metrics']['recommended_nprobe'] == recommendation, row
            assert all(ann['summary'][group]['recall_at_10'] >= .98
                       for group in ('unfiltered', 'filtered')), ann['summary']
            report[label] = {'exact': exact, 'ann': ann}

        check('heldout', queries, qcats)
        # Each inserted/updated vector is queried both globally and in its category.
        targeted = np.asarray(targets, dtype=np.float32)
        target_categories = np.asarray(target_cats)
        check('affected_vectors', np.concatenate((targeted, targeted)),
              np.concatenate((target_categories, target_categories)))
        report['external_nprobe_commands'] = sum(x == 'SET NPROBE' for x in client.sql_trace)
        assert report['external_nprobe_commands'] == 0
        report['status'] = 'complete'
    finally:
        report['close'] = client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    args.binary = args.binary.resolve(strict=True)
    report = {'status': 'running', 'seed': 20261109, 'rows': 10000, 'dimensions': 128,
              'automatic_ann': True, 'budget_argument': 16 * 1024 * 1024}
    try:
        run(args, report)
    except BaseException:
        report['status'] = 'failed'
        report['error'] = traceback.format_exc()
        raise
    finally:
        if args.output:
            save(args.output.resolve(), report)
    print('dynamic DML: 50 inserts + 50 updates + 50 deletes; calibrated IVF; exact/oracle and ANN recall passed', flush=True)


if __name__ == '__main__':
    main()
