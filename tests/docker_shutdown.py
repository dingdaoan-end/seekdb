"""Actual PID-1 SIGTERM/dockerd-stop rollback with stdin deliberately held open."""
import argparse
import json
import subprocess
import time
import uuid

p=argparse.ArgumentParser();p.add_argument('--docker',default='docker');p.add_argument('--image',required=True);a=p.parse_args()
prefix='mvdb-signal-'+uuid.uuid4().hex
def docker(*args,**kwargs):
    return subprocess.run([a.docker,*args],capture_output=True,text=True,check=True,timeout=45,**kwargs)
docker('volume','create',prefix)
results=[]
try:
    for signal_name in ['SIGTERM','SIGINT','stop']:
        name=prefix+'-'+signal_name.lower()
        child=subprocess.Popen([a.docker,'run','--rm','-i','--name',name,'--read-only','--network=none',
                                '--cap-drop=ALL','--security-opt=no-new-privileges',
                                '--mount',f'type=volume,src={prefix},dst=/data',a.image],
                               stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        assert json.loads(child.stdout.readline())=={'type':'ready'}
        def request(sql):
            child.stdin.write(json.dumps({'id':1,'sql':sql})+'\n');child.stdin.flush()
            response=json.loads(child.stdout.readline());assert response['ok'],response
            return response
        if signal_name=='SIGTERM':
            request('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2));')
            request("INSERT INTO t VALUES(1,'[1,2]');")
        assert request('SELECT v FROM t;')['rows']==[[[1,2]]]
        request('BEGIN;');request("UPDATE t SET v='[9,9]' WHERE id=1;")
        start=time.perf_counter()
        if signal_name=='stop':docker('stop','--time','3',name)
        else:docker('kill','--signal',signal_name,name)
        child.wait(timeout=5);elapsed=time.perf_counter()-start
        assert child.returncode==0,(signal_name,child.returncode,child.stderr.read())
        assert child.stdout.read()==''
        for stream in [child.stdin,child.stdout,child.stderr]:stream.close()
        results.append({'signal':signal_name,'exit_code':0,'seconds':elapsed})
    check=docker('run','--rm','-i','--read-only','--network=none','--mount',f'type=volume,src={prefix},dst=/data',
                 a.image,input=json.dumps({'id':1,'sql':'SELECT v FROM t;'})+'\n')
    assert json.loads(check.stdout.splitlines()[1])['rows']==[[[1,2]]]
    print(json.dumps({'status':'PASS','pid1_signal_tests':results,'rollback_after_restart':True},indent=2))
finally:
    # Only objects created with this fresh UUID are removed.
    for signal_name in ['SIGTERM','SIGINT','stop']:
        subprocess.run([a.docker,'rm','-f',prefix+'-'+signal_name.lower()],capture_output=True)
    docker('volume','rm',prefix)
