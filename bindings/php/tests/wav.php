<?php
// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
declare(strict_types=1);

function make_wav(int $frames = 20000, int $channels = 2, int $rate = 48000): string
{
    $samples = '';
    for ($f = 0; $f < $frames; $f++) {
        for ($c = 0; $c < $channels; $c++) {
            $samples .= pack('v', ((int) (8000 * sin($f * (0.01 + 0.002 * $c)))) & 0xFFFF);
        }
    }
    $fmt = pack('vvVVvv', 1, $channels, $rate, $rate * $channels * 2, $channels * 2, 16);
    $body = 'WAVE' . 'fmt ' . pack('V', strlen($fmt)) . $fmt . 'data' . pack('V', strlen($samples)) . $samples;
    return 'RIFF' . pack('V', strlen($body)) . $body;
}
