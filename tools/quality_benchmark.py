#!/usr/bin/env python3
"""Research-only quality search on registered short clips; never replaces input.

A metric threshold is a research target, not a lossless or perceptual guarantee.
Corpus coverage and subjective A/B review are separate release gates.
"""
import argparse
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import stat
import subprocess
import tempfile
import time

FF = '/usr/bin/ffmpeg'
PROBE = '/usr/bin/ffprobe'
WORK_LIMIT = 4 * 1024**3


def directory_bytes(directory):
    total = 0
    for path in Path(directory).rglob('*'):
        try:
            if path.is_file() and not path.is_symlink():
                total += path.stat().st_size
        except FileNotFoundError:
            continue
    return total


def terminate(child):
    for sig, timeout in [(signal.SIGTERM, 0.25), (signal.SIGKILL, None)]:
        try:
            os.killpg(child.pid, sig)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=timeout)
            return
        except subprocess.TimeoutExpired:
            pass
CATALOG = {
    'bbb': {'title': 'Big Buck Bunny', 'credit': 'Blender Foundation', 'license': 'CC-BY-3.0', 'license_url': 'https://creativecommons.org/licenses/by/3.0/', 'source_url': 'https://peach.blender.org/about/'},
    'sintel': {'title': 'Sintel', 'credit': 'Blender Foundation', 'license': 'CC-BY-3.0', 'license_url': 'https://creativecommons.org/licenses/by/3.0/', 'source_url': 'https://durian.blender.org/sharing/'},
    'tos': {'title': 'Tears of Steel', 'credit': 'Blender Foundation', 'license': 'CC-BY-3.0', 'license_url': 'https://creativecommons.org/licenses/by/3.0/', 'source_url': 'https://mango.blender.org/about/'},
    'sparks': {'title': 'Sparks', 'credit': 'Netflix', 'license': 'CC-BY-4.0', 'license_url': 'https://creativecommons.org/licenses/by/4.0/', 'source_url': 'https://opencontent.netflix.com/'},
    'nocturne': {'title': 'Nocturne', 'credit': 'Netflix', 'license': 'CC-BY-4.0', 'license_url': 'https://creativecommons.org/licenses/by/4.0/', 'source_url': 'https://opencontent.netflix.com/'},
}
CODECS = {'libx264': (16, 36), 'libx265': (18, 38), 'libsvtav1': (20, 44),
          'h264_vaapi': (16, 36), 'hevc_vaapi': (18, 38), 'h264_nvenc': (16, 36), 'hevc_nvenc': (18, 38)}


class Rejected(Exception):
    pass


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, stage = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write('\n')
        os.replace(stage, path)
    finally:
        if os.path.exists(stage):
            os.unlink(stage)


def child_limits():
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    resource.setrlimit(resource.RLIMIT_FSIZE, (2 * 1024**3, 2 * 1024**3))


def run(argv, cwd, deadline, usage='usage.txt'):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise Rejected('Time budget exhausted')
    command = ['/usr/bin/time', '-f', '%M', '-o', usage, '--', *map(str, argv)]
    started = time.monotonic()
    with tempfile.TemporaryFile(dir=cwd) as stdout, tempfile.TemporaryFile(dir=cwd) as stderr:
        child = subprocess.Popen(command, cwd=cwd, stdout=stdout, stderr=stderr,
                                 start_new_session=True, preexec_fn=child_limits,
                                 env=dict(os.environ, OMP_NUM_THREADS='2', OPENBLAS_NUM_THREADS='2'))
        try:
            while True:
                try:
                    code = child.wait(timeout=min(0.1, max(0.001, deadline - time.monotonic())))
                    break
                except subprocess.TimeoutExpired:
                    reason = None
                    if time.monotonic() >= deadline:
                        reason = 'Time budget exhausted'
                    elif os.fstat(stdout.fileno()).st_size > 8 * 1024**2 or os.fstat(stderr.fileno()).st_size > 8 * 1024**2:
                        reason = 'Diagnostic output exceeded 8 MiB'
                    elif directory_bytes(cwd) + os.fstat(stdout.fileno()).st_size + os.fstat(stderr.fileno()).st_size > WORK_LIMIT:
                        reason = 'Research workspace exceeded its disk budget'
                    elif shutil.disk_usage(cwd).free < 256 * 1024**2:
                        reason = 'Research workspace reached its disk headroom limit'
                    if reason:
                        terminate(child)
                        raise Rejected(reason + '; process group terminated')
        finally:
            if child.poll() is None:
                terminate(child)
        stdout.seek(0, 2)
        length = stdout.tell()
        if length > 8 * 1024**2:
            raise Rejected('Probe/metric output exceeded 8 MiB')
        stdout.seek(0)
        result = stdout.read().decode(errors='replace')
        stderr.seek(0, 2)
        stderr.seek(max(0, stderr.tell() - 65536))
        diagnostic = stderr.read().decode(errors='replace')
    if code:
        raise Rejected(f'Command failed ({code}): {diagnostic[-2000:]}')
    rss = (Path(cwd) / usage).read_text().strip().splitlines()[-1]
    return {'stdout': result, 'stderr': diagnostic, 'wall_seconds': time.monotonic() - started,
            'peak_rss_kib': int(rss), 'command': list(map(str, argv))}


def probe(path, cwd, deadline):
    return json.loads(run([PROBE, '-v', 'error', '-show_streams', '-show_format', '-show_chapters', '-of', 'json', path], cwd, deadline)['stdout'])


def frames(path, cwd, deadline):
    obj = json.loads(run([PROBE, '-v', 'error', '-select_streams', 'v:0', '-show_frames',
                         '-show_entries', 'frame=best_effort_timestamp_time', '-of', 'json', path], cwd, deadline)['stdout'])
    values = [Fraction(f['best_effort_timestamp_time']) for f in obj['frames']]
    if not values or len(values) > 10000:
        raise Rejected('Unknown or excessive frame count')
    return values


def raw_hash(path, cwd, deadline):
    return run([FF, '-v', 'error', '-nostdin', '-threads', '2', '-i', path, '-map', '0:v:0', '-an',
                '-c:v', 'rawvideo', '-pix_fmt', 'yuv420p', '-threads', '2', '-fps_mode', 'passthrough',
                '-f', 'hash', '-hash', 'sha256', '-'], cwd, deadline)['stdout'].strip()


def audio_signature(path, info, cwd, deadline):
    audio = [s for s in info['streams'] if s['codec_type'] == 'audio']
    if not audio:
        return None
    hashes = run([FF, '-v', 'error', '-nostdin', '-i', path, '-map', '0:a', '-c', 'copy',
                  '-f', 'streamhash', '-hash', 'sha256', '-'], cwd, deadline)['stdout']
    packets = json.loads(run([PROBE, '-v', 'error', '-select_streams', 'a', '-show_packets',
                             '-show_entries', 'packet=stream_index,pts_time,dts_time,duration_time,flags,size',
                             '-of', 'json', path], cwd, deadline)['stdout'])
    keys = ['codec_name', 'sample_rate', 'channels', 'channel_layout', 'bits_per_raw_sample', 'tags', 'disposition']
    return {'parameters': [{k: s.get(k) for k in keys} for s in audio], 'data': hashes, 'packets': packets}


def metrics(reference, candidate, cwd, deadline, name, vmaf_model):
    # Timestamps/frame counts are checked before this pairing. Common index PTS
    # avoids FFV1/Matroska millisecond rounding affecting objective frame pairs.
    result = {}
    base = '[0:v]settb=AVTB,setpts=N/TB[r];[1:v]settb=AVTB,setpts=N/TB[d];[d][r]'
    for metric in ['ssim', 'psnr']:
        tail = f'ssim=stats_file={name}.ssim' if metric == 'ssim' else 'psnr'
        run_result = run([FF, '-hide_banner', '-nostdin', '-threads', '2', '-i', reference,
                          '-threads', '2', '-i', candidate, '-filter_complex_threads', '2',
                          '-lavfi', base + tail, '-an', '-f', 'null', '-'], cwd, deadline)
        pattern = r'All:([0-9.]+)' if metric == 'ssim' else r'average:([0-9.inf]+)'
        match = re.findall(pattern, run_result['stderr'])
        if not match:
            raise Rejected(f'{metric} did not return a valid score')
        value = float(match[-1])
        result[metric] = value if math.isfinite(value) else 'inf'
    per_frame = []
    for line in (Path(cwd) / f'{name}.ssim').read_text().splitlines():
        match = re.search(r'n:(\d+).*All:([0-9.]+)', line)
        if match:
            per_frame.append((int(match[1]), float(match[2])))
    if not per_frame:
        raise Rejected('No per-frame SSIM results')
    result['minimum_frame_ssim'] = min(score for _, score in per_frame)
    result['worst_frame'] = min(per_frame, key=lambda item: item[1])[0] - 1
    if vmaf_model:
        log = f'{name}.vmaf.json'
        run([FF, '-hide_banner', '-nostdin', '-threads', '2', '-i', reference, '-threads', '2', '-i', candidate,
             '-filter_complex_threads', '2', '-lavfi', base + f'libvmaf=model=path=vmaf-model.json:log_path={log}:log_fmt=json:n_threads=2',
             '-an', '-f', 'null', '-'], cwd, deadline)
        result['vmaf'] = json.loads((Path(cwd) / log).read_text())['pooled_metrics']['vmaf']['mean']
    else:
        result['vmaf'] = None
    return result


def benchmark(entry, manifest_dir, args, root):
    if not entry.get('path') or not entry.get('sha256'):
        return {'id': entry['id'], 'status': 'missing_registered_media'}
    source = (manifest_dir / entry['path']).resolve()
    source_stat = source.stat()
    if not stat.S_ISREG(source_stat.st_mode) or not 0 < source_stat.st_size <= 512 * 1024**2:
        raise Rejected('Research input must be a regular file no larger than 512 MiB')
    expected = entry['sha256']
    if sha256(source) != expected:
        raise Rejected('Source checksum does not match the manifest')
    case = root / entry['id']
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', entry['id']):
        raise Rejected('Unsafe clip ID')
    case.mkdir()
    deadline = time.monotonic() + args.seconds_per_clip
    if args.vmaf_model:
        shutil.copyfile(args.vmaf_model, case / 'vmaf-model.json')
    info = probe(source, case, deadline)
    video = [s for s in info['streams'] if s['codec_type'] == 'video']
    if len(video) != 1 or info.get('chapters') or any(s['codec_type'] not in ['audio', 'video'] for s in info['streams']):
        raise Rejected('Research harness currently requires one video, optional audio and no chapters/other tracks')
    v = video[0]
    duration = float(info['format'].get('duration', 0))
    if not 0 < duration <= 30 or v['width'] > 3840 or v['height'] > 2160:
        raise Rejected('Register a clip of at most 30 seconds and 3840×2160')
    if v.get('pix_fmt') != 'yuv420p' or any(v.get(k) != value for k, value in
            [('color_range', 'tv'), ('color_space', 'bt709'), ('color_transfer', 'bt709'), ('color_primaries', 'bt709')]):
        raise Rejected('HDR, higher depth or unknown color contract: SDR metrics are not applicable')
    fps = Fraction(v['avg_frame_rate'])
    if not 0 < fps <= 240:
        raise Rejected('Unknown or excessive frame rate')
    estimated = int(v['width'] * v['height'] * 3 * float(fps) * duration * 2)
    if shutil.disk_usage(root).free < estimated + 512 * 1024**2:
        raise Rejected('Insufficient disk headroom for bounded research intermediates')
    original_frames = frames(source, case, deadline)
    original_audio = audio_signature(source, info, case, deadline)
    reference = case / 'reference.mkv'
    run([FF, '-v', 'error', '-nostdin', '-y', '-threads', '2', '-i', source, '-map', '0:v:0', '-an',
         '-c:v', 'ffv1', '-level', '3', '-threads', '2', '-pix_fmt', 'yuv420p', '-fps_mode', 'passthrough', reference], case, deadline)
    if raw_hash(source, case, deadline) != raw_hash(reference, case, deadline):
        raise Rejected('Lossless metric reference did not preserve decoded samples')
    def validate(target, name):
        candidate_info = probe(target, case, deadline)
        candidate_video = [s for s in candidate_info['streams'] if s['codec_type'] == 'video'][0]
        for key in ['width', 'height', 'pix_fmt', 'color_range', 'color_space', 'color_transfer', 'color_primaries']:
            if candidate_video.get(key) != v.get(key):
                raise Rejected(f'Video invariant changed: {key}')
        candidate_frames = frames(target, case, deadline)
        tolerance = max(Fraction(v['time_base']), Fraction(candidate_video['time_base']))
        if len(candidate_frames) != len(original_frames) or any(abs(a - b) > tolerance for a, b in zip(candidate_frames, original_frames)):
            raise Rejected('Frame count or timestamp alignment changed')
        if len(candidate_info['streams']) != len(info['streams']) or audio_signature(target, candidate_info, case, deadline) != original_audio:
            raise Rejected('Audio parameters, packet bytes or timing changed')
        return metrics(reference, target, case, deadline, name, args.vmaf_model)

    def meets_target(scores):
        return scores['ssim'] >= args.ssim_target and (args.minimum_frame_ssim is None or scores['minimum_frame_ssim'] >= args.minimum_frame_ssim)

    rows = []
    repeats = []
    for codec in args.codecs:
        low, high = CODECS[codec]
        best = None
        for iteration in range(args.max_candidates):
            if low > high or time.monotonic() >= deadline:
                break
            quality = (low + high) // 2
            name = f'{codec}-{iteration}'
            target = case / f'{name}.mp4'
            command = [FF, '-v', 'error', '-nostdin', '-y', '-copyts', '-filter_threads', '2', '-threads', '2']
            if codec.endswith('_vaapi'):
                command += ['-vaapi_device', args.vaapi_device]
            command += ['-i', source, '-map', '0', '-map_metadata', '0', '-map_chapters', '0', '-c', 'copy',
                        '-c:v:0', codec, '-threads', '2', '-fps_mode:v:0', 'passthrough', '-avoid_negative_ts', 'disabled']
            if codec == 'libx264':
                command += ['-preset', 'medium', '-crf', str(quality), '-pix_fmt', 'yuv420p', '-x264-params', 'colorprim=bt709:transfer=bt709:colormatrix=bt709']
            elif codec == 'libx265':
                command += ['-preset', 'medium', '-crf', str(quality), '-x265-params', 'pools=1:frame-threads=1:colorprim=bt709:transfer=bt709:colormatrix=bt709', '-tag:v', 'hvc1']
            elif codec == 'libsvtav1':
                command += ['-preset', '8', '-crf', str(quality), '-svtav1-params', 'lp=2']
            elif codec.endswith('_vaapi'):
                command += ['-vf', 'format=nv12,hwupload', '-qp', str(quality)]
            else:
                command += ['-rc', 'constqp', '-qp', str(quality)]
            command += ['-color_range', 'tv', '-colorspace', 'bt709', '-color_trc', 'bt709', '-color_primaries', 'bt709',
                        '-movflags', '+faststart', target]
            row = {'codec': codec, 'quality_parameter': quality, 'status': 'rejected'}
            try:
                measurement = run(command, case, deadline)
                scores = validate(target, name)
                meets = meets_target(scores)
                smaller = target.stat().st_size < source.stat().st_size
                row.update(status='metric_pass' if meets else 'quality_rejected', scores=scores,
                           source_bytes=source.stat().st_size, output_bytes=target.stat().st_size,
                           wall_seconds=measurement['wall_seconds'], peak_rss_kib=measurement['peak_rss_kib'],
                           command=measurement['command'], smaller=smaller)
                if meets:
                    if smaller and (best is None or target.stat().st_size < best[0]):
                        best = (target.stat().st_size, target, scores, command)
                    low = quality + 1
                else:
                    high = quality - 1
            except Rejected as error:
                row['reason'] = str(error)
                rows.append(row)
                break
            rows.append(row)
        if best:
            # Repeat the selected full encode and revalidate all frames/audio;
            # a successful search trial alone cannot promote a candidate.
            repeat = {'codec': codec, 'status': 'rejected'}
            try:
                selected = case / f'{codec}-final-repeat.mp4'
                command = [*best[3][:-1], selected]
                measurement = run(command, case, deadline)
                scores = validate(selected, f'{codec}-final-repeat')
                if not meets_target(scores) or selected.stat().st_size >= source.stat().st_size:
                    raise Rejected('Repeated full candidate failed quality or size criteria')
                repeat.update(status='metric_pass', scores=scores, output_bytes=selected.stat().st_size,
                              wall_seconds=measurement['wall_seconds'], peak_rss_kib=measurement['peak_rss_kib'],
                              command=measurement['command'], first_trial_sha256=sha256(best[1]),
                              repeat_sha256=sha256(selected))
                review = case / f'review-{codec}'
                review.mkdir()
                pair = [(reference, 'source'), (selected, 'candidate')]
                if os.urandom(1)[0] & 1:
                    pair.reverse()
                for label, (path, _) in zip(['A', 'B'], pair):
                    frame = scores['worst_frame']
                    run([FF, '-v', 'error', '-nostdin', '-threads', '2', '-i', path, '-vf', f'select=eq(n\\,{frame})',
                         '-frames:v', '1', '-threads', '1', '-fps_mode', 'passthrough', review / f'{label}.png'], case, deadline)
                write_json(review / 'reviewer-key.json', {label: kind for label, (_, kind) in zip(['A', 'B'], pair)})
                repeat['blind_review_directory'] = str(review)
            except Rejected as error:
                repeat['reason'] = str(error)
                # A/B export may fail after repeat validation: keep both facts.
                repeat['subjective_review'] = 'export_incomplete'
            repeats.append(repeat)
    if sha256(source) != expected:
        raise Rejected('Source changed while benchmarking')
    return {'id': entry['id'], 'status': 'measured', 'cases': rows, 'full_encode_repeats': repeats, 'source_sha256': expected,
            'source_video': {k:v.get(k) for k in ['codec_name','width','height','pix_fmt','avg_frame_rate','time_base']},
            'duration_seconds': duration, 'source_color_contract': '8-bit BT.709 limited-range YUV420', 'subjective_review': 'pending'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--init-manifest', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--register', metavar='CLIP_ID')
    parser.add_argument('--path', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--codecs', nargs='+', choices=CODECS, default=['libx264', 'libx265', 'libsvtav1'])
    parser.add_argument('--seconds-per-clip', type=int, default=180)
    parser.add_argument('--max-candidates', type=int, default=4)
    parser.add_argument('--max-work-mib', type=int, default=4096)
    parser.add_argument('--ssim-target', type=float, default=0.99)
    parser.add_argument('--minimum-frame-ssim', type=float)
    parser.add_argument('--vmaf-model', type=Path)
    parser.add_argument('--vaapi-device', default='/dev/dri/renderD128')
    args = parser.parse_args()
    global WORK_LIMIT
    if not 512 <= args.max_work_mib <= 16384:
        parser.error("Workspace budget must be 512–16384 MiB per clip")
    WORK_LIMIT = args.max_work_mib * 1024**2
    if args.init_manifest:
        if args.init_manifest.exists():
            parser.error('Refusing to overwrite an existing corpus manifest')
        write_json(args.init_manifest, {'schema': 1, 'sources': CATALOG,
                   'clips': [{'id': f'{source}-{index:02}', 'source': source, 'path': None,
                              'sha256': None, 'scene_review': 'pending'} for source in CATALOG for index in range(1, 6)]})
        return 0
    if not args.manifest:
        parser.error('--manifest is required')
    args.manifest = args.manifest.resolve()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    ids = [e.get('id') for e in manifest.get('clips', [])]
    if not 1 <= len(ids) <= 30 or len(set(ids)) != len(ids) or any(not isinstance(i,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,80}',i) for i in ids):
        parser.error('Expected 1–30 uniquely named registered clips')
    if args.register:
        if not args.path or not args.path.is_file():
            parser.error('--register requires an existing local --path')
        matches = [e for e in manifest['clips'] if e['id'] == args.register]
        if len(matches) != 1:
            parser.error('Unknown or duplicate clip ID')
        matches[0].update(path=str(args.path.resolve()), sha256=sha256(args.path))
        write_json(args.manifest, manifest)
        return 0
    if not args.output or not args.work_dir:
        parser.error('--output and --work-dir are required')
    if not 0 < args.seconds_per_clip <= 3600 or not 1 <= args.max_candidates <= 12 or not 0 < args.ssim_target <= 1:
        parser.error('Invalid time/candidate/SSIM budget')
    if args.minimum_frame_ssim is not None and not 0 < args.minimum_frame_ssim <= 1:
        parser.error('Invalid minimum per-frame SSIM')
    args.work_dir = args.work_dir.resolve()
    args.output = args.output.resolve()
    if args.vmaf_model:
        args.vmaf_model = args.vmaf_model.resolve(strict=True)
    if args.work_dir.exists():
        parser.error('Use a new work directory to preserve previous experiments')
    args.work_dir.mkdir(parents=True)
    report = {'schema': 1, 'scope': 'Research candidates; full registered clips, SDR-only metrics; no automatic promotion or source replacement',
              'rss_measurement': 'GNU time maximum child-process RSS; not aggregate simultaneous process-tree RSS',
              'workspace_limit_bytes_per_clip': WORK_LIMIT,
              'ffmpeg_version': run([FF, '-version'], args.work_dir, time.monotonic() + 10)['stdout'].splitlines()[0],
              'ssim_research_target': args.ssim_target, 'minimum_frame_ssim': args.minimum_frame_ssim,
              'vmaf_model_sha256': sha256(args.vmaf_model) if args.vmaf_model else None,
              'clips': [], 'release_ready': False, 'subjective_review': 'pending'}
    for entry in manifest['clips']:
        source = manifest['sources'].get(entry.get('source'))
        if not source or not source.get('license') or not source.get('credit') or not source.get('source_url'):
            row = {'id': entry.get('id'), 'status': 'missing_provenance'}
        else:
            try:
                row = benchmark(entry, args.manifest.parent, args, args.work_dir)
            except (Rejected, OSError, ValueError) as error:
                row = {'id': entry['id'], 'status': 'rejected', 'reason': str(error)}
        report['clips'].append(row)
        report['measured_clip_count'] = sum(x['status'] == 'measured' for x in report['clips'])
        report['corpus_20_to_30_measured'] = 20 <= report['measured_clip_count'] <= 30
        write_json(args.output, report)
        print(row['id'], row['status'], flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
