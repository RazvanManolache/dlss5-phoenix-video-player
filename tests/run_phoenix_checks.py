"""Development-only native playback/export checks; no Python ships with the player."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import os

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args, timeout=180):
    with tempfile.TemporaryDirectory(prefix='phoenix-test-recovery-',dir=ROOT/'build') as folder:
        env=os.environ.copy();env['PHOENIX_RECOVERY_ROOT']=folder
        done = subprocess.run([str(x) for x in args], capture_output=True, text=True,
                              encoding='utf-8', errors='replace', timeout=timeout, cwd=ROOT,env=env)
    if done.returncode:
        raise RuntimeError(f'{Path(args[0]).name} failed: {done.stdout[-3000:]} {done.stderr[-3000:]}')
    return done.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu-models', action='store_true')
    parser.add_argument('--media-only', action='store_true', help='Run media/cache checks after separate renderer validation')
    parser.add_argument('--helpers', type=Path, default=Path('C:/Utils/FFMpeg/bin'))
    parser.add_argument('--model-dll', type=Path, default=ROOT.parent / 'research/model-compatibility-310.9.16/nvngx_dlssnr.310.9.16.dll')
    args = parser.parse_args()
    build = ROOT / 'build/current'
    native = ROOT.parent / 'dlss5-phoenix-addon/artifacts/current/pipeline-native/phoenix_native.dll'
    bundle = ROOT.parent / 'dlss5-phoenix-addon/artifacts/current/native-kernels'
    output = ROOT / 'artifacts/current'
    output.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'native_library_sha256': digest(native), 'cases': [],
              'media_tests_sha256':digest(build/'PhoenixMediaJobTests.exe'),'player_sha256':digest(build/'PhoenixVideoPlayer.exe')}
    try:
        with tempfile.TemporaryDirectory(prefix='phoenix-checks-', dir=ROOT / 'build') as folder:
            work = Path(folder)
            spatial = work / 'spatial'
            spatial.mkdir()
            if not args.media_only:
                status = json.loads(run([build / 'PhoenixPipelineTests.exe', native, spatial]))
                report['cases'].append({'suite': 'playback-comparison', 'result': status})
                print('Playback/comparison passed.', flush=True)
            cases = [('spatial', 64, 40, None)]
            scaled = work / 'spatial-2x.json'
            scaled.write_text(json.dumps({'schema':'phoenix-media/1','native_library':str(native),
                'native_library_sha256':digest(native),'runtime':{'spatial_only':True,'warp':True},
                'pipeline':{'PipelineMonitoring':0},'output_scale':2}),encoding='utf-8')
            cases.append(('spatial-2x',64,40,scaled))
            if args.gpu_models:
                model_hash = digest(args.model_dll)
                for model in ('original', 'small-fp8'):
                    recipe = work / (model + '.json')
                    recipe.write_text(json.dumps({'schema': 'phoenix-media/1',
                        'native_library': str(native), 'native_library_sha256': digest(native),
                        'runtime': {'bundle_path': str(bundle), 'weight_source': {'kind': 'nvidia_dll',
                            'path': str(args.model_dll), 'sha256': model_hash, 'model': model}},
                        'pipeline': {'MotionEstimation': True, 'MotionProvider': 2,
                            'SceneDetection': True, 'PipelineMonitoring': 0}}), encoding='utf-8')
                    checks = work / ('renderer-' + model)
                    checks.mkdir()
                    if not args.media_only:
                        status = json.loads(run([build / 'PhoenixPipelineTests.exe', native, checks, recipe]))
                        report['cases'].append({'suite': 'renderer-' + model, 'result': status})
                        print(model + ' renderer passed.', flush=True)
                    cases.append((model, 320, 180, recipe))
            for name, width, height, recipe in cases:
                case = work / ('jobs-' + name)
                case.mkdir()
                clip = case / 'input.mkv'
                run([args.helpers / 'ffmpeg.exe', '-hide_banner', '-loglevel', 'error', '-nostdin',
                    '-f', 'lavfi', '-i', f'testsrc2=size={width}x{height}:rate=30', '-frames:v', '8',
                    '-c:v', 'ffv1', clip])
                command = [build / 'PhoenixMediaJobTests.exe', native, case, args.helpers, clip, '8']
                if recipe:
                    command.append(recipe)
                status = json.loads(run(command, timeout=300))
                # Independent decoded-pixel oracle. Container byte equality is not
                # required: muxers may write different dates or segment identifiers.
                hashes = []
                for filename in ('resumed.mkv', 'baseline.mkv', 'cached.mkv'):
                    decoded = subprocess.run([str(args.helpers / 'ffmpeg.exe'), '-hide_banner',
                        '-loglevel', 'error', '-nostdin', '-i', str(case / filename),
                        '-f', 'rawvideo', '-pix_fmt', 'bgra', '-'], capture_output=True, check=True, timeout=60)
                    output_width,output_height=status['output_extent']
                    if len(decoded.stdout) != output_width * output_height * 4 * 8:
                        raise RuntimeError('Export has a different decoded frame count')
                    hashes.append(hashlib.sha256(decoded.stdout).hexdigest())
                if len(set(hashes)) != 1:
                    raise RuntimeError('Resumed and continuous exports differ in decoded pixels')
                status.update(decoded_resume_matches_continuous=True, decoded_sha256=hashes[0])
                report['cases'].append({'suite': 'media-resume-' + name, 'result': status})
                print(name + ' resumable export passed.', flush=True)
        report['passed'] = True
    finally:
        (output / 'phoenix-integration-validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
