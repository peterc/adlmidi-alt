import assert from 'node:assert/strict';
import {readFileSync, writeFileSync} from 'node:fs';
import {performance} from 'node:perf_hooks';
import createEngine from '../engine.mjs';
import {parseMidi} from '../midi.mjs';
const demo = readFileSync(new URL('../assets/copper-circuit.mid', import.meta.url));
const song = parseMidi(demo);
assert.equal(song.duration, 32); assert.equal(song.tracks, 5);
const vlq = n => { const out = [n & 127]; while (n >>= 7) out.unshift((n & 127) | 128); return out; };
function midi(events, division = 480) {
  const track = Buffer.from(events), header = Buffer.alloc(22);
  header.write('MThd'); header.writeUInt32BE(6, 4); header.writeUInt16BE(1, 10); header.writeUInt16BE(division, 12);
  header.write('MTrk', 14); header.writeUInt32BE(track.length, 18);
  return Buffer.concat([header, track]);
}
const short = midi([0,0xc0,0, 0,0x90,60,90, ...vlq(480),0x80,60,0, 0,0xff,0x2f,0]);
const tempoChange = midi([0,0x90,60,80, ...vlq(480),0xff,0x51,3,15,66,64, ...vlq(480),0x80,60,0, 0,0xff,0x2f,0]);
assert.equal(parseMidi(short).duration, .5); assert.equal(parseMidi(tempoChange).duration, 1.5);
assert.throws(() => parseMidi(Buffer.from('not midi')));
assert.throws(() => parseMidi(demo.subarray(0, -1)), /truncated/);
assert.throws(() => parseMidi(midi([0,0x90,60])), /truncated/);
assert.throws(() => parseMidi(midi([0,60,80,0,0xff,0x2f,0])), /running status/);
assert.throws(() => parseMidi(midi([0,0xff,0x2f,0], 0xe728)), /SMPTE/);
assert.throws(() => parseMidi(midi([0,0xff,0x51,3,0,0,0,0,0xff,0x2f,0])), /zero/);
const riff = Buffer.alloc(20); riff.write('RIFF'); riff.writeUInt32LE(short.length + 12, 4); riff.write('RMIDdata', 8); riff.writeUInt32LE(short.length,16);
const rmi = Buffer.concat([riff, short]); assert.deepEqual(Array.from(parseMidi(rmi).bytes), Array.from(short));
writeFileSync(new URL('short.mid', import.meta.url), short);
writeFileSync(new URL('short.rmi', import.meta.url), rmi);
writeFileSync(new URL('invalid.mid', import.meta.url), 'not a MIDI file');
console.log('PASS MIDI validation, RMI extraction, default tempo, and tempo changes');
const e = await createEngine({wasmBinary: readFileSync(new URL('../engine.wasm', import.meta.url))});
function load(bytes=demo, bank=1, cards=4, rate=48000, gain=12, reverb=1) {
  e.FS.writeFile('/song.mid', bytes);
  assert.equal(e._engine_load(rate, bank, cards), 1);
  e._engine_settings(gain, reverb, 24, 0);
}
function render(frames, quantum=128) {
  const output = new Float32Array(frames * 2);
  for (let offset=0; offset<frames; offset+=quantum) {
    const count=Math.min(quantum, frames-offset), ptr=e._engine_render(count)>>2;
    output.set(e.HEAPF32.subarray(ptr,ptr+count*2),offset*2);
  }
  return output;
}
function stats(data) {
  let peak=0,energy=0;
  for (const v of data) { assert.ok(Number.isFinite(v)); peak=Math.max(peak,Math.abs(v)); energy+=v*v; }
  assert.ok(peak<1); return {peak,rms:Math.sqrt(energy/data.length)};
}
load(); assert.equal(e._engine_fourops(),16);
const start=performance.now(), pcm=render(48000), measured=stats(pcm), milliseconds=performance.now()-start;
assert.ok(measured.rms>.01); assert.ok(Math.abs(e._engine_time()-1)<1e-8);
console.log('PASS one second of stereo audio', {...measured,renderMs:Math.round(milliseconds*100)/100});
load(short); render(48000*3); assert.equal(e._engine_ended(),1);
assert.ok(Math.abs(e._engine_time()-2.5)<2/48000);
load(tempoChange); render(48000*4); assert.equal(e._engine_ended(),1);
assert.ok(Math.abs(e._engine_time()-3.5)<2/48000);
console.log('PASS natural endings and two-second release tail');
for (const rate of [44100,48000,96000]) {
  load(demo,1,4,rate); const out=render(rate,257); assert.ok(stats(out).rms>.01); assert.ok(Math.abs(e._engine_time()-1)<1e-8);
}
console.log('PASS 44.1 / 48 / 96 kHz and nonstandard render block sizes');
load(demo,1,4,48000,-6,0); const quiet=stats(render(48000)).rms;
load(demo,1,4,48000,0,0); const normal=stats(render(48000)).rms;
assert.ok(Math.abs(normal/quiet-Math.pow(10,6/20))<.02);
load(demo,1,4,48000,0,1); const wet=render(48000);
load(demo,1,4,48000,0,0); const dry=render(48000);
assert.ok(wet.some((v,i)=>Math.abs(v-dry[i])>.0001));
console.log('PASS gain scaling and audible reverb contribution');
for(let bank=0;bank<67;bank++) { load(demo,bank); stats(render(4800)); }
for(const cards of [1,2,4,8,16]) { load(demo,1,cards); assert.ok(stats(render(4800)).rms>.001); }
console.log('PASS all 67 banks and 1 / 2 / 4 / 8 / 16 cards');
for(let n=0;n<20;n++) { load(short); stats(render(1024)); }
console.log('PASS repeated song replacement');
// Bypass JavaScript validation to exercise the new C++ bounded parser and catches.
const badInputs = [
  demo.subarray(0, -1),
  midi([0,0x90,60]),
  midi([0,60,80,0,0xff,0x2f,0]),
  midi([0,0xff,0x51,3,0,0,0,0,0xff,0x2f,0]),
  midi([0,0xff,0x2f,0], 0),
  midi([0,0x90,60,80,0,0xf0,1,0xf7,0,60,0,0,0xff,0x2f,0]),
];
for (const bytes of badInputs) {
  e.FS.writeFile('/song.mid', bytes);
  assert.equal(e._engine_load(48000,1,4),0);
  load(short); assert.ok(stats(render(4800)).rms>.001);
}
assert.throws(() => parseMidi(badInputs.at(-1)), /running status/);
console.log('PASS C++ malformed-input rejection, SysEx running-status reset, and recovery');
load(midi([0,0xff,0x2f,0]));
assert.ok(render(128).every(v=>v===0));
assert.equal(e._engine_ended(),1); assert.equal(e._engine_time(),0);
// A held, sustained note must release at EOF, without adding a second tail.
load(midi([0,0xb0,64,127,0,0x90,60,90,...vlq(480),0xff,0x2f,0]));
render(48000/2);
const notes = () => {
  const base=e._engine_notes()>>2;
  return Array.from({length:4*23},(_,i)=>e.HEAP32[base+i*4+3]).some(v=>v>0);
};
assert.equal(notes(),false);
assert.ok(stats(render(4800)).rms>0, 'release should retain audible decay');
render(48000*2); assert.equal(e._engine_ended(),1);
assert.ok(Math.abs(e._engine_time()-2.5)<2/48000);
console.log('PASS silent files and sustained-note release at EOF');
const marker = name => [0,0xff,6,name.length,...Buffer.from(name)];
const loopSong = midi([
  ...marker('loopStart'),0,0x90,60,90,
  ...vlq(240),0x80,60,0,0,0xff,0x51,3,15,66,64,
  ...vlq(240),0xff,6,7,...Buffer.from('loopEnd'),0,0xff,0x2f,0,
]);
load(loopSong); e._engine_settings(12,1,24,1);
render(43200); assert.equal(notes(),true); // .90 s: second iteration
render(9600); assert.equal(notes(),false); // 1.10 s: restored tempo released at 1.00 s
render(24000); assert.equal(notes(),true); // 1.60 s: third iteration
assert.equal(e._engine_ended(),0);
e._engine_settings(12,1,24,0); render(48000*3);
assert.equal(e._engine_ended(),1);
load(midi([...marker('loopStart'),...marker('loopEnd'),0,0xff,0x2f,0]));
e._engine_settings(12,1,24,1); render(128); assert.equal(e._engine_ended(),1);
console.log('PASS marker loops restore tempo, loop toggle, and zero-duration loops');
const levels = () => {
  const ptr=e._engine_levels()>>2;
  return Array.from(e.HEAPF32.subarray(ptr,ptr+16));
};
const meterSong = velocity => midi([0,0xb3,7,127,0,0xb3,11,127,
  0,0x93,60,velocity,...vlq(480),0x83,60,0,0,0xff,0x2f,0]);
load(meterSong(96)); render(4800);
const loudLevels=levels();
assert.ok(loudLevels[3]>0);
assert.ok(loudLevels.every((value,i)=>Number.isFinite(value) && (i===3 || value===0)), 'meter routing');
assert.ok(levels().every(value=>value===0), 'snapshot clears measured peaks');
load(meterSong(32)); render(4800);
assert.ok(levels()[3]<loudLevels[3], 'meters follow audio amplitude, not just note count');
load(meterSong(96),1,4,48000,6); render(4800);
assert.ok(Math.abs(levels()[3]/loudLevels[3]-Math.pow(10,-6/20))<.001, 'meters follow output gain');
load(short); assert.ok(levels().every(value=>value===0), 'new songs reset meters');
console.log('PASS synthesized channel levels, routing, dynamics, gain, and reset');
const defaultReverb=[.7,.6,.8,0,1];
function withReverb(parameters) {
  load(); e._engine_reverb(...parameters);
  return render(48000);
}
const reverbReference=withReverb(defaultReverb);
for (const parameters of [[.2,.6,.8,0,1],[.7,.1,.8,0,1],[.7,.6,0,0,1],[.7,.6,.8,.2,1],[.7,.6,.8,0,0]]) {
  const actual=withReverb(parameters);
  assert.ok(actual.some((v,i)=>Math.abs(v-reverbReference[i])>.0001), 'reverb parameter changes audio');
  stats(actual);
}
const beforeReverbChange=e._engine_time();
e._engine_reverb(0,1,1,.5,0); stats(render(4800));
assert.ok(Math.abs(e._engine_time()-beforeReverbChange-.1)<1e-8, 'reverb preserves song position');
e._engine_reverb(...defaultReverb);
function withSynth(flags) {
  e._engine_synth_options(...flags); load(demo,1,4,48000,12,0);
  return render(96000);
}
const synthReference=withSynth([0,0,0]);
for (const flags of [[1,0,0],[0,1,0],[0,0,1]]) {
  const actual=withSynth(flags);
  assert.ok(actual.some((v,i)=>Math.abs(v-synthReference[i])>.001), 'synthesis option changes audio');
  stats(actual);
}
assert.deepEqual(withSynth([0,0,0]),synthReference, 'restoring synthesis defaults restores audio');
console.log('PASS all five reverb parameters and three synthesis options affect audio');
const report={engine:'Emscripten WebAssembly in Node',demoSeconds:1,...measured,renderMilliseconds:milliseconds,banks:67,sampleRates:[44100,48000,96000]};
writeFileSync(new URL('engine-results.json',import.meta.url), JSON.stringify(report,null,2)+'\n');
