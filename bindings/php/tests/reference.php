<?php
// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
// Writes the reference archive that the Python tests compare byte for byte.
declare(strict_types=1);
require __DIR__ . '/../src/Satchel.php';
require __DIR__ . '/wav.php';

$ctx = new Satchel\Context(2);
$files = [
    'docs/readme.txt' => str_repeat('reference ', 300),
    'audio/take.wav' => make_wav(5000),
    'bin/noise.bin' => str_repeat(hash('sha256', 'x', true), 4000),
];
$plan = $ctx->planMemory($files, ['docs', 'audio', 'bin'], 1700000000);
file_put_contents($argv[1], $plan->buildBytes());
