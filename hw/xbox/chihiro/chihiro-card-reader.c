/*
 * Chihiro card reader (Sega CRP-1231) emulation
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chihiro-card-reader.h"

static uint8_t compute_checksum(const uint8_t *buf, int len)
{
    uint8_t chk = 0;
    for (int i = 0; i < len; i++)
        chk ^= buf[i];
    return chk;
}

static void build_response(CardReaderState *s, uint8_t cmd)
{
    memset(s->tx_buf, 0, sizeof(s->tx_buf));
    s->tx_buf[0] = 0x10;
    s->tx_buf[1] = cmd;

    uint16_t payload_len = 0;

    switch (cmd) {
    case 0x10: /* INIT */
        payload_len = 0x02;
        s->tx_buf[5] = 0x20;
        break;

    case 0x11: /* GET STATUS / RESET */
        payload_len = 0x02;
        s->tx_buf[5] = 0x20;
        break;

    case 0x14: /* EJECT */
    case 0x15: /* STANDBY */
    case 0x26:
    case 0x27:
        payload_len = 0x02;
        break;

    case 0x20: /* DETECT CARD */
        payload_len = 0x02;
        if (!s->card_present)
            s->tx_buf[4] = 0x80;
        break;

    case 0x21: /* GET CARD UID */
        payload_len = 0x0A;
        s->tx_buf[6] = 0x81;
        s->tx_buf[8] = 0x59;
        s->tx_buf[9] = 0xDA;
        break;

    case 0x22: /* GET CARD TYPE */
        payload_len = 0x0A;
        s->tx_buf[7] = 0x08;
        break;

    case 0x33: /* GET CAPACITY */
        payload_len = 0x0A;
        s->tx_buf[6] = 0xFF;
        s->tx_buf[7] = 0xFF;
        break;

    case 0x34: { /* READ BLOCKS */
        uint16_t block_start = (s->rx_buf[6] << 8) | s->rx_buf[7];
        uint16_t block_count = (s->rx_buf[8] << 8) | s->rx_buf[9];
        uint32_t byte_offset = block_start * CARD_BLOCK_SIZE;
        uint32_t byte_count = block_count * CARD_BLOCK_SIZE;
        if (byte_offset + byte_count <= CARD_TOTAL_SIZE &&
            byte_count + 6 < sizeof(s->tx_buf)) {
            payload_len = byte_count + 2;
            memcpy(&s->tx_buf[6], &s->card_data[byte_offset], byte_count);
        } else {
            payload_len = 0x02;
            s->tx_buf[4] = 0x80;
            fprintf(stderr, "[CARD] READ out of bounds!\n");
        }
        break;
    }

    case 0x35: { /* WRITE BLOCKS */
        uint16_t block_start = (s->rx_buf[6] << 8) | s->rx_buf[7];
        uint16_t block_count = (s->rx_buf[8] << 8) | s->rx_buf[9];
        uint32_t byte_offset = block_start * CARD_BLOCK_SIZE;
        uint32_t byte_count = block_count * CARD_BLOCK_SIZE;
        if (byte_offset + byte_count <= CARD_TOTAL_SIZE &&
            byte_count + 10 <= (uint32_t)sizeof(s->rx_buf)) {
            memcpy(&s->card_data[byte_offset], &s->rx_buf[10], byte_count);
            s->dirty = true;
        }
        payload_len = 0x02;
        break;
    }

    default:
        payload_len = 0x02;
        break;
    }

    s->tx_buf[2] = (uint8_t)(payload_len >> 8);
    s->tx_buf[3] = (uint8_t)(payload_len & 0xFF);
    s->tx_len = payload_len + 5;
    s->tx_pos = 0;
    s->tx_buf[s->tx_len - 1] = compute_checksum(s->tx_buf, s->tx_len - 1);
}

void card_reader_init(CardReaderState *s)
{
    memset(s, 0, sizeof(*s));
}

void card_reader_insert(CardReaderState *s, const char *path)
{
    if (!path || !path[0]) return;

    snprintf(s->card_path, sizeof(s->card_path), "%s", path);

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(s->card_data, 1, CARD_TOTAL_SIZE, f);
        fclose(f);
        if (n == CARD_TOTAL_SIZE) {
            s->card_present = true;
            s->dirty = false;
            return;
        }
    }

    /* Assigned file missing or short: insert a fresh card and create the
     * file. A fresh card from the reader's stock carries the factory
     * metadata header (blocks 4-7, big-endian) with an empty game-data
     * area: the game validates that header first, then finds no game
     * structure and treats the card as NEW, offering registration. An
     * all-zero card would fail the header check and be rejected. */
    memset(s->card_data, 0, CARD_TOTAL_SIZE);
    s->card_data[32] = 0x95;
    s->card_data[33] = 0x71;
    s->card_data[34] = 0x36;
    s->card_data[35] = 0x40;
    s->card_data[54] = 0x03;
    s->card_data[55] = 0xF2;
    s->card_present = true;
    s->dirty = true;
    card_reader_flush(s);
}

void card_reader_remove(CardReaderState *s)
{
    if (s->dirty)
        card_reader_flush(s);
    s->card_present = false;
    s->rx_pos = 0;
    s->rx_expected = 0;
    s->tx_len = 0;
    s->tx_pos = 0;
}

void card_reader_write_byte(CardReaderState *s, uint8_t byte)
{
    card_reader_tap_byte(s, byte);
}

void card_reader_tap_byte(CardReaderState *s, uint8_t byte)
{
    /* SC firmware uses 0x00 as sync byte (not 0x10 like standalone CRP-1231) */
    if (s->rx_pos == 0 && byte != 0x00)
        return;

    if (s->rx_pos == 1 && byte == 0x00) {
        s->rx_pos = 0;
        return;
    }

    if (s->rx_pos >= (int)sizeof(s->rx_buf))
        s->rx_pos = 0;

    s->rx_buf[s->rx_pos++] = byte;

    if (s->rx_pos == 4) {
        s->rx_expected = ((s->rx_buf[2] << 8) | s->rx_buf[3]) + 5;
        if (s->rx_expected > (int)sizeof(s->rx_buf))
            s->rx_expected = sizeof(s->rx_buf);
    }

    if (s->rx_pos >= 5 && s->rx_pos == s->rx_expected) {
        build_response(s, s->rx_buf[1]);

        s->rx_pos = 0;
        s->rx_expected = 0;

        if (s->dirty)
            card_reader_flush(s);
    }
}

int card_reader_read(CardReaderState *s, uint8_t *buf, int max_len)
{
    int avail = s->tx_len - s->tx_pos;
    if (avail <= 0) return 0;

    int n = avail < max_len ? avail : max_len;
    memcpy(buf, &s->tx_buf[s->tx_pos], n);
    s->tx_pos += n;
    return n;
}

bool card_reader_has_response(CardReaderState *s)
{
    return s->tx_pos < s->tx_len;
}

void card_reader_flush(CardReaderState *s)
{
    if (!s->card_path[0]) return;

    FILE *f = fopen(s->card_path, "wb");
    if (f) {
        fwrite(s->card_data, 1, CARD_TOTAL_SIZE, f);
        fclose(f);
        s->dirty = false;
    }
}
