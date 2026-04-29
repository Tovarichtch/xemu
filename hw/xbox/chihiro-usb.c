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
#define TS_MS ((long long)(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)))
#define DEBUG_CUSB
#ifdef DEBUG_CUSB
#define DPRINTF(s, ...) printf("chihiro-usb: " s, ## __VA_ARGS__)
#else
#define DPRINTF(...)
#endif

typedef struct ChihiroUSBState {
    USBDevice dev;

    /* Device identity (set at realize, safe to use in all callbacks) */
    bool is_qc;  /* true = AN2131QC (PID 0x0002), false = AN2131SC (PID 0x0003) */

    /* I2C EEPROM data (8KB): ic10 for QC, pc20 for SC — loaded at realize */
    uint8_t eeprom[8192];

    /* Pending bulk transfer (queued by vendor request 0x16/0x17) */
    uint8_t bulk_buf[256];
    int bulk_pending;      /* bytes pending for bulk IN */
    int bulk_offset;       /* current read offset in bulk_buf */
    int bulk_ep;           /* which EP the data is queued for */

    /* EZ-USB firmware download state (ANCHOR_LOAD / bRequest 0xA0) */
    uint32_t fw_bytes_written;  /* total bytes received via 0xA0 */
    bool fw_cpu_held;           /* true if CPUCS register set to hold CPU */

    /* ic11 EEPROM (128 bytes) — baseboard config, "ACBU0001" + game ID */
    uint8_t ic11[128];

    /* SC UART buffers (for JVS communication) */
    uint8_t uart0_rx[256];  /* UART0 receive buffer */
    int uart0_rx_len;
    uint8_t uart1_rx[256];  /* UART1 / JVS receive buffer */
    int uart1_rx_len;

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
        printf("[%07lld] chihiro-usb [%s]: v302 disconnect skipped (already detached)\n", TS_MS, id);
        return;
    }

    printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — DISCONNECT (CSC will fire)\n", TS_MS, id);
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
        printf("[%07lld] chihiro-usb [%s]: v302 reconnect skipped (already attached)\n", TS_MS, id);
        return;
    }

    printf("[%07lld] chihiro-usb [%s]: ★ v302 EZ-USB firmware reboot — RECONNECT (CSC will fire → Phase 2)\n", TS_MS, id);
    usb_device_attach(dev, &error_abort);
}

static void handle_reset(USBDevice *dev)
{
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    printf("[%07lld] chihiro-usb [%s]: device reset\n", TS_MS, id);
    fflush(stdout);
}

static void handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";

    printf("[%07lld] chihiro-usb [%s]: control req=0x%04X val=0x%04X idx=0x%04X len=%d [%s]\n", TS_MS,
           id, request, value, index, length,
           (request == 0x8006 && (value >> 8) == 1) ? "GET_DESCRIPTOR(DEVICE)" :
           (request == 0x8006 && (value >> 8) == 2) ? "GET_DESCRIPTOR(CONFIG)" :
           (request == 0x8006 && (value >> 8) == 3) ? "GET_DESCRIPTOR(STRING)" :
           (request == 0x0005) ? "SET_ADDRESS" :
           (request == 0x0009) ? "SET_CONFIG" :
           (request == 0x010B) ? "SET_INTERFACE" :
           (request == 0x0001) ? "CLEAR_FEATURE" :
           (request == 0x0003) ? "SET_FEATURE" :
           (request == 0x8000) ? "GET_STATUS" :
           ((request >> 8) == 0x40 || (request >> 8) == 0xC0) ? "VENDOR" :
           "OTHER");
    fflush(stdout);

    int ret = usb_desc_handle_control(dev, p, request, value, index,
                                      length, data);
    if (ret >= 0) {
        int actual = p->actual_length;
        printf("[%07lld] chihiro-usb [%s]: → std handled actual=%d addr=%d", TS_MS, id, actual, dev->addr);
        if (actual > 0) {
            printf(" data=");
            for (int i = 0; i < actual && i < 18; i++) printf("%02X", data[i]);
        }
        printf("\n");

        /* v302: After SET_ADDRESS completes, schedule EZ-USB firmware reboot.
         * Real AN2131 loads firmware from EEPROM, then disconnects+reconnects.
         * SEGABOOT waits for the CSC from reconnect to start Phase 2. */
        if (request == (DeviceOutRequest | USB_REQ_SET_ADDRESS) && !s->ezusb_rebooted) {
            s->ezusb_rebooted = true;
            printf("[%07lld] chihiro-usb [%s]: v302 SET_ADDRESS done (addr=%d) → scheduling EZ-USB reboot in 100ms\n",
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
            printf("[%07lld] chihiro-usb [%s]: ⚠ std handler REJECTED req=0x%04X (ret=%d) — "
                   "falling through to vendor handler\n", TS_MS, id, request, ret);
        }
    }

    /* Vendor request — extract bRequest from combined field.
     * QEMU encodes: request = (bmRequestType << 8) | bRequest */
    int bRequest = request & 0xFF;

    /* Default response (MAME: every vendor request gets this) */
    for (int n = 0; n < length && n < 6; n++) {
        data[n] = 0x50 ^ n;
    }
    data[0] = 0x00;  /* success */
    data[1] = 0x6B;  /* PINSA: DIP switches (pull-up: 0=ON, 1=OFF)
                      * HOD3 setting: DIP 1,2,5=OFF  DIP 3,4=ON
                      * bit0=1 DIP1 OFF (horizontal monitor)
                      * bit1=1 DIP2 OFF
                      * bit2=0 DIP3 ON
                      * bit3=1 CS pin
                      * bit4=0 DIP4 ON
                      * bit5=1 DIP5 OFF (was 0 in MAME)
                      * bit6-7 buttons */
    data[2] = 0x52;  /* PINSB: JVS sense = all addressed */
    data[3] = 0x53;  /* OUTB register */

    switch (bRequest) {
    case 0x16: /* Read ic10 EEPROM #1 — queue for bulk EP1 IN */
    {
        int addr = value;     /* wValue = start address in ic10 */
        int count = index;    /* wIndex = byte count */
        if (count > 256) count = 256;
        if (addr + count > 8192) count = 8192 - addr;
        if (addr >= 0 && count > 0) {
            memcpy(s->bulk_buf, s->eeprom + addr, count);
        } else {
            memset(s->bulk_buf, 0xFF, count);
        }
        s->bulk_pending = count;
        s->bulk_offset = 0;
        s->bulk_ep = 1;
        printf("[%07lld] chihiro-usb [%s]: READ EEPROM1 addr=0x%04X count=%d → queued for EP1\n", TS_MS,
               id, addr, count);
        break;
    }
    case 0x17: /* Read baseboard EEPROM ic11 (128 bytes: "ACBU0001" + game config) */
    {
        int addr = value;
        int count = index;
        if (count > 256) count = 256;
        if (addr + count > 128) count = 128 - addr;
        if (addr >= 0 && addr < 128 && count > 0) {
            memcpy(s->bulk_buf, s->ic11 + addr, count);
        } else {
            memset(s->bulk_buf, 0xFF, count);
            count = (count > 0) ? count : 0;
        }
        s->bulk_pending = count;
        s->bulk_offset = 0;
        s->bulk_ep = 2;
        printf("[%07lld] chihiro-usb [%s]: READ ic11 EEPROM addr=0x%02X count=%d data=", TS_MS, id, addr, count);
        for (int i = 0; i < count && i < 16; i++) printf("%02X", s->bulk_buf[i]);
        printf("\n");
        break;
    }
    case 0x19: /* Get JVS responses — no JVS data pending */
        data[0] = 0x00;  /* not busy */
        data[4] = 0;     /* 0 bytes of JVS response */
        data[5] = 0;
        break;
    case 0x20: /* Send JVS packets — accept and discard */
        printf("[%07lld] chihiro-usb [%s]: JVS SEND count=%d (stub)\n", TS_MS, id, index);
        break;
    case 0x30: /* External interrupt control */
        data[4] = (value & 0xFF) > 0 ? 1 : 0;  /* enabled? */
        data[5] = 0;  /* IRQ counter */
        printf("[%07lld] chihiro-usb [%s]: EXT IRQ control val=%d\n", TS_MS, id, value);
        break;
    case 0x1C: /* Read RTC — queue BCD time for bulk IN EP4 (data[0]=0 success status) */
    {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        #define TO_BCD(v) ((uint8_t)((v) + 6 * ((v) / 10)))
        int rtc_count = index;
        if (rtc_count > 256) rtc_count = 256;
        memset(s->bulk_buf, 0, rtc_count);
        s->bulk_buf[0] = TO_BCD(t->tm_sec);
        s->bulk_buf[1] = TO_BCD(t->tm_min);
        s->bulk_buf[2] = TO_BCD(t->tm_hour);
        s->bulk_buf[3] = 0;
        s->bulk_buf[4] = TO_BCD(t->tm_mday);
        s->bulk_buf[5] = TO_BCD(t->tm_mon + 1);
        s->bulk_buf[6] = TO_BCD(t->tm_year - 100);
        s->bulk_buf[7] = 0;
        s->bulk_pending = rtc_count;
        s->bulk_offset = 0;
        s->bulk_ep = 5;
        #undef TO_BCD
        printf("[%07lld] chihiro-usb [%s]: RTC READ → %02X:%02X:%02X %02X/%02X/%02X (%d bytes queued EP5)\n",
               TS_MS, id, s->bulk_buf[2], s->bulk_buf[1], s->bulk_buf[0],
               s->bulk_buf[4], s->bulk_buf[5], s->bulk_buf[6], rtc_count);
        break;
    }
    case 0x1D: /* Write ic10 EEPROM #1 — accept */
    case 0x1E: /* Write ic10 EEPROM #2 — accept */
    case 0x1F: /* Write external memory — accept */
    case 0x24: /* Write RTC — accept */
        break;
    case 0x18: /* Read external memory — return zeros */
    {
        int count = index;
        if (count > 256) count = 256;
        memset(s->bulk_buf, 0, count);
        s->bulk_pending = count;
        s->bulk_offset = 0;
        s->bulk_ep = 3;
        break;
    }
    case 0xA0: /* ANCHOR_LOAD — EZ-USB firmware download (Cypress AN2131) */
    {
        uint16_t ram_addr = value;  /* wValue = target address in 8051 RAM */
        int count = length;
        if (ram_addr == 0x7F92) {
            /* CPUCS register: bit 0 = 1 → hold CPU in reset, 0 → run */
            bool hold = (count > 0 && data[0] & 0x01);
            printf("[%07lld] chihiro-usb [%s]: ANCHOR_LOAD CPUCS=%s (total %u bytes downloaded)\n",
                   TS_MS, id, hold ? "HOLD" : "RUN", s->fw_bytes_written);
            if (s->fw_cpu_held && !hold) {
                printf("[%07lld] chihiro-usb [%s]: ★ EZ-USB firmware loaded — CPU released\n",
                       TS_MS, id);
            }
            s->fw_cpu_held = hold;
        } else {
            s->fw_bytes_written += count;
            if (s->fw_bytes_written <= count) {
                printf("[%07lld] chihiro-usb [%s]: ANCHOR_LOAD start addr=0x%04X len=%d\n",
                       TS_MS, id, ram_addr, count);
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
        printf("[%07lld] chihiro-usb [%s]: GET UART0 → %d bytes\n", TS_MS, id, avail);
        return;
    }
    case 0x1B: /* Get UART1 / JVS response (SC only) */
    {
        int avail = s->uart1_rx_len;
        if (avail > 0 && avail <= length) {
            memcpy(data, s->uart1_rx, avail);
            p->actual_length = avail;
            s->uart1_rx_len = 0;
        } else {
            p->actual_length = 0;
        }
        printf("[%07lld] chihiro-usb [%s]: GET UART1/JVS → %d bytes\n", TS_MS, id, avail);
        return;
    }
    case 0x22: /* Send UART0 data (SC only) — accept and discard */
        printf("[%07lld] chihiro-usb [%s]: SEND UART0 len=%d (stub)\n", TS_MS, id, length);
        break;
    case 0x23: /* Send UART1 / JVS command (SC only) — accept and discard */
        printf("[%07lld] chihiro-usb [%s]: SEND UART1/JVS len=%d (stub)\n", TS_MS, id, length);
        break;
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
        printf("[%07lld] chihiro-usb [%s]: UART/GPIO config 0x%02X (stub)\n", TS_MS, id, bRequest);
        break;
    case 0x31: /* Set PORTB pins (SC only) — accept */
        printf("[%07lld] chihiro-usb [%s]: SET PORTB val=0x%04X (stub)\n", TS_MS, id, value);
        break;
    default:
        printf("[%07lld] chihiro-usb [%s]: UNHANDLED vendor req 0x%02X val=0x%04X idx=0x%04X len=%d → accepting\n",
               TS_MS, id, bRequest, value, index, length);
        break;
    }

    /* v202: Log vendor response data bytes */
    if (length >= 4) {
        printf("[%07lld] chihiro-usb [%s]: → vendor resp data[0..3]=%02X %02X %02X %02X (len=%d)\n",
               TS_MS, id, data[0], data[1], data[2], data[3], length);
    }

    p->actual_length = length;
}

static void handle_data(USBDevice *dev, USBPacket *p)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    int ep = p->ep->nr;

    if (p->pid == USB_TOKEN_IN) {
        /* Bulk IN — return queued data */
        if (ep == s->bulk_ep && s->bulk_pending > 0) {
            int len = MIN(p->iov.size, s->bulk_pending);
            usb_packet_copy(p, s->bulk_buf + s->bulk_offset, len);
            s->bulk_offset += len;
            s->bulk_pending -= len;
            s->bulk_in_count++;
            printf("[%07lld] chihiro-usb [%s]: bulk IN ep%d %d bytes (%d remaining)\n", TS_MS,
                   id, ep, len, s->bulk_pending);
        } else {
            s->nak_count++;
            if (s->bulk_pending > 0) {
                printf("[%07lld] chihiro-usb [%s]: NAK ep%d (data queued on ep%d, %d bytes)\n",
                       TS_MS, id, ep, s->bulk_ep, s->bulk_pending);
            }
            p->status = USB_RET_NAK;
        }
    } else {
        /* Bulk OUT — accept and discard for now */
        int len = p->iov.size;
        uint8_t discard[64];
        while (len > 0) {
            int chunk = MIN(len, (int)sizeof(discard));
            usb_packet_copy(p, discard, chunk);
            len -= chunk;
        }
        s->bulk_out_count++;
        printf("[%07lld] chihiro-usb [%s]: bulk OUT ep%d %zd bytes (discarded)\n", TS_MS,
               id, ep, p->iov.size);
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

    /* Override region to USA (0x02) to match game's bootid regionFlags.
     * HOD3 bootid has regionFlags=0xFFFFFF0E (US+EXP, no JP).
     * Original ic10 EEPROM has 0x01 (Japan) which would cause ERROR 31. */
    s->eeprom[0x1F00] = 0x02;  /* Region: 01=JPN, 02=USA, 03=EXP */

    /* Initialize bulk transfer state */
    s->bulk_pending = 0;
    s->bulk_offset = 0;
    s->bulk_ep = 0;

    /* Load ic11 baseboard EEPROM (128 bytes) — shared by QC and SC */
    _Static_assert(sizeof(hotd3_ic11_24lc024) == 128,
                   "ic11 EEPROM must be exactly 128 bytes");
    memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(s->ic11));

    /* Initialize EZ-USB firmware state */
    s->fw_bytes_written = 0;
    s->fw_cpu_held = false;

    /* v302: EZ-USB firmware reboot timers */
    s->ezusb_disconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_disconnect_cb, s);
    s->ezusb_reconnect_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ezusb_reconnect_cb, s);
    s->ezusb_rebooted = true;  /* v307: reconnect disabled — Path B fix makes it unnecessary */

    printf("[%07lld] Chihiro QC: loaded ic10 firmware (8192B) + ic11 (128B), "
           "region patched to USA (0x02), serial=%.16s\n",
           TS_MS, (const char *)&s->eeprom[0x1F10]);
}

static void chihiro_an2131qc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
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

    /* Initialize bulk transfer state */
    s->bulk_pending = 0;
    s->bulk_offset = 0;
    s->bulk_ep = 0;

    /* Load ic11 baseboard EEPROM (128 bytes) */
    memcpy(s->ic11, hotd3_ic11_24lc024, sizeof(s->ic11));

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

    printf("[%07lld] Chihiro SC: loaded pc20 firmware (8192B) + ic11 (128B)\n", TS_MS);
}

static void chihiro_an2131sc_unrealize(USBDevice *dev)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    timer_free(s->ezusb_disconnect_timer);
    timer_free(s->ezusb_reconnect_timer);
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
