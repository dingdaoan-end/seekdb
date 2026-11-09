"""Verify real cgroup v2 defaults in memory-limited containers."""
import argparse,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--docker',default='docker');p.add_argument('--image',default='minivectordb:final');a=p.parse_args()
checks=[]
for memory,expected,extra in [(None,20971520,[]),('16m',9227468,[]),('32m',18454937,[]),('64m',25165824,[]),('32m',65536,['--data-dir','/data','--memory-budget','65536'])]:
 cmd=[a.docker,'run','--rm','-i','--network','none']
 if memory:cmd+=['--memory',memory]
 cmd+=[a.image]+extra
 r=subprocess.run(cmd,input='CREATE TABLE budgets (id BIGINT PRIMARY KEY);\n',text=True,capture_output=True,timeout=120)
 assert r.returncode==0,(cmd,r.returncode,r.stderr)
 m=next(json.loads(l.removeprefix('METRICS ')) for l in r.stderr.splitlines() if l.startswith('METRICS '))
 assert m['engine_budget_bytes']==expected,(memory,m)
 assert m['budget_source']==('explicit' if extra else 'cgroup' if memory else 'fallback'),m
 checks.append({'memory':memory,'expected_bytes':expected,'actual_bytes':m['engine_budget_bytes'],'source':m['budget_source'],'status':'PASS'})
print(json.dumps({'real_container_budget_checks':checks,'status':'PASS'},indent=2))
