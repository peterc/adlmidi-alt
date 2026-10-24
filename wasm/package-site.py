#!/usr/bin/env python3
"""Package the static site, optionally creating a separate source archive."""
import argparse
from pathlib import Path
import shutil
import tarfile

RUNTIME_FILES = [
    'index.html', 'style.css', 'app.mjs', 'midi.mjs', 'playlist.mjs', 'worklet.mjs',
    'engine.mjs', 'engine.wasm', 'LICENSE-GPL-3.0.txt', 'README.md', 'DEPLOY.md',
]
ASSET_FILES = [
    'assets/favicon.svg', 'assets/banks.json', 'assets/programs.json',
    'assets/copper-circuit.mid', 'assets/demo-playlist.json',
    'assets/fonts/WebPlus_IBM_VGA_8x16.woff',
    'assets/fonts/README.txt', 'assets/fonts/LICENSE-CC-BY-SA-4.0.txt',
]
SOURCE_FILES = [name for name in RUNTIME_FILES if name not in ('engine.mjs', 'engine.wasm')] + ASSET_FILES + [
    '.gitignore', 'nginx.conf', 'build.sh', 'prepare.py', 'package-site.py',
    'src/engine.cpp', 'assets/create-demo.py',
    'vendor/source.json', 'vendor/UPSTREAM-README.md', 'vendor/midiplay.cc',
    'vendor/dbopl.cpp', 'vendor/dbopl.h', 'vendor/adldata.cc', 'vendor/adldata.hh',
    'vendor/fraction', 'tests/engine.test.mjs', 'tests/metadata.test.mjs',
    'tests/playlist.test.mjs', 'tests/package.test.py', 'tests/verify-vendor.py',
    'tests/compare-native.mjs', 'tests/browser-smoke.py', 'tests/playlist-browser.py',
    'tests/url-browser.py',
]

def public_metadata(info):
    # Archive metadata must not disclose the builder's account or local times.
    info.uid = info.gid = info.mtime = 0
    info.uname = info.gname = ''
    info.pax_headers = {}
    return info

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('destination', type=Path)
parser.add_argument('--source-archive', type=Path, help='optional archive path outside the site directory')
args = parser.parse_args()
destination = args.destination.resolve()
source_archive = args.source_archive.resolve() if args.source_archive else None
root = Path(__file__).resolve().parent
optional_demos = ['assets/'+name for name in ('so-pastdays.mid', 'so-town2.mid', 'wiz5-artifacts.mid')
                  if (root/'assets'/name).is_file()]
ASSET_FILES += optional_demos
SOURCE_FILES += optional_demos
if destination == root or root in destination.parents:
    parser.error('Use a new destination outside the wasm source directory.')
if source_archive and (source_archive == destination or destination in source_archive.parents):
    parser.error('The source archive must be outside the public site directory.')
if source_archive and source_archive.exists():
    parser.error('The source archive destination already exists.')
for name in set(RUNTIME_FILES + ASSET_FILES + (SOURCE_FILES if source_archive else [])):
    if not (root/name).is_file() or (root/name).is_symlink():
        parser.error(f'Missing or non-regular release file: {name}. Build the engine first.')
destination.mkdir(parents=True, exist_ok=False)
for name in RUNTIME_FILES + ASSET_FILES:
    (destination/name).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root/name, destination/name)
if source_archive:
    source_archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(source_archive, 'w:gz') as archive:
        for name in sorted(SOURCE_FILES):
            archive.add(root/name, arcname=Path('adlmidi-wasm')/name,
                        recursive=False, filter=public_metadata)
print(destination)
