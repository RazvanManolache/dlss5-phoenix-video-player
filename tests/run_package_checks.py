"""Check shipped relative paths and execution against the validated SDK build."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
ADDON=ROOT.parent/'dlss5-phoenix-addon'


def half(value):return struct.unpack('<e',struct.pack('<e',value))[0]


def stored_half(value):
    # Typed RGBA16F UAV stores truncate toward zero on the checked D3D12 path.
    # Keep the independent blend oracle in that storage domain.
    bits=struct.unpack('<H',struct.pack('<e',value))[0]
    if abs(half(value))>abs(value) and bits&0x7fff:bits-=1
    return struct.unpack('<e',struct.pack('<H',bits))[0]


def fusion_reference(executable,kernels,settings,pixels,width,height,work):
    """Independent model runs, checked downsampling and a CPU blend oracle."""
    full_pixels=list(struct.iter_unpack('<4e',pixels));small_width,small_height=width//2,height//2
    small=[]
    for y in range(small_height):
        for x in range(small_width):
            small.append(tuple(half(sum(full_pixels[(y*2+dy)*width+x*2+dx][c] for dy in (0,1) for dx in (0,1))*.25) for c in range(4)))
    # A half-input rounding difference can be amplified by the neural model.
    # Check the scaler against the CPU bound, then feed its actual FP16 pixels
    # to a separate model session. This isolates fusion from scaler roundoff.
    down=work/('reference-'+settings['weight_source']['model']+'-down.json')
    down.write_text(json.dumps({'width':width,'height':height,'output_width':small_width,'output_height':small_height,
        'spatial_only':True,'pipeline':{'ProcessingChain':True,'ChainNeural':False,'PipelineMonitoring':0}}))
    down_source=down.with_suffix('.input');down_source.write_bytes(pixels);down_output=down.with_suffix('.output')
    done=subprocess.run([str(executable),'--config',str(down),'--input',str(down_source),'--output',str(down_output)],capture_output=True,text=True,timeout=60)
    if done.returncode:raise RuntimeError('Independent scaler failed: '+done.stderr[-1000:])
    small_pixels=down_output.read_bytes()
    down_error=max(abs(a-b) for actual,reference in zip(struct.iter_unpack('<4e',small_pixels),small) for a,b in zip(actual,reference))
    if down_error>.001:raise RuntimeError('Independent scaler exceeds CPU FP16 bound')
    predictions=[]
    for name,w,h,data in [('full',width,height,pixels),('half',small_width,small_height,small_pixels)]:
        options=json.loads(json.dumps(settings));options.update(width=w,height=h,bundle_path=str(kernels))
        options['pipeline'].update(ChainFusion=False,MultiscaleFusion=False,ChainColor=False)
        config=work/('reference-'+settings['weight_source']['model']+'-'+name+'.json');config.write_text(json.dumps(options))
        source=config.with_suffix('.input');source.write_bytes(data);target=config.with_suffix('.output')
        done=subprocess.run([str(executable),'--config',str(config),'--input',str(source),'--output',str(target)],capture_output=True,text=True,timeout=150)
        if done.returncode:raise RuntimeError('Independent fusion prediction failed: '+done.stderr[-1000:])
        predictions.append(list(struct.iter_unpack('<4e',target.read_bytes())))
    full,small=predictions;expected=[]
    def read(x,y,c):return small[max(0,min(y,small_height-1))*small_width+max(0,min(x,small_width-1))][c]
    for y in range(height):
        py=(y+.5)*.5-.5;iy=math.floor(py);fy=py-iy
        for x in range(width):
            px=(x+.5)*.5-.5;ix=math.floor(px);fx=px-ix;pixel=[]
            for c in range(3):
                top=read(ix,y=iy,c=c)*(1-fx)+read(ix+1,y=iy,c=c)*fx
                bottom=read(ix,y=iy+1,c=c)*(1-fx)+read(ix+1,y=iy+1,c=c)*fx
                restored=stored_half(top*(1-fy)+bottom*fy)
                blended=stored_half((full[y*width+x][c]+restored)*.5)
                pixel.append(stored_half(max(0,min(1,blended+struct.unpack('<f',struct.pack('<f',.04))[0]))))
            expected.append((*pixel,1.))
    return expected,down_error


def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def granular_reference(executable,kernels,settings,pixels,width,height,work):
    """The established direct block/repeat API is an independent route oracle."""
    results=[]
    repeats=[1,0,2,1,1,1,1,1]
    for route in ('legacy','tuning'):
        config={'width':width,'height':height,'bundle_path':str(kernels),
            'weight_source':settings['weight_source'],'pipeline':{'PipelineMonitoring':0}}
        if route=='legacy':config.update(transformer_blocks=4,repeats=repeats)
        else:config['tuning']={'central_blocks':4,'repeats':repeats}
        path=work/(settings['weight_source']['model']+'-granular-'+route+'.json')
        path.write_text(json.dumps(config));source=path.with_suffix('.input');source.write_bytes(pixels)
        target=path.with_suffix('.output')
        done=subprocess.run([str(executable),'--config',str(path),'--input',str(source),'--output',str(target)],capture_output=True,text=True,timeout=150)
        if done.returncode:raise RuntimeError('Granular execution failed: '+done.stderr[-1000:])
        status=json.loads(done.stderr.strip())['runtime']
        if status['chain_execution']!='original executor (equivalent stages folded)':raise RuntimeError('Block controls introduced general-chain work')
        if status['completed_tuning']['central_blocks']!=4 or status['completed_tuning']['repeats']!=repeats:raise RuntimeError('Granular controls were ignored')
        results.append(target.read_bytes())
    if results[0]!=results[1]:raise RuntimeError('Folded tuning differs from established granular execution')
    return {'legacy_pixels_exact':True,'blocks':4,'repeats':repeats,'extra_chain_work':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gpu-models',action='store_true')
    parser.add_argument('--package',type=Path,default=ROOT.parent/'dlss5-everything/runtime/media')
    parser.add_argument('--model-dll',type=Path,default=ROOT.parent/'research/model-compatibility-310.9.16/nvngx_dlssnr.310.9.16.dll')
    args=parser.parse_args();package=args.package.resolve()
    report={'passed':False,'cases':[],'models_included':False}
    try:
        manifest=json.loads((package/'package-manifest.json').read_text())
        for name,digest in manifest['files'].items():
            path=(package/name).resolve()
            if not path.is_relative_to(package) or sha(path)!=digest:raise RuntimeError('Packaged file differs: '+name)
        runtime=json.loads((package/'phoenix-runtime.json').read_text())
        if Path(runtime['native_library']).is_absolute() or Path(runtime['bundle_path']).is_absolute():raise RuntimeError('Package requires a developer path')
        sdk=ADDON/'artifacts/current/pipeline-native';bundle=ADDON/'artifacts/current/native-kernels'
        cases=[('spatial',31,19)]
        if args.gpu_models:cases.extend((model,160,96) for model in ('original','small-fp8'))
        with tempfile.TemporaryDirectory(prefix='package-check-',dir=ROOT/'build') as folder:
            work=Path(folder)
            for model,width,height in cases:
                pixels=b''.join(struct.pack('<4e',((x*17+y*7)%256)/255,((x*3+y*29)%256)/255,((x*11+y*13)%256)/255,1.)
                    for y in range(height) for x in range(width))
                source=work/(model+'.rgba16f');source.write_bytes(pixels)
                settings={'width':width,'height':height,'pipeline':{'PipelineMonitoring':0}}
                if model=='spatial':settings.update(spatial_only=True,warp=True)
                else:
                    settings['weight_source']={'kind':'nvidia_dll','path':str(args.model_dll.resolve()),'sha256':sha(args.model_dll),'model':model}
                    settings['pipeline'].update(ProcessingChain=True,ChainStage1=1,ChainStage2=0,ChainStage3=3,ChainStage4=2,
                        ChainColor=True,ChainBrightness=.04,ChainFusion=True,MultiscaleFusion=True,FusionLayers=2)
                    settings['tuning']={'intensity':.8,'network_passes':1}
                outputs=[];status=None
                for label,executable,kernels in [('baseline',sdk/'phoenix-process.exe',bundle),('packaged',package/'phoenix-process.exe',package/runtime['bundle_path'])]:
                    config=work/(model+'-'+label+'.json');config.write_text(json.dumps({**settings,'bundle_path':str(kernels)}))
                    target=work/(model+'-'+label+'.rgba16f')
                    done=subprocess.run([str(executable),'--config',str(config),'--input',str(source),'--output',str(target)],
                        cwd=package,capture_output=True,text=True,timeout=150)
                    if done.returncode:raise RuntimeError(label+': '+done.stderr[-1500:])
                    status=json.loads(done.stderr.strip())['runtime'];outputs.append(target.read_bytes())
                if outputs[0]!=outputs[1] or len(outputs[1])!=len(pixels):raise RuntimeError('Packaged pixels differ from validated SDK')
                if not all(math.isfinite(p[0]) for p in struct.iter_unpack('<e',outputs[1])):raise RuntimeError('Nonfinite package output')
                if model=='spatial' and outputs[1]!=pixels:raise RuntimeError('Spatial package changed source pixels')
                if model!='spatial' and (outputs[1]==pixels or status['model_id']!=model or status['independent_temporal_stages']<2):raise RuntimeError('Packaged neural chain was bypassed')
                oracle=None
                if model!='spatial':
                    expected,scaler_error=fusion_reference(sdk/'phoenix-process.exe',bundle,settings,pixels,width,height,work)
                    errors=[abs(a-b) for actual,reference in zip(struct.iter_unpack('<4e',outputs[1]),expected) for a,b in zip(actual[:3],reference[:3])]
                    oracle={'max_error':max(errors),'mean_error':sum(errors)/len(errors),'scaler_cpu_max_error':scaler_error}
                    if oracle['max_error']>.003 or oracle['mean_error']>.0005:raise RuntimeError('Fusion does not average same-source predictions: '+str(oracle))
                report['cases'].append({'model':model,'backend':status['backend'],'extent':[width,height],
                    'packed_weights_sha256':status['packed_weights_sha256'],'packaged_pixels_match_sdk':True,
                    'chain_and_fusion':model!='spatial','independent_fusion_oracle':oracle,
                    'granular_direct_oracle':granular_reference(package/'phoenix-process.exe',package/runtime['bundle_path'],settings,pixels,width,height,work) if model!='spatial' else None,
                    'output_sha256':hashlib.sha256(outputs[1]).hexdigest()})
                print(model+' packaged execution passed.',flush=True)
            if args.gpu_models:
                portable=runtime['portable_runtime'];provider=package/portable['root']
                if Path(portable['root']).is_absolute():raise RuntimeError('Portable package requires a developer path')
                inventory=json.loads(subprocess.check_output([str(provider/'phoenix_cpp_devices.exe'),str(provider)],text=True))['devices']
                devices=[item['id'] for item in inventory if item['id']=='cpu' or item['id'].startswith(('cuda:','vulkan:')) and item.get('capable',True)]
                for device in devices:
                    for model in ('original','small-fp8'):
                        name='portable-'+str(len(report['cases']));width,height=32,24
                        pixels=b''.join(struct.pack('<4e',.1+(x%9)*.07,.2+(y%7)*.08,.15+((x+y)%5)*.1,.5) for y in range(height) for x in range(width))
                        config={'width':width,'height':height,'bundle_path':str(package/runtime['bundle_path']),
                            'weight_source':{'kind':'nvidia_dll','path':str(args.model_dll.resolve()),'sha256':sha(args.model_dll),'model':model},
                            'portable_runtime':{'runtime_root':str(provider),'runtime_sha256':portable['files']['phoenix_runtime_cpp.dll'],
                                'files':portable['files'],'backend':'cpu' if device=='cpu' else device.split(':')[0],
                                'device':device,'allow_unvalidated':True,'cpu_threads':2},
                            'pipeline':{'PipelineMonitoring':0},'tuning':{'central_blocks':1 if device=='cpu' else 8}}
                        path=work/(name+'.json');path.write_text(json.dumps(config));source=work/(name+'.input');source.write_bytes(pixels)
                        outputs=[]
                        for label,executable in [('baseline',sdk/'phoenix-process.exe'),('packaged',package/'phoenix-process.exe')]:
                            target=work/(name+'-'+label+'.output')
                            done=subprocess.run([str(executable),'--config',str(path),'--input',str(source),'--output',str(target)],cwd=package,capture_output=True,timeout=150)
                            if done.returncode:raise RuntimeError('Portable package execution failed: '+done.stderr.decode(errors='replace')[-1500:])
                            outputs.append(target.read_bytes())
                        if outputs[0]!=outputs[1] or outputs[1]==pixels:raise RuntimeError('Packaged portable pixels differ or bypassed inference')
                        headers=[struct.pack('<4sHHIIQQqQQI',b'PXF1',1,60,width,height,7,i,i*16666667,len(pixels),0,int(i==0)) for i in range(2)]
                        streamed=subprocess.run([str(package/'phoenix-process.exe'),'--config',str(path),'--stream'],
                            input=b''.join(header+pixels for header in headers),cwd=package,capture_output=True,timeout=150)
                        if streamed.returncode or streamed.stdout!=b''.join(header+outputs[1] for header in headers):
                            raise RuntimeError('Packaged portable stream changed pixels or source identity: '+streamed.stderr.decode(errors='replace')[-1500:])
                        report['cases'].append({'model':model,'device':device,'backend':'portable_pipeline_d3d12','extent':[width,height],
                            'packaged_pixels_match_sdk':True,'stream_pixels_and_timestamps_exact':True})
                        print(model+' '+device+' packaged portable file/stream passed.',flush=True)
        report['passed']=True;report['package_manifest_sha256']=sha(package/'package-manifest.json')
    except Exception as error:
        report['error']=str(error);raise
    finally:(ROOT/'artifacts/current/package-runtime-validation.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
