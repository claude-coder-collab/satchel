<?php
// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Self-contained test runner for the PHP binding: php -d ffi.enable=1 tests/run.php
declare(strict_types=1);
require __DIR__ . '/../src/Satchel.php';
require __DIR__ . '/wav.php';

use Satchel\Context;
use Satchel\Preview;
use Satchel\SatchelException;

$failures = 0;
function check(bool $ok, string $what): void
{
    global $failures;
    if (!$ok) {
        $failures++;
        fwrite(STDERR, "FAIL: $what\n");
    }
}
function tmpdir(): string
{
    $d = sys_get_temp_dir() . '/satchel-php-' . bin2hex(random_bytes(6));
    mkdir($d);
    return $d;
}

$ctx = new Context(2);
$dir = tmpdir();

mkdir("$dir/src/project", 0777, true);
$wav = make_wav();
file_put_contents("$dir/src/project/take1.wav", $wav);
file_put_contents("$dir/src/project/notes.txt", str_repeat('hello ', 100));
$plan = $ctx->plan(["$dir/src/project"]);
$names = array_column($plan->entries(), 'output_name');
check(in_array('project/take1.flac', $names, true), 'WAV planned as FLAC');
check($plan->executable(), 'plan executable');
$progressCalls = 0;
$results = $plan->build("$dir/out.zip", function (int $done, int $total) use (&$progressCalls) {
    $progressCalls++;
    return true;
});
check($progressCalls > 0, 'progress callback called');
check(count($results) === 4, 'four entry results');
$archive = $ctx->open("$dir/out.zip");
check(str_starts_with((string) $archive->appVersion(), 'Satchel'), 'app version');
$archive->extract("$dir/dest");
check(file_get_contents("$dir/dest/project/take1.wav") === $wav, 'WAV restored bit-exactly');

$p = $ctx->planMemory(['a.txt' => '1', 'A.TXT' => '2']);
check(!$p->executable(), 'case-only collision blocks');
$conflict = $p->conflicts()[0];
try {
    $p->buildBytes();
    check(false, 'build refused');
} catch (SatchelException $e) {
    check($e->statusName === 'CONFLICTS_UNRESOLVED', 'refused with CONFLICTS_UNRESOLVED');
}
$p->resolve([['entry' => $conflict['entries'][1], 'action' => 'rename', 'new_name' => 'b.txt']]);
check($p->executable(), 'resolved');
check(substr($p->buildBytes(), 0, 2) === 'PK', 'zip written');

$ctx->planMemory(['keep.txt' => 'k', 'drop.txt' => 'd'])->build("$dir/e.zip");
$editor = $ctx->edit("$dir/e.zip");
$editNames = array_column($editor->entries(), 'output_name');
$editor->remove(array_search('drop.txt', $editNames, true));
$editor->commit();
unset($editor);
$entries = array_column($ctx->open("$dir/e.zip")->entries(), 'name');
check($entries === ['keep.txt'], 'editor removed an entry');

try {
    $ctx->open("$dir/missing.zip");
    check(false, 'missing archive throws');
} catch (SatchelException $e) {
    check($e->statusName === 'IO_ERROR', 'IO_ERROR for a missing archive');
}

$summary = Preview::of("$dir/out.zip");
check($summary['kind'] === 'zip', 'preview of a zip');
check(str_contains(Preview::of("$dir/out.zip", true), '<h1>out.zip</h1>'), 'HTML preview');
try {
    Preview::of(__FILE__);
    check(false, 'preview of a non-archive throws');
} catch (SatchelException $e) {
    check(true, 'preview of a non-archive throws');
}

exec('rm -rf ' . escapeshellarg($dir));
if ($failures > 0) {
    fwrite(STDERR, "$failures failure(s)\n");
    exit(1);
}
echo "php binding ok\n";
