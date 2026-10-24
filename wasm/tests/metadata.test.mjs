import assert from 'node:assert/strict';
import {readFileSync, writeFileSync} from 'node:fs';
import createEngine from '../engine.mjs';
import {parseMidi} from '../midi.mjs';

const vlq = n => { const out=[n&127]; while(n>>=7) out.unshift((n&127)|128); return out; };
const meta = (type, data, delta=0) => { const bytes=Buffer.from(data); return [...vlq(delta),0xff,type,...vlq(bytes.length),...bytes]; };
const end = delta => [...vlq(delta),0xff,0x2f,0];
function midi(tracks) {
  const header=Buffer.alloc(14); header.write('MThd'); header.writeUInt32BE(6,4);
  header.writeUInt16BE(tracks.length>1?1:0,8); header.writeUInt16BE(tracks.length,10); header.writeUInt16BE(480,12);
  return Buffer.concat([header,...tracks.map(data=>{
    const chunk=Buffer.alloc(8); chunk.write('MTrk'); chunk.writeUInt32BE(data.length,4);
    return Buffer.concat([chunk,Buffer.from(data)]);
  })]);
}
const title='<img src=x onerror=alert(1)> Suite';
const fixture=midi([
  [...meta(3,title),...meta(2,'Copyright Example'),...meta(0x58,[3,2,24,8]),
   ...meta(0x51,[7,161,32]),...meta(0x51,[15,66,64],480),...meta(0x58,[6,3,24,8]),...end(480)],
  [...meta(3,'Lead'),...meta(4,'Soft bell'),0,0xc2,10,0,0x92,60,90,...vlq(960),0x82,60,0,...end(0)],
  [...meta(3,'Drums'),0,0x99,36,90,...vlq(960),0x89,36,0,...end(0)],
]);
const parsed=parseMidi(fixture);
assert.equal(parsed.title,title); assert.equal(parsed.copyright,'Copyright Example');
assert.equal(parsed.initialBpm,120); assert.deepEqual(parsed.initialSignature,{tick:0,numerator:3,denominator:4});
assert.equal(parsed.duration,1.5);
assert.deepEqual(parsed.trackDetails[1],{name:'Lead',instrumentNames:['Soft bell'],programs:[10],percussion:false});
assert.equal(parsed.trackDetails[2].percussion,true); assert.deepEqual(parsed.trackDetails[2].programs,[]);
const shared=parseMidi(midi([
  [0,0xc0,24,...end(960)],
  [...meta(3,'Only a track'),...vlq(480),0x90,60,90,...end(480)],
]));
assert.equal(shared.title,''); assert.deepEqual(shared.trackDetails[1].programs,[24]);
const defaults=parseMidi(midi([[0,0x90,60,80,...end(480)]]));
assert.equal(defaults.initialBpm,120); assert.equal(defaults.initialSignature,null);
assert.deepEqual(defaults.trackDetails[0].programs,[0]); assert.equal(defaults.copyright,'');
const separateDevices=parseMidi(midi([
  [...meta(9,'Other device'),0,0xc0,24,...end(480)],
  [0,0x90,60,90,...end(480)],
]));
assert.deepEqual(separateDevices.trackDetails[1].programs,[0]);
console.log('PASS embedded title/copyright, track names/instruments, shared channels, devices, and defaults');

const e=await createEngine({wasmBinary:readFileSync(new URL('../engine.wasm',import.meta.url))});
function load(bytes,loop=0) {
  e.FS.writeFile('/song.mid',bytes); assert.equal(e._engine_load(48000,1,4),1);
  e._engine_settings(12,1,24,loop);
}
function render(frames) { for(let f=0;f<frames;f+=128)e._engine_render(Math.min(128,frames-f)); }
load(fixture); render(12000);
assert.ok(Math.abs(e._engine_bpm()-120)<1e-8); assert.equal(e._engine_signature(),(3<<8)|2);
render(24000);
assert.ok(Math.abs(e._engine_bpm()-60)<1e-8); assert.equal(e._engine_signature(),(6<<8)|3);
load(midi([[0,0x90,60,80,...end(480)]])); render(4800);
assert.equal(e._engine_signature(),0); assert.ok(Math.abs(e._engine_bpm()-120)<1e-8);
const loop=midi([[
  ...meta(0x58,[4,2,24,8]),...meta(6,'loopStart'),0,0x90,60,90,
  ...meta(0x51,[15,66,64],240),...meta(0x58,[3,2,24,8]),0,0x80,60,0,
  ...meta(6,'loopEnd',240),...end(0),
]]);
load(loop,1); render(16800);
assert.ok(Math.abs(e._engine_bpm()-60)<1e-8); assert.equal(e._engine_signature(),(3<<8)|2);
render(21600);
assert.ok(Math.abs(e._engine_bpm()-120)<1e-8); assert.equal(e._engine_signature(),(4<<8)|2);
console.log('PASS live tempo/signature changes, song reset, and marker-loop restoration');
const channelPrograms = () => {
  const ptr = e._engine_programs() >> 2;
  return Array.from(e.HEAP32.subarray(ptr, ptr + 16));
};
load(midi([
  [...meta(9, 'Device A'), 0,0xc0,71, 0,0x90,60,90,
   ...vlq(480),0xc0,73, 0,0x90,64,90, ...end(480)],
  [...meta(9, 'Device B'), 0,0xc1,32, 0,0x91,48,90, ...end(960)],
]));
render(4800);
assert.equal(channelPrograms()[0], 71, 'Device A maps to visible channel 1');
assert.equal(channelPrograms()[1], 32, 'Device B maps to visible channel 2');
render(24000);
assert.equal(channelPrograms()[0], 73, 'Live program changes update the visible channel');
load(midi([[...Array.from({length: 20}, (_, i) => meta(9, `Device ${i}`)).flat(),
  0,0xc2,11, 0,0x92,60,90, ...end(480)]]));
render(4800);
assert.equal(channelPrograms()[2], 11, 'Devices beyond the old 256-channel snapshot are visible');
load(midi([[0,0x90,60,90,...end(480)]])); render(4800);
assert.deepEqual(channelPrograms(), Array(16).fill(0), 'Loading another song resets programs');
console.log('PASS virtual-device channel programs, live changes, large device offsets, and reset');
writeFileSync(new URL('metadata.mid',import.meta.url),fixture);
