#ifndef CHIHIRO_CARD_READER_H
#define CHIHIRO_CARD_READER_H

#include <stdint.h>
#include <stdbool.h>

#define CARD_BLOCK_SIZE 8
#define CARD_TOTAL_SIZE 2048

typedef struct {
    uint8_t rx_buf[256];
    int rx_pos;
    int rx_expected;

    uint8_t tx_buf[2048 + 8];
    int tx_len;
    int tx_pos;

    bool card_present;
    uint8_t card_data[CARD_TOTAL_SIZE];
    bool dirty;
    char card_path[512];

    uint8_t inject_buf[256];
    int inject_len;
    bool inject_pending;
    bool ignore_usb;
} CardReaderState;

void card_reader_init(CardReaderState *s);
void card_reader_insert(CardReaderState *s, const char *path);
void card_reader_remove(CardReaderState *s);
void card_reader_write_byte(CardReaderState *s, uint8_t byte);
void card_reader_tap_byte(CardReaderState *s, uint8_t byte);
int  card_reader_read(CardReaderState *s, uint8_t *buf, int max_len);
bool card_reader_has_response(CardReaderState *s);
void card_reader_flush(CardReaderState *s);

#endif
