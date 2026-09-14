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
#ifndef CHIHIRO_CARD_READER_H
#define CHIHIRO_CARD_READER_H

#include <stdint.h>
#include <stdbool.h>

#define CARD_BLOCK_SIZE 8
#define CARD_TOTAL_SIZE 2048

typedef struct {
    /* A full-card WRITE command is 10 header bytes + 2048 data + checksum;
     * a short buffer silently truncates it, corrupts the card, and the
     * game's verify pass reports Write Failure / NG. */
    uint8_t rx_buf[CARD_TOTAL_SIZE + 32];
    int rx_pos;
    int rx_expected;

    uint8_t tx_buf[2048 + 8];
    int tx_len;
    int tx_pos;

    bool card_present;
    uint8_t card_data[CARD_TOTAL_SIZE];
    bool dirty;
    char card_path[512];
} CardReaderState;

void card_reader_init(CardReaderState *s);
void card_reader_insert(CardReaderState *s, const char *path);
void card_reader_remove(CardReaderState *s);
void card_reader_write_byte(CardReaderState *s, uint8_t byte);
void card_reader_tap_byte(CardReaderState *s, uint8_t byte);
int  card_reader_read(CardReaderState *s, uint8_t *buf, int max_len);
bool card_reader_has_response(CardReaderState *s);
void card_reader_flush(CardReaderState *s);

/* chihiro.c owns the two reader instances and exposes them to the SC (AN2131)
 * layer, which drives them through the real 8051 UART.
 * Index [0] = MIDI/UART1 reader, [1] = RS-232C/UART0 reader. */
extern CardReaderState *chihiro_card_reader_global;
/* True once a card game has enabled its readers: the SC MIDI (UART1) channel
 * then belongs to the card reader, not the OutRun 2 drive board. */
extern bool chihiro_card_reader_enabled;

#endif
