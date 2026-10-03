/*
 * xemu Input Management
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qemu/config-file.h"

#include "xemu-input.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"
#include <stdio.h>
#include <stdlib.h>

#include "system/blockdev.h"
#include "system/block-backend-global-state.h"

// #define DEBUG_INPUT

#ifdef DEBUG_INPUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US  2500
#define XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US 2500

#if 0
static void xemu_input_print_controller_state(ControllerState *state)
{
    DPRINTF("     A = %d,      B = %d,     X = %d,     Y = %d\n"
           "  Left = %d,     Up = %d, Right = %d,  Down = %d\n"
           "  Back = %d,  Start = %d, White = %d, Black = %d\n"
           "Lstick = %d, Rstick = %d, Guide = %d\n"
           "\n"
           "LTrig   = %.3f, RTrig   = %.3f\n"
           "LStickX = %.3f, RStickX = %.3f\n"
           "LStickY = %.3f, RStickY = %.3f\n\n",
        !!(state->buttons & CONTROLLER_BUTTON_A),
        !!(state->buttons & CONTROLLER_BUTTON_B),
        !!(state->buttons & CONTROLLER_BUTTON_X),
        !!(state->buttons & CONTROLLER_BUTTON_Y),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_LEFT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_UP),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_RIGHT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_DOWN),
        !!(state->buttons & CONTROLLER_BUTTON_BACK),
        !!(state->buttons & CONTROLLER_BUTTON_START),
        !!(state->buttons & CONTROLLER_BUTTON_WHITE),
        !!(state->buttons & CONTROLLER_BUTTON_BLACK),
        !!(state->buttons & CONTROLLER_BUTTON_LSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_RSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_GUIDE),
        state->axis[CONTROLLER_AXIS_LTRIG],
        state->axis[CONTROLLER_AXIS_RTRIG],
        state->axis[CONTROLLER_AXIS_LSTICK_X],
        state->axis[CONTROLLER_AXIS_RSTICK_X],
        state->axis[CONTROLLER_AXIS_LSTICK_Y],
        state->axis[CONTROLLER_AXIS_RSTICK_Y]
        );
}
#endif

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);
XemuVirtualControllerPort virtual_controllers[4];
const char *bound_drivers[4] = { DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE,
                                 DRIVER_DUKE };
int test_mode;

static const char **port_index_to_settings_key_map[] = {
    &g_config.input.bindings.port1,
    &g_config.input.bindings.port2,
    &g_config.input.bindings.port3,
    &g_config.input.bindings.port4,
};

static const char **port_index_to_driver_settings_key_map[] = {
    &g_config.input.bindings.port1_driver,
    &g_config.input.bindings.port2_driver,
    &g_config.input.bindings.port3_driver, 
    &g_config.input.bindings.port4_driver
};

static int *port_index_to_virtual_settings_key_map[] = {
    &g_config.input.virtual_ports.port1_connected,
    &g_config.input.virtual_ports.port2_connected,
    &g_config.input.virtual_ports.port3_connected,
    &g_config.input.virtual_ports.port4_connected,
};

static int *peripheral_types_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_type_0,
      &g_config.input.peripherals.port1.peripheral_type_1 },
    { &g_config.input.peripherals.port2.peripheral_type_0,
      &g_config.input.peripherals.port2.peripheral_type_1 },
    { &g_config.input.peripherals.port3.peripheral_type_0,
      &g_config.input.peripherals.port3.peripheral_type_1 },
    { &g_config.input.peripherals.port4.peripheral_type_0,
      &g_config.input.peripherals.port4.peripheral_type_1 }
};

static const char **peripheral_params_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_param_0,
      &g_config.input.peripherals.port1.peripheral_param_1 },
    { &g_config.input.peripherals.port2.peripheral_param_0,
      &g_config.input.peripherals.port2.peripheral_param_1 },
    { &g_config.input.peripherals.port3.peripheral_param_0,
      &g_config.input.peripherals.port3.peripheral_param_1 },
    { &g_config.input.peripherals.port4.peripheral_param_0,
      &g_config.input.peripherals.port4.peripheral_param_1 }
};

static void xemu_input_report_error(const char *action, Error *err)
{
    const char *detail = err ? error_get_pretty(err) : "unknown error";
    char *message = g_strdup_printf("%s: %s", action, detail);
    error_report("%s", message);
    xemu_queue_error_message(message);
    g_free(message);
    if (err) {
        error_free(err);
    }
}

static DeviceState *xemu_input_add_device(QDict *dict, Error **errp)
{
    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("device"), dict,
                                           errp);
    if (!opts) {
        return NULL;
    }

    DeviceState *dev = qdev_device_add(opts, errp);
    if (!dev) {
        /* qdev_device_add() takes ownership only when it succeeds. */
        qemu_opts_del(opts);
    }
    return dev;
}

static void xemu_input_remove_drive(DriveInfo *dinfo)
{
    BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
    monitor_remove_blk(blk);
    blk_unref(blk);
}

typedef struct XemuInputXmuCreateContext {
    QDict *drive_dict;
    QDict *device_dict;
} XemuInputXmuCreateContext;

static void *xemu_input_create_xmu_drive(void *opaque, Error **errp)
{
    XemuInputXmuCreateContext *context = opaque;
    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("drive"),
                                           context->drive_dict, errp);
    if (!opts) {
        return NULL;
    }

    DriveInfo *dinfo = drive_new(opts, 0, errp);
    if (!dinfo) {
        /* drive_new() retains the options only when it succeeds. */
        qemu_opts_del(opts);
    }
    return dinfo;
}

static void *xemu_input_create_xmu_storage(void *opaque, void *drive,
                                           Error **errp)
{
    XemuInputXmuCreateContext *context = opaque;
    (void)drive;
    return xemu_input_add_device(context->device_dict, errp);
}

static void xemu_input_remove_xmu_drive(void *opaque, void *drive)
{
    (void)opaque;
    xemu_input_remove_drive(drive);
}

static const XemuInputXmuOps xemu_input_xmu_ops = {
    .create_drive = xemu_input_create_xmu_drive,
    .create_device = xemu_input_create_xmu_storage,
    .remove_drive = xemu_input_remove_xmu_drive,
};

int *g_keyboard_scancode_map[25] = {
    &g_config.input.keyboard_controller_scancode_map.a,
    &g_config.input.keyboard_controller_scancode_map.b,
    &g_config.input.keyboard_controller_scancode_map.x,
    &g_config.input.keyboard_controller_scancode_map.y,
    &g_config.input.keyboard_controller_scancode_map.back,
    &g_config.input.keyboard_controller_scancode_map.guide,
    &g_config.input.keyboard_controller_scancode_map.start,
    &g_config.input.keyboard_controller_scancode_map.lstick_btn,
    &g_config.input.keyboard_controller_scancode_map.rstick_btn,
    &g_config.input.keyboard_controller_scancode_map.white,
    &g_config.input.keyboard_controller_scancode_map.black,
    &g_config.input.keyboard_controller_scancode_map.dpad_up,
    &g_config.input.keyboard_controller_scancode_map.dpad_down,
    &g_config.input.keyboard_controller_scancode_map.dpad_left,
    &g_config.input.keyboard_controller_scancode_map.dpad_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_up,
    &g_config.input.keyboard_controller_scancode_map.lstick_left,
    &g_config.input.keyboard_controller_scancode_map.lstick_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_down,
    &g_config.input.keyboard_controller_scancode_map.ltrigger,
    &g_config.input.keyboard_controller_scancode_map.rstick_up,
    &g_config.input.keyboard_controller_scancode_map.rstick_left,
    &g_config.input.keyboard_controller_scancode_map.rstick_right,
    &g_config.input.keyboard_controller_scancode_map.rstick_down,
    &g_config.input.keyboard_controller_scancode_map.rtrigger,
};

static void check_and_reset_in_range(int *btn, int min, int max,
                                     const char *message)
{
    if (*btn < min || *btn >= max) {
        fprintf(stderr, "%s\n", message);
        *btn = min;
    }
}

static void xemu_input_bindings_set_in_range(ControllerState *con)
{
#define CHECK_RESET_BUTTON(btn)                                            \
    check_and_reset_in_range(&con->controller_map->controller_mapping.btn, \
                             SDL_GAMEPAD_BUTTON_INVALID,                   \
                             SDL_GAMEPAD_BUTTON_COUNT,                     \
                             "Invalid entry for button " #btn ", resetting")

    CHECK_RESET_BUTTON(a);
    CHECK_RESET_BUTTON(b);
    CHECK_RESET_BUTTON(x);
    CHECK_RESET_BUTTON(y);
    CHECK_RESET_BUTTON(dpad_left);
    CHECK_RESET_BUTTON(dpad_up);
    CHECK_RESET_BUTTON(dpad_right);
    CHECK_RESET_BUTTON(dpad_down);
    CHECK_RESET_BUTTON(back);
    CHECK_RESET_BUTTON(start);
    CHECK_RESET_BUTTON(lshoulder);
    CHECK_RESET_BUTTON(rshoulder);
    CHECK_RESET_BUTTON(lstick_btn);
    CHECK_RESET_BUTTON(rstick_btn);
    CHECK_RESET_BUTTON(guide);

#undef CHECK_RESET_BUTTON

#define CHECK_RESET_AXIS(axis)                                              \
    check_and_reset_in_range(&con->controller_map->controller_mapping.axis, \
                             SDL_GAMEPAD_AXIS_INVALID,                      \
                             SDL_GAMEPAD_AXIS_COUNT,                        \
                             "Invalid entry for button " #axis ", resetting")

    CHECK_RESET_AXIS(axis_trigger_left);
    CHECK_RESET_AXIS(axis_trigger_right);
    CHECK_RESET_AXIS(axis_left_x);
    CHECK_RESET_AXIS(axis_left_y);
    CHECK_RESET_AXIS(axis_right_x);
    CHECK_RESET_AXIS(axis_right_y);

#undef CHECK_RESET_AXIS
}

static void xemu_input_bindings_reload_map(ControllerState *con)
{
    assert(con->type == INPUT_DEVICE_SDL_GAMEPAD);

    char guid[35] = { 0 };
    SDL_GUIDToString(con->sdl_joystick_guid, guid, sizeof(guid));
    if (!xemu_settings_load_gamepad_mapping(guid, &con->controller_map)) {
        return;
    }

    // If this controller did not exist in the mapping array, the config will
    // have been reallocated. Any gamepad mapping pointers for other controllers
    // are now invalid, and need to be reloaded.
    ControllerState *iter, *next;
    bool is_new_mapping;
    QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
        if (iter == con || iter->type != INPUT_DEVICE_SDL_GAMEPAD) {
            continue;
        }

        memset(guid, 0, sizeof(guid));
        SDL_GUIDToString(iter->sdl_joystick_guid, guid, sizeof(guid));

        is_new_mapping =
            xemu_settings_load_gamepad_mapping(guid, &iter->controller_map);
        assert(!is_new_mapping &&
               "Existing controller GUIDs should exist in the config");

        xemu_input_bindings_set_in_range(iter);
    }
}

static const char *get_bound_driver(int port)
{
    assert(port >= 0 && port <= 3);
    const char *driver = *port_index_to_driver_settings_key_map[port];

    // If the driver in the config is NULL, empty, or unrecognized 
    // then default to DRIVER_DUKE
    if (driver == NULL)
        return DRIVER_DUKE;
    if (strlen(driver) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_DUKE) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_S) == 0)
        return DRIVER_S;

    return DRIVER_DUKE;
}

static const int port_map[4] = { 3, 4, 1, 2 };

void xemu_input_init(void)
{
    if (g_config.input.background_input_capture) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "Failed to initialize SDL gamepad subsystem: %s\n", SDL_GetError());
        exit(1);
    }

    if (g_config.input.gamecontrollerdb_path && strlen(g_config.input.gamecontrollerdb_path) > 0) {
        int count = SDL_AddGamepadMappingsFromFile(g_config.input.gamecontrollerdb_path);
        if (count > 0) {
            fprintf(stderr, "Loaded %d custom gamepad mapping(s) from '%s'\n",
                    count, g_config.input.gamecontrollerdb_path);
        } else if (count < 0) {
            fprintf(stderr, "Failed to load gamepad mappings from '%s': %s\n",
                    g_config.input.gamecontrollerdb_path, SDL_GetError());
        }
    }

    // Create the keyboard input (always first)
    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_SDL_KEYBOARD;
    new_con->name = "Keyboard";
    new_con->bound = -1;

    for (int i = 0; i < 25; i++) {
        static const char *format_str =
            "WARNING: Keyboard controller map scancode out of range "
            "(%d) : Disabled\n";
        char buf[128];
        snprintf(buf, sizeof(buf), format_str, i);
        check_and_reset_in_range(g_keyboard_scancode_map[i],
                                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_COUNT, buf);
    }

    bound_drivers[0] = get_bound_driver(0);
    bound_drivers[1] = get_bound_driver(1);
    bound_drivers[2] = get_bound_driver(2);
    bound_drivers[3] = get_bound_driver(3);

    /* Legacy GUID bindings imply guest presence, even without that provider. */
    for (int i = 0; i < 4; i++) {
        int *setting_ptr = port_index_to_virtual_settings_key_map[i];
        int setting = xemu_input_normalize_virtual_presence(*setting_ptr);
        const char *guid = *port_index_to_settings_key_map[i];
        if (setting != *setting_ptr) {
            warn_report("Invalid virtual controller presence %d for port %d; "
                        "using legacy binding migration",
                        *setting_ptr, i + 1);
            *setting_ptr = setting;
        }
        if (xemu_input_port_should_exist(setting, guid)) {
            xemu_input_virtual_connect(i, bound_drivers[i], 0);
        }
    }

    // Check to see if we should auto-bind the keyboard
    int port = xemu_input_get_controller_default_bind_port(new_con, 0);
    if (port >= 0) {
        xemu_input_set_provider(port, new_con, 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
        xemu_queue_notification(buf);
    }

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
}

int xemu_input_get_controller_default_bind_port(ControllerState *state, int start)
{
    char guid[35] = { 0 };
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        snprintf(guid, sizeof(guid), "keyboard");
    }

    for (int i = start; i < 4; i++) {
        if (*port_index_to_virtual_settings_key_map[i] != 0 &&
            strcmp(guid, *port_index_to_settings_key_map[i]) == 0) {
            return i;
        }
    }

    return -1;
}

void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter)
{
    int *peripheral_type_ptr =
        peripheral_types_settings_map[player_index][peripheral_index];
    const char **peripheral_param_ptr =
        peripheral_params_settings_map[player_index][peripheral_index];

    assert(peripheral_type_ptr);
    assert(peripheral_param_ptr);

    *peripheral_type_ptr = peripheral_type;
    xemu_settings_set_string(
        peripheral_param_ptr,
        peripheral_parameter == NULL ? "" : peripheral_parameter);
}

void xemu_input_process_sdl_events(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        DPRINTF("Controller Added: %d\n", event->gdevice.which);

        // Attempt to open the added controller
        SDL_Gamepad *sdl_con;
        sdl_con = SDL_OpenGamepad(event->gdevice.which);
        if (sdl_con == NULL) {
            DPRINTF("Could not open joystick %d as a Gamepad\n", event->gdevice.which);
            return;
        }

        // Success! Create a new node to track this controller and continue init
        ControllerState *new_con = malloc(sizeof(ControllerState));
        memset(new_con, 0, sizeof(ControllerState));
        new_con->type                 = INPUT_DEVICE_SDL_GAMEPAD;
        new_con->name                 = SDL_GetGamepadName(sdl_con);
        new_con->sdl_gamepad          = sdl_con;
        new_con->sdl_joystick         = SDL_GetGamepadJoystick(new_con->sdl_gamepad);
        new_con->sdl_joystick_id      = SDL_GetJoystickID(new_con->sdl_joystick);
        new_con->sdl_joystick_guid    = SDL_GetJoystickGUID(new_con->sdl_joystick);
        new_con->bound                = -1;

        char guid_buf[35] = { 0 };
        SDL_GUIDToString(new_con->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
        DPRINTF("Opened %s (%s)\n", new_con->name, guid_buf);

        QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
        xemu_input_bindings_reload_map(new_con);

        // Do not replace binding for a currently bound device. In the case that
        // the same GUID is specified multiple times, on different ports, allow
        // any available port to be bound.
        //
        // This can happen naturally with X360 wireless receiver, in which each
        // controller gets the same GUID (go figure). We cannot remember which
        // controller is which in this case, but we can try to tolerate this
        // situation by binding to any previously bound port with this GUID. The
        // upside in this case is that a person can use the same GUID on all
        // ports and just needs to bind to the receiver and never needs to hit
        // this dialog.


        // Attempt to re-bind to port previously bound to
        int port = 0;
        bool did_bind = false;
        while (!did_bind) {
            port = xemu_input_get_controller_default_bind_port(new_con, port);
            if (port < 0) {
                // No (additional) default mappings
                break;
            } else if (!xemu_input_get_bound(port)) {
                xemu_input_set_provider(port, new_con, 0);
                did_bind = true;
                break;
            } else {
                // Try again for another port
                port++;
            }
        }

        // Try to bind to any open port, and if so remember the binding
        if (!did_bind && g_config.input.auto_bind) {
            for (port = 0; port < 4; port++) {
                if (!xemu_input_get_bound(port) &&
                    *port_index_to_virtual_settings_key_map[port] != 0) {
                    xemu_input_set_provider(port, new_con, 1);
                    did_bind = true;
                    break;
                }
            }
        }

        if (did_bind) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
            xemu_queue_notification(buf);
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        DPRINTF("Controller Removed: %d\n", event->gdevice.which);
        int handled = 0;
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE(iter, &available_controllers, entry, next) {
            if (iter->type != INPUT_DEVICE_SDL_GAMEPAD) continue;

            if (iter->sdl_joystick_id == event->gdevice.which) {
                DPRINTF("Device removed: %s\n", iter->name);

                // Disconnect
                if (iter->bound >= 0) {
                    // Queue a notification to inform user controller disconnected
                    // FIXME: Probably replace with a callback registration thing,
                    // but this works well enough for now.
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Port %d disconnected", iter->bound+1);
                    xemu_queue_notification(buf);

                    // Unbind the controller, but don't save the unbinding in
                    // case the controller is reconnected
                    xemu_input_set_provider(iter->bound, NULL, 0);
                }

                // Unlink
                QTAILQ_REMOVE(&available_controllers, iter, entry);

                // Deallocate
                if (iter->sdl_gamepad) {
                    SDL_CloseGamepad(iter->sdl_gamepad);
                }

                free(iter);

                handled = 1;
                break;
            }
        }
        if (!handled) {
            DPRINTF("Could not find handle for joystick instance\n");
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMAPPED) {
        DPRINTF("Controller Remapped: %d\n", event->gdevice.which);
    } else if (event->type == SDL_EVENT_JOYSTICK_ADDED) {
        if (!SDL_IsGamepad(event->jdevice.which)) {
            const char *name = SDL_GetJoystickNameForID(event->jdevice.which);
            SDL_GUID guid = SDL_GetJoystickGUIDForID(event->jdevice.which);
            char guid_buf[35] = { 0 };
            SDL_GUIDToString(guid, guid_buf, sizeof(guid_buf));
            fprintf(
                stderr,
                "Input: Detected unmapped joystick '%s' (GUID: %s, ID: %u). "
                "This device is not recognized as a gamepad by SDL and "
                "requires an SDL gamepad mapping to work with xemu.\n",
                name ? name : "Unknown", guid_buf,
                (unsigned int)event->jdevice.which);
        }
    }
}

void xemu_input_update_controller(ControllerState *state)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_input_updated_ts) <
        XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US) {
        return;
    }

    if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_input_update_sdl_kbd_controller_state(state);
    } else if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        xemu_input_update_sdl_controller_state(state);
    }

    state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

void xemu_input_update_controllers(void)
{
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_controller(iter);
    }
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_rumble(iter);
    }
}

void xemu_input_update_sdl_kbd_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    const bool *kbd = SDL_GetKeyboardState(NULL);

#define KBD_STATE(btn) \
    (kbd[g_config.input.keyboard_controller_scancode_map.btn])

    state->buttons |= KBD_STATE(a) << 0;
    state->buttons |= KBD_STATE(b) << 1;
    state->buttons |= KBD_STATE(x) << 2;
    state->buttons |= KBD_STATE(y) << 3;
    state->buttons |= KBD_STATE(dpad_left) << 4;
    state->buttons |= KBD_STATE(dpad_up) << 5;
    state->buttons |= KBD_STATE(dpad_right) << 6;
    state->buttons |= KBD_STATE(dpad_down) << 7;
    state->buttons |= KBD_STATE(back) << 8;
    state->buttons |= KBD_STATE(start) << 9;
    state->buttons |= KBD_STATE(white) << 10;
    state->buttons |= KBD_STATE(black) << 11;
    state->buttons |= KBD_STATE(lstick_btn) << 12;
    state->buttons |= KBD_STATE(rstick_btn) << 13;
    state->buttons |= KBD_STATE(guide) << 14;

    if (KBD_STATE(lstick_up))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = 32767;
    if (KBD_STATE(lstick_left))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = -32768;
    if (KBD_STATE(lstick_right))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = 32767;
    if (KBD_STATE(lstick_down))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = -32768;
    if (KBD_STATE(ltrigger))
        state->axis[CONTROLLER_AXIS_LTRIG] = 32767;

    if (KBD_STATE(rstick_up))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = 32767;
    if (KBD_STATE(rstick_left))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = -32768;
    if (KBD_STATE(rstick_right))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = 32767;
    if (KBD_STATE(rstick_down))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = -32768;
    if (KBD_STATE(rtrigger))
        state->axis[CONTROLLER_AXIS_RTRIG] = 32767;

#undef KBD_STATE
}

void xemu_input_update_sdl_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

#define SDL_MASK_BUTTON(state, btn, idx)                  \
    (SDL_GetGamepadButton(                                \
         (state)->sdl_gamepad,                            \
         (state)->controller_map->controller_mapping.btn) \
     << idx)

    state->buttons |= SDL_MASK_BUTTON(state, a, 0);
    state->buttons |= SDL_MASK_BUTTON(state, b, 1);
    state->buttons |= SDL_MASK_BUTTON(state, x, 2);
    state->buttons |= SDL_MASK_BUTTON(state, y, 3);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_left, 4);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_up, 5);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_right, 6);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_down, 7);
    state->buttons |= SDL_MASK_BUTTON(state, back, 8);
    state->buttons |= SDL_MASK_BUTTON(state, start, 9);
    state->buttons |= SDL_MASK_BUTTON(state, lshoulder, 10);
    state->buttons |= SDL_MASK_BUTTON(state, rshoulder, 11);
    state->buttons |= SDL_MASK_BUTTON(state, lstick_btn, 12);
    state->buttons |= SDL_MASK_BUTTON(state, rstick_btn, 13);
    state->buttons |= SDL_MASK_BUTTON(state, guide, 14);

#undef SDL_MASK_BUTTON

#define SDL_GET_AXIS(state, axis)    \
    SDL_GetGamepadAxis(              \
        (state)->sdl_gamepad,        \
        (state)->controller_map->controller_mapping.axis)

    state->axis[0] = SDL_GET_AXIS(state, axis_trigger_left);
    state->axis[1] = SDL_GET_AXIS(state, axis_trigger_right);
    state->axis[2] = SDL_GET_AXIS(state, axis_left_x);
    state->axis[3] = SDL_GET_AXIS(state, axis_left_y);
    state->axis[4] = SDL_GET_AXIS(state, axis_right_x);
    state->axis[5] = SDL_GET_AXIS(state, axis_right_y);

#undef SDL_GET_AXIS

// FIXME: Check range
#define INVERT_AXIS(controller_axis) \
    state->axis[controller_axis] = -1 - state->axis[controller_axis]

    if (state->controller_map->controller_mapping.invert_axis_left_x) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_left_y) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_Y);
    }

    if (state->controller_map->controller_mapping.invert_axis_right_x) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_right_y) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_Y);
    }

#undef INVERT_AXIS

    // xemu_input_print_controller_state(state);
}

void xemu_input_update_rumble(ControllerState *state)
{
    if (state->type != INPUT_DEVICE_SDL_GAMEPAD) {
        return;
    }

    if (!state->controller_map->enable_rumble) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_rumble_updated_ts) <
        XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US) {
        return;
    }

    SDL_RumbleGamepad(state->sdl_gamepad, state->rumble_l, state->rumble_r, 250);
    state->last_rumble_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

ControllerState *xemu_input_get_bound(int index)
{
    assert(index >= 0 && index < 4);
    return virtual_controllers[index].provider;
}

static void xemu_input_save_provider_binding(int index, ControllerState *state,
                                             bool save)
{
    XemuVirtualControllerPort *port = &virtual_controllers[index];
    char guid_buf[35] = { 0 };

    if (state) {
        if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
            SDL_GUIDToString(state->sdl_joystick_guid, guid_buf,
                             sizeof(guid_buf));
        } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
            snprintf(guid_buf, sizeof(guid_buf), "keyboard");
        }
    }

    XemuInputProviderPreference preference =
        xemu_input_provider_preference(save, port->connected, guid_buf);
    if (preference.write_identifier) {
        xemu_settings_set_string(port_index_to_settings_key_map[index],
                                 preference.identifier);
    }
    if (preference.write_presence) {
        /* Preserve guest presence when the host preference is cleared. */
        *port_index_to_virtual_settings_key_map[index] = preference.presence;
    }
}

bool xemu_input_set_provider(int index, ControllerState *state, int save)
{
    assert(index >= 0 && index < 4);
    assert(bql_locked());
    XemuVirtualControllerPort *port = &virtual_controllers[index];

    if (port->provider == state) {
        xemu_input_save_provider_binding(index, state, save);
        return true;
    }

    /* Do not strand a provider on its old port if target creation fails. */
    if (state && !port->connected &&
        !xemu_input_virtual_connect(index, bound_drivers[index], save)) {
        return false;
    }

    if (port->provider) {
        ControllerState *old = port->provider;
        if (old->type == INPUT_DEVICE_SDL_GAMEPAD && old->sdl_gamepad) {
            SDL_RumbleGamepad(old->sdl_gamepad, 0, 0, 0);
        }
        old->rumble_l = old->rumble_r = 0;
        xemu_input_port_assign_provider(port, NULL, index);
    }

    if (state && state->bound >= 0) {
        /* One live provider may drive only one virtual port. */
        xemu_input_set_provider(state->bound, NULL, 1);
    }

    xemu_input_save_provider_binding(index, state, save);

    if (state) {
        xemu_input_port_assign_provider(port, state, index);
    }
    return true;
}

bool xemu_input_virtual_disconnect(int index, int save)
{
    assert(index >= 0 && index < 4);
    assert(bql_locked());
    XemuVirtualControllerPort *port = &virtual_controllers[index];

    if (port->connected) {
        for (int i = 0; i < 2; i++) {
            if (port->peripheral_types[i] == PERIPHERAL_XMU &&
                !xemu_input_unbind_xmu(index, i)) {
                return false;
            }
        }

        Error *local_err = NULL;
        qdev_unplug((DeviceState *)port->hub, &local_err);
        if (local_err) {
            xemu_input_report_error("Unable to disconnect Xbox controller",
                                    local_err);
            return false;
        }

        xemu_input_set_provider(index, NULL, 0);
        for (int i = 0; i < 2; i++) {
            g_free(port->peripherals[i]);
            port->peripherals[i] = NULL;
            port->peripheral_types[i] = PERIPHERAL_NONE;
        }
        port->hub = NULL;
        port->gamepad = NULL;
        port->connected = false;
    } else if (port->hub) {
        /* Retry cleanup after a failed partial construction rollback. */
        Error *local_err = NULL;
        qdev_unplug((DeviceState *)port->hub, &local_err);
        if (local_err) {
            xemu_input_report_error(
                "Unable to remove incomplete Xbox controller", local_err);
            return false;
        }
        port->hub = NULL;
    }
    if (save) {
        *port_index_to_virtual_settings_key_map[index] = 0;
    }
    return true;
}

typedef struct XemuInputTopologyCreateContext {
    int index;
    const char *driver;
} XemuInputTopologyCreateContext;

static void *xemu_input_create_hub(void *opaque, Error **errp)
{
    XemuInputTopologyCreateContext *context = opaque;
    QDict *dict = qdict_new();
    char *path = g_strdup_printf("1.%d", port_map[context->index]);

    qdict_put_str(dict, "driver", "usb-hub");
    qdict_put_str(dict, "port", path);
    qdict_put_int(dict, "ports", 3);
    g_free(path);

    DeviceState *hub = xemu_input_add_device(dict, errp);
    qobject_unref(dict);
    return hub;
}

static void *xemu_input_create_gamepad(void *opaque, Error **errp)
{
    XemuInputTopologyCreateContext *context = opaque;
    QDict *dict = qdict_new();
    static int id_counter;
    char *id = g_strdup_printf("gamepad_%d", id_counter++);
    char *path = g_strdup_printf("1.%d.1", port_map[context->index]);

    qdict_put_str(dict, "driver", context->driver);
    qdict_put_str(dict, "id", id);
    qdict_put_int(dict, "index", context->index);
    qdict_put_str(dict, "port", path);
    g_free(id);
    g_free(path);

    DeviceState *gamepad = xemu_input_add_device(dict, errp);
    qobject_unref(dict);
    return gamepad;
}

static bool xemu_input_remove_hub(void *opaque, void *hub, Error **errp)
{
    (void)opaque;
    qdev_unplug((DeviceState *)hub, errp);
    return !*errp;
}

static void xemu_input_release_device(void *device)
{
    object_unref(OBJECT(device));
}

static const XemuInputTopologyOps xemu_input_topology_ops = {
    .create_hub = xemu_input_create_hub,
    .create_gamepad = xemu_input_create_gamepad,
    .remove_hub = xemu_input_remove_hub,
    .release_device = xemu_input_release_device,
};

bool xemu_input_virtual_connect(int index, const char *driver, int save)
{
    assert(index >= 0 && index < 4);
    assert(bql_locked());
    assert(driver && (!strcmp(driver, DRIVER_DUKE) ||
                      !strcmp(driver, DRIVER_S)));
    XemuVirtualControllerPort *port = &virtual_controllers[index];

    if (port->connected && strcmp(bound_drivers[index], driver) == 0) {
        if (save) {
            *port_index_to_virtual_settings_key_map[index] = 1;
            xemu_settings_set_string(
                port_index_to_driver_settings_key_map[index], driver);
        }
        return true;
    }
    if (!port->connected && port->hub &&
        !xemu_input_virtual_disconnect(index, 0)) {
        return false;
    }

    ControllerState *provider = port->provider;
    if (port->connected && !xemu_input_virtual_disconnect(index, 0)) {
        return false;
    }

    Error *local_err = NULL;
    XemuInputTopologyCreateContext context = {
        .index = index,
        .driver = driver,
    };
    if (!xemu_input_create_virtual_devices(port, &xemu_input_topology_ops,
                                           &context, &local_err)) {
        xemu_input_report_error("Unable to create Xbox controller", local_err);
        return false;
    }

    bound_drivers[index] = driver;
    if (provider) {
        xemu_input_set_provider(index, provider, 0);
    }
    xemu_input_rebind_xmu(index);

    if (save) {
        *port_index_to_virtual_settings_key_map[index] = 1;
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index],
                                 driver);
    }
    return true;
}

bool xemu_input_set_virtual_model(int index, const char *driver, int save)
{
    assert(index >= 0 && index < 4);
    assert(bql_locked());
    assert(driver && (!strcmp(driver, DRIVER_DUKE) ||
                      !strcmp(driver, DRIVER_S)));
    if (virtual_controllers[index].connected) {
        return xemu_input_virtual_connect(index, driver, save);
    } else {
        bound_drivers[index] = driver;
        if (save) {
            xemu_settings_set_string(port_index_to_driver_settings_key_map[index],
                                     driver);
        }
    }
    return true;
}

bool xemu_input_bind_xmu(int player_index, int expansion_slot_index,
                         const char *filename, bool is_rebind)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);
    assert(bql_locked());

    XemuVirtualControllerPort *player = &virtual_controllers[player_index];
    if (!player->connected) {
        return false;
    }
    enum peripheral_type peripheral_type =
        player->peripheral_types[expansion_slot_index];
    if (peripheral_type != PERIPHERAL_XMU)
        return false;

    XmuState *xmu = (XmuState *)player->peripherals[expansion_slot_index];

    // Unbind existing XMU
    if (xmu->dev != NULL) {
        if (!xemu_input_unbind_xmu(player_index, expansion_slot_index)) {
            return false;
        }
    }

    if (filename == NULL)
        return false;

    // Look for any other XMUs that are using this file, and unbind them
    for (int player_i = 0; player_i < 4; player_i++) {
        XemuVirtualControllerPort *state = &virtual_controllers[player_i];
        if (state->connected) {
            for (int peripheral_i = 0; peripheral_i < 2; peripheral_i++) {
                if (state->peripheral_types[peripheral_i] == PERIPHERAL_XMU) {
                    XmuState *xmu_i =
                        (XmuState *)state->peripherals[peripheral_i];
                    assert(xmu_i);

                    if (xmu_i->filename != NULL &&
                        strcmp(xmu_i->filename, filename) == 0) {
                        char *buf =
                            g_strdup_printf("This XMU is already mounted on "
                                            "player %d slot %c\r\n",
                                            player_i + 1, 'A' + peripheral_i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                        return false;
                    }
                }
            }
        }
    }

    const int xmu_map[2] = { 2, 3 };
    char *tmp;

    static int id_counter = 0;
    tmp = g_strdup_printf("xmu_%d", id_counter++);

    // Add the file as a drive
    QDict *qdict1 = qdict_new();
    qdict_put_str(qdict1, "id", tmp);
    qdict_put_str(qdict1, "format", "raw");
    qdict_put_str(qdict1, "file", filename);

    // Create the usb-storage device
    QDict *qdict2 = qdict_new();

    // Specify device driver
    qdict_put_str(qdict2, "driver", "usb-storage");

    // Specify device identifier
    qdict_put_str(qdict2, "drive", tmp);
    g_free(tmp);

    // Specify index/port
    tmp = g_strdup_printf("1.%d.%d", port_map[player_index],
                          xmu_map[expansion_slot_index]);
    qdict_put_str(qdict2, "port", tmp);
    g_free(tmp);

    Error *local_err = NULL;
    XemuInputXmuCreateContext context = {
        .drive_dict = qdict1,
        .device_dict = qdict2,
    };
    void *dev = NULL;
    if (!xemu_input_create_xmu_device(&xemu_input_xmu_ops, &context, &dev,
                                      &local_err)) {
        qobject_unref(qdict1);
        qobject_unref(qdict2);
        xemu_input_report_error("Unable to mount XMU", local_err);
        return false;
    }

    xmu->filename = g_strdup(filename);
    xmu->dev = dev;

    // Unref for eventual cleanup
    qobject_unref(qdict1);
    qobject_unref(qdict2);

    if (!is_rebind) {
        xemu_save_peripheral_settings(player_index, expansion_slot_index,
                                      peripheral_type, xmu->filename);
    }

    return true;
}

bool xemu_input_unbind_xmu(int player_index, int expansion_slot_index)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);
    assert(bql_locked());

    XemuVirtualControllerPort *state = &virtual_controllers[player_index];
    if (state->peripheral_types[expansion_slot_index] != PERIPHERAL_XMU)
        return true;

    XmuState *xmu = (XmuState *)state->peripherals[expansion_slot_index];
    if (xmu != NULL) {
        if (xmu->dev != NULL) {
            Error *local_err = NULL;
            qdev_unplug((DeviceState *)xmu->dev, &local_err);
            if (local_err) {
                xemu_input_report_error("Unable to disconnect XMU", local_err);
                return false;
            }
            object_unref(OBJECT(xmu->dev));
            xmu->dev = NULL;
        }

        g_free((void *)xmu->filename);
        xmu->filename = NULL;
    }
    return true;
}

void xemu_input_rebind_xmu(int port)
{
    assert(port >= 0 && port < 4);
    assert(bql_locked());
    if (!virtual_controllers[port].connected) {
        return;
    }
    // Try to bind peripherals back to controller
    for (int i = 0; i < 2; i++) {
        enum peripheral_type peripheral_type =
            (enum peripheral_type)(*peripheral_types_settings_map[port][i]);

        // If peripheralType is out of range, change the settings for this
        // controller and peripheral port to default
        if (peripheral_type < PERIPHERAL_NONE ||
            peripheral_type >= PERIPHERAL_TYPE_COUNT) {
            xemu_save_peripheral_settings(port, i, PERIPHERAL_NONE, NULL);
            peripheral_type = PERIPHERAL_NONE;
        }

        const char *param = *peripheral_params_settings_map[port][i];

        if (peripheral_type == PERIPHERAL_XMU) {
            virtual_controllers[port].peripheral_types[i] = peripheral_type;
            if (!virtual_controllers[port].peripherals[i]) {
                virtual_controllers[port].peripherals[i] =
                    g_new0(XmuState, 1);
            }
            if (param != NULL && strlen(param) > 0) {
                bool did_bind = xemu_input_bind_xmu(port, i, param, true);
                if (did_bind) {
                    char *buf =
                        g_strdup_printf("Connected XMU %s to port %d%c",
                                        param, port + 1, 'A' + i);
                    xemu_queue_notification(buf);
                    g_free(buf);
                }
            }
        }
    }
}

void xemu_input_set_test_mode(int enabled)
{
    test_mode = enabled;
}

int xemu_input_get_test_mode(void)
{
    return test_mode;
}

void xemu_input_reset_input_mapping(ControllerState *state)
{
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        char guid[35] = { 0 };
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
        xemu_settings_reset_controller_mapping(guid);
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_settings_reset_keyboard_mapping();
    }
}
