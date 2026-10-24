import assert from 'node:assert/strict';
import {tracksFromFiles, tracksFromDrop, trackFromURL} from '../playlist.mjs';

for (const url of ['invalid', 'javascript:alert(1)', 'file:///tmp/song.mid', 'https://user:password@example.com/a.mid'])
  assert.throws(() => trackFromURL(url));
assert.equal(trackFromURL(' https://example.com/a%20song.mid?download=1 ').name, 'a song.mid');
assert.equal(trackFromURL('https://example.com/').name, 'remote.mid');

let reads = 0;
const file = (name, path = name) => ({name, webkitRelativePath: path, arrayBuffer: async () => { reads++; return name; }});
const entry = name => ({name, isFile: true, file: resolve => resolve(file(name))});
const directory = (name, batches) => ({name, isDirectory: true, createReader() {
  let i = 0;
  return {readEntries: resolve => resolve(batches[i++] || [])};
}});
const collection = tracksFromFiles([file('10.mid'), file('2.MIDI'), file('notes.txt'), file('same.rmi', 'sub/same.rmi')]);
assert.deepEqual(collection.map(t => t.path), ['2.MIDI', '10.mid', 'sub/same.rmi']);
assert.equal(reads, 0);
assert.equal(await collection[0].read(), '2.MIDI');
assert.equal(reads, 1);

const root = directory('music', [
  Array.from({length: 100}, (_, i) => entry(`${i + 1}.mid`)),
  [entry('101.mid'), entry('readme.txt'), directory('nested', [[entry('1.rmi')]])],
]);
const dropped = await tracksFromDrop({items: [{kind: 'file', webkitGetAsEntry: () => root, getAsFile: () => null}]});
assert.equal(dropped.length, 102);
assert.equal(dropped[100].path, 'music/101.mid');
assert.equal(dropped[101].path, 'music/nested/1.rmi');
assert.equal(reads, 1, 'Directory enumeration must not read MIDI contents');
assert.equal(await dropped[101].read(), '1.rmi');
const fallback = await tracksFromDrop({items: [{kind: 'file', getAsFile: () => file('plain.mid')}], files: []});
assert.deepEqual(fallback.map(t => t.path), ['plain.mid']);
assert.equal((await tracksFromDrop({files: [file('fallback.mid')]}))[0].name, 'fallback.mid');
await assert.rejects(tracksFromDrop({items: [{kind: 'file', getAsFile: () => null, getAsEntry: () => ({
  name: 'denied', isDirectory: true, createReader: () => ({readEntries: (_, reject) => reject(Error('denied'))}),
})}]}), /denied/);
console.log('PASS file filtering, natural sorting, relative paths, recursive batches, lazy reads, fallbacks and read errors');
