/*
 * QEMU Geforce NV2A implementation
 * PTIMER - time measurement and time-based alarms
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "nv2a_int.h"
#include "qemu/host-utils.h"
#include "ptimer_core.h"

/* Reconcile the fork's PTIMER semantics from PR #59 / #81 with the upstream
 * rounded-deadline and masked-callback changes integrated by PR #120.
 * Semantic/arithmetic origin:
 * https://github.com/Mainkill1/xemu/commit/8da17c3e68c525475f55f3e9d1ddda0ad9c11d5b
 * Upstream scheduling origin:
 * https://github.com/xemu-project/xemu/commit/75650bd8cd91945f7b79774e2cee0b200ca373ff
 * Keep guest armed state independent of the host timer, including target zero.
 */

#define CLOCK_HIGH_MASK 0x1fffffffULL
#define ALARM_MASK 0xffffffe0ULL

#define PTIMER_REG_TIME_HIGH_MASK ((uint64_t)CLOCK_HIGH_MASK << 32)
#define PTIMER_REG_TIME_LOW_MASK 0xffffffffULL
#define PTIMER_REG_TIME_MASK (PTIMER_REG_TIME_HIGH_MASK | ALARM_MASK)

#define PTIMER_INTERNAL_TIME_SHIFT 5
#define PTIMER_REG_TIME_PER_TICK (1ULL << PTIMER_INTERNAL_TIME_SHIFT)

#define PTIMER_INTERNAL_TIME_MASK \
    (PTIMER_REG_TIME_MASK >> PTIMER_INTERNAL_TIME_SHIFT)

#define PTIMER_INTERNAL_TO_REG_TIME(internal_ticks) \
    (((uint64_t)(internal_ticks) << PTIMER_INTERNAL_TIME_SHIFT) & \
     PTIMER_REG_TIME_MASK)

#define PTIMER_REG_TO_INTERNAL_TIME(reg_time) \
    (((uint64_t)(reg_time) >> PTIMER_INTERNAL_TIME_SHIFT) & \
     PTIMER_INTERNAL_TIME_MASK)

#define PTIMER_MAKE_REG_TIME(time_1, time_0)          \
    ((((uint64_t)(time_1) & CLOCK_HIGH_MASK) << 32) | \
     ((uint64_t)(time_0) & PTIMER_REG_TIME_LOW_MASK))

#define PTIMER_REG_TIME_GET_TIME_0(reg_time) \
    ((uint32_t)((reg_time) & PTIMER_REG_TIME_LOW_MASK))

#define PTIMER_REG_TIME_GET_TIME_1(reg_time) \
    ((uint32_t)(((reg_time) >> 32) & CLOCK_HIGH_MASK))

static void ptimer_alarm_fired(void *opaque);

static inline bool ptimer_clock_running(const NV2AState *d)
{
    return d->ptimer.numerator != 0 && d->ptimer.denominator != 0 &&
           d->pramdac.core_clock_freq != 0;
}

static inline bool ptimer_irq_contribution(const NV2AState *d)
{
    return (d->ptimer.pending_interrupts &
            d->ptimer.enabled_interrupts) != 0;
}

static inline void ptimer_publish_irq_change(NV2AState *d, bool before)
{
    if (before != ptimer_irq_contribution(d)) {
        nv2a_update_irq(d);
    }
}

/* PTIMER MMIO, callbacks and migration run under the BQL. Compare the
 * actual queue state at the effect boundary: callback consumption or VMState
 * replacement must not be hidden by a cached deadline from an earlier call.
 */
static PtimerHostSchedule ptimer_host_schedule(NV2AState *d)
{
    PtimerHostSchedule host = {
        .queued = timer_pending(&d->ptimer.timer),
    };

    if (host.queued) {
        host.deadline_ns = timer_expire_time_ns(&d->ptimer.timer);
    }

    return host;
}

static void ptimer_apply_deadline(NV2AState *d, const PtimerDeadline *desired)
{
    PtimerHostSchedule host = ptimer_host_schedule(d);

    switch (ptimer_queue_action(&host, desired)) {
    case PTIMER_QUEUE_KEEP:
        break;
    case PTIMER_QUEUE_CANCEL:
        timer_del(&d->ptimer.timer);
        break;
    case PTIMER_QUEUE_ARM:
        timer_mod(&d->ptimer.timer, desired->deadline_ns);
        break;
    }
}

void ptimer_reset(NV2AState *d)
{
    d->ptimer.alarm_armed = false;
    d->ptimer.alarm_time = 0;
    d->ptimer.time_offset = 0;
    const PtimerDeadline absent = { 0 };
    ptimer_apply_deadline(d, &absent);
}

void ptimer_init(NV2AState *d)
{
    timer_init_ns(&d->ptimer.timer, QEMU_CLOCK_VIRTUAL, ptimer_alarm_fired, d);
    ptimer_reset(d);
}

typedef struct PtimerClockSample {
    bool valid;
    int64_t now_ns;
} PtimerClockSample;

/* One virtual instant per MMIO operation/callback. Keep the two forward
 * truncations and resample arithmetic under any changed clock mapping.
 */
static int64_t ptimer_now_ns(PtimerClockSample *sample)
{
    if (!sample->valid) {
        sample->now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        sample->valid = true;
    }
    return sample->now_ns;
}

static uint64_t ptimer_get_absolute_clock(NV2AState *d,
                                          PtimerClockSample *sample)
{
    /*
     * The ratio registers reset to zero and are guest writable. Keep their
     * raw values guest-visible, but define a stopped clock until the complete
     * ratio and source clock are non-zero. This avoids guest-triggerable
     * division by zero without inventing a non-zero register value.
     */
    if (!ptimer_clock_running(d)) {
        return 0;
    }

    return muldiv64(muldiv64(ptimer_now_ns(sample),
                             d->pramdac.core_clock_freq,
                             NANOSECONDS_PER_SECOND),
                    d->ptimer.denominator, d->ptimer.numerator);
}

static inline uint64_t get_internal_clock(NV2AState *d, uint64_t absolute_clock)
{
    return (absolute_clock + d->ptimer.time_offset) & PTIMER_INTERNAL_TIME_MASK;
}

static inline uint64_t get_reg_time(NV2AState *d, PtimerClockSample *sample)
{
    uint64_t internal_clock =
        get_internal_clock(d, ptimer_get_absolute_clock(d, sample));
    return PTIMER_INTERNAL_TO_REG_TIME(internal_clock);
}

static void ptimer_div_ceil(uint64_t *lo, uint64_t *hi, uint64_t divisor)
{
    uint64_t remainder = divu128(lo, hi, divisor);

    if (remainder != 0) {
        (*lo)++;
        if (*lo == 0) {
            (*hi)++;
        }
    }
}

static uint64_t ptimer_sample_clock(NV2AState *d, int64_t now_ns,
                                   uint64_t *gpu_clock, uint64_t *gpu_phase,
                                   uint64_t *timer_phase)
{
    uint64_t lo, hi;

    /* Match both truncations in ptimer_get_absolute_clock(). */
    mulu64(&lo, &hi, now_ns, d->pramdac.core_clock_freq);
    *gpu_phase = divu128(&lo, &hi, NANOSECONDS_PER_SECOND);
    *gpu_clock = lo;
    mulu64(&lo, &hi, lo, d->ptimer.denominator);
    *timer_phase = divu128(&lo, &hi, d->ptimer.numerator);
    return PTIMER_INTERNAL_TO_REG_TIME(get_internal_clock(d, lo));
}

static uint64_t ptimer_ticks_to_ns(NV2AState *d, uint64_t internal_ticks,
                                  uint64_t gpu_clock, uint64_t gpu_phase,
                                  uint64_t timer_phase)
{
    uint64_t lo, hi;

    assert(ptimer_clock_running(d) && internal_ticks > 0);

    /* Invert each quantization using its phase at the same clock sample:
     * delta_gpu = ceil((ticks * numerator - timer_phase) / denominator)
     * delta_ns  = ceil((delta_gpu * 1e9 - gpu_phase) / core_clock_freq)
     * Keep wide quotients until after checking the source-clock wrap.
     */
    mulu64(&lo, &hi, internal_ticks, d->ptimer.numerator);
    hi -= lo < timer_phase;
    lo -= timer_phase;
    ptimer_div_ceil(&lo, &hi, d->ptimer.denominator);

    /* The forward model truncates GPU ticks to 64 bits. Reconcile at that
     * discontinuity instead of extrapolating the ratio through it.
     * For gpu_clock == 0 the distance to wrap is exactly 2^64.
     */
    uint64_t until_wrap = -gpu_clock;
    if (hi || (gpu_clock && lo > until_wrap)) {
        lo = until_wrap;
        hi = !gpu_clock;
    }
    if (mulu128(&lo, &hi, NANOSECONDS_PER_SECOND)) {
        return UINT64_MAX;
    }
    hi -= lo < gpu_phase;
    lo -= gpu_phase;
    ptimer_div_ceil(&lo, &hi, d->pramdac.core_clock_freq);

    return hi ? UINT64_MAX : lo;
}

static inline uint64_t ptimer_alarm_distance(uint64_t reg_now,
                                             uint64_t alarm_time)
{
    uint64_t diff = (alarm_time - reg_now) & PTIMER_REG_TIME_MASK;
    if (diff > (PTIMER_REG_TIME_MASK >> 1)) {
        return 0;
    }
    return diff;
}

static inline bool is_alarm_reached(uint64_t reg_now, uint64_t alarm_time)
{
    return !ptimer_alarm_distance(reg_now, alarm_time);
}

static inline uint64_t advance_alarm_epoch(uint64_t reg_time)
{
    return (reg_time + (1ULL << 32)) & PTIMER_REG_TIME_MASK;
}

static uint64_t next_alarm_time(uint64_t reg_now, uint32_t alarm_low)
{
    uint32_t now_low = PTIMER_REG_TIME_GET_TIME_0(reg_now) & ALARM_MASK;
    uint64_t target =
        (reg_now & ~PTIMER_REG_TIME_LOW_MASK) | (alarm_low & ALARM_MASK);

    if ((alarm_low & ALARM_MASK) <= now_low) {
        target = advance_alarm_epoch(target);
    }
    return target & PTIMER_REG_TIME_MASK;
}

static bool ptimer_latch_overdue_alarm(NV2AState *d, uint64_t reg_now)
{
    if (!d->ptimer.alarm_armed || !ptimer_clock_running(d) ||
        !is_alarm_reached(reg_now, d->ptimer.alarm_time)) {
        return false;
    }

    d->ptimer.pending_interrupts |= NV_PTIMER_INTR_0_ALARM;
    d->ptimer.alarm_time = next_alarm_time(
        reg_now, PTIMER_REG_TIME_GET_TIME_0(d->ptimer.alarm_time));
    return true;
}

static bool ptimer_alarm_armed(NV2AState *d)
{
    return d->ptimer.alarm_armed;
}

/* Return whether an elapsed occurrence was materialized. The caller
 * publishes the final IRQ contribution once, after all state/queue effects.
 */
static bool schedule_qemu_timer(NV2AState *d, bool state_changed,
                                PtimerClockSample *sample)
{
    PtimerDeadline desired = { 0 };
    bool caught_up = false;

    if (!ptimer_alarm_armed(d) || !ptimer_clock_running(d) ||
        !(d->ptimer.enabled_interrupts & NV_PTIMER_INTR_0_ALARM)) {
        /* Cancel only an existing callback; retain guest armed state. */
        ptimer_apply_deadline(d, &desired);
        return false;
    }

    int64_t now_ns = ptimer_now_ns(sample);
    if (!state_changed) {
        PtimerHostSchedule host = ptimer_host_schedule(d);

        if (ptimer_schedule_reusable(&host, now_ns)) {
            /* Skip both phase sampling and inversion on a true no-op write.
             * Reconciliation already checked elapsed state before the write.
             */
            return false;
        }
    }
    uint64_t gpu_clock, gpu_phase, timer_phase;
    uint64_t reg_now = ptimer_sample_clock(d, now_ns, &gpu_clock,
                                         &gpu_phase, &timer_phase);
    uint64_t diff_reg_time =
        ptimer_alarm_distance(reg_now, d->ptimer.alarm_time);

    if (!diff_reg_time) {
        caught_up = ptimer_latch_overdue_alarm(d, reg_now);
        diff_reg_time =
            ptimer_alarm_distance(reg_now, d->ptimer.alarm_time);
    }

    /* There is no representable future deadline at the signed horizon.
     * Requeuing INT64_MAX here would spin inside one timer-dispatch pass.
     * The comparator stays armed and can still be observed or restored.
     */
    if (now_ns == INT64_MAX) {
        ptimer_apply_deadline(d, &desired);
        return caught_up;
    }

    uint64_t internal_diff_ticks = PTIMER_REG_TO_INTERNAL_TIME(
        ROUND_UP(diff_reg_time, PTIMER_REG_TIME_PER_TICK));
    uint64_t diff_ns = ptimer_ticks_to_ns(d, internal_diff_ticks,
                                         gpu_clock, gpu_phase, timer_phase);
    diff_ns = MAX(diff_ns, 1);

    /* Do not shift the deadline with a second sample, or wrap it signed. */
    desired.queued = true;
    desired.deadline_ns =
        diff_ns > (uint64_t)INT64_MAX - (uint64_t)now_ns ?
        INT64_MAX : now_ns + diff_ns;
    ptimer_apply_deadline(d, &desired);
    return caught_up;
}

static void ptimer_alarm_fired(void *opaque)
{
    NV2AState *d = (NV2AState *)opaque;
    PtimerClockSample sample = { 0 };
    bool irq_before = ptimer_irq_contribution(d);
    uint64_t reg_now = get_reg_time(d, &sample);
    ptimer_latch_overdue_alarm(d, reg_now);

    /* QEMU removed the event before calling us. The queue adapter observes
     * that removal even if an early wake retains the same desired expiry. */
    schedule_qemu_timer(d, true, &sample);
    ptimer_publish_irq_change(d, irq_before);
}

void ptimer_post_load(NV2AState *d, int version_id)
{
    PtimerClockSample sample = { 0 };
    if (version_id < 4) {
        /* These streams contain no alarm/time/host-timer fields. */
        ptimer_reset(d);
    } else if (version_id == 4) {
        /* Pre-#120 v4 stored arming in the host timer. Post-#120 v4 also
         * permits a masked nonzero guest alarm with no host callback.
         * Preserve both encodings; v5 stores zero-target arming explicitly.
         */
        d->ptimer.alarm_armed = timer_pending(&d->ptimer.timer) ||
                               d->ptimer.alarm_time != 0;
    }

    ptimer_latch_overdue_alarm(d, get_reg_time(d, &sample));
    schedule_qemu_timer(d, true, &sample);
    nv2a_update_irq(d);
}

void ptimer_set_core_clock(NV2AState *d, uint64_t frequency)
{
    PtimerClockSample sample = { 0 };
    /* Materialize elapsed state under the old frequency. Retain the
     * existing absolute-time clock model, not an anchored-clock redesign.
     */
    bool irq_before = ptimer_irq_contribution(d);
    bool caught_up = ptimer_latch_overdue_alarm(d, get_reg_time(d, &sample));
    bool state_changed = caught_up || d->pramdac.core_clock_freq != frequency;
    d->pramdac.core_clock_freq = frequency;
    schedule_qemu_timer(d, state_changed, &sample);
    ptimer_publish_irq_change(d, irq_before);
}

uint64_t ptimer_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = opaque;
    PtimerClockSample sample = { 0 };

    uint64_t r = 0;
    switch (addr) {
    case NV_PTIMER_INTR_0: {
        bool irq_before = ptimer_irq_contribution(d);
        if (ptimer_alarm_armed(d)) {
            uint64_t reg_now = get_reg_time(d, &sample);
            if (ptimer_latch_overdue_alarm(d, reg_now)) {
                if (d->ptimer.enabled_interrupts & NV_PTIMER_INTR_0_ALARM) {
                    schedule_qemu_timer(d, true, &sample);
                }
            }
        }
        ptimer_publish_irq_change(d, irq_before);
        r = d->ptimer.pending_interrupts;
    } break;
    case NV_PTIMER_INTR_EN_0:
        r = d->ptimer.enabled_interrupts;
        break;
    case NV_PTIMER_NUMERATOR:
        r = d->ptimer.numerator;
        break;
    case NV_PTIMER_DENOMINATOR:
        r = d->ptimer.denominator;
        break;
    case NV_PTIMER_TIME_0: {
        uint64_t reg_now = get_reg_time(d, &sample);
        r = PTIMER_REG_TIME_GET_TIME_0(reg_now);
    } break;
    case NV_PTIMER_TIME_1: {
        uint64_t reg_now = get_reg_time(d, &sample);
        r = PTIMER_REG_TIME_GET_TIME_1(reg_now);
    } break;
    case NV_PTIMER_ALARM_0:
        r = PTIMER_REG_TIME_GET_TIME_0(d->ptimer.alarm_time);
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PTIMER, addr, size, r);
    return r;
}

void ptimer_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = opaque;
    PtimerClockSample sample = { 0 };
    bool irq_before = ptimer_irq_contribution(d);

    nv2a_reg_log_write(NV_PTIMER, addr, size, val);

    bool caught_up = false;

    /* Reconcile under the old state before W1C, reprogramming or a clock
     * change. Otherwise a later poll/unmask can resurrect an acknowledged
     * occurrence or silently discard one when ALARM is replaced.
     */
    switch (addr) {
    case NV_PTIMER_INTR_0:
    case NV_PTIMER_INTR_EN_0:
    case NV_PTIMER_NUMERATOR:
    case NV_PTIMER_DENOMINATOR:
    case NV_PTIMER_ALARM_0:
    case NV_PTIMER_TIME_0:
    case NV_PTIMER_TIME_1:
        if (ptimer_alarm_armed(d) && ptimer_clock_running(d)) {
            caught_up = ptimer_latch_overdue_alarm(d,
                                                  get_reg_time(d, &sample));
        }
        break;
    default:
        break;
    }

    switch (addr) {
    case NV_PTIMER_INTR_0:
        d->ptimer.pending_interrupts &= ~val;
        /* Preserve upstream's no-requeue path for an unchanged ACK. */
        if (caught_up &&
            (d->ptimer.enabled_interrupts & NV_PTIMER_INTR_0_ALARM)) {
            schedule_qemu_timer(d, true, &sample);
        }
        break;
    case NV_PTIMER_INTR_EN_0: {
        bool changed = caught_up ||
                       d->ptimer.enabled_interrupts != (uint32_t)val;
        d->ptimer.enabled_interrupts = val;
        schedule_qemu_timer(d, changed, &sample);
    } break;
    case NV_PTIMER_DENOMINATOR: {
        bool changed = caught_up || d->ptimer.denominator != (uint32_t)val;
        d->ptimer.denominator = val;
        if (ptimer_alarm_armed(d)) {
            schedule_qemu_timer(d, changed, &sample);
        }
    } break;
    case NV_PTIMER_NUMERATOR: {
        bool changed = caught_up || d->ptimer.numerator != (uint32_t)val;
        d->ptimer.numerator = val;
        if (ptimer_alarm_armed(d)) {
            schedule_qemu_timer(d, changed, &sample);
        }
    } break;
    case NV_PTIMER_ALARM_0: {
        uint64_t reg_now = get_reg_time(d, &sample);
        uint64_t alarm_time = next_alarm_time(reg_now, val);
        bool changed = caught_up || !d->ptimer.alarm_armed ||
                       d->ptimer.alarm_time != alarm_time;
        d->ptimer.alarm_time = alarm_time;
        d->ptimer.alarm_armed = true;
        schedule_qemu_timer(d, changed, &sample);
    } break;
    case NV_PTIMER_TIME_0: {
        uint64_t current_reg_time = get_reg_time(d, &sample);
        uint64_t target_reg_time = PTIMER_MAKE_REG_TIME(
            PTIMER_REG_TIME_GET_TIME_1(current_reg_time), val & ALARM_MASK);
        uint64_t target_internal = PTIMER_REG_TO_INTERNAL_TIME(target_reg_time);
        d->ptimer.time_offset = target_internal -
                                ptimer_get_absolute_clock(d, &sample);
        if (ptimer_alarm_armed(d)) {
            schedule_qemu_timer(d, true, &sample);
        }
    } break;
    case NV_PTIMER_TIME_1: {
        uint64_t current_reg_time = get_reg_time(d, &sample);
        uint64_t target_reg_time = PTIMER_MAKE_REG_TIME(
            val, PTIMER_REG_TIME_GET_TIME_0(current_reg_time));
        uint64_t target_internal = PTIMER_REG_TO_INTERNAL_TIME(target_reg_time);
        d->ptimer.time_offset = target_internal -
                                ptimer_get_absolute_clock(d, &sample);
        if (ptimer_alarm_armed(d)) {
            schedule_qemu_timer(d, true, &sample);
        }
    } break;
    default:
        break;
    }
    ptimer_publish_irq_change(d, irq_before);
}
