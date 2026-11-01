#!/usr/bin/env python3
"""Package an x86-64 Linux libretro core and matching info for Steam Deck."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['steam-deck'], required=True)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--info', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    core, info, output = (p.resolve() for p in (args.core, args.info, args.output_dir))
    if not core.is_file() or not info.is_file():
        parser.error('core and info files must exist')
    if 'supported_extensions = "gb|gbc"' not in info.read_text():
        parser.error('core info must advertise gb|gbc')
    description = subprocess.check_output(['file', '-b', str(core)], text=True)
    if 'ELF 64-bit LSB shared object, x86-64' not in description:
        parser.error(f'expected an x86-64 Linux ELF shared object, got: {description.strip()}')
    output.mkdir(parents=True, exist_ok=True)
    name = 'LotusGB-Steam-Deck-x86_64'
    with tempfile.TemporaryDirectory(prefix='lotusgb-deck-package-', dir=output) as tmp:
        bundle = Path(tmp) / name
        payload = bundle / 'core'
        payload.mkdir(parents=True)
        shutil.copyfile(core, payload / 'lotusgb_libretro.so')
        shutil.copyfile(info, payload / 'lotusgb_libretro.info')
        shutil.copyfile(ROOT / 'LICENSE', bundle / 'LICENSE.txt')
        (bundle / 'README.txt').write_text('''LotusGB core for Steam Deck / Linux x86_64 RetroArch

Contents: lotusgb_libretro.so and matching lotusgb_libretro.info.
Supports .gb and .gbc cartridges; model selection uses the cartridge header.

Install into the RetroArch directories configured on your Deck:
- copy core/lotusgb_libretro.so to the configured core directory
- copy core/lotusgb_libretro.info to the configured info directory
- refresh the core information cache or restart RetroArch

This is an unofficial third-party core and is not affiliated with Nintendo.
It contains no game ROMs or game captures. Licensed homebrew is recommended
for reproducible testing. The project license is MIT; see LICENSE.txt.
''', encoding='utf-8')
        manifest = {
            'product': 'LotusGB RetroArch core for Steam Deck / Linux x86_64',
            'created_utc': datetime.now(timezone.utc).isoformat(),
            'format': 'ELF64 x86-64', 'supported_extensions': ['gb', 'gbc'],
            'core_sha256': sha256(payload / 'lotusgb_libretro.so'),
            'info_sha256': sha256(payload / 'lotusgb_libretro.info'),
            'contains_commercial_game_content': False,
            'device_verification': 'pending',
        }
        (bundle / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        archive = output / f'{name}.zip'
        staged = Path(tmp) / archive.name
        with zipfile.ZipFile(staged, 'w', zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(bundle.rglob('*')):
                if path.is_file():
                    zf.write(path, path.relative_to(bundle.parent))
        staged.replace(archive)
        (output / f'{archive.name}.sha256').write_text(
            f'{sha256(archive)}  {archive.name}\n', encoding='ascii')
    print(archive)
    print(f'core_sha256={sha256(core)}')
    print(f'package_sha256={sha256(archive)}')


if __name__ == '__main__':
    main()
