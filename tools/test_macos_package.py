#!/usr/bin/env python3
"""Exercise the delivered installer in temporary RetroArch directories."""
import argparse
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = None

class MacPackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='installer-check-', dir=ROOT/'build')
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        with zipfile.ZipFile(PACKAGE) as archive:
            for entry in archive.infolist():
                target = self.work / entry.filename
                self.assertTrue(target.resolve().is_relative_to(self.work.resolve()))
                archive.extract(entry, self.work)
                target.chmod((entry.external_attr >> 16) & 0o777)
        self.bundle = self.work / 'LotusGB-RetroArch-macOS-universal'
        self.installer = self.bundle / 'Install LotusGB.command'
        self.assertTrue(self.installer.stat().st_mode & stat.S_IXUSR)
        self.data = self.work / 'RetroArch'
        (self.data/'config').mkdir(parents=True)
        self.core = self.work/'Custom cores $(touch SHOULD_NOT_EXIST)'
        self.info = self.work/'Custom info with spaces'
        self.core.mkdir(); self.info.mkdir()
        tilde_info = '~/' + str(self.info.relative_to(Path.home()))
        self.config = self.data/'config/retroarch.cfg'
        self.config.write_text(f'libretro_directory = "/unused/path"\nlibretro_directory = "{self.core}" # last setting\nlibretro_info_path = "{tilde_info}"\n')
        (self.core/'lotusgb_libretro.dylib').write_bytes(b'previous-core')
        (self.info/'lotusgb_libretro.info').write_bytes(b'previous-info')
        (self.core/'another_libretro.dylib').write_bytes(b'unrelated-core')
        (self.data/'game.srm').write_bytes(b'battery-save')
        self.before_config = self.config.read_bytes()

    def run_installer(self, *args, env=None):
        return subprocess.run(['/bin/bash',str(self.installer),'--data-dir',str(self.data),*args],
            cwd=self.work, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def assert_untouched(self):
        self.assertEqual((self.core/'another_libretro.dylib').read_bytes(),b'unrelated-core')
        self.assertEqual((self.data/'game.srm').read_bytes(),b'battery-save')
        self.assertEqual(self.config.read_bytes(),self.before_config)
        self.assertFalse((self.work/'SHOULD_NOT_EXIST').exists())

    def test_configured_paths_backups_reinstall_and_preserved_saves(self):
        result=self.run_installer(); self.assertEqual(result.returncode,0,result.stdout)
        for filename, folder in [('lotusgb_libretro.dylib',self.core),('lotusgb_libretro.info',self.info)]:
            self.assertEqual((folder/filename).read_bytes(),(self.bundle/'core'/filename).read_bytes())
        backups=sorted((self.data/'backups/LotusGB').iterdir())
        self.assertEqual(len(backups),1)
        self.assertEqual((backups[0]/'lotusgb_libretro.dylib').read_bytes(),b'previous-core')
        self.assertEqual((backups[0]/'lotusgb_libretro.info').read_bytes(),b'previous-info')
        self.assertTrue((self.info/'core_info.refresh').exists())
        self.assert_untouched()
        result=self.run_installer(); self.assertEqual(result.returncode,0,result.stdout)
        self.assertEqual(len(list((self.data/'backups/LotusGB').iterdir())),2)
        self.assert_untouched()

    def test_corrupt_payload_is_rejected_before_replacing_files(self):
        with (self.bundle/'core/lotusgb_libretro.info').open('ab') as file: file.write(b'corrupt')
        result=self.run_installer(); self.assertNotEqual(result.returncode,0)
        self.assertIn('checksum verification failed',result.stdout)
        self.assertEqual((self.core/'lotusgb_libretro.dylib').read_bytes(),b'previous-core')
        self.assertEqual((self.info/'lotusgb_libretro.info').read_bytes(),b'previous-info')
        self.assertFalse((self.data/'backups').exists())
        self.assert_untouched()

    def test_info_replace_failure_rolls_back_core(self):
        # Force only the second rename to fail; rollback must recover the first.
        stub=self.work/'stub-bin';stub.mkdir()
        command=stub/'mv'
        command.write_text('#!/bin/bash\ncase "${!#}" in *lotusgb_libretro.info) exit 71;; esac\nexec /bin/mv "$@"\n')
        command.chmod(0o755)
        env=dict(os.environ,PATH=str(stub)+os.pathsep+os.environ['PATH'])
        result=self.run_installer(env=env); self.assertNotEqual(result.returncode,0)
        self.assertIn('restoring any replaced files',result.stdout)
        self.assertNotIn('Installation complete',result.stdout)
        self.assertEqual((self.core/'lotusgb_libretro.dylib').read_bytes(),b'previous-core')
        self.assertEqual((self.info/'lotusgb_libretro.info').read_bytes(),b'previous-info')
        self.assertFalse(list(self.core.glob('.lotusgb-install.*')))
        self.assertFalse(list(self.info.glob('.lotusgb-install.*')))
        self.assert_untouched()

    def test_missing_config_uses_default_user_directories(self):
        self.config.unlink()
        result=self.run_installer(); self.assertEqual(result.returncode,0,result.stdout)
        self.assertTrue((self.data/'cores/lotusgb_libretro.dylib').is_file())
        self.assertTrue((self.data/'info/lotusgb_libretro.info').is_file())
        self.assertTrue((self.data/'info/core_info.refresh').is_file())
        self.assertFalse(self.config.exists())
        self.assertEqual((self.core/'lotusgb_libretro.dylib').read_bytes(),b'previous-core')
        self.assertEqual((self.data/'game.srm').read_bytes(),b'battery-save')

    def test_explicit_directories_override_config(self):
        core=self.work/'Explicit cores';info=self.work/'Explicit info'
        result=self.run_installer('--core-dir',str(core),'--info-dir',str(info))
        self.assertEqual(result.returncode,0,result.stdout)
        self.assertTrue((core/'lotusgb_libretro.dylib').is_file())
        self.assertTrue((info/'lotusgb_libretro.info').is_file())
        self.assertEqual((self.core/'lotusgb_libretro.dylib').read_bytes(),b'previous-core')
        self.assert_untouched()

def main():
    global PACKAGE
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',type=Path,required=True)
    args=parser.parse_args(); PACKAGE=args.package.resolve()
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(MacPackageTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1

if __name__=='__main__': sys.exit(main())
