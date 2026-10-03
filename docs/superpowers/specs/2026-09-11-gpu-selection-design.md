# Selectable Host GPU Design

**Issue:** https://github.com/Mainkill1/xemu/issues/72

## Goal

Let users and automated test runners select an exact Vulkan physical device on multi-GPU systems, verify which device and presentation path were actually initialized, and change the saved choice through the Display settings UI with a process restart.

## Scope and sequence

The issue's design is implemented as three independently reviewable PRs:

1. **Selection and strict startup:** copied Vulkan inventory, UUID-based resolution, configuration migration, process-only command-line overrides, machine-readable requested-versus-actual status, and strict rejection of device/backend substitution.
2. **Presentation compatibility:** correlate the selected Vulkan device with the active OpenGL presenter. Preserve qualified shared memory and add a host-staged copy of the finished display-composition image only when the real dual-GPU rig requires it.
3. **Display UI and test integration:** adapter dropdown below Backend, active/pending status, restart notice, OpenGL routing limitation, and automated identity admission.

Each PR branches from accepted `main` or the accepted preceding PR. Each remains draft until its own correctness and performance gates are published. No performance improvement is assumed.

## Identity and selection contract

Inventory records copy device name, vendor/device IDs, device type, API and raw driver versions, device UUID, driver UUID, optional valid LUID/node mask, renderer compatibility, and a rejection reason. Live `VkPhysicalDevice` handles remain owned by the Vulkan instance and never escape it.

The resolver is pure and follows these rules:

- A UUID request must match exactly one record before compatibility filtering. Missing, duplicate, malformed, or unsupported matches fail explicitly.
- A legacy name is accepted for configuration compatibility. An ambiguous name fails in strict mode and is reported in interactive mode.
- Automatic chooses the first compatible hardware device in enumeration order. Software devices are reported but cannot silently satisfy a strict hardware request.
- Runtime observation never overwrites the saved preference. A command-line request remains process-local.

`display.vulkan.preferred_physical_device` remains for compatibility. A new optional `device_uuid` is the stable preference token. Selecting Automatic clears both values; selecting a discovered adapter writes its UUID and descriptive name.

## Startup and reporting contract

PR A provides these process arguments:

```text
-list-gpus
-gpu uuid:<32 lowercase hex digits> | auto
-gpu-strict
-gpu-info <path>
```

`-list-gpus` initializes only enough Vulkan state to enumerate devices. It exits before settings-save registration, NVIDIA profile mutation, SDL video/window creation, VM initialization, and guest-storage access. `-gpu` is removed from the QEMU argument vector and never persisted.

The status document is versioned JSON and written through a temporary sibling followed by atomic replacement. It identifies the request source, requested and actual backend/device, raw identity fields, fallback status, and either `initialized` or `failed`. A strict request fails nonzero and without a modal recovery dialog if the configured backend, exact device, or later explicit presentation mode is not honored.

## Vulkan discovery

Both `vkEnumeratePhysicalDevices()` calls are checked. `VK_INCOMPLETE` and changing counts use a small bounded retry. Properties use `VkPhysicalDeviceProperties2` chained to `VkPhysicalDeviceIDProperties`; optional driver metadata is queried only when supported. Capability evaluation covers Vulkan 1.1, a combined graphics/compute queue, required extensions, and the required physical-device features used by logical-device creation.

Startup-only listing owns a temporary instance and finalizes it before returning. Renderer initialization re-enumerates on its own live instance. Discovery never runs per frame and never carries handles between instances.

## Presentation contract

The current Vulkan renderer exports its composed display image and imports it into an OpenGL context. A different selected Vulkan GPU can therefore be incompatible with the presenter. PR A reports presentation compatibility as unknown for inventory-only mode and prevents claims of successful cross-device presentation.

PR B compares Vulkan and OpenGL device UUID sets while the correct GL context is current. Shared mode is allowed only for a qualified pair and handle/format capabilities. If the test rig needs cross-device output, copy mode transfers the finished scaled/composited RGBA image to bounded host-visible staging and uploads it through the presentation GL context. It preserves PVIDEO, orientation, crop, channel order, alpha, screenshots, resize and lifecycle behavior. Shared and copy results are measured separately.

## UI contract

The interactive route is **Machine → Settings → Display → Renderer**. The adapter control appears directly below the existing Backend control in that view. Vulkan shows Automatic and compatible discovered devices. OpenGL shows OS/driver controlled plus the actual renderer because this selector does not enforce OpenGL routing. A changed selection is pending until process restart; renderer switching or guest reset cannot apply it to the live device.

## Verification

Pure tests cover reordered devices, duplicate names and UUIDs, malformed/missing/stale UUIDs, incompatible exact matches, automatic choice, software devices, legacy migration, command-line precedence, and unchanged persisted settings. Enumeration tests cover zero devices, failures, count changes and `VK_INCOMPLETE`. Startup tests cover list-only side effects, atomic status output, strict device/backend failure, and renderer fallback blocking.

Hardware qualification records actual AMD and NVIDIA identities, GL identity, transport, driver and build identities, correctness, guest progression, frame tails, resources, and copied bytes/time where relevant. Same-GPU baseline/candidate regression testing is separate from AMD-versus-NVIDIA comparison. Invalid identity or fallback makes a run invalid rather than a performance sample.

## Out of scope

No live GPU migration, multi-GPU workload splitting, vendor registry/profile abstraction, encoder integration, guest-clock changes, frame skipping, scheduler changes, shader-cache redesign, or native Vulkan presenter is included.
