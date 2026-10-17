#!/usr/bin/env python3
"""Run native no-ISO network/persistence smoke tests on a DISPOSABLE installed disk.
Requires host TCP/UDP echo peers and HTTP :8000 /hello.txt. Writes /irq-net*.txt.
"""
from pathlib import Path
import subprocess,socket,time,os,signal,re,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--disk',required=True,type=Path,help='disposable installed raw image; never an existing user disk')
parser.add_argument('--prefix',default='/tmp/capmix-e1000-product')
parser.add_argument('--httpd-port',type=int,default=18083)
parser.add_argument('--micropython',action='store_true',help='also verify optional interpreter on a Linux-enabled installed disk')
args=parser.parse_args()
assert args.disk.is_file(), 'install a fresh disposable raw image first'


def boot(cold=False):
 tag='second' if cold else 'first';log=Path(args.prefix+'-'+tag+'.log');log.write_text('')
 monitor=args.prefix+'-monitor.sock'
 p=subprocess.Popen(['qemu-system-x86_64','-enable-kvm','-m','512','-cpu','host','-smp','4','-drive','file='+str(args.disk)+',if=virtio,format=raw','-netdev','user,id=n0,hostfwd=tcp:127.0.0.1:'+str(args.httpd_port)+'-:8080','-device','e1000,netdev=n0','-serial','file:'+str(log),'-display','none','-no-reboot','-monitor','unix:'+monitor+',server=on,wait=off'],stdout=open(args.prefix+'-'+tag+'.stderr','w'),stderr=subprocess.STDOUT,start_new_session=True)
 def wait(marker,offset=0,timeout=15):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   s=log.read_text(errors='replace')
   if marker in s[offset:]:return s
   assert '[FAULT]' not in s and p.poll() is None, 'guest died: '+marker
   time.sleep(.1)
  raise AssertionError('timeout '+marker)
 try:
  wait('capmix> ')
  con=socket.socket(socket.AF_UNIX);con.connect(monitor);con.settimeout(.001)
  keys={' ':'spc','&':'shift-7','/':'slash','.':'dot','-':'minus','>':'shift-dot','<':'shift-comma','|':'shift-backslash','\n':'ret'}
  def send(text):
   for c in text+'\n':
    con.sendall(('sendkey '+keys.get(c,c)+' 8\n').encode());time.sleep(.035)
    try:con.recv(65536)
    except TimeoutError:pass
  def run(text,marker='exit=0 reap=ok'):
   off=len(log.read_text(errors='replace'));send(text);return wait(marker,off)
  if args.micropython:
   s=run('run /bin/micropython.elf /data/linux/micropython-demo.py')
   assert 'micropython golden\n42\n' in s
   run('run /bin/linux-hello.elf')
   run('run /bin/busybox-sh.elf sh /data/linux/sh-smoke.sh')
   if not cold:
    run('run /bin/micropython.elf /data/linux/micropython-demo.py > /mp-result.txt','shell redirect committed: status=0')
   s=run('run /bin/cat.elf /mp-result.txt');assert 'micropython golden\n42\n' in s
  if not cold:
   run('run /bin/hello.elf')
   run('echo e1000fix-persist > /irq-net.txt','shell redirect committed: status=0')
   run('echo second-line >> /irq-net.txt','shell redirect committed: status=0')
   run('run /bin/cat.elf < /irq-net.txt > /irq-net-copy.txt','shell redirect committed: status=0')
  s=run('run /bin/cat.elf /irq-net-copy.txt');assert 'e1000fix-persist\n' in s and 'second-line\n' in s
  if not cold:
   run('run /bin/http-get.elf 10.0.2.2 8000 /hello.txt')
   assert 'http-get status: HTTP/1.0 200 OK' in log.read_text()
   run('run -i /bin/nc.elf 10.0.2.2 9998','nc connected:')
   run('e1000fix echo probe','nc rx bytes=')
   run('/quit')
   run('run /bin/httpd.elf 8080 2 &','httpd listening')
   subprocess.run(['python3','tests/check_httpd.py','--port',str(args.httpd_port)],check=True,stdout=open(args.prefix+'-httpd-client.log','w'))
   wait('httpd handled=2')
   off=len(log.read_text());s=run('run /bin/spin.elf &','shell background start: pid=')
   time.sleep(.3);s=log.read_text();m=re.search(r'exec spawn: pid=(\d+) path=/bin/spin.elf',s[off:]);assert m
   run('kill '+m[1],'exit=137 reap=ok')
  send('poweroff');p.wait(timeout=15);assert p.returncode==0
  s=log.read_text();assert 'root=disk' in s and 'ACPI' in s
  if args.micropython:print('micropython product no-ISO '+tag+': PASS (script/exit/native redirect/persist/Linux-L0/hush)',flush=True)
  print('product no-ISO '+tag+': PASS (native redirect/persist'+('/nc/http-get/httpd/kill' if not cold else '/cold-read')+'/poweroff)',flush=True)
 finally:
  if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait()
  if 'con' in locals():con.close()
boot();boot(True)
