#!/usr/bin/env python3
"""Check that releases contain required files without including local extras."""
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    temporary = Path(temporary)
    checkout = temporary/'checkout'
    shutil.copytree(root, checkout, ignore=shutil.ignore_patterns('*.gz', '*.png', '__pycache__'))
    extras = ['local-settings.txt', 'assets/private-notes.txt',
              'vendor/internal-notes.md', 'tests/browser-results.json']
    for name in extras:
        (checkout/name).write_text('Must not be published.\n')
    site = temporary/'site'
    subprocess.run([sys.executable, str(checkout/'package-site.py'), str(site)],
                   check=True, capture_output=True)
    for name in extras:
        assert not (site/name).exists(), name
    assert (site/'engine.wasm').read_bytes() == (root/'engine.wasm').read_bytes()
    assert (site/'DEPLOY.md').is_file()
    assert not (site/'source.tar.gz').exists()
    source_archive = temporary/'source.tar.gz'
    second_site = temporary/'second-site'
    subprocess.run([sys.executable, str(checkout/'package-site.py'), str(second_site),
                    '--source-archive', str(source_archive)], check=True, capture_output=True)
    assert not (second_site/'source.tar.gz').exists()
    rejected = subprocess.run([sys.executable, str(checkout/'package-site.py'), str(temporary/'unsafe-site'),
                              '--source-archive', str(temporary/'unsafe-site/source.tar.gz')], capture_output=True)
    assert rejected.returncode != 0
    assert not (temporary/'unsafe-site').exists()
    with tarfile.open(source_archive) as archive:
        members = archive.getmembers()
        names = {member.name for member in members}
        assert len(names) == len(members), 'Duplicate source entries'
        assert 'adlmidi-wasm/src/engine.cpp' in names
        assert 'adlmidi-wasm/vendor/midiplay.cc' in names
        assert 'adlmidi-wasm/build.sh' in names
        assert 'adlmidi-wasm/src/core.inc' not in names
        assert 'adlmidi-wasm/engine.wasm' not in names
        for name in extras:
            assert 'adlmidi-wasm/'+name not in names, name
        for member in members:
            assert member.uid == member.gid == member.mtime == 0
            assert not member.uname and not member.gname
        for name in ['so-pastdays.mid', 'so-town2.mid', 'wiz5-artifacts.mid']:
            path = 'assets/'+name
            if not (root/path).is_file():
                assert not (site/path).exists()
                assert 'adlmidi-wasm/'+path not in names
                continue
            expected = (root/path).read_bytes()
            assert (site/path).read_bytes() == expected
            assert archive.extractfile('adlmidi-wasm/'+path).read() == expected
print('PASS release contents, excluded local files, anonymous archive metadata, and unchanged MIDI assets')
