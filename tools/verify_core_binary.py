#!/usr/bin/env python3
"""Validate and stage a platform-specific LotusGB libretro core artifact."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


PLATFORMS = {
    'linux-x86_64': ('lotusgb_libretro.so', r'ELF 64-bit.*shared object.*x86-64'),
    'linux-aarch64': ('lotusgb_libretro.so', r'ELF 64-bit.*shared object.*ARM aarch64'),
    'windows-x86_64': ('lotusgb_libretro.dll', r'PE32\+.*(x86-64|x86_64)'),
    'macos-arm64': ('lotusgb_libretro.dylib', r'Mach-O 64-bit.*arm64'),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=sorted(PLATFORMS), required=True)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--info', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    core = args.core.resolve()
    info = args.info.resolve()
    output = args.output.resolve()
    if not core.is_file() or not info.is_file():
        parser.error('core and info files must exist')
    if 'supported_extensions = "gb|gbc"' not in info.read_text(encoding='utf-8'):
        parser.error('core info must advertise gb|gbc')

    description = subprocess.check_output(['file', '-b', str(core)], text=True).strip()
    filename, expected_format = PLATFORMS[args.platform]
    if not re.search(expected_format, description, re.IGNORECASE):
        parser.error(f'expected {args.platform} binary format, got: {description}')

    output.mkdir(parents=True, exist_ok=True)
    staged_core = output / filename
    staged_info = output / 'lotusgb_libretro.info'
    shutil.copyfile(core, staged_core)
    shutil.copyfile(info, staged_info)
    root = Path(__file__).resolve().parent.parent
    shutil.copyfile(root / 'LICENSE', output / 'LICENSE')
    libretro_header = root / 'src/libretro/include/libretro.h'
    header_notice = libretro_header.read_text(encoding='utf-8').split('*/', 1)[0] + '*/\n'
    (output / 'THIRD_PARTY_NOTICES.txt').write_text(
        'Vendored libretro API header notice:\n\n' + header_notice,
        encoding='utf-8',
    )
    manifest = {
        'product': 'LotusGB RetroArch libretro core',
        'platform': args.platform,
        'binary': filename,
        'binary_format': description,
        'supported_extensions': ['gb', 'gbc'],
        'core_sha256': sha256(staged_core),
        'info_sha256': sha256(staged_info),
        'device_verification': 'not performed by the build workflow',
    }
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'SHA256SUMS').write_text(
        f'{sha256(staged_core)}  {filename}\n{sha256(staged_info)}  lotusgb_libretro.info\n',
        encoding='ascii',
    )
    print(f'{args.platform}: {description}')
    print(f'core_sha256={manifest["core_sha256"]}')


if __name__ == '__main__':
    main()
