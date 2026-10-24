#!/usr/bin/env python3
"""Exercise the real UI and AudioWorklet with a private agent-browser session."""
import json, os, subprocess, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
if len(sys.argv) != 2:
    raise SystemExit(f"Usage: {Path(__file__).name} SERVER_URL")
URL = sys.argv[1]
ENV={**os.environ,'AGENT_BROWSER_SESSION':f'adlmidi-smoke-{os.getpid()}'}
def browser(*args):
    result=subprocess.run(['agent-browser',*args],env=ENV,text=True,capture_output=True,timeout=30)
    if result.returncode: raise RuntimeError(result.stderr+result.stdout)
    return result.stdout.strip()
def evaluate(code): return json.loads(browser('eval',code))
def click(selector): browser('scrollintoview',selector); browser('click',selector)
def until(expression,seconds=8):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if evaluate(expression): return
        time.sleep(.1)
    raise AssertionError(f'Timed out: {expression}')
def check(expression): assert evaluate(expression),expression
results=[]
def passed(name): results.append(name); print('PASS',name,flush=True)
try:
    browser('--args','--mute-audio,--use-fake-device-for-media-stream','open',URL)
    until('!!window.adlmidi && !document.querySelector("#play").disabled')
    click('#play'); until('window.adlmidi.playing && window.adlmidi.time>.2')
    signal=evaluate('(()=>{let a=new Float32Array(2048);adlmidi.analyser.getFloatTimeDomainData(a);return {peak:Math.max(...a.map(Math.abs)),sampleRate:adlmidi.context.sampleRate,state:adlmidi.context.state}})()')
    assert signal['peak']>.0001 and signal['state']=='running',signal
    passed('AudioWorklet produces a nonzero stereo signal')
    until('Array.from(document.querySelectorAll(".channel-meter")).some(el=>+el.getAttribute("aria-valuenow")>0)')
    check('document.querySelectorAll(".channel").length===16 && !document.querySelector(".channel.active")')
    passed('Instrument rows stay static while channel level meters move')
    click('#play'); until('adlmidi.state==="paused"'); t=evaluate('adlmidi.time'); time.sleep(.2); assert evaluate('adlmidi.time')==t
    until('Array.from(document.querySelectorAll(".channel-meter")).every(el=>+el.getAttribute("aria-valuenow")===0)')
    click('#play'); until('adlmidi.playing && adlmidi.time>'+str(t))
    passed('Pause freezes the clock and resume advances it')
    browser('scrollintoview','#bank'); browser('select','#bank','14'); browser('select','#cards','8')
    until('adlmidi.playing && document.querySelector("#voices").textContent.includes("80 two-op")')
    browser('uncheck','#reverb'); check('document.querySelector("#wet").disabled')
    evaluate('document.querySelector("#gain").value=6;document.querySelector("#gain").dispatchEvent(new Event("input"));true')
    check('document.querySelector("#gain-value").textContent==="+6 dB"')
    passed('Bank, card, gain, and reverb controls work during playback')
    check('Array.from(document.querySelectorAll(".reverb-settings input")).every(el=>el.disabled)')
    browser('check','#reverb')
    before=evaluate('adlmidi.time')
    evaluate('''(()=>{
      for(const [id,value] of Object.entries({room:35,decay:75,damping:40,predelay:150,stereo:60})) {
        const el=document.getElementById(id);el.value=value;
        el.dispatchEvent(new Event('input'));el.dispatchEvent(new Event('change'));
      }
      return true;
    })()''')
    until('adlmidi.playing && adlmidi.time>'+str(before))
    check('document.querySelector("#room-value").textContent==="35%" && document.querySelector("#predelay-value").textContent==="150 ms" && document.querySelector("#error").hidden')
    for control in ['#tremolo','#vibrato','#modulators']: browser('check',control)
    until('adlmidi.playing && adlmidi.time>.1')
    click('#reset-settings')
    until('adlmidi.playing && adlmidi.time>.1')
    check('document.querySelector("#bank").value==="1" && document.querySelector("#cards").value==="4" && document.querySelector("#room").value==="70" && document.querySelector("#predelay").value==="0" && Array.from(document.querySelectorAll(".synth-options input")).every(el=>!el.checked) && document.querySelector("#error").hidden')
    passed('Expanded reverb, synthesis options, and reset work during playback')
    browser('upload','#file',str(ROOT/'tests/invalid.mid'))
    check('!document.querySelector("#error").hidden && adlmidi.playing')
    passed('Invalid input is rejected without interrupting the current song')
    evaluate('''(async()=>{
      const bytes=await (await fetch('./tests/short.mid')).arrayBuffer();
      const transfer=new DataTransfer();
      transfer.items.add(new File([bytes],'dropped.mid',{type:'audio/midi'}));
      document.querySelector('#dropzone').dispatchEvent(new DragEvent('drop',{
        bubbles:true,cancelable:true,dataTransfer:transfer
      }));
      return true;
    })()''')
    until('document.querySelector("#song-title").textContent==="dropped" && adlmidi.playing')
    check('document.querySelector("#error").hidden')
    passed('Dropping a MIDI file loads and plays without an error')
    browser('upload','#file',str(ROOT/'tests/short.rmi')); browser('scrollintoview','#loop'); browser('uncheck','#loop')
    until('adlmidi.state==="ended"',6)
    check('document.querySelector("#error").hidden && document.querySelector("#song-title").textContent==="short"')
    passed('RMI upload and nonlooping completion')
    click('#play'); until('adlmidi.playing'); click('#stop'); until('adlmidi.state==="ready" && adlmidi.time===0')
    passed('Replay and stop/rewind')
    browser('upload','#file',str(ROOT/'assets/copper-circuit.mid')); browser('scrollintoview','#bank'); browser('select','#bank','1'); browser('select','#cards','4')
    browser('check','#reverb'); browser('scrollintoview','#loop'); browser('check','#loop')
    evaluate('document.querySelector("#gain").value=12;document.querySelector("#gain").dispatchEvent(new Event("input"));true')
    until('!document.querySelector("#play").disabled'); click('#play'); until('adlmidi.playing && adlmidi.time>.2')
    browser('set','viewport','1280','960')
    check('(()=>{const [p,l,s,c]=Array.from(document.querySelectorAll(".panel"),el=>el.getBoundingClientRect());return p.left===12 && p.top===12 && s.top===p.top && s.left===p.right+12 && l.top===p.bottom+12 && c.top===p.top && c.left===s.right+12 && p.width===360 && s.width===360 && c.width===360})()')
    browser('screenshot','--full',str(ROOT/'tests/desktop.png'))
    browser('set','viewport','390','844'); check('document.documentElement.scrollWidth<=innerWidth')
    check('document.querySelector(".sound-settings").getBoundingClientRect().top>document.querySelector(".player").getBoundingClientRect().bottom')
    browser('screenshot','--full',str(ROOT/'tests/mobile.png'))
    passed('Desktop and mobile layouts fit without horizontal overflow')
    check('document.querySelector("#error").hidden')
    errors=browser('errors'); assert not errors,errors
    passed('No uncaught browser errors')
    (ROOT/'tests/browser-results.json').write_text(json.dumps({'signal':signal,'checks':results},indent=2)+'\n')
finally:
    browser('close')
