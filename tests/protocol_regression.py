"""Exercise strict framing, SQL comments, exact IDs and orderly pipe/signal exits."""
import json
import array
import errno
import fcntl
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import termios
import threading
import time

# Keep the public CTest protocol entry point while also exercising the private
# output formatter against its unchanged R9 oracle when CMake supplies it.
if len(sys.argv) > 2:
    serializer_report = subprocess.check_output([sys.argv[2]], text=True)
    serializer = json.loads(serializer_report)
    assert serializer['formatter_compatibility'] is True
    for sample in serializer['samples']:
        if sample['common_zero_allocation_required']:
            assert sample['response_allocations'] == 0, sample
        else:
            assert sample['response_allocations'] < sample['r9_allocations'], sample
    print(serializer_report.strip())


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
        child.last_response_bytes = len(raw) + 1
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
    # Exercise both sides of the serializer's reserved capacity and the stdio
    # transport buffer using exact wire lengths, including the JSONL newline.
    for wire_bytes in [16383, 16384, 16385, 65535, 65536, 65537]:
        empty_wire = len(json.dumps({'id': '', 'ok': True}, separators=(',', ':')).encode()) + 1
        ident = 'b' * (wire_bytes - empty_wire)
        assert send(p, {'id': ident, 'op': 'ping'}) == {'id': ident, 'ok': True}
        assert p.last_response_bytes == wire_bytes
        no_response(p)
    # Split a multibyte code point across request writes. The input parser must
    # wait for the complete record rather than decoding each transport chunk.
    unicode_record = json.dumps({'id': 'split中文😀', 'op': 'ping'}, ensure_ascii=False).encode() + b'\n'
    first_multibyte = next(i for i, value in enumerate(unicode_record) if value >= 128)
    cuts = [first_multibyte + 1, first_multibyte + 5, len(unicode_record) - 1]
    before = 0
    for after in cuts:
        p.stdin.write(unicode_record[before:after]);p.stdin.flush();before = after
        no_response(p)
    p.stdin.write(unicode_record[before:]);p.stdin.flush()
    assert read(p) == {'id': 'split中文😀', 'ok': True}
    # Many maximum-length aliases and escaped Unicode values make one response
    # exceed 64 KiB without relying solely on an oversized echoed request id.
    assert send(p, {'id': 'wide-table', 'sql': 'CREATE TABLE wide_output (id BIGINT PRIMARY KEY,text VARCHAR(4096));'})['ok']
    wide_text = ('quote" slash\\ newline\n tab\t 中文😀 ' * 60)
    assert len(wide_text.encode()) < 4096
    assert send(p, {'id': 'wide-write', 'sql': "INSERT INTO wide_output VALUES(1,'" + wide_text.replace("'", "''") + "');"})['ok']
    aliases = ['col' + str(i).zfill(2) + '_' + 'a' * 57 for i in range(48)]
    assert all(len(alias) == 63 for alias in aliases)
    wide = send(p, {'id': 'wide-read中文😀', 'sql': 'SELECT ' + ','.join('text AS ' + alias for alias in aliases) + ' FROM wide_output;'})
    assert wide['columns'] == aliases and wide['rows'] == [[wide_text] * len(aliases)]
    assert p.last_response_bytes > 65536
    no_response(p)
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
    # Force an atomic-size response to block in a full stdout pipe. Sending
    # requests from a producer avoids deadlocking this test on a full stdin
    # pipe before it can interrupt the child or start draining its responses.
    p = start()
    assert send(p, {'id': 'small-output-begin', 'sql': 'BEGIN;'})['ok']
    assert send(p, {'id': 'small-output-update', 'sql': "UPDATE t SET v='[9,9,9]' WHERE id=1;"})['ok']
    capacity = fcntl.fcntl(p.stdout.fileno(), fcntl.F_GETPIPE_SZ)
    request_count = max(4096, capacity // 32 + 1024)
    small_ids = ['blocked-small-' + str(i).zfill(6) for i in range(request_count)]
    payload = ''.join(json.dumps({'id': ident, 'op': 'ping'}) + '\n' for ident in small_ids).encode()
    producer_errors = []
    def produce_small_requests():
        offset = 0
        try:
            while offset < len(payload):
                count = os.write(p.stdin.fileno(), payload[offset:])
                assert count > 0
                offset += count
        except OSError as error:
            if error.errno != errno.EPIPE:
                producer_errors.append(error)
    producer = threading.Thread(target=produce_small_requests, daemon=True)
    producer.start()
    try:
        response_bytes = len(json.dumps({'id': small_ids[0], 'ok': True}, separators=(',', ':')).encode()) + 1
        assert response_bytes <= 4096
        unread = array.array('i', [0])
        deadline = time.monotonic() + 10
        while True:
            fcntl.ioctl(p.stdout.fileno(), termios.FIONREAD, unread, True)
            # Atomic records can leave unusable tail space in individual
            # pipe pages. Near-full byte occupancy is only a preliminary
            # check; /proc wchan below proves the writer actually blocked.
            if unread[0] > 0 and unread[0] >= capacity - 4096:
                break
            assert p.poll() is None and time.monotonic() < deadline, ('stdout did not fill', unread[0], capacity)
            time.sleep(0.005)
        # A full pipe alone could catch the child between records. Confirm
        # the child is sleeping inside the pipe write before either signal.
        def wait_for_blocked_write():
            deadline = time.monotonic() + 5
            observed = ''
            while True:
                assert p.poll() is None, 'child exited before the blocked write'
                try:
                    observed = Path('/proc/' + str(p.pid) + '/wchan').read_text().strip()
                except OSError as error:
                    raise AssertionError('cannot prove blocked stdout write via /proc wchan') from error
                if 'pipe_write' in observed:
                    return
                assert time.monotonic() < deadline, ('child not blocked in pipe write', observed)
                time.sleep(0.005)
        # No stdout read has occurred since starting the producer. Page-slot
        # fragmentation can block the next record before FIONREAD equals the
        # nominal capacity; pipe_write is the decisive blocking proof.
        wait_for_blocked_write()
        p.send_signal(signal.SIGTERM)
        time.sleep(0.02)
        wait_for_blocked_write()
        p.send_signal(signal.SIGINT)
        chunks = [p.pending_output]
        p.pending_output = b''
        deadline = time.monotonic() + 20
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([p.stdout], [], [], remaining)[0], 'interrupted small response drain timed out'
            chunk = os.read(p.stdout.fileno(), 65536)
            if not chunk:
                break
            chunks.append(chunk)
        assert p.wait(timeout=5) == 0
        producer.join(timeout=5)
        assert not producer.is_alive() and not producer_errors, producer_errors
        wire = b''.join(chunks)
        assert wire.endswith(b'\n')
        records = wire.split(b'\n')[:-1]
        assert 0 < len(records) <= request_count
        for index, raw in enumerate(records):
            response = json.loads(raw, object_pairs_hook=strict_object, parse_constant=invalid_constant)
            assert response == {'id': small_ids[index], 'ok': True}, (index, response)
        stopped(p)
    finally:
        if p.poll() is None:
            p.kill();p.wait(timeout=5)
        producer.join(timeout=5)
        p.stdin.close();p.stdout.close()
    p = start()
    assert send(p, {'id': 'small-output-signal-rollback', 'sql': 'SELECT v FROM t;'})['rows'] == [[[1,2,3]]]
    assert send(p, {'id': 'small-output-signal-close', 'op': 'close'})['ok']
    stopped(p)
    # A failed startup flush is a fatal transport error. Keep stdin open to
    # prove the adapter exits rather than waiting for EOF or another request.
    with open('/dev/full', 'wb', buffering=0) as failed_stdout:
        failed_output = subprocess.Popen(
            [sys.argv[1]], env={**os.environ, 'VECTORDB_DATA_DIR': root},
            stdin=subprocess.PIPE, stdout=failed_stdout, stderr=subprocess.DEVNULL,
            bufsize=0)
        try:
            assert failed_output.wait(timeout=5) == 3
        finally:
            failed_output.stdin.close()
            if failed_output.poll() is None:
                failed_output.kill();failed_output.wait(timeout=5)
    assert (Path(root)/'data').is_dir()
print('protocol_regression: strict JSONL, exact IDs, comments, single-statement SQL, EOF and signal rollback PASS')
