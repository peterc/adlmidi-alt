#!/usr/bin/env python3
"""Playlist integration checks against the real browser and AudioWorklet."""
import json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) != 2:
    raise SystemExit(f"Usage: {Path(__file__).name} SERVER_URL")
URL = sys.argv[1]
ENV = {**os.environ, 'AGENT_BROWSER_SESSION': f'adlmidi-playlist-test-{os.getpid()}'}
def browser(*args):
    result = subprocess.run(['agent-browser', *args], env=ENV, text=True, capture_output=True, timeout=30)
    if result.returncode: raise RuntimeError(result.stderr + result.stdout)
    return result.stdout.strip()
def evaluate(code): return json.loads(browser('eval', code))
def check(code): assert evaluate(code), code
def until(code): browser('wait', '--fn', code)
def click(selector): browser('scrollintoview', selector); browser('click', selector)
def passed(message): print('PASS', message, flush=True)
def drop(items):
    evaluate('''(async () => {
      const transfer = new DataTransfer();
      for (const [name, url] of ''' + json.dumps(items) + ''') {
        const bytes = await (await fetch(url)).arrayBuffer();
        transfer.items.add(new File([bytes], name));
      }
      window.dispatchEvent(new DragEvent('drop', {cancelable: true, dataTransfer: transfer}));
      return true;
    })()''')
try:
    browser('--args', '--mute-audio', 'open', URL)
    until('window.adlmidi && !document.querySelector("#play").disabled')
    check('document.querySelectorAll(".playlist-track").length >= 1 && !document.querySelector("#loop").checked && !document.querySelector(".reverb-details").open && window.scrollY === 0')
    with tempfile.TemporaryDirectory(prefix='adlmidi-playlist-') as folder:
        folder = Path(folder)
        for name in ['2.mid', '10.mid']:
            shutil.copy(ROOT/'assets/copper-circuit.mid', folder/name)
        browser('upload', '#file', str(folder/'10.mid'), str(folder/'2.mid'))
        until('document.querySelector("#song-title").textContent === "2"')
        check('Array.from(document.querySelectorAll(".playlist-track"), el => el.textContent).join() === "2.mid,10.mid" && !adlmidi.playing')
        click('#playlist-list li:last-child button')
        until('adlmidi.playing && document.querySelector("#song-title").textContent === "10"')
        check('document.querySelector("#next").disabled && !document.querySelector("#previous").disabled')
        click('#previous')
        until('adlmidi.playing && document.querySelector("#song-title").textContent === "2"')
        click('#next')
        until('adlmidi.playing && document.querySelector("#song-title").textContent === "10"')
        passed('Multiple-file picker, natural sorting, click-to-play, Previous and Next')
        evaluate('document.querySelector("#play").click(); document.querySelector(".playlist-track").click(); true')
        until('document.querySelector("#song-title").textContent === "2" && adlmidi.playing && adlmidi.time > .15')
        evaluate('document.querySelector("#stop").click(); document.querySelector("#playlist-list li:last-child button").click(); true')
        until('document.querySelector("#song-title").textContent === "10" && adlmidi.playing && adlmidi.time > .15')
        passed('Rapid track changes are not paused or rewound by stale transport commands')
        browser('upload', '#file', str(ROOT/'tests/invalid.mid'))
        until('!document.querySelector("#error").hidden')
        check('adlmidi.playing && document.querySelectorAll(".playlist-track").length === 2 && document.querySelector("#song-title").textContent === "10"')
        passed('Invalid selection leaves current playback and playlist intact')
        (folder/'nested').mkdir()
        shutil.copy(ROOT/'assets/copper-circuit.mid', folder/'nested/2.mid')
        (folder/'notes.txt').write_text('ignore this')
        browser('upload', '#folder', str(folder))
        until('document.querySelectorAll(".playlist-track").length === 3')
        check('Array.from(document.querySelectorAll(".playlist-track"), el => el.textContent).some(path => path.endsWith("/nested/2.mid")) && adlmidi.playing')
        passed('Real folder picker includes subfolders, ignores non-MIDI files and retains paths')
    browser('check', '#loop')
    drop([['03-last.mid', './assets/copper-circuit.mid'], ['01-short.mid', './tests/short.mid'], ['02-bad.mid', './tests/invalid.mid'], ['ignored.txt', './tests/invalid.mid']])
    until('adlmidi.playing && document.querySelector("#song-title").textContent === "01-short"')
    check('document.querySelectorAll(".playlist-track").length === 3')
    time.sleep(3)
    check('adlmidi.playing && document.querySelector("#song-title").textContent === "01-short"')
    browser('uncheck', '#loop')
    until('adlmidi.playing && document.querySelector("#song-title").textContent === "03-last"')
    check('document.querySelectorAll(".playlist-track.invalid").length === 1 && document.querySelector("#error").hidden')
    passed('Multi-file drop starts playback; Loop repeats; disabling Loop advances past invalid tracks')
    drop([['1.mid', './tests/short.mid'], ['2.mid', './tests/short.mid']])
    until('document.querySelector("#song-title").textContent === "2" && adlmidi.state === "ended"')
    check('!adlmidi.playing && document.querySelector("#next").disabled')
    passed('Playlist stops after the final track')
    # Emulate the browser entry interface at the drop boundary, retaining real file reads.
    evaluate('''(async () => {
      const bytes = await (await fetch('./assets/copper-circuit.mid')).arrayBuffer();
      const fileEntry = name => ({name, isFile: true, file: done => done(new File([bytes], name))});
      const dir = (name, children) => ({name, isDirectory: true, createReader: () => {
        let read = false; return {readEntries: done => {done(read ? [] : children); read = true;}};
      }});
      const root = dir('music', [fileEntry('2.mid'), dir('sub', [fileEntry('1.mid')])]);
      const event = new Event('drop', {cancelable: true});
      Object.defineProperty(event, 'dataTransfer', {value: {types: ['Files'], files: [], items: [
        {kind: 'file', webkitGetAsEntry: () => root, getAsFile: () => null}
      ]}});
      window.dispatchEvent(event); return true;
    })()''')
    until('adlmidi.playing && document.querySelector(".playlist-track").textContent === "music/2.mid"')
    check('document.querySelector("#playlist-list li:last-child button").textContent === "music/sub/1.mid"')
    passed('Folder drops populate the playlist with nested relative paths')
    # Hold an older file read while a newer selection completes.
    evaluate('''(async () => {
      const bytes = await (await fetch('./assets/copper-circuit.mid')).arrayBuffer();
      const slow = new File([bytes], 'slow.mid');
      slow.arrayBuffer = () => new Promise(resolve => { window.finishSlowRead = () => resolve(bytes); });
      const input = document.querySelector('#file');
      Object.defineProperty(input, 'files', {value: [slow], configurable: true});
      input.dispatchEvent(new Event('change'));
      delete input.files;
      return true;
    })()''')
    drop([['newest.mid', './assets/copper-circuit.mid']])
    until('document.querySelector("#song-title").textContent === "newest" && adlmidi.playing')
    evaluate('window.finishSlowRead(); true')
    check('document.querySelector("#song-title").textContent === "newest" && document.querySelectorAll(".playlist-track").length === 1')
    passed('A slow earlier selection cannot replace a newer playlist')
    for width in [1280, 800, 390, 320]:
        browser('set', 'viewport', str(width), '960')
        check('document.documentElement.scrollWidth <= innerWidth')
    check('document.querySelector("#error").hidden')
    assert not browser('errors')
    passed('Responsive layout and no uncaught browser errors')
finally:
    browser('close')
