// Validate standard MIDI before replacing the current song in the C++ engine.
export function parseMidi(input) {
  let bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  if (bytes.length > 8 * 1024 * 1024) throw Error('Please choose a MIDI file smaller than 8 MB.');
  const tag = (b, p) => String.fromCharCode(...b.subarray(p, p + 4));
  if (tag(bytes, 0) === 'RIFF') {
    if (tag(bytes, 8) !== 'RMID') throw Error('This RIFF file is not an RMI song.');
    let found;
    for (let p = 12; p + 8 <= bytes.length;) {
      const length = new DataView(bytes.buffer, bytes.byteOffset + p + 4, 4).getUint32(0, true);
      if (p + 8 + length > bytes.length) throw Error('The RMI file is truncated.');
      if (tag(bytes, p) === 'data') { found = bytes.slice(p + 8, p + 8 + length); break; }
      p += 8 + length + (length & 1);
    }
    if (!found) throw Error('The RMI file contains no MIDI data.');
    bytes = found;
  }
  if (bytes.length < 14 || tag(bytes, 0) !== 'MThd') throw Error('Choose a standard .mid, .midi, or .rmi file.');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerLength = view.getUint32(4), format = view.getUint16(8), tracks = view.getUint16(10), division = view.getUint16(12);
  if (headerLength !== 6 || format > 1 || !tracks || tracks > 256 || (format === 0 && tracks !== 1)) throw Error('This prototype supports MIDI format 0 or 1, with up to 256 tracks.');
  if (!division || division & 0x8000) throw Error('SMPTE-timed MIDI files are not supported in this prototype.');
  let offset = 14, events = 0, maxTick = 0;
  const tempos = [{tick: 0, tempo: 500000}], devices = new Map([['', 0]]);
  const timeSignatures = [], trackDetails = [], instrumentEvents = [], copyrights = new Set();
  const decoder = new TextDecoder();
  const text = (start, length) => decoder.decode(bytes.subarray(start, start + Math.min(length, 2048)))
    .replace(/[\x00-\x1f\x7f]/g, ' ').trim().slice(0, 512);
  let title = '';
  for (let track = 0; track < tracks; track++) {
    if (offset + 8 > bytes.length || tag(bytes, offset) !== 'MTrk') throw Error('Missing MIDI track header.');
    const end = offset + 8 + view.getUint32(offset + 4);
    let p = offset + 8, tick = 0, running = 0, ended = false;
    let device = 0;
    const detail = {name: '', instrumentNames: new Set(), programs: new Set(), percussion: false};
    trackDetails.push(detail);
    if (end > bytes.length) throw Error('The MIDI track is truncated.');
    const byte = () => { if (p >= end) throw Error('The MIDI event is truncated.'); return bytes[p++]; };
    const vlq = () => {
      let n = 0;
      for (let i = 0; i < 4; i++) { const b = byte(); n = n * 128 + (b & 127); if (!(b & 128)) return n; }
      throw Error('Invalid MIDI variable-length value.');
    };
    while (p < end) {
      if (++events > 500000) throw Error('This MIDI file has too many events for the prototype.');
      tick += vlq(); maxTick = Math.max(maxTick, tick);
      let status = byte();
      if (status < 128) { if (!running) throw Error('Invalid MIDI running status.'); p--; status = running; }
      if (status === 255) {
        const type = byte(), length = vlq();
        if (type > 127 || p + length > end) throw Error('Invalid MIDI metadata.');
        if (type === 81) {
          if (length !== 3) throw Error('Invalid tempo event.');
          const tempo = bytes[p] * 65536 + bytes[p + 1] * 256 + bytes[p + 2];
          if (!tempo) throw Error('MIDI tempo cannot be zero.');
          tempos.push({tick, tempo});
        }
        if (type === 47) { if (length !== 0) throw Error('Invalid end-of-track event.'); ended = true; break; }
        if (type === 2) { const value = text(p, length); if (value) copyrights.add(value); }
        if (type === 3) {
          if (!detail.name) detail.name = text(p, length);
          // In format 1 the first track's name is the sequence title. A name
          // on a later instrument track must not become the whole file title.
          if (track === 0 && !title) title = detail.name;
        }
        if (type === 4) { const value = text(p, length); if (value) detail.instrumentNames.add(value); }
        if (type === 88 && length === 4 && bytes[p] > 0 && bytes[p + 1] <= 7)
          timeSignatures.push({tick, numerator: bytes[p], denominator: 2 ** bytes[p + 1]});
        if (type === 9) {
          const name = decoder.decode(bytes.subarray(p, p + length));
          if (!devices.has(name)) devices.set(name, devices.size);
          if (devices.size > 16) throw Error('Too many MIDI devices.');
          device = devices.get(name);
        }
        p += length;
      } else if (status === 240 || status === 247) {
        const length = vlq(); if (p + length > end) throw Error('Truncated MIDI SysEx.'); p += length;
        running = 0;
      } else if (status >= 128 && status <= 239) {
        running = status;
        const count = (status >> 4) === 12 || (status >> 4) === 13 ? 1 : 2;
        const dataStart = p;
        for (let i = 0; i < count; i++) if (byte() > 127) throw Error('Invalid MIDI event data.');
        const kind = status >> 4;
        if (kind === 12 || (kind === 9 && bytes[dataStart + 1] > 0))
          instrumentEvents.push({tick, track, channel: device * 16 + (status & 15),
            program: kind === 12 ? bytes[dataStart] : null});
      } else throw Error('Unsupported MIDI system event.');
    }
    if (!ended) throw Error('MIDI track is missing its end marker.');
    offset = end;
  }
  tempos.sort((a, b) => a.tick - b.tick);
  let previous = 0, tempo = 500000, duration = 0;
  for (const change of tempos) {
    duration += (change.tick - previous) * tempo / division / 1e6;
    previous = change.tick; tempo = change.tempo;
  }
  duration += (maxTick - previous) * tempo / division / 1e6;
  // Program changes can live on a different track from notes on the same
  // channel. Resolve them in musical time rather than independently per track.
  const channelPrograms = new Map();
  instrumentEvents.sort((a, b) => a.tick - b.tick);
  for (const event of instrumentEvents) {
    if (event.program !== null) channelPrograms.set(event.channel, event.program);
    const detail = trackDetails[event.track];
    if (event.channel % 16 === 9) detail.percussion = true;
    else detail.programs.add(channelPrograms.get(event.channel) ?? 0);
  }
  timeSignatures.sort((a, b) => a.tick - b.tick);
  const initialTempo = tempos.filter(change => change.tick === 0).at(-1).tempo;
  const initialSignature = timeSignatures.filter(change => change.tick === 0).at(-1) ?? null;
  return {bytes, title, copyright: [...copyrights].join('\n'), duration, tracks, events,
    initialBpm: 60000000 / initialTempo, initialSignature,
    trackDetails: trackDetails.map(detail => ({...detail,
      instrumentNames: [...detail.instrumentNames], programs: [...detail.programs]}))};
}
