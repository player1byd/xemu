# PR #267 rework: validation record

Date: September 29, 2026. Parent under review:
`60f59e0116b988125b8b74a03a9dd5da6409258f`.
This record supersedes the initial 25-case local validation as the current
handoff record; historical results remain available in the parent commit.

## Executed environment and limits

Linux x86-64, GCC 14.2.0 and Clang 17.0.0. Production PTIMER, queue core and
PRAMDAC translation units are compiled with the checked-in unit shim/stubs.
The upstream run uses its checked-in mock. The generic countdown suite uses
production `hw/core/ptimer.c` and the shared timer stubs changed by this PR.

**A local compatibility shell supplies missing GLib/QEMU environment types and
test registration. This is not a full Meson/product build or a real GLib,
QEMU timer-list mutex/BQL, PCI IRQ, VMState stream or Xbox execution test.**
Unlike the initial harness, these runs use the repository's actual
`qemu/host-utils.h` and native-128 implementation, and separately its actual
`util/host-utils.c` and `util/int128.c` software-wide fallback. No replacement
PTIMER arithmetic is used. Guest NVPLL handler code is exercised; guest MMIO
address routing is not.

## Results

| Check | Observed result |
|---|---|
| Parent checked-in shim | Reproduced missing armed field / stale prototype compilation errors |
| Parent plus shim correction, before production scheduler changes | Original 25/25 fork cases pass with QEMU math helpers |
| Adapted prior clock/observation matrix, before scheduler changes | 45/45 pass; locks in upstream immediate-publication behavior |
| Reworked fork suite, GCC native-128 | 104/104 pass |
| Reworked fork suite, GCC software-wide QEMU fallback | 104/104 pass |
| Reworked fork suite, Clang native-128 | 104/104 pass |
| Reworked fork suite, GCC ASan + UBSan | 104/104 pass, no sanitizer diagnostic; leak checking not disabled |
| Independent forward-deadline oracle within each fork run | 55,296 phase/ratio/frequency/distance cases pass |
| Retained upstream suite, native-128 / software-wide | 20/20 pass in each mode |
| Shared-stub generic timer suite, native-128 / software-wide | 576/576 pass in each mode |
| Seven deliberate negative controls | Each fails the intended assertion; restored production code passes again |

Local compilation uses `-std=gnu11 -O2 -g -Wall -Wextra -Werror
-Wno-unused-parameter -Wno-sign-compare`, section garbage collection, and for
the sanitizer run `-fsanitize=address,undefined -fno-omit-frame-pointer`.
The sign-comparison exemption permits unchanged generic QEMU math sources.
The compatibility shell and raw logs are review artifacts, not production
files or a substitute for exact-head CI.

## Reproduced defects and performance controls

Before the scheduler changes, 1,000 unchanged enabled numerator writes raised
`timer_mod` calls from 1 to 1,001. Repeated masked numerator writes raised
`timer_del` calls from 4 to 1,004 in that setup. The new matrix covers eight
operations in enabled, masked and stopped states: 24 scenarios, each making
1,000 writes with no additional queue API requests after setup.

Other regressions first failed on the preceding implementation:

- A callback dispatched at `INT64_MAX` queued the same due deadline again.
- Replacing one timer in the test stub discarded a neighboring timer.

New controls also cover no-op writes after actual queue removal, callback
consumption, changed/restored deadlines, actual PRAMDAC coefficient handling,
wide GPU-source wrap and a due wrap check that must not take the reuse shortcut.
A crossing rate write now publishes one final IRQ update rather than two.

These are exact fixture API counts and state assertions, **not measured
end-to-end CPU, notification, OS wake, latency, power or FPS savings**.

## Negative controls

Each mutation was applied independently and removed before final validation:

| Mutation | Detecting case |
|---|---|
| Remove same-deadline KEEP | Enabled TIME-low no-op operation count |
| Cancel an already absent timer | Masked numerator no-op operation count |
| Remove pre-ACK reconciliation | Masked expiry acknowledged before unmask |
| Remove signed-horizon stop | Callback at `INT64_MAX` must not requeue now |
| Recover v4 arming from queued timer only | Post-#120 masked v4 alarm survives restore |
| Reuse a queued event whose expiry is already due | Due GPU-counter-wrap checkpoint is rescheduled |
| Publish IRQ both inside scheduler and outer operation | Clock-change contribution is published once |

## Preserved upstream expectation reconciliation

The initial PR corrected four one-nanosecond upstream expectations. At virtual
time 1,000,000,100 ns and source 233,333,333 Hz, three ratio-1/1 cases expect
575,218,740 ns remaining, and the denominator-2 case expects 75,218,738 ns.
The preceding fractional clock period has already elapsed.

| Denominator | Absolute deadline ns | Internal ticks one ns earlier | Ticks at deadline |
|---|---:|---:|---:|
| 1 | 1,575,218,840 | 367,551,061 | 367,551,062 |
| 2 | 1,075,218,838 | 501,768,788 | 501,768,790 |

No upstream test is removed. Clock-change characterization imported from
#59/#81 is explicitly adapted to #120's synchronous overdue publication for
enabled alarms, rather than restoring the old due-now-host-callback policy.
Masked state remains lazy and W1C ordering is preserved.

## Outstanding product gates

The local checks do not establish that Def Jam is fixed or that #81's
historical performance hold is resolved. Still required: exact-head platform
CI and native suites, real timer-list/IRQ integration, old/new VMState streams
and repeated/failed restores, normal-clock Def Jam cold starts and sustained
matches on both renderers, current XISO correctness, and matched PGR2/Morrowind
pacing/resource checks. See [README.md](README.md) for commands, lineage and
acceptance criteria. Keep #267 draft, #266 open and the fixed baseline intact.
