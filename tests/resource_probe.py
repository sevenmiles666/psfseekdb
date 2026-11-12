"""Generate fresh data larger than the configured memory budget; check reopen."""
import argparse
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--image', required=True)
parser.add_argument('--rows', type=int, default=12000)
parser.add_argument('--text-bytes', type=int, default=32768)
parser.add_argument('--memory', default='256m')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if args.rows < 35 or not 16 <= args.text_bytes <= 16 * 1024 * 1024:
    parser.error('use at least 35 rows and 16..16777216 bytes per text value')
args.output.mkdir(parents=True, exist_ok=False)
data = args.output.resolve() / 'data'
data.mkdir()
name = 'mvdb-resource-' + str(int(time.time()))
process = None
ident = 0
measurements = []


def start():
    global process
    process = subprocess.Popen([
        'docker', 'run', '-i', '--name', name, '--network=none', '--read-only',
        '--cap-drop=ALL', '--security-opt=no-new-privileges', '--cpus=2',
        '--memory=' + args.memory, '--memory-swap=' + args.memory,
        '--pids-limit=128', '-v', str(data) + ':/data', args.image],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert json.loads(process.stdout.readline()) == {'type': 'ready'}


def sql(text):
    global ident
    ident += 1
    process.stdin.write(json.dumps(dict(id=ident, op='execute', sql=text, search_mode='exact')).encode() + b'\n')
    process.stdin.flush()
    line = process.stdout.readline()
    assert line, process.stderr.read().decode()
    value = json.loads(line)
    assert value['ok'] and value['id'] == ident, value
    return value


def finish():
    stats = json.loads(subprocess.check_output(['docker', 'inspect', name]))[0]
    cgroups = Path('/proc') / str(stats['State']['Pid']) / 'cgroup'
    group = next(line.split(':', 2)[2] for line in cgroups.read_text().splitlines()
                 if line.startswith('0::'))
    root = Path('/sys/fs/cgroup') / group.lstrip('/')
    peak = int((root / 'memory.peak').read_text())
    events = dict(line.split() for line in (root / 'memory.events').read_text().splitlines())
    assert peak <= stats['HostConfig']['Memory'], peak
    assert int(events['oom_kill']) == 0, events
    measurements.append(dict(memory_peak_bytes=peak, memory_events=events,
                             io_stat=(root / 'io.stat').read_text()))
    process.stdin.write(b'{"id":"stop","op":"shutdown"}\n')
    process.stdin.flush()
    assert json.loads(process.stdout.readline()) == {'id': 'stop', 'ok': True}
    process.stdin.close()
    assert process.wait(timeout=120) == 0, process.stderr.read().decode()
    assert process.stdout.read() == b''
    stats = json.loads(subprocess.check_output(['docker', 'inspect', name]))[0]
    assert not stats['State']['OOMKilled'], stats['State']
    subprocess.run(['docker', 'rm', name], check=True, stdout=subprocess.DEVNULL)
    for stream in (process.stdout, process.stderr):
        stream.close()


try:
    before = time.monotonic()
    start()
    sql(f'CREATE TABLE large (id BIGINT PRIMARY KEY, payload VARCHAR({max(40000, args.text_bytes)}), v VECTOR(2));')
    payload = 'q' * (args.text_bytes - 16)
    for first in range(0, args.rows, 100):
        values = ','.join(f"({i},'{i:016d}{payload}','[{i % 17},0]')"
                          for i in range(first, min(first + 100, args.rows)))
        assert sql('INSERT INTO large VALUES ' + values + ';')['affected_rows'] == min(100, args.rows - first)
        if first % 1000 == 0:
            print('inserted', first + min(100, args.rows - first), flush=True)
    sql('CREATE VECTOR INDEX vi ON large(v);')
    assert sql('SELECT id FROM large ORDER BY id DESC LIMIT 3;')['rows'] == [[args.rows - 1], [args.rows - 2], [args.rows - 3]]
    assert sql("SELECT id FROM large ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 3;")['rows'] == [[0], [17], [34]]
    file_bytes = (data / 'database.mvdb').stat().st_size
    finish()
    start()
    assert sql('SELECT id FROM large ORDER BY id DESC LIMIT 3;')['rows'] == [[args.rows - 1], [args.rows - 2], [args.rows - 3]]
    for ident_key in (0, args.rows // 2, args.rows - 1):
        actual = sql(f'SELECT payload FROM large WHERE id={ident_key};')['rows']
        assert actual == [[f'{ident_key:016d}' + payload]]
    finish()
    report = dict(passed=True, rows=args.rows, text_bytes=args.text_bytes,
                  input_payload_bytes=args.rows * args.text_bytes, storage_bytes=file_bytes,
                  memory_limit=args.memory, elapsed_seconds=time.monotonic() - before,
                  container_measurements=measurements,
                  note='Local resource/restart check, not official performance scoring.')
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
finally:
    subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
