"""Exercise strict framing, SQL comments, exact IDs and orderly pipe/signal exits."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, ('duplicate response key', key)
        result[key] = value
    return result


def invalid_constant(value):
    raise AssertionError(('non-finite response number', value))

with tempfile.TemporaryDirectory(prefix='mvdb-jsonl-') as root:
    def start():
        child = subprocess.Popen([sys.argv[1]], env={**os.environ, 'VECTORDB_DATA_DIR': root},
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 bufsize=0)
        child.pending_output = b''
        assert read(child) == {'type': 'ready'}
        return child

    def read(child):
        deadline = time.monotonic() + 20
        while b'\n' not in child.pending_output:
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([child.stdout], [], [], remaining)[0], 'response timeout'
            chunk = os.read(child.stdout.fileno(), 65536)
            assert chunk, ('response stream closed', child.pending_output, child.poll())
            child.pending_output += chunk
        raw, child.pending_output = child.pending_output.split(b'\n', 1)
        return json.loads(raw, object_pairs_hook=strict_object, parse_constant=invalid_constant)

    def no_response(child):
        assert child.pending_output == b'', ('extra response', child.pending_output)
        assert not select.select([child.stdout], [], [], 0.01)[0], 'unexpected extra response'

    def send(child, request):
        child.stdin.write((json.dumps(request, ensure_ascii=False)+'\n').encode());child.stdin.flush()
        response = read(child)
        assert type(response['id']) is type(request['id']) and response['id'] == request['id'], response
        return response

    def stopped(child):
        child.wait(timeout=5)
        assert child.returncode == 0, child.returncode
        assert child.pending_output == b'' and child.stdout.read() == b'', 'extra response at exit'
        child.stdin.close()
        child.stdout.close()

    p = start()
    for raw in [b'{"id":1,"id":2,"op":"ping"}\n', b'{"id":NaN,"op":"ping"}\n',
                b'{"id":1,"sql":"\xc0\xaf"}\n', b'{"id":1,}\n', b'\n', b' \t\r\n',
                b'null\n', b'true\n', b'false\n', b'[]\n', b'{}\n', b'"text"\n']:
        p.stdin.write(raw);p.stdin.flush()
        invalid = read(p)
        assert invalid['ok'] is False and invalid['error']['code'], invalid
        assert invalid['id'] is None, invalid
        no_response(p)
    for ident in [None, True, False, 1, -42, 9007199254740993, 18446744073709551616,
                  '编号\"\n😀', ['nested', 1], {'opaque': 'id'}]:
        assert send(p, {'id': ident, 'op': 'ping'}) == {'id': ident, 'ok': True}
        no_response(p)
    for request in [{'op': 'unknown'}, {'op': 1}, {'sql': None}, {},
                    {'sql': 'SELECT id FROM t;', 'search_mode': 'unsupported'}]:
        request['id'] = 'invalid-branch\n😀'
        error = send(p, request)
        assert error['ok'] is False and error['error']['code'], error
        no_response(p)
    for ident, sql in enumerate(['CREATE TABLE t (id INT, v VECTOR(3));',
                                "INSERT INTO t VALUES(1,'[1,2,3]');"], 20):
        assert send(p, {'id':ident,'sql':sql})['ok']
    result = send(p, {'id':22,'sql':'SELECT id,v FROM t ORDER BY id;','search_mode':'approx'})
    assert result['rows'] == [[1,[1,2,3]]] and result['columns'] == ['id','v'], result
    assert send(p, {'id': 'numeric-table', 'sql': 'CREATE TABLE numeric_vectors (id INT, v VECTOR(6));'})['ok']
    assert send(p, {'id': 'numeric-valid', 'sql': "INSERT INTO numeric_vectors VALUES(1,'[ +1.25, -2e1, 3E-2, 1e-9999, -1e-9999, 0x1p+2 ]');"})['ok']
    numbers = send(p, {'id': 'numeric-read', 'sql': 'SELECT v FROM numeric_vectors;'})['rows'][0][0]
    assert numbers[:2] == [1.25, -20] and abs(numbers[2] - 0.03) < 1e-8 and numbers[3:] == [0, 0, 4], numbers
    for component in ['NaN', 'nan', 'Inf', '-Infinity', '1e9999', '-1e9999', '3.5e38', '-3.5e38', '+-1', '1e', '1.2.3', '1 2']:
        error = send(p, {'id': 'numeric-rejected', 'sql': "INSERT INTO numeric_vectors VALUES(2,'[" + component + ",0,0,0,0,0]');"})
        assert error['ok'] is False and error['error']['code'], (component, error)
    assert send(p, {'id': 'numeric-atomic', 'sql': 'SELECT id FROM numeric_vectors;'})['rows'] == [[1]]
    # JSON escapes newlines while the SQL lexer treats comments as whitespace.
    for sql in ['-- leading comment\nSELECT id FROM t; -- trailing comment',
                '/* leading ; SELECT */ SELECT/*between*/id FROM t; ; /* tail */ ;',
                '-- CR-terminated comment\rSELECT id FROM t;',
                'SELECT id FROM t;--end without newline',
                'SELECT id FROM t; ; ;']:
        result = send(p, {'id': 'comments\n😀', 'sql': sql})
        assert result.get('rows') == [[1]], result
        no_response(p)
    for sql in ['', ' \n\t', '-- only a comment', '/* only a comment */', '; ;',
                '/* unterminated', 'SELECT id FROM t; /* unterminated',
                "INSERT INTO t VALUES(2,'[4,5,6]'); INSERT INTO t VALUES(3,'[7,8,9]');",
                'SELECT id FROM t; ; SELECT id FROM t;']:
        error = send(p, {'id': 'rejected-sql', 'sql': sql})
        assert error['ok'] is False and error['error']['code'], error
        no_response(p)
    assert send(p, {'id': 'no-partial-execution', 'sql': 'SELECT id FROM t ORDER BY id;'})['rows'] == [[1]]
    assert send(p, {'id': 'literal-table', 'sql': 'CREATE TABLE strings (id INT, text VARCHAR(100));'})['ok']
    text = "literal -- /* ; */\nnext 'line'"
    assert send(p, {'id': 'literal-write', 'sql': "INSERT INTO strings VALUES(1,'" + text.replace("'", "''") + "');"})['ok']
    assert send(p, {'id': 'literal-read', 'sql': 'SELECT text FROM strings;'})['rows'] == [[text]]
    assert send(p, {'id': 'large-record', 'sql': '/*' + 'x' * 131072 + '*/ SELECT id FROM t;'})['rows'] == [[1]]
    split_record = b'{"id":"split-record","op":"ping"}\n'
    p.stdin.write(split_record[:13]);p.stdin.flush()
    no_response(p)
    p.stdin.write(split_record[13:]);p.stdin.flush()
    assert read(p) == {'id': 'split-record', 'ok': True}
    # Several records in one write exercise framing without relying on pipe reads
    # lining up with requests. Each complete record must produce one response.
    requests = [{'id': 'batch-' + str(i), 'op': 'ping'} for i in range(3)]
    p.stdin.write((''.join(json.dumps(request) + '\n' for request in requests)).encode())
    p.stdin.flush()
    for request in requests:
        assert read(p) == {'id': request['id'], 'ok': True}
    no_response(p)
    for sql in ["INSERT INTO t VALUES(1,'[1,2,3]');", 'ALTER TABLE t ADD x INT;']:
        error = send(p, {'id':23,'sql':sql})
        assert error['ok'] is False and error['error']['code'], error
    for sql in ['BEGIN;', "UPDATE t SET v='[9,9,9]' WHERE id=1;"]:
        assert send(p, {'id':24,'sql':sql})['ok']
    p.stdin.close();stopped(p)
    p = start()
    assert send(p, {'id':25,'sql':'SELECT v FROM t;'})['rows'] == [[[1,2,3]]]
    assert send(p, {'id':27,'sql':'BEGIN;'})['ok']
    assert send(p, {'id':28,'sql':"INSERT INTO t VALUES(2,'[4,5,6]');"})['ok']
    assert send(p, {'id':29,'op':'close'}) == {'id':29,'ok':True}
    stopped(p)
    p = start()
    assert send(p, {'id':30,'sql':'SELECT id FROM t ORDER BY id;'})['rows'] == [[1]]
    assert send(p, {'id':26,'op':'shutdown'}) == {'id':26,'ok':True}
    stopped(p)
    # EOF terminates a final record without a newline, including error paths.
    for raw, ident, ok in [(b'{"id":"last-record","op":"ping"}', 'last-record', True),
                           (b'{"id":"empty-eof","sql":""}', 'empty-eof', False),
                           (b'{"id":"truncated"', None, False)]:
        p = start()
        p.stdin.write(raw)
        p.stdin.close()
        response = read(p)
        assert response['id'] == ident and response['ok'] is ok, response
        stopped(p)
    for signum in [signal.SIGTERM, signal.SIGINT]:
        for partial in [b'', b'{"id":"incomplete']:
            p = start()
            assert send(p, {'id': 'committed', 'sql': 'SELECT v FROM t;'})['rows'] == [[[1,2,3]]]
            assert send(p, {'id': 'begin', 'sql': 'BEGIN;'})['ok']
            assert send(p, {'id': 'pending-update', 'sql': "UPDATE t SET v='[9,9,9]' WHERE id=1;"})['ok']
            assert send(p, {'id': 'pending-insert', 'sql': "INSERT INTO t VALUES(2,'[4,5,6]');"})['ok']
            if partial:
                p.stdin.write(partial)
                p.stdin.flush()
            # Keep stdin open: an EOF-driven shutdown would miss this failure.
            p.send_signal(signum)
            stopped(p)
            p = start()
            assert send(p, {'id': 'signal-rollback', 'sql': 'SELECT id,v FROM t ORDER BY id;'})['rows'] == [[1,[1,2,3]]]
            assert send(p, {'id': 'close-after-signal', 'op': 'shutdown'})['ok']
            stopped(p)
    # Stop while a response exceeds pipe capacity. Interrupted writes must resume
    # so the final response remains one complete JSON record before clean close.
    p = start()
    assert send(p, {'id': 'begin-output-stop', 'sql': 'BEGIN;'})['ok']
    assert send(p, {'id': 'update-output-stop', 'sql': "UPDATE t SET v='[9,9,9]' WHERE id=1;"})['ok']
    large_id = 'response-interruption-' + 'x' * 262144
    p.stdin.write((json.dumps({'id': large_id, 'op': 'ping'}) + '\n').encode())
    p.stdin.flush()
    assert select.select([p.stdout], [], [], 20)[0], 'large response never started'
    p.send_signal(signal.SIGTERM)
    assert read(p) == {'id': large_id, 'ok': True}
    stopped(p)
    p = start()
    assert send(p, {'id': 'output-stop-rollback', 'sql': 'SELECT v FROM t;'})['rows'] == [[[1,2,3]]]
    assert send(p, {'id': 'output-stop-close', 'op': 'close'})['ok']
    stopped(p)
    assert (Path(root)/'data').is_dir()
print('protocol_regression: strict JSONL, exact IDs, comments, single-statement SQL, EOF and signal rollback PASS')
