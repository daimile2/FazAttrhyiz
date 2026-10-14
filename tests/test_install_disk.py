#!/usr/bin/env python3
"""Installer preflight: rejection must leave both bytes and inode unchanged."""
import fcntl
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('installer', REPO/'scripts/install-disk.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while data := f.read(1024*1024):
            h.update(data)
    return h.hexdigest()


class InstallSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='capmix-install-test-')
        self.root = Path(self.temp.name)
        self.image = self.root/'disk.img'
        with self.image.open('wb') as f:
            f.truncate(installer.MIN_BYTES)

    def tearDown(self):
        self.temp.cleanup()

    def call(self, path=None):
        installer.install(path or self.image, installer.MIN_BYTES,
                          REPO/'capmix.elf', REPO/'boot/limine-disk.conf', REPO/'third_party/limine')

    def unchanged(self, action, exception):
        before, inode = digest(self.image), self.image.stat().st_ino
        with self.assertRaises(exception):
            action()
        self.assertEqual(digest(self.image), before)
        self.assertEqual(self.image.stat().st_ino, inode)
        self.assertFalse(list(self.root.glob('.capmix-install-*')))

    def test_nonempty_last_byte_refused(self):
        with self.image.open('r+b') as f:
            f.seek(-1, os.SEEK_END)
            f.write(b'X')
        self.unchanged(self.call, ValueError)

    def test_symlink_and_hardlink_refused(self):
        link = self.root/'link.img'
        link.symlink_to(self.image)
        self.unchanged(lambda: self.call(link), ValueError)
        link.unlink()
        os.link(self.image, link)
        self.unchanged(self.call, ValueError)

    def test_small_image_refused(self):
        with self.image.open('r+b') as f:
            f.truncate(64*1024*1024)
        self.unchanged(self.call, ValueError)

    def test_locked_image_refused(self):
        with self.image.open('r+b') as f:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.unchanged(self.call, OSError)

    def test_tool_failure_does_not_publish(self):
        with patch.object(installer, 'run', side_effect=subprocess.CalledProcessError(1, 'injected')):
            self.unchanged(self.call, subprocess.CalledProcessError)

    def test_missing_target_not_created_on_failure(self):
        self.image.unlink()
        with patch.object(installer, 'run', side_effect=subprocess.CalledProcessError(1, 'injected')):
            with self.assertRaises(subprocess.CalledProcessError):
                self.call()
        self.assertFalse(self.image.exists())


if __name__ == '__main__':
    unittest.main()
