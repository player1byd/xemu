/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/xemu-input.h"

bool xemu_input_create_virtual_devices(XemuVirtualControllerPort *port,
                                       const XemuInputTopologyOps *ops,
                                       void *opaque, Error **errp)
{
    Error *local_err = NULL;
    void *hub;
    void *gamepad;

    assert(!port->connected);
    assert(!port->hub);
    assert(!port->gamepad);

    hub = ops->create_hub(opaque, &local_err);
    if (!hub) {
        if (!local_err) {
            error_setg(&local_err, "Xbox controller hub creation failed");
        }
        error_propagate(errp, local_err);
        return false;
    }

    gamepad = ops->create_gamepad(opaque, &local_err);
    if (!gamepad) {
        if (!local_err) {
            error_setg(&local_err, "Xbox controller creation failed");
        }
        Error *cleanup_err = NULL;
        if (!ops->remove_hub(opaque, hub, &cleanup_err)) {
            const char *create_message = local_err ? error_get_pretty(local_err)
                                                   : "unknown error";
            const char *cleanup_message = cleanup_err
                                              ? error_get_pretty(cleanup_err)
                                              : "unknown error";
            Error *combined_err = NULL;
            error_setg(&combined_err, "%s; hub rollback failed: %s",
                       create_message, cleanup_message);
            error_free(local_err);
            error_free(cleanup_err);
            local_err = combined_err;
            port->hub = hub;
        }
        ops->release_device(hub);
        error_propagate(errp, local_err);
        return false;
    }

    port->hub = hub;
    port->gamepad = gamepad;
    port->connected = true;
    ops->release_device(hub);
    ops->release_device(gamepad);
    return true;
}

bool xemu_input_create_xmu_device(const XemuInputXmuOps *ops, void *opaque,
                                  void **device, Error **errp)
{
    Error *local_err = NULL;
    *device = NULL;
    void *drive = ops->create_drive(opaque, &local_err);
    if (!drive) {
        if (!local_err) {
            error_setg(&local_err, "XMU drive creation failed");
        }
        error_propagate(errp, local_err);
        return false;
    }

    *device = ops->create_device(opaque, drive, &local_err);
    if (!*device) {
        if (!local_err) {
            error_setg(&local_err, "XMU device creation failed");
        }
        ops->remove_drive(opaque, drive);
        error_propagate(errp, local_err);
        return false;
    }
    return true;
}
