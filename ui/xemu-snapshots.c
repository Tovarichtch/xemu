/*
 * xemu User Interface
 *
 * Copyright (C) 2020-2025 Matt Borgerson
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

#include "xemu-snapshots.h"
#include "xemu-settings.h"
#include "xemu-xbe.h"

#include <SDL3/SDL.h>
#include <epoxy/gl.h>

#include "block/aio.h"
#include "block/block_int.h"
#include "block/qapi.h"
#include "block/qdict.h"
#include "block/block-io.h"
#include "migration/qemu-file.h"
#include "migration/snapshot.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "system/runstate.h"

#include "ui/console.h"
#include "ui/input.h"
#include "hw/xbox/chihiro/chihiro.h"

static QEMUSnapshotInfo *xemu_snapshots_metadata = NULL;
static XemuSnapshotData *xemu_snapshots_extra_data = NULL;
static int xemu_snapshots_len = 0;
static bool xemu_snapshots_dirty = true;

const char **g_snapshot_shortcut_index_key_map[] = {
    &g_config.general.snapshots.shortcuts.f5,
    &g_config.general.snapshots.shortcuts.f6,
    &g_config.general.snapshots.shortcuts.f7,
    &g_config.general.snapshots.shortcuts.f8,
};

static bool xemu_chihiro_mode(void)
{
    return (int)g_config.sys.mem_limit >= 1;
}

/* The image the snapshots live in: the Chihiro store, else the Xbox disk. */
static const char *xemu_snapshots_image_path(void)
{
    return xemu_chihiro_mode() ? g_config.chihiro.roms.snapshot_store_path :
                                 g_config.sys.files.hdd_path;
}

static void xemu_snapshots_load_data(BlockDriverState *bs_ro,
                                     QEMUSnapshotInfo *info,
                                     XemuSnapshotData *data, Error **err)
{
    data->disc_path = NULL;
    data->xbe_title_name = NULL;
    data->gl_thumbnail = 0;

    int res = bdrv_snapshot_load_tmp(bs_ro, info->id_str, info->name, err);
    if (res < 0) {
        return;
    }

    uint32_t header[3];
    int64_t offset = 0;
    res = bdrv_load_vmstate(bs_ro, (uint8_t *)&header, offset, sizeof(header));
    if (res != sizeof(header)) {
        return;
    }
    offset += res;

    if (be32_to_cpu(header[0]) != XEMU_SNAPSHOT_DATA_MAGIC ||
        be32_to_cpu(header[1]) != XEMU_SNAPSHOT_DATA_VERSION) {
        return;
    }

    size_t size = be32_to_cpu(header[2]);
    uint8_t *buf = g_malloc(size);
    res = bdrv_load_vmstate(bs_ro, buf, offset, size);
    if (res != size) {
        g_free(buf);
        return;
    }

    assert(size >= 9);

    offset = 0;

    const size_t disc_path_size = be32_to_cpu(*(uint32_t *)&buf[offset]);
    offset += 4;

    if (disc_path_size) {
        data->disc_path = (char *)g_malloc(disc_path_size + 1);
        assert(size >= (offset + disc_path_size));
        memcpy(data->disc_path, &buf[offset], disc_path_size);
        data->disc_path[disc_path_size] = 0;
        offset += disc_path_size;
    }

    assert(size >= (offset + 4));
    const size_t xbe_title_name_size = buf[offset];
    offset += 1;

    if (xbe_title_name_size) {
        data->xbe_title_name = (char *)g_malloc(xbe_title_name_size + 1);
        assert(size >= (offset + xbe_title_name_size));
        memcpy(data->xbe_title_name, &buf[offset], xbe_title_name_size);
        data->xbe_title_name[xbe_title_name_size] = 0;
        offset += xbe_title_name_size;
    }

    const size_t thumbnail_size = be32_to_cpu(*(uint32_t *)&buf[offset]);
    offset += 4;

    if (thumbnail_size) {
        GLuint thumbnail;
        glGenTextures(1, &thumbnail);
        assert(size >= (offset + thumbnail_size));
        if (xemu_snapshots_load_png_to_texture(thumbnail, &buf[offset],
                                               thumbnail_size)) {
            data->gl_thumbnail = thumbnail;
        } else {
            glDeleteTextures(1, &thumbnail);
        }
        offset += thumbnail_size;
    }

    g_free(buf);
}

static void xemu_snapshots_all_load_data(QEMUSnapshotInfo **info,
                                         XemuSnapshotData **data,
                                         int snapshots_len, Error **err)
{
    BlockDriverState *bs_ro;
    QDict *opts = qdict_new();

    assert(info && data);

    if (*data) {
        for (int i = 0; i < xemu_snapshots_len; ++i) {
            g_free((*data)[i].xbe_title_name);
            if ((*data)[i].gl_thumbnail) {
                glDeleteTextures(1, &((*data)[i].gl_thumbnail));
            }
        }
        g_free(*data);
    }

    *data =
        (XemuSnapshotData *)g_malloc(sizeof(XemuSnapshotData) * snapshots_len);
    memset(*data, 0, sizeof(XemuSnapshotData) * snapshots_len);

    qdict_put_bool(opts, BDRV_OPT_READ_ONLY, true);
    bs_ro = bdrv_open(xemu_snapshots_image_path(), NULL, opts,
                      BDRV_O_RO_WRITE_SHARE | BDRV_O_AUTO_RDONLY, err);
    if (!bs_ro) {
        return;
    }

    for (int i = 0; i < snapshots_len; ++i) {
        xemu_snapshots_load_data(bs_ro, (*info) + i, (*data) + i, err);
        if (*err) {
            break;
        }
    }

    bdrv_flush(bs_ro);
    bdrv_drain(bs_ro);
    assert(bs_ro->refcnt == 1);
    bdrv_unref(bs_ro);
    if (!(*err))
        xemu_snapshots_dirty = false;
}

int xemu_snapshots_list(QEMUSnapshotInfo **info, XemuSnapshotData **extra_data,
                        Error **err)
{
    BlockDriverState *bs;
    int snapshots_len;
    assert(err);

    if (!xemu_snapshots_dirty && xemu_snapshots_extra_data &&
        xemu_snapshots_metadata) {
        goto done;
    }

    if (xemu_snapshots_metadata)
        g_free(xemu_snapshots_metadata);

    bs = bdrv_all_find_vmstate_bs(NULL, false, NULL, err);
    if (!bs) {
        return -1;
    }

    snapshots_len = bdrv_snapshot_list(bs, &xemu_snapshots_metadata);
    xemu_snapshots_all_load_data(&xemu_snapshots_metadata,
                                 &xemu_snapshots_extra_data, snapshots_len,
                                 err);
    if (*err) {
        return -1;
    }

    xemu_snapshots_len = snapshots_len;

done:
    if (info) {
        *info = xemu_snapshots_metadata;
    }

    if (extra_data) {
        *extra_data = xemu_snapshots_extra_data;
    }

    return xemu_snapshots_len;
}

char *xemu_get_currently_loaded_disc_path(void)
{
    char *file = NULL;
    BlockInfoList *block_list, *info;

    block_list = qmp_query_block(NULL);
    
    for (info = block_list; info; info = info->next) {
        if (strcmp("ide0-cd1", info->value->device)) {
            continue;
        }

        if (info->value->inserted && info->value->inserted->node_name) {
            file = g_strdup(info->value->inserted->file);
        }
    }

    qapi_free_BlockInfoList(block_list);
    return file;
}

/* The DIMM hooks can only fail with an errno; say why in the box. */
static void xemu_snapshots_chihiro_reason(Error **err, const char *what,
                                          const char *vm_name)
{
    const char *reason = chihiro_dimm_last_error();
    if (!err || !*err || !reason) {
        return;
    }
    error_free(*err);
    *err = NULL;
    error_setg(err, "Cannot %s snapshot '%s': %s.", what, vm_name, reason);
}

/* After a half-restored load the machine sits in restore-vm, from which
 * neither save-vm nor another restore is a legal transition (QEMU aborts). */
static bool xemu_snapshots_machine_unusable(Error **err)
{
    if (!runstate_is_running() && runstate_check(RUN_STATE_RESTORE_VM)) {
        error_setg(err, "The machine is unusable since a snapshot failed to "
                   "load: restart xemu.");
        return true;
    }
    return false;
}

void xemu_snapshots_load(const char *vm_name, Error **err)
{
    if (xemu_snapshots_machine_unusable(err)) {
        return;
    }
    bool vm_running = runstate_is_running();
    vm_stop(RUN_STATE_RESTORE_VM);
    load_snapshot(vm_name, NULL, false, NULL, err);
    if (err && *err && chihiro_dimm_last_error()) {
        /* The DIMM check sits late in the stream: RAM and devices are
         * already overwritten and QEMU cannot reset such a machine (the
         * PCI reset asserts). Stay stopped; the box says to restart. */
        error_free(*err);
        *err = NULL;
        error_setg(err, "Snapshot '%s' does not belong to the mounted game "
                   "and the machine is now unusable: restart xemu.", vm_name);
        return;
    }
    /* A load refused by validation leaves the machine untouched: resume
     * instead of staying paused forever. */
    if (vm_running) {
        vm_start();
    }
}

/* Whether a snapshot's DIMM delta was taken against the mounted image:
 * read from the snapshot's own state stream, before anything is restored.
 * Snapshots saved without a game name in their header need this; a
 * mismatch found later, by the DIMM hook, leaves a half-restored machine.
 * True on any doubt (no DIMM section found, image unreadable). */
bool xemu_snapshots_chihiro_image_matches(const char *vm_name)
{
    if (!xemu_chihiro_mode()) {
        return true;
    }
    Error *err = NULL;
    BlockDriverState *bs = bdrv_all_find_vmstate_bs(NULL, false, NULL, &err);
    if (!bs) {
        error_free(err);
        return true;
    }
    QEMUSnapshotInfo sn;
    if (bdrv_snapshot_find(bs, &sn, vm_name) < 0) {
        return true;
    }
    QDict *opts = qdict_new();
    qdict_put_bool(opts, BDRV_OPT_READ_ONLY, true);
    BlockDriverState *bs_ro = bdrv_open(xemu_snapshots_image_path(), NULL, opts,
                                        BDRV_O_RO_WRITE_SHARE | BDRV_O_AUTO_RDONLY,
                                        &err);
    if (!bs_ro) {
        error_free(err);
        return true;
    }
    bool match = true;
    if (bdrv_snapshot_load_tmp(bs_ro, sn.id_str, sn.name, &err) < 0) {
        error_free(err);
        goto out;
    }

    /* The section: idstr "chihiro-dimm", instance id, version id, then the
     * blob size and the blob itself, whose 28-byte header is CDIM, 1, size
     * (lo, hi), crc, page size, pages. */
    static const char idstr[] = "chihiro-dimm";
    const size_t chunk = 1 << 20, keep = 64;
    uint8_t *buf = g_malloc(chunk + keep);
    size_t carry = 0;
    int64_t offset = 0;
    for (;;) {
        int n = bdrv_load_vmstate(bs_ro, buf + carry, offset, chunk);
        if (n <= 0) {
            break;
        }
        offset += n;
        size_t avail = carry + n;
        uint8_t *hit = NULL;
        for (size_t i = 0; i + sizeof(idstr) - 1 <= avail; i++) {
            if (buf[i] == 'c' && memcmp(buf + i, idstr, sizeof(idstr) - 1) == 0) {
                hit = buf + i;
                break;
            }
        }
        if (hit && (size_t)(hit - buf) + sizeof(idstr) - 1 + 12 + 28 <= avail) {
            const uint8_t *p = hit + sizeof(idstr) - 1 + 4 + 4 + 4;
            uint32_t hdr[7];
            memcpy(hdr, p, sizeof(hdr));
            if (hdr[0] == 0x4344494d) {
                uint64_t snap_size = hdr[2] | (uint64_t)hdr[3] << 32;
                uint64_t cur_size;
                uint32_t cur_crc;
                if (chihiro_dimm_image_identity(&cur_size, &cur_crc)) {
                    match = snap_size == cur_size && hdr[4] == cur_crc;
                }
                break;
            }
        }
        if (avail > keep) {
            memmove(buf, buf + avail - keep, keep);
            carry = keep;
        } else {
            carry = avail;
        }
    }
    g_free(buf);
out:
    bdrv_flush(bs_ro);
    bdrv_drain(bs_ro);
    bdrv_unref(bs_ro);
    return match;
}

void xemu_snapshots_save(const char *vm_name, Error **err)
{
    if (xemu_snapshots_machine_unusable(err)) {
        return;
    }
    save_snapshot(vm_name, true, NULL, false, NULL, err);
    if (err && *err) {
        xemu_snapshots_chihiro_reason(err, "save", vm_name);
    }
}

void xemu_snapshots_delete(const char *vm_name, Error **err)
{
    delete_snapshot(vm_name, false, NULL, err);
}

void xemu_snapshots_save_extra_data(QEMUFile *f)
{
    /* Chihiro: the game is the netboot image, not a DVD, and the title is
     * the executable SEGABOOT launched. */
    char *path = xemu_chihiro_mode() ?
                     g_strdup(g_config.sys.files.dvd_path) :
                     xemu_get_currently_loaded_disc_path();
    size_t path_size = path ? strlen(path) : 0;

    size_t xbe_title_name_size = 0;
    char *xbe_title_name = NULL;
    struct xbe *xbe_data = xemu_chihiro_mode() ? NULL : xemu_get_xbe_info();
    if (xbe_data && xbe_data->cert) {
        glong items_written = 0;
        xbe_title_name = g_utf16_to_utf8(xbe_data->cert->m_title_name, 40, NULL, &items_written, NULL);
        if (xbe_title_name) {
            xbe_title_name_size = items_written;
        }
    } else if (xemu_chihiro_mode() && chihiro_game_filename[0]) {
        xbe_title_name = g_strdup(chihiro_game_filename);
        xbe_title_name_size = strlen(xbe_title_name);
    }

    size_t thumbnail_size = 0;
    void *thumbnail_buf = xemu_snapshots_create_framebuffer_thumbnail_png(&thumbnail_size);

    qemu_put_be32(f, XEMU_SNAPSHOT_DATA_MAGIC);
    qemu_put_be32(f, XEMU_SNAPSHOT_DATA_VERSION);
    qemu_put_be32(f, 4 + path_size + 1 + xbe_title_name_size + 4 + thumbnail_size);

    qemu_put_be32(f, path_size);
    if (path_size) {
        qemu_put_buffer(f, (const uint8_t *)path, path_size);
        g_free(path);
    }

    qemu_put_byte(f, xbe_title_name_size);
    if (xbe_title_name_size) {
        qemu_put_buffer(f, (const uint8_t *)xbe_title_name, xbe_title_name_size);
        g_free(xbe_title_name);
    }

    qemu_put_be32(f, thumbnail_size);
    if (thumbnail_size) {
        qemu_put_buffer(f, (const uint8_t *)thumbnail_buf, thumbnail_size);
        g_free(thumbnail_buf);
    }

    xemu_snapshots_dirty = true;
}

bool xemu_snapshots_offset_extra_data(QEMUFile *f)
{
    unsigned int v;
    uint32_t version;
    uint32_t size;

    v = qemu_get_be32(f);
    if (v != XEMU_SNAPSHOT_DATA_MAGIC) {
        qemu_file_skip(f, -4);
        return true;
    }

    version = qemu_get_be32(f);
    (void)version;

    /* qemu_file_skip only works if you aren't skipping past internal buffer limit.
     * Unfortunately, it's not usable here.
     */
    size = qemu_get_be32(f);
    void *buf = g_malloc(size);
    qemu_get_buffer(f, buf, size);
    g_free(buf);

    return true;
}

void xemu_snapshots_mark_dirty(void)
{
    xemu_snapshots_dirty = true;
}
