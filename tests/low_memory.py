"""10K/128D request-latency, small-budget paging, rollback and crash regression."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import select
import shutil
import subprocess
import threading
import time
import numpy as np
from benchmark_local import SCHEMA, make_data, vector_literal, statements, oracle


class Client:
    def __init__(self, binary, root, budget, records):
        self.records = records;self.sequence = 0;self.metrics = queue.Queue()
        start = time.perf_counter()
        self.p = subprocess.Popen([str(binary), '--data-dir', str(root), '--memory-budget', str(budget)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.final = None
        def drain():
            for raw in self.p.stderr:
                if raw.startswith(b'METRICS '): self.metrics.put(json.loads(raw[8:]))
                elif raw.startswith(b'FINAL_METRICS '): self.final = json.loads(raw[14:])
        self.thread=threading.Thread(target=drain,daemon=True);self.thread.start()
        assert self.read() == {'type':'ready'}
        self.records.append({'kind':'startup','seconds':time.perf_counter()-start})

    def read(self):
        assert select.select([self.p.stdout], [], [], 180)[0], 'JSON response timeout'
        line=self.p.stdout.readline()
        assert line, 'process exited without response'
        return json.loads(line)

    def sql(self, sql, mode='exact', label=None):
        self.sequence+=1
        request=(json.dumps({'id':self.sequence,'op':'execute','sql':sql,'search_mode':mode})+'\n').encode()
        start=time.perf_counter();self.p.stdin.write(request);self.p.stdin.flush();result=self.read()
        seconds=time.perf_counter()-start
        metrics=self.metrics.get(timeout=10)
        assert result['id']==self.sequence and result['ok'], result
        self.records.append({'kind':label or sql.split()[0].lower(),'mode':mode,'seconds':seconds,
                             'sql_sha256':hashlib.sha256(sql.encode()).hexdigest(),'metrics':metrics})
        return result

    def close(self):
        start=time.perf_counter();self.p.stdin.close();self.p.wait(timeout=180);self.thread.join(timeout=10)
        assert self.p.returncode==0
        self.p.stdout.close();self.p.stderr.close()
        self.records.append({'kind':'close','seconds':time.perf_counter()-start,'final_metrics':self.final})

    def kill(self):
        self.p.kill();self.p.wait(timeout=15);self.thread.join(timeout=10)
        for f in (self.p.stdin,self.p.stdout,self.p.stderr): f.close()


def run(binary, budget, phase, count, require_ivf=False):
    root=Path('/tmp/mvdb_low_memory')/phase/str(budget)
    assert root.resolve().parent.parent == Path('/tmp/mvdb_low_memory')
    shutil.rmtree(root,ignore_errors=True);root.mkdir(parents=True)
    vectors,categories,queries,qcats,*_=make_data('clustered',10000,count,20261109,2)
    truth=oracle(vectors,categories,queries,qcats)
    records=[];client=Client(binary,root,budget,records)
    client.sql(SCHEMA)
    for first in range(0,10000,100):
        rows=[f"({i+1},'cat-{int(categories[i]):02d}','content-{i+1:06d}','{vector_literal(vectors[i])}')" for i in range(first,first+100)]
        client.sql('INSERT INTO t VALUES '+','.join(rows)+';',label='insert_batch')
    client.sql('CREATE VECTOR INDEX vi ON t(embedding);',label='create_index')
    recalls={};ann_routes={};ann_probes=set()
    sqls=statements(queries,qcats)
    for mode in ('exact','ann'):
        hits=[[],[]]
        for i,(sql,expected) in enumerate(zip(sqls,truth)):
            actual=[row[0] for row in client.sql(sql,mode,label='query')['rows']]
            if mode=='exact':assert actual==expected,(actual,expected)
            else:
                metrics=records[-1]['metrics'];route=metrics['ann_route']
                ann_routes[route]=ann_routes.get(route,0)+1;ann_probes.add(metrics['effective_nprobe'])
                if require_ivf:
                    assert route=='ivf' and metrics['effective_nprobe']==5 and metrics['recommended_nprobe']==5,metrics
            hits[i>=count//2].append(len(set(actual)&set(expected))/len(expected))
        recalls[mode]=[float(np.mean(x)) for x in hits]
        print(f'{phase} budget={budget} {mode} {count} queries recall={recalls[mode]}',flush=True)
    client.sql('BEGIN;')
    client.sql("UPDATE t SET content='round7-rollback';",label='large_update')
    assert client.sql('SELECT content FROM t WHERE id=1;')['rows']==[['round7-rollback']]
    client.sql('ROLLBACK;',label='large_rollback')
    assert client.sql('SELECT content FROM t WHERE id=1;')['rows']==[['content-000001']]
    client.close()
    client=Client(binary,root,budget,records)
    client.sql("UPDATE t SET content='committed-recovery' WHERE id=1;")
    client.sql('BEGIN;')
    client.sql("UPDATE t SET content='uncommitted-crash';",label='large_uncommitted_update')
    client.kill()
    client=Client(binary,root,budget,records)
    assert client.sql('SELECT content FROM t WHERE id=1;')['rows']==[['committed-recovery']]
    assert client.sql('SELECT content FROM t WHERE id=10000;')['rows']==[['content-010000']]
    client.close()
    request_records=[r for r in records if r['kind'] not in ('startup','close')]
    summary={kind:{'count':len(group),'seconds':sum(r['seconds'] for r in group),
                   'max_seconds':max(r['seconds'] for r in group)}
             for kind in sorted({r['kind'] for r in records}) if (group:=[r for r in records if r['kind']==kind])}
    return {'budget':budget,'records':records,'summary':summary,'recalls':recalls,
            'ann_route_counts':ann_routes,'ann_effective_nprobes':sorted(ann_probes),
            'index_below_03s':summary['create_index']['seconds']<.3,
            'single_sql_below_05s':all(r['seconds']<.5 for r in request_records),
            'max_request_seconds':max(r['seconds'] for r in request_records),
            'recall_at_least_095':all(x>=.95 for x in recalls['ann'])}


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--phase',required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--budgets',type=int,nargs='+',default=[4194304,8388608]);p.add_argument('--queries',type=int,default=1000)
    p.add_argument('--enforce-latency',action='store_true')
    p.add_argument('--require-ivf',action='store_true',help='clustered 10k must auto-route IVF at measured probe 5')
    a=p.parse_args();assert a.queries>0 and a.queries%2==0
    report={'phase':a.phase,'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'datasets':[]}
    try:
        for budget in a.budgets:
            result=run(a.binary.resolve(),budget,a.phase,a.queries,a.require_ivf);report['datasets'].append(result)
            a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2))
            print(f'{a.phase} budget={budget} maxSQL={result["max_request_seconds"]:.6f}s below0.5={result["single_sql_below_05s"]}',flush=True)
    finally:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2))
    if a.enforce_latency:
        assert all(d['single_sql_below_05s'] and d['index_below_03s'] and d['recall_at_least_095'] for d in report['datasets']), 'latency or recall threshold failed; see report'
