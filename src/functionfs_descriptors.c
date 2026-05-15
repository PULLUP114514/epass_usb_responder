#include "functionfs_descriptors.h"

#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

#include <stdlib.h>
#include <string.h>

static void write_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static bool append(uint8_t** buf, size_t* used, size_t* cap, const void* src, size_t n) {
    uint8_t* next = NULL;
    if (*used + n > *cap) {
        size_t new_cap = (*cap == 0) ? 256u : *cap;
        while (new_cap < *used + n) {
            new_cap *= 2u;
        }
        next = (uint8_t*)realloc(*buf, new_cap);
        if (!next) {
            return false;
        }
        *buf = next;
        *cap = new_cap;
    }
    memcpy(*buf + *used, src, n);
    *used += n;
    return true;
}

static bool append_usb_interface_desc(uint8_t** buf, size_t* used, size_t* cap) {
    struct usb_interface_descriptor intf;
    memset(&intf, 0, sizeof(intf));
    intf.bLength = sizeof(intf);
    intf.bDescriptorType = USB_DT_INTERFACE;
    intf.bNumEndpoints = 2;
    intf.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    intf.iInterface = 1;
    return append(buf, used, cap, &intf, sizeof(intf));
}

static bool append_usb_bulk_ep_desc(
    uint8_t** buf, size_t* used, size_t* cap, uint8_t addr, uint16_t max_packet) {
    struct usb_endpoint_descriptor_no_audio ep;
    memset(&ep, 0, sizeof(ep));
    ep.bLength = sizeof(ep);
    ep.bDescriptorType = USB_DT_ENDPOINT;
    ep.bEndpointAddress = addr;
    ep.bmAttributes = USB_ENDPOINT_XFER_BULK;
    ep.wMaxPacketSize = max_packet;
    return append(buf, used, cap, &ep, sizeof(ep));
}

static bool append_os_desc_ext_compat(uint8_t** buf, size_t* used, size_t* cap) {
    struct usb_os_desc_header hdr;
    struct usb_ext_compat_desc compat;

    memset(&hdr, 0, sizeof(hdr));
    hdr.interface = 0;
    hdr.dwLength = sizeof(hdr) + sizeof(compat);
    hdr.bcdVersion = 1;
    hdr.wIndex = 4;
    hdr.bCount = 1;

    memset(&compat, 0, sizeof(compat));
    compat.bFirstInterfaceNumber = 0;
    compat.Reserved1 = 1;
    memcpy(compat.CompatibleID, "WINUSB", 6);

    return append(buf, used, cap, &hdr, sizeof(hdr)) &&
           append(buf, used, cap, &compat, sizeof(compat));
}

static bool append_os_desc_ext_prop(uint8_t** buf, size_t* used, size_t* cap) {
    static const char kPropName[] = "DeviceInterfaceGUIDs";
    static const char kPropValue[] = "{0D8E9A49-5DB7-4D38-88A0-5A8C5A9A6401}";
    struct usb_os_desc_header hdr;
    struct usb_ext_prop_desc prop;
    uint8_t name_utf16[(sizeof(kPropName)) * 2];
    uint8_t data_utf16[(sizeof(kPropValue) + 1) * 2];
    uint16_t name_len = 0;
    uint32_t data_len = 0;
    uint32_t desc_size = 0;

    memset(name_utf16, 0, sizeof(name_utf16));
    memset(data_utf16, 0, sizeof(data_utf16));
    for (size_t i = 0; i < sizeof(kPropName); ++i) {
        name_utf16[i * 2] = (uint8_t)kPropName[i];
    }
    for (size_t i = 0; i < sizeof(kPropValue) + 1; ++i) {
        data_utf16[i * 2] = (i < sizeof(kPropValue)) ? (uint8_t)kPropValue[i] : 0;
    }

    name_len = (uint16_t)sizeof(name_utf16);
    data_len = (uint32_t)sizeof(data_utf16);
    desc_size = (uint32_t)(sizeof(prop) + name_len + 4u + data_len);

    memset(&hdr, 0, sizeof(hdr));
    hdr.interface = 0;
    hdr.dwLength = sizeof(hdr) + desc_size;
    hdr.bcdVersion = 1;
    hdr.wIndex = 5;
    hdr.wCount = 1;

    memset(&prop, 0, sizeof(prop));
    prop.dwSize = desc_size;
    prop.dwPropertyDataType = 7;
    prop.wPropertyNameLength = name_len;

    return append(buf, used, cap, &hdr, sizeof(hdr)) &&
           append(buf, used, cap, &prop, sizeof(prop)) &&
           append(buf, used, cap, name_utf16, name_len) &&
           append(buf, used, cap, &data_len, sizeof(data_len)) &&
           append(buf, used, cap, data_utf16, data_len);
}

bool usb_responder_build_descriptors_blob(uint8_t** out_data, size_t* out_size) {
    uint8_t* buf = NULL;
    size_t used = 0;
    size_t cap = 0;
    struct usb_functionfs_descs_head_v2 hdr;
    uint32_t counts[3];
    bool ok = false;

    if (!out_data || !out_size) {
        return false;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2;
    hdr.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_MS_OS_DESC;
    hdr.length = 0;

    counts[0] = 3;  /* fs_count */
    counts[1] = 3;  /* hs_count */
    counts[2] = 2;  /* os_count */

    ok = append(&buf, &used, &cap, &hdr, sizeof(hdr)) &&
         append(&buf, &used, &cap, counts, sizeof(counts)) &&
         append_usb_interface_desc(&buf, &used, &cap) &&
         append_usb_bulk_ep_desc(&buf, &used, &cap, 1u | USB_DIR_IN, 64u) &&
         append_usb_bulk_ep_desc(&buf, &used, &cap, 2u | USB_DIR_OUT, 64u) &&
         append_usb_interface_desc(&buf, &used, &cap) &&
         append_usb_bulk_ep_desc(&buf, &used, &cap, 1u | USB_DIR_IN, 512u) &&
         append_usb_bulk_ep_desc(&buf, &used, &cap, 2u | USB_DIR_OUT, 512u) &&
         append_os_desc_ext_compat(&buf, &used, &cap) &&
         append_os_desc_ext_prop(&buf, &used, &cap);
    if (!ok) {
        free(buf);
        return false;
    }

    write_le32(buf + 4, (uint32_t)used);
    *out_data = buf;
    *out_size = used;
    return true;
}

bool usb_responder_build_strings_blob(uint8_t** out_data, size_t* out_size) {
    struct usb_functionfs_strings_head header;
    uint8_t* buf = NULL;
    uint16_t lang = 0x0409;
    const char* str = "ePass USB Responder";
    size_t str_len = strlen(str) + 1;
    size_t total = sizeof(header) + sizeof(lang) + str_len;

    if (!out_data || !out_size) {
        return false;
    }
    buf = (uint8_t*)calloc(1, total);
    if (!buf) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.magic = FUNCTIONFS_STRINGS_MAGIC;
    header.length = (uint32_t)total;
    header.str_count = 1;
    header.lang_count = 1;

    memcpy(buf, &header, sizeof(header));
    write_le16(buf + sizeof(header), lang);
    memcpy(buf + sizeof(header) + sizeof(lang), str, str_len);

    *out_data = buf;
    *out_size = total;
    return true;
}
