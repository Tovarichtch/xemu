/*
 * xemu Network Management
 *
 * Wrapper functions to configure network settings at runtime.
 *
 * Copyright (C) 2020-2021 Matt Borgerson
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

#include "xemu-net.h"
#include "xemu-settings.h"

#include "qemu/osdep.h"
#include "qapi/qapi-commands-net.h"
#include "qemu/sockets.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "net/net.h"
#include "net/hub.h"
#include "net/slirp.h"
#include "qemu/timer.h"
#include <libslirp.h>
#if defined(_WIN32)
#include <pcap/pcap.h>
#endif
#include "xemu-notifications.h"

static const char *id = "xemu-netdev";
static const char *id_hubport = "xemu-netdev-hubport";

void xemu_net_enable(void)
{
    Error *local_err = NULL;

    NetClientState *nc = qemu_find_netdev(id);
    if (nc != NULL) {
        return;
    }

    // Create the netdev
    QDict *qdict;
    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        qdict = qdict_new();
        qdict_put_str(qdict, "id",   id);
        qdict_put_str(qdict, "type", "user");
    } else if (g_config.net.backend == CONFIG_NET_BACKEND_UDP) {
        qdict = qdict_new();
        qdict_put_str(qdict, "id",        id);
        qdict_put_str(qdict, "type",      "socket");
        qdict_put_str(qdict, "udp",       g_config.net.udp.remote_addr);
        qdict_put_str(qdict, "localaddr", g_config.net.udp.bind_addr);
    } else if (g_config.net.backend == CONFIG_NET_BACKEND_PCAP) {
#if defined(_WIN32)
        if (pcap_load_library()) {
            return;
        }
#endif
        qdict = qdict_new();
        qdict_put_str(qdict, "id",        id);
        qdict_put_str(qdict, "type",      "pcap");
        qdict_put_str(qdict, "ifname",    g_config.net.pcap.netif);
    } else {
        // Unsupported backend type
        return;
    }

    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("netdev"), qdict, &error_abort);
    qobject_unref(qdict);
    netdev_add(opts, &local_err);
    if (local_err) {
        qemu_opts_del(opts);
        // error_propagate(errp, local_err);
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        return;
    }

    // Create the hubport
    qdict = qdict_new();
    qdict_put_str(qdict, "id",     id_hubport);
    qdict_put_str(qdict, "type",   "hubport");
    qdict_put_int(qdict, "hubid",  0);
    qdict_put_str(qdict, "netdev", id);
    opts = qemu_opts_from_qdict(qemu_find_opts("netdev"), qdict, &error_abort);
    qobject_unref(qdict);
    netdev_add(opts, &local_err);
    if (local_err) {
        qemu_opts_del(opts);
        // error_propagate(errp, local_err);
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        return;
    }

    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        void *s = slirp_get_state_from_netdev(id);
        assert(s != NULL);

        struct in_addr host_addr = { .s_addr = INADDR_ANY };
        struct in_addr guest_addr = { .s_addr = 0 };
        inet_aton("10.0.2.15", &guest_addr);

        for (int i = 0; i < g_config.net.nat.forward_ports_count; i++) {
            bool is_udp = g_config.net.nat.forward_ports[i].protocol ==
                          CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP;
            int host_port = g_config.net.nat.forward_ports[i].host;
            int guest_port = g_config.net.nat.forward_ports[i].guest;

            if (slirp_add_hostfwd(s, is_udp, host_addr, host_port, guest_addr,
                                  guest_port) < 0) {
                error_setg(&local_err,
                           "Could not set host forwarding rule "
                           "%d -> %d (%s)\n",
                           host_port, guest_port, is_udp ? "udp" : "tcp");
                xemu_queue_error_message(error_get_pretty(local_err));
                break;
            }

        }
    }

    if (local_err) {
        xemu_net_disable();
    }

    qmp_set_link("nvnet.0", true, NULL);
    g_config.net.enable = true;
}

static void remove_netdev(const char *name)
{
    NetClientState *nc;
    QemuOpts *opts;

    nc = qemu_find_netdev(name);
    if (!nc) {
        // error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
        //           "Device '%s' not found", name);
        return;
    }

    opts = qemu_opts_find(qemu_find_opts_err("netdev", NULL), name);
    if (!opts) {
        // error_setg(errp, "Device '%s' is not a netdev", name);
        return;
    }
    qemu_opts_del(opts);
    qemu_del_net_client(nc);
}

static void clear_slirp_port_forwards(void)
{
    void *s = slirp_get_state_from_netdev(id);
    if (!s) {
        return;
    }

    struct in_addr host_addr = { .s_addr = INADDR_ANY };
    for (int i = 0; i < g_config.net.nat.forward_ports_count; i++) {
        slirp_remove_hostfwd(s,
                             g_config.net.nat.forward_ports[i].protocol ==
                                 CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP,
                             host_addr,
                             g_config.net.nat.forward_ports[i].host);
    }
}

void xemu_net_disable(void)
{
    if (g_config.net.backend == CONFIG_NET_BACKEND_NAT) {
        clear_slirp_port_forwards();
    }

    remove_netdev(id);
    remove_netdev(id_hubport);

    qmp_set_link("nvnet.0", false, NULL);
    g_config.net.enable = false;
}

int xemu_net_is_enabled(void)
{
    NetClientState *nc;
    nc = qemu_find_netdev(id);
    g_config.net.enable = (nc != NULL);
    return g_config.net.enable;
}

/*
 * The cabinet link: socket netdevs on hub 0, beside the network board. The
 * host listens on port + each other cabinet's number and its hub relays every
 * frame; a joining cabinet connects on port + its own number until it answers.
 */

static QEMUTimer *link_retry_timer;

/* Makes the netdev the qdict describes, and drops the qdict. */
static bool link_netdev_add(QDict *qdict, Error **errp)
{
    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("netdev"), qdict,
                                          &error_abort);
    Error *err = NULL;

    qobject_unref(qdict);
    netdev_add(opts, &err);
    if (err) {
        qemu_opts_del(opts);
        error_propagate(errp, err);
        return false;
    }
    return true;
}

/* One socket netdev and its port on hub 0. Returns false when the netdev
 * could not be made (a connection refused, a port already taken). */
static bool link_add_socket(const char *name, const char *how, const char *where)
{
    Error *local_err = NULL;
    char hub_name[64];
    QDict *qdict = qdict_new();

    qdict_put_str(qdict, "id", name);
    qdict_put_str(qdict, "type", "socket");
    qdict_put_str(qdict, how, where);
    if (!link_netdev_add(qdict, NULL)) {
        return false;
    }

    snprintf(hub_name, sizeof(hub_name), "%s-hub", name);
    qdict = qdict_new();
    qdict_put_str(qdict, "id", hub_name);
    qdict_put_str(qdict, "type", "hubport");
    qdict_put_int(qdict, "hubid", 0);
    qdict_put_str(qdict, "netdev", name);
    if (!link_netdev_add(qdict, &local_err)) {
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        remove_netdev(name);
        return false;
    }
    return true;
}

/* Every three seconds: a refused or dropped connection leaves a dead netdev
 * behind, so it is taken down and made again until the host answers, and a
 * host that restarts is joined again. */
static void link_connect_retry(void *opaque)
{
    static bool connected;
    char where[300];
    NetClientState *nc = qemu_find_netdev("xemu-link");

    if (nc && net_socket_is_connected(nc)) {
        if (!connected)
            xemu_queue_notification("Cabinet link: connected to the host");
        connected = true;
    } else {
        connected = false;
        if (nc) {
            remove_netdev("xemu-link-hub");
            remove_netdev("xemu-link");
        }
        snprintf(where, sizeof(where), "%s:%d", g_config.chihiro.link.host,
                 g_config.chihiro.link.port + g_config.chihiro.link.cabinet);
        link_add_socket("xemu-link", "connect", where);
    }
    timer_mod(link_retry_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 3000);
}

void xemu_link_enable(void)
{
    int me = g_config.chihiro.link.cabinet;
    int cabinets = g_config.chihiro.link.cabinets;
    int port = g_config.chihiro.link.port;
    const char *host = g_config.chihiro.link.host;

    if (!g_config.chihiro.link.enable || !xemu_chihiro_mode()) {
        return;
    }
    if (me < 1 || me > 4 || cabinets < 2 || cabinets > 4 || me > cabinets) {
        xemu_queue_error_message("Cabinet link: the current cabinet ID must be "
                                 "1 to 4 and within the number of cabinets "
                                 "(2 to 4)");
        return;
    }

    if (host == NULL || host[0] == '\0') {
        /* The host: a door for every other cabinet. */
        for (int other = 1; other <= cabinets; other++) {
            char name[32], where[32];
            if (other == me) {
                continue;
            }
            snprintf(name, sizeof(name), "xemu-link-%d", other);
            snprintf(where, sizeof(where), ":%d", port + other);
            if (!link_add_socket(name, "listen", where)) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Cabinet link: cannot listen on port %d "
                         "for cabinet %d", port + other, other);
                xemu_queue_error_message(msg);
            }
        }
        return;
    }

    link_retry_timer = timer_new_ms(QEMU_CLOCK_REALTIME, link_connect_retry, NULL);
    link_connect_retry(NULL);
}
