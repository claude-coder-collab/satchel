// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Node test of the JavaScript binding: node test/glue.mjs path/to/satchel.mjs
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { Satchel, SatchelError } from '../src/satchel.mjs';

const { default: factory } = await import(pathToFileURL(process.argv[2]).href);
const sat = await Satchel.load({ factory, threads: 4 });

function makeWav(frames, channels) {
    const data = new Uint8Array(frames * channels * 2);
    const view = new DataView(data.buffer);
    for (let f = 0; f < frames; f++) {
        for (let c = 0; c < channels; c++) {
            view.setInt16((f * channels + c) * 2, Math.trunc(8000 * Math.sin(f * (0.01 + 0.002 * c))), true);
        }
    }
    const header = new Uint8Array(44);
    const h = new DataView(header.buffer);
    const ascii = (o, s) => [...s].forEach((ch, i) => (header[o + i] = ch.charCodeAt(0)));
    ascii(0, 'RIFF');
    h.setUint32(4, 36 + data.length, true);
    ascii(8, 'WAVE');
    ascii(12, 'fmt ');
    h.setUint32(16, 16, true);
    h.setUint16(20, 1, true);
    h.setUint16(22, channels, true);
    h.setUint32(24, 48000, true);
    h.setUint32(28, 48000 * channels * 2, true);
    h.setUint16(32, channels * 2, true);
    h.setUint16(34, 16, true);
    ascii(36, 'data');
    h.setUint32(40, data.length, true);
    const out = new Uint8Array(44 + data.length);
    out.set(header);
    out.set(data, 44);
    return out;
}

const text = new TextEncoder().encode('hello from javascript '.repeat(2000));
const wav = makeWav(30000, 2);
const multi = makeWav(5000, 10);
const noise = new Uint8Array(3 << 20).map((_, i) => (i * 2654435761) >>> 24);

// Plan and build.
const plan = sat.plan([
    { path: 'session', directory: true, lastModified: 1700000000000 },
    { path: 'session/notes.txt', data: text, lastModified: 1700000000000 },
    { path: 'session/take1.wav', data: wav, lastModified: 1700000000000 },
    { path: 'session/multi.wav', data: multi, lastModified: 1700000000000 },
    { path: 'session/noise.bin', data: noise, lastModified: 1700000000000 },
]);
assert.ok(plan.executable);
const names = plan.entries.map((e) => e.output_name);
assert.ok(names.includes('session/take1.flac'));
assert.ok(names.includes('session/multi_ch10.flac'));
let progressCalls = 0;
const result = plan.build(new (await import('../src/satchel.mjs')).MemoryWriter(), { onProgress: () => (progressCalls++, true) });
assert.equal(result.status, 'OK');
assert.ok(progressCalls > 0);
const zip = plan.buildBytes();
assert.equal(zip[0], 0x50);

// Open, list, extract (parallel, through callbacks), verify.
const archive = sat.open(zip);
const entries = archive.entries;
assert.ok(entries.find((e) => e.name === 'session/take1.flac').flac_restorable);
assert.ok(archive.describe().app_version.startsWith('Satchel'));
const { files, result: x } = archive.extractToMemory();
assert.equal(x.statusCode, 0, JSON.stringify(x.outcomes));
assert.deepEqual(files.get('session/take1.wav'), wav);
assert.deepEqual(files.get('session/multi.wav'), multi);
assert.deepEqual(files.get('session/notes.txt'), text);
assert.deepEqual(files.get('session/noise.bin'), noise);
const verified = archive.verify();
assert.equal(verified.statusCode, 0);
archive.free();

// Conflicts.
const clash = sat.plan([
    { path: 'a.txt', data: new Uint8Array([1]) },
    { path: 'A.TXT', data: new Uint8Array([2]) },
]);
assert.equal(clash.executable, false);
assert.throws(() => clash.buildBytes(), (e) => e instanceof SatchelError && e.statusName === 'CONFLICTS_UNRESOLVED');
clash.resolve([{ entry: clash.conflicts[0].entries[1], action: 'rename', newName: 'b.txt' }]);
assert.ok(clash.executable);
assert.ok(clash.buildBytes().length > 0);

// Paths in the module's file system (memory in Node; OPFS in browsers).
plan.buildToPath('/tmp/out.zip');
const fsBytes = sat.m.FS.readFile('/tmp/out.zip');
const fromFs = sat.open(fsBytes);
const x2 = fromFs.extractToPath('/tmp/extracted');
assert.equal(x2.statusCode, 0, JSON.stringify(x2.outcomes));
assert.deepEqual(sat.m.FS.readFile('/tmp/extracted/session/take1.wav'), wav);
fromFs.free();

assert.throws(() => sat.open(new Uint8Array(100)), (e) => e.statusName === 'CORRUPT_ARCHIVE');
plan.free();
clash.free();
console.log(`js glue ok: ${zip.length} byte archive, ${entries.length} entries`);
process.exit(0);
