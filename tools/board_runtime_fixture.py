"""Compare the production STOP-board job with actual individual board ticks."""
import argparse,hashlib,json,pathlib,subprocess
from drawing_runtime_fixture import build_fixture

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','rom','disc'):p.add_argument(name,type=pathlib.Path)
    p.add_argument('--output',type=pathlib.Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    source=pathlib.Path(__file__).resolve().parents[1]/'runner/tests/cdi_board_runtime_test.c'
    wraps=('cdi_irq_raise','cdi_irq_raise_vector','cdi_irq_raise_onchip_level','cdi_irq_clear','runtime_pace_cycles')
    exe,runtime,provenance=build_fixture(a.build,a.output,source,wraps)
    cases=[]
    for variant in range(4):
        states={}
        for mode in ('reference','native'):
            output=a.output/f'{variant:02d}-{mode}.json'
            result=subprocess.run([str(exe),str(a.rom.resolve()),str(a.disc.resolve()),str(output),mode,str(variant)],
                stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
            (a.output/f'{variant:02d}-{mode}.stderr').write_bytes(result.stderr)
            if result.returncode:raise RuntimeError((variant,mode,result.returncode,result.stderr.decode(errors='replace')[-1500:]))
            states[mode]=json.loads(output.read_text())
        left,right=states['reference'],states['native'];left.pop('jobs');jobs=right.pop('jobs')
        differences=[key for key in left if left[key]!=right[key]]
        row=dict(variant=variant,jobs=jobs,differences=differences,reference=left);cases.append(row)
        (a.output/'results.json').write_text(json.dumps(cases,indent=2)+'\n')
        if differences:raise RuntimeError(row)
        assert jobs['jobs']>0 and jobs['quiet_quanta']>0,row
        if variant in (1,2):assert jobs['observation_exits']>0,row
    summary=dict(status='pass',cases=cases,provenance=provenance,
        production_binary_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),
        fixture_binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('Board job: 4 real-disc/IRQ/pacing/input/observer/reset/quit timelines PASS')
if __name__=='__main__':main()
