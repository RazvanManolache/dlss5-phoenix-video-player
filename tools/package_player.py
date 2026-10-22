"""Development packager: one current model-free player package, no deployed Python."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import zipfile
import sys

ROOT = Path(__file__).resolve().parents[1]
ADDON = ROOT.parent / 'dlss5-phoenix-addon'
sys.path.insert(0,str(ADDON/'tools'))
from prepare_addon_release import safe_payload
from build_experimental_rtx_addon import extract_runtime


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write(path, document):
    path.write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')


def plain(path):
    for part in (path, *path.parents):
        if part.is_symlink() or part.is_junction():
            raise RuntimeError('Package path traverses a link: ' + str(path))


def install(stage):
    destination = ROOT.parent / 'dlss5-everything/runtime/media'
    plain(destination)
    current = read(stage / 'package-manifest.json')['files']
    previous = read(destination / 'package-manifest.json')['files'] if destination.exists() else {}
    for name in [*current, *previous]:
        relative=PurePosixPath(name)
        if relative.is_absolute() or relative.as_posix()!=name or '..' in relative.parts or '\\' in name or ':' in name:
            raise RuntimeError('Invalid package member path')
    if destination.exists():
        for path in destination.rglob('*'):
            plain(path)
            if not path.is_file():
                continue
            name = path.relative_to(destination).as_posix()
            if name == 'package-manifest.json':
                continue
            if name not in previous and name not in current:
                raise RuntimeError('Preserving an unknown package file: ' + name)
            if sha(path) not in {previous.get(name), current.get(name)}:
                raise RuntimeError('Preserving a locally modified package file: ' + name)
    destination.mkdir(parents=True, exist_ok=True)
    for name in [*current, 'package-manifest.json']:
        source = stage / name
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and sha(target) == sha(source):
            continue
        temporary = target.with_name(target.name + '.package.tmp')
        if temporary.exists():
            raise RuntimeError('Preserving an interrupted package staging file: ' + str(temporary))
        try:
            shutil.copy2(source, temporary)
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
    for name in previous.keys() - current.keys():
        (destination / name).unlink(missing_ok=True)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage-everything', action='store_true')
    parser.add_argument('--bundle',type=Path,default=ADDON/'artifacts/current/native-kernels')
    args = parser.parse_args()
    build = ROOT / 'build/current'
    sdk = ADDON / 'artifacts/current/pipeline-native'
    bundle = args.bundle.resolve()
    checks = {
        'native': sdk / 'validation.json',
        'comparison': ROOT / 'artifacts/current/comparison-validation.json',
        'media': ROOT / 'artifacts/current/phoenix-integration-validation.json',
        'live_playback': ROOT / 'artifacts/current/live-playback-validation.json',
        'portable_player': ROOT / 'artifacts/current/portable-player-validation.json',
    }
    evidence = {name: read(path) for name, path in checks.items()}
    if not all(item.get('passed') for item in evidence.values()):
        raise RuntimeError('Required native/player validation has not passed')
    if not evidence['native'].get('public_binary_audit_passed'):
        raise RuntimeError('Native SDK public-binary audit has not passed')
    portable_checks=next((case['result'] for case in evidence['native']['cases'] if case['suite']=='portable-shared-pipeline'),None)
    if not portable_checks or not portable_checks.get('passed'):
        raise RuntimeError('Shared portable backend pipeline has not passed validation')
    capture=portable_checks.get('asynchronous_capture_bridge',{})
    if not all(capture.get(key) for key in ('neural_appearance_oracle','zero_intensity_source_oracle','worker_identity_and_retirement')):
        raise RuntimeError('Real capture-worker neural/appearance and process-lifetime checks are missing')
    if not all(case.get('distinct_native_worker_identities') for case in portable_checks['cases']):
        raise RuntimeError('Independent portable stage worker checks are missing')
    for path in [sdk/'phoenix_native.dll',sdk/'phoenix-process.exe',build/'PhoenixVideoPlayer.exe',build/'phoenix-media.exe']:
        safe_payload('runtime/'+path.name,path.read_bytes())
    native_hash = sha(sdk / 'phoenix_native.dll')
    portable_player=evidence['portable_player']
    if native_hash!=portable_player.get('native_library_sha256') or sha(build/'PhoenixVideoPlayer.exe')!=portable_player.get('player_sha256') or sha(build/'PhoenixPipelineTests.exe')!=portable_player.get('test_sha256') or sha(build/'PhoenixMediaJobTests.exe')!=portable_player.get('media_tests_sha256'):
        raise RuntimeError('Portable playback/export validation describes another build')
    if len(portable_player.get('cases',[]))<4 or not all(case.get('media',{}).get('decoded_resume_matches_continuous') for case in portable_player['cases']):
        raise RuntimeError('Portable playback and resumable export checks are missing')
    expected={(case['device'],case['model']) for case in portable_checks['cases']}
    if {(case['device'],case['model']) for case in portable_player['cases']}!=expected:
        raise RuntimeError('Portable player coverage differs from the validated backend/model matrix')
    if native_hash != evidence['native']['native_dll_sha256'] or native_hash != evidence['media']['native_library_sha256']:
        raise RuntimeError('SDK validation describes another binary')
    if sha(build / 'PhoenixVideoPlayer.exe') != evidence['comparison']['player_sha256']:
        raise RuntimeError('Comparison validation describes another player')
    if sha(build/'PhoenixMediaJobTests.exe')!=evidence['media'].get('media_tests_sha256') or sha(build/'PhoenixVideoPlayer.exe')!=evidence['media'].get('player_sha256'):
        raise RuntimeError('Media validation describes another build')
    live=evidence['live_playback']
    if sha(build/'PhoenixVideoPlayer.exe')!=live.get('player_sha256') or sha(build/'PhoenixLivePlaybackTests.exe')!=live.get('test_sha256'):
        raise RuntimeError('Live playback validation describes another build')
    attachment=live.get('result',{})
    if attachment.get('cases')!=4 or attachment.get('failure_paths')!=8 or attachment.get('comparison_readbacks')!=8 or not attachment.get('decoder_swap'):
        raise RuntimeError('Required real player attachment/rollback checks are missing')
    media_cases = [case['result'] for case in evidence['media']['cases'] if case['suite'].startswith('media-resume-')]
    if not media_cases or not all(case.get('live_segments_authenticated_and_republished') and
                                 case.get('live_pair_synchronized') and case.get('active_cache_preserved') and case.get('inactive_native_cache_cleared') for case in media_cases):
        raise RuntimeError('Native cache/segment checks are missing')
    manifest = read(bundle / 'native_manifest.json')
    if manifest.get('model_weights_included') is not False or manifest.get('captured_plans_included') is not False:
        raise RuntimeError('Kernel bundle is not declared model-free')
    for kernel in manifest['kernels']:
        path = (bundle / kernel['binary']).resolve()
        if not path.is_relative_to(bundle.resolve()) or sha(path) != kernel['binary_sha256']:
            raise RuntimeError('Kernel identity differs: ' + kernel['binary'])
    for name, key in [('native_graph.json','native_graph_sha256'), ('native_graph_v2.json','native_graph_v2_sha256')]:
        if key not in manifest:continue
        if sha(bundle / name) != manifest[key]:
            raise RuntimeError('Kernel graph identity differs')
    helpers = read(build / 'media-helpers.json')
    if not helpers.get('ready'):
        raise RuntimeError('Media helper package is incomplete')
    output = ROOT / 'artifacts/current'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='player-package-', dir=ROOT / 'build') as temporary:
        stage = Path(temporary) / 'PhoenixVideoPlayer'
        stage.mkdir()
        for name in ['PhoenixVideoPlayer.exe', 'phoenix-media.exe', 'media-helpers.json']:
            shutil.copy2(build / name, stage / name)
        for tool in helpers['tools']:
            if tool['name'] not in {'ffmpeg.exe','ffprobe.exe','yt-dlp.exe','deno.exe'} or sha(build / tool['name']) != tool['sha256']:
                raise RuntimeError('Helper identity differs: ' + tool['name'])
            shutil.copy2(build / tool['name'], stage / tool['name'])
        shutil.copy2(sdk / 'phoenix_native.dll', stage / 'phoenix_native.dll')
        shutil.copy2(sdk / 'phoenix-process.exe', stage / 'phoenix-process.exe')
        source_addon=ADDON/'artifacts/current/addon/Phoenix.addon64'
        if sha(source_addon)!=portable_checks['source_addon_sha256']:
            raise RuntimeError('Portable provider source differs from its validation')
        provider=Path(temporary)/'portable-provider';resource_scratch=Path(temporary)/'portable-resources'
        provider.mkdir();resource_scratch.mkdir();extract_runtime(source_addon,provider,resource_scratch)
        shutil.copy2(ADDON/'artifacts/current/portable-pipeline/phoenix_runtime_cpp.dll',provider/'phoenix_runtime_cpp.dll')
        portable_files={}
        for name,digest in portable_checks['provider_files'].items():
            source=provider/name
            if sha(source)!=digest:raise RuntimeError('Portable dependency differs: '+name)
            safe_payload('runtime/'+name,source.read_bytes())
            target=stage/'portable-runtime'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
            portable_files[name]=digest
        graphs=['native_graph.json']+(['native_graph_v2.json'] if 'native_graph_v2_sha256' in manifest else [])
        for name in sorted({'native_manifest.json',*graphs,*[kernel['binary'] for kernel in manifest['kernels']]}):
            source=bundle/name;plain(source)
            target=stage/'native-kernels'/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        shutil.copytree(build / 'THIRD_PARTY_LICENSES', stage / 'THIRD_PARTY_LICENSES')
        for name in ['LICENSE', 'THIRD_PARTY.md']:
            shutil.copy2(ROOT / name, stage / name)
        shutil.copy2(ROOT / 'third_party/video-player/LICENSE', stage / 'THIRD_PARTY_LICENSES/player-upstream-MIT.txt')
        shutil.copy2(ADDON / 'THIRD_PARTY.md', stage / 'THIRD_PARTY_LICENSES/Phoenix-runtime-notices.md')
        shutil.copy2(ADDON / 'NOTICE', stage / 'THIRD_PARTY_LICENSES/Phoenix-runtime-NOTICE.txt')
        for dependency in ['aio-motion','nis','tiny-distill']:
            for source in (ADDON / 'third_party' / dependency).rglob('*'):
                if source.is_file() and ('license' in source.name.lower() or 'notice' in source.name.lower()):
                    target=stage/'THIRD_PARTY_LICENSES'/dependency/source.relative_to(ADDON/'third_party'/dependency)
                    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        shutil.copy2(ROOT / 'tools/Configure-PhoenixPlayer.ps1', stage / 'Configure-PhoenixPlayer.ps1')
        write(stage / 'phoenix-runtime.json', {'schema':'phoenix-media-runtime/1','native_library':'phoenix_native.dll',
            'native_library_sha256':native_hash,'bundle_path':'native-kernels',
            'portable_runtime':{'root':'portable-runtime','files':portable_files}})
        (stage / 'README.txt').write_text(
            'Phoenix Video Player - experimental native Windows x64 package\n\n'
            'Everything can discover this companion under Runtime/media or Components/Media.\n'
            'It supplies a recipe pointing to its central model library. No model is bundled.\n'
            'For standalone use, run Configure-PhoenixPlayer.ps1 -PlayerDirectory . '
            '-ModelDll C:\\Models\\nvngx_dlssnr.dll -Model original (or small-fp8).\n'
            'That model remains external. Native processing was checked on RTX 5080; this\n'
            'package does not establish older GPU, game, NVIDIA parity or frame-rate claims.\n'
            'Kernel variants in this package: '+', '.join(manifest.get('architectures',['sm_120']))+'.\n'
            'Media jobs currently export SDR constant-frame-rate video. VSR is deferred.\n'
            'All processing and helpers are native executables; Python is not required.\n'
            'See THIRD_PARTY.md and THIRD_PARTY_LICENSES for upstream credits/notices.\n', encoding='utf-8')
        files={path.relative_to(stage).as_posix():sha(path) for path in sorted(stage.rglob('*')) if path.is_file()}
        forbidden={'nvngx_dlssnr.dll','weights.bin','phoenix-media.json','phoenixvideoplayer.log'}
        if any(Path(name).name.lower() in forbidden for name in files):
            raise RuntimeError('Package contains a model or local session file')
        write(stage / 'package-manifest.json', {'schema':'phoenix-player-package/1','model_weights_included':False,
            'files':files,'checks':{name:sha(path) for name,path in checks.items()}})
        archive=Path(temporary)/'PhoenixVideoPlayer-Windows-x64.zip'
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=5) as package:
            for path in sorted(stage.rglob('*')):
                if path.is_file():package.write(path,path.relative_to(stage).as_posix())
        with zipfile.ZipFile(archive) as package:
            if package.testzip():raise RuntimeError('Package CRC check failed')
            for name,digest in files.items():
                if hashlib.sha256(package.read(name)).hexdigest()!=digest:raise RuntimeError('Packaged file differs: '+name)
        destination=install(stage) if args.stage_everything else None
        target=output/archive.name;os.replace(archive,target)
        write(output/'package-validation.json',{'passed':True,'sha256':sha(target),'bytes':target.stat().st_size,
            'files':len(files)+1,'model_weights_included':False,'archive_files_verified':True,
            'everything_staged':destination is not None})
        print('Verified package:',target,flush=True)
        if destination:print('Everything media payload staged:',destination,flush=True)


if __name__ == '__main__':
    main()
