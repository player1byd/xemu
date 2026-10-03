# Host GPU selection

xemu can select a Vulkan physical device by its stable Vulkan device UUID.
This is useful on systems where automatic selection does not choose the GPU
needed for testing or normal use.

## Settings

Open **Machine → Settings → Display → Renderer**. Select **Vulkan** as the
Backend, then choose an **Adapter** directly below it. The saved adapter takes
effect after xemu restarts. The panel shows the adapter used by the current
process separately from the saved choice.

The saved setting is `display.vulkan.device_uuid`. Its value is either empty
for automatic selection or exactly 32 hexadecimal digits. The older
`preferred_physical_device` name remains readable for configuration migration,
but newly selected devices are stored by UUID.

OpenGL device selection remains controlled by the operating system and graphics
driver. The settings panel states this explicitly when OpenGL is selected.

## Command line

The following options are parsed before normal xemu initialization:

| Option | Behavior |
| --- | --- |
| `-list-gpus` | Print Vulkan adapters and exit before loading settings or starting the VM. |
| `-gpu auto` | Use automatic Vulkan adapter selection for this process. |
| `-gpu uuid:<id>` | Request one exact Vulkan device UUID for this process. |
| `-gpu-strict` | Fail instead of substituting another renderer or device. |
| `-gpu-info <path>` | Atomically write schema-versioned requested and actual GPU state. |

Command-line adapter choices are process-local and are not saved. `-gpu`
applies when the configured rendering backend is Vulkan.

Example:

```text
xemu -list-gpus -gpu-info gpu-inventory.json
xemu -gpu uuid:00112233445566778899aabbccddeeff -gpu-strict \
     -gpu-info gpu-run.json
```

## Selection and failure rules

- An exact UUID selects one compatible matching record or fails.
- Duplicate UUID or legacy-name matches are rejected as ambiguous.
- Automatic selection prefers a supported hardware device.
- Software Vulkan devices are excluded in strict automatic mode.
- Strict mode prevents fallback to another renderer after initialization fails.
- Unsupported devices remain visible in inventory with a rejection reason.

The compatibility record checks the Vulkan API version, a combined graphics
and compute queue, external memory and semaphore support, and every Vulkan
feature required by the renderer. Shader modules target the selected device's
Vulkan level: Vulkan 1.1 uses SPIR-V 1.3, Vulkan 1.2 uses SPIR-V 1.5, and
Vulkan 1.3 or newer uses SPIR-V 1.6.

## JSON report

Schema version 1 records:

- request source, selector, and strict policy;
- every discovered Vulkan device, UUIDs, raw IDs and versions, device type,
  compatibility, and rejection reason;
- the actual initialized backend and Vulkan device;
- fallback status or initialization error;
- presentation mode and OpenGL vendor/renderer after shared-image import.

Inventory-only reports leave actual and presentation fields unavailable.
Runtime reports first identify successful renderer initialization, then replace
the file after the first shared presentation image is imported successfully.
The writer flushes a same-directory temporary file and atomically replaces the
requested destination.

## Presentation compatibility

The Vulkan renderer exports its completed display image and imports it into the
OpenGL presentation context. A selected Vulkan GPU may differ from the GPU that
owns the OpenGL context on a hybrid system. The JSON presentation state changes
to `shared` only after that import and texture creation succeed. Systems that
cannot share the image across those devices still require a host-copy fallback;
the selector does not silently claim that path exists.
