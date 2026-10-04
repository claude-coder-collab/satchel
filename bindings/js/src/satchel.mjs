// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
//
// JavaScript binding over the WebAssembly build of the C API. Jobs block the calling thread, so in
// a browser use it inside a Web Worker (see apps/web/worker.mjs). All callbacks into JavaScript
// (file reads, output writes) run on that same thread.

const ZP_KIND_FILE = 0;
const ZP_KIND_DIRECTORY = 1;
const ACTIONS = { rename: 0, skip: 1, disable_flac: 2 };
const OVERWRITE = { ask: 0, skip: 1, replace: 2 };

export class SatchelError extends Error {
    constructor(status, name, message) {
        super(message || name);
        this.status = status;
        this.statusName = name;
    }
}

// Synchronous byte source over Uint8Array or Blob (Blob needs FileReaderSync, i.e. a worker).
function byteSource(data) {
    if (data instanceof Uint8Array) {
        return { size: data.length, read: (offset, length) => data.subarray(offset, offset + length) };
    }
    if (typeof Blob !== 'undefined' && data instanceof Blob) {
        if (typeof FileReaderSync === 'undefined') {
            throw new Error('reading a Blob synchronously needs FileReaderSync (run this in a Web Worker)');
        }
        const reader = new FileReaderSync();
        return { size: data.size, read: (offset, length) => new Uint8Array(reader.readAsArrayBuffer(data.slice(offset, offset + length))) };
    }
    throw new TypeError('expected a Uint8Array, Blob or File');
}

export class Satchel {
    static async load(options = {}) {
        const factory = options.factory ?? (await import(options.moduleUrl ?? './satchel.mjs')).default;
        const module = await factory(options.moduleOptions ?? {});
        return new Satchel(module, options);
    }

    constructor(module, options = {}) {
        this.m = module;
        this.ctx = module._zp_context_create(options.threads ?? 0, BigInt(options.memoryBudget ?? 0));
        if (!this.ctx) {
            this.#throw();
        }
    }

    // Mounts the Origin Private File System at `path` (browser workers only).
    mountOpfs(path = '/opfs') {
        const p = this._string(path);
        const rc = this.m._zp_wasm_mount_opfs(p);
        this.m._free(p);
        if (rc !== 0) {
            throw new Error(`cannot mount OPFS at ${path}`);
        }
    }

    get version() {
        return this.m.UTF8ToString(this.m._zp_version());
    }

    #throw(status) {
        const m = this.m;
        const s = status ?? m._zp_last_status();
        throw new SatchelError(s, m.UTF8ToString(m._zp_status_name(s)), m.UTF8ToString(m._zp_last_error()));
    }

    _check(status) {
        if (status !== 0) {
            this.#throw(status);
        }
    }

    _notNull(ptr) {
        if (!ptr) {
            this.#throw();
        }
        return ptr;
    }

    _string(s) {
        const m = this.m;
        const n = m.lengthBytesUTF8(s) + 1;
        const p = m._malloc(n);
        m.stringToUTF8(s, p, n);
        return p;
    }

    _json(ptr) {
        const text = this.m.UTF8ToString(this._notNull(ptr));
        this.m._zp_free(ptr);
        return JSON.parse(text);
    }

    _progress(onProgress) {
        if (!onProgress) {
            return { fn: 0, release() {} };
        }
        const fn = this.m.addFunction((user, done, total) => (onProgress(Number(done), Number(total)) === false ? 1 : 0), 'iijj');
        return { fn, release: () => this.m.removeFunction(fn) };
    }

    // items: [{ path, data: Uint8Array|Blob|File, lastModified (ms), mode }, { path, directory: true }]
    plan(items, options = {}) {
        const m = this.m;
        const sources = [];
        const itemSize = 32;
        const array = m._malloc(Math.max(1, items.length) * itemSize);
        const strings = [];
        items.forEach((item, i) => {
            const base = array + i * itemSize;
            const path = this._string(item.path);
            strings.push(path);
            const directory = Boolean(item.directory);
            const src = directory ? null : byteSource(item.data ?? item.file);
            sources.push(src);
            m.setValue(base, path, '*');
            m.setValue(base + 4, directory ? ZP_KIND_DIRECTORY : ZP_KIND_FILE, 'i32');
            m.setValue(base + 8, BigInt(src ? src.size : 0), 'i64');
            const lastModified = item.lastModified ?? (item.file?.lastModified ?? 0);
            m.setValue(base + 16, BigInt(Math.floor(lastModified / 1000)), 'i64');
            m.setValue(base + 24, item.mode ?? 0, 'i32');
        });
        const read = m.addFunction((user, item, offset, buf, len) => {
            try {
                const bytes = sources[item].read(Number(offset), len);
                m.HEAPU8.set(bytes, buf);
                return BigInt(bytes.length);
            } catch {
                return -1n;
            }
        }, 'jiijii');
        const input = m._zp_input_from_callbacks(array, items.length, read, 0);
        m._free(array);
        strings.forEach((p) => m._free(p));
        this._notNull(input);
        const opts = m._malloc(12);
        m._zp_plan_options_init(opts);
        if (options.flac === false) {
            m.setValue(opts, 0, 'i32');
        }
        if (options.deflateLevel) {
            m.setValue(opts + 4, options.deflateLevel, 'i32');
        }
        if (options.flacLevel !== undefined) {
            m.setValue(opts + 8, options.flacLevel, 'i32');
        }
        const ptr = m._zp_plan_create(this.ctx, input, opts);
        m._free(opts);
        if (!ptr) {
            m._zp_input_free(input);
            m.removeFunction(read);
            this._notNull(0);
        }
        return new Plan(this, ptr, () => {
            m._zp_input_free(input);
            m.removeFunction(read);
        });
    }

    // data: Uint8Array | Blob | File
    open(data) {
        return new Archive(this, data);
    }

    // A seekable output written by callbacks: { write(bytes, position), size() }.
    _outputStream(writer) {
        const m = this.m;
        let position = 0;
        const write = m.addFunction((user, buf, len) => {
            try {
                writer.write(m.HEAPU8.subarray(buf, buf + len), position);
                position += len;
                return BigInt(len);
            } catch {
                return -1n;
            }
        }, 'jiii');
        const seek = writer.seekable === false ? 0 : m.addFunction((user, pos) => {
            position = Number(pos);
            return 0;
        }, 'iij');
        const size = m.addFunction(() => BigInt(writer.size ? writer.size() : position), 'ji');
        const cb = m._malloc(20);
        m.setValue(cb, 0, '*');
        m.setValue(cb + 4, 0, '*');
        m.setValue(cb + 8, write, '*');
        m.setValue(cb + 12, seek, '*');
        m.setValue(cb + 16, size, '*');
        const stream = m._zp_stream_from_callbacks(cb);
        m._free(cb);
        return {
            stream: this._notNull(stream),
            release: () => {
                m._zp_stream_free(stream);
                m.removeFunction(write);
                if (seek) {
                    m.removeFunction(seek);
                }
                m.removeFunction(size);
            },
        };
    }

    free() {
        this.m._zp_context_free(this.ctx);
        this.ctx = 0;
    }
}

// Growable in-memory output for archives.
export class MemoryWriter {
    constructor() {
        this.buffer = new Uint8Array(1 << 16);
        this.length = 0;
    }
    write(bytes, position) {
        const end = position + bytes.length;
        if (end > this.buffer.length) {
            const next = new Uint8Array(Math.max(end, this.buffer.length * 2));
            next.set(this.buffer.subarray(0, this.length));
            this.buffer = next;
        }
        this.buffer.set(bytes, position);
        this.length = Math.max(this.length, end);
    }
    size() {
        return this.length;
    }
    bytes() {
        return this.buffer.slice(0, this.length);
    }
}

// Writes to an OPFS FileSystemSyncAccessHandle (worker only).
export class SyncHandleWriter {
    constructor(handle) {
        this.handle = handle;
        this.handle.truncate(0);
    }
    write(bytes, position) {
        let done = 0;
        while (done < bytes.length) {
            done += this.handle.write(bytes.subarray(done), { at: position + done });
        }
    }
    size() {
        return this.handle.getSize();
    }
}

export class Plan {
    constructor(satchel, ptr, cleanup) {
        this.s = satchel;
        this.ptr = ptr;
        this.cleanup = cleanup;
    }

    describe() {
        return this.s._json(this.s.m._zp_plan_describe(this.ptr));
    }

    get entries() {
        return this.describe().entries;
    }

    get conflicts() {
        return this.describe().conflicts;
    }

    get warnings() {
        return this.describe().warnings;
    }

    get executable() {
        return this.s.m._zp_plan_executable(this.ptr) === 1;
    }

    // [{ entry, action: 'rename'|'skip'|'disable_flac', newName }]
    resolve(resolutions) {
        const m = this.s.m;
        const array = m._malloc(Math.max(1, resolutions.length) * 12);
        const strings = [];
        resolutions.forEach((r, i) => {
            m.setValue(array + i * 12, r.entry, 'i32');
            m.setValue(array + i * 12 + 4, ACTIONS[r.action], 'i32');
            const name = r.newName ? this.s._string(r.newName) : 0;
            if (name) {
                strings.push(name);
            }
            m.setValue(array + i * 12 + 8, name, '*');
        });
        const status = m._zp_plan_resolve(this.ptr, array, resolutions.length);
        m._free(array);
        strings.forEach((p) => m._free(p));
        this.s._check(status);
    }

    // writer: { write(bytes, position), size() }, e.g. MemoryWriter or SyncHandleWriter.
    build(writer, { onProgress, readmeTemplate } = {}) {
        const m = this.s.m;
        const out = this.s._outputStream(writer);
        const progress = this.s._progress(onProgress);
        const options = m._malloc(16);
        m.setValue(options, 0n, 'i64');
        const template = readmeTemplate ? this.s._string(readmeTemplate) : 0;
        m.setValue(options + 8, template, '*');
        m.setValue(options + 12, 0, '*');
        const resultPtr = m._malloc(4);
        m.setValue(resultPtr, 0, '*');
        try {
            const status = m._zp_build(this.ptr, out.stream, options, progress.fn, 0, resultPtr);
            const raw = m.getValue(resultPtr, '*');
            const result = raw ? this.s._json(m._zp_build_result_describe(raw)) : { status: 'INTERNAL', entries: [] };
            if (raw) {
                m._zp_build_result_free(raw);
            }
            if (status !== 0) {
                this.s._check(status);
            }
            return result;
        } finally {
            m._free(resultPtr);
            m._free(options);
            if (template) {
                m._free(template);
            }
            progress.release();
            out.release();
        }
    }

    // Builds atomically into the module's file system (e.g. under an OPFS mount).
    buildToPath(path, { onProgress, readmeTemplate } = {}) {
        const m = this.s.m;
        const p = this.s._string(path);
        const stream = m._zp_stream_create_file(p);
        m._free(p);
        this.s._notNull(stream);
        const progress = this.s._progress(onProgress);
        const options = m._malloc(16);
        m.setValue(options, 0n, 'i64');
        const template = readmeTemplate ? this.s._string(readmeTemplate) : 0;
        m.setValue(options + 8, template, '*');
        m.setValue(options + 12, 0, '*');
        const resultPtr = m._malloc(4);
        m.setValue(resultPtr, 0, '*');
        try {
            const status = m._zp_build(this.ptr, stream, options, progress.fn, 0, resultPtr);
            const raw = m.getValue(resultPtr, '*');
            const result = raw ? this.s._json(m._zp_build_result_describe(raw)) : null;
            if (raw) {
                m._zp_build_result_free(raw);
            }
            this.s._check(status);
            this.s._check(m._zp_stream_commit(stream));
            return result;
        } finally {
            m._zp_stream_free(stream);
            m._free(resultPtr);
            m._free(options);
            if (template) {
                m._free(template);
            }
            progress.release();
        }
    }

    buildBytes(options = {}) {
        const writer = new MemoryWriter();
        this.build(writer, options);
        return writer.bytes();
    }

    free() {
        this.s.m._zp_plan_free(this.ptr);
        this.cleanup();
    }
}

export class Archive {
    constructor(satchel, data) {
        const m = satchel.m;
        this.s = satchel;
        this.source = byteSource(data);
        let position = 0;
        this.read = m.addFunction((user, buf, len) => {
            try {
                const bytes = this.source.read(position, len);
                m.HEAPU8.set(bytes, buf);
                position += bytes.length;
                return BigInt(bytes.length);
            } catch {
                return -1n;
            }
        }, 'jiii');
        this.seek = m.addFunction((user, pos) => {
            position = Number(pos);
            return 0;
        }, 'iij');
        this.sizeFn = m.addFunction(() => BigInt(this.source.size), 'ji');
        const cb = m._malloc(20);
        m.setValue(cb, 0, '*');
        m.setValue(cb + 4, this.read, '*');
        m.setValue(cb + 8, 0, '*');
        m.setValue(cb + 12, this.seek, '*');
        m.setValue(cb + 16, this.sizeFn, '*');
        this.stream = satchel._notNull(m._zp_stream_from_callbacks(cb));
        m._free(cb);
        this.reader = m._zp_reader_open(satchel.ctx, this.stream);
        if (!this.reader) {
            this.#release();
            satchel._notNull(0);
        }
    }

    describe() {
        return this.s._json(this.s.m._zp_reader_describe(this.reader));
    }

    get entries() {
        return this.describe().entries;
    }

    // sink: { exists(path), mkdir(path), open(path, replace) -> handle, write(handle, bytes), commit(handle, mtime, mode), discard(handle) }
    // Returns { issues, items, outcomes, status }. selection: entry indices.
    extract(sink, { selection = [], restoreWav = true, includeReadme = false, overwrite = 'ask', onProgress, decide } = {}) {
        const m = this.s.m;
        const fns = [];
        const fn = (f, sig) => {
            const p = m.addFunction(f, sig);
            fns.push(p);
            return p;
        };
        const guard = (f, fallback) => (...args) => {
            try {
                return f(...args);
            } catch {
                return fallback;
            }
        };
        const cb = m._malloc(28);
        m.setValue(cb, 0, '*');
        m.setValue(cb + 4, fn(guard((user, path) => (sink.exists?.(m.UTF8ToString(path)) ? 1 : 0), 0), 'iii'), '*');
        m.setValue(cb + 8, fn(guard((user, path) => (sink.mkdir?.(m.UTF8ToString(path)), 0), 1), 'iii'), '*');
        m.setValue(cb + 12, fn(guard((user, path, replace) => BigInt(sink.open(m.UTF8ToString(path), replace === 1)), -1n), 'jiii'), '*');
        m.setValue(cb + 16, fn(guard((user, handle, buf, len) => (sink.write(Number(handle), m.HEAPU8.slice(buf, buf + len)), 0), 1), 'iijii'), '*');
        m.setValue(cb + 20, fn(guard((user, handle, mtime, mode, hasMode) => (sink.commit?.(Number(handle), Number(mtime), hasMode ? mode : null), 0), 1), 'iijjii'), '*');
        m.setValue(cb + 24, fn(guard((user, handle) => sink.discard?.(Number(handle)), undefined), 'vij'), '*');
        const sinkPtr = m._zp_sink_from_callbacks(cb);
        m._free(cb);
        const opts = m._malloc(12);
        m._zp_extract_options_init(opts);
        m.setValue(opts, restoreWav ? 1 : 0, 'i32');
        m.setValue(opts + 4, includeReadme ? 1 : 0, 'i32');
        m.setValue(opts + 8, OVERWRITE[overwrite], 'i32');
        const sel = m._malloc(Math.max(1, selection.length) * 4);
        selection.forEach((v, i) => m.setValue(sel + i * 4, v, 'i32'));
        const progress = this.s._progress(onProgress);
        let xplan = 0;
        try {
            this.s._notNull(sinkPtr);
            xplan = this.s._notNull(m._zp_extract_plan(this.reader, selection.length ? sel : 0, selection.length, sinkPtr, opts));
            const before = this.s._json(m._zp_xplan_describe(xplan));
            if (decide) {
                before.items.forEach((item, i) => {
                    if (item.decision === 'undecided') {
                        this.s._check(m._zp_xplan_decide(xplan, i, decide(item) === 'replace' ? 2 : 1));
                    }
                });
            }
            const status = m._zp_extract(xplan, progress.fn, 0);
            const after = this.s._json(m._zp_xplan_describe(xplan));
            after.statusCode = status;
            return after;
        } finally {
            if (xplan) {
                m._zp_xplan_free(xplan);
            }
            if (sinkPtr) {
                m._zp_sink_free(sinkPtr);
            }
            m._free(opts);
            m._free(sel);
            progress.release();
            fns.forEach((p) => m.removeFunction(p));
        }
    }

    // Extracts into a folder of the module's file system (e.g. under an OPFS mount).
    extractToPath(path, { selection = [], restoreWav = true, includeReadme = false, overwrite = 'replace', onProgress } = {}) {
        const m = this.s.m;
        const p = this.s._string(path);
        const sink = m._zp_sink_filesystem(p);
        m._free(p);
        this.s._notNull(sink);
        const opts = m._malloc(12);
        m._zp_extract_options_init(opts);
        m.setValue(opts, restoreWav ? 1 : 0, 'i32');
        m.setValue(opts + 4, includeReadme ? 1 : 0, 'i32');
        m.setValue(opts + 8, OVERWRITE[overwrite], 'i32');
        const sel = m._malloc(Math.max(1, selection.length) * 4);
        selection.forEach((v, i) => m.setValue(sel + i * 4, v, 'i32'));
        const progress = this.s._progress(onProgress);
        let xplan = 0;
        try {
            xplan = this.s._notNull(m._zp_extract_plan(this.reader, selection.length ? sel : 0, selection.length, sink, opts));
            const status = m._zp_extract(xplan, progress.fn, 0);
            const after = this.s._json(m._zp_xplan_describe(xplan));
            after.statusCode = status;
            return after;
        } finally {
            if (xplan) {
                m._zp_xplan_free(xplan);
            }
            m._zp_sink_free(sink);
            m._free(opts);
            m._free(sel);
            progress.release();
        }
    }

    // Extracts into a Map(path -> Uint8Array).
    extractToMemory(options = {}) {
        const files = new Map();
        const open = new Map();
        let next = 0;
        const sink = {
            exists: (path) => files.has(path),
            mkdir: () => {},
            open: (path) => {
                open.set(next, { path, chunks: [] });
                return next++;
            },
            write: (h, bytes) => open.get(h).chunks.push(bytes),
            commit: (h) => {
                const { path, chunks } = open.get(h);
                const total = chunks.reduce((n, c) => n + c.length, 0);
                const data = new Uint8Array(total);
                let at = 0;
                for (const c of chunks) {
                    data.set(c, at);
                    at += c.length;
                }
                files.set(path, data);
                open.delete(h);
            },
            discard: (h) => open.delete(h),
        };
        const result = this.extract(sink, options);
        return { files, result };
    }

    verify(options = {}) {
        let next = 0;
        const sink = { exists: () => false, open: () => next++, write: () => {}, commit: () => {} };
        return this.extract(sink, { includeReadme: true, overwrite: 'replace', ...options });
    }

    #release() {
        const m = this.s.m;
        if (this.stream) {
            m._zp_stream_free(this.stream);
            this.stream = 0;
        }
        for (const p of [this.read, this.seek, this.sizeFn]) {
            m.removeFunction(p);
        }
    }

    free() {
        if (this.reader) {
            this.s.m._zp_reader_free(this.reader);
            this.reader = 0;
        }
        this.#release();
    }
}
