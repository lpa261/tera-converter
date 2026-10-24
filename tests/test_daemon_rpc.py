#!/usr/bin/env python3
"""Production daemon/CLI integration in an owned runtime, without credential mocks."""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(argv, **kwargs):
    return subprocess.run(list(map(str, argv)), capture_output=True, text=True, timeout=60, **kwargs)


def peer_credentials_available(runtime):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        path = str(runtime / 'capability.socket')
        try:
            listener.bind(path)
            listener.listen(1)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.connect(path)
                peer, _ = listener.accept()
                with peer:
                    _, uid, _ = struct.unpack('3i', peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                    if uid != os.getuid():
                        raise PermissionError('Peer UID is not mapped to the current UID')
        except OSError as error:
            print(f'SKIP: production AF_UNIX/SO_PEERCRED is unavailable in this environment: {error}')
            return False
        finally:
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
    return True


def main():
    installed = sys.argv[1:] == ['--installed']
    if installed:
        daemon, cli = Path('/usr/bin/teraconvert-daemon'), Path('/usr/bin/teraconvert-cli')
    elif len(sys.argv) == 3:
        daemon, cli = map(lambda p: Path(p).resolve(), sys.argv[1:])
    else:
        raise ValueError('Usage: test_daemon_rpc.py DAEMON CLI | --installed')
    with tempfile.TemporaryDirectory(prefix='tc-production-rpc-') as work:
        directory = Path(work)
        runtime = directory / 'runtime'
        runtime.mkdir(mode=0o700)
        if not peer_credentials_available(runtime):
            return 77
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), TC_JOB_STATE_DIR=str(directory / 'state'),
                   TC_HELPER_DIR='/usr/libexec/teraconvert' if installed else str(ROOT / 'src/scripts'))
        env.pop('LISTEN_PID', None)
        env.pop('LISTEN_FDS', None)
        ff = ['/usr/bin/ffmpeg', '-v', 'error', '-nostdin', '-y', '-filter_threads', '1']
        video, padded, image, audio, pdf, office, invalid = [directory / n for n in
            ['video.mp4', 'padded.mp4', 'ảnh có dấu.png', 'audio.wav', 'document.pdf', 'document.docx', 'invalid.mp4']]
        def generate(argv):
            result = run(argv, env=env)
            if result.returncode:
                raise RuntimeError(result.stderr[-2000:])
        generate(ff + ['-f', 'lavfi', '-i', 'testsrc2=size=160x120:rate=12:duration=0.5',
                       '-f', 'lavfi', '-i', 'sine=duration=0.5:sample_rate=48000', '-c:v', 'libx264',
                       '-threads', '2', '-c:a', 'aac', '-movflags', '+faststart', video])
        padded.write_bytes(video.read_bytes() + struct.pack('>I4s', 262144, b'free') + bytes(262136))
        generate(ff + ['-f', 'lavfi', '-i', 'testsrc=size=160x120:duration=1:rate=1',
                       '-frames:v', '1', '-threads', '1', '-compression_level', '0', image])
        generate(ff + ['-f', 'lavfi', '-i', 'sine=duration=0.5:sample_rate=48000', '-c:a', 'pcm_s16le', audio])
        generate(['python3', '-c', "import pikepdf as q,sys;p=q.new();page=p.add_blank_page(page_size=(600,800));page.Contents=p.make_stream(b'q 1 0 0 1 0 0 cm Q\\n'*4096);p.save(sys.argv[1],compress_streams=False)", pdf])
        with zipfile.ZipFile(office, 'w', compression=zipfile.ZIP_STORED) as package:
            package.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
            package.writestr('_rels/.rels', '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
            package.writestr('word/document.xml', '<document>' + 'TeraConvert unchanged. ' * 4096 + '</document>')
        invalid.write_bytes(b'intentionally invalid media')
        originals = {p: digest(p) for p in [video, padded, image, audio, pdf, office, invalid]}
        log_path = directory / 'daemon.log'
        with log_path.open('w') as log:
            process = subprocess.Popen([daemon], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError(log_path.read_text(errors='replace')[-3000:])
                    probe = run([cli, '--doctor'], env=env)
                    if probe.returncode == 0:
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError('Private daemon never negotiated IPC v2: ' + probe.stderr[-1000:])
                for fmt, path, result_key in [
                        ('comp-padding', padded, 'created'), ('comp-padding', video, 'no_benefit'),
                        ('comp-remux', padded, 'created'), ('comp-image', image, 'created'),
                        ('comp-office', office, 'created'), ('comp-pdf-safe', pdf, 'created'),
                        ('safe-flac', audio, 'created'), ('webm', invalid, 'failed')]:
                    result = run([cli, '--timeout', '30', '--format', fmt, path], env=env)
                    expected_exit = 1 if result_key == 'failed' else 0
                    if result.returncode != expected_exit:
                        raise RuntimeError(f'{fmt}: {result.stderr}\n{result.stdout}')
                    events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
                    if not events or events[-1]['phase'] != 6 or events[-1][result_key] != 1:
                        raise AssertionError((fmt, events))
                    event = events[-1]
                    replay = run([cli, '--listen', str(event['batch_id']), '--timeout', '3'], env=env)
                    if replay.returncode != expected_exit:
                        raise AssertionError('Late completion routing failed: ' + replay.stdout + replay.stderr)
                    if fmt == 'comp-padding' and path == padded and event['saved_bytes'] != 262144:
                        raise AssertionError('Padding savings are incorrect')
                    if any(digest(p) != original for p, original in originals.items()):
                        raise AssertionError('An original source changed')
                    print(f'OK: {fmt}, {result_key}, batch={event["batch_id"]}')
                if list(directory.glob('.teraconvert-*')) or list((directory / 'state').glob('*.job')):
                    raise AssertionError('Owned workspace/record leaked after completion')
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            if process.returncode != 0:
                raise AssertionError('Daemon did not shut down cleanly')
        print('OK: real IPC credentials, structured results, preserved sources, cleanup and shutdown')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
