"""Independent write/query containers; host-side cgroup v2 B/M sampling.

The collector lives outside each measured cgroup. A tiny shell stays alive after
the DB shutdown so counters include checkpoint writeback before collection.
"""
import argparse
import json
from pathlib import Path
import subprocess
import threading
import time
import uuid
import numpy as np
from benchmark_local import SCHEMA,make_data,oracle,statements,vector_literal


def run(args):
    token='mvdb-r8-'+uuid.uuid4().hex[:12];collector=token+'-metrics'
    def docker(*parts,check=True):
        return subprocess.run([args.docker,*parts],text=True,capture_output=True,check=check,timeout=120)
    docker('run','-d','--name',collector,'--read-only','--cap-drop=ALL','--network=none',
           '--pid=host','--cgroupns=host','--mount','type=bind,src=/sys/fs/cgroup,dst=/cg,readonly',
           '--mount','type=bind,src=/proc,dst=/hp,readonly','ubuntu:24.04','sleep','infinity')
    report={'phase':args.phase,'image':args.image,'image_id':docker('image','inspect',args.image,'--format','{{.Id}}').stdout.strip(),
            'rounds':args.rounds,'notes':{'B':'full target io.stat counters from cgroup creation through DB close, including startup; initial snapshot diagnostic only',
             'M':'target memory.peak; collector outside target cgroup; no exec into target for stats',
             'T_write':'wall time of 100 pre-encoded INSERT requests plus clean shutdown/checkpoint round trip',
             'T_query':'1000 JSON execute round trips; excludes load,index,20 fixed warmup requests,shutdown',
             'scope':'each project fresh volume/cgroup; readonly root,2CPU,no swap; shell holder included equally',
             'cache':'fresh data volumes, same fixed warmup, no global drop_caches; Docker Desktop local environment',
             'correctness':'write content and restart verification uses a separate unmeasured container after counters freeze',
             'scope_caveat':'Docker Desktop device also charges image startup reads; not the official dedicated data device'},'runs':[]}
    def persist():args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2))
    persist()
    try:
        for distribution in args.distributions:
            data,cats,queries,qcats,*_=make_data(distribution,10000,1000,20261109,40)
            truth=oracle(data,cats,queries,qcats);queries_sql=statements(queries,qcats)
            batches=[]
            for first in range(0,10000,100):
                values=[f"({i+1},'cat-{int(cats[i]):02d}','content-{i+1:06d}','{vector_literal(data[i])}')" for i in range(first,first+100)]
                batches.append('INSERT INTO t VALUES '+','.join(values)+';')
            for memory in args.memories:
                for repeat in range(args.rounds):
                    for project in args.projects:
                        name=token+'-'+uuid.uuid4().hex[:8];volume=name+'-data'
                        docker('volume','create',volume)
                        child=None
                        try:
                            command=[args.docker,'run','--name',name,'-i','--read-only','--network=none','--cpus=2',
                                     '--memory='+memory,'--memory-swap='+memory,'--pids-limit=128','--cap-drop=ALL',
                                     '--security-opt=no-new-privileges','--mount',f'type=volume,src={volume},dst=/data',
                                     '--entrypoint','/bin/sh',args.image,'-c',
                                     '/usr/local/bin/minivectordb --data-dir /data; s=$?; echo ENGINE_EXIT:$s >&2; read finish; exit "$s"']
                            child=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
                            exited=threading.Event();final={};last={};errors=[];status=[]
                            def stderr():
                                for line in child.stderr:
                                    if line.startswith('METRICS '):last.clear();last.update(json.loads(line[8:]))
                                    elif line.startswith('FINAL_METRICS '):final.update(json.loads(line[14:]))
                                    elif line.startswith('ENGINE_EXIT:'):status.append(int(line.split(':')[1]));exited.set()
                                    else:errors.append(line.rstrip())
                            thread=threading.Thread(target=stderr,daemon=True);thread.start()
                            assert json.loads(child.stdout.readline())=={'type':'ready'}
                            pid=docker('inspect',name,'--format','{{.State.Pid}}').stdout.strip();assert pid.isdigit()
                            path=docker('exec',collector,'cat','/hp/'+pid+'/cgroup').stdout.strip().split('0::')[-1]
                            cg='/cg/'+path.lstrip('/')
                            def sample():
                                text=docker('exec',collector,'cat',cg+'/memory.peak',cg+'/io.stat').stdout.splitlines()
                                peak=int(text[0]);devices={}
                                for line in text[1:]:
                                    parts=line.split()
                                    if parts:devices[parts[0]]={k:int(v) for k,v in (p.split('=') for p in parts[1:])}
                                return {'memory_peak_bytes':peak,'io_stat':devices,'read_bytes':sum(d.get('rbytes',0) for d in devices.values()),
                                        'write_bytes':sum(d.get('wbytes',0) for d in devices.values())}
                            initial=sample();sequence=0
                            def request(sql=None,mode='exact',op='execute'):
                                nonlocal sequence
                                sequence+=1;payload={'id':sequence,'op':op}
                                if sql is not None:payload.update(sql=sql,search_mode=mode)
                                encoded=json.dumps(payload)+'\n'
                                start=time.perf_counter();child.stdin.write(encoded);child.stdin.flush()
                                raw=child.stdout.readline();elapsed=time.perf_counter()-start
                                assert raw,errors
                                result=json.loads(raw);assert result.get('id')==sequence and result.get('ok'),result
                                return result,elapsed
                            request(SCHEMA);write_seconds=0;write_start=time.perf_counter()
                            for sql in batches:write_seconds+=request(sql)[1]
                            write_last=time.perf_counter()
                            index_seconds=0;query_seconds=None;recalls=None
                            if project!='write':
                                index_seconds=request('CREATE VECTOR INDEX vi ON t(embedding);')[1]
                                mode=project
                                for i in list(range(10))+list(range(500,510)):request(queries_sql[i],mode)
                                hits=[[],[]];query_seconds=0
                                for i,(sql,expected) in enumerate(zip(queries_sql,truth)):
                                    result,elapsed=request(sql,mode);query_seconds+=elapsed
                                    ids=[r[0] for r in result['rows']]
                                    assert len(ids)==10 and len(set(ids))==10
                                    if mode=='exact':assert ids==expected,(ids,expected)
                                    hits[i>=500].append(len(set(ids)&set(expected))/10)
                                recalls=[float(np.mean(x)) for x in hits];assert min(recalls)>=.95
                            _,close_seconds=request(op='shutdown')
                            assert exited.wait(30) and status==[0],(status,errors)
                            finished=sample()
                            # No target process remains to delay writeback beyond this point.
                            child.stdin.write('finish\n');child.stdin.flush();child.stdin.close();child.wait(timeout=30);thread.join(timeout=5)
                            assert child.returncode==0,(child.returncode,errors)
                            row={'distribution':distribution,'memory':memory,'repeat':repeat+1,'project':project,
                                 'T_seconds':write_last-write_start+close_seconds if project=='write' else query_seconds,
                                 'write_requests_seconds':write_seconds,'write_loop_wall_seconds':write_last-write_start,
                                 'index_seconds':index_seconds,'close_seconds':close_seconds,'recall_unfiltered_filtered':recalls,
                                 'B_read_bytes':finished['read_bytes'],'B_write_bytes':finished['write_bytes'],
                                 'M_peak_bytes':finished['memory_peak_bytes'],'initial_cgroup':initial,'final_cgroup':finished,
                                 'engine_final_metrics':final,'last_query_metrics':dict(last),'diagnostics':errors}
                            row['B_bytes']=row['B_read_bytes']+row['B_write_bytes'];assert row['B_bytes']>0,'io.stat unavailable; do not fabricate physical I/O'
                            if project=='write':
                                # Correctness consumes the same data volume in a different
                                # cgroup only after the measured process exits and B/M freeze.
                                requests=[{'id':first+1,'sql':f'SELECT id,category,content,embedding FROM t WHERE id >= {first+1} AND id <= {first+100} ORDER BY id;'}
                                          for first in range(0,10000,100)]
                                requests.append({'id':10001,'op':'shutdown'})
                                payload=''.join(json.dumps(p)+'\n' for p in requests)
                                check=subprocess.run([args.docker,'run','--rm','-i','--read-only','--network=none',
                                    '--cap-drop=ALL','--security-opt=no-new-privileges','--memory=64m','--memory-swap=64m',
                                    '--mount',f'type=volume,src={volume},dst=/data',args.image],
                                    input=payload,capture_output=True,text=True,timeout=120,check=True)
                                answers=list(map(json.loads,check.stdout.splitlines()))
                                assert answers[0]=={'type':'ready'} and len(answers)==102
                                assert all(a.get('ok') for a in answers[1:])
                                assert [a['id'] for a in answers[1:]]==[r['id'] for r in requests]
                                rows=[r for a in answers[1:-1] for r in a['rows']];assert len(rows)==10000
                                for i,record in enumerate(rows):
                                    assert record[:3]==[i+1,f'cat-{int(cats[i]):02d}',f'content-{i+1:06d}']
                                actual=np.asarray([r[3] for r in rows],dtype=np.float32)
                                assert np.array_equal(actual,data),'restart changed float32 payload'
                                row['restart_verification']='10000/10000 full records and float32 vectors identical; separate cgroup'
                            report['runs'].append(row);persist()
                            print(f'{args.phase} {distribution} {memory} r{repeat+1} {project}: T={row["T_seconds"]:.6f} B={row["B_bytes"]} M={row["M_peak_bytes"]}',flush=True)
                        finally:
                            docker('rm','-f',name,check=False)
                            if child:
                                if child.poll() is None:child.kill();child.wait(timeout=10)
                                for stream in (child.stdin,child.stdout,child.stderr):
                                    if stream and not stream.closed:stream.close()
                            docker('volume','rm',volume,check=False)
        report['status']='complete';persist()
    finally:docker('rm','-f',collector,check=False)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--docker',default='docker');p.add_argument('--image',required=True)
    p.add_argument('--phase',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--rounds',type=int,default=3)
    p.add_argument('--memories',nargs='+',default=['16m','2g']);p.add_argument('--distributions',nargs='+',default=['clustered','uniform'])
    p.add_argument('--projects',nargs='+',default=['write','exact','ann']);run(p.parse_args())
