/*
 * Geforce NV2A PGRAPH GLSL combiner ubershader
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_PSH_UBER_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_PSH_UBER_H

#include "qemu/mstring.h"

void pgraph_glsl_append_psh_uber_declarations(MString *source, int binding);
void pgraph_glsl_append_psh_uber_body(MString *source,
                                      bool texture_stage_zero_active);

#endif
