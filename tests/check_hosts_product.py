#!/usr/bin/env python3
"""Drive hosts-only HTTP on a disposable Linux-enabled installed disk.
HTTP :8000 /hello.txt must contain "hosts stub body verified". Replaces /etc/hosts.
Only disposable prepared labs. QEMU and VBox are run sequentially, never together.
"""
import argparse
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--backend',choices=['qemu','vbox'],required=True)
p.add_argument('--lab',type=Path,required=True)
p.add_argument('--port',type=int,default=18080)
a=p.parse_args();lab=a.lab.resolve();disk=lab/'system.img'
assert (lab/'.capmix-hosts').is_file() and disk.is_file(), 'prepare a disposable lab first'
assert 1024<=a.port<=65535
assert not list(lab.glob('hosts-*.log')), 'logs already exist; choose a new prepared lab'
# Do not replace someone's VM, medium or image.
vm='CapMix-Hosts-'+str(time.time_ns())
proc=None;monitor=None;created=False;running=False;medium=None;log=None

def call(*args):
    try:
        return subprocess.run(list(map(str,args)),check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60).stdout
    except subprocess.CalledProcessError as e:
        print(e.stdout,file=sys.stderr);raise

def text():
    return log.read_text(errors='replace') if log and log.exists() else ''

def wait(marker,offset=0,timeout=30):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        s=text()
        if marker in s[offset:]:return s[offset:]
        assert '*** PANIC' not in s and '[FAULT]' not in s, s[-1000:]
        if a.backend=='qemu':assert proc.poll() is None, s[-1000:]
        time.sleep(.1)
    raise AssertionError('timeout waiting for '+marker+'; see '+str(log))

def boot(stage):
    global proc,monitor,running,log,medium
    log=lab/('hosts-'+stage+'.log')
    if a.backend=='qemu':
        sock=lab/'monitor.sock'
        if sock.exists():sock.unlink()
        with (lab/('hosts-'+stage+'-host.log')).open('w') as output:
            proc=subprocess.Popen(['qemu-system-x86_64','-enable-kvm','-m','512','-cpu','host','-smp','4',
                '-drive',f'id=root,file={disk},if=none,format=raw','-device','ich9-ahci,id=ahci','-device','ide-hd,drive=root,bus=ahci.0',
                '-netdev',f'user,id=n0,hostfwd=tcp:127.0.0.1:{a.port}-:8080','-device','e1000,netdev=n0',
                '-serial','file:'+str(log),'-display','none','-no-reboot','-monitor','unix:'+str(sock)+',server=on,wait=off'],stdout=output,stderr=subprocess.STDOUT)
    else:
        medium=lab/(stage+'.vdi')
        call('VBoxManage','convertfromraw',disk,medium,'--format','VDI')
        call('VBoxManage','storageattach',vm,'--storagectl','SATA','--port','0','--device','0','--type','hdd','--medium',medium)
        call('VBoxManage','modifyvm',vm,'--uart-mode1','file',log)
        call('VBoxManage','startvm',vm,'--type','headless')
    running=True
    wait('fazattrhyiz> ')
    assert 'root=disk' in text() and 'boot mode: product' in text()
    if a.backend=='qemu':
        monitor=socket.socket(socket.AF_UNIX);monitor.connect(str(sock));monitor.settimeout(.002)

def send(command):
    if a.backend=='qemu':
        keys={' ':'spc','/':'slash','.':'dot','-':'minus','>':'shift-dot','&':'shift-7'}
        for c in command+'\n':
            key='ret' if c=='\n' else keys.get(c,c)
            monitor.sendall(('sendkey '+key+' 8\n').encode());time.sleep(.035)
            try:monitor.recv(65536)
            except TimeoutError:pass
    else:
        codes={c:int(h,16) for c,h in zip('1234567890qwertyuiopasdfghjklzxcvbnm',
            '02 03 04 05 06 07 08 09 0a 0b 10 11 12 13 14 15 16 17 18 19 1e 1f 20 21 22 23 24 25 26 2c 2d 2e 2f 30 31 32'.split())}
        codes.update({' ':0x39,'/':0x35,'.':0x34,'-':0x0c,'\n':0x1c})
        for c in command+'\n':
            shift=c in '>&';key=0x34 if c=='>' else 0x08 if c=='&' else codes[c]
            seq=([0x2a] if shift else [])+[key,key|0x80]+([0xaa] if shift else [])
            call('VBoxManage','controlvm',vm,'keyboardputscancode',*[f'{n:02x}' for n in seq])
            time.sleep(.025)

def command(cmd,marker):
    offset=len(text());send(cmd);return wait(marker,offset)

def wait_vbox_off():
    # A shutdown-time state query failed once here; retry reads with diagnostics.
    # Retry reads only; persistent errors/aborted state never count as poweroff.
    end=time.monotonic()+20;last=''
    while time.monotonic()<end:
        r=subprocess.run(['VBoxManage','showvminfo',vm,'--machinereadable'],
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=5)
        last=r.stdout
        if r.returncode==0 and 'VMState="poweroff"' in last:return
        if r.returncode:
            with (lab/'hosts-control-host.log').open('a') as f:
                f.write('shutdown state query retry: '+last+'\n')
        time.sleep(.2)
    raise AssertionError('VBox poweroff timeout: '+last[-1500:])

def stop(stage):
    global running,monitor
    send('poweroff')
    if a.backend=='qemu':
        proc.wait(timeout=20);assert proc.returncode==0
        monitor.close();monitor=None
    else:
        wait_vbox_off()
    assert 'ACPI' in text()
    running=False
    if a.backend=='vbox':
        # The VDI is authoritative after a boot. Export before changing profile.
        exported=lab/(stage+'-after.img')
        call('VBoxManage','clonemedium','disk',medium,exported,'--format','RAW')
        call('VBoxManage','storageattach',vm,'--storagectl','SATA','--port','0','--device','0','--type','hdd','--medium','none')
        call('VBoxManage','closemedium','disk',medium)
        # Keep VDI/RAW evidence, replace only this disposable lab's RAW working copy.
        exported.replace(disk)
    print(f'hosts {a.backend} {stage}: poweroff=ok no-ISO root=disk',flush=True)

try:
    status=call('python3',ROOT/'scripts/linux_package.py','status',disk)
    assert 'configured=enabled' in status, 'hosts Linux fixture requires enabled disposable disk'
    if a.backend=='vbox':
        call('VBoxManage','createvm','--name',vm,'--platform-architecture','x86','--ostype','Other_64','--basefolder',lab/'vm','--register');created=True
        call('VBoxManage','modifyvm',vm,'--memory','512','--cpus','4','--ioapic','on','--acpi','on','--chipset','piix3','--firmware','efi64',
             '--graphicscontroller','vmsvga','--vram','16','--keyboard','ps2','--mouse','ps2','--nic1','nat','--nic-type1','82540EM','--cable-connected1','on','--nat-localhostreachable1','on',
             '--boot1','disk','--boot2','none','--boot3','none','--boot4','none','--uart1','0x3f8','4',
             '--natpf1',f'httpd,tcp,127.0.0.1,{a.port},10.0.2.15,8080')
        call('VBoxManage','storagectl',vm,'--name','SATA','--add','sata','--controller','IntelAhci','--portcount','1')
    for stage in ('first','cold'):
        boot(stage)
        if stage=='first':
            command('echo 10.0.2.2 demo.host alias.host > /etc/hosts','shell redirect committed: status=0')
        assert '10.0.2.2 demo.host alias.host' in command('run /bin/cat.elf /etc/hosts','exit=0 reap=ok')
        for host in ('demo.host','alias.host','10.0.2.2'):
            s=command('run /bin/http-get.elf '+host+' 8000 /hello.txt','exit=0 reap=ok')
            assert 'http-get status: HTTP/1.0 200 OK' in s and 'hosts stub body verified' in s
            s=command('run /bin/linux-hosts-http.elf '+host+' 8000','exit=0 reap=ok')
            assert 'linux hosts HTTP status=200 received verified' in s and 'hosts stub body verified' in s
        for tool in ('http-get.elf','linux-hosts-http.elf'):
            s=command('run /bin/'+tool+' no-record.invalid 8000','exit=2 reap=ok')
            assert 'resolve: name not found (hosts only)' in s
        stop(stage)
    result=f'hosts {a.backend}: PASS (2 cold boots, no ISO, native/Linux hostname+alias+literal HTTP body, missing=exit2, persistent hosts)\n'
    (lab/'hosts-result.log').write_text(result);print(result,end='')
finally:
    if monitor:monitor.close()
    if a.backend=='qemu' and proc and proc.poll() is None:
        proc.terminate();proc.wait(timeout=5)
    if a.backend=='vbox' and created:
        if running:
            subprocess.run(['VBoxManage','controlvm',vm,'poweroff'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        # Detach first so unregister cannot delete retained demonstration media.
        detached=subprocess.run(['VBoxManage','storageattach',vm,'--storagectl','SATA','--port','0','--device','0','--type','hdd','--medium','none'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        args=['VBoxManage','unregistervm',vm]+(['--delete'] if detached.returncode==0 else [])
        subprocess.run(args,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
