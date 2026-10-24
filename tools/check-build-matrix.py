#!/usr/bin/env python3
"""Build and test optional Meson configurations in isolated directories."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ENGINES = ['vips', 'ffmpeg', 'poppler', 'office', 'ai', 'safe_images']
CASES = {
    'cli-only': {'daemon': False},
    'daemon-core': {'daemon': True},
    'client-only': {'daemon': False, 'client': True},
    'ui-only': {'daemon': False, 'ui': 'enabled'},
    'safe-images-only': {'daemon': True, 'engine_safe_images': 'enabled'},
    'without-ffmpeg': {'daemon': True, 'engine_vips': 'enabled', 'engine_poppler': 'enabled', 'engine_office': 'enabled', 'engine_safe_images': 'enabled'},
    'without-office': {'daemon': True, 'engine_vips': 'enabled', 'engine_ffmpeg': 'enabled', 'engine_poppler': 'enabled', 'engine_safe_images': 'enabled'},
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    args = parser.parse_args()
    root = args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix='tc-build-matrix-'))
    root.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[1]
    rows = []
    for name in args.cases:
        build = root / name
        if build.exists():
            parser.error(f'Use a new matrix directory; {build} already exists')
        options = {'client': False, 'daemon': False, 'ui': 'disabled', **{'engine_' + engine: 'disabled' for engine in ENGINES}, **CASES[name]}
        flags = [f'-D{key}={str(value).lower() if isinstance(value, bool) else value}' for key, value in options.items()]
        commands = [['meson', 'setup', str(build), str(source), '--buildtype=debugoptimized', *flags],
                    ['meson', 'compile', '-C', str(build), '-j', '4'],
                    ['meson', 'test', '-C', str(build), '--print-errorlogs']]
        row = {'case': name, 'options': options, 'commands': commands, 'status': 'passed'}
        with (root / f'{name}.log').open('w') as log:
            for command in commands:
                try:
                    completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180)
                    code = completed.returncode
                except subprocess.TimeoutExpired:
                    code = 124
                if code:
                    row.update(status='failed', failed_command=command, exit_code=code)
                    break
        testlog = build / 'meson-logs/testlog.json'
        if testlog.exists():
            row['tests'] = [{'name': item['name'], 'result': item['result']} for line in testlog.read_text().splitlines() if line.strip() for item in [json.loads(line)]]
        rows.append(row)
        print(name, row['status'], flush=True)
        (root / 'report.json').write_text(json.dumps({'source': str(source), 'configurations': rows}, indent=2) + '\n')
    print(root / 'report.json')
    return int(any(row['status'] != 'passed' for row in rows))


if __name__ == '__main__':
    raise SystemExit(main())
