// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
//
// Owns the WebAssembly module. Jobs block this worker; files live in the Origin Private File
// System (mounted at /opfs) so multi-GB archives never sit in memory.

import { Satchel } from './satchel-api.mjs';

let satchel = null;
let plan = null;
let archive = null;
let counter = 0;
let storage = 'opfs';

// OPFS sync access handles are needed for multi-GB output; some engines lack them (or fail in
// private/ephemeral sessions). Then results are kept in memory.
async function opfsUsable() {
    try {
        const root = await navigator.storage.getDirectory();
        const handle = await root.getFileHandle('.satchel-probe', { create: true });
        const access = await handle.createSyncAccessHandle();
        access.close();
        await root.removeEntry('.satchel-probe');
        return true;
    } catch {
        return false;
    }
}

function collect(path, prefix = '') {
    const FS = satchel.m.FS;
    const out = [];
    for (const name of FS.readdir(path)) {
        if (name === '.' || name === '..') {
            continue;
        }
        const full = `${path}/${name}`;
        if ((FS.stat(full).mode & 0o170000) === 0o040000) {
            out.push(...collect(full, `${prefix}${name}/`));
        } else {
            out.push({ path: `${prefix}${name}`, blob: new Blob([FS.readFile(full)]) });
            FS.unlink(full);
        }
    }
    return out;
}

async function init() {
    if (!self.crossOriginIsolated) {
        throw new Error('This page is not cross-origin isolated (it needs the Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers), so the archiver cannot run.');
    }
    const threads = Math.max(1, Math.min(navigator.hardwareConcurrency || 4, 16));
    satchel = await Satchel.load({ moduleUrl: './satchel.mjs', threads });
    if (await opfsUsable()) {
        satchel.mountOpfs('/opfs');
    } else {
        storage = 'memory';
        satchel.m.FS.mkdir('/opfs');
    }
    return { version: satchel.version, threads, storage };
}

function progress(id) {
    let last = 0;
    return (done, total) => {
        const now = performance.now();
        if (now - last > 100 || done === total) {
            last = now;
            self.postMessage({ id, progress: { done, total } });
        }
        return true;
    };
}

async function removeEntry(name) {
    const root = await navigator.storage.getDirectory();
    try {
        await root.removeEntry(name, { recursive: true });
    } catch {
        // already gone
    }
}

const ops = {
    init,

    plan({ items, options }) {
        plan?.free();
        plan = satchel.plan(items, options);
        return plan.describe();
    },

    resolve({ resolutions }) {
        plan.resolve(resolutions);
        return plan.describe();
    },

    build({ id }) {
        const name = `build-${Date.now()}-${counter++}.zip`;
        const result = plan.buildToPath(`/opfs/${name}`, { onProgress: progress(id) });
        if (storage === 'memory') {
            const blob = new Blob([satchel.m.FS.readFile(`/opfs/${name}`)], { type: 'application/zip' });
            satchel.m.FS.unlink(`/opfs/${name}`);
            return { result, blob };
        }
        return { result, opfsName: name };
    },

    open({ file }) {
        archive?.free();
        archive = satchel.open(file);
        return archive.describe();
    },

    extract({ id, options }) {
        const dir = `extract-${Date.now()}-${counter++}`;
        const result = archive.extractToPath(`/opfs/${dir}`, { ...options, onProgress: progress(id) });
        if (storage === 'memory') {
            let files = [];
            try {
                files = collect(`/opfs/${dir}`);
            } catch {
                files = [];
            }
            return { result, files };
        }
        return { result, opfsDir: dir };
    },

    verify({ id }) {
        return archive.verify({ onProgress: progress(id) });
    },

    async cleanup({ names }) {
        if (storage === 'memory') {
            return true;
        }
        for (const n of names) {
            await removeEntry(n);
        }
        return true;
    },
};

self.onmessage = async (event) => {
    const { id, op, ...args } = event.data;
    try {
        const result = await ops[op]({ id, ...args });
        self.postMessage({ id, ok: true, result });
    } catch (e) {
        self.postMessage({ id, ok: false, error: { message: e.message, status: e.statusName ?? null } });
    }
};
