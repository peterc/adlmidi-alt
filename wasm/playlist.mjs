const isMidi = name => /\.(mid|midi|rmi)$/i.test(name);
const sortTracks = tracks => tracks.sort((a, b) => a.path.localeCompare(b.path, undefined, {numeric: true, sensitivity: 'base'}));

export function trackFromURL(value) {
  let url;
  try { url = new URL(value.trim()); }
  catch { throw Error('Enter a complete http:// or https:// MIDI URL.'); }
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password)
    throw Error('Use an http:// or https:// MIDI URL without embedded login details.');
  let name = url.pathname.split('/').pop() || 'remote.mid';
  try { name = decodeURIComponent(name); } catch {}
  let bytes;
  return {name, path: name, read: async () => {
    if (bytes) return bytes;
    let response;
    try {
      response = await fetch(url.href, {credentials: 'omit', signal: AbortSignal.timeout(20000)});
      if (response.ok) bytes = await response.arrayBuffer();
    } catch (err) {
      if (err.name === 'TimeoutError') throw Error('The MIDI download timed out. Try again.');
      throw Error('Could not fetch MIDI. The URL may be unreachable or blocked by CORS. Download the file and use Open MIDI instead.');
    }
    if (!response.ok) throw Error(`The MIDI server returned HTTP ${response.status}.`);
    return bytes;
  }};
}

export function tracksFromFiles(files) {
  return sortTracks(Array.from(files).filter(file => isMidi(file.name)).map(file => ({
    name: file.name, path: file.webkitRelativePath || file.name,
    read: () => file.arrayBuffer(),
  })));
}

// Capture entries synchronously: the browser clears drag data after the event.
// Retain file handles, and read MIDI contents only when a track is selected.
export async function tracksFromDrop(transfer) {
  const roots = Array.from(transfer.items || []).filter(item => item.kind === 'file').map(item => ({
    entry: (item.getAsEntry || item.webkitGetAsEntry)?.call(item), file: item.getAsFile(),
  }));
  const fallback = Array.from(transfer.files || []);
  if (!roots.length) return tracksFromFiles(fallback);
  const tracks = [];
  async function walk(entry, parent = '') {
    const path = parent + entry.name;
    if (entry.isFile && isMidi(entry.name)) {
      tracks.push({name: entry.name, path, read: async () => {
        const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
        return file.arrayBuffer();
      }});
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      for (;;) {
        const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
        if (!batch.length) break;
        for (const child of batch) await walk(child, path + '/');
      }
    }
  }
  for (const {entry, file} of roots) {
    if (entry) await walk(entry);
    else if (file) tracks.push(...tracksFromFiles([file]));
  }
  return sortTracks(tracks);
}
