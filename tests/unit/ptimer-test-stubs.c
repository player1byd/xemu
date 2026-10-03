/*
 * Stubs for the ptimer-test
 *
 * Copyright (c) 2016 Dmitry Osipenko <digetx@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "exec/replay-core.h"
#include "migration/vmstate.h"

#include "ptimer-test.h"

const VMStateInfo vmstate_info_uint8;
const VMStateInfo vmstate_info_uint32;
const VMStateInfo vmstate_info_uint64;
const VMStateInfo vmstate_info_int64;
const VMStateInfo vmstate_info_timer;

struct QEMUBH {
    QEMUBHFunc *cb;
    void *opaque;
};

QEMUTimerListGroup main_loop_tlg;

int64_t ptimer_test_time_ns;
uint64_t ptimer_test_timer_mod_calls;
uint64_t ptimer_test_timer_del_calls;

/* under qtest_enabled(), will not artificially limit period - see hw/core/ptimer.c. */
int use_icount;
bool qtest_allowed;

void timer_init_full(QEMUTimer *ts,
                     QEMUTimerListGroup *timer_list_group, QEMUClockType type,
                     int scale, int attributes,
                     QEMUTimerCB *cb, void *opaque)
{
    if (!timer_list_group) {
        timer_list_group = &main_loop_tlg;
    }
    ts->timer_list = timer_list_group->tl[type];
    ts->cb = cb;
    ts->opaque = opaque;
    ts->scale = scale;
    ts->attributes = attributes;
    ts->expire_time = -1;
}

static void ptimer_test_unlink_timer(QEMUTimer *ts)
{
    QEMUTimer **link = &ts->timer_list->active_timers.next;

    while (*link && *link != ts) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = ts->next;
    }
    ts->next = NULL;
}

void timer_mod(QEMUTimer *ts, int64_t expire_time)
{
    ptimer_test_timer_mod_calls++;
    ptimer_test_unlink_timer(ts);
    ts->expire_time = MAX(expire_time * ts->scale, 0);
    QEMUTimer **link = &ts->timer_list->active_timers.next;
    while (*link && (*link)->expire_time <= ts->expire_time) {
        link = &(*link)->next;
    }
    ts->next = *link;
    *link = ts;
}

void timer_del(QEMUTimer *ts)
{
    ptimer_test_timer_del_calls++;
    ptimer_test_unlink_timer(ts);
    ts->expire_time = -1;
}

bool timer_pending(const QEMUTimer *ts)
{
    return ts->expire_time >= 0;
}

uint64_t timer_expire_time_ns(const QEMUTimer *ts)
{
    return timer_pending(ts) ? ts->expire_time : -1;
}

uint64_t ptimer_test_clock_read_calls;

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    ptimer_test_clock_read_calls++;
    return ptimer_test_time_ns;
}

int64_t qemu_clock_deadline_ns_all(QEMUClockType type, int attr_mask)
{
    QEMUTimerList *timer_list = main_loop_tlg.tl[QEMU_CLOCK_VIRTUAL];
    QEMUTimer *t = timer_list->active_timers.next;
    int64_t deadline = -1;

    while (t != NULL) {
        if (deadline == -1) {
            deadline = t->expire_time;
        } else {
            deadline = MIN(deadline, t->expire_time);
        }

        t = t->next;
    }

    return deadline;
}

QEMUBH *qemu_bh_new_full(QEMUBHFunc *cb, void *opaque, const char *name,
                         MemReentrancyGuard *reentrancy_guard)
{
    QEMUBH *bh = g_new(QEMUBH, 1);

    bh->cb = cb;
    bh->opaque = opaque;

    return bh;
}

void qemu_bh_delete(QEMUBH *bh)
{
    g_free(bh);
}
