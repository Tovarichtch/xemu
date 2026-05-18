/*
 * Cypress AN2131 (EZ-USB) register mapping for Chihiro baseboard.
 *
 * Maps XDATA-space USB registers, I2C bus master, and port I/O
 * to the generic 8051 CPU core. Runs the ic10/pc20 firmware natively
 * to handle USB vendor requests, EEPROM I2C, JVS relay, and
 * ACBU/SBHQ security handshakes without per-game HLE.
 *
 * Register addresses from Cypress EZ-USB TRM v1.9/1.10.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "chihiro-an2131.h"
#include "chihiro-jvs.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── helpers ──────────────────────────────────────────────────────── */

static inline int min_int(int a, int b) { return a < b ? a : b; }
static void jvs_rx_deliver(AN2131State *s);

static inline uint16_t canon(uint16_t addr)
{
    if (addr >= 0x1B40 && addr <= 0x1FFF)
        return addr + 0x6000;
    return addr;
}

/* ── I2C state machine ────────────────────────────────────────────── */

static void i2c_fire_done(AN2131State *s, bool ack)
{
    s->i2cs = I2CS_DONE | (ack ? I2CS_ACK : 0);
    if (s->i2c_lastrd) s->i2cs |= I2CS_LASTRD;
    s->i2c_irq_pending = true;
}

static void i2c_select_device(AN2131State *s, uint8_t addr7)
{
    s->i2c.slave_addr = addr7;
    if (addr7 == 0x50 && s->ic10_eeprom) {
        s->i2c.eeprom = s->ic10_eeprom;
        s->i2c.eeprom_size = s->ic10_size;
        s->i2c.addr_bytes_needed = 2;   /* 24LC64: 2-byte address */
    } else if (addr7 == 0x51 && s->ic10_eeprom) {
        s->i2c.eeprom = s->ic10_eeprom;
        s->i2c.eeprom_size = s->ic10_size;
        s->i2c.addr_bytes_needed = 2;   /* ic10 24LC64: A0=1 on baseboard → addr 0x51 */
    } else if (addr7 == 0x55 && s->ic11_eeprom) {
        s->i2c.eeprom = s->ic11_eeprom;
        s->i2c.eeprom_size = s->ic11_size;
        s->i2c.addr_bytes_needed = 1;   /* baseboard data EEPROM (same format as ic11) */
    } else if (addr7 == 0x32) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        #define TO_BCD(v) (uint8_t)(((v)/10 << 4) | ((v)%10))
        s->rtc_regs[0] = TO_BCD(t->tm_sec);
        s->rtc_regs[1] = TO_BCD(t->tm_min);
        s->rtc_regs[2] = TO_BCD(t->tm_hour);
        s->rtc_regs[3] = t->tm_wday + 1;
        s->rtc_regs[4] = TO_BCD(t->tm_mday);
        s->rtc_regs[5] = TO_BCD(t->tm_mon + 1);
        s->rtc_regs[6] = TO_BCD(t->tm_year % 100);
        #undef TO_BCD
        s->i2c.eeprom = s->rtc_regs;
        s->i2c.eeprom_size = 16;
        s->i2c.addr_bytes_needed = 1;
    } else {
        s->i2c.eeprom = NULL;
        s->i2c.eeprom_size = 0;
        s->i2c.addr_bytes_needed = 0;
    }
}

static void i2c_dat_write(AN2131State *s, uint8_t val)
{
    switch (s->i2c.phase) {
    case I2C_ADDR: {
        uint8_t addr7 = val >> 1;
        s->i2c.reading = val & 1;
        i2c_select_device(s, addr7);
        s->i2c.addr_bytes_sent = 0;
        if (!s->i2c.eeprom) {
            fprintf(stderr, "[AN2131] I2C NACK — unknown device 0x%02X\n", addr7);
            i2c_fire_done(s, false);
            return;
        }
        s->i2c.phase = s->i2c.reading ? I2C_DATA : I2C_MEM_ADDR;
        s->i2c.first_read = s->i2c.reading;
        i2c_fire_done(s, true);
        break;
    }
    case I2C_MEM_ADDR:
        if (s->i2c.addr_bytes_needed == 2 && s->i2c.addr_bytes_sent == 0) {
            s->i2c.mem_addr = (uint16_t)val << 8;
        } else if (s->i2c.addr_bytes_needed == 2) {
            s->i2c.mem_addr |= val;
        } else {
            s->i2c.mem_addr = val;
        }
        s->i2c.addr_bytes_sent++;
        if (s->i2c.addr_bytes_sent >= s->i2c.addr_bytes_needed)
            s->i2c.phase = I2C_DATA;
        i2c_fire_done(s, true);
        break;
    case I2C_DATA:
        if (s->i2c.eeprom && s->i2c.mem_addr < (uint16_t)s->i2c.eeprom_size) {
            s->i2c.eeprom[s->i2c.mem_addr] = val;
        }
        s->i2c.mem_addr++;
        i2c_fire_done(s, true);
        break;
    default:
        if (s->i2c.phase != I2C_IDLE) {
            i2c_fire_done(s, false);
        }
        break;
    }
}

static uint8_t i2c_dat_read(AN2131State *s)
{
    uint8_t val = 0xFF;
    if (s->i2c.first_read) {
        s->i2c.first_read = false;
        i2c_fire_done(s, true);
        return val;
    }
    if (s->i2c.reading && s->i2c.eeprom &&
        s->i2c.mem_addr < (uint16_t)s->i2c.eeprom_size) {
        val = s->i2c.eeprom[s->i2c.mem_addr];
        s->i2c.mem_addr++;
    }
    if (s->i2c.phase != I2C_IDLE) {
        i2c_fire_done(s, !s->i2c_lastrd);
    }
    return val;
}

/* ── XDATA read callback ─────────────────────────────────────────── */

static uint8_t an2131_xdata_read(Cpu8051State *cpu, uint16_t addr)
{
    AN2131State *s = (AN2131State *)cpu->opaque;

    if (addr < 0x1B40)
        return s->ram[addr];

    uint16_t ca = canon(addr);

    /* Endpoint buffers: 0x7B40-0x7F3F → ram[0x1B40-0x1F3F] */
    if (ca >= 0x7B40 && ca < 0x7F40)
        return s->ram[ca - 0x6000];

    /* Register space: 0x7F40-0x7FFF */
    if (ca >= 0x7F40 && ca <= 0x7FFF) {
        switch (ca) {
        case AN_CPUCS:    return s->cpucs;

        case AN_PORTACFG: return s->portacfg;
        case AN_PORTBCFG: return s->portbcfg;
        case AN_PORTCCFG: return s->portccfg;
        case AN_OUTA:     return s->outa;
        case AN_OUTB:     return s->outb;
        case AN_OUTC:     return s->outc;
        case AN_PINSA:    return s->pinsa;
        case AN_PINSB: {

            uint8_t sense = chihiro_jvs_global ? (chihiro_jvs_global->sense & 0x03) : 0x03;
            return (s->pinsb & ~0x03) | sense;
        }
        case AN_PINSC:    return s->pinsc;
        case AN_OEA:      return s->oea;
        case AN_OEB:      return s->oeb;
        case AN_OEC:      return s->oec;

        case AN_I2CS:     return s->i2cs;
        case AN_I2DAT:    return i2c_dat_read(s);

        case AN_IVEC:     return s->ivec;
        case AN_IN07IRQ:  return s->in07irq;
        case AN_OUT07IRQ: return s->out07irq;
        case AN_USBIRQ:   return s->usbirq;
        case AN_IN07IEN:  return s->in07ien;
        case AN_OUT07IEN: return s->out07ien;
        case AN_USBIEN:   return s->usbien;
        case AN_USBBAV:   return s->usbbav;

        case AN_EP0CS:    return s->ep0cs;
        case AN_IN0BC:    return s->ep[0].bc_in;
        case AN_OUT0BC:   return s->ep[0].bc_out;

        case AN_SUDPTRH:  return (uint8_t)(s->sudptr >> 8);
        case AN_SUDPTRL:  return (uint8_t)s->sudptr;
        case AN_USBCS:    return s->usbcs;
        case AN_TOGCTL:   return s->togctl;
        case AN_FNADDR:   return s->fnaddr;
        case AN_USBPAIR:  return s->usbpair;
        case AN_IN07VAL:  return s->in07val;
        case AN_OUT07VAL: return s->out07val;

        case AN_FASTXFR:  return s->fastxfr;
        case AN_AUTOPTRH: return (uint8_t)(s->autoptr >> 8);
        case AN_AUTOPTRL: return (uint8_t)s->autoptr;
        case AN_AUTODATA: {
            uint8_t v = an2131_xdata_read(cpu, s->autoptr);
            s->autoptr++;
            return v;
        }

        case AN_SETUPDAT:     case AN_SETUPDAT + 1:
        case AN_SETUPDAT + 2: case AN_SETUPDAT + 3:
        case AN_SETUPDAT + 4: case AN_SETUPDAT + 5:
        case AN_SETUPDAT + 6: case AN_SETUPDAT + 7:
            return s->setupdat[ca - AN_SETUPDAT];

        default:
            break;
        }

        /* EP1-7 IN CS/BC: 0x7FB6-0x7FC3 */
        if (ca >= 0x7FB6 && ca <= 0x7FC3) {
            int n = (ca - 0x7FB6) / 2 + 1;
            return ((ca - 0x7FB6) & 1) ? s->ep[n].bc_in : s->ep[n].cs_in;
        }
        /* EP1-7 OUT CS/BC: 0x7FC6-0x7FD3 */
        if (ca >= 0x7FC6 && ca <= 0x7FD3) {
            int n = (ca - 0x7FC6) / 2 + 1;
            return ((ca - 0x7FC6) & 1) ? s->ep[n].bc_out : s->ep[n].cs_out;
        }
        return 0xFF;
    }

    /* External SRAM: 0x2000-0x7B3F and 0x8000-0xFFFF */
    if (s->extmem)
        return s->extmem[addr];
    return 0xFF;
}

/* ── XDATA write callback ────────────────────────────────────────── */

static void an2131_xdata_write(Cpu8051State *cpu, uint16_t addr, uint8_t val)
{
    AN2131State *s = (AN2131State *)cpu->opaque;

    if (addr < 0x1B40) { s->ram[addr] = val; return; }

    uint16_t ca = canon(addr);

    /* Endpoint buffers */
    if (ca >= 0x7B40 && ca < 0x7F40) {
        s->ram[ca - 0x6000] = val;
        return;
    }

    /* Register space: 0x7F40-0x7FFF */
    if (ca >= 0x7F40 && ca <= 0x7FFF) {
        switch (ca) {
        case AN_CPUCS:    s->cpucs = val; return;

        case AN_PORTACFG: s->portacfg = val; return;
        case AN_PORTBCFG: s->portbcfg = val; return;
        case AN_PORTCCFG: s->portccfg = val; return;
        case AN_OUTA:     s->outa = val; return;
        case AN_OUTB:     s->outb = val; return;
        case AN_OUTC:     s->outc = val; return;
        case AN_OEA:      s->oea = val; return;
        case AN_OEB:      s->oeb = val; return;
        case AN_OEC:      s->oec = val; return;

        /* I2C */
        case AN_I2CS: {
            if (val & I2CS_START) s->i2c.phase = I2C_ADDR;
            if (val & I2CS_STOP) {
                s->i2c.phase = I2C_IDLE;
                s->i2c_lastrd = false;
            } else {
                s->i2c_lastrd = !!(val & I2CS_LASTRD);
            }
            return;
        }
        case AN_I2DAT:
            s->i2dat = val;
            i2c_dat_write(s, val);
            return;

        /* Interrupt registers — write-1-to-clear for IRQ, direct for IEN */
        case AN_IVEC:     return;
        case AN_IN07IRQ:  s->in07irq  &= ~val; return;
        case AN_OUT07IRQ: s->out07irq &= ~val; return;
        case AN_USBIRQ:   s->usbirq   &= ~val; return;
        case AN_IN07IEN:  s->in07ien  = val; return;
        case AN_OUT07IEN: s->out07ien = val; return;
        case AN_USBIEN:   s->usbien   = val; return;
        case AN_USBBAV:   s->usbbav   = val; return;

        /* EP0 */
        case AN_EP0CS:
            s->ep0cs = val;
            return;
        case AN_IN0BC:
            s->ep[0].bc_in = val;
            s->ep[0].in_armed = true;
            s->ep0cs |= EP0CS_INBSY;
            return;
        case AN_OUT0BC:
            s->ep[0].bc_out = 0;
            s->ep0cs &= ~EP0CS_OUTBSY;
            return;

        /* USB global */
        case AN_SUDPTRH: s->sudptr = (s->sudptr & 0x00FF) | ((uint16_t)val << 8); return;
        case AN_SUDPTRL: s->sudptr = (s->sudptr & 0xFF00) | val; return;
        case AN_USBCS:   s->usbcs = val; return;
        case AN_TOGCTL:  s->togctl = val; return;
        case AN_FNADDR:  s->fnaddr = val; return;
        case AN_USBPAIR: s->usbpair = val; return;
        case AN_IN07VAL: s->in07val = val; return;
        case AN_OUT07VAL: s->out07val = val; return;

        /* Autopointer */
        case AN_FASTXFR:  s->fastxfr = val; return;
        case AN_AUTOPTRH: s->autoptr = (s->autoptr & 0x00FF) | ((uint16_t)val << 8); return;
        case AN_AUTOPTRL: s->autoptr = (s->autoptr & 0xFF00) | val; return;
        case AN_AUTODATA:
            an2131_xdata_write(cpu, s->autoptr, val);
            s->autoptr++;
            return;

        /* SETUPDAT is read-only from firmware side */
        case AN_SETUPDAT:     case AN_SETUPDAT + 1:
        case AN_SETUPDAT + 2: case AN_SETUPDAT + 3:
        case AN_SETUPDAT + 4: case AN_SETUPDAT + 5:
        case AN_SETUPDAT + 6: case AN_SETUPDAT + 7:
            return;

        default:
            break;
        }

        /* EP1-7 IN CS/BC */
        if (ca >= 0x7FB6 && ca <= 0x7FC3) {
            int n = (ca - 0x7FB6) / 2 + 1;
            if ((ca - 0x7FB6) & 1) {
                s->ep[n].bc_in = val;
                s->ep[n].in_armed = true;
                s->ep[n].cs_in |= EPCS_BSY;
                if (n == 4) s->diag_ep4_arms++;
            } else {
                s->ep[n].cs_in = val;
            }
            return;
        }
        /* EP1-7 OUT CS/BC */
        if (ca >= 0x7FC6 && ca <= 0x7FD3) {
            int n = (ca - 0x7FC6) / 2 + 1;
            if ((ca - 0x7FC6) & 1) {
                s->ep[n].bc_out = 0;
                s->ep[n].cs_out &= ~EPCS_BSY;
            } else {
                s->ep[n].cs_out = val;
            }
            return;
        }
        return;
    }

    /* External SRAM: 0x2000-0x7B3F and 0x8000-0xFFFF */
    if (s->extmem) {
        s->extmem[addr] = val;
    }
}

/* ── SFR callbacks (AN2131-specific registers) ────────────────────── */

static uint8_t an2131_sfr_read(Cpu8051State *cpu, uint8_t addr)
{
    AN2131State *s = (AN2131State *)cpu->opaque;
    switch (addr) {
    case SFR_EXIF:  return s->exif;
    case SFR_MPAGE: return s->mpage;
    case SFR_DPS:   return s->dps;
    case SFR_EIE:   return s->eie;
    case SFR_EIP:   return s->eip;
    default:        return cpu->sfr[addr - 0x80];
    }
}

static void an2131_sfr_write(Cpu8051State *cpu, uint8_t addr, uint8_t val)
{
    AN2131State *s = (AN2131State *)cpu->opaque;
    switch (addr) {
    case SFR_EXIF:
        s->exif = val;
        return;
    case SFR_MPAGE:
        s->mpage = val;
        return;
    case SFR_DPS: {
        uint8_t old = s->dps;
        s->dps = val & 0x01;
        if ((val & 1) != (old & 1)) {
            uint16_t tmp = cpu->dptr;
            cpu->dptr = cpu->dptr_alt;
            cpu->dptr_alt = tmp;
        }
        return;
    }
    case SFR_EIE:
        s->eie = val;
        return;
    case SFR_EIP:
        s->eip = val;
        return;
    case 0x99: /* SBUF0 — serial TX (channel 0, unused for JVS) */
        cpu->sfr[addr - 0x80] = val;
        cpu->sfr[0x98 - 0x80] |= 0x02;  /* set TI */
        return;
    case 0xC0: /* SCON1 — defer next RX byte until after RETI */
    {
        uint8_t old = cpu->sfr[addr - 0x80];
        cpu->sfr[addr - 0x80] = val;
        if ((old & 0x01) && !(val & 0x01)) {
            s->jvs_rx_pending = true;
        }
        return;
    }
    case 0xC1: /* SBUF1 — serial TX (channel 1, JVS) */
    {
        s->diag_sbuf1_writes++;
        cpu->sfr[0xC0 - 0x80] |= 0x02;  /* set TI1 — byte "sent" */

        /* RS-485 unescape: firmware escapes 0xE0→{0xD0,0xDF}, 0xD0→{0xD0,0xCF} */
        if (val == 0xE0) {
            if (s->jvs_tx_len > 0) {
                s->jvs_tx_len = 0;
                s->jvs_tx_expected = 0;
            }
            s->jvs_tx_escape = false;
            s->jvs_tx_buf[s->jvs_tx_len++] = val;
            return;
        }
        if (s->jvs_tx_escape) {
            val = val + 1;
            s->jvs_tx_escape = false;
        } else if (val == 0xD0) {
            s->jvs_tx_escape = true;
            return;
        }

        if (s->jvs_tx_len < (int)sizeof(s->jvs_tx_buf)) {
            s->jvs_tx_buf[s->jvs_tx_len++] = val;
        }
        if (s->jvs_tx_len == 3 && s->jvs_tx_buf[0] == 0xE0) {
            s->jvs_tx_expected = 3 + s->jvs_tx_buf[2];
        }

        if (s->jvs_tx_expected > 0 && s->jvs_tx_len >= s->jvs_tx_expected) {
            s->diag_jvs_tx++;

            if (chihiro_jvs_global) {
                uint8_t raw_resp[256];
                int rlen = chihiro_jvs_process(chihiro_jvs_global,
                                               s->jvs_tx_buf, s->jvs_tx_expected,
                                               raw_resp, sizeof(raw_resp));
                if (rlen > 0) {
                    /* RS-485 escape: bytes after SYNC that are 0xE0 or 0xD0 */
                    int epos = 0;
                    s->jvs_rx_buf[epos++] = raw_resp[0]; /* SYNC */
                    for (int i = 1; i < rlen && epos < (int)sizeof(s->jvs_rx_buf) - 1; i++) {
                        if (raw_resp[i] == 0xE0 || raw_resp[i] == 0xD0) {
                            s->jvs_rx_buf[epos++] = 0xD0;
                            s->jvs_rx_buf[epos++] = raw_resp[i] - 1;
                        } else {
                            s->jvs_rx_buf[epos++] = raw_resp[i];
                        }
                    }
                    s->jvs_rx_len = epos;
                    s->jvs_rx_pos = 0;
                    s->diag_jvs_rx++;
                    s->jvs_response_ready = true;
                    s->jvs_response_set_cycles = s->total_cycles;
                }
            }
            s->jvs_tx_len = 0;
            s->jvs_tx_expected = 0;
        }
        return;
    }
    default:
        cpu->sfr[addr - 0x80] = val;
        return;
    }
}

/* ── Interrupt logic ──────────────────────────────────────────────── */

static bool usb_irq_pending(AN2131State *s)
{
    if (s->usbirq & s->usbien) return true;
    if (s->in07irq & s->in07ien) return true;
    if (s->out07irq & s->out07ien) return true;
    return false;
}

static uint8_t usb_avec(AN2131State *s)
{
    /* USBIRQ sources (highest priority) */
    if (s->usbirq & s->usbien & USBIRQ_SUDAV)  return AVEC_SUDAV;
    if (s->usbirq & s->usbien & USBIRQ_SOF)    return AVEC_SOF;
    if (s->usbirq & s->usbien & USBIRQ_SUTOK)  return AVEC_SUTOK;
    if (s->usbirq & s->usbien & USBIRQ_SUSP)   return AVEC_SUSPEND;
    if (s->usbirq & s->usbien & USBIRQ_URES)   return AVEC_USBRESET;

    /* IN endpoint IRQs */
    for (int i = 0; i < 8; i++) {
        if (s->in07irq & s->in07ien & (1 << i))
            return (uint8_t)(AVEC_EP0IN + i * 8);
    }
    /* OUT endpoint IRQs */
    for (int i = 0; i < 8; i++) {
        if (s->out07irq & s->out07ien & (1 << i))
            return (uint8_t)(AVEC_EP0OUT + i * 8);
    }
    return 0xFF;
}

static void check_interrupts(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;

    /* Need global interrupt enable (IE.EA = bit 7) */
    if (!(cpu->sfr[SFR_IE - 0x80] & 0x80)) return;

    /* Don't nest interrupts */
    if (cpu->in_interrupt) return;

    /* Timer 0 overflow (TF0 = TCON.5), enabled by IE.1 (ET0) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x02) &&
        (cpu->sfr[SFR_TCON - 0x80] & 0x20)) {
        cpu->sfr[SFR_TCON - 0x80] &= ~0x20;  /* clear TF0 */
        s->diag_t0_overflows++;
        cpu8051_interrupt(cpu, 0x000B);
        return;
    }

    /* Timer 1 overflow (TF1 = TCON.7), enabled by IE.3 (ET1) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x08) &&
        (cpu->sfr[SFR_TCON - 0x80] & 0x80)) {
        cpu->sfr[SFR_TCON - 0x80] &= ~0x80;  /* clear TF1 */
        s->diag_t1_overflows++;
        cpu8051_interrupt(cpu, 0x001B);
        return;
    }

    /* Serial interrupt (RI|TI in SCON), enabled by IE.4 (ES) */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x10) &&
        (cpu->sfr[SFR_SCON - 0x80] & 0x03)) {
        s->diag_serial0_irqs++;
        cpu8051_interrupt(cpu, 0x0023);
        return;
    }

    /* Second serial channel (SFR 0xC0 bits 0-1), enabled by IE.6 */
    if ((cpu->sfr[SFR_IE - 0x80] & 0x40) &&
        (cpu->sfr[0xC0 - 0x80] & 0x03)) {
        s->diag_serial1_irqs++;
        cpu8051_interrupt(cpu, 0x003B);
        return;
    }

    /* USB interrupt (INT2), enabled by EIE.0 */
    if ((s->eie & 0x01) && usb_irq_pending(s)) {
        uint8_t avec = usb_avec(s);
        if (avec != 0xFF) {
            s->ivec = avec;
            /* Autovector: patch the LJMP target low byte at code[0x0045] */
            if (s->usbbav & USBBAV_AVEN) {
                s->ram[0x0045] = avec;
            }
            s->exif |= 0x10;
            s->diag_usb_irqs++;
            cpu8051_interrupt(cpu, INT2_VECTOR);
            return;
        }
    }

    /* I2C interrupt (INT3), enabled by EIE.1 */
    if ((s->eie & 0x02) && s->i2c_irq_pending) {
        s->i2c_irq_pending = false;
        s->exif |= 0x20;
        s->diag_i2c_irqs++;
        cpu8051_interrupt(cpu, INT3_VECTOR);
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void an2131_init(AN2131State *s)
{
    memset(s, 0, sizeof(*s));
    cpu8051_init(&s->cpu);

    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;
    s->cpu.xdata_read = an2131_xdata_read;
    s->cpu.xdata_write = an2131_xdata_write;
    s->cpu.sfr_read_cb = an2131_sfr_read;
    s->cpu.sfr_write_cb = an2131_sfr_write;
    s->cpu.opaque = s;

    an2131_reset(s);
}

void an2131_reset(AN2131State *s)
{
    cpu8051_reset(&s->cpu);

    s->cpu.code = s->ram;
    s->cpu.code_size = AN2131_RAM_SIZE;

    s->cpucs = CPUCS_8051RES;
    s->cpu_running = false;

    s->ep0cs = 0;
    s->usbbav = 0;
    s->usbcs = 0;
    s->usbpair = 0;
    s->in07val = 0x01;
    s->out07val = 0x01;
    s->fnaddr = 0;
    s->sudptr = 0;
    s->autoptr = 0;
    s->fastxfr = 0;
    s->togctl = 0;

    s->usbirq = 0;
    s->in07irq = 0;
    s->out07irq = 0;
    s->usbien = 0;
    s->in07ien = 0;
    s->out07ien = 0;
    s->ivec = 0;

    s->i2cs = I2CS_DONE;
    s->i2dat = 0;
    s->i2c_irq_pending = false;
    s->i2c_lastrd = false;
    memset(&s->i2c, 0, sizeof(s->i2c));
    s->i2c.phase = I2C_IDLE;

    /* DIP switch / baseboard port defaults for Chihiro */
    s->pinsa = 0xCB;
    s->pinsb = 0x52;
    s->pinsc = 0x00;
    s->outa = 0;
    s->outb = 0;
    s->outc = 0;
    s->oea = 0;
    s->oeb = 0;
    s->oec = 0;
    s->portacfg = 0;
    s->portbcfg = 0;
    s->portccfg = 0;

    s->exif = 0;
    s->eie = 0;
    s->eip = 0;
    s->mpage = 0;
    s->dps = 0;

    memset(s->ep, 0, sizeof(s->ep));
    memset(s->setupdat, 0, sizeof(s->setupdat));
}

void an2131_b2_boot(AN2131State *s, const uint8_t *eeprom, int eeprom_size)
{
    if (eeprom_size < 7 || eeprom[0] != 0xB2) {
        fprintf(stderr, "[AN2131] B2 boot skipped — invalid header (0x%02X)\n",
                eeprom_size > 0 ? eeprom[0] : 0);
        return;
    }

    uint16_t vid = eeprom[1] | ((uint16_t)eeprom[2] << 8);
    uint16_t pid = eeprom[3] | ((uint16_t)eeprom[4] << 8);
    fprintf(stderr, "[AN2131] B2 boot: VID=0x%04X PID=0x%04X\n", vid, pid);

    int pos = 7;
    int total_bytes = 0;

    while (pos + 4 <= eeprom_size) {
        uint8_t lenh = eeprom[pos];
        uint8_t lenl = eeprom[pos + 1];
        bool last = lenh & 0x80;
        uint16_t len  = ((uint16_t)(lenh & 0x7F) << 8) | lenl;
        uint16_t addr = ((uint16_t)eeprom[pos + 2] << 8) | eeprom[pos + 3];
        pos += 4;

        if (len == 0) break;
        if (pos + len > eeprom_size) break;

        an2131_anchor_load(s, addr, &eeprom[pos], len);
        total_bytes += len;
        pos += len;

        if (last)
            break;
    }

    fprintf(stderr, "[AN2131] B2 boot: loaded %d bytes, cpu_running=%d\n",
            total_bytes, s->cpu_running);
}

void an2131_anchor_load(AN2131State *s, uint16_t addr,
                        const uint8_t *data, int len)
{
    if (addr == 0x7F92) {
        if (len > 0)
            an2131_set_cpucs(s, data[0]);
        return;
    }
    for (int i = 0; i < len && (addr + i) < AN2131_RAM_SIZE; i++)
        s->ram[addr + i] = data[i];
}

void an2131_set_cpucs(AN2131State *s, uint8_t val)
{
    bool was_reset = s->cpucs & CPUCS_8051RES;
    s->cpucs = val;

    if (was_reset && !(val & CPUCS_8051RES)) {
        s->cpu_running = true;
        s->cpu.pc = 0x0000;
        s->cpu.sp = 0x07;
        fprintf(stderr, "[AN2131] CPU released — running firmware from 0x0000\n");
        an2131_run(s, 8000000);
        fprintf(stderr, "[AN2131] Init done — PC=0x%04X SP=0x%02X IE=0x%02X "
                "EIE=0x%02X USBIEN=0x%02X CKCON=0x%02X\n",
                s->cpu.pc, s->cpu.sp,
                s->cpu.sfr[SFR_IE - 0x80], s->eie, s->usbien,
                s->cpu.sfr[0x8E - 0x80]);
    } else if (!was_reset && (val & CPUCS_8051RES)) {
        s->cpu_running = false;
        fprintf(stderr, "[AN2131] CPU held in reset\n");
    }
}

int an2131_setup_packet(AN2131State *s, const uint8_t setup[8],
                        const uint8_t *out_data, int out_len,
                        uint8_t *resp_buf, int resp_max)
{
    if (!s->cpu_running) return -1;

    s->diag_setup_calls++;

    /* Force-clear stale interrupt state */
    s->cpu.in_interrupt = false;

    memcpy(s->setupdat, setup, 8);

    /* Pre-load OUT0BUF for OUT control transfers */
    if (out_data && out_len > 0) {
        int copy = min_int(out_len, AN2131_EP_BUFSZ);
        memcpy(&s->ram[AN_OUT0BUF - 0x6000], out_data, copy);
        s->ep[0].bc_out = copy;
        s->ep0cs |= EP0CS_OUTBSY;
        s->out07irq |= 0x01;
    }

    s->usbirq |= USBIRQ_SUDAV;
    s->ep[0].in_armed = false;

    bool is_in = setup[0] & 0x80;
    bool need_ep4 = (setup[1] == 0x19 && is_in);

    int cycles = 0;
    int limit = 10000;
    int drain = 0;
    bool mainloop_hit = false;

    while (cycles < limit) {
        jvs_rx_deliver(s);
        check_interrupts(s);
        int c = cpu8051_step(&s->cpu);
        if (c <= 0) {
            s->total_cycles++;
            if (usb_irq_pending(s) || s->i2c_irq_pending ||
                s->jvs_response_ready || s->jvs_rx_pending) {
                s->cpu.halted = false;
                continue;
            }
            break;
        }
        cycles += c;
        s->total_cycles += c;

        if (drain && s->cpu.pc == 0x0696) mainloop_hit = true;

        if (is_in && s->ep[0].in_armed)
            s->ep0cs &= ~EP0CS_INBSY;

        if (!drain) {
            if (is_in && s->ep[0].in_armed) {
                if (!need_ep4 || s->ep[4].in_armed)
                    drain = cycles;
            }
            if (!is_in && !(s->usbirq & USBIRQ_SUDAV)) drain = cycles;
        }
        if (drain && mainloop_hit && setup[1] == 0x20) break;
        if (drain && mainloop_hit && (cycles - drain) > 2000) break;
        if (drain && (cycles - drain) > 20000) break;
    }

    if (is_in && s->ep[0].in_armed) {
        int bc = s->ep[0].bc_in;
        int copy = min_int(bc, resp_max);
        memcpy(resp_buf, &s->ram[AN_IN0BUF - 0x6000], copy);

        /* v0x19 pending_len override — REMOVED for testing (session 26).
         * Was needed when JVS RX delivery was broken (virtual-time delay).
         * With cycle-based delay, firmware handles B000 correctly. */

        s->ep[0].in_armed = false;
        s->ep0cs &= ~EP0CS_INBSY;
        return copy;
    }

    /* SUDPTR-based EP0 IN (AN2131 silicon feature for standard USB requests):
     * Firmware sets SUDPTRH:SUDPTRL to point to descriptor data in RAM,
     * then sets EP0CS. The silicon reads from SUDPTR automatically.
     * Vendor requests bypass this — they write IN0BUF + IN0BC directly. */
    if (is_in && s->sudptr != 0) {
        uint16_t ptr = s->sudptr;
        if (ptr < AN2131_RAM_SIZE) {
            int desc_len = s->ram[ptr];
            if (ptr + 1 < AN2131_RAM_SIZE && s->ram[ptr + 1] == 0x02) {
                desc_len = s->ram[ptr + 2] | (s->ram[ptr + 3] << 8);
            }
            int wLength = s->setupdat[6] | (s->setupdat[7] << 8);
            if (desc_len > wLength) desc_len = wLength;
            if (desc_len > resp_max) desc_len = resp_max;
            if (ptr + desc_len <= AN2131_RAM_SIZE) {
                memcpy(resp_buf, &s->ram[ptr], desc_len);
                s->sudptr = 0;
                return desc_len;
            }
        }
        s->sudptr = 0;
    }

    return 0;
}

int an2131_ep_in_poll(AN2131State *s, int ep_nr)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return 0;
    if (!s->cpu_running) return 0;

    an2131_run(s, 1000);

    return s->ep[ep_nr].in_armed ? s->ep[ep_nr].bc_in : -1;
}

int an2131_ep_in_read(AN2131State *s, int ep_nr,
                      uint8_t *buf, int max_len)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return 0;
    if (!s->ep[ep_nr].in_armed) return 0;

    int bc = s->ep[ep_nr].bc_in;
    int copy = min_int(bc, max_len);

    /* INnBUF RAM offset = 0x1F00 - ep_nr * 0x80 */
    uint16_t buf_off = 0x1F00 - (uint16_t)ep_nr * 0x80;
    if (buf_off + copy <= AN2131_RAM_SIZE)
        memcpy(buf, &s->ram[buf_off], copy);

    s->ep[ep_nr].in_armed = false;
    s->ep[ep_nr].bc_in = 0;
    if (ep_nr == 0)
        s->ep0cs &= ~EP0CS_INBSY;
    else
        s->ep[ep_nr].cs_in &= ~EPCS_BSY;

    /* Notify firmware: IN data was sent to host */
    s->in07irq |= (1 << ep_nr);

    return copy;
}

void an2131_ep_out_write(AN2131State *s, int ep_nr,
                         const uint8_t *data, int len)
{
    if (ep_nr < 0 || ep_nr >= AN2131_EP_COUNT) return;
    if (!s->cpu_running) return;

    int copy = min_int(len, AN2131_EP_BUFSZ);

    /* OUTnBUF RAM offset = 0x1EC0 - ep_nr * 0x80 */
    uint16_t buf_off = 0x1EC0 - (uint16_t)ep_nr * 0x80;
    if (buf_off + copy <= AN2131_RAM_SIZE)
        memcpy(&s->ram[buf_off], data, copy);

    s->ep[ep_nr].bc_out = copy;
    if (ep_nr == 0)
        s->ep0cs |= EP0CS_OUTBSY;
    else
        s->ep[ep_nr].cs_out |= EPCS_BSY;

    s->out07irq |= (1 << ep_nr);

    an2131_run(s, 2000);
}

static void jvs_rx_deliver(AN2131State *s)
{
    Cpu8051State *cpu = &s->cpu;
    if (cpu->in_interrupt) return;

    /* First byte: simulate RS-485 serial latency (~2ms at 115200 baud).
     * Use CPU cycles (not virtual time) so delay works during an2131_run bursts. */
    if (s->jvs_response_ready) {
        uint64_t elapsed = s->total_cycles - s->jvs_response_set_cycles;
        if (elapsed < 15000) return;
        if (cpu->sfr[0xC0 - 0x80] & 0x02) return;  /* TI1 still set */
        cpu->sfr[0xC1 - 0x80] = s->jvs_rx_buf[0];
        s->jvs_rx_pos = 1;
        cpu->sfr[0xC0 - 0x80] |= 0x01;  /* set RI1 */
        s->jvs_response_ready = false;
        s->jvs_rx_pending = false;
        return;
    }

    /* Subsequent bytes: deliver after ISR returns */
    if (s->jvs_rx_pending) {
        if (s->jvs_rx_pos < s->jvs_rx_len) {
            uint8_t rxbyte = s->jvs_rx_buf[s->jvs_rx_pos];
            cpu->sfr[0xC1 - 0x80] = rxbyte;
            s->jvs_rx_pos++;
            cpu->sfr[0xC0 - 0x80] |= 0x01;  /* set RI1 */
        }
        s->jvs_rx_pending = false;
    }
}

int an2131_run(AN2131State *s, int max_cycles)
{
    if (!s->cpu_running) return 0;

    int total = 0;
    while (total < max_cycles) {
        jvs_rx_deliver(s);
        check_interrupts(s);
        int c = cpu8051_step(&s->cpu);
        if (c <= 0) {
            s->total_cycles++;
            if (usb_irq_pending(s) || s->i2c_irq_pending ||
                s->jvs_response_ready || s->jvs_rx_pending) {
                s->cpu.halted = false;
                continue;
            }
            break;
        }
        total += c;
        s->total_cycles += c;
    }
    return total;
}
