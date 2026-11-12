"""Independent reopen check of every primary key and sampled large values."""
import argparse
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--image', required=True)
parser.add_argument('--data', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
process = subprocess.Popen([
    'docker', 'run', '--rm', '-i', '--network=none', '--read-only',
    '--cap-drop=ALL', '--security-opt=no-new-privileges', '--cpus=2',
    '--memory=2g', '--memory-swap=2g', '--pids-limit=128',
    '-v', str(args.data.resolve()) + ':/data', args.image],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
ident = 0


def sql(text):
    global ident
    ident += 1
    process.stdin.write(json.dumps(dict(id=ident, op='execute', sql=text, search_mode='exact')).encode() + b'\n')
    process.stdin.flush()
    value = json.loads(process.stdout.readline())
    assert value['ok'] and value['id'] == ident, value
    return value


try:
    before = time.monotonic()
    assert json.loads(process.stdout.readline()) == {'type': 'ready'}
    rows = sql('SELECT id FROM large;')['rows']
    assert rows == [[i] for i in range(70000)]
    for key in (0, 1, 32767, 35000, 65535, 69999):
        result = sql(f'SELECT payload,v FROM large WHERE id={key};')['rows']
        assert result == [[f'{key:016d}' + 'q' * (32768 - 16), [key % 17, 0]]]
    assert sql("SELECT id FROM large ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3;")['rows'] == [[0], [17], [34]]
    process.stdin.write(b'{"id":"stop","op":"shutdown"}\n')
    process.stdin.flush()
    assert json.loads(process.stdout.readline()) == dict(id='stop', ok=True)
    process.stdin.close()
    assert process.wait(timeout=120) == 0
    report = dict(passed=True, primary_keys_checked=len(rows), sampled_content_keys=[0, 1, 32767, 35000, 65535, 69999],
                  memory_limit='2g', elapsed_seconds=time.monotonic() - before,
                  exact_topk_rows=[[0], [17], [34]],
                  note='Independent local reopen check; all primary keys, sampled full text and vectors. Not official hidden tests.')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
finally:
    if process.poll() is None:
        process.kill()
    process.wait(timeout=120)
    for stream in (process.stdin, process.stdout, process.stderr):
        stream.close()
