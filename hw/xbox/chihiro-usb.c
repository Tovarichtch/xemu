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

    /* ic10 EEPROM data (8KB, read via vendor request 0x16) */
    uint8_t ic10_eeprom[8192];

    /* Pending bulk transfer (queued by vendor request 0x16/0x17) */
    uint8_t bulk_buf[256];
    int bulk_pending;      /* bytes pending for bulk IN */
    int bulk_offset;       /* current read offset in bulk_buf */
    int bulk_ep;           /* which EP the data is queued for */
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

static void handle_reset(USBDevice *dev)
{
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";
    printf("chihiro-usb [%s]: device reset\n", id);
    fflush(stdout);
}

static void handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    const char *id = ((ChihiroUSBState *)dev)->is_qc ? "QC" : "SC";

    printf("chihiro-usb [%s]: control req=0x%04X val=0x%04X idx=0x%04X len=%d\n",
           id, request, value, index, length);
    fflush(stdout);

    int ret = usb_desc_handle_control(dev, p, request, value, index,
                                      length, data);
    if (ret >= 0) {
        printf("chihiro-usb [%s]: → std handled, dev addr=%d\n", id, dev->addr);
        return;
    }

    /* Vendor request — extract bRequest from combined field.
     * QEMU encodes: request = (bmRequestType << 8) | bRequest */
    int bRequest = request & 0xFF;

    /* Default response (MAME: every vendor request gets this) */
    for (int n = 0; n < length && n < 6; n++) {
        data[n] = 0x50 ^ n;
    }
    data[0] = 0x00;  /* success */
    data[1] = 0x4B;  /* PINSA: DIP switches */
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
            memcpy(s->bulk_buf, s->ic10_eeprom + addr, count);
        } else {
            memset(s->bulk_buf, 0xFF, count);
        }
        s->bulk_pending = count;
        s->bulk_offset = 0;
        s->bulk_ep = 1;
        printf("chihiro-usb [%s]: READ EEPROM1 addr=0x%04X count=%d → queued for EP1\n",
               id, addr, count);
        break;
    }
    case 0x17: /* Read ic10 EEPROM #2 (offset +0x2000) */
    {
        int addr = 0x2000 + value;
        int count = index;
        if (count > 256) count = 256;
        if (addr + count > 8192) count = 8192 - addr;
        if (addr >= 0 && addr < 8192 && count > 0) {
            memcpy(s->bulk_buf, s->ic10_eeprom + addr, count);
        } else {
            memset(s->bulk_buf, 0xFF, count);
        }
        s->bulk_pending = count;
        s->bulk_offset = 0;
        s->bulk_ep = 2;
        printf("chihiro-usb [%s]: READ EEPROM2 addr=0x%04X count=%d\n", id, addr, count);
        break;
    }
    case 0x19: /* Get JVS responses — no JVS data pending */
        data[0] = 0x00;  /* not busy */
        data[4] = 0;     /* 0 bytes of JVS response */
        data[5] = 0;
        break;
    case 0x20: /* Send JVS packets — accept and discard */
        printf("chihiro-usb [%s]: JVS SEND count=%d (stub)\n", id, index);
        break;
    case 0x30: /* External interrupt control */
        data[4] = (value & 0xFF) > 0 ? 1 : 0;  /* enabled? */
        data[5] = 0;  /* IRQ counter */
        printf("chihiro-usb [%s]: EXT IRQ control val=%d\n", id, value);
        break;
    case 0x1C: /* Read RTC — stub */
        break;
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
    default:
        printf("chihiro-usb [%s]: unknown vendor req 0x%02X val=0x%04X idx=0x%04X\n",
               id, bRequest, value, index);
        p->status = USB_RET_STALL;
        return;
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
            printf("chihiro-usb [%s]: bulk IN ep%d %d bytes (%d remaining)\n",
                   id, ep, len, s->bulk_pending);
        } else {
            /* No data pending — return NAK (not ready, try later) */
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
        printf("chihiro-usb [%s]: bulk OUT ep%d %zd bytes (discarded)\n",
               id, ep, p->iov.size);
    }
}

static void chihiro_an2131qc_realize(USBDevice *dev, Error **errp)
{
    ChihiroUSBState *s = (ChihiroUSBState *)dev;
    s->is_qc = true;
    usb_desc_init(dev);
    dev->auto_attach = 0;  /* Attach later via hotplug timer */
    printf("chihiro-usb [QC]: realized (auto_attach=0, will hotplug later)\n");

    /* Initialize ic10 EEPROM with default data.
     * TODO: load from ic10_g24lc64.bin file instead of hardcoding. */
    memset(s->ic10_eeprom, 0xFF, sizeof(s->ic10_eeprom));

    /* Region + flags at 0x1F00 */
    s->ic10_eeprom[0x1F00] = 0x02;  /* Region: 01=JPN, 02=USA, 03=EXP */
    s->ic10_eeprom[0x1F01] = 0xFE;  /* Flags */

    /* Baseboard serial at 0x1F10 — "AAEE-01D44744715"
     * Validated against mask "%%%@-##@########" */
    memcpy(&s->ic10_eeprom[0x1F10], "AAEE-01D44744715", 16);

    /* Initialize bulk transfer state */
    s->bulk_pending = 0;
    s->bulk_offset = 0;
    s->bulk_ep = 0;

    printf("Chihiro: AN2131QC USB device initialized "
           "(serial=AAEE-01D44744715, region=JPN)\n");
}

static void chihiro_an2131qc_unrealize(USBDevice *dev)
{
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
    printf("chihiro-usb [SC]: realized (auto_attach=0, will hotplug later)\n");
}

static void chihiro_an2131sc_unrealize(USBDevice *dev)
{
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
