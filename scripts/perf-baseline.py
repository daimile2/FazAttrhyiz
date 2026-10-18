#!/usr/bin/env python3
"""Bounded product baseline, not a kernel microbenchmark. Fresh disposable lab only.
No kernel modifications. Native HTTP/1.0 serial server + pinned Linux hush pipes.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import time

ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--lab',type=Path,required=True,help='new directory; no existing disk accepted')
p.add_argument('--backend',choices=('qemu','vbox'),default='qemu')
p.add_argument('--cpus',default='1,4')
p.add_argument('--samples',type=int,default=3)
p.add_argument('--requests',type=int,default=20,help='requests per sample; total including 8 warmups <=100')
p.add_argument('--pipes',type=int,default=32,help='6000 bytes each; output <=1MiB')
p.add_argument('--port',type=int,default=18086)
a=p.parse_args()
cpus=[int(c) for c in a.cpus.split(',')]
assert cpus and len(cpus)==len(set(cpus)) and all(1<=c<=8 for c in cpus)
assert 1<=a.samples<=5 and 1<=a.requests and 8+a.samples*a.requests<=100
assert 1<=a.pipes<=128 and 1024<=a.port<=65535
lab=a.lab.resolve();lab.mkdir(parents=True,exist_ok=False)
(lab/'.capmix-perf-disposable').write_text('perf baseline v1\n')
date=datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')


def call(*args):
    r=subprocess.run(list(map(str,args)),cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
    if r.returncode:raise RuntimeError(str(args)+':\n'+r.stdout)
    return r.stdout


class Guest:
    def __init__(self,disk,cpus,tag):
        self.disk=disk;self.cpus=cpus;self.tag=tag;self.proc=None;self.monitor=None;self.created=False
        self.vm='CapMix-Perf-'+str(time.time_ns());self.log=lab/(tag+'-serial.log')
    def text(self):return self.log.read_text(errors='replace') if self.log.exists() else ''
    def wait(self,marker,offset=0,timeout=60):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            s=self.text()
            if marker in s[offset:]:return s[offset:]
            assert '*** PANIC' not in s and '[FAULT]' not in s,s[-2000:]
            if self.proc:assert self.proc.poll() is None,s[-2000:]
            time.sleep(.005)
        raise AssertionError('timeout '+marker+'; see '+str(self.log))
    def boot(self):
        if a.backend=='qemu':
            self.sock=lab/(self.tag+'.sock')
            self.output=(lab/(self.tag+'-host.log')).open('w')
            self.proc=subprocess.Popen(['qemu-system-x86_64','-enable-kvm','-m','512','-cpu','host','-smp',str(self.cpus),
                '-drive',f'id=root,file={self.disk},if=none,format=raw','-device','ich9-ahci,id=ahci','-device','ide-hd,drive=root,bus=ahci.0',
                '-netdev',f'user,id=n0,hostfwd=tcp:127.0.0.1:{a.port}-:8080','-device','e1000,netdev=n0',
                '-serial','file:'+str(self.log),'-display','none','-no-reboot','-monitor','unix:'+str(self.sock)+',server=on,wait=off'],stdout=self.output,stderr=subprocess.STDOUT)
        else:
            self.medium=lab/(self.tag+'.vdi')
            call('VBoxManage','convertfromraw',self.disk,self.medium,'--format','VDI')
            call('VBoxManage','createvm','--name',self.vm,'--ostype','Other_64','--basefolder',lab/'vms','--register');self.created=True
            call('VBoxManage','modifyvm',self.vm,'--memory','512','--cpus',self.cpus,'--ioapic','on','--acpi','on',
                 '--chipset','piix3','--firmware','efi64','--graphicscontroller','vmsvga','--vram','16','--keyboard','ps2','--mouse','ps2',
                 '--nic1','nat','--nic-type1','82540EM','--cable-connected1','on','--boot1','disk','--boot2','none','--boot3','none','--boot4','none',
                 '--uart1','0x3f8','4','--uart-mode1','file',self.log,'--natpf1',f'perf,tcp,127.0.0.1,{a.port},10.0.2.15,8080')
            call('VBoxManage','storagectl',self.vm,'--name','SATA','--add','sata','--controller','IntelAhci','--portcount','1')
            call('VBoxManage','storageattach',self.vm,'--storagectl','SATA','--port','0','--device','0','--type','hdd','--medium',self.medium)
            call('VBoxManage','startvm',self.vm,'--type','headless')
        self.wait('capmix> ')
        assert 'root=disk' in self.text() and 'boot mode: product' in self.text() and 'persona: compiled=1 enabled=1' in self.text()
        assert re.search(r'\bcpu_count='+str(self.cpus)+r'\b',self.text()),'guest CPU count mismatch'
        if self.cpus>1:assert f'online={self.cpus}/{self.cpus}' in self.text(),'AP bringup incomplete'
        if self.proc:
            self.monitor=socket.socket(socket.AF_UNIX);self.monitor.connect(str(self.sock));self.monitor.settimeout(.001)
    def send(self,text):
        if self.proc:
            keys={' ':'spc','/':'slash','.':'dot','-':'minus','>':'shift-dot','&':'shift-7','\n':'ret'}
            for c in text:
                self.monitor.sendall(('sendkey '+keys.get(c,c)+' 8\n').encode());time.sleep(0 if c=='\n' else .025)
                try:self.monitor.recv(65536)
                except TimeoutError:pass
        else:
            codes={c:int(h,16) for c,h in zip('1234567890qwertyuiopasdfghjklzxcvbnm',
                '02 03 04 05 06 07 08 09 0a 0b 10 11 12 13 14 15 16 17 18 19 1e 1f 20 21 22 23 24 25 26 2c 2d 2e 2f 30 31 32'.split())}
            codes.update({' ':0x39,'/':0x35,'.':0x34,'-':0x0c,'\n':0x1c})
            for c in text:
                shift=c in '>&';key=0x34 if c=='>' else 0x08 if c=='&' else codes[c]
                seq=([0x2a] if shift else [])+[key,key|0x80]+([0xaa] if shift else [])
                call('VBoxManage','controlvm',self.vm,'keyboardputscancode',*[f'{n:02x}' for n in seq]);time.sleep(0 if c=='\n' else .02)
    def command(self,cmd,marker):
        offset=len(self.text());self.send(cmd)
        # Timer begins immediately before Return. Typing command text is excluded;
        # Return injection, monitor/CLI latency and 5ms completion polling remain.
        start=time.perf_counter();self.send('\n');s=self.wait(marker,offset)
        elapsed=time.perf_counter()-start
        assert 'shell error=' not in s,s
        return s,elapsed
    def stop(self,export=False):
        self.command('poweroff','ACPI S5 request')
        if self.proc:self.proc.wait(timeout=20);assert self.proc.returncode==0
        else:
            end=time.monotonic()+20
            while time.monotonic()<end:
                state=subprocess.run(['VBoxManage','showvminfo',self.vm,'--machinereadable'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=5)
                if not state.returncode and 'VMState="poweroff"' in state.stdout:break
                if state.returncode:
                    with (lab/(self.tag+'-control.log')).open('a') as out:out.write(state.stdout)
                time.sleep(.1)
            else:raise AssertionError('VBox poweroff timeout: '+state.stdout[-1000:])
            if export:
                target=lab/(self.tag+'-after.img')
                call('VBoxManage','clonemedium','disk',self.medium,target,'--format','RAW');target.replace(self.disk)
    def close(self):
        if self.monitor:self.monitor.close()
        if self.proc:
            if self.proc.poll() is None:self.proc.terminate();self.proc.wait(timeout=5)
            self.output.close()
        if self.created:
            subprocess.run(['VBoxManage','controlvm',self.vm,'poweroff'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            detached=subprocess.run(['VBoxManage','storageattach',self.vm,'--storagectl','SATA','--port','0','--device','0','--type','hdd','--medium','none'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            if detached.returncode==0:
                # Retain lab VDI files, remove only our unused media registrations.
                subprocess.run(['VBoxManage','closemedium','disk',str(self.medium)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            subprocess.run(['VBoxManage','unregistervm',self.vm]+(['--delete'] if detached.returncode==0 else []),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)


def http_batch(count):
    start=time.perf_counter();deadline=start+40
    for _ in range(count):
        remaining=deadline-time.perf_counter();assert remaining>0,'HTTP batch deadline exceeded'
        with socket.create_connection(('127.0.0.1',a.port),timeout=min(3,remaining)) as s:
            s.sendall(b'GET / HTTP/1.0\r\nHost: perf\r\nConnection: close\r\n\r\n')
            response=bytearray()
            while True:
                remaining=deadline-time.perf_counter();assert remaining>0,'HTTP deadline exceeded'
                s.settimeout(min(3,remaining));b=s.recv(1024)
                if not b:break
                response.extend(b);assert len(response)<=4096
        headers,body=bytes(response).split(b'\r\n\r\n',1)
        assert headers.startswith(b'HTTP/1.0 200 OK\r\n') and b'Content-Length: 25' in headers.split(b'\r\n')
        assert body==b'hello from CapMix httpd!\n'
    return time.perf_counter()-start


def main():
    metadata=dict(date=date,backend=a.backend,cpus=cpus,memory_mib=512,nic='e1000',disk='AHCI/no-ISO',linux=1,linux_interp=0,
        samples=a.samples,http_requests=a.requests,http_warmup=8,pipes=a.pipes,pipe_bytes=6000,
        pipe_total_bytes=a.pipes*6000,vbox_carrier_setup_wait_seconds=6 if a.backend=='vbox' else 0,clock='host perf_counter, wall time',host_platform=platform.platform(),
        git_revision=call('git','rev-parse','HEAD').strip(),qemu_cpu='host',qemu_accel='kvm',
        qemu=call('qemu-system-x86_64','--version').splitlines()[0],
        vbox=call('VBoxManage','--version').strip() if a.backend=='vbox' else 'not used')
    (lab/'config.json').write_text(json.dumps(metadata,indent=2)+'\n')
    (lab/'build.log').write_text(call('make','kernel','build/cow-put','build/perf-verify.cmx'))
    seed=lab/'seed.img';(lab/'install.log').write_text(call('make','install-disk','DISK_IMAGE='+str(seed),'INSTALL_LINUX=1'))
    g=Guest(seed,1,'provision')
    try:g.boot();g.stop(export=True)
    finally:g.close()
    payload=lab/'input.txt';payload.write_bytes(b'x'*6000)
    script=lab/'pipe.sh';script.write_text(('/bin/busybox-sh.elf cat /perf-input.txt | /bin/busybox-sh.elf cat || exit 91\n')*a.pipes+'exit 0\n')
    verify=ROOT/'build/perf-verify.cmx'
    (lab/'workload-sha256.txt').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.name+'\n' for f in (payload,script,verify)))
    for src,dest in ((payload,'/perf-input.txt'),(script,'/perf-pipe.sh'),(verify,'/bin/perf-verify.cmx')):
        with (lab/'import.log').open('a') as out:out.write(call('python3','scripts/copy-to-disk.py',seed,src,dest))
    rows=[]
    for ncpu in cpus:
        disk=lab/f'smp{ncpu}.img';shutil.copyfile(seed,disk);g=Guest(disk,ncpu,f'smp{ncpu}')
        try:
            g.boot()
            # VBox can expose the product shell before virtual carrier comes up.
            # Fixed, bounded setup wait only; never retry/omit failed measured requests.
            if a.backend=='vbox':time.sleep(6)
            g.command(f'run /bin/httpd.elf 8080 {8+a.samples*a.requests} &','httpd accept=nonblocking')
            http_batch(8);http=[http_batch(a.requests) for _ in range(a.samples)]
            g.wait('httpd handled='+str(8+a.samples*a.requests))
            hush=[]
            for sample in range(a.samples+1):
                text,seconds=g.command('run /bin/busybox-sh.elf sh /perf-pipe.sh > /perf-output.txt','shell redirect committed: status=0')
                assert 'exit=0 reap=ok' in text
                proof,_=g.command('run /bin/perf-verify.cmx /perf-output.txt '+str(a.pipes*6000),'exit=0 reap=ok')
                assert 'perf pipe verified bytes='+str(a.pipes*6000) in proof
                if sample:hush.append(seconds)
            for i,(h,t) in enumerate(zip(http,hush),1):
                row=dict(date=date,backend=a.backend,smp=ncpu,nic='e1000',linux=1,sample=i,
                    http_requests=a.requests,http_seconds=h,http_rps=a.requests/h,
                    hush_bytes=a.pipes*6000,hush_seconds=t,hush_Bps=a.pipes*6000/t)
                rows.append(row)
                line=(f'PERF date={date} backend={a.backend} smp={ncpu} nic=e1000 linux=1 sample={i} '
                      f'http_n={a.requests} http_s={h:.6f} http_rps={a.requests/h:.3f} '
                      f'hush_bytes={a.pipes*6000} hush_s={t:.6f} hush_Bps={a.pipes*6000/t:.3f} verified=1')
                print(line,flush=True)
                with (lab/'numbers.log').open('a') as out:out.write(line+'\n')
            g.stop()
        finally:g.close()
    (lab/'results.json').write_text(json.dumps(dict(config=metadata,samples=rows),indent=2)+'\n')
    print('performance baseline: PASS (payload/status/EOF/exit/reap verified; no speedup assertion)',flush=True)


if __name__=='__main__':main()
