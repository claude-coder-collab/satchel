<?php
// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
//
// PHP binding for Satchel over PHP FFI (PHP >= 7.4, ffi.enable=1). Mirrors the two-phase C API:
// plan, resolve, build; open, plan extraction, extract; edit, commit.

declare(strict_types=1);

namespace Satchel;

use FFI;
use FFI\CData;

final class SatchelException extends \RuntimeException
{
    public function __construct(public readonly int $status, public readonly string $statusName, string $message)
    {
        parent::__construct($message, $status);
    }
}

final class Lib
{
    private static ?FFI $ffi = null;

    public static function get(): FFI
    {
        if (self::$ffi === null) {
            $header = file_get_contents(__DIR__ . '/zp_cdef.h');
            self::$ffi = FFI::cdef($header, self::libraryPath());
        }
        return self::$ffi;
    }

    private static function libraryPath(): string
    {
        $env = getenv('SATCHEL_LIBRARY');
        if ($env !== false && $env !== '') {
            return $env;
        }
        $name = match (PHP_OS_FAMILY) {
            'Windows' => 'satchel.dll',
            'Darwin' => 'libsatchel.dylib',
            default => 'libsatchel.so',
        };
        $candidates = [__DIR__ . '/' . $name];
        $repo = dirname(__DIR__, 3);
        foreach (['clang', 'gcc', 'msvc'] as $preset) {
            foreach (['Release', 'Debug'] as $config) {
                $candidates[] = "$repo/build/$preset/core/$config/$name";
            }
        }
        foreach ($candidates as $c) {
            if (is_file($c)) {
                return $c;
            }
        }
        throw new \RuntimeException('libsatchel not found; set SATCHEL_LIBRARY');
    }

    public static function error(?int $status = null): SatchelException
    {
        $ffi = self::get();
        $status ??= $ffi->zp_last_status();
        return new SatchelException($status, $ffi->zp_status_name($status), $ffi->zp_last_error());
    }

    public static function check(int $status): void
    {
        if ($status !== 0) {
            throw self::error($status);
        }
    }

    public static function notNull(?CData $ptr): CData
    {
        if ($ptr === null || FFI::isNull($ptr)) {
            throw self::error();
        }
        return $ptr;
    }

    public static function cstring(string $s): CData
    {
        $len = strlen($s);
        $buf = self::get()->new("char[" . ($len + 1) . "]", false);
        FFI::memcpy($buf, $s, $len);
        $buf[$len] = "\0";
        return $buf;
    }

    public static function str(mixed $p): string
    {
        if (is_string($p)) {
            return $p;
        }
        return ($p === null || FFI::isNull($p)) ? '' : FFI::string($p);
    }

    public static function progress(?callable $progress): ?\Closure
    {
        if ($progress === null) {
            return null;
        }
        return static function ($user, $done, $total) use ($progress): int {
            $keepGoing = $progress((int) $done, (int) $total);
            return ($keepGoing === null || $keepGoing) ? 0 : 1;
        };
    }
}

/** Owns the worker threads and memory budget shared by every job. */
final class Context
{
    public readonly CData $ptr;
    /** @var list<CData> */
    private array $owned = [];

    public function __construct(int $threads = 0, int $memoryBudget = 0)
    {
        $this->ptr = Lib::notNull(Lib::get()->zp_context_create($threads, $memoryBudget));
    }

    public function __destruct()
    {
        Lib::get()->zp_context_free($this->ptr);
    }

    /** @param list<string> $paths */
    public function plan(array $paths, bool $flac = true, int $deflateLevel = 6, int $flacLevel = 5): Plan
    {
        $ffi = Lib::get();
        $strings = array_map(fn ($p) => Lib::cstring($p), $paths);
        $array = $ffi->new('const char*[' . max(1, count($strings)) . ']');
        foreach ($strings as $i => $s) {
            $array[$i] = $ffi->cast('const char*', $s);
        }
        $input = Lib::notNull($ffi->zp_input_from_paths($array, count($strings)));
        foreach ($strings as $s) {
            FFI::free($s);
        }
        return Plan::create($this, $input, $flac, $deflateLevel, $flacLevel);
    }

    /** @param array<string, string> $files */
    public function planMemory(array $files, array $directories = [], int $mtime = 0, bool $flac = true, int $deflateLevel = 6, int $flacLevel = 5): Plan
    {
        $ffi = Lib::get();
        $input = Lib::notNull($ffi->zp_input_memory());
        foreach ($directories as $d) {
            Lib::check($ffi->zp_input_memory_add_directory($input, $d, $mtime, 0755));
        }
        foreach ($files as $name => $data) {
            $len = strlen($data);
            $buf = $ffi->new('uint8_t[' . max(1, $len) . ']');
            if ($len > 0) {
                FFI::memcpy($buf, $data, $len);
            }
            Lib::check($ffi->zp_input_memory_add_file($input, (string) $name, $buf, $len, $mtime, 0644));
        }
        return Plan::create($this, $input, $flac, $deflateLevel, $flacLevel);
    }

    public function open(string $path): Archive
    {
        return new Archive($this, $path);
    }

    public function edit(string $path): Editor
    {
        return new Editor($this, $path);
    }
}

final class Plan
{
    private function __construct(private readonly Context $ctx, public readonly CData $ptr, private readonly ?CData $input)
    {
    }

    public static function create(Context $ctx, CData $input, bool $flac, int $deflateLevel, int $flacLevel): self
    {
        $ffi = Lib::get();
        $opts = $ffi->new('zp_plan_options_t');
        $ffi->zp_plan_options_init(FFI::addr($opts));
        $opts->flac_enabled = $flac ? 1 : 0;
        $opts->deflate_level = $deflateLevel;
        $opts->flac_level = $flacLevel;
        $ptr = $ffi->zp_plan_create($ctx->ptr, $input, FFI::addr($opts));
        if ($ptr === null || FFI::isNull($ptr)) {
            $e = Lib::error();
            $ffi->zp_input_free($input);
            throw $e;
        }
        return new self($ctx, $ptr, $input);
    }

    public static function wrap(Context $ctx, CData $ptr): self
    {
        return new self($ctx, $ptr, null);
    }

    public function __destruct()
    {
        $ffi = Lib::get();
        $ffi->zp_plan_free($this->ptr);
        if ($this->input !== null) {
            $ffi->zp_input_free($this->input);
        }
    }

    /** @return list<array<string, mixed>> */
    public function entries(): array
    {
        $ffi = Lib::get();
        $e = $ffi->new('zp_plan_entry_t');
        $out = [];
        $n = $ffi->zp_plan_entry_count($this->ptr);
        for ($i = 0; $i < $n; $i++) {
            Lib::check($ffi->zp_plan_get_entry($this->ptr, $i, FFI::addr($e)));
            $out[] = [
                'source_path' => Lib::str($e->source_path),
                'output_name' => Lib::str($e->output_name),
                'kind' => ['file', 'directory', 'symlink'][$e->kind] ?? 'file',
                'codec' => ['general', 'flac', 'flac_mono', 'generated', 'kept'][$e->codec] ?? 'general',
                'size' => $e->size,
                'mtime' => $e->mtime,
                'channel_index' => $e->channel_index,
                'channel_count' => $e->channel_count,
                'fallback_detail' => Lib::str($e->fallback_detail),
                'restored_name' => Lib::str($e->restored_name),
            ];
        }
        return $out;
    }

    /** @return list<array{kind: string, key: string, detail: string, entries: list<int>}> */
    public function conflicts(): array
    {
        $ffi = Lib::get();
        $c = $ffi->new('zp_conflict_t');
        $out = [];
        $n = $ffi->zp_plan_conflict_count($this->ptr);
        for ($i = 0; $i < $n; $i++) {
            Lib::check($ffi->zp_plan_get_conflict($this->ptr, $i, FFI::addr($c)));
            $entries = [];
            for ($k = 0; $k < $c->entry_count; $k++) {
                $entries[] = $c->entries[$k];
            }
            $out[] = ['kind' => $c->kind === 0 ? 'collision' : 'invalid_name', 'key' => Lib::str($c->collision_key), 'detail' => Lib::str($c->detail), 'entries' => $entries];
        }
        return $out;
    }

    /** @return list<array{kind: string, source_path: string, detail: string}> */
    public function warnings(): array
    {
        $ffi = Lib::get();
        $w = $ffi->new('zp_warning_t');
        $out = [];
        $n = $ffi->zp_plan_warning_count($this->ptr);
        for ($i = 0; $i < $n; $i++) {
            Lib::check($ffi->zp_plan_get_warning($this->ptr, $i, FFI::addr($w)));
            $out[] = ['kind' => $w->kind === 0 ? 'symlink_skipped' : 'flac_fallback', 'source_path' => Lib::str($w->source_path), 'detail' => Lib::str($w->detail)];
        }
        return $out;
    }

    public function executable(): bool
    {
        return Lib::get()->zp_plan_executable($this->ptr) === 1;
    }

    /** @param list<array{entry: int, action: string, new_name?: string}> $resolutions */
    public function resolve(array $resolutions): void
    {
        $ffi = Lib::get();
        $actions = ['rename' => 0, 'skip' => 1, 'disable_flac' => 2];
        $array = $ffi->new('zp_resolution_t[' . max(1, count($resolutions)) . ']');
        $keep = [];
        foreach ($resolutions as $i => $r) {
            $array[$i]->entry = $r['entry'];
            $array[$i]->action = $actions[$r['action']];
            if (isset($r['new_name'])) {
                $keep[] = $s = Lib::cstring($r['new_name']);
                $array[$i]->new_name = $ffi->cast('const char*', $s);
            }
        }
        $status = $ffi->zp_plan_resolve($this->ptr, $array, count($resolutions));
        foreach ($keep as $s) {
            FFI::free($s);
        }
        Lib::check($status);
    }

    /** Builds atomically to $path. Returns per-entry results. */
    public function build(string $path, ?callable $progress = null): array
    {
        $ffi = Lib::get();
        $stream = Lib::notNull($ffi->zp_stream_create_file($path));
        try {
            $result = $ffi->new('zp_build_result_t*');
            $status = $ffi->zp_build($this->ptr, $stream, null, Lib::progress($progress), null, FFI::addr($result));
            $entries = self::results($result);
            Lib::check($status);
            Lib::check($ffi->zp_stream_commit($stream));
            return $entries;
        } finally {
            $ffi->zp_stream_free($stream);
        }
    }

    public function buildBytes(): string
    {
        $ffi = Lib::get();
        $stream = Lib::notNull($ffi->zp_stream_memory());
        try {
            Lib::check($ffi->zp_build($this->ptr, $stream, null, null, null, null));
            $data = $ffi->new('const uint8_t*');
            $len = $ffi->new('size_t');
            Lib::check($ffi->zp_stream_memory_data($stream, FFI::addr($data), FFI::addr($len)));
            return FFI::string($data, $len->cdata);
        } finally {
            $ffi->zp_stream_free($stream);
        }
    }

    private static function results(CData $result): array
    {
        $ffi = Lib::get();
        if (FFI::isNull($result)) {
            return [];
        }
        $er = $ffi->new('zp_entry_result_t');
        $out = [];
        $n = $ffi->zp_build_result_entry_count($result);
        for ($i = 0; $i < $n; $i++) {
            Lib::check($ffi->zp_build_result_get_entry($result, $i, FFI::addr($er)));
            $out[] = ['name' => Lib::str($er->name), 'method' => $er->method, 'compressed_size' => $er->compressed_size, 'uncompressed_size' => $er->uncompressed_size, 'status' => $er->status, 'message' => Lib::str($er->message)];
        }
        $ffi->zp_build_result_free($result);
        return $out;
    }
}

/** A zip archive opened for listing and extraction (central directory only). */
final class Archive
{
    public readonly CData $ptr;
    private CData $stream;

    public function __construct(private readonly Context $ctx, string $path)
    {
        $ffi = Lib::get();
        $this->stream = Lib::notNull($ffi->zp_stream_open_file($path));
        $ptr = $ffi->zp_reader_open($ctx->ptr, $this->stream);
        if ($ptr === null || FFI::isNull($ptr)) {
            $e = Lib::error();
            $ffi->zp_stream_free($this->stream);
            throw $e;
        }
        $this->ptr = $ptr;
    }

    public function __destruct()
    {
        $ffi = Lib::get();
        $ffi->zp_reader_free($this->ptr);
        $ffi->zp_stream_free($this->stream);
    }

    /** @return list<array<string, mixed>> */
    public function entries(): array
    {
        $ffi = Lib::get();
        $e = $ffi->new('zp_entry_info_t');
        $out = [];
        $n = $ffi->zp_reader_entry_count($this->ptr);
        for ($i = 0; $i < $n; $i++) {
            Lib::check($ffi->zp_reader_get_entry($this->ptr, $i, FFI::addr($e)));
            $out[] = ['name' => Lib::str($e->name), 'kind' => ['file', 'directory', 'symlink'][$e->kind] ?? 'file', 'method' => $e->method, 'size' => $e->uncompressed_size, 'compressed_size' => $e->compressed_size, 'crc32' => $e->crc32, 'mtime' => $e->mtime, 'flac_restorable' => $e->flac_restorable === 1];
        }
        return $out;
    }

    public function appVersion(): ?string
    {
        $ffi = Lib::get();
        $buf = $ffi->new('char[256]');
        return $ffi->zp_reader_get_app_version($this->ptr, $buf, 256) === 0 ? FFI::string($buf) : null;
    }

    /** Extracts everything to $destination; returns the extraction issues. */
    public function extract(string $destination, bool $restoreWav = true, bool $includeReadme = false, string $overwrite = 'ask'): array
    {
        $ffi = Lib::get();
        $sink = Lib::notNull($ffi->zp_sink_filesystem($destination));
        $opts = $ffi->new('zp_extract_options_t');
        $ffi->zp_extract_options_init(FFI::addr($opts));
        $opts->restore_wav = $restoreWav ? 1 : 0;
        $opts->include_readme = $includeReadme ? 1 : 0;
        $opts->overwrite = ['ask' => 0, 'skip' => 1, 'replace' => 2][$overwrite];
        $plan = null;
        try {
            $plan = Lib::notNull($ffi->zp_extract_plan($this->ptr, null, 0, $sink, FFI::addr($opts)));
            $issues = [];
            $x = $ffi->new('zp_extract_issue_t');
            $n = $ffi->zp_xplan_issue_count($plan);
            for ($i = 0; $i < $n; $i++) {
                Lib::check($ffi->zp_xplan_get_issue($plan, $i, FFI::addr($x)));
                $issues[] = ['kind' => $x->kind, 'name' => Lib::str($x->name), 'detail' => Lib::str($x->detail), 'is_error' => $x->is_error === 1];
            }
            Lib::check($ffi->zp_extract($plan, null, null));
            return $issues;
        } finally {
            if ($plan !== null) {
                $ffi->zp_xplan_free($plan);
            }
            $ffi->zp_sink_free($sink);
        }
    }
}

/** Edits an archive in place: changes are written to a temporary file and swapped in on commit. */
final class Editor
{
    public readonly CData $ptr;
    private CData $stream;
    /** @var list<CData> */
    private array $inputs = [];

    public function __construct(private readonly Context $ctx, private readonly string $path)
    {
        $ffi = Lib::get();
        $this->stream = Lib::notNull($ffi->zp_stream_open_file($path));
        $ptr = $ffi->zp_editor_open($ctx->ptr, $this->stream);
        if ($ptr === null || FFI::isNull($ptr)) {
            $e = Lib::error();
            $ffi->zp_stream_free($this->stream);
            throw $e;
        }
        $this->ptr = $ptr;
    }

    public function __destruct()
    {
        $ffi = Lib::get();
        $ffi->zp_editor_free($this->ptr);
        foreach ($this->inputs as $i) {
            $ffi->zp_input_free($i);
        }
        $ffi->zp_stream_free($this->stream);
    }

    public function entries(): array
    {
        return Plan::wrap($this->ctx, Lib::notNull(Lib::get()->zp_editor_plan($this->ptr)))->entries();
    }

    public function remove(int $index): void
    {
        Lib::check(Lib::get()->zp_editor_remove($this->ptr, $index));
    }

    public function rename(int $index, string $newName): void
    {
        Lib::check(Lib::get()->zp_editor_rename($this->ptr, $index, $newName));
    }

    /** @param list<string> $paths */
    public function add(array $paths): void
    {
        $ffi = Lib::get();
        $strings = array_map(fn ($p) => Lib::cstring($p), $paths);
        $array = $ffi->new('const char*[' . max(1, count($strings)) . ']');
        foreach ($strings as $i => $s) {
            $array[$i] = $ffi->cast('const char*', $s);
        }
        $input = Lib::notNull($ffi->zp_input_from_paths($array, count($strings)));
        foreach ($strings as $s) {
            FFI::free($s);
        }
        $this->inputs[] = $input;
        Lib::check($ffi->zp_editor_add($this->ptr, $input));
    }

    public function commit(?string $destination = null, ?callable $progress = null): void
    {
        $ffi = Lib::get();
        $out = Lib::notNull($ffi->zp_stream_create_file($destination ?? $this->path));
        try {
            Lib::check($ffi->zp_editor_commit($this->ptr, $out, Lib::progress($progress), null));
            Lib::check($ffi->zp_stream_commit($out));
        } finally {
            $ffi->zp_stream_free($out);
        }
    }
}
