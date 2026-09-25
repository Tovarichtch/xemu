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

#define DRIVER_DUKE "usb-xbox-gamepad"
#define DRIVER_S "usb-xbox-gamepad-s"
#define DRIVER_LIGHT_GUN "usb-xbox-light-gun"

#define DRIVER_DUKE_DISPLAY_NAME "Xbox Controller"
#define DRIVER_S_DISPLAY_NAME "Xbox Controller S"
#define DRIVER_LIGHT_GUN_DISPLAY_NAME "Light Gun"

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
    INPUT_DEVICE_SDL_JOYSTICK, // raw joystick (steering wheel etc.), not a mapped gamepad
};

enum peripheral_type { PERIPHERAL_NONE, PERIPHERAL_XMU, PERIPHERAL_TYPE_COUNT };

typedef struct XmuState {
    const char *filename;
    void *dev;
} XmuState;

typedef struct GamepadState {
    // Input state
    uint16_t buttons;
    int16_t  axis[CONTROLLER_AXIS__COUNT];

    // Rendering state hacked on here for convenience but needs to be moved (FIXME)
    uint32_t animate_guide_button_end;
    uint32_t animate_trigger_end;

    // Rumble state
    uint16_t rumble_l, rumble_r;
} GamepadState;

typedef struct LightGunState {
    // Input State
    uint16_t buttons;
    uint8_t status;
    int16_t axis[2];

    // Calibration
    int16_t offsetX;
    int16_t offsetY;
    float scaleX;
    float scaleY;
} LightGunState;

typedef struct ControllerState {
    QTAILQ_ENTRY(ControllerState) entry;

    int64_t last_input_updated_ts;
    int64_t last_rumble_updated_ts;

    GamepadState gp;
    LightGunState lg;

    enum controller_input_device_type type;
    const char         *name;
    SDL_Gamepad        *sdl_gamepad; // if type == INPUT_DEVICE_SDL_GAMEPAD
    SDL_Joystick       *sdl_joystick;
    SDL_JoystickID      sdl_joystick_id;
    SDL_GUID            sdl_joystick_guid;

    // Chihiro drive-board force feedback on an FFB steering wheel (OutRun 2).
    // haptic is NULL for pads with only rumble motors.
    SDL_Haptic         *haptic;
    uint32_t            haptic_features;
    int                 haptic_spring;   // effect ids, -1 when unavailable
    int                 haptic_constant;
    int                 haptic_damper;
    bool                haptic_running;  // effects currently engaged (running)
    // Last value uploaded to each effect. Re-uploading an unchanged condition
    // effect every frame glitches new-lg4ff (wheel feels stuck), so only update
    // on change. -999999 = "force next update".
    int                 haptic_spring_lv, haptic_damper_lv;
    int                 haptic_constant_lv;
    int                 haptic_autocenter_lv; // built-in autocentre %, -1 = unknown

    enum peripheral_type peripheral_types[2];
    void *peripherals[2];

    GamepadMappings *controller_map;

    int   bound;  // Which port this input device is bound to
    void *device; // DeviceState opaque
} ControllerState;

typedef QTAILQ_HEAD(, ControllerState) ControllerStateList;
extern ControllerStateList available_controllers;
extern ControllerState *bound_controllers[4];
extern const char *bound_drivers[4];

#ifdef __cplusplus
extern "C" {
#endif

/* 1001..1013: buttons of the device the player aims with (the system mouse
 * or its own pointer device), SDL layout: left, middle, right, x1, x2, then
 * the 8 extra buttons some light guns carry. */
#define CHIHIRO_MOUSE_BUTTON_BASE 1001
#define CHIHIRO_POINTER_BUTTONS 13
#define CHIHIRO_GAMEPAD_BUTTON_BASE 2001

/* A JVS input may also be bound to one half of a gamepad axis. */
#define CHIHIRO_GAMEPAD_AXIS_BASE 3001
#define CHIHIRO_AXIS_BINDING(axis, positive) \
    (CHIHIRO_GAMEPAD_AXIS_BASE + (axis) * 2 + ((positive) ? 1 : 0))
#define CHIHIRO_BINDING_AXIS(b) (((b) - CHIHIRO_GAMEPAD_AXIS_BASE) / 2)
#define CHIHIRO_BINDING_AXIS_POSITIVE(b) ((((b) - CHIHIRO_GAMEPAD_AXIS_BASE) & 1) != 0)

/*
 * Raw SDL_Joystick bindings (steering wheels etc.) live in namespaces above the
 * gamepad ones: a wheel exposes more axes/buttons than the fixed Xbox layout,
 * and the read side must use the SDL_Joystick API, not SDL_Gamepad. Steering is
 * a half-axis (centred at 0, like a stick); a pedal is a full-axis whose
 * "pressed" direction is captured at bind time, then the raw value is fed
 * straight to the JVS analog so the GAME's test-menu calibrates range/direction
 * (exactly as an operator calibrates the pots on a real cabinet).
 */
#define CHIHIRO_JOYSTICK_BUTTON_BASE   4001 /* + button index */
#define CHIHIRO_JOYSTICK_HALFAXIS_BASE 5001 /* + axis*2 + positive       (steering) */
#define CHIHIRO_JOYSTICK_PEDAL_BASE    6001 /* + axis*2 + press_positive  (pedals) */
#define CHIHIRO_JOYSTICK_PEDAL_END     8000
/* A raw-joystick binding also names the port its device is bound to, so a
 * pedal set or a shifter on another port maps like the wheel: value +
 * port * stride. Port 1 keeps the historical values. */
#define CHIHIRO_JOY_PORT_STRIDE        10000
#define CHIHIRO_JOY_PORTS              4
#define CHIHIRO_JOYSTICK_END \
    (CHIHIRO_JOYSTICK_BUTTON_BASE + CHIHIRO_JOY_PORTS * CHIHIRO_JOY_PORT_STRIDE)

#define CHIHIRO_JOY_BUTTON_BINDING(btn) (CHIHIRO_JOYSTICK_BUTTON_BASE + (btn))
#define CHIHIRO_JOY_HALFAXIS_BINDING(axis, positive) \
    (CHIHIRO_JOYSTICK_HALFAXIS_BASE + (axis) * 2 + ((positive) ? 1 : 0))
#define CHIHIRO_JOY_PEDAL_BINDING(axis, press_positive) \
    (CHIHIRO_JOYSTICK_PEDAL_BASE + (axis) * 2 + ((press_positive) ? 1 : 0))
#define CHIHIRO_JOY_PORTED(b, port) ((b) + (port) * CHIHIRO_JOY_PORT_STRIDE)

/* gamepad axis: 3001..4000 */
#define CHIHIRO_BINDING_IS_AXIS(b) \
    ((b) >= CHIHIRO_GAMEPAD_AXIS_BASE && (b) < CHIHIRO_JOYSTICK_BUTTON_BASE)
/* raw joystick, any port: 4001..44000 */
#define CHIHIRO_BINDING_IS_JOY(b) \
    ((b) >= CHIHIRO_JOYSTICK_BUTTON_BASE && (b) < CHIHIRO_JOYSTICK_END)
#define CHIHIRO_JOY_PORT(b) \
    (((b) - CHIHIRO_JOYSTICK_BUTTON_BASE) / CHIHIRO_JOY_PORT_STRIDE)
#define CHIHIRO_JOY_UNPORTED(b) \
    ((b) - CHIHIRO_JOY_PORT(b) * CHIHIRO_JOY_PORT_STRIDE)
/* raw joystick button: 4001..5000 */
#define CHIHIRO_BINDING_IS_JOY_BUTTON(b) \
    (CHIHIRO_BINDING_IS_JOY(b) && \
     CHIHIRO_JOY_UNPORTED(b) < CHIHIRO_JOYSTICK_HALFAXIS_BASE)
#define CHIHIRO_JOY_BUTTON(b) \
    (CHIHIRO_JOY_UNPORTED(b) - CHIHIRO_JOYSTICK_BUTTON_BASE)
/* raw joystick half-axis (steering): 5001..6000 */
#define CHIHIRO_BINDING_IS_JOY_HALFAXIS(b) \
    (CHIHIRO_BINDING_IS_JOY(b) && \
     CHIHIRO_JOY_UNPORTED(b) >= CHIHIRO_JOYSTICK_HALFAXIS_BASE && \
     CHIHIRO_JOY_UNPORTED(b) < CHIHIRO_JOYSTICK_PEDAL_BASE)
#define CHIHIRO_JOY_HALFAXIS(b) \
    ((CHIHIRO_JOY_UNPORTED(b) - CHIHIRO_JOYSTICK_HALFAXIS_BASE) / 2)
#define CHIHIRO_JOY_HALFAXIS_POSITIVE(b) \
    (((CHIHIRO_JOY_UNPORTED(b) - CHIHIRO_JOYSTICK_HALFAXIS_BASE) & 1) != 0)
/* raw joystick full-axis pedal: 6001..7999 */
#define CHIHIRO_BINDING_IS_JOY_PEDAL(b) \
    (CHIHIRO_BINDING_IS_JOY(b) && \
     CHIHIRO_JOY_UNPORTED(b) >= CHIHIRO_JOYSTICK_PEDAL_BASE && \
     CHIHIRO_JOY_UNPORTED(b) < CHIHIRO_JOYSTICK_PEDAL_END)
#define CHIHIRO_JOY_PEDAL_AXIS(b) \
    ((CHIHIRO_JOY_UNPORTED(b) - CHIHIRO_JOYSTICK_PEDAL_BASE) / 2)
#define CHIHIRO_JOY_PEDAL_PRESS_POSITIVE(b) \
    (((CHIHIRO_JOY_UNPORTED(b) - CHIHIRO_JOYSTICK_PEDAL_BASE) & 1) != 0)

/* "Progressive" = any analog travel input (vs a digital button/key). */
#define CHIHIRO_BINDING_IS_PROGRESSIVE(b) \
    (CHIHIRO_BINDING_IS_AXIS(b) || CHIHIRO_BINDING_IS_JOY_HALFAXIS(b) || \
     CHIHIRO_BINDING_IS_JOY_PEDAL(b))

extern int *g_keyboard_scancode_map[25];
extern int *g_chihiro_universal_map[4];
extern int *g_chihiro_hotd3_map[2];
extern int *g_chihiro_vc3_map[4];
extern int *g_chihiro_gs_map[5];
extern int *g_chihiro_drive_map[4];
extern int *g_chihiro_ctx_map[3];
extern int *g_chihiro_or2_map[3];
extern int *g_chihiro_wmmt2_map[7];
extern int *g_chihiro_ok_map[6];
extern int *g_chihiro_gundam_map[14];
extern int *g_chihiro_p2_universal_map[2];
extern int *g_chihiro_p2_hotd3_map[2];
extern int *g_chihiro_p2_vc3_map[4];
extern int *g_chihiro_p2_gs_map[5];

void xemu_input_init(void);
void xemu_input_process_sdl_events(const SDL_Event *event); // SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED
void xemu_input_update_controllers(void);
void xemu_input_update_controller(ControllerState *state);
void xemu_input_update_sdl_kbd_controller_state(ControllerState *state);
void xemu_input_update_light_gun(ControllerState *state); /* LIGHTGUN (not upstream) */
void xemu_input_update_sdl_mouse_controller_state(ControllerState *state);
void xemu_input_update_sdl_controller_state(ControllerState *state);
void xemu_input_update_rumble(ControllerState *state);
ControllerState *xemu_input_get_bound(int index);
void xemu_input_bind(int index, ControllerState *state, int save);
bool xemu_input_bind_xmu(int player_index, int peripheral_port_index,
                         const char *filename, bool is_rebind);
void xemu_input_rebind_xmu(int port);
void xemu_input_unbind_xmu(int player_index, int peripheral_port_index);
int xemu_input_get_controller_default_bind_port(ControllerState *state, 
                                                int start);
void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter);

void xemu_input_set_test_mode(int enabled);
int xemu_input_get_test_mode(void);
void xemu_input_reset_input_mapping(ControllerState *state);
int xemu_input_lightgun_active(void);
/* Buttons of the player's pointer device (0 when it aims otherwise). */
uint32_t xemu_input_pointer_device_buttons(int player);

#ifdef __cplusplus
}
#endif

#endif
