import hashlib,json,os,pathlib,subprocess,sys,time
root=pathlib.Path('/tmp/stanli-allocator-rollout.l6S5ZY/pr')
out=pathlib.Path('/tmp/stanli-allocator-rollout.l6S5ZY/apple-profiles')
out.mkdir()
models=pathlib.Path('/tmp/stanli-allocator-rollout.l6S5ZY/ci-34751295812-linux-x86_64/models/inputs/normal_262144')
commands=[]
hashes=[]
for slot,mode in [('system-on','system'),('private-on','private')]:
    lib=pathlib.Path('/tmp/stanli-allocator-rollout.l6S5ZY/apple-ablation-libs')/slot/'libstanli_allocator_benchmark.dylib'
    cmd=list(map(str,[sys.executable,root/'tools/allocator/bench.py','worker','--library',lib,'--mode',mode,
        '--mir',models/'model.mir','--data',models/'data.json','--workers',8,'--reps',64000,'--samples',1,
        '--snapshot',out/(slot+'.snapshot')]))
    env={k:v for k,v in os.environ.items() if not k.startswith(('DYLD_','MIMALLOC_','TCMALLOC_','Malloc','STANLI_'))}
    with (out/(slot+'.stdout')).open('x') as stdout,(out/(slot+'.stderr')).open('x') as stderr:
        process=subprocess.Popen(cmd,stdout=stdout,stderr=stderr,env=env,cwd=root)
        time.sleep(1)
        sample=['sample',str(process.pid),'8','1','-file',str(out/(slot+'.sample'))]
        result=subprocess.run(sample,capture_output=True,text=True,timeout=30)
        (out/(slot+'.sample-stdout')).write_text(result.stdout)
        (out/(slot+'.sample-stderr')).write_text(result.stderr)
        commands.append(dict(slot=slot,worker=cmd,sample=sample,sample_returncode=result.returncode,
            library_sha256=hashlib.sha256(lib.read_bytes()).hexdigest()))
        assert process.wait(timeout=60)==0
    hashes.append(hashlib.sha256((out/(slot+'.snapshot')).read_bytes()).hexdigest())
assert len(set(hashes))==1
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
(out/'parity.json').write_text(json.dumps(dict(snapshot_sha256=hashes[0],matched=True))+'\n')
print('Both profiled executions completed with exact gradient snapshots.')
