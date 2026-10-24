import createEngine from './engine.mjs?v=9';

class ADLMIDIProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.engine = null; this.playing = false; this.loaded = false;
    this.frames = 0; this.loop = true;
    this.settings = {gain: 12, reverb: true, wet: 24, room: .7, decay: .6, damping: .8,
      predelay: 0, stereo: 1, tremolo: false, vibrato: false, modulators: false};
    this.port.onmessage = ({data}) => this.command(data);
    createEngine({wasmBinary: options.processorOptions.wasmBytes, print() {}, printErr: text => this.send({type: 'log', text})})
      .then(engine => {
        this.engine = engine;
        const banks = Array.from({length: 67}, (_, i) => engine.UTF8ToString(engine._engine_bank(i)));
        this.send({type: 'ready', banks});
      }).catch(error => this.fail(error));
  }
  send(data) { this.port.postMessage({...data, id: this.loadId}); }
  fail(error) {
    this.playing = false;
    this.send({type: 'error', message: String(error.message || error)});
  }
  settingsChanged() {
    const s = this.settings;
    this.engine._engine_settings(s.gain, Number(s.reverb), s.wet, Number(this.loop));
    this.engine._engine_reverb(s.room, s.decay, s.damping, s.predelay, s.stereo);
  }
  load() {
    const e = this.engine;
    const s = this.settings;
    e._engine_synth_options(Number(s.tremolo), Number(s.vibrato), Number(s.modulators));
    if (!e._engine_load(sampleRate, this.bank, this.cards)) throw Error('The audio engine could not load this MIDI file.');
    this.settingsChanged(); this.loaded = true;
  }
  command(data) {
    if (!this.engine) return;
    try {
      if (data.type === 'load') {
        this.loadId = data.id;
        this.playing = false; this.loaded = false;
        this.engine.FS.writeFile('/song.mid', new Uint8Array(data.bytes));
        this.bank = data.bank; this.cards = data.cards; this.loop = data.loop;
        this.settings = data.settings; this.load(); this.playing = data.play;
        this.send({type: 'loaded', playing: this.playing, fourOps: this.engine._engine_fourops(), id: data.id});
        this.snapshot();
      } else if (data.type === 'play' && this.loaded) {
        if (this.engine._engine_ended()) this.load();
        this.playing = true; this.send({type: 'playing'});
      } else if (data.type === 'pause') {
        this.playing = false; this.send({type: 'paused'});
      } else if (data.type === 'stop' && this.loaded) {
        this.playing = false; this.load(); this.snapshot(); this.send({type: 'stopped'});
      } else if (data.type === 'settings') {
        this.settings = data.settings; this.loop = data.loop; this.settingsChanged();
      }
    } catch (error) { this.fail(error); }
  }
  snapshot() {
    const e = this.engine, ptr = e._engine_notes() >> 2, notes = [];
    for (let i = 0; i < this.cards * 23; i++) {
      const p = ptr + i * 4;
      if (e.HEAP32[p + 3]) notes.push(Array.from(e.HEAP32.subarray(p, p + 4)));
    }
    const programs = Array.from(e.HEAP32.subarray(e._engine_programs() >> 2, (e._engine_programs() >> 2) + 16));
    const levelPtr = e._engine_levels() >> 2;
    const levels = Array.from(e.HEAPF32.subarray(levelPtr, levelPtr + 16));
    this.send({type: 'state', time: e._engine_time(), notes, programs, levels,
      bpm: e._engine_bpm(), signature: e._engine_signature()});
  }
  process(inputs, outputs) {
    const channels = outputs[0];
    if (!channels?.length || !this.engine || !this.playing) return true;
    try {
      const frames = channels[0].length;
      for (let offset = 0; offset < frames; offset += 512) {
        const n = Math.min(512, frames - offset), ptr = this.engine._engine_render(n) >> 2;
        for (let i = 0; i < n; i++) {
          channels[0][offset + i] = this.engine.HEAPF32[ptr + i * 2];
          if (channels[1]) channels[1][offset + i] = this.engine.HEAPF32[ptr + i * 2 + 1];
        }
      }
      if ((this.frames += frames) >= sampleRate / 24) { this.frames = 0; this.snapshot(); }
      if (this.engine._engine_ended()) {
        if (this.loop) { this.load(); this.send({type: 'looped'}); }
        else { this.playing = false; this.send({type: 'ended'}); }
      }
    } catch (error) { this.fail(error); }
    return true;
  }
}
registerProcessor('adlmidi', ADLMIDIProcessor);
