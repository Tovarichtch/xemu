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

void ActionEjectDisc();
void ActionLoadDisc();
void ActionLoadDiscFile(const char *file_path);
void ActionTogglePause();
void ActionReset();
void ActionShutdown();
void ActionScreenshot();
void ActionCreateSnapshot();
void ActionQuickSave(int slot);
void ActionQuickLoad(int slot);
/* When quick slots 0 to 3 of the game running were saved, in seconds since
 * 1970; 0 when empty. */
void QuickSlotDates(int64_t dates[4]);
void ActionLoadSnapshotChecked(const char *name);
