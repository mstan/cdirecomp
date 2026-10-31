"""Real TCP instruction stops enabled after admission of a native drawing job.

Uses production objects and actual generated reference, isolated retained RAM,
and task-only processes. No guest-progress writes or owner settings/saves.
"""
import argparse,hashlib,json,os,pathlib,queue,socket,subprocess,threading,time,uuid
from drawing_runtime_fixture import build_fixture

def request(port,command):
    with socket.create_connection(('127.0.0.1',port),timeout=2) as connection:
        connection.sendall((json.dumps(command)+'\n').encode())
        return json.loads(connection.makefile('rb').readline())

def memory(port,base,length):
    output=bytearray()
    for offset in range(0,length,1024):
        response=request(port,{'cmd':'read_mem','addr':base+offset,'len':min(1024,length-offset)})
        assert response.get('ok'),response
        chunk=bytes.fromhex(response['bytes']);assert len(chunk)==min(1024,length-offset),response
        output.extend(chunk)
    return bytes(output)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','rom','ram0','ram1'):p.add_argument(name,type=pathlib.Path)
    p.add_argument('--output',type=pathlib.Path,required=True)
    p.add_argument('--port',type=int,default=4475)
    p.add_argument('--expect-regression',action='store_true')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    exe,runtime,provenance=build_fixture(a.build,a.output)
    results=[]
    for case,target,delay,clock in (('callback-delay',0x1800,2,0),('mid-segment-arm',0x2762E6,0,0x2762E0)):
        # 0x2762E0 = module+0x17020; future CLR D5 is module+0x17026.
        states={}
        for mode in ('reference','native'):
            directory=a.output/(case+'-'+mode);directory.mkdir()
            with socket.socket() as probe:
                assert probe.connect_ex(('127.0.0.1',a.port))!=0,'Test port already occupied'
            session=uuid.uuid4().hex;commands=[];lines=queue.Queue();stdout_lines=[]
            command=[str(exe),str(a.rom.resolve()),str(a.ram0.resolve()),str(a.ram1.resolve()),
                str(directory/'finished.json'),mode,'8','1','3','0','594484','1',str(a.port),session]
            if clock:command.append(hex(clock))
            env={**os.environ,'SDL_AUDIODRIVER':'dummy','SDL_VIDEODRIVER':'dummy'}
            with (directory/'stderr.log').open('wb') as stderr:
                proc=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=stderr,
                    env=env,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                def reader(stream=proc.stdout):
                    for line in iter(stream.readline,b''):
                        text=line.decode(errors='replace').strip();stdout_lines.append(text);lines.put(text)
                thread=threading.Thread(target=reader,daemon=True);thread.start()
                def line(expected):
                    deadline=time.monotonic()+10
                    while time.monotonic()<deadline:
                        try:got=lines.get(timeout=.1)
                        except queue.Empty:
                            assert proc.poll() is None,stdout_lines
                            continue
                        if got==expected:return
                    raise TimeoutError((expected,stdout_lines))
                try:
                    line('drawing-debug-ready')
                    deadline=time.monotonic()+10
                    while True:
                        try:
                            ping=request(a.port,{'cmd':'ping'})
                            assert ping.get('session')==session,ping
                            break
                        except OSError:
                            if time.monotonic()>deadline:raise
                            time.sleep(.01)
                    if clock:
                        proc.stdin.write(b'\n');proc.stdin.flush();line('drawing-clock-ready')
                    stop={'cmd':'stop_pc','pc':target,'delay':delay}
                    reply=request(a.port,stop);commands.append({'request':stop,'response':reply});assert reply['ok'],reply
                    proc.stdin.write(b'\n');proc.stdin.flush()
                    deadline=time.monotonic()+10;held=False
                    while time.monotonic()<deadline:
                        if proc.poll() is not None:break
                        try:status=request(a.port,{'cmd':'status'})
                        except OSError:continue
                        if status.get('held'):held=True;break
                        time.sleep(.01)
                    state={'held':held}
                    if held:
                        state['registers']=request(a.port,{'cmd':'get_registers'})
                        for name in ('video_state','audio_state','ciap_state'):
                            state[name]=request(a.port,{'cmd':name})
                        state['ram_sha256']=[]
                        for base in (0,0x200000):
                            data=memory(a.port,base,0x80000);(directory/f'ram-{base:06x}.bin').write_bytes(data)
                            state['ram_sha256'].append(hashlib.sha256(data).hexdigest())
                        state['tail']=int.from_bytes(memory(a.port,0x51B2,4),'big')
                        state['drawing_jobs']=request(a.port,{'cmd':'drawing_jobs'})
                    else:
                        assert proc.poll() is not None,'Fixture did not stop or finish'
                        state['finished']=json.loads((directory/'finished.json').read_text())
                    (directory/'state.json').write_text(json.dumps(state,indent=2)+'\n')
                    (directory/'commands.json').write_text(json.dumps({'session':session,'command':command,'tcp':commands},indent=2)+'\n')
                    states[mode]=state
                finally:
                    if proc.poll() is None:proc.terminate()
                    proc.wait(timeout=10);proc.stdin.close();proc.stdout.close();thread.join(timeout=2)
                    (directory/'stdout.log').write_text('\n'.join(stdout_lines)+'\n')
        left=states['reference'];right=states['native']
        assert left['held'],left
        expected=0x276304 if case=='callback-delay' else target
        assert left['registers']['pc']==expected,(case,left['registers'])
        assert left['tail']==0xCC18,(case,left['tail'])
        reference_jobs=left['drawing_jobs']
        native_jobs=right['drawing_jobs'] if right['held'] else right['finished']['jobs']
        assert reference_jobs['jobs']==0,(case,reference_jobs)
        assert native_jobs['jobs']>0,(case,native_jobs)
        assert native_jobs['fallbacks']==0 if a.expect_regression else native_jobs['fallbacks']>0,(case,native_jobs)
        left.pop('drawing_jobs',None);right.pop('drawing_jobs',None)
        equal=left==right;results.append({'case':case,'equal':equal,'expected_pc':expected,'expected_tail':0xCC18,
                                        'reference_jobs':reference_jobs,'native_jobs':native_jobs})
        if a.expect_regression:assert not equal,'Expected baseline debugger defect was not reproduced'
        else:assert equal,(case,left,right)
    summary={'status':'regression reproduced' if a.expect_regression else 'pass','cases':results,
        'provenance':provenance,'production_binary_sha256':hashlib.sha256(runtime.read_bytes()).hexdigest(),
        'fixture_binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()}
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({'status':summary['status'],'cases':results}))
if __name__=='__main__':main()
