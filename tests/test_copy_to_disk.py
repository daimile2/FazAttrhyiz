#!/usr/bin/env python3
"""Offline import refusal tests; optional initialized-disk integration fixture."""
import fcntl
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import importlib.util

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'build/external-hello.elf'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as file:
        for data in iter(lambda: file.read(1024 * 1024), b''):
            h.update(data)
    return h.digest()


class CopySafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.disk = self.base / 'disk.img'
        with self.disk.open('wb') as file:
            file.truncate(128 * 1024 * 1024)

    def tearDown(self):
        self.temp.cleanup()

    def refused(self, image=None, destination='/bin/external.elf'):
        before = digest(self.disk)
        result = subprocess.run(['python3', str(ROOT / 'scripts/copy-to-disk.py'),
                                 str(image or self.disk), str(SOURCE), destination],
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(before, digest(self.disk))

    def test_unknown_or_blank_is_never_formatted(self):
        self.refused()

    def test_invalid_path(self):
        for path in ('relative', '/bin/../shell.elf', '/bin//x', '/bin/x/'):
            self.refused(destination=path)

    def test_symlink(self):
        link = self.base / 'link.img'
        link.symlink_to(self.disk)
        self.refused(link)

    def test_hardlink(self):
        link = self.base / 'hard.img'
        os.link(self.disk, link)
        self.refused(link)

    def test_locked_image(self):
        with self.disk.open('r+b') as file:
            fcntl.lockf(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.refused()

    def test_posix_lock_survives_staging_copy(self):
        spec = importlib.util.spec_from_file_location('disk_copy_test', ROOT / 'scripts/copy-to-disk.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        real_run = subprocess.run
        before = digest(self.disk)
        code = ("import fcntl,sys\n"
                "with open(sys.argv[1],'r+b') as f:\n"
                " try: fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)\n"
                " except BlockingIOError: sys.exit(0)\n"
                " sys.exit(1)\n")
        def during_helper(command, **kwargs):
            result = real_run(['python3', '-c', code, str(self.disk)], capture_output=True)
            self.assertEqual(result.returncode, 0, 'POSIX lock was dropped before helper invocation')
            raise subprocess.CalledProcessError(1, command)
        with patch.object(module.subprocess, 'run', side_effect=during_helper):
            with self.assertRaises(subprocess.CalledProcessError):
                module.copy_to_disk(self.disk, SOURCE, '/bin/new.elf')
        self.assertEqual(before, digest(self.disk))

    def test_device(self):
        self.refused(Path('/dev/null'))

    @unittest.skipUnless(os.environ.get('CAPMIX_TEST_DISK'), 'no initialized offline disk fixture')
    def test_existing_file_and_nonfs_preservation(self):
        fixture = Path(os.environ['CAPMIX_TEST_DISK'])
        import shutil
        shutil.copyfile(fixture, self.disk)
        with self.disk.open('rb') as file:
            original = file.read()
        destination = '/bin/sdk-import-test.elf'
        command = ['python3', str(ROOT / 'scripts/copy-to-disk.py'), str(self.disk), str(SOURCE), destination]
        subprocess.run(command, check=True, capture_output=True)
        with self.disk.open('rb') as file:
            updated = file.read()
        fs_start = (2048 + 8256) * 512
        fs_end = (2048 + 16384) * 512
        self.assertEqual(original[:fs_start], updated[:fs_start])
        self.assertEqual(original[fs_end:], updated[fs_end:])
        self.refused(destination=destination)  # no implicit overwrite


if __name__ == '__main__':
    unittest.main()
