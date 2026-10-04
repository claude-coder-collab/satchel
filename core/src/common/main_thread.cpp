// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/main_thread.hpp"

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
    #include <emscripten/proxying.h>
    #include <emscripten/threading.h>
#endif

namespace zp
{

void run_on_main_thread(const std::function<void()>& fn)
{
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
    if (!emscripten_is_main_runtime_thread())
    {
        const auto trampoline = [](void* arg) { (*static_cast<const std::function<void()>*>(arg))(); };
        emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), trampoline, const_cast<void*>(static_cast<const void*>(&fn)));
        return;
    }
#endif
    fn();
}

}
