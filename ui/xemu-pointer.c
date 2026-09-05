/*
 * xemu per-device pointer input (light guns, mice)
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

#ifdef __linux__

#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>

extern SDL_Window *m_window;

typedef struct Pointer {
    XemuPointer info;
    int fd;
    char node[32];
    bool touch_is_left;   /* BTN_TOUCH stands in for a missing BTN_LEFT */
    bool grabbed;
    bool seen;            /* a position has been reported */
    struct input_absinfo abs_x, abs_y;
    int cur_x, cur_y;     /* absolute devices, device units */
    float rel_x, rel_y;   /* relative devices, window pixels */
    bool rel_init;
    uint32_t buttons;
} Pointer;

static Pointer pointers[XEMU_POINTER_MAX];
static int npointers;
static int inotify_fd = -1;
static bool running;

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static bool test_bit(const unsigned long *bits, unsigned int bit)
{
    return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1;
}

static void window_size(float *w, float *h)
{
    int iw = 0, ih = 0;
    if (m_window) {
        SDL_GetWindowSizeInPixels(m_window, &iw, &ih);
    }
    *w = iw > 0 ? iw : 1;
    *h = ih > 0 ? ih : 1;
}

/* Button number in the SDL layout, -1 when the code is not a button. */
static int button_bit(uint16_t code, bool touch_is_left)
{
    switch (code) {
    case BTN_LEFT:   return 0;
    case BTN_MIDDLE: return 1;
    case BTN_RIGHT:  return 2;
    case BTN_SIDE:   return 3;
    case BTN_EXTRA:  return 4;
    case BTN_TOUCH:  return touch_is_left ? 0 : -1;
    default:
        if (code >= BTN_1 && code <= BTN_8) {
            return 5 + (code - BTN_1);
        }
        return -1;
    }
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

static Pointer *find_node(const char *node)
{
    for (int i = 0; i < npointers; i++) {
        if (pointers[i].fd >= 0 && strcmp(pointers[i].node, node) == 0) {
            return &pointers[i];
        }
    }
    return NULL;
}

static void close_pointer(Pointer *p)
{
    if (p->fd < 0) {
        return;
    }
    if (p->grabbed) {
        ioctl(p->fd, EVIOCGRAB, 0);
        p->grabbed = false;
    }
    close(p->fd);
    p->fd = -1;
    p->info.present = false;
    p->seen = false;
    p->buttons = 0;
}

/* Open one event node if it is a pointer: a left button (or touch) with
 * absolute or relative X/Y axes, and not a gamepad or joystick. */
static void open_node(const char *node)
{
    if (find_node(node)) {
        return;
    }
    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return;
    }

    unsigned long ev[NLONGS(EV_MAX + 1)] = { 0 };
    unsigned long key[NLONGS(KEY_MAX + 1)] = { 0 };
    unsigned long abs[NLONGS(ABS_MAX + 1)] = { 0 };
    unsigned long rel[NLONGS(REL_MAX + 1)] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);

    bool has_left = test_bit(key, BTN_LEFT);
    bool pointer = has_left || test_bit(key, BTN_TOUCH);
    bool pad = test_bit(key, BTN_SOUTH) || test_bit(key, BTN_JOYSTICK) ||
               test_bit(key, BTN_TRIGGER);
    bool absolute = test_bit(ev, EV_ABS) && test_bit(abs, ABS_X) &&
                    test_bit(abs, ABS_Y);
    bool relative = test_bit(ev, EV_REL) && test_bit(rel, REL_X) &&
                    test_bit(rel, REL_Y);
    if (!pointer || pad || !(absolute || relative)) {
        close(fd);
        return;
    }

    struct input_absinfo ax = { 0 }, ay = { 0 };
    if (absolute) {
        if (ioctl(fd, EVIOCGABS(ABS_X), &ax) < 0 ||
            ioctl(fd, EVIOCGABS(ABS_Y), &ay) < 0 ||
            ax.maximum <= ax.minimum || ay.maximum <= ay.minimum) {
            absolute = false;
            if (!relative) {
                close(fd);
                return;
            }
        }
    }

    char name[80] = "", phys[96] = "", uniq[64] = "";
    struct input_id id = { 0 };
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys);
    ioctl(fd, EVIOCGUNIQ(sizeof(uniq) - 1), uniq);
    ioctl(fd, EVIOCGID, &id);
    trim(name);
    trim(phys);
    trim(uniq);

    /* Identity: serial, else VID:PID, else port, else name. Serials under
     * 8 characters are firmware placeholders shared by every unit. */
    char identity[64];
    if (strlen(uniq) >= 8) {
        snprintf(identity, sizeof(identity), "uniq:%.56s", uniq);
    } else if (id.vendor && id.product) {
        snprintf(identity, sizeof(identity), "vidpid:%04x:%04x", id.vendor,
                 id.product);
    } else if (phys[0]) {
        snprintf(identity, sizeof(identity), "path:%.56s", phys);
    } else if (name[0]) {
        snprintf(identity, sizeof(identity), "name:%.56s", name);
    } else {
        snprintf(identity, sizeof(identity), "node:%.56s", node);
    }
    /* Twins: fall back to the port, then the node. */
    Pointer *p = find_identity(identity);
    if (p && p->fd >= 0) {
        if (phys[0]) {
            snprintf(identity, sizeof(identity), "path:%.56s", phys);
        }
        p = find_identity(identity);
        if (p && p->fd >= 0) {
            snprintf(identity, sizeof(identity), "node:%.56s", node);
            p = find_identity(identity);
        }
    }
    if (!p) {
        if (npointers >= XEMU_POINTER_MAX) {
            close(fd);
            return;
        }
        p = &pointers[npointers++];
        memset(p, 0, sizeof(*p));
        p->fd = -1;
        snprintf(p->info.identity, sizeof(p->info.identity), "%s", identity);
    }

    close_pointer(p);
    p->fd = fd;
    g_strlcpy(p->node, node, sizeof(p->node));
    if (id.vendor && id.product) {
        snprintf(p->info.name, sizeof(p->info.name), "%s [%04X:%04X]",
                 name[0] ? name : "Pointer", id.vendor, id.product);
    } else {
        snprintf(p->info.name, sizeof(p->info.name), "%s",
                 name[0] ? name : "Pointer");
    }
    p->info.present = true;
    p->info.absolute = absolute;
    p->touch_is_left = !has_left;
    p->abs_x = ax;
    p->abs_y = ay;
    p->cur_x = ax.value;
    p->cur_y = ay.value;
    p->rel_init = false;
    fprintf(stderr, "pointer: %s = %s (%s, %s)\n", p->info.name, node,
            absolute ? "absolute" : "relative", p->info.identity);
}

static void scan(void)
{
    /* Drop nodes that went away. */
    for (int i = 0; i < npointers; i++) {
        Pointer *p = &pointers[i];
        if (p->fd >= 0 && access(p->node, F_OK) != 0) {
            fprintf(stderr, "pointer: %s unplugged\n", p->info.name);
            close_pointer(p);
        }
    }
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) == 0) {
            char node[32];
            snprintf(node, sizeof(node), "/dev/input/%.20s", de->d_name);
            open_node(node);
        }
    }
    closedir(dir);
}

static void start(void)
{
    scan();
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0) {
        /* IN_ATTRIB: udev sets the node permissions after creation. */
        inotify_add_watch(inotify_fd, "/dev/input",
                          IN_CREATE | IN_DELETE | IN_ATTRIB);
    }
    running = true;
}

static void stop(void)
{
    for (int i = 0; i < npointers; i++) {
        close_pointer(&pointers[i]);
    }
    npointers = 0;
    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }
    running = false;
}

/* The kernel dropped events: re-read the state instead of trusting the stream. */
static void resync(Pointer *p)
{
    if (p->info.absolute && ioctl(p->fd, EVIOCGABS(ABS_X), &p->abs_x) >= 0 &&
        ioctl(p->fd, EVIOCGABS(ABS_Y), &p->abs_y) >= 0) {
        p->cur_x = p->abs_x.value;
        p->cur_y = p->abs_y.value;
        p->seen = true;
    }
    unsigned long key[NLONGS(KEY_MAX + 1)] = { 0 };
    if (ioctl(p->fd, EVIOCGKEY(sizeof(key)), key) < 0) {
        return;
    }
    static const uint16_t codes[] = {
        BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA, BTN_TOUCH,
        BTN_1, BTN_2, BTN_3, BTN_4, BTN_5, BTN_6, BTN_7, BTN_8,
    };
    p->buttons = 0;
    for (size_t i = 0; i < ARRAY_SIZE(codes); i++) {
        int bit = button_bit(codes[i], p->touch_is_left);
        if (bit >= 0 && test_bit(key, codes[i])) {
            p->buttons |= 1u << bit;
        }
    }
}

static void handle_event(Pointer *p, const struct input_event *e)
{
    switch (e->type) {
    case EV_ABS:
        if (e->code == ABS_X) {
            p->cur_x = e->value;
            p->seen = true;
        } else if (e->code == ABS_Y) {
            p->cur_y = e->value;
            p->seen = true;
        }
        break;
    case EV_REL:
        if (e->code == REL_X) {
            p->rel_x += e->value;
            p->seen = true;
        } else if (e->code == REL_Y) {
            p->rel_y += e->value;
            p->seen = true;
        }
        break;
    case EV_KEY: {
        if (e->value > 1) {
            break;
        }
        int bit = button_bit(e->code, p->touch_is_left);
        if (bit >= 0) {
            if (e->value) {
                p->buttons |= 1u << bit;
            } else {
                p->buttons &= ~(1u << bit);
            }
        }
        break;
    }
    case EV_SYN:
        if (e->code == SYN_DROPPED) {
            resync(p);
        }
        break;
    default:
        break;
    }
}

static void drain(Pointer *p)
{
    struct input_event evs[64];
    for (;;) {
        ssize_t r = read(p->fd, evs, sizeof(evs));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "pointer: %s unplugged\n", p->info.name);
                close_pointer(p);
            }
            return;
        }
        if (r == 0) {
            return;
        }
        size_t n = (size_t)r / sizeof(evs[0]);
        for (size_t i = 0; i < n; i++) {
            handle_event(p, &evs[i]);
        }
    }
}

const char *xemu_pointer_backend(void)
{
    return "evdev";
}

void xemu_pointer_init(void)
{
    for (int i = 0; i < XEMU_POINTER_MAX; i++) {
        pointers[i].fd = -1;
    }
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
    if (inotify_fd >= 0) {
        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        bool changed = false;
        for (;;) {
            ssize_t len = read(inotify_fd, buf, sizeof(buf));
            if (len <= 0) {
                break;
            }
            for (char *q = buf; q < buf + len;) {
                const struct inotify_event *ie = (const struct inotify_event *)q;
                if (ie->len > 0 && strncmp(ie->name, "event", 5) == 0) {
                    changed = true;
                }
                q += sizeof(struct inotify_event) + ie->len;
            }
        }
        if (changed) {
            scan();
        }
    }
    for (int i = 0; i < npointers; i++) {
        if (pointers[i].fd >= 0) {
            drain(&pointers[i]);
        }
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
    if (!p || p->fd < 0 || !p->seen) {
        return false;
    }
    float w, h;
    window_size(&w, &h);
    if (p->info.absolute) {
        /* The device range spans the whole window; in fullscreen that is the
         * screen the gun is calibrated to. */
        float rx = (float)(p->cur_x - p->abs_x.minimum) /
                   (float)(p->abs_x.maximum - p->abs_x.minimum);
        float ry = (float)(p->cur_y - p->abs_y.minimum) /
                   (float)(p->abs_y.maximum - p->abs_y.minimum);
        *x = rx * w;
        *y = ry * h;
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
    return (p && p->fd >= 0) ? p->buttons : 0;
}

void xemu_pointer_set_grab(bool on, const char *id1, const char *id2)
{
    for (int i = 0; i < npointers; i++) {
        Pointer *p = &pointers[i];
        if (p->fd < 0) {
            continue;
        }
        bool want = on && ((id1 && strcmp(p->info.identity, id1) == 0) ||
                           (id2 && strcmp(p->info.identity, id2) == 0));
        if (want == p->grabbed) {
            continue;
        }
        if (ioctl(p->fd, EVIOCGRAB, want ? 1 : 0) >= 0) {
            p->grabbed = want;
        } else if (!want) {
            p->grabbed = false;
        }
    }
}

#else /* no per-device backend on this host */

const char *xemu_pointer_backend(void)
{
    return NULL;
}

void xemu_pointer_init(void)
{
}

void xemu_pointer_poll(void)
{
}

void xemu_pointer_rescan(void)
{
}

int xemu_pointer_count(void)
{
    return 0;
}

const XemuPointer *xemu_pointer_get(int i)
{
    return NULL;
}

bool xemu_pointer_position(const char *identity, float *x, float *y)
{
    return false;
}

uint32_t xemu_pointer_buttons(const char *identity)
{
    return 0;
}

void xemu_pointer_set_grab(bool on, const char *id1, const char *id2)
{
}

#endif
