// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Checks an installed npm package: run from a folder where `npm install <tarball>` was done.
import assert from 'node:assert/strict';
import { Satchel } from '@vennaudio/satchel';

const sat = await Satchel.load({ threads: 2 });
const text = new TextEncoder().encode('hello from the npm package\n'.repeat(100));
const plan = sat.plan([{ path: 'docs/hello.txt', data: text, lastModified: 1700000000000 }]);
const zip = plan.buildBytes();
plan.free();
assert.equal(zip[0], 0x50);
const archive = sat.open(zip);
const { files } = archive.extractToMemory();
archive.free();
assert.deepEqual(files.get('docs/hello.txt'), text);
console.log(`ok: ${zip.length}-byte archive`);
process.exit(0);
