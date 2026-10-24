#!/usr/bin/env python3
"""Small reproducible compression study; never opens user media or changes engines.

Requires a configured build directory, cc/pkg-config, /usr/bin/ffmpeg and ffprobe.
Synthetic SDR samples only. JSON includes commands, versions and source hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
FF = '/usr/bin/ffmpeg'
PROBE = '/usr/bin/ffprobe'


def run(args, timeout=120):
    result = subprocess.run([str(x) for x in args], capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{shlex.join(map(str, args))}\n{result.stderr.decode(errors="replace")[-3000:]}')
    return result


def probe(path):
    return json.loads(run([PROBE, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', path]).stdout)


def video_hash(path):
    # This experiment fixes all fixtures to 8-bit 4:2:0; not a generic HDR validator.
    return run([FF, '-v', 'error', '-i', path, '-map', '0:v:0', '-pix_fmt', 'yuv420p',
                '-c:v', 'rawvideo', '-f', 'hash', '-hash', 'sha256', '-']).stdout.decode().strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    opts = parser.parse_args()
    opts.output.parent.mkdir(parents=True, exist_ok=True)
    report = {
        'scope': 'Synthetic SDR video only; engine called directly, bypassing worker size guard.',
        'ffmpeg': run([FF, '-version']).stdout.decode().splitlines()[0],
        'source_hashes': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for folder in ('src', 'include', 'tests') for p in sorted((ROOT / folder).rglob('*'))
                          if p.is_file()},
        'cases': [],
    }
    with tempfile.TemporaryDirectory(prefix='tc-compression-study-') as tmp:
        work = Path(tmp)
        harness = work / 'engine_probe.c'
        harness.write_text('''#include "tc_engine.h"
extern TcEngine tc_engine_ffmpeg;
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    char *error = NULL;
    GCancellable *cancel = g_cancellable_new();
    tc_engine_ffmpeg.init();
    TcStatusCode status = tc_engine_ffmpeg.convert(argv[1], argv[2], argv[3],
        0, 0, cancel, &error, NULL, NULL);
    if (error) { fprintf(stderr, "%s\\n", error); g_free(error); }
    g_object_unref(cancel);
    tc_engine_ffmpeg.cleanup();
    return status;
}
''')
        flags = shlex.split(run(['pkg-config', '--cflags', '--libs', 'glib-2.0', 'gio-2.0',
                                 'libavformat', 'libavcodec', 'libavutil']).stdout.decode())
        engine_bin = work / 'engine_probe'
        run(['cc', '-std=gnu11', '-O2', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'build'),
             '-I' + str(ROOT / 'src/engines'), harness, ROOT / 'src/engines/tc_media_ffmpeg.c', ROOT / 'src/engines/tc_media_plan.c', ROOT / 'src/engines/tc_media_estimate.c',
             ROOT / 'src/engines/tc_media_probe.c', ROOT / 'src/engines/tc_media_padding.c',
             ROOT / 'src/engines/tc_process_runner.c',
             '-o', engine_bin, *flags, '-lm'])
        for name, size, duration, crf in [('motion', '640x360', 3, 18),
                                          ('already_small', '640x360', 3, 35),
                                          ('resolution_check', '2560x1440', 1, 18)]:
            source = work / (name + '.mp4')
            generation = [FF, '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i',
                          f'testsrc2=size={size}:rate=12:duration={duration}',
                          '-c:v', 'libx264', '-preset', 'medium', '-crf', str(crf),
                          '-pix_fmt', 'yuv420p', '-threads', '2', source]
            run(generation)
            original = probe(source)
            original_video = next(s for s in original['streams'] if s['codec_type'] == 'video')
            original_hash = video_hash(source)
            paths = {
                'current_hq': [engine_bin, source, work / 'current_hq.mp4', 'comp-hq'],
                'current_saver': [engine_bin, source, work / 'current_saver.mp4', 'comp-saver'],
                'stream_copy': [FF, '-v', 'error', '-nostdin', '-y', '-i', source, '-map', '0',
                                '-c', 'copy', work / 'stream_copy.mp4'],
                'x265_crf20_medium': [FF, '-v', 'error', '-nostdin', '-y', '-i', source,
                                     '-c:v', 'libx265', '-preset', 'medium', '-crf', '20',
                                     '-x265-params', 'pools=2:frame-threads=1:log-level=error',
                                     '-tag:v', 'hvc1', work / 'x265_crf20_medium.mp4'],
                'x265_crf24_medium': [FF, '-v', 'error', '-nostdin', '-y', '-i', source,
                                     '-c:v', 'libx265', '-preset', 'medium', '-crf', '24',
                                     '-x265-params', 'pools=2:frame-threads=1:log-level=error',
                                     '-tag:v', 'hvc1', work / 'x265_crf24_medium.mp4'],
                'x265_crf28_medium': [FF, '-v', 'error', '-nostdin', '-y', '-i', source,
                                     '-c:v', 'libx265', '-preset', 'medium', '-crf', '28',
                                     '-x265-params', 'pools=2:frame-threads=1:log-level=error',
                                     '-tag:v', 'hvc1', work / 'x265_crf28_medium.mp4'],
                'x264_lossless': [FF, '-v', 'error', '-nostdin', '-y', '-i', source,
                                  '-c:v', 'libx264', '-preset', 'medium', '-crf', '0',
                                  '-threads', '2', work / 'x264_lossless.mp4'],
            }
            for variant, cmd in paths.items():
                destination = Path(cmd[-2]) if variant.startswith('current_') else Path(cmd[-1])
                started = time.monotonic()
                outcome = run(cmd)
                elapsed = time.monotonic() - started
                output = probe(destination)
                v = next(s for s in output['streams'] if s['codec_type'] == 'video')
                w, h = original_video['width'], original_video['height']
                same_geometry = (v['width'], v['height']) == (w, h)
                # Compare at source resolution; explicitly upscale changed-size output.
                filters = f'[0:v]scale={w}:{h}:flags=bicubic,setpts=PTS-STARTPTS[d];[1:v]setpts=PTS-STARTPTS[r];[d][r]ssim'
                metric = run([FF, '-hide_banner', '-nostdin', '-i', destination, '-i', source,
                              '-lavfi', filters, '-f', 'null', '-']).stderr.decode(errors='replace')
                match = re.search(r'All:([0-9.]+)', metric)
                entry = {
                    'sample': name, 'variant': variant,
                    'source_bytes': source.stat().st_size, 'output_bytes': destination.stat().st_size,
                    'saving_percent': round(100 * (1 - destination.stat().st_size / source.stat().st_size), 2),
                    'wall_seconds': round(elapsed, 3),
                    'source_dimensions': [w, h], 'output_dimensions': [v['width'], v['height']],
                    'codec': v['codec_name'], 'same_dimensions': same_geometry,
                    'decoded_video_equal': video_hash(destination) == original_hash,
                    'ssim': float(match[1]) if match else None,
                    'ssim_note': 'same size' if same_geometry else 'upscaled to source; includes resize loss',
                    'command': list(map(str, cmd)), 'generation_command': list(map(str, generation)),
                    'engine_diagnostics': outcome.stderr.decode(errors='replace')[-1500:] if variant.startswith('current_') else None,
                }
                report['cases'].append(entry)
                opts.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
                print(name, variant, entry['saving_percent'], entry['ssim'], entry['output_dimensions'], flush=True)
                destination.unlink()
    print('Results:', opts.output)


if __name__ == '__main__':
    main()
