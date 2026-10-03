/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "system/cpus.h"

unsigned int xemu_tweaks_active;
const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = { 0 };
static unsigned int kicks;
static unsigned int irq_updates;
static unsigned int profile_updates;
static _Thread_local bool test_bql_held;
static QemuMutex test_bql;
static _Thread_local bool publishing_context;
static QemuEvent publication_ready;
static uint32_t published_pending;
static uint32_t published_mask;

bool bql_locked(void)
{
    return test_bql_held;
}

void bql_lock_impl(const char *file, int line)
{
    g_assert_false(test_bql_held);
    if (publishing_context) {
        qemu_event_set(&publication_ready);
    }
    qemu_mutex_lock(&test_bql);
    test_bql_held = true;
}

void bql_unlock(void)
{
    g_assert_true(test_bql_held);
    test_bql_held = false;
    qemu_mutex_unlock(&test_bql);
}

bool mutex_is_bql(QemuMutex *mutex)
{
    return mutex == &test_bql;
}

void bql_update_status(bool locked)
{
    test_bql_held = locked;
}

void bql_block_unlock(bool increase)
{
    g_assert_true(bql_locked());
}

uint64_t memory_region_size(MemoryRegion *mr)
{
    g_assert_not_reached();
}

void pfifo_kick(NV2AState *d)
{
    kicks++;
}

void nv2a_update_irq(NV2AState *d)
{
    g_assert_true(bql_locked());
    irq_updates++;
    published_pending = d->pgraph.pending_interrupts;
    published_mask = d->pgraph.enabled_interrupts;
}

int64_t nv2a_profile_increment(void)
{
    g_assert_cmpuint(kicks, ==, 0);
    profile_updates++;
    return 123;
}

void nv2a_profile_log_increment(int64_t now)
{
    g_assert_cmpint(now, ==, 123);
    g_assert_cmpuint(kicks, ==, 1);
}

uint32_t pgraph_rdi_read(PGRAPHState *pg, unsigned int select,
                         unsigned int address)
{
    g_assert_not_reached();
}

void pgraph_rdi_write(PGRAPHState *pg, unsigned int select,
                      unsigned int address, uint32_t value)
{
    g_assert_not_reached();
}

typedef struct ControlCall {
    NV2AState *d;
    unsigned int address;
    uint32_t value;
    bool write;
    GMutex mutex;
    GCond condition;
    bool entered;
    bool completed;
    uint64_t result;
} ControlCall;

static void *control_call(void *opaque)
{
    ControlCall *call = opaque;
    g_mutex_lock(&call->mutex);
    call->entered = true;
    g_cond_broadcast(&call->condition);
    g_mutex_unlock(&call->mutex);
    bql_lock();
    if (call->write) {
        pgraph_write(call->d, call->address, call->value, 4);
    } else {
        call->result = pgraph_read(call->d, call->address, 4);
    }
    bql_unlock();
    g_mutex_lock(&call->mutex);
    call->completed = true;
    g_cond_broadcast(&call->condition);
    g_mutex_unlock(&call->mutex);
    return NULL;
}

/* Exercise the actual MMIO functions while another thread owns the renderer
 * state lock. Control traffic must finish before that owner releases it. */
static void control_while_renderer_busy(unsigned int address, bool write,
                                        uint32_t value, uint32_t expected)
{
    NV2AState *d = g_new0(NV2AState, 1);
    qemu_mutex_init(&d->pgraph.lock);
    qemu_mutex_init(&d->pfifo.lock);
    d->pgraph.pending_interrupts = NV_PGRAPH_INTR_ERROR;
    d->pgraph.enabled_interrupts = NV_PGRAPH_INTR_ERROR;
    d->pgraph.waiting_for_nop = true;
    d->pgraph.regs_[NV_PGRAPH_FIFO] = NV_PGRAPH_FIFO_ACCESS;
    d->pgraph.regs_[NV_PGRAPH_INCREMENT] = 0x5d;
    d->pgraph.regs_[NV_PGRAPH_SURFACE] =
        (3U << 28) | (2U << 24) | (1U << 20) | 0x1234;
    kicks = irq_updates = profile_updates = 0;
    ControlCall call = {
        .d = d, .address = address, .write = write, .value = value
    };
    g_mutex_init(&call.mutex);
    g_cond_init(&call.condition);
    qemu_mutex_lock(&d->pgraph.lock);
    QemuThread thread;
    qemu_thread_create(&thread, "pgraph-control", control_call, &call,
                       QEMU_THREAD_JOINABLE);
    g_mutex_lock(&call.mutex);
    while (!call.entered) {
        g_cond_wait(&call.condition, &call.mutex);
    }
    int64_t deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (!call.completed &&
           g_cond_wait_until(&call.condition, &call.mutex, deadline)) {
    }
    bool completed_before_release = call.completed;
    g_mutex_unlock(&call.mutex);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_thread_join(&thread);
    g_assert_true(completed_before_release);
    if (!write) {
        g_assert_cmpuint(call.result, ==, expected);
    } else if (address == NV_PGRAPH_FIFO) {
        g_assert_cmpuint(d->pgraph.regs_[NV_PGRAPH_FIFO], ==, value);
        g_assert_cmpuint(kicks, ==, 1);
        g_assert_true(
            bitmap_empty(d->pgraph.regs_dirty, 0x2000 / sizeof(uint32_t)));
        g_assert_false(d->pgraph.regs_written_since_draw);
    } else if (address == NV_PGRAPH_INCREMENT) {
        uint32_t surface = d->pgraph.regs_[NV_PGRAPH_SURFACE];
        g_assert_cmpuint(surface, ==, (3U << 28) | (1U << 20) | 0x1234);
        g_assert_cmpuint(kicks, ==, 1);
        g_assert_cmpuint(profile_updates, ==, 1);
        g_assert_true(
            bitmap_empty(d->pgraph.regs_dirty, 0x2000 / sizeof(uint32_t)));
        g_assert_false(d->pgraph.regs_written_since_draw);
    } else if (address == NV_PGRAPH_SURFACE) {
        g_assert_cmpuint(d->pgraph.regs_[NV_PGRAPH_SURFACE], ==, value);
        g_assert_cmpuint(kicks, ==, 1);
        g_assert_cmpuint(profile_updates, ==, 0);
        g_assert_true(
            bitmap_empty(d->pgraph.regs_dirty, 0x2000 / sizeof(uint32_t)));
        g_assert_false(d->pgraph.regs_written_since_draw);
    } else if (address == NV_PGRAPH_INTR) {
        g_assert_cmpuint(d->pgraph.pending_interrupts, ==, 0);
        g_assert_false(d->pgraph.waiting_for_nop);
        g_assert_cmpuint(kicks, ==, 1);
        g_assert_cmpuint(irq_updates, ==, 1);
    } else if (address == NV_PGRAPH_INTR_EN) {
        g_assert_cmpuint(d->pgraph.enabled_interrupts, ==, value);
        g_assert_cmpuint(irq_updates, ==, 1);
    }
    g_cond_clear(&call.condition);
    g_mutex_clear(&call.mutex);
    qemu_mutex_destroy(&d->pfifo.lock);
    qemu_mutex_destroy(&d->pgraph.lock);
    g_free(d);
}

static void intr_read(void)
{
    control_while_renderer_busy(NV_PGRAPH_INTR, false, 0, NV_PGRAPH_INTR_ERROR);
}

static void mask_read(void)
{
    control_while_renderer_busy(NV_PGRAPH_INTR_EN, false, 0,
                                NV_PGRAPH_INTR_ERROR);
}

static void fifo_read(void)
{
    control_while_renderer_busy(NV_PGRAPH_FIFO, false, 0,
                                NV_PGRAPH_FIFO_ACCESS);
}

static void intr_ack(void)
{
    control_while_renderer_busy(NV_PGRAPH_INTR, true, NV_PGRAPH_INTR_ERROR, 0);
}

static void mask_write(void)
{
    control_while_renderer_busy(NV_PGRAPH_INTR_EN, true, 0, 0);
}

static void mask_write_while_pfifo_busy(void)
{
    NV2AState *d = g_new0(NV2AState, 1);
    qemu_mutex_init(&d->pgraph.lock);
    qemu_mutex_init(&d->pfifo.lock);
    d->pgraph.pending_interrupts = NV_PGRAPH_INTR_ERROR;
    ControlCall call = { .d = d,
                         .address = NV_PGRAPH_INTR_EN,
                         .write = true,
                         .value = NV_PGRAPH_INTR_ERROR };
    g_mutex_init(&call.mutex);
    g_cond_init(&call.condition);
    irq_updates = 0;
    qemu_mutex_lock(&d->pfifo.lock);
    QemuThread thread;
    qemu_thread_create(&thread, "pgraph-mask", control_call, &call,
                       QEMU_THREAD_JOINABLE);
    g_mutex_lock(&call.mutex);
    while (!call.entered) {
        g_cond_wait(&call.condition, &call.mutex);
    }
    int64_t deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (!call.completed &&
           g_cond_wait_until(&call.condition, &call.mutex, deadline)) {
    }
    bool completed_before_release = call.completed;
    g_mutex_unlock(&call.mutex);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_thread_join(&thread);
    g_assert_true(completed_before_release);
    g_assert_cmpuint(d->pgraph.enabled_interrupts, ==, NV_PGRAPH_INTR_ERROR);
    g_assert_cmpuint(irq_updates, ==, 1);
    g_cond_clear(&call.condition);
    g_mutex_clear(&call.mutex);
    qemu_mutex_destroy(&d->pfifo.lock);
    qemu_mutex_destroy(&d->pgraph.lock);
    g_free(d);
}

static void fifo_write(void)
{
    control_while_renderer_busy(NV_PGRAPH_FIFO, true, 0, 0);
}

static void increment_read(void)
{
    control_while_renderer_busy(NV_PGRAPH_INCREMENT, false, 0, 0x5d);
}

static void increment_write(void)
{
    control_while_renderer_busy(NV_PGRAPH_INCREMENT, true,
                                NV_PGRAPH_INCREMENT_READ_3D, 0);
}

static void surface_read(void)
{
    control_while_renderer_busy(NV_PGRAPH_SURFACE, false, 0,
                                (3U << 28) | (2U << 24) | (1U << 20) | 0x1234);
}

static void surface_write(void)
{
    control_while_renderer_busy(NV_PGRAPH_SURFACE, true, 0x23456789, 0);
}

static void acknowledge_only_requested_bits(void)
{
    NV2AState *d = g_new0(NV2AState, 1);
    qemu_mutex_init(&d->pfifo.lock);
    qemu_mutex_init(&d->pgraph.lock);
    d->pgraph.pending_interrupts =
        NV_PGRAPH_INTR_ERROR | NV_PGRAPH_INTR_CONTEXT_SWITCH;
    d->pgraph.enabled_interrupts = d->pgraph.pending_interrupts;
    d->pgraph.waiting_for_nop = true;
    d->pgraph.waiting_for_context_switch = true;
    irq_updates = 0;
    bql_lock();
    pgraph_write(d, NV_PGRAPH_INTR, NV_PGRAPH_INTR_ERROR, 4);
    g_assert_cmpuint(pgraph_read(d, NV_PGRAPH_INTR, 4), ==,
                     NV_PGRAPH_INTR_CONTEXT_SWITCH);
    g_assert_false(d->pgraph.waiting_for_nop);
    g_assert_true(d->pgraph.waiting_for_context_switch);
    g_assert_cmpuint(irq_updates, ==, 1);
    g_assert_cmpuint(published_pending, ==, NV_PGRAPH_INTR_CONTEXT_SWITCH);
    pgraph_write(d, NV_PGRAPH_INTR_EN, 0, 4);
    g_assert_cmpuint(published_mask, ==, 0);
    g_assert_cmpuint(published_pending, ==, NV_PGRAPH_INTR_CONTEXT_SWITCH);
    pgraph_write(d, NV_PGRAPH_INTR_EN, NV_PGRAPH_INTR_CONTEXT_SWITCH, 4);
    g_assert_cmpuint(published_mask, ==, NV_PGRAPH_INTR_CONTEXT_SWITCH);
    pgraph_write(d, NV_PGRAPH_INTR, NV_PGRAPH_INTR_CONTEXT_SWITCH, 4);
    g_assert_cmpuint(published_pending, ==, 0);
    g_assert_false(d->pgraph.waiting_for_context_switch);
    g_assert_cmpuint(irq_updates, ==, 4);
    bql_unlock();
    qemu_mutex_destroy(&d->pgraph.lock);
    qemu_mutex_destroy(&d->pfifo.lock);
    g_free(d);
}

static void *publish_context(void *opaque)
{
    NV2AState *d = opaque;
    qemu_mutex_lock(&d->pgraph.lock);
    publishing_context = true;
    pgraph_context_switch(d, 1);
    publishing_context = false;
    qemu_mutex_unlock(&d->pgraph.lock);
    return NULL;
}

static void acknowledge_before_publication(void)
{
    NV2AState *d = g_new0(NV2AState, 1);
    qemu_mutex_init(&d->pgraph.lock);
    qemu_mutex_init(&d->pfifo.lock);
    qemu_event_init(&publication_ready, false);
    d->pgraph.pending_interrupts = NV_PGRAPH_INTR_ERROR;
    d->pgraph.waiting_for_nop = true;
    bql_lock();
    QemuThread thread;
    qemu_thread_create(&thread, "pgraph-publish", publish_context, d,
                       QEMU_THREAD_JOINABLE);
    /* The real producer has released the renderer lock and is waiting at
     * the BQL boundary. ACK cannot clear a not-yet-published wait flag. */
    qemu_event_wait(&publication_ready);
    pgraph_write(d, NV_PGRAPH_INTR,
                 NV_PGRAPH_INTR_ERROR | NV_PGRAPH_INTR_CONTEXT_SWITCH, 4);
    g_assert_cmpuint(d->pgraph.pending_interrupts, ==, 0);
    bql_unlock();
    qemu_thread_join(&thread);
    bql_lock();
    g_assert_cmpuint(d->pgraph.pending_interrupts, ==,
                     NV_PGRAPH_INTR_CONTEXT_SWITCH);
    g_assert_true(d->pgraph.waiting_for_context_switch);
    g_assert_false(d->pgraph.waiting_for_nop);
    pgraph_write(d, NV_PGRAPH_INTR, NV_PGRAPH_INTR_CONTEXT_SWITCH, 4);
    g_assert_false(d->pgraph.waiting_for_context_switch);
    bql_unlock();
    qemu_event_destroy(&publication_ready);
    qemu_mutex_destroy(&d->pfifo.lock);
    qemu_mutex_destroy(&d->pgraph.lock);
    g_free(d);
}

typedef struct FlipWriter {
    PGRAPHState *pg;
    QemuEvent *start;
    uint32_t mask;
    unsigned int count;
    bool full_write;
} FlipWriter;

static void *flip_writer(void *opaque)
{
    FlipWriter *writer = opaque;
    qemu_event_wait(writer->start);
    uint32_t generation = 0;
    for (unsigned int i = 1; i <= writer->count; i++) {
        if (writer->full_write) {
            pgraph_reg_w(writer->pg, NV_PGRAPH_SURFACE,
                         (7U << 28) | (3U << 24) | (4U << 20) | i);
        } else {
            pgraph_flip_increment(writer->pg, writer->mask);
        }
        uint32_t seen = pgraph_reg_r(writer->pg, NV_PGRAPH_SURFACE) & 0xffff;
        g_assert_cmpuint(seen, >=, generation);
        generation = seen;
        if (i % 127 == 0) {
            g_thread_yield();
        }
    }
    return NULL;
}

static void concurrent_flip_updates(bool full_writes)
{
    NV2AState *d = g_new0(NV2AState, 1);
    pgraph_reg_w(&d->pgraph, NV_PGRAPH_SURFACE, (7U << 28) | 0x1234);
    if (full_writes) {
        pgraph_reg_w(&d->pgraph, NV_PGRAPH_SURFACE, 7U << 28);
    }
    QemuEvent start;
    qemu_event_init(&start, false);
    FlipWriter writers[] = {
        { .pg = &d->pgraph,
          .start = &start,
          .mask = NV_PGRAPH_SURFACE_READ_3D,
          .count = 20003 },
        { .pg = &d->pgraph,
          .start = &start,
          .mask = NV_PGRAPH_SURFACE_WRITE_3D,
          .count = 40001 },
        { .pg = &d->pgraph,
          .start = &start,
          .full_write = true,
          .count = 20000 },
    };
    QemuThread threads[3];
    unsigned int count = full_writes ? 3 : 2;
    for (unsigned int i = 0; i < count; i++) {
        qemu_thread_create(&threads[i], "flip-writer", flip_writer, &writers[i],
                           QEMU_THREAD_JOINABLE);
    }
    qemu_event_set(&start);
    for (unsigned int i = 0; i < count; i++) {
        qemu_thread_join(&threads[i]);
    }
    uint32_t state = pgraph_reg_r(&d->pgraph, NV_PGRAPH_SURFACE);
    g_assert_cmpuint(GET_MASK(state, NV_PGRAPH_SURFACE_MODULO_3D), ==, 7);
    if (full_writes) {
        /* Masked updates must never resurrect a stale full-write generation. */
        g_assert_cmpuint(state & 0xffff, ==, writers[2].count);
    } else {
        g_assert_cmpuint(GET_MASK(state, NV_PGRAPH_SURFACE_READ_3D), ==,
                         writers[0].count % 7);
        g_assert_cmpuint(GET_MASK(state, NV_PGRAPH_SURFACE_WRITE_3D), ==,
                         writers[1].count % 7);
        g_assert_cmpuint(state & 0xffff, ==, 0x1234);
    }
    g_assert_true(
        bitmap_empty(d->pgraph.regs_dirty, 0x2000 / sizeof(uint32_t)));
    g_assert_false(d->pgraph.regs_written_since_draw);
    qemu_event_destroy(&start);
    g_free(d);
}

static void concurrent_counters(void)
{
    concurrent_flip_updates(false);
}

static void concurrent_full_writes(void)
{
    concurrent_flip_updates(true);
}

static void flip_masked_set(void)
{
    NV2AState *d = g_new0(NV2AState, 1);
    pgraph_reg_w(&d->pgraph, NV_PGRAPH_SURFACE,
                 (7U << 28) | (3U << 24) | (4U << 20) | 0x1234);
    pgraph_flip_set(&d->pgraph, NV_PGRAPH_SURFACE_READ_3D, 5);
    pgraph_flip_set(&d->pgraph, NV_PGRAPH_SURFACE_WRITE_3D, 1);
    pgraph_flip_set(&d->pgraph, NV_PGRAPH_SURFACE_MODULO_3D, 6);
    g_assert_cmpuint(pgraph_reg_r(&d->pgraph, NV_PGRAPH_SURFACE), ==,
                     (6U << 28) | (5U << 24) | (1U << 20) | 0x1234);
    g_free(d);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qemu_mutex_init(&test_bql);
    g_test_add_func("/nv2a/pgraph/control/intr-read", intr_read);
    g_test_add_func("/nv2a/pgraph/control/mask-read", mask_read);
    g_test_add_func("/nv2a/pgraph/control/fifo-read", fifo_read);
    g_test_add_func("/nv2a/pgraph/control/intr-ack", intr_ack);
    g_test_add_func("/nv2a/pgraph/control/mask-write", mask_write);
    g_test_add_func("/nv2a/pgraph/control/mask-write-pfifo-busy",
                    mask_write_while_pfifo_busy);
    g_test_add_func("/nv2a/pgraph/control/fifo-write", fifo_write);
    g_test_add_func("/nv2a/pgraph/control/increment-read", increment_read);
    g_test_add_func("/nv2a/pgraph/control/increment-write", increment_write);
    g_test_add_func("/nv2a/pgraph/control/surface-read", surface_read);
    g_test_add_func("/nv2a/pgraph/control/surface-write", surface_write);
    g_test_add_func("/nv2a/pgraph/control/ack-selected-bits",
                    acknowledge_only_requested_bits);
    g_test_add_func("/nv2a/pgraph/control/ack-before-publication",
                    acknowledge_before_publication);
    g_test_add_func("/nv2a/pgraph/surface/concurrent-counters",
                    concurrent_counters);
    g_test_add_func("/nv2a/pgraph/surface/concurrent-full-writes",
                    concurrent_full_writes);
    g_test_add_func("/nv2a/pgraph/surface/masked-set", flip_masked_set);
    return g_test_run();
}
