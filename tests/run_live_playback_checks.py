"""Development-only real player attachment/readback regressions; no desktop automation."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    build = ROOT / 'build/current'
    binary = build / 'PhoenixLivePlaybackTests.exe'
    report = {'passed': False, 'test_sha256': sha(binary), 'player_sha256': sha(build / 'PhoenixVideoPlayer.exe'),
              'desktop_automation': False, 'audio_fixture': False}
    target = ROOT / 'artifacts/current/live-playback-validation.json'
    try:
        with tempfile.TemporaryDirectory(prefix='phoenix-live-check-', dir=ROOT / 'build') as folder:
            work = Path(folder)
            # Isolate the test executable's INI and all native helper children.
            for name in ('PhoenixLivePlaybackTests.exe', 'ffmpeg.exe', 'ffprobe.exe'):
                os.link(build / name, work / name)
            clips = []
            for name, color, extent in [('original', 'red', '64x40'), ('neural', 'green', '64x40'), ('neural-2x', 'green', '128x80')]:
                clip = work / (name + '.mp4')
                subprocess.run([str(work/'ffmpeg.exe'), '-hide_banner', '-loglevel', 'error', '-nostdin',
                    '-f', 'lavfi', '-i', f'color={color}:s={extent}:r=30:d=2', '-an', '-c:v', 'libx264',
                    '-pix_fmt', 'yuv420p', '-colorspace', 'bt709', '-color_primaries', 'bt709',
                    '-color_trc', 'bt709', '-color_range', 'tv', str(clip)], check=True, timeout=60)
                clips.append(clip)
            # A selected Phoenix recipe bypasses unrelated NGX initialization;
            # cached rendering must not initialize or execute its neural model.
            recipe = work / 'recipe.json'
            recipe.write_text(json.dumps({'schema':'phoenix-media/1','runtime':{'spatial_only':True}}))
            env = os.environ.copy()
            env['PHOENIX_MEDIA_RECIPE'] = str(recipe)
            env['PHOENIX_RECOVERY_ROOT'] = str(work / 'recovery')
            result = subprocess.run([str(work/binary.name), *map(str, clips)], cwd=work, env=env,
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180)
            if result.returncode:
                logs = '\n'.join(p.read_text(errors='replace')[-5000:] for p in work.glob('*.log'))
                raise RuntimeError(result.stdout[-1000:] + result.stderr[-2000:] + logs[-3000:])
            report['result'] = json.loads(result.stdout)
            report['passed'] = report['result']['passed']
            print(json.dumps(report['result']), flush=True)
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
