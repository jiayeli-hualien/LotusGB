#!/usr/bin/env python3
"""Create a user-local Universal LotusGB installer ZIP, without RetroArch/ROMs."""
import argparse
import ctypes
import re
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
NAME = 'LotusGB-RetroArch-macOS-universal'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def package(core, info, output):
    architectures = subprocess.check_output(['lipo', '-archs', str(core)], text=True).split()
    if set(architectures) != {'arm64', 'x86_64'}:
        raise ValueError('package requires a Universal arm64 + x86_64 core')
    commands = subprocess.check_output(['otool', '-l', str(core)], text=True)
    minimums = re.findall(r'\bminos\s+([0-9.]+)', commands)
    if len(minimums) != 2 or any(tuple(map(int, v.split('.'))) != (11, 0) for v in minimums):
        raise ValueError('both core slices must target macOS 11.0')
    if 'supported_extensions = "gb|gbc"' not in info.read_text():
        raise ValueError('core info must advertise gb|gbc')
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='lotusgb-package-', dir=output) as temporary:
        bundle = Path(temporary) / NAME
        payload = bundle / 'core'
        payload.mkdir(parents=True)
        target = payload / 'lotusgb_libretro.dylib'
        shutil.copyfile(core, target)
        # Remove the developer's absolute build path from LC_ID_DYLIB. Re-sign
        # this copy ad hoc after the Mach-O edit; this is not a Developer ID.
        subprocess.run(['install_name_tool', '-id', '@rpath/lotusgb_libretro.dylib', str(target)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', '--timestamp=none', str(target)], check=True)
        subprocess.run(['codesign', '--verify', '--strict', '--all-architectures', str(target)], check=True)
        dependencies = subprocess.check_output(['otool', '-L', str(target)], text=True)
        for line in dependencies.splitlines():
            if line.startswith('\t'):
                dependency = line.strip().split(' (', 1)[0]
                if not dependency.startswith(('/usr/lib/', '/System/Library/')) and dependency != '@rpath/lotusgb_libretro.dylib':
                    raise ValueError(f'non-system dependency must be packaged explicitly: {dependency}')
        class SystemInfo(ctypes.Structure):
            _fields_ = [('name', ctypes.c_char_p), ('version', ctypes.c_char_p),
                        ('extensions', ctypes.c_char_p), ('need_fullpath', ctypes.c_bool),
                        ('block_extract', ctypes.c_bool)]
        library = ctypes.CDLL(str(target))
        library.retro_get_system_info.argtypes = [ctypes.POINTER(SystemInfo)]
        system_info = SystemInfo()
        library.retro_get_system_info(ctypes.byref(system_info))
        if system_info.extensions != b'gb|gbc':
            raise ValueError('packaged core API does not advertise gb|gbc')
        shutil.copyfile(info, payload / 'lotusgb_libretro.info')
        shutil.copyfile(ROOT / 'tools/install_retroarch_macos.command', bundle / 'Install LotusGB.command')
        shutil.copyfile(ROOT / 'LICENSE', bundle / 'LICENSE.txt')
        header_notice = (ROOT / 'src/libretro/include/libretro.h').read_text().split('*/', 1)[0] + '*/\n'
        (bundle / 'THIRD_PARTY_NOTICES.txt').write_text('Vendored libretro API header notice (RetroArch.app is not included):\n\n' + header_notice)
        (bundle / 'README.txt').write_text('''LotusGB for RetroArch — macOS Universal (.gb / .gbc)

內容是 LotusGB 核心；不包含 RetroArch.app、遊戲 ROM 或 BIOS。
Apple Silicon 與 Intel 皆包含；部署目標 macOS 11.0。

1. 先安裝並開啟過 macOS RetroArch，再完全結束 RetroArch。
2. 將 ZIP 完整解壓縮，保留 core 資料夾與其餘檔案在一起。
3. 雙擊「Install LotusGB.command」。它不需要 sudo、Python 或 Xcode。
   若 Finder 不允許開啟，可在 Terminal 進入解壓縮後資料夾，執行：
   bash "Install LotusGB.command"
4. 重新開啟 RetroArch，選擇 LotusGB，載入 .gb 或 .gbc。

安裝器會讀取 ~/Library/Application Support/RetroArch/config/retroarch.cfg
中的 libretro_directory / libretro_info_path，並自動備份原核心及 .info。
它會設定 core_info.refresh，讓 RetroArch 下次啟動重建核心資訊快取。
不會修改設定、遊戲存檔、即時存檔、其他核心或 RetroArch.app。
備份路徑會顯示於 Terminal，通常位於 RetroArch/backups/LotusGB/。
需要還原時，將該次備份的 .dylib / .info 複製回 paths.txt 中的目錄。

自訂目錄例：
bash "Install LotusGB.command" --data-dir "/path/to/RetroArch"
bash "Install LotusGB.command" --core-dir "/path/to/cores" --info-dir "/path/to/info"
可加 --config "/path/to/retroarch.cfg" 讀取其他設定檔。

Native CGB 不需要 BIOS；其他 GBC 遊戲相容性與精確時序仍持續改善。
此開發套件只使用 ad-hoc 簽章，未經 Developer ID 簽章或 Apple notarization。
SHA256SUMS 可檢查檔案完整性，不代表 Apple 認證。

This bundle installs only LotusGB into an existing RetroArch user setup.
It includes arm64 and x86_64, with a macOS 11.0 deployment target. It does not
include RetroArch, ROMs or firmware. Quit RetroArch, extract the whole ZIP,
run Install LotusGB.command, then reopen RetroArch. Existing core/info files
are backed up; settings and saves are preserved. No administrator account,
Python or developer tools are needed to install. See manifest.json for build
identity. The core is ad-hoc signed, not Developer ID signed or notarized.
''', encoding='utf-8')
        revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True))
        manifest = {
            'product': 'LotusGB core for an existing macOS RetroArch',
            'created_utc': datetime.now(timezone.utc).isoformat(),
            'architectures': sorted(architectures), 'deployment_target_macos': '11.0',
            'supported_extensions': ['gb', 'gbc'],
            'git_revision': revision, 'working_tree_dirty': dirty,
            'signing': 'ad-hoc; not Developer ID signed or notarized',
            'core_sha256': digest(target),
            'info_sha256': digest(payload / 'lotusgb_libretro.info'),
        }
        (bundle / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        for file in bundle.rglob('*'):
            if file.is_file():
                file.chmod(0o755 if file.name.endswith(('.command', '.dylib')) else 0o644)
        files = sorted(p for p in bundle.rglob('*') if p.is_file())
        (bundle / 'SHA256SUMS').write_text(''.join(f'{digest(f)}  {f.relative_to(bundle)}\n' for f in files))
        archive = Path(temporary) / (NAME + '.zip')
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as zip:
            for file in sorted(bundle.rglob('*')):
                if file.is_file():
                    zip.write(file, str(Path(NAME) / file.relative_to(bundle)))
        destination = output / archive.name
        archive.replace(destination)
        (output / (destination.name + '.sha256')).write_text(f'{digest(destination)}  {destination.name}\n')
        print(destination.resolve())
        return destination

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--info', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build/packages')
    args = parser.parse_args()
    package(args.core.resolve(), args.info.resolve(), args.output_dir.resolve())

if __name__ == '__main__':
    main()
