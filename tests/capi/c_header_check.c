/* SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (c) 2026 Satchel contributors */
#include "zp/zp.h"

int zp_c_header_check(void)
{
    zp_plan_options_t options;
    zp_plan_options_init(&options);
    return options.deflate_level == 6 ? ZP_OK : ZP_INTERNAL;
}
