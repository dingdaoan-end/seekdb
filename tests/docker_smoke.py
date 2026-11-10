"""Exercise the built image's foreground protocol, volume persistence and SIGKILL."""
import argparse
import json
import os
import subprocess
import time

p=argparse.ArgumentParser()
p.add_argument('--docker',default='docker')
p.add_argument('--image',default='minivectordb:phase3')
args=p.parse_args()
volume=f'mvdb-phase3-smoke-{os.getpid()}-{int(time.time())}'
container=volume+'-crash'
owned_volume=False

def docker(*arguments,check=True):
    return subprocess.run([args.docker,*arguments],text=True,capture_output=True,check=check,timeout=120)

def command(*extra):
    return [args.docker,'run','--rm','-i','--network','none','--mount',f'type=volume,src={volume},dst=/data',args.image,*extra]

def execute(sql,*extra):
    run=subprocess.run(command(*extra),input=sql,text=True,capture_output=True,timeout=120)
    assert run.returncode==0,(run.returncode,run.stderr)
    rows=[json.loads(line) for line in run.stdout.splitlines()[1:]]
    assert all(row['ok'] is True for row in rows),rows
    return rows,run.stderr

started=time.perf_counter()
try:
    config=json.loads(docker('image','inspect',args.image,'--format','{{json .Config}}').stdout)
    assert not config.get('ExposedPorts'),config.get('ExposedPorts')
    assert '/data' in config.get('Volumes',{})
    assert config['Entrypoint']==['/usr/local/bin/minivectordb']
    docker('volume','create',volume)
    owned_volume=True
    results,_=execute("CREATE TABLE t (id BIGINT PRIMARY KEY,s VARCHAR(8),v VECTOR(2));\nINSERT INTO t VALUES (1,'甲','[3,4]');\nSELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY id;\n")
    assert results[-1]['rows']==[[1,5]],results
    results,_=execute('SELECT id,s FROM t ORDER BY id;\n')
    assert results[-1]['rows']==[[1,'甲']],results
    crash_command=command()
    crash_command[2:2]=['--name',container]
    # docker run syntax is docker run --name NAME --rm ...
    child=subprocess.Popen(crash_command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        assert json.loads(child.stdout.readline()) == {"type":"ready"}
        for sql in ('BEGIN;','UPDATE t SET s = \'乙\', v = \'[9,9]\' WHERE id=1;'):
            child.stdin.write(sql+'\n');child.stdin.flush()
            answer=json.loads(child.stdout.readline())
            assert answer['ok'],answer
        docker('kill','--signal','KILL',container)
        child.stdin.close();child.wait(timeout=30)
        assert child.returncode!=0
    finally:
        docker('rm','-f',container,check=False)
        if child.poll() is None:child.kill();child.wait(timeout=10)
        for stream in (child.stdin,child.stdout,child.stderr):
            if stream and not stream.closed:stream.close()
    results,_=execute("SELECT id,s,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY id;\n")
    assert results[-1]['rows']==[[1,'甲',5]],results
    custom=('--data-dir','/data/custom','--memory-budget','65536')
    execute('CREATE TABLE independent (id BIGINT PRIMARY KEY);\nINSERT INTO independent VALUES (7);\n',*custom)
    results,_=execute('SELECT id FROM independent;\n',*custom)
    assert results[-1]['rows']==[[7]],results
    directories=docker('run','--rm','--network','none','--mount',f'type=volume,src={volume},dst=/data','--entrypoint','/bin/sh',args.image,'-c','find /data -maxdepth 1 -type d -name "snapshot.*"').stdout
    assert not directories.strip(),directories
    print(json.dumps({'image':args.image,'default_data_directory':'/data','network':'none','exposed_ports':False,'foreground_entrypoint':True,'volume_restart':'PASS','sigkill_rollback':'PASS','custom_data_directory':'PASS','snapshot_directories':0,'elapsed_seconds':time.perf_counter()-started},ensure_ascii=False,indent=2))
finally:
    if owned_volume:docker('volume','rm',volume,check=False)
