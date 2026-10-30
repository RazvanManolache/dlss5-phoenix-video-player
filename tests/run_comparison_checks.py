"""Development-only pixel checks; uses hidden render targets, no desktop control."""
import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/current'
OUTPUT = ROOT / 'artifacts/current'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--youtube-url', help='Optional public video to inspect with the production resolver/decoder')
    parser.add_argument('--seek', type=float, default=43)
    args = parser.parse_args()
    recipe = json.loads((BUILD / 'phoenix-media.json').read_text(encoding='utf-8'))
    # Comparison fixtures own their runtime selection. Portable replacement has
    # its own live matrix; a saved developer recipe may point at an older package.
    recipe.pop('portable_package', None)
    recipe.pop('execution_devices', None)
    native = Path(recipe['native_library'])
    env = os.environ.copy()
    env.pop('PHOENIX_MEDIA_RECIPE', None)
    report = {'passed': False, 'cases': [], 'player_sha256':
              hashlib.sha256((BUILD / 'PhoenixVideoPlayer.exe').read_bytes()).hexdigest()}
    OUTPUT.mkdir(parents=True, exist_ok=True)

    def check(name, command):
        isolated = env.copy(); isolated['PHOENIX_RECOVERY_ROOT'] = str(work / ('recovery-' + name))
        done = subprocess.run([str(x) for x in command], env=isolated, cwd=ROOT,
                              capture_output=True, text=True, timeout=150)
        if done.returncode:
            raise RuntimeError(name + ': ' + done.stderr[-1500:])
        data = json.loads(done.stdout)
        report['cases'].append({'name': name, 'result': data})
        print(name + ' passed.', flush=True)
        return data

    try:
        with tempfile.TemporaryDirectory(prefix='comparison-check-', dir=ROOT / 'build') as folder:
            work = Path(folder)
            spatial = work / 'spatial'
            spatial.mkdir()
            check('spatial-transfer', [BUILD / 'PhoenixPipelineTests.exe', native, spatial])
            hardware = work / 'hardware-transfer';hardware.mkdir()
            check('hardware-sdr-hdr-transfer',[BUILD / 'PhoenixPipelineTests.exe',native,hardware,'--transfer-gpu'])
            # Exercise real executable-adjacent recipe discovery in an isolated
            # folder, without borrowing or changing the developer's saved recipe.
            standalone = work / 'PhoenixPipelineTests.exe'
            shutil.copy2(BUILD / standalone.name, standalone)
            (work / 'phoenix-media.json').write_text(json.dumps(recipe), encoding='utf-8')
            check('standalone-bgra-nv12', [standalone, native, work, '--standalone'])
            for model in ('original', 'small-fp8'):
                selected = json.loads(json.dumps(recipe))
                selected['runtime']['weight_source']['model'] = model
                selected.pop('model', None)
                path = work / (model + '.json')
                path.write_text(json.dumps(selected), encoding='utf-8')
                check(model + '-bgra-nv12', [BUILD / 'PhoenixPipelineTests.exe', native, work, path])
            if args.youtube_url:
                source = check('media-source', [BUILD / 'PhoenixYouTubeProbe.exe', args.youtube_url, work, args.seek])
                width, height = source['first_frame_extent']
                planar = work / 'input.nv12'
                # Feed the same source through the NV12 path the player uses.
                # Retain neither signed stream URLs nor a duplicate source video.
                subprocess.run([str(BUILD / 'ffmpeg.exe'), '-hide_banner', '-loglevel', 'error', '-nostdin',
                    '-f', 'rawvideo', '-pixel_format', 'bgra', '-video_size', f'{width}x{height}',
                    '-i', str(work / 'input.bgra'), '-vf',
                    'scale=in_range=full:out_range=limited:out_color_matrix=bt709',
                    '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'nv12', str(planar)], check=True)
                destination = OUTPUT / 'split-comparison/trailer'
                destination.mkdir(parents=True, exist_ok=True)
                data = check('media-nv12-window', [BUILD / 'PhoenixPipelineTests.exe', native, destination,
                    BUILD / 'phoenix-media.json', planar, width, height, 'NV12', 'scaled'])
                data['source'] = source
                data['source_video'] = args.youtube_url
                data['image_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                        for p in destination.glob('*.png')}
                (destination / 'validation.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
            report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (OUTPUT / 'comparison-validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
