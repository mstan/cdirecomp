"""Qualify the selected process lookup using the existing production linker."""
import argparse,hashlib,json,pathlib,subprocess
from drawing_runtime_fixture import build_fixture

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','rom','ram0','ram1'):p.add_argument(name,type=pathlib.Path)
    p.add_argument('--output',type=pathlib.Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    source=pathlib.Path(__file__).resolve().parents[1]/'runner/tests/cdi_service_runtime_test.c'
    exe,runtime,provenance=build_fixture(a.build,a.output,source)
    cases=[];frontiers=set()
    def run(pid,count,variant=0,irq=0,seed=31):
        number=len(cases);states={}
        for mode in ('reference','native'):
            output=a.output/f'{number:03d}-{mode}.json'
            result=subprocess.run([str(exe),str(a.rom.resolve()),str(a.ram0.resolve()),str(a.ram1.resolve()),str(output),mode,
                str(pid),str(count),str(variant),str(irq),str(seed)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=20)
            (a.output/f'{number:03d}-{mode}.stderr').write_bytes(result.stderr)
            if result.returncode:raise RuntimeError((number,mode,result.returncode,result.stderr.decode(errors='replace')[-1500:]))
            states[mode]=json.loads(output.read_text())
        left,right=states['reference'],states['native'];points=left.pop('reference_frontiers');right.pop('reference_frontiers')
        left.pop('jobs');jobs=right.pop('jobs')
        differences=[key for key in left if left[key]!=right[key]]
        row=dict(case=number,pid=pid,count=count,variant=variant,irq_cycle=irq,seed=seed,jobs=jobs,differences=differences)
        cases.append(row);(a.output/'results.json').write_text(json.dumps(cases,indent=2)+'\n')
        if differences:raise RuntimeError(row)
        assert jobs['jobs']==(0 if variant in (7,8,9) else 1),row
        if not irq and variant==0 and pid==5 and count==16:
            frontiers.update(point[2] for point in points if point[2]>0)
    for pid,count in ((5,16),(0,16),(17,16),(1,1),(0xffff,0xffff),(0x4000,0xffff),
                      (0x8000,0xffff),(0x2001,0xffff),(0xc005,0xffff),(0x12340005,16)):
        run(pid,count)
    for variant in range(2,13):run(5,16,variant)
    for seed in (0,1,4,8,16):run(5,16,seed=seed)
    for cycles in sorted(frontiers):run(5,16,irq=cycles)
    summary=dict(status='pass',cases=cases,provenance=provenance,
        production_binary_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),
        fixture_binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(f'Process service: {len(cases)} actual generated-reference cases PASS')
if __name__=='__main__':main()
