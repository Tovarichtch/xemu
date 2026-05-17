/*
 * QEMU Chihiro USB Devices
 *
 * Copyright (c) 2016 espes
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

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"
#include "qapi/error.h"

#include "qemu/timer.h"
#include "chihiro-firmware.h"
#include "chihiro.h"
#include "chihiro-jvs.h"
#include "chihiro-an2131.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
extern bool chihiro_game_running;
extern bool lpc_log_verbose;

typedef struct ChihiroUSBState {
    USBDevice dev;

    /* Device identity (set at realize, safe to use in all callbacks) */
    bool is_qc;  /* true = AN2131QC (PID 0x0002), false = AN2131SC (PID 0x0003) */

    /* I2C EEPROM data (8KB): ic10 for QC, pc20 for SC — loaded at realize */
    uint8_t eeprom[8192];


    /* EZ-USB firmware download state (ANCHOR_LOAD / bRequest 0xA0) */
    uint32_t fw_bytes_written;  /* total bytes received via 0xA0 */
    bool fw_cpu_held;           /* true if CPUCS register set to hold CPU */
    bool fw_loaded;             /* true after game firmware uploaded and CPU released */
    bool eeprom_reloaded;       /* true after post-QuickReboot EEPROM reload (once) */

    /* ic11 EEPROM (512 bytes) — baseboard config, "ACBU0001" + game ID */
    uint8_t ic11[512];

    /* External memory (64KB, mapped at 0x0000–0xFFFF on the AN2131 8051) */
    uint8_t extmem[65536];

    /* Pending write tracking for 0x1F (extmem via EP3 OUT, item E) */
    uint16_t write_1f_addr;
    int write_1f_remaining;



    /* Real AN2131 loads firmware from EEPROM after initial enumeration,
     * then disconnects and reconnects. SEGABOOT waits for the CSC
     * (Connect Status Change) from the reconnect to start Phase 2. */

    /* ACBU protocol state (item E — EP3 OUT shadow, to be removed in Phase 2) */
    bool acbu_response_ready;   /* write-complete flag for 0x18 status poll */

    /* JVS I/O board emulation state (shared between QC and SC paths) */
    ChihiroJVSState jvs;

    /* AN2131 LLE: 8051 CPU + register layer (runs ic10/pc20 firmware) */
    AN2131State an2131;
    bool use_lle;  /* true after firmware loaded and AN2131 CPU running */
    QEMUTimer *lle_tick_timer;

    /* JVS watchdog diagnostic */
    int64_t last_jvs_send_ms;
    int64_t last_jvs_recv_ms;
    bool jvs_watchdog_fired;

    /* ── DIAG: event-driven listeners (no behavior change) ──── */
    int64_t diag_last_report_ms;
    uint64_t diag_tick_count;
    uint64_t diag_cycles_total;
    /* state snapshot for transition detection */
    bool diag_prev_tr0;
    bool diag_prev_ep4_armed;
    bool diag_prev_halted;
    bool diag_prev_cpu_running;
    int  diag_zero_cycle_streak;
} ChihiroUSBState;

static ChihiroUSBState *chihiro_qc_instance;

enum chihiro_usb_strings {
    STRING_SERIALNUMBER,
    STRING_MANUFACTURER,
    STRING_PRODUCT,
};

static const USBDescStrings chihiro_usb_stringtable = {
    [STRING_SERIALNUMBER]       = "\x00",
    [STRING_MANUFACTURER]       = "SEGA",
    [STRING_PRODUCT]            = "BASEBD" // different for qc?
};

static const USBDescIface desc_iface_chihiro_an2131qc = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 10,
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = 0x00,
    .bInterfaceProtocol            = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x05,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x05,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
    },
};

static const USBDescDevice desc_device_chihiro_an2131qc = {
    .bcdUSB                        = 0x0100,
    .bDeviceClass                  = 0x60,
    .bDeviceSubClass               = 0x00,
    .bDeviceProtocol               = 0x00,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = 0x80,
            .bMaxPower             = 0x96,
            .nif = 1,
            .ifs = &desc_iface_chihiro_an2131qc,
        },
    },
};

static const USBDesc desc_chihiro_an2131qc = {
    .id = {
        .idVendor          = 0x0CA3,
        .idProduct         = 0x0002,
        .bcdDevice         = 0x0108,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_chihiro_an2131qc,
    .str  = chihiro_usb_stringtable,
};

static const USBDescIface desc_iface_chihiro_an2131sc = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 6,
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = 0x00,
    .bInterfaceProtocol            = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x0040,
            .bInterval             = 0,
        },
    },
};

static const USBDescDevice desc_device_chihiro_an2131sc = {
    .bcdUSB                        = 0x0100,
    .bDeviceClass                  = 0x60,
    .bDeviceSubClass               = 0x01,
    .bDeviceProtocol               = 0x00,
    .bMaxPacketSize0               = 0x40,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = 0x80,
            .bMaxPower             = 0x96,
            .nif = 1,
            .ifs = &desc_iface_chihiro_an2131sc,
        },
    },
};

static const USBDesc desc_chihiro_an2131sc = {
    .id = {
        .idVendor          = 0x0CA3,
        .idProduct         = 0x0003,
        .bcdDevice         = 0x0110,
        .iManufacturer     = STRING_MANUFACTURER,
        .iProduct          = STRING_PRODUCT,
        .iSerialNumber     = STRING_SERIALNUMBER,
    },
    .full = &desc_device_chihiro_an2131sc,
    .str  = chihiro_usb_stringtable,
};


static void lle_tick_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    if (s->use_lle && s->an2131.cpu_running) {
        an2131_run(&s->an2131, 6000);
        timer_mod(s->lle_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

static void handle_reset(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = s->is_qc ? "QC" : "SC";
    fprintf(stderr, "[%07lld] chihiro-usb [%s]: USB RESET (lle=%d)\n", TS_MS, id, s->use_lle);
    if (s->use_lle) {
        s->an2131.usbirq |= USBIRQ_URES;
        an2131_run(&s->an2131, 10000);
    }
}


static void handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = s->is_qc ? "QC" : "SC";

    int ret = usb_desc_handle_control(dev, p, request, value, index,
                                      length, data);
    if (ret >= 0) {
        return;
    }

    /* Vendor request — extract bRequest from combined field.
     * QEMU encodes: request = (bmRequestType << 8) | bRequest */
    int bRequest = request & 0xFF;

    /* LLE path: route ALL vendor requests through 8051 firmware.
     * The EEPROM firmware (v0x08) has a full pre-dispatch in the
     * default handler's 0x069B function that handles 0x16-0x30.
     * Only ANCHOR_LOAD (0xA0) bypasses firmware (silicon-level). */
    bool lle_exclude = (bRequest == 0xA0);

    if (s->use_lle && !lle_exclude) {
        uint8_t setup[8];
        setup[0] = (uint8_t)(request >> 8);  /* bmRequestType */
        setup[1] = (uint8_t)bRequest;
        setup[2] = (uint8_t)(value & 0xFF);
        setup[3] = (uint8_t)(value >> 8);
        setup[4] = (uint8_t)(index & 0xFF);
        setup[5] = (uint8_t)(index >> 8);
        setup[6] = (uint8_t)(length & 0xFF);
        setup[7] = (uint8_t)(length >> 8);



        const uint8_t *out_data = NULL;
        int out_len = 0;
        if (!(setup[0] & 0x80) && length > 0) {
            out_data = data;
            out_len = length;
        }

        uint8_t resp[64];
        int resp_len = an2131_setup_packet(&s->an2131, setup,
                                           out_data, out_len,
                                           resp, sizeof(resp));

        if (resp_len > 0) {
            int copy = MIN(resp_len, length);
            memcpy(data, resp, copy);
            p->actual_length = copy;
        } else {
            p->actual_length = length;
        }

        if (s->is_qc && bRequest == 0x20) {
            s->last_jvs_send_ms = TS_MS;
        }

        if (lpc_log_verbose && (bRequest == 0x1C || bRequest == 0x24 || bRequest == 0x15
                                || bRequest == 0x17 || bRequest == 0x18 || bRequest == 0x16)) {
            fprintf(stderr, "[%07lld] LLE [%s] v0x%02X val=0x%04X idx=0x%04X resp_len=%d ctrl={",
                    TS_MS, id, bRequest, value, index, resp_len);
            int show = resp_len > 0 ? MIN(resp_len, 16) : MIN(length, 16);
            for (int i = 0; i < show; i++)
                fprintf(stderr, "%s0x%02X", i?",":"", data[i]);
            fprintf(stderr, "}");
            if (bRequest == 0x17) {
                if (s->an2131.ep[2].in_armed) {
                    int bc = s->an2131.ep[2].bc_in;
                    int ep_show = MIN(bc, 16);
                    fprintf(stderr, " IN2BUF[%d]={", bc);
                    for (int i = 0; i < ep_show; i++)
                        fprintf(stderr, "%s0x%02X", i?",":"", s->an2131.ram[0x1E00 + i]);
                    fprintf(stderr, "}");
                } else {
                    fprintf(stderr, " IN2BUF=NOT_ARMED");
                }
            }
            fprintf(stderr, "\n");
        }

        if (lpc_log_verbose && !s->is_qc &&
            (bRequest == 0x1A || bRequest == 0x1B || bRequest == 0x22 || bRequest == 0x23)) {
            static int sc_uart_log = 0;
            static int sc_uart_1a_count = 0, sc_uart_1b_count = 0;
            static int64_t sc_uart_last_summary = 0;
            if (bRequest == 0x1A) sc_uart_1a_count++;
            if (bRequest == 0x1B) sc_uart_1b_count++;
            bool log_this = (bRequest == 0x22 || bRequest == 0x23 || sc_uart_log < 50 ||
                             (resp_len > 0 && sc_uart_log < 500));
            if (log_this) {
                sc_uart_log++;
                fprintf(stderr, "[%lld] SC LLE v0x%02X: val=0x%04X len=%d resp_len=%d",
                        TS_MS, bRequest, value, length, resp_len);
                if (resp_len > 0) {
                    fprintf(stderr, " data:");
                    for (int i = 0; i < resp_len && i < 16; i++)
                        fprintf(stderr, " %02X", resp[i]);
                }
                if (bRequest == 0x22 && out_len > 0) {
                    fprintf(stderr, " out:");
                    for (int i = 0; i < out_len && i < 16; i++)
                        fprintf(stderr, " %02X", out_data[i]);
                }
                fprintf(stderr, "\n");
            }
            int64_t now = TS_MS;
            if (now - sc_uart_last_summary >= 5000) {
                fprintf(stderr, "[%ld] SC UART summary: 0x1A=%d 0x1B=%d polls\n",
                        (long)now, sc_uart_1a_count, sc_uart_1b_count);
                sc_uart_last_summary = now;
            }
        }

        return;
    }

    /* Default response (MAME: every vendor request gets this).
     * WARNING (item B): this overwrites data[0], corrupting ANCHOR_LOAD
     * CPUCS writes (0x01 → 0x00). See audit for details. */
    for (int n = 0; n < length && n < 6; n++) {
        data[n] = 0x50 ^ n;
    }
    data[0] = 0x00;  /* success */
    data[1] = 0xCB;  /* PINSA (active low: 0=pressed/ON, 1=released/OFF)
                      * bit0=1 DIP1 OFF
                      * bit1=1 DIP2 OFF
                      * bit2=0 DIP3 ON  (horiz freq — required to avoid CAUTION 51)
                      * bit3=1 CS pin (ignored)
                      * bit4=0 DIP4 ON  (horiz freq — required to avoid CAUTION 51)
                      * bit5=0 DIP5 ON
                      * bit6=1 TEST released
                      * bit7=1 SERVICE released */
    data[2] = 0x52 | (s->jvs.sense & 0x03);  /* PINSB with current JVS sense */
    data[3] = 0x53;  /* OUTB register */

    /* Vendor request map (all handled by 8051 firmware in LLE mode):
     *
     * QC (baseboard controller):
     *   0x16  Read ic10 EEPROM #1 → bulk EP1 IN
     *   0x17  Read baseboard EEPROM ic11 (24LC024) → bulk EP2 IN
     *   0x18  Read external memory / ACBU write-complete status poll → EP3 IN
     *   0x19  JVS poll (triggers EP4 IN arm with switch/analog data)
     *   0x1C  Read RTC (BCD time) → bulk EP5 IN
     *   0x1D  Write ic10 EEPROM #1
     *   0x1E  Write ic11 EEPROM #2 via EP2 OUT
     *   0x1F  Write external memory (ACBU backup) via EP3 OUT
     *   0x20  JVS send (payload in SETUP data, firmware sends via SBUF1)
     *   0x24  Write RTC
     *   0x30  External interrupt control
     *
     * SC (serial controller):
     *   0x1A  Get UART0 data (gun controller response)
     *   0x1B  Get UART1 / JVS response (SC-side JVS for lightgun games)
     *   0x22  Send UART0 data (gun controller command)
     *   0x23  Trigger UART1 / JVS poll
     *   0x25-0x2F  UART config
     *   0x31  Set PORTB pins
     *
     * Silicon-level (not firmware):
     *   0xA0  ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131)
     */

    switch (bRequest) {
    case 0xA0: /* ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131) */
    {
        uint16_t ram_addr = value;
        int count = length;
        bool is_read = (request >> 8) & 0x80;
        if (!is_read) {
            if (lpc_log_verbose && count > 0 && count <= 8) {
                fprintf(stderr, "[%07lld] ANCHOR_LOAD addr=0x%04X len=%d data[]={",
                       TS_MS, ram_addr, count);
                for (int i = 0; i < count && i < 8; i++)
                    fprintf(stderr, "%s0x%02X", i?",":"", data[i]);
                fprintf(stderr, "}\n");
            }
            an2131_anchor_load(&s->an2131, ram_addr, data, count);
            s->fw_bytes_written += count;
            if (lpc_log_verbose && ram_addr != 0x7F92 && count > 0) {
                fprintf(stderr, "[%07lld] chihiro-usb [%s]: ANCHOR_LOAD addr=0x%04X len=%d (total %u)\n",
                       TS_MS, id, ram_addr, count, s->fw_bytes_written);
            }
            if (ram_addr == 0x7F92) {
                bool hold = (count > 0 && data[0] & 0x01);
                fprintf(stderr, "[%07lld] chihiro-usb [%s]: ANCHOR_LOAD CPUCS=%s (total %u bytes)\n",
                       TS_MS, id, hold ? "HOLD" : "RUN", s->fw_bytes_written);
                if (s->fw_cpu_held && !hold) {
                    s->fw_loaded = true;
                    s->use_lle = s->an2131.cpu_running;
                    fprintf(stderr, "[%07lld] chihiro-usb [%s]: FW LOADED — LLE %s\n",
                           TS_MS, id, s->use_lle ? "ACTIVE" : "INACTIVE (fallback HLE)");
                }
                s->fw_cpu_held = hold;
            }
        }
        break;
    }
    default:
        break;
    }

    p->actual_length = length;
}

static void handle_data(USBDevice *dev, USBPacket *p)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    int ep = p->ep->nr;

    if (s->is_qc && ep == 4) {
        static int ep4_data_log = 0;
        if (ep4_data_log < 50) {
            fprintf(stderr, "[%07lld] handle_data EP4 pid=%s armed=%d bc=%d\n",
                    TS_MS, p->pid == USB_TOKEN_IN ? "IN" : "OUT",
                    s->an2131.ep[4].in_armed, s->an2131.ep[4].bc_in);
            ep4_data_log++;
        }
    }

    /* LLE path: route bulk transfers through AN2131 firmware */
    if (s->use_lle) {
        if (p->pid == USB_TOKEN_IN) {
            int avail = an2131_ep_in_poll(&s->an2131, ep);
            if (lpc_log_verbose && !s->is_qc && (ep == 1 || ep == 2)) {
                static int ep_poll_log = 0;
                if (ep_poll_log < 100) {
                    ep_poll_log++;
                    fprintf(stderr, "[%lld] SC BULK EP%d IN poll: avail=%d\n",
                            TS_MS, ep, avail);
                }
            }
            if (avail >= 0) {
                uint8_t buf[64];
                int got = an2131_ep_in_read(&s->an2131, ep, buf, MIN((int)p->iov.size, (int)sizeof(buf)));
                if (got > 0) {
                    if (lpc_log_verbose && !s->is_qc && (ep == 1 || ep == 2)) {
                        fprintf(stderr, "[%lld] SC BULK EP%d IN: %d bytes:", TS_MS, ep, got);
                        for (int i = 0; i < got && i < 32; i++)
                            fprintf(stderr, " %02X", buf[i]);
                        fprintf(stderr, "\n");
                    }
                    if (ep == 4 && s->is_qc) {
                        static int ep4in_data_log = 0;
                        if (ep4in_data_log < 50) {
                            fprintf(stderr, "[%07lld] EP4_IN_DATA: %d bytes:", TS_MS, got);
                            for (int i = 0; i < got && i < 16; i++)
                                fprintf(stderr, " %02X", buf[i]);
                            fprintf(stderr, "\n");
                            ep4in_data_log++;
                        }
                    }
                    usb_packet_copy(p, buf, got);
                    if (ep == 4 && s->is_qc)
                        s->last_jvs_recv_ms = TS_MS;
                }
            } else {
                p->status = USB_RET_NAK;
            }
        } else {
            int len = p->iov.size;
            uint8_t buf[64];
            int chunk = MIN(len, (int)sizeof(buf));
            usb_packet_copy(p, buf, chunk);
            {
                static int bulk_out_log = 0;
                if (bulk_out_log < 200) {
                    fprintf(stderr, "[%07lld] BULK_OUT ep=%d len=%d", TS_MS, ep, chunk);
                    for (int i = 0; i < 8 && i < chunk; i++)
                        fprintf(stderr, " %02X", buf[i]);
                    fprintf(stderr, "\n");
                    bulk_out_log++;
                }
            }
            an2131_ep_out_write(&s->an2131, ep, buf, chunk);
            if (ep == 3) {
                static int ep3out_run_diag = 0;
                if (ep3out_run_diag < 10)
                    fprintf(stderr, "[EP3OUT-RUN] pre: out07irq=%02X cpu_run=%d in_int=%d\n",
                            s->an2131.out07irq, s->an2131.cpu_running,
                            s->an2131.cpu.in_interrupt);
                an2131_run(&s->an2131, 5000);
                if (ep3out_run_diag < 10) {
                    fprintf(stderr, "[EP3OUT-RUN] post: out07irq=%02X in_int=%d PC=0x%04X\n",
                            s->an2131.out07irq, s->an2131.cpu.in_interrupt,
                            s->an2131.cpu.pc);
                    ep3out_run_diag++;
                }
            } else {
                an2131_run(&s->an2131, 2000);
            }

            /* Backup write HLE: track EP3 OUT writes and signal completion
             * for the 0x18 count=0 status poll. */
            if (ep == 3 && s->is_qc && chunk > 0) {
                uint16_t addr = s->write_1f_addr;
                if (addr + chunk <= 65536) {
                    memcpy(s->extmem + addr, buf, chunk);
                    s->write_1f_addr += chunk;
                }
                s->write_1f_remaining -= chunk;
                if (addr == 0x8000 && chunk >= 4 &&
                    buf[0] == 'A' && buf[1] == 'C' &&
                    buf[2] == 'B' && buf[3] == 'U') {
                    memcpy(s->extmem + 0x8400, s->ic11, 128);
                    s->acbu_response_ready = true;
                }
                if (s->write_1f_remaining <= 0 && addr >= 0x8000) {
                    s->acbu_response_ready = true;
                    if (s->extmem[0x8000] == 'A' && s->extmem[0x8001] == 'C' &&
                        s->extmem[0x8002] == 'B' && s->extmem[0x8003] == 'U') {
                        memcpy(s->ic11, s->extmem + 0x8000, 128);
                    }
                }
            }

            /* Point A: JVS command incoming to AN2131 via EP4 OUT */
        }
        return;
    }

    /* Non-LLE fallback never reached (use_lle always true after B2 boot).
     * Bulk IN/OUT for QC: vendor 0x16/0x17 → EP1/EP2, 0x1E/0x1F → EP2/EP3.
     * Bulk IN/OUT for SC: UART data via EP1-EP3.
     * All now handled natively by AN2131 firmware. */
    p->status = USB_RET_NAK;
}

static void chihiro_an2131qc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = true;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load ic10 QC EEPROM firmware (8192 bytes).
     * Contains AN2131 8051 firmware code + region/serial/game data.
     * SEGABOOT reads this via vendor request 0x16 + bulk IN EP1. */
    if (chihiro_ic10_data && chihiro_ic10_size == sizeof(s->eeprom)) {
        memcpy(s->eeprom, chihiro_ic10_data, sizeof(s->eeprom));
    } else {
        memcpy(s->eeprom, hotd3_ic10_g24lc64, sizeof(s->eeprom));
    }

    /* Region byte at eeprom[0x1F00]: SEGABOOT checks boot.id[0x38] bitmask
     * against (1 << region). JPN-only games (e.g. Golf SBLF, bitmask=0x02)
     * fail with ERROR 05 if region=2. Region=1 (JPN) works for all known
     * games since all bitmasks include bit 1. */
    s->eeprom[0x1F00] = 0x01;  /* Region: 01=JPN, 02=USA, 03=EXP */
    memcpy(&s->eeprom[0x1F10], "BEER-01A00000001", 16);


    /* Load ic11 baseboard EEPROM (256 bytes, 24LC024) */
    memset(s->ic11, 0, sizeof(s->ic11));
    if (chihiro_ic11_data && chihiro_ic11_size <= sizeof(s->ic11)) {
        memcpy(s->ic11, chihiro_ic11_data, chihiro_ic11_size);
    } else {
        memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));
    }
    memset(s->extmem, 0, sizeof(s->extmem));
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    chihiro_jvs_init(&s->jvs);
    chihiro_jvs_global = &s->jvs;

    /* AN2131 LLE: init 8051 CPU + register layer, wire EEPROMs + extmem */
    an2131_init(&s->an2131);
    s->an2131.ic10_eeprom = s->eeprom;
    s->an2131.ic10_size = sizeof(s->eeprom);
    s->an2131.ic11_eeprom = s->ic11;
    s->an2131.ic11_size = 256;
    s->an2131.extmem = s->extmem;
    s->an2131.extmem_size = sizeof(s->extmem);
    s->an2131.usb_dev = s;

    /* B2 boot: parse ic10 EEPROM firmware and start 8051 CPU */
    an2131_b2_boot(&s->an2131, s->eeprom, sizeof(s->eeprom));
    s->use_lle = s->an2131.cpu_running;

    s->lle_tick_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, lle_tick_cb, s);
    if (s->use_lle) {
        timer_mod(s->lle_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }

    chihiro_qc_instance = s;

    printf("[%07lld] Chihiro QC: ic10=%s ic11=%s, "
           "region=0x%02X, serial=%.16s, LLE=%s\n",
           TS_MS,
           chihiro_ic10_data ? "disk" : "builtin",
           chihiro_ic11_data ? "disk" : "builtin",
           s->eeprom[0x1F00],
           (const char *)&s->eeprom[0x1F10],
           s->use_lle ? "ACTIVE" : "OFF");
}

static void chihiro_an2131qc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->lle_tick_timer);
}

static void chihiro_an2131qc_class_init(ObjectClass *klass, const void *data)
{
    // DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131qc_realize;
    uc->unrealize      = chihiro_an2131qc_unrealize;
    uc->product_desc   = "Chihiro an2131qc";
    uc->usb_desc       = &desc_chihiro_an2131qc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    //dc->vmsd = &vmstate_usb_kbd;
}

static const TypeInfo chihiro_an2131qc_info = {
    .name          = "chihiro-an2131qc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131qc_class_init,
};

static void chihiro_an2131sc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = false;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load pc20 SC EEPROM firmware (8192 bytes). */
    if (chihiro_pc20_data && chihiro_pc20_size == sizeof(s->eeprom)) {
        memcpy(s->eeprom, chihiro_pc20_data, sizeof(s->eeprom));
    } else {
        memcpy(s->eeprom, hotd3_pc20_g24lc64, sizeof(s->eeprom));
    }


    /* Load ic11 baseboard EEPROM (256 bytes, 24LC024) */
    memset(s->ic11, 0, sizeof(s->ic11));
    if (chihiro_ic11_data && chihiro_ic11_size <= sizeof(s->ic11)) {
        memcpy(s->ic11, chihiro_ic11_data, chihiro_ic11_size);
    } else {
        memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));
    }
    memset(s->extmem, 0, sizeof(s->extmem));
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    chihiro_jvs_init(&s->jvs);

    /* AN2131 LLE: init 8051 CPU + register layer, wire EEPROMs + extmem */
    an2131_init(&s->an2131);
    s->an2131.ic10_eeprom = s->eeprom;
    s->an2131.ic10_size = sizeof(s->eeprom);
    s->an2131.ic11_eeprom = s->ic11;
    s->an2131.ic11_size = 256;
    s->an2131.extmem = s->extmem;
    s->an2131.extmem_size = sizeof(s->extmem);
    s->an2131.usb_dev = s;

    /* SC firmware init (0x1156) clears extmem[0x2601-0x2626] then polls
     * extmem[0x260F] and [0x2635] for mailbox triggers from QC/baseboard.
     * B2 boot runs 8M init cycles — firmware clears the area then stalls at
     * the poll. Set triggers AFTER boot (clears done), run more cycles. */
    an2131_b2_boot(&s->an2131, s->eeprom, sizeof(s->eeprom));
    if (s->an2131.cpu_running) {
        s->extmem[0x260F] = 0x01;
        s->extmem[0x2635] = 0x01;
        an2131_run(&s->an2131, 8000000);
    }
    s->use_lle = s->an2131.cpu_running;

    s->lle_tick_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, lle_tick_cb, s);
    if (s->use_lle) {
        timer_mod(s->lle_tick_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }

    printf("[%07lld] Chihiro SC: loaded pc20 (8192B) + ic11 (128B), LLE=%s\n",
           TS_MS, s->use_lle ? "ACTIVE" : "OFF");
}

static void chihiro_an2131sc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->lle_tick_timer);
}

static void chihiro_an2131sc_class_init(ObjectClass *klass, const void *data)
{
    // DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = chihiro_an2131sc_realize;
    uc->unrealize      = chihiro_an2131sc_unrealize;
    uc->product_desc   = "Chihiro an2131sc";
    uc->usb_desc       = &desc_chihiro_an2131sc;

    uc->handle_reset   = handle_reset;
    uc->handle_control = handle_control;
    uc->handle_data    = handle_data;
    uc->handle_attach  = usb_desc_attach;

    //dc->vmsd = &vmstate_usb_kbd;
}

static const TypeInfo chihiro_an2131sc_info = {
    .name          = "chihiro-an2131sc",
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(ChihiroUSBState),
    .class_init    = chihiro_an2131sc_class_init,
};

#define CHIHIRO_SAVE_MAGIC 0x56534843  /* "CHSV" LE */
#define CHIHIRO_SAVE_VERSION 1
#define CHIHIRO_SAVE_IC11_SIZE 512
#define CHIHIRO_SAVE_EXTMEM_OFF 0x8000
#define CHIHIRO_SAVE_EXTMEM_SIZE 0x8000

bool chihiro_usb_save_load(const char *path)
{
    if (!chihiro_qc_instance || !path) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1 || magic != CHIHIRO_SAVE_MAGIC) {
        fclose(f);
        return false;
    }
    if (fread(&version, 4, 1, f) != 1 || version != CHIHIRO_SAVE_VERSION) {
        fclose(f);
        return false;
    }

    ChihiroUSBState *s = chihiro_qc_instance;
    if (fread(s->ic11, 1, CHIHIRO_SAVE_IC11_SIZE, f) != CHIHIRO_SAVE_IC11_SIZE) {
        fclose(f);
        return false;
    }
    if (fread(s->extmem + CHIHIRO_SAVE_EXTMEM_OFF, 1, CHIHIRO_SAVE_EXTMEM_SIZE, f)
        != CHIHIRO_SAVE_EXTMEM_SIZE) {
        fclose(f);
        return false;
    }

    fclose(f);
    fprintf(stderr, "Chihiro: save loaded from %s\n", path);
    return true;
}

bool chihiro_usb_save_flush(const char *path)
{
    if (!chihiro_qc_instance || !path) return false;

    ChihiroUSBState *s = chihiro_qc_instance;

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    uint32_t magic = CHIHIRO_SAVE_MAGIC;
    uint32_t version = CHIHIRO_SAVE_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(s->ic11, 1, CHIHIRO_SAVE_IC11_SIZE, f);
    fwrite(s->extmem + CHIHIRO_SAVE_EXTMEM_OFF, 1, CHIHIRO_SAVE_EXTMEM_SIZE, f);

    fclose(f);
    fprintf(stderr, "Chihiro: save flushed to %s\n", path);
    return true;
}

static void chihiro_usb_register_types(void)
{
    type_register_static(&chihiro_an2131qc_info);
    type_register_static(&chihiro_an2131sc_info);
}

type_init(chihiro_usb_register_types)
