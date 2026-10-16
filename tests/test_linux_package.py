#!/usr/bin/env python3
"""Host regression checks for offline package configuration and refusal policy."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('linux_package',Path(__file__).resolve().parents[1]/'scripts/linux_package.py')
pkg=importlib.util.module_from_spec(spec);spec.loader.exec_module(pkg)
BASE='timeout: 0\n/CapMix\n    protocol: limine\n    kernel_path: boot():/boot/capmix.elf\n    cmdline: capmix.mode=product capmix.installed=1 capmix.console=serial\n'
class Package(unittest.TestCase):
    def test_profile(self):
        on=pkg.configure(BASE,True)
        self.assertIn('capmix.linux=1 capmix.linux_interp=0',on)
        self.assertIn('capmix.console=serial',on)
        self.assertEqual(pkg.configure(on,True),on)
        off=pkg.configure(on,False)
        self.assertNotIn('capmix.linux=1',off)
        self.assertIn('capmix.linux=0',off)
        self.assertEqual(pkg.configure(off,False),off)
    def test_refuse_ambiguous(self):
        for text in (BASE+BASE,BASE+'/Other\n',BASE.replace('/boot/capmix.elf','/boot/other.elf'),BASE.replace('product','selftest'),BASE.replace('capmix.installed=1',''),BASE+'\0',BASE.replace('capmix.installed=1','capmix.installed=1 capmix.installed=0'),BASE.replace('serial','x'*256)):
            with self.assertRaises(ValueError):pkg.configure(text,True)
    def test_purge_explicit(self):
        with self.assertRaises(ValueError):pkg.manage('/not/accessed','enable',True)
        with self.assertRaises(ValueError):pkg.manage('/not/accessed','typo')
    def test_unknown_unchanged_and_links(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'unknown.img';p.write_bytes(b'unknown data')
            before=hashlib.sha256(p.read_bytes()).digest()
            with self.assertRaises(ValueError):pkg.manage(p,'disable',True)
            self.assertEqual(hashlib.sha256(p.read_bytes()).digest(),before)
            link=Path(tmp)/'link';link.symlink_to(p)
            with self.assertRaises(ValueError):pkg.manage(link,'enable')
    def test_support(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'kernel';p.write_bytes(b'CapMix.LinuxPersona.v1:compiled=0\0')
            with self.assertRaises(ValueError):pkg.require_support(p)
if __name__=='__main__':unittest.main()
