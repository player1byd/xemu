/*
 * NV2A PTIMER host scheduling decisions.
 *
 * Copyright (c) 2026 Mainkill1 contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Adapted from PR #81 (98f1a7a49cb7a6ccb8feba20438dc86f9ead58d5).
 * Read actual queue state instead of maintaining a second lifetime cache.
 */

#include "qemu/osdep.h"
#include "ptimer_core.h"

PtimerQueueAction ptimer_queue_action(const PtimerHostSchedule *host,
                                     const PtimerDeadline *desired)
{
    if (!desired->queued) {
        return host->queued ? PTIMER_QUEUE_CANCEL : PTIMER_QUEUE_KEEP;
    }
    if (host->queued && host->deadline_ns == desired->deadline_ns) {
        return PTIMER_QUEUE_KEEP;
    }
    return PTIMER_QUEUE_ARM;
}

bool ptimer_schedule_reusable(const PtimerHostSchedule *host, int64_t now_ns)
{
    return host->queued && host->deadline_ns > now_ns;
}
