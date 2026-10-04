// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.

const $ = (id) => document.getElementById(id);
const worker = new Worker('./worker.mjs', { type: 'module' });
const pending = new Map();
let nextId = 1;
let busy = false;
let lastOpfs = [];
let storage = 'opfs';

worker.onmessage = (event) => {
    const { id, progress, ok, result, error } = event.data;
    const job = pending.get(id);
    if (!job) {
        return;
    }
    if (progress) {
        job.onProgress?.(progress);
        return;
    }
    pending.delete(id);
    ok ? job.resolve(result) : job.reject(Object.assign(new Error(error.message), { status: error.status }));
};

function call(op, args = {}, onProgress) {
    const id = nextId++;
    return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject, onProgress });
        worker.postMessage({ id, op, ...args });
    });
}

function size(bytes) {
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let v = bytes;
    let u = 0;
    while (v >= 1000 && u < units.length - 1) {
        v /= 1000;
        u++;
    }
    return u === 0 ? `${bytes} B` : `${v.toFixed(1)} ${units[u]}`;
}

function el(tag, attrs = {}, ...children) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
        if (k === 'class') {
            e.className = v;
        } else if (k.startsWith('on')) {
            e.addEventListener(k.slice(2), v);
        } else {
            e.setAttribute(k, v);
        }
    }
    for (const c of children) {
        e.append(c);
    }
    return e;
}

function showResult(text, kind = 'ok', extra = []) {
    const box = $('result');
    box.hidden = false;
    box.className = 'banner';
    box.replaceChildren(el('div', { class: kind }, text, ...extra));
}

function fatal(message) {
    const f = $('fatal');
    f.hidden = false;
    f.textContent = message;
}

async function runJob(label, fn) {
    if (busy) {
        return null;
    }
    busy = true;
    document.querySelectorAll('button').forEach((b) => (b.disabled = true));
    $('job').hidden = false;
    $('job-label').textContent = label;
    $('job-progress').value = 0;
    $('job-detail').textContent = '';
    const start = performance.now();
    const onProgress = ({ done, total }) => {
        $('job-progress').value = total ? done / total : 0;
        const seconds = (performance.now() - start) / 1000;
        const rate = seconds > 0 ? done / seconds : 0;
        $('job-detail').textContent = `${size(done)} of ${size(total)} · ${size(rate)}/s`;
    };
    try {
        return await fn(onProgress);
    } catch (e) {
        showResult(e.message, 'error');
        return null;
    } finally {
        busy = false;
        $('job').hidden = true;
        document.querySelectorAll('button').forEach((b) => (b.disabled = false));
        updateBuildButton();
    }
}

async function cleanupOpfs() {
    if (lastOpfs.length) {
        await call('cleanup', { names: lastOpfs });
        lastOpfs = [];
    }
}

async function opfsFile(name) {
    const root = await navigator.storage.getDirectory();
    return (await root.getFileHandle(name)).getFile();
}

async function* walkOpfs(dir, prefix = '') {
    for await (const [name, handle] of dir.entries()) {
        if (handle.kind === 'directory') {
            yield* walkOpfs(handle, `${prefix}${name}/`);
        } else {
            yield [`${prefix}${name}`, handle];
        }
    }
}

function downloadLink(file, name) {
    const a = el('a', { href: URL.createObjectURL(file), download: name }, name);
    return a;
}

// ---- open / extract ----

let archiveInfo = null;

async function openArchive(file) {
    await cleanupOpfs();
    const info = await runJob(`Reading ${file.name}`, () => call('open', { file }));
    if (!info) {
        return;
    }
    archiveInfo = { info, name: file.name };
    $('archive').hidden = false;
    const total = info.entries.reduce((n, e) => n + e.uncompressed_size, 0);
    const packed = info.entries.reduce((n, e) => n + e.compressed_size, 0);
    const restorable = info.entries.filter((e) => e.flac_restorable).length;
    $('archive-summary').textContent = `${info.entries.length} entries · ${size(total)} → ${size(packed)} · created by ${info.app_version ?? 'another tool'}`
        + (restorable ? ` · ${restorable} restorable audio file(s)` : '');
    const body = $('entries').tBodies[0];
    body.replaceChildren(...info.entries.map((e) => el('tr', {},
        el('td', {}, e.name),
        el('td', { class: 'num' }, e.kind === 'directory' ? '' : size(e.uncompressed_size)),
        el('td', { class: 'num' }, e.kind === 'directory' ? '' : size(e.compressed_size)),
        el('td', {}, e.flac_restorable ? (e.channel_count ? 'FLAC multi-mono' : 'FLAC') : !e.supported ? `unsupported (${e.method})` : e.method === 8 ? 'Deflate' : 'Store'),
        el('td', {}, e.restores_to ? el('span', { class: 'badge' }, e.restores_to) : ''),
    )));
}

async function extractAll() {
    const options = { restoreWav: $('opt-restore').checked, includeReadme: $('opt-readme').checked, overwrite: 'replace' };
    const out = await runJob('Extracting', (onProgress) => call('extract', { options }, onProgress));
    if (!out) {
        return;
    }
    const { result } = out;
    const failed = result.outcomes.filter((o) => o.status !== 'OK');
    const refused = result.issues.filter((i) => i.is_error);
    let files = [];
    let dir = null;
    if (out.opfsDir) {
        lastOpfs.push(out.opfsDir);
        const root = await navigator.storage.getDirectory();
        dir = await root.getDirectoryHandle(out.opfsDir);
        for await (const entry of walkOpfs(dir)) {
            files.push(entry);
        }
    } else {
        files = out.files.map(({ path, blob }) => [path, { getFile: async () => blob }]);
    }
    const message = `${files.length} file(s) extracted and verified` + (failed.length || refused.length ? `, ${failed.length + refused.length} problem(s)` : '');
    const extra = [];
    if (window.showDirectoryPicker) {
        extra.push(' ', el('button', { class: 'primary', onclick: () => saveToFolder(files) }, 'Save to a folder…'));
    } else {
        extra.push(el('div', {}, ...files.flatMap(([path, handle]) => [el('a', { href: '#', onclick: async (ev) => {
            ev.preventDefault();
            const f = await handle.getFile();
            const a = downloadLink(f, path.split('/').pop());
            a.click();
        } }, path), ' '])));
    }
    for (const p of [...failed.map((o) => `${o.target}: ${o.message}`), ...refused.map((i) => `${i.name}: ${i.detail}`)]) {
        extra.push(el('div', { class: 'warning' }, p));
    }
    showResult(message, failed.length || refused.length ? 'error' : 'ok', extra);
}

async function saveToFolder(files) {
    const target = await window.showDirectoryPicker({ mode: 'readwrite' });
    await runJob('Saving', async (onProgress) => {
        let done = 0;
        const total = (await Promise.all(files.map(async ([, h]) => (await h.getFile()).size))).reduce((a, b) => a + b, 0);
        for (const [path, handle] of files) {
            const parts = path.split('/');
            let d = target;
            for (const part of parts.slice(0, -1)) {
                d = await d.getDirectoryHandle(part, { create: true });
            }
            const file = await handle.getFile();
            const writable = await (await d.getFileHandle(parts.at(-1), { create: true })).createWritable();
            await file.stream().pipeTo(writable);
            done += file.size;
            onProgress({ done, total });
        }
        showResult(`Saved ${files.length} file(s).`);
    });
}

async function verifyArchive() {
    const result = await runJob('Verifying', (onProgress) => call('verify', {}, onProgress));
    if (!result) {
        return;
    }
    const errors = result.outcomes.filter((o) => o.status !== 'OK');
    showResult(errors.length ? `${errors.length} error(s): ${errors.map((e) => `${e.target} (${e.message})`).join(', ')}` : `${result.outcomes.length} entries verified, no errors`,
        errors.length ? 'error' : 'ok');
}

// ---- create ----

let planInfo = null;
let planItems = [];

async function collectEntry(entry, prefix, out) {
    if (entry.isFile) {
        const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
        out.push({ path: prefix + entry.name, file, lastModified: file.lastModified });
    } else if (entry.isDirectory) {
        const path = prefix + entry.name;
        out.push({ path, directory: true, lastModified: Date.now() });
        const reader = entry.createReader();
        for (;;) {
            const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
            if (!batch.length) {
                break;
            }
            for (const child of batch) {
                await collectEntry(child, `${path}/`, out);
            }
        }
    }
}

function itemsFromFiles(files) {
    const items = [];
    const dirs = new Set();
    for (const file of files) {
        const path = file.webkitRelativePath || file.name;
        const parts = path.split('/');
        for (let i = 1; i < parts.length; i++) {
            const dir = parts.slice(0, i).join('/');
            if (!dirs.has(dir)) {
                dirs.add(dir);
                items.push({ path: dir, directory: true, lastModified: file.lastModified });
            }
        }
        items.push({ path, file, lastModified: file.lastModified });
    }
    return items;
}

async function planFrom(items) {
    await cleanupOpfs();
    planItems = items;
    if (items.length === 1 && !items[0].directory) {
        $('archive-name').value = `${items[0].path.replace(/\.[^.]*$/, '')}.zip`;
    } else if (items.length && items[0].directory) {
        $('archive-name').value = `${items[0].path.split('/')[0]}.zip`;
    }
    await replan();
}

async function replan() {
    const options = { flac: $('opt-flac').checked };
    const info = await runJob('Planning', () => call('plan', { items: planItems, options }));
    if (info) {
        renderPlan(info);
    }
}

async function resolve(resolution) {
    try {
        renderPlan(await call('resolve', { resolutions: [resolution] }));
    } catch (e) {
        showResult(e.message, 'error');
    }
}

function renderPlan(info) {
    planInfo = info;
    $('plan').hidden = false;
    const counts = {};
    for (const e of info.entries) {
        counts[e.codec] = (counts[e.codec] ?? 0) + 1;
    }
    const files = info.entries.filter((e) => e.kind === 'file' && e.codec !== 'generated').length;
    $('plan-summary').textContent = `${files} file(s), ${size(info.total_bytes)}` + (counts.flac || counts.flac_mono ? ` · ${(counts.flac ?? 0) + (counts.flac_mono ?? 0)} FLAC entr${(counts.flac ?? 0) + (counts.flac_mono ?? 0) === 1 ? 'y' : 'ies'}` : '');

    const issues = [];
    for (const c of info.conflicts) {
        const box = el('div', { class: 'issue blocking' }, el('strong', {}, c.kind === 'collision' ? 'Name collision: ' : 'Invalid name: '), c.detail);
        for (const i of c.entries) {
            const e = info.entries[i];
            const input = el('input', { type: 'text', value: e.output_name, 'aria-label': `New name for ${e.output_name}` });
            const row = el('div', { class: 'row' }, el('code', {}, e.output_name), input,
                el('button', { onclick: () => resolve({ entry: i, action: 'rename', newName: input.value }) }, 'Rename'),
                el('button', { onclick: () => resolve({ entry: i, action: 'skip' }) }, 'Skip'));
            if (e.codec === 'flac' || e.codec === 'flac_mono') {
                row.append(el('button', { onclick: () => resolve({ entry: i, action: 'disable_flac' }) }, 'Store unconverted'));
            }
            box.append(row);
        }
        issues.push(box);
    }
    for (const w of info.warnings) {
        issues.push(el('div', { class: 'warning' }, `${w.kind === 'symlink_skipped' ? 'Skipped' : 'Stored as is'}: ${w.source_path} — ${w.detail}`));
    }
    $('issues').replaceChildren(...issues);
    $('plan-entries').tBodies[0].replaceChildren(...info.entries.map((e) => el('tr', {},
        el('td', {}, e.output_name),
        el('td', { class: 'num' }, e.kind === 'directory' ? '' : size(e.size)),
        el('td', {}, e.codec === 'flac' ? 'FLAC' : e.codec === 'flac_mono' ? `FLAC (channel ${e.channel_index}/${e.channel_count})` : e.codec === 'generated' ? 'Readme' : e.kind === 'directory' ? 'Folder' : e.fallback ? 'Store' : 'Deflate/store'),
        el('td', {}, e.restored_name ? el('span', { class: 'badge' }, e.restored_name) : ''),
    )));
    updateBuildButton();
}

function updateBuildButton() {
    $('build').disabled = busy || !planInfo || !planInfo.executable;
}

async function build() {
    const out = await runJob('Compressing', (onProgress) => call('build', {}, onProgress));
    if (!out) {
        return;
    }
    let file = out.blob;
    if (out.opfsName) {
        lastOpfs.push(out.opfsName);
        file = await opfsFile(out.opfsName);
    }
    const name = $('archive-name').value || 'archive.zip';
    const saved = planInfo.total_bytes ? Math.round(100 * (1 - file.size / planInfo.total_bytes)) : 0;
    const save = async () => {
        if (window.showSaveFilePicker) {
            const handle = await window.showSaveFilePicker({ suggestedName: name, types: [{ description: 'Zip archive', accept: { 'application/zip': ['.zip'] } }] });
            const writable = await handle.createWritable();
            await file.stream().pipeTo(writable);
            showResult(`Saved ${name}.`);
        } else {
            downloadLink(file, name).click();
        }
    };
    showResult(`${size(planInfo.total_bytes)} → ${size(file.size)} (${saved >= 0 ? `${saved}% smaller` : `${-saved}% larger`}). `, 'ok',
        [el('button', { class: 'primary', onclick: save }, `Save ${name}`), ' ', downloadLink(file, name)]);
}

// ---- wiring ----

function dropZone(zone, onFiles) {
    zone.addEventListener('dragover', (e) => {
        e.preventDefault();
        zone.classList.add('over');
    });
    zone.addEventListener('dragleave', () => zone.classList.remove('over'));
    zone.addEventListener('drop', async (e) => {
        e.preventDefault();
        zone.classList.remove('over');
        await onFiles(e.dataTransfer);
    });
}

dropZone($('open-drop'), async (dt) => {
    if (dt.files.length) {
        await openArchive(dt.files[0]);
    }
});
dropZone($('create-drop'), async (dt) => {
    const items = [];
    const entries = [...dt.items].map((i) => i.webkitGetAsEntry?.()).filter(Boolean);
    if (entries.length) {
        for (const entry of entries) {
            await collectEntry(entry, '', items);
        }
    } else {
        items.push(...itemsFromFiles(dt.files));
    }
    await planFrom(items);
});
$('open-input').addEventListener('change', (e) => e.target.files.length && openArchive(e.target.files[0]));
$('create-input').addEventListener('change', (e) => planFrom(itemsFromFiles(e.target.files)));
$('create-folder').addEventListener('change', (e) => planFrom(itemsFromFiles(e.target.files)));
$('opt-flac').addEventListener('change', () => planItems.length && replan());
$('extract').addEventListener('click', extractAll);
$('verify').addEventListener('click', verifyArchive);
$('build').addEventListener('click', build);

if (!window.crossOriginIsolated) {
    fatal('This page is not cross-origin isolated: the server must send Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp. Reload once if this is the first visit.');
} else {
    call('init').then(({ version, storage: where }) => {
        storage = where;
        $('version').textContent = `Satchel ${version}` + (storage === 'memory' ? ' · in-memory mode (this browser has no usable OPFS; keep archives small)' : '');
        document.body.dataset.storage = storage;
        document.body.dataset.ready = 'true';
    }, (e) => fatal(e.message));
}
