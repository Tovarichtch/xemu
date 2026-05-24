#include <stdio.h>
#include <string.h>
#include "chihiro-card-reader.h"

static uint32_t card_crc32(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc;
}

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
        fprintf(stderr, "[CARD] READ blk=%d+%d (off=0x%X len=%d)\n",
                block_start, block_count, byte_offset, byte_count);
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

    memset(s->card_data, 0, CARD_TOTAL_SIZE);

    /* Card byte layout (81 blocks × 8 bytes = 648 bytes read by game):
     *   [0x00-0x1F]  blocks 0-3: Mifare overhead (block 3 skipped)
     *   [0x20-0x3F]  blocks 4-7: card metadata (FUN_0002b8b0 validates)
     *   [0x40-0x13F] blocks 8-39: SBHU slot 0 (256 bytes, FUN_0002b940)
     *   [0x140-0x23F] blocks 40-71: SBHU slot 1 (mirror)
     */

    /* Blocks 4-7: card metadata, read big-endian by FUN_0002b8b0 */
    s->card_data[32] = 0x95;
    s->card_data[33] = 0x71;
    s->card_data[34] = 0x36;
    s->card_data[35] = 0x40;
    /* bytes 36-39 = 0 (required for version ≥ 1000 pass) */
    s->card_data[52] = 0x00;
    s->card_data[53] = 0x00;
    s->card_data[54] = 0x03;
    s->card_data[55] = 0xF2;

    /* Blocks 8+: SBHU structure at offset 64 (FUN_0002b740 layout) */
    uint32_t *d = (uint32_t *)(s->card_data + 64);
    d[0]    = 0x55484253;  /* "SBHU" magic */
    d[1]    = 0x000003F2;  /* format version (1010) */
    d[2]    = 0x4754584E;  /* "NXTG" game ID */
    d[3]    = 0x00000100;  /* data length (256) */
    d[4]    = 0x00010001;  /* card ID 1 */
    d[5]    = 0x00020002;  /* card ID 2 */
    d[9]    = 100;         /* initial credits/rank */
    d[0x1d] = 2;           /* costume default */
    d[0x1e] = 0x11;        /* costume count */
    d[0x34] = 2;           /* weapon default */
    d[0x35] = 0x11;        /* weapon count */
    s->card_data[64 + 0x6d] = 0xFF;  /* name marker */

    /* CRC-32 over SBHU slot 0 (252 bytes at offset 64), stored at offset 64+0xFC */
    uint32_t crc = card_crc32(s->card_data + 64, 0xFC);
    memcpy(s->card_data + 64 + 0xFC, &crc, 4);
    /* Slot 1 = mirror of slot 0 */
    memcpy(s->card_data + 64 + 0x100, s->card_data + 64, 0x100);

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
    if (s->ignore_usb) return;
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
        fprintf(stderr, "[CARD] RX cmd: ");
        for (int i = 0; i < s->rx_pos && i < 16; i++)
            fprintf(stderr, "%02X ", s->rx_buf[i]);
        fprintf(stderr, "(%d bytes)\n", s->rx_pos);

        build_response(s, s->rx_buf[1]);

        if (s->tx_len > 0 && s->tx_len <= (int)sizeof(s->inject_buf)) {
            memcpy(s->inject_buf, s->tx_buf, s->tx_len);
            s->inject_len = s->tx_len;
            s->inject_pending = true;

            fprintf(stderr, "[CARD] TX rsp: ");
            for (int i = 0; i < s->tx_len && i < 16; i++)
                fprintf(stderr, "%02X ", s->tx_buf[i]);
            if (s->tx_len > 16) fprintf(stderr, "...");
            fprintf(stderr, "(%d bytes)\n", s->tx_len);
        }

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
