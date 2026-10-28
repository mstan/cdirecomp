"""Compare the production drawing job with actual generated Hotel Mario code.

Provide a supported built consumer, BIOS, and the two RAM banks retained by
loader_job_compare.py at its deterministic gameplay endpoint. The fixture uses
isolated RAM copies and never changes a running game or persistent save.
"""
import argparse,hashlib,json,os,pathlib,shutil,subprocess
from build_provenance import verify

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','rom','ram0','ram1'):p.add_argument(name,type=pathlib.Path)
    p.add_argument('--output',type=pathlib.Path,required=True)
    a=p.parse_args();repo=pathlib.Path(__file__).resolve().parents[1]
    a.output.mkdir(parents=True,exist_ok=False)
    windows=os.name=='nt';compiler=pathlib.Path('C:/msys64/mingw64/bin/gcc.exe') if windows else pathlib.Path('cc')
    objcopy=compiler.with_name('objcopy.exe') if windows else pathlib.Path('objcopy')
    suffix='.obj' if windows else '.o'
    object_root=a.build/'cdirecomp-runtime/CMakeFiles/CdiRuntime.dir'
    if not object_root.is_dir():object_root=a.build/'CMakeFiles/CdiRuntime.dir'
    objects=sorted(object_root.rglob('*'+suffix));main_obj=next(x for x in objects if x.name=='main.c'+suffix)
    objects.remove(main_obj);renamed=a.output/('product-main'+suffix)
    subprocess.run([str(objcopy),'--redefine-sym','main=cdi_product_main',str(main_obj),str(renamed)],check=True)
    exe=a.output/('DrawingRuntimeTest.exe' if windows else 'DrawingRuntimeTest')
    runtime=a.build/('HotelMarioRecomp.exe' if windows else 'HotelMarioRecomp')
    provenance=verify(runtime)
    args=['-std=c11','-O2','-I'+str(repo/'runner/include'),'-I'+str(repo/'common'),
        str(repo/'runner/tests/cdi_drawing_runtime_test.c'),*[str(x) for x in objects],str(renamed)]
    args+=['-LC:/msys64/mingw64/lib','-lSDL2','-lws2_32'] if windows else ['-lSDL2','-pthread']
    args+=['-o',str(exe)];rsp=a.output/'link.rsp'
    rsp.write_text('\n'.join('"'+x.replace('\\','/')+'"' for x in args))
    subprocess.run([str(compiler),'@'+str(rsp)],check=True)
    if windows:shutil.copy2(a.build/'SDL2.dll',a.output/'SDL2.dll')
    cases=[];frontiers={}
    def run_case(budget,queued,variant=0,irq=0,seed=0x91234,wrap=0):
        number=len(cases);states={}
        for mode in ('reference','native'):
            output=a.output/f'{number:03d}-{mode}.json'
            cmd=[str(exe),str(a.rom.resolve()),str(a.ram0.resolve()),str(a.ram1.resolve()),str(output),
                 mode,str(budget),str(queued),str(variant),str(irq),str(seed),str(wrap)]
            result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
            (a.output/f'{number:03d}-{mode}.stderr').write_bytes(result.stderr)
            if result.returncode:raise RuntimeError((cmd,result.returncode,result.stderr.decode(errors='replace')[-2000:]))
            states[mode]=json.loads(output.read_text())
        reference=states['reference'];native=states['native']
        points=reference.pop('reference_frontiers');native.pop('reference_frontiers')
        jobs=native.pop('jobs');reference.pop('jobs')
        differences=[key for key in reference if reference[key]!=native[key]]
        row={'case':number,'budget':budget,'queued':queued,'variant':variant,'irq_cycle':irq,
             'seed':seed,'wrap':wrap,'native_jobs':jobs,'differences':differences}
        cases.append(row);(a.output/'results.json').write_bytes((json.dumps(cases,indent=2)+'\n').encode())
        if differences:raise RuntimeError(row)
        assert jobs['jobs']==0 if budget==1025 else jobs['jobs']>0,row
        if not irq:
            if variant==1 and budget>0 and queued:assert native['painted'],row
            for point in points:frontiers.setdefault((variant,point['pc']),point['after_cycles'])
    for n in (0,1,2,8,16,-1,1025):run_case(n,0)
    for count in (1,3,15):
        for wrap in (0,1):run_case(8,count,wrap=wrap)
    for variant in (1,2,3,4,5):run_case(8,1,variant=variant,wrap=1)
    timed=sorted({cycle for (variant,pc),cycle in frontiers.items() if variant in (0,1) and cycle>0})
    # IRQ targets come from actual original operations, including callbacks and
    # wrapping. The native implementation never defines its own passing oracle.
    for cycle in timed:run_case(8,1,variant=1,irq=cycle,seed=1,wrap=1)
    summary={'status':'pass','cases':cases,'timed_reference_cycles':timed,'provenance':provenance,
        'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
        'production_binary_sha256':hashlib.sha256(runtime.read_bytes()).hexdigest(),
        'fixtures':{name:{'path':str(path.resolve()),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
                    for name,path in [('rom',a.rom),('ram0',a.ram0),('ram1',a.ram1)]},
        'notes':'Actual generated reference and production native objects; full RAM/register/physical-clock/IRQ/fault/redirect equality; real bitmap callback must paint.'}
    (a.output/'summary.json').write_bytes((json.dumps(summary,indent=2)+'\n').encode())
    print(f'Drawing job: {len(cases)} production/reference cases PASS')
if __name__=='__main__':main()
