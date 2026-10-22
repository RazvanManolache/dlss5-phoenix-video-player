"""Actual portable playback/backend replacement checks; native executables only at deployment."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil

ROOT=Path(__file__).resolve().parents[1]
ADDON=ROOT.parent/'dlss5-phoenix-addon'
sys.path.insert(0,str(ADDON/'tools'))
from build_experimental_rtx_addon import extract_runtime


def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    build=ROOT/'build/current';sdk=ADDON/'artifacts/current/pipeline-native/phoenix_native.dll'
    model=ROOT.parent/'research/model-compatibility-310.9.16/nvngx_dlssnr.310.9.16.dll'
    report={'passed':False,'native_library_sha256':sha(sdk),'player_sha256':sha(build/'PhoenixVideoPlayer.exe'),
        'test_sha256':sha(build/'PhoenixPipelineTests.exe'),'media_tests_sha256':sha(build/'PhoenixMediaJobTests.exe'),
        'cases':[],'desktop_automation':False}
    try:
        with tempfile.TemporaryDirectory(prefix='portable-player-',dir=ROOT/'build') as temporary:
            work=Path(temporary);provider=work/'provider';resources=work/'resources';provider.mkdir();resources.mkdir()
            extract_runtime(ADDON/'artifacts/current/addon/Phoenix.addon64',provider,resources)
            shutil.copy2(ADDON/'artifacts/current/portable-pipeline/phoenix_runtime_cpp.dll',provider/'phoenix_runtime_cpp.dll')
            inventory=json.loads(subprocess.check_output([str(provider/'phoenix_cpp_devices.exe'),str(provider)],text=True))['devices']
            devices=[item for item in inventory if item['id']=='cpu' or item['id'].startswith(('cuda:','vulkan:')) and item.get('capable',True)]
            package={'runtime_root':str(provider),'runtime_sha256':sha(provider/'phoenix_runtime_cpp.dll'),'allow_unvalidated':True,'cpu_threads':2}
            for device in devices:
                for selected in ('original','small-fp8'):
                    identifier=device['id'];case=work/(selected+'-'+str(len(report['cases'])));case.mkdir()
                    portable={**package,'backend':'cpu' if identifier=='cpu' else identifier.split(':')[0],'device':identifier}
                    recipe={'schema':'phoenix-media/1','model':selected,'native_library':str(sdk),'native_library_sha256':sha(sdk),
                        'runtime':{'bundle_path':str(ADDON/'artifacts/current/native-kernels'),'portable_runtime':portable,
                            'weight_source':{'kind':'nvidia_dll','path':str(model),'sha256':sha(model),'model':selected}},
                        'pipeline':{'MotionEstimation':True,'SceneDetection':True,'PipelineMonitoring':0},
                        'tuning':{'central_blocks':1 if identifier=='cpu' else 8},'portable_package':package,'execution_devices':devices}
                    path=case/'recipe.json';path.write_text(json.dumps(recipe),encoding='utf-8')
                    env=os.environ.copy();env['PHOENIX_MEDIA_RECIPE']=str(path);env['PHOENIX_RECOVERY_ROOT']=str(case/'recovery')
                    result=subprocess.run([str(build/'PhoenixPipelineTests.exe'),str(sdk),str(case),str(path),'--portable'],
                        env=env,cwd=ROOT,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=240)
                    if result.returncode:raise RuntimeError(identifier+' '+selected+': '+result.stderr[-3000:])
                    data=json.loads(result.stdout)
                    if not data.get('passed') or data['runtime_backend']!='portable_pipeline_d3d12':raise RuntimeError('Portable renderer bypassed requested execution')
                    clip=case/'input.mkv'
                    subprocess.run([str(build/'ffmpeg.exe'),'-hide_banner','-loglevel','error','-nostdin',
                        '-f','lavfi','-i','testsrc2=size=32x24:rate=30','-frames:v','8','-c:v','ffv1',str(clip)],check=True,timeout=30)
                    jobs=case/'jobs';jobs.mkdir()
                    result=subprocess.run([str(build/'PhoenixMediaJobTests.exe'),str(sdk),str(jobs),str(build),str(clip),'8',str(path)],
                        env=env,cwd=ROOT,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=300)
                    if result.returncode:raise RuntimeError(identifier+' '+selected+' export: '+result.stderr[-3000:])
                    media=json.loads(result.stdout);hashes=[]
                    for name in ('resumed.mkv','baseline.mkv','cached.mkv'):
                        decoded=subprocess.run([str(build/'ffmpeg.exe'),'-hide_banner','-loglevel','error','-nostdin',
                            '-i',str(jobs/name),'-f','rawvideo','-pix_fmt','bgra','-'],capture_output=True,check=True,timeout=30)
                        if len(decoded.stdout)!=32*24*4*8:raise RuntimeError('Portable export lost source frames')
                        hashes.append(hashlib.sha256(decoded.stdout).hexdigest())
                    if len(set(hashes))!=1 or media['backend']!='portable_pipeline_d3d12':raise RuntimeError('Portable resumed export differs')
                    media.update(decoded_resume_matches_continuous=True,decoded_sha256=hashes[0])
                    report['cases'].append({'device':identifier,'model':selected,'result':data,'media':media})
                    print(selected+' '+identifier+': playback, comparisons, backend replacement and resumable exports passed.',flush=True)
            report['passed']=True
    except Exception as error:
        report['error']=str(error);raise
    finally:
        output=ROOT/'artifacts/current/portable-player-validation.json';output.parent.mkdir(parents=True,exist_ok=True)
        output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
