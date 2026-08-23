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
#include "qemu/config-file.h"

#include "xemu-input.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"
#include <stdio.h>
#include <stdlib.h>

#include "system/blockdev.h"
#include "hw/xbox/chihiro/chihiro-jvs.h"
#include "hw/xbox/chihiro/chihiro.h"
#include "hw/xbox/chihiro/chihiro-driveboard.h"

extern SDL_Window *m_window;
extern int viewport_coords[4];

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
ControllerState *bound_controllers[4] = { NULL, NULL, NULL, NULL };
const char *bound_drivers[4] = { DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE,
                                 DRIVER_DUKE };
int test_mode;

// Chihiro drive-board force feedback (defined below, used in the SDL device
// add/remove handlers and the per-frame update).
static void chihiro_ffb_open(ControllerState *c);
static void chihiro_ffb_close(ControllerState *c);
static void chihiro_ffb_update(ControllerState *c);

static float m_mouseX;
static float m_mouseY;

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

int *g_chihiro_universal_map[4] = {
    &g_config.chihiro.jvs.start,
    &g_config.chihiro.jvs.service,
    &g_config.chihiro.jvs.coin,
    &g_config.chihiro.jvs.test,
};

int *g_chihiro_hotd3_map[2] = {
    &g_config.chihiro.jvs.hotd3.trigger,
    &g_config.chihiro.jvs.hotd3.body_button,
};

int *g_chihiro_vc3_map[3] = {
    &g_config.chihiro.jvs.vc3.trigger,
    &g_config.chihiro.jvs.vc3.body_button,
    &g_config.chihiro.jvs.vc3.pedal,
};

int *g_chihiro_gs_map[3] = {
    &g_config.chihiro.jvs.gs.trigger,
    &g_config.chihiro.jvs.gs.body_button,
    &g_config.chihiro.jvs.gs.change,
};

int *g_chihiro_ctx_map[7] = {
    &g_config.chihiro.jvs.ctx.steer_left,
    &g_config.chihiro.jvs.ctx.steer_right,
    &g_config.chihiro.jvs.ctx.gas,
    &g_config.chihiro.jvs.ctx.brake,
    &g_config.chihiro.jvs.ctx.drive_gear,
    &g_config.chihiro.jvs.ctx.reverse,
    &g_config.chihiro.jvs.ctx.jump,
};

int *g_chihiro_or2_map[7] = {
    &g_config.chihiro.jvs.or2.steer_left,
    &g_config.chihiro.jvs.or2.steer_right,
    &g_config.chihiro.jvs.or2.gas,
    &g_config.chihiro.jvs.or2.brake,
    &g_config.chihiro.jvs.or2.gear_up,
    &g_config.chihiro.jvs.or2.gear_down,
    &g_config.chihiro.jvs.or2.view_change,
};

int *g_chihiro_ok_map[6] = {
    &g_config.chihiro.jvs.ok.swing_left,
    &g_config.chihiro.jvs.ok.swing_right,
    &g_config.chihiro.jvs.ok.board_front,
    &g_config.chihiro.jvs.ok.board_rear,
    &g_config.chihiro.jvs.ok.left_grab,
    &g_config.chihiro.jvs.ok.right_grab,
};

int *g_chihiro_p2_universal_map[4] = {
    &g_config.chihiro.jvs_p2.start,
    &g_config.chihiro.jvs_p2.service,
    &g_config.chihiro.jvs_p2.coin,
    &g_config.chihiro.jvs_p2.test,
};

int *g_chihiro_p2_hotd3_map[2] = {
    &g_config.chihiro.jvs_p2.hotd3_trigger,
    &g_config.chihiro.jvs_p2.hotd3_body_button,
};

int *g_chihiro_p2_vc3_map[3] = {
    &g_config.chihiro.jvs_p2.vc3_trigger,
    &g_config.chihiro.jvs_p2.vc3_body_button,
    &g_config.chihiro.jvs_p2.vc3_pedal,
};

int *g_chihiro_p2_gs_map[3] = {
    &g_config.chihiro.jvs_p2.gs_trigger,
    &g_config.chihiro.jvs_p2.gs_body_button,
    &g_config.chihiro.jvs_p2.gs_change,
};

int *g_chihiro_p2_ctx_map[7] = {
    &g_config.chihiro.jvs_p2.ctx_steer_left,
    &g_config.chihiro.jvs_p2.ctx_steer_right,
    &g_config.chihiro.jvs_p2.ctx_gas,
    &g_config.chihiro.jvs_p2.ctx_brake,
    &g_config.chihiro.jvs_p2.ctx_drive_gear,
    &g_config.chihiro.jvs_p2.ctx_reverse,
    &g_config.chihiro.jvs_p2.ctx_jump,
};

int *g_chihiro_p2_or2_map[7] = {
    &g_config.chihiro.jvs_p2.or2_steer_left,
    &g_config.chihiro.jvs_p2.or2_steer_right,
    &g_config.chihiro.jvs_p2.or2_gas,
    &g_config.chihiro.jvs_p2.or2_brake,
    &g_config.chihiro.jvs_p2.or2_gear_up,
    &g_config.chihiro.jvs_p2.or2_gear_down,
    &g_config.chihiro.jvs_p2.or2_view_change,
};

int *g_chihiro_p2_ok_map[6] = {
    &g_config.chihiro.jvs_p2.ok_swing_left,
    &g_config.chihiro.jvs_p2.ok_swing_right,
    &g_config.chihiro.jvs_p2.ok_board_front,
    &g_config.chihiro.jvs_p2.ok_board_rear,
    &g_config.chihiro.jvs_p2.ok_left_grab,
    &g_config.chihiro.jvs_p2.ok_right_grab,
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
    if (strcmp(driver, DRIVER_LIGHT_GUN) == 0)
        return DRIVER_LIGHT_GUN;

    return DRIVER_DUKE;
}

static const int port_map[4] = { 3, 4, 1, 2 };

void xemu_input_init(void)
{
    if (g_config.input.background_input_capture) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        fprintf(stderr, "Failed to initialize SDL gamepad subsystem: %s\n", SDL_GetError());
        exit(1);
    }

    // Haptic is optional: needed only for steering-wheel force feedback.
    // A failure here just means wheels fall back to rumble (or nothing).
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        fprintf(stderr, "SDL haptic subsystem unavailable (wheel FFB disabled): %s\n",
                SDL_GetError());
    }

    // Create the keyboard input (always first)
    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_SDL_KEYBOARD;
    new_con->name = "Keyboard";
    new_con->bound = -1;
    new_con->peripheral_types[0] = PERIPHERAL_NONE;
    new_con->peripheral_types[1] = PERIPHERAL_NONE;
    new_con->peripherals[0] = NULL;
    new_con->peripherals[1] = NULL;
    new_con->lg.scaleX = 1.0f;
    new_con->lg.scaleY = 1.0f;

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

    // Check to see if we should auto-bind the keyboard
    int port = xemu_input_get_controller_default_bind_port(new_con, 0);
    if (port >= 0) {
        xemu_input_bind(port, new_con, 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, 
                 port+1);
        xemu_queue_notification(buf);
        xemu_input_rebind_xmu(port);
    }

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
}

int xemu_input_get_controller_default_bind_port(ControllerState *state, 
                                                int start)
{
    char guid[35] = { 0 };
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD ||
        state->type == INPUT_DEVICE_SDL_JOYSTICK) {
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        snprintf(guid, sizeof(guid), "keyboard");
    }

    for (int i = start; i < 4; i++) {
        if (strcmp(guid, *port_index_to_settings_key_map[i]) == 0) {
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

// Finish setting up a freshly-created controller (gamepad or raw joystick):
// open its haptic, list it, and auto-bind it to a port (restoring a saved
// binding by GUID when possible). Shared by the gamepad and joystick add paths.
static void xemu_input_register_controller(ControllerState *new_con)
{
    chihiro_ffb_open(new_con);

    char guid_buf[35] = { 0 };
    SDL_GUIDToString(new_con->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
    DPRINTF("Opened %s (%s)\n", new_con->name, guid_buf);

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
    // The gamepad remap (controller_map) is a gamepad-only concept; a raw
    // joystick has none (it is mapped per game in the Chihiro tab).
    if (new_con->type == INPUT_DEVICE_SDL_GAMEPAD) {
        xemu_input_bindings_reload_map(new_con);
    }

    // Do not replace binding for a currently bound device. If the same GUID is
    // specified on multiple ports, allow any available port to be bound (e.g.
    // an X360 wireless receiver hands every pad the same GUID).

    // Attempt to re-bind to a port previously bound to this GUID
    int port = 0;
    bool did_bind = false;
    while (!did_bind) {
        port = xemu_input_get_controller_default_bind_port(new_con, port);
        if (port < 0) {
            break; // No (additional) default mappings
        } else if (!xemu_input_get_bound(port)) {
            xemu_input_bind(port, new_con, 0);
            did_bind = true;
            break;
        } else {
            port++; // Try again for another port
        }
    }

    // Otherwise bind to any open port, and remember the binding
    if (!did_bind && g_config.input.auto_bind) {
        for (port = 0; port < 4; port++) {
            if (!xemu_input_get_bound(port)) {
                xemu_input_bind(port, new_con, 1);
                did_bind = true;
                break;
            }
        }
    }

    if (did_bind) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d",
                 new_con->name, port + 1);
        xemu_queue_notification(buf);
        xemu_input_rebind_xmu(port);
    }
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
        new_con->peripheral_types[0] = PERIPHERAL_NONE;
        new_con->peripheral_types[1] = PERIPHERAL_NONE;
        new_con->peripherals[0] = NULL;
        new_con->peripherals[1] = NULL;
        new_con->lg.scaleX = 1.0f;
        new_con->lg.scaleY = 1.0f;

        xemu_input_register_controller(new_con);
    } else if (event->type == SDL_EVENT_JOYSTICK_ADDED) {
        // Gamepads also raise SDL_EVENT_GAMEPAD_ADDED (handled above) and must
        // not be added twice; here we only take joysticks SDL does NOT map as a
        // gamepad -- steering wheels, flight sticks, etc.
        SDL_JoystickID which = event->jdevice.which;
        if (SDL_IsGamepad(which)) {
            return;
        }
        SDL_Joystick *sdl_joy = SDL_OpenJoystick(which);
        if (sdl_joy == NULL) {
            DPRINTF("Could not open joystick %d\n", which);
            return;
        }

        ControllerState *new_con = malloc(sizeof(ControllerState));
        memset(new_con, 0, sizeof(ControllerState));
        new_con->type              = INPUT_DEVICE_SDL_JOYSTICK;
        new_con->name              = SDL_GetJoystickName(sdl_joy);
        new_con->sdl_gamepad       = NULL;
        new_con->sdl_joystick      = sdl_joy;
        new_con->sdl_joystick_id   = SDL_GetJoystickID(sdl_joy);
        new_con->sdl_joystick_guid = SDL_GetJoystickGUID(sdl_joy);
        new_con->bound             = -1;
        new_con->peripheral_types[0] = PERIPHERAL_NONE;
        new_con->peripheral_types[1] = PERIPHERAL_NONE;
        new_con->peripherals[0] = NULL;
        new_con->peripherals[1] = NULL;
        new_con->lg.scaleX = 1.0f;
        new_con->lg.scaleY = 1.0f;

        xemu_input_register_controller(new_con);
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED ||
               event->type == SDL_EVENT_JOYSTICK_REMOVED) {
        // A removed gamepad raises both GAMEPAD_REMOVED and JOYSTICK_REMOVED;
        // whichever fires first removes it, the other simply finds nothing.
        SDL_JoystickID which = (event->type == SDL_EVENT_GAMEPAD_REMOVED)
                                   ? event->gdevice.which
                                   : event->jdevice.which;
        DPRINTF("Controller Removed: %d\n", which);
        int handled = 0;
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE(iter, &available_controllers, entry, next) {
            if (iter->type == INPUT_DEVICE_SDL_KEYBOARD) continue;

            if (iter->sdl_joystick_id == which) {
                DPRINTF("Device removed: %s\n", iter->name);

                // Disconnect
                if (iter->bound >= 0) {
                    // Queue a notification to inform user controller disconnected
                    // FIXME: Probably replace with a callback registration thing,
                    // but this works well enough for now.
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Port %d disconnected", 
                             iter->bound+1);
                    xemu_queue_notification(buf);

                    // Unbind the controller, but don't save the unbinding in
                    // case the controller is reconnected
                    xemu_input_bind(iter->bound, NULL, 0);
                }

                // Unlink
                QTAILQ_REMOVE (&available_controllers, iter, entry);

                // Deallocate
                chihiro_ffb_close(iter);
                if (iter->sdl_gamepad) {
                    SDL_CloseGamepad(iter->sdl_gamepad);
                } else if (iter->sdl_joystick) {
                    SDL_CloseJoystick(iter->sdl_joystick);
                }

                for (int i = 0; i < 2; i++) {
                    if (iter->peripherals[i])
                        g_free(iter->peripherals[i]);
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

static uint16_t jvs_axis_smooth(uint16_t pos, bool neg, bool posv)
{
    uint16_t target = 0x8000;
    if (neg && !posv) target = 0x0000;
    else if (posv && !neg) target = 0xFFFF;
    int delta = (int)target - (int)pos;
    int step = 0x1000;
    if (delta > step) return pos + step;
    if (delta < -step) return pos - step;
    return target;
}

// Steering scale from the wheel-rotation setting: `desired` degrees of physical
// rotation map to full in-game lock. Set by chihiro_apply_wheel_rotation().
static float g_wheel_steering_scale = 1.0f;

static float chihiro_axis_travel(int binding)
{
    ControllerState *pad = bound_controllers[0];
    if (!pad)
        return 0.0f;

    /* Gamepad axis: one half, with a deadzone (sticks/triggers are noisy). */
    if (CHIHIRO_BINDING_IS_AXIS(binding)) {
        if (!pad->sdl_gamepad)
            return 0.0f;
        int16_t raw = SDL_GetGamepadAxis(
            pad->sdl_gamepad, (SDL_GamepadAxis)CHIHIRO_BINDING_AXIS(binding));
        float v = raw / 32767.0f;
        if (!CHIHIRO_BINDING_AXIS_POSITIVE(binding))
            v = -v;
        if (v < 0.12f)
            return 0.0f;
        return v > 1.0f ? 1.0f : v;
    }

    /* Raw wheel steering: a half-axis passed through with only a tiny centre
     * deadzone (keeps analog[0] pinned to 0x8000 for the drive-board centring
     * check). travel(right)-travel(left) reproduces the signed axis; the game's
     * test menu calibrates the full lock range. */
    if (CHIHIRO_BINDING_IS_JOY_HALFAXIS(binding)) {
        if (!pad->sdl_joystick)
            return 0.0f;
        int16_t raw = SDL_GetJoystickAxis(pad->sdl_joystick,
                                          CHIHIRO_JOY_HALFAXIS(binding));
        float v = raw / 32767.0f;
        if (!CHIHIRO_JOY_HALFAXIS_POSITIVE(binding))
            v = -v;
        if (v < 0.02f)
            return 0.0f;
        // Range scaling: 270 deg of the physical wheel reaches full lock (arcade),
        // unless "Full range" (scale 1.0) maps the whole wheel to full lock.
        v *= g_wheel_steering_scale;
        return v > 1.0f ? 1.0f : v;
    }

    /* Raw wheel pedal: full axis oriented so "pressed" (captured at bind) runs
     * toward 1. No calibration here -- the game's test menu learns the actual
     * released/pressed range (handles pedals that rest at either extreme). */
    if (CHIHIRO_BINDING_IS_JOY_PEDAL(binding)) {
        if (!pad->sdl_joystick)
            return 0.0f;
        int raw = SDL_GetJoystickAxis(pad->sdl_joystick,
                                      CHIHIRO_JOY_PEDAL_AXIS(binding));
        if (!CHIHIRO_JOY_PEDAL_PRESS_POSITIVE(binding))
            raw = -raw;
        float v = (raw + 32768) / 65535.0f; /* released extreme -> ~0 */
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    return 0.0f;
}

static bool chihiro_check_input(int binding, const bool *kbd, uint32_t mouseBtn);

/* How far an input is pressed, 0..1. */
static float chihiro_input_travel(int binding, const bool *kbd,
                                  uint32_t mouseBtn)
{
    if (CHIHIRO_BINDING_IS_PROGRESSIVE(binding))
        return chihiro_axis_travel(binding);
    return chihiro_check_input(binding, kbd, mouseBtn) ? 1.0f : 0.0f;
}

static bool chihiro_check_input(int binding, const bool *kbd, uint32_t mouseBtn)
{
    if (CHIHIRO_BINDING_IS_PROGRESSIVE(binding))
        return chihiro_axis_travel(binding) > 0.5f;

    if (CHIHIRO_BINDING_IS_JOY_BUTTON(binding)) {
        ControllerState *pad = bound_controllers[0];
        if (pad && pad->sdl_joystick) {
            return SDL_GetJoystickButton(pad->sdl_joystick,
                                         CHIHIRO_JOY_BUTTON(binding));
        }
        return false;
    }
    if (binding >= CHIHIRO_GAMEPAD_BUTTON_BASE &&
        binding < CHIHIRO_JOYSTICK_BUTTON_BASE) {
        ControllerState *pad = bound_controllers[0];
        if (pad && pad->sdl_gamepad) {
            return SDL_GetGamepadButton(pad->sdl_gamepad,
                       (SDL_GamepadButton)(binding - CHIHIRO_GAMEPAD_BUTTON_BASE));
        }
        return false;
    }
    if (binding >= CHIHIRO_MOUSE_BUTTON_BASE) {
        int btn = binding - CHIHIRO_MOUSE_BUTTON_BASE + 1;
        return (mouseBtn & SDL_BUTTON_MASK(btn)) != 0;
    }
    return (binding > 0 && kbd[binding]);
}

/*
 * Player 2 switches. Only the light gun games have a second player; the
 * driving and skating cabinets have a single set of controls. Aiming for P2
 * would need a second pointing device, so only the buttons and CARD_IN are
 * wired here.
 */
static void chihiro_update_jvs_p2(ChihiroJVSState *jvs, const bool *kbd,
                                  uint32_t mouseBtn, int profile)
{
    uint8_t sw0 = 0, sw1 = 0;
    int tkey = 0, bkey = 0, xkey = 0;

    switch (profile) {
    case CONFIG_CHIHIRO_JVS_PROFILE_HOTD3:
        tkey = g_config.chihiro.jvs_p2.hotd3_trigger;
        bkey = g_config.chihiro.jvs_p2.hotd3_body_button;
        break;
    case CONFIG_CHIHIRO_JVS_PROFILE_VC3:
        tkey = g_config.chihiro.jvs_p2.vc3_trigger;
        bkey = g_config.chihiro.jvs_p2.vc3_body_button;
        xkey = g_config.chihiro.jvs_p2.vc3_pedal;
        break;
    case CONFIG_CHIHIRO_JVS_PROFILE_GS:
        tkey = g_config.chihiro.jvs_p2.gs_trigger;
        bkey = g_config.chihiro.jvs_p2.gs_body_button;
        xkey = g_config.chihiro.jvs_p2.gs_change;
        break;
    default:
        jvs->player_switches[1][0] = 0;
        jvs->player_switches[1][1] = 0;
        return;
    }

    if (chihiro_check_input(tkey, kbd, mouseBtn)) sw0 |= 0x02;
    if (chihiro_check_input(bkey, kbd, mouseBtn)) sw1 |= 0x80;
    if (chihiro_check_input(xkey, kbd, mouseBtn)) sw1 |= 0x40;

    if (chihiro_check_input(g_config.chihiro.jvs_p2.start, kbd, mouseBtn))
        sw0 |= 0x80;
    if (chihiro_check_input(g_config.chihiro.jvs_p2.service, kbd, mouseBtn))
        sw0 |= 0x40;

    /* The second reader reports its card the same way the first one does. */
    if (profile == CONFIG_CHIHIRO_JVS_PROFILE_GS &&
        g_config.chihiro.card_reader.enable)
        sw1 |= 0x20;

    jvs->player_switches[1][0] = sw0;
    jvs->player_switches[1][1] = sw1;

    static bool coin_prev;
    bool coin_key = chihiro_check_input(g_config.chihiro.jvs_p2.coin, kbd,
                                        mouseBtn);
    if (coin_key && !coin_prev)
        jvs->coin_count[1]++;
    coin_prev = coin_key;
}

static void xemu_input_update_jvs(void)
{
    if (!chihiro_jvs_global) return;
    ChihiroJVSState *jvs = chihiro_jvs_global;

    const bool *kbd = SDL_GetKeyboardState(NULL);
    float fmx, fmy;
    uint32_t mouseBtn = SDL_GetMouseState(&fmx, &fmy);
    uint8_t sw0 = 0;
    uint8_t sw1 = 0;

    int profile = chihiro_detected_game_profile();
    if (profile < 0) profile = g_config.chihiro.jvs.profile;

    switch (profile) {
    case CONFIG_CHIHIRO_JVS_PROFILE_HOTD3:
    case CONFIG_CHIHIRO_JVS_PROFILE_VC3:
    case CONFIG_CHIHIRO_JVS_PROFILE_GS: {
        int32_t winW, winH;
        SDL_GetWindowSize(m_window, &winW, &winH);
        float mx = fmx, my = fmy;
        if (viewport_coords[2] > 0 && viewport_coords[3] > 0) {
            int32_t drawW, drawH;
            SDL_GetWindowSizeInPixels(m_window, &drawW, &drawH);
            float scaleW = (float)winW / (float)drawW;
            float scaleH = (float)winH / (float)drawH;
            mx -= viewport_coords[0] * scaleW;
            my -= viewport_coords[1] * scaleH;
            winW = (int)(viewport_coords[2] * scaleW);
            winH = (int)(viewport_coords[3] * scaleH);
        }

        float safe = 0.01f;
        bool offscreen = (mx < safe * winW || mx > (1.0f - safe) * winW ||
                          my < safe * winH || my > (1.0f - safe) * winH);

        if (offscreen) {
            jvs->analog[0] = 0;
            jvs->analog[1] = 0;
        } else {
            float nx = mx / winW;
            float ny = my / winH;
            if (nx < 0) nx = 0;
            if (nx > 1) nx = 1;
            if (ny < 0) ny = 0;
            if (ny > 1) ny = 1;
            jvs->analog[0] = (uint16_t)(nx * 0xFFFF);
            jvs->analog[1] = (uint16_t)(ny * 0xFFFF);
        }

        int tkey, bkey;
        if (profile == CONFIG_CHIHIRO_JVS_PROFILE_HOTD3) {
            tkey = g_config.chihiro.jvs.hotd3.trigger;
            bkey = g_config.chihiro.jvs.hotd3.body_button;
        } else if (profile == CONFIG_CHIHIRO_JVS_PROFILE_VC3) {
            tkey = g_config.chihiro.jvs.vc3.trigger;
            bkey = g_config.chihiro.jvs.vc3.body_button;
        } else {
            tkey = g_config.chihiro.jvs.gs.trigger;
            bkey = g_config.chihiro.jvs.gs.body_button;
        }

        bool trigger = chihiro_check_input(tkey, kbd, mouseBtn);
        bool body = chihiro_check_input(bkey, kbd, mouseBtn);

        if (trigger) sw0 |= 0x02;
        if (body)    sw1 |= 0x80;

        if (profile == CONFIG_CHIHIRO_JVS_PROFILE_HOTD3 ||
            profile == CONFIG_CHIHIRO_JVS_PROFILE_GS) {
            if (offscreen) sw0 |= 0x01;
        } else if (profile == CONFIG_CHIHIRO_JVS_PROFILE_VC3) {
            if (offscreen && trigger) sw0 |= 0x01;
        }

        if (profile == CONFIG_CHIHIRO_JVS_PROFILE_VC3) {
            if (chihiro_check_input(g_config.chihiro.jvs.vc3.pedal, kbd, mouseBtn))
                sw1 |= 0x40;
        }
        if (profile == CONFIG_CHIHIRO_JVS_PROFILE_GS) {
            if (chihiro_check_input(g_config.chihiro.jvs.gs.change, kbd, mouseBtn))
                sw1 |= 0x40;
            if (g_config.chihiro.card_reader.enable)
                sw1 |= 0x20;
        }
        break;
    }
    case CONFIG_CHIHIRO_JVS_PROFILE_CTX:
    case CONFIG_CHIHIRO_JVS_PROFILE_OR2: {
        int b_sl, b_sr, b_gas, b_brk;
        if (profile == CONFIG_CHIHIRO_JVS_PROFILE_CTX) {
            b_sl  = g_config.chihiro.jvs.ctx.steer_left;
            b_sr  = g_config.chihiro.jvs.ctx.steer_right;
            b_gas = g_config.chihiro.jvs.ctx.gas;
            b_brk = g_config.chihiro.jvs.ctx.brake;
        } else {
            b_sl  = g_config.chihiro.jvs.or2.steer_left;
            b_sr  = g_config.chihiro.jvs.or2.steer_right;
            b_gas = g_config.chihiro.jvs.or2.gas;
            b_brk = g_config.chihiro.jvs.or2.brake;
        }

        bool sl = chihiro_check_input(b_sl, kbd, mouseBtn);
        bool sr = chihiro_check_input(b_sr, kbd, mouseBtn);

        static uint16_t steer_pos = 0x8000;
        {
            /* Keys ramp to full lock; an axis is already progressive. */
            uint16_t steer_val;
            if (CHIHIRO_BINDING_IS_PROGRESSIVE(b_sl) || CHIHIRO_BINDING_IS_PROGRESSIVE(b_sr)) {
                float a = chihiro_input_travel(b_sr, kbd, mouseBtn) -
                          chihiro_input_travel(b_sl, kbd, mouseBtn);
                steer_val = (uint16_t)(0x8000 + (int)(a * 32767.0f));
            } else {
                steer_pos = jvs_axis_smooth(steer_pos, sl, sr);
                steer_val = (sl || sr) ? steer_pos : 0x8000;
            }
            jvs->analog[0] = steer_val;
        }

        jvs->analog[1] =
            (uint16_t)(chihiro_input_travel(b_gas, kbd, mouseBtn) * 65535.0f);
        jvs->analog[2] =
            (uint16_t)(chihiro_input_travel(b_brk, kbd, mouseBtn) * 65535.0f);

        if (profile == CONFIG_CHIHIRO_JVS_PROFILE_CTX) {
            if (chihiro_check_input(g_config.chihiro.jvs.ctx.drive_gear, kbd, mouseBtn))
                sw0 |= 0x20;
            if (chihiro_check_input(g_config.chihiro.jvs.ctx.reverse, kbd, mouseBtn))
                sw0 |= 0x10;
            if (chihiro_check_input(g_config.chihiro.jvs.ctx.jump, kbd, mouseBtn))
                sw0 |= 0x02;
        } else {
            /* OR2: view change is player-1 sw0 bit 4. The sequential shifter is
             * NOT on player 1 -- it is wired to the SECOND player's up/down pins,
             * applied after the P2 update below (see there). */
            if (chihiro_check_input(g_config.chihiro.jvs.or2.view_change, kbd, mouseBtn))
                sw0 |= 0x10;
        }
        break;
    }
    case CONFIG_CHIHIRO_JVS_PROFILE_OK: {
        bool sl = chihiro_check_input(g_config.chihiro.jvs.ok.swing_left, kbd, mouseBtn);
        bool sr = chihiro_check_input(g_config.chihiro.jvs.ok.swing_right, kbd, mouseBtn);

        static uint16_t swing_pos = 0x8000;
        {
            int b_sl = g_config.chihiro.jvs.ok.swing_left;
            int b_sr = g_config.chihiro.jvs.ok.swing_right;
            /* The board reads left above centre and right below it. */
            uint16_t swing_val;
            if (CHIHIRO_BINDING_IS_PROGRESSIVE(b_sl) || CHIHIRO_BINDING_IS_PROGRESSIVE(b_sr)) {
                float a = chihiro_input_travel(b_sl, kbd, mouseBtn) -
                          chihiro_input_travel(b_sr, kbd, mouseBtn);
                swing_val = (uint16_t)(0x8000 + (int)(a * 32767.0f));
            } else {
                swing_pos = jvs_axis_smooth(swing_pos, sr, sl);
                swing_val = (sl || sr) ? swing_pos : 0x8000;
            }
            jvs->analog[1] = swing_val;
        }

        if (chihiro_check_input(g_config.chihiro.jvs.ok.board_front, kbd, mouseBtn))
            sw0 |= 0x20;
        if (chihiro_check_input(g_config.chihiro.jvs.ok.board_rear, kbd, mouseBtn))
            sw0 |= 0x10;
        if (chihiro_check_input(g_config.chihiro.jvs.ok.left_grab, kbd, mouseBtn))
            sw0 |= 0x02;
        if (chihiro_check_input(g_config.chihiro.jvs.ok.right_grab, kbd, mouseBtn))
            sw0 |= 0x01;
        break;
    }
    }

    if (chihiro_check_input(g_config.chihiro.jvs.start, kbd, mouseBtn))
        sw0 |= 0x80;
    if (chihiro_check_input(g_config.chihiro.jvs.service, kbd, mouseBtn))
        sw0 |= 0x40;

    jvs->player_switches[0][0] = sw0;
    jvs->player_switches[0][1] = sw1;

    chihiro_update_jvs_p2(jvs, kbd, mouseBtn, profile);

    // OR2's sequential shifter is wired to the SECOND player's UP/DOWN switch
    // pins: JVS sw0 bits 5/4 of player 2. Testmode.xbe (and the game) read gear
    // from that player-2 word, not from player 1 -- verified by decompiling the
    // input-test display. Applied after the P2 update so it is not cleared.
    if (profile == CONFIG_CHIHIRO_JVS_PROFILE_OR2) {
        if (chihiro_check_input(g_config.chihiro.jvs.or2.gear_up, kbd, mouseBtn))
            jvs->player_switches[1][0] |= 0x20;
        if (chihiro_check_input(g_config.chihiro.jvs.or2.gear_down, kbd, mouseBtn))
            jvs->player_switches[1][0] |= 0x10;
    }

    jvs->system_switches = chihiro_check_input(g_config.chihiro.jvs.test, kbd, mouseBtn) ? 0x80 : 0x00;

    static bool coin_prev;
    bool coin_key = chihiro_check_input(g_config.chihiro.jvs.coin, kbd, mouseBtn);
    if (coin_key && !coin_prev)
        jvs->coin_count[0]++;
    coin_prev = coin_key;
}

// ---------------------------------------------------------------------------
// Chihiro drive-board force feedback -> host haptics (OutRun 2)
//
// OutRun 2 drives the 838-13683 drive board with SPRING / TORQUE / DAMPER /
// VIBRATION commands (decoded in chihiro-driveboard.c). We translate the live
// effect state onto the bound player-1 device: a real force-feedback wheel gets
// proportional SDL_Haptic condition/constant/periodic effects; a plain gamepad
// gets rumble from the vibration channel.
//
// The arcade board itself runs a fixed-PWM binary motor; these proportional
// mappings realise the game's *computed* FFB intent on modern hardware. The
// scaling constants are host tuning knobs (feel), not emulated values.
// ---------------------------------------------------------------------------

#define CHIHIRO_FFB_SPRING_SAT   500  // per spring magnitude unit (0-127)
#define CHIHIRO_FFB_SPRING_COEFF 340  // SDL SPRING condition effect -- WEAK/ineffective on the G29's
                                      // geared motor (Logitech condition springs barely register), so
                                      // real centering is done by CONSTANT force below, not this.
// Host-computed centering: a CONSTANT force toward center from the live wheel position
// (what consumer-wheel arcade FFB plugins do, since an SDL SPRING doesn't bite a G29).
#define CHIHIRO_FFB_CENTER_GAIN  100  // force per wheel displacement (100 = full force at full lock;
                                      // higher = firmer near center but risks 60 Hz hunting/oscillation)
#define CHIHIRO_FFB_CENTER_SIGN  (1)  // Universal, NOT per-wheel. SDL_HAPTIC_STEERING_AXIS keeps force
                                      // and position polarity consistent across wheels -- the same
                                      // mechanism that lets PCSX2 center every wheel with one fixed
                                      // sign and zero invert option. +1 counters displacement toward
                                      // center (feel-confirmed on the G29; the convention is the wheel-
                                      // agnostic Linux evdev FF direction standard, not a G29 quirk).
#define CHIHIRO_FFB_TORQUE_LEVEL 240  // per torque force unit
#define CHIHIRO_FFB_DAMPER_COEFF 18   // per damper unit (mild; too high fights the spring & slows the return)
#define CHIHIRO_FFB_ROAD_MAG     40   // continuous road buzz (0x8B) -- DISABLED (see sine block):
                                      // the G29's geared motor renders a low continuous sine as a
                                      // notchy "catching", not a rumble. Kept for reference/re-enable.
#define CHIHIRO_FFB_EVENT_MAG    180  // package jolt (0xFB) -- punchy discrete kerb/wall/sand hits
// SINE period (ms) for the discrete package jolts. A geared G29 renders a low-frequency
// periodic as a notchy step; a higher frequency reads as a cleaner tap. 10 ms = 100 Hz,
// the documented sweet spot for arcade FFB on consumer wheels (BackForceFeeder).
#define CHIHIRO_FFB_SINE_PERIOD  10

static int16_t chihiro_ffb_clamp(int v)
{
    if (v > 0x7FFF)  return 0x7FFF;
    if (v < -0x7FFF) return -0x7FFF;
    return (int16_t)v;
}

// Create a zero-force effect but do NOT start it: running effects seize FFB
// control and would suppress the wheel's own auto-centering while idle. Effects
// are started only when the game's FFB actually engages (chihiro_ffb_update).
static int chihiro_ffb_make(ControllerState *c, SDL_HapticEffect *e, uint32_t feature)
{
    if (!(c->haptic_features & feature)) return -1;
    return SDL_CreateHapticEffect(c->haptic, e);
}

// Start/stop all created effects together. Stopping releases FFB control back
// to the wheel (restoring its default behaviour) when the game isn't driving it.
static void chihiro_ffb_set_running(ControllerState *c, bool run)
{
    if (c->haptic_running == run) {
        return;
    }
    // Taking over: disable the wheel's built-in autocentre so our SPRING is the
    // only centring force (otherwise the weak default autocentre is all you feel).
    if (run) {
        SDL_SetHapticAutocenter(c->haptic, 0);
        // Re-apply every effect on the next update after (re)engaging.
        c->haptic_spring_lv = c->haptic_damper_lv =
            c->haptic_constant_lv = c->haptic_sine_lv = -999999;
    }
    const int ids[] = { c->haptic_spring, c->haptic_damper,
                        c->haptic_constant, c->haptic_sine };
    for (int i = 0; i < 4; i++) {
        if (ids[i] < 0) continue;
        if (run) SDL_RunHapticEffect(c->haptic, ids[i], SDL_HAPTIC_INFINITY);
        else     SDL_StopHapticEffect(c->haptic, ids[i]);
    }
    c->haptic_running = run;
}

// Steering rotation range. The setting is the player's PHYSICAL wheel range in
// degrees; the game (the OutRun 2 cabinet) is 270 deg lock-to-lock. We software-
// scale the steering so that turning 270 deg of the wheel reaches full in-game
// lock -- arcade feel -- on a larger wheel, while a smaller wheel still reaches
// full lock at its own extreme (scale never drops below 1). 0 means "Full range":
// the whole wheel maps to full lock, no scaling. Pure math with no OS wheel API,
// so it behaves identically on Linux, Windows and macOS.
static void chihiro_apply_wheel_rotation(int wheel_degrees)
{
    const float game_degrees = 270.0f; // OutRun 2 cabinet, lock-to-lock
    if (wheel_degrees <= 0) {          // "Full range": use the whole wheel
        g_wheel_steering_scale = 1.0f;
        return;
    }
    if (wheel_degrees < 150) wheel_degrees = 150;
    float s = (float)wheel_degrees / game_degrees;
    g_wheel_steering_scale = s < 1.0f ? 1.0f : s;
}

static void chihiro_ffb_open(ControllerState *c)
{
    c->haptic = NULL;
    c->haptic_features = 0;
    c->haptic_spring = c->haptic_constant = c->haptic_sine = c->haptic_damper = -1;
    c->haptic_spring_lv = c->haptic_damper_lv =
        c->haptic_constant_lv = c->haptic_sine_lv = -999999;

    if (!c->sdl_joystick || !SDL_IsJoystickHaptic(c->sdl_joystick)) {
        return;
    }
    c->haptic = SDL_OpenHapticFromJoystick(c->sdl_joystick);
    if (!c->haptic) {
        return;
    }
    c->haptic_features = SDL_GetHapticFeatures(c->haptic);

    // All effects target the wheel's steering axis. This is REQUIRED for a real
    // wheel: SDL_HAPTIC_POLAR aims condition effects at a nonexistent second axis
    // so spring/damper produce no felt centring (only the wheel's own weak
    // autocentre survives). SDL_HAPTIC_STEERING_AXIS binds them to the wheel.
    SDL_HapticEffect e;

    memset(&e, 0, sizeof(e));
    e.condition.type = SDL_HAPTIC_SPRING;
    e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    e.condition.length = SDL_HAPTIC_INFINITY;
    c->haptic_spring = chihiro_ffb_make(c, &e, SDL_HAPTIC_SPRING);

    memset(&e, 0, sizeof(e));
    e.condition.type = SDL_HAPTIC_DAMPER;
    e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    e.condition.length = SDL_HAPTIC_INFINITY;
    c->haptic_damper = chihiro_ffb_make(c, &e, SDL_HAPTIC_DAMPER);

    memset(&e, 0, sizeof(e));
    e.constant.type = SDL_HAPTIC_CONSTANT;
    e.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
    e.constant.length = SDL_HAPTIC_INFINITY;
    c->haptic_constant = chihiro_ffb_make(c, &e, SDL_HAPTIC_CONSTANT);

    memset(&e, 0, sizeof(e));
    e.periodic.type = SDL_HAPTIC_SINE;
    e.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
    e.periodic.length = SDL_HAPTIC_INFINITY;
    e.periodic.period = CHIHIRO_FFB_SINE_PERIOD;
    c->haptic_sine = chihiro_ffb_make(c, &e, SDL_HAPTIC_SINE);
}

static void chihiro_ffb_close(ControllerState *c)
{
    if (!c->haptic) {
        return;
    }
    SDL_CloseHaptic(c->haptic);  // destroys created effects as well
    c->haptic = NULL;
    c->haptic_spring = c->haptic_constant = c->haptic_sine = c->haptic_damper = -1;
}

static void chihiro_ffb_update(ControllerState *c)
{
    if (!c || !chihiro_driveboard_global) {
        return;
    }

    // Wheel rotation range: recompute the steering scale when the setting changes.
    // Pure software (see chihiro_apply_wheel_rotation), independent of the FFB switch.
    if (c->sdl_joystick) {
        static int applied_rotation = -1;
        int deg = g_config.chihiro.settings.wheel_rotation;
        if (deg != applied_rotation) {
            applied_rotation = deg;
            chihiro_apply_wheel_rotation(deg);
        }
    }

    // Master switch: Chihiro > Force Feedback. When off, release any running
    // wheel effects and silence pad rumble; the drive board keeps responding
    // regardless, so OutRun 2 still boots past its drive-board init.
    if (!g_config.chihiro.settings.force_feedback) {
        chihiro_ffb_set_running(c, false);
        c->gp.rumble_l = c->gp.rumble_r = 0;
        return;
    }

    DriveBoardFFB ffb;
    driveboard_get_ffb(chihiro_driveboard_global, &ffb);

    // Path 1: a real force-feedback wheel (supports condition/constant force).
    if (c->haptic && (c->haptic_spring >= 0 || c->haptic_constant >= 0)) {
        // Engage effects only while the game is actually driving FFB; stopping
        // them hands control back to the wheel's own centering when idle.
        bool engage = ffb.active;
        chihiro_ffb_set_running(c, engage);
        if (!engage) {
            return;
        }

        // Combined power scaling: user slider (percent) x the game's in-game
        // FFB power (0x83; 0x60 = 100%, 0 = unset -> full).
        int str = g_config.chihiro.settings.ffb_strength;
        if (str < 0) str = 0;
        // Global power (0x83). Per the OR2 manual, MOTOR POWER is 60/80/90/100%
        // with 80% (=0x40) the shipping default, so fall back to 0x40 when the
        // game has not set it (0x60 = 100% would over-drive the servo).
        int gp = ffb.global_power ? ffb.global_power : 0x40;
        int scale = str * gp / 0x60;

        SDL_HapticEffect e;

        // CENTERING (0x87 SPRING) -> spring on the steering axis, the drive
        // board's dominant force. Re-upload ONLY when it changes: re-applying a
        // condition effect every frame glitches new-lg4ff and is exactly what
        // makes the wheel feel stuck instead of centring.
        if (c->haptic_spring >= 0) {
            int coeff = chihiro_ffb_clamp(
                ffb.centering_power * CHIHIRO_FFB_SPRING_COEFF * scale / 100);
            if (coeff != c->haptic_spring_lv) {
                c->haptic_spring_lv = coeff;
                uint16_t sat = (uint16_t)chihiro_ffb_clamp(
                    ffb.centering_power * CHIHIRO_FFB_SPRING_SAT * scale / 100);
                memset(&e, 0, sizeof(e));
                e.condition.type = SDL_HAPTIC_SPRING;
                e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
                e.condition.length = SDL_HAPTIC_INFINITY;
                e.condition.right_sat[0] = e.condition.left_sat[0] = sat;
                e.condition.right_coeff[0] = e.condition.left_coeff[0] = (Sint16)coeff;
                SDL_UpdateHapticEffect(c->haptic, c->haptic_spring, &e);
            }
        }
        // DAMPER (0x86 torque + 0x88 damper) -> a MILD resistance. Too strong and
        // it fights the spring so the wheel cannot return to centre.
        if (c->haptic_damper >= 0) {
            int coeff = chihiro_ffb_clamp(
                ffb.friction_power * CHIHIRO_FFB_DAMPER_COEFF * scale / 100);
            if (coeff != c->haptic_damper_lv) {
                c->haptic_damper_lv = coeff;
                memset(&e, 0, sizeof(e));
                e.condition.type = SDL_HAPTIC_DAMPER;
                e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
                e.condition.length = SDL_HAPTIC_INFINITY;
                e.condition.right_coeff[0] = e.condition.left_coeff[0] = (Sint16)coeff;
                e.condition.right_sat[0] = e.condition.left_sat[0] = 0x7FFF;
                SDL_UpdateHapticEffect(c->haptic, c->haptic_damper, &e);
            }
        }
        // CONSTANT force on the steering axis = MOVEMENT push (0x84, ~0 in OR2) PLUS the
        // real CENTERING. The SDL SPRING above does not bite the G29's geared motor, so
        // centering is synthesised here as a constant force toward center, proportional to
        // the live wheel displacement and gated on the game engaging centering (0x87).
        if (c->haptic_constant >= 0) {
            int lvl = ffb.movement_power * CHIHIRO_FFB_TORQUE_LEVEL * scale / 100;
            if (ffb.movement_dir) lvl = -lvl;
            if (ffb.centering_power > 0 && c->sdl_joystick) {
                int wheel = SDL_GetJoystickAxis(c->sdl_joystick, 0); // 0 = center
                lvl += CHIHIRO_FFB_CENTER_SIGN * wheel *
                       CHIHIRO_FFB_CENTER_GAIN / 100 * scale / 100;
            }
            // Cross-platform FFB polarity safety net. On Linux the sign is the universal
            // SDL STEERING_AXIS convention; Windows (DirectInput) and macOS (IOKit) can
            // invert a wheel's physical direction. This user toggle flips the whole
            // directional (constant) force -- default off = correct on Linux and the
            // standard case. Sine (buzz) and damper are direction-agnostic, so unaffected.
            if (g_config.chihiro.settings.ffb_invert) lvl = -lvl;
            lvl = chihiro_ffb_clamp(lvl);
            if (lvl != c->haptic_constant_lv) {
                c->haptic_constant_lv = lvl;
                memset(&e, 0, sizeof(e));
                e.constant.type = SDL_HAPTIC_CONSTANT;
                e.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
                e.constant.length = SDL_HAPTIC_INFINITY;
                e.constant.level = (Sint16)lvl;
                SDL_UpdateHapticEffect(c->haptic, c->haptic_constant, &e);
            }
        }
        // Package jolts (0xFB) -> sine on the steering axis: discrete kerb/wall/sand hits.
        if (c->haptic_sine >= 0) {
            // The continuous road/engine vibration (ffb.vibration, 0x8B) is intentionally
            // NOT rendered: its idle level (0x20 = the [4,15] floor) is near-imperceptible on
            // the real heavy 500W servo, and the G29's geared motor turns a low continuous
            // sine into a notchy "catching" feel, not a rumble (OutRun2-on-wheel plugins
            // likewise render only discrete events). Only the punchy package jolts play.
            int mag = chihiro_ffb_clamp(ffb.event * CHIHIRO_FFB_EVENT_MAG * scale / 100);
            if (mag != c->haptic_sine_lv) {
                c->haptic_sine_lv = mag;
                memset(&e, 0, sizeof(e));
                e.periodic.type = SDL_HAPTIC_SINE;
                e.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
                e.periodic.length = SDL_HAPTIC_INFINITY;
                e.periodic.period = CHIHIRO_FFB_SINE_PERIOD;
                e.periodic.magnitude = (Sint16)mag;
                SDL_UpdateHapticEffect(c->haptic, c->haptic_sine, &e);
            }
        }
        return;
    }

    // Path 2: plain gamepad -> rumble motors from the vibration/event channel.
    // (The rumble sender already gates on enable_rumble; setting 0 when
    // disabled keeps the state clean.)
    if (c->type == INPUT_DEVICE_SDL_GAMEPAD) {
        int str = g_config.chihiro.settings.ffb_strength;
        if (str < 0) str = 0;
        uint32_t raw = driveboard_get_rumble(chihiro_driveboard_global);
        raw = raw * (uint32_t)str / 100;
        uint16_t r = raw > 0xFFFF ? 0xFFFF : (uint16_t)raw;
        c->gp.rumble_l = r;
        c->gp.rumble_r = r;
    }
}

void xemu_input_update_controllers(void)
{
    ControllerState *iter;
    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        xemu_input_update_controller(iter);
    }

    // Chihiro drive-board force feedback -> bound player 1 (steering-wheel
    // haptics if available, otherwise gamepad rumble). See chihiro_ffb_update().
    chihiro_ffb_update(bound_controllers[0]);

    QTAILQ_FOREACH (iter, &available_controllers, entry) {
        xemu_input_update_rumble(iter);
    }
    xemu_input_update_jvs();

}

void xemu_input_update_sdl_kbd_controller_state(ControllerState *state)
{
    state->gp.buttons = 0;
    state->lg.buttons = 0;
    memset(state->gp.axis, 0, sizeof(state->gp.axis));
    memset(state->lg.axis, 0, sizeof(state->lg.axis));

    const bool *kbd = SDL_GetKeyboardState(NULL);

    if (state->bound < 0)
        return;

    const char *bound_driver = get_bound_driver(state->bound);
    if (strcmp(bound_driver, DRIVER_LIGHT_GUN) == 0) {
        uint32_t mouseBtn = SDL_GetMouseState(&m_mouseX, &m_mouseY);

        int32_t windowWidth, windowHeight;
        // Use SDL_GetWindowSize to match SDL_GetMouseState coordinate space
        // (both return logical/window coordinates, not physical/drawable pixels)
        SDL_GetWindowSize(m_window, &windowWidth, &windowHeight);

        DPRINTF("[Lightgun] Window Coordinates: %.0f, %.0f\n", m_mouseX, m_mouseY);

        // Adjust to viewport coordinates if available
        if (viewport_coords[2] > 0 && viewport_coords[3] > 0) {
            // viewport_coords are in drawable (pixel) space.
            // Scale them to window (logical) space for HiDPI compat.
            int32_t drawW, drawH;
            SDL_GetWindowSizeInPixels(m_window, &drawW, &drawH);
            float scaleW = (float)windowWidth / (float)drawW;
            float scaleH = (float)windowHeight / (float)drawH;

            // Switch from Window coordinates to Viewport Coordinates
            m_mouseX -= viewport_coords[0] * scaleW;
            m_mouseY -= viewport_coords[1] * scaleH;
            windowWidth = (int)(viewport_coords[2] * scaleW);
            windowHeight = (int)(viewport_coords[3] * scaleH);
        }

        // Check bounds AFTER viewport adjustment — mouse must be inside
        // the actual game viewport, not just the window
        if (m_mouseX >= 0 && m_mouseX <= windowWidth &&
            m_mouseY >= 0 && m_mouseY <= windowHeight) {

            DPRINTF("[Lightgun] Viewport Coordinates: %.0f, %.0f\n", m_mouseX, m_mouseY);
            // Direct linear mapping - no scale/offset correction needed.
            // Emulated gun provides pixel-perfect coordinates.
            int32_t x = (int32_t)((m_mouseX - (windowWidth / 2)) *
                                  65535 / windowWidth);
            int32_t y = (int32_t)(((windowHeight / 2) - m_mouseY) *
                                  65535 / windowHeight);

            state->lg.axis[0] = (int16_t)MIN(MAX(x, -32768), 32767);
            state->lg.axis[1] = (int16_t)MIN(MAX(y, -32768), 32767);
            state->lg.status = 0x20; // Light Visible

            DPRINTF("[LightGun] X: %d, Y: %d", state->lg.axis[0], state->lg.axis[1]);
        } else {
            state->lg.status = 0x00;
        }

        // Left mouse button is the trigger (A), right mouse button is B
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) {
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        }
        if (mouseBtn & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) {
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        }

        if (kbd[g_config.input.keyboard_controller_scancode_map.a])
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        if (kbd[g_config.input.keyboard_controller_scancode_map.b])
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        if (kbd[g_config.input.keyboard_controller_scancode_map.x])
            state->lg.buttons |= CONTROLLER_BUTTON_X;
        if (kbd[g_config.input.keyboard_controller_scancode_map.y])
            state->lg.buttons |= CONTROLLER_BUTTON_Y;
        if (kbd[g_config.input.keyboard_controller_scancode_map.start])
            state->lg.buttons |= CONTROLLER_BUTTON_START;
        if (kbd[g_config.input.keyboard_controller_scancode_map.back])
            state->lg.buttons |= CONTROLLER_BUTTON_BACK;
        if (kbd[g_config.input.keyboard_controller_scancode_map.black])
            state->lg.buttons |= CONTROLLER_BUTTON_BLACK;
        if (kbd[g_config.input.keyboard_controller_scancode_map.white])
            state->lg.buttons |= CONTROLLER_BUTTON_WHITE;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_up])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_UP;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_down])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_left])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
        if (kbd[g_config.input.keyboard_controller_scancode_map.dpad_right])
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;

    } else {
#define KBD_STATE(btn) \
        (kbd[g_config.input.keyboard_controller_scancode_map.btn])

        state->gp.buttons |= KBD_STATE(a) << 0;
        state->gp.buttons |= KBD_STATE(b) << 1;
        state->gp.buttons |= KBD_STATE(x) << 2;
        state->gp.buttons |= KBD_STATE(y) << 3;
        state->gp.buttons |= KBD_STATE(dpad_left) << 4;
        state->gp.buttons |= KBD_STATE(dpad_up) << 5;
        state->gp.buttons |= KBD_STATE(dpad_right) << 6;
        state->gp.buttons |= KBD_STATE(dpad_down) << 7;
        state->gp.buttons |= KBD_STATE(back) << 8;
        state->gp.buttons |= KBD_STATE(start) << 9;
        state->gp.buttons |= KBD_STATE(white) << 10;
        state->gp.buttons |= KBD_STATE(black) << 11;
        state->gp.buttons |= KBD_STATE(lstick_btn) << 12;
        state->gp.buttons |= KBD_STATE(rstick_btn) << 13;
        state->gp.buttons |= KBD_STATE(guide) << 14;

        if (KBD_STATE(lstick_up))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_Y] = 32767;
        if (KBD_STATE(lstick_left))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_X] = -32768;
        if (KBD_STATE(lstick_right))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_X] = 32767;
        if (KBD_STATE(lstick_down))
            state->gp.axis[CONTROLLER_AXIS_LSTICK_Y] = -32768;
        if (KBD_STATE(ltrigger))
            state->gp.axis[CONTROLLER_AXIS_LTRIG] = 32767;

        if (KBD_STATE(rstick_up))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_Y] = 32767;
        if (KBD_STATE(rstick_left))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_X] = -32768;
        if (KBD_STATE(rstick_right))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_X] = 32767;
        if (KBD_STATE(rstick_down))
            state->gp.axis[CONTROLLER_AXIS_RSTICK_Y] = -32768;
        if (KBD_STATE(rtrigger))
            state->gp.axis[CONTROLLER_AXIS_RTRIG] = 32767;

#undef KBD_STATE
    }
}

void xemu_input_update_sdl_controller_state(ControllerState *state)
{
    state->gp.buttons = 0;
    memset(state->gp.axis, 0, sizeof(state->gp.axis));

    if (state->bound < 0)
        return;

    const char *bound_driver = get_bound_driver(state->bound);
    if (strcmp(bound_driver, DRIVER_LIGHT_GUN) == 0) {
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_EAST))
            state->lg.buttons |= CONTROLLER_BUTTON_A;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_SOUTH))
            state->lg.buttons |= CONTROLLER_BUTTON_B;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_WEST))
            state->lg.buttons |= CONTROLLER_BUTTON_X;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_NORTH))
            state->lg.buttons |= CONTROLLER_BUTTON_Y;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_START))
            state->lg.buttons |= CONTROLLER_BUTTON_START;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_BACK))
            state->lg.buttons |= CONTROLLER_BUTTON_BACK;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
            state->lg.buttons |= CONTROLLER_BUTTON_BLACK;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
            state->lg.buttons |= CONTROLLER_BUTTON_WHITE;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_UP;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_DOWN;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_LEFT;
        if (SDL_GetGamepadButton(state->sdl_gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
            state->lg.buttons |= CONTROLLER_BUTTON_DPAD_RIGHT;

        state->lg.axis[0] = SDL_GetGamepadAxis(
            state->sdl_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        state->lg.axis[1] = SDL_GetGamepadAxis(
            state->sdl_gamepad, SDL_GAMEPAD_AXIS_LEFTY);

    } else {
#define SDL_MASK_BUTTON(state, btn, idx)                  \
    (SDL_GetGamepadButton(                                \
         (state)->sdl_gamepad,                            \
         (state)->controller_map->controller_mapping.btn) \
     << idx)

        state->gp.buttons |= SDL_MASK_BUTTON(state, a, 0);
        state->gp.buttons |= SDL_MASK_BUTTON(state, b, 1);
        state->gp.buttons |= SDL_MASK_BUTTON(state, x, 2);
        state->gp.buttons |= SDL_MASK_BUTTON(state, y, 3);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_left, 4);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_up, 5);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_right, 6);
        state->gp.buttons |= SDL_MASK_BUTTON(state, dpad_down, 7);
        state->gp.buttons |= SDL_MASK_BUTTON(state, back, 8);
        state->gp.buttons |= SDL_MASK_BUTTON(state, start, 9);
        state->gp.buttons |= SDL_MASK_BUTTON(state, lshoulder, 10);
        state->gp.buttons |= SDL_MASK_BUTTON(state, rshoulder, 11);
        state->gp.buttons |= SDL_MASK_BUTTON(state, lstick_btn, 12);
        state->gp.buttons |= SDL_MASK_BUTTON(state, rstick_btn, 13);
        state->gp.buttons |= SDL_MASK_BUTTON(state, guide, 14);

#undef SDL_MASK_BUTTON

#define SDL_GET_AXIS(state, axis)    \
    SDL_GetGamepadAxis(              \
        (state)->sdl_gamepad,        \
        (state)->controller_map->controller_mapping.axis)

        state->gp.axis[0] = SDL_GET_AXIS(state, axis_trigger_left);
        state->gp.axis[1] = SDL_GET_AXIS(state, axis_trigger_right);
        state->gp.axis[2] = SDL_GET_AXIS(state, axis_left_x);
        state->gp.axis[3] = SDL_GET_AXIS(state, axis_left_y);
        state->gp.axis[4] = SDL_GET_AXIS(state, axis_right_x);
        state->gp.axis[5] = SDL_GET_AXIS(state, axis_right_y);

#undef SDL_GET_AXIS

// FIXME: Check range
#define INVERT_AXIS(controller_axis) \
        state->gp.axis[controller_axis] = -1 - state->gp.axis[controller_axis]

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

    SDL_RumbleGamepad(state->sdl_gamepad, state->gp.rumble_l, state->gp.rumble_r, 250);
    state->last_rumble_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

ControllerState *xemu_input_get_bound(int index)
{
    return bound_controllers[index];
}

void xemu_input_bind(int index, ControllerState *state, int save)
{
    // FIXME: Attempt to disable rumble when unbinding so it's not left
    // in rumble mode

    // Unbind existing controller
    if (bound_controllers[index]) {
        // A Chihiro pad has no USB device of its own: nothing to unplug.
        if (bound_controllers[index]->device) {
            Error *err = NULL;

            // Unbind any XMUs
            for (int i = 0; i < 2; i++) {
                if (bound_controllers[index]->peripherals[i]) {
                    // If this was an XMU, unbind the XMU
                    if (bound_controllers[index]->peripheral_types[i] ==
                        PERIPHERAL_XMU)
                        xemu_input_unbind_xmu(index, i);

                    // Free up the XmuState and set the peripheral type to none
                    g_free(bound_controllers[index]->peripherals[i]);
                    bound_controllers[index]->peripherals[i] = NULL;
                    bound_controllers[index]->peripheral_types[i] =
                        PERIPHERAL_NONE;
                }
            }

            qdev_unplug((DeviceState *)bound_controllers[index]->device, &err);
            assert(err == NULL);
        }

        bound_controllers[index]->bound = -1;
        bound_controllers[index]->device = NULL;
        bound_controllers[index] = NULL;
    }

    // Save this controller's GUID in settings for auto re-connect
    if (save) {
        char guid_buf[35] = { 0 };
        if (state) {
            if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
                SDL_GUIDToString(state->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
            } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
                snprintf(guid_buf, sizeof(guid_buf), "keyboard");
            }
        }
        xemu_settings_set_string(port_index_to_settings_key_map[index], guid_buf);
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index],
                                 bound_drivers[index]);
    }

    // Bind new controller
    if (state) {
        if (state->bound >= 0) {
            // Device was already bound to another port. Unbind it.
            xemu_input_bind(state->bound, NULL, 1);
        }

        bound_controllers[index] = state;
        bound_controllers[index]->bound = index;

        /* In Chihiro mode, USB ports are used by baseboard AN2131 devices.
         * Skip gamepad hub/controller creation — input comes via JVS I/O. */
        int mem_chk = ((int)g_config.sys.mem_limit + 1) * 64;
        if (mem_chk > 64) {
            return;
        }

        char *tmp;
        QDict *usbhub_qdict = NULL;
        DeviceState *usbhub_dev = NULL;

        // Create controller's internal USB hub.
        usbhub_qdict = qdict_new();
        qdict_put_str(usbhub_qdict, "driver", "usb-hub");
        tmp = g_strdup_printf("1.%d", port_map[index]);
        qdict_put_str(usbhub_qdict, "port", tmp);
        qdict_put_int(usbhub_qdict, "ports", 3);
        QemuOpts *usbhub_opts = qemu_opts_from_qdict(
            qemu_find_opts("device"), usbhub_qdict, &error_abort);
        usbhub_dev = qdev_device_add(usbhub_opts, &error_abort);
        g_free(tmp);

        // Create XID controller. This is connected to Port 1 of the
        // controller's internal USB Hub
        QDict *qdict = qdict_new();

        // Specify device driver
        qdict_put_str(qdict, "driver", bound_drivers[index]);

        // Specify device identifier
        static int id_counter = 0;
        tmp = g_strdup_printf("gamepad_%d", id_counter++);
        qdict_put_str(qdict, "id", tmp);
        g_free(tmp);

        // Specify index/port
        qdict_put_int(qdict, "index", index);
        tmp = g_strdup_printf("1.%d.1", port_map[index]);
        qdict_put_str(qdict, "port", tmp);
        g_free(tmp);

        // Create the device
        QemuOpts *opts = 
            qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &error_abort);
        DeviceState *dev = qdev_device_add(opts, &error_abort);
        assert(dev);

        // Unref for eventual cleanup
        qobject_unref(usbhub_qdict);
        object_unref(OBJECT(usbhub_dev));
        qobject_unref(qdict);
        object_unref(OBJECT(dev));

        state->device = usbhub_dev;
    }
}

bool xemu_input_bind_xmu(int player_index, int expansion_slot_index,
                         const char *filename, bool is_rebind)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *player = bound_controllers[player_index];
    enum peripheral_type peripheral_type =
        player->peripheral_types[expansion_slot_index];
    if (peripheral_type != PERIPHERAL_XMU)
        return false;

    XmuState *xmu = (XmuState *)player->peripherals[expansion_slot_index];

    // Unbind existing XMU
    if (xmu->dev != NULL) {
        xemu_input_unbind_xmu(player_index, expansion_slot_index);
    }

    if (filename == NULL)
        return false;

    // Look for any other XMUs that are using this file, and unbind them
    for (int player_i = 0; player_i < 4; player_i++) {
        ControllerState *state = bound_controllers[player_i];
        if (state != NULL) {
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

    xmu->filename = g_strdup(filename);

    const int xmu_map[2] = { 2, 3 };
    char *tmp;

    static int id_counter = 0;
    tmp = g_strdup_printf("xmu_%d", id_counter++);

    // Add the file as a drive
    QDict *qdict1 = qdict_new();
    qdict_put_str(qdict1, "id", tmp);
    qdict_put_str(qdict1, "format", "raw");
    qdict_put_str(qdict1, "file", filename);

    QemuOpts *drvopts =
        qemu_opts_from_qdict(qemu_find_opts("drive"), qdict1, &error_abort);

    DriveInfo *dinfo = drive_new(drvopts, 0, &error_abort);
    assert(dinfo);

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

    // Create the device
    QemuOpts *opts =
        qemu_opts_from_qdict(qemu_find_opts("device"), qdict2, &error_abort);

    DeviceState *dev = qdev_device_add(opts, &error_abort);
    assert(dev);

    xmu->dev = (void *)dev;

    // Unref for eventual cleanup
    qobject_unref(qdict1);
    qobject_unref(qdict2);

    if (!is_rebind) {
        xemu_save_peripheral_settings(player_index, expansion_slot_index,
                                      peripheral_type, xmu->filename);
    }

    return true;
}

void xemu_input_unbind_xmu(int player_index, int expansion_slot_index)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *state = bound_controllers[player_index];
    if (state->peripheral_types[expansion_slot_index] != PERIPHERAL_XMU)
        return;

    XmuState *xmu = (XmuState *)state->peripherals[expansion_slot_index];
    if (xmu != NULL) {
        if (xmu->dev != NULL) {
            qdev_unplug((DeviceState *)xmu->dev, &error_abort);
            object_unref(OBJECT(xmu->dev));
            xmu->dev = NULL;
        }

        g_free((void *)xmu->filename);
        xmu->filename = NULL;
    }
}

void xemu_input_rebind_xmu(int port)
{
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
            if (param != NULL && strlen(param) > 0) {
                // This is an XMU and needs to be bound to this controller
                if (qemu_access(param, R_OK | W_OK) == 0) {
                    bound_controllers[port]->peripheral_types[i] =
                        peripheral_type;
                    bound_controllers[port]->peripherals[i] =
                        g_malloc(sizeof(XmuState));
                    memset(bound_controllers[port]->peripherals[i], 0,
                           sizeof(XmuState));
                    bool did_bind = xemu_input_bind_xmu(port, i, param, true);
                    if (did_bind) {
                        char *buf =
                            g_strdup_printf("Connected XMU %s to port %d%c",
                                            param, port + 1, 'A' + i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                    }
                } else {
                    char *buf =
                        g_strdup_printf("Unable to bind XMU at %s to port %d%c",
                                        param, port + 1, 'A' + i);
                    xemu_queue_error_message(buf);
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

int xemu_input_lightgun_active(void)
{
    for (int i = 0; i < 4; i++) {
        if (bound_drivers[i] &&
            strcmp(bound_drivers[i], DRIVER_LIGHT_GUN) == 0) {
            return 1;
        }
    }
    return 0;
}

int xemu_input_get_lightgun_pos(int player_index, int16_t *x, int16_t *y)
{
    if (player_index < 0 || player_index >= 4) return 0;
    if (!bound_drivers[player_index] ||
        strcmp(bound_drivers[player_index], DRIVER_LIGHT_GUN) != 0)
        return 0;
    ControllerState *s = bound_controllers[player_index];
    if (!s || s->lg.status != 0x20) return 0;
    *x = s->lg.axis[0];
    *y = s->lg.axis[1];
    return 1;
}
