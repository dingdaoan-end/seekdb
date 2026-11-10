"""Exercise official framing, exact IDs, environment directory and EOF rollback."""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='mvdb-jsonl-') as root:
    def start():
        child = subprocess.Popen([sys.argv[1]], env={**os.environ, 'VECTORDB_DATA_DIR': root},
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        assert read(child) == {'type': 'ready'}
        return child

    def read(child):
        assert select.select([child.stdout], [], [], 20)[0], 'response timeout'
        raw = child.stdout.readline()
        assert raw.endswith(b'\n'), raw
        return json.loads(raw)

    def send(child, request):
        child.stdin.write((json.dumps(request, ensure_ascii=False)+'\n').encode());child.stdin.flush()
        response = read(child)
        assert type(response['id']) is type(request['id']) and response['id'] == request['id'], response
        return response

    p = start()
    for raw in [b'{"id":1,"id":2,"op":"ping"}\n', b'{"id":NaN,"op":"ping"}\n',
                b'{"id":1,"sql":"\xc0\xaf"}\n', b'{"id":1,}\n']:
        p.stdin.write(raw);p.stdin.flush()
        invalid = read(p)
        assert invalid['ok'] is False and invalid['error']['code'], invalid
    for ident in [1, -42, 9007199254740993, 18446744073709551616, '编号\"\n😀']:
        assert send(p, {'id': ident, 'op': 'ping'}) == {'id': ident, 'ok': True}
    for ident, sql in enumerate(['CREATE TABLE t (id INT, v VECTOR(3));',
                                "INSERT INTO t VALUES(1,'[1,2,3]');"], 20):
        assert send(p, {'id':ident,'sql':sql})['ok']
    result = send(p, {'id':22,'sql':'SELECT id,v FROM t ORDER BY id;','search_mode':'approx'})
    assert result['rows'] == [[1,[1,2,3]]] and result['columns'] == ['id','v'], result
    for sql in ["INSERT INTO t VALUES(1,'[1,2,3]');", 'ALTER TABLE t ADD x INT;']:
        error = send(p, {'id':23,'sql':sql})
        assert error['ok'] is False and error['error']['code'], error
    for sql in ['BEGIN;', "UPDATE t SET v='[9,9,9]' WHERE id=1;"]:
        assert send(p, {'id':24,'sql':sql})['ok']
    p.stdin.close();p.wait(timeout=20);assert p.returncode == 0 and p.stdout.read() == b''
    p.stdout.close()
    p = start()
    assert send(p, {'id':25,'sql':'SELECT v FROM t;'})['rows'] == [[[1,2,3]]]
    assert send(p, {'id':27,'sql':'BEGIN;'})['ok']
    assert send(p, {'id':28,'sql':"INSERT INTO t VALUES(2,'[4,5,6]');"})['ok']
    assert send(p, {'id':29,'op':'close'}) == {'id':29,'ok':True}
    p.stdin.close();p.wait(timeout=20);assert p.returncode == 0 and p.stdout.read() == b''
    p.stdout.close()
    p = start()
    assert send(p, {'id':30,'sql':'SELECT id FROM t ORDER BY id;'})['rows'] == [[1]]
    assert send(p, {'id':26,'op':'shutdown'}) == {'id':26,'ok':True}
    p.stdin.close();p.wait(timeout=20);assert p.returncode == 0 and p.stdout.read() == b''
    p.stdout.close()
    assert (Path(root)/'data').is_dir()
print('protocol_regression: ID precision/types, Unicode, env directory, errors, vector projection, EOF rollback PASS')
