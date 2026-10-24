import {parseMidi} from './midi.mjs?v=8';
import {tracksFromFiles, tracksFromDrop, trackFromURL} from './playlist.mjs?v=2';
const $ = id => document.getElementById(id);
const controls = {play: $('play'), stop: $('stop'), bank: $('bank'), cards: $('cards'), gain: $('gain'), wet: $('wet'), reverb: $('reverb'), loop: $('loop')};
const reverbShape = ['room', 'decay', 'damping', 'predelay', 'stereo'];
const synthOptions = ['tremolo', 'vibrato', 'modulators'];
for (const id of [...reverbShape, ...synthOptions]) controls[id] = $(id);
let song, context, node, analyser, volume, engineReady, loaded = false, playing = false, busy = false, loadId = 0, frameData;
let programs = [], activeNotes = [], elapsed = 0, state = 'ready';
let songSelectionId = 0;
let playlist = [], trackIndex = -1, selectionPending = false;
function renderPlaylist() {
  $('playlist-count').textContent = `${playlist.length} track${playlist.length === 1 ? '' : 's'}`;
  $('playlist-list').replaceChildren(...playlist.map((track, index) => {
    const item = document.createElement('li'), button = document.createElement('button');
    button.type = 'button'; button.textContent = track.path;
    button.title = track.error ? `${track.path}: ${track.error}` : track.path;
    button.className = 'playlist-track';
    if (track.error) button.classList.add('invalid');
    if (index === trackIndex) button.setAttribute('aria-current', 'true');
    button.addEventListener('click', () => selectTrack(index));
    item.append(button); return item;
  }));
  updateButtons();
}
const meterTargets = new Float32Array(16), meterDisplay = new Float32Array(16);
const meterSegmentCount = 12, meterSteps = new Uint8Array(16);
let meterTime = performance.now();
const channelEls = Array.from({length: 16}, (_, i) => {
  const el = document.createElement('div'); el.className = 'channel';
  el.innerHTML = `<span class="channel-number">${String(i + 1).padStart(2, '0')}</span><span class="channel-name">${i === 9 ? 'Percussion' : 'Grand Piano'}</span><div class="channel-meter" role="meter" aria-label="Channel ${i + 1} level" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"><div class="channel-meter-fill" aria-hidden="true">${'<span class="meter-segment"></span>'.repeat(meterSegmentCount)}</div></div>`;
  $('channels').append(el); return el;
});
const meterEls = channelEls.map(el => el.querySelector('.channel-meter'));
const meterSegments = channelEls.map(el => Array.from(el.querySelectorAll('.meter-segment')));
const formatTime = value => `${Math.floor(value / 60)}:${String(Math.floor(value % 60)).padStart(2, '0')}`;
function displayMusicalInfo(bpm, signature) {
  $('bpm').textContent = Number.isFinite(bpm) && bpm > 0 ? `${Number(bpm.toFixed(1))} BPM` : '';
  $('time-signature').hidden = !signature;
  $('time-signature').textContent = signature ? `${signature.numerator}/${signature.denominator}` : '';
}
function renderFileInfo() {
  if (!song) return;
  $('title-row').hidden = !song.title;
  $('file-title').textContent = song.title;
  $('copyright-row').hidden = !song.copyright;
  $('file-copyright').textContent = song.copyright;
  $('track-list').replaceChildren(...song.trackDetails.map((track, i) => {
    const row = document.createElement('tr'), name = document.createElement('td'), instruments = document.createElement('td');
    name.textContent = track.name ? `${i + 1}. ${track.name}` : `Track ${i + 1}`;
    const names = [...track.instrumentNames, ...track.programs.map(program => programs[program] || `Program ${program + 1}`)];
    if (track.percussion) names.push('Percussion');
    instruments.textContent = [...new Set(names)].join(', ') || '—';
    row.append(name, instruments); return row;
  }));
}
const settings = () => ({
  gain: +controls.gain.value, wet: +controls.wet.value, reverb: controls.reverb.checked,
  room: +controls.room.value / 100, decay: +controls.decay.value / 100,
  damping: +controls.damping.value / 100, predelay: +controls.predelay.value / 1000,
  stereo: +controls.stereo.value / 100,
  tremolo: controls.tremolo.checked, vibrato: controls.vibrato.checked, modulators: controls.modulators.checked,
});
function setStatus(text, nextState = state) { $('status').textContent = text; state = nextState; }
function clearError() { $('error').hidden = true; }
function error(err) {
  $('error').textContent = err.message || String(err); $('error').hidden = false;
  busy = false; updateButtons();
}
function updateButtons() {
  controls.play.disabled = !song || busy || selectionPending; controls.stop.disabled = !loaded || busy || selectionPending;
  $('previous').disabled = trackIndex <= 0 || busy || selectionPending;
  $('next').disabled = trackIndex < 0 || trackIndex >= playlist.length - 1 || busy || selectionPending;
  controls.play.innerHTML = playing ? 'Ⅱ <span>Pause</span>' : '▶ <span>Play</span>';
  controls.play.setAttribute('aria-label', playing ? 'Pause playback' : 'Play MIDI');
}
function displayTime(seconds) {
  elapsed = seconds; $('elapsed').textContent = formatTime(song?.duration ? Math.min(seconds, song.duration) : seconds);
  const progress = song?.duration ? Math.min(100, seconds / song.duration * 100) : 0;
  $('progress-fill').style.width = `${progress}%`;
  document.querySelector('.progress').setAttribute('aria-valuenow', String(Math.round(progress)));
}
function displayNotes(notes = [], patches, levels = []) {
  activeNotes = notes;
  channelEls.forEach((el, i) => {
    // A 48 dB display range makes quieter patches visible without tying the
    // meter to note count. The worklet supplies measured synth voice peaks.
    meterTargets[i] = levels[i] > 0 ? Math.max(0, Math.min(1, 1 + 20 * Math.log10(levels[i]) / 48)) : 0;
    if (patches) {
      const name = i === 9 ? 'Percussion' : programs[patches[i]] || `Program ${patches[i] + 1}`;
      const label = el.querySelector('.channel-name');
      label.textContent = name; label.title = name;
    }
  });
}
function setPlaying(value) { playing = value; updateButtons(); }
function ramp(value) {
  if (!volume) return;
  const now = context.currentTime;
  volume.gain.cancelScheduledValues(now); volume.gain.setValueAtTime(volume.gain.value, now);
  volume.gain.linearRampToValueAtTime(value, now + .015);
}
async function ensureAudio(waitForResume = true) {
  // Treat this as a music player on Safari/iOS, where the default Web Audio
  // session can be silenced by the phone's Ring/Silent switch.
  try {
    if (navigator.audioSession) navigator.audioSession.type = 'playback';
  } catch { /* Optional API: unsupported session settings must not block audio. */ }
  if (engineReady) {
    const resumed = context.resume().catch(err => err);
    if (waitForResume) { const err = await resumed; if (err) throw err; }
    return engineReady;
  }
  if (!window.AudioContext || !window.AudioWorkletNode || !window.isSecureContext) throw Error('Use a current browser at localhost or an HTTPS address to play audio.');
  context = new AudioContext({latencyHint: 'playback', sampleRate: 48000});
  // Resume synchronously from the Play click, before awaiting downloads.
  const resume = context.resume().catch(err => err);
  engineReady = (async () => {
    const response = await fetch('./engine.wasm?v=9');
    if (!response.ok) throw Error('Could not load engine.wasm. Run build.sh if it is missing.');
    const wasmBytes = await response.arrayBuffer();
    await context.audioWorklet.addModule('./worklet.mjs?v=10');
    node = new AudioWorkletNode(context, 'adlmidi', {numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2], processorOptions: {wasmBytes}});
    volume = context.createGain(); volume.gain.value = 1;
    analyser = context.createAnalyser(); analyser.fftSize = 2048; frameData = new Float32Array(analyser.fftSize);
    node.connect(volume).connect(analyser).connect(context.destination);
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(Error('Audio engine startup timed out. Try reloading the page.')), 15000);
      node.port.onmessage = ({data}) => {
        if (data.type === 'ready') { clearTimeout(timeout); resolve(); }
        else if (data.type === 'error') { clearTimeout(timeout); reject(Error(data.message)); handleMessage(data); }
        else handleMessage(data);
      };
      node.onprocessorerror = () => { clearTimeout(timeout); const err = Error('The audio processor stopped. Reload the page to restart it.'); reject(err); setPlaying(false); error(err); };
    });
    $('sample-rate').textContent = `${context.sampleRate / 1000} kHz · Stereo`;
  })().catch(async err => { engineReady = null; node = null; await context.close(); throw err; });
  await engineReady;
  if (waitForResume) { const err = await resume; if (err) throw err; }
  return engineReady;
}
function handleMessage(data) {
  if (data.id !== undefined && data.id !== loadId) return;
  if (data.type === 'loaded' && data.id === loadId) {
    loaded = true; busy = false; setPlaying(data.playing); ramp(1);
    setStatus(playing ? 'Playing' : context.state === 'suspended' ? 'Press Play to enable audio' : 'Ready to play', playing ? 'playing' : 'ready');
    $('voices').textContent = `${data.fourOps} four-op / ${+controls.cards.value * 18 - data.fourOps * 2} two-op voices`;
  } else if (data.type === 'state') {
    displayTime(data.time); displayNotes(data.notes, data.programs, data.levels);
    if (data.time > 0) displayMusicalInfo(data.bpm, data.signature ? {
      numerator: data.signature >> 8, denominator: 2 ** (data.signature & 255),
    } : null);
    else if (song) displayMusicalInfo(song.initialBpm, song.initialSignature);
  } else if (data.type === 'playing') { busy = false; setPlaying(true); ramp(1); setStatus('Playing', 'playing'); }
  else if (data.type === 'paused') { busy = false; setPlaying(false); setStatus('Paused', 'paused'); }
  else if (data.type === 'stopped') { busy = false; setPlaying(false); displayNotes(); displayTime(0); setStatus('Ready to play', 'ready'); }
  else if (data.type === 'ended') {
    setPlaying(false); displayNotes(); displayTime(song.duration); setStatus('Finished', 'ended');
    if (!selectionPending && !controls.loop.checked && trackIndex + 1 < playlist.length)
      selectTrack(trackIndex + 1, {skipInvalid: true});
  }
  else if (data.type === 'looped') displayTime(0);
  else if (data.type === 'error') { setPlaying(false); setStatus('Playback error', 'error'); error(Error(data.message)); }
}
function loadIntoEngine(play) {
  const bytes = song.bytes.slice().buffer;
  loaded = false; busy = true; updateButtons(); setStatus('Loading song…', 'loading');
  node.port.postMessage({type: 'load', id: ++loadId, bytes, bank: +controls.bank.value, cards: +controls.cards.value, loop: controls.loop.checked, settings: settings(), play}, [bytes]);
}
function chooseSong(parsed, filename, demo = false, autoplay = false) {
  song = {...parsed, filename, demo}; clearError();
  $('song-title').textContent = filename.replace(/\.(mid|midi|rmi)$/i, '');
  $('song-title').title = filename;
  $('song-detail').textContent = `${demo ? 'Demo · ' : ''}${parsed.tracks} track${parsed.tracks === 1 ? '' : 's'}`;
  $('duration').textContent = formatTime(parsed.duration); displayTime(0); displayNotes([], new Array(16).fill(0));
  displayMusicalInfo(parsed.initialBpm, parsed.initialSignature); renderFileInfo();
  const resume = autoplay || playing; setPlaying(false); loaded = false;
  if (node) loadIntoEngine(resume);
  else { setStatus('Ready to play', 'ready'); updateButtons(); }
}
async function selectTrack(index, {list = playlist, autoplay = true, skipInvalid = false,
  selection = ++songSelectionId, audioReady} = {}) {
  if (!list[index]) return;
  selectionPending = true; updateButtons();
  audioReady ??= autoplay ? ensureAudio(false).then(() => null, err => err) : Promise.resolve(null);
  let lastError;
  try {
    for (; index < list.length; index++) {
      const track = list[index];
      let parsed;
      try { parsed = parseMidi(await track.read()); }
      catch (err) {
        if (selection !== songSelectionId) return;
        track.error = err.message || String(err); lastError = Error(`${track.path}: ${track.error}`);
        if (!skipInvalid) throw lastError;
        continue;
      }
      if (selection !== songSelectionId) return;
      const startupError = await audioReady;
      if (selection !== songSelectionId) return;
      if (startupError) throw startupError;
      delete track.error;
      playlist = list; trackIndex = index; renderPlaylist();
      chooseSong(parsed, track.name, !!track.demo, autoplay && context?.state === 'running');
      const listElement = $('playlist-list'), current = listElement.querySelector('[aria-current]');
      if (current && (current.offsetTop < listElement.scrollTop || current.offsetTop + current.offsetHeight > listElement.scrollTop + listElement.clientHeight))
        listElement.scrollTop = current.offsetTop;
      return true;
    }
    throw lastError || Error('No playable MIDI files found.');
  } catch (err) {
    if (selection === songSelectionId) { renderPlaylist(); error(err); }
  } finally {
    if (selection === songSelectionId) { selectionPending = false; updateButtons(); }
  }
}
async function loadDemo() {
  const selection = ++songSelectionId;
  try {
    const response = await fetch('./assets/demo-playlist.json');
    if (!response.ok) throw Error('Could not load the demo playlist. Run build.sh first.');
    const names = await response.json();
    if (selection !== songSelectionId) return;
    return selectTrack(0, {selection, autoplay: false, list: names.map(name => ({
      name, path: name, demo: true, read: async () => {
        const r = await fetch(`./assets/${name}`);
        if (!r.ok) throw Error('Could not load the bundled demo.');
        return r.arrayBuffer();
      },
    }))});
  } catch (err) { if (selection === songSelectionId) error(err); }
}
controls.play.addEventListener('click', async () => {
  const selection = songSelectionId, commandLoadId = loadId;
  clearError(); busy = true; updateButtons();
  try {
    if (playing) { ramp(0); setTimeout(() => {
      if (commandLoadId === loadId) node.port.postMessage({type: 'pause'});
    }, 20); return; }
    setStatus('Starting audio…', 'loading'); await ensureAudio();
    if (selection !== songSelectionId) return;
    if (!loaded) loadIntoEngine(true);
    else node.port.postMessage({type: 'play'});
  } catch (err) { setStatus('Could not start audio', 'error'); error(err); }
});
controls.stop.addEventListener('click', () => {
  const commandLoadId = loadId;
  busy = true; updateButtons(); ramp(0);
  setTimeout(() => {
    if (commandLoadId === loadId) node.port.postMessage({type: 'stop'});
  }, 20);
});
function restartForSettings() {
  clearError(); if (node && song) loadIntoEngine(playing);
  else $('voices').textContent = 'Auto voice allocation';
}
for (const control of [controls.bank, controls.cards, ...synthOptions.map(id => controls[id])])
  control.addEventListener('change', restartForSettings);
function updateSettingsLabels() {
  $('gain-value').textContent = `${+controls.gain.value >= 0 ? '+' : ''}${controls.gain.value} dB`;
  $('wet-value').textContent = `+${controls.wet.value} dB`;
  for (const id of reverbShape) $(id + '-value').textContent = controls[id].value + (id === 'predelay' ? ' ms' : '%');
  for (const id of ['wet', ...reverbShape]) controls[id].disabled = !controls.reverb.checked;
}
function sendSettings() {
  node?.port.postMessage({type: 'settings', settings: settings(), loop: controls.loop.checked});
}
for (const control of [controls.gain, controls.wet, controls.reverb, controls.loop]) control.addEventListener('input', () => {
  updateSettingsLabels(); sendSettings();
});
for (const id of reverbShape) {
  controls[id].addEventListener('input', updateSettingsLabels);
  // Reverb geometry allocates delay buffers: commit once at the end of a drag.
  controls[id].addEventListener('change', sendSettings);
}
$('reset-settings').addEventListener('click', () => {
  for (const [id, value] of Object.entries({bank: 1, cards: 4, gain: 12, wet: 24, room: 70, decay: 60, damping: 80, predelay: 0, stereo: 100})) controls[id].value = value;
  for (const id of synthOptions) controls[id].checked = false;
  controls.reverb.checked = true;
  updateSettingsLabels(); restartForSettings();
});
updateSettingsLabels();
async function openTracks(tracks, autoplay = false) {
  const selection = ++songSelectionId;
  selectionPending = true; updateButtons();
  // Unlock during the drop gesture, before directory traversal or file reading.
  const audioReady = autoplay ? ensureAudio(false).then(() => null, err => err) : null;
  try {
    const list = await tracks;
    if (selection !== songSelectionId) return;
    if (!list.length) throw Error('No MIDI files found. Choose .mid, .midi, or .rmi files.');
    await selectTrack(0, {list, autoplay, skipInvalid: true, selection, audioReady});
  } catch (err) {
    if (selection === songSelectionId) error(err);
  } finally {
    if (selection === songSelectionId) { selectionPending = false; updateButtons(); }
  }
}
for (const id of ['file', 'folder']) $(id).addEventListener('change', event => {
  if (event.target.files.length) openTracks(tracksFromFiles(event.target.files));
  event.target.value = '';
});
$('previous').addEventListener('click', () => selectTrack(trackIndex - 1));
$('next').addEventListener('click', () => selectTrack(trackIndex + 1));
function showURLForm(show) {
  $('url-form').hidden = !show;
  $('open-url').setAttribute('aria-expanded', String(show));
  if (show) $('midi-url').focus();
  else $('open-url').focus();
}
$('open-url').addEventListener('click', () => showURLForm($('url-form').hidden));
$('close-url').addEventListener('click', () => showURLForm(false));
$('midi-url').addEventListener('keydown', event => {
  if (event.key === 'Escape') { event.preventDefault(); showURLForm(false); }
});
$('url-form').addEventListener('submit', async event => {
  event.preventDefault();
  let track;
  try { track = trackFromURL($('midi-url').value); }
  catch (err) { error(err); return; }
  const button = $('load-url');
  button.disabled = true; button.textContent = 'Loading…'; clearError();
  try {
    if (await selectTrack(0, {list: [track]})) showURLForm(false);
  } finally { button.disabled = false; button.textContent = 'Open'; }
});
let dragDepth = 0;
const isFileDrag = event => Array.from(event.dataTransfer?.types || []).includes('Files');
function clearDrag() { dragDepth = 0; document.body.classList.remove('file-dragging'); }
window.addEventListener('dragenter', event => {
  if (!isFileDrag(event)) return;
  event.preventDefault(); dragDepth++;
  document.body.classList.add('file-dragging');
});
window.addEventListener('dragover', event => {
  if (!isFileDrag(event)) return;
  event.preventDefault(); event.dataTransfer.dropEffect = 'copy';
  document.body.classList.add('file-dragging');
});
window.addEventListener('dragleave', event => {
  if (!isFileDrag(event)) return;
  if (--dragDepth <= 0) clearDrag();
});
window.addEventListener('drop', event => {
  clearDrag();
  if (!event.dataTransfer || !isFileDrag(event)) return;
  event.preventDefault(); openTracks(tracksFromDrop(event.dataTransfer), true);
});
window.addEventListener('dragend', clearDrag);
window.addEventListener('blur', clearDrag);
const canvas = $('scope'), ctx = canvas.getContext('2d');
function drawScope(now = performance.now()) {
  const dt = Math.min(.1, Math.max(0, (now - meterTime) / 1000)); meterTime = now;
  for (let i = 0; i < 16; i++) {
    const target = playing ? meterTargets[i] : 0;
    meterDisplay[i] += (target - meterDisplay[i]) * (1 - Math.exp(-dt / (target > meterDisplay[i] ? .025 : .18)));
    if (meterDisplay[i] < .001) meterDisplay[i] = 0;
    const steps = Math.ceil(meterDisplay[i] * meterSegmentCount);
    if (steps !== meterSteps[i]) {
      meterSegments[i].forEach((segment, index) => segment.classList.toggle('lit', index < steps));
      meterSteps[i] = steps;
    }
    meterEls[i].setAttribute('aria-valuenow', String(Math.round(meterDisplay[i] * 100)));
  }
  const rect = canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(rect.width * dpr) || canvas.height !== Math.round(rect.height * dpr)) {
    canvas.width = Math.round(rect.width * dpr); canvas.height = Math.round(rect.height * dpr);
  }
  const w = rect.width, h = rect.height; ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = '#262c34'; ctx.lineWidth = 1; ctx.beginPath();
  for (let x = 0; x < w; x += 24) { ctx.moveTo(x, 20); ctx.lineTo(x, h); }
  for (let y = 28; y < h; y += 16) { ctx.moveTo(0, y); ctx.lineTo(w, y); }
  ctx.stroke();
  if (analyser) analyser.getFloatTimeDomainData(frameData);
  ctx.strokeStyle = playing ? '#8aaecb' : '#4b5f70'; ctx.lineWidth = 1; ctx.beginPath();
  const centre = h * .62;
  for (let x = 0; x < w; x++) {
    const sample = playing && frameData ? frameData[Math.floor(x / w * frameData.length)] : 0;
    const y = centre + sample * h * .34;
    if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke(); requestAnimationFrame(drawScope);
}
drawScope();
// Read-only diagnostics for local verification; no data leaves this page.
window.adlmidi = {get state() { return state; }, get time() { return elapsed; }, get notes() { return activeNotes; }, get context() { return context; }, get analyser() { return analyser; }, get playing() { return playing; }};
try {
  const [bankResponse, programResponse] = await Promise.all([fetch('./assets/banks.json'), fetch('./assets/programs.json')]);
  if (!bankResponse.ok || !programResponse.ok) throw Error('Could not load the instrument lists.');
  const banks = await bankResponse.json(); programs = await programResponse.json();
  renderFileInfo();
  controls.bank.replaceChildren(...banks.map((name, i) => new Option(`${i} · ${name}`, i, false, i === 1)));
  if (!songSelectionId) await loadDemo();
} catch (err) { error(err); }
