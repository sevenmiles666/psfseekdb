"""Check provisional large projections under a real container memory limit."""
import argparse
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--image', required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
data = args.output.resolve() / 'data'
data.mkdir()
name = 'mvdb-topk-resource-' + str(time.time_ns())
ident = 0
process = subprocess.Popen([
    'docker', 'run', '-i', '--name', name, '--network=none', '--read-only',
    '--cap-drop=ALL', '--security-opt=no-new-privileges', '--cpus=2',
    '--memory=256m', '--memory-swap=256m', '--pids-limit=128',
    '-v', str(data) + ':/data', args.image], stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def sql(text):
    global ident
    ident += 1
    process.stdin.write(json.dumps(dict(id=ident, op='execute', sql=text, search_mode='exact')).encode() + b'\n')
    process.stdin.flush()
    reply = json.loads(process.stdout.readline())
    assert reply['ok'] and reply['id'] == ident, reply
    return reply


try:
    assert json.loads(process.stdout.readline()) == {'type': 'ready'}
    sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4194304));')
    # The provisional projected values total 256 MiB; each is later displaced.
    payload = 'q' * (4194304 - 16)
    for first in range(0, 64, 2):
        sql('INSERT INTO t VALUES ' + ','.join(f"({i},'{i:016d}{payload}')" for i in range(first, first + 2)) + ';')
    sql('INSERT INTO t VALUES ' + ','.join(f"({i},'z{i:08d}')" for i in range(64, 128)) + ';')
    expected = [[i, f'z{i:08d}'] for i in range(127, 63, -1)]
    for order in ('id', 's'):
        assert sql(f'SELECT id,s FROM t ORDER BY {order} DESC LIMIT 64;')['rows'] == expected
    inspected = json.loads(subprocess.check_output(['docker', 'inspect', name]))[0]
    cgroup = next(line.split(':', 2)[2] for line in (Path('/proc') / str(inspected['State']['Pid']) / 'cgroup').read_text().splitlines() if line.startswith('0::'))
    control = Path('/sys/fs/cgroup') / cgroup.lstrip('/')
    events = dict(line.split() for line in (control / 'memory.events').read_text().splitlines())
    peak = int((control / 'memory.peak').read_text())
    assert int(events['oom_kill']) == 0 and peak <= 256 * 1024**2
    process.stdin.write(b'{"id":"stop","op":"shutdown"}\n')
    process.stdin.flush()
    assert json.loads(process.stdout.readline()) == dict(id='stop', ok=True)
    process.stdin.close()
    assert process.wait(timeout=120) == 0
    assert not json.loads(subprocess.check_output(['docker', 'inspect', name]))[0]['State']['OOMKilled']
    report = dict(passed=True, memory_limit_bytes=256 * 1024**2, memory_peak_bytes=peak,
                  provisional_payload_bytes=64 * 4194304, final_rows=64,
                  orders=['id DESC', 's DESC'], memory_events=events,
                  note='Local constrained-container correctness check, not official scoring.')
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
finally:
    subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for stream in (process.stdin, process.stdout, process.stderr):
        stream.close()
    process.wait(timeout=120)
