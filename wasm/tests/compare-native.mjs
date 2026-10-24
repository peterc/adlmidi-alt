// Optional comparison against a desktop adlmidi binary. No source files are changed.
import createEngine from '../engine.mjs';
import {readFileSync,writeFileSync,unlinkSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
const nativeBinary=process.argv[2] ? resolve(process.argv[2]) : fileURLToPath(new URL('../../adlmidi',import.meta.url));
const nativeWav=new URL('native-reference.wav',import.meta.url);
const demo=new URL('../assets/copper-circuit.mid',import.meta.url);
const result=spawnSync(nativeBinary,[fileURLToPath(demo),'-nl','-w',fileURLToPath(nativeWav)],{encoding:'utf8',timeout:30000});
if(result.error || result.status!==0) throw result.error || Error(result.stderr);
try {
  const e=await createEngine({wasmBinary:readFileSync(new URL('../engine.wasm',import.meta.url))});
  e.FS.writeFile('/song.mid',readFileSync(demo));e._engine_load(48000,1,4);e._engine_settings(12,1,24,0);
  const native=readFileSync(nativeWav);
  let squaredError=0,referenceEnergy=0,product=0,wasmEnergy=0,maxError=0;
  for(let f=0;f<48000;f+=128) {
    const count=Math.min(128,48000-f),ptr=e._engine_render(count)>>2;
    for(let i=0;i<count*2;i++) {
      const a=e.HEAPF32[ptr+i],b=native.readInt16LE(44+(f*2+i)*2)/32768,delta=a-b;
      squaredError+=delta*delta;referenceEnergy+=b*b;wasmEnergy+=a*a;product+=a*b;maxError=Math.max(maxError,Math.abs(delta));
    }
  }
  const report={seconds:1,maxError,rmsError:Math.sqrt(squaredError/96000),referenceRms:Math.sqrt(referenceEnergy/96000),correlation:product/Math.sqrt(referenceEnergy*wasmEnergy)};
  console.log(report);writeFileSync(new URL('native-comparison.json',import.meta.url),JSON.stringify(report,null,2)+'\n');
} finally { unlinkSync(nativeWav); }
