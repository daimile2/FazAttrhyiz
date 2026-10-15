#!/usr/bin/env python3
"""Offline, staged import into an initialized installed CapMix COW root."""
import argparse
import fcntl
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import tempfile

REPO = Path(__file__).resolve().parent.parent


def copy_to_disk(image, source, destination):
    image = Path(os.path.abspath(image))
    source = Path(source)
    parts = destination.split('/')
    if (not destination.startswith('/') or len(destination.encode()) >= 128 or
            any(p in ('.', '..', '') for p in parts[1:]) or
            str(PurePosixPath(destination)) != destination):
        raise ValueError('destination must be a canonical absolute file path (<128 bytes)')
    preliminary = image.lstat()
    if not stat.S_ISREG(preliminary.st_mode) or preliminary.st_nlink != 1:
        raise ValueError('image must be a single-link regular file; devices/symlinks refused')
    if source.resolve() == image.resolve():
        raise ValueError('source cannot be the disk image')
    helper = REPO / 'build/cow-put'
    if not helper.is_file():
        raise ValueError('build helper first: make build/cow-put')
    fd = os.open(image, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        original = os.fstat(fd)
        if ((original.st_dev, original.st_ino) != (preliminary.st_dev, preliminary.st_ino) or
                not stat.S_ISREG(original.st_mode) or original.st_nlink != 1):
            raise ValueError('image identity changed while opening')
        # QEMU uses POSIX/OFD region locks; flock alone is insufficient.
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with tempfile.TemporaryDirectory(prefix='.capmix-copy-', dir=image.parent) as temp:
            stage = Path(temp) / 'disk.img'
            # Read from the locked descriptor, never a newly resolved path.
            # Closing even a dup of fd would release this process's POSIX
            # locks on the original inode. Keep exactly one descriptor open.
            with stage.open('wb') as dst:
                copied = 0
                while copied < original.st_size:
                    data = os.pread(fd, min(1024 * 1024, original.st_size - copied), copied)
                    if not data:
                        raise ValueError('original image truncated during staging')
                    dst.write(data)
                    copied += len(data)
                dst.flush()
                os.fsync(dst.fileno())
            os.chmod(stage, stat.S_IMODE(original.st_mode))
            subprocess.run([str(helper), str(stage), str(source.resolve()), destination], check=True)
            with stage.open('rb') as staged:
                os.fsync(staged.fileno())
            current = image.lstat()
            identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns, s.st_nlink)
            if identity(current) != identity(original):
                raise ValueError('original image changed; staged import discarded')
            os.replace(stage, image)
            directory = os.open(image.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        print(f'cow import published: {destination}; boot/ESP/raw/swap preserved')
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', help='offline RAW installed image; first-boot COW already initialized')
    parser.add_argument('source', help='host file (<=1MiB)')
    parser.add_argument('destination', help='new absolute COW path; parent directory must exist')
    args = parser.parse_args()
    try:
        copy_to_disk(args.image, args.source, args.destination)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'cow import refused: {error}\n')


if __name__ == '__main__':
    main()
