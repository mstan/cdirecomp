"""Matched real module loading, ending at its verified native execution entry."""
import argparse,base64,copy,hashlib,json,os,pathlib,platform,shutil,socket,statistics,subprocess,time
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

def gpu_memory(pid):
    """One endpoint sample of actual WDDM process allocations, outside timing."""
    if os.name!='nt':return None
    script=r"""$s=@((Get-Counter '\GPU Process Memory(*)\Dedicated Usage','\GPU Process Memory(*)\Shared Usage' -ErrorAction SilentlyContinue).CounterSamples | Where-Object {$_.InstanceName -like 'pid_PID_*'});
if($s.Count -lt 2 -or @($s | Where-Object {$_.Status -gt 1}).Count){throw 'Missing valid process GPU memory counters'}
@{source='Windows GPU Process Memory';dedicated_bytes=[int64](($s | Where-Object {$_.Path -like '*\Dedicated Usage'} | Measure-Object CookedValue -Sum).Sum);shared_bytes=[int64](($s | Where-Object {$_.Path -like '*\Shared Usage'} | Measure-Object CookedValue -Sum).Sum);counter_samples=$s.Count} | ConvertTo-Json -Compress
""".replace('PID',str(int(pid)))
    encoded=base64.b64encode(script.encode('utf-16le')).decode()
    output=subprocess.check_output(['C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
        '-NoProfile','-OutputFormat','Text','-EncodedCommand',encoded],text=True,stderr=subprocess.PIPE,timeout=20,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    return json.loads(output)

def exercise_window(pid):
    """Resize/expose this runtime offscreen without activating any window."""
    import ctypes
    from ctypes import wintypes as w
    user=ctypes.WinDLL('user32',use_last_error=True)
    user.GetForegroundWindow.restype=w.HWND
    user.GetWindowThreadProcessId.argtypes=[w.HWND,ctypes.POINTER(w.DWORD)]
    user.GetClassNameW.argtypes=[w.HWND,w.LPWSTR,ctypes.c_int]
    user.GetWindowRect.argtypes=[w.HWND,ctypes.POINTER(w.RECT)]
    user.GetClientRect.argtypes=[w.HWND,ctypes.POINTER(w.RECT)]
    user.SetWindowPos.argtypes=[w.HWND,w.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.UINT]
    user.ShowWindow.argtypes=[w.HWND,ctypes.c_int]
    user.RedrawWindow.argtypes=[w.HWND,ctypes.c_void_p,ctypes.c_void_p,w.UINT]
    callback_type=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    user.EnumWindows.argtypes=[callback_type,w.LPARAM]
    windows=[]
    @callback_type
    def collect(hwnd,unused):
        owner=w.DWORD();user.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        if owner.value==pid:
            name=ctypes.create_unicode_buffer(128);user.GetClassNameW(hwnd,name,len(name))
            if name.value=='SDL_app':windows.append(hwnd)
        return True
    user.EnumWindows(collect,0);assert len(windows)==1,windows
    hwnd=windows[0];foreground=user.GetForegroundWindow();before=w.RECT();after=w.RECT();client=w.RECT()
    assert user.GetWindowRect(hwnd,ctypes.byref(before))
    width=2*(before.right-before.left)+1;height=2*(before.bottom-before.top)+1
    x=user.GetSystemMetrics(76)-width-128;y=user.GetSystemMetrics(77)-height-128
    assert user.SetWindowPos(hwnd,None,x,y,width,height,0x14) # NOACTIVATE | NOZORDER
    user.ShowWindow(hwnd,4) # SHOWNOACTIVATE; the complete window is offscreen
    assert user.RedrawWindow(hwnd,None,None,0x105) # INVALIDATE | ERASE | UPDATENOW
    assert user.GetForegroundWindow()==foreground,'Owner foreground changed'
    assert user.GetWindowRect(hwnd,ctypes.byref(after)) and user.GetClientRect(hwnd,ctypes.byref(client))
    return dict(kind='real_Win32_resize_expose_offscreen',foreground_preserved=True,
        outer_before=[before.right-before.left,before.bottom-before.top],
        outer_after=[after.right-after.left,after.bottom-after.top],client_after=[client.right,client.bottom])

def run(exe,args,label):
    out=args.output/label;out.mkdir()
    env=os.environ.copy();env.update(CDI_PACE_SPEED=str(args.pace_speed),SDL_AUDIODRIVER='dummy',
        CDI_PLAYER_CONFIG_PATH=str(args.output/'player.cfg'),TEMP=str(args.output),TMP=str(args.output))
    if args.presentation=='dummy':env.update(SDL_VIDEODRIVER='dummy',SDL_RENDER_DRIVER='software')
    if args.presentation=='hardware':env.update(SDL_RENDER_DRIVER='opengl',CDI_WINDOW_HIDDEN='1',CDI_GPU_PROFILE='1')
    command=[str(exe.resolve()),str(args.rom.resolve()),'--disc',str(args.disc.resolve()),
        '--fault-hold','--port',str(args.port),'--input-script',SCRIPT]
    if args.presentation=='headless':command.append('--headless')
    if args.end_frame and not args.live_frame_stop:command+=['--stop-frame',str(args.end_frame)]
    samples=[];started=time.monotonic();host_before=psutil.cpu_times();armed=False;window_event=None
    with (out/'stdout.log').open('w') as stdout,(out/'stderr.log').open('w') as stderr:
        proc=subprocess.Popen(command,env=env,stdout=stdout,stderr=stderr,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        cycles=None
        try:
            process=psutil.Process(proc.pid)
            cycles=CycleCounter(proc.pid)
            if args.cpu_core is not None:process.cpu_affinity([args.cpu_core])
            collection_limit=max(120,(args.end_frame or 0)/(50*args.pace_speed)+30)
            while time.monotonic()-started<collection_limit:
                if proc.poll() is not None:raise RuntimeError(f'{label}: runtime exited')
                try:s=request(args.port,{'cmd':'status'},timeout=1)
                except OSError:time.sleep(.02);continue
                if args.end_frame and args.live_frame_stop and not armed:
                    assert s['frame']<args.end_frame,'Frame stop armed too late'
                    arm=request(args.port,{'cmd':'stop_frame','frame':args.end_frame});assert arm['ok'];armed=True
                if args.window_events and window_event is None and s['frame']>=900:
                    before=request(args.port,{'cmd':'render_state'})['jobs']
                    window_event=exercise_window(proc.pid);window_event['before']=before
                    (out/'window-events.json').write_text(json.dumps(window_event,indent=2))
                if not args.end_frame and not armed and s['frame']>=args.arm_frame:
                    arm=request(args.port,{'cmd':'stop_pc','pc':args.stop_pc});assert arm['ok'];armed=True
                cpu=process.cpu_times();samples.append({'wall_s':time.monotonic()-started,
                    'cpu_s':cpu.user+cpu.system,'process_cycles':cycles.read(),
                    'rss_bytes':process.memory_info().rss,'status':s})
                if s['miss_count'] or s['main_resets']:raise RuntimeError(s)
                if s['held']:break
                time.sleep(.01)
            assert s['held'] and (s['frame']==args.end_frame if args.end_frame else s['pc']==args.stop_pc),s
            if args.expect_end_frame is not None:assert s['frame']==args.expect_end_frame,s
            host_after=psutil.cpu_times()
            gpu_memory_sample=None
            if args.presentation=='hardware':
                try:gpu_memory_sample=gpu_memory(proc.pid)
                except (subprocess.SubprocessError,ValueError) as error:
                    # Retain timing/correctness even if this separate resource
                    # sample fails; an error does not qualify memory headroom.
                    gpu_memory_sample={'error':str(error),'source':'Windows GPU Process Memory'}
            state={k:request(args.port,{'cmd':k}) for k in ('status','get_registers','ciap_state',
                'video_frame','video_state','audio_state','native_state','native_events','ikat_events')}
            state['loader_jobs']=request(args.port,{'cmd':'loader_jobs'})
            state['drawing_jobs']=request(args.port,{'cmd':'drawing_jobs'})
            state['memory_jobs']=request(args.port,{'cmd':'memory_jobs'})
            state['service_jobs']=request(args.port,{'cmd':'service_jobs'})
            state['streaming_jobs']=request(args.port,{'cmd':'streaming_jobs'})
            state['board_jobs']=request(args.port,{'cmd':'board_jobs'})
            state['render_state']=request(args.port,{'cmd':'render_state'})
            if args.presentation=='hardware':
                render=state['render_state']
                assert render['enabled'] and render['gpu_timestamps'] and all(render['gpu_counts']),render
                assert not any(name in render['renderer'].lower() for name in ('llvmpipe','software','swiftshader')),render
                assert not render['native_mismatches'],render
                if int(env.get('CDI_GPU_READBACK_STRIDE','60')):assert render['native_readbacks']>0,render
                if args.window_events:
                    gpu=render['jobs'];assert window_event and (gpu['output_width']>window_event['before']['output_width'] or gpu['output_height']>window_event['before']['output_height']),gpu
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
            if args.start_frame==0:
                # Process accounting starts at creation. Zero is the defined
                # origin, not a fabricated observed frame/counter sample.
                cpu_start=0.0;cycle_start=0 if samples[-1]['process_cycles'] is not None else None
                start_bracket=None;start_origin='process_creation'
            else:
                upper=next(i for i,v in enumerate(samples) if v['status']['frame']>=args.start_frame)
                assert upper
                lo,hi=samples[upper-1:upper+1]
                f=(args.start_frame-lo['status']['frame'])/(hi['status']['frame']-lo['status']['frame'])
                cpu_start=lo['cpu_s']+f*(hi['cpu_s']-lo['cpu_s'])
                cycle_start=(lo['process_cycles']+f*(hi['process_cycles']-lo['process_cycles'])
                             if lo['process_cycles'] is not None else None)
                start_bracket=[lo,hi];start_origin='frame_interpolation'
            result={'label':label,'binary':identity(exe),'command':command,'start_frame':args.start_frame,
                'cpu_affinity':process.cpu_affinity(),
                'end_frame':s['frame'],'cpu_ms':1000*(samples[-1]['cpu_s']-cpu_start),
                'process_cycles':samples[-1]['process_cycles']-cycle_start if cycle_start is not None else None,
                'peak_rss_bytes':max(v['rss_bytes'] for v in samples),'start_bracket':start_bracket,
                'start_origin':start_origin,'first_sample':samples[0],'final_sample':samples[-1],
                'host_cpu_before':host_before._asdict(),'host_cpu_after':host_after._asdict(),
                'gpu_memory':gpu_memory_sample,
                'window_event':window_event,
                'loader_jobs':state['loader_jobs'],'drawing_jobs':state['drawing_jobs'],'memory_jobs':state['memory_jobs'],
                'service_jobs':state['service_jobs'],'streaming_jobs':state['streaming_jobs'],
                'board_jobs':state['board_jobs'],'render_state':state['render_state']}
            (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
            return result,state
        finally:
            if cycles is not None:cycles.close()
            if proc.poll() is None:proc.terminate()
            proc.wait(timeout=10)

def semantic(state,presentation='headless'):
    d=copy.deepcopy(state);d.pop('loader_jobs',None);d.pop('drawing_jobs',None);d.pop('memory_jobs',None)
    d.pop('service_jobs',None)
    d.pop('streaming_jobs',None)
    d.pop('board_jobs',None)
    d.pop('render_state',None)
    for key in ('insns','blocks','interp','irq_first_seq'):d['status'].pop(key,None)
    for key in ('scanout_jobs','fused_jobs','reused_jobs'):d['video_state'].pop(key,None)
    d['native_state'].pop('dispatches',None)
    if presentation in ('dummy','hardware'):
        # Host-time SDL queue consumption is not guest transport or PCM output.
        for key in ('queued_frames','dropped_frames'):d['audio_state'].pop(key,None)
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
    p.add_argument('--live-frame-stop',action='store_true',help='Arm existing TCP frame stop after startup, preserving pacing and frontend')
    p.add_argument('--expect-crc-error',action='store_true')
    p.add_argument('--reference-control',action='store_true',help='Explicit A/A run; no native activity expected')
    p.add_argument('--cpu-core',type=int,help='Matched process-local affinity; does not change other applications')
    p.add_argument('--pace-speed',type=float,default=32,help='Emulated pacing multiplier, identical for both builds')
    p.add_argument('--expect-end-frame',type=int,help='Reject a repeated stop PC reached at the wrong frame')
    p.add_argument('--presentation',choices=('headless','dummy','hardware'),default='headless')
    p.add_argument('--window-events',action='store_true',help='Exercise this runtime window offscreen with real resize/expose and preserve foreground')
    p.add_argument('--native-job',choices=('loader','drawing','memory','service','streaming','board','gpu'),default='loader')
    args=p.parse_args()
    assert args.start_frame>=0,'Start frame must be zero (process creation) or positive'
    assert args.pace_speed>0,'Pacing multiplier must be positive'
    assert not args.live_frame_stop or args.end_frame,'Live frame stop requires --end-frame'
    assert not args.window_events or (os.name=='nt' and args.presentation=='hardware'),'Window event check requires Windows hardware frontend'
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
        'native_job':args.native_job,
        'cpu_core':args.cpu_core,
        'machine':{'platform':platform.platform(),'processor':platform.processor(),
                   'processor_identifier':os.environ.get('PROCESSOR_IDENTIFIER'),'logical_cpus':psutil.cpu_count()},
        'settings':{'presentation':args.presentation,'audio':'dummy','pace_speed':args.pace_speed,
                    'window_events':args.window_events,
                    'rtc_sync_host_on_startup':False,'start_frame':args.start_frame,
                    'actual_pacing':not args.end_frame or args.live_frame_stop,
                    'actual_frontend':args.presentation in ('dummy','hardware') and (not args.end_frame or args.live_frame_stop),
                    'frame_stop_control':'tcp_live' if args.live_frame_stop else 'startup_investigation' if args.end_frame else None},
        'render_environment':{key:os.environ.get(key) for key in ('CDI_GPU_READBACK_STRIDE','CDI_INTERNAL_SCALE')},
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
            if name=='candidate' and not args.reference_control:
                if args.native_job=='gpu':
                    gpu=state['render_state']['jobs']
                    assert gpu.get('available') and not gpu.get('failed') and gpu.get('frames',0)>0,gpu
                else:
                    job,key=({'drawing':('drawing_jobs','jobs'),'memory':('memory_jobs','jobs'),'service':('service_jobs','jobs'),'streaming':('streaming_jobs','jobs'),'board':('board_jobs','jobs')}.get(args.native_job,('loader_jobs','words')))
                    assert state[job].get(key,0)>0,'Native job was not exercised'
            if pair:results.append(result)
        left,right=semantic(states['reference'],args.presentation),semantic(states['candidate'],args.presentation)
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
           f'Hotel Mario USA process service/publication through ${args.stop_pc:06X}' if args.native_job=='service' else
           f'Hotel Mario USA firmware fill through caller continuation ${args.stop_pc:06X}' if args.native_job=='memory' else
           'Hotel Mario USA real main-module CRC rejection' if args.expect_crc_error else
           'Hotel Mario USA real main-module load to first native entry')
    summary={'schema':1,'status':'collected; assess samples and variation','scope':scope,
        'results':results,'contracts':contracts,'paired_cpu_savings_percent':savings,
        'paired_process_cycle_savings_percent':cycle_savings,
        'median_process_cycle_savings_percent':statistics.median(cycle_savings) if cycle_savings else None,
             'median_cpu_savings_percent':statistics.median(savings) if savings else None}
    (args.output/'campaign-evidence.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
