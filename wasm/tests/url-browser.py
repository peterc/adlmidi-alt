#!/usr/bin/env python3
"""Exercise direct URL loading, including real cross-origin failures."""
import json, os, subprocess, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
if len(sys.argv) != 2:
    raise SystemExit(f"Usage: {Path(__file__).name} SERVER_URL")
URL = sys.argv[1]
MIDI = (ROOT/'assets/copper-circuit.mid').read_bytes()
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(404 if self.path == '/missing.mid' else 200)
        if self.path != '/blocked.mid': self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Type', 'audio/midi')
        self.end_headers()
        self.wfile.write(b'not midi' if self.path == '/invalid.mid' else MIDI)
    def log_message(self, *args): pass
server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
origin = f'http://127.0.0.1:{server.server_port}'
env = {**os.environ, 'AGENT_BROWSER_SESSION': f'adlmidi-url-{os.getpid()}'}
def b(*args):
    r = subprocess.run(['agent-browser', *args], env=env, text=True, capture_output=True, timeout=30)
    if r.returncode: raise RuntimeError(r.stderr+r.stdout)
    return r.stdout.strip()
def ev(code): return json.loads(b('eval', code))
def check(code): assert ev(code), code
def click(selector): b('scrollintoview', selector); b('click', selector)
def submit(path):
    if ev('document.querySelector("#url-form").hidden'): click('#open-url')
    b('fill', '#midi-url', origin+path)
    click('#load-url')
try:
    b('--args', '--mute-audio', 'open', URL)
    b('wait', '--fn', 'window.adlmidi && !document.querySelector("#play").disabled')
    check('document.querySelectorAll(".playlist-track").length>=1 && !document.querySelector("#demo")')
    submit('/remote%20song.mid?download=1')
    b('wait', '--fn', 'adlmidi.playing && document.querySelector("#song-title").textContent==="remote song"')
    check('document.querySelector("#url-form").hidden && document.querySelectorAll(".playlist-track").length===1')
    print('PASS cross-origin MIDI fetch, filename decoding, playlist replacement and autoplay', flush=True)
    for path, message in [('/missing.mid', 'HTTP 404'), ('/invalid.mid', None), ('/blocked.mid', 'CORS')]:
        submit(path)
        b('wait', '--fn', '!document.querySelector("#load-url").disabled && !document.querySelector("#error").hidden')
        check('adlmidi.playing && document.querySelector("#song-title").textContent==="remote song" && document.querySelectorAll(".playlist-track").length===1')
        if message: assert message in ev('document.querySelector("#error").textContent')
        print('PASS failed URL preserves playback:', path, flush=True)
    for width in [1280, 912, 390, 320]:
        b('set', 'viewport', str(width), '900')
        check('document.documentElement.scrollWidth<=innerWidth')
    b('set', 'viewport', '1000', '900')
    b('screenshot', '--full', str(ROOT/'tests/url.png'))
    click('#close-url')
    check('document.querySelector("#url-form").hidden && document.querySelector("#open-url").getAttribute("aria-expanded")==="false"')
    print('PASS URL form layout and close control', flush=True)
finally:
    b('close')
    server.shutdown(); server.server_close()
