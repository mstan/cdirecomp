"""Matched real module loading, ending at its verified native execution entry."""
import argparse,copy,hashlib,json,os,pathlib,platform,shutil,socket,statistics,subprocess,time
import psutil
from bios_options_smoke import request
from build_provenance import verify
from hotelmario_controller_probe import memory
from scanout_hle_compare import SCRIPT,identity
from hotelmario_launch_probe import disc_bin_path

class CycleCounter:
    """Raw Windows scheduled process cycles; never convert to elapsed time.

    GetProcessTimes is too coarsely quantized for this short loading interval.
    Keep both measurements and use matched process affinity for cycle results.
    Other hosts retain CPU-time measurements without a Windows dependency.
    """
    def __init__(self,pid):
        self.handle=None
        if os.name=='nt':
            import ctypes
            from ctypes import wintypes
            self.ctypes=ctypes;self.api=ctypes.WinDLL('kernel32',use_last_error=True)
            self.api.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
            self.api.OpenProcess.restype=wintypes.HANDLE
            self.api.QueryProcessCycleTime.argtypes=[wintypes.HANDLE,ctypes.POINTER(ctypes.c_ulonglong)]
            self.api.QueryProcessCycleTime.restype=wintypes.BOOL
            self.api.CloseHandle.argtypes=[wintypes.HANDLE]
            self.api.CloseHandle.restype=wintypes.BOOL
            self.handle=self.api.OpenProcess(0x1000,False,pid)
            if not self.handle:raise ctypes.WinError(ctypes.get_last_error())
    def read(self):
        if self.handle is None:return None
        value=self.ctypes.c_ulonglong()
        if not self.api.QueryProcessCycleTime(self.handle,self.ctypes.byref(value)):
            raise self.ctypes.WinError(self.ctypes.get_last_error())
        return value.value
    def close(self):
        if self.handle is not None:self.api.CloseHandle(self.handle);self.handle=None

def run(exe,args,label):
    out=args.output/label;out.mkdir()
    env=os.environ.copy();env.update(CDI_PACE_SPEED='32',SDL_AUDIODRIVER='dummy',
        CDI_PLAYER_CONFIG_PATH=str(args.output/'player.cfg'),TEMP=str(args.output),TMP=str(args.output))
    if args.presentation=='dummy':env.update(SDL_VIDEODRIVER='dummy',SDL_RENDER_DRIVER='software')
    command=[str(exe.resolve()),str(args.rom.resolve()),'--disc',str(args.disc.resolve()),
        '--fault-hold','--port',str(args.port),'--input-script',SCRIPT]
    if args.presentation=='headless':command.append('--headless')
    if args.end_frame:command+=['--stop-frame',str(args.end_frame)]
    samples=[];started=time.monotonic();host_before=psutil.cpu_times();armed=False
    with (out/'stdout.log').open('w') as stdout,(out/'stderr.log').open('w') as stderr:
        proc=subprocess.Popen(command,env=env,stdout=stdout,stderr=stderr,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        cycles=None
        try:
            process=psutil.Process(proc.pid)
            cycles=CycleCounter(proc.pid)
            if args.cpu_core is not None:process.cpu_affinity([args.cpu_core])
            while time.monotonic()-started<120:
                if proc.poll() is not None:raise RuntimeError(f'{label}: runtime exited')
                try:s=request(args.port,{'cmd':'status'},timeout=1)
                except OSError:time.sleep(.02);continue
                if not args.end_frame and not armed and s['frame']>=args.arm_frame:
                    arm=request(args.port,{'cmd':'stop_pc','pc':args.stop_pc});assert arm['ok'];armed=True
                cpu=process.cpu_times();samples.append({'wall_s':time.monotonic()-started,
                    'cpu_s':cpu.user+cpu.system,'process_cycles':cycles.read(),
                    'rss_bytes':process.memory_info().rss,'status':s})
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
            cycle_start=(lo['process_cycles']+f*(hi['process_cycles']-lo['process_cycles'])
                         if lo['process_cycles'] is not None else None)
            result={'label':label,'binary':identity(exe),'command':command,'start_frame':args.start_frame,
                'cpu_affinity':process.cpu_affinity(),
                'end_frame':s['frame'],'cpu_ms':1000*(samples[-1]['cpu_s']-cpu_start),
                'process_cycles':samples[-1]['process_cycles']-cycle_start if cycle_start is not None else None,
                'peak_rss_bytes':max(v['rss_bytes'] for v in samples),'start_bracket':[lo,hi],
                'host_cpu_before':host_before._asdict(),'host_cpu_after':host_after._asdict(),
                'loader_jobs':state['loader_jobs']}
            (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
            return result,state
        finally:
            if cycles is not None:cycles.close()
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
    p.add_argument('--reference-control',action='store_true',help='Explicit A/A run; no native activity expected')
    p.add_argument('--cpu-core',type=int,help='Matched process-local affinity; does not change other applications')
    p.add_argument('--presentation',choices=('headless','dummy'),default='headless')
    args=p.parse_args()
    with socket.socket() as probe:assert probe.connect_ex(('127.0.0.1',args.port))!=0
    args.output.mkdir(parents=True,exist_ok=False)
    harness=args.output/'harness';harness.mkdir()
    for name in ('loader_job_compare.py','bios_options_smoke.py','build_provenance.py',
                 'hotelmario_controller_probe.py','hotelmario_launch_probe.py','scanout_hle_compare.py'):
        shutil.copy2(pathlib.Path(__file__).with_name(name),harness/name)
    (args.output/'player.cfg').write_text('[input]\ncapture_mouse = false\n[rtc]\nsync_host_on_startup = false\n')
    meta={'schema':1,'reference':verify(args.reference),'candidate':verify(args.candidate),
        'rom':identity(args.rom),'disc':identity(disc_bin_path(args.disc)),'script':SCRIPT,
        'end_contract':{'pc':None if args.end_frame else args.stop_pc,'frame':args.end_frame,'expect_crc_error':args.expect_crc_error},
        'reference_control':args.reference_control,
        'cpu_core':args.cpu_core,
        'machine':{'platform':platform.platform(),'processor':platform.processor(),
                   'processor_identifier':os.environ.get('PROCESSOR_IDENTIFIER'),'logical_cpus':psutil.cpu_count()},
        'settings':{'presentation':args.presentation,'audio':'dummy','pace_speed':32,
                    'rtc_sync_host_on_startup':False,'start_frame':args.start_frame},
        'harness':[identity(x) for x in sorted(harness.iterdir())],
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
            if name=='candidate' and not args.reference_control:assert state['loader_jobs'].get('words',0)>0,'Native job was not exercised'
            if pair:results.append(result)
        left,right=semantic(states['reference']),semantic(states['candidate'])
        differences=[key for key in left if left[key]!=right[key]]
        contract={'pair':prefix,'equal':not differences,'differences':differences};contracts.append(contract)
        (args.output/'contracts.json').write_text(json.dumps(contracts,indent=2))
        if differences:raise RuntimeError(f'Loader job contract mismatch: {differences}')
    savings=[];cycle_savings=[]
    for pair in range(1,args.pairs+1):
        a=next(r for r in results if r['label']==f'pair{pair}-reference')
        b=next(r for r in results if r['label']==f'pair{pair}-candidate')
        savings.append(100*(1-b['cpu_ms']/a['cpu_ms']))
        if a['process_cycles'] is not None and b['process_cycles'] is not None:
            cycle_savings.append(100*(1-b['process_cycles']/a['process_cycles']))
    scope=(f'Hotel Mario USA scripted gameplay fields {args.start_frame}..{args.end_frame}' if args.end_frame else
           'Hotel Mario USA real main-module CRC rejection' if args.expect_crc_error else
           'Hotel Mario USA real main-module load to first native entry')
    summary={'schema':1,'status':'collected; assess samples and variation','scope':scope,
        'results':results,'contracts':contracts,'paired_cpu_savings_percent':savings,
        'paired_process_cycle_savings_percent':cycle_savings,
        'median_process_cycle_savings_percent':statistics.median(cycle_savings) if cycle_savings else None,
        'median_cpu_savings_percent':statistics.median(savings)}
    (args.output/'campaign-evidence.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
