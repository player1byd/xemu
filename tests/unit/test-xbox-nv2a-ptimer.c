/*
 * Deterministic NV2A PTIMER alarm, IRQ, and restore tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"

#include "hw/xbox/nv2a/nv2a_int.h"
#include "ptimer-test.h"
#include "hw/xbox/nv2a/ptimer_core.h"

static bool irq_asserted;
static unsigned irq_update_calls;

#define PTIMER_REG_EPOCH_NS (1ULL << 27)
#define TEST_ALARM_LOW 0x100

void nv2a_update_irq(NV2AState *d)
{
    irq_update_calls++;
    if (d->ptimer.pending_interrupts & d->ptimer.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PTIMER;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PTIMER;
    }

    irq_asserted = d->pmc.pending_interrupts && d->pmc.enabled_interrupts;
}

static void init_nv2a_ptimer(NV2AState *d)
{
    memset(d, 0, sizeof(*d));
    ptimer_test_time_ns = 0;
    irq_asserted = false;

    d->pramdac.core_clock_freq = NANOSECONDS_PER_SECOND;
    d->ptimer.numerator = 1;
    d->ptimer.denominator = 1;
    d->pmc.enabled_interrupts = NV_PMC_INTR_EN_0_HARDWARE;
    ptimer_init(d);
}

static void fire_alarm_at(NV2AState *d, int64_t now_ns)
{
    QEMUTimer *timer = &d->ptimer.timer;

    g_assert_true(timer_pending(timer));
    ptimer_test_time_ns = now_ns;
    timer_del(timer);
    timer->next = NULL;
    timer->expire_time = -1;
    timer->cb(timer->opaque);
}

static void expire_alarm(NV2AState *d)
{
    fire_alarm_at(d, timer_expire_time_ns(&d->ptimer.timer));
}

static void make_alarm_four_epochs_overdue(NV2AState *d)
{
    /* The alarm can be masked, so derive its first occurrence from guest
     * state rather than an intentionally absent host timer (1 GHz, 1/1). */
    int64_t first_expiry_ns = (d->ptimer.alarm_time & 0xffffffffULL) >> 5;

    ptimer_test_time_ns = first_expiry_ns + 4 * PTIMER_REG_EPOCH_NS + 1000;
}

static void assert_alarm_caught_up(const NV2AState *d)
{
    g_assert_cmphex(d->ptimer.alarm_time, ==,
                    (5ULL << 32) | TEST_ALARM_LOW);
    g_assert_cmpint(timer_expire_time_ns(&d->ptimer.timer), >,
                    ptimer_test_time_ns);
}

static void test_alarm_assert_and_ack(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x100, 4);

    g_assert_false(irq_asserted);
    expire_alarm(&d);

    g_assert_cmphex(d.ptimer.pending_interrupts, ==,
                    NV_PTIMER_INTR_0_ALARM);
    g_assert_cmphex(d.pmc.pending_interrupts & NV_PMC_INTR_0_PTIMER, ==,
                    NV_PMC_INTR_0_PTIMER);
    g_assert_true(irq_asserted);
    g_assert_cmphex(d.ptimer.alarm_time, ==,
                    (1ULL << 32) | TEST_ALARM_LOW);

    ptimer_write(&d, NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM, 4);
    g_assert_cmphex(d.ptimer.pending_interrupts, ==, 0);
    g_assert_false(irq_asserted);

    ptimer_reset(&d);
}

static void test_irq_contribution_with_other_source(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    d.pmc.pending_interrupts = NV_PMC_INTR_0_PGRAPH;
    nv2a_update_irq(&d);
    g_assert_true(irq_asserted);

    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    unsigned updates = irq_update_calls;
    expire_alarm(&d);
    g_assert_cmpuint(irq_update_calls, ==, updates + 1);
    g_assert_cmphex(d.pmc.pending_interrupts & NV_PMC_INTR_0_PTIMER, ==,
                    NV_PMC_INTR_0_PTIMER);

    updates = irq_update_calls;
    expire_alarm(&d);
    g_assert_cmpuint(irq_update_calls, ==, updates);
    g_assert_cmphex(d.ptimer.pending_interrupts, ==, NV_PTIMER_INTR_0_ALARM);

    updates = irq_update_calls;
    ptimer_write(&d, NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM, 4);
    g_assert_cmpuint(irq_update_calls, ==, updates + 1);
    g_assert_cmphex(d.pmc.pending_interrupts & NV_PMC_INTR_0_PTIMER, ==, 0);
    g_assert_true(irq_asserted);

    updates = irq_update_calls;
    ptimer_write(&d, NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM, 4);
    g_assert_cmpuint(irq_update_calls, ==, updates);
    ptimer_reset(&d);
}

static void test_pending_alarm_asserts_when_enabled(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x100, 4);
    g_assert_false(timer_pending(&d.ptimer.timer));
    g_assert_false(irq_asserted);

    ptimer_test_time_ns = 8;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    g_assert_true(irq_asserted);

    ptimer_reset(&d);
}

static void test_time_registers_and_future_epoch(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_TIME_1, 0x12, 4);
    ptimer_write(&d, NV_PTIMER_TIME_0, 0x345678e0, 4);

    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_1, 4), ==, 0x12);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_0, 4), ==, 0x345678e0);

    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x345678c0, 4);
    g_assert_true(timer_pending(&d.ptimer.timer));
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), >,
                    ptimer_test_time_ns);

    ptimer_reset(&d);
}

static void test_runtime_overdue_alarm_skips_missed_epochs(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    d.ptimer.enabled_interrupts = NV_PTIMER_INTR_EN_0_ALARM;
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);

    make_alarm_four_epochs_overdue(&d);
    fire_alarm_at(&d, ptimer_test_time_ns);

    g_assert_cmphex(d.ptimer.pending_interrupts, ==,
                    NV_PTIMER_INTR_0_ALARM);
    g_assert_true(irq_asserted);
    assert_alarm_caught_up(&d);

    ptimer_reset(&d);
}

static void test_post_load_reconciles_overdue_alarm(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    d.ptimer.enabled_interrupts = NV_PTIMER_INTR_EN_0_ALARM;
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);

    make_alarm_four_epochs_overdue(&d);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), <,
                    ptimer_test_time_ns);

    ptimer_post_load(&d, 5);

    g_assert_cmphex(d.ptimer.pending_interrupts, ==,
                    NV_PTIMER_INTR_0_ALARM);
    g_assert_true(irq_asserted);
    assert_alarm_caught_up(&d);

    ptimer_reset(&d);
}

static void test_intr_read_reconciles_overdue_alarm(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    d.ptimer.enabled_interrupts = NV_PTIMER_INTR_EN_0_ALARM;
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    make_alarm_four_epochs_overdue(&d);

    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_INTR_0, 4), ==,
                    NV_PTIMER_INTR_0_ALARM);
    g_assert_true(irq_asserted);
    assert_alarm_caught_up(&d);

    ptimer_reset(&d);
}

static void test_intr_enable_reconciles_overdue_alarm(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    make_alarm_four_epochs_overdue(&d);

    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);

    g_assert_cmphex(d.ptimer.pending_interrupts, ==,
                    NV_PTIMER_INTR_0_ALARM);
    g_assert_true(irq_asserted);
    assert_alarm_caught_up(&d);

    ptimer_reset(&d);
}

static void test_zero_ratio_stops_clock_without_division(void)
{
    NV2AState d;

    memset(&d, 0, sizeof(d));
    ptimer_test_time_ns = 0;
    irq_asserted = false;
    d.pramdac.core_clock_freq = NANOSECONDS_PER_SECOND;
    d.pmc.enabled_interrupts = NV_PMC_INTR_EN_0_HARDWARE;
    ptimer_init(&d);

    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_0, 4), ==, 0);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_1, 4), ==, 0);

    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));

    ptimer_write(&d, NV_PTIMER_DENOMINATOR, 1, 4);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));

    ptimer_write(&d, NV_PTIMER_NUMERATOR, 1, 4);
    g_assert_true(timer_pending(&d.ptimer.timer));
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), <, INT64_MAX);

    ptimer_write(&d, NV_PTIMER_NUMERATOR, 0, 4);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_NUMERATOR, 4), ==, 0);

    ptimer_write(&d, NV_PTIMER_NUMERATOR, UINT32_MAX, 4);
    ptimer_write(&d, NV_PTIMER_DENOMINATOR, UINT32_MAX, 4);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_NUMERATOR, 4), ==,
                    UINT32_MAX);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_DENOMINATOR, 4), ==,
                    UINT32_MAX);

    ptimer_write(&d, NV_PTIMER_DENOMINATOR, 0, 4);
    ptimer_post_load(&d, 5);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));

    ptimer_reset(&d);
}

static void test_post_load_rebuilds_irq_without_timer(void)
{
    NV2AState d;

    init_nv2a_ptimer(&d);
    d.ptimer.pending_interrupts = NV_PTIMER_INTR_0_ALARM;
    d.ptimer.enabled_interrupts = NV_PTIMER_INTR_EN_0_ALARM;

    ptimer_post_load(&d, 5);

    g_assert_true(irq_asserted);
    g_assert_cmphex(d.pmc.pending_interrupts & NV_PMC_INTR_0_PTIMER, ==,
                    NV_PMC_INTR_0_PTIMER);

    ptimer_reset(&d);
}

/* Restored semantic controls from #59/#81 plus post-#120 v4 recovery.
 * These exercise production functions; post-load cases are not VMState streams.
 */
static void test_masked_expiry_ack_before_unmask(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    ptimer_test_time_ns = 8;

    /* A status read here would hide the missing pre-W1C reconciliation. */
    ptimer_write(&d, NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    g_assert_cmphex(d.ptimer.pending_interrupts, ==, 0);
    g_assert_false(irq_asserted);
    g_assert_true(timer_pending(&d.ptimer.timer));
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), >, 8);
    ptimer_reset(&d);
}

static void test_enabled_expiry_ack_before_callback(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    ptimer_test_time_ns = 8;
    ptimer_write(&d, NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM, 4);
    g_assert_cmphex(d.ptimer.pending_interrupts, ==, 0);
    g_assert_false(irq_asserted);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), >, 8);
    ptimer_reset(&d);
}

static void test_masked_alarm_replacement_keeps_pending(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    ptimer_test_time_ns = 8;
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x200, 4);
    g_assert_cmphex(d.ptimer.pending_interrupts, ==, NV_PTIMER_INTR_0_ALARM);
    g_assert_false(timer_pending(&d.ptimer.timer));
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    g_assert_true(irq_asserted);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 16);
    ptimer_reset(&d);
}

static void arm_wrapped_zero(NV2AState *d)
{
    init_nv2a_ptimer(d);
    ptimer_write(d, NV_PTIMER_TIME_1, 0x1fffffff, 4);
    ptimer_write(d, NV_PTIMER_TIME_0, 0xffffffe0, 4);
    ptimer_write(d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(d, NV_PTIMER_ALARM_0, 0, 4);
}

static void test_full_counter_wrap_zero_alarm(void)
{
    NV2AState d;
    arm_wrapped_zero(&d);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_cmphex(d.ptimer.alarm_time, ==, 0);
    g_assert_true(timer_pending(&d.ptimer.timer));
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 1);
    expire_alarm(&d);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

static void test_phase_correct_first_tick(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = 100000000;
    ptimer_test_time_ns = 9;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 32, 4);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 10);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_0, 4), ==, 0);
    expire_alarm(&d);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_0, 4), ==, 32);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

/* Bounded inputs below fit in 64 bits, providing an independent forward
 * oracle without calling the production inverse or its wide helpers. */
static uint64_t forward_test_ticks(uint64_t ns, uint64_t frequency,
                                   uint32_t numerator, uint32_t denominator)
{
    uint64_t gpu = ns * frequency / NANOSECONDS_PER_SECOND;
    return gpu * denominator / numerator;
}

static void test_phase_deadline_forward_oracle(void)
{
    const uint64_t frequencies[] = { 100000000, 233333333, 1000000000 };
    unsigned int cases = 0;

    for (size_t f = 0; f < G_N_ELEMENTS(frequencies); f++) {
        for (uint32_t n = 1; n <= 6; n++) {
            for (uint32_t den = 1; den <= 6; den++) {
                for (int64_t now = 0; now < 32; now++) {
                    for (uint64_t ticks = 1; ticks <= 16; ticks++) {
                        NV2AState d;
                        init_nv2a_ptimer(&d);
                        d.pramdac.core_clock_freq = frequencies[f];
                        d.ptimer.numerator = n;
                        d.ptimer.denominator = den;
                        ptimer_test_time_ns = now;
                        uint64_t target = forward_test_ticks(
                            now, frequencies[f], n, den) + ticks;
                        ptimer_write(&d, NV_PTIMER_INTR_EN_0,
                                     NV_PTIMER_INTR_EN_0_ALARM, 4);
                        ptimer_write(&d, NV_PTIMER_ALARM_0, target << 5, 4);
                        int64_t deadline = timer_expire_time_ns(&d.ptimer.timer);
                        g_assert_cmpint(deadline, >, now);
                        g_assert_cmpuint(forward_test_ticks(
                            deadline, frequencies[f], n, den), >=, target);
                        g_assert_cmpuint(forward_test_ticks(
                            deadline - 1, frequencies[f], n, den), <, target);
                        expire_alarm(&d);
                        g_assert_true(irq_asserted);
                        g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer),
                                        >, ptimer_test_time_ns);
                        ptimer_reset(&d);
                        cases++;
                    }
                }
            }
        }
    }
    g_assert_cmpuint(cases, ==, 55296);
}

static void test_ack_keeps_valid_schedule(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    uint64_t calls = ptimer_test_timer_mod_calls;
    ptimer_test_time_ns = 1;
    ptimer_write(&d, NV_PTIMER_INTR_0, NV_PTIMER_INTR_0_ALARM, 4);
    g_assert_cmpuint(ptimer_test_timer_mod_calls, ==, calls);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 8);
    ptimer_reset(&d);
}

static void test_core_clock_change_rebuilds_deadline(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    ptimer_set_core_clock(&d, 100000000);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 80);
    ptimer_set_core_clock(&d, 0);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));
    ptimer_set_core_clock(&d, NANOSECONDS_PER_SECOND);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 8);
    ptimer_test_time_ns = 8;
    ptimer_set_core_clock(&d, 100000000);
    g_assert_true(irq_asserted);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), >, 8);
    ptimer_reset(&d);
}

static void test_stopped_equality_preserves_pending(void)
{
    NV2AState d;
    arm_wrapped_zero(&d);
    ptimer_write(&d, NV_PTIMER_NUMERATOR, 0, 4);
    ptimer_write(&d, NV_PTIMER_TIME_1, 0, 4);
    ptimer_write(&d, NV_PTIMER_TIME_0, 0, 4);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_INTR_0, 4), ==, 0);
    ptimer_post_load(&d, 5);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));
    g_assert_false(irq_asserted);
    d.ptimer.pending_interrupts = NV_PTIMER_INTR_0_ALARM;
    ptimer_post_load(&d, 5);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

static void test_v4_masked_restore_preserves_alarm(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    d.ptimer.alarm_armed = false; /* Absent from the v4 stream. */
    g_assert_false(timer_pending(&d.ptimer.timer));
    ptimer_post_load(&d, 4);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));
    ptimer_test_time_ns = 8;
    ptimer_post_load(&d, 4);
    g_assert_cmphex(d.ptimer.pending_interrupts, ==, NV_PTIMER_INTR_0_ALARM);
    g_assert_false(irq_asserted);
    g_assert_false(timer_pending(&d.ptimer.timer));
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

static void test_v4_queued_zero_restore(void)
{
    NV2AState d;
    arm_wrapped_zero(&d);
    d.ptimer.alarm_armed = false;
    ptimer_post_load(&d, 4);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 1);
    ptimer_reset(&d);
}

static void test_v5_zero_restore_without_host_timer(void)
{
    NV2AState d;
    arm_wrapped_zero(&d);
    timer_del(&d.ptimer.timer);
    ptimer_post_load(&d, 5);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 1);
    ptimer_reset(&d);
}

static void test_v5_disarmed_restore_stays_disarmed(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    d.ptimer.alarm_time = TEST_ALARM_LOW;
    d.ptimer.enabled_interrupts = NV_PTIMER_INTR_EN_0_ALARM;
    ptimer_post_load(&d, 5);
    g_assert_false(d.ptimer.alarm_armed);
    g_assert_false(timer_pending(&d.ptimer.timer));
    g_assert_false(irq_asserted);
    ptimer_reset(&d);
}

static void test_pre_v4_restore_discards_old_alarm(void)
{
    NV2AState d;
    arm_wrapped_zero(&d);
    d.ptimer.pending_interrupts = NV_PTIMER_INTR_0_ALARM;
    ptimer_post_load(&d, 3);
    g_assert_false(d.ptimer.alarm_armed);
    g_assert_cmphex(d.ptimer.alarm_time, ==, 0);
    g_assert_cmphex(d.ptimer.time_offset, ==, 0);
    g_assert_false(timer_pending(&d.ptimer.timer));
    g_assert_true(irq_asserted); /* The old stream does contain pending state. */
    ptimer_reset(&d);
}

static void test_deadline_saturates_signed_horizon(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = 1;
    d.ptimer.numerator = UINT32_MAX;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0xffffffe0, 4);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, INT64_MAX);
    g_assert_false(irq_asserted);
    ptimer_reset(&d);
}

static void test_source_counter_wrap_revalidates_alarm(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = 4000000000ULL;
    int64_t start = (1LL << 62) - 1;
    ptimer_test_time_ns = start;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, NV_PTIMER_INTR_EN_0_ALARM, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x180, 4);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, start + 1);
    expire_alarm(&d);
    g_assert_false(irq_asserted);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, start + 4);
    expire_alarm(&d);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

/* Scheduling controls continue PR #81; the state is sampled from QEMUTimer,
 * not cached in a second device-owned queue record. */
static void test_noop_queue_operations(gconstpointer opaque)
{
    unsigned scenario = GPOINTER_TO_UINT(opaque);
    unsigned operation = scenario % 8;
    unsigned mode = scenario / 8; /* enabled, masked, stopped */
    NV2AState d;

    init_nv2a_ptimer(&d);
    pramdac_write(&d, NV_PRAMDAC_NVPLL_COEFF, 0x601, 4);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, mode == 1 ? 0 : 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    if (mode == 2) {
        ptimer_write(&d, NV_PTIMER_NUMERATOR, 0, 4);
    }
    ptimer_test_time_ns = 1;
    uint64_t mods = ptimer_test_timer_mod_calls;
    uint64_t dels = ptimer_test_timer_del_calls;
    uint64_t deadline = timer_expire_time_ns(&d.ptimer.timer);
    unsigned irq_updates = irq_update_calls;

    for (unsigned i = 0; i < 1000; i++) {
        switch (operation) {
        case 0:
            ptimer_write(&d, NV_PTIMER_NUMERATOR, d.ptimer.numerator, 4);
            break;
        case 1:
            ptimer_write(&d, NV_PTIMER_DENOMINATOR, d.ptimer.denominator, 4);
            break;
        case 2:
            ptimer_write(&d, NV_PTIMER_INTR_EN_0,
                         d.ptimer.enabled_interrupts, 4);
            break;
        case 3:
            ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
            break;
        case 4:
            ptimer_write(&d, NV_PTIMER_TIME_0,
                         ptimer_read(&d, NV_PTIMER_TIME_0, 4), 4);
            break;
        case 5:
            ptimer_write(&d, NV_PTIMER_TIME_1,
                         ptimer_read(&d, NV_PTIMER_TIME_1, 4), 4);
            break;
        case 6:
            ptimer_set_core_clock(&d, d.pramdac.core_clock_freq);
            break;
        case 7:
            pramdac_write(&d, NV_PRAMDAC_NVPLL_COEFF, 0x601, 4);
            break;
        default:
            g_assert_not_reached();
        }
    }
    g_assert_cmpuint(ptimer_test_timer_mod_calls, ==, mods);
    g_assert_cmpuint(ptimer_test_timer_del_calls, ==, dels);
    g_assert_cmpuint(irq_update_calls, ==, irq_updates);
    g_assert_cmpuint(timer_expire_time_ns(&d.ptimer.timer), ==, deadline);
    g_assert_cmpint(timer_pending(&d.ptimer.timer), ==, mode == 0);
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(irq_asserted);
    ptimer_reset(&d);
}

static void test_consumed_early_callback_rearms(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    uint64_t mods = ptimer_test_timer_mod_calls;

    /* QEMU has removed this callback. A same-valued desired deadline is
     * not proof that the event is still queued. No early IRQ is allowed. */
    fire_alarm_at(&d, 7);
    g_assert_cmpuint(ptimer_test_timer_mod_calls, ==, mods + 1);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 8);
    g_assert_false(irq_asserted);
    expire_alarm(&d);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

static void test_restore_observes_actual_queue(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    uint64_t mods = ptimer_test_timer_mod_calls;
    ptimer_post_load(&d, 5);
    g_assert_cmpuint(ptimer_test_timer_mod_calls, ==, mods);

    /* VMState can replace or remove the queued event independently. */
    timer_del(&d.ptimer.timer);
    ptimer_post_load(&d, 5);
    g_assert_cmpuint(ptimer_test_timer_mod_calls, ==, mods + 1);
    timer_mod(&d.ptimer.timer, 1234);
    mods = ptimer_test_timer_mod_calls;
    ptimer_post_load(&d, 5);
    g_assert_cmpuint(ptimer_test_timer_mod_calls, ==, mods + 1);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 8);
    g_assert_false(irq_asserted);
    ptimer_reset(&d);
}

static void test_signed_horizon_does_not_requeue_now(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_test_time_ns = INT64_MAX - 1;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, INT64_MAX);
    expire_alarm(&d);
    g_assert_false(timer_pending(&d.ptimer.timer));
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_false(irq_asserted);
    /* Repeated guest writes at the horizon must not restart a dispatch loop. */
    ptimer_write(&d, NV_PTIMER_NUMERATOR, 1, 4);
    g_assert_false(timer_pending(&d.ptimer.timer));
    ptimer_reset(&d);
}

static void test_reset_retains_other_timer(void)
{
    NV2AState a, b;
    init_nv2a_ptimer(&a);
    init_nv2a_ptimer(&b);
    ptimer_write(&a, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&b, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&a, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    ptimer_write(&b, NV_PTIMER_ALARM_0, TEST_ALARM_LOW * 2, 4);
    ptimer_write(&a, NV_PTIMER_ALARM_0, TEST_ALARM_LOW * 3, 4);
    ptimer_reset(&a);
    g_assert_true(timer_pending(&b.ptimer.timer));
    g_assert_cmpint(qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                              QEMU_TIMER_ATTR_ALL), ==, 16);
    ptimer_reset(&b);
}

/* The following 9 x 5 clock/observation matrix is adapted from #59/#81.
 * Keep #120's immediate publication for enabled overdue alarms; do not
 * resurrect the historical due-now deferred callback expectations. */
enum TimebaseChange {
    CHANGE_CORE, CHANGE_NUMERATOR, CHANGE_DENOMINATOR, CHANGE_TIME_LOW,
    CHANGE_TIME_HIGH, CHANGE_CORE_RESTART, CHANGE_PLL,
    CHANGE_NUMERATOR_RESTART, CHANGE_DENOMINATOR_RESTART,
    TIMEBASE_CHANGE_COUNT,
};
enum TimebaseObservation {
    OBSERVE_POLL, OBSERVE_CALLBACK, OBSERVE_MASKED_POLL,
    OBSERVE_ALREADY_PENDING, OBSERVE_MASKED_ACK,
    TIMEBASE_OBSERVATION_COUNT,
};

static void test_timebase_crosses_alarm(gconstpointer opaque)
{
    unsigned scenario = GPOINTER_TO_UINT(opaque);
    enum TimebaseChange change = scenario / TIMEBASE_OBSERVATION_COUNT;
    enum TimebaseObservation observation = scenario % TIMEBASE_OBSERVATION_COUNT;
    bool masked = observation == OBSERVE_MASKED_POLL ||
                  observation == OBSERVE_MASKED_ACK;
    uint32_t initial_pending = observation == OBSERVE_ALREADY_PENDING ? 1 : 0;
    NV2AState d;

    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = 100000000;
    if (change == CHANGE_NUMERATOR || change == CHANGE_NUMERATOR_RESTART) {
        d.pramdac.core_clock_freq = 1000000000;
        d.ptimer.numerator = 10;
    }
    ptimer_test_time_ns = 100;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x1e0, 4);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_0, 4), ==, 0x140);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 150);
    if (masked) {
        ptimer_write(&d, NV_PTIMER_INTR_EN_0, 0, 4);
    }
    d.ptimer.pending_interrupts = initial_pending;
    nv2a_update_irq(&d);
    switch (change) {
    case CHANGE_CORE:
        ptimer_set_core_clock(&d, 1000000000);
        break;
    case CHANGE_NUMERATOR:
        ptimer_write(&d, NV_PTIMER_NUMERATOR, 1, 4);
        break;
    case CHANGE_DENOMINATOR:
        ptimer_write(&d, NV_PTIMER_DENOMINATOR, 10, 4);
        break;
    case CHANGE_TIME_LOW:
        ptimer_write(&d, NV_PTIMER_TIME_0, 0xc80, 4);
        break;
    case CHANGE_TIME_HIGH:
        ptimer_write(&d, NV_PTIMER_TIME_1, 1, 4);
        break;
    case CHANGE_CORE_RESTART:
        ptimer_set_core_clock(&d, 0);
        g_assert_false(timer_pending(&d.ptimer.timer));
        ptimer_set_core_clock(&d, 1000000000);
        break;
    case CHANGE_PLL:
        pramdac_write(&d, NV_PRAMDAC_NVPLL_COEFF, 0x3c01, 4);
        g_assert_cmpuint(d.pramdac.core_clock_freq, ==, 999999960);
        break;
    case CHANGE_NUMERATOR_RESTART:
        ptimer_write(&d, NV_PTIMER_NUMERATOR, 0, 4);
        g_assert_false(timer_pending(&d.ptimer.timer));
        ptimer_write(&d, NV_PTIMER_NUMERATOR, 1, 4);
        break;
    case CHANGE_DENOMINATOR_RESTART:
        ptimer_write(&d, NV_PTIMER_DENOMINATOR, 0, 4);
        g_assert_false(timer_pending(&d.ptimer.timer));
        ptimer_write(&d, NV_PTIMER_DENOMINATOR, 10, 4);
        break;
    default:
        g_assert_not_reached();
    }
    g_assert_true(d.ptimer.alarm_armed);
    g_assert_cmpint(timer_pending(&d.ptimer.timer), ==, !masked);
    if (masked) {
        g_assert_cmphex(d.ptimer.alarm_time, ==, 0x1e0);
        g_assert_cmphex(d.ptimer.pending_interrupts, ==, initial_pending);
        g_assert_false(irq_asserted);
    } else {
        g_assert_cmphex(d.ptimer.alarm_time, >, 0x1e0);
        g_assert_cmphex(d.ptimer.pending_interrupts, ==, 1);
        g_assert_true(irq_asserted);
        g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), >, 100);
    }
    if (observation == OBSERVE_MASKED_ACK) {
        ptimer_write(&d, NV_PTIMER_INTR_0, 1, 4);
        g_assert_cmphex(ptimer_read(&d, NV_PTIMER_INTR_0, 4), ==, 0);
        g_assert_false(irq_asserted);
    } else {
        if (observation == OBSERVE_CALLBACK) {
            /* Simulate an already-consumed early wake after the rate change. */
            fire_alarm_at(&d, ptimer_test_time_ns);
        }
        g_assert_cmphex(ptimer_read(&d, NV_PTIMER_INTR_0, 4), ==, 1);
        g_assert_cmpint(irq_asserted, ==, !masked);
    }
    g_assert_cmphex(d.ptimer.alarm_time, >, 0x1e0);
    if (masked) {
        g_assert_false(timer_pending(&d.ptimer.timer));
        ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
        g_assert_cmpint(irq_asserted, ==, observation != OBSERVE_MASKED_ACK);
    }
    g_assert_true(timer_pending(&d.ptimer.timer));
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), >,
                   ptimer_test_time_ns);
    ptimer_reset(&d);
}

static void test_pll_mmio_and_backward_time(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    pramdac_write(&d, NV_PRAMDAC_NVPLL_COEFF, 0x601, 4);
    g_assert_cmphex(pramdac_read(&d, NV_PRAMDAC_NVPLL_COEFF, 4), ==, 0x601);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 81);
    ptimer_test_time_ns = 20;
    pramdac_write(&d, NV_PRAMDAC_NVPLL_COEFF, 0x301, 4);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 161);
    ptimer_test_time_ns = 161;
    pramdac_write(&d, NV_PRAMDAC_NVPLL_COEFF, 0, 4);
    g_assert_false(timer_pending(&d.ptimer.timer));
    g_assert_true(irq_asserted);
    ptimer_write(&d, NV_PTIMER_INTR_0, 1, 4);
    pramdac_write(&d, NV_PRAMDAC_NVPLL_COEFF, 0x601, 4);
    g_assert_false(irq_asserted);
    ptimer_reset(&d);

    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = 100000000;
    ptimer_test_time_ns = 100;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x1e0, 4);
    ptimer_write(&d, NV_PTIMER_TIME_0, 0x80, 4);
    g_assert_cmphex(ptimer_read(&d, NV_PTIMER_TIME_0, 4), ==, 0x80);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 210);
    g_assert_false(irq_asserted);
    expire_alarm(&d);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

static void test_queue_decisions(void)
{
    const PtimerHostSchedule absent = { 0 };
    const PtimerHostSchedule queued = { .queued = true, .deadline_ns = 8 };
    const PtimerDeadline desired[] = {
        { 0 },
        { .queued = true, .deadline_ns = 0 },
        { .queued = true, .deadline_ns = 7 },
        { .queued = true, .deadline_ns = 8 },
        { .queued = true, .deadline_ns = 9 },
        { .queued = true, .deadline_ns = INT64_MAX },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(desired); i++) {
        g_assert_cmpint(ptimer_queue_action(&absent, &desired[i]), ==,
                        i == 0 ? PTIMER_QUEUE_KEEP : PTIMER_QUEUE_ARM);
        g_assert_cmpint(ptimer_queue_action(&queued, &desired[i]), ==,
                        i == 0 ? PTIMER_QUEUE_CANCEL :
                        i == 3 ? PTIMER_QUEUE_KEEP : PTIMER_QUEUE_ARM);
    }
    g_assert_false(ptimer_schedule_reusable(&absent, 0));
    g_assert_true(ptimer_schedule_reusable(&queued, 7));
    g_assert_false(ptimer_schedule_reusable(&queued, 8));
    g_assert_false(ptimer_schedule_reusable(&queued, 9));
}

static void test_noop_after_queue_removal(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    timer_del(&d.ptimer.timer);
    ptimer_test_time_ns = 1;
    uint64_t mods = ptimer_test_timer_mod_calls;
    ptimer_write(&d, NV_PTIMER_NUMERATOR, 1, 4);
    g_assert_cmpuint(ptimer_test_timer_mod_calls, ==, mods + 1);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, 8);
    expire_alarm(&d);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

static void test_clock_write_publishes_irq_once(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = 100000000;
    ptimer_test_time_ns = 100;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x1e0, 4);
    unsigned updates = irq_update_calls;
    ptimer_write(&d, NV_PTIMER_DENOMINATOR, 10, 4);
    g_assert_cmpuint(irq_update_calls, ==, updates + 1);
    g_assert_true(irq_asserted);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), >, 100);
    ptimer_reset(&d);
}

static void test_one_clock_sample_per_operation(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);

    uint64_t reads = ptimer_test_clock_read_calls;
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW * 2, 4);
    g_assert_cmpuint(ptimer_test_clock_read_calls - reads, ==, 1);

    reads = ptimer_test_clock_read_calls;
    ptimer_write(&d, NV_PTIMER_TIME_0, 0x80, 4);
    g_assert_cmpuint(ptimer_test_clock_read_calls - reads, ==, 1);

    reads = ptimer_test_clock_read_calls;
    ptimer_write(&d, NV_PTIMER_NUMERATOR, 2, 4);
    g_assert_cmpuint(ptimer_test_clock_read_calls - reads, ==, 1);

    reads = ptimer_test_clock_read_calls;
    ptimer_set_core_clock(&d, 500000000);
    g_assert_cmpuint(ptimer_test_clock_read_calls - reads, ==, 1);

    reads = ptimer_test_clock_read_calls;
    expire_alarm(&d);
    g_assert_cmpuint(ptimer_test_clock_read_calls - reads, ==, 1);
    ptimer_reset(&d);
}

static void test_wide_deadline_reconciles_source_wrap(void)
{
    /* Retain PR #81's wide-quotient control, not just ordinary low wrap. */
    NV2AState d;
    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = UINT32_MAX;
    d.ptimer.numerator = UINT32_MAX;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, TEST_ALARM_LOW, 4);
    d.ptimer.alarm_time = 1ULL << 38;
    ptimer_post_load(&d, 5);
    g_assert_cmpuint(timer_expire_time_ns(&d.ptimer.timer), ==,
                    UINT64_C(4294967297000000001));
    expire_alarm(&d);
    g_assert_false(irq_asserted);
    g_assert_cmpuint(timer_expire_time_ns(&d.ptimer.timer), >,
                    UINT64_C(4294967297000000001));
    ptimer_reset(&d);
}

static void test_due_source_wrap_noop_reschedules(void)
{
    NV2AState d;
    init_nv2a_ptimer(&d);
    d.pramdac.core_clock_freq = 4000000000ULL;
    int64_t start = (1LL << 62) - 1;
    ptimer_test_time_ns = start;
    ptimer_write(&d, NV_PTIMER_INTR_EN_0, 1, 4);
    ptimer_write(&d, NV_PTIMER_ALARM_0, 0x180, 4);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, start + 1);
    /* The queued wrap-check is now due, but the guest alarm is not. */
    ptimer_test_time_ns = start + 1;
    ptimer_write(&d, NV_PTIMER_NUMERATOR, 1, 4);
    g_assert_cmpint(timer_expire_time_ns(&d.ptimer.timer), ==, start + 4);
    g_assert_false(irq_asserted);
    expire_alarm(&d);
    g_assert_true(irq_asserted);
    ptimer_reset(&d);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    for (int i = 0; i < QEMU_CLOCK_MAX; i++) {
        main_loop_tlg.tl[i] = g_new0(QEMUTimerList, 1);
    }
    qtest_allowed = true;

    g_test_add_func("/xbox/nv2a/ptimer/alarm-assert-ack",
                    test_alarm_assert_and_ack);
    g_test_add_func("/xbox/nv2a/ptimer/irq-with-other-source",
                    test_irq_contribution_with_other_source);
    g_test_add_func("/xbox/nv2a/ptimer/enable-pending",
                    test_pending_alarm_asserts_when_enabled);
    g_test_add_func("/xbox/nv2a/ptimer/time-registers-epoch",
                    test_time_registers_and_future_epoch);
    g_test_add_func("/xbox/nv2a/ptimer/runtime-overdue",
                    test_runtime_overdue_alarm_skips_missed_epochs);
    g_test_add_func("/xbox/nv2a/ptimer/post-load-overdue",
                    test_post_load_reconciles_overdue_alarm);
    g_test_add_func("/xbox/nv2a/ptimer/intr-read-overdue",
                    test_intr_read_reconciles_overdue_alarm);
    g_test_add_func("/xbox/nv2a/ptimer/intr-enable-overdue",
                    test_intr_enable_reconciles_overdue_alarm);
    g_test_add_func("/xbox/nv2a/ptimer/zero-ratio",
                    test_zero_ratio_stops_clock_without_division);
    g_test_add_func("/xbox/nv2a/ptimer/post-load-irq",
                    test_post_load_rebuilds_irq_without_timer);

    g_test_add_func("/xbox/nv2a/ptimer/reconcile/masked-expiry-ack-before-unmask",
                    test_masked_expiry_ack_before_unmask);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/enabled-expiry-ack-before-callback",
                    test_enabled_expiry_ack_before_callback);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/masked-alarm-replacement-keeps-pending",
                    test_masked_alarm_replacement_keeps_pending);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/full-counter-wrap-zero-alarm",
                    test_full_counter_wrap_zero_alarm);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/phase-correct-first-tick",
                    test_phase_correct_first_tick);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/phase-deadline-forward-oracle",
                    test_phase_deadline_forward_oracle);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/ack-keeps-valid-schedule",
                    test_ack_keeps_valid_schedule);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/core-clock-change-rebuilds-deadline",
                    test_core_clock_change_rebuilds_deadline);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/stopped-equality-preserves-pending",
                    test_stopped_equality_preserves_pending);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/v4-masked-restore-preserves-alarm",
                    test_v4_masked_restore_preserves_alarm);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/v4-queued-zero-restore",
                    test_v4_queued_zero_restore);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/v5-zero-restore-without-host-timer",
                    test_v5_zero_restore_without_host_timer);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/v5-disarmed-restore-stays-disarmed",
                    test_v5_disarmed_restore_stays_disarmed);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/pre-v4-restore-discards-old-alarm",
                    test_pre_v4_restore_discards_old_alarm);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/deadline-saturates-signed-horizon",
                    test_deadline_saturates_signed_horizon);
    g_test_add_func("/xbox/nv2a/ptimer/reconcile/source-counter-wrap-revalidates-alarm",
                    test_source_counter_wrap_revalidates_alarm);

    g_test_add_func("/xbox/nv2a/ptimer/schedule/consumed-early-callback-rearms",
                    test_consumed_early_callback_rearms);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/restore-observes-actual-queue",
                    test_restore_observes_actual_queue);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/signed-horizon-no-loop",
                    test_signed_horizon_does_not_requeue_now);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/reset-retains-other-timer",
                    test_reset_retains_other_timer);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/pll-mmio-backward-time",
                    test_pll_mmio_and_backward_time);
    const char *modes[] = { "enabled", "masked", "stopped" };
    const char *operations[] = {
        "numerator", "denominator", "enable", "alarm", "time-low",
        "time-high", "core-clock", "pll",
    };
    char path[160];
    for (unsigned mode = 0; mode < G_N_ELEMENTS(modes); mode++) {
        for (unsigned op = 0; op < G_N_ELEMENTS(operations); op++) {
            snprintf(path, sizeof(path), "/xbox/nv2a/ptimer/schedule/noop/%s/%s",
                     modes[mode], operations[op]);
            g_test_add_data_func(path, GUINT_TO_POINTER(mode * 8 + op),
                                 test_noop_queue_operations);
        }
    }
    const char *changes[] = {
        "core", "numerator", "denominator", "time-low", "time-high",
        "core-restart", "pll", "numerator-restart", "denominator-restart",
    };
    const char *observations[] = {
        "poll", "callback", "masked-poll", "already-pending", "masked-ack",
    };
    for (unsigned change = 0; change < TIMEBASE_CHANGE_COUNT; change++) {
        for (unsigned obs = 0; obs < TIMEBASE_OBSERVATION_COUNT; obs++) {
            snprintf(path, sizeof(path), "/xbox/nv2a/ptimer/timebase/%s/%s",
                     changes[change], observations[obs]);
            g_test_add_data_func(path,
                GUINT_TO_POINTER(change * TIMEBASE_OBSERVATION_COUNT + obs),
                test_timebase_crosses_alarm);
        }
    }
    g_test_add_func("/xbox/nv2a/ptimer/schedule/queue-decisions",
                    test_queue_decisions);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/noop-after-queue-removal",
                    test_noop_after_queue_removal);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/clock-write-one-irq-update",
                    test_clock_write_publishes_irq_once);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/one-clock-sample-per-operation",
                    test_one_clock_sample_per_operation);
    g_test_add_func("/xbox/nv2a/ptimer/schedule/wide-source-wrap",
                    test_wide_deadline_reconciles_source_wrap);

    g_test_add_func("/xbox/nv2a/ptimer/schedule/due-source-wrap-noop",
                    test_due_source_wrap_noop_reschedules);

    int ret = g_test_run();
    for (int i = 0; i < QEMU_CLOCK_MAX; i++) {
        g_assert_null(main_loop_tlg.tl[i]->active_timers.next);
        g_free(main_loop_tlg.tl[i]);
    }
    return ret;
}
