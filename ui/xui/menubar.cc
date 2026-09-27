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
#include "ui/xemu-notifications.h"
#include "common.hh"
#include "main-menu.hh"
#include "menubar.hh"
#include "misc.hh"
#include "widgets.hh"
#include "monitor.hh"
#include "debug.hh"
#include "actions.hh"
#include "compat.hh"
#include "update.hh"
#include "../xemu-os-utils.h"

extern float g_main_menu_height; // FIXME

#ifdef CONFIG_RENDERDOC
bool g_capture_renderdoc_frame = false;
#endif

void ProcessKeyboardShortcuts(void)
{
    if (HotkeyPressed(g_config.input.hotkeys.eject_disc)) {
        ActionEjectDisc();
    }

    if (HotkeyPressed(g_config.input.hotkeys.load_disc)) {
        ActionLoadDisc();
    }

    if (HotkeyPressed(g_config.input.hotkeys.pause)) {
        ActionTogglePause();
    }

    if (HotkeyPressed(g_config.input.hotkeys.reset)) {
        ActionReset();
    }

    if (HotkeyPressed(g_config.input.hotkeys.quit)) {
        ActionShutdown();
    }

    /* Its own key also closes the monitor from its command line. */
    if (HotkeyPressed(g_config.input.hotkeys.monitor, true)) {
        monitor_window.ToggleOpen();
    }

    if (HotkeyPressed(g_config.input.hotkeys.screenshot)) {
        ActionScreenshot();
    }

    if (HotkeyPressed(g_config.input.hotkeys.fullscreen)) {
        xemu_toggle_fullscreen();
    }

#ifdef CONFIG_RENDERDOC
    if (ImGui::IsKeyPressed(ImGuiKey_F10) && nv2a_dbg_renderdoc_available()) {
        ImGuiIO& io = ImGui::GetIO();
        int num_frames = io.KeyShift ? 5 : 1;
        nv2a_dbg_renderdoc_capture_frames(num_frames, io.KeyCtrl);
    }
#endif
}

void ShowMainMenu()
{
    bool running = runstate_is_running();

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("Machine"))
        {
            std::string pause_key = HotkeyName(g_config.input.hotkeys.pause);
            if (ImGui::MenuItem(running ? "Pause" : "Resume",
                                pause_key.c_str())) {
                ActionTogglePause();
            }
            std::string screenshot_key =
                HotkeyName(g_config.input.hotkeys.screenshot);
            if (ImGui::MenuItem("Screenshot", screenshot_key.c_str())) {
                ActionScreenshot();
            }

            if (ImGui::BeginMenu("Snapshot")) {
                if (ImGui::MenuItem("Create Snapshot")) {
                    xemu_snapshots_save(NULL, NULL);
                    xemu_queue_notification("Created new snapshot");
                }

                int64_t dates[4];
                QuickSlotDates(dates);
                for (int i = 0; i < 4; ++i) {
                    std::string slot = std::to_string(i + 1);
                    std::string save_key = HotkeyName(QuickSaveHotkey(i));
                    std::string load_key = HotkeyName(QuickLoadHotkey(i));

                    /* When the slot was saved, or that it is empty. */
                    std::string load_name = "Quick Load " + slot + " (empty)";
                    if (dates[i]) {
                        g_autoptr(GDateTime) date =
                            g_date_time_new_from_unix_local(dates[i]);
                        g_autofree char *when =
                            date ? g_date_time_format(date, "%Y-%m-%d %H:%M")
                                 : NULL;
                        load_name = "Quick Load " + slot + " (" +
                                    (when ? when : "saved") + ")";
                    }

                    ImGui::Separator();

                    if (ImGui::MenuItem(("Quick Save " + slot).c_str(),
                                        save_key.c_str())) {
                        ActionQuickSave(i);
                    }

                    if (ImGui::MenuItem(load_name.c_str(), load_key.c_str(),
                                        false, dates[i] != 0)) {
                        ActionQuickLoad(i);
                    }
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            std::string eject_key =
                HotkeyName(g_config.input.hotkeys.eject_disc);
            std::string load_disc_key =
                HotkeyName(g_config.input.hotkeys.load_disc);
            if (ImGui::MenuItem("Eject Disc", eject_key.c_str())) {
                ActionEjectDisc();
            }
            if (ImGui::MenuItem("Load Disc...", load_disc_key.c_str())) {
                ActionLoadDisc();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Settings...")) g_main_menu.ShowSettings();

            ImGui::Separator();

            std::string reset_key = HotkeyName(g_config.input.hotkeys.reset);
            std::string quit_key = HotkeyName(g_config.input.hotkeys.quit);
            if (ImGui::MenuItem("Reset", reset_key.c_str())) {
                ActionReset();
            }
            if (ImGui::MenuItem("Exit", quit_key.c_str())) {
                ActionShutdown();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            int ui_scale_idx;
            if (g_config.display.ui.auto_scale) {
                ui_scale_idx = 0;
            } else {
                ui_scale_idx = g_config.display.ui.scale;
                if (ui_scale_idx < 0) ui_scale_idx = 0;
                else if (ui_scale_idx > 2) ui_scale_idx = 2;
            }
            if (ImGui::Combo("UI Scale", &ui_scale_idx,
                             "Auto\0"
                             "1x\0"
                             "2x\0")) {
                if (ui_scale_idx == 0) {
                    g_config.display.ui.auto_scale = true;
                } else {
                    g_config.display.ui.auto_scale = false;
                    g_config.display.ui.scale = ui_scale_idx;
                }
            }

            ImGui::Combo("Backend", &g_config.display.renderer,
                 "Null\0"
                 "OpenGL\0"
#ifdef CONFIG_VULKAN
                 "Vulkan\0"
#endif
                );

            int rendering_scale = nv2a_get_surface_scale_factor() - 1;
            if (ImGui::Combo("Int. Resolution Scale", &rendering_scale,
                             "1x\0"
                             "2x\0"
                             "3x\0"
                             "4x\0"
                             "5x\0"
                             "6x\0"
                             "7x\0"
                             "8x\0"
                             "9x\0"
                             "10x\0")) {
                nv2a_set_surface_scale_factor(rendering_scale + 1);
            }

            ImGui::Combo("Display Mode", &g_config.display.ui.fit,
                         "Center\0Scale\0Stretch\0");
            ImGui::SameLine();
            HelpMarker("Controls how the rendered content should be scaled "
                       "into the window");
            ImGui::Combo("Filter Method", &g_config.display.filtering,
                         "Linear\0Nearest\0");
            ImGui::Combo("Aspect Ratio", &g_config.display.ui.aspect_ratio,
                         "Native\0Auto\0""4:3\0""16:9\0");
            std::string fullscreen_key =
                HotkeyName(g_config.input.hotkeys.fullscreen);
            if (ImGui::MenuItem("Fullscreen", fullscreen_key.c_str(),
                                xemu_is_fullscreen(), true)) {
                xemu_toggle_fullscreen();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            std::string monitor_key =
                HotkeyName(g_config.input.hotkeys.monitor);
            ImGui::MenuItem("Monitor", monitor_key.c_str(),
                            &monitor_window.is_open);
            ImGui::MenuItem("Audio", NULL, &apu_window.m_is_open);
            ImGui::MenuItem("Video", NULL, &video_window.m_is_open);
#ifdef CONFIG_RENDERDOC
            if (nv2a_dbg_renderdoc_available()) {
                ImGui::MenuItem("RenderDoc: Capture", NULL, &g_capture_renderdoc_frame);
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Help", NULL)) {
                SDL_OpenURL("https://xemu.app/docs/getting-started/");
            }

            /* Commented out: it would send this build's reports to
             * upstream xemu's server.
            ImGui::MenuItem("Report Compatibility...", NULL,
                            &compatibility_reporter_window.is_open);
             */
            /* Commented out: xemu's updater would install upstream xemu
             * over this build.
#if defined(_WIN32)
            ImGui::MenuItem("Check for Updates...", NULL, &update_window.is_open);
#endif
             */

            ImGui::Separator();
            if (ImGui::MenuItem("About")) g_main_menu.ShowAbout();
            ImGui::EndMenu();
        }

        g_main_menu_height = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
    }
}
