# NV2A PTIMER reconciliation and handoff

## Purpose and lineage

[PR #267](https://github.com/Mainkill1/xemu/pull/267) is the current integration
for [issue #266](https://github.com/Mainkill1/xemu/issues/266). Preserve the
owner's earlier working timer behavior while reconciling subsequent upstream
changes. Keep the issue open until normal-clock Def Jam validation is complete.

| Reference | Reused work / integration constraint |
|---|---|
| [#39](https://github.com/Mainkill1/xemu/issues/39), [#40](https://github.com/Mainkill1/xemu/issues/40) | Original future-deadline and masked-comparator correctness requirements. |
| [PR #59](https://github.com/Mainkill1/xemu/pull/59), head `0043629b0bc13d2b34a1bf3c1008171ad8eecb8f` | Explicit guest arming, pre-ACK/reprogramming reconciliation, exact two-stage deadlines, PLL and snapshot behavior; clock-transition tests. |
| [RFC #73](https://github.com/Mainkill1/xemu/issues/73) | Separate comparator semantics from queue effects; avoid no-op mutations; qualify clock-model and host-wait experiments separately. This historical RFC is not closed by #267. |
| [PR #81](https://github.com/Mainkill1/xemu/pull/81), head `98f1a7a49cb7a6ccb8feba20438dc86f9ead58d5` | Central KEEP/CANCEL/ARM decision, reusable future schedule, operation-count and lifecycle tests. Adapted without its persistent host-state cache. Its historical performance hold is not a pass for this integration. |
| [Upstream #3035](https://github.com/xemu-project/xemu/pull/3035), [fork #120](https://github.com/Mainkill1/xemu/pull/120) | Retain masked-host-callback suppression, upward rounding and synchronous overdue publication. Do not revert unrelated upstream changes. |
| Initial #267 head `60f59e0116b988125b8b74a03a9dd5da6409258f` | Retain phase-aware arithmetic and post-#120 masked v4 recovery; repair its stale unit shim and incomplete queue optimization. |

The integration base remains `2cbabc7152866dd19fb2c6279c776019f3f5df9a`.
Windows waiting (#98/#25), QPC conversion (#99), global QEMU clocks, renderer
work and the fixed performance baseline are outside this patch.

## Implemented design

```text
Guest operation / callback / restore
    -> reconcile elapsed comparator state under the old mapping
    -> apply the requested semantic change
    -> retain a future schedule only when the mapping and target are unchanged
    -> otherwise calculate the exact desired virtual deadline
    -> observe the actual QEMUTimer
    -> KEEP / CANCEL / ARM
    -> publish the final IRQ contribution once
```

`ptimer_core.[ch]` carries PR #81's small deterministic queue decision.
`PtimerHostSchedule` is now a fresh observation, not persistent device state.
Only `ptimer_apply_deadline()` changes the queue. Equal deadlines are kept;
absent timers are not repeatedly canceled. Callback consumption, early wakes
and timer replacement during restore are observed directly, so no cached
queued flag, dirty bit or generation needs migration or invalidation.

A caller may use `ptimer_schedule_reusable()` only after checking that the
semantic mapping/target did not change and reconciling elapsed state. The
queued expiry must be strictly later than the sampled virtual time. Unchanged
mask, ratio, alarm and source-clock writes can then skip phase sampling and
inverse arithmetic. TIME writes always rebuild their mapping; callbacks and
post-load always re-evaluate. A due source-counter-wrap check is not reusable.
Ordinary ACK retains the existing no-requeue path.

The exact two-stage forward/inverse clock mapping, full guest-counter wrap,
source GPU-counter wrap, zero-ratio behavior and multi-epoch catch-up remain.
At `INT64_MAX` virtual nanoseconds, no later signed deadline exists: reconcile
status and leave the comparator armed but unqueued. Requeuing the same maximum
inside its own dispatch would spin. Reset/restore can reconstruct it normally.

Pending status does not suppress otherwise-required recurring callbacks.
Reconciliation still precedes W1C, replacement, mask/rate/TIME and NVPLL writes.
No forced interrupt, title patch, anchored clock or `-icount` workaround is added.

## Build and test integration

- `hw/xbox/nv2a/meson.build` includes the queue core once.
- The maintained unit target links production `ptimer.c`, `ptimer_core.c` and
  `pramdac.c`. Its checked-in shim now contains the armed field, current
  prototypes and PRAMDAC fields required by those translation units.
- The upstream target links the queue core and models dequeue-before-callback;
  its mock reports each timer's actual expiry instead of a global active flag.
- Shared unit timer stubs unlink/reinsert without discarding neighboring events.
  Public API counters do not count the stub's private unlink as a cancellation.
- All original 25 fork and 20 upstream cases remain. The fork suite now has
  104 registered cases, including the 55,296-case independent deadline oracle,
  24 no-op operation scenarios and 45 adapted clock/observation scenarios.

The old CI failure was a **compile failure**, not a runtime failed assertion:
`xbox-nv2a-ptimer-test-shim.h` lacked `alarm_armed`, the version argument of
`ptimer_post_load()` and the clock-change declaration. Do not use the earlier
local shim-only pass as evidence that the checked-in Meson target built.

## Snapshot contract

NV2A v4 alarm/time/timer fields keep their order; v5 adds guest `alarm_armed`.
Pre-v4 absent state is cleared before loading. Preserve both legacy v4 forms:
queued alarms and post-#120 masked nonzero alarms without a host callback.
Recovery uses `timer_pending() || alarm_time != 0`. An ambiguous zero-target,
no-timer v4 stream cannot be recovered exactly. New v5 streams preserve zero
arming directly and are not readable by v4-only builds.

No queue cache is serialized. Unit helper tests cover v3/v4/v5 and timer
replacement/removal, but they do not replace real VMState stream validation.

## Qualification handoff

[RESULTS.md](RESULTS.md) separates executed source-path checks from remaining
product checks. From a normal dependency-complete checkout, use the existing
build workflow. A focused Linux invocation after configuration is:

```sh
./configure --target-list=i386-softmmu --disable-werror
ninja -C build tests/unit/test-xbox-nv2a-ptimer \
  tests/xbox/ptimer/test-nv2a-ptimer tests/unit/ptimer-test
./build/tests/unit/test-xbox-nv2a-ptimer
./build/tests/xbox/ptimer/test-nv2a-ptimer
./build/tests/unit/ptimer-test
make -j2 check-unit
```

Use the established pinned Windows Release build for product qualification;
do not substitute this local harness or an old executable for the final head.

1. Obtain green exact-head supported-platform CI and execute the three timer
   suites natively. Exercise native-128 and QEMU software-wide arithmetic.
2. Validate real timer-list/IRQ integration, actual guest NVPLL MMIO, pause/
   resume, reset and old/new snapshot streams. Include repeated old/new loads,
   masked/overdue/stopped/zero-target states and failed-load cleanup.
3. Compare the previously working reference and this exact candidate using
   repeated normal-clock Def Jam cold starts and sustained match progression
   on OpenGL and Vulkan. Preserve source, executable hash, configuration,
   input, firmware/media and cache-state identities. A menu alone is not a pass.
4. Run current XISO correctness and matched PGR2/Morrowind fixed-work pacing
   and resource controls. Preserve #81's adverse OpenGL results as historical
   context, not as a current failure or success attributed without measurement.
5. If Def Jam still fails, capture the first missed callback with bounded
   correlated virtual/host timestamps, target/ACK/mask/pending transitions,
   guest callback entry/RDTSC/error return and BQL/scheduler/compilation waits.
   Disabled PTIMER enable after the stall is an endpoint, not initiating proof.

Record timer API requests, actual queue mutations, notifications, callbacks,
IRQ changes and OS wakes as distinct measurements. Keep traces/performance
artifacts with the emulator issue/PR, not evidence-only test-suite PRs.
No broad FPS benefit or completed title fix is claimed from unit tests.
