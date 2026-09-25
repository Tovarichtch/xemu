/*
 * xemu session log
 *
 * Copyright (c) 2026 Réda Chérif-Touil
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

#ifndef XEMU_LOG_H
#define XEMU_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the process prints goes to logs/<image>-<date>.log in the xemu
 * data folder: a file per session, the last XEMU_LOG_KEEP kept, for a player
 * to attach to a bug report. On Linux and macOS the lines still reach the
 * stderr the process started with (stdout too when it shared stderr's
 * destination; a stdout sent elsewhere is left alone). On Windows both
 * streams go to the file only. Called once, as soon as the settings are
 * loaded; runs that only print (-version, help listings) get no file. */
void xemu_log_start(int argc, char **argv);

/* At the exit: the terminal gets the last lines. The copying thread does
 * the writing, waited for 200 ms at most, so a stopped terminal cannot hold
 * up the exit. Registered with atexit; main() calls it before its _exit. */
void xemu_log_flush(void);

/* The logs folder as SDL_OpenURL wants it (a file:// URI, the plain path on
 * Windows), and this session's file name; NULL without a session log. */
const char *xemu_log_dir_link(void);
const char *xemu_log_file_name(void);

#ifdef __cplusplus
}
#endif

#endif
