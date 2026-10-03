// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';

const modulePath = process.argv[2];
const { default: createSatchel } = await import(pathToFileURL(modulePath).href);
const zp = await createSatchel();

const call = (name, ret, types, args) => zp.ccall(name, ret, types, args);
const str = (s) => {
    const n = zp.lengthBytesUTF8(s) + 1;
    const p = zp._malloc(n);
    zp.stringToUTF8(s, p, n);
    return p;
};

const ctx = call('zp_context_create', 'number', ['number', 'bigint'], [2, 0n]);
assert.ok(ctx);
const input = call('zp_input_memory', 'number', [], []);
const payload = new TextEncoder().encode('hello from the browser build '.repeat(1000));
const buf = zp._malloc(payload.length);
zp.HEAPU8.set(payload, buf);
const name = str('docs/hello.txt');
assert.equal(call('zp_input_memory_add_file', 'number', ['number', 'number', 'number', 'number', 'bigint', 'number'],
    [input, name, buf, payload.length, 1700000000n, 0o644]), 0);

const plan = call('zp_plan_create', 'number', ['number', 'number', 'number'], [ctx, input, 0]);
assert.ok(plan, zp.UTF8ToString(call('zp_last_error', 'number', [], [])));
assert.equal(call('zp_plan_executable', 'number', ['number'], [plan]), 1);

const out = call('zp_stream_memory', 'number', [], []);
const status = call('zp_build', 'number', ['number', 'number', 'number', 'number', 'number', 'number'], [plan, out, 0, 0, 0, 0]);
assert.equal(status, 0, zp.UTF8ToString(call('zp_last_error', 'number', [], [])));

const dataPtr = zp._malloc(8);
const lenPtr = zp._malloc(8);
assert.equal(call('zp_stream_memory_data', 'number', ['number', 'number', 'number'], [out, dataPtr, lenPtr]), 0);
const zipPtr = zp.getValue(dataPtr, '*');
const zipLen = zp.getValue(lenPtr, 'i32');
const zip = zp.HEAPU8.slice(zipPtr, zipPtr + zipLen);
assert.equal(zip[0], 0x50);
assert.equal(zip[1], 0x4b);
assert.ok(zipLen < payload.length / 4, 'deflate should shrink repetitive text');

const zipCopy = zp._malloc(zip.length);
zp.HEAPU8.set(zip, zipCopy);
const rin = call('zp_stream_memory_from', 'number', ['number', 'number'], [zipCopy, zip.length]);
const reader = call('zp_reader_open', 'number', ['number', 'number'], [ctx, rin]);
assert.ok(reader);
assert.equal(call('zp_reader_entry_count', 'number', ['number'], [reader]), 1);

for (const [fn, p] of [['zp_reader_free', reader], ['zp_stream_free', rin], ['zp_stream_free', out], ['zp_plan_free', plan],
    ['zp_input_free', input], ['zp_context_free', ctx]]) {
    call(fn, null, ['number'], [p]);
}
for (const p of [buf, name, dataPtr, lenPtr, zipCopy]) zp._free(p);
console.log(`js smoke ok: ${zipLen} byte archive`);
process.exit(0);
