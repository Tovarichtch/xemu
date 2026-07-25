/*
 * Chihiro runtime logging categories
 *
 * Selected at run time with XEMU_CHIHIRO_LOG, e.g.
 *     XEMU_CHIHIRO_LOG=boot,card   XEMU_CHIHIRO_LOG=all
 * Nothing is logged unless a category is requested, except errors.
 */

#ifndef HW_XBOX_CHIHIRO_LOG_H
#define HW_XBOX_CHIHIRO_LOG_H

enum {
    CHIHIRO_LOG_BOOT    = 1 << 0,  /* SEGABOOT state machine, boot handoff */
    CHIHIRO_LOG_JVS     = 1 << 1,  /* JVS frames and I/O board */
    CHIHIRO_LOG_CARD    = 1 << 2,  /* CRP-1231 card reader */
    CHIHIRO_LOG_FFB     = 1 << 3,  /* OutRun 2 drive board */
    CHIHIRO_LOG_MBCOM   = 1 << 4,  /* media board mailbox commands */
    CHIHIRO_LOG_USB     = 1 << 5,  /* AN2131 vendor requests, endpoints */
    CHIHIRO_LOG_VERBOSE = 1 << 6,  /* legacy firehose: LPC/SADDR traffic */
};

static const struct {
    const char *name;
    unsigned bit;
} chihiro_log_categories[] = {
    { "boot",    CHIHIRO_LOG_BOOT    },
    { "jvs",     CHIHIRO_LOG_JVS     },
    { "card",    CHIHIRO_LOG_CARD    },
    { "ffb",     CHIHIRO_LOG_FFB     },
    { "mbcom",   CHIHIRO_LOG_MBCOM   },
    { "usb",     CHIHIRO_LOG_USB     },
    { "verbose", CHIHIRO_LOG_VERBOSE },
};

extern unsigned chihiro_log_mask;

#define CHIHIRO_LOGF(cat, fmt, ...)                                          \
    do {                                                                     \
        if (chihiro_log_mask & (CHIHIRO_LOG_##cat)) {                        \
            fprintf(stderr, "[%07lld][chihiro] " fmt,                        \
                    (long long)qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),        \
                    ##__VA_ARGS__);                                    \
        }                                                                    \
    } while (0)

/* Always reported: the user needs these in a bug report. */
#define CHIHIRO_ERRF(fmt, ...)                                               \
    fprintf(stderr, "[%07lld][chihiro] " fmt,                                \
            (long long)qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), ##__VA_ARGS__)

static inline void chihiro_log_init(void)
{
    const char *spec = getenv("XEMU_CHIHIRO_LOG");
    if (!spec || !*spec) {
        return;
    }

    for (const char *p = spec; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *end = p;
        while (*end && *end != ',' && *end != ' ') end++;
        size_t len = end - p;
        if (len) {
            if (len == 3 && !strncasecmp(p, "all", 3)) {
                chihiro_log_mask = ~0u;
            } else {
                bool found = false;
                for (size_t i = 0; i < ARRAY_SIZE(chihiro_log_categories); i++) {
                    const char *n = chihiro_log_categories[i].name;
                    if (strlen(n) == len && !strncasecmp(p, n, len)) {
                        chihiro_log_mask |= chihiro_log_categories[i].bit;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    fprintf(stderr, "Chihiro: unknown log category '%.*s' "
                            "(known: boot jvs card ffb mbcom usb verbose all)\n",
                            (int)len, p);
                }
            }
        }
        p = end;
    }

    if (chihiro_log_mask) {
        fprintf(stderr, "Chihiro: logging enabled (%s)\n", spec);
    }
}

#endif /* HW_XBOX_CHIHIRO_LOG_H */
