/* SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
 * Copyright (c) 2026 Venn Audio Ltd. */
#include "zp/zp.h"

#include <emscripten/emscripten.h>
#include <emscripten/wasmfs.h>

/* Mounts the Origin Private File System at `path` (browser workers only). Returns 0 on success. */
EMSCRIPTEN_KEEPALIVE int zp_wasm_mount_opfs(const char* path)
{
    backend_t backend = wasmfs_create_opfs_backend();
    return wasmfs_create_directory(path, 0777, backend);
}

int main(void)
{
    return 0;
}
