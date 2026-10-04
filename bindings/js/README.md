<!-- SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial -->
# @vennaudio/satchel

WebAssembly build of Satchel: creates standard zip archives, storing WAV, AIFF, CAF, RF64 and
Wave64 audio as FLAC, and restores the original files bit-exactly on extraction.

Jobs block the calling thread, so in a browser run them in a Web Worker. The page must be
cross-origin isolated (`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`) because the module uses threads.

```js
import { Satchel } from '@vennaudio/satchel';

const sat = await Satchel.load({ threads: 4 });
const plan = sat.plan([{ path: 'take.wav', data: wavBytes, lastModified: Date.now() }]);
const zip = plan.buildBytes();
plan.free();

const archive = sat.open(zip);
const { files } = archive.extractToMemory();
archive.free();
```

Licensed under AGPL-3.0-only, or commercially from Venn Audio Ltd.
