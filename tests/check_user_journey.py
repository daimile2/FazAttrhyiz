#!/usr/bin/env python3
"""Drive real product PS/2 shell: Linux off -> offline enable -> demo/httpd -> disable.
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
p.add_argument('--scenario',choices=['journey','console-case'],default='journey')
p.add_argument('--copy-safety',action='store_true',help='verify binary cat copies across cold boots')
a=p.parse_args();lab=a.lab.resolve();disk=lab/'system.img'
assert (lab/'.capmix-user-journey').is_file() and disk.is_file(), 'prepare a disposable lab first'
assert 1024<=a.port<=65535
assert not list(lab.glob('journey-*.log')), 'logs already exist; choose a new prepared lab'
# Do not replace someone's VM, medium or image.
vm='CapMix-Journey-'+str(time.time_ns())
sequence=1
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
    global proc,monitor,running,log,medium,sequence
    log=lab/('journey-'+stage+'.log')
    if a.backend=='qemu':
        sock=lab/'monitor.sock'
        if sock.exists():sock.unlink()
        with (lab/('journey-'+stage+'-host.log')).open('w') as output:
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
    sequence=1
    wait('[SHELL v=1 event=ready seq=1 status=0]')
    wait('fazattrhyiz> ')
    assert 'root=disk' in text() and 'boot mode: product' in text()
    if a.backend=='qemu':
        monitor=socket.socket(socket.AF_UNIX);monitor.connect(str(sock));monitor.settimeout(.002)

def send(command,enter=True):
    if a.backend=='qemu':
        keys={' ':'spc','/':'slash','.':'dot','-':'minus','>':'shift-dot','<':'shift-comma','&':'shift-7'}
        for c in command+('\n' if enter else ''):
            key='ret' if c=='\n' else ('shift-'+c.lower() if c.isupper() else keys.get(c,c))
            monitor.sendall(('sendkey '+key+' 8\n').encode());time.sleep(.035)
            try:monitor.recv(65536)
            except TimeoutError:pass
    else:
        codes={c:int(h,16) for c,h in zip('1234567890qwertyuiopasdfghjklzxcvbnm',
            '02 03 04 05 06 07 08 09 0a 0b 10 11 12 13 14 15 16 17 18 19 1e 1f 20 21 22 23 24 25 26 2c 2d 2e 2f 30 31 32'.split())}
        codes.update({' ':0x39,'/':0x35,'.':0x34,'-':0x0c,'\n':0x1c})
        for c in command+('\n' if enter else ''):
            shift=c in '<>&' or c.isupper();key=0x34 if c=='>' else 0x33 if c=='<' else 0x08 if c=='&' else codes[c.lower()]
            seq=([0x2a] if shift else [])+[key,key|0x80]+([0xaa] if shift else [])
            call('VBoxManage','controlvm',vm,'keyboardputscancode',*[f'{n:02x}' for n in seq])
            time.sleep(.025)

def command(cmd,marker,status=0,input_fn=None):
    global sequence
    offset=len(text());input_fn() if input_fn else send(cmd)
    begin=f'[SHELL v=1 event=begin seq={sequence} status=0]'
    s=wait(begin,offset)
    # Exclude every echoed input byte, even if it contains the expected result.
    start=offset+s.index(begin)+len(begin)
    wait(f'[SHELL v=1 event=end seq={sequence} status={status}]',start)
    sequence+=1
    wait(f'[SHELL v=1 event=ready seq={sequence} status=0]',start)
    result=wait(marker,start)
    print(f'shell protocol: seq={sequence-1} status={status} boundaries=verified',flush=True)
    return result

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
            with (lab/'journey-control-host.log').open('a') as f:
                f.write('shutdown state query retry: '+last+'\n')
        time.sleep(.2)
    raise AssertionError('VBox poweroff timeout: '+last[-1500:])

def stop(stage):
    global running,monitor
    offset=len(text());send('poweroff')
    wait(f'[SHELL v=1 event=begin seq={sequence} status=0]',offset)
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
    print(f'journey {a.backend} {stage}: poweroff=ok no-ISO root=disk',flush=True)

try:
    status=call('python3',ROOT/'scripts/linux_package.py','status',disk)
    assert 'configured=disabled' in status, 'journey starts with default Linux OFF'
    (lab/'journey-profile-before.log').write_text(status)
    if a.backend=='vbox':
        call('VBoxManage','createvm','--name',vm,'--platform-architecture','x86','--ostype','Other_64','--basefolder',lab/'vm','--register');created=True
        call('VBoxManage','modifyvm',vm,'--memory','512','--cpus','4','--ioapic','on','--acpi','on','--chipset','piix3','--firmware','efi64',
             '--graphicscontroller','vmsvga','--vram','16','--keyboard','ps2','--mouse','ps2','--nic1','nat','--nic-type1','82540EM','--cable-connected1','on',
             '--boot1','disk','--boot2','none','--boot3','none','--boot4','none','--uart1','0x3f8','4',
             '--natpf1',f'httpd,tcp,127.0.0.1,{a.port},10.0.2.15,8080')
        call('VBoxManage','storagectl',vm,'--name','SATA','--add','sata','--controller','IntelAhci','--portcount','1')
    if a.scenario=='console-case':
        for stage in ('case-first','case-second'):
            boot(stage)
            assert 'FazAttrhyiz' in text() and '/sbin/init.faz' in text()
            command('hello','exit=0 reap=ok')
            command('cat /example.txt','persistent root ready')
            command('run /bin/hello.elf','exit=0 reap=ok')
            command('httpd 8080 1 &','httpd listening')
            reply=call('python3',ROOT/'tests/check_httpd.py','--port',a.port,'--requests','1')
            (lab/(stage+'-http-client.log')).write_text(reply)
            wait('httpd handled=1')
            assert 'font=ASCII-mixed-case' in text(), 'framebuffer required for this visual test'
            if stage=='case-first':
                command('echo lower-file > /case.txt','shell redirect committed: status=0')
                command('echo upper-file > /Case.txt','shell redirect committed: status=0')
            command('cat /case.txt','lower-file')
            command('cat /Case.txt','upper-file')
            command('cat /CASE.txt','shell error=-12',status=-12)
            def caps_key():
                if a.backend=='qemu':
                    monitor.sendall(b'sendkey caps_lock 8\n');time.sleep(.1)
                    try:monitor.recv(65536)
                    except TimeoutError:pass
                else:
                    call('VBoxManage','controlvm',vm,'keyboardputscancode','3a','ba')
                    time.sleep(.1)
            def modifiers():
                send('echo ',enter=False)
                caps_key()
                send('abc',enter=False)  # Caps -> uppercase
                send('A',enter=False)    # Shift + Caps -> lowercase
                caps_key()
                send('def')             # Caps off -> lowercase
            command('modifier PS/2 sequence','ABCadef',input_fn=modifiers)
            command('echo lowercase abcdefghijklmnopqrstuvwxyz','lowercase abcdefghijklmnopqrstuvwxyz')
            command('echo UPPERCASE ABCDEFGHIJKLMNOPQRSTUVWXYZ','UPPERCASE ABCDEFGHIJKLMNOPQRSTUVWXYZ')
            command('echo /bin /Bin /BIN g j p q y','/bin /Bin /BIN g j p q y')
            png=lab/(stage+'.png')
            if a.backend=='qemu':
                from PIL import Image
                ppm=lab/(stage+'.ppm')
                monitor.sendall(('screendump '+str(ppm)+'\n').encode())
                time.sleep(1)
                Image.open(ppm).save(png)
            else:
                call('VBoxManage','controlvm',vm,'screenshotpng',png,'0')
            stop(stage)
        result=f'console case {a.backend}: PASS lowercase PS/2, mixed-case paths distinct, cold-boot persistence, framebuffer screenshots saved\n'
        (lab/'journey-result.log').write_text(result);print(result,end='')
    else:
        boot('native')
        command('help','COMMAND [ARGS]')
        if a.copy_safety:
            command('nc 10.0.2.2 9998','use run -i /bin/nc.faz HOST PORT',status=-7)
            command('cat /bin/shell.faz','terminal preview limited',status=-6)
            command('cat /bin/shell.faz > /copy-direct.faz','shell redirect committed: status=0')
            command('cat < /bin/shell.faz > /copy-stdin.faz','shell redirect committed: status=0')
            command('run /bin/cat.faz /bin/shell.faz > /copy-external.faz','shell redirect committed: status=0')
        assert 'hello from elf' in command('hello','exit=0 reap=ok')
        command('run /bin/hello.faz','exit=0 reap=ok')
        command('missing-journey-command','shell error=-12',status=-12)
        command('poweroff > /short-guard.txt','shell error=-7',status=-7)
        command('cat /short-guard.txt','shell error=-12',status=-12)
        command('hello > /short-output.txt','shell redirect committed: status=0')
        command('cat /short-output.txt','hello from elf')
        command('echo journey-native > /journey-native.txt','shell redirect committed: status=0')
        stop('native')
        if a.copy_safety:
            call('make','build/copy-verify.faz')
            blob=lab/'copy-input.bin';blob.write_bytes(bytes(range(256))*256)
            for source,dest in ((ROOT/'build/copy-verify.faz','/bin/copy-verify.faz'),(blob,'/copy-input.bin')):
                call('python3',ROOT/'scripts/copy-to-disk.py',disk,source,dest)
        (lab/'journey-enable.log').write_text(call('python3',ROOT/'scripts/linux_package.py','enable',disk))
        boot('linux')
        if a.copy_safety:
            r=command('run /bin/copy-verify.faz /bin/shell.faz /copy-direct.faz /copy-stdin.faz /copy-external.faz','exit=0 reap=ok')
            assert r.count('copy integrity verified bytes')==3
            command('cat /copy-input.bin > /blob-builtin.bin','shell redirect committed: status=0')
            command('run /bin/cat.faz /copy-input.bin > /blob-external.bin','shell redirect committed: status=0')
        assert 'journey-native' in command('cat /journey-native.txt','journey-native')
        # SMP shell status writes may interleave with direct stdout. Verify exit
        # here, then verify the complete output through the existing FILE path.
        command('micropython /data/linux/micropython-demo.py','exit=0 reap=ok')
        command('micropython /data/linux/micropython-demo.py > /journey-python.txt','shell redirect committed: status=0')
        assert 'micropython golden\n42\n' in command('cat /journey-python.txt','micropython golden')
        command('httpd 8080 2 &','httpd listening')
        # The full host payload/EOF checker proves wire traffic, not just link-up.
        result=subprocess.run(['python3',str(ROOT/'tests/check_httpd.py'),'--port',str(a.port),'--requests','2'],
            stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (lab/'journey-http-client.log').write_text(result.stdout)
        result.check_returncode()
        wait('httpd handled=2')
        command('hello','exit=0 reap=ok')
        stop('linux')
        (lab/'journey-disable.log').write_text(call('python3',ROOT/'scripts/linux_package.py','disable',disk))
        boot('disabled')
        if a.copy_safety:
            r=command('run /bin/copy-verify.faz /copy-input.bin /blob-builtin.bin /blob-external.bin','exit=0 reap=ok')
            assert r.count('copy integrity verified bytes=65536')==2
            print('binary cat copy: PASS 3 ELF copies + 2 all-byte 65536B copies, cold-boot verified',flush=True)
        assert 'journey-native' in command('cat /journey-native.txt','journey-native')
        assert 'micropython golden\n42\n' in command('cat /journey-python.txt','micropython golden')
        s=command('micropython /data/linux/micropython-demo.py','shell error=-7',status=-7)
        assert 'linux persona disabled' in s
        command('hello','exit=0 reap=ok')
        stop('disabled')
        result=f'user journey {a.backend}: PASS (3 cold boots, no ISO, native -> offline Linux -> MicroPython -> native HTTP200 -> disabled/native retained)\n'
        (lab/'journey-result.log').write_text(result);print(result,end='')

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
