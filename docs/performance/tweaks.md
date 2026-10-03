# Advance performance settings

**Machine → Settings → Advance** exposes independent performance controls.
Vulkan ubershader mode defaults to Prewarm; the shader-work shortcut and other
accepted controls default to On. Explicit saved choices remain after restart.

| Setting | On | Off | Apply |
| --- | --- | --- | --- |
| Reduce CPU usage while waiting | Interruptible Windows short waits | Existing short busy-wait path | Next wait |
| Process vertex packets in bulk | Batch eligible non-incrementing packets | Scalar packet processing | Next packet |
| Fast GPU fence polling | Atomic exact 32-bit fence read | Existing locked register read | Next read |
| Cache shaders | Reuse compiled shaders | Compile without disk reuse | Renderer-defined |
| Skip unchanged shader work | Reuse the current hybrid executable when shader state is unchanged | Repeat hybrid route checks | Next draw |
| Combine color downloads with rendering | Fold eligible downloads into the active submission | Submit the download separately | Next download |
| Upload only used vertex ranges | Skip unused leading remapped vertices | Copy from vertex zero | Next repack |
| Grow transient buffers to fit batches | Retain the pre-flush batch requirement while growing | Reuse drained storage; still grow for a large single draw | Restart xemu |
| Upload compressed textures directly | Native OpenGL S3TC upload where eligible | CPU S3TC decode | Restart xemu |

Vulkan ubershader mode is a restart-latched selection:

| Mode | Behavior |
| --- | --- |
| Off | Use specialized fragment pipelines only. |
| Fallback | Use an already-ready fragment-combiner fallback while specialization is prepared. Uncovered states can still wait. |
| Prewarm | Use persisted family history and cached shader artifacts to opportunistically prepare interpreter pipelines before demand. Missing artifacts are skipped; an uncovered draw can still wait. |
| Always | Diagnostic mode: force the fragment-combiner interpreter for supported draws without preparing specialized fragment shaders. It also prewarms learned interpreter families when persistent caching is enabled. Missing interpreter executables can still be constructed synchronously; GPU performance may be lower, and unsupported states or rejected interpreter resources use specialization. |

Mode changes require restarting xemu. Prewarm uses at most 32 families per
launch and only prepares artifacts already available in the shader cache; it
does not guarantee a prepared pipeline before first use. The menu reports both
requested and active modes so a saved restart-only change is not presented as
already active. Runtime status comes from the installed renderer, so an OpenGL
fallback or live backend switch cannot be mistaken for an active Vulkan
ubershader mode.

The tweak controls publish one atomic active-options snapshot. Renderer and
FIFO workers do not read the UI-owned configuration directly. Restart-only
choices stay pending until the next process so texture-cache and buffer-growth
policies cannot change halfway through their lifetime.

Cache shaders retains its existing `perf.cache_shaders` setting and renderer
behavior. Its menu choice is saved immediately.

The Vulkan ubershader is available only with the Vulkan renderer. It interprets
the fragment combiner while retaining specialized vertex, geometry, and
pipeline state. Its design history and diagnostic trace format are preserved
in the [project wiki](https://github.com/Mainkill1/xemu/wiki/PR-0071-feature-add-optional-Vulkan-ubershader);
the [PR #71 test evidence](https://github.com/Mainkill1/xemu-perf-tests/tree/tooling/pr71-native-qualification/docs/evidence/pr71-native-qualification-20260913)
records the exact build and measured workloads.

Turning a control Off selects its maintained fallback. It does not disable
size checks, dirty tracking, failure propagation, layout validation, or other
correctness behavior around the optimized operation.

The Windows wait control uses a per-thread high-resolution waitable timer for
short waits. When that facility is unavailable or fails, xemu reports the
failure once and retains the compatibility polling path. This integration also
retains `main`'s optimized QueryPerformanceCounter conversion.

## Current-main integration

PR #37 was originally stacked on a historical validation branch. Before
integration, that head was preserved by tag
`archive/pr-37-pre-main-integration-1636d1ef9f`. The active branch was then
rebuilt from the `main` containing PR #75 so it cannot merge the historical S
stack, validation-only marker receiver, raw evidence, or rejected branch
history into the product branch.

The reconciled change adds controls around the implementations already present
in `main`, plus the retained optional Windows short-wait implementation. It does
not replace the current texture layout, transient-buffer management, guarded
packet handling, Vulkan failure handling, or GPU-selector work.

## Validation scope

`test-xemu-tweaks-config` exercises generated defaults, saved Off values,
migration, independent choices, live controls, and restart-only publication.

`test-xemu-tweak-paths.py` compiles the current production packet dispatcher
and buffer reservation function with small API doubles. It covers 24 packet
modes and both buffer-growth policies, including mandatory capacity for a
single draw.

The Windows polling tests exercise enable/disable behavior, event readiness,
timer failures, cleanup, and compatibility fallback. These focused tests do not
replace final game and performance qualification.
