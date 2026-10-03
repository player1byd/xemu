/*
 * Keep unrelated NV2A device registration out of report integration units.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef XBOX_NV2A_REPORT_TEST_SHIM_H
#define XBOX_NV2A_REPORT_TEST_SHIM_H

/* Establish normal QEMU config/types before overriding only this root. */
#include "qemu/osdep.h"
#include "qemu/module.h"

#undef type_init
#define type_init(function)

/* renderer.h stores the opaque callback pointer but the fixture never uses it. */
typedef struct MemAccessCallback MemAccessCallback;

#endif
