#ifndef CHIHIRO_8051_H
#define CHIHIRO_8051_H

#include <stdint.h>
#include <stdbool.h>

/* PSW bits */
#define PSW_CY  0x80
#define PSW_AC  0x40
#define PSW_F0  0x20
#define PSW_RS1 0x10
#define PSW_RS0 0x08
#define PSW_OV  0x04
#define PSW_F1  0x02
#define PSW_P   0x01

/* Standard SFR addresses */
#define SFR_P0   0x80
#define SFR_SP   0x81
#define SFR_DPL  0x82
#define SFR_DPH  0x83
#define SFR_PCON 0x87
#define SFR_TCON 0x88
#define SFR_TMOD 0x89
#define SFR_TL0  0x8A
#define SFR_TL1  0x8B
#define SFR_TH0  0x8C
#define SFR_TH1  0x8D
#define SFR_P1   0x90
#define SFR_SCON 0x98
#define SFR_SBUF 0x99
#define SFR_P2   0xA0
#define SFR_IE   0xA8
#define SFR_P3   0xB0
#define SFR_IP   0xB8
#define SFR_PSW  0xD0
#define SFR_ACC  0xE0
#define SFR_B    0xF0

typedef struct Cpu8051State Cpu8051State;

/* Callbacks for external memory (XDATA) — AN2131 registers plug in here */
typedef uint8_t (*Cpu8051XdataRead)(Cpu8051State *s, uint16_t addr);
typedef void    (*Cpu8051XdataWrite)(Cpu8051State *s, uint16_t addr, uint8_t val);

/* Callback for SFR access — lets AN2131 layer intercept chip-specific SFRs */
typedef uint8_t (*Cpu8051SfrRead)(Cpu8051State *s, uint8_t addr);
typedef void    (*Cpu8051SfrWrite)(Cpu8051State *s, uint8_t addr, uint8_t val);

struct Cpu8051State {
    /* Core registers */
    uint16_t pc;
    uint8_t  sp;
    uint8_t  acc;
    uint8_t  b;
    uint16_t dptr;
    uint16_t dptr_alt;  /* Second DPTR for dual-DPTR 8051 variants */
    uint8_t  psw;

    /* Internal RAM: 256 bytes
     *   0x00-0x7F: directly + indirectly addressable
     *   0x80-0xFF: indirectly addressable only (direct goes to SFR) */
    uint8_t iram[256];

    /* SFR space: 128 bytes (0x80-0xFF), direct addressing only.
     * Standard SFRs stored here; AN2131-specific ones forwarded via callback. */
    uint8_t sfr[128];

    /* Code memory — ic10 firmware loaded from disk */
    const uint8_t *code;
    uint32_t code_size;

    /* External data memory (XDATA) — 8KB on-chip + AN2131 registers */
    uint8_t xram[8192];

    /* Callbacks for XDATA access beyond on-chip RAM */
    Cpu8051XdataRead  xdata_read;
    Cpu8051XdataWrite xdata_write;

    /* Callbacks for SFR access (AN2131-specific registers) */
    Cpu8051SfrRead  sfr_read_cb;
    Cpu8051SfrWrite sfr_write_cb;

    /* Interrupt state */
    bool     halted;
    bool     in_interrupt;  /* Set by cpu8051_interrupt, cleared by RETI */
    uint64_t cycles;

    /* Timer 0 CLK/12 prescaler (ticks once per 3 machine cycles when CKCON.3=0) */
    uint8_t timer0_prescale;

    /* Opaque pointer for AN2131 layer */
    void *opaque;
};

void     cpu8051_init(Cpu8051State *s);
void     cpu8051_reset(Cpu8051State *s);
int      cpu8051_step(Cpu8051State *s);
void     cpu8051_interrupt(Cpu8051State *s, uint8_t vector);

#endif /* CHIHIRO_8051_H */
