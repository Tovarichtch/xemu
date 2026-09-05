/*
 * xemu Controller Binding Management
 *
 * Copyright (C) 2025 Matt Borgerson
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

#include "xemu-controllers.h"
#include "xemu-settings.h"
#include <assert.h>
#include <cmath>
#include <limits>

constexpr int controller_button_count = 15;
constexpr int controller_axes_count = 6;

RebindEventResult
ControllerKeyboardRebindingMap::ConsumeRebindEvent(SDL_Event *event)
{
    // Bind on key up
    // This ensures the UI does not immediately respond once the new binding is
    // applied
    if (event->type == SDL_EVENT_KEY_UP) {
        *(g_keyboard_scancode_map[m_table_row]) = event->key.scancode;
        return RebindEventResult::Complete;
    }

    return RebindEventResult::Ignore;
}

RebindEventResult ControllerGamepadRebindingMap::HandleButtonEvent(
    SDL_GamepadButtonEvent *event)
{
    if (m_state->sdl_joystick_id != event->which) {
        return RebindEventResult::Ignore;
    }

    int *button_map[controller_button_count] = {
        &m_state->controller_map->controller_mapping.a,
        &m_state->controller_map->controller_mapping.b,
        &m_state->controller_map->controller_mapping.x,
        &m_state->controller_map->controller_mapping.y,
        &m_state->controller_map->controller_mapping.back,
        &m_state->controller_map->controller_mapping.guide,
        &m_state->controller_map->controller_mapping.start,
        &m_state->controller_map->controller_mapping.lstick_btn,
        &m_state->controller_map->controller_mapping.rstick_btn,
        &m_state->controller_map->controller_mapping.lshoulder,
        &m_state->controller_map->controller_mapping.rshoulder,
        &m_state->controller_map->controller_mapping.dpad_up,
        &m_state->controller_map->controller_mapping.dpad_down,
        &m_state->controller_map->controller_mapping.dpad_left,
        &m_state->controller_map->controller_mapping.dpad_right,
    };

    // FIXME: Allow face buttons to map to axes
    if (m_table_row >= controller_button_count) {
        return RebindEventResult::Ignore;
    }

    // If we only track up events, then we might rebind to a button
    // that was already held down when the rebinding event began
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        m_seen_key_down = true;
        return RebindEventResult::Ignore;
    }

    // Bind on controller button up
    // This ensures the UI does not immediately respond once the new binding is
    // applied
    if (event->type != SDL_EVENT_GAMEPAD_BUTTON_UP || !m_seen_key_down) {
        return RebindEventResult::Ignore;
    }

    *(button_map[m_table_row]) = event->button;

    return RebindEventResult::Complete;
}

RebindEventResult
ControllerGamepadRebindingMap::HandleAxisEvent(SDL_GamepadAxisEvent *event)
{
    if (m_state->sdl_joystick_id != event->which) {
        return RebindEventResult::Ignore;
    }

    // Axis inputs cannot be bound to controller buttons
    if (m_table_row < controller_button_count) {
        return RebindEventResult::Ignore;
    }

    // Requre that the input be sufficiently outside of any deadzone range
    // before using it for rebinding
    if (std::abs(event->value >> 1) <=
        (std::numeric_limits<Sint16>::max() >> 2)) {
        return RebindEventResult::Ignore;
    }

    int *axis_map[controller_axes_count] = {
        &m_state->controller_map->controller_mapping.axis_left_x,
        &m_state->controller_map->controller_mapping.axis_left_y,
        &m_state->controller_map->controller_mapping.axis_right_x,
        &m_state->controller_map->controller_mapping.axis_right_y,
        &m_state->controller_map->controller_mapping.axis_trigger_left,
        &m_state->controller_map->controller_mapping.axis_trigger_right,
    };

    *(axis_map[m_table_row - controller_button_count]) = event->axis;

    return RebindEventResult::Complete;
}

ChihiroRebindingMap::ChihiroRebindingMap(int table_row, int *scancode,
                                         int player)
    : RebindingMap(table_row), m_scancode(scancode), m_joy_id(0),
      m_joy_num_axes(0), m_player(player), m_pointer_seen(0)
{
    // Snapshot the bound wheel's axes at rest, so a pedal that idles at an
    // extreme can still be captured by movement (see the header note).
    ControllerState *pad = bound_controllers[0];
    if (pad && pad->type == INPUT_DEVICE_SDL_JOYSTICK && pad->sdl_joystick) {
        m_joy_id = pad->sdl_joystick_id;
        int n = SDL_GetNumJoystickAxes(pad->sdl_joystick);
        int cap = (int)(sizeof(m_joy_baseline) / sizeof(m_joy_baseline[0]));
        m_joy_num_axes = n < cap ? n : cap;
        for (int i = 0; i < m_joy_num_axes; i++)
            m_joy_baseline[i] = SDL_GetJoystickAxis(pad->sdl_joystick, i);
    }
}

RebindEventResult
ChihiroRebindingMap::ConsumeRebindEvent(SDL_Event *event)
{
    if (event->type == SDL_EVENT_KEY_UP) {
        *m_scancode = event->key.scancode;
        return RebindEventResult::Complete;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        *m_scancode = CHIHIRO_MOUSE_BUTTON_BASE - 1 + event->button.button;
        return RebindEventResult::Complete;
    }
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        *m_scancode = CHIHIRO_GAMEPAD_BUTTON_BASE + event->gbutton.button;
        return RebindEventResult::Complete;
    }
    /* Well past rest, so a resting stick cannot bind itself. */
    if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
        abs(event->gaxis.value) > 24000) {
        *m_scancode = CHIHIRO_AXIS_BINDING(event->gaxis.axis,
                                           event->gaxis.value > 0);
        return RebindEventResult::Complete;
    }
    /* Raw wheel button. */
    if (event->type == SDL_EVENT_JOYSTICK_BUTTON_UP && m_joy_id &&
        event->jbutton.which == m_joy_id) {
        *m_scancode = CHIHIRO_JOY_BUTTON_BINDING(event->jbutton.button);
        return RebindEventResult::Complete;
    }
    /* Raw wheel axis: capture by movement from the rest snapshot, then classify
     * centre-rest (steering half-axis) vs extreme-rest (pedal, keeping the
     * pressed direction). Works for any wheel regardless of pedal wiring. */
    if (event->type == SDL_EVENT_JOYSTICK_AXIS_MOTION && m_joy_id &&
        event->jaxis.which == m_joy_id && event->jaxis.axis < m_joy_num_axes) {
        int axis = event->jaxis.axis;
        int delta = (int)event->jaxis.value - (int)m_joy_baseline[axis];
        if (abs(delta) > 12000) {
            bool moved_positive = delta > 0;
            if (abs(m_joy_baseline[axis]) < 8000) {
                *m_scancode = CHIHIRO_JOY_HALFAXIS_BINDING(axis, moved_positive);
            } else {
                *m_scancode = CHIHIRO_JOY_PEDAL_BINDING(axis, moved_positive);
            }
            return RebindEventResult::Complete;
        }
    }
    return RebindEventResult::Ignore;
}

/* A button of the player's pointer device, captured on release. */
bool ChihiroRebindingMap::PollPointer()
{
    uint32_t now = xemu_input_pointer_device_buttons(m_player);
    uint32_t released = m_pointer_seen & ~now;
    m_pointer_seen |= now;
    if (released) {
        *m_scancode = CHIHIRO_MOUSE_BUTTON_BASE + __builtin_ctz(released);
        return true;
    }
    return false;
}

RebindEventResult
ControllerGamepadRebindingMap::ConsumeRebindEvent(SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_REMOVED:
        return (m_state->sdl_joystick_id == event->gdevice.which) ?
                   RebindEventResult::Complete :
                   RebindEventResult::Ignore;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return HandleButtonEvent(&event->gbutton);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return HandleAxisEvent(&event->gaxis);
    default:
        return RebindEventResult::Ignore;
    }
}
