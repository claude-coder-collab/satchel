# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
# Copyright (c) 2026 Venn Audio Ltd.
"""Python binding for Satchel: lossless media packaging as standard zip archives.

The binding mirrors the two-phase C API: plan (names, conflicts, warnings), resolve, then build.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Mapping, Sequence

from ._lib import ffi, lib

__all__ = [
    "Archive",
    "BuildResult",
    "Conflict",
    "Context",
    "Editor",
    "EntryInfo",
    "EntryResult",
    "ExtractionPlan",
    "Plan",
    "PlanEntry",
    "Resolution",
    "SatchelError",
    "Warning",
    "version",
]

Progress = Callable[[int, int], bool | None]

_KINDS = {0: "file", 1: "directory", 2: "symlink"}
_CODECS = {0: "general", 1: "flac", 2: "flac_mono", 3: "generated", 4: "kept"}
_ISSUES = {0: "symlink_skipped", 1: "unsupported_method", 2: "unsafe_path", 3: "name_collision", 4: "incomplete_group", 5: "exists_at_destination"}
_DECISIONS = {0: "write", 1: "skip", 2: "replace", 3: "undecided"}
_OVERWRITE = {"ask": 0, "skip": 1, "replace": 2}
_FALLBACKS = ["not_parsable", "float_samples", "unsupported_encoding", "bit_depth", "sample_rate", "too_many_samples", "chunk_too_large", "not_seekable"]


class SatchelError(Exception):
    """A failed library call; `status` is the ZP_* code and `name` its symbolic name."""

    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.name = ffi.string(lib.zp_status_name(status)).decode()


def _error(status: int | None = None) -> SatchelError:
    message = ffi.string(lib.zp_last_error()).decode("utf-8", "replace")
    return SatchelError(status if status is not None else lib.ZP_INTERNAL, message)


def _check(status: int) -> None:
    if status != lib.ZP_OK:
        raise _error(status)


def _not_null(ptr):
    if ptr == ffi.NULL:
        raise _error(lib.zp_last_status())
    return ptr


def _str(ptr) -> str:
    return "" if ptr == ffi.NULL else ffi.string(ptr).decode("utf-8")


def _path(p: str | os.PathLike[str]) -> bytes:
    return os.fspath(p).encode("utf-8")


def version() -> str:
    return _str(lib.zp_version())


class _ProgressHandle:
    def __init__(self, callback: Progress | None) -> None:
        self.callback = callback
        self.error: BaseException | None = None
        self.cfunc = ffi.NULL
        if callback is not None:

            @ffi.callback("int(void*, uint64_t, uint64_t)")
            def trampoline(_user, done, total):
                try:
                    keep_going = self.callback(int(done), int(total))
                    return 0 if keep_going is None or keep_going else 1
                except BaseException as e:  # noqa: BLE001 - re-raised after the call
                    self.error = e
                    return 1

            self.cfunc = trampoline

    def reraise(self) -> None:
        if self.error is not None:
            raise self.error


@dataclass(frozen=True)
class PlanEntry:
    source_path: str
    output_name: str
    kind: str
    codec: str
    size: int
    mtime: int
    unix_mode: int
    group_id: bytes | None
    channel_index: int
    channel_count: int
    fallback_reason: str | None
    fallback_detail: str
    restored_name: str


@dataclass(frozen=True)
class Conflict:
    kind: str
    collision_key: str
    detail: str
    entries: tuple[int, ...]


@dataclass(frozen=True)
class Warning:
    kind: str
    source_path: str
    detail: str


@dataclass(frozen=True)
class Resolution:
    entry: int
    action: str  # "rename", "skip", "disable_flac"
    new_name: str | None = None

    @staticmethod
    def rename(entry: int, new_name: str) -> "Resolution":
        return Resolution(entry, "rename", new_name)

    @staticmethod
    def skip(entry: int) -> "Resolution":
        return Resolution(entry, "skip")

    @staticmethod
    def disable_flac(entry: int) -> "Resolution":
        return Resolution(entry, "disable_flac")


@dataclass(frozen=True)
class EntryResult:
    plan_index: int
    name: str
    method: int
    compressed_size: int
    uncompressed_size: int
    crc32: int
    status: int
    message: str


@dataclass
class BuildResult:
    status: int
    entries: list[EntryResult] = field(default_factory=list)
    zip64: bool = False

    @property
    def ok(self) -> bool:
        return self.status == lib.ZP_OK


class Context:
    """Owns the worker threads and memory budget shared by every job."""

    def __init__(self, threads: int = 0, memory_budget: int = 0) -> None:
        self._ptr = ffi.gc(_not_null(lib.zp_context_create(threads, memory_budget)), lib.zp_context_free)

    def plan(self, paths: Iterable[str | os.PathLike[str]], *, flac: bool = True, deflate_level: int = 6, flac_level: int = 5) -> "Plan":
        encoded = [ffi.new("char[]", _path(p)) for p in paths]
        array = ffi.new("const char*[]", encoded)
        inp = ffi.gc(_not_null(lib.zp_input_from_paths(array, len(encoded))), lib.zp_input_free)
        return Plan._create(self, inp, flac, deflate_level, flac_level)

    def plan_memory(self, files: Mapping[str, bytes], *, directories: Sequence[str] = (), mtime: int = 0, flac: bool = True,
                    deflate_level: int = 6, flac_level: int = 5) -> "Plan":
        inp = ffi.gc(_not_null(lib.zp_input_memory()), lib.zp_input_free)
        for d in directories:
            _check(lib.zp_input_memory_add_directory(inp, d.encode("utf-8"), mtime, 0o755))
        for name, data in files.items():
            _check(lib.zp_input_memory_add_file(inp, name.encode("utf-8"), data, len(data), mtime, 0o644))
        return Plan._create(self, inp, flac, deflate_level, flac_level)

    def open(self, path: str | os.PathLike[str]) -> "Archive":
        return Archive(self, path)

    def edit(self, path: str | os.PathLike[str]) -> "Editor":
        return Editor(self, path)


def _entries_of(plan_ptr) -> list[PlanEntry]:
    out = []
    e = ffi.new("zp_plan_entry_t*")
    for i in range(lib.zp_plan_entry_count(plan_ptr)):
        _check(lib.zp_plan_get_entry(plan_ptr, i, e))
        out.append(PlanEntry(
            source_path=_str(e.source_path),
            output_name=_str(e.output_name),
            kind=_KINDS.get(e.kind, "file"),
            codec=_CODECS.get(e.codec, "general"),
            size=int(e.size),
            mtime=int(e.mtime),
            unix_mode=int(e.unix_mode),
            group_id=bytes(ffi.buffer(e.group_id, 16)) if e.has_group else None,
            channel_index=int(e.channel_index),
            channel_count=int(e.channel_count),
            fallback_reason=_FALLBACKS[e.fallback_reason] if 0 <= e.fallback_reason < len(_FALLBACKS) else None,
            fallback_detail=_str(e.fallback_detail),
            restored_name=_str(e.restored_name),
        ))
    return out


def _conflicts_of(plan_ptr) -> list[Conflict]:
    out = []
    c = ffi.new("zp_conflict_t*")
    for i in range(lib.zp_plan_conflict_count(plan_ptr)):
        _check(lib.zp_plan_get_conflict(plan_ptr, i, c))
        out.append(Conflict("collision" if c.kind == 0 else "invalid_name", _str(c.collision_key), _str(c.detail),
                            tuple(int(c.entries[k]) for k in range(c.entry_count))))
    return out


def _warnings_of(plan_ptr) -> list[Warning]:
    out = []
    w = ffi.new("zp_warning_t*")
    for i in range(lib.zp_plan_warning_count(plan_ptr)):
        _check(lib.zp_plan_get_warning(plan_ptr, i, w))
        out.append(Warning("symlink_skipped" if w.kind == 0 else "flac_fallback", _str(w.source_path), _str(w.detail)))
    return out


class Plan:
    """Phase one: every output name, conflict and warning, before anything is written."""

    def __init__(self, ctx: Context, ptr, keep_alive=()) -> None:
        self._ctx = ctx
        self._ptr = ptr
        self._keep = keep_alive

    @classmethod
    def _create(cls, ctx: Context, inp, flac: bool, deflate_level: int, flac_level: int) -> "Plan":
        opts = ffi.new("zp_plan_options_t*")
        lib.zp_plan_options_init(opts)
        opts.flac_enabled = 1 if flac else 0
        opts.deflate_level = deflate_level
        opts.flac_level = flac_level
        ptr = ffi.gc(_not_null(lib.zp_plan_create(ctx._ptr, inp, opts)), lib.zp_plan_free)
        return cls(ctx, ptr, (inp,))

    @property
    def entries(self) -> list[PlanEntry]:
        return _entries_of(self._ptr)

    @property
    def conflicts(self) -> list[Conflict]:
        return _conflicts_of(self._ptr)

    @property
    def warnings(self) -> list[Warning]:
        return _warnings_of(self._ptr)

    @property
    def executable(self) -> bool:
        return bool(lib.zp_plan_executable(self._ptr))

    @property
    def total_bytes(self) -> int:
        return int(lib.zp_plan_total_bytes(self._ptr))

    def resolve(self, resolutions: Sequence[Resolution]) -> None:
        actions = {"rename": lib.ZP_RESOLVE_RENAME, "skip": lib.ZP_RESOLVE_SKIP, "disable_flac": lib.ZP_RESOLVE_DISABLE_FLAC}
        array = ffi.new("zp_resolution_t[]", max(1, len(resolutions)))
        names = []
        for i, r in enumerate(resolutions):
            array[i].entry = r.entry
            array[i].action = actions[r.action]
            if r.new_name is not None:
                names.append(ffi.new("char[]", r.new_name.encode("utf-8")))
                array[i].new_name = names[-1]
        _check(lib.zp_plan_resolve(self._ptr, array, len(resolutions)))

    def _build(self, stream, progress: Progress | None, readme_template: str | None, temp_dir: str | os.PathLike[str] | None) -> BuildResult:
        opts = ffi.new("zp_build_options_t*")
        keep = []
        if readme_template is not None:
            keep.append(ffi.new("char[]", readme_template.encode("utf-8")))
            opts.readme_template = keep[-1]
        if temp_dir is not None:
            keep.append(ffi.new("char[]", _path(temp_dir)))
            opts.temp_dir = keep[-1]
        handle = _ProgressHandle(progress)
        out = ffi.new("zp_build_result_t**")
        status = lib.zp_build(self._ptr, stream, opts, handle.cfunc, ffi.NULL, out)
        handle.reraise()
        result = BuildResult(status)
        if out[0] != ffi.NULL:
            r = ffi.gc(out[0], lib.zp_build_result_free)
            er = ffi.new("zp_entry_result_t*")
            for i in range(lib.zp_build_result_entry_count(r)):
                _check(lib.zp_build_result_get_entry(r, i, er))
                result.entries.append(EntryResult(int(er.plan_index), _str(er.name), int(er.method), int(er.compressed_size),
                                                  int(er.uncompressed_size), int(er.crc32), int(er.status), _str(er.message)))
            result.zip64 = bool(lib.zp_build_result_zip64(r))
        return result

    def build(self, path: str | os.PathLike[str], *, progress: Progress | None = None, readme_template: str | None = None,
              temp_dir: str | os.PathLike[str] | None = None, check: bool = True) -> BuildResult:
        """Writes the archive atomically: `path` only appears when the build succeeds."""
        stream = ffi.gc(_not_null(lib.zp_stream_create_file(_path(path))), lib.zp_stream_free)
        try:
            result = self._build(stream, progress, readme_template, temp_dir)
            if result.ok:
                _check(lib.zp_stream_commit(stream))
            elif check:
                raise SatchelError(result.status, ffi.string(lib.zp_last_error()).decode("utf-8", "replace"))
            return result
        finally:
            ffi.release(stream)

    def build_bytes(self, *, progress: Progress | None = None, readme_template: str | None = None) -> bytes:
        stream = ffi.gc(_not_null(lib.zp_stream_memory()), lib.zp_stream_free)
        result = self._build(stream, progress, readme_template, None)
        if not result.ok:
            raise SatchelError(result.status, ffi.string(lib.zp_last_error()).decode("utf-8", "replace"))
        data = ffi.new("const uint8_t**")
        size = ffi.new("size_t*")
        _check(lib.zp_stream_memory_data(stream, data, size))
        return bytes(ffi.buffer(data[0], size[0]))


@dataclass(frozen=True)
class EntryInfo:
    index: int
    name: str
    kind: str
    method: int
    supported: bool
    compressed_size: int
    uncompressed_size: int
    crc32: int
    mtime: int
    unix_mode: int | None
    flac_restorable: bool
    flac_group: tuple[bytes, int, int] | None


@dataclass(frozen=True)
class ExtractIssue:
    kind: str
    entry: int
    name: str
    detail: str
    is_error: bool


@dataclass(frozen=True)
class ExtractItem:
    index: int
    entry: int
    target: str
    kind: str
    decision: str


@dataclass(frozen=True)
class ExtractOutcome:
    entry: int
    target: str
    status: int
    message: str


class Archive:
    """A zip archive opened for listing and extraction (central directory only)."""

    def __init__(self, ctx: Context, path: str | os.PathLike[str]) -> None:
        self._ctx = ctx
        self._stream = ffi.gc(_not_null(lib.zp_stream_open_file(_path(path))), lib.zp_stream_free)
        self._ptr = ffi.gc(_not_null(lib.zp_reader_open(ctx._ptr, self._stream)), lib.zp_reader_free)

    @property
    def entries(self) -> list[EntryInfo]:
        out = []
        e = ffi.new("zp_entry_info_t*")
        for i in range(lib.zp_reader_entry_count(self._ptr)):
            _check(lib.zp_reader_get_entry(self._ptr, i, e))
            out.append(EntryInfo(i, _str(e.name), _KINDS.get(e.kind, "file"), int(e.method), bool(e.supported), int(e.compressed_size),
                                 int(e.uncompressed_size), int(e.crc32), int(e.mtime), int(e.unix_mode) if e.has_unix_mode else None,
                                 bool(e.flac_restorable),
                                 (bytes(ffi.buffer(e.flac_group_id, 16)), int(e.flac_channel_index), int(e.flac_channel_count)) if e.has_flac_group else None))
        return out

    @property
    def app_version(self) -> str | None:
        buf = ffi.new("char[256]")
        if lib.zp_reader_get_app_version(self._ptr, buf, 256) != lib.ZP_OK:
            return None
        return ffi.string(buf).decode("utf-8")

    @property
    def zip64(self) -> bool:
        return bool(lib.zp_reader_zip64(self._ptr))

    def extraction_plan(self, destination: str | os.PathLike[str] | None, *, selection: Sequence[int] | None = None, restore_wav: bool = True,
                        include_readme: bool = False, overwrite: str = "ask") -> "ExtractionPlan":
        """destination=None verifies without writing anything."""
        sink = lib.zp_sink_null() if destination is None else lib.zp_sink_filesystem(_path(destination))
        sink = ffi.gc(_not_null(sink), lib.zp_sink_free)
        opts = ffi.new("zp_extract_options_t*")
        lib.zp_extract_options_init(opts)
        opts.restore_wav = 1 if restore_wav else 0
        opts.include_readme = 1 if include_readme else 0
        opts.overwrite = _OVERWRITE[overwrite]
        sel = list(selection or [])
        array = ffi.new("size_t[]", sel) if sel else ffi.NULL
        ptr = ffi.gc(_not_null(lib.zp_extract_plan(self._ptr, array, len(sel), sink, opts)), lib.zp_xplan_free)
        return ExtractionPlan(self, sink, ptr)

    def extract(self, destination: str | os.PathLike[str], **options) -> "ExtractionPlan":
        plan = self.extraction_plan(destination, **options)
        plan.extract()
        return plan

    def verify(self, progress: Progress | None = None) -> "ExtractionPlan":
        plan = self.extraction_plan(None, include_readme=True, overwrite="replace")
        plan.extract(progress=progress, check=False)
        return plan


class ExtractionPlan:
    def __init__(self, archive: Archive, sink, ptr) -> None:
        self._archive = archive
        self._sink = sink
        self._ptr = ptr

    @property
    def issues(self) -> list[ExtractIssue]:
        out = []
        x = ffi.new("zp_extract_issue_t*")
        for i in range(lib.zp_xplan_issue_count(self._ptr)):
            _check(lib.zp_xplan_get_issue(self._ptr, i, x))
            out.append(ExtractIssue(_ISSUES.get(x.kind, "unknown"), int(x.entry), _str(x.name), _str(x.detail), bool(x.is_error)))
        return out

    @property
    def items(self) -> list[ExtractItem]:
        out = []
        x = ffi.new("zp_extract_item_t*")
        for i in range(lib.zp_xplan_item_count(self._ptr)):
            _check(lib.zp_xplan_get_item(self._ptr, i, x))
            out.append(ExtractItem(i, int(x.entry), _str(x.target), _KINDS.get(x.kind, "file"), _DECISIONS.get(x.decision, "write")))
        return out

    def decide(self, item: int, decision: str) -> None:
        _check(lib.zp_xplan_decide(self._ptr, item, lib.ZP_DECISION_SKIP if decision == "skip" else lib.ZP_DECISION_REPLACE))

    def extract(self, *, progress: Progress | None = None, check: bool = True) -> int:
        handle = _ProgressHandle(progress)
        status = lib.zp_extract(self._ptr, handle.cfunc, ffi.NULL)
        handle.reraise()
        if check and status != lib.ZP_OK:
            raise _error(status)
        return status

    @property
    def outcomes(self) -> list[ExtractOutcome]:
        out = []
        x = ffi.new("zp_extract_outcome_t*")
        for i in range(lib.zp_xplan_outcome_count(self._ptr)):
            _check(lib.zp_xplan_get_outcome(self._ptr, i, x))
            out.append(ExtractOutcome(int(x.entry), _str(x.target), int(x.status), _str(x.message)))
        return out


class Editor:
    """Edits an archive in place: changes are written to a temporary file and swapped in on commit."""

    def __init__(self, ctx: Context, path: str | os.PathLike[str]) -> None:
        self._ctx = ctx
        self._path = Path(path)
        self._stream = ffi.gc(_not_null(lib.zp_stream_open_file(_path(path))), lib.zp_stream_free)
        self._ptr = ffi.gc(_not_null(lib.zp_editor_open(ctx._ptr, self._stream)), lib.zp_editor_free)
        self._inputs = []

    def _plan(self):
        return ffi.gc(_not_null(lib.zp_editor_plan(self._ptr)), lib.zp_plan_free)

    @property
    def entries(self) -> list[PlanEntry]:
        return _entries_of(self._plan())

    @property
    def conflicts(self) -> list[Conflict]:
        return _conflicts_of(self._plan())

    def add(self, paths: Iterable[str | os.PathLike[str]]) -> None:
        encoded = [ffi.new("char[]", _path(p)) for p in paths]
        inp = ffi.gc(_not_null(lib.zp_input_from_paths(ffi.new("const char*[]", encoded), len(encoded))), lib.zp_input_free)
        self._inputs.append(inp)
        _check(lib.zp_editor_add(self._ptr, inp))

    def remove(self, index: int) -> None:
        _check(lib.zp_editor_remove(self._ptr, index))

    def rename(self, index: int, new_name: str) -> None:
        _check(lib.zp_editor_rename(self._ptr, index, new_name.encode("utf-8")))

    def replace(self, index: int, path: str | os.PathLike[str]) -> None:
        encoded = [ffi.new("char[]", _path(path))]
        inp = ffi.gc(_not_null(lib.zp_input_from_paths(ffi.new("const char*[]", encoded), 1)), lib.zp_input_free)
        self._inputs.append(inp)
        _check(lib.zp_editor_replace(self._ptr, index, inp))

    def commit(self, destination: str | os.PathLike[str] | None = None, *, progress: Progress | None = None) -> None:
        """Writes the edited archive to `destination` (default: replace the original atomically)."""
        target = self._path if destination is None else Path(destination)
        out = ffi.gc(_not_null(lib.zp_stream_create_file(_path(target))), lib.zp_stream_free)
        try:
            handle = _ProgressHandle(progress)
            status = lib.zp_editor_commit(self._ptr, out, handle.cfunc, ffi.NULL)
            handle.reraise()
            _check(status)
            _check(lib.zp_stream_commit(out))
        finally:
            ffi.release(out)
