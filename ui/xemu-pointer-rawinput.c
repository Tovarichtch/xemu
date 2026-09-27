/*
 * xemu per-device pointer input (light guns, mice): Raw Input backend (Windows)
 *
 * Copyright (c) 2026 Réda Chérif-Touil (Tovarichtch)
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
#include "xemu-pointer.h"
#include "xemu-settings.h"
#include <SDL3/SDL.h>


/*
 * Raw Input backend. Windows merges every mouse into the system cursor;
 * WM_INPUT still names the device behind each packet. Modelled on the
 * PCSX2X6 RawInputSource: the SDL window registers for mouse raw input,
 * an SDL message hook reads the packets, devices are told apart by their
 * interface path (one per physical port). Unlike evdev nothing can take a
 * device away from the desktop cursor: the grab has no counterpart here.
 */

#include <hidsdi.h>

extern SDL_Window *m_window;

typedef struct Pointer {
    XemuPointer info;
    HANDLE handle;        /* NULL once unplugged */
    char path[192];       /* interface path, one per physical port */
    bool seen;            /* a position has been reported */
    bool logged;          /* the first packet went to the log */
    int cur_x, cur_y;     /* absolute devices, 0..65535 */
    bool virtual_desktop; /* cur_x/cur_y span every monitor */
    float rel_x, rel_y;   /* relative devices, window pixels */
    bool rel_init;
    uint32_t buttons;
} Pointer;

static Pointer pointers[XEMU_POINTER_MAX];
static int npointers;
static HWND hwnd;
static bool running;
static bool rescan_pending; /* raised by the hook, served by the poll */

static void window_size(float *w, float *h)
{
    int iw = 0, ih = 0;
    if (m_window) {
        SDL_GetWindowSizeInPixels(m_window, &iw, &ih);
    }
    *w = iw > 0 ? iw : 1;
    *h = ih > 0 ? ih : 1;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

static Pointer *find_identity(const char *identity)
{
    for (int i = 0; i < npointers; i++) {
        if (strcmp(pointers[i].info.identity, identity) == 0) {
            return &pointers[i];
        }
    }
    return NULL;
}

static Pointer *find_path(const char *path)
{
    for (int i = 0; i < npointers; i++) {
        if (pointers[i].handle && strcmp(pointers[i].path, path) == 0) {
            return &pointers[i];
        }
    }
    return NULL;
}

static Pointer *find_handle(HANDLE handle)
{
    for (int i = 0; i < npointers; i++) {
        if (pointers[i].handle == handle) {
            return &pointers[i];
        }
    }
    return NULL;
}

static void close_pointer(Pointer *p)
{
    p->handle = NULL;
    p->info.present = false;
    p->seen = false;
    p->logged = false;
    p->buttons = 0;
}

/* UTF-8 copy of a wide string, empty when it cannot be converted. */
static void wide_copy(char *dst, size_t n, const wchar_t *src)
{
    char *u = g_utf16_to_utf8((const gunichar2 *)src, -1, NULL, NULL, NULL);
    snprintf(dst, n, "%s", u ? u : "");
    g_free(u);
}

/* The interface path of a device, in full for CreateFile and without the
 * \\?\ prefix and class GUID for the table:
 * "HID#VID_16C0&PID_0F01&MI_01#8&2f5b0c8&0&0000". */
static bool device_path(HANDLE h, wchar_t *wpath, UINT wn, char *path,
                        size_t n)
{
    UINT len = wn;
    UINT r = GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, wpath, &len);
    if (r == (UINT)-1 || r == 0) {
        return false;
    }
    wpath[MIN(r, wn - 1)] = L'\0';

    char full[512];
    wide_copy(full, sizeof(full), wpath);
    const char *s = full;
    if (strncmp(s, "\\\\?\\", 4) == 0 || strncmp(s, "\\\\.\\", 4) == 0) {
        s += 4;
    }
    g_strlcpy(path, s, n);
    char *guid = strstr(path, "#{");
    if (guid) {
        *guid = '\0';
    }
    return path[0] != '\0';
}

/* VID and PID from the path ("VID_16C0&PID_0F01"); false without them
 * (I2C touchpads, PS/2, Bluetooth). */
static bool path_vidpid(const char *path, unsigned *vid, unsigned *pid)
{
    const char *v = strstr(path, "VID_");
    const char *p = strstr(path, "PID_");
    return v && p && sscanf(v + 4, "%4x", vid) == 1 &&
           sscanf(p + 4, "%4x", pid) == 1 && *vid && *pid;
}

/* The path identity keeps the end of a long path: the instance id there is
 * what tells two identical devices apart. */
static void path_identity(char *identity, size_t n, const char *path)
{
    size_t len = strlen(path);
    snprintf(identity, n, "path:%s", len > 56 ? path + len - 56 : path);
}

/* Product string from the HID driver. The device is opened without access
 * rights, which mice and keyboards allow. */
static void hid_product(const wchar_t *wpath, char *name, size_t nn)
{
    name[0] = '\0';
    HANDLE dev = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        return;
    }
    wchar_t w[128];
    if (HidD_GetProductString(dev, w, sizeof(w))) {
        w[ARRAY_SIZE(w) - 1] = L'\0';
        wide_copy(name, nn, w);
    }
    CloseHandle(dev);
    trim(name);
}

/* One mouse of the Raw Input list. A path already open only gets its new
 * handle (they change across sleep and replug). */
static void open_device(HANDLE h, const wchar_t *wpath, const char *path)
{
    Pointer *p = find_path(path);
    if (p) {
        p->handle = h;
        return;
    }

    char name[80];
    hid_product(wpath, name, sizeof(name));
    unsigned vid = 0, pid = 0;
    bool has_vidpid = path_vidpid(path, &vid, &pid);

    /* Identity as in PCSX2X6: VID:PID, which survives a change of port,
     * else the port itself. */
    char identity[64];
    if (has_vidpid) {
        snprintf(identity, sizeof(identity), "vidpid:%04x:%04x", vid, pid);
    } else {
        path_identity(identity, sizeof(identity), path);
    }
    /* Twins: fall back to the port. */
    p = find_identity(identity);
    if (p && p->handle) {
        path_identity(identity, sizeof(identity), path);
        p = find_identity(identity);
        if (p && p->handle) {
            return;
        }
    }
    if (!p) {
        if (npointers >= XEMU_POINTER_MAX) {
            return;
        }
        p = &pointers[npointers++];
        memset(p, 0, sizeof(*p));
        snprintf(p->info.identity, sizeof(p->info.identity), "%s", identity);
    }

    close_pointer(p);
    p->handle = h;
    g_strlcpy(p->path, path, sizeof(p->path));
    if (has_vidpid) {
        snprintf(p->info.name, sizeof(p->info.name), "%s [%04X:%04X]",
                 name[0] ? name : "Pointer", vid, pid);
    } else {
        snprintf(p->info.name, sizeof(p->info.name), "%s",
                 name[0] ? name : "Pointer");
    }
    p->info.present = true;
    /* Absolute or relative is only told by the first motion packet. */
    p->rel_init = false;
    fprintf(stderr, "pointer: %s = %s (%s)\n", p->info.name, path,
            p->info.identity);
}

typedef struct Listed {
    HANDLE handle;
    wchar_t wpath[256];
    char path[192];
} Listed;

static void scan(void)
{
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) ==
        (UINT)-1) {
        return;
    }
    RAWINPUTDEVICELIST *list = g_new0(RAWINPUTDEVICELIST, n + 1);
    UINT got = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    if (got == (UINT)-1) {
        /* The list changed under us; the next device change tries again. */
        g_free(list);
        return;
    }
    Listed *mice = g_new0(Listed, got + 1);
    int nmice = 0;
    for (UINT i = 0; i < got; i++) {
        if (list[i].dwType != RIM_TYPEMOUSE) {
            continue;
        }
        Listed *m = &mice[nmice];
        if (device_path(list[i].hDevice, m->wpath, ARRAY_SIZE(m->wpath),
                        m->path, sizeof(m->path))) {
            m->handle = list[i].hDevice;
            nmice++;
        }
    }
    g_free(list);

    /* Drop the devices whose port is no longer listed. */
    for (int i = 0; i < npointers; i++) {
        Pointer *p = &pointers[i];
        if (!p->handle) {
            continue;
        }
        bool listed = false;
        for (int j = 0; j < nmice; j++) {
            if (strcmp(mice[j].path, p->path) == 0) {
                listed = true;
            }
        }
        if (!listed) {
            fprintf(stderr, "pointer: %s unplugged\n", p->info.name);
            close_pointer(p);
        }
    }
    for (int j = 0; j < nmice; j++) {
        open_device(mice[j].handle, mice[j].wpath, mice[j].path);
    }
    g_free(mice);
}

static void handle_mouse(Pointer *p, const RAWMOUSE *m)
{
    /* The first packet tells absolute from relative; keep it in the log
     * for bug reports (bit 0 of the flags = absolute). */
    if (!p->logged) {
        p->logged = true;
        fprintf(stderr, "pointer: %s first packet: flags 0x%04x x=%ld y=%ld "
                "buttons 0x%04x\n", p->info.name, m->usFlags, m->lLastX,
                m->lLastY, m->usButtonFlags);
    }
    /* A Sinden reports 0,0 while its camera cannot see the border, which
     * the aim code then reads as off the picture: that is the reload. */
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
        p->info.absolute = true;
        p->cur_x = m->lLastX;
        p->cur_y = m->lLastY;
        p->virtual_desktop = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        p->seen = true;
    } else if (m->lLastX || m->lLastY) {
        p->rel_x += m->lLastX;
        p->rel_y += m->lLastY;
        p->seen = true;
    }

    /* Bits in the SDL layout: left, middle, right, x1, x2. */
    static const struct {
        USHORT down, up;
        int bit;
    } map[] = {
        { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0 },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 1 },
        { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2 },
        { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 3 },
        { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 4 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (m->usButtonFlags & map[i].down) {
            p->buttons |= 1u << map[i].bit;
        } else if (m->usButtonFlags & map[i].up) {
            p->buttons &= ~(1u << map[i].bit);
        }
    }
}

/* Runs inside SDL_PollEvent on the UI thread, outside the main-loop lock.
 * The Chihiro readers run on that same thread (the input update of the main
 * loop). An Xbox light gun also reads the table from the USB poll on the
 * QEMU thread, with no lock: a reading can then mix two packets (harmless).
 * The hook only writes into a device's entry; arrivals and removals wait for
 * xemu_pointer_poll. */
static bool SDLCALL message_hook(void *userdata, MSG *msg)
{
    if (msg->message == WM_INPUT_DEVICE_CHANGE) {
        rescan_pending = true;
    } else if (msg->message == WM_INPUT) {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT, &raw, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            raw.header.dwType == RIM_TYPEMOUSE && raw.header.hDevice) {
            Pointer *p = find_handle(raw.header.hDevice);
            if (p) {
                handle_mouse(p, &raw.data.mouse);
            }
        }
    }
    /* SDL dispatches it; DefWindowProc then frees the packet. */
    return true;
}

static void start(void)
{
    running = true;
    hwnd = m_window ? SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                             NULL)
                    : NULL;
    if (!hwnd) {
        fprintf(stderr, "pointer: no window for raw input\n");
        return;
    }
    /* Mice (usage page 1, usage 2), also while the window is not in the
     * foreground, with arrival and removal notices. */
    RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                           hwnd };
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        fprintf(stderr, "pointer: raw input registration failed (%lu)\n",
                GetLastError());
        return;
    }
    SDL_SetWindowsMessageHook(message_hook, NULL);
    scan();
}

static void stop(void)
{
    RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_REMOVE, NULL };
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    SDL_SetWindowsMessageHook(NULL, NULL);
    for (int i = 0; i < npointers; i++) {
        close_pointer(&pointers[i]);
    }
    npointers = 0;
    rescan_pending = false;
    running = false;
}

const char *xemu_pointer_backend(void)
{
    return "raw input";
}

bool xemu_pointer_exclusive_grab(void)
{
    return false;
}

void xemu_pointer_init(void)
{
    if (g_config.chihiro.settings.pointer_devices) {
        start();
    }
}

void xemu_pointer_poll(void)
{
    bool want = g_config.chihiro.settings.pointer_devices;
    if (want != running) {
        if (want) {
            start();
        } else {
            stop();
        }
    }
    if (!running) {
        return;
    }
    if (rescan_pending) {
        rescan_pending = false;
        scan();
    }
}

void xemu_pointer_rescan(void)
{
    if (running) {
        scan();
    }
}

int xemu_pointer_count(void)
{
    return npointers;
}

const XemuPointer *xemu_pointer_get(int i)
{
    return (i >= 0 && i < npointers) ? &pointers[i].info : NULL;
}

bool xemu_pointer_position(const char *identity, float *x, float *y)
{
    Pointer *p = identity ? find_identity(identity) : NULL;
    if (!p || !p->handle || !p->seen) {
        return false;
    }
    float w, h;
    window_size(&w, &h);
    if (p->info.absolute) {
        /* The device reports the screen (or the whole desktop). In
         * fullscreen that is the window, as on Linux; in a window only the
         * part of the screen under it aims, like PCSX2X6. */
        bool vd = p->virtual_desktop;
        float sw = GetSystemMetrics(vd ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        float sh = GetSystemMetrics(vd ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        float sx = p->cur_x / 65535.0f * sw;
        float sy = p->cur_y / 65535.0f * sh;
        if (vd) {
            sx += GetSystemMetrics(SM_XVIRTUALSCREEN);
            sy += GetSystemMetrics(SM_YVIRTUALSCREEN);
        }
        POINT origin = { 0, 0 };
        ClientToScreen(hwnd, &origin);
        *x = sx - origin.x;
        *y = sy - origin.y;
    } else {
        if (!p->rel_init) {
            p->rel_x = w / 2;
            p->rel_y = h / 2;
            p->rel_init = true;
        }
        p->rel_x = MIN(MAX(p->rel_x, 0.0f), w - 1);
        p->rel_y = MIN(MAX(p->rel_y, 0.0f), h - 1);
        *x = p->rel_x;
        *y = p->rel_y;
    }
    return true;
}

uint32_t xemu_pointer_buttons(const char *identity)
{
    Pointer *p = identity ? find_identity(identity) : NULL;
    return (p && p->handle) ? p->buttons : 0;
}

/* Windows has one desktop cursor for every device: confining it to the
 * window would lock the user out of the title bar (PCSX2X6 only clips it
 * in its relative mouse mode, never for guns), so nothing is held. */
void xemu_pointer_set_grab(bool on, const char *id1, const char *id2)
{
}
