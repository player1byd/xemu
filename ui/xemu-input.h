/*
 * xemu Input Management
 *
 * This is the main input abstraction layer for xemu, which is basically just a
 * wrapper around SDL3 Gamepad/Keyboard API to map specifically to an
 * Xbox gamepad and support automatic binding, hotplugging, and removal at
 * runtime.
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

#ifndef XEMU_INPUT_H
#define XEMU_INPUT_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "qemu/queue.h"
#include "xemu-settings.h"
#include <SDL3/SDL.h>

typedef struct Error Error;

#define DRIVER_DUKE "usb-xbox-gamepad"
#define DRIVER_S "usb-xbox-gamepad-s"

#define DRIVER_DUKE_DISPLAY_NAME "Xbox Controller"
#define DRIVER_S_DISPLAY_NAME "Xbox Controller S"

enum controller_state_buttons_mask {
    CONTROLLER_BUTTON_A          = (1 << 0),
    CONTROLLER_BUTTON_B          = (1 << 1),
    CONTROLLER_BUTTON_X          = (1 << 2),
    CONTROLLER_BUTTON_Y          = (1 << 3),
    CONTROLLER_BUTTON_DPAD_LEFT  = (1 << 4),
    CONTROLLER_BUTTON_DPAD_UP    = (1 << 5),
    CONTROLLER_BUTTON_DPAD_RIGHT = (1 << 6),
    CONTROLLER_BUTTON_DPAD_DOWN  = (1 << 7),
    CONTROLLER_BUTTON_BACK       = (1 << 8),
    CONTROLLER_BUTTON_START      = (1 << 9),
    CONTROLLER_BUTTON_WHITE      = (1 << 10),
    CONTROLLER_BUTTON_BLACK      = (1 << 11),
    CONTROLLER_BUTTON_LSTICK     = (1 << 12),
    CONTROLLER_BUTTON_RSTICK     = (1 << 13),
    CONTROLLER_BUTTON_GUIDE      = (1 << 14), // Extension
};

#define CONTROLLER_STATE_BUTTON_ID_TO_MASK(x) (1<<x)

enum controller_state_axis_index {
    CONTROLLER_AXIS_LTRIG,
    CONTROLLER_AXIS_RTRIG,
    CONTROLLER_AXIS_LSTICK_X,
    CONTROLLER_AXIS_LSTICK_Y,
    CONTROLLER_AXIS_RSTICK_X,
    CONTROLLER_AXIS_RSTICK_Y,
    CONTROLLER_AXIS__COUNT,
};

enum controller_input_device_type {
    INPUT_DEVICE_SDL_KEYBOARD,
    INPUT_DEVICE_SDL_GAMEPAD,
};

enum peripheral_type { PERIPHERAL_NONE, PERIPHERAL_XMU, PERIPHERAL_TYPE_COUNT };

typedef struct XmuState {
    const char *filename;
    void *dev;
} XmuState;

typedef struct ControllerState {
    QTAILQ_ENTRY(ControllerState) entry;

    int64_t last_input_updated_ts;
    int64_t last_rumble_updated_ts;

    // Input state
    uint16_t buttons;
    int16_t  axis[CONTROLLER_AXIS__COUNT];

    // Rendering state hacked on here for convenience but needs to be moved (FIXME)
    uint32_t animate_guide_button_end;
    uint32_t animate_trigger_end;

    // Rumble state
    uint16_t rumble_l, rumble_r;

    enum controller_input_device_type type;
    const char         *name;
    SDL_Gamepad        *sdl_gamepad; // if type == INPUT_DEVICE_SDL_GAMEPAD
    SDL_Joystick       *sdl_joystick;
    SDL_JoystickID      sdl_joystick_id;
    SDL_GUID            sdl_joystick_guid;

    GamepadMappings *controller_map;

    int   bound;  // Which port this input device is bound to
} ControllerState;

/* Guest USB topology and expansion devices outlive a host input provider. */
typedef struct XemuVirtualControllerPort {
    bool connected;
    ControllerState *provider;
    void *hub;     // DeviceState opaque
    void *gamepad; // DeviceState opaque
    enum peripheral_type peripheral_types[2];
    void *peripherals[2];
} XemuVirtualControllerPort;

typedef struct XemuInputTopologyOps {
    void *(*create_hub)(void *opaque, Error **errp);
    void *(*create_gamepad)(void *opaque, Error **errp);
    bool (*remove_hub)(void *opaque, void *hub, Error **errp);
    void (*release_device)(void *device);
} XemuInputTopologyOps;

bool xemu_input_create_virtual_devices(XemuVirtualControllerPort *port,
                                       const XemuInputTopologyOps *ops,
                                       void *opaque, Error **errp);

typedef struct XemuInputXmuOps {
    void *(*create_drive)(void *opaque, Error **errp);
    void *(*create_device)(void *opaque, void *drive, Error **errp);
    void (*remove_drive)(void *opaque, void *drive);
} XemuInputXmuOps;

bool xemu_input_create_xmu_device(const XemuInputXmuOps *ops, void *opaque,
                                  void **device, Error **errp);

/* Host assignment never changes guest USB or expansion ownership. */
static inline ControllerState *xemu_input_port_assign_provider(
    XemuVirtualControllerPort *port, ControllerState *provider, int index)
{
    ControllerState *previous = port->provider;
    if (previous) {
        previous->bound = -1;
    }
    port->provider = provider;
    if (provider) {
        provider->bound = index;
    }
    return previous;
}

static inline bool xemu_input_port_should_exist(int setting,
                                                 const char *legacy_guid)
{
    return setting == 1 ||
           (setting < 0 && legacy_guid && legacy_guid[0]);
}

static inline int xemu_input_normalize_virtual_presence(int setting)
{
    return setting == 0 || setting == 1 ? setting : -1;
}

typedef struct XemuInputProviderPreference {
    const char *identifier;
    bool write_identifier;
    bool write_presence;
    int presence;
} XemuInputProviderPreference;

static inline XemuInputProviderPreference xemu_input_provider_preference(
    bool save, bool virtual_controller_connected, const char *identifier)
{
    XemuInputProviderPreference preference;
    preference.identifier = identifier ? identifier : "";
    preference.write_identifier = save;
    preference.write_presence = save && virtual_controller_connected;
    preference.presence = 1;
    return preference;
}

typedef QTAILQ_HEAD(, ControllerState) ControllerStateList;
extern ControllerStateList available_controllers;
extern XemuVirtualControllerPort virtual_controllers[4];
extern const char *bound_drivers[4];

#ifdef __cplusplus
extern "C" {
#endif

extern int *g_keyboard_scancode_map[25];

void xemu_input_init(void);
void xemu_input_process_sdl_events(const SDL_Event *event);
void xemu_input_update_controllers(void);
void xemu_input_update_controller(ControllerState *state);
void xemu_input_update_sdl_kbd_controller_state(ControllerState *state);
void xemu_input_update_sdl_controller_state(ControllerState *state);
void xemu_input_update_rumble(ControllerState *state);
ControllerState *xemu_input_get_bound(int index);
/* Guest topology and provider mutation functions require the BQL. */
bool xemu_input_set_provider(int index, ControllerState *state, int save);
bool xemu_input_virtual_connect(int index, const char *driver, int save);
bool xemu_input_virtual_disconnect(int index, int save);
bool xemu_input_set_virtual_model(int index, const char *driver, int save);
bool xemu_input_bind_xmu(int player_index, int peripheral_port_index,
                         const char *filename, bool is_rebind);
void xemu_input_rebind_xmu(int port);
bool xemu_input_unbind_xmu(int player_index, int peripheral_port_index);
int xemu_input_get_controller_default_bind_port(ControllerState *state, int start);
void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter);

void xemu_input_set_test_mode(int enabled);
int xemu_input_get_test_mode(void);
void xemu_input_reset_input_mapping(ControllerState *state);

#ifdef __cplusplus
}
#endif

#endif
