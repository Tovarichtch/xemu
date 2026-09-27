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
#include "common.hh"
#include "actions.hh"
#include "misc.hh"
#include "xemu-hud.h"
#include "../xemu-snapshots.h"
#include "../xemu-notifications.h"
#include "snapshot-manager.hh"
#include <filesystem>

void ActionEjectDisc(void)
{
    /* Chihiro has no tray: ejecting would only wipe the configured image. */
    if (xemu_chihiro_mode()) {
        return;
    }

    Error *err = NULL;
    xemu_eject_disc(&err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}

void ActionLoadDisc(void)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disc Image Files (*.iso, *.xiso)", "iso;xiso" },
        { "Chihiro Netboot Image (*.bin)", "bin" },
        { "All Files", "*" }
    };
    const char *default_path = g_config.sys.files.dvd_path;
    if (!default_path || !default_path[0]) {
        default_path = g_config.general.games_dir;
    }
    ShowOpenFileDialog(filters, 3, default_path, [](const char *path) {
        ActionLoadDiscFile(path);
    });
}

static void remember_games_dir(const char *file_path)
{
    const char *games_dir = g_config.general.games_dir;
    if (!games_dir || !games_dir[0]) {
        std::string dir = std::filesystem::path(file_path).parent_path().string();
        xemu_settings_set_string(&g_config.general.games_dir, dir.c_str());
    }
}

void ActionLoadDiscFile(const char *file_path)
{
    Error *err = NULL;

    /* The machine follows the image: a Chihiro image is mapped into the
     * baseboard when the machine is created, and an Xbox disc needs the
     * Xbox, so either takes effect on the next launch unless this Xbox is
     * already running and the file is a disc. */
    if (xemu_chihiro_mode() || !xemu_media_is_xbox_disc(file_path)) {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, file_path);
        remember_games_dir(file_path);
        xemu_queue_notification("Image selected. Restart xemu to boot it.");
        return;
    }

    xemu_load_disc(file_path, &err);

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    } else {
        remember_games_dir(file_path);
    }
}

void ActionTogglePause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    } else {
        vm_start();
    }
}

void ActionReset(void)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

void ActionShutdown(void)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

void ActionScreenshot(void)
{
	g_screenshot_pending = true;
}

/* The game running as the snapshots record it: the netboot image on a
 * Chihiro, the disc on an Xbox. Its quick slots are named after it. */
static std::string QuickSlotGame(void)
{
    char *path = xemu_chihiro_mode() ? g_strdup(xemu_chihiro_image())
                                     : xemu_get_currently_loaded_disc_path();
    std::string game = "Xbox";
    if (path && path[0]) {
        char *base = g_path_get_basename(path);
        char *dot = strrchr(base, '.');
        if (dot && dot != base) {
            *dot = '\0';
        }
        game = base;
        g_free(base);
    }
    g_free(path);
    return game;
}

/* Quick slot 0 to 3 of a game: one snapshot per game and slot, "vcop3 slot
 * 1". */
static std::string QuickSlotName(const std::string &game, int slot)
{
    assert(slot < 4 && slot >= 0);
    return game + " slot " + std::to_string(slot + 1);
}

/* When the snapshot was taken, or 0 if there is none by that name. */
static int64_t SnapshotDate(const std::string &name)
{
    g_snapshot_mgr.Refresh();
    for (int i = 0; i < g_snapshot_mgr.m_snapshots_len; i++) {
        if (name == g_snapshot_mgr.m_snapshots[i].name) {
            return g_snapshot_mgr.m_snapshots[i].date_sec;
        }
    }
    return 0;
}

void QuickSlotDates(int64_t dates[4])
{
    std::string game = QuickSlotGame();
    for (int slot = 0; slot < 4; slot++) {
        dates[slot] = SnapshotDate(QuickSlotName(game, slot));
    }
}

void ActionQuickSave(int slot)
{
    std::string name = QuickSlotName(QuickSlotGame(), slot);
    Error *err = NULL;
    xemu_snapshots_save(name.c_str(), &err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
        return;
    }
    char *msg = g_strdup_printf("Saved to slot %d", slot + 1);
    xemu_queue_notification(msg);
    g_free(msg);
}

void ActionQuickLoad(int slot)
{
    std::string name = QuickSlotName(QuickSlotGame(), slot);
    if (SnapshotDate(name)) {
        ActionLoadSnapshotChecked(name.c_str());
        return;
    }
    std::string save_key = HotkeyName(QuickSaveHotkey(slot));
    char *msg = save_key.empty() ?
        g_strdup_printf("Slot %d is empty", slot + 1) :
        g_strdup_printf("Slot %d is empty (%s saves)", slot + 1,
                        save_key.c_str());
    xemu_queue_notification(msg);
    g_free(msg);
}

void ActionLoadSnapshotChecked(const char *name)
{
    g_snapshot_mgr.LoadSnapshotChecked(name);
}
