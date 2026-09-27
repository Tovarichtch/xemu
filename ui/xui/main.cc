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

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <functional>
#include <assert.h>
#include <fpng.h>

#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "actions.hh"
#include "common.hh"
#include "xemu-hud.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "scene.hh"
#include "scene-manager.hh"
#include "main-menu.hh"
#include "popup-menu.hh"
#include "notifications.hh"
#include "monitor.hh"
#include "debug.hh"
#include "welcome.hh"
#include "../xemu-input.h"
#include "hw/xbox/chihiro/chihiro-jvs.h"
#include "menubar.hh"
#include "compat.hh"
#if defined(_WIN32)
#include "update.hh"
#endif
#include <stb_image.h>

bool g_screenshot_pending;
const char *g_snapshot_pending_load_name;

float g_main_menu_height;

extern "C" {
extern int viewport_coords[4];
/* chihiro.h itself is not C++-safe (it pulls in block-backend.h). */
int chihiro_detected_game_profile(void);
}

static ImGuiStyle g_base_style;
static float g_last_scale;
static int g_vsync;
static GLuint g_tex;
static bool g_flip_req;

/* One player's crosshair image (chihiro.jvs*.crosshair_path). */
struct Crosshair {
    GLuint tex = 0;
    int w = 0, h = 0;
    std::string path;
};
static Crosshair g_crosshair[2];

static void LoadCrosshair(Crosshair &c, const char *want_c)
{
    std::string want = want_c ? want_c : "";
    if (c.path == want) return;

    if (c.tex) {
        glDeleteTextures(1, &c.tex);
        c.tex = 0;
    }
    c.path = want;
    if (want.empty()) return;

    int w, h, ch;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *data = stbi_load(want.c_str(), &w, &h, &ch, 4);
    if (!data) return;

    glGenTextures(1, &c.tex);
    glBindTexture(GL_TEXTURE_2D, c.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, data);
    c.w = w;
    c.h = h;
    stbi_image_free(data);
}

static void RenderSindenBorder(float vx, float vy, float vw, float vh,
                               int winW, int winH)
{
    if (!g_config.chihiro.settings.sinden_border)
        return;

    float t = (float)g_config.chihiro.settings.sinden_border_size;
    if (t < 2) t = 2;
    if (t > 30) t = 30;

    float bx, by, bw, bh;
    if (g_config.chihiro.settings.sinden_border_style ==
        CONFIG_CHIHIRO_SETTINGS_SINDEN_BORDER_STYLE_FULLSCREEN) {
        bx = 0; by = 0; bw = (float)winW; bh = (float)winH;
    } else {
        bx = vx; by = vy; bw = vw; bh = vh;
    }

    ImU32 white = IM_COL32(255, 255, 255, 255);
    auto dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + t), white);
    dl->AddRectFilled(ImVec2(bx, by + bh - t), ImVec2(bx + bw, by + bh), white);
    dl->AddRectFilled(ImVec2(bx, by + t), ImVec2(bx + t, by + bh - t), white);
    dl->AddRectFilled(ImVec2(bx + bw - t, by + t), ImVec2(bx + bw, by + bh - t), white);
}

/* Keys pressed since the last frame (key down, not a repeat), by scancode:
 * the emulator's shortcuts are read from it. */
static bool hotkeys_down[SDL_SCANCODE_COUNT];
static int hotkeys_mods[SDL_SCANCODE_COUNT];

bool HotkeyPressed(int hotkey, bool while_typing)
{
    int sc = HOTKEY_SCANCODE(hotkey);
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT || !hotkeys_down[sc] ||
        hotkeys_mods[sc] != (hotkey & ~0xFFFF)) {
        return false;
    }
    /* A text field in use keeps the keys that type: only F-keys and Ctrl or
     * Alt combos are shortcuts there. */
    bool fkey = (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) ||
                (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24);
    if (!while_typing && ImGui::GetIO().WantTextInput && !fkey &&
        !(hotkey & (HOTKEY_CTRL | HOTKEY_ALT))) {
        return false;
    }
    /* A single key bound to a cabinet input is the game's while a Chihiro
     * runs; a combo is the user's choice. */
    return hotkey != sc ||
           !(xemu_chihiro_mode() && xemu_input_chihiro_key_bound(sc));
}

std::string HotkeyName(int hotkey)
{
    int sc = HOTKEY_SCANCODE(hotkey);
    const char *key = sc > 0 && sc < SDL_SCANCODE_COUNT ?
        SDL_GetScancodeName((SDL_Scancode)sc) : NULL;
    if (!key || !key[0]) {
        return "";
    }
    std::string name;
    if (hotkey & HOTKEY_CTRL) {
#ifdef __APPLE__
        name += "Cmd+";
#else
        name += "Ctrl+";
#endif
    }
    if (hotkey & HOTKEY_ALT) {
        name += "Alt+";
    }
    if (hotkey & HOTKEY_SHIFT) {
        name += "Shift+";
    }
    return name + key;
}

int QuickSaveHotkey(int slot)
{
    const int keys[4] = {
        g_config.input.hotkeys.quick_save1, g_config.input.hotkeys.quick_save2,
        g_config.input.hotkeys.quick_save3, g_config.input.hotkeys.quick_save4,
    };
    return keys[slot];
}

int QuickLoadHotkey(int slot)
{
    const int keys[4] = {
        g_config.input.hotkeys.quick_load1, g_config.input.hotkeys.quick_load2,
        g_config.input.hotkeys.quick_load3, g_config.input.hotkeys.quick_load4,
    };
    return keys[slot];
}

/* No crosshair, border or hidden pointer outside a Chihiro gun game. */
static bool ChihiroGunGame(void)
{
    if (!xemu_chihiro_mode())
        return false;

    int profile = chihiro_detected_game_profile();
    if (profile < 0)
        profile = g_config.chihiro.jvs.profile;

    return profile == CONFIG_CHIHIRO_JVS_PROFILE_HOTD3 ||
           profile == CONFIG_CHIHIRO_JVS_PROFILE_VC3 ||
           profile == CONFIG_CHIHIRO_JVS_PROFILE_GS;
}

static void RenderLightGunOverlays(void)
{
    bool gun_game = ChihiroGunGame();

    /* Gun mode hides the cursor; the menus keep it. Asked through ImGui, as
     * "Hide mouse cursor" does, since a direct SDL call would undo it. */
    if (gun_game && g_config.chihiro.settings.lightgun_mode &&
        !g_scene_mgr.IsDisplayingScene())
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    if (!gun_game)
        return;

    if (viewport_coords[2] <= 0 || viewport_coords[3] <= 0)
        return;

    int drawW, drawH, winW, winH;
    SDL_GetWindowSizeInPixels(xemu_get_window(), &drawW, &drawH);
    SDL_GetWindowSize(xemu_get_window(), &winW, &winH);
    float sx = (float)winW / (float)drawW;
    float sy = (float)winH / (float)drawH;

    float vx = viewport_coords[0] * sx;
    float vy = viewport_coords[1] * sy;
    float vw = viewport_coords[2] * sx;
    float vh = viewport_coords[3] * sy;

    RenderSindenBorder(vx, vy, vw, vh, winW, winH);

    if (!chihiro_jvs_global)
        return;

    /* Each player's crosshair follows the aim the game receives; 0,0 means
     * off the picture, so nothing is drawn. */
    for (int p = 0; p < 2; p++) {
        Crosshair &c = g_crosshair[p];
        LoadCrosshair(c, p ? g_config.chihiro.jvs_p2.crosshair_path
                           : g_config.chihiro.jvs.crosshair_path);
        if (!c.tex) continue;
        uint16_t jx = chihiro_jvs_global->analog[p * 2];
        uint16_t jy = chihiro_jvs_global->analog[p * 2 + 1];
        if (!jx && !jy) continue;

        int pct = p ? g_config.chihiro.jvs_p2.crosshair_scale
                    : g_config.chihiro.jvs.crosshair_scale;
        float half_w = c.w * pct / 200.0f;
        float half_h = c.h * pct / 200.0f;
        float cx = vx + jx / 65535.0f * vw;
        float cy = vy + jy / 65535.0f * vh;
        ImGui::GetForegroundDrawList()->AddImage(
            (ImTextureID)(intptr_t)c.tex,
            ImVec2(cx - half_w, cy - half_h),
            ImVec2(cx + half_w, cy + half_h));
    }
}

static void InitializeStyle()
{
    g_font_mgr.Rebuild();

    ImGui::StyleColorsDark();
    ImVec4 *c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.98f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 0.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.13f, 0.87f, 0.16f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.25f, 0.75f, 0.10f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.83f, 0.49f, 0.04f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.26f, 0.67f, 0.23f, 0.95f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.21f, 0.54f, 0.19f, 0.99f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.24f, 0.60f, 0.21f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.16f, 0.16f, 0.16f, 0.73f);

    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0;
    s.FrameRounding = 6.0;
    s.PopupRounding = 6.0;
    g_base_style = s;
}

void xemu_hud_init(SDL_Window* window, void* sdl_gl_context)
{
    xemu_monitor_init();
    g_vsync = g_config.display.window.vsync;

    InitCustomRendering();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    // Setup Platform/Renderer bindings
    ImGui_ImplSDL3_InitForOpenGL(window, sdl_gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");
    ImPlot::CreateContext();

    /* No update check at start: xemu's updater would install upstream
     * xemu over this build. */
    g_last_scale = g_viewport_mgr.m_scale;
    InitializeStyle();
    g_main_menu.SetNextViewIndex(g_config.general.last_viewed_menu_index);
    first_boot_window.is_open = g_config.general.show_welcome;
}

void xemu_hud_cleanup(void)
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void xemu_hud_process_sdl_events(SDL_Event *event)
{
    // Ignore inputs that are consumed by rebinding
    if (g_main_menu.ConsumeRebindEvent(event)) {
        return;
    }

    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
        event->key.scancode < SDL_SCANCODE_COUNT) {
        hotkeys_down[event->key.scancode] = true;
        hotkeys_mods[event->key.scancode] = hotkey_mods(event->key.mod);
    }

    ImGui_ImplSDL3_ProcessEvent(event);
}

void xemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    if (kbd) *kbd = io.WantCaptureKeyboard;
    if (mouse) *mouse = io.WantCaptureMouse;
}

void xemu_hud_set_framebuffer_texture(GLuint tex, bool flip)
{
    g_tex = tex;
    g_flip_req = flip;
}

/* UPSTREAM CANDIDATE: the guest framebuffer is laid out under the main-loop
 * lock (xemu_hud_update) and drawn after it is released: a driver that blocks
 * on the draw until the display is ready (full-screen Xwayland) must not hold
 * the machine. */
void xemu_hud_render_framebuffer(void)
{
    if (!first_boot_window.is_open) {
        RenderFramebuffer(g_tex, g_flip_req);
    }
}

void xemu_hud_update(void)
{
    ImGuiIO& io = ImGui::GetIO();
    uint32_t now = SDL_GetTicks();

    g_viewport_mgr.Update();
    g_font_mgr.Update();
    if (g_last_scale != g_viewport_mgr.m_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(g_viewport_mgr.m_scale);
        g_last_scale = g_viewport_mgr.m_scale;
    }

    if (!first_boot_window.is_open) {
        int ww, wh;
        SDL_GetWindowSizeInPixels(xemu_get_window(), &ww, &wh);
        LayoutFramebuffer(g_tex, ww, wh);
    }

    ImGui_ImplOpenGL3_NewFrame();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_NewFrame();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_input_mgr.Update();

    ImGui::NewFrame();
    RenderLightGunOverlays();
    ProcessKeyboardShortcuts();

#if defined(CONFIG_RENDERDOC)
    if (g_capture_renderdoc_frame) {
        nv2a_dbg_renderdoc_capture_frames(1, false);
        g_capture_renderdoc_frame = false;
    }
#endif

    if (g_config.display.ui.show_menubar && !first_boot_window.is_open) {
        // Auto-hide main menu after 5s of inactivity
        static uint32_t last_check = 0;
        float alpha = 1.0;
        const uint32_t timeout = 5000;
        const float fade_duration = 1000.0;
        bool menu_wakeup = g_input_mgr.MouseMoved() &&
                           !(ChihiroGunGame() &&
                             g_config.chihiro.settings.lightgun_mode);
        if (menu_wakeup) {
            last_check = now;
        }
        if ((now-last_check) > timeout) {
            if (g_config.display.ui.use_animations) {
                float t = fmin((float)((now-last_check)-timeout)/fade_duration, 1);
                alpha = 1-t;
                if (t >= 1) {
                    alpha = 0;
                }
            } else {
                alpha = 0;
            }
        }
        if (alpha > 0.0) {
            ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Text];
            tc.w = alpha;
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
            ImGui::SetNextWindowBgAlpha(alpha);
            ShowMainMenu();
            ImGui::PopStyleColor();
        } else {
            g_main_menu_height = 0;
        }
    }

    static uint32_t last_mouse_move = 0;
    if (g_input_mgr.MouseMoved()) {
        last_mouse_move = now;
    }

    // FIXME: Handle time wrap around
    if (g_config.display.ui.hide_cursor && (now - last_mouse_move) > 3000) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
        !g_scene_mgr.IsDisplayingScene()) {

        // If the guide button is pressed, wake the ui
        bool menu_button = false;
        uint32_t buttons = g_input_mgr.CombinedButtons();
        if (buttons & CONTROLLER_BUTTON_GUIDE) {
            menu_button = true;
        }

        // Allow controllers without a guide button to also work
        if ((buttons & CONTROLLER_BUTTON_BACK) &&
            (buttons & CONTROLLER_BUTTON_START)) {
            menu_button = true;
        }

        if (HotkeyPressed(g_config.input.hotkeys.lightgun_mode)) {
            g_config.chihiro.settings.lightgun_mode =
                !g_config.chihiro.settings.lightgun_mode;
        }

        if (HotkeyPressed(g_config.input.hotkeys.settings)) {
            g_scene_mgr.PushScene(g_main_menu);
        } else if (HotkeyPressed(g_config.input.hotkeys.quick_menu)) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (menu_button ||
                   (!xemu_input_lightgun_active() &&
                    !g_config.chihiro.settings.lightgun_mode &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    !ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemHovered())) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (!xemu_input_lightgun_active() &&
                   !g_config.chihiro.settings.lightgun_mode &&
                   ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            xemu_toggle_fullscreen();
        }

        for (int i = 0; i < 4; ++i) {
            if (HotkeyPressed(QuickSaveHotkey(i))) {
                ActionQuickSave(i);
                break;
            }
            if (HotkeyPressed(QuickLoadHotkey(i))) {
                ActionQuickLoad(i);
                break;
            }
        }
    }

    first_boot_window.Draw();
    monitor_window.Draw();
    apu_window.Draw();
    video_window.Draw();
    compatibility_reporter_window.Draw();
#if defined(_WIN32)
    update_window.Draw();
#endif
    g_scene_mgr.Draw();
    if (!first_boot_window.is_open) notification_manager.Draw();
    g_snapshot_mgr.Draw();

    memset(hotkeys_down, 0, sizeof(hotkeys_down));

    // static bool show_demo = true;
    // if (show_demo) ImGui::ShowDemoWindow(&show_demo);
}

void xemu_hud_render()
{
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (g_vsync != g_config.display.window.vsync) {
        g_vsync = g_config.display.window.vsync;
        SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
    }

    if (g_screenshot_pending) {
        SaveScreenshot(g_tex, g_flip_req);
        g_screenshot_pending = false;
    }
}
