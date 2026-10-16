#!/usr/bin/env python3
"""Create a BIOS/UEFI CapMix disk image. No raw devices, force, or in-place upgrade."""
import argparse
import fcntl
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

DISK_ID = 0x43504D58
ROOT_LBA, ROOT_SECT = 2048, 16384
ESP_LBA, ESP_SECT = 18432, 131072
MIN_BYTES = 128 * 1024 * 1024
REPO = Path(__file__).resolve().parent.parent


def run(argv):
    subprocess.run([str(a) for a in argv], check=True, stdin=subprocess.DEVNULL)


def require_blank(fd, size):
    os.lseek(fd, 0, os.SEEK_SET)
    remaining = size
    while remaining:
        data = os.read(fd, min(remaining, 1024 * 1024))
        if not data or data.count(0) != len(data):
            raise ValueError('nonempty/unknown image refused; no formatting or overwrite')
        remaining -= len(data)


def install(target, size, kernel, config, limine_dir):
    target = Path(os.path.abspath(target))
    if size < MIN_BYTES or size % 512 or size // 512 > 0xFFFFFFFF:
        raise ValueError('image must be >=128MiB, sector-aligned, and within MBR capacity')
    for cmd in ('mkfs.fat', 'mmd', 'mcopy', 'mdir'):
        if not shutil.which(cmd):
            raise ValueError(f'missing tool: {cmd}')
    inputs = [kernel, config, limine_dir / 'BOOTX64.EFI', limine_dir / 'limine-bios.sys', limine_dir / 'limine']
    for source in inputs:
        if not source.is_file():
            raise ValueError(f'missing input: {source}')
    if not os.access(inputs[-1], os.X_OK):
        raise ValueError('Limine host installer is not executable')
    original_fd = None
    original_stat = None
    stage_name = None
    try:
        try:
            preliminary = os.lstat(target)
            if not stat.S_ISREG(preliminary.st_mode) or preliminary.st_nlink != 1:
                raise ValueError("device/symlink/hardlink refused before opening target")
            original_fd = os.open(target, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError:
            if target.is_symlink():
                raise ValueError('symlink target refused')
        if original_fd is not None:
            original_stat = os.fstat(original_fd)
            if not stat.S_ISREG(original_stat.st_mode) or original_stat.st_nlink != 1:
                raise ValueError('only a single-link regular file is accepted; device/symlink refused')
            fcntl.flock(original_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.lockf(original_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            size = original_stat.st_size
            if size < MIN_BYTES or size % 512 or size // 512 > 0xFFFFFFFF:
                raise ValueError('existing zero image has invalid capacity (minimum 128MiB)')
            require_blank(original_fd, size)
        with tempfile.TemporaryDirectory(prefix='capmix-fat-') as tmp:
            fat = Path(tmp) / 'esp.img'
            with fat.open('wb') as f:
                f.truncate(ESP_SECT * 512)
            run(['mkfs.fat', '-F', '32', '-n', 'CAPMIXBOOT', fat])
            run(['mmd', '-i', fat, '::/EFI', '::/EFI/BOOT', '::/boot', '::/boot/limine'])
            for src, dest in [(inputs[2], '::/EFI/BOOT/BOOTX64.EFI'),
                              (inputs[3], '::/boot/limine/limine-bios.sys'),
                              (kernel, '::/boot/capmix.elf'),
                              (config, '::/boot/limine/limine.conf')]:
                run(['mcopy', '-i', fat, src, dest])
            run(['mdir', '-i', fat, '::/boot'])
            stage_fd, stage_name = tempfile.mkstemp(prefix='.capmix-install-', dir=target.parent)
            with os.fdopen(stage_fd, 'w+b') as disk:
                disk.truncate(size)
                mbr = bytearray(512)
                struct.pack_into('<I', mbr, 440, DISK_ID)
                for i, (status, kind, start, count) in enumerate(
                        [(0, 0xDA, ROOT_LBA, ROOT_SECT), (0x80, 0xEF, ESP_LBA, ESP_SECT)]):
                    struct.pack_into('<B3sB3sII', mbr, 446+i*16,
                                     status, b'\xfe\xff\xff', kind, b'\xfe\xff\xff', start, count)
                mbr[510:512] = b'\x55\xaa'
                disk.write(mbr)
                disk.seek(ESP_LBA * 512)
                with fat.open('rb') as src:
                    shutil.copyfileobj(src, disk, 1024*1024)
                disk.flush()
                os.fsync(disk.fileno())
            run([inputs[4], 'bios-install', stage_name])
            # Verify the host bootloader did not overwrite the marker/partitions or COW window.
            with open(stage_name, 'rb') as disk:
                disk.seek(440)
                if disk.read(72) != mbr[440:512]:
                    raise ValueError('Limine changed install marker/partition table')
                disk.seek(ROOT_LBA*512)
                root = disk.read(ROOT_SECT*512)
                if len(root) != ROOT_SECT*512 or root.count(0) != len(root):
                    raise ValueError('bootloader touched root partition; refusing publication')
            if original_fd is None:
                os.link(stage_name, target)  # publish without replacing a concurrently created file
                os.unlink(stage_name)
            else:
                current = os.stat(target, follow_symlinks=False)
                if (current.st_dev, current.st_ino, current.st_nlink) != (original_stat.st_dev, original_stat.st_ino, 1):
                    raise ValueError('target changed during preparation')
                require_blank(original_fd, size)
                os.replace(stage_name, target)
            stage_name = None
            with target.open('rb') as disk:
                os.fsync(disk.fileno())
            parent_fd = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(parent_fd)
            finally:
                os.close(parent_fd)
        print(f'install boot staged: {target} size={size} root_lba={ROOT_LBA} esp_lba={ESP_LBA}')
        print('First boot from this disk (no ISO) initializes the blank COW root and fsyncs system ELF files.')
        print('Second cold boot mounts that persisted root; existing data images and installed images are refused by this tool.')
    finally:
        if stage_name is not None:
            os.unlink(stage_name)
        if original_fd is not None:
            os.close(original_fd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('image', help='new path or existing all-zero regular disk image')
    p.add_argument('--size-mib', type=int, default=128, help='new file capacity (default 128MiB)')
    p.add_argument('--kernel', type=Path, default=REPO / 'capmix.elf')
    p.add_argument('--linux', action='store_true', help='request optional Linux persona at first boot; L4a stays disabled')
    p.add_argument('--config', type=Path, default=REPO / 'boot/limine-disk.conf')
    p.add_argument('--limine-dir', type=Path, default=REPO / 'third_party/limine')
    args = p.parse_args()
    try:
        if args.linux:
            from linux_package import configure, require_support
            require_support(args.kernel)
            with tempfile.TemporaryDirectory(prefix='capmix-linux-config-') as temp:
                config = Path(temp)/'limine.conf'
                config.write_text(configure(args.config.read_text(), True))
                install(args.image, args.size_mib*1024*1024, args.kernel, config, args.limine_dir)
        else:
            install(args.image, args.size_mib*1024*1024, args.kernel, args.config, args.limine_dir)
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        print(f'install refused/failed: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
