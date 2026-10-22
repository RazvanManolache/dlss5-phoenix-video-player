"""Development-only isolated native pipeline measurements and reproducible pictures.

Times the synchronous host API, including upload/readback. This is not a game
FPS test. Python/numpy/Pillow/matplotlib are never part of the shipped player.
"""
import argparse
import ctypes as C
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ADDON = ROOT.parent / 'dlss5-phoenix-addon'
OUT = ROOT / 'artifacts/current/pipeline-measurements'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


class Controls(C.Structure):
    _fields_ = [('bytes', C.c_uint32), ('reset', C.c_uint32),
                ('mx', C.c_float), ('my', C.c_float), ('tone', C.c_float),
                ('structure', C.c_float), ('skin', C.c_float),
                ('mask', C.c_uint32), ('inverted', C.c_uint32)]


def stats(values):
    return {'samples': len(values), 'median_ms': float(np.median(values)),
            'p95_ms': float(np.percentile(values, 95)), 'min_ms': min(values),
            'max_ms': max(values), 'mean_ms': float(np.mean(values))}


def measure(job_path):
    job = json.loads(job_path.read_text())
    config = job['config']; w, h = config['width'], config['height']
    ow, oh = config.get('output_width', w), config.get('output_height', h)
    image = Image.open(job['source']).convert('RGB')
    # The full-size fixture is never resized. Only explicit 2x scaling recipes
    # receive a half-size input, documented in each receipt.
    if image.size != (w, h): image = image.resize((w, h), Image.Resampling.LANCZOS)
    color = np.ones((h, w, 4), np.float16)
    color[..., :3] = np.asarray(image, np.float32) / 255
    motion = np.zeros((h, w, 2), np.float16)
    output = np.empty((oh, ow, 4), np.float16)
    api = C.CDLL(job['sdk'])
    api.phoenix_native_create.argtypes = [C.c_char_p, C.c_void_p, C.c_void_p]
    api.phoenix_native_create.restype = C.c_void_p
    api.phoenix_native_error.restype = C.c_char_p
    api.phoenix_native_destroy.argtypes = [C.c_void_p]
    api.phoenix_native_status.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
    api.phoenix_native_status.restype = C.c_size_t
    api.phoenix_native_frame_host.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t, C.c_size_t,
        C.c_void_p, C.c_size_t, C.c_size_t, C.c_void_p, C.c_size_t, C.c_size_t, C.POINTER(Controls)]
    memory_api = C.CDLL(job['memory_probe'])
    memory_api.phoenix_benchmark_memory.argtypes = [C.c_uint]
    memory_api.phoenix_benchmark_memory.restype = C.c_char_p
    def memory(): return json.loads(memory_api.phoenix_benchmark_memory(0x10de))
    def check(code):
        if code: raise RuntimeError(api.phoenix_native_error().decode())
    def status():
        text = C.create_string_buffer(api.phoenix_native_status(session, None, 0))
        api.phoenix_native_status(session, text, len(text))
        return json.loads(text.value)
    report = {'passed': False, 'case': job['case'], 'model': job['model'],
        'process_index': job['process_index'], 'config': config, 'memory_before': memory()}
    start = time.perf_counter()
    session = api.phoenix_native_create(json.dumps(config).encode(), None, None)
    if not session: raise RuntimeError(api.phoenix_native_error().decode())
    report['create_ms'] = (time.perf_counter()-start)*1000
    controls = Controls(C.sizeof(Controls), 1, 1, 1, 1, 1, -1, 0, 0)
    estimate = config.get('engine_motion_valid') is False
    def call():
        before = time.perf_counter_ns()
        check(api.phoenix_native_frame_host(session, color.ctypes.data, color.nbytes, w*8,
            None if estimate else motion.ctypes.data, 0 if estimate else motion.nbytes, w*4,
            output.ctypes.data, output.nbytes, ow*8, C.byref(controls)))
        return (time.perf_counter_ns()-before)/1e6
    try:
        report['first_call_ms'] = call()
        controls.reset = 0
        start = time.perf_counter(); warm = 0
        while warm < 6 or time.perf_counter()-start < .6:
            call(); warm += 1
        walls = []; gpu = []; start = time.perf_counter()
        while (len(walls) < 24 or time.perf_counter()-start < 1.) and time.perf_counter()-start < 8:
            walls.append(call())
            s = status()
            if s['gpu_sampled_last_frame']: gpu.append(s['gpu_processing_ms'])
        report.update(warmup_frames=warm, measured_seconds=time.perf_counter()-start,
                      host_call=stats(walls), host_samples_ms=walls,
                      model_gpu=stats(gpu) if gpu else None, memory_after=memory(), runtime=status())
        if not np.isfinite(output).all(): raise RuntimeError('Nonfinite output')
        if not np.all(output[..., 3] == 1): raise RuntimeError('Alpha was not preserved')
        # Same reset input for every visual and both process repetitions.
        controls.reset = 1; call()
        report['reset_output_sha256'] = hashlib.sha256(output.tobytes()).hexdigest()
        rgb = np.uint8(np.clip(output[..., :3].astype(np.float32), 0, 1)*255+.5)
        if job['process_index'] == 0:
            Image.fromarray(rgb).save(OUT/(job['key']+'.png'))
        reference = np.asarray(Image.open(job['source']).convert('RGB'), np.float32)/255
        difference = np.abs(output[..., :3].astype(np.float32)-reference)
        report['change_from_source'] = {'mean_rgb': float(difference.mean()), 'max_rgb': float(difference.max())}
        report['passed'] = True
    finally:
        check(api.phoenix_native_destroy(session))
        report['memory_after_destroy'] = memory()
        write(Path(job['result']), report)


def render(report):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    grouped = {}
    for item in report['cases']:
        grouped.setdefault((item['case'], item['model']), []).append(item)
    rows = []
    for (case, model), items in grouped.items():
        values = [t for i in items for t in i['host_samples_ms']]
        memory = [i['memory_after'].get('device_local_bytes', 0) for i in items]
        rows.append({'case':case, 'model':model, **stats(values),
                     'device_local_mib': float(np.median(memory))/2**20,
                     'reset_outputs_equal': len({i['reset_output_sha256'] for i in items}) == 1})
    report['summary'] = rows
    fig, axes = plt.subplots(1, 2, figsize=(15, 7), layout='constrained')
    names = [f"{r['model']} / {r['case']}" for r in rows]
    colors = ['#69b7ff' if r['model']=='original' else '#f3b35b' if r['model']=='small-fp8' else '#8ed1a0' for r in rows]
    for ax, key, label in [(axes[0], 'median_ms', 'Synchronous host API median (ms)'),
                           (axes[1], 'device_local_mib', 'DXGI process local usage after warmup (MiB)')]:
        ax.barh(names, [r[key] for r in rows], color=colors); ax.invert_yaxis()
        ax.set_xlabel(label); ax.grid(axis='x', alpha=.2); ax.set_axisbelow(True)
    fig.suptitle('Phoenix pipeline · RTX 5080 · 1920×1080 output\nHost timings include upload/readback; process memory is not allocation-peak memory')
    fig.savefig(OUT/'timings-memory.png', dpi=150); plt.close(fig)
    selected = [('original','baseline'), ('small-fp8','baseline'), ('original','fusion2'), ('small-fp8','fusion2'),
                ('original','half-neural'), ('small-fp8','half-neural'), ('spatial','spatial2x'), ('spatial','nis2x'), ('spatial','tiny2x')]
    board = Image.new('RGB', (3*400, 3*530), '#161b22'); draw = ImageDraw.Draw(board)
    for i, (model, case) in enumerate(selected):
        picture = OUT/f'{model}-{case}.png'
        if not picture.exists(): continue
        crop = Image.open(picture).crop((770,415,1135,880))
        x, y = (i%3)*400+17, (i//3)*530+40
        board.paste(crop, (x,y)); draw.text((x,y-25), f'{model} / {case} · native pixels', fill='white')
    board.save(OUT/'face-comparisons.png')
    lines = ['# Native pipeline measurements', '',
        'RTX 5080, 1920×1080 output. Two fresh processes per recipe, at least six warmup calls and 24 measured calls per process. Fixed quality and full eight blocks unless the recipe explicitly changes them.', '',
        'Times include host upload, synchronization and readback; they are not GPU-resident game FPS. GPU samples, when present, cover neural processing only and omit custom shader/flow/transfer costs. DXGI memory is process usage sampled after warmup, not an allocation peak. Static SDR character fixture, zero engine vectors except optical-flow cases. No game or desktop control.', '',
        '| Model | Recipe | Host median ms | Host p95 ms | Process local MiB |', '| --- | --- | ---: | ---: | ---: |']
    for r in rows: lines.append(f"| {r['model']} | {r['case']} | {r['median_ms']:.2f} | {r['p95_ms']:.2f} | {r['device_local_mib']:.0f} |")
    lines += ['', '![Timings and memory](timings-memory.png)', '', '![Native-pixel face crops](face-comparisons.png)', '',
        'Original/Lightweight, fusion and reduced neural resolution use the original 1920×1080 input. Spatial/NIS/Tiny explicitly receive a Lanczos 960×540 input and reconstruct to 1920×1080. The small checkpoint is not a replacement for the Phoenix neural renderer. Crops preserve native pixels; these subjective examples have no ground-truth target and do not establish improved accuracy.', '',
        'In this example, fusion and half-size neural processing soften fine facial detail. The current Tiny port produces conspicuous texture/edge artifacts and is substantially slower than spatial/NIS scaling; its checkpoint and implementation remain experimental. Numerical agreement with its independent network oracle does not establish good image quality.', '',
        'Source/model/runtime/checkpoint identities, complete recipes, raw samples and reset repeatability are recorded in `measurements.json`. The character source is reused from the existing locally rendered woman fixture; its provenance is linked there.']
    previous=OUT/'before-granular-fast-path.json'
    if previous.exists():
        baseline=json.loads(previous.read_text())
        if baseline.get('source_sha256')==report['source_sha256'] and baseline.get('model_dll_sha256')==report['model_dll_sha256']:
            comparison=[]
            for model in ('original','small-fp8'):
                old=[i for i in baseline['cases'] if i['case']=='blocks4' and i['model']==model]
                new=[i for i in report['cases'] if i['case']=='blocks4' and i['model']==model]
                if len(old)!=2 or len(new)!=2:continue
                if {i['reset_output_sha256'] for i in old}!={i['reset_output_sha256'] for i in new}:
                    raise RuntimeError('Granular fast path changed reset pixels')
                before=stats([t for i in old for t in i['host_samples_ms']])['median_ms']
                after=stats([t for i in new for t in i['host_samples_ms']])['median_ms']
                comparison.append({'model':model,'before_ms':before,'after_ms':after,
                    'reduction_percent':(before-after)/before*100,'reset_pixels_exact':True})
            report['granular_fast_path']={'before_sdk_sha256':baseline['sdk_sha256'],
                'after_sdk_sha256':report['sdk_sha256'],'cases':comparison}
            lines += ['', 'The four-block recipe now stays on the established executor when initialized. Resubmitting identical tuning preserves that executor and its histories. Compared with the preceding general-chain build on the same fixture:', '']
            for item in comparison:
                lines.append(f"- {item['model']}: {item['before_ms']:.2f} → {item['after_ms']:.2f} ms ({item['reduction_percent']:.1f}% lower host time), identical reset pixels.")
            lines += ['', 'These are sequential microbenchmark runs with normal scheduling variation. Later structural live tuning changes can still rebuild through the general chain.']
    (OUT/'README.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')


def publish_docs(report):
    destination=ADDON/'docs/benchmarks/current/pipeline'
    destination.mkdir(parents=True,exist_ok=True)
    for name in ('README.md','timings-memory.png','face-comparisons.png'):
        shutil.copy2(OUT/name,destination/name)
    # The public report records identities and statistics without workstation
    # paths. Full local runtime/configuration receipts remain in artifacts.
    public={key:report[key] for key in ('schema','passed','started_utc','sdk_sha256',
        'source_sha256','model_dll_sha256','tiny_checkpoint_sha256','tiny_native_weights_sha256',
        'host_timing_scope','memory_scope','gpu','summary')}
    public['source']='Existing 1920x1080 woman character fixture, source skin diffusion disabled'
    public['raw_receipt_sha256']=sha(OUT/'measurements.json')
    if 'granular_fast_path' in report:public['granular_fast_path']=report['granular_fast_path']
    write(destination/'measurements.json',public)
    path=destination/'README.md'
    text=path.read_text(encoding='utf-8').replace(
        'Source/model/runtime/checkpoint identities, complete recipes, raw samples and reset repeatability are recorded in `measurements.json`. The character source is reused from the existing locally rendered woman fixture; its provenance is linked there.',
        'Source/model/runtime/checkpoint identities, repeatability and summary statistics are in [measurements.json](measurements.json). Complete local recipes and raw samples are kept in the player’s `artifacts/current/pipeline-measurements/measurements.json`; its hash is pinned in this public summary. The character source is reused from the existing rendered woman fixture, with source skin diffusion disabled.')
    path.write_text(text,encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--job', type=Path)
    args = parser.parse_args()
    if args.job: measure(args.job); return
    OUT.mkdir(parents=True, exist_ok=True)
    sdk = ADDON/'artifacts/current/pipeline-native/phoenix_native.dll'
    source = ADDON/'artifacts/current/four-model-comparison/woman-closeup/input.png'
    model = ROOT.parent/'research/model-compatibility-310.9.16/nvngx_dlssnr.310.9.16.dll'
    memory = ADDON/'build/four_model_comparison/readme_runtime/readme_memory.dll'
    checkpoint = ROOT.parent/'dlss5_huggingface/github/Benjamin-Wegener/dlss5-distill/weights/model_dlss5_distill_deploy.pt'
    sys.path.insert(0, str(ADDON/'tools'))
    from convert_tiny_model import convert
    import tempfile
    with tempfile.TemporaryDirectory(prefix='pipeline-benchmark-', dir=ROOT/'build') as temporary:
        work = Path(temporary); tiny = work/'tiny.bin'; tiny.write_bytes(convert(checkpoint)[0])
        report = {'passed': False, 'schema':'phoenix-pipeline-measurements/1', 'cases':[],
            'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'sdk_sha256':sha(sdk), 'source':str(source), 'source_sha256':sha(source),
            'source_provenance':str(source.parent/'source-render.json'),
            'model_dll_sha256':sha(model), 'memory_probe_sha256':sha(memory),
            'tiny_checkpoint_sha256':sha(checkpoint), 'tiny_native_weights_sha256':sha(tiny),
            'host_timing_scope':'complete synchronous native host call including upload/readback; no game FPS',
            'memory_scope':'DXGI current process local usage, after warmup; not peak',
            'gpu':subprocess.run(['nvidia-smi','--query-gpu=name,driver_version,memory.total','--format=csv,noheader'], capture_output=True,text=True,check=True).stdout.strip()}
        jobs = []
        recipes = [('baseline', {}, {}), ('monitor-off', {'PipelineMonitoring':0}, {}),
            ('color', {'ProcessingChain':True,'ChainBrightness':.02}, {}),
            ('fusion2', {'ProcessingChain':True,'ChainFusion':True,'MultiscaleFusion':True,'FusionLayers':2}, {}),
            ('fusion3', {'ProcessingChain':True,'ChainFusion':True,'MultiscaleFusion':True,'FusionLayers':3}, {}),
            ('passes2', {}, {'network_passes':2}), ('blocks4', {}, {'central_blocks':4}),
            ('half-neural', {}, {'neural_scale':50}),
            ('flow180', {'MotionEstimation':True,'MotionProvider':2,'FlowAnalysisHeight':180}, {}),
            ('flow360', {'MotionEstimation':True,'MotionProvider':2,'FlowAnalysisHeight':360}, {})]
        for model_id in ('original','small-fp8'):
            for name, pipeline, tuning in recipes:
                config = {'width':1920,'height':1080,'bundle_path':str(ADDON/'artifacts/current/native-kernels'),
                    'weight_source':{'kind':'nvidia_dll','path':str(model),'sha256':sha(model),'model':model_id},
                    'pipeline':{'PipelineMonitoring':2,'MonitoringSampleInterval':1,**pipeline}}
                if tuning: config['tuning'] = tuning
                if name.startswith('flow'): config['engine_motion_valid'] = False
                jobs.append((model_id,name,config))
        for name, scaler in [('spatial2x',0),('nis2x',1),('tiny2x',3)]:
            jobs.append(('spatial',name,{'width':960,'height':540,'output_width':1920,'output_height':1080,
                'spatial_only':True,'tiny_model_path':str(tiny),
                'pipeline':{'ProcessingChain':True,'ChainNeural':False,'AdditionalScaler':scaler,'PipelineMonitoring':2,'MonitoringSampleInterval':1}}))
        try:
            for repetition in range(2):
                for model_id,name,config in (jobs if repetition==0 else list(reversed(jobs))):
                    key = model_id+'-'+name
                    job_path = work/'job.json'; result = work/'result.json'
                    write(job_path, {'case':name,'model':model_id,'key':key,'config':config,
                        'process_index':repetition,'sdk':str(sdk),'memory_probe':str(memory),
                        'source':str(source),'result':str(result)})
                    subprocess.run([sys.executable,__file__,'--job',str(job_path)],check=True,timeout=180)
                    item = json.loads(result.read_text())
                    if not item['passed']: raise RuntimeError(key+' did not pass')
                    report['cases'].append(item); write(OUT/'measurements.json',report)
                    print(f"{repetition+1}/2 {key}: {item['host_call']['median_ms']:.2f} ms",flush=True)
            if sha(sdk)!=report['sdk_sha256']: raise RuntimeError('SDK changed during benchmark')
            render(report); report['passed'] = True
        except Exception as error:
            report['error'] = str(error); raise
        finally: write(OUT/'measurements.json',report)
        if report['passed']:publish_docs(report)


if __name__=='__main__': main()
