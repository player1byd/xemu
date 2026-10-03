/*
 * NV2A PTIMER host scheduling decisions.
 *
 * Copyright (c) 2026 Mainkill1 contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_XBOX_NV2A_PTIMER_CORE_H
#define HW_XBOX_NV2A_PTIMER_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* Desired QEMU_CLOCK_VIRTUAL state; deadline_ns is ignored when absent. */
typedef struct PtimerDeadline {
    bool queued;
    int64_t deadline_ns;
} PtimerDeadline;

/* A fresh observation of QEMUTimer, never cached or serialized. */
typedef PtimerDeadline PtimerHostSchedule;

typedef enum PtimerQueueAction {
    PTIMER_QUEUE_KEEP,
    PTIMER_QUEUE_CANCEL,
    PTIMER_QUEUE_ARM,
} PtimerQueueAction;

PtimerQueueAction ptimer_queue_action(const PtimerHostSchedule *host,
                                     const PtimerDeadline *desired);

/* Only valid when the caller proved that clock/comparator state is unchanged. */
bool ptimer_schedule_reusable(const PtimerHostSchedule *host, int64_t now_ns);

#endif
