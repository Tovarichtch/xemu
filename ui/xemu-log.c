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

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include <glib/gstdio.h>
#ifndef _WIN32
#include <poll.h>
#endif

#include "xemu-log.h"
#include "xemu-settings.h"
#include "xemu-version.h"

#define XEMU_LOG_KEEP 20
/* The "-YYYYMMDD-HHMMSS.log" end of every file written here. */
#define STAMP_LEN 20

static char *log_dir_link;
static char *log_file_name;

const char *xemu_log_dir_link(void)
{
    return log_dir_link;
}

const char *xemu_log_file_name(void)
{
    return log_file_name;
}

static bool is_session_log(const char *name)
{
    size_t len = strlen(name);
    if (len <= STAMP_LEN || strcmp(name + len - 4, ".log") != 0) {
        return false;
    }
    const char *s = name + len - STAMP_LEN;
    for (int i = 0; i < STAMP_LEN - 4; i++) {
        bool dash = i == 0 || i == 9;
        if (dash ? s[i] != '-' : !g_ascii_isdigit(s[i])) {
            return false;
        }
    }
    return true;
}

/* Oldest first: the end of the name sorts by date, whatever the image. */
static gint by_date(gconstpointer a, gconstpointer b)
{
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    return strcmp(x + strlen(x) - STAMP_LEN, y + strlen(y) - STAMP_LEN);
}

/* Leaves room for this session's file among the XEMU_LOG_KEEP kept. */
static void prune(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) {
        return;
    }
    GPtrArray *logs = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (is_session_log(name)) {
            g_ptr_array_add(logs, g_strdup(name));
        }
    }
    g_dir_close(d);
    g_ptr_array_sort(logs, by_date);
    for (guint i = 0; i + XEMU_LOG_KEEP - 1 < logs->len; i++) {
        g_autofree char *path = g_build_filename(dir, logs->pdata[i], NULL);
        g_unlink(path);
    }
    g_ptr_array_free(logs, TRUE);
}

/* A run that only prints (-version, -help, "-machine help", "-cpu ?"). */
static bool info_run(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) {
            continue;
        }
        if (a[0] == '-' && a[1] == '-') {
            a++;
        }
        if (!strcmp(a, "-version") || !strcmp(a, "-help") || !strcmp(a, "-h") ||
            !strcmp(a, "help") || !strcmp(a, "?") || g_str_has_suffix(a, ",help")) {
            return true;
        }
    }
    return false;
}

/* The image this session loads (the first -dvd_path wins over the config,
 * as in ui/xemu.c), or "xemu", kept to [A-Za-z0-9._-]. */
static char *session_name(int argc, char **argv)
{
    const char *dvd = g_config.sys.files.dvd_path;
    for (int i = 1; i + 1 < argc; i++) {
        if (argv[i] && argv[i + 1] && !strcmp(argv[i], "-dvd_path")) {
            dvd = argv[i + 1];
            break;
        }
    }
    char *name = dvd && dvd[0] ? g_path_get_basename(dvd) : g_strdup("xemu");
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        *dot = '\0';
    }
    for (char *p = name; *p; p++) {
        if (!g_ascii_isalnum(*p) && !strchr("._-", *p)) {
            *p = '_';
        }
    }
    return name;
}

#ifndef _WIN32
/* The stderr the process started with gets what the file receives. The file
 * is written first, so a crash loses nothing there. Only this thread writes
 * to the terminal: a stopped one cannot hold up the exit. */
static struct {
    int in, out;
    bool started, gone;
    QemuSemaphore wake, flushed;
} mirror;

/* Writes all of buf; a full non-blocking terminal (stdio chardevs share the
 * tty) is waited for. */
static bool mirror_write(const char *buf, ssize_t n)
{
    for (ssize_t done = 0; done < n;) {
        ssize_t w = write(mirror.out, buf + done, n - done);
        if (w > 0) {
            done += w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = mirror.out, .events = POLLOUT };
            poll(&p, 1, 100);
        } else {
            return false;
        }
    }
    return true;
}

/* Copies what the file holds; false once the terminal is gone. */
static bool mirror_copy(void)
{
    char buf[4096];

    for (;;) {
        ssize_t n = read(mirror.in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return true;
        }
        if (!mirror_write(buf, n)) {
            return false;
        }
    }
}

static void *mirror_thread(void *opaque)
{
    for (;;) {
        /* Every 100 ms, or at once when xemu_log_flush asks. */
        bool asked = qemu_sem_timedwait(&mirror.wake, 100) == 0;
        bool alive = mirror_copy();
        if (asked) {
            qemu_sem_post(&mirror.flushed);
        }
        if (!alive) {
            qatomic_set(&mirror.gone, true);
            return NULL;
        }
    }
}
#endif

void xemu_log_flush(void)
{
#ifndef _WIN32
    static bool flushed;
    if (!mirror.started || flushed || qatomic_read(&mirror.gone)) {
        return;
    }
    flushed = true;
    qemu_sem_post(&mirror.wake);
    qemu_sem_timedwait(&mirror.flushed, 200);
#endif
}

void xemu_log_start(int argc, char **argv)
{
    if (info_run(argc, argv)) {
        return;
    }
#ifndef _WIN32
    /* A standard stream closed at launch would get the log file's descriptor
     * (and stderr would then be read-only): /dev/null takes the place. */
    bool had_stdout = fcntl(STDOUT_FILENO, F_GETFD) >= 0;
    bool had_stderr = fcntl(STDERR_FILENO, F_GETFD) >= 0;
    for (int i = 0; i <= 2; i++) {
        if (fcntl(i, F_GETFD) < 0) {
            int n = open("/dev/null", i ? O_WRONLY : O_RDONLY);
            if (n >= 0 && n != i) {
                dup2(n, i);
                close(n);
            }
        }
    }
#endif

    g_autofree char *dir = g_build_filename(xemu_settings_get_base_path(),
                                            "logs", NULL);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        fprintf(stderr, "xemu: no session log, cannot create %s\n", dir);
        return;
    }
    prune(dir);

    g_autofree char *name = session_name(argc, argv);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_autofree char *path = NULL;
    int fd = -1;
    /* Two sessions started in the same second (linked cabinets) each get a
     * file: <image>.2-<date>.log and so on. */
    for (int k = 1; fd < 0 && k <= 9; k++) {
        g_free(log_file_name);
        log_file_name = k == 1 ? g_strdup_printf("%s-%s.log", name, stamp) :
                        g_strdup_printf("%s.%d-%s.log", name, k, stamp);
        g_free(path);
        path = g_build_filename(dir, log_file_name, NULL);
        fd = g_open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
    }
    if (fd < 0) {
        fprintf(stderr, "xemu: no session log, cannot create %s\n", path);
        g_clear_pointer(&log_file_name, g_free);
        return;
    }

    /* What was printed before the file existed. */
    g_autofree char *head = g_strdup_printf(
        "xemu_version: %s\nxemu_commit: %s\nxemu_date: %s\nconfig path: %s\n",
        xemu_version, xemu_commit, xemu_date, xemu_settings_get_path());
    size_t head_len = strlen(head);
    if (write(fd, head, head_len) != (ssize_t)head_len) {
        head_len = 0;
    }

    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    /* SDL_OpenURL hands a plain path to the shell: a file:// URI with an
     * accented user name may not open there. */
    log_dir_link = g_strdup(dir);
    /* A console user learns where the lines go from now on (g_printerr
     * converts the UTF-8 path to the console's code page). */
    g_printerr("xemu: session log %s\n", path);
    fflush(stderr);
    /* Bound by name like the upstream xemu.log, which this replaces: a GUI
     * process has no standard streams of its own. */
    close(fd);
    g_freopen(path, "a", stdout);
    g_freopen(path, "a", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#else
    log_dir_link = g_filename_to_uri(dir, NULL, NULL);
    /* stdout joins the file when it went where stderr goes, or nowhere;
     * sent elsewhere (a pipe, a file) it is left alone. */
    struct stat so, se, dn;
    bool stats = fstat(STDOUT_FILENO, &so) == 0 &&
                 fstat(STDERR_FILENO, &se) == 0;
    bool take_stdout = !had_stdout || (stats && so.st_dev == se.st_dev &&
                                       so.st_ino == se.st_ino);
    /* Nothing to copy to a stderr that was closed or is /dev/null. */
    bool err_null = stats && S_ISCHR(se.st_mode) &&
                    stat("/dev/null", &dn) == 0 && se.st_rdev == dn.st_rdev;
    mirror.out = had_stderr && !err_null ?
                 fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3) : -1;
    if (take_stdout) {
        dup2(fd, STDOUT_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
    if (mirror.out >= 0) {
        mirror.in = open(path, O_RDONLY | O_CLOEXEC);
        if (mirror.in >= 0 && lseek(mirror.in, head_len, SEEK_SET) >= 0) {
            QemuThread thread;
            qemu_sem_init(&mirror.wake, 0);
            qemu_sem_init(&mirror.flushed, 0);
            mirror.started = true;
            qemu_thread_create(&thread, "log-mirror", mirror_thread, NULL,
                               QEMU_THREAD_DETACHED);
            /* The exit() paths (errors, help listings); main() calls it
             * before its _exit. */
            atexit(xemu_log_flush);
        } else {
            if (mirror.in >= 0) {
                close(mirror.in);
            }
            close(mirror.out);
        }
    }
#endif

    fprintf(stderr, "xemu: session log %s\n", path);
    if (xemu_debug_mode()) {
        fprintf(stderr, "xemu: Debug mode on\n");
    }
}
