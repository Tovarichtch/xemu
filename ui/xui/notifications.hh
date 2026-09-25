//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once
#include <stdint.h>
#include <deque>

#include "../xemu-notifications.h"

class NotificationManager
{
private:
    struct Notification {
        const char *msg;
        bool warning;  // drawn in the warning colour, not the theme's
    };
    std::deque<Notification> m_notification_queue;
    std::deque<const char *> m_error_queue;

    const int kNotificationDuration = 4000;
    uint32_t m_notification_end_time;
    Notification m_current;
    bool m_active;

public:
    NotificationManager();
    void QueueNotification(const char *msg, bool warning = false);
    void QueueError(const char *msg);
    void Draw();

private:
    void DrawNotification(float t, const Notification &n);
};

extern NotificationManager notification_manager;
