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
#include "chihiro-jvs.h"
#include "chihiro-an2131.h"
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
extern bool chihiro_game_running;
extern int64_t freeze_last_usb_activity_ms;
static bool usb_log_verbose = false;
#define DEBUG_CUSB
#ifdef DEBUG_CUSB
#define DPRINTF(s, ...) do { } while(0)
#else
#define DPRINTF(...)
#endif

typedef struct ChihiroUSBState {
    USBDevice dev;

    /* Device identity (set at realize, safe to use in all callbacks) */
    bool is_qc;  /* true = AN2131QC (PID 0x0002), false = AN2131SC (PID 0x0003) */

    /* I2C EEPROM data (8KB): ic10 for QC, pc20 for SC — loaded at realize */
    uint8_t eeprom[8192];

    /* Per-endpoint bulk IN buffers (EP1–EP5, index 0 unused) */
    #define CHIHIRO_USB_MAX_EP 6
    #define CHIHIRO_USB_EP_BUFSZ 8192
    struct {
        uint8_t buf[CHIHIRO_USB_EP_BUFSZ];
        int pending;
        int offset;
    } ep_in[CHIHIRO_USB_MAX_EP];

    /* EZ-USB firmware download state (ANCHOR_LOAD / bRequest 0xA0) */
    uint32_t fw_bytes_written;  /* total bytes received via 0xA0 */
    bool fw_cpu_held;           /* true if CPUCS register set to hold CPU */
    bool fw_loaded;             /* true after game firmware uploaded and CPU released */

    /* ic11 EEPROM (512 bytes) — baseboard config, "ACBU0001" + game ID */
    uint8_t ic11[512];

    /* External memory (64KB, mapped at 0x0000–0xFFFF on the AN2131 8051) */
    uint8_t extmem[65536];

    /* Pending write tracking for 0x1E (ic11 via EP2) and 0x1F (extmem via EP3) */
    uint16_t write_1e_addr;
    uint16_t write_1f_addr;
    int write_1f_remaining;

    /* SC UART buffers (for JVS communication) */
    uint8_t uart0_rx[256];  /* UART0 receive buffer */
    int uart0_rx_len;
    uint8_t uart1_rx[256];  /* UART1 / JVS receive buffer */
    int uart1_rx_len;
    bool sc_jvs_polling;    /* SC: firmware JVS poll loop active (set by VENDOR 0x23) */

    /* v202: instrumentation counters (read/reset by DIAG timer) */
    uint32_t nak_count;    /* bulk IN NAK count since last report */
    uint32_t bulk_in_count;  /* successful bulk IN count */
    uint32_t bulk_out_count; /* bulk OUT count */

    /* v302: EZ-USB firmware reboot simulation timers.
     * Real AN2131 loads firmware from EEPROM after initial enumeration,
     * then disconnects and reconnects. SEGABOOT waits for the CSC. */
    QEMUTimer *ezusb_disconnect_timer;
    QEMUTimer *ezusb_reconnect_timer;
    bool ezusb_rebooted;

    /* ACBU protocol HLE state (multi-round cmd/response exchange) */
    bool acbu_response_ready;   /* ic11 response is available for reading */
    bool acbu_sbhq_pending;     /* SBHQ data write in progress, response not yet generated */
    int64_t acbu_last_ep3_out_ms; /* timestamp of last EP3 OUT write (for SBHQ completion detect) */

    /* JVS I/O board emulation state (shared between QC and SC paths) */
    ChihiroJVSState jvs;

    /* AN2131 LLE: 8051 CPU + register layer (runs ic10/pc20 firmware) */
    AN2131State an2131;
    bool use_lle;  /* true after firmware loaded and AN2131 CPU running */
    QEMUTimer *lle_tick_timer;
} ChihiroUSBState;

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

/* v302: EZ-USB firmware reboot — disconnect callback */
static void ezusb_disconnect_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    USBDevice *dev = &s->dev;
    const char *id = s->is_qc ? "QC" : "SC";

    if (!dev->attached) {
        if(0) printf("[%07lld] chihiro-usb [%s]: v302 disconnect skipped (already detached)\n", TS_MS, id);
        return;
    }

    if(0) printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — DISCONNECT (CSC will fire)\n", TS_MS, id);
    usb_device_detach(dev);

    /* Schedule reconnect 50ms later */
    timer_mod(s->ezusb_reconnect_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

/* v302: EZ-USB firmware reboot — reconnect callback */
static void ezusb_reconnect_cb(void *opaque)
{
    ChihiroUSBState *s = (ChihiroUSBState *)opaque;
    USBDevice *dev = &s->dev;
    const char *id = s->is_qc ? "QC" : "SC";

    if (dev->attached) {
        if(0) printf("[%07lld] chihiro-usb [%s]: v302 reconnect skipped (already attached)\n", TS_MS, id);
        return;
    }

    if(0) printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — RECONNECT (CSC will fire → Phase 2)\n", TS_MS, id);
    usb_device_attach(dev, &error_abort);
}

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

static uint64_t jvs_send_count = 0;
static uint64_t jvs_recv_count = 0;
static uint64_t jvs_recv_has_data = 0;
static uint64_t vendor_req_counts[256] = {0};

static void handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    extern uint64_t perf_cnt_usb_control;
    perf_cnt_usb_control++;
    freeze_last_usb_activity_ms = TS_MS;
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";

    {
        static int ctrl_log = 0;
        if (usb_log_verbose && ctrl_log < 10000) {
            ctrl_log++;
            fprintf(stderr, "[%07lld] chihiro-usb [%s]: CTRL req=0x%04X val=0x%04X idx=0x%04X len=%d [%s]\n", TS_MS,
                   id, request, value, index, length,
                   (request == 0x8006 && (value >> 8) == 1) ? "GET_DESC(DEV)" :
                   (request == 0x8006 && (value >> 8) == 2) ? "GET_DESC(CFG)" :
                   (request == 0x8006 && (value >> 8) == 3) ? "GET_DESC(STR)" :
                   (request == 0x0005) ? "SET_ADDRESS" :
                   (request == 0x0009) ? "SET_CONFIG" :
                   (request == 0x010B) ? "SET_IFACE" :
                   ((request >> 8) == 0x40 || (request >> 8) == 0xC0) ? "VENDOR" :
                   "OTHER");
        }
    }

    int ret = usb_desc_handle_control(dev, p, request, value, index,
                                      length, data);
    if (ret >= 0) {
        int actual = p->actual_length;
        if(0) {
            if(0) printf("[%07lld] chihiro-usb [%s]: std handled actual=%d addr=%d", TS_MS, id, actual, dev->addr);
            if (actual > 0) {
                if(0) printf(" data=");
                if(0) for (int i = 0; i < actual && i < 18; i++) printf("%02X", data[i]);
            }
            if(0) printf("\n");
        }

        /* v302: After SET_ADDRESS completes, schedule EZ-USB firmware reboot.
         * Real AN2131 loads firmware from EEPROM, then disconnects+reconnects.
         * SEGABOOT waits for the CSC from reconnect to start Phase 2. */
        if (request == (DeviceOutRequest | USB_REQ_SET_ADDRESS) && !s->ezusb_rebooted) {
            s->ezusb_rebooted = true;
            if(0) printf("[%07lld] chihiro-usb [%s]: v302 SET_ADDRESS done (addr=%d) → scheduling EZ-USB reboot in 100ms\n",
                   TS_MS, id, dev->addr);
            timer_mod(s->ezusb_disconnect_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        }

        return;
    }

    /* v202: Log when standard handler rejects a request */
    {
        uint8_t bmRequestType = request >> 8;
        uint8_t reqType = (bmRequestType >> 5) & 0x03;  /* 0=std, 1=class, 2=vendor */
        if (reqType != 2) {
            /* Non-vendor request rejected by usb_desc — could indicate descriptor issue */
            if(0) printf("[%07lld] chihiro-usb [%s]: ⚠ std handler REJECTED req=0x%04X (ret=%d) — "
                   "falling through to vendor handler\n", TS_MS, id, request, ret);
        }
    }

    /* Vendor request — extract bRequest from combined field.
     * QEMU encodes: request = (bmRequestType << 8) | bRequest */
    int bRequest = request & 0xFF;
    vendor_req_counts[(uint8_t)bRequest]++;

    if (chihiro_game_running && s->is_qc && bRequest != 0x20 && bRequest != 0x19) {
        static int game_vendor_log = 0;
        if (game_vendor_log < 500) {
            game_vendor_log++;
            fprintf(stderr, "[%07lld] chihiro-usb [%s]: VENDOR 0x%02X val=0x%04X idx=0x%04X len=%d%s\n",
                   TS_MS, id, bRequest, value, index, length,
                   s->use_lle ? " [LLE]" : "");
        }
    }

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

        static int lle_log = 0;
        if (lle_log < 200) {
            lle_log++;
            fprintf(stderr, "[%07lld] LLE [%s]: VENDOR 0x%02X val=0x%04X idx=0x%04X → resp_len=%d",
                   TS_MS, id, bRequest, value, index, resp_len);
            if (resp_len > 0) {
                fprintf(stderr, " data=");
                for (int i = 0; i < resp_len && i < 8; i++)
                    fprintf(stderr, "%02X", resp[i]);
            }
            fprintf(stderr, "\n");
        }

        if (bRequest == 0x15 && s->is_qc) {
            static int v15_log = 0;
            if (v15_log < 30) {
                v15_log++;
                fprintf(stderr, "[%07lld] LLE 0x15: resp_len=%d data=",
                       TS_MS, resp_len);
                for (int i = 0; i < resp_len && i < 8; i++)
                    fprintf(stderr, "%02X", resp[i]);
                fprintf(stderr, " game=%d\n", chihiro_game_running);
            }
        }

        if (resp_len > 0) {
            int copy = MIN(resp_len, length);
            memcpy(data, resp, copy);
            p->actual_length = copy;
        } else {
            p->actual_length = length;
        }

        /* ACBU/SBHQ HLE: firmware callback 3 overwrites extmem[0x8000],
         * destroying ACBU data. Until JVS serial is connected to firmware,
         * handle ACBU status polls and write_1f_addr tracking here. */
        if (bRequest == 0x1F) {
            s->write_1f_addr = value;
            s->write_1f_remaining = index;
            s->acbu_response_ready = false;
            if (chihiro_game_running && s->is_qc) {
                static int w1f_cnt = 0;
                w1f_cnt++;
                fprintf(stderr, "[%07lld] WRITE#%d: 0x1F addr=0x%04X size=%d → rem=%d ready=0\n",
                       TS_MS, w1f_cnt, value, index, s->write_1f_remaining);
            }
        }
        if (bRequest == 0x19 && chihiro_game_running && s->is_qc) {
            static int v19g_log = 0;
            if (v19g_log < 30) {
                v19g_log++;
                fprintf(stderr, "[%07lld] GAME 0x19: data=",
                       TS_MS);
                for (int i = 0; i < 8; i++)
                    fprintf(stderr, "%02X", data[i]);
                fprintf(stderr, "\n");
            }
        }
        if (bRequest == 0x18) {
            int count = index;
            if (count == 0) {
                if (s->acbu_sbhq_pending) {
                    memcpy(s->extmem + 0x8000, s->ic11, 128);
                    s->acbu_sbhq_pending = false;
                    s->acbu_response_ready = true;
                }
                if (s->acbu_response_ready) {
                    data[0] = 0x01;
                }
                if (chihiro_game_running && s->is_qc) {
                    static int v18_log = 0;
                    if (v18_log < 30) {
                        v18_log++;
                        fprintf(stderr, "[%07lld] GAME 0x18 cnt=0: fw=%02X%02X%02X%02X%02X%02X%02X%02X → data[0]=%02X ready=%d rem=%d\n",
                               TS_MS,
                               resp[0], resp[1], resp[2], resp[3],
                               resp[4], resp[5], resp[6], resp[7],
                               data[0], s->acbu_response_ready, s->write_1f_remaining);
                    }
                }
            } else {
                if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
                int addr = value;
                if (addr + count > 65536) count = 65536 - addr;
                if (addr >= 0 && count > 0) {
                    memcpy(s->ep_in[3].buf, s->extmem + addr, count);
                }
                s->ep_in[3].pending = count;
                s->ep_in[3].offset = 0;
                s->acbu_response_ready = false;
            }
        }

        return;
    }

    /* Save original host data for OUT (host-to-device) vendor requests.
     * The default fill below overwrites data[], so we need a copy for
     * requests like 0x20/0x23 that carry JVS payload. */
    uint8_t host_data[256];
    int host_len = MIN(length, (int)sizeof(host_data));
    if (!((request >> 8) & 0x80) && host_len > 0) {
        memcpy(host_data, data, host_len);
    }

    /* Default response (MAME: every vendor request gets this) */
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

    switch (bRequest) {
    case 0x16: /* Read ic10 EEPROM #1 — queue for bulk EP1 IN */
    {
        int addr = value;     /* wValue = start address in ic10 */
        int count = index;    /* wIndex = byte count */
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        if (addr + count > 8192) count = 8192 - addr;
        if (addr >= 0 && count > 0) {
            memcpy(s->ep_in[1].buf, s->eeprom + addr, count);
        } else {
            memset(s->ep_in[1].buf, 0xFF, count);
        }
        s->ep_in[1].pending = count;
        s->ep_in[1].offset = 0;
        if(0) printf("[%07lld] chihiro-usb [%s]: READ EEPROM1 addr=0x%04X count=%d → queued for EP1\n", TS_MS,
               id, addr, count);
        break;
    }
    case 0x17: /* Read baseboard EEPROM ic11 (512 bytes, 24LC024) */
    {
        int addr = value;
        int count = index;
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        if (addr + count > 512) count = 512 - addr;
        if (addr >= 0 && addr < 512 && count > 0) {
            memcpy(s->ep_in[2].buf, s->ic11 + addr, count);
        } else {
            memset(s->ep_in[2].buf, 0xFF, count);
            count = (count > 0) ? count : 0;
        }
        s->ep_in[2].pending = count;
        s->ep_in[2].offset = 0;
        if(0) {
            if(0) printf("[%07lld] chihiro-usb [%s]: READ ic11 EEPROM addr=0x%02X count=%d data=", TS_MS, id, addr, count);
            for (int i = 0; i < count && i < 16; i++) printf("%02X", s->ep_in[2].buf[i]);
            if(0) printf("\n");
        }
        break;
    }
    case 0x19: { /* Get JVS responses (QC path) */
        jvs_recv_count++;
        data[0] = 0x00;  /* not busy */
        /* Update sense in PINSB */
        data[2] = (data[2] & 0xFC) | (s->jvs.sense & 0x03);
        if (s->jvs.response_len > 0) {
            jvs_recv_has_data++;
            int rlen = s->jvs.response_len;
            uint8_t *ep = s->ep_in[4].buf;
            int wrapped = 0;
            uint8_t resp_dest = (s->jvs.last_target == JVS_BROADCAST) ? 0
                                                                     : s->jvs.device_id;
            ep[wrapped++] = 0x00;
            ep[wrapped++] = 0x01;
            ep[wrapped++] = resp_dest;
            ep[wrapped++] = 0x00;
            ep[wrapped++] = rlen & 0xFF;
            ep[wrapped++] = (rlen >> 8) & 0xFF;
            int copy = MIN(rlen, (int)sizeof(s->ep_in[4].buf) - wrapped);
            memcpy(ep + wrapped, s->jvs.response, copy);
            wrapped += copy;
            data[4] = wrapped & 0xFF;
            data[5] = (wrapped >> 8) & 0xFF;
            s->ep_in[4].pending = wrapped;
            s->ep_in[4].offset = 0;
            s->jvs.response_len = 0;
        } else if (s->ep_in[4].pending > 0) {
            /* Data already queued by EP4 OUT auto-queue — report it */
            data[4] = s->ep_in[4].pending & 0xFF;
            data[5] = (s->ep_in[4].pending >> 8) & 0xFF;
        } else {
            data[4] = 0;
            data[5] = 0;
        }
        {
            static int v19_log = 0;
            int pending = data[4] | (data[5] << 8);
            if (usb_log_verbose && (v19_log < 50 || (pending == 0 && v19_log < 200))) {
                v19_log++;
                fprintf(stderr, "[%07lld] JVS-0x19: sense=%d pending=%d data:",
                        TS_MS, s->jvs.sense, pending);
                for (int i = 0; i < 8; i++) fprintf(stderr, " %02X", data[i]);
                fprintf(stderr, "\n");
            }
        }
        break;
    }
    case 0x20: { /* Send JVS packets (QC path) */
        jvs_send_count++;
        /* AN2131QC format: byte 0 = sequence counter, bytes 1+ = JVS frame */
        uint8_t *jvs_data = host_data;
        int jvs_len = host_len;
        if (jvs_len >= 2 && host_data[0] != JVS_SYNC && host_data[1] == JVS_SYNC) {
            jvs_data = host_data + 1;
            jvs_len -= 1;
        }
        if (jvs_len > 0 && jvs_data[0] == JVS_SYNC) {
            int rlen = chihiro_jvs_process(&s->jvs, jvs_data, jvs_len,
                                            s->jvs.response, sizeof(s->jvs.response));
            s->jvs.response_len = rlen;
            {
                static int v20_log = 0;
                if (usb_log_verbose && v20_log < 50) {
                    v20_log++;
                    fprintf(stderr, "[%07lld] JVS-0x20: sent %d → resp %d bytes:",
                            TS_MS, jvs_len, rlen);
                    for (int i = 0; i < rlen && i < 40; i++)
                        fprintf(stderr, " %02X", s->jvs.response[i]);
                    fprintf(stderr, "\n");
                }
            }
        }
        break;
    }
    case 0x30: /* External interrupt control */
        data[4] = (value & 0xFF) > 0 ? 1 : 0;  /* enabled? */
        data[5] = 0;  /* IRQ counter */
        if(0) printf("[%07lld] chihiro-usb [%s]: EXT IRQ control val=%d\n", TS_MS, id, value);
        break;
    case 0x1C: /* Read RTC — queue BCD time for bulk IN EP4 (data[0]=0 success status) */
    {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        #define TO_BCD(v) ((uint8_t)((v) + 6 * ((v) / 10)))
        int rtc_count = index;
        if (rtc_count > CHIHIRO_USB_EP_BUFSZ) rtc_count = CHIHIRO_USB_EP_BUFSZ;
        memset(s->ep_in[5].buf, 0, rtc_count);
        s->ep_in[5].buf[0] = TO_BCD(t->tm_sec);
        s->ep_in[5].buf[1] = TO_BCD(t->tm_min);
        s->ep_in[5].buf[2] = TO_BCD(t->tm_hour);
        s->ep_in[5].buf[3] = 0;
        s->ep_in[5].buf[4] = TO_BCD(t->tm_mday);
        s->ep_in[5].buf[5] = TO_BCD(t->tm_mon + 1);
        s->ep_in[5].buf[6] = TO_BCD(t->tm_year - 100);
        s->ep_in[5].buf[7] = 0;
        s->ep_in[5].pending = rtc_count;
        s->ep_in[5].offset = 0;
        #undef TO_BCD
        if(0) printf("[%07lld] chihiro-usb [%s]: RTC READ → %02X:%02X:%02X %02X/%02X/%02X (%d bytes queued EP5)\n",
               TS_MS, id, s->ep_in[5].buf[2], s->ep_in[5].buf[1], s->ep_in[5].buf[0],
               s->ep_in[5].buf[4], s->ep_in[5].buf[5], s->ep_in[5].buf[6], rtc_count);
        break;
    }
    case 0x1D: /* Write ic10 EEPROM #1 — accept */
        break;
    case 0x1E: /* Write ic11 EEPROM #2 via EP2 OUT */
        s->write_1e_addr = value;
        break;
    case 0x1F: /* Write external memory via EP3 OUT */
        s->write_1f_addr = value;
        s->write_1f_remaining = index;
        s->acbu_response_ready = false;
        break;
    case 0x24: /* Write RTC — accept */
        break;
    case 0x18: /* Read external memory / write-complete status poll */
    {
        int count = index;
        if (count == 0) {
            if (s->acbu_sbhq_pending) {
                memcpy(s->extmem + 0x8000, s->ic11, 128);
                s->acbu_sbhq_pending = false;
                s->acbu_response_ready = true;
                fprintf(stderr, "[%07lld] chihiro-usb [%s]: SBHQ HLE — "
                       "copied ic11 (128B) to extmem[0x8000]\n", TS_MS, id);
            }
            if (s->acbu_response_ready) {
                data[0] = 0x01;
                p->actual_length = 1;
            }
            if (chihiro_game_running) {
                fprintf(stderr, "[%07lld] chihiro-usb [%s]: STATUS POLL → %s\n",
                       TS_MS, id, s->acbu_response_ready ? "READY" : "IDLE");
            }
            break;
        }
        if (count > CHIHIRO_USB_EP_BUFSZ) count = CHIHIRO_USB_EP_BUFSZ;
        int addr = value;
        if (addr + count > 65536) count = 65536 - addr;
        if (addr >= 0 && count > 0) {
            memcpy(s->ep_in[3].buf, s->extmem + addr, count);
        } else {
            memset(s->ep_in[3].buf, 0, count > 0 ? count : 0);
        }
        s->ep_in[3].pending = count;
        s->ep_in[3].offset = 0;
        s->acbu_response_ready = false;
        if (chihiro_game_running) {
            fprintf(stderr, "[%07lld] chihiro-usb [%s]: EXTMEM READ addr=0x%04X count=%d data=",
                   TS_MS, id, addr, count);
            for (int i = 0; i < count && i < 16; i++)
                fprintf(stderr, "%02X", s->ep_in[3].buf[i]);
            fprintf(stderr, "\n");
        }
        break;
    }
    case 0xA0: /* ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131) */
    {
        uint16_t ram_addr = value;
        int count = length;
        bool is_read = (request >> 8) & 0x80;
        if (!is_read) {
            an2131_anchor_load(&s->an2131, ram_addr, data, count);
            s->fw_bytes_written += count;
            if (ram_addr != 0x7F92 && count > 0) {
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
    /* === SC-specific handlers (UART / JVS) === */
    case 0x1A: /* Get UART0 data (SC only) */
    {
        int avail = s->uart0_rx_len;
        if (avail > 0 && avail <= length) {
            memcpy(data, s->uart0_rx, avail);
            p->actual_length = avail;
            s->uart0_rx_len = 0;
        } else {
            p->actual_length = 0;
        }
        return;
    }
    case 0x1B: /* Get UART1 / JVS response (SC only) */
    {
        if (s->sc_jvs_polling && s->ep_in[3].pending == 0) {
            /* SC firmware auto-polls gun controller — generate JVS response.
             * Queue on EP3 IN (same pattern as QC's 0x19 → EP4). */
            uint8_t resp[64];
            int rp = 0;
            resp[rp++] = JVS_SYNC;
            resp[rp++] = 0x00;  /* dest = host */
            int count_pos = rp++;  /* placeholder for count */
            resp[rp++] = JVS_STATUS_OK;
            /* ReadSW response: report + system + 2x2 player bytes */
            resp[rp++] = JVS_REPORT_OK;
            resp[rp++] = 0x00;  /* system (test/tilt off) */
            resp[rp++] = 0x00;  /* P1 sw1 (no buttons) */
            resp[rp++] = 0x00;  /* P1 sw2 */
            resp[rp++] = 0x00;  /* P2 sw1 */
            resp[rp++] = 0x00;  /* P2 sw2 */
            /* ReadAnalog response: report + 4 channels (center=0x8000) */
            resp[rp++] = JVS_REPORT_OK;
            resp[rp++] = 0x80; resp[rp++] = 0x00;  /* P1 X */
            resp[rp++] = 0x80; resp[rp++] = 0x00;  /* P1 Y */
            resp[rp++] = 0x80; resp[rp++] = 0x00;  /* P2 X */
            resp[rp++] = 0x80; resp[rp++] = 0x00;  /* P2 Y */
            /* Count and checksum */
            int payload_len = rp - 3;
            resp[count_pos] = payload_len + 1;
            uint8_t csum = 0;
            for (int i = 1; i < rp; i++) csum += resp[i];
            resp[rp++] = csum;
            memcpy(s->ep_in[3].buf, resp, rp);
            s->ep_in[3].pending = rp;
            s->ep_in[3].offset = 0;
            data[4] = rp & 0xFF;
            data[5] = (rp >> 8) & 0xFF;
        } else if (s->ep_in[3].pending > 0) {
            data[4] = s->ep_in[3].pending & 0xFF;
            data[5] = (s->ep_in[3].pending >> 8) & 0xFF;
        } else {
            p->actual_length = 0;
            return;
        }
        break;
    }
    case 0x22: /* Send UART0 data (SC only) — accept and discard */
        if(0) printf("[%07lld] chihiro-usb [%s]: SEND UART0 len=%d (stub)\n", TS_MS, id, length);
        break;
    case 0x23: { /* Trigger UART1 / JVS poll (SC only, IN request) */
        if (!s->is_qc && !s->sc_jvs_polling) {
            s->jvs.device_id = 1;
            s->jvs.sense = 0;
            s->sc_jvs_polling = true;
            fprintf(stderr, "[%07lld] chihiro-usb [%s]: SC JVS polling started (id=1, idx=%d)\n",
                   TS_MS, id, index);
        }
        break;
    }
    case 0x25: /* UART config (SC only) — accept */
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2A:
    case 0x2B:
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        if(0) printf("[%07lld] chihiro-usb [%s]: UART/GPIO config 0x%02X (stub)\n", TS_MS, id, bRequest);
        break;
    case 0x31: /* Set PORTB pins (SC only) — accept */
        if(0) printf("[%07lld] chihiro-usb [%s]: SET PORTB val=0x%04X (stub)\n", TS_MS, id, value);
        break;
    default:
        if(0) printf("[%07lld] chihiro-usb [%s]: UNHANDLED vendor req 0x%02X val=0x%04X idx=0x%04X len=%d → accepting\n",
               TS_MS, id, bRequest, value, index, length);
        break;
    }

    if (0) { /* Diagnostic: log vendor 0x15 (PINSB/sense check) */
        if (bRequest == 0x15) {
            printf("[%07lld] chihiro-usb [%s]: VENDOR 0x15 → data[0..3]=%02X %02X %02X %02X sense=%d\n",
                   TS_MS, id, data[0], data[1], data[2], data[3], s->jvs.sense);
        }
    }

    p->actual_length = length;
}

static void handle_data(USBDevice *dev, USBPacket *p)
{
    extern uint64_t perf_cnt_usb_handle;
    perf_cnt_usb_handle++;
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    int ep = p->ep->nr;

    if (chihiro_game_running && !s->is_qc) {
        static int sc_data_log = 0;
        if (usb_log_verbose && sc_data_log < 10000) {
            sc_data_log++;
            fprintf(stderr, "[%07lld] chihiro-usb [SC]: GAME BULK %s EP%d size=%d pending=%d\n",
                   TS_MS, p->pid == USB_TOKEN_IN ? "IN" : "OUT", ep,
                   (int)p->iov.size, s->ep_in[ep].pending);
        }
    }

    /* DIAG: trace ALL OUT tokens on QC */
    if (s->is_qc && p->pid == USB_TOKEN_OUT && ep == 3) {
        static int out_trace = 0;
        if (out_trace < 200) {
            out_trace++;
            uint8_t peek[4] = {0};
            iov_to_buf(p->iov.iov, p->iov.niov, 0, peek, MIN(4, (int)p->iov.size));
            fprintf(stderr, "[%07lld] DIAG EP3 OUT: size=%d w1f=0x%04X data=%02X%02X%02X%02X game=%d rem=%d\n",
                   TS_MS, (int)p->iov.size, s->write_1f_addr,
                   peek[0], peek[1], peek[2], peek[3], chihiro_game_running,
                   s->write_1f_remaining);
        }
    }

    /* LLE path: route bulk transfers through AN2131 firmware */
    if (s->use_lle) {
        if (p->pid == USB_TOKEN_IN && s->is_qc && chihiro_game_running) {
            int fw_avail = (ep < 8 && s->an2131.ep[ep].in_armed)
                           ? s->an2131.ep[ep].bc_in : 0;
            static int game_in_try = 0;
            if (game_in_try < 60) {
                game_in_try++;
                fprintf(stderr, "[%07lld] GAME EP%d IN: ep_in=%d fw=%d\n",
                       TS_MS, ep, s->ep_in[ep].pending, fw_avail);
            }
        }
        if (p->pid == USB_TOKEN_IN) {
            /* HLE-excluded vendor requests (0x16/0x17 etc.) queue data on
             * s->ep_in[] buffers.  Serve those first so the host sees them
             * even though the bulk IN path is otherwise LLE-driven. */
            if (ep >= 1 && ep < CHIHIRO_USB_MAX_EP && s->ep_in[ep].pending > 0) {
                int len = MIN((int)p->iov.size, s->ep_in[ep].pending);
                usb_packet_copy(p, s->ep_in[ep].buf + s->ep_in[ep].offset, len);
                if (s->is_qc && chihiro_game_running) {
                    static int hle_in_log = 0;
                    if (hle_in_log < 30) {
                        hle_in_log++;
                        uint8_t *d = s->ep_in[ep].buf + s->ep_in[ep].offset;
                        fprintf(stderr, "[%07lld] HLE EP%d IN: %d bytes: %02X %02X %02X %02X\n",
                               TS_MS, ep, len, d[0], len>1?d[1]:0, len>2?d[2]:0, len>3?d[3]:0);
                    }
                }
                s->ep_in[ep].offset += len;
                s->ep_in[ep].pending -= len;
                if (ep == 4 && s->ep_in[ep].pending == 0 &&
                    s->an2131.ep[4].in_armed) {
                    s->an2131.ep[4].in_armed = false;
                    s->an2131.ep[4].bc_in = 0;
                    s->an2131.ep[4].cs_in &= ~0x02;
                    s->an2131.in07irq |= (1 << 4);
                }
                s->bulk_in_count++;
                freeze_last_usb_activity_ms = TS_MS;
            } else {
                int avail = an2131_ep_in_poll(&s->an2131, ep);
                if (avail > 0) {
                    uint8_t buf[64];
                    int got = an2131_ep_in_read(&s->an2131, ep, buf, MIN((int)p->iov.size, (int)sizeof(buf)));
                    if (got > 0) {
                        usb_packet_copy(p, buf, got);
                        s->bulk_in_count++;
                        freeze_last_usb_activity_ms = TS_MS;
                        if (s->is_qc) {
                            static int eprd_sb = 0, eprd_gm = 0;
                            if (!chihiro_game_running && eprd_sb < 20) {
                                eprd_sb++;
                                fprintf(stderr, "[%07lld] LLE EP%d IN read: %d bytes:",
                                       TS_MS, ep, got);
                                for (int i = 0; i < got && i < 16; i++)
                                    fprintf(stderr, " %02X", buf[i]);
                                fprintf(stderr, " game=0\n");
                            }
                            if (chihiro_game_running && eprd_gm < 40) {
                                eprd_gm++;
                                fprintf(stderr, "[%07lld] LLE EP%d IN read: %d bytes:",
                                       TS_MS, ep, got);
                                for (int i = 0; i < got && i < 16; i++)
                                    fprintf(stderr, " %02X", buf[i]);
                                fprintf(stderr, " game=1\n");
                            }
                        }
                    } else {
                        p->status = USB_RET_NAK;
                    }
                } else {
                    s->nak_count++;
                    p->status = USB_RET_NAK;
                }
            }
        } else {
            int len = p->iov.size;
            uint8_t buf[64];
            int chunk = MIN(len, (int)sizeof(buf));
            usb_packet_copy(p, buf, chunk);
            an2131_ep_out_write(&s->an2131, ep, buf, chunk);
            s->bulk_out_count++;
            freeze_last_usb_activity_ms = TS_MS;

            /* Backup write HLE: track EP3 OUT writes and signal completion
             * for the 0x18 count=0 status poll. */
            if (ep == 3 && s->is_qc && chunk > 0) {
                uint16_t addr = s->write_1f_addr;
                if (addr + chunk <= 65536) {
                    memcpy(s->extmem + addr, buf, chunk);
                    s->write_1f_addr += chunk;
                }
                s->write_1f_remaining -= chunk;
                if (s->write_1f_remaining <= 0 && addr >= 0x8000) {
                    s->acbu_response_ready = true;
                }
                if (chihiro_game_running) {
                    static int ep3_trace = 0;
                    if (ep3_trace < 50) {
                        ep3_trace++;
                        fprintf(stderr, "[%07lld] LLE EP3 OUT: addr=0x%04X buf=%02X%02X%02X%02X rem=%d status=%d\n",
                               TS_MS, addr, buf[0], buf[1], buf[2], buf[3],
                               s->write_1f_remaining, p->status);
                    }
                    if (s->write_1f_remaining <= 0 && addr >= 0x8000) {
                        fprintf(stderr, "[%07lld] WRITE COMPLETE: addr=0x%04X ready=%d status=%d\n",
                               TS_MS, addr, s->acbu_response_ready, p->status);
                    }
                }
                if (addr == 0x8000 && chunk >= 4 &&
                    buf[0] == 'A' && buf[1] == 'C' &&
                    buf[2] == 'B' && buf[3] == 'U') {
                    memcpy(s->extmem + 0x8400, s->ic11, 128);
                    s->acbu_response_ready = true;
                }
            }

            /* EP4 OUT: trace JVS data going to firmware */
            if (ep == 4 && s->is_qc && chunk > 0) {
                static int ep4_trace = 0;
                if (ep4_trace < 30) {
                    ep4_trace++;
                    fprintf(stderr, "[%07lld] EP4 OUT %d bytes:",
                           TS_MS, chunk);
                    for (int i = 0; i < chunk && i < 16; i++)
                        fprintf(stderr, " %02X", buf[i]);
                    fprintf(stderr, "\n");
                }
            }
        }
        return;
    }

    if (p->pid == USB_TOKEN_IN) {
        /* Bulk IN — return queued data from per-endpoint buffer */
        if (ep >= 1 && ep < CHIHIRO_USB_MAX_EP && s->ep_in[ep].pending > 0) {
            int len = MIN(p->iov.size, s->ep_in[ep].pending);
            usb_packet_copy(p, s->ep_in[ep].buf + s->ep_in[ep].offset, len);
            s->ep_in[ep].offset += len;
            s->ep_in[ep].pending -= len;
            s->bulk_in_count++;
            freeze_last_usb_activity_ms = TS_MS;
            if (chihiro_game_running && s->is_qc) {
                static int in_by_ep[8] = {0};
                if (ep < 8 && in_by_ep[ep] < 5) {
                    in_by_ep[ep]++;
                    fprintf(stderr, "[%07lld] chihiro-usb [QC]: BULK IN EP%d → %d bytes (sbhq=%d)\n",
                           TS_MS, ep, len, (int)s->acbu_sbhq_pending);
                }
            }
        } else {
            if (s->acbu_sbhq_pending && s->is_qc && ep == 3 &&
                s->acbu_last_ep3_out_ms > 0 &&
                (TS_MS - s->acbu_last_ep3_out_ms) > 500) {
                memcpy(s->ep_in[3].buf, s->ic11, 128);
                s->ep_in[3].pending = 128;
                s->ep_in[3].offset = 0;
                s->acbu_sbhq_pending = false;
                s->acbu_response_ready = true;
                fprintf(stderr, "[%07lld] chihiro-usb [QC]: SBHQ HLE — "
                       "ic11 (128B) queued on EP3 IN (waited %lldms after upload)\n",
                       TS_MS, (long long)(TS_MS - s->acbu_last_ep3_out_ms));
                int len = MIN(p->iov.size, s->ep_in[3].pending);
                usb_packet_copy(p, s->ep_in[3].buf, len);
                s->ep_in[3].offset += len;
                s->ep_in[3].pending -= len;
                s->bulk_in_count++;
            } else {
                s->nak_count++;
                if (chihiro_game_running && s->is_qc) {
                    static int nak_by_ep[8] = {0};
                    if (ep < 8 && nak_by_ep[ep] < 5) {
                        nak_by_ep[ep]++;
                        fprintf(stderr, "[%07lld] chihiro-usb [QC]: NAK EP%d IN (sbhq=%d last_out=%lld gap=%lld)\n",
                               TS_MS, ep, (int)s->acbu_sbhq_pending,
                               (long long)s->acbu_last_ep3_out_ms,
                               s->acbu_last_ep3_out_ms > 0 ? (long long)(TS_MS - s->acbu_last_ep3_out_ms) : -1LL);
                    }
                    if (s->acbu_sbhq_pending && ep == 3) {
                        static int sbhq_ep3_diag = 0;
                        if (sbhq_ep3_diag < 3) {
                            sbhq_ep3_diag++;
                            fprintf(stderr, "[%07lld] chihiro-usb [QC]: SBHQ EP3 NAK check: "
                                   "last_out=%lld gap=%lld (need>500)\n",
                                   TS_MS, (long long)s->acbu_last_ep3_out_ms,
                                   (long long)(TS_MS - s->acbu_last_ep3_out_ms));
                        }
                    }
                }
                p->status = USB_RET_NAK;
            }
        }
    } else {
        /* Bulk OUT — capture data for JVS processing */
        if(0) printf("[%07lld] chihiro-usb [%s]: BULK OUT EP%d %d bytes\n",
               TS_MS, id, ep, (int)p->iov.size);
        if (chihiro_game_running && s->is_qc) {
            static int out_by_ep[8] = {0};
            if (ep < 8 && out_by_ep[ep] < 5) {
                out_by_ep[ep]++;
                fprintf(stderr, "[%07lld] chihiro-usb [QC]: BULK OUT EP%d %d bytes\n",
                       TS_MS, ep, (int)p->iov.size);
            }
        }
        int len = p->iov.size;
        uint8_t buf[256];
        int total = 0;
        while (len > 0) {
            int chunk = MIN(len, (int)sizeof(buf) - total);
            if (chunk <= 0) {
                uint8_t discard[64];
                chunk = MIN(len, (int)sizeof(discard));
                usb_packet_copy(p, discard, chunk);
            } else {
                usb_packet_copy(p, buf + total, chunk);
                total += chunk;
            }
            len -= chunk;
        }
        if (total > 0) {
            if(0) { printf("[%07lld] chihiro-usb [%s]: BULK OUT data:", TS_MS, id);
            for (int i = 0; i < total && i < 32; i++) printf(" %02X", buf[i]);
            printf("\n"); }

            if (ep == 2) {
                /* EP2 OUT: ic11 EEPROM write (from vendor 0x1E) */
                uint16_t addr = s->write_1e_addr;
                int copy = MIN(total, 512 - (int)addr);
                if (copy > 0) {
                    memcpy(s->ic11 + addr, buf, copy);
                    s->write_1e_addr += copy;
                }
            } else if (ep == 3) {
                /* EP3 OUT: external memory write (from vendor 0x1F) */
                uint16_t addr = s->write_1f_addr;
                int copy = MIN(total, 65536 - (int)addr);
                if (copy > 0) {
                    memcpy(s->extmem + addr, buf, copy);
                    s->write_1f_addr += copy;
                }
                s->write_1f_remaining -= copy;
                s->acbu_last_ep3_out_ms = TS_MS;

                if (s->write_1f_remaining <= 0 && addr >= 0x8000) {
                    s->acbu_response_ready = true;
                }

                if (addr == 0x8000 && total >= 4 &&
                    buf[0] == 'A' && buf[1] == 'C' &&
                    buf[2] == 'B' && buf[3] == 'U') {
                    memcpy(s->extmem + 0x8400, s->ic11, 128);
                    s->acbu_response_ready = true;
                }

            } else if (ep == 4) {
                /* EP4 OUT: JVS data with 3-byte AN2131QC header.
                 * Process and auto-queue response on EP4 IN (simulates
                 * AN2131QC firmware's automatic JVS bus relay). */
                static int ep4_out_log = 0;
                if (usb_log_verbose && ep4_out_log < 30) {
                    ep4_out_log++;
                    fprintf(stderr, "[%07lld] GAME EP4 OUT %d bytes:",
                           TS_MS, total);
                    for (int i = 0; i < total && i < 32; i++) fprintf(stderr, " %02X", buf[i]);
                    fprintf(stderr, "\n");
                }
                uint8_t *jvs_p = NULL;
                int jvs_n = 0;
                if (total >= 4 && buf[3] == JVS_SYNC) {
                    jvs_p = buf + 3;
                    jvs_n = total - 3;
                } else if (total >= 1 && buf[0] == JVS_SYNC) {
                    jvs_p = buf;
                    jvs_n = total;
                }
                if (jvs_p && jvs_n > 0) {
                    int rlen = chihiro_jvs_process(&s->jvs, jvs_p, jvs_n,
                                                    s->jvs.response, sizeof(s->jvs.response));
                    s->jvs.response_len = rlen;
                    if (rlen > 0 && chihiro_game_running) {
                        uint8_t *ep4 = s->ep_in[4].buf;
                        int wrapped = 0;
                        uint8_t resp_dest = (s->jvs.last_target == JVS_BROADCAST) ? 0
                                                                                  : s->jvs.device_id;
                        ep4[wrapped++] = 0x00;
                        ep4[wrapped++] = 0x01;
                        ep4[wrapped++] = resp_dest;
                        ep4[wrapped++] = 0x00;
                        ep4[wrapped++] = rlen & 0xFF;
                        ep4[wrapped++] = (rlen >> 8) & 0xFF;
                        int copy = MIN(rlen, (int)sizeof(s->ep_in[4].buf) - wrapped);
                        memcpy(ep4 + wrapped, s->jvs.response, copy);
                        wrapped += copy;
                        s->ep_in[4].pending = wrapped;
                        s->ep_in[4].offset = 0;
                        s->jvs.response_len = 0;
                        static int ep4_in_log = 0;
                        if (usb_log_verbose && ep4_in_log < 30) {
                            ep4_in_log++;
                            fprintf(stderr, "[%07lld] GAME EP4 IN queued %d bytes:",
                                   TS_MS, wrapped);
                            for (int i = 0; i < wrapped && i < 32; i++) fprintf(stderr, " %02X", ep4[i]);
                            fprintf(stderr, "\n");
                        }
                    }
                } else if (total > 0) {
                    static int ep4_nojvs_log = 0;
                    if (usb_log_verbose && ep4_nojvs_log < 30) {
                        ep4_nojvs_log++;
                        fprintf(stderr, "[%07lld] GAME EP4 OUT NOT-JVS (no sync)\n",
                               TS_MS);
                    }
                }
            }
        }
        s->bulk_out_count++;
        freeze_last_usb_activity_ms = TS_MS;
    }
}

/* v202: Counter accessors for DIAG timer in chihiro.c */
void chihiro_usb_get_counters(USBDevice *dev, uint32_t *nak, uint32_t *bulk_in, uint32_t *bulk_out)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    if (nak)      *nak      = s->nak_count;
    if (bulk_in)  *bulk_in  = s->bulk_in_count;
    if (bulk_out) *bulk_out = s->bulk_out_count;
}

void chihiro_usb_reset_counters(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->nak_count = 0;
    s->bulk_in_count = 0;
    s->bulk_out_count = 0;
}

void chihiro_usb_get_jvs_counters(uint64_t *send, uint64_t *recv, uint64_t *recv_data)
{
    if (send)      *send      = jvs_send_count;
    if (recv)      *recv      = jvs_recv_count;
    if (recv_data) *recv_data = jvs_recv_has_data;
}

void chihiro_usb_dump_vendor_histogram(void)
{
    printf("=== VENDOR REQUEST HISTOGRAM ===\n");
    for (int i = 0; i < 256; i++) {
        if (vendor_req_counts[i] > 0) {
            printf("  req 0x%02X: %lu\n", i, (unsigned long)vendor_req_counts[i]);
        }
    }
    printf("================================\n");
}

static void chihiro_an2131qc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = true;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */

    /* Load real ic10 QC EEPROM firmware (8192 bytes from MAME hotd3.zip).
     * Contains AN2131 8051 firmware code + region/serial/game data.
     * SEGABOOT reads this via vendor request 0x16 + bulk IN EP1. */
    _Static_assert(sizeof(hotd3_ic10_g24lc64) == 8192,
                   "ic10 firmware must be exactly 8KB");
    memcpy(s->eeprom, hotd3_ic10_g24lc64, sizeof(s->eeprom));

    /* Region byte at eeprom[0x1F00]: SEGABOOT checks boot.id[0x38] bitmask
     * against (1 << region). JPN-only games (e.g. Golf SBLF, bitmask=0x02)
     * fail with ERROR 05 if region=2. Region=1 (JPN) works for all known
     * games since all bitmasks include bit 1. */
    s->eeprom[0x1F00] = 0x01;  /* Region: 01=JPN, 02=USA, 03=EXP */

    /* Initialize per-endpoint bulk transfer state */
    memset(s->ep_in, 0, sizeof(s->ep_in));

    /* Load ic11 baseboard EEPROM (256 bytes, 24LC024) — first 128 from dump, rest zero */
    _Static_assert(sizeof(hotd3_ic11_24lc024) == 128,
                   "ic11 EEPROM dump must be exactly 128 bytes");
    memset(s->ic11, 0, sizeof(s->ic11));
    memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));
    memset(s->extmem, 0, sizeof(s->extmem));
    s->write_1e_addr = 0;
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    /* v302: EZ-USB firmware reboot timers */
    s->ezusb_disconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_disconnect_cb, s);
    s->ezusb_reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_reconnect_cb, s);
    s->ezusb_rebooted = true;  /* v307: reconnect disabled — Path B fix makes it unnecessary */

    chihiro_jvs_init(&s->jvs);
    chihiro_jvs_global = &s->jvs;

    /* AN2131 LLE: init 8051 CPU + register layer, wire EEPROMs + extmem */
    an2131_init(&s->an2131);
    s->an2131.ic10_eeprom = s->eeprom;
    s->an2131.ic10_size = sizeof(s->eeprom);
    s->an2131.ic11_eeprom = s->ic11;
    s->an2131.ic11_size = sizeof(hotd3_ic11_24lc024);
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

    printf("[%07lld] Chihiro QC: loaded ic10 (8192B) + ic11 (128B), "
           "region=JPN (0x01), serial=%.16s, LLE=%s\n",
           TS_MS, (const char *)&s->eeprom[0x1F10],
           s->use_lle ? "ACTIVE" : "OFF");
}

static void chihiro_an2131qc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
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

    /* Load real pc20 SC EEPROM firmware (8192 bytes from MAME hotd3.zip). */
    _Static_assert(sizeof(hotd3_pc20_g24lc64) == 8192,
                   "pc20 firmware must be exactly 8KB");
    memcpy(s->eeprom, hotd3_pc20_g24lc64, sizeof(s->eeprom));

    /* Initialize per-endpoint bulk transfer state */
    memset(s->ep_in, 0, sizeof(s->ep_in));

    /* Load ic11 baseboard EEPROM (256 bytes, 24LC024) */
    memset(s->ic11, 0, sizeof(s->ic11));
    memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(hotd3_ic11_24lc024));
    memset(s->extmem, 0, sizeof(s->extmem));
    s->write_1e_addr = 0;
    s->write_1f_addr = 0;

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    /* Initialize UART buffers */
    s->uart0_rx_len = 0;
    s->uart1_rx_len = 0;

    /* v302: EZ-USB firmware reboot timers */
    s->ezusb_disconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_disconnect_cb, s);
    s->ezusb_reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_reconnect_cb, s);
    s->ezusb_rebooted = true;  /* v307: reconnect disabled — Path B fix makes it unnecessary */

    chihiro_jvs_init(&s->jvs);

    /* AN2131 LLE: init 8051 CPU + register layer, wire EEPROMs + extmem */
    an2131_init(&s->an2131);
    s->an2131.ic10_eeprom = s->eeprom;
    s->an2131.ic10_size = sizeof(s->eeprom);
    s->an2131.ic11_eeprom = s->ic11;
    s->an2131.ic11_size = sizeof(hotd3_ic11_24lc024);
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
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
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

static void chihiro_usb_register_types(void)
{
    type_register_static(&chihiro_an2131qc_info);
    type_register_static(&chihiro_an2131sc_info);
}

type_init(chihiro_usb_register_types)
