#ifndef USB_RESPONDER_CRC32_H
#define USB_RESPONDER_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t usb_responder_crc32(const uint8_t* data, size_t size);

#endif
