#!/usr/bin/env python3
"""Offline Linux persona package profile; next boot applies it. No kernel hot unload."""
import argparse
import fcntl
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent
ESP_LBA, ESP_SECT = 18432, 131072
ROOT_LBA, ROOT_SECT = 2048, 16384


def configure(text, enabled):
    if '\0' in text:
        raise ValueError('NUL in boot configuration')
    lines = text.splitlines(keepends=True)
    entries=[line for line in lines if re.match(r'^\s*/',line)]
    protocols=[line.split(':',1)[1].strip() for line in lines if re.match(r'^\s*protocol\s*:',line)]
    kernels=[line.split(':',1)[1].strip() for line in lines if re.match(r'^\s*kernel_path\s*:',line)]
    if len(entries)!=1 or protocols!=['limine'] or kernels!=['boot():/boot/capmix.elf']:
        raise ValueError('only a single Limine entry using boot():/boot/capmix.elf is supported')
    indices = [i for i, line in enumerate(lines) if re.match(r'^\s*cmdline\s*:', line)]
    if len(indices) != 1:
        raise ValueError('expected exactly one installed product cmdline')
    i = indices[0]
    prefix, value = lines[i].split(':', 1)
    words = value.split()
    if (words.count('capmix.mode=product') != 1 or
            any(w.startswith('capmix.mode=') and w != 'capmix.mode=product' for w in words) or
            words.count('capmix.installed=1') != 1 or
            any(w.startswith('capmix.installed=') and w != 'capmix.installed=1' for w in words)):
        raise ValueError('only installed product configuration accepted')
    words = [w for w in words if not w.startswith(('capmix.linux=', 'capmix.linux_interp='))]
    words += [f'capmix.linux={int(enabled)}', 'capmix.linux_interp=0']
    value = ' '.join(words)
    if len(value.encode()) > 255:
        raise ValueError('cmdline exceeds kernel snapshot capacity')
    lines[i] = prefix + ': ' + value + '\n'
    return ''.join(lines)


def layout(fd, size):
    b = os.pread(fd, 512, 0)
    if size < 128*1024*1024 or size % 512 or len(b) != 512 or b[440:446] != struct.pack('<I', 0x43504D58)+b'\0\0' or b[510:] != b'\x55\xaa':
        raise ValueError('not a supported installed CapMix disk')
    for i, (active, kind, start, count) in enumerate(((0, 0xda, ROOT_LBA, ROOT_SECT), (0x80, 0xef, ESP_LBA, ESP_SECT))):
        p = b[446+i*16:462+i*16]
        if p[0] != active or p[4] != kind or struct.unpack_from('<II', p, 8) != (start, count):
            raise ValueError('unsupported partition layout')
    if any(b[478:510]) or (ESP_LBA+ESP_SECT)*512 > size:
        raise ValueError('invalid extra partitions or capacity')


def require_support(kernel):
    data=Path(kernel).read_bytes()
    if b'CapMix.LinuxPersona.v1:compiled=1\0' not in data or b'CapMix.LinuxPersona.v1:compiled=0\0' in data:
        raise ValueError('kernel lacks Linux package support marker (rebuild with LINUX_ABI=1 and update ESP kernel first)')


def identity(st):
    return st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_nlink


def manage(image, action, purge=False):
    if action not in ('enable', 'disable', 'status'):
        raise ValueError('unknown package action')
    if purge and action != 'disable':
        raise ValueError('--purge-data is only valid with disable')
    image = Path(os.path.abspath(image))
    first = image.lstat()
    if not stat.S_ISREG(first.st_mode) or first.st_nlink != 1:
        raise ValueError('only single-link regular RAW images; no devices/symlinks/hardlinks')
    fd = os.open(image, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        original = os.fstat(fd)
        if identity(first) != identity(original):
            raise ValueError('image identity changed')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        layout(fd, original.st_size)
        with tempfile.TemporaryDirectory(prefix='.capmix-linux-', dir=image.parent) as temp:
            temp = Path(temp);stage = temp/'disk.img'
            with stage.open('wb') as out:
                for offset in range(0, original.st_size, 1024*1024):
                    n = min(1024*1024, original.st_size-offset)
                    b = os.pread(fd, n, offset)
                    if len(b) != n:
                        raise ValueError('image truncated while staging')
                    out.write(b)
                out.flush();os.fsync(out.fileno())
            os.chmod(stage, stat.S_IMODE(original.st_mode))
            fat = f'{stage}@@{ESP_LBA*512}'
            conf = temp/'limine.conf'
            subprocess.run(['mcopy', '-i', fat, '::/boot/limine/limine.conf', str(conf)], check=True, stdin=subprocess.DEVNULL)
            text = conf.read_text()
            if action == 'enable':
                kernel=temp/'capmix.elf'
                subprocess.run(['mcopy','-i',fat,'::/boot/capmix.elf',str(kernel)],check=True,stdin=subprocess.DEVNULL)
                require_support(kernel)
            # Validate even status; refuse ambiguous/multi-entry configurations.
            changed = configure(text, action == 'enable')
            blank = True
            with stage.open('rb') as disk:
                disk.seek(ROOT_LBA*512)
                for _ in range(ROOT_SECT//2048):
                    if any(disk.read(2048*512)):
                        blank = False;break
            if not blank:
                helper = REPO/'build/linux-package'
                if not helper.is_file():
                    raise ValueError('build helper first: make build/linux-package')
                op = 'purge' if purge else 'prepare' if action == 'enable' else 'status'
                subprocess.run([str(helper), str(stage), op], check=True)
            else:
                print('linux package data: root uninitialized; no formatting; data created only when booting with Linux enabled')
            if action == 'status':
                tokens = next(line.split(':', 1)[1].split() for line in text.splitlines() if re.match(r'^\s*cmdline\s*:', line))
                on = 'capmix.linux=1' in tokens and 'capmix.linux=0' not in tokens
                print(f'linux package configured={"enabled" if on else "disabled"} (not live status; kernel support checked at boot)')
                return
            conf.write_text(changed)
            subprocess.run(['mcopy', '-o', '-i', fat, str(conf), '::/boot/limine/limine.conf'], check=True, stdin=subprocess.DEVNULL)
            verify = temp/'verify.conf'
            subprocess.run(['mcopy', '-i', fat, '::/boot/limine/limine.conf', str(verify)], check=True, stdin=subprocess.DEVNULL)
            if verify.read_text() != changed:
                raise ValueError('configuration readback mismatch')
            with stage.open('rb') as disk:
                os.fsync(disk.fileno())
            if identity(image.lstat()) != identity(original):
                raise ValueError('original image changed; refusing publication')
            os.replace(stage, image)
            directory = os.open(image.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        print(f'linux package {action} published: next boot; data={"purged /data/linux" if purge else "retained"}; native /bin retained; no hot unload')
    finally:
        os.close(fd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['enable', 'disable', 'status', 'install', 'uninstall'])
    p.add_argument('image', help='offline installed RAW image; stop VM first')
    p.add_argument('--purge-data', action='store_true', help='disable AND permanently remove only /data/linux from the live COW root')
    a = p.parse_args()
    try:
        manage(a.image, {'install':'enable','uninstall':'disable'}.get(a.action,a.action), a.purge_data)
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        p.exit(1, f'linux package refused: {e}\n')


if __name__ == '__main__':
    main()
