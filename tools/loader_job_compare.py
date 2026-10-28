"""Matched real module loading, ending at its verified native execution entry."""
import argparse,copy,hashlib,json,os,pathlib,socket,statistics,subprocess,time
import psutil
from bios_options_smoke import request
from build_provenance import verify
from hotelmario_controller_probe import memory
from scanout_hle_compare import SCRIPT,identity
from hotelmario_launch_probe import disc_bin_path

def run(exe,args,label):
    out=args.output/label;out.mkdir()
    env=os.environ.copy();env.update(CDI_PACE_SPEED='32',SDL_AUDIODRIVER='dummy',
        CDI_PLAYER_CONFIG_PATH=str(args.output/'player.cfg'),TEMP=str(args.output),TMP=str(args.output))
    command=[str(exe.resolve()),str(args.rom.resolve()),'--disc',str(args.disc.resolve()),
        '--headless','--fault-hold','--port',str(args.port),'--input-script',SCRIPT]
    if args.end_frame:command+=['--stop-frame',str(args.end_frame)]
    samples=[];started=time.monotonic();host_before=psutil.cpu_times();armed=False
    with (out/'stdout.log').open('w') as stdout,(out/'stderr.log').open('w') as stderr:
        proc=subprocess.Popen(command,env=env,stdout=stdout,stderr=stderr,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            process=psutil.Process(proc.pid)
            while time.monotonic()-started<120:
                if proc.poll() is not None:raise RuntimeError(f'{label}: runtime exited')
                try:s=request(args.port,{'cmd':'status'},timeout=1)
                except OSError:time.sleep(.02);continue
                if not args.end_frame and not armed and s['frame']>=args.arm_frame:
                    arm=request(args.port,{'cmd':'stop_pc','pc':args.stop_pc});assert arm['ok'];armed=True
                cpu=process.cpu_times();samples.append({'wall_s':time.monotonic()-started,
                    'cpu_s':cpu.user+cpu.system,'rss_bytes':process.memory_info().rss,'status':s})
                if s['miss_count'] or s['main_resets']:raise RuntimeError(s)
                if s['held']:break
                time.sleep(.01)
            assert s['held'] and (s['frame']==args.end_frame if args.end_frame else s['pc']==args.stop_pc),s
            host_after=psutil.cpu_times()
            state={k:request(args.port,{'cmd':k}) for k in ('status','get_registers','ciap_state',
                'video_frame','video_state','audio_state','native_state','native_events','ikat_events')}
            state['loader_jobs']=request(args.port,{'cmd':'loader_jobs'})
            if args.expect_crc_error:
                assert state['get_registers']['d1']&0xFFFF==0xE8 and state['get_registers']['sr']&1,state['get_registers']
            frames=[];cursor=1
            while cursor<=s['frame']:
                page=request(args.port,{'cmd':'frame_hashes','from':cursor,'count':256})['records']
                assert page and page[0]['frame']==cursor
                frames+=page;cursor=page[-1]['frame']+1
            state['frames']=frames;hashes=[]
            for base in (0,0x200000):
                data=memory(args.port,base,0x80000);(out/f'ram-{base:06x}.bin').write_bytes(data)
                hashes.append(hashlib.sha256(data).hexdigest())
            state['ram_sha256']=hashes
            (out/'state.json').write_text(json.dumps(state,indent=2))
            (out/'samples.json').write_text(json.dumps(samples,indent=2))
            upper=next(i for i,v in enumerate(samples) if v['status']['frame']>=args.start_frame)
            assert upper
            lo,hi=samples[upper-1:upper+1]
            f=(args.start_frame-lo['status']['frame'])/(hi['status']['frame']-lo['status']['frame'])
            cpu_start=lo['cpu_s']+f*(hi['cpu_s']-lo['cpu_s'])
            result={'label':label,'binary':identity(exe),'command':command,'start_frame':args.start_frame,
                'end_frame':s['frame'],'cpu_ms':1000*(samples[-1]['cpu_s']-cpu_start),
                'peak_rss_bytes':max(v['rss_bytes'] for v in samples),'start_bracket':[lo,hi],
                'host_cpu_before':host_before._asdict(),'host_cpu_after':host_after._asdict(),
                'loader_jobs':state['loader_jobs']}
            (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
            return result,state
        finally:
            if proc.poll() is None:proc.terminate()
            proc.wait(timeout=10)

def semantic(state):
    d=copy.deepcopy(state);d.pop('loader_jobs',None)
    for key in ('insns','blocks','interp','irq_first_seq'):d['status'].pop(key,None)
    for key in ('scanout_jobs','fused_jobs','reused_jobs'):d['video_state'].pop(key,None)
    d['native_state'].pop('dispatches',None)
    for name in ('native_events','ikat_events'):
        for event in d[name].get('events',[]):event.pop('trace_seq',None)
    return d

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('reference','candidate','rom','disc'):p.add_argument(n,type=pathlib.Path)
    p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--pairs',type=int,default=1)
    p.add_argument('--port',type=int,default=4471);p.add_argument('--start-frame',type=int,default=1000)
    p.add_argument('--stop-pc',type=lambda x:int(x,0),default=0x25F312)
    p.add_argument('--arm-frame',type=int,default=0)
    p.add_argument('--end-frame',type=int)
    p.add_argument('--expect-crc-error',action='store_true')
    args=p.parse_args()
    with socket.socket() as probe:assert probe.connect_ex(('127.0.0.1',args.port))!=0
    args.output.mkdir(parents=True,exist_ok=False)
    (args.output/'player.cfg').write_text('[input]\ncapture_mouse = false\n[rtc]\nsync_host_on_startup = false\n')
    meta={'schema':1,'reference':verify(args.reference),'candidate':verify(args.candidate),
        'rom':identity(args.rom),'disc':identity(disc_bin_path(args.disc)),'script':SCRIPT,
        'end_contract':{'pc':args.stop_pc,'frame':args.end_frame,'expect_crc_error':args.expect_crc_error},
        'source':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        'source_diff':subprocess.check_output(['git','diff'],text=True),
        'core_diff':subprocess.check_output(['git','-C','external/m68k-recomp-core','diff'],text=True)}
    (args.output/'identity.json').write_text(json.dumps(meta,indent=2))
    results=[];contracts=[]
    for pair in range(args.pairs+1):
        states={};prefix='warm' if pair==0 else f'pair{pair}'
        order=('reference','candidate') if pair%2 else ('candidate','reference')
        for name in order:
            result,state=run(getattr(args,name),args,f'{prefix}-{name}');states[name]=state
            if name=='candidate':assert state['loader_jobs'].get('words',0)>0,'Native job was not exercised'
            if pair:results.append(result)
        left,right=semantic(states['reference']),semantic(states['candidate'])
        differences=[key for key in left if left[key]!=right[key]]
        contract={'pair':prefix,'equal':not differences,'differences':differences};contracts.append(contract)
        (args.output/'contracts.json').write_text(json.dumps(contracts,indent=2))
        if differences:raise RuntimeError(f'Loader job contract mismatch: {differences}')
    savings=[]
    for pair in range(1,args.pairs+1):
        a=next(r for r in results if r['label']==f'pair{pair}-reference')
        b=next(r for r in results if r['label']==f'pair{pair}-candidate')
        savings.append(100*(1-b['cpu_ms']/a['cpu_ms']))
    summary={'schema':1,'status':'collected; assess samples and variation','scope':'Hotel Mario USA real main-module load to first native entry',
        'results':results,'contracts':contracts,'paired_cpu_savings_percent':savings,
        'median_cpu_savings_percent':statistics.median(savings)}
    (args.output/'campaign-evidence.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
