# GPU Selection PR A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add deterministic Vulkan adapter inventory and selection, process-local strict command-line requests, and requested-versus-actual startup evidence without changing presentation transport.

**Architecture:** Keep selection policy in a pure C module over copied records. Vulkan discovery adapts live handles into those records, while startup argument parsing and result reporting remain separate UI-layer modules. The renderer consumes one immutable launch request and publishes the actual initialized identity; strict mode prevents backend or device fallback.

**Tech Stack:** C11, Vulkan 1.1/Volk, GLib, QEMU Error/QObject utilities, Meson, xemu generated configuration.

**Spec:** `docs/superpowers/specs/2026-09-11-gpu-selection-design.md`

## Global Constraints

- Base every change on `Mainkill1/xemu:main` at `e18ba8d6274cf227cc9e5ae1b5684f28ed911a99`.
- Keep `display.vulkan.preferred_physical_device` readable; add UUID identity without silently rewriting existing configuration.
- A command-line override is process-local and must never be saved.
- Never silently substitute an explicitly requested UUID, backend, or strict policy.
- Do not add vendor dependencies, registry changes, per-frame discovery, or a presentation-copy path in PR A.
- Inventory-only mode must not initialize SDL video, open guest storage, mutate driver profiles, register settings save, or boot the VM.
- UUIDs are exactly 16 bytes and are serialized as 32 lowercase hexadecimal digits.
- Every behavior change follows red-green-refactor and includes the failing-test output in the development record.

---

### Task 1: Pure adapter identity and resolver

**Files:**
- Create: `hw/xbox/nv2a/pgraph/vk/device-selection.h`
- Create: `hw/xbox/nv2a/pgraph/vk/device-selection.c`
- Create: `tests/unit/test-xbox-vk-device-selection.c`
- Modify: `tests/unit/meson.build`

**Interfaces:**
- Produces `PGRAPHVkDeviceRecord`, `PGRAPHVkSelectionRequest`, `PGRAPHVkSelectionResult`, `pgraph_vk_device_uuid_parse()`, `pgraph_vk_device_uuid_format()`, and `pgraph_vk_resolve_device()`.
- Resolver consumes copied records only and returns a record index plus a typed outcome; it performs no Vulkan calls and does not access `g_config`.

- [x] Write literal table tests for UUID formatting/parsing, including mixed-case input normalization and malformed length/characters.
- [x] Compile the focused test and verify it fails because the selection interface is absent. The full Meson target remains part of Task 6's pinned build verification.
- [x] Add the minimal UUID helpers and rerun until those tests pass.
- [x] Add failing resolver tests for automatic hardware selection, exact UUID, missing UUID, duplicate UUID, unsupported exact match, duplicate legacy name, reordered records, and software-only inventory.
- [x] Implement the minimal pure resolver and typed error strings; rerun the focused target.
- [x] Mutation-check wrong index, name-only UUID resolution, fallback after an unsupported exact match, and software selection under a hardware requirement.
- [x] Commit the resolver and tests.

### Task 2: Checked Vulkan inventory and capability evaluation

**Files:**
- Create: `hw/xbox/nv2a/pgraph/vk/device-inventory.h`
- Create: `hw/xbox/nv2a/pgraph/vk/device-inventory.c`
- Modify: `hw/xbox/nv2a/pgraph/vk/instance.c`
- Modify: `hw/xbox/nv2a/pgraph/vk/meson.build`
- Create: `tests/unit/test-xbox-vk-device-inventory.c`
- Modify: `tests/unit/meson.build`

**Interfaces:**
- Produces `pgraph_vk_enumerate_device_handles()` with injectable enumeration callbacks for unit tests, `pgraph_vk_collect_device_inventory()`, and `pgraph_vk_device_record_check_renderer_support()`.
- Returns owned arrays of copied records paired with live handles only for the caller's current instance.

- [x] Write failing enumeration tests for zero devices, first-call failure, second-call failure, count growth, count shrink, repeated `VK_INCOMPLETE`, and bounded retry exhaustion.
- [x] Implement bounded two-call enumeration and verify focused tests pass.
- [x] Write failing capability tests proving required features, combined graphics/compute queue, Vulkan 1.1, and required external-memory/semaphore extensions affect the same compatibility record used by selection.
- [x] Implement properties2/ID-property copying and unified capability evaluation; preserve raw driver version.
- [x] Replace `select_physical_device()` enumeration with inventory plus pure resolution while retaining renderer-owned handles.
- [x] Remove runtime `preferred_physical_device` write-back; configuration-preservation integration coverage is completed in Task 5.
- [x] Commit checked inventory integration.

### Task 3: Launch request and configuration migration

**Files:**
- Modify: `config_spec.yml`
- Create: `ui/xemu-gpu-launch.h`
- Create: `ui/xemu-gpu-launch.c`
- Modify: `ui/meson.build`
- Create: `tests/unit/test-xemu-gpu-launch.c`
- Modify: `tests/unit/meson.build`

**Interfaces:**
- Produces immutable `XemuGpuLaunchRequest` and `xemu_gpu_launch_parse_early()`.
- Consumes saved `device_uuid` and legacy preferred name only after settings load; CLI fields take precedence without mutating `g_config`.
- Removes recognized arguments from the argv array using the existing NULL convention.

- [x] Write failing tests for `-gpu auto`, exact UUID, malformed UUID, missing argument, repeated conflicting options, `-gpu-strict`, `-list-gpus`, `-gpu-info`, and CLI precedence.
- [x] Add `device_uuid` to the generated configuration schema and implement minimal parser/request ownership.
- [x] Write tests around the const saved-value boundary showing CLI precedence without mutating saved UUID/name inputs. End-to-end file preservation remains in Task 5.
- [x] Implement saved/CLI resolution and cleanup, then rerun focused tests.
- [x] Commit launch request and configuration migration.

### Task 4: Inventory-only command and atomic JSON output

**Files:**
- Create: `ui/xemu-gpu-info.h`
- Create: `ui/xemu-gpu-info.c`
- Modify: `ui/xemu.c`
- Modify: `ui/meson.build`
- Create: `tests/unit/test-xemu-gpu-info.c`
- Modify: `tests/unit/meson.build`

**Interfaces:**
- Produces `xemu_gpu_list_devices()` and `xemu_gpu_info_write_atomic()`.
- The inventory entry point owns a temporary Volk instance and returns before normal startup side effects.
- JSON schema version 1 records request, actual state or failure, identities, fallback, and presentation identity as unknown when no GL context exists.

- [x] Write failing JSON tests using a temporary directory: initialized record, failed record, null unavailable fields, escaped device names, and atomic replacement with no temporary sibling left behind.
- [x] Implement schema-owned JSON construction and same-directory temporary write/flush/close/rename; propagate all file failures.
- [x] Verify with a native inventory-only launch that list mode exits before settings-save registration, profile setup, SDL video, VM start, and guest-file access.
- [x] Parse early flags before normal initialization and implement the temporary inventory path.
- [x] Verify `-list-gpus` prints deterministic records and optional `-gpu-info` emits schema-valid JSON without a config file.
- [x] Commit inventory-only startup and reporting.

### Task 5: Exact renderer selection and strict fallback behavior

**Files:**
- Modify: `hw/xbox/nv2a/pgraph/vk/instance.c`
- Modify: `hw/xbox/nv2a/pgraph/vk/renderer.h`
- Modify: `hw/xbox/nv2a/pgraph/pgraph.c`
- Modify: `ui/xemu-gpu-launch.h`
- Modify: `ui/xemu-gpu-info.c`
- Create: `tests/unit/test-xbox-renderer-selection.c`
- Modify: `tests/unit/meson.build`

**Interfaces:**
- Renderer obtains the immutable request through `xemu_gpu_launch_request_get()` and publishes success/failure through `xemu_gpu_launch_record_*()`.
- `xemu_gpu_strict_mode()` gates both unavailable-renderer substitution in `nv2a_context_init()` and initialization fallback in `init_renderer()`.

- [ ] Write failing tests for exact-device success, missing/unsupported/ambiguous UUID failure, legacy ambiguity, unavailable requested renderer, logical-device failure, and non-strict legacy fallback.
- [x] Integrate request resolution into renderer initialization and publish actual UUID/name/type/vendor/device/API/raw-driver fields only after logical-device and allocator success.
- [x] Block both fallback locations in strict mode and ensure failure exits nonzero without a modal recovery prompt.
- [x] Verify the saved UUID/name remain byte-identical after a strict process-local override and after a saved-UUID launch.
- [x] Commit strict renderer integration.

### Task 6: Documentation, host inventory, and PR A verification

**Files:**
- Modify: `README.md` or the existing command-line/settings documentation selected during implementation.
- Create: `docs/performance/gpu-selection.md`
- Modify: `docs/superpowers/plans/2026-09-11-gpu-selection-pr-a.md` to mark completed steps.

**Interfaces:**
- Documents exact CLI/config behavior, restart semantics, OpenGL limitation, JSON schema, and PR B presentation gate.

- [x] Run all new Windows unit targets and record exact pass counts.
- [ ] Run the repository's generated-config validation and build the affected Linux target.
- [ ] Run `scripts/checkpatch.pl` on the final code diff and `git diff --check`.
- [x] Run `-list-gpus` on the dual-GPU Windows rig and retain sanitized inventory showing both compatible devices.
- [x] Attempt strict initialization for each GPU and record shared-presentation success only after the OpenGL import succeeds.
- [ ] Run the Mainkill1 privacy scan, commit final documentation/evidence links, push only to the `mainkill1` remote, and open a draft PR linked to issue #72.

## PR B and PR C handoff

PR B starts only after PR A's real inventory identifies the Vulkan and GL topology. It implements UUID correlation and either qualifies shared memory for both requested devices or adds the bounded post-composite host-staged path described in the spec. PR C then consumes the stable inventory/request/result APIs for the restart-required **Machine → Settings → Display → Renderer** UI and benchmark admission. Neither later PR is folded into PR A merely to make the dropdown look complete.
