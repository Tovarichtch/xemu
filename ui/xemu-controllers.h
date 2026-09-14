/*
 * xemu Settings Management
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

#ifndef XEMU_CONTROLLERS_H
#define XEMU_CONTROLLERS_H

#include "xemu-input.h"
#include <SDL3/SDL.h>

enum class RebindEventResult {
    Ignore,
    Complete,
};

struct RebindingMap {
protected:
    int m_table_row;
    RebindingMap(int table_row) : m_table_row{ table_row }
    {
    }

public:
    virtual RebindEventResult ConsumeRebindEvent(SDL_Event *event) = 0;

    int GetTableRow() const
    {
        return m_table_row;
    }

    virtual ~RebindingMap() = default;
};

struct ControllerKeyboardRebindingMap : public virtual RebindingMap {
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;

    ControllerKeyboardRebindingMap(int table_row) : RebindingMap(table_row)
    {
    }
};

class ControllerGamepadRebindingMap : public virtual RebindingMap {
    ControllerState *m_state;
    bool m_seen_key_down;

    RebindEventResult HandleButtonEvent(SDL_GamepadButtonEvent *event);
    RebindEventResult HandleAxisEvent(SDL_GamepadAxisEvent *event);

public:
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;
    ControllerGamepadRebindingMap(int table_row, ControllerState *state)
        : RebindingMap(table_row), m_state{ state }, m_seen_key_down{ false }
    {
    }
};

struct ChihiroRebindingMap : public virtual RebindingMap {
    /* The binding itself: the row number is not its index in its group. */
    int *m_scancode;

    /* Raw-joystick capture: a wheel pedal rests at an extreme, so axes are
     * captured by MOVEMENT from a rest snapshot (taken here) rather than by
     * an absolute threshold, and the moved axis is classified centre-rest
     * (steering) vs extreme-rest (pedal). Every raw joystick bound to a
     * port is listened to; the binding records the port. */
    SDL_JoystickID m_joy_id[4];       /* per port, 0 = no raw joystick */
    int            m_joy_num_axes[4];
    Sint16         m_joy_baseline[4][24];

    /* The player whose row is bound: its pointer device has no SDL events,
     * so its buttons are polled (PollPointer) while the row waits. */
    int      m_player;
    uint32_t m_pointer_seen;

    ChihiroRebindingMap(int table_row, int *scancode, int player);
    RebindEventResult ConsumeRebindEvent(SDL_Event *event) override;
    bool PollPointer();
};

#endif // XEMU_CONTROLLERS_H
