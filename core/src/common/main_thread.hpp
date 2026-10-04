// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <functional>

namespace zp
{

// Runs `fn` on the thread that owns the JavaScript objects behind C API callbacks (WebAssembly:
// the main runtime thread, which serves proxied calls while it waits for a job). Elsewhere it
// simply calls `fn`.
void run_on_main_thread(const std::function<void()>& fn);

}
